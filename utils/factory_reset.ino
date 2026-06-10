#include <Arduino.h>
#include <HardwareSerial.h>
#include <RoboCore_SMW_SX1276M0.h>

HardwareSerial LoRaSerial(2);
#define RXD2 16
#define TXD2 17

SMW_SX1276M0 lorawan(LoRaSerial);

void setup() {
  Serial.begin(115200);
  delay(500);
  LoRaSerial.begin(115200, SERIAL_8N1, RXD2, TXD2);

  lorawan.setPinReset(5);
  lorawan.reset();

  Serial.println("Fazendo factory reset...");
  delay(2000);

  LoRaSerial.println("AT+FRESET");
  delay(5000);

  while (LoRaSerial.available()) {
    Serial.write(LoRaSerial.read());
  }

  Serial.println("Pronto!");
}

void loop() {}
