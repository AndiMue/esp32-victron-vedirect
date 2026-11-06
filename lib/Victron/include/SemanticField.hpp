/******************************************************************************
 * @file SemanticField.hpp
 *
 * Fachliche Bedeutung eines VE.Direct-Feldes.
 *
 * Unabhängig vom eigentlichen Label ("V", "I", "H19" ...). Spätere Schichten
 * (Gerätezustand, Display, Relaislogik) arbeiten mit diesen Werten und
 * müssen die VE.Direct-Kürzel nicht kennen.
 *
 * Die Zuordnung Label -> SemanticField steht im FieldCatalog.
 ******************************************************************************/

#pragma once

#include <cstdint>

enum class SemanticField : uint16_t
{
    Unknown = 0,

    // ---- Geräteinformationen ------------------------------------------------
    ProductId,              ///< PID
    FirmwareVersion,        ///< FW
    FirmwareVersionExt,     ///< FWE (24 Bit)
    SerialNumber,           ///< SER#
    ModelDescription,       ///< BMV (veraltet)

    // ---- Batterie (Shunt und MPPT) -------------------------------------------
    BatteryVoltage,         ///< V
    BatteryCurrent,         ///< I
    BatteryPower,           ///< P
    BatteryTemperature,     ///< T
    AuxVoltage,             ///< VS  (Starterbatterie)
    MidPointVoltage,        ///< VM
    MidPointDeviation,      ///< DM
    ConsumedAmpHours,       ///< CE
    StateOfCharge,          ///< SOC
    TimeToGo,               ///< TTG

    // ---- Alarme / Relais ----------------------------------------------------
    AlarmActive,            ///< Alarm
    RelayState,             ///< Relay
    AlarmReason,            ///< AR
    MonitorMode,            ///< MON

    // ---- Historie Batteriemonitor (H1..H18) ---------------------------------
    DepthDeepestDischarge,  ///< H1
    DepthLastDischarge,     ///< H2
    DepthAverageDischarge,  ///< H3
    ChargeCycles,           ///< H4
    FullDischarges,         ///< H5
    CumulativeAmpHours,     ///< H6
    MinimumBatteryVoltage,  ///< H7
    MaximumBatteryVoltage,  ///< H8
    SecondsSinceFullCharge, ///< H9
    AutoSynchronizations,   ///< H10
    LowVoltageAlarms,       ///< H11
    HighVoltageAlarms,      ///< H12
    LowAuxVoltageAlarms,    ///< H13
    HighAuxVoltageAlarms,   ///< H14
    MinimumAuxVoltage,      ///< H15
    MaximumAuxVoltage,      ///< H16
    DischargedEnergy,       ///< H17
    ChargedEnergy,          ///< H18

    // ---- Solarladeregler ----------------------------------------------------
    PvVoltage,              ///< VPV
    PvPower,                ///< PPV
    LoadCurrent,            ///< IL
    LoadOutput,             ///< LOAD
    ChargerState,           ///< CS
    TrackerMode,            ///< MPPT
    ErrorCode,              ///< ERR
    OffReason,              ///< OR
    BleCapabilities,        ///< CAP_BLE
    YieldTotal,             ///< H19
    YieldToday,             ///< H20
    MaximumPowerToday,      ///< H21
    YieldYesterday,         ///< H22
    MaximumPowerYesterday,  ///< H23
    DaySequence,            ///< HSDS

    Count                   ///< Anzahl der Einträge (kein Feld, immer zuletzt!)
};
