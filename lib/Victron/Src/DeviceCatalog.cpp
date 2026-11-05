/******************************************************************************
 * @file DeviceCatalog.cpp
 *
 * Katalog bekannter VE.Direct-Geräte.
 *
 * Product-IDs laut "VE.Direct Protocol" 3.34, Abschnitt "PID".
 * Ausnahme: 0xC035 ist dort nicht gelistet, wird aber von dem im Projekt
 * verwendeten SmartShunt 500A/50mV (Firmware 4.19) gesendet.
 ******************************************************************************/

#include "DeviceCatalog.hpp"

#include <cstddef>

namespace
{

/******************************************************************************
 * Bekannte VE.Direct-Geräte.
 ******************************************************************************/
const DeviceInfo Devices[] =
{
    // BMV
    { "0x203",  "BMV-700",                  DeviceCategory::BatteryMonitor },
    { "0x204",  "BMV-702",                  DeviceCategory::BatteryMonitor },
    { "0x205",  "BMV-700H",                 DeviceCategory::BatteryMonitor },
    { "0xA381", "BMV-712 Smart",            DeviceCategory::BatteryMonitor },
    { "0xA382", "BMV-710H Smart",           DeviceCategory::BatteryMonitor },
    { "0xA383", "BMV-712 Smart Rev2",       DeviceCategory::BatteryMonitor },

    // SmartShunt
    { "0xA389", "SmartShunt 500A/50mV",     DeviceCategory::BatteryMonitor },
    { "0xA38A", "SmartShunt 1000A/50mV",    DeviceCategory::BatteryMonitor },
    { "0xA38B", "SmartShunt 2000A/50mV",    DeviceCategory::BatteryMonitor },
    { "0xC035", "SmartShunt 500A/50mV",     DeviceCategory::BatteryMonitor }, // beobachtet, nicht in Doku 3.34

    // Orion XS
    { "0xA3F0", "Orion XS 12V/12V-50A",     DeviceCategory::DcDcCharger },
    { "0xA3F1", "Orion XS 1400",            DeviceCategory::DcDcCharger },

    // SmartSolar
    { "0xA050", "SmartSolar MPPT 250/100",  DeviceCategory::Mppt },
    { "0xA051", "SmartSolar MPPT 150/100",  DeviceCategory::Mppt },
    { "0xA052", "SmartSolar MPPT 150/85",   DeviceCategory::Mppt },
    { "0xA053", "SmartSolar MPPT 75/15",    DeviceCategory::Mppt },
    { "0xA054", "SmartSolar MPPT 75/10",    DeviceCategory::Mppt },
    { "0xA055", "SmartSolar MPPT 100/15",   DeviceCategory::Mppt },
    { "0xA056", "SmartSolar MPPT 100/30",   DeviceCategory::Mppt },
    { "0xA057", "SmartSolar MPPT 100/50",   DeviceCategory::Mppt },
    { "0xA058", "SmartSolar MPPT 150/35",   DeviceCategory::Mppt },
    { "0xA059", "SmartSolar MPPT 150/100 rev2", DeviceCategory::Mppt },

    // BlueSolar
    { "0xA040", "BlueSolar MPPT 75/50",     DeviceCategory::Mppt },
    { "0xA041", "BlueSolar MPPT 150/35",    DeviceCategory::Mppt },
    { "0xA042", "BlueSolar MPPT 75/15",     DeviceCategory::Mppt },
    { "0xA043", "BlueSolar MPPT 100/15",    DeviceCategory::Mppt },
    { "0xA044", "BlueSolar MPPT 100/30",    DeviceCategory::Mppt },
    { "0xA045", "BlueSolar MPPT 100/50",    DeviceCategory::Mppt },
};

constexpr std::size_t DeviceCount = sizeof(Devices) / sizeof(Devices[0]);

/// Vergleich ohne Groß-/Kleinschreibung ("0xa057" == "0xA057").
bool equalsIgnoreCase(const char* a, const char* b)
{
    for (; *a != '\0' && *b != '\0'; ++a, ++b)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 'a' + 'A');
        if (ca != cb)
        {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

} // namespace

/******************************************************************************
 * Gerät anhand der Product-ID (Text wie übertragen, z. B. "0xA057") suchen.
 ******************************************************************************/
const DeviceInfo* DeviceCatalog::find(const char* productId)
{
    if (productId == nullptr)
    {
        return nullptr;
    }

    for (std::size_t i = 0; i < DeviceCount; ++i)
    {
        if (equalsIgnoreCase(Devices[i].productId, productId))
        {
            return &Devices[i];
        }
    }
    return nullptr;
}
