/******************************************************************************
 * @file WebUi.cpp
 *
 * Beschreibung siehe WebUi.hpp.
 ******************************************************************************/

#include "WebUi.hpp"

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
