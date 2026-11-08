/******************************************************************************
 * @file StatusDisplay.cpp
 ******************************************************************************/

#include "StatusDisplay.hpp"

#include <SPI.h>
#include <GxEPD2_BW.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>

#include <cstdio>
#include <cstring>

namespace
{

// ---- Anschluss (Pinplan 06.11.2026) ----
constexpr int8_t PinSck  = 18;
constexpr int8_t PinMosi = 23;
constexpr int8_t PinCs   = 5;
constexpr int8_t PinDc   = 13;   // auf dem Board "TCK"
constexpr int8_t PinRst  = 4;
constexpr int8_t PinBusy = 34;

/// Drehung des Bildes (0..3), je nach Einbaulage anpassen.
constexpr uint8_t Rotation = 0;

/// 1.54" 200×200 SSD1681 (Waveshare V2), voller Bildpuffer.
///
/// Treiberwahl (Test 06.11.2026): Mit GxEPD2_154_D67 (älteres Panel
/// GDEH0154D67) war Schwarz nach einer Vollaktualisierung nur grau/körnig,
/// nach Teilaktualisierung tiefschwarz. Das Panel trägt den Aufdruck
/// "FPC-B001-exp 24.08.02" (2024) -> neuerer Typ GDEY0154D67.
using EpdDriver = GxEPD2_154_GDEY0154D67;

GxEPD2_BW<EpdDriver, EpdDriver::HEIGHT>
    g_epd(EpdDriver(PinCs, PinDc, PinRst, PinBusy));

/******************************************************************************
 * UTF-8 -> ASCII (Schriften ohne Umlaute): ä->ae, ö->oe, ü->ue, ß->ss ...
 ******************************************************************************/
void toAscii(const char* in, char* out, size_t size)
{
    size_t n = 0;
    auto put = [&](const char* s) {
        for (; *s != '\0' && n + 1 < size; ++s)
        {
            out[n++] = *s;
        }
    };

    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(in); *p != '\0'; ++p)
    {
        if (*p == 0xC3 && p[1] != '\0')
        {
            switch (p[1])
            {
                case 0xA4: put("ae"); break;
                case 0xB6: put("oe"); break;
                case 0xBC: put("ue"); break;
                case 0x84: put("Ae"); break;
                case 0x96: put("Oe"); break;
                case 0x9C: put("Ue"); break;
                case 0x9F: put("ss"); break;
                default:   put("?");  break;
            }
            ++p;
        }
        else if (*p == 0xC2 && p[1] != '\0')
        {
            ++p;   // z. B. "°" weglassen
        }
        else if (*p < 0x80)
        {
            const char c[2] = { static_cast<char>(*p), '\0' };
            put(c);
        }
    }
    out[n] = '\0';
}

/// Messwert mit Dezimalkomma: "12,33V", ungültig "--V".
void formatReading(const Reading& r, int decimals, const char* unit, char* out, size_t size)
{
    if (!r.valid)
    {
        snprintf(out, size, "--%s", unit);
        return;
    }
    snprintf(out, size, "%.*f%s", decimals, static_cast<double>(r.value), unit);
    for (char* p = out; *p != '\0'; ++p)
    {
        if (*p == '.')
        {
            *p = ',';
        }
    }
}

/// Text in der aktuellen Schrift zentriert ausgeben (Grundlinie y).
void printCentered(const char* text, int16_t centerX, int16_t y)
{
    int16_t x1, y1;
    uint16_t w, h;
    g_epd.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
    g_epd.setCursor(centerX - static_cast<int16_t>(w) / 2 - x1, y);
    g_epd.print(text);
}

/// Text rechtsbündig bis rightX ausgeben.
void printRight(const char* text, int16_t rightX, int16_t y)
{
    int16_t x1, y1;
    uint16_t w, h;
    g_epd.getTextBounds(text, 0, y, &x1, &y1, &w, &h);
    g_epd.setCursor(rightX - static_cast<int16_t>(w) - x1, y);
    g_epd.print(text);
}

/// Text auf maxWidth Pixel kürzen (in der aktuellen Schrift).
void fitText(char* text, uint16_t maxWidth)
{
    int16_t x1, y1;
    uint16_t w, h;
    size_t len = std::strlen(text);
    while (len > 0)
    {
        g_epd.getTextBounds(text, 0, 20, &x1, &y1, &w, &h);
        if (w <= maxWidth)
        {
            return;
        }
        text[--len] = '\0';
    }
}

} // namespace

/******************************************************************************
 * Symbole (Stand 08.11.2026)
 ******************************************************************************/
namespace
{

/// WLAN-Balken: 4 Balken, unten bündig bei yBottom. bars < 0 = kein WLAN
/// (leere Balken + durchgestrichen).
void drawWifi(int16_t x, int16_t yBottom, int8_t bars)
{
    for (int8_t i = 0; i < 4; ++i)
    {
        const int16_t h  = static_cast<int16_t>(4 + i * 4);   // 4, 8, 12, 16
        const int16_t bx = static_cast<int16_t>(x + i * 5);
        if (bars > i)
        {
            g_epd.fillRect(bx, yBottom - h, 3, h, GxEPD_BLACK);
        }
        else
        {
            g_epd.drawRect(bx, yBottom - h, 3, h, GxEPD_BLACK);
        }
    }
    if (bars < 0)
    {
        g_epd.drawLine(x - 1, yBottom, x + 19, yBottom - 17, GxEPD_BLACK);
        g_epd.drawLine(x - 1, yBottom - 1, x + 19, yBottom - 18, GxEPD_BLACK);
    }
}

/// Textkästchen: gefüllt (schwarz, weiße Schrift) oder umrandet;
/// optional durchgestrichen. Breite wird aus dem Text berechnet.
/// Liefert die Breite zurück.
int16_t drawTextBox(int16_t x, int16_t y, const char* text, bool filled, bool struck)
{
    g_epd.setFont(&FreeSans9pt7b);
    int16_t x1, y1;
    uint16_t w, h;
    g_epd.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    const int16_t boxW = static_cast<int16_t>(w + 8);
    const int16_t boxH = 18;

    if (filled)
    {
        g_epd.fillRect(x, y, boxW, boxH, GxEPD_BLACK);
        g_epd.setTextColor(GxEPD_WHITE);
    }
    else
    {
        g_epd.drawRect(x, y, boxW, boxH, GxEPD_BLACK);
        g_epd.setTextColor(GxEPD_BLACK);
    }
    g_epd.setCursor(x + 4 - x1, y + 14);
    g_epd.print(text);
    g_epd.setTextColor(GxEPD_BLACK);

    if (struck)
    {
        g_epd.drawLine(x, y + boxH - 1, x + boxW - 1, y, GxEPD_BLACK);
    }
    return boxW;
}

/// Warndreieck mit "!", gefüllt (kritisch) oder umrandet (Warnung).
void drawWarningIcon(int16_t x, int16_t y, bool filled)
{
    const int16_t w = 20, h = 18;
    const uint16_t fg = filled ? GxEPD_WHITE : GxEPD_BLACK;
    if (filled)
    {
        g_epd.fillTriangle(x, y + h, x + w / 2, y, x + w, y + h, GxEPD_BLACK);
    }
    else
    {
        g_epd.drawTriangle(x, y + h, x + w / 2, y, x + w, y + h, GxEPD_BLACK);
    }
    // Ausrufezeichen
    g_epd.fillRect(x + w / 2 - 1, y + 6, 3, 7, fg);
    g_epd.fillRect(x + w / 2 - 1, y + 14, 3, 2, fg);
}

/// Lade-/Entladepfeil bzw. Ruhe-Kreis, zentriert um (cx, cy).
void drawChargeArrow(int16_t cx, int16_t cy, uint8_t dir)
{
    switch (dir)
    {
        case 1:   // Laden: Pfeil nach oben
            g_epd.fillTriangle(cx - 9, cy - 2, cx, cy - 13, cx + 9, cy - 2, GxEPD_BLACK);
            g_epd.fillRect(cx - 3, cy - 2, 7, 14, GxEPD_BLACK);
            break;
        case 2:   // Entladen: Pfeil nach unten
            g_epd.fillTriangle(cx - 9, cy + 2, cx, cy + 13, cx + 9, cy + 2, GxEPD_BLACK);
            g_epd.fillRect(cx - 3, cy - 12, 7, 14, GxEPD_BLACK);
            break;
        case 3:   // Ruhe: Kreis "o"
            g_epd.drawCircle(cx, cy, 7, GxEPD_BLACK);
            g_epd.drawCircle(cx, cy, 6, GxEPD_BLACK);
            break;
        default:  // unbekannt: nichts
            break;
    }
}

/// Verbindungssymbol: ● ok, ◐ instabil, ⊗ defekt (Radius 7).
void drawLinkIcon(int16_t cx, int16_t cy, uint8_t state)
{
    const int16_t r = 7;
    switch (state)
    {
        case 0:   // ok
            g_epd.fillCircle(cx, cy, r, GxEPD_BLACK);
            break;
        case 1:   // instabil: halb gefüllt
            g_epd.fillCircle(cx, cy, r, GxEPD_BLACK);
            g_epd.fillRect(cx + 1, cy - r, r + 1, 2 * r + 1, GxEPD_WHITE);
            g_epd.drawCircle(cx, cy, r, GxEPD_BLACK);
            break;
        default:  // defekt: Kreis mit X
            g_epd.drawCircle(cx, cy, r, GxEPD_BLACK);
            g_epd.drawLine(cx - 4, cy - 4, cx + 4, cy + 4, GxEPD_BLACK);
            g_epd.drawLine(cx - 4, cy + 4, cx + 4, cy - 4, GxEPD_BLACK);
            break;
    }
}

} // namespace

/******************************************************************************
 * Inhalt aufbereiten
 ******************************************************************************/
void StatusDisplay::buildContent(const DisplayData& data, Content& out)
{
    std::memset(&out, 0, sizeof(out));
    const SystemState& s = data.system;

    formatReading(s.stateOfCharge,  1, "%", out.soc,     sizeof(out.soc));
    formatReading(s.batteryVoltage, 2, "V", out.voltage, sizeof(out.voltage));
    if (s.batteryVoltage.valid && s.batteryVoltage.source == ValueSource::PvCharger)
    {
        // Ersatzwert vom MPPT kennzeichnen
        std::strncat(out.voltage, "*", sizeof(out.voltage) - std::strlen(out.voltage) - 1);
    }
    formatReading(s.batteryCurrent, 1, "A", out.current, sizeof(out.current));
    formatReading(s.batteryPower,   0, "W", out.power,   sizeof(out.power));

    char pv[12];
    formatReading(s.pvPower, 0, "W", pv, sizeof(pv));
    snprintf(out.pv, sizeof(out.pv), "PV %s", pv);

    for (size_t i = 0; i < RelayCount; ++i)
    {
        out.relayOn[i]      = data.relayOn[i];
        out.relayEnabled[i] = data.relayEnabled[i];
        toAscii(data.relayShortName[i] != nullptr ? data.relayShortName[i] : "?",
                out.relayName[i], sizeof(out.relayName[i]));
    }

    // ---- Lade-/Entladerichtung aus dem Batteriestrom (Shunt) ----
    if (s.batteryCurrent.valid)
    {
        const float i = s.batteryCurrent.value;
        out.chargeDir = (i > IdleCurrentA)  ? DirCharge
                      : (i < -IdleCurrentA) ? DirDischarge
                                            : DirIdle;
    }
    else
    {
        out.chargeDir = DirUnknown;
    }

    // ---- Netzwerk ----
    if (!data.wifiConnected)
    {
        out.wifiBars = -1;
    }
    else
    {
        const int r = data.wifiRssi;
        out.wifiBars = (r >= -55) ? 4 : (r >= -65) ? 3 : (r >= -75) ? 2 : (r >= -85) ? 1 : 0;
    }
    out.apActive      = data.apActive;
    out.mqttShown     = data.wifiConnected && data.mqttConfigured;
    out.mqttConnected = out.mqttShown && data.mqttConnected;

    // ---- Override, Warnsymbol ----
    out.overrideActive = data.overrideActive;
    out.warning = (s.severity == Severity::Critical) ? WarnCritical
                : (s.severity == Severity::Warning)  ? WarnWarning
                                                     : WarnNone;

    // ---- Verbindungen: defekt = fehlt/ausgefallen/mehrdeutig ----
    if (s.faults & (SystemFault::BatteryMonitorMissing | SystemFault::MultipleBatteryMonitors))
        out.linkShunt = LinkBroken;
    else if (s.faults & SystemFault::BatteryLinkUnstable)
        out.linkShunt = LinkUnstable;
    else
        out.linkShunt = LinkOk;

    if (s.faults & (SystemFault::PvChargerMissing | SystemFault::MultiplePvChargers))
        out.linkMppt = LinkBroken;
    else if (s.faults & SystemFault::PvLinkUnstable)
        out.linkMppt = LinkUnstable;
    else
        out.linkMppt = LinkOk;

    // ---- Ersteinrichtung: WLAN-Name und Startpasswort (ersetzt die
    //      Verbindungszeile, damit man das Gerät immer einrichten kann) ----
    if (data.showSetup)
    {
        out.setup = true;
        snprintf(out.setupLine1, sizeof(out.setupLine1), "WLAN %s", data.setupSsid);
        snprintf(out.setupLine2, sizeof(out.setupLine2), "PW %s", data.setupPassword);
    }
}

/// Ändert sich ein Symbol/Zustand (nicht nur ein Messwert)? -> sofort zeigen.
bool StatusDisplay::importantChange(const Content& a, const Content& b)
{
    return a.apActive       != b.apActive ||
           a.mqttShown      != b.mqttShown ||
           a.mqttConnected  != b.mqttConnected ||
           a.overrideActive != b.overrideActive ||
           a.warning        != b.warning ||
           a.linkShunt      != b.linkShunt ||
           a.linkMppt       != b.linkMppt ||
           (a.wifiBars < 0) != (b.wifiBars < 0) ||   // WLAN weg/da: sofort
           std::memcmp(a.relayOn, b.relayOn, sizeof(a.relayOn)) != 0 ||
           std::memcmp(a.relayEnabled, b.relayEnabled, sizeof(a.relayEnabled)) != 0 ||
           a.setup != b.setup ||
           std::strcmp(a.setupLine1, b.setupLine1) != 0 ||
           std::strcmp(a.setupLine2, b.setupLine2) != 0;
}

/******************************************************************************
 * Zeichnen
 ******************************************************************************/
void StatusDisplay::render(const Content& c, bool fullRefresh)
{
    const int16_t W = static_cast<int16_t>(g_epd.width());

    if (fullRefresh)
    {
        g_epd.setFullWindow();
    }
    else
    {
        g_epd.setPartialWindow(0, 0, g_epd.width(), g_epd.height());
    }

    g_epd.firstPage();
    do
    {
        g_epd.fillScreen(GxEPD_WHITE);
        g_epd.setTextColor(GxEPD_BLACK);

        // ---- Symbolleiste (y 0..20) ----
        drawWifi(2, 19, c.wifiBars);
        int16_t x = 26;
        x = static_cast<int16_t>(x + drawTextBox(x, 2, "AP", c.apActive, !c.apActive) + 4);
        if (c.mqttShown)
        {
            drawTextBox(x, 2, "MQTT", c.mqttConnected, !c.mqttConnected);
        }
        if (c.overrideActive)
        {
            drawTextBox(132, 2, "OVR", true, false);
        }
        if (c.warning != WarnNone)
        {
            drawWarningIcon(W - 22, 2, c.warning == WarnCritical);
        }

        // ---- Lade-/Entladepfeil + SOC groß ----
        drawChargeArrow(14, 46, c.chargeDir);
        g_epd.setFont(&FreeSansBold24pt7b);
        printCentered(c.soc, W / 2 + 10, 62);

        // ---- Spannung / Strom ----
        g_epd.setFont(&FreeSansBold12pt7b);
        g_epd.setCursor(4, 90);
        g_epd.print(c.voltage);
        printRight(c.current, W - 4, 90);

        // ---- Leistung / PV ----
        g_epd.setFont(&FreeSans9pt7b);
        g_epd.setCursor(4, 113);
        g_epd.print(c.power);
        printRight(c.pv, W - 4, 113);

        g_epd.drawFastHLine(0, 121, W, GxEPD_BLACK);

        // ---- Relais: schwarz = EIN, weiß = AUS, durchgestrichen = deaktiviert ----
        const int16_t boxW = 62, boxH = 28, boxY = 127;
        for (size_t i = 0; i < RelayCount; ++i)
        {
            const int16_t bx = static_cast<int16_t>(4 + i * (boxW + 3));
            const int16_t x = bx;
            if (c.relayEnabled[i] && c.relayOn[i])
            {
                g_epd.fillRect(x, boxY, boxW, boxH, GxEPD_BLACK);
                g_epd.setTextColor(GxEPD_WHITE);
            }
            else
            {
                g_epd.drawRect(x, boxY, boxW, boxH, GxEPD_BLACK);
                g_epd.setTextColor(GxEPD_BLACK);
            }
            printCentered(c.relayName[i], x + boxW / 2, boxY + 19);
            if (!c.relayEnabled[i])
            {
                g_epd.drawLine(x, boxY + boxH - 1, x + boxW - 1, boxY, GxEPD_BLACK);
            }
        }
        g_epd.setTextColor(GxEPD_BLACK);

        // ---- Ersteinrichtung: zwei Zeilen statt Verbindungszeile ----
        if (c.setup)
        {
            char line1[48];
            char line2[48];
            std::strncpy(line1, c.setupLine1, sizeof(line1) - 1);
            line1[sizeof(line1) - 1] = '\0';
            std::strncpy(line2, c.setupLine2, sizeof(line2) - 1);
            line2[sizeof(line2) - 1] = '\0';
            g_epd.setFont(&FreeSans9pt7b);
            fitText(line1, static_cast<uint16_t>(W - 8));
            fitText(line2, static_cast<uint16_t>(W - 8));
            g_epd.setCursor(4, 181);
            g_epd.print(line1);
            g_epd.setCursor(4, 197);
            g_epd.print(line2);
            continue;   // nächste Seite (bei vollem Puffer: Ende)
        }

        // ---- Verbindungen: Shunt und MPPT ----
        g_epd.setFont(&FreeSans9pt7b);
        g_epd.setCursor(4, 186);
        g_epd.print("Shunt");
        drawLinkIcon(66, 181, c.linkShunt);
        g_epd.setCursor(104, 186);
        g_epd.print("MPPT");
        drawLinkIcon(166, 181, c.linkMppt);
    }
    while (g_epd.nextPage());

    // Treiberspannungen abschalten (spart Strom, Bild bleibt stehen)
    g_epd.powerOff();
}

/******************************************************************************
 * Öffentliche Funktionen
 ******************************************************************************/
void StatusDisplay::begin(const DisplayData& data, uint32_t nowMs)
{
    // SPI nur mit SCK/MOSI starten: MISO wird nicht gebraucht, GPIO19
    // bleibt als Reserve (R4) frei. Spätere SPI.begin()-Aufrufe der
    // Bibliothek werden ignoriert, da SPI bereits läuft.
    SPI.begin(PinSck, -1, PinMosi, -1);

    // init(diagBaud, initial, resetDurationMs, pulldownRstMode):
    //   0     = keine Diagnoseausgabe auf Serial (nur der Logger schreibt dort)
    //   true  = erstes Bild als Vollaktualisierung
    //   2 ms  = kurzer Reset-Impuls. Laut GxEPD2-README nötig für
    //           Waveshare-Platinen mit "clever reset circuit" (Rev2.1).
    //           Symptom ohne: Vollaktualisierung mal deckend, mal grau.
    //   false = RST normal treiben (kein Pull-down-Modus)
    g_epd.init(0, true, 2, false);
    g_epd.setRotation(Rotation);

    buildContent(data, m_shown);
    render(m_shown, true);
    m_hasShown     = true;
    m_lastUpdateMs = nowMs;
    m_partialCount = 0;
    ++m_updates;
}

void StatusDisplay::update(const DisplayData& data, uint32_t nowMs)
{
    Content next;
    buildContent(data, next);

    if (m_hasShown && std::memcmp(&next, &m_shown, sizeof(next)) == 0)
    {
        return;   // nichts geändert
    }

    // Symbol-/Zustandsänderung: sofort. Nur Werte (inkl. WLAN-Balken,
    // Ladepfeil): gedrosselt.
    const bool important = !m_hasShown || importantChange(next, m_shown);

    if (!important && static_cast<uint32_t>(nowMs - m_lastUpdateMs) < MinValueIntervalMs)
    {
        return;
    }

    const bool full = !UsePartialRefresh || (m_partialCount >= FullRefreshEvery);
    render(next, full);
    m_partialCount = full ? 0 : m_partialCount + 1;

    m_shown        = next;
    m_hasShown     = true;
    m_lastUpdateMs = nowMs;
    ++m_updates;
}
