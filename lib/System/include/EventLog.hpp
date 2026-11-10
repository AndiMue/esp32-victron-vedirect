/******************************************************************************
 * @file EventLog.hpp
 *
 * Ereignisliste (W4a Schritt 4, Reiter "Fehler" der WebUI).
 *
 * Ringpuffer fester Größe im RAM: Ist er voll, wird der älteste Eintrag
 * überschrieben. Geht beim Neustart verloren (Entscheidung Projektinhaber
 * 09.11.2026: dauerhaft speichern erst mit Echtzeituhr, sonst fehlen die
 * Zeitstempel).
 *
 * Zeit: millis() beim Eintrag. Die WebUI zeigt "vor x min" (Laufzeit), bis
 * eine Echtzeituhr vorhanden ist (W4e/W5).
 *
 * Jeder Eintrag bekommt eine fortlaufende Nummer (seq, ab 1). Damit kann
 * die WebUI später z. B. "neue Ereignisse seit dem letzten Abruf" erkennen.
 *
 * Texte sind UTF-8 (Umlaute). Zu lange Texte werden an einer Zeichengrenze
 * gekürzt, nie mitten in einem Umlaut.
 *
 * NICHT threadsicher: Der Aufrufer schützt die Liste (main.cpp: Spinlock).
 * Reines C++ ohne Arduino -> auf dem PC testbar.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include "SystemState.hpp"   // Severity

class EventLog
{
public:

    static constexpr std::size_t Capacity = 50;   ///< Anzahl Einträge
    static constexpr std::size_t TextSize = 96;   ///< Bytes inkl. '\0'

    struct Entry
    {
        uint32_t seq      = 0;               ///< fortlaufende Nummer, 0 = leer
        uint32_t timeMs   = 0;               ///< millis() beim Eintrag
        Severity severity = Severity::Info;
        char     text[TextSize] = {};
    };

    /// Ereignis anhängen (überschreibt bei voller Liste das älteste).
    void add(uint32_t nowMs, Severity severity, const char* text);

    /// Anzahl gespeicherter Einträge (0..Capacity).
    std::size_t size() const { return m_count; }

    /// Eintrag Nummer index, 0 = NEUESTER. Voraussetzung: index < size().
    const Entry& newest(std::size_t index) const;

    /// Nummer des zuletzt eingetragenen Ereignisses (0 = noch keins).
    uint32_t lastSeq() const { return m_seq; }

    void clear();

private:

    Entry       m_entries[Capacity];
    std::size_t m_next  = 0;   ///< Schreibposition
    std::size_t m_count = 0;
    uint32_t    m_seq   = 0;
};
