/******************************************************************************
 * @file SettingsBackup.hpp
 *
 * W5: Sicherung und Wiederherstellung aller Einstellungen als Datei.
 *
 * Format: flaches JSON-Objekt (eine Ebene, Schlüssel mit Punkt gegliedert),
 * gut lesbar und mit jedem Editor prüfbar, z. B.
 *
 *   {"format":"vedirect-backup","version":1,"firmware":"26.11.11-1803",
 *    "installation":"camper","relay1.lock":25.0,"relay1.off":15.0, ...}
 *
 * Grundsätze (Projektinhaber 11.11.2026):
 *   - Passwörter (WLAN, MQTT, Gerät) sind NIE enthalten.
 *   - Wiederherstellt wird immer ALLES; eine fehlerhafte Datei wird
 *     komplett abgelehnt (nichts wird halb übernommen).
 *   - Unbekannte Schlüssel werden ignoriert (z. B. aus neuerer Firmware
 *     derselben Formatversion); eine höhere Formatversion wird abgelehnt.
 *
 * Reines C++ (std::string) ohne Arduino -> auf dem PC testbar.
 ******************************************************************************/

#pragma once

#include <cstdint>
#include <string>

#include "LoadController.hpp"

struct BackupData
{
    static constexpr uint32_t FormatVersion = 1;

    std::string firmware;           ///< nur Information
    std::string created;            ///< nur Information (Ortszeit oder leer)

    bool balcony = false;           ///< Anlage: false = Camper

    /// Relais: immer die WIRKSAMEN Werte aller Relais (Strategie C,
    /// Projektinhaber 11.11.2026). Beim Wiederherstellen entscheidet der
    /// Vergleich mit der Voreinstellung der Anlage: gleich = Voreinstellung,
    /// abweichend = geändert. Kein Merker -> in der Datei direkt änderbar.
    RelayLimits relays[RelayCount];

    std::string tz  = "berlin";
    std::string ntp = "pool.ntp.org";

    bool        mqttEnabled  = false;
    std::string mqttHost;
    uint32_t    mqttPort     = 1883;
    std::string mqttUser;
    std::string mqttBase     = "vedirect";
    uint32_t    mqttInterval = 10;    ///< s
    std::string mqttClient;           ///< leer = automatisch

    std::string wifiSsid;             ///< leer = kein WLAN eingerichtet
    bool        apAlways = false;
    std::string hostname;             ///< leer = Standard
};

/// Sicherung als JSON-Text.
std::string backupToJson(const BackupData& data);

/// JSON-Text einlesen UND prüfen. false = abgelehnt, error = Grund (deutsch).
/// "out" wird nur bei Erfolg verändert.
bool backupFromJson(const char* text, BackupData& out, std::string& error);
