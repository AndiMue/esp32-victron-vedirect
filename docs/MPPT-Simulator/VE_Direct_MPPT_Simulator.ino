/*
  VE.Direct MPPT simulator for Arduino Pro Mini 328P (5 V / 16 MHz)

  UART output: 19200 baud, 8N1, hardware Serial (pins D0/RX and D1/TX).
  Connect D1/TX to the receiver's VE.Direct RX input, and connect GNDs.
  The TX signal is 5 V logic; use suitable level conversion if the receiver
  requires 3.3 V. This sketch only transmits; it does not use USB serial
  simultaneously on the Pro Mini's hardware UART.

  Sends one VE.Direct text frame per second, with a valid packet checksum.
*/

#include <Arduino.h>

static uint32_t frameCounter = 0;

static void appendField(String &packet, const char *key, const String &value) {
  packet += key;
  packet += '\t';
  packet += value;
  packet += "\r\n";
}

static void appendField(String &packet, const char *key, const char *value) {
  packet += key;
  packet += '\t';
  packet += value;
  packet += "\r\n";
}

static void sendMpptFrame() {
  // Repeatable, slowly changing sample values for a fictional 12 V system.
  const uint16_t vpv = 18500 + ((frameCounter % 9) * 100);  // mV
  const uint16_t vbat = 13400 + ((frameCounter % 7) * 20);  // mV
  const uint16_t ppv = 72 + ((frameCounter % 6) * 8);       // W
  const int16_t  ibat = (int16_t)((ppv * 1000L) / vbat);    // mA, approx.

  String packet;
  packet.reserve(300);

  // Common VE.Direct MPPT text fields. Values are sent as ASCII.
  appendField(packet, "PID",   "0xA057");          // SmartSolar MPPT 100/50 ID
  appendField(packet, "FW",    "159");
  appendField(packet, "SER#",  "HQ25419R9R3");
  appendField(packet, "V",     String(vbat));      // Battery voltage, mV
  appendField(packet, "I",     String(ibat));      // Battery current, mA
  appendField(packet, "VPV",   String(vpv));       // PV voltage, mV
  appendField(packet, "PPV",   String(ppv));       // PV power, W
  appendField(packet, "CS",    "3");               // Bulk/Absorption/Float state code: 3 = Bulk
  appendField(packet, "MPPT",  "2");               // 2 = MPPT tracker active
  appendField(packet, "OR",    "0x00000001");       // Off reason
  appendField(packet, "ERR",   "0");               // No error
  appendField(packet, "LOAD",  "OFF");             // Load output state
  appendField(packet, "IL",    "0");               // Load current, A
  appendField(packet, "H19",   String((frameCounter * ppv) / 36000UL)); // Yield total, 0.01 kWh units
  appendField(packet, "H20",   "0");               // Yield today, 0.01 kWh units
  appendField(packet, "H21",   String(ppv));        // Maximum power today, W
  appendField(packet, "H22",   "0");               // Yield yesterday, 0.01 kWh units
  appendField(packet, "H23",   "0");               // Maximum power yesterday, W
  appendField(packet, "HSDS",  "0");               // Day sequence number

  // VE.Direct checksum: sum of every byte in the frame, including the
  // "Checksum<TAB>" label and final CRLF, must equal zero modulo 256.
  const char checksumLabel[] = "Checksum\t";
  uint8_t sum = 0;

  for (size_t i = 0; i < packet.length(); ++i) {
    sum += (uint8_t)packet[i];
  }
  for (size_t i = 0; i < sizeof(checksumLabel) - 1; ++i) {
    sum += (uint8_t)checksumLabel[i];
  }
  sum += (uint8_t)'\r';
  sum += (uint8_t)'\n';

  const uint8_t checksum = (uint8_t)(0 - sum);

  Serial.print(packet);
  Serial.print(checksumLabel);
  Serial.write(checksum);   // Binary checksum byte, not decimal ASCII
  Serial.print("\r\n");

  ++frameCounter;
}

void setup() {
  Serial.begin(19200);
}

void loop() {
  sendMpptFrame();
  delay(1000);
}