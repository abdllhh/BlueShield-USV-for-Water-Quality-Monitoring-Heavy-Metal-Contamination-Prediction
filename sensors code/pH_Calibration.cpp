/*
 * ESP32 pH Sensor Calibration Sketch
 * Designed for calibration with pH 10.0 and pH 4.0 buffer solutions
 */

#define PH_PIN 34       // Analog pin connected to pH sensor
const float VREF = 3.3; // Reference voltage
const int ADC_RES = 4095; // 12-bit ADC resolution

// Variables for calibration
float voltage_pH10 = 0.0;
float voltage_pH4 = 0.0;
float pH_coefficient = 0.0;
float pH_offset = 0.0;

// For averaging readings
const int NUM_SAMPLES = 20;

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10); // Wait for serial connection
  
  analogReadResolution(12); // Set ADC resolution to 12 bits
  
  Serial.println("\n\n=== ESP32 pH Sensor Calibration ===");
  Serial.println("Follow these steps:");
  Serial.println("1. When prompted, place probe in pH 10.0 solution and press Enter");
  Serial.println("2. When prompted, place probe in pH 4.0 solution and press Enter");
  Serial.println("3. Calibration values will be displayed");
  
  startCalibration();
}

void startCalibration() {
  Serial.println("\nCalibration starting...");
  
  // Step 1: pH 10.0 calibration
  Serial.println("\n--- pH 10.0 Calibration ---");
  Serial.println("1. Rinse your pH probe with distilled water and dry it");
  Serial.println("2. Place probe in pH 10.0 solution");
  Serial.println("3. Press Enter when ready");
  
  waitForInput();
  
  // Take readings for pH 10.0
  voltage_pH10 = takePHReadings();
  Serial.print("pH 10.0 Voltage: ");
  Serial.print(voltage_pH10, 4);
  Serial.println(" V");
  
  // Step 2: pH 4.0 calibration
  Serial.println("\n--- pH 4.0 Calibration ---");
  Serial.println("1. Rinse your pH probe with distilled water and dry it");
  Serial.println("2. Place probe in pH 4.0 solution");
  Serial.println("3. Press Enter when ready");
  
  waitForInput();
  
  // Take readings for pH 4.0
  voltage_pH4 = takePHReadings();
  Serial.print("pH 4.0 Voltage: ");
  Serial.print(voltage_pH4, 4);
  Serial.println(" V");
  
  // Calculate calibration values
  calculateCalibration();
  
  // Show live readings
  Serial.println("\n--- Live pH Readings ---");
  Serial.println("Now showing pH readings with calibration values");
  Serial.println("Press Enter to restart calibration");
}

float takePHReadings() {
  Serial.println("Taking readings, please wait...");
  
  // Wait a moment for stability
  delay(2000);
  
  // Take multiple samples and average them
  float sum = 0;
  for (int i = 0; i < NUM_SAMPLES; i++) {
    int adcValue = analogRead(PH_PIN);
    float voltage = adcValue * (VREF / ADC_RES);
    sum += voltage;
    
    Serial.print("Sample ");
    Serial.print(i + 1);
    Serial.print(": ");
    Serial.print(voltage, 4);
    Serial.println(" V");
    
    delay(100);
  }
  
  return sum / NUM_SAMPLES;
}

void calculateCalibration() {
  // Calculate slope (coefficient)
  pH_coefficient = (10.0 - 4.0) / (voltage_pH10 - voltage_pH4);
  
  // Calculate intercept (offset)
  pH_offset = 10.0 - (pH_coefficient * voltage_pH10);
  
  Serial.println("\n--- Calibration Results ---");
  Serial.print("pH 10.0 Voltage: ");
  Serial.print(voltage_pH10, 4);
  Serial.println(" V");
  
  Serial.print("pH 4.0 Voltage: ");
  Serial.print(voltage_pH4, 4);
  Serial.println(" V");
  
  Serial.print("Coefficient: ");
  Serial.println(pH_coefficient, 4);
  
  Serial.print("Offset: ");
  Serial.println(pH_offset, 4);
  
  Serial.println("\nUse these values in your main code:");
  Serial.println("const float pH_COEFFICIENT = " + String(pH_coefficient, 4) + ";");
  Serial.println("const float pH_OFFSET = " + String(pH_offset, 4) + ";");
  Serial.println("float phValue = pH_COEFFICIENT * voltage + pH_OFFSET;");
}

void waitForInput() {
  Serial.println("Press Enter to continue...");
  while (!Serial.available()) {
    delay(100);
  }
  while (Serial.available()) {
    Serial.read(); // Clear buffer
  }
  Serial.println("Starting...");
}

void loop() {
  // Show live pH readings
  float voltage = 0;
  for (int i = 0; i < 10; i++) {
    voltage += analogRead(PH_PIN) * (VREF / ADC_RES);
    delay(10);
  }
  voltage /= 10.0;
  
  float ph = pH_coefficient * voltage + pH_offset;
  
  Serial.print("Voltage: ");
  Serial.print(voltage, 4);
  Serial.print(" V | pH: ");
  Serial.println(ph, 2);
  
  // Check if user wants to restart calibration
  if (Serial.available()) {
    while (Serial.available()) {
      Serial.read(); // Clear buffer
    }
    startCalibration();
  }
  
  delay(500);
}
