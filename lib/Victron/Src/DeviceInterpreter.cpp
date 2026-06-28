/******************************************************************************
 * @file DeviceInterpreter.cpp
 *
 * Ausgabe erkannter Geräteinformationen.
 ******************************************************************************/

#include "DeviceInterpreter.hpp"

#include <Arduino.h>

#include "DeviceCatalog.hpp"

/******************************************************************************
 * Geräteinformationen ausgeben.
 ******************************************************************************/
void DeviceInterpreter::print(const VeDirectFrame& frame)
{
    const VeDirectField* pidField =
        frame.find("PID");

    if (pidField == nullptr)
    {
        return;
    }

    const DeviceInfo* device =
        DeviceCatalog::find(
            pidField->value.data());

    if (device == nullptr)
    {
        return;
    }

    Serial.println();
    Serial.println("=========== DEVICE ===========");

    Serial.print("Name      : ");
    Serial.println(device->name);

    Serial.print("Category  : ");

    switch (device->category)
    {
        case DeviceCategory::Mppt:
            Serial.println("MPPT");
            break;

        case DeviceCategory::BatteryMonitor:
            Serial.println("Battery Monitor");
            break;

        case DeviceCategory::DcDcCharger:
            Serial.println("DC-DC Charger");
            break;

        default:
            Serial.println("Unknown");
            break;
    }

    Serial.println("==============================");
}