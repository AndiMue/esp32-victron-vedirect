/******************************************************************************
 * @file FieldCatalog.hpp
 *
 * Nachschlagetabelle: VE.Direct-Label -> FieldDefinition.
 *
 * Umfang: alle Text-Felder, die BMV-71x/SmartShunt und SmartSolar-MPPT laut
 * "VE.Direct Protocol" 3.34 (Table I) senden können. Unbekannte Labels
 * liefern nullptr und werden von der Ausgabe als Rohwert angezeigt.
 ******************************************************************************/

#pragma once

#include <cstddef>

#include "FieldDefinition.hpp"

class FieldCatalog
{
public:

    /// Definition zu einem Label suchen (exakter Vergleich), sonst nullptr.
    static const FieldDefinition* find(const char* label);

    /// Anzahl der Einträge (für Tests).
    static std::size_t size();

    /// Eintrag Nummer index (für Tests), index < size().
    static const FieldDefinition& at(std::size_t index);
};
