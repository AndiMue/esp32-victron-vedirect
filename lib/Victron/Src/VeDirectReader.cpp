/******************************************************************************
 * @file VeDirectReader.cpp
 ******************************************************************************/

/*
 * Der Reader benötigt Arduino (HardwareSerial). In der PC-Testumgebung
 * (env:native) gibt es kein Arduino; PlatformIO übersetzt aber alle Dateien
 * der Bibliothek "Victron". Daher wird der Inhalt dort ausgeblendet.
 * Auf dem ESP32 ist ARDUINO immer definiert -> keine Änderung des Verhaltens.
 */
#ifdef ARDUINO

#include "VeDirectReader.hpp"

/******************************************************************************
 * Konstruktor
 ******************************************************************************/

VeDirectReader::VeDirectReader(
    HardwareSerial& serial,
    uint8_t rxPin,
    uint8_t txPin,
    uint32_t baudrate)
    :
    m_serial(serial),
    m_rxPin(rxPin),
    m_txPin(txPin),
    m_baudrate(baudrate)
{
}

/******************************************************************************
 * UART initialisieren
 ******************************************************************************/

void VeDirectReader::begin()
{
    m_serial.begin(
        m_baudrate,
        SERIAL_8N1,
        m_rxPin,
        m_txPin);
}

/******************************************************************************
 * Prüfen ob Daten vorhanden sind
 ******************************************************************************/

bool VeDirectReader::available() const
{
    return m_serial.available();
}

/******************************************************************************
 * Ein Byte lesen
 ******************************************************************************/

char VeDirectReader::read()
{
    return static_cast<char>(m_serial.read());
}

void VeDirectReader::flush()
{
    while (m_serial.available())
    {
        m_serial.read();
    }
}
#endif // ARDUINO
