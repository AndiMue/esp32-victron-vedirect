/******************************************************************************
 * @file LoadController.hpp
 *
 * Abschaltlogik (Stufe 4b): entscheidet anhand des Systemzustands, welche
 * Relais ein- oder ausgeschaltet sein SOLLEN. Schaltet selbst keine GPIOs
 * (das kommt in Stufe 4c).
 *
 * Relais (Entscheidung Projektinhaber 06.11.2026):
 *   R1 Hauptverbraucher  Camper: Wechselrichter, Balkonkraftwerk: DC-DC
 *   R2 Licht             (nur Camper)
 *   R3 Kühlschrank       (nur Camper)
 *
 * Schwellen je Relais (alle konfigurierbar, SOC in %):
 *   lockBelowSoc   unter diesem SOC darf das Relais NICHT eingeschaltet werden
 *   offBelowSoc    unter diesem SOC wird es zwangsweise ausgeschaltet
 *   releaseAtSoc   ab diesem SOC ist die Sperre wieder aufgehoben (Hysterese)
 *
 *   Beispiel R1: gesperrt < 25 %, aus < 15 %, frei ab 30 %.
 *   -> Ein laufender Wechselrichter läuft bei 20 % weiter, kann aber nach
 *      dem Ausschalten erst ab 30 % wieder eingeschaltet werden.
 *
 * Betriebsarten:
 *   manual = true   schaltet nur auf Befehl ein (R1). Erlaubte Befehlsquellen
 *                   je Anlage (Camper: Taster/Touch + WebUI, BKW: MQTT + WebUI).
 *                   rememberRequest = true (BKW): Ein gespeicherter Wunsch "ein"
 *                   wird nach einer Schutzabschaltung bei ausreichendem SOC
 *                   automatisch wieder ausgeführt.
 *   manual = false  schaltet automatisch (R2, R3), frühestens minOffTimeMs nach
 *                   dem Ausschalten wieder ein (Schutz z. B. für Kompressor).
 *
 * Ohne gültigen SOC (Shunt fehlt, nicht synchronisiert):
 *   requiresSoc = true   (R1)   gesperrt; forceOffWithoutSoc = true -> aus
 *   requiresSoc = false  (R2/3) Zustand bleibt (Kleinverbraucher laufen weiter)
 *
 * Override: umgeht ALLE Sperren. Manuelle Relais folgen nur dem Befehl,
 * automatische sind ein. Wird nicht gespeichert -> nach Neustart Normalbetrieb.
 *
 * Nach dem Start (start()): manuelle Relais aus, automatische Relais
 * gemäß onAtStartup (Camper: R2/R3 ein).
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

struct RelayConfig
{
    const char* name               = "";
    const char* shortName          = "";     ///< kurz und nur ASCII (E-Paper)
    bool        enabled            = true;
    bool        manual             = false;
    uint8_t     allowedSources     = 0;      ///< nur manuell: Bitmaske sourceBit()
    bool        rememberRequest    = false;  ///< nur manuell: Wunsch nach Schutzabschaltung behalten
    bool        onAtStartup        = false;  ///< nur automatisch
    bool        requiresSoc        = false;  ///< ohne gültigen SOC gesperrt
    bool        forceOffWithoutSoc = false;  ///< ohne gültigen SOC zusätzlich aus
    float       lockBelowSoc       = 0.0f;
    float       offBelowSoc        = 0.0f;
    float       releaseAtSoc       = 0.0f;
    uint32_t    minOffTimeMs       = 0;      ///< nur automatisch
};

struct LoadControllerConfig
{
    RelayConfig relays[RelayCount];

    /// Voreinstellung Camper (Wechselrichter per Taster, Licht, Kühlschrank)
    static LoadControllerConfig camper();

    /// Voreinstellung Balkonkraftwerk (DC-DC per MQTT, R2/R3 deaktiviert)
    static LoadControllerConfig balcony();
};

enum class CommandResult : uint8_t
{
    Accepted,           ///< ausgeführt
    Deferred,           ///< gespeichert, wird bei ausreichendem SOC ausgeführt
    RejectedLocked,     ///< SOC zu niedrig oder kein gültiger SOC
    RejectedSource,     ///< Befehlsquelle für dieses Relais nicht erlaubt
    RejectedNotManual,  ///< Relais schaltet automatisch
    RejectedDisabled    ///< Relais ist deaktiviert
};

/// Aktueller Sollzustand eines Relais.
struct RelayStatus
{
    bool        on          = false;  ///< soll eingeschaltet sein
    bool        locked      = false;  ///< Einschalten gesperrt
    bool        requested   = false;  ///< nur manuell: Wunsch "ein" liegt vor
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
