/******************************************************************************
 * @file VeDirectHexReader.hpp
 *
 * W4b Schritt 3: Register eines Victron-Geräts per HEX-Protokoll abfragen
 * (NUR LESEN) und die Antworten zwischenspeichern.
 *
 *   HexRegisterCache  Zwischenspeicher: Register -> Wert, Zustand, Zeitpunkt
 *   HexJobRunner      arbeitet eine Liste von Registern ab: eine Anfrage nach
 *                     der anderen, Zeitlimit, eine Wiederholung, Abbruch wenn
 *                     das Gerät gar nicht antwortet
 *   HexJobs           Registerlisten je Gerätetyp (Shunt, MPPT)
 *
 * Ablauf (Projektinhaber 10.11.2026): gelesen wird nach dem Erkennen eines
 * Geräts, beim Öffnen des Reiters und per Knopf - NIE regelmäßig.
 *
 * Benutzung (je Kanal, nur aus der UART-Task):
 *   runner.start(regs, n, now);
 *   in jeder Schleife: if (runner.poll(now, request, cache)) uart.write(request)
 *   je HEX-Nachricht:  if (!runner.onMessage(msg, now, cache))
 *                          cache.store(msg, now, true);   // z. B. ":A"-Meldung
 *
 * Reines C++ ohne Arduino -> auf dem PC testbar.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include "VeDirectHex.hpp"

/******************************************************************************
 * Zwischenspeicher
 ******************************************************************************/
class HexRegisterCache
{
public:

    static constexpr std::size_t Capacity = 48;   ///< Register je Gerät
    static constexpr std::size_t MaxValue = 34;   ///< längster Wert (Historientag)

    enum class State : uint8_t
    {
        Ok,         ///< Wert gültig
        NoData,     ///< Gerät meldet "keine Daten" (Flag 0x04, z. B. Historientag leer)
        Unknown,    ///< Gerät kennt das Register nicht (Flag 0x01)
        NoAnswer    ///< keine Antwort (Zeitlimit)
    };

    struct Entry
    {
        uint16_t id        = 0;
        State    state     = State::NoAnswer;
        uint8_t  flags     = 0;
        uint8_t  length    = 0;
        uint8_t  value[MaxValue] = {};
        uint32_t updatedMs = 0;
    };

    void clear() { m_count = 0; }

    std::size_t  size() const              { return m_count; }
    const Entry& at(std::size_t i) const   { return m_entries[i]; }

    const Entry* find(uint16_t id) const;

    /// Get- oder Async-Antwort übernehmen. onlyKnown = nur Register
    /// aktualisieren, die schon im Speicher sind (für ungefragte Meldungen:
    /// der MPPT sendet sonst sehr viele Register).
    /// false = nicht übernommen (kein Register, unbekannt bei onlyKnown, voll).
    bool store(const VeHex::Message& message, uint32_t nowMs, bool onlyKnown);

    /// Register als "keine Antwort" eintragen (Wert eines früheren Lesens bleibt
    /// NICHT stehen, damit nichts Veraltetes als gültig erscheint).
    void markNoAnswer(uint16_t id, uint32_t nowMs);

private:

    Entry* findOrAdd(uint16_t id);

    Entry       m_entries[Capacity];
    std::size_t m_count = 0;
};

/******************************************************************************
 * Ablaufsteuerung
 ******************************************************************************/
class HexJobRunner
{
public:

    static constexpr uint32_t    TimeoutMs        = 500;   ///< Warten auf Antwort (gemessen: 8–14 ms)
    static constexpr uint8_t     MaxAttempts      = 2;     ///< 1 Wiederholung
    static constexpr uint8_t     MaxFailsInARow   = 2;     ///< danach Abbruch (Gerät antwortet nicht)
    static constexpr std::size_t MaxRegisters     = HexRegisterCache::Capacity;

    struct Result
    {
        uint16_t requested  = 0;   ///< Register in der Liste
        uint16_t answered   = 0;
        uint16_t failed     = 0;   ///< ohne Antwort
        bool     aborted    = false;   ///< abgebrochen: Gerät antwortet gar nicht
        bool     finished   = false;   ///< Lauf beendet (normal oder abgebrochen)
        uint32_t startedMs  = 0;
        uint32_t finishedMs = 0;
    };

    /// Neuen Lauf starten. false = läuft schon, leere oder zu lange Liste.
    bool start(const uint16_t* registers, std::size_t count, uint32_t nowMs);

    /// Lauf abbrechen (z. B. Gerät verloren). Kein Ergebnis "aborted".
    void cancel() { m_busy = false; }

    bool busy() const { return m_busy; }

    /// Regelmäßig aufrufen. true = request füllen und senden.
    /// Beendet den Lauf selbst; danach liefert finishedNow() einmal true.
    bool poll(uint32_t nowMs, VeHex::Request& request, HexRegisterCache& cache);

    /// Empfangene Nachricht prüfen. true = Antwort auf die laufende Anfrage
    /// (bereits im Cache gespeichert).
    bool onMessage(const VeHex::Message& message, uint32_t nowMs, HexRegisterCache& cache);

    /// Ist seit dem letzten Aufruf ein Lauf zu Ende gegangen? (einmalig true)
    bool finishedNow();

    const Result& result() const { return m_result; }

private:

    void finish(uint32_t nowMs, bool aborted);

    uint16_t    m_registers[MaxRegisters] = {};
    std::size_t m_count    = 0;
    std::size_t m_index    = 0;
    bool        m_busy     = false;
    bool        m_waiting  = false;
    bool        m_finishedFlag = false;
    uint8_t     m_attempts = 0;
    uint8_t     m_failsInARow = 0;
    uint32_t    m_sentMs   = 0;
    Result      m_result;
};

/******************************************************************************
 * Registerlisten (W4b, Projektinhaber 10.11.2026)
 ******************************************************************************/
namespace HexJobs
{
/// SmartShunt: Einstellungen (Kapazität, Ladespannung, Schweifstrom,
/// Ladeerkennungszeit, Ladeeffizienz, Peukert, Strom-Schwellwert,
/// durchschnittliche Restlaufzeit, Entladeboden).
const uint16_t* shunt(std::size_t& count);

/// MPPT: Historie gesamt + 31 Tage, Einstellungen (Batteriespannung,
/// Absorption, Erhaltung, max. Ladestrom, Batterietyp, Temperaturkompensation,
/// Lastausgang).
const uint16_t* mppt(std::size_t& count);
}
