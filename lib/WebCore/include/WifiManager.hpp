/******************************************************************************
 * @file WifiManager.hpp
 *
 * WLAN-Verwaltung der WebUI-Basis (lib/WebCore).
 *
 * Verhalten (Projektinhaber 08.11.2026):
 *   - Erststart / kein WLAN konfiguriert:
 *       -> nur Access Point (AP), WPA2-geschützt mit dem Gerätepasswort
 *   - WLAN konfiguriert:
 *       -> verbinden (max. ConnectTimeoutMs)
 *       -> klappt das nicht: AP zusätzlich einschalten ("Fallback") und
 *          alle RetryIntervalMs erneut verbinden
 *       -> sobald verbunden: AP wieder aus (außer apAlways oder solange
 *          noch ein Gerät am AP angemeldet ist)
 *       -> Verbindung im Betrieb verloren: nach LostToFallbackMs Fallback
 *   - apAlways = true: AP ist immer an (z. B. Camper ohne Router)
 *   - Im AP: Captive Portal (DNS beantwortet alles mit der AP-Adresse,
 *     das Handy öffnet die Seite automatisch).
 *   - mDNS: erreichbar als http://<hostname>.local (Standard "ve-direct")
 *
 * Gerätepasswort:
 *   - beim ersten Start zufällig erzeugt (10 Zeichen, ohne verwechselbare
 *     Zeichen), gilt für AP UND WebUI-Login (Benutzer "admin")
 *   - solange unverändert, darf es auf dem Display angezeigt werden
 *     (NetStatus::displayPassword); nach Änderung nie mehr
 *
 * Sicherheit: WiFi.persistent(false) -> die WLAN-Zugangsdaten werden NICHT
 * zusätzlich vom WLAN-Treiber im Flash abgelegt, nur in unserem
 * Einstellungsspeicher.
 *
 * Nur aus EINER Task verwenden (Web-Task). Andere Tasks bekommen eine
 * Kopie von status().
 ******************************************************************************/

#pragma once

#include <Arduino.h>

enum class NetState : uint8_t
{
    AccessPoint,  ///< nur AP (kein WLAN konfiguriert)
    Connecting,   ///< verbindet mit dem WLAN
    Connected,    ///< mit dem WLAN verbunden
    Fallback      ///< WLAN nicht erreichbar: AP an, Verbindung wird weiter versucht
};

/// Zustand für Anzeige/Log (Kopie, frei zwischen Tasks weiterzugeben).
struct NetStatus
{
    NetState state           = NetState::AccessPoint;
    bool     apActive        = false;
    bool     staConnected    = false;
    char     apSsid[33]      = {};
    char     staSsid[33]     = {};
    char     hostname[33]    = {};
    uint32_t staIp           = 0;     ///< IPv4 als Zahl (0 = keine)
    uint32_t apIp            = 0;
    int8_t   rssi            = 0;     ///< Signalstärke in dBm
    uint8_t  apClients       = 0;     ///< am AP angemeldete Geräte
    bool     defaultPassword = true;  ///< Gerätepasswort noch unverändert
    char     displayPassword[24] = {};///< nur gefüllt, solange defaultPassword
    uint32_t connectCount    = 0;     ///< erfolgreiche Verbindungen seit Start
    uint32_t attempt         = 0;     ///< Nummer des aktuellen Verbindungsversuchs
    uint8_t  lastFailReason  = 0;     ///< Grund des letzten Fehlschlags (0 = keiner),
                                      ///< Text über WifiManager::reasonText()
};

class WifiManager
{
public:

    /// Geduld beim Verbinden, bevor der AP als Rückfall startet. 30 s, weil
    /// manche Router (Fritz!Box) direkt nach einem Neustart des ESP32 den
    /// ersten Versuch ablehnen (Code 202) (Finding 08.11.2026).
    static constexpr uint32_t ConnectTimeoutMs = 30000;
    static constexpr uint32_t FirstRetryMs     = 15000;   ///< 1. Versuch im Rückfall
    static constexpr uint32_t RetryIntervalMs  = 60000;   ///< weitere Versuche
    static constexpr uint32_t ScanPauseMs      = 15000;   ///< Wiederverbinden während Suche pausiert
    static constexpr uint32_t LostToFallbackMs = 20000;
    static constexpr size_t   MinPasswordLength = 8;   ///< WPA2-Minimum

    /// Einstellungen laden, ggf. Gerätepasswort erzeugen, WLAN starten.
    void begin(const char* defaultHostname, const char* apPrefix);

    /// Regelmäßig aufrufen (z. B. alle paar ms aus der Web-Task).
    void loop(uint32_t nowMs);

    NetStatus status() const { return m_status; }

    // ---- Konfiguration (aus der WebUI) ----
    String ssid() const        { return m_ssid; }
    bool   passwordSet() const { return m_password.length() > 0; }
    bool   apAlways() const    { return m_apAlways; }
    String hostname() const    { return m_hostname; }

    /// WLAN setzen. password leer = bisheriges behalten. Wird sofort
    /// angewendet (kein Neustart nötig).
    bool setWifi(const String& ssid, const String& password, bool apAlways);

    /// WLAN-Zugangsdaten löschen -> nur noch AP.
    bool resetWifi();

    /// Gerätepasswort (AP + Login).
    const String& devicePassword() const { return m_devicePassword; }
    bool changeDevicePassword(const String& newPassword);

    /// Netzwerk-Einstellungen komplett löschen (Reset-Taste). Beim nächsten
    /// Start wird ein neues Gerätepasswort erzeugt.
    static bool factoryResetNetwork();

    /// Zufälliges Passwort (ohne 0/O/1/l/I).
    static String generatePassword(size_t length);

    /// Verständlicher Text zum Fehlergrund (NetStatus::lastFailReason).
    static const char* reasonText(uint8_t reason);

    /// Vor einer WLAN-Suche aufrufen: STA einschalten (im reinen AP-Modus
    /// aus) und einen laufenden Verbindungsversuch anhalten (sonst schlägt
    /// die Suche fehl). Der nächste Wiederholversuch verbindet wieder.
    void prepareScan();

private:

    void loadConfig(const char* defaultHostname);
    void startStation(uint32_t nowMs);
    void startAp();
    void stopAp();
    void updateStatus();

    String   m_ssid;
    String   m_password;
    String   m_devicePassword;
    String   m_hostname;
    String   m_apSsid;
    bool     m_apAlways        = false;
    bool     m_defaultPassword = true;

    NetState m_state           = NetState::AccessPoint;
    bool     m_apActive        = false;
    bool     m_reconfigure     = false;
    uint32_t m_stateSinceMs    = 0;
    uint32_t m_lastRetryMs     = 0;
    uint32_t m_lostSinceMs     = 0;
    bool     m_lost            = false;
    uint32_t m_connectCount    = 0;
    uint32_t m_attempt         = 0;
    uint32_t m_fallbackRetries = 0;
    bool     m_scanPause       = false;
    uint32_t m_scanUntilMs     = 0;

    NetStatus m_status;
};
