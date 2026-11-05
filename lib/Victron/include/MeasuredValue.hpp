/******************************************************************************
 * @file MeasuredValue.hpp
 *
 * Ergebnis der Umrechnung eines VE.Direct-Feldes (ValueConverter).
 *
 * Wichtig für die spätere Relais-/SOC-Logik: Nur Werte mit
 * status == ValueStatus::Valid dürfen verwendet werden. Insbesondere wird
 * "---" (z. B. SOC bei nicht synchronisiertem Shunt) NIE zu 0.
 ******************************************************************************/

#pragma once

#include <cstdint>

#include "FieldDefinition.hpp"

enum class ValueStatus : uint8_t
{
    Valid,          ///< Wert gültig
    NotAvailable,   ///< Gerät sendet "---" (z. B. nicht synchronisiert, kein Sensor)
    Infinite,       ///< TTG = -1 (Batterie wird nicht entladen)
    Invalid,        ///< Rohtext passt nicht zum erwarteten Format
    UnknownField    ///< Label ist im FieldCatalog nicht bekannt
};

struct MeasuredValue
{
    const FieldDefinition* definition = nullptr; ///< nullptr bei UnknownField
    ValueStatus status = ValueStatus::Invalid;

    /// Ganzzahliger Rohwert, wie übertragen (z. B. 12327 für "V 12327").
    /// Bei Bitmasken/PID der Zahlenwert, bei OnOff 1/0, bei FW/FWE die
    /// Versionsnummer (z. B. 419). Bei Text immer 0.
    int64_t raw = 0;

    /// Physikalischer Wert: raw / 10^decimals (z. B. 12.327 für 12327 mV).
    /// Für die Anzeige wird raw verwendet (exakt, ohne Rundungsfehler).
    float value = 0.0f;

    bool isValid() const { return status == ValueStatus::Valid; }
};
