#include <SPI.h>
#include <LoRa.h>
#include <HardwareSerial.h>
#include <TinyGPS++.h>
#include <OneWire.h>
#include <DallasTemperature.h>
// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// ----- Pin Definitions -----
#define SS 18
#define RST 14
#define DIO0 26
#define PH_PIN 34    
#define TDS_PIN 32    
#define DO_PIN 33    
#define LED_PIN 2
#define ONE_WIRE_BUS 25  // DS18B20 for water temperature

// ----- Constants -----
const float VREF = 3.3;
const int ADC_RES = 4095;

// ----- pH Calibration Parameters -----
const float pH_COEFFICIENT = -5.7980;  // Coefficient from calibration
const float pH_OFFSET = 21.3928;       // Offset from calibration

// ----- DO Calibration Parameters -----
const float DO_RAW_ZERO = 500.38;      // Raw reading at 0% oxygen
const float DO_RAW_MAX = 2700.0;       // Raw reading at 100% oxygen
const float DO_CALIB_SLOPE = 0.045462; // Calculated slope
const float DO_CALIB_INTERCEPT = -22.748; // Calculated intercept

// ----- pH Sensor Variables -----
#define SAMPLE_SIZE 10  // Number of samples for averaging
float pH_samples[SAMPLE_SIZE];
int sample_index = 0;

// ----- DO Sensor Variables -----
#define DO_SAMPLE_SIZE 5  // Number of samples for DO averaging
float DO_samples[DO_SAMPLE_SIZE];
int do_sample_index = 0;

// ----- EC/TDS Calibration Constants -----
namespace device {
  const float aref = 3.3;
}

namespace calibration {
  // Updated with calibration results
  const float calVoltage = 0.036774;
  const float calEC = 84.00;
  const float kValue = 2284.215576;
  const float tdsFactor = 0.50;
  const float ecSlope = calEC / calVoltage;
}

namespace sensor {
  float ec = 0.0;
  float tds = 0.0;
  float waterTemp = 25.0;
  
  // Variables for filtering EC/TDS readings
  const int EC_SAMPLE_SIZE = 5;
  float ec_samples[EC_SAMPLE_SIZE];
  int ec_sample_index = 0;
}

// ----- GPS -----
static const int RXPin = 16, TXPin = 17;
static const uint32_t GPSBaud = 9600;
TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

// ----- Temp Sensor Setup -----
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

// ----- FreeRTOS Task Handles -----
TaskHandle_t taskPH = NULL;
TaskHandle_t taskDO = NULL;
TaskHandle_t taskTDS = NULL;
TaskHandle_t taskTemp = NULL;
TaskHandle_t taskGPS = NULL;
TaskHandle_t taskLora = NULL;

// ----- FreeRTOS Synchronization Objects -----
SemaphoreHandle_t xI2CMutex = NULL;
SemaphoreHandle_t xSPIMutex = NULL;
SemaphoreHandle_t xSerialMutex = NULL;
QueueHandle_t sensorDataQueue = NULL;

// ----- Data Structure for Sensor Readings -----
typedef struct {
  float pH;
  float ec;
  float tds;
  float waterTemp;
  float dissolvedOxygen;
  bool gpsValid;
  float latitude;
  float longitude;
  float altitude;
  float speed;
  int satellites;
} SensorData_t;

// Global instance of sensor data, protected by mutex
SensorData_t currentData = {7.0, 0.0, 0.0, 25.0, 0.0, false, 0.0, 0.0, 0.0, 0.0, 0};
SemaphoreHandle_t xDataMutex = NULL;

// Debugging helper for FreeRTOS
void printTaskInfo(const char* taskName) {
  if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    Serial.print("[");
    Serial.print(taskName);
    Serial.print("] Free stack: ");
    Serial.print(uxTaskGetStackHighWaterMark(NULL));
    Serial.print(", Free heap: ");
    Serial.println(xPortGetFreeHeapSize());
    xSemaphoreGive(xSerialMutex);
  }
}

float readPH(float voltage) {
  // Convert analog voltage to pH using the calibrated equation
  float phValue = pH_COEFFICIENT * voltage + pH_OFFSET;
  return phValue;
}

// Average pH readings to reduce noise
float getAveragePH() {
  float sum = 0;
  for (int i = 0; i < SAMPLE_SIZE; i++) {
    sum += pH_samples[i];
  }
  return sum / SAMPLE_SIZE;
}

// Function to read DO sensor with averaging
float readDO() {
  // Read the raw value with multiple samples for stability
  float raw_sum = 0;
  for (int i = 0; i < 10; i++) {
    raw_sum += analogRead(DO_PIN);
    delay(10);
  }
  float raw_value = raw_sum / 10.0;
  
  // Calculate DO percentage using calibration equation
  float do_percent = DO_CALIB_SLOPE * raw_value + DO_CALIB_INTERCEPT;
  
  // Clamp values to valid range
  if (do_percent < 0) do_percent = 0;
  if (do_percent > 100) do_percent = 100;
  
  // Convert from % saturation to mg/L (approximate at 25°C)
  float do_mg_l = (do_percent / 100.0) * 8.3;
  
  return do_mg_l;
}

// Average DO readings to reduce noise
float getAverageDO() {
  float sum = 0;
  for (int i = 0; i < DO_SAMPLE_SIZE; i++) {
    sum += DO_samples[i];
  }
  return sum / DO_SAMPLE_SIZE;
}

void updateWaterTemp() {
  if (xSemaphoreTake(xI2CMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    sensors.requestTemperatures();
    float temp = sensors.getTempCByIndex(0);
    xSemaphoreGive(xI2CMutex);
    
    if (temp != DEVICE_DISCONNECTED_C && temp > -127) {
      // Update the global variable using mutex
      if (xSemaphoreTake(xDataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        sensor::waterTemp = temp;
        currentData.waterTemp = temp;
        xSemaphoreGive(xDataMutex);
      }
      
      // Debug output
      if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        Serial.print("Water temperature: ");
        Serial.println(temp);
        xSemaphoreGive(xSerialMutex);
      }
    } else {
      if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        Serial.println("Error: DS18B20 not responding or invalid reading.");
        xSemaphoreGive(xSerialMutex);
      }
    }
  }
}

float getAverageEC() {
  float sum = 0;
  for (int i = 0; i < sensor::EC_SAMPLE_SIZE; i++) {
    sum += sensor::ec_samples[i];
  }
  return sum / sensor::EC_SAMPLE_SIZE;
}

void readTDS() {
  // Improved: Add multiple readings for averaging
  float voltage_sum = 0;
  for (int i = 0; i < 10; i++) {
    int adcValue = analogRead(TDS_PIN);
    voltage_sum += adcValue * device::aref / ADC_RES;
    vTaskDelay(pdMS_TO_TICKS(10));  // Small delay between readings
  }
  float voltage = voltage_sum / 10;  // Average of 10 readings
  
  // Get current water temperature with mutex protection
  float waterTemp = 25.0; // Default
  if (xSemaphoreTake(xDataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    waterTemp = sensor::waterTemp;
    xSemaphoreGive(xDataMutex);
  }
  
  // Calculate EC using calibrated kValue and temperature compensation
  float tempCoef = 1.0 + 0.02 * (waterTemp - 25.0);
  float rawEC = voltage * calibration::kValue;
  float compensatedEC = rawEC * tempCoef;
  
  // Store EC reading in rolling buffer
  sensor::ec_samples[sensor::ec_sample_index] = compensatedEC;
  sensor::ec_sample_index = (sensor::ec_sample_index + 1) % sensor::EC_SAMPLE_SIZE;
  
  // Get average EC
  float ec = getAverageEC();
  float tds = ec * calibration::tdsFactor;
  
  // Update global variables with mutex protection
  if (xSemaphoreTake(xDataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    sensor::ec = ec;
    sensor::tds = tds;
    currentData.ec = ec;
    currentData.tds = tds;
    xSemaphoreGive(xDataMutex);
  }
  
  // Debug output
  if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    Serial.print("Raw ADC: ");
    Serial.print(voltage);
    Serial.print("V, EC: ");
    Serial.print(ec);
    Serial.print(" μS/cm, TDS: ");
    Serial.print(tds);
    Serial.println(" ppm");
    xSemaphoreGive(xSerialMutex);
  }
}

// ----- FreeRTOS Task Functions -----

// Task to read pH sensor
void TaskReadPH(void *pvParameters) {
  const TickType_t xFrequency = pdMS_TO_TICKS(20); // 20ms sampling rate
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  for (;;) {
    // Wait for the next cycle
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    
    // Read pH sensor
    int phAnalog = analogRead(PH_PIN);
    float phVoltage = phAnalog * (VREF / ADC_RES);
    float ph = readPH(phVoltage);
    
    // Store in circular buffer
    pH_samples[sample_index] = ph;
    sample_index = (sample_index + 1) % SAMPLE_SIZE;
    
    // Update global data
    if (xSemaphoreTake(xDataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      currentData.pH = getAveragePH();
      xSemaphoreGive(xDataMutex);
    }
    
    // Task debugging - uncomment for debugging
    // printTaskInfo("pH Task");
  }
}

// Task to read Dissolved Oxygen sensor
void TaskReadDO(void *pvParameters) {
  const TickType_t xFrequency = pdMS_TO_TICKS(100); // 100ms sampling rate
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  for (;;) {
    // Wait for the next cycle
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    
    // Read DO sensor
    float do_value = readDO();
    DO_samples[do_sample_index] = do_value;
    do_sample_index = (do_sample_index + 1) % DO_SAMPLE_SIZE;
    
    // Update global data
    if (xSemaphoreTake(xDataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      currentData.dissolvedOxygen = getAverageDO();
      xSemaphoreGive(xDataMutex);
    }
    
    // printTaskInfo("DO Task");
  }
}

// Task to read TDS/EC sensor
void TaskReadTDS(void *pvParameters) {
  const TickType_t xFrequency = pdMS_TO_TICKS(1000); // 1 second sampling rate
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  for (;;) {
    // Wait for the next cycle
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    
    // Read TDS sensor
    readTDS();
    
    // printTaskInfo("TDS Task");
  }
}

// Task to read temperature sensor
void TaskReadTemp(void *pvParameters) {
  const TickType_t xFrequency = pdMS_TO_TICKS(2000); // 2 second sampling rate
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  for (;;) {
    // Wait for the next cycle
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    
    // Update water temperature
    updateWaterTemp();
    
    // printTaskInfo("Temp Task");
  }
}

// Task to process GPS data
void TaskProcessGPS(void *pvParameters) {
  for (;;) {
    // Process GPS data as it becomes available
    while (gpsSerial.available()) {
      char c = gpsSerial.read();
      gps.encode(c);
    }
    
    // Update global data
    if (gps.location.isValid() && xSemaphoreTake(xDataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      currentData.gpsValid = true;
      currentData.latitude = gps.location.lat();
      currentData.longitude = gps.location.lng();
      currentData.altitude = gps.altitude.meters();
      currentData.speed = gps.speed.kmph();
      currentData.satellites = gps.satellites.value();
      xSemaphoreGive(xDataMutex);
    }
    
    // Short delay to prevent this task from hogging CPU
    vTaskDelay(pdMS_TO_TICKS(10));
    
    // printTaskInfo("GPS Task");
  }
}

// Task to send data over LoRa
void TaskSendData(void *pvParameters) {
  const TickType_t xFrequency = pdMS_TO_TICKS(3000); // 3 second transmission rate
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  for (;;) {
    // Wait for the next cycle
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    
    // Take a copy of the current data
    SensorData_t dataSnapshot;
    if (xSemaphoreTake(xDataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      dataSnapshot = currentData;
      xSemaphoreGive(xDataMutex);
    } else {
      // If we can't get the mutex, skip this cycle
      continue;
    }
    
    // Format GPS data
    String gpsData = "";
    if (dataSnapshot.gpsValid) {
      gpsData = "Lat:" + String(dataSnapshot.latitude, 6) + 
                ",Lon:" + String(dataSnapshot.longitude, 6) +
                ",Alt:" + String(dataSnapshot.altitude) + 
                ",Speed:" + String(dataSnapshot.speed) +
                ",Sat:" + String(dataSnapshot.satellites);
    } else {
      gpsData = "GPS:NoFix";
    }
    
    // Prepare LoRa packet
    String dataToSend = "pH:" + String(dataSnapshot.pH, 2) +
                        ",EC:" + String(dataSnapshot.ec, 2) +
                        ",TDS:" + String(dataSnapshot.tds, 2) +
                        ",Temp:" + String(dataSnapshot.waterTemp, 2) +
                        ",DO:" + String(dataSnapshot.dissolvedOxygen, 2) + "," + gpsData;
    
    // Debug print
    if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      Serial.println("\n=== SENSOR DATA ===");
      Serial.println("pH: " + String(dataSnapshot.pH, 2));
      Serial.println("EC: " + String(dataSnapshot.ec, 2) + " μS/cm");
      Serial.println("TDS: " + String(dataSnapshot.tds, 2) + " ppm");
      Serial.println("Water Temp: " + String(dataSnapshot.waterTemp, 2) + "°C");
      
      // Get raw DO value for debugging
      int doRaw = analogRead(DO_PIN);
      float doPercent = DO_CALIB_SLOPE * doRaw + DO_CALIB_INTERCEPT;
      
      Serial.println("DO Raw: " + String(doRaw));
      Serial.println("DO Percent: " + String(doPercent, 2) + "%");
      Serial.println("DO: " + String(dataSnapshot.dissolvedOxygen, 2) + " mg/L");
      
      if (dataSnapshot.gpsValid) {
        Serial.println("GPS Location: " + String(dataSnapshot.latitude, 6) + ", " + 
                      String(dataSnapshot.longitude, 6));
      } else {
        Serial.println("GPS: No Fix");
      }
      
      Serial.println("==================");
      Serial.println("Sending packet: " + dataToSend);
      xSemaphoreGive(xSerialMutex);
    }
    
    // Send data via LoRa
    digitalWrite(LED_PIN, HIGH);
    
    // Take SPI mutex for LoRa transmission
    if (xSemaphoreTake(xSPIMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      LoRa.beginPacket();
      LoRa.print(dataToSend);
      LoRa.endPacket();
      xSemaphoreGive(xSPIMutex);
      
      if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        Serial.println("Packet Sent!");
        xSemaphoreGive(xSerialMutex);
      }
    } else {
      if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        Serial.println("Failed to get SPI mutex for LoRa transmission!");
        xSemaphoreGive(xSerialMutex);
      }
    }
    
    digitalWrite(LED_PIN, LOW);
    
    // printTaskInfo("LoRa Task");
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);  // Wait for serial to connect
  
  Serial.println("\nESP32 Water Quality Monitoring System with FreeRTOS");
  
  // Create mutexes
  xI2CMutex = xSemaphoreCreateMutex();
  xSPIMutex = xSemaphoreCreateMutex();
  xSerialMutex = xSemaphoreCreateMutex();
  xDataMutex = xSemaphoreCreateMutex();
  
  // Create queue
  sensorDataQueue = xQueueCreate(5, sizeof(SensorData_t));
  
  // Initialize GPS
  gpsSerial.begin(GPSBaud, SERIAL_8N1, RXPin, TXPin);
  
  // Initialize LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);

  // Initialize pH sample array
  for (int i = 0; i < SAMPLE_SIZE; i++) {
    pH_samples[i] = 7.0;  // Neutral pH as default
  }
  
  // Initialize DO sample array
  for (int i = 0; i < DO_SAMPLE_SIZE; i++) {
    DO_samples[i] = 0.0;  // Initialize with zero
  }
  
  // Initialize EC sample array
  for (int i = 0; i < sensor::EC_SAMPLE_SIZE; i++) {
    sensor::ec_samples[i] = 0.0;
  }

  Serial.println("Initializing LoRa...");
  SPI.begin(5, 19, 27, 18);
  
  // Take SPI mutex for LoRa initialization
  xSemaphoreTake(xSPIMutex, portMAX_DELAY);
  
  LoRa.setPins(SS, RST, DIO0);
  if (!LoRa.begin(433E6)) {
    Serial.println("LoRa init failed. Check wiring!");
    while (1) {
      digitalWrite(LED_PIN, HIGH);
      delay(300);
      digitalWrite(LED_PIN, LOW);
      delay(300);
    }
  }
  
  LoRa.setSpreadingFactor(9);  
  LoRa.setSignalBandwidth(125E3);  
  LoRa.setCodingRate4(5);  
  LoRa.setPreambleLength(8);  
  LoRa.setTxPower(14);
  
  xSemaphoreGive(xSPIMutex);
  
  Serial.println("LoRa Initialized Successfully!");

  analogReadResolution(12);
  
  // Initialize temperature sensor
  sensors.begin();
  Serial.println("Initializing sensors...");
  updateWaterTemp();  // Get initial temperature reading
  
  // Test sensor readings
  Serial.println("Testing sensors...");
  int phAnalog = analogRead(PH_PIN);
  float phVoltage = phAnalog * (VREF / ADC_RES);
  Serial.print("pH raw analog: ");
  Serial.print(phAnalog);
  Serial.print(", Voltage: ");
  Serial.println(phVoltage);
  
  int tdsAnalog = analogRead(TDS_PIN);
  Serial.print("TDS raw analog: ");
  Serial.println(tdsAnalog);
  
  // Test DO sensor with calibration data
  int doAnalog = analogRead(DO_PIN);
  Serial.print("DO raw analog: ");
  Serial.println(doAnalog);
  Serial.print("DO calibration points - Zero: ");
  Serial.print(DO_RAW_ZERO);
  Serial.print(", Max: ");
  Serial.println(DO_RAW_MAX);
  
  // Create FreeRTOS tasks
  xTaskCreatePinnedToCore(
    TaskReadPH,   // Function to implement the task
    "pH_Task",    // Name of the task
    2048,         // Stack size in words
    NULL,         // Task input parameter
    2,            // Priority of the task (higher = higher priority)
    &taskPH,      // Task handle
    1             // Core where the task should run (1 = non-WiFi core)
  );
  
  xTaskCreatePinnedToCore(
    TaskReadDO, "DO_Task", 2048, NULL, 2, &taskDO, 1
  );
  
  xTaskCreatePinnedToCore(
    TaskReadTDS, "TDS_Task", 2048, NULL, 2, &taskTDS, 1
  );
  
  xTaskCreatePinnedToCore(
    TaskReadTemp, "Temp_Task", 2048, NULL, 2, &taskTemp, 1
  );
  
  xTaskCreatePinnedToCore(
    TaskProcessGPS, "GPS_Task", 2048, NULL, 1, &taskGPS, 1
  );
  
  xTaskCreatePinnedToCore(
    TaskSendData, "LoRa_Task", 4096, NULL, 3, &taskLora, 1
  );
  
  Serial.println("Setup complete, FreeRTOS tasks started.");
  delay(500);
}

void loop() {
  // The main loop is now mostly empty because the tasks are doing all the work
  // We can use it for system monitoring or additional non-time-critical tasks
  
  // Print system information every 10 seconds
  static unsigned long lastSystemInfoTime = 0;
  if (millis() - lastSystemInfoTime >= 10000) {
    lastSystemInfoTime = millis();
    
    if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      // Print header
      Serial.println("\n=== SYSTEM INFO ===");
      
      // Print task states and stack info
      Serial.print("Free heap: ");
      Serial.print(xPortGetFreeHeapSize());
      Serial.print(" bytes, Min free heap: ");
      Serial.print(xPortGetMinimumEverFreeHeapSize());
      Serial.println(" bytes");
      
      // Print task states
      if (taskPH != NULL) {
        Serial.print("pH Task state: ");
        Serial.print(eTaskGetState(taskPH) == eRunning ? "Running" : 
                    (eTaskGetState(taskPH) == eBlocked ? "Blocked" : 
                    (eTaskGetState(taskPH) == eSuspended ? "Suspended" : "Ready")));
        Serial.print(", Stack HWM: ");
        Serial.println(uxTaskGetStackHighWaterMark(taskPH));
      }
      
      Serial.println("==================");
      xSemaphoreGive(xSerialMutex);
    }
  }
  
  // Don't hog CPU time in this task, yield to FreeRTOS tasks
  vTaskDelay(pdMS_TO_TICKS(100));
}
