/******************************************************************************
 * @file MqttManager.cpp
 *
 * Beschreibung siehe MqttManager.hpp.
 ******************************************************************************/

#include "MqttManager.hpp"

#include "SettingsStore.hpp"

#include <PubSubClient.h>
#include <algorithm>
#include <cstring>

#if defined(ESP32)
#include <WiFi.h>
#else
#error "MqttManager: ESP8266-Variante folgt in einem eigenen Schritt (siehe Claude.md)"
#endif

namespace
{

const char* const Ns = "mqtt";

WiFiClient   g_net;
PubSubClient g_client(g_net);

/// Statt Lambda mit Zustand: PubSubClient braucht einen Funktionszeiger
/// bzw. std::function; wir leiten an die (einzige) Instanz weiter.
MqttManager* g_instance = nullptr;

const char* stateErrorText(int state)
{
    switch (state)
    {
        case -4: return "Zeitüberschreitung (Broker antwortet nicht)";
        case -3: return "Verbindung verloren";
        case -2: return "Broker nicht erreichbar (Adresse/Port prüfen)";
        case -1: return "getrennt";
        case 1:  return "Protokollversion abgelehnt";
        case 2:  return "Client-ID abgelehnt";
        case 3:  return "Broker nicht verfügbar";
        case 4:  return "Benutzer oder Passwort falsch";
        case 5:  return "nicht berechtigt";
        default: return "unbekannter Fehler";
    }
}

} // namespace

/******************************************************************************
 * Konfiguration (NVS)
 ******************************************************************************/
void MqttManager::loadConfig()
{
    MqttConfig c;
    c.enabled   = SettingsStore::getBool(Ns, "enabled", false);
    c.host      = SettingsStore::getString(Ns, "host");
    c.port      = static_cast<uint16_t>(SettingsStore::getUInt(Ns, "port", 1883));
    c.user      = SettingsStore::getString(Ns, "user");
    c.password  = SettingsStore::getString(Ns, "pass");
    c.baseTopic = SettingsStore::getString(Ns, "base", "vedirect");
    c.intervalS = SettingsStore::getUInt(Ns, "interval", 10);
    c.clientId  = SettingsStore::getString(Ns, "client");
    m_config = c;
}

void MqttManager::saveConfig()
{
    SettingsStore::putBool(Ns, "enabled", m_config.enabled);
    SettingsStore::putString(Ns, "host", m_config.host);
    SettingsStore::putUInt(Ns, "port", m_config.port);
    SettingsStore::putString(Ns, "user", m_config.user);
    SettingsStore::putString(Ns, "pass", m_config.password);
    SettingsStore::putString(Ns, "base", m_config.baseTopic);
    SettingsStore::putUInt(Ns, "interval", m_config.intervalS);
    SettingsStore::putString(Ns, "client", m_config.clientId);
}

MqttConfig MqttManager::config() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

bool MqttManager::passwordSet() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config.password.length() > 0;
}

bool MqttManager::setConfig(const MqttConfig& config)
{
    // Plausibilität
    String base = config.baseTopic;
    base.trim();
    while (base.endsWith("/")) base.remove(base.length() - 1);
    if (base.length() == 0 || base.indexOf('#') >= 0 || base.indexOf('+') >= 0)
    {
        return false;
    }
    if (config.enabled && config.host.length() == 0)
    {
        return false;
    }
    if (config.port == 0 || config.intervalS < 1 || config.intervalS > 3600)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    const String keepPassword = m_config.password;
    m_config = config;
    m_config.baseTopic = base;
    if (config.password.length() == 0)
    {
        m_config.password = keepPassword;   // leer = behalten
    }
    saveConfig();
    m_reconfigure = true;
    return true;
}

void MqttManager::resetConfig()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    SettingsStore::clear(Ns);
    m_config = MqttConfig();
    m_reconfigure = true;
}

void MqttManager::reconnectNow()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_reconnectNow = true;
}

MqttStatus MqttManager::status() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

uint32_t MqttManager::intervalMs() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config.intervalS * 1000;
}

String MqttManager::baseTopic() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config.baseTopic;
}

void MqttManager::setState(MqttState state, const char* error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.state = state;
    if (error != nullptr)
    {
        std::strncpy(m_status.lastError, error, sizeof(m_status.lastError) - 1);
        m_status.lastError[sizeof(m_status.lastError) - 1] = '\0';
    }
    else if (state == MqttState::Connected)
    {
        m_status.lastError[0] = '\0';
    }
}

/******************************************************************************
 * MQTT-Task
 ******************************************************************************/
void MqttManager::begin(const char* defaultClientId)
{
    m_defaultClientId = defaultClientId;
    g_instance = this;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        loadConfig();
        m_active = m_config;
    }
    g_client.setBufferSize(BufferSize);
    g_client.setSocketTimeout(3);   // Sekunden
    g_client.setKeepAlive(30);
    g_client.setCallback([](char* topic, uint8_t* payload, unsigned int length) {
        if (g_instance) g_instance->handleMessage(topic, payload, length);
    });
}

String MqttManager::fullTopic(const char* subtopic) const
{
    return m_active.baseTopic + "/" + subtopic;
}

bool MqttManager::connected() const
{
    return g_client.connected();
}

bool MqttManager::tryConnect(uint32_t nowMs)
{
    setState(MqttState::Connecting);

    const String clientId = m_active.clientId.length() ? m_active.clientId : m_defaultClientId;
    const String willTopic = fullTopic("status");

    g_client.setServer(m_active.host.c_str(), m_active.port);

    const bool withLogin = m_active.user.length() > 0;
    const bool ok = g_client.connect(clientId.c_str(),
                                     withLogin ? m_active.user.c_str() : nullptr,
                                     withLogin ? m_active.password.c_str() : nullptr,
                                     willTopic.c_str(), 0, true, "offline");
    if (!ok)
    {
        setState(MqttState::Error, stateErrorText(g_client.state()));
        m_nextAttemptMs = nowMs + m_retryMs;
        m_retryMs = std::min<uint32_t>(m_retryMs * 2, MaxRetryMs);
        return false;
    }

    m_retryMs = MinRetryMs;
    g_client.publish(willTopic.c_str(), "online", true);
    for (const String& sub : m_subscriptions)
    {
        g_client.subscribe(fullTopic(sub.c_str()).c_str());
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_status.connectCount;
    }
    setState(MqttState::Connected);
    return true;
}

void MqttManager::loop(uint32_t nowMs, bool networkUp)
{
    // Neue Konfiguration / "Verbindung testen" übernehmen
    bool reconfigure = false;
    bool reconnect   = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        reconfigure = m_reconfigure;
        reconnect   = m_reconnectNow;
        m_reconfigure = false;
        m_reconnectNow = false;
        if (reconfigure) m_active = m_config;
    }
    if (reconfigure || reconnect)
    {
        if (g_client.connected())
        {
            // "offline" sauber melden, bevor neu verbunden wird
            g_client.publish(fullTopic("status").c_str(), "offline", true);
            g_client.disconnect();
        }
        m_nextAttemptMs = nowMs;   // sofort neu versuchen
        m_retryMs = MinRetryMs;
    }

    if (!m_active.enabled || m_active.host.length() == 0)
    {
        if (g_client.connected()) g_client.disconnect();
        setState(MqttState::Disabled);
        return;
    }
    if (!networkUp)
    {
        if (g_client.connected()) g_client.disconnect();
        setState(MqttState::NoNetwork);
        return;
    }

    if (g_client.connected())
    {
        g_client.loop();
        return;
    }

    // Nicht verbunden: Versuch, wenn die Wartezeit abgelaufen ist
    if (static_cast<int32_t>(nowMs - m_nextAttemptMs) >= 0)
    {
        tryConnect(nowMs);
    }
    else
    {
        MqttStatus st = status();
        if (st.state == MqttState::Connected)
        {
            setState(MqttState::Error, "Verbindung verloren");
        }
    }
}

bool MqttManager::publish(const char* subtopic, const String& payload, bool retain)
{
    if (!g_client.connected())
    {
        return false;
    }
    const bool ok = g_client.publish(fullTopic(subtopic).c_str(), payload.c_str(), retain);
    if (ok)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_status.publishCount;
        m_status.lastPublishMs = millis();
    }
    return ok;
}

void MqttManager::subscribe(const char* subtopic)
{
    m_subscriptions.push_back(subtopic);
    if (g_client.connected())
    {
        g_client.subscribe(fullTopic(subtopic).c_str());
    }
}

void MqttManager::handleMessage(char* topic, uint8_t* payload, unsigned int length)
{
    String t(topic);
    const String prefix = m_active.baseTopic + "/";
    if (!t.startsWith(prefix))
    {
        return;
    }
    String p;
    p.reserve(length);
    for (unsigned int i = 0; i < length; ++i)
    {
        p += static_cast<char>(payload[i]);
    }
    const String sub = t.substring(prefix.length());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_status.receiveCount;
        std::strncpy(m_status.lastRxTopic, sub.c_str(), sizeof(m_status.lastRxTopic) - 1);
        m_status.lastRxTopic[sizeof(m_status.lastRxTopic) - 1] = '\0';
        std::strncpy(m_status.lastRxPayload, p.c_str(), sizeof(m_status.lastRxPayload) - 1);
        m_status.lastRxPayload[sizeof(m_status.lastRxPayload) - 1] = '\0';
    }
    if (m_handler)
    {
        m_handler(sub, p);
    }
}
