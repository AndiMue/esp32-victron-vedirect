/******************************************************************************
 * @file DeviceCatalog.hpp
 *
 * Zuordnung von Product-ID zu Geräteinformationen.
 ******************************************************************************/

#pragma once

#include "DeviceInfo.hpp"

/******************************************************************************
 * Nachschlagetabelle für bekannte Victron-Geräte.
 ******************************************************************************/
class DeviceCatalog
{
public:

    static const DeviceInfo* find(const char* productId);
};