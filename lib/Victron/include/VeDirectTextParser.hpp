/******************************************************************************
 * @file VeDirectTextParser.hpp
 *
 * Parser für das VE.Direct Text-Protokoll (Victron "VE.Direct Protocol" 3.34).
 *
 * Aufgabe:
 *   Byteweise eingehende Daten eines EINZELNEN VE.Direct-Kanals in
 *   vollständige, prüfsummengesicherte Blöcke (VeDirectFrame) zerlegen.
 *
 * Bewusst NICHT Aufgabe dieses Parsers:
 *   - UART-Zugriff (kennt kein Arduino/HardwareSerial -> auf dem PC testbar)
 *   - Geräteerkennung, Interpretation, Einheiten, Skalierung
 *
 * Aufbau eines Blocks laut Victron-Doku:
 *
 *   "\r\n" LABEL "\t" WERT     (beliebig oft, max. 22 Felder)
 *   "\r\n" "Checksum" "\t" X   (X = EIN binäres Byte, kann jeden Wert haben,
 *                               also auch '\r', '\n', '\t' oder ':')
 *
 *   Die Modulo-256-Summe aller Bytes eines Blocks (inklusive X) ist 0.
 *
 * Wichtige Designentscheidungen:
 *
 *   1. Das erste Byte nach "Checksum\t" ist IMMER die Prüfsumme und beendet
 *      den Block. Es wird nie als Steuerzeichen interpretiert.
 *
 *   2. Felder werden zunächst in einem temporären Frame gesammelt. Erst wenn
 *      die Prüfsumme stimmt, wird er in den gültigen Frame übernommen
 *      (Empfehlung "Implementation guidelines" der Victron-Doku).
 *      Ein fehlerhafter Block verändert den zuletzt gültigen Frame nicht.
 *
 *   3. Jeder Block beginnt mit einem leeren Frame. Werte eines vorherigen
 *      Blocks (z. B. die PID) können nicht in einen Folgeblock "durchrutschen".
 *      Der SmartShunt sendet z. B. zwei Blöcke pro Sekunde; der zweite
 *      (H1..H18) enthält KEINE PID.
 *
 *   4. Asynchrone HEX-Nachrichten (":....\n") können laut Doku mitten in einen
 *      Text-Block fallen. Sie werden bis zum '\n' übersprungen, zählen nicht
 *      zur Prüfsumme und der Parser setzt danach exakt dort fort, wo er war
 *      (Vorgehen wie in Victrons Referenzimplementierung).
 *
 *   5. Nach dem Start bzw. reset() beginnt der Empfang meist mitten in einem
 *      Block. Dieser erste Block wird stillschweigend verworfen (keine
 *      Fehlerzählung), erst danach ist der Parser "synchronisiert".
 *
 *   6. Zu lange Labels/Werte, zu viele Felder oder Zeilen ohne Tabulator
 *      werden NICHT abgeschnitten, sondern der ganze Block wird verworfen.
 *
 *   7. Labels und Werte dürfen nur druckbares ASCII (0x20..0x7E) enthalten.
 *      Andere Bytes (z. B. 0x00 beim Einstecken eines Steckers) verwerfen
 *      den Block, da die Prüfsumme ein 0x00 nicht erkennen kann.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include "VeDirectFrame.hpp"

class VeDirectTextParser
{
public:

    /**************************************************************************
     * Ergebnis der Verarbeitung eines Bytes.
     **************************************************************************/
    enum class Event : uint8_t
    {
        None,           ///< Block noch nicht zu Ende (Normalfall)
        FrameReady,     ///< Gültiger Block empfangen -> frame() ist neu
        ChecksumError,  ///< Block zu Ende, Prüfsumme falsch -> verworfen
        FrameDiscarded  ///< Block zu Ende, Prüfsumme ok, aber Format/Größe
                        ///< ungültig (zu lang, zu viele Felder ...) -> verworfen
    };

    /**************************************************************************
     * Zähler für die Diagnose. Laufen seit Start bzw. letztem reset().
     **************************************************************************/
    struct Statistics
    {
        uint32_t framesOk           = 0;  ///< gültige Blöcke
        uint32_t checksumErrors     = 0;  ///< Blöcke mit falscher Prüfsumme
        uint32_t framesDiscarded    = 0;  ///< Blöcke mit Format-/Größenfehler
        uint32_t hexMessagesSkipped = 0;  ///< übersprungene HEX-Nachrichten
    };

    VeDirectTextParser();

    /**************************************************************************
     * Ein empfangenes Byte verarbeiten.
     *
     * @return Event::FrameReady, sobald ein gültiger Block vollständig ist.
     **************************************************************************/
    Event parse(uint8_t byte);

    /**************************************************************************
     * Zuletzt empfangener GÜLTIGER Block.
     * Der Inhalt ändert sich nur, wenn parse() Event::FrameReady liefert.
     **************************************************************************/
    const VeDirectFrame& frame() const { return m_frame; }

    const Statistics& statistics() const { return m_stats; }

    /**************************************************************************
     * Parser vollständig zurücksetzen (Zustand, Frame UND Zähler).
     * Danach wird der nächste Block wieder als Synchronisationsblock
     * verworfen.
     **************************************************************************/
    void reset();

private:

    /**************************************************************************
     * Zustände der Empfangs-Zustandsmaschine.
     **************************************************************************/
    enum class State : uint8_t
    {
        WaitForNewline, ///< zwischen Blöcken: warten auf '\n' vor dem 1. Label
        ReadLabel,      ///< Label bis '\t' lesen
        ReadValue,      ///< Wert bis '\n' lesen
        ReadChecksum,   ///< nächstes Byte ist die Prüfsumme
        SkipHex         ///< HEX-Nachricht bis '\n' überspringen
    };

    void startNewBlock();
    void clearFieldBuffers();
    void storeField();
    Event finishBlock(uint8_t checksumByte);

    State m_state          = State::WaitForNewline;
    State m_stateBeforeHex = State::WaitForNewline; ///< Rücksprung nach HEX

    VeDirectFrame m_work;   ///< temporärer Frame des laufenden Blocks
    VeDirectFrame m_frame;  ///< zuletzt gültiger Frame

    char        m_label[VeDirectField::LabelSize] = {};
    char        m_value[VeDirectField::ValueSize] = {};
    std::size_t m_labelLength = 0;
    std::size_t m_valueLength = 0;

    uint8_t m_checksum      = 0;     ///< laufende Modulo-256-Summe
    bool    m_blockCorrupt  = false; ///< Format-/Größenfehler im Block
    bool    m_synchronized  = false; ///< erster Block nach Start verworfen?

    Statistics m_stats;
};
