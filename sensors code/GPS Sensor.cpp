#include <HardwareSerial.h>
#include <TinyGPS++.h>

static const int RXPin = 16, TXPin = 17; // GPS TX→GPIO16, RX→GPIO17
static const uint32_t GPSBaud = 9600;

TinyGPSPlus gps;
HardwareSerial gpsSerial(1);

void setup() {
  Serial.begin(115200);
  gpsSerial.begin(GPSBaud, SERIAL_8N1, RXPin, TXPin);
  Serial.println("TinyGPS++ Parser Running...");
}

void loop() {
  while (gpsSerial.available() > 0) {
    char c = gpsSerial.read();
    gps.encode(c); // feed data into TinyGPS++
  }

  if (gps.location.isUpdated()) {
    Serial.println("\n=== GPS Fix Received ===");
    Serial.print("Latitude: "); Serial.println(gps.location.lat(), 6);
    Serial.print("Longitude: "); Serial.println(gps.location.lng(), 6);
    Serial.print("Altitude: "); Serial.print(gps.altitude.meters()); Serial.println(" m");
    Serial.print("Speed: "); Serial.print(gps.speed.kmph()); Serial.println(" km/h");
    Serial.print("Satellites: "); Serial.println(gps.satellites.value());

    Serial.print("Date: ");
    Serial.print(gps.date.day()); Serial.print("/");
    Serial.print(gps.date.month()); Serial.print("/");
    Serial.println(gps.date.year());

    Serial.print("Time: ");
    Serial.print(gps.time.hour()); Serial.print(":");
    Serial.print(gps.time.minute()); Serial.print(":");
    Serial.println(gps.time.second());

    Serial.println("=========================\n");
  }

  delay(1000);
}
