/******************************************************************************
 * @file SystemState.cpp
 ******************************************************************************/

#include "SystemState.hpp"

#include "FieldCatalog.hpp"

namespace
{

/// Frischen Wert eines Kanals als Reading holen.
Reading readValue(const DeviceState& state, SemanticField field,
                  ValueSource source, uint32_t nowMs, uint32_t maxAgeMs)
{
    Reading reading;
    float value = 0.0f;
    if (state.freshValue(field, nowMs, maxAgeMs, value))
    {
        reading.valid  = true;
        reading.value  = value;
        reading.source = source;
    }
    return reading;
}

Severity maxSeverity(Severity a, Severity b)
{
    return static_cast<uint8_t>(a) >= static_cast<uint8_t>(b) ? a : b;
}

} // namespace

/******************************************************************************
 * Schwere je Fehlerbit
 ******************************************************************************/
Severity SystemEvaluator::severityOf(uint32_t faultBit, const SystemConfig& config)
{
    switch (faultBit)
    {
        case SystemFault::BatteryMonitorMissing:
            return config.requireBatteryMonitor ? Severity::Critical : Severity::Info;
        case SystemFault::MultipleBatteryMonitors: return Severity::Critical;
        case SystemFault::SocNotAvailable:
            return config.requireBatteryMonitor ? Severity::Critical : Severity::Info;
        case SystemFault::BatteryAlarm:            return Severity::Warning;
        case SystemFault::BatteryLinkUnstable:     return Severity::Warning;
        case SystemFault::PvChargerMissing:
            return config.requirePvCharger ? Severity::Warning : Severity::Info;
        case SystemFault::MultiplePvChargers:      return Severity::Info;
        case SystemFault::PvChargerError:          return Severity::Warning;
        case SystemFault::PvLinkUnstable:          return Severity::Warning;
        case SystemFault::UnknownDevice:           return Severity::Info;
        default:                                   return Severity::None;
    }
}

const char* SystemEvaluator::faultText(uint32_t faultBit)
{
    switch (faultBit)
    {
        case SystemFault::BatteryMonitorMissing:   return "Batteriemonitor (Shunt) fehlt";
        case SystemFault::MultipleBatteryMonitors: return "Mehrere Batteriemonitore";
        case SystemFault::SocNotAvailable:         return "SOC nicht verfügbar";
        case SystemFault::BatteryAlarm:            return "Batteriealarm";
        case SystemFault::BatteryLinkUnstable:     return "Verbindung Shunt instabil";
        case SystemFault::PvChargerMissing:        return "MPPT fehlt";
        case SystemFault::MultiplePvChargers:      return "Mehrere MPPT";
        case SystemFault::PvChargerError:          return "MPPT-Fehler";
        case SystemFault::PvLinkUnstable:          return "Verbindung MPPT instabil";
        case SystemFault::UnknownDevice:           return "Unbekanntes Gerät";
        default:                                   return "?";
    }
}

const char* SystemEvaluator::severityText(Severity severity)
{
    switch (severity)
    {
        case Severity::Critical: return "KRITISCH";
        case Severity::Warning:  return "Warnung";
        case Severity::Info:     return "Info";
        default:                 return "";
    }
}

/******************************************************************************
 * Gesamtzustand berechnen
 ******************************************************************************/
SystemState SystemEvaluator::evaluate(const ChannelView* channels, std::size_t count,
                                      const SystemConfig& config, uint32_t nowMs)
{
    SystemState result;
    const uint32_t maxAge = config.valueMaxAgeMs;

    /**************************************************************************
     * 1. Rollen nach Gerätekategorie bestimmen
     **************************************************************************/
    int batteryCount = 0;
    int pvCount      = 0;
    int batteryIndex = -1;
    int pvIndex      = -1;

    for (std::size_t i = 0; i < count; ++i)
    {
        const DeviceState* state = channels[i].state;
        if (state == nullptr || !state->isIdentified())
        {
            continue;
        }

        const DeviceInfo* device = state->identity().device;
        if (device == nullptr)
        {
            result.faults |= SystemFault::UnknownDevice;
            continue;
        }

        if (device->category == DeviceCategory::BatteryMonitor)
        {
            ++batteryCount;
            batteryIndex = static_cast<int>(i);
        }
        else if (device->category == DeviceCategory::Mppt)
        {
            ++pvCount;
            pvIndex = static_cast<int>(i);
        }
    }

    if (batteryCount == 0)
    {
        result.faults |= SystemFault::BatteryMonitorMissing;
    }
    else if (batteryCount > 1)
    {
        // Zuordnung unklar -> keine Batterierolle (wie Ausfall Shunt)
        result.faults |= SystemFault::MultipleBatteryMonitors;
    }
    else
    {
        result.batteryChannel = batteryIndex;
    }

    if (pvCount == 0)
    {
        result.faults |= SystemFault::PvChargerMissing;
    }
    else if (pvCount > 1)
    {
        result.faults |= SystemFault::MultiplePvChargers;
    }
    else
    {
        result.pvChannel = pvIndex;
    }

    /**************************************************************************
     * 2. Batteriewerte vom Shunt
     **************************************************************************/
    if (result.batteryChannel >= 0)
    {
        const ChannelView& ch    = channels[result.batteryChannel];
        const DeviceState& shunt = *ch.state;
        const ValueSource  src   = ValueSource::BatteryMonitor;

        result.batteryVoltage   = readValue(shunt, SemanticField::BatteryVoltage,   src, nowMs, maxAge);
        result.batteryCurrent   = readValue(shunt, SemanticField::BatteryCurrent,   src, nowMs, maxAge);
        result.batteryPower     = readValue(shunt, SemanticField::BatteryPower,     src, nowMs, maxAge);
        result.stateOfCharge    = readValue(shunt, SemanticField::StateOfCharge,    src, nowMs, maxAge);
        result.consumedAmpHours = readValue(shunt, SemanticField::ConsumedAmpHours, src, nowMs, maxAge);
        result.timeToGo         = readValue(shunt, SemanticField::TimeToGo,         src, nowMs, maxAge);

        // TTG = -1 ist kein "gültiger" Zahlenwert, aber eine gültige Aussage
        const DeviceState::Entry* ttg = shunt.entry(SemanticField::TimeToGo);
        result.timeToGoInfinite =
            ttg != nullptr && ttg->value.status == ValueStatus::Infinite &&
            DeviceState::ageMs(ttg->updatedMs, nowMs) <= maxAge;

        // Alarm: "Alarm = ON" oder Alarmgrund != 0
        int64_t alarm = 0;
        int64_t reason = 0;
        if ((shunt.freshRaw(SemanticField::AlarmActive, nowMs, maxAge, alarm) && alarm != 0) ||
            (shunt.freshRaw(SemanticField::AlarmReason, nowMs, maxAge, reason) && reason != 0))
        {
            result.faults |= SystemFault::BatteryAlarm;
        }

        if (ch.link != nullptr && ch.link->isUnstable())
        {
            result.faults |= SystemFault::BatteryLinkUnstable;
        }
    }

    // Ohne gültigen SOC ist keine SOC-Logik möglich (auch wenn der Shunt fehlt)
    if (!result.stateOfCharge.valid)
    {
        result.faults |= SystemFault::SocNotAvailable;
    }

    /**************************************************************************
     * 3. PV-Werte vom MPPT
     **************************************************************************/
    if (result.pvChannel >= 0)
    {
        const ChannelView& ch   = channels[result.pvChannel];
        const DeviceState& mppt = *ch.state;
        const ValueSource  src  = ValueSource::PvCharger;

        result.pvPower    = readValue(mppt, SemanticField::PvPower,    src, nowMs, maxAge);
        result.pvVoltage  = readValue(mppt, SemanticField::PvVoltage,  src, nowMs, maxAge);
        result.yieldToday = readValue(mppt, SemanticField::YieldToday, src, nowMs, maxAge);

        int64_t cs = 0;
        if (mppt.freshRaw(SemanticField::ChargerState, nowMs, maxAge, cs))
        {
            result.chargerState = static_cast<int32_t>(cs);
            const DeviceState::Entry* e = mppt.entry(SemanticField::ChargerState);
            result.chargerStateText = FieldCatalog::codeText(*e->value.definition, cs);
        }

        int64_t err = 0;
        if (mppt.freshRaw(SemanticField::ErrorCode, nowMs, maxAge, err))
        {
            result.pvErrorCode = static_cast<int32_t>(err);
            if (err != 0)
            {
                result.faults |= SystemFault::PvChargerError;
            }
        }

        if (ch.link != nullptr && ch.link->isUnstable())
        {
            result.faults |= SystemFault::PvLinkUnstable;
        }

        // Ersatz: Batteriespannung vom MPPT, wenn KEIN Shunt zugeordnet ist
        // (fehlt oder mehrere). Gekennzeichnet über source == PvCharger.
        if (result.batteryChannel < 0)
        {
            result.batteryVoltage = readValue(mppt, SemanticField::BatteryVoltage, src, nowMs, maxAge);
        }
    }

    /**************************************************************************
     * 4. Höchste Schwere
     **************************************************************************/
    for (unsigned bit = 0; bit < SystemFault::BitCount; ++bit)
    {
        const uint32_t mask = 1u << bit;
        if ((result.faults & mask) != 0)
        {
            result.severity = maxSeverity(result.severity, severityOf(mask, config));
        }
    }

    return result;
}
