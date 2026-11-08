/******************************************************************************
 * @file StatusDisplay.hpp
 *
 * Kleine Statusanzeige am Steuergerät:
 * Waveshare 1.54inch e-Paper Module Rev2.1 (V2), SSD1681, 200×200, s/w.
 *
 * Anschluss (Pinplan 06.11.2026): SCK 18, DIN 23, CS 5, DC 13 (TCK),
 * RST 4, BUSY 34, VCC 3,3 V. BS-Brücke = 0 (4-Draht-SPI).
 *
 * Inhalt (Projektinhaber 08.11.2026: Status nur noch als Symbole):
 *   ┌──────────────────────────────┐
 *   │ ▂▄▆█  [AP] [MQTT]   [OVR]  ⚠ │  Symbolleiste
 *   │  ▲        100,0%             │  Lade-/Entladepfeil + SOC
 *   │ 12,33V               -1,5A   │  Spannung (* = vom MPPT), Strom
 *   │ -18W               PV 96W    │  Leistung, PV-Leistung
 *   │──────────────────────────────│
 *   │ [ WR ] [Licht] [Kuehl]       │  Relais: schwarz = EIN
 *   │ Shunt ●          MPPT ◐      │  Verbindung: ● ok ◐ instabil ⊗ defekt
 *   └──────────────────────────────┘
 *
 *   WLAN   4 Balken nach Signalstärke; ohne WLAN leer + durchgestrichen
 *   AP     schwarz = an, durchgestrichen = aus
 *   MQTT   nur sichtbar bei WLAN + eingerichtetem MQTT (ab W3);
 *          schwarz = verbunden, durchgestrichen = nicht verbunden
 *   OVR    nur bei aktivem Override
 *   ⚠      gefüllt = kritischer Fehler, leer = Warnung, sonst nicht sichtbar
 *   Pfeil  ▲ lädt, ▼ entlädt, o Ruhe (|I| < IdleCurrentA)
 *   Während der Ersteinrichtung: WLAN-Name + Startpasswort statt der
 *   Verbindungszeile.
 *
 * Aktualisierung:
 *   - Sofort: Relais, AP, MQTT, Override, Warnsymbol, Verbindungen,
 *     Ersteinrichtung.
 *   - Höchstens alle MinValueIntervalMs (30 s): Messwerte, WLAN-Balken,
 *     Ladepfeil (schwanken leicht, sonst ständiges Flackern).
 *   - Immer als VOLLBILD (UsePartialRefresh = false), da die
 *     Teilaktualisierung dieses Panels Schwarz grau werden lässt.
 *
 * Die Schriften (Adafruit GFX) enthalten keine Umlaute -> Texte werden
 * nach ASCII umgeschrieben (ä -> ae ...).
 *
 * Nur aus EINER Task verwenden (displayTask in main.cpp). Schreibt nicht
 * auf Serial (Diagnoseausgabe der Bibliothek ist abgeschaltet).
 ******************************************************************************/

#pragma once

#include <Arduino.h>

#include "LoadController.hpp"
#include "SystemState.hpp"

/// Eingabe für die Anzeige (Kopie, von main.cpp gefüllt).
struct DisplayData
{
    SystemState system;
    bool        relayOn[RelayCount]      = {};
    bool        relayEnabled[RelayCount] = {};
    const char* relayShortName[RelayCount] = {};
    bool        overrideActive = false;

    // Netzwerk
    bool        wifiConnected  = false;
    int8_t      wifiRssi       = 0;      ///< dBm
    bool        apActive       = false;
    bool        mqttConfigured = false;  ///< ab W3
    bool        mqttConnected  = false;  ///< ab W3

    // Ersteinrichtung (W1): AP-Name + Startpasswort statt Verbindungszeile
    bool        showSetup = false;
    char        setupSsid[33]     = {};
    char        setupPassword[24] = {};
};

class StatusDisplay
{
public:

    static constexpr uint32_t MinValueIntervalMs = 30000;  ///< Werte höchstens alle 30 s
    static constexpr uint32_t FullRefreshEvery   = 30;     ///< Teilaktualisierungen bis Vollbild
    static constexpr float    IdleCurrentA       = 0.05f;  ///< darunter: weder Laden noch Entladen

    /// Teilaktualisierung verwenden? (Test 06.11.2026: Bei diesem Panel
    /// hellt die eingebaute Teilaktualisierungs-Kurve unveränderte schwarze
    /// Pixel grau auf. Daher nur Vollbilder: deckendes Schwarz, dafür
    /// kurzes Flackern bei jeder Aktualisierung.)
    static constexpr bool UsePartialRefresh = false;

    /// Display initialisieren und erstes Bild (Vollaktualisierung) zeigen.
    void begin(const DisplayData& data, uint32_t nowMs);

    /// Regelmäßig aufrufen (z. B. 1×/s); aktualisiert nur bei Bedarf.
    void update(const DisplayData& data, uint32_t nowMs);

    uint32_t updateCount() const { return m_updates; }

private:

    /// Verbindungszustand eines Geräts.
    enum LinkState : uint8_t { LinkOk = 0, LinkUnstable = 1, LinkBroken = 2 };

    /// Lade-/Entladerichtung.
    enum ChargeDir : uint8_t { DirUnknown = 0, DirCharge, DirDischarge, DirIdle };

    /// Warnsymbol.
    enum Warning : uint8_t { WarnNone = 0, WarnWarning, WarnCritical };

    /// Aufbereiteter Inhalt; Vergleich entscheidet über Aktualisierung.
    struct Content
    {
        // ---- Werte (gedrosselt) ----
        char    soc[16];
        char    voltage[16];
        char    current[16];
        char    power[16];
        char    pv[16];
        int8_t  wifiBars;        ///< 0..4, -1 = kein WLAN
        uint8_t chargeDir;       ///< ChargeDir

        // ---- Symbole/Zustände (sofort) ----
        bool    apActive;
        bool    mqttShown;
        bool    mqttConnected;
        bool    overrideActive;
        uint8_t warning;         ///< Warning
        uint8_t linkShunt;       ///< LinkState
        uint8_t linkMppt;        ///< LinkState
        bool    relayOn[RelayCount];
        bool    relayEnabled[RelayCount];
        char    relayName[RelayCount][8];
        bool    setup;           ///< Ersteinrichtung: 2 Zeilen statt Verbindungen
        char    setupLine1[48];
        char    setupLine2[48];
    };

    static void buildContent(const DisplayData& data, Content& out);
    static bool importantChange(const Content& a, const Content& b);
    void render(const Content& content, bool fullRefresh);

    Content  m_shown        = {};
    bool     m_hasShown     = false;
    uint32_t m_lastUpdateMs = 0;
    uint32_t m_partialCount = 0;
    uint32_t m_updates      = 0;
};
