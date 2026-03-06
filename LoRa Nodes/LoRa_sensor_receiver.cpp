//Receiver code

#include <SPI.h>
#include <LoRa.h>

#define SS 18
#define RST 14
#define DIO0 26
#define LED 2  // Built-in LED on ESP32

void setup() {
    Serial.begin(115200);
    delay(1000);

    pinMode(LED, OUTPUT);
    
    Serial.println("Starting LoRa Receiver...");

    // Manually initialize SPI
    SPI.begin(5, 19, 27, 18);  // SCK, MISO, MOSI, SS
    LoRa.setPins(SS, RST, DIO0);
    LoRa.setSPIFrequency(1E6); // Set SPI clock to 1MHz

    if (!LoRa.begin(920E6)) {  // Adjust frequency based on region
        Serial.println("LoRa init failed. Check connections.");
        while (1);
    }

    Serial.println("LoRa Receiver Initialized!");
}

void loop() {

    LoRa.setSpreadingFactor(12);   // Max SF for best range
    LoRa.setSignalBandwidth(62.5E3);  // Narrow bandwidth for better range
    LoRa.setCodingRate4(8);   // Max error correction
    LoRa.setPreambleLength(12);  // Improve reception
    LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);  // Max transmission power

    
   int packetSize = LoRa.parsePacket();
    if (packetSize) {
        String receivedData = "";
        while (LoRa.available()) {
            receivedData += (char)LoRa.read();
        }

        Serial.println("Received Data: " + receivedData);

        // Parse received data (Expected format: "pH:7.1,EC:500,TDS:300")
        float pH, EC, TDS;
        sscanf(receivedData.c_str(), "pH:%f,EC:%f,TDS:%f", &pH, &EC, &TDS);

        Serial.print("pH Value: "); Serial.println(pH);
        Serial.print("EC Value: "); Serial.println(EC);
        Serial.print("TDS Value: "); Serial.println(TDS);
        Serial.println("-----------------------------");
        
        // Blink LED to indicate reception
        digitalWrite(LED, HIGH);
        delay(500);
        digitalWrite(LED, LOW);
    }
}
