/******************************************************************************
 * @file SettingsStore.hpp
 *
 * Dauerhafter Einstellungsspeicher (Schlüssel/Wert, nach Namensräumen).
 * Teil der wiederverwendbaren WebUI-Basis (lib/WebCore).
 *
 *   ESP32  : NVS über die Preferences-Bibliothek
 *   ESP8266: folgt in einem eigenen Schritt (LittleFS-Datei)
 *
 * Sicherheit: NVS ist ohne Flash-Verschlüsselung NICHT verschlüsselt.
 * Wer das Gerät in der Hand hat, kann den Flash auslesen. Gegen Angriffe
 * über das Netz schützt, dass Passwörter das Gerät nie verlassen (die
 * WebUI zeigt nur "gesetzt/nicht gesetzt"). Siehe Claude.md.
 *
 * Alle Funktionen öffnen und schließen den Namensraum selbst -> einfach
 * und sicher, für seltene Zugriffe (Konfiguration) ausreichend schnell.
 ******************************************************************************/

#pragma once

#include <Arduino.h>

class SettingsStore
{
public:

    static String   getString(const char* ns, const char* key, const char* defaultValue = "");
    static bool     putString(const char* ns, const char* key, const String& value);

    static bool     getBool(const char* ns, const char* key, bool defaultValue);
    static bool     putBool(const char* ns, const char* key, bool value);

    static uint32_t getUInt(const char* ns, const char* key, uint32_t defaultValue);
    static bool     putUInt(const char* ns, const char* key, uint32_t value);

    /// Schlüssel löschen.
    static bool     remove(const char* ns, const char* key);

    /// Ganzen Namensraum löschen (z. B. Netzwerk-Reset).
    static bool     clear(const char* ns);
};
