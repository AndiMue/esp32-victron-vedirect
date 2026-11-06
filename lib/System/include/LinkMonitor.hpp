/******************************************************************************
 * @file LinkMonitor.hpp
 *
 * Überwachung der Verbindungsqualität EINES Kanals.
 *
 * Hintergrund (Projektinhaber 06.11.2026): Im Betrieb (Camper, Vibration)
 * können Stecker locker werden, Crimpverbindungen sich lösen oder Kabel
 * brechen. Das zeigt sich als Ausfälle, Prüfsummenfehler oder verworfene
 * Blöcke (z. B. Null-Byte beim Einstecken).
 *
 * Regeln (bewusst OHNE festes Zeitfenster):
 *   - "Instabil", sobald seit der letzten Erholung
 *       * mindestens outageThreshold Ausfälle (Standard 1)   ODER
 *       * mindestens errorThreshold Prüfsummenfehler + verworfene
 *         Blöcke (Standard 3)
 *     aufgetreten sind.
 *   - "Erholung": recoveryMs (Standard 2 min) ohne neuen Fehler UND bei
 *     verbundenem Gerät -> Warnung erlischt, die Zähler "seit Erholung"
 *     werden zurückgesetzt. Solange das Gerät getrennt ist (Kabel ab),
 *     läuft die Erholzeit nicht.
 *   - recoveryMs = 0: keine Erholung, Warnung bleibt bis zum Neustart.
 *   - Gesamtzähler (total) laufen immer weiter, für die Diagnose.
 *
 * Reines C++ ohne Arduino -> auf dem PC testbar.
 ******************************************************************************/

#pragma once

#include <cstdint>

class LinkMonitor
{
public:

    struct Config
    {
        uint32_t errorThreshold  = 3;        ///< Prüfsummenfehler + verworfene Blöcke
        uint32_t outageThreshold = 1;        ///< Ausfälle
        uint32_t recoveryMs      = 120000;   ///< fehlerfreie Zeit bis zur Erholung (0 = nie)
    };

    struct Counters
    {
        uint32_t checksumErrors  = 0;
        uint32_t discardedFrames = 0;
        uint32_t outages         = 0;
    };

    LinkMonitor();

    void setConfig(const Config& config) { m_config = config; }
    const Config& config() const         { return m_config; }

    /// Fehlerereignisse melden
    void recordChecksumError(uint32_t nowMs);
    void recordDiscardedFrame(uint32_t nowMs);
    void recordOutage(uint32_t nowMs);

    /// Regelmäßig aufrufen: prüft die Erholung.
    /// @param deviceConnected  true, wenn das Gerät des Kanals erkannt ist
    void update(uint32_t nowMs, bool deviceConnected);

    bool isUnstable() const { return m_unstable; }

    /// Wie oft wurde die Verbindung bisher als instabil eingestuft?
    uint32_t unstableCount() const { return m_unstableCount; }

    const Counters& total() const   { return m_total; }    ///< seit Start
    const Counters& current() const { return m_current; }  ///< seit letzter Erholung

    void reset();

private:

    void onError(uint32_t nowMs);

    Config   m_config;
    Counters m_total;
    Counters m_current;
    uint32_t m_lastErrorMs   = 0;
    bool     m_hasErrors     = false;   ///< Fehler seit letzter Erholung?
    bool     m_unstable      = false;
    uint32_t m_unstableCount = 0;
};
