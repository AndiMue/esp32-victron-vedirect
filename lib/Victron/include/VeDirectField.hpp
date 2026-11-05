/******************************************************************************
 * @file VeDirectField.hpp
 *
 * Ein VE.Direct-Feld bestehend aus Label und Wert (Rohtext, nullterminiert).
 *
 * Keine Interpretation der Daten.
 *
 * Puffergrößen laut Victron "Implementation guidelines":
 *   - Label: 9 Byte  (8 Zeichen + '\0')  -> hier großzügig 16
 *   - Wert : 33 Byte (32 Zeichen + '\0')
 ******************************************************************************/

#pragma once

#include <array>
#include <cstddef>

struct VeDirectField
{
    static constexpr std::size_t LabelSize = 16;
    static constexpr std::size_t ValueSize = 33;

    std::array<char, LabelSize> label{};
    std::array<char, ValueSize> value{};
};
