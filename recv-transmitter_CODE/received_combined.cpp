#include <SPI.h>
#include <LoRa.h>
#include <WiFi.h>
#include <HTTPClient.h>
// FreeRTOS includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// --- Wi-Fi credentials ---
const char* ssid = "AzzaniPhone";
const char* password = "Azzan123";

// --- Flask API endpoint ---
const char* serverURL = "http://172.20.10.9:5000/lora";  // Replace with your actual server URL

// --- LoRa configuration ---
#define SS 18
#define RST 14
#define DIO0 26

// --- LED indicator ---
#define LED_PIN 2  // Onboard LED

// --- FreeRTOS Task Handles ---
TaskHandle_t taskLoRaReceive = NULL;
TaskHandle_t taskWiFiMonitor = NULL;
TaskHandle_t taskAPIForward = NULL;

// --- FreeRTOS Synchronization Objects ---
SemaphoreHandle_t xSPIMutex = NULL;
SemaphoreHandle_t xWiFiMutex = NULL;
SemaphoreHandle_t xSerialMutex = NULL;
QueueHandle_t xLoRaQueue = NULL;

// Message buffer size
#define MAX_MESSAGE_LENGTH 256

// WiFi connection status
bool isWiFiConnected = false;

// Function to safely print to Serial with mutex protection
void safeSerial(String message) {
  if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Serial.println(message);
    xSemaphoreGive(xSerialMutex);
  }
}

// Task to monitor and maintain WiFi connection
void TaskWiFiMonitor(void *pvParameters) {
  const TickType_t xFrequency = pdMS_TO_TICKS(10000); // Check every 10 seconds
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  for (;;) {
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
    
    if (WiFi.status() != WL_CONNECTED) {
      if (xSemaphoreTake(xWiFiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        isWiFiConnected = false;
        xSemaphoreGive(xWiFiMutex);
      }
      
      safeSerial("WiFi disconnected! Reconnecting...");
      WiFi.disconnect();
      
      // Attempt to reconnect
      WiFi.begin(ssid, password);
      
      // Wait for connection with timeout
      int connectionAttempts = 0;
      while (WiFi.status() != WL_CONNECTED && connectionAttempts < 20) {
        vTaskDelay(pdMS_TO_TICKS(500));
        connectionAttempts++;
      }
      
      if (WiFi.status() == WL_CONNECTED) {
        if (xSemaphoreTake(xWiFiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
          isWiFiConnected = true;
          xSemaphoreGive(xWiFiMutex);
        }
        safeSerial("WiFi reconnected. IP: " + WiFi.localIP().toString());
      } else {
        safeSerial("Failed to reconnect to WiFi after multiple attempts.");
      }
    } else {
      // WiFi is connected, update status if needed
      if (xSemaphoreTake(xWiFiMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (!isWiFiConnected) {
          isWiFiConnected = true;
          safeSerial("WiFi connection stable. IP: " + WiFi.localIP().toString());
        }
        xSemaphoreGive(xWiFiMutex);
      }
    }
    
    // Print task statistics
    // Uncomment for debugging
    /*
    if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      Serial.print("WiFi Task Stack: ");
      Serial.print(uxTaskGetStackHighWaterMark(NULL));
      Serial.print(", Heap: ");
      Serial.println(xPortGetFreeHeapSize());
      xSemaphoreGive(xSerialMutex);
    }
    */
  }
}

// Task to receive LoRa packets
void TaskLoRaReceive(void *pvParameters) {
  for (;;) {
    // Take SPI mutex to access LoRa module
    if (xSemaphoreTake(xSPIMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      int packetSize = LoRa.parsePacket();
      
      if (packetSize > 0) {
        String received = "";
        
        // Read the packet
        while (LoRa.available()) {
          received += (char)LoRa.read();
        }
        
        xSemaphoreGive(xSPIMutex); // Release mutex before processing
        
        // Flash LED to indicate packet received
        digitalWrite(LED_PIN, HIGH);
        
        safeSerial("Received via LoRa: " + received);
        
        // Send to queue for API forwarding task
        if (received.length() > 0) {
          char* messageBuffer = new char[received.length() + 1];
          strcpy(messageBuffer, received.c_str());
          
          // Send to queue with timeout
          if (xQueueSend(xLoRaQueue, (void*)&messageBuffer, pdMS_TO_TICKS(1000)) != pdPASS) {
            // Failed to send to queue (maybe full)
            safeSerial("Failed to queue message - queue might be full");
            delete[] messageBuffer; // Clean up if we couldn't queue it
          } else {
            safeSerial("Message queued for API forwarding");
          }
        }
        
        // Turn off LED after short delay
        vTaskDelay(pdMS_TO_TICKS(100));
        digitalWrite(LED_PIN, LOW);
      } else {
        // No packet available, release mutex
        xSemaphoreGive(xSPIMutex);
      }
    }
    
    // Small delay to prevent this task from hogging CPU
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Task to forward messages to API
void TaskAPIForward(void *pvParameters) {
  char* receivedMessage;
  
  for (;;) {
    // Wait for a message in the queue
    if (xQueueReceive(xLoRaQueue, &receivedMessage, portMAX_DELAY) == pdPASS) {
      String message(receivedMessage);
      
      // Check WiFi status
      bool wifiStatus = false;
      if (xSemaphoreTake(xWiFiMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        wifiStatus = isWiFiConnected;
        xSemaphoreGive(xWiFiMutex);
      }
      
      if (wifiStatus) {
        HTTPClient http;
        
        // Start HTTP connection with timeout and retry mechanism
        bool httpInitiated = false;
        int retries = 0;
        
        while (!httpInitiated && retries < 3) {
          http.begin(serverURL);
          http.addHeader("Content-Type", "application/json");
          httpInitiated = true;
          
          // Create JSON payload
          String jsonPayload = "{\"message\": \"" + message + "\"}";
          
          safeSerial("Sending to API: " + jsonPayload);
          
          // Send POST with timeout
          http.setTimeout(10000); // 10-second timeout
          int httpCode = http.POST(jsonPayload);
          
          if (httpCode > 0) {
            safeSerial("HTTP Response code: " + String(httpCode));
            
            if (httpCode == HTTP_CODE_OK) {
              String payload = http.getString();
              safeSerial("Server response: " + payload);
            }
            
            // Success, break out of retry loop
            break;
          } else {
            safeSerial("HTTP Error: " + http.errorToString(httpCode));
            retries++;
            
            if (retries < 3) {
              safeSerial("Retrying API call... (" + String(retries) + "/3)");
              vTaskDelay(pdMS_TO_TICKS(1000)); // Wait before retry
            }
          }
          
          http.end();
        }
        
        if (retries >= 3) {
          safeSerial("Failed to send data to API after multiple attempts");
        }
      } else {
        safeSerial("WiFi disconnected! Cannot forward data to API.");
      }
      
      // Free the dynamically allocated memory
      delete[] receivedMessage;
    }
  }
}

void setup() {
  // Initialize serial communication
  Serial.begin(115200);
  delay(1000); // Short delay for serial to initialize
  
  // Initialize LED
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  Serial.println("\nESP32 LoRa Receiver with FreeRTOS");
  
  // Create FreeRTOS synchronization objects
  xSPIMutex = xSemaphoreCreateMutex();
  xWiFiMutex = xSemaphoreCreateMutex();
  xSerialMutex = xSemaphoreCreateMutex();
  xLoRaQueue = xQueueCreate(10, sizeof(char*)); // Queue can hold 10 messages
  
  // Connect to WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  
  // Wait for WiFi with timeout
  int wifiTimeout = 0;
  while (WiFi.status() != WL_CONNECTED && wifiTimeout < 20) {
    delay(500);
    Serial.print(".");
    wifiTimeout++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    isWiFiConnected = true;
    Serial.println("\nConnected to WiFi!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nFailed to connect to WiFi!");
  }
  
  // Initialize LoRa
  SPI.begin(5, 19, 27, 18);  // SCK, MISO, MOSI, SS
  
  xSemaphoreTake(xSPIMutex, portMAX_DELAY); // Take SPI mutex for initialization
  
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
  
  // Match transmitter LoRa settings
  LoRa.setSpreadingFactor(9);
  LoRa.setSignalBandwidth(125E3);
  LoRa.setCodingRate4(5);
  LoRa.setPreambleLength(8);
  LoRa.setTxPower(14);
  
  xSemaphoreGive(xSPIMutex); // Release SPI mutex
  
  Serial.println("LoRa Receiver initialized successfully!");
  
  // Create FreeRTOS tasks
  xTaskCreatePinnedToCore(
    TaskWiFiMonitor,  // Function to implement the task
    "WiFiMonitor",    // Name of the task
    4096,             // Stack size in words (larger for WiFi operations)
    NULL,             // Task input parameter
    1,                // Priority of the task
    &taskWiFiMonitor, // Task handle
    0                 // Core where the task should run (0 = WiFi core)
  );
  
  xTaskCreatePinnedToCore(
    TaskLoRaReceive,  // Function to implement the task
    "LoRaReceive",    // Name of the task
    2048,             // Stack size in words
    NULL,             // Task input parameter
    2,                // Priority of the task (higher priority than WiFi monitoring)
    &taskLoRaReceive, // Task handle
    1                 // Core where the task should run (1 = non-WiFi core)
  );
  
  xTaskCreatePinnedToCore(
    TaskAPIForward,   // Function to implement the task
    "APIForward",     // Name of the task
    4096,             // Stack size in words (larger for HTTP operations)
    NULL,             // Task input parameter
    1,                // Priority of the task
    &taskAPIForward,  // Task handle
    0                 // Core where the task should run (0 = WiFi core)
  );
  
  Serial.println("FreeRTOS tasks started!");
}

void loop() {
  // The main loop now only monitors system health
  static unsigned long lastSystemInfoTime = 0;
  
  if (millis() - lastSystemInfoTime >= 30000) { // Every 30 seconds
    lastSystemInfoTime = millis();
    
    if (xSemaphoreTake(xSerialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      Serial.println("\n=== SYSTEM INFO ===");
      Serial.print("Free heap: ");
      Serial.print(xPortGetFreeHeapSize());
      Serial.print(" bytes, Min free heap: ");
      Serial.print(xPortGetMinimumEverFreeHeapSize());
      Serial.println(" bytes");
      
      // Queue state
      Serial.print("Messages in queue: ");
      Serial.print(uxQueueMessagesWaiting(xLoRaQueue));
      Serial.print("/");
      Serial.println(10); // Queue capacity
      
      // WiFi status
      Serial.print("WiFi status: ");
      Serial.println(WiFi.status() == WL_CONNECTED ? "Connected" : "Disconnected");
      
      // Task states
      if (taskLoRaReceive != NULL) {
        Serial.print("LoRa Task state: ");
        Serial.print(eTaskGetState(taskLoRaReceive) == eRunning ? "Running" : 
                    (eTaskGetState(taskLoRaReceive) == eBlocked ? "Blocked" : 
                    (eTaskGetState(taskLoRaReceive) == eSuspended ? "Suspended" : "Ready")));
        Serial.print(", Stack HWM: ");
        Serial.println(uxTaskGetStackHighWaterMark(taskLoRaReceive));
      }
      
      if (taskAPIForward != NULL) {
        Serial.print("API Task state: ");
        Serial.print(eTaskGetState(taskAPIForward) == eRunning ? "Running" : 
                    (eTaskGetState(taskAPIForward) == eBlocked ? "Blocked" : 
                    (eTaskGetState(taskAPIForward) == eSuspended ? "Suspended" : "Ready")));
        Serial.print(", Stack HWM: ");
        Serial.println(uxTaskGetStackHighWaterMark(taskAPIForward));
      }
      
      Serial.println("==================");
      xSemaphoreGive(xSerialMutex);
    }
  }
  
  // Yield to other tasks
  vTaskDelay(pdMS_TO_TICKS(1000));
}
