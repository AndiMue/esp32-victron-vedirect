/******************************************************************************
 * @file LoadController.cpp
 *
 * Regeln siehe LoadController.hpp.
 ******************************************************************************/

#include "LoadController.hpp"

#include "DeviceState.hpp"   // DeviceState::ageMs (überlaufsichere Zeitdifferenz)

namespace
{

// Gründe (statische Texte, sicher über Task-Grenzen weiterzugeben)
const char* const ReasonStartup      = "Startzustand";
const char* const ReasonCommandOn    = "Befehl ein";
const char* const ReasonCommandOff   = "Befehl aus";
const char* const ReasonSocLow       = "SOC unter Abschaltschwelle";
const char* const ReasonNoSoc        = "kein gültiger SOC";
const char* const ReasonSocRecovered = "SOC wieder ausreichend";
const char* const ReasonRestored     = "gespeicherter Wunsch ausgeführt";
const char* const ReasonOverride     = "Override";
const char* const ReasonOverrideEnd  = "Override beendet";
const char* const ReasonDisabled     = "deaktiviert";

bool socValid(const SystemState& state)
{
    return state.stateOfCharge.valid;
}

} // namespace

/******************************************************************************
 * Voreinstellungen
 ******************************************************************************/
LoadControllerConfig LoadControllerConfig::camper()
{
    LoadControllerConfig c;

    RelayConfig& main = c.relays[static_cast<std::size_t>(RelayId::Main)];
    main.name               = "Wechselrichter";
    main.shortName          = "WR";
    main.manual             = true;
    main.allowedSources     = sourceBit(CommandSource::Local) | sourceBit(CommandSource::WebUi);
    main.rememberRequest    = false;   // nach Schutzabschaltung neu per Taster einschalten
    main.requiresSoc        = true;    // ohne Shunt kein Wechselrichter
    main.forceOffWithoutSoc = true;
    main.lockBelowSoc       = 25.0f;
    main.offBelowSoc        = 15.0f;
    main.releaseAtSoc       = 30.0f;

    RelayConfig& light = c.relays[static_cast<std::size_t>(RelayId::Light)];
    light.name         = "Licht";
    light.shortName    = "Licht";
    light.onAtStartup  = true;
    light.lockBelowSoc = 15.0f;
    light.offBelowSoc  = 15.0f;
    light.releaseAtSoc = 20.0f;
    light.minOffTimeMs = 60000;

    RelayConfig& fridge = c.relays[static_cast<std::size_t>(RelayId::Fridge)];
    fridge.name         = "Kühlschrank";
    fridge.shortName    = "Kuehl";
    fridge.onAtStartup  = true;
    fridge.lockBelowSoc = 5.0f;
    fridge.offBelowSoc  = 5.0f;
    fridge.releaseAtSoc = 10.0f;
    fridge.minOffTimeMs = 60000;

    return c;
}

LoadControllerConfig LoadControllerConfig::balcony()
{
    LoadControllerConfig c = camper();

    RelayConfig& main = c.relays[static_cast<std::size_t>(RelayId::Main)];
    main.name            = "DC-DC-Wandler";
    main.shortName       = "DC-DC";
    main.allowedSources  = sourceBit(CommandSource::Mqtt) | sourceBit(CommandSource::WebUi);
    main.rememberRequest = true;   // Hausautomatisierung: Wunsch bleibt bestehen

    c.relays[static_cast<std::size_t>(RelayId::Light)].enabled  = false;
    c.relays[static_cast<std::size_t>(RelayId::Fridge)].enabled = false;

    return c;
}

/******************************************************************************
 * LoadController
 ******************************************************************************/
LoadController::LoadController(const LoadControllerConfig& config)
    : m_config(config)
{
    start(0);
}

void LoadController::start(uint32_t nowMs)
{
    m_override = false;

    for (std::size_t i = 0; i < RelayCount; ++i)
    {
        const RelayConfig& cfg = m_config.relays[i];
        RelayStatus& st = m_status[i];

        st = RelayStatus();
        st.on        = cfg.enabled && !cfg.manual && cfg.onAtStartup;
        st.reason    = cfg.enabled ? ReasonStartup : ReasonDisabled;
        st.changedMs = nowMs;
        st.lastOffMs = nowMs;
    }
}

void LoadController::setRelay(std::size_t index, bool on, const char* reason, uint32_t nowMs)
{
    RelayStatus& st = m_status[index];
    if (st.on == on)
    {
        return;
    }
    st.on        = on;
    st.reason    = reason;
    st.changedMs = nowMs;
    ++st.switchCount;
    if (!on)
    {
        st.lastOffMs = nowMs;
    }
}

/******************************************************************************
 * Sperre mit Hysterese aktualisieren
 ******************************************************************************/
void LoadController::updateLock(std::size_t index, const SystemState& state)
{
    const RelayConfig& cfg = m_config.relays[index];
    RelayStatus& st = m_status[index];

    if (socValid(state))
    {
        const float soc = state.stateOfCharge.value;
        if (soc < cfg.lockBelowSoc)
        {
            st.locked = true;
        }
        else if (soc >= cfg.releaseAtSoc)
        {
            st.locked = false;
        }
        // dazwischen: Zustand halten (Hysterese)
    }
    else if (cfg.requiresSoc)
    {
        st.locked = true;
    }
    // ohne SOC und !requiresSoc: Sperre bleibt, wie sie ist
}

/******************************************************************************
 * Ein Relais auswerten
 ******************************************************************************/
void LoadController::updateRelay(std::size_t index, const SystemState& state, uint32_t nowMs)
{
    const RelayConfig& cfg = m_config.relays[index];
    RelayStatus& st = m_status[index];

    if (!cfg.enabled)
    {
        st.requested = false;
        setRelay(index, false, ReasonDisabled, nowMs);
        return;
    }

    // ---- Override: alle Sperren umgehen ----
    if (m_override)
    {
        const bool desired = cfg.manual ? st.requested : true;
        setRelay(index, desired, ReasonOverride, nowMs);
        return;
    }

    updateLock(index, state);

    // ---- Zwangsabschaltung ----
    const bool valid = socValid(state);
    const bool socTooLow = valid && state.stateOfCharge.value < cfg.offBelowSoc;
    const bool noSocOff  = !valid && cfg.requiresSoc && cfg.forceOffWithoutSoc;

    if (socTooLow || noSocOff)
    {
        setRelay(index, false, socTooLow ? ReasonSocLow : ReasonNoSoc, nowMs);
        if (cfg.manual && !cfg.rememberRequest)
        {
            st.requested = false;   // Camper: neu per Taster einschalten
        }
        return;
    }

    if (st.locked || st.on)
    {
        return;   // gesperrt oder bereits ein: nichts zu tun
    }

    // ---- Einschalten ----
    if (cfg.manual)
    {
        if (cfg.rememberRequest && st.requested)
        {
            setRelay(index, true, ReasonRestored, nowMs);
        }
        return;
    }

    // Automatisches Relais: nicht gesperrt und lange genug aus -> ein.
    // (Ohne gültigen SOC bleibt eine bestehende Sperre erhalten, siehe
    // updateLock(); ein nie gesperrtes Relais darf dann weiterlaufen.)
    if (DeviceState::ageMs(st.lastOffMs, nowMs) >= cfg.minOffTimeMs)
    {
        setRelay(index, true, ReasonSocRecovered, nowMs);
    }
}

void LoadController::update(const SystemState& state, uint32_t nowMs)
{
    for (std::size_t i = 0; i < RelayCount; ++i)
    {
        updateRelay(i, state, nowMs);
    }
}

/******************************************************************************
 * Befehle
 ******************************************************************************/
CommandResult LoadController::request(RelayId relay, bool on, CommandSource source,
                                      const SystemState& state, uint32_t nowMs)
{
    const std::size_t index = static_cast<std::size_t>(relay);
    const RelayConfig& cfg = m_config.relays[index];
    RelayStatus& st = m_status[index];

    if (!cfg.enabled)                                return CommandResult::RejectedDisabled;
    if (!cfg.manual)                                 return CommandResult::RejectedNotManual;
    if ((cfg.allowedSources & sourceBit(source)) == 0) return CommandResult::RejectedSource;

    if (!on)
    {
        // Ausschalten ist immer erlaubt
        st.requested = false;
        setRelay(index, false, ReasonCommandOff, nowMs);
        return CommandResult::Accepted;
    }

    if (m_override)
    {
        st.requested = true;
        setRelay(index, true, ReasonOverride, nowMs);
        return CommandResult::Accepted;
    }

    updateLock(index, state);
    if (st.locked)
    {
        if (cfg.rememberRequest)
        {
            st.requested = true;   // BKW: später automatisch
            return CommandResult::Deferred;
        }
        return CommandResult::RejectedLocked;
    }

    st.requested = true;
    setRelay(index, true, ReasonCommandOn, nowMs);
    return CommandResult::Accepted;
}

void LoadController::setOverride(bool active, const SystemState& state, uint32_t nowMs)
{
    if (m_override == active)
    {
        return;
    }
    m_override = active;

    if (!active)
    {
        // Override beendet: sofort normale Regeln anwenden. Relais, die nur
        // wegen des Overrides an waren, gehen ggf. aus.
        for (std::size_t i = 0; i < RelayCount; ++i)
        {
            const bool wasOn = m_status[i].on;
            updateRelay(i, state, nowMs);
            if (wasOn && !m_status[i].on)
            {
                m_status[i].reason = ReasonOverrideEnd;
            }
        }
        return;
    }

    update(state, nowMs);
}

const char* LoadController::resultText(CommandResult result)
{
    switch (result)
    {
        case CommandResult::Accepted:          return "ausgeführt";
        case CommandResult::Deferred:          return "gespeichert, wird bei ausreichendem SOC ausgeführt";
        case CommandResult::RejectedLocked:    return "abgelehnt: SOC zu niedrig oder kein gültiger SOC";
        case CommandResult::RejectedSource:    return "abgelehnt: Befehlsquelle nicht erlaubt";
        case CommandResult::RejectedNotManual: return "abgelehnt: Relais schaltet automatisch";
        case CommandResult::RejectedDisabled:  return "abgelehnt: Relais deaktiviert";
    }
    return "?";
}
