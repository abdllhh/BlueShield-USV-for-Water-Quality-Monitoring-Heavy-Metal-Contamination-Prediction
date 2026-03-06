#include <SPI.h>
#include <LoRa.h>

#define SS 18
#define RST 14
#define DIO0 26

#define PH_PIN 34    
#define TDS_PIN 35   
#define EC_PIN 32    

const float VREF = 3.3;
const int ADC_RES = 4095;
float ph_offset = 0.0;
float ec_offset = 0.0;
float tds_factor = 0.5;


void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("Starting LoRa...");

    SPI.begin(5, 19, 27, 18);
    LoRa.setPins(SS, RST, DIO0);

    if (!LoRa.begin(920E6)) {
        Serial.println("LoRa init failed. Check wiring!");
        while (1);
    }

    Serial.println("LoRa Initialized Successfully!");
}

void loop() {
    int phAnalog = analogRead(PH_PIN);
    int tdsAnalog = analogRead(TDS_PIN);
    int ecAnalog = analogRead(EC_PIN);

    float phVoltage = phAnalog * (VREF / ADC_RES);
    float tdsVoltage = tdsAnalog * (VREF / ADC_RES);
    float ecVoltage = ecAnalog * (VREF / ADC_RES);

    float pHValue = 3.5 * phVoltage + ph_offset;
    float ecValue = (ecVoltage - 0.4) / 0.19 + ec_offset;
    float tdsValue = ecValue * tds_factor;

    String dataToSend = "pH:" + String(pHValue) + ",EC:" + String(ecValue) + ",TDS:" + String(tdsValue);

    LoRa.setSpreadingFactor(12);   // Max SF for best range
    LoRa.setSignalBandwidth(62.5E3);  // Narrow bandwidth for better range
    LoRa.setCodingRate4(8);   // Max error correction
    LoRa.setPreambleLength(12);  // Improve reception
    LoRa.setTxPower(20, PA_OUTPUT_PA_BOOST_PIN);  // Max transmission power

    Serial.println("Sending packet...");

    LoRa.beginPacket();
    LoRa.print("Hello from ESP32");
    LoRa.endPacket();  // Send the packet

    Serial.println("Packet Sent!");
    delay(2000);  // Send message every 2 seconds
}
