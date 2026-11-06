/******************************************************************************
 * @file StatusDisplay.hpp
 *
 * Kleine Statusanzeige am Steuergerät (Stufe 4c-2):
 * Waveshare 1.54inch e-Paper Module Rev2.1 (V2), SSD1681, 200×200, s/w.
 *
 * Anschluss (Pinplan 06.11.2026): SCK 18, DIN 23, CS 5, DC 13 (TCK),
 * RST 4, BUSY 34, VCC 3,3 V. BS-Brücke = 0 (4-Draht-SPI).
 *
 * Inhalt (Projektinhaber 06.11.2026, Änderungen jederzeit möglich):
 *   ┌────────────────────────┐
 *   │        100,0%          │  SOC (groß)
 *   │ 12,33V          -1,5A  │  Batteriespannung, Strom
 *   │ -18W          PV 96W   │  Leistung, PV-Leistung
 *   │────────────────────────│
 *   │ [ WR ] [Licht] [Kuehl] │  Relais: schwarz = EIN, weiß = AUS
 *   │ ! Warnung / OVERRIDE   │  Statuszeile
 *   └────────────────────────┘
 *
 * Aktualisierung (Projektinhaber 06.11.2026):
 *   - Reine Wertänderungen höchstens alle MinValueIntervalMs (30 s).
 *   - Relais-, Fehler- oder Override-Änderung: sofort.
 *   - Immer als VOLLBILD (UsePartialRefresh = false), da die
 *     Teilaktualisierung dieses Panels Schwarz grau werden lässt.
 *     Mit UsePartialRefresh = true: Teilaktualisierung, alle
 *     FullRefreshEvery Mal ein Vollbild.
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
};

class StatusDisplay
{
public:

    static constexpr uint32_t MinValueIntervalMs = 30000;  ///< Werte höchstens alle 30 s
    static constexpr uint32_t FullRefreshEvery   = 30;     ///< Teilaktualisierungen bis Vollbild

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

    /// Aufbereiteter Inhalt; Vergleich entscheidet über Aktualisierung.
    struct Content
    {
        char soc[16];
        char voltage[16];
        char current[16];
        char power[16];
        char pv[16];
        char status[48];
        bool statusInverted;          ///< schwarzer Balken (Warnung/Override)
        bool relayOn[RelayCount];
        bool relayEnabled[RelayCount];
        char relayName[RelayCount][8];
    };

    static void buildContent(const DisplayData& data, Content& out);
    void render(const Content& content, bool fullRefresh);

    Content  m_shown        = {};
    bool     m_hasShown     = false;
    uint32_t m_lastUpdateMs = 0;
    uint32_t m_partialCount = 0;
    uint32_t m_updates      = 0;
};
