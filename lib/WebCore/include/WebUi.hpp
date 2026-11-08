/******************************************************************************
 * @file WebUi.hpp
 *
 * Webserver der WebUI-Basis (lib/WebCore), Stufe W1.
 *
 * Dateien der Oberfläche liegen im Dateisystem (LittleFS, Projektordner
 * data/). Alles ist lokal, ohne Internet/CDN (im AP gibt es kein Internet).
 *
 * Sicherheit:
 *   - Jede Seite und jede API erfordert Login (HTTP Basic Auth,
 *     Benutzer "admin", Passwort = Gerätepasswort).
 *   - Nach MaxFailedLogins Fehlversuchen ist der Login LockoutMs gesperrt.
 *   - Passwörter werden NIE an den Browser zurückgegeben, nur
 *     "gesetzt / nicht gesetzt".
 *   - Hinweis: Basic Auth überträgt das Passwort nur Base64-kodiert. Schutz
 *     bietet die WLAN-Verschlüsselung (WPA2), HTTPS ist auf dem ESP zu
 *     aufwendig.
 *
 * API (W1):
 *   GET  /api/status        Gerät, Firmware, Laufzeit, WLAN-Zustand
 *   GET  /api/wifi          WLAN-Konfiguration (ohne Passwort)
 *   POST /api/wifi          ssid, password (leer = behalten), apAlways
 *   GET  /api/wifi/scan     WLAN-Suche (async; ggf. "scanning")
 *   POST /api/password      current, new  (Gerätepasswort ändern)
 *   POST /api/wifi/reset    WLAN-Zugangsdaten löschen
 *   POST /api/restart       Neustart
 *
 * Nur aus EINER Task verwenden (Web-Task): begin(), dann loop().
 ******************************************************************************/

#pragma once

#include <Arduino.h>
#include <functional>

class WifiManager;

class WebUi
{
public:

    static constexpr uint8_t  MaxFailedLogins = 5;
    static constexpr uint32_t LockoutMs       = 60000;

    /// Zusatzfelder für /api/status (projektspezifisch). Muss gültiges JSON
    /// für ein Objekt-Inneres liefern, z. B. "\"soc\":95.0" (ohne Klammern).
    using StatusExtension = std::function<String()>;

    void begin(WifiManager& wifi, const char* deviceName, const char* firmwareVersion);
    void loop(uint32_t nowMs);

    void setStatusExtension(StatusExtension extension) { m_extension = extension; }

    /// Hilfsfunktion: Text als JSON-String (mit Anführungszeichen, escaped).
    static String jsonString(const String& text);

private:

    bool authorize();
    void serveFile(const String& path);
    void handleNotFound();
    void sendJson(int code, const String& body);

    void handleStatus();
    void handleWifiGet();
    void handleWifiPost();
    void handleWifiScan();
    void handlePassword();
    void handleWifiReset();
    void handleRestart();

    WifiManager*    m_wifi = nullptr;
    String          m_deviceName;
    String          m_firmwareVersion;
    StatusExtension m_extension;

    uint8_t  m_failedLogins  = 0;
    uint32_t m_lockUntilMs   = 0;
    bool     m_locked        = false;
    bool     m_restartPending = false;
    uint32_t m_restartAtMs   = 0;
};
