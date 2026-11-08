/******************************************************************************
 * @file WifiManager.cpp
 *
 * Verhalten siehe WifiManager.hpp.
 ******************************************************************************/

#include "WifiManager.hpp"

#include "SettingsStore.hpp"

#include <cstring>

#if defined(ESP32)
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_system.h>   // esp_random()
#else
#error "WifiManager: ESP8266-Variante folgt in einem eigenen Schritt (siehe Claude.md)"
#endif

namespace
{

const char* const Ns = "net";   ///< Namensraum im Einstellungsspeicher

// Schlüssel (max. 15 Zeichen, NVS-Grenze)
const char* const KeySsid      = "ssid";
const char* const KeyPassword  = "pass";
const char* const KeyDevicePw  = "devpw";
const char* const KeyPwChanged = "pwchg";
const char* const KeyApAlways  = "apalways";
const char* const KeyHostname  = "host";

const IPAddress ApIp(192, 168, 4, 1);
const IPAddress ApMask(255, 255, 255, 0);

DNSServer g_dns;           ///< Captive Portal (nur bei aktivem AP)
bool      g_dnsRunning = false;

/// Grund des letzten Verbindungsabbruchs (wifi_err_reason_t), 0 = keiner.
/// Wird aus der WLAN-Ereignis-Task geschrieben (ein Byte -> atomar).
volatile uint8_t g_lastReason = 0;

/// Grund 8 = ASSOC_LEAVE: Wir haben selbst getrennt -> kein Fehler.
constexpr uint8_t ReasonAssocLeave = 8;

void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info)
{
    if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
    {
        const uint8_t reason = info.wifi_sta_disconnected.reason;
        if (reason != ReasonAssocLeave)
        {
            g_lastReason = reason;
        }
    }
    else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP)
    {
        g_lastReason = 0;   // verbunden: kein Fehler
    }
}

void copyString(char* target, size_t size, const String& value)
{
    std::strncpy(target, value.c_str(), size - 1);
    target[size - 1] = '\0';
}

} // namespace

/******************************************************************************
 * Hilfsfunktionen
 ******************************************************************************/
String WifiManager::generatePassword(size_t length)
{
    // Ohne leicht verwechselbare Zeichen (0/O, 1/l/I), damit man es vom
    // Display sicher abtippen kann.
    static const char Chars[] = "abcdefghjkmnpqrstuvwxyzABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    const size_t count = sizeof(Chars) - 1;

    String pw;
    for (size_t i = 0; i < length; ++i)
    {
        pw += Chars[esp_random() % count];   // Hardware-Zufallszahl
    }
    return pw;
}

const char* WifiManager::reasonText(uint8_t reason)
{
    // Codes aus ESP-IDF wifi_err_reason_t
    switch (reason)
    {
        case 0:   return "";
        case 2:                                          // AUTH_EXPIRE
        case 15:                                         // 4WAY_HANDSHAKE_TIMEOUT
        case 202:                                        // AUTH_FAIL
        case 204:                                        // HANDSHAKE_TIMEOUT
            return "Anmeldung abgelehnt - WLAN-Passwort falsch?";
        case 201:                                        // NO_AP_FOUND
            return "WLAN nicht gefunden - Name prüfen. Der ESP32 kann nur 2,4 GHz!";
        case 203:                                        // ASSOC_FAIL
            return "Router hat die Verbindung abgelehnt";
        case 200:                                        // BEACON_TIMEOUT
            return "Verbindung zum Router verloren (Signal zu schwach?)";
        default:
            return "Verbindung fehlgeschlagen";
    }
}

bool WifiManager::factoryResetNetwork()
{
    return SettingsStore::clear(Ns);
}

void WifiManager::loadConfig(const char* defaultHostname)
{
    m_ssid            = SettingsStore::getString(Ns, KeySsid);
    m_password        = SettingsStore::getString(Ns, KeyPassword);
    m_apAlways        = SettingsStore::getBool(Ns, KeyApAlways, false);
    m_hostname        = SettingsStore::getString(Ns, KeyHostname, defaultHostname);
    m_devicePassword  = SettingsStore::getString(Ns, KeyDevicePw);
    m_defaultPassword = !SettingsStore::getBool(Ns, KeyPwChanged, false);

    if (m_devicePassword.length() < MinPasswordLength)
    {
        // Erststart oder nach Reset: neues Gerätepasswort erzeugen
        // 8 Zeichen = WPA2-Minimum; aus 55 Zeichen -> ca. 8·10^13 Möglichkeiten
        m_devicePassword = generatePassword(8);
        SettingsStore::putString(Ns, KeyDevicePw, m_devicePassword);
        SettingsStore::putBool(Ns, KeyPwChanged, false);
        m_defaultPassword = true;
    }
}

/******************************************************************************
 * Start
 ******************************************************************************/
void WifiManager::begin(const char* defaultHostname, const char* apPrefix)
{
    loadConfig(defaultHostname);

    // AP-Name eindeutig machen: Präfix + letzte 2 Bytes der MAC
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "-%02X%02X", mac[4], mac[5]);
    m_apSsid = String(apPrefix) + suffix;

    WiFi.persistent(false);          // Zugangsdaten NICHT im Treiber-Flash ablegen
    WiFi.setAutoReconnect(true);
    WiFi.onEvent(onWifiEvent);       // Grund von Verbindungsfehlern erfassen
    WiFi.setHostname(m_hostname.c_str());

    const uint32_t now = millis();
    if (m_ssid.length() == 0)
    {
        WiFi.mode(WIFI_AP);
        startAp();
        m_state = NetState::AccessPoint;
        m_stateSinceMs = now;
    }
    else
    {
        startStation(now);
        if (m_apAlways)
        {
            startAp();
        }
    }

    if (MDNS.begin(m_hostname.c_str()))
    {
        MDNS.addService("http", "tcp", 80);
    }

    updateStatus();
}

void WifiManager::startStation(uint32_t nowMs)
{
    WiFi.mode(m_apActive ? WIFI_AP_STA : WIFI_STA);
    g_lastReason = 0;
    ++m_attempt;
    WiFi.begin(m_ssid.c_str(), m_password.c_str());
    m_state        = NetState::Connecting;
    m_stateSinceMs = nowMs;
    m_lastRetryMs  = nowMs;
    m_lost         = false;
}

void WifiManager::startAp()
{
    if (m_apActive)
    {
        return;
    }
    WiFi.mode(m_ssid.length() > 0 ? WIFI_AP_STA : WIFI_AP);
    WiFi.softAPConfig(ApIp, ApIp, ApMask);
    WiFi.softAP(m_apSsid.c_str(), m_devicePassword.c_str());   // WPA2

    g_dns.setErrorReplyCode(DNSReplyCode::NoError);
    g_dnsRunning = g_dns.start(53, "*", ApIp);   // Captive Portal
    m_apActive = true;
}

void WifiManager::stopAp()
{
    if (!m_apActive)
    {
        return;
    }
    if (g_dnsRunning)
    {
        g_dns.stop();
        g_dnsRunning = false;
    }
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    m_apActive = false;
}

/******************************************************************************
 * Zustandsmaschine
 ******************************************************************************/
void WifiManager::loop(uint32_t nowMs)
{
    if (g_dnsRunning)
    {
        g_dns.processNextRequest();
    }

    // Neue Konfiguration aus der WebUI anwenden
    if (m_reconfigure)
    {
        m_reconfigure = false;
        WiFi.disconnect(false);
        if (m_ssid.length() == 0)
        {
            startAp();
            m_state = NetState::AccessPoint;
            m_stateSinceMs = nowMs;
        }
        else
        {
            if (m_apAlways) startAp();
            startStation(nowMs);
        }
    }

    const bool connected = (m_ssid.length() > 0) && (WiFi.status() == WL_CONNECTED);

    switch (m_state)
    {
        case NetState::AccessPoint:
            // nichts zu tun, warten auf Konfiguration
            break;

        case NetState::Connecting:
            if (connected)
            {
                m_state = NetState::Connected;
                m_stateSinceMs = nowMs;
                ++m_connectCount;
            }
            else if (static_cast<uint32_t>(nowMs - m_stateSinceMs) > ConnectTimeoutMs)
            {
                // WLAN nicht erreichbar -> AP als Rückfall
                startAp();
                m_state = NetState::Fallback;
                m_stateSinceMs = nowMs;
                m_lastRetryMs  = nowMs;
            }
            break;

        case NetState::Connected:
            if (connected)
            {
                m_lost = false;
                // AP nur abschalten, wenn nicht dauerhaft gewünscht und
                // niemand mehr daran angemeldet ist
                if (m_apActive && !m_apAlways && WiFi.softAPgetStationNum() == 0)
                {
                    stopAp();
                }
            }
            else
            {
                if (!m_lost)
                {
                    m_lost = true;
                    m_lostSinceMs = nowMs;
                }
                else if (static_cast<uint32_t>(nowMs - m_lostSinceMs) > LostToFallbackMs)
                {
                    startAp();
                    m_state = NetState::Fallback;
                    m_stateSinceMs = nowMs;
                    m_lastRetryMs  = nowMs;
                }
            }
            break;

        case NetState::Fallback:
            if (connected)
            {
                m_state = NetState::Connected;
                m_stateSinceMs = nowMs;
                ++m_connectCount;
            }
            else if (static_cast<uint32_t>(nowMs - m_lastRetryMs) > RetryIntervalMs)
            {
                // Erneuter Versuch, AP bleibt dabei an
                m_lastRetryMs = nowMs;
                WiFi.disconnect(false);
                g_lastReason = 0;
                ++m_attempt;
                WiFi.begin(m_ssid.c_str(), m_password.c_str());
            }
            break;
    }

    updateStatus();
}

void WifiManager::updateStatus()
{
    NetStatus s;
    s.state           = m_state;
    s.apActive        = m_apActive;
    s.staConnected    = (m_ssid.length() > 0) && (WiFi.status() == WL_CONNECTED);
    copyString(s.apSsid,   sizeof(s.apSsid),   m_apSsid);
    copyString(s.staSsid,  sizeof(s.staSsid),  m_ssid);
    copyString(s.hostname, sizeof(s.hostname), m_hostname);
    s.staIp           = s.staConnected ? static_cast<uint32_t>(WiFi.localIP()) : 0;
    s.apIp            = m_apActive ? static_cast<uint32_t>(WiFi.softAPIP()) : 0;
    s.rssi            = s.staConnected ? static_cast<int8_t>(WiFi.RSSI()) : 0;
    s.apClients       = m_apActive ? WiFi.softAPgetStationNum() : 0;
    s.defaultPassword = m_defaultPassword;
    if (m_defaultPassword)
    {
        copyString(s.displayPassword, sizeof(s.displayPassword), m_devicePassword);
    }
    s.connectCount    = m_connectCount;
    s.attempt         = m_attempt;
    s.lastFailReason  = s.staConnected ? 0 : g_lastReason;
    m_status = s;
}

/******************************************************************************
 * Konfiguration
 ******************************************************************************/
bool WifiManager::setWifi(const String& ssid, const String& password, bool apAlways)
{
    if (ssid.length() == 0 || ssid.length() > 32)
    {
        return false;
    }
    if (password.length() > 0 && password.length() < MinPasswordLength)
    {
        return false;
    }

    m_ssid = ssid;
    if (password.length() > 0)
    {
        m_password = password;
    }
    m_apAlways = apAlways;

    const bool ok = SettingsStore::putString(Ns, KeySsid, m_ssid) &&
                    SettingsStore::putString(Ns, KeyPassword, m_password) &&
                    SettingsStore::putBool(Ns, KeyApAlways, m_apAlways);
    m_reconfigure = true;
    return ok;
}

void WifiManager::prepareScan()
{
    if ((WiFi.getMode() & WIFI_MODE_STA) == 0)
    {
        WiFi.enableSTA(true);   // reiner AP-Modus: STA für die Suche einschalten
    }
    if (m_state == NetState::Connecting || m_state == NetState::Fallback)
    {
        WiFi.disconnect(false); // laufenden Verbindungsversuch anhalten
        if (m_state == NetState::Connecting)
        {
            // Nach der Suche über den Fallback-Pfad erneut versuchen
            m_state = NetState::Fallback;
            if (!m_apActive) startAp();
        }
        m_lastRetryMs = millis();   // Wiederholung erst nach RetryIntervalMs
    }
}

bool WifiManager::resetWifi()
{
    m_ssid = "";
    m_password = "";
    m_apAlways = false;
    SettingsStore::remove(Ns, KeySsid);
    SettingsStore::remove(Ns, KeyPassword);
    SettingsStore::remove(Ns, KeyApAlways);
    m_reconfigure = true;
    return true;
}

bool WifiManager::changeDevicePassword(const String& newPassword)
{
    if (newPassword.length() < MinPasswordLength || newPassword.length() > 63)
    {
        return false;
    }
    m_devicePassword  = newPassword;
    m_defaultPassword = false;
    const bool ok = SettingsStore::putString(Ns, KeyDevicePw, m_devicePassword) &&
                    SettingsStore::putBool(Ns, KeyPwChanged, true);

    // AP mit neuem Passwort neu starten
    if (m_apActive)
    {
        stopAp();
        startAp();
    }
    return ok;
}
