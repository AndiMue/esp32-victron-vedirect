#pragma once

#include <cstdint>

namespace victron {

enum class DeviceType {
    Unknown,
    SmartShunt,
    SmartSolarMppt,
    Bmv,
    PhoenixInverter,
    PhoenixCharger,
    OrionXs
};

} // namespace victron