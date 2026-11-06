/******************************************************************************
 * @file SystemState.hpp
 *
 * Gesamtzustand der Anlage, berechnet aus den Kanalzuständen:
 *   - Rollen: Batterie (Batteriemonitor = SmartShunt/BMV) und PV (MPPT),
 *     zugeordnet nach GERÄTEKATEGORIE, nicht nach UART.
 *   - Messwerte für Anzeige und Steuerung, nur wenn frisch und gültig.
 *   - Fehlerwort (32-Bit-Bitmaske) mit Schwere je Fehler.
 *
 * evaluate() ist eine reine Funktion: gleiche Eingaben -> gleiches Ergebnis.
 * Auf dem PC testbar und in Stufe 4b Grundlage der Abschaltlogik.
 *
 * Entscheidungen des Projektinhabers (06.11.2026):
 *   - SmartShunt ist Pflicht, MPPT optional (beides konfigurierbar).
 *   - Zwei Batteriemonitore: keine Steuerung/Anzeige, wie Ausfall Shunt.
 *   - Fehlt der Shunt, darf die Batteriespannung vom MPPT übernommen
 *     werden (gekennzeichnet, source == PvCharger). In 4b darf dann das
 *     Wechselrichter-Relais NICHT schalten; Kleinverbraucher dürfen
 *     weiterlaufen.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include "DeviceState.hpp"
#include "LinkMonitor.hpp"

/******************************************************************************
 * Fehlerbits. Werte bleiben fest (später in Logs/WebUI/MQTT sichtbar).
 ******************************************************************************/
namespace SystemFault
{
    constexpr uint32_t BatteryMonitorMissing   = 1u << 0; ///< kein Shunt erkannt/verloren
    constexpr uint32_t MultipleBatteryMonitors = 1u << 1; ///< Zuordnung unklar
    constexpr uint32_t SocNotAvailable         = 1u << 2; ///< kein gültiger, frischer SOC
    constexpr uint32_t BatteryAlarm            = 1u << 3; ///< Alarm = ON oder AR != 0
    constexpr uint32_t BatteryLinkUnstable     = 1u << 4; ///< Verbindung zum Shunt instabil
    constexpr uint32_t PvChargerMissing        = 1u << 5; ///< kein MPPT erkannt/verloren
    constexpr uint32_t MultiplePvChargers      = 1u << 6; ///< Zuordnung unklar
    constexpr uint32_t PvChargerError          = 1u << 7; ///< ERR != 0
    constexpr uint32_t PvLinkUnstable          = 1u << 8; ///< Verbindung zum MPPT instabil
    constexpr uint32_t UnknownDevice           = 1u << 9; ///< Gerät mit unbekannter PID

    constexpr unsigned BitCount = 10;   ///< Anzahl belegter Bits
}

enum class Severity : uint8_t
{
    None = 0,
    Info,
    Warning,
    Critical
};

/// Herkunft eines Messwerts.
enum class ValueSource : uint8_t
{
    None,            ///< kein gültiger Wert
    BatteryMonitor,  ///< vom SmartShunt/BMV (Normalfall)
    PvCharger        ///< vom MPPT (Ersatz, wenn der Shunt fehlt)
};

/// Ein Messwert mit Gültigkeit und Herkunft.
struct Reading
{
    bool        valid  = false;
    float       value  = 0.0f;
    ValueSource source = ValueSource::None;
};

/// Einstellungen für evaluate() (Kopie aus AppConfig).
struct SystemConfig
{
    bool     requireBatteryMonitor = true;   ///< Shunt Pflicht
    bool     requirePvCharger      = false;  ///< MPPT optional
    uint32_t valueMaxAgeMs         = 5000;   ///< max. Alter frischer Werte
};

/// Eingabe für evaluate(): ein Kanal.
struct ChannelView
{
    const DeviceState* state = nullptr;
    const LinkMonitor* link  = nullptr;
};

/// Ergebnis von evaluate().
struct SystemState
{
    uint32_t faults   = 0;               ///< Bitmaske SystemFault::*
    Severity severity = Severity::None;  ///< höchste Schwere aller Fehler

    int batteryChannel = -1;             ///< Index des Shunt-Kanals, -1 = keiner
    int pvChannel      = -1;             ///< Index des MPPT-Kanals, -1 = keiner

    // ---- Batterie (vom Shunt; Spannung ersatzweise vom MPPT) ----
    Reading batteryVoltage;              ///< V
    Reading batteryCurrent;              ///< A (nur vom Shunt)
    Reading batteryPower;                ///< W
    Reading stateOfCharge;               ///< %
    Reading consumedAmpHours;            ///< Ah
    Reading timeToGo;                    ///< min
    bool    timeToGoInfinite = false;    ///< TTG = -1 (keine Entladung)

    // ---- PV (vom MPPT) ----
    Reading     pvPower;                 ///< W
    Reading     pvVoltage;               ///< V
    Reading     yieldToday;              ///< kWh
    int32_t     chargerState     = -1;   ///< CS-Code, -1 = ungültig
    const char* chargerStateText = nullptr;
    int32_t     pvErrorCode      = -1;   ///< ERR, -1 = ungültig
};

class SystemEvaluator
{
public:

    /// Gesamtzustand berechnen. nowMs NACH dem Kopieren der Kanäle lesen!
    static SystemState evaluate(const ChannelView* channels, std::size_t count,
                                const SystemConfig& config, uint32_t nowMs);

    /// Schwere eines einzelnen Fehlerbits (abhängig von der Konfiguration).
    static Severity severityOf(uint32_t faultBit, const SystemConfig& config);

    /// Deutscher Kurztext eines Fehlerbits.
    static const char* faultText(uint32_t faultBit);

    /// Text zur Schwere ("KRITISCH", "Warnung", "Info", "").
    static const char* severityText(Severity severity);
};
