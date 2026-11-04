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

private:

    HardwareSerial& m_serial;

    uint8_t m_rxPin;
    uint8_t m_txPin;
    uint32_t m_baudrate;
};