/******************************************************************************
 * @file RtcTime.hpp
 *
 * Teil des RTC-Minitreibers (lib/RtcPcf8523), projektneutral.
 *
 * Datum/Uhrzeit für Echtzeituhren: Umrechnung zwischen Kalenderdatum und
 * Unix-Zeit (Sekunden seit 1.1.1970 UTC) sowie BCD (Format der RTC-Chips).
 *
 * Grundsatz: Die RTC läuft in UTC. Zeitzone und Sommerzeit werden erst bei
 * der Anzeige berücksichtigt (auf dem ESP32 über die POSIX-TZ-Regel der
 * C-Bibliothek). So kann beim Umstellen auf Sommer-/Winterzeit nichts
 * doppelt oder falsch laufen.
 *
 * Gültiger Bereich: 2000-01-01 bis 2099-12-31 (zweistelliges Jahr der Chips).
 *
 * Reines C++ ohne Arduino -> auf dem PC testbar.
 ******************************************************************************/

#pragma once

#include <cstdint>

namespace Rtc
{

/// Kalenderdatum und Uhrzeit (UTC).
struct DateTime
{
    uint16_t year   = 2000;   ///< 2000..2099
    uint8_t  month  = 1;      ///< 1..12
    uint8_t  day    = 1;      ///< 1..31
    uint8_t  hour   = 0;      ///< 0..23
    uint8_t  minute = 0;      ///< 0..59
    uint8_t  second = 0;      ///< 0..59

    /// Liegen alle Felder im gültigen Bereich (inkl. Monatslänge, Schaltjahr)?
    bool isValid() const;
};

/// Unix-Zeit von 2000-01-01 00:00:00 UTC.
constexpr uint32_t Epoch2000 = 946684800UL;

bool    isLeapYear(uint16_t year);
uint8_t daysInMonth(uint16_t year, uint8_t month);   ///< 0 bei ungültigem Monat

/// Wochentag 0 = Sonntag .. 6 = Samstag (wie PCF8523/DS3231-Zählung ab 0).
uint8_t weekday(const DateTime& t);

/// Datum -> Unix-Zeit. Ungültiges Datum -> 0.
uint32_t toUnix(const DateTime& t);

/// Unix-Zeit -> Datum. Außerhalb 2000..2099 -> false.
bool fromUnix(uint32_t unix, DateTime& out);

/// BCD-Hilfen (z. B. 0x59 <-> 59).
uint8_t toBcd(uint8_t value);
uint8_t fromBcd(uint8_t bcd);

} // namespace Rtc
