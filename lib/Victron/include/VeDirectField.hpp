/******************************************************************************
 * @file VeDirectField.hpp
 *
 * Ein VE.Direct-Feld bestehend aus Label und Wert.
 *
 * Keine Interpretation der Daten.
 ******************************************************************************/

#pragma once

#include <array>

struct VeDirectField
{
    static constexpr std::size_t LabelSize = 16;
    static constexpr std::size_t ValueSize = 32;

    std::array<char, LabelSize> label{};
    std::array<char, ValueSize> value{};
};