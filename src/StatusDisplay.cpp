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

    // ---- Statuszeile: Override > Fehler (nach Schwere) > OK ----
    if (data.overrideActive)
    {
        snprintf(out.status, sizeof(out.status), "! OVERRIDE AKTIV !");
        out.statusInverted = true;
        return;
    }

    if (s.faults == 0)
    {
        snprintf(out.status, sizeof(out.status), "OK");
        return;
    }

    // Wichtigsten Fehler anzeigen: den mit der höchsten Schwere
    uint32_t shownBit = 0;
    for (unsigned bit = 0; bit < SystemFault::BitCount; ++bit)
    {
        const uint32_t mask = 1u << bit;
        if ((s.faults & mask) != 0 && SystemEvaluator::severityOf(mask, SystemConfig()) == s.severity)
        {
            shownBit = mask;
            break;
        }
    }

    char text[40];
    toAscii(shownBit != 0 ? SystemEvaluator::faultText(shownBit) : "Fehler", text, sizeof(text));

    if (s.severity == Severity::Info)
    {
        snprintf(out.status, sizeof(out.status), "i %s", text);
    }
    else
    {
        snprintf(out.status, sizeof(out.status), "! %s", text);
        out.statusInverted = true;
    }
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

        // ---- SOC groß ----
        g_epd.setFont(&FreeSansBold24pt7b);
        printCentered(c.soc, W / 2, 44);

        // ---- Spannung / Strom ----
        g_epd.setFont(&FreeSansBold12pt7b);
        g_epd.setCursor(4, 82);
        g_epd.print(c.voltage);
        printRight(c.current, W - 4, 82);

        // ---- Leistung / PV ----
        g_epd.setFont(&FreeSans9pt7b);
        g_epd.setCursor(4, 110);
        g_epd.print(c.power);
        printRight(c.pv, W - 4, 110);

        g_epd.drawFastHLine(0, 120, W, GxEPD_BLACK);

        // ---- Relais: schwarz = EIN, weiß = AUS, durchgestrichen = deaktiviert ----
        const int16_t boxW = 62, boxH = 30, boxY = 128;
        for (size_t i = 0; i < RelayCount; ++i)
        {
            const int16_t x = static_cast<int16_t>(4 + i * (boxW + 3));
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
            printCentered(c.relayName[i], x + boxW / 2, boxY + 20);
            if (!c.relayEnabled[i])
            {
                g_epd.drawLine(x, boxY + boxH - 1, x + boxW - 1, boxY, GxEPD_BLACK);
            }
        }
        g_epd.setTextColor(GxEPD_BLACK);

        // ---- Statuszeile ----
        const int16_t barY = 166, barH = 32;
        char status[48];
        std::strncpy(status, c.status, sizeof(status) - 1);
        status[sizeof(status) - 1] = '\0';
        fitText(status, static_cast<uint16_t>(W - 8));

        if (c.statusInverted)
        {
            g_epd.fillRect(0, barY, W, barH, GxEPD_BLACK);
            g_epd.setTextColor(GxEPD_WHITE);
        }
        printCentered(status, W / 2, barY + 22);
        g_epd.setTextColor(GxEPD_BLACK);
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

    // Relais-, Status- oder Override-Änderung: sofort. Nur Werte: gedrosselt.
    const bool important =
        !m_hasShown ||
        std::memcmp(next.relayOn, m_shown.relayOn, sizeof(next.relayOn)) != 0 ||
        std::memcmp(next.relayEnabled, m_shown.relayEnabled, sizeof(next.relayEnabled)) != 0 ||
        std::strcmp(next.status, m_shown.status) != 0 ||
        next.statusInverted != m_shown.statusInverted;

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
