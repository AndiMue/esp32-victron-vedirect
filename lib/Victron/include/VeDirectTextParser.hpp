/******************************************************************************
 * @file VeDirectTextParser.hpp
 *
 * Parser für VE.Direct Texttelegramme.
 *
 * Verantwortlich für:
 *  - Label lesen
 *  - Value lesen
 *  - Checksum-Byte lesen
 *  - Frame erzeugen
 *
 * Keine Geräteerkennung.
 ******************************************************************************/

#pragma once

#include <array>

#include "VeDirectFrame.hpp"

class VeDirectTextParser
{
public:

    VeDirectTextParser();

    bool parse(char c);

    void reset();

    bool hasFrame() const;

    const VeDirectFrame& frame() const;

private:

    enum class State
    {
        WaitStart,
        ReadLabel,
        ReadValue,
        ReadChecksum
    };

    void storeField();

    State m_state = State::WaitStart;

    VeDirectFrame m_frame;

    bool m_frameReady = false;

    std::array<char,16> m_label{};
    std::array<char,32> m_value{};

    std::size_t m_labelIndex = 0;
    std::size_t m_valueIndex = 0;

    bool m_checksumField = false;
};