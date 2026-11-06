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

/// Anlage, für die die Firmware arbeitet (bestimmt die Relais-Voreinstellung,
/// siehe LoadControllerConfig::camper() / ::balcony()).
enum class Installation : uint8_t
{
    Camper,    ///< R1 Wechselrichter (Taster/Touch), R2 Licht, R3 Kühlschrank
    Balcony    ///< R1 DC-DC-Wandler (MQTT), R2/R3 deaktiviert
};

struct AppConfig
{
    /**************************************************************************
     * Anlage. Wirkt beim Start der systemTask (später per WebUI/NVS).
     **************************************************************************/
    std::atomic<Installation> installation{Installation::Camper};

    /**************************************************************************
     * Geräte-Timeout in Millisekunden.
     *
     * Kommt so lange kein gültiger Block mit PID, gilt das Gerät eines
     * Kanals als verloren ("Ausfall"). Victron sendet jede Sekunde einen
     * Block; 5000 ms entsprechen also 5 verpassten Blöcken.
     **************************************************************************/
    std::atomic<uint32_t> deviceTimeoutMs{DeviceDetector::DefaultTimeoutMs};

    /**************************************************************************
     * Erforderliche Geräte (Projektinhaber: Shunt Pflicht, MPPT optional).
     * Bestimmt die Schwere von "fehlt": erforderlich -> kritisch bzw.
     * Warnung, optional -> nur Info.
     **************************************************************************/
    std::atomic<bool> requireBatteryMonitor{true};
    std::atomic<bool> requirePvCharger{false};

    /**************************************************************************
     * Verbindungsüberwachung (LinkMonitor), siehe LinkMonitor.hpp.
     * "Instabil" ab linkOutageThreshold Ausfällen ODER ab
     * linkErrorThreshold Prüfsummenfehlern + verworfenen Blöcken.
     * Erholung nach linkRecoveryMs ohne Fehler (0 = nie).
     **************************************************************************/
    std::atomic<uint32_t> linkErrorThreshold{3};
    std::atomic<uint32_t> linkOutageThreshold{1};
    std::atomic<uint32_t> linkRecoveryMs{120000};
};

/// Die eine globale Instanz (definiert in main.cpp).
extern AppConfig g_config;
