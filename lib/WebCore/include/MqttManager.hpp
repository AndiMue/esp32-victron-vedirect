/******************************************************************************
 * @file MqttManager.hpp
 *
 * MQTT-Verbindung der WebUI-Basis (lib/WebCore), Stufe W3.
 * Bibliothek: PubSubClient (ESP32 und ESP8266).
 *
 * Vorgaben (Projektinhaber 09.11.2026):
 *   - Broker: Mosquitto im Heimnetz (Node-RED, OpenCCU). Benutzer/Passwort
 *     möglich (derzeit leer), keine Verschlüsselung (TLS).
 *   - Alle Parameter in der WebUI einstellbar und im NVS gespeichert:
 *     ein/aus, Host/IP, Port, Benutzer, Passwort, Basis-Topic,
 *     Sendeintervall (Standard 10 s), Client-ID.
 *   - Passwort verlässt das Gerät nie (WebUI zeigt nur "gesetzt").
 *
 * Verhalten:
 *   - Letzter Wille: <base>/status = "offline" (retained), nach dem
 *     Verbinden "online" (retained). Node-RED sieht so sofort, ob das Gerät
 *     erreichbar ist.
 *   - Neuer Verbindungsversuch mit wachsendem Abstand (5 s ... 60 s).
 *   - Abonnements werden nach jedem (Wieder-)Verbinden erneuert.
 *   - Ohne WLAN-Verbindung (z. B. Camper, nur AP) wird nichts versucht.
 *
 * Threads:
 *   loop(), publish(), subscribe(), onMessage() NUR aus der MQTT-Task.
 *   config(), setConfig(), resetConfig(), reconnectNow(), status() sind
 *   threadsicher (z. B. aus der Web-Task).
 ******************************************************************************/

#pragma once

#include <Arduino.h>
#include <functional>
#include <mutex>
#include <vector>

struct MqttConfig
{
    bool     enabled   = false;
    String   host;                  ///< Hostname oder IP des Brokers
    uint16_t port      = 1883;
    String   user;                  ///< leer = ohne Anmeldung
    String   password;              ///< nie an die WebUI zurückgeben!
    String   baseTopic = "vedirect";
    uint32_t intervalS = 10;        ///< Sendeintervall
    String   clientId;              ///< leer = automatisch (Gerätename + MAC)
};

enum class MqttState : uint8_t
{
    Disabled,     ///< ausgeschaltet oder kein Broker eingetragen
    NoNetwork,    ///< kein WLAN (z. B. nur AP)
    Connecting,   ///< Verbindungsversuch läuft / steht an
    Connected,
    Error         ///< letzter Versuch fehlgeschlagen (siehe lastError)
};

struct MqttStatus
{
    MqttState state        = MqttState::Disabled;
    char      lastError[64] = {};
    uint32_t  connectCount = 0;
    uint32_t  publishCount = 0;
    uint32_t  lastPublishMs = 0;
    uint32_t  receiveCount = 0;
    char      lastRxTopic[64]   = {};   ///< zuletzt empfangen (ohne Basis-Topic), für Log
    char      lastRxPayload[32] = {};
};

class MqttManager
{
public:

    static constexpr uint32_t MinRetryMs = 5000;
    static constexpr uint32_t MaxRetryMs = 60000;
    static constexpr uint16_t BufferSize = 1024;   ///< max. Nachrichtengröße

    /// Nachricht: Topic OHNE Basis-Topic (z. B. "relay/r1-set") und Inhalt.
    using MessageHandler = std::function<void(const String& subtopic, const String& payload)>;

    // ---- nur MQTT-Task ----
    void begin(const char* defaultClientId);
    void loop(uint32_t nowMs, bool networkUp);
    bool publish(const char* subtopic, const String& payload, bool retain);
    void subscribe(const char* subtopic);
    void onMessage(MessageHandler handler) { m_handler = handler; }
    bool connected() const;

    // ---- threadsicher ----
    MqttConfig config() const;
    bool       passwordSet() const;
    /// Konfiguration speichern. password leer = bisheriges behalten.
    bool       setConfig(const MqttConfig& config);
    void       resetConfig();
    void       reconnectNow();
    MqttStatus status() const;
    uint32_t   intervalMs() const;
    String     baseTopic() const;

private:

    void loadConfig();
    void saveConfig();
    bool tryConnect(uint32_t nowMs);
    void setState(MqttState state, const char* error = nullptr);
    void handleMessage(char* topic, uint8_t* payload, unsigned int length);
    String fullTopic(const char* subtopic) const;

    mutable std::mutex m_mutex;     ///< schützt m_config, m_status, Flags
    MqttConfig  m_config;
    MqttStatus  m_status;
    bool        m_reconfigure = false;
    bool        m_reconnectNow = false;
    String      m_defaultClientId;

    // nur MQTT-Task
    MqttConfig  m_active;           ///< aktuell verwendete Konfiguration
    uint32_t    m_nextAttemptMs = 0;
    uint32_t    m_retryMs = MinRetryMs;
    std::vector<String> m_subscriptions;
    MessageHandler m_handler;
};
