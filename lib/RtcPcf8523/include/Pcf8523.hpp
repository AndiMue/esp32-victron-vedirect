/******************************************************************************
 * @file Pcf8523.hpp
 *
 * RTC-Minitreiber für den NXP PCF8523 (z. B. Adafruit-Board ADA3295).
 * Projektneutral, schlank (keine Adafruit-RTClib nötig), nur Wire.
 *
 * Was der Treiber tut:
 *   - begin(): Chip suchen, 24-h-Modus, Uhr laufen lassen, Umschaltung auf
 *              die Knopfzelle und Batterie-Überwachung einschalten
 *              (ab Werk ist die Umschaltung beim PCF8523 AUS!)
 *   - read():  Zeit lesen; ungültig, wenn der Oszillator stand (OS-Flag,
 *              z. B. erster Start oder Stromausfall ohne Knopfzelle)
 *   - write(): Zeit stellen (löscht das OS-Flag)
 *   - batteryLow(): Knopfzelle schwach/fehlt (BLF-Flag)
 *
 * Register (Datenblatt PCF8523):
 *   0x00 Control_1   Bit 5 STOP, Bit 3 12_24 (0 = 24 h)
 *   0x02 Control_3   Bit 7..5 PM (000 = Umschaltung + Überwachung an),
 *                    Bit 2 BLF (Batterie schwach)
 *   0x03..0x09       Sekunden (Bit 7 = OS), Minuten, Stunden, Tag,
 *                    Wochentag, Monat, Jahr (BCD, Jahr 00..99 = 2000..2099)
 *
 * Der Teil "Pcf8523Codec" (Umrechnung der Register) ist ohne Arduino und
 * auf dem PC testbar. Die Klasse Pcf8523 (I²C) gibt es nur mit Arduino.
 *
 * Wire muss vorher gestartet sein, z. B. Wire.begin(21, 22).
 * Nicht threadsicher: aus nur EINER Task verwenden (oder selbst sperren).
 ******************************************************************************/

#pragma once

#include <cstdint>

#include "HardwareRtc.hpp"
#include "RtcTime.hpp"

namespace Pcf8523Codec
{
constexpr uint8_t Address   = 0x68;
constexpr uint8_t RegControl1 = 0x00;
constexpr uint8_t RegControl3 = 0x02;
constexpr uint8_t RegSeconds  = 0x03;
constexpr uint8_t TimeRegs    = 7;     ///< 0x03..0x09

constexpr uint8_t Control1Stop  = 0x20;
constexpr uint8_t Control1Mode12 = 0x08;
constexpr uint8_t Control3PmMask = 0xE0;
constexpr uint8_t Control3Blf    = 0x04;
constexpr uint8_t SecondsOs      = 0x80;

/// Zeit -> 7 Register (0x03..0x09). Ungültige Zeit -> false.
bool encode(const Rtc::DateTime& utc, uint8_t regs[TimeRegs]);

/// 7 Register -> Zeit. false = OS-Flag gesetzt (Uhr stand) oder Inhalt ungültig.
bool decode(const uint8_t regs[TimeRegs], Rtc::DateTime& utc);

/// Control_1 für den Betrieb: Uhr läuft, 24-h-Modus, Rest unverändert.
uint8_t runningControl1(uint8_t control1);

/// Control_3 für den Betrieb: Umschaltung auf Knopfzelle + Überwachung an.
uint8_t runningControl3(uint8_t control3);
} // namespace Pcf8523Codec

#ifdef ARDUINO
#include <Wire.h>

class Pcf8523 : public HardwareRtc
{
public:

    explicit Pcf8523(TwoWire& wire = Wire, uint8_t address = Pcf8523Codec::Address)
        : m_wire(wire), m_address(address)
    {
    }

    bool begin() override;
    bool read(Rtc::DateTime& utc) override;
    bool write(const Rtc::DateTime& utc) override;
    bool batteryLow() override;
    const char* name() const override { return "PCF8523"; }

private:

    bool readRegs(uint8_t reg, uint8_t* data, uint8_t count);
    bool writeRegs(uint8_t reg, const uint8_t* data, uint8_t count);

    TwoWire& m_wire;
    uint8_t  m_address;
};
#endif // ARDUINO
