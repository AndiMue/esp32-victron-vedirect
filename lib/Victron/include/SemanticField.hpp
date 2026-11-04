/******************************************************************************
 * @file SemanticField.hpp
 *
 * Semantische Bedeutung eines VE.Direct-Feldes.
 *
 * Diese Enum ist unabhängig vom eigentlichen Label ("V", "I", "H19"...)
 * und beschreibt ausschließlich die fachliche Bedeutung.
 ******************************************************************************/

#pragma once

#include <cstdint>

enum class SemanticField : uint16_t
{
    Unknown = 0,

    ProductId,
    FirmwareVersion,
    SerialNumber,

    BatteryVoltage,
    BatteryCurrent,

    PvVoltage,
    PvPower,

    ChargerState,
    MpptState,

    RelayState,

    ErrorCode,

    OffReason, 
    
    YieldTotal,
    YieldToday,
    YieldYesterday,

    MaximumPowerToday,
    MaximumPowerYesterday,

    DaySequence,

    BatteryPower,

    StateOfCharge,
    TimeToGo,

    AlarmState,
    AlarmReason,

    ConsumedAmpHours,

    DepthDeepestDischarge,
    DepthLastDischarge,
    DepthAverageDischarge,

    ChargeCycles,
    FullDischarges,

    CumulativeAmpHours,

    MinimumBatteryVoltage,
    MaximumBatteryVoltage,

    SecondsSinceFullCharge,

    AutoSynchronizations,

    LowVoltageAlarms,
    HighVoltageAlarms,

    MinimumAuxVoltage,
    MaximumAuxVoltage,

    DischargedEnergy,
    ChargedEnergy,

    DeviceName,
    MonitorMode
};