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
 * API (W2, Updates; Datei als multipart/form-data, Feld "file"):
 *   POST /api/update/firmware   firmware.bin              -> Neustart
 *   POST /api/update/webui      webui_*.tar               -> kein Neustart
 *   POST /api/update/package    package_*.tar             -> Neustart
 *   Ergebnis und Versionen auch in /api/status ("update", "firmware", "webui").
 *
 * API (W3, MQTT – nur wenn setMqtt() aufgerufen wurde):
 *   GET  /api/mqtt          Konfiguration (ohne Passwort) + Zustand
 *   POST /api/mqtt          enabled, host, port, user, password (leer =
 *                           behalten), baseTopic, intervalS, clientId
 *   POST /api/mqtt/test     sofort neu verbinden
 *   POST /api/mqtt/reset    MQTT-Einstellungen löschen
 *   Zustand auch in /api/status ("mqtt").
 *
 * Projekt-Endpunkte (W4a): Das Projekt meldet eigene APIs mit addApi() an
 * (VOR begin()). WebCore bleibt dadurch projektneutral (Basis auch für
 * andere Projekte und die spätere ESP8266-Variante). Login und Sperre
 * prüft WebUi selbst, der Handler liefert nur Statuscode und JSON.
 *
 * Nur aus EINER Task verwenden (Web-Task): begin(), dann loop().
 ******************************************************************************/

#pragma once

#include <Arduino.h>
#include <atomic>
#include <functional>
#include <vector>

#include "UpdateManager.hpp"

class WifiManager;
class MqttManager;

/// Lesezugriff auf die Parameter einer Anfrage (Formular- oder URL-
/// Parameter). Gilt nur während des Handler-Aufrufs.
class ApiRequest
{
public:
    String arg(const char* name) const;
    bool   hasArg(const char* name) const;
};

class WebUi
{
public:

    static constexpr uint8_t  MaxFailedLogins = 5;
    static constexpr uint32_t LockoutMs       = 60000;

    /// Zusatzfelder für /api/status (projektspezifisch). Muss gültiges JSON
    /// für ein Objekt-Inneres liefern, z. B. "\"soc\":95.0" (ohne Klammern).
    using StatusExtension = std::function<String()>;

    /// Projekt-Endpunkt: füllt responseJson (vollständiges JSON) und gibt den
    /// HTTP-Statuscode zurück (z. B. 200, 400). Läuft in der Web-Task.
    using ApiHandler = std::function<int(const ApiRequest& request, String& responseJson)>;

    enum class Method : uint8_t { Get, Post };

    void begin(WifiManager& wifi, const char* deviceName, const char* firmwareVersion);
    void loop(uint32_t nowMs);

    void setStatusExtension(StatusExtension extension) { m_extension = extension; }

    /// Projekt-Endpunkt anmelden (vor begin()). path z. B. "/api/live".
    void addApi(const char* path, Method method, ApiHandler handler);

    /// Neustart anfordern (z. B. aus einem Projekt-Endpunkt). Wird in loop()
    /// nach delayMs ausgeführt, damit die Antwort noch ausgeliefert wird.
    /// Nur aus der Web-Task aufrufen.
    void requestRestart(uint32_t delayMs = 1000)
    {
        m_restartPending = true;
        m_restartAtMs    = millis() + delayMs;
    }

    /// MQTT-Verwaltung einbinden (optional, vor begin() aufrufen).
    void setMqtt(MqttManager* mqtt) { m_mqtt = mqtt; }

    /// Läuft gerade ein Update? (aus anderen Tasks lesbar, z. B. Display)
    bool updateRunning() const { return m_updateRunning.load(); }

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
    void handleUpload(UpdateManager::Kind kind);
    void finishUpload();
    String webUiVersion() const;

    void handleMqttGet();
    void handleMqttPost();
    void handleMqttTest();
    void handleMqttReset();
    String mqttStatusJson() const;

    MqttManager*      m_mqtt = nullptr;

    struct ProjectApi
    {
        String     path;
        Method     method;
        ApiHandler handler;
    };
    std::vector<ProjectApi> m_projectApis;

    UpdateManager     m_update;
    bool              m_uploadAuthorized = false;
    std::atomic<bool> m_updateRunning{false};

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
