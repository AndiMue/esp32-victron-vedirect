/******************************************************************************
 * @file ValueFormatter.hpp
 *
 * Erzeugt menschenlesbaren Text aus einem umgerechneten Wert, z. B.
 *   V    12327       -> "12,327 V"
 *   SOC  1000        -> "100,0 %"
 *   CS   3           -> "Bulk (Konstantstrom)"
 *   OR   0x00000001  -> "Keine Eingangsleistung"
 *   PID  0xC035      -> "SmartShunt 500A/50mV"
 *
 * Zahlen werden exakt aus dem ganzzahligen Rohwert formatiert (keine
 * Float-Rundung), mit deutschem Dezimalkomma.
 *
 * Kennt kein Serial -> auf dem PC testbar. Texte sind UTF-8.
 ******************************************************************************/

#pragma once

#include <cstddef>

#include "MeasuredValue.hpp"
#include "VeDirectField.hpp"

class ValueFormatter
{
public:

    /// Empfohlene Puffergröße für format().
    static constexpr std::size_t BufferSize = 128;

    /// Klarname des Feldes, bei unbekanntem Label "(unbekanntes Feld)".
    static const char* name(const MeasuredValue& value);

    /**************************************************************************
     * Wert mit Einheit als Text in buffer schreiben (immer nullterminiert,
     * ggf. gekürzt).
     *
     * @param field  Rohfeld (wird für Text, FW-Kennung usw. benötigt)
     * @param value  Ergebnis von ValueConverter::convert(field)
     * @return buffer
     **************************************************************************/
    static const char* format(const VeDirectField& field,
                              const MeasuredValue& value,
                              char* buffer,
                              std::size_t size);
};
