/******************************************************************************
 * @file DeviceInfo.hpp
 *
 * Beschreibung eines bekannten Victron-Geräts.
 ******************************************************************************/

#pragma once

/******************************************************************************
 * Gerätekategorien für den späteren SystemState.
 ******************************************************************************/
enum class DeviceCategory
{
    Unknown,

    Mppt,
    BatteryMonitor,
    DcDcCharger
};

/******************************************************************************
 * Informationen zu einem bekannten Gerät.
 ******************************************************************************/
struct DeviceInfo
{
    const char* productId;

    const char* name;

    DeviceCategory category;
};