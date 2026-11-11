/******************************************************************************
 * @file HardwareRtc.hpp
 *
 * Teil des RTC-Minitreibers (lib/RtcPcf8523), projektneutral.
 *
 * Gemeinsame Schnittstelle für Echtzeituhr-Chips. Das Programm arbeitet nur
 * hiermit; ein Umstieg z. B. vom PCF8523 auf einen DS3231 braucht nur eine
 * weitere kleine Klasse (Projektinhaber 11.11.2026: "Umstieg vorhalten").
 *
 * Zeiten sind immer UTC.
 ******************************************************************************/

#pragma once

#include "RtcTime.hpp"

class HardwareRtc
{
public:

    virtual ~HardwareRtc() = default;

    /// Chip suchen und für den Betrieb einstellen. false = nicht gefunden.
    virtual bool begin() = 0;

    /// Zeit lesen. false = Lesefehler ODER Zeit ungültig (Uhr stand, z. B.
    /// erster Start oder Stromausfall ohne Knopfzelle).
    virtual bool read(Rtc::DateTime& utc) = 0;

    /// Zeit stellen (macht die Zeit wieder gültig). false = Schreibfehler.
    virtual bool write(const Rtc::DateTime& utc) = 0;

    /// Knopfzelle schwach oder fehlt (soweit der Chip das erkennt).
    virtual bool batteryLow() = 0;

    /// Name des Chips für Anzeigen (z. B. "PCF8523").
    virtual const char* name() const = 0;
};
