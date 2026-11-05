/******************************************************************************
 * @file ValueConverter.hpp
 *
 * Wandelt den Rohtext eines VE.Direct-Feldes in einen MeasuredValue um.
 *
 * Aufgaben:
 *   - Feld im FieldCatalog nachschlagen
 *   - Rohtext streng prüfen und als Zahl lesen (dezimal oder "0x..." hex)
 *   - Sonderwerte erkennen: "---" -> NotAvailable, TTG -1 -> Infinite
 *   - physikalischen Wert berechnen (roh / 10^decimals)
 *
 * Kennt keine Ausgabe (kein Serial) -> auf dem PC testbar und später auch
 * für Display und Relaislogik verwendbar.
 ******************************************************************************/

#pragma once

#include "MeasuredValue.hpp"
#include "VeDirectField.hpp"

class ValueConverter
{
public:

    static MeasuredValue convert(const VeDirectField& field);

    static MeasuredValue convert(const char* label, const char* value);
};
