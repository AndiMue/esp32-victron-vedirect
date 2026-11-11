/******************************************************************************
 * @file RtcZones.hpp
 *
 * Teil des RTC-Minitreibers (lib/RtcPcf8523), projektneutral.
 *
 * Auswahlliste der Zeitzonen (POSIX-TZ-Regeln inkl. Sommerzeit) und
 * Prüfung von NTP-Servernamen. Die RTC selbst läuft immer in UTC; die
 * Zeitzone wird nur für die Anzeige verwendet (ESP32: setenv("TZ") + tzset()
 * bzw. configTzTime()).
 *
 * Reines C++ ohne Arduino -> auf dem PC testbar.
 ******************************************************************************/

#pragma once

#include <cstddef>

namespace Rtc
{

struct TimeZone
{
    const char* key;     ///< kurzer Schlüssel zum Speichern (NVS), z. B. "berlin"
    const char* label;   ///< Anzeige, z. B. "Deutschland (MEZ/MESZ)"
    const char* posix;   ///< POSIX-TZ-Regel, z. B. "CET-1CEST,M3.5.0,M10.5.0/3"
};

/// Alle wählbaren Zeitzonen. Index 0 = Standard (Deutschland).
const TimeZone* timeZones(std::size_t& count);

/// Zeitzone zu einem Schlüssel; unbekannt/leer -> Standard (Index 0).
const TimeZone& timeZone(const char* key);

/// Vorschläge für den NTP-Server. Index 0 = Standard ("pool.ntp.org").
const char* const* ntpServers(std::size_t& count);

/// Zulässiger Hostname bzw. IPv4-Adresse für NTP? (1..63 Zeichen,
/// nur Buchstaben, Ziffern, '.', '-'; nicht mit '.'/'-' beginnen)
bool isValidHostName(const char* name);

} // namespace Rtc
