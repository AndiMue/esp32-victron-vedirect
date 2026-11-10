/******************************************************************************
 * @file LoadController.hpp
 *
 * Abschaltlogik: entscheidet anhand des Systemzustands, welche Relais ein-
 * oder ausgeschaltet sein SOLLEN. Schaltet selbst keine GPIOs (RelayDriver).
 *
 * Relais (Entscheidung Projektinhaber 06.11.2026):
 *   R1 Hauptverbraucher  Camper: Wechselrichter, Balkonkraftwerk: DC-DC
 *   R2 Licht             (nur Camper)
 *   R3 Kühlschrank       (nur Camper)
 *
 * Modell "Schaltwunsch" (seit W3, 09.11.2026 – alle Relais schaltbar):
 *   Jedes Relais hat einen Wunsch (requested) ein/aus. Der Wunsch kommt von
 *   Befehlen (Taster/Touch, WebUI, MQTT) bzw. nach dem Start aus
 *   startupRequest. Das Relais ist EIN, wenn
 *       Wunsch = ein  UND  nicht gesperrt (SOC-Schutz)
 *   (Override: Wunsch allein entscheidet, alle Sperren aufgehoben.)
 *
 * Schwellen je Relais (alle konfigurierbar, SOC in %):
 *   lockBelowSoc   unter diesem SOC darf das Relais NICHT eingeschaltet werden
 *   offBelowSoc    unter diesem SOC wird es zwangsweise ausgeschaltet
 *   releaseAtSoc   ab diesem SOC ist die Sperre wieder aufgehoben (Hysterese)
 *   Beispiel R1: gesperrt < 25 %, aus < 15 %, frei ab 30 % -> ein laufender
 *   Wechselrichter läuft bei 20 % weiter, kann aber nach dem Ausschalten
 *   erst ab 30 % wieder eingeschaltet werden.
 *
 * Nach einer Schutzabschaltung:
 *   rememberRequest = true   Wunsch bleibt; bei ausreichendem SOC und nach
 *                            minOffTimeMs wieder EIN (R2/R3, BKW-R1)
 *   rememberRequest = false  Wunsch wird gelöscht; neu einschalten nur per
 *                            Befehl (Camper-R1: Wechselrichter)
 *
 * Ohne gültigen SOC (Shunt fehlt, nicht synchronisiert):
 *   requiresSoc = true   (R1)   gesperrt; forceOffWithoutSoc = true -> aus
 *   requiresSoc = false  (R2/3) Zustand bleibt (Kleinverbraucher laufen weiter)
 *
 * Override: umgeht ALLE Sperren, Relais folgen nur dem Wunsch. Wird nicht
 * gespeichert -> nach Neustart Normalbetrieb.
 *
 * Nach dem Start (start()): Wunsch = startupRequest (Camper: R1 aus,
 * R2/R3 ein). Relais mit requiresSoc gehen erst ein, wenn der SOC passt.
 *
 * Reines C++ ohne Arduino -> auf dem PC testbar.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include "SystemState.hpp"

enum class RelayId : uint8_t
{
    Main   = 0,   ///< R1 Wechselrichter / DC-DC
    Light  = 1,   ///< R2 Licht
    Fridge = 2    ///< R3 Kühlschrank
};

constexpr std::size_t RelayCount = 3;

/// Herkunft eines Schaltbefehls.
enum class CommandSource : uint8_t
{
    Local = 0,   ///< Taster/Touch am Bediengerät (vorerst auch serieller Monitor)
    WebUi = 1,
    Mqtt  = 2
};

/// Bit einer Befehlsquelle für RelayConfig::allowedSources.
constexpr uint8_t sourceBit(CommandSource source)
{
    return static_cast<uint8_t>(1u << static_cast<uint8_t>(source));
}

/// Alle Befehlsquellen (Entscheidung Projektinhaber 09.11.2026).
constexpr uint8_t AllSources = 0x07;

struct RelayConfig
{
    const char* name               = "";
    const char* shortName          = "";     ///< kurz und nur ASCII (E-Paper)
    bool        enabled            = true;
    uint8_t     allowedSources     = AllSources;
    bool        startupRequest     = false;  ///< Wunsch nach dem Start
    bool        rememberRequest    = false;  ///< Wunsch nach Schutzabschaltung behalten
    bool        requiresSoc        = false;  ///< ohne gültigen SOC gesperrt
    bool        forceOffWithoutSoc = false;  ///< ohne gültigen SOC zusätzlich aus
    float       lockBelowSoc       = 0.0f;
    float       offBelowSoc        = 0.0f;
    float       releaseAtSoc       = 0.0f;
    uint32_t    minOffTimeMs       = 0;      ///< automatisches Wiedereinschalten frühestens nach
};

/******************************************************************************
 * W4a Schritt 5: In der WebUI änderbare Werte eines Relais (Projektinhaber
 * 09.11.2026). NICHT änderbar: Name, aktiv/deaktiviert, Verhalten ohne SOC
 * (requiresSoc/forceOffWithoutSoc) und erlaubte Befehlsquellen.
 ******************************************************************************/
struct RelayLimits
{
    float    lockBelowSoc    = 0.0f;   ///< Sperre unter (%)
    float    offBelowSoc     = 0.0f;   ///< Aus unter (%)
    float    releaseAtSoc    = 0.0f;   ///< Frei ab (%)
    uint32_t minOffTimeMs    = 0;      ///< Mindest-Auszeit
    bool     startupRequest  = false;  ///< Zustand nach Neustart (Wunsch ein)
    bool     rememberRequest = false;  ///< nach Schutzabschaltung automatisch wieder ein

    /// Längste erlaubte Mindest-Auszeit (1 h).
    static constexpr uint32_t MaxMinOffTimeMs = 3600000;

    /// Änderbare Werte aus einer Relais-Konfiguration übernehmen.
    static RelayLimits from(const RelayConfig& config);

    /// Werte in eine Relais-Konfiguration schreiben (Rest bleibt).
    void applyTo(RelayConfig& config) const;

    /// Plausibilität prüfen. nullptr = in Ordnung, sonst Fehlertext.
    /// Regeln: 0 <= Aus <= Sperre <= Frei <= 100, Auszeit <= 1 h.
    const char* validate() const;
};

struct LoadControllerConfig
{
    RelayConfig relays[RelayCount];

    /// Voreinstellung Camper (Wechselrichter, Licht, Kühlschrank)
    static LoadControllerConfig camper();

    /// Voreinstellung Balkonkraftwerk (DC-DC, R2/R3 deaktiviert)
    static LoadControllerConfig balcony();
};

enum class CommandResult : uint8_t
{
    Accepted,           ///< ausgeführt
    Deferred,           ///< Wunsch gespeichert, wird bei ausreichendem SOC ausgeführt
    RejectedLocked,     ///< SOC zu niedrig oder kein gültiger SOC
    RejectedSource,     ///< Befehlsquelle für dieses Relais nicht erlaubt
    RejectedDisabled,   ///< Relais ist deaktiviert
    RejectedInvalid     ///< W4a: ungültige Einstellungswerte
};

/// Aktueller Sollzustand eines Relais.
struct RelayStatus
{
    bool        on          = false;  ///< soll eingeschaltet sein
    bool        locked      = false;  ///< Einschalten gesperrt
    bool        requested   = false;  ///< Schaltwunsch "ein"
    const char* reason      = "";     ///< Grund der letzten Änderung
    uint32_t    changedMs   = 0;      ///< Zeitpunkt der letzten Änderung
    uint32_t    switchCount = 0;      ///< Anzahl Schaltvorgänge seit Start
    uint32_t    lastOffMs   = 0;      ///< Zeitpunkt des letzten Ausschaltens
};

class LoadController
{
public:

    explicit LoadController(const LoadControllerConfig& config = LoadControllerConfig::camper());

    /// Konfiguration ändern (z. B. später aus der WebUI), Zustand bleibt.
    void setConfig(const LoadControllerConfig& config) { m_config = config; }
    const LoadControllerConfig& config() const       { return m_config; }

    /// W4a: Änderbare Werte eines Relais setzen (WebUI). Zustand und Wunsch
    /// bleiben; die neuen Schwellen wirken beim nächsten update().
    /// Abgelehnt bei deaktiviertem Relais oder ungültigen Werten.
    CommandResult setLimits(RelayId relay, const RelayLimits& limits);

    /// Zustand nach (Neu-)Start herstellen. Override wird beendet.
    void start(uint32_t nowMs);

    /// Schaltbefehl für ein Relais.
    CommandResult request(RelayId relay, bool on, CommandSource source,
                          const SystemState& state, uint32_t nowMs);

    /// Override ein-/ausschalten (umgeht alle Sperren).
    void setOverride(bool active, const SystemState& state, uint32_t nowMs);
    bool overrideActive() const { return m_override; }

    /// Regelmäßig aufrufen (z. B. 1×/s).
    void update(const SystemState& state, uint32_t nowMs);

    const RelayStatus& status(RelayId relay) const
    {
        return m_status[static_cast<std::size_t>(relay)];
    }

    static const char* resultText(CommandResult result);

private:

    void setRelay(std::size_t index, bool on, const char* reason, uint32_t nowMs);
    void updateLock(std::size_t index, const SystemState& state);
    void updateRelay(std::size_t index, const SystemState& state, uint32_t nowMs);

    LoadControllerConfig m_config;
    RelayStatus          m_status[RelayCount];
    bool                 m_override = false;
};
