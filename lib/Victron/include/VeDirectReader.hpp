/******************************************************************************
 * @file VeDirectReader.hpp
 *
 * VE.Direct UART Reader
 *
 * Verantwortlich ausschließlich für den Empfang von Bytes über VE.Direct.
 *
 * Aufgaben:
 *  - UART initialisieren
 *  - Bytes empfangen
 *
 * Keine Parser- oder Geräte-Logik.
 ******************************************************************************/

#pragma once

#include <Arduino.h>

#include <cstddef>
class VeDirectReader
{
public:

    VeDirectReader(
        HardwareSerial& serial,
        uint8_t rxPin,
        uint8_t txPin,
        uint32_t baudrate = 19200);

    void begin();
    bool available() const;
    char read();
    void flush();

    /// W4b: Text an das Gerät senden (HEX-Anfragen, nur lesend, siehe VeDirectHex).
    void write(const char* text, std::size_t length);

private:

    HardwareSerial& m_serial;

    uint8_t m_rxPin;
    uint8_t m_txPin;
    uint32_t m_baudrate;
};