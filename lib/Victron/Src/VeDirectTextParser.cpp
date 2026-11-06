/******************************************************************************
 * @file VeDirectTextParser.cpp
 *
 * Implementierung siehe Beschreibung in VeDirectTextParser.hpp.
 *
 * Hinweis: Reines C++ (C++11), keine Arduino-Abhängigkeit. Dadurch läuft
 * der Parser sowohl auf dem ESP32 als auch in den PC-Tests (env:native).
 ******************************************************************************/

#include "VeDirectTextParser.hpp"

#include <cstring>

namespace
{
    /// Label des letzten Feldes eines Blocks (Groß-/Kleinschreibung laut Doku).
    const char* const ChecksumLabel = "Checksum";

    /**************************************************************************
     * Darf dieses Byte in einem Label oder Wert vorkommen?
     *
     * Victron sendet Labels und Werte ausschließlich als druckbares ASCII
     * (0x20..0x7E, Leerzeichen kommt z. B. in "SmartShunt 500A/50mV" vor).
     *
     * Hintergrund (Fund im Praxistest 05.11.2026): Beim Einstecken eines
     * VE.Direct-Steckers entsteht ein Null-Byte (0x00). Die Modulo-256-
     * Prüfsumme bemerkt das NICHT, weil 0x00 nichts zur Summe beiträgt.
     * Ein 0x00 im Label machte "PID" unsichtbar; in einem Wert hätte es den
     * Wert abgeschnitten ("13400" -> "13"). Deshalb führt jedes andere Byte
     * zum Verwerfen des ganzen Blocks.
     **************************************************************************/
    bool isPrintableAscii(char c)
    {
        const uint8_t byte = static_cast<uint8_t>(c);
        return byte >= 0x20 && byte <= 0x7E;
    }
}

/******************************************************************************
 * Konstruktor
 ******************************************************************************/
VeDirectTextParser::VeDirectTextParser()
{
    reset();
}

/******************************************************************************
 * Vollständiger Reset: Zustand, gültiger Frame und Zähler.
 ******************************************************************************/
void VeDirectTextParser::reset()
{
    m_frame.clear();
    m_stats        = Statistics();
    m_synchronized = false;

    startNewBlock();
}

/******************************************************************************
 * Vorbereitung auf einen neuen Block.
 *
 * Der temporäre Frame wird komplett geleert, damit keine Felder des
 * vorherigen Blocks übrig bleiben.
 ******************************************************************************/
void VeDirectTextParser::startNewBlock()
{
    m_work.clear();

    m_checksum       = 0;
    m_blockCorrupt   = false;
    m_state          = State::WaitForNewline;
    m_stateBeforeHex = State::WaitForNewline;

    clearFieldBuffers();
}

/******************************************************************************
 * Label- und Wertpuffer für das nächste Feld leeren.
 ******************************************************************************/
void VeDirectTextParser::clearFieldBuffers()
{
    m_labelLength = 0;
    m_valueLength = 0;
    m_label[0]    = '\0';
    m_value[0]    = '\0';
}

/******************************************************************************
 * Aktuelles Feld (Label + Wert) in den temporären Frame übernehmen.
 ******************************************************************************/
void VeDirectTextParser::storeField()
{
    if (m_work.fieldCount >= VeDirectFrame::MaxFields)
    {
        // Mehr Felder als vorgesehen: Block ist unbrauchbar
        // (z. B. weil das "Checksum"-Label durch einen Übertragungsfehler
        // verloren ging und zwei Blöcke ineinanderlaufen).
        m_blockCorrupt = true;
        return;
    }

    VeDirectField& field = m_work.fields[m_work.fieldCount++];

    std::memcpy(field.label.data(), m_label, m_labelLength);
    field.label[m_labelLength] = '\0';

    std::memcpy(field.value.data(), m_value, m_valueLength);
    field.value[m_valueLength] = '\0';
}

/******************************************************************************
 * Block abschließen, nachdem das Prüfsummenbyte empfangen wurde.
 *
 * m_checksum enthält zu diesem Zeitpunkt bereits das Prüfsummenbyte.
 * Ein fehlerfreier Block ergibt daher exakt 0.
 ******************************************************************************/
VeDirectTextParser::Event VeDirectTextParser::finishBlock(uint8_t checksumByte)
{
    Event event = Event::None;

    if (!m_synchronized)
    {
        // Erster Block nach Start/Reset: Der Empfang hat mit hoher
        // Wahrscheinlichkeit mitten im Block begonnen. Verwerfen, aber
        // NICHT als Fehler zählen. Ab jetzt ist die Blockgrenze bekannt.
        m_synchronized = true;
    }
    else if (m_checksum != 0)
    {
        // Übertragungsfehler. Hat Vorrang vor Formatfehlern, weil ein
        // Formatfehler meist nur die Folge eines Übertragungsfehlers ist.
        ++m_stats.checksumErrors;
        event = Event::ChecksumError;
    }
    else if (m_blockCorrupt)
    {
        // Prüfsumme stimmt, aber der Block passt nicht in unsere Puffer
        // oder ist formal falsch -> nicht übernehmen.
        ++m_stats.framesDiscarded;
        event = Event::FrameDiscarded;
    }
    else
    {
        // Gültiger Block: temporären Frame übernehmen.
        m_work.checksum      = checksumByte;
        m_work.checksumValid = true;
        m_frame              = m_work;

        ++m_stats.framesOk;
        event = Event::FrameReady;
    }

    startNewBlock();
    return event;
}

/******************************************************************************
 * Ein Byte verarbeiten (Zustandsmaschine).
 ******************************************************************************/
VeDirectTextParser::Event VeDirectTextParser::parse(uint8_t byte)
{
    const char c = static_cast<char>(byte);

    /**************************************************************************
     * 1. HEX-Nachrichten überspringen
     *
     *    Eine HEX-Nachricht beginnt mit ':' und endet mit '\n'. Ihre Bytes
     *    gehören nicht zur Prüfsumme des Text-Blocks. Danach geht es im
     *    vorherigen Zustand weiter.
     *
     *    Ausnahme: Im Zustand ReadChecksum ist ':' ein gültiges
     *    Prüfsummenbyte und KEIN HEX-Beginn.
     **************************************************************************/
    if (m_state == State::SkipHex)
    {
        if (c == '\n')
        {
            ++m_stats.hexMessagesSkipped;
            m_state = m_stateBeforeHex;
        }
        return Event::None;
    }

    if (c == ':' && m_state != State::ReadChecksum)
    {
        m_stateBeforeHex = m_state;
        m_state          = State::SkipHex;
        return Event::None;
    }

    /**************************************************************************
     * 2. Alle übrigen Bytes zählen zur Prüfsumme (uint8_t -> automatisch
     *    Modulo 256). Das gilt auch für '\r' und '\n' zwischen den Feldern.
     **************************************************************************/
    m_checksum = static_cast<uint8_t>(m_checksum + byte);

    switch (m_state)
    {
        /**********************************************************************
         * Zwischen zwei Blöcken: Das '\n' der "\r\n"-Sequenz leitet das
         * erste Label ein. Alles andere (insbesondere '\r') wird ignoriert,
         * zählt aber zur Prüfsumme.
         **********************************************************************/
        case State::WaitForNewline:

            if (c == '\n')
            {
                clearFieldBuffers();
                m_state = State::ReadLabel;
            }
            break;

        /**********************************************************************
         * Label lesen, bis ein Tabulator kommt.
         **********************************************************************/
        case State::ReadLabel:

            if (c == '\t')
            {
                m_label[m_labelLength] = '\0';

                if (m_labelLength == 0)
                {
                    // Tabulator ohne Label -> Formatfehler
                    m_blockCorrupt = true;
                }

                if (std::strcmp(m_label, ChecksumLabel) == 0)
                {
                    // Das nächste Byte ist die binäre Prüfsumme.
                    m_state = State::ReadChecksum;
                }
                else
                {
                    m_state = State::ReadValue;
                }
            }
            else if (c == '\r')
            {
                // Teil des Zeilenendes, ignorieren
            }
            else if (c == '\n')
            {
                if (m_labelLength > 0)
                {
                    // Zeile ohne Tabulator -> Formatfehler.
                    m_blockCorrupt = true;
                }
                // Leere Zeile (z. B. nach einer HEX-Nachricht am
                // Zeilenanfang) ist unkritisch: einfach neu beginnen.
                clearFieldBuffers();
            }
            else if (!isPrintableAscii(c))
            {
                // Unzulässiges Byte (z. B. 0x00 beim Einstecken)
                m_blockCorrupt = true;
            }
            else if (m_labelLength < VeDirectField::LabelSize - 1)
            {
                m_label[m_labelLength++] = c;
            }
            else
            {
                // Label zu lang (Victron: max. 8 Zeichen, Puffer: 15)
                m_blockCorrupt = true;
            }
            break;

        /**********************************************************************
         * Wert lesen bis '\n'. '\r' gehört zum Zeilenende.
         **********************************************************************/
        case State::ReadValue:

            if (c == '\r')
            {
                // Teil des Zeilenendes, ignorieren
            }
            else if (c == '\n')
            {
                storeField();
                clearFieldBuffers();
                m_state = State::ReadLabel;
            }
            else if (!isPrintableAscii(c))
            {
                // Unzulässiges Byte im Wert (z. B. 0x00 oder Tabulator):
                // Wert wäre verfälscht -> ganzen Block verwerfen.
                m_blockCorrupt = true;
            }
            else if (m_valueLength < VeDirectField::ValueSize - 1)
            {
                m_value[m_valueLength++] = c;
            }
            else
            {
                // Wert zu lang (Victron: max. 32 Zeichen)
                m_blockCorrupt = true;
            }
            break;

        /**********************************************************************
         * Dieses Byte ist die Prüfsumme -> Block ist zu Ende.
         **********************************************************************/
        case State::ReadChecksum:

            return finishBlock(byte);

        case State::SkipHex:
            // Wird oben behandelt, hier nie erreicht.
            break;
    }

    return Event::None;
}
