/******************************************************************************
 * @file FieldDefinition.hpp
 *
 * Beschreibung eines VE.Direct-Feldes für den FieldCatalog:
 * Label, deutscher Klarname, Bedeutung, Datentyp, Einheit, Skalierung.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include "SemanticField.hpp"

/******************************************************************************
 * Wie der Rohtext eines Feldes zu lesen ist.
 ******************************************************************************/
enum class ValueType : uint8_t
{
    Number,         ///< Dezimalzahl, physikalisch = roh / 10^decimals
    TimeToGo,       ///< wie Number, aber -1 bedeutet "unendlich"
    OnOff,          ///< "ON"/"OFF" (Groß-/Kleinschreibung egal)
    Text,           ///< Text, wird unverändert angezeigt (SER#, BMV)
    ProductId,      ///< Hex-Zahl "0x...." -> Gerätename aus DeviceCatalog
    Code,           ///< Dezimaler Code -> Text aus CodeTable (CS, ERR ...)
    BitmaskDecimal, ///< Dezimal übertragene Bitmaske (AR)
    BitmaskHex,     ///< Hex übertragene Bitmaske "0x...." (OR, CAP_BLE)
    Firmware,       ///< FW:  "419" -> v4.19, "C208" -> v2.08 Release Candidate C
    FirmwareExt     ///< FWE: "0208FF" -> v2.08, "020801" -> v2.08-beta-01
};

/******************************************************************************
 * Ein Eintrag einer Code- oder Bitmaskentabelle.
 * Bei Bitmasken ist "code" der Wert des Bits (1, 2, 4, ...).
 ******************************************************************************/
struct CodeEntry
{
    int32_t     code;
    const char* text;
};

struct CodeTable
{
    const CodeEntry* entries;
    std::size_t      count;
};

/******************************************************************************
 * Definition eines Feldes.
 ******************************************************************************/
struct FieldDefinition
{
    const char*      label;     ///< VE.Direct-Label, z. B. "V"
    const char*      name;      ///< deutscher Klarname, z. B. "Batteriespannung"
    SemanticField    semantic;  ///< fachliche Bedeutung
    ValueType        type;      ///< Interpretation des Rohtexts
    uint8_t          decimals;  ///< Nur Number/TimeToGo: physikalisch = roh / 10^decimals
                                ///< (z. B. mV -> V: 3, 0,01 kWh -> kWh: 2)
    const char*      unit;      ///< Einheit des physikalischen Werts ("" = keine)
    const CodeTable* codes;     ///< Nur Code/Bitmask*, sonst nullptr
};
