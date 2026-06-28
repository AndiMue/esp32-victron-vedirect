/******************************************************************************
 * @file VeDirectFrame.hpp
 *
 * Enthält genau ein vollständiges VE.Direct-Telegramm.
 ******************************************************************************/

#pragma once

#include <array>
#include <cstring>

#include "VeDirectField.hpp"

struct VeDirectFrame
{
    static constexpr std::size_t MaxFields = 32;

    std::array<VeDirectField, MaxFields> fields{};

    std::size_t fieldCount = 0;

    uint8_t checksum = 0;

    bool checksumValid = false;

    /**************************************************************************
     * Frame zurücksetzen.
     **************************************************************************/
    void clear()
    {
        fieldCount = 0;
        checksum = 0;
        checksumValid = false;
    }

    /**************************************************************************
     * Feld anhand seines Labels suchen.
     **************************************************************************/
    const VeDirectField* find(const char* label) const
    {
        for (std::size_t i = 0; i < fieldCount; ++i)
        {
            if (std::strcmp(fields[i].label.data(),
                            label) == 0)
            {
                return &fields[i];
            }
        }

        return nullptr;
    }
};