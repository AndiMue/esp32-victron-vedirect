/******************************************************************************
 * @file VeDirectFrame.hpp
 *
 * Enthält genau ein vollständiges VE.Direct-Telegramm.
 ******************************************************************************/

#pragma once

#include <array>

#include "VeDirectField.hpp"

struct VeDirectFrame
{
    static constexpr std::size_t MaxFields = 32;

    std::array<VeDirectField, MaxFields> fields{};

    std::size_t fieldCount = 0;

    uint8_t checksum = 0;

    bool checksumValid = false;

    void clear()
    {
        fieldCount = 0;
        checksum = 0;
        checksumValid = false;
    }
};