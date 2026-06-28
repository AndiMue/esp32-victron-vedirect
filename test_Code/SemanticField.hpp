#pragma once

namespace victron {

enum class SemanticField {
    Unknown,

    BatteryVoltage,
    BatteryCurrent,
    StateOfCharge,
    TimeToGo,
    PanelVoltage,
    PanelCurrent,
    PanelPower,
    ChargerState,
    ErrorCode,
    FirmwareVersion,
    SerialNumber,
    ProductId,
    DeviceMode,
    RelayState
};

} // namespace victron