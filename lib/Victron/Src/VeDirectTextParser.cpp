/******************************************************************************
 * @file VeDirectTextParser.cpp
 *
 * Parser für VE.Direct Texttelegramme.
 ******************************************************************************/

#include "VeDirectTextParser.hpp"

#include <cstring>

VeDirectTextParser::VeDirectTextParser()
{
    reset();
}

/******************************************************************************
 * Parser zurücksetzen
 ******************************************************************************/
void VeDirectTextParser::reset()
{
    m_frame.clear();

    m_frameReady = false;

    m_state = State::WaitStart;

    m_labelIndex = 0;
    m_valueIndex = 0;

    m_checksumField = false;

    std::memset(m_label.data(), 0, m_label.size());
    std::memset(m_value.data(), 0, m_value.size());
}

/******************************************************************************
 * Frame vorhanden?
 ******************************************************************************/
bool VeDirectTextParser::hasFrame() const
{
    return m_frameReady;
}

/******************************************************************************
 * Aktuelles Frame liefern
 ******************************************************************************/
const VeDirectFrame& VeDirectTextParser::frame() const
{
    return m_frame;
}

/******************************************************************************
 * Feld im Frame speichern
 ******************************************************************************/
void VeDirectTextParser::storeField()
{
    if (m_frame.fieldCount >= VeDirectFrame::MaxFields)
    {
        return;
    }

    auto& field = m_frame.fields[m_frame.fieldCount++];

    std::memcpy(field.label.data(),
                m_label.data(),
                field.label.size());

    std::memcpy(field.value.data(),
                m_value.data(),
                field.value.size());

    /**********************************************************************
     * Letztes Feld?
     **********************************************************************/
    if (std::strcmp(field.label.data(), "Checksum") == 0)
    {
        m_frameReady = true;
    }

    /**********************************************************************
     * Für nächstes Feld vorbereiten
     **********************************************************************/
    std::memset(m_label.data(), 0, m_label.size());
    std::memset(m_value.data(), 0, m_value.size());

    m_labelIndex = 0;
    m_valueIndex = 0;

    m_state = State::ReadLabel;
}

/******************************************************************************
 * Ein Byte verarbeiten
 ******************************************************************************/
bool VeDirectTextParser::parse(char c)
{
    if (m_frameReady)
    {
        return true;
    }

    switch (m_state)
    {
        /******************************************************************
         * Auf erstes Zeichen warten
         ******************************************************************/
        case State::WaitStart:

        // Leerzeichen und Zeilenenden ignorieren
        if (c == '\r' || c == '\n')
        {
            break;
        }

        // VE.Direct HEX-Protokoll erkannt
        if (c == ':')
        {
            #ifdef DEBUG
                Serial.println("[Protocol] HEX Telegram erkannt");
            #endif
                // Vorläufig ignorieren.
                // Der eigentliche HEX-Parser kommt später.
            break;
        }

    // Beginn eines normalen Texttelegramms
    m_label[0] = c;
    m_labelIndex = 1;

    m_state = State::ReadLabel;

    break;

        /******************************************************************
         * Label lesen
         ******************************************************************/
        case State::ReadLabel:

            if (c == '\t')
            {
                m_state = State::ReadValue;
            }
            else
            {
                if (m_labelIndex < (m_label.size() - 1))
                {
                    m_label[m_labelIndex++] = c;
                }
            }

            break;

        /******************************************************************
         * Value lesen
         ******************************************************************/
        case State::ReadValue:

            if (c == '\r')
            {
                break;
            }

            if (c == '\n')
            {
                storeField();
                break;
            }

            if (m_valueIndex < (m_value.size() - 1))
            {
                m_value[m_valueIndex++] = c;
            }

            break;
    }

    return m_frameReady;
}