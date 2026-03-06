/*
 * ESP32 EC/TDS Sensor Calibration Sketch
 * For TDS Meter V1.0 Module Water Quality Sensor
 * Using 84 μS/cm calibration solution
 */

#include <OneWire.h>
#include <DallasTemperature.h>

// Pin definitions
#define TDS_PIN 32        // TDS/EC sensor analog pin
#define ONE_WIRE_BUS 25   // DS18B20 temperature sensor

// Constants
const float VREF = 3.3;   // Reference voltage
const int ADC_RES = 4095; // 12-bit ADC resolution
const float CAL_EC = 84.0; // Calibration solution EC value in μS/cm

// Variables
float kValue = 1.0;       // Cell constant to be calibrated
float rawEC = 0.0;
float calibratedEC = 0.0;
float waterTemp = 25.0;   // Default temperature
float calVoltage = 0.0;   // Voltage reading at calibration

// Temperature sensor setup
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature tempSensors(&oneWire);

// For averaging readings
const int NUM_SAMPLES = 30;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  
  analogReadResolution(12);
  tempSensors.begin();
  
  Serial.println("\n\n=== ESP32 EC/TDS Sensor Calibration ===");
  Serial.println("This sketch will calibrate your TDS Meter V1.0 Module");
  Serial.println("You will need an 84 μS/cm calibration solution");
  
  delay(1000);
  
  startCalibration();
}

void startCalibration() {
  Serial.println("\n--- EC/TDS Calibration Process ---");
  Serial.println("1. Rinse your EC/TDS probe with distilled water and dry it");
  Serial.println("2. Place probe in 84 μS/cm calibration solution");
  Serial.println("3. Wait for readings to stabilize (~30 seconds)");
  Serial.println("4. Press Enter when ready to begin calibration");
  
  waitForInput();
  
  // Calibration procedure
  Serial.println("Starting calibration...");
  Serial.println("Taking temperature readings...");
  
  // Get current temperature
  updateTemperature();
  
  Serial.println("Taking EC readings, please wait...");
  delay(2000);
  
  // Take multiple samples for accuracy
  float sumVoltage = 0.0;
  
  for (int i = 0; i < NUM_SAMPLES; i++) {
    int adcValue = analogRead(TDS_PIN);
    float voltage = adcValue * (VREF / ADC_RES);
    sumVoltage += voltage;
    
    Serial.print("Sample ");
    Serial.print(i + 1);
    Serial.print("/");
    Serial.print(NUM_SAMPLES);
    Serial.print(": ADC=");
    Serial.print(adcValue);
    Serial.print(", Voltage=");
    Serial.print(voltage, 4);
    Serial.println("V");
    
    delay(100);
  }
  
  // Average voltage reading
  calVoltage = sumVoltage / NUM_SAMPLES;
  
  // Calculate K value
  // EC = Voltage * K * (1 + 0.02 * (T-25))
  // For calibration at a known EC and measured voltage:
  // K = EC / (Voltage * (1 + 0.02 * (T-25)))
  float tempCoefficient = 1.0 + 0.02 * (waterTemp - 25.0);
  kValue = CAL_EC / (calVoltage * tempCoefficient);
  
  // Calculate TDS factor (typically EC * 0.5 for most freshwater)
  float tdsFactor = 0.5;
  
  // Show calibration results
  Serial.println("\n--- Calibration Results ---");
  Serial.print("Water Temperature: ");
  Serial.print(waterTemp, 2);
  Serial.println(" °C");
  
  Serial.print("Average ADC: ");
  Serial.println(int(calVoltage * ADC_RES / VREF));
  
  Serial.print("Average Voltage: ");
  Serial.print(calVoltage, 4);
  Serial.println(" V");
  
  Serial.print("Calibration EC: ");
  Serial.print(CAL_EC, 2);
  Serial.println(" μS/cm");
  
  Serial.print("Calculated K Value: ");
  Serial.println(kValue, 4);
  
  Serial.print("TDS Factor: ");
  Serial.println(tdsFactor, 2);
  
  Serial.println("\nUse these values in your main code:");
  Serial.println("namespace calibration {");
  Serial.println("  const float calVoltage = " + String(calVoltage, 6) + ";");
  Serial.println("  const float calEC = " + String(CAL_EC, 2) + ";");
  Serial.println("  const float kValue = " + String(kValue, 6) + ";");
  Serial.println("  const float tdsFactor = " + String(tdsFactor, 2) + ";");
  Serial.println("}");
  
  Serial.println("\n--- Live Readings ---");
  Serial.println("Now showing live EC/TDS readings with calibration values");
  Serial.println("Press Enter to restart calibration");
}

void updateTemperature() {
  tempSensors.requestTemperatures();
  float temp = tempSensors.getTempCByIndex(0);
  
  if (temp != DEVICE_DISCONNECTED_C && temp > -127) {
    waterTemp = temp;
    Serial.print("Water temperature: ");
    Serial.print(waterTemp, 2);
    Serial.println(" °C");
  } else {
    Serial.println("Error: Temperature sensor not responding or invalid reading.");
    Serial.println("Using default value of 25 °C");
    waterTemp = 25.0;
  }
}

float readEC() {
  // Take multiple readings and average them
  float sumVoltage = 0.0;
  for (int i = 0; i < 10; i++) {
    int adcValue = analogRead(TDS_PIN);
    float voltage = adcValue * (VREF / ADC_RES);
    sumVoltage += voltage;
    delay(10);
  }
  float voltage = sumVoltage / 10.0;
  
  // Calculate EC using the calibrated K value
  float tempCoefficient = 1.0 + 0.02 * (waterTemp - 25.0);
  float ec = voltage * kValue * tempCoefficient;
  
  return ec;
}

void waitForInput() {
  Serial.println("Press Enter to continue...");
  while (!Serial.available()) {
    delay(100);
  }
  while (Serial.available()) {
    Serial.read(); // Clear buffer
  }
}

void loop() {
  // Update temperature occasionally
  static unsigned long lastTempUpdate = 0;
  if (millis() - lastTempUpdate > 5000) {
    updateTemperature();
    lastTempUpdate = millis();
  }
  
  // Read EC and calculate TDS
  float ec = readEC();
  float tds = ec * 0.5; // TDS factor
  
  // Display live readings
  Serial.print("EC: ");
  Serial.print(ec, 2);
  Serial.print(" μS/cm | TDS: ");
  Serial.print(tds, 2);
  Serial.println(" ppm");
  
  // Check if user wants to restart calibration
  if (Serial.available()) {
    while (Serial.available()) {
      Serial.read(); // Clear buffer
    }
    startCalibration();
  }
  
  delay(1000);
}
