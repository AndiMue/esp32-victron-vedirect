/******************************************************************************
 * @file DeviceCatalog.cpp
 *
 * Katalog bekannter VE.Direct-Geräte.
 ******************************************************************************/

#include "DeviceCatalog.hpp"

#include <cstring>

namespace
{

/******************************************************************************
 * Bekannte VE.Direct Geräte.
 ******************************************************************************/
constexpr DeviceInfo Devices[] =
{
    // BMV
    { "0x203", "BMV-700", DeviceCategory::BatteryMonitor },
    { "0x204", "BMV-702", DeviceCategory::BatteryMonitor },
    { "0x205", "BMV-700H", DeviceCategory::BatteryMonitor },

    { "0x381", "BMV-712 Smart", DeviceCategory::BatteryMonitor },
    { "0x382", "BMV-710H Smart", DeviceCategory::BatteryMonitor },
    { "0x383", "BMV-712 Smart Rev2", DeviceCategory::BatteryMonitor },

    // SmartShunt
    { "0x389", "SmartShunt 500A/50mV", DeviceCategory::BatteryMonitor },
    { "0x38A", "SmartShunt 1000A/50mV", DeviceCategory::BatteryMonitor },
    { "0x38B", "SmartShunt 2000A/50mV", DeviceCategory::BatteryMonitor },

    // Orion XS
    { "0x3F0", "Orion XS 12V/12V-50A", DeviceCategory::DcDcCharger },
    { "0x3F1", "Orion XS 1400", DeviceCategory::DcDcCharger },

    // SmartSolar
    { "0xA050", "SmartSolar MPPT 250/100", DeviceCategory::Mppt },
    { "0xA051", "SmartSolar MPPT 150/100", DeviceCategory::Mppt },
    { "0xA052", "SmartSolar MPPT 150/85", DeviceCategory::Mppt },
    { "0xA053", "SmartSolar MPPT 75/15", DeviceCategory::Mppt },
    { "0xA054", "SmartSolar MPPT 75/10", DeviceCategory::Mppt },
    { "0xA055", "SmartSolar MPPT 100/15", DeviceCategory::Mppt },
    { "0xA056", "SmartSolar MPPT 100/30", DeviceCategory::Mppt },
    { "0xA057", "SmartSolar MPPT 100/50", DeviceCategory::Mppt },
    { "0xA058", "SmartSolar MPPT 150/35", DeviceCategory::Mppt },
    { "0xA059", "SmartSolar MPPT 100/100 rev2", DeviceCategory::Mppt },

    // BlueSolar
    { "0xA040", "BlueSolar MPPT 75/50", DeviceCategory::Mppt },
    { "0xA041", "BlueSolar MPPT 150/35", DeviceCategory::Mppt },
    { "0xA042", "BlueSolar MPPT 75/15", DeviceCategory::Mppt },
    { "0xA043", "BlueSolar MPPT 100/15", DeviceCategory::Mppt },
    { "0xA044", "BlueSolar MPPT 100/30", DeviceCategory::Mppt },
    { "0xA045", "BlueSolar MPPT 100/50", DeviceCategory::Mppt }
};

constexpr size_t DeviceCount =
    sizeof(Devices) / sizeof(Devices[0]);

} // namespace

/******************************************************************************
 * Gerät anhand der Product-ID suchen.
 ******************************************************************************/
const DeviceInfo* DeviceCatalog::find(const char* productId)
{
    for (size_t i = 0; i < DeviceCount; ++i)
    {
        if (std::strcmp(Devices[i].productId,
                        productId) == 0)
        {
            return &Devices[i];
        }
    }

    return nullptr;
}