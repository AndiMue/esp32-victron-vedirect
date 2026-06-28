#include "DeviceCatalog.hpp"
#include <array>

namespace victron {

namespace {

constexpr std::array<DeviceDescriptor, 3> kDevices = {{
    {
        0xA389,
        DeviceType::SmartShunt,
        "SmartShunt 500A/50mV",
        {
            SemanticField::BatteryVoltage,
            SemanticField::BatteryCurrent,
            SemanticField::StateOfCharge,
            SemanticField::TimeToGo,
            SemanticField::ProductId,
            SemanticField::FirmwareVersion,
            SemanticField::SerialNumber
        }
    },
    {
        0xA38A,
        DeviceType::SmartShunt,
        "SmartShunt 1000A/50mV",
        {
            SemanticField::BatteryVoltage,
            SemanticField::BatteryCurrent,
            SemanticField::StateOfCharge,
            SemanticField::TimeToGo,
            SemanticField::ProductId,
            SemanticField::FirmwareVersion,
            SemanticField::SerialNumber
        }
    },
    {
        0xA057,
        DeviceType::SmartSolarMppt,
        "SmartSolar MPPT 100/50",
        {
            SemanticField::BatteryVoltage,
            SemanticField::BatteryCurrent,
            SemanticField::PanelVoltage,
            SemanticField::PanelCurrent,
            SemanticField::PanelPower,
            SemanticField::ChargerState,
            SemanticField::ErrorCode,
            SemanticField::ProductId,
            SemanticField::FirmwareVersion,
            SemanticField::SerialNumber
        }
    }
}};

constexpr std::array<std::pair<std::string_view, SemanticField>, 12> kFieldMap = {{
    {"V",    SemanticField::BatteryVoltage},
    {"I",    SemanticField::BatteryCurrent},
    {"SOC",  SemanticField::StateOfCharge},
    {"TTG",  SemanticField::TimeToGo},
    {"VPV",  SemanticField::PanelVoltage},
    {"PPV",  SemanticField::PanelPower},
    {"CS",   SemanticField::ChargerState},
    {"ERR",  SemanticField::ErrorCode},
    {"FW",   SemanticField::FirmwareVersion},
    {"FWE",  SemanticField::FirmwareVersion},
    {"PID",  SemanticField::ProductId},
    {"SER#", SemanticField::SerialNumber}
}};

} // namespace

const DeviceDescriptor* DeviceCatalog::findByPid(uint16_t pid) {
    for (const auto& d : kDevices) {
        if (d.pid == pid) {
            return &d;
        }
    }
    return nullptr;
}

DeviceType DeviceCatalog::resolveType(uint16_t pid) {
    if (const auto* d = findByPid(pid)) {
        return d->type;
    }
    return DeviceType::Unknown;
}

std::vector<SemanticField> DeviceCatalog::expectedFields(DeviceType type) {
    for (const auto& d : kDevices) {
        if (d.type == type) {
            return d.expectedFields;
        }
    }
    return {};
}

SemanticField DeviceCatalog::resolveSemanticField(std::string_view label) {
    for (const auto& [name, field] : kFieldMap) {
        if (name == label) {
            return field;
        }
    }
    return SemanticField::Unknown;
}

std::string_view DeviceCatalog::typeName(DeviceType type) {
    switch (type) {
        case DeviceType::SmartShunt:    return "SmartShunt";
        case DeviceType::SmartSolarMppt: return "SmartSolarMPPT";
        case DeviceType::Bmv:            return "BMV";
        case DeviceType::PhoenixInverter:return "PhoenixInverter";
        case DeviceType::PhoenixCharger: return "PhoenixCharger";
        case DeviceType::OrionXs:        return "OrionXS";
        default:                         return "Unknown";
    }
}

} // namespace victron