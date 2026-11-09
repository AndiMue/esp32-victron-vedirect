/******************************************************************************
 * @file WebUi.cpp
 *
 * Beschreibung siehe WebUi.hpp.
 ******************************************************************************/

#include "WebUi.hpp"

#include "MqttManager.hpp"
#include "WifiManager.hpp"

#if defined(ESP32)
#include <LittleFS.h>
#include <WebServer.h>
#include <WiFi.h>
#else
#error "WebUi: ESP8266-Variante folgt in einem eigenen Schritt (siehe Claude.md)"
#endif

namespace
{

WebServer g_server(80);

const char* const AdminUser = "admin";
const char* const Realm     = "VE-Direct";

String ipToString(uint32_t ip)
{
    return IPAddress(ip).toString();
}

const char* netStateText(NetState state)
{
    switch (state)
    {
        case NetState::AccessPoint: return "access_point";
        case NetState::Connecting:  return "connecting";
        case NetState::Connected:   return "connected";
        case NetState::Fallback:    return "fallback";
    }
    return "unknown";
}

String contentType(const String& path)
{
    if (path.endsWith(".html")) return "text/html; charset=utf-8";
    if (path.endsWith(".css"))  return "text/css; charset=utf-8";
    if (path.endsWith(".js"))   return "application/javascript; charset=utf-8";
    if (path.endsWith(".svg"))  return "image/svg+xml";
    if (path.endsWith(".json")) return "application/json; charset=utf-8";
    if (path.endsWith(".ico"))  return "image/x-icon";
    return "application/octet-stream";
}

} // namespace

/******************************************************************************
 * JSON-Hilfe
 ******************************************************************************/
String WebUi::jsonString(const String& text)
{
    String out = "\"";
    for (size_t i = 0; i < text.length(); ++i)
    {
        const char c = text[i];
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<uint8_t>(c) < 0x20)
                {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                    out += buf;
                }
                else
                {
                    out += c;
                }
        }
    }
    out += "\"";
    return out;
}

void WebUi::sendJson(int code, const String& body)
{
    g_server.sendHeader("Cache-Control", "no-store");
    g_server.send(code, "application/json; charset=utf-8", body);
}

/******************************************************************************
 * Login (Basic Auth) mit Sperre nach Fehlversuchen
 ******************************************************************************/
bool WebUi::authorize()
{
    const uint32_t now = millis();

    if (m_locked)
    {
        if (static_cast<uint32_t>(now - m_lockUntilMs) < 0x80000000u)
        {
            m_locked = false;          // Sperrzeit abgelaufen
            m_failedLogins = 0;
        }
        else
        {
            g_server.send(429, "text/plain; charset=utf-8",
                          "Zu viele Fehlversuche. Bitte 1 Minute warten.");
            return false;
        }
    }

    if (g_server.authenticate(AdminUser, m_wifi->devicePassword().c_str()))
    {
        m_failedLogins = 0;
        return true;
    }

    // Nur echte Fehlversuche zählen (Browser fragt zuerst ohne Daten an)
    if (g_server.hasHeader("Authorization"))
    {
        if (++m_failedLogins >= MaxFailedLogins)
        {
            m_locked = true;
            m_lockUntilMs = now + LockoutMs;
        }
    }

    g_server.requestAuthentication(BASIC_AUTH, Realm, "Anmeldung erforderlich");
    return false;
}

/******************************************************************************
 * Dateien
 ******************************************************************************/
void WebUi::serveFile(const String& path)
{
    String file = path;
    if (file.endsWith("/"))
    {
        file += "index.html";
    }

    if (!LittleFS.exists(file))
    {
        g_server.send(404, "text/plain; charset=utf-8",
                      "Datei nicht gefunden. Wurde das Dateisystem (WebUI) hochgeladen?");
        return;
    }

    File f = LittleFS.open(file, "r");
    // Nicht zwischenspeichern: Nach einem WebUI-Update soll der Browser nie
    // mit einer alten app.js arbeiten (Finding 08.11.2026)
    g_server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    g_server.streamFile(f, contentType(file));
    f.close();
}

void WebUi::handleNotFound()
{
    // Captive Portal: Im AP fragen Handys fremde Adressen an (z. B.
    // connectivitycheck). Alles, was nicht an uns gerichtet ist, auf die
    // Startseite umleiten.
    const NetStatus st = m_wifi->status();
    const String host = g_server.hostHeader();
    const String apIp = ipToString(st.apIp);

    if (st.apActive && host.length() > 0 && host != apIp &&
        host != st.hostname && host != String(st.hostname) + ".local")
    {
        g_server.sendHeader("Location", String("http://") + apIp + "/", true);
        g_server.send(302, "text/plain", "");
        return;
    }

    if (!authorize())
    {
        return;
    }
    serveFile(g_server.uri());
}

/******************************************************************************
 * API
 ******************************************************************************/
void WebUi::handleStatus()
{
    if (!authorize()) return;

    const NetStatus st = m_wifi->status();
    String j = "{";
    j += "\"device\":"   + jsonString(m_deviceName);
    j += ",\"firmware\":" + jsonString(m_firmwareVersion);
    j += ",\"uptimeS\":" + String(millis() / 1000);
    j += ",\"freeHeap\":" + String(ESP.getFreeHeap());
    j += ",\"wifi\":{";
    j += "\"state\":"        + jsonString(netStateText(st.state));
    j += ",\"apActive\":"    + String(st.apActive ? "true" : "false");
    j += ",\"apSsid\":"      + jsonString(st.apSsid);
    j += ",\"apIp\":"        + jsonString(st.apIp ? ipToString(st.apIp) : "");
    j += ",\"apClients\":"   + String(st.apClients);
    j += ",\"connected\":"   + String(st.staConnected ? "true" : "false");
    j += ",\"ssid\":"        + jsonString(st.staSsid);
    j += ",\"ip\":"          + jsonString(st.staIp ? ipToString(st.staIp) : "");
    j += ",\"rssi\":"        + String(st.rssi);
    j += ",\"hostname\":"    + jsonString(st.hostname);
    j += ",\"attempt\":"     + String(st.attempt);
    j += ",\"lastError\":"   + jsonString(WifiManager::reasonText(st.lastFailReason));
    j += ",\"lastErrorCode\":" + String(st.lastFailReason);
    j += "}";
    j += ",\"defaultPassword\":" + String(st.defaultPassword ? "true" : "false");

    // ---- W2: Versionen und Update-Zustand ----
    const char* updState = "idle";
    switch (m_update.state())
    {
        case UpdateManager::State::Idle:    updState = "idle";    break;
        case UpdateManager::State::Running: updState = "running"; break;
        case UpdateManager::State::Success: updState = "ok";      break;
        case UpdateManager::State::Failed:  updState = "error";   break;
    }
    j += ",\"webui\":" + jsonString(webUiVersion());
    j += ",\"update\":{";
    j += "\"state\":"       + jsonString(updState);
    j += ",\"message\":"    + jsonString(m_update.message());
    j += ",\"rolledBack\":" + String(UpdateManager::rolledBack() ? "true" : "false");
    j += ",\"trial\":"      + String(UpdateManager::onTrial() ? "true" : "false");
    j += "}";
    if (m_mqtt)
    {
        j += ",\"mqtt\":" + mqttStatusJson();
    }
    if (m_extension)
    {
        const String extra = m_extension();
        if (extra.length() > 0)
        {
            j += "," + extra;
        }
    }
    j += "}";
    sendJson(200, j);
}

void WebUi::handleWifiGet()
{
    if (!authorize()) return;

    String j = "{";
    j += "\"ssid\":"         + jsonString(m_wifi->ssid());
    j += ",\"passwordSet\":" + String(m_wifi->passwordSet() ? "true" : "false");
    j += ",\"apAlways\":"    + String(m_wifi->apAlways() ? "true" : "false");
    j += ",\"hostname\":"    + jsonString(m_wifi->hostname());
    j += "}";
    sendJson(200, j);
}

void WebUi::handleWifiPost()
{
    if (!authorize()) return;

    const String ssid     = g_server.arg("ssid");
    const String password = g_server.arg("password");
    const bool   apAlways = g_server.arg("apAlways") == "1" || g_server.arg("apAlways") == "true";

    if (!m_wifi->setWifi(ssid, password, apAlways))
    {
        sendJson(400, "{\"ok\":false,\"error\":\"SSID fehlt/zu lang oder Passwort kürzer als 8 Zeichen\"}");
        return;
    }
    sendJson(200, "{\"ok\":true}");
}

void WebUi::handleWifiScan()
{
    if (!authorize()) return;

    const int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING)
    {
        sendJson(200, "{\"scanning\":true,\"networks\":[]}");
        return;
    }
    if (result < 0)
    {
        // STA einschalten bzw. laufenden Verbindungsversuch anhalten,
        // sonst schlägt die Suche fehl (z. B. im reinen AP-Modus)
        m_wifi->prepareScan();
        if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED)   // asynchron starten
        {
            sendJson(503, "{\"ok\":false,\"error\":\"WLAN-Suche konnte nicht gestartet werden\"}");
            return;
        }
        sendJson(200, "{\"scanning\":true,\"networks\":[]}");
        return;
    }

    String j = "{\"scanning\":false,\"networks\":[";
    for (int16_t i = 0; i < result; ++i)
    {
        if (i > 0) j += ",";
        j += "{\"ssid\":" + jsonString(WiFi.SSID(i));
        j += ",\"rssi\":" + String(WiFi.RSSI(i));
        j += ",\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false");
        j += "}";
    }
    j += "]}";
    WiFi.scanDelete();   // nächster Aufruf startet neu
    sendJson(200, j);
}

void WebUi::handlePassword()
{
    if (!authorize()) return;

    const String current = g_server.arg("current");
    const String next    = g_server.arg("new");

    if (current != m_wifi->devicePassword())
    {
        sendJson(403, "{\"ok\":false,\"error\":\"Aktuelles Passwort falsch\"}");
        return;
    }
    if (!m_wifi->changeDevicePassword(next))
    {
        sendJson(400, "{\"ok\":false,\"error\":\"Neues Passwort: 8 bis 63 Zeichen\"}");
        return;
    }
    sendJson(200, "{\"ok\":true}");
}

void WebUi::handleWifiReset()
{
    if (!authorize()) return;
    m_wifi->resetWifi();
    sendJson(200, "{\"ok\":true}");
}

void WebUi::handleRestart()
{
    if (!authorize()) return;
    sendJson(200, "{\"ok\":true}");
    m_restartPending = true;
    m_restartAtMs = millis() + 1000;   // Antwort noch ausliefern
}

/******************************************************************************
 * Updates (W2)
 ******************************************************************************/
String WebUi::webUiVersion() const
{
    File f = LittleFS.open("/version.json", "r");
    if (!f)
    {
        return "unbekannt";
    }
    const String content = f.readString();
    f.close();
    // Inhalt: {"version": "26.11.08-1612"} -> Wert herausschneiden
    const int key = content.indexOf("\"version\"");
    const int q1  = content.indexOf('"', content.indexOf(':', key) + 1);
    const int q2  = content.indexOf('"', q1 + 1);
    return (key >= 0 && q1 > 0 && q2 > q1) ? content.substring(q1 + 1, q2) : String("unbekannt");
}

/// Wird für jedes Stück der hochgeladenen Datei aufgerufen.
void WebUi::handleUpload(UpdateManager::Kind kind)
{
    HTTPUpload& up = g_server.upload();

    switch (up.status)
    {
        case UPLOAD_FILE_START:
            // Login VOR dem ersten geschriebenen Byte prüfen
            m_uploadAuthorized = !m_locked &&
                g_server.authenticate(AdminUser, m_wifi->devicePassword().c_str());
            if (m_uploadAuthorized)
            {
                m_updateRunning = true;
                m_update.begin(kind);
            }
            break;

        case UPLOAD_FILE_WRITE:
            if (m_uploadAuthorized)
            {
                m_update.write(up.buf, up.currentSize);
            }
            break;

        case UPLOAD_FILE_END:
            if (m_uploadAuthorized)
            {
                m_update.end();
                m_updateRunning = false;
            }
            break;

        case UPLOAD_FILE_ABORTED:
            if (m_uploadAuthorized)
            {
                m_update.abort("Verbindung unterbrochen");
                m_updateRunning = false;
            }
            break;
    }
}

/// Nach dem vollständigen Upload: Antwort an den Browser.
void WebUi::finishUpload()
{
    if (!m_uploadAuthorized)
    {
        authorize();   // fordert Login an bzw. meldet Sperre
        return;
    }

    const bool ok = m_update.state() == UpdateManager::State::Success;
    String j = "{\"ok\":";
    j += ok ? "true" : "false";
    j += ",\"message\":" + jsonString(m_update.message());
    j += ",\"restart\":" + String(m_update.restartRequired() ? "true" : "false");
    j += "}";
    sendJson(ok ? 200 : 400, j);

    if (m_update.restartRequired())
    {
        m_restartPending = true;
        m_restartAtMs = millis() + 1500;   // Antwort noch ausliefern
    }
}

/******************************************************************************
 * MQTT (W3)
 ******************************************************************************/
String WebUi::mqttStatusJson() const
{
    const MqttStatus st = m_mqtt->status();
    const MqttConfig cfg = m_mqtt->config();
    const char* state = "disabled";
    switch (st.state)
    {
        case MqttState::Disabled:   state = "disabled";   break;
        case MqttState::NoNetwork:  state = "no_network"; break;
        case MqttState::Connecting: state = "connecting"; break;
        case MqttState::Connected:  state = "connected";  break;
        case MqttState::Error:      state = "error";      break;
    }
    String j = "{";
    j += "\"state\":"         + jsonString(state);
    j += ",\"error\":"        + jsonString(st.lastError);
    j += ",\"broker\":"       + jsonString(cfg.host.length() ? cfg.host + ":" + String(cfg.port) : String(""));
    j += ",\"baseTopic\":"    + jsonString(cfg.baseTopic);
    j += ",\"connectCount\":" + String(st.connectCount);
    j += ",\"publishCount\":" + String(st.publishCount);
    j += ",\"receiveCount\":" + String(st.receiveCount);
    j += ",\"lastPublishAgoS\":" + String(st.lastPublishMs ? (millis() - st.lastPublishMs) / 1000 : 0);
    j += "}";
    return j;
}

void WebUi::handleMqttGet()
{
    if (!authorize()) return;
    const MqttConfig c = m_mqtt->config();
    String j = "{";
    j += "\"enabled\":"      + String(c.enabled ? "true" : "false");
    j += ",\"host\":"        + jsonString(c.host);
    j += ",\"port\":"        + String(c.port);
    j += ",\"user\":"        + jsonString(c.user);
    j += ",\"passwordSet\":" + String(m_mqtt->passwordSet() ? "true" : "false");
    j += ",\"baseTopic\":"   + jsonString(c.baseTopic);
    j += ",\"intervalS\":"   + String(c.intervalS);
    j += ",\"clientId\":"    + jsonString(c.clientId);
    j += ",\"status\":"      + mqttStatusJson();
    j += "}";
    sendJson(200, j);
}

void WebUi::handleMqttPost()
{
    if (!authorize()) return;
    MqttConfig c;
    c.enabled   = g_server.arg("enabled") == "1" || g_server.arg("enabled") == "true";
    c.host      = g_server.arg("host");
    c.host.trim();
    c.port      = static_cast<uint16_t>(g_server.arg("port").toInt());
    c.user      = g_server.arg("user");
    c.password  = g_server.arg("password");   // leer = behalten
    c.baseTopic = g_server.arg("baseTopic");
    c.intervalS = static_cast<uint32_t>(g_server.arg("intervalS").toInt());
    c.clientId  = g_server.arg("clientId");
    c.clientId.trim();

    if (!m_mqtt->setConfig(c))
    {
        sendJson(400, "{\"ok\":false,\"error\":\"Ungültig: Broker fehlt, Port 0, Intervall nicht 1–3600 s oder Basis-Topic leer/mit # bzw. +\"}");
        return;
    }
    sendJson(200, "{\"ok\":true}");
}

void WebUi::handleMqttTest()
{
    if (!authorize()) return;
    m_mqtt->reconnectNow();
    sendJson(200, "{\"ok\":true}");
}

void WebUi::handleMqttReset()
{
    if (!authorize()) return;
    m_mqtt->resetConfig();
    sendJson(200, "{\"ok\":true}");
}

/******************************************************************************
 * Start / Schleife
 ******************************************************************************/
void WebUi::begin(WifiManager& wifi, const char* deviceName, const char* firmwareVersion)
{
    m_wifi            = &wifi;
    m_deviceName      = deviceName;
    m_firmwareVersion = firmwareVersion;

    g_server.on("/api/status",     HTTP_GET,  [this]() { handleStatus(); });
    g_server.on("/api/wifi",       HTTP_GET,  [this]() { handleWifiGet(); });
    g_server.on("/api/wifi",       HTTP_POST, [this]() { handleWifiPost(); });
    g_server.on("/api/wifi/scan",  HTTP_GET,  [this]() { handleWifiScan(); });
    g_server.on("/api/wifi/reset", HTTP_POST, [this]() { handleWifiReset(); });
    g_server.on("/api/password",   HTTP_POST, [this]() { handlePassword(); });
    g_server.on("/api/restart",    HTTP_POST, [this]() { handleRestart(); });

    // W3: MQTT
    if (m_mqtt)
    {
        g_server.on("/api/mqtt",       HTTP_GET,  [this]() { handleMqttGet(); });
        g_server.on("/api/mqtt",       HTTP_POST, [this]() { handleMqttPost(); });
        g_server.on("/api/mqtt/test",  HTTP_POST, [this]() { handleMqttTest(); });
        g_server.on("/api/mqtt/reset", HTTP_POST, [this]() { handleMqttReset(); });
    }

    // W2: Updates (erster Handler = Antwort, zweiter = Datenstücke)
    g_server.on("/api/update/firmware", HTTP_POST, [this]() { finishUpload(); },
                [this]() { handleUpload(UpdateManager::Kind::Firmware); });
    g_server.on("/api/update/webui",    HTTP_POST, [this]() { finishUpload(); },
                [this]() { handleUpload(UpdateManager::Kind::WebUi); });
    g_server.on("/api/update/package",  HTTP_POST, [this]() { finishUpload(); },
                [this]() { handleUpload(UpdateManager::Kind::Package); });
    g_server.onNotFound([this]() { handleNotFound(); });

    g_server.begin();
}

void WebUi::loop(uint32_t nowMs)
{
    g_server.handleClient();

    if (m_restartPending && static_cast<int32_t>(nowMs - m_restartAtMs) >= 0)
    {
        ESP.restart();
    }
}
