/******************************************************************************
 * @file DeviceDetector.hpp
 *
 * Geräteerkennung für EINEN VE.Direct-Kanal.
 *
 * Ziel: Jedes Gerät funktioniert an jedem UART. Der Kanal erkennt anhand
 * der PID, welches Gerät angeschlossen ist, und bemerkt Ausfall und Wechsel.
 *
 * Zustände:
 *
 *               Block mit PID                    Block mit anderer PID
 *   Unknown  ──────────────────►  Identified  ─────────────────────────► Identified
 *      ▲                              │                                  (Event Changed)
 *      │  Timeout: zu lange kein      │
 *      └───── Block MIT PID ──────────┘  (Event Lost)
 *
 * Regeln:
 *   1. Ein gültiger Block mit PID legt das Gerät fest. Unbekannte PIDs
 *      (nicht im DeviceCatalog) gelten als "unbekanntes Gerät", sind aber
 *      trotzdem erkannt.
 *   2. Ein Block OHNE PID wird nur einem erkannten BATTERIEMONITOR
 *      zugeordnet (Origin::Inherited). Nur BMV/SmartShunt teilen ihre Daten
 *      auf zwei Blöcke auf (H-Block ohne PID). In allen anderen Fällen ist
 *      der Block "nicht zuordenbar" (Origin::Unassigned).
 *   3. Nur Blöcke MIT PID halten die Erkennung aufrecht. Kommt länger als
 *      timeoutMs() kein solcher Block, gilt das Gerät als verloren.
 *      Typische Ursachen in der Praxis: lockerer Stecker, gelöste
 *      Crimpverbindung, Kabelbruch. Dafür gibt es Zähler (Statistics).
 *   4. Eine geänderte PID wird sofort als Gerätewechsel gemeldet.
 *
 * Benutzung (je Kanal eine Instanz, nur aus EINER Task aufrufen):
 *   - checkTimeout(now) regelmäßig aufrufen (z. B. in jeder Schleife) und
 *     unmittelbar vor update().
 *   - update(frame, now) für jeden GÜLTIGEN Block des Parsers.
 *
 * Zeitwerte sind millis()-Werte. Der Überlauf nach ca. 49 Tagen wird durch
 * vorzeichenlose Differenzbildung korrekt behandelt.
 *
 * Reines C++ ohne Arduino -> auf dem PC testbar.
 ******************************************************************************/

#pragma once

#include <cstdint>

#include "DeviceInfo.hpp"
#include "VeDirectFrame.hpp"

class DeviceDetector
{
public:

    /// Standard-Timeout (änderbar über setTimeoutMs, z. B. später per WebUI).
    static constexpr uint32_t DefaultTimeoutMs = 5000;

    enum class State : uint8_t
    {
        Unknown,     ///< kein Gerät erkannt (Start oder nach Ausfall)
        Identified   ///< Gerät anhand der PID erkannt
    };

    /// Was hat sich durch einen Aufruf geändert?
    enum class Event : uint8_t
    {
        None,
        Identified,  ///< Gerät (erstmals oder nach Ausfall wieder) erkannt
        Changed,     ///< andere PID als bisher -> anderes Gerät
        Lost         ///< Timeout: zu lange kein Block mit PID
    };

    /// Herkunft eines Blocks.
    enum class Origin : uint8_t
    {
        Own,         ///< Block enthält selbst die PID
        Inherited,   ///< ohne PID, dem erkannten Batteriemonitor zugeordnet
        Unassigned   ///< ohne PID und keinem Gerät zuordenbar
    };

    /// Erkanntes Gerät des Kanals.
    struct Identity
    {
        State             state = State::Unknown;
        char              productId[VeDirectField::ValueSize] = {}; ///< PID wie gesendet
        const DeviceInfo* device = nullptr; ///< nullptr = unbekanntes Gerät
        uint32_t          lastPidMs = 0;    ///< Zeit des letzten Blocks mit PID
    };

    /// Zähler für die Diagnose (laufen seit Start bzw. reset()).
    struct Statistics
    {
        uint32_t identifiedCount  = 0;  ///< Erkennungen (inkl. Wiedererkennung)
        uint32_t changedCount     = 0;  ///< Gerätewechsel
        uint32_t lostCount        = 0;  ///< Ausfälle (Timeouts)
        uint32_t unassignedFrames = 0;  ///< nicht zuordenbare Blöcke
        uint32_t lastOutageMs     = 0;  ///< Dauer des letzten Ausfalls
                                        ///< (letzte PID vor Ausfall bis Wiedererkennung)
    };

    /// Ergebnis von update().
    struct Result
    {
        Origin origin = Origin::Unassigned;
        Event  event  = Event::None;
    };

    DeviceDetector();

    /// Gültigen Block auswerten. Vorher checkTimeout() aufrufen.
    Result update(const VeDirectFrame& frame, uint32_t nowMs);

    /// Timeout prüfen. Liefert Event::Lost genau einmal pro Ausfall.
    Event checkTimeout(uint32_t nowMs);

    void     setTimeoutMs(uint32_t timeoutMs) { m_timeoutMs = timeoutMs; }
    uint32_t timeoutMs() const                { return m_timeoutMs; }

    /// Aktuell erkanntes Gerät.
    const Identity& identity() const { return m_identity; }

    /// Gerät VOR dem letzten Wechsel bzw. Ausfall (für Meldungen).
    const Identity& previousIdentity() const { return m_previous; }

    const Statistics& statistics() const { return m_stats; }

    /// Alles zurücksetzen (Zustand und Zähler). Timeout bleibt erhalten.
    void reset();

private:

    bool isTimedOut(uint32_t nowMs) const;
    void identify(const char* productId, uint32_t nowMs);

    Identity   m_identity;
    Identity   m_previous;
    Statistics m_stats;
    uint32_t   m_timeoutMs = DefaultTimeoutMs;
    uint32_t   m_lostLastPidMs = 0;   ///< lastPidMs zum Zeitpunkt des Ausfalls
    bool       m_wasLost = false;     ///< Ausfall seit letzter Erkennung?
};
