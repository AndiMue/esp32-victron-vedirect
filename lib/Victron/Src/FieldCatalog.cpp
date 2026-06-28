/******************************************************************************
 * @file FieldCatalog.cpp
 ******************************************************************************/

#include "FieldCatalog.hpp"

#include <cstring>

namespace
{

constexpr FieldDefinition Catalog[] =
{
    { "PID",  "Product ID",        SemanticField::ProductId,             ValueType::Hex,     1.0f,   "" },
    { "FW",   "Firmware Version",  SemanticField::FirmwareVersion,       ValueType::Integer, 1.0f,   "" },
    { "SER#", "Serial Number",     SemanticField::SerialNumber,          ValueType::String,  1.0f,   "" },
    { "V",    "Battery Voltage",   SemanticField::BatteryVoltage,        ValueType::Float,   0.001f, "V" },
    { "I",    "Battery Current",   SemanticField::BatteryCurrent,        ValueType::Float,   0.001f, "A" },
    { "VPV",  "PV Voltage",        SemanticField::PvVoltage,             ValueType::Float,   0.001f, "V" },
    { "PPV",  "PV Power",          SemanticField::PvPower,               ValueType::Integer, 1.0f,   "W" },
    { "CS",   "Charge State",      SemanticField::ChargerState,          ValueType::Integer, 1.0f,   "" },
    { "MPPT", "MPPT State",        SemanticField::MpptState,             ValueType::Integer, 1.0f,   "" },
    { "ERR",  "Error Code",        SemanticField::ErrorCode,             ValueType::Integer, 1.0f,   "" },
    { "LOAD", "Load Output",       SemanticField::RelayState,            ValueType::Boolean, 1.0f,   "" },
    { "H19",  "Yield Total",       SemanticField::YieldTotal,            ValueType::Float,   0.01f,  "kWh" },
    { "H20",  "Yield Today",       SemanticField::YieldToday,            ValueType::Float,   0.01f,  "kWh" },
    { "H21",  "Max Power Today",   SemanticField::MaximumPowerToday,     ValueType::Integer, 1.0f,   "W" },
    { "H22",  "Yield Yesterday",   SemanticField::YieldYesterday,        ValueType::Float,   0.01f,  "kWh" },
    { "H23",  "Max Power Yesterday", SemanticField::MaximumPowerYesterday, ValueType::Integer, 1.0f, "W" },
    { "HSDS", "Day Sequence",      SemanticField::DaySequence,           ValueType::Integer, 1.0f,   "" },
    { "OR",   "Off Reason",        SemanticField::OffReason,             ValueType::Hex,     1.0f,   "" },
};

constexpr size_t CatalogSize = sizeof(Catalog) / sizeof(Catalog[0]);

} // namespace

/******************************************************************************
 * Feld anhand des VE.Direct-Labels suchen.
 ******************************************************************************/
const FieldDefinition* FieldCatalog::find(const char* label)
{
    for (size_t i = 0; i < CatalogSize; ++i)
    {
        if (std::strcmp(Catalog[i].label, label) == 0)
        {
            return &Catalog[i];
        }
    }

    return nullptr;
}