/******************************************************************************
 * @file AppConfig.hpp
 *
 * Zentrale, zur Laufzeit änderbare Einstellungen der Anwendung.
 *
 * Hier landen alle Werte, die später über WebUI oder MQTT eingestellt und
 * im Flash (NVS/Preferences) gespeichert werden sollen. Bis dahin gelten
 * die Standardwerte unten.
 *
 * Threadsicherheit: Die Werte sind std::atomic. Eine spätere WebUI-Task
 * kann sie also jederzeit setzen, während die UART-Tasks sie lesen.
 *
 * Beispiel (später, z. B. in der WebUI):
 *   g_config.deviceTimeoutMs.store(8000);
 ******************************************************************************/

#pragma once

#include <atomic>
#include <cstdint>

#include "DeviceDetector.hpp"

struct AppConfig
{
    /**************************************************************************
     * Geräte-Timeout in Millisekunden.
     *
     * Kommt so lange kein gültiger Block mit PID, gilt das Gerät eines
     * Kanals als verloren ("Ausfall"). Victron sendet jede Sekunde einen
     * Block; 5000 ms entsprechen also 5 verpassten Blöcken.
     **************************************************************************/
    std::atomic<uint32_t> deviceTimeoutMs{DeviceDetector::DefaultTimeoutMs};
};

/// Die eine globale Instanz (definiert in main.cpp).
extern AppConfig g_config;
