/******************************************************************************
 * @file FieldCatalog.cpp
 *
 * Feld- und Codetabellen nach "VE.Direct Protocol" 3.34
 * (docs/VeDirect-Doku/Victron_VE.Direct-Protocol-3.34.pdf).
 *
 * Einheiten der ROHWERTE laut Doku -> angezeigte Einheit (decimals):
 *   mV        -> V    (3)        mA       -> A    (3)
 *   mAh       -> Ah   (3)        0,01 kWh -> kWh  (2)
 *   Promille  -> %    (1)        W, °C, min, s    (0)
 ******************************************************************************/

#include "FieldCatalog.hpp"

#include <cstring>

namespace
{

#define TABLE(entries) { entries, sizeof(entries) / sizeof(entries[0]) }

/******************************************************************************
 * CS – Betriebszustand (Doku: "CS")
 ******************************************************************************/
const CodeEntry ChargerStates[] =
{
    {   0, "Aus" },
    {   1, "Geringe Leistung (Lastsuche)" },
    {   2, "Fehler" },
    {   3, "Bulk (Konstantstrom)" },
    {   4, "Absorption" },
    {   5, "Float (Erhaltung)" },
    {   6, "Lagerung (Storage)" },
    {   7, "Ausgleichsladung (manuell)" },
    {   9, "Wechselrichterbetrieb" },
    {  11, "Netzteilbetrieb" },
    { 245, "Startet" },
    { 246, "Wiederholte Absorption" },
    { 247, "Auto-Ausgleich / Rekonditionierung" },
    { 248, "BatterySafe" },
    { 252, "Externe Steuerung" },
};
const CodeTable ChargerStateTable = TABLE(ChargerStates);

/******************************************************************************
 * ERR – Fehlercode (Doku: "ERR")
 ******************************************************************************/
const CodeEntry ErrorCodes[] =
{
    {   0, "Kein Fehler" },
    {   2, "Batteriespannung zu hoch" },
    {  17, "Temperatur Laderegler zu hoch" },
    {  18, "Überstrom Laderegler" },
    {  19, "Ladestrom umgekehrt" },
    {  20, "Bulk-Zeitlimit überschritten" },
    {  21, "Stromsensor-Problem" },
    {  26, "Anschlussklemmen überhitzt" },
    {  28, "Wandlerproblem (nur Doppelwandler)" },
    {  33, "PV-Eingangsspannung zu hoch" },
    {  34, "PV-Eingangsstrom zu hoch" },
    {  38, "Eingang abgeschaltet (Batteriespannung zu hoch)" },
    {  39, "Eingang abgeschaltet (Stromfluss im Aus-Zustand)" },
    {  65, "Kommunikation mit Gerät verloren" },
    {  66, "Konfigurationsproblem synchronisiertes Laden" },
    {  67, "BMS-Verbindung verloren" },
    {  68, "Netzwerk falsch konfiguriert" },
    { 116, "Werkskalibrierung verloren" },
    { 117, "Ungültige/inkompatible Firmware" },
    { 119, "Benutzereinstellungen ungültig" },
};
const CodeTable ErrorCodeTable = TABLE(ErrorCodes);

/******************************************************************************
 * MPPT – Tracker-Modus (Doku: "MPPT")
 ******************************************************************************/
const CodeEntry TrackerModes[] =
{
    { 0, "Aus" },
    { 1, "Spannungs- oder Strombegrenzung" },
    { 2, "MPP-Tracker aktiv" },
};
const CodeTable TrackerModeTable = TABLE(TrackerModes);

/******************************************************************************
 * MON – DC-Monitor-Modus (Doku: "MON")
 ******************************************************************************/
const CodeEntry MonitorModes[] =
{
    { -9, "Solarladegerät" },
    { -8, "Windgenerator" },
    { -7, "Wellengenerator" },
    { -6, "Lichtmaschine" },
    { -5, "Brennstoffzelle" },
    { -4, "Wassergenerator" },
    { -3, "DC/DC-Ladegerät" },
    { -2, "AC-Ladegerät" },
    { -1, "Allgemeine Quelle" },
    {  0, "Batteriemonitor" },
    {  1, "Allgemeine Last" },
    {  2, "Elektroantrieb" },
    {  3, "Kühlschrank" },
    {  4, "Wasserpumpe" },
    {  5, "Bilgepumpe" },
    {  6, "DC-System" },
    {  7, "Wechselrichter" },
    {  8, "Warmwasserbereiter" },
};
const CodeTable MonitorModeTable = TABLE(MonitorModes);

/******************************************************************************
 * AR – Alarmgrund, Bitmaske, dezimal übertragen (Doku: "AR")
 ******************************************************************************/
const CodeEntry AlarmReasons[] =
{
    {    1, "Unterspannung" },
    {    2, "Überspannung" },
    {    4, "Niedriger SOC" },
    {    8, "Unterspannung Starterbatterie" },
    {   16, "Überspannung Starterbatterie" },
    {   32, "Untertemperatur" },
    {   64, "Übertemperatur" },
    {  128, "Mittelpunktspannung" },
    {  256, "Überlast" },
    {  512, "DC-Welligkeit" },
    { 1024, "AC-Ausgangsspannung zu niedrig" },
    { 2048, "AC-Ausgangsspannung zu hoch" },
    { 4096, "Kurzschluss" },
    { 8192, "BMS-Sperre" },
};
const CodeTable AlarmReasonTable = TABLE(AlarmReasons);

/******************************************************************************
 * OR – Abschaltgrund, Bitmaske, hex übertragen (Doku: "OR")
 ******************************************************************************/
const CodeEntry OffReasons[] =
{
    { 0x00000001, "Keine Eingangsleistung" },
    { 0x00000002, "Ausgeschaltet (Schalter)" },
    { 0x00000004, "Ausgeschaltet (Gerätemodus-Register)" },
    { 0x00000008, "Fernsteuereingang" },
    { 0x00000010, "Schutzfunktion aktiv" },
    { 0x00000020, "Paygo" },
    { 0x00000040, "BMS" },
    { 0x00000080, "Motorabschaltung erkannt" },
    { 0x00000100, "Eingangsspannung wird analysiert" },
};
const CodeTable OffReasonTable = TABLE(OffReasons);

/******************************************************************************
 * CAP_BLE – Bluetooth-Fähigkeiten, Bitmaske, hex (Doku: "CAP_BLE")
 ******************************************************************************/
const CodeEntry BleCapabilities[] =
{
    { 0x00000001, "BLE abschaltbar" },
    { 0x00000002, "BLE-Abschaltung dauerhaft" },
};
const CodeTable BleCapabilityTable = TABLE(BleCapabilities);

#undef TABLE

/******************************************************************************
 * Feldkatalog
 *
 *  Label       Klarname                            Bedeutung                              Typ                       dec Einheit Codes
 ******************************************************************************/
using S = SemanticField;
using T = ValueType;

const FieldDefinition Catalog[] =
{
    // ---- Geräteinformationen ------------------------------------------------
    { "PID",     "Produkt-ID",                        S::ProductId,              T::ProductId,      0, "",    nullptr },
    { "FW",      "Firmware-Version",                  S::FirmwareVersion,        T::Firmware,       0, "",    nullptr },
    { "FWE",     "Firmware-Version (erweitert)",      S::FirmwareVersionExt,     T::FirmwareExt,    0, "",    nullptr },
    { "SER#",    "Seriennummer",                      S::SerialNumber,           T::Text,           0, "",    nullptr },
    { "BMV",     "Modellbezeichnung",                 S::ModelDescription,       T::Text,           0, "",    nullptr },

    // ---- Batterie -----------------------------------------------------------
    { "V",       "Batteriespannung",                  S::BatteryVoltage,         T::Number,         3, "V",   nullptr },
    { "VS",      "Spannung Starterbatterie",          S::AuxVoltage,             T::Number,         3, "V",   nullptr },
    { "VM",      "Mittelpunktspannung",               S::MidPointVoltage,        T::Number,         3, "V",   nullptr },
    { "DM",      "Mittelpunktabweichung",             S::MidPointDeviation,      T::Number,         1, "%",   nullptr },
    { "I",       "Batteriestrom",                     S::BatteryCurrent,         T::Number,         3, "A",   nullptr },
    { "P",       "Leistung",                          S::BatteryPower,           T::Number,         0, "W",   nullptr },
    { "T",       "Batterietemperatur",                S::BatteryTemperature,     T::Number,         0, "°C",  nullptr },
    { "CE",      "Verbrauchte Amperestunden",         S::ConsumedAmpHours,       T::Number,         3, "Ah",  nullptr },
    { "SOC",     "Ladezustand (SOC)",                 S::StateOfCharge,          T::Number,         1, "%",   nullptr },
    { "TTG",     "Restlaufzeit",                      S::TimeToGo,               T::TimeToGo,       0, "min", nullptr },

    // ---- Alarme / Relais ----------------------------------------------------
    { "Alarm",   "Alarm aktiv",                       S::AlarmActive,            T::OnOff,          0, "",    nullptr },
    { "Relay",   "Relais",                            S::RelayState,             T::OnOff,          0, "",    nullptr },
    { "AR",      "Alarmgrund",                        S::AlarmReason,            T::BitmaskDecimal, 0, "",    &AlarmReasonTable },
    { "MON",     "DC-Monitor-Modus",                  S::MonitorMode,            T::Code,           0, "",    &MonitorModeTable },

    // ---- Historie Batteriemonitor -------------------------------------------
    // Hinweis: Bei MON != 0 (DC-Monitor) bedeuten H17/H18 laut Doku
    // "erzeugte" bzw. "verbrauchte" Energie. Bei MON = 0 gilt der Name unten.
    { "H1",      "Tiefste Entladung",                 S::DepthDeepestDischarge,  T::Number,         3, "Ah",  nullptr },
    { "H2",      "Letzte Entladung",                  S::DepthLastDischarge,     T::Number,         3, "Ah",  nullptr },
    { "H3",      "Durchschnittliche Entladung",       S::DepthAverageDischarge,  T::Number,         3, "Ah",  nullptr },
    { "H4",      "Ladezyklen",                        S::ChargeCycles,           T::Number,         0, "",    nullptr },
    { "H5",      "Vollentladungen",                   S::FullDischarges,         T::Number,         0, "",    nullptr },
    { "H6",      "Kumulierte Amperestunden",          S::CumulativeAmpHours,     T::Number,         3, "Ah",  nullptr },
    { "H7",      "Minimale Batteriespannung",         S::MinimumBatteryVoltage,  T::Number,         3, "V",   nullptr },
    { "H8",      "Maximale Batteriespannung",         S::MaximumBatteryVoltage,  T::Number,         3, "V",   nullptr },
    { "H9",      "Sekunden seit Vollladung",          S::SecondsSinceFullCharge, T::Number,         0, "s",   nullptr },
    { "H10",     "Automatische Synchronisationen",    S::AutoSynchronizations,   T::Number,         0, "",    nullptr },
    { "H11",     "Alarme Unterspannung",              S::LowVoltageAlarms,       T::Number,         0, "",    nullptr },
    { "H12",     "Alarme Überspannung",               S::HighVoltageAlarms,      T::Number,         0, "",    nullptr },
    { "H13",     "Alarme Unterspannung Hilfseingang", S::LowAuxVoltageAlarms,    T::Number,         0, "",    nullptr },
    { "H14",     "Alarme Überspannung Hilfseingang",  S::HighAuxVoltageAlarms,   T::Number,         0, "",    nullptr },
    { "H15",     "Minimale Spannung Hilfseingang",    S::MinimumAuxVoltage,      T::Number,         3, "V",   nullptr },
    { "H16",     "Maximale Spannung Hilfseingang",    S::MaximumAuxVoltage,      T::Number,         3, "V",   nullptr },
    { "H17",     "Entladene Energie",                 S::DischargedEnergy,       T::Number,         2, "kWh", nullptr },
    { "H18",     "Geladene Energie",                  S::ChargedEnergy,          T::Number,         2, "kWh", nullptr },

    // ---- Solarladeregler ----------------------------------------------------
    { "VPV",     "PV-Spannung",                       S::PvVoltage,              T::Number,         3, "V",   nullptr },
    { "PPV",     "PV-Leistung",                       S::PvPower,                T::Number,         0, "W",   nullptr },
    { "IL",      "Laststrom",                         S::LoadCurrent,            T::Number,         3, "A",   nullptr },
    { "LOAD",    "Lastausgang",                       S::LoadOutput,             T::OnOff,          0, "",    nullptr },
    { "CS",      "Betriebszustand",                   S::ChargerState,           T::Code,           0, "",    &ChargerStateTable },
    { "MPPT",    "Tracker-Modus",                     S::TrackerMode,            T::Code,           0, "",    &TrackerModeTable },
    { "ERR",     "Fehlercode",                        S::ErrorCode,              T::Code,           0, "",    &ErrorCodeTable },
    { "OR",      "Abschaltgrund",                     S::OffReason,              T::BitmaskHex,     0, "",    &OffReasonTable },
    { "CAP_BLE", "Bluetooth-Fähigkeiten",             S::BleCapabilities,        T::BitmaskHex,     0, "",    &BleCapabilityTable },
    { "H19",     "Ertrag gesamt",                     S::YieldTotal,             T::Number,         2, "kWh", nullptr },
    { "H20",     "Ertrag heute",                      S::YieldToday,             T::Number,         2, "kWh", nullptr },
    { "H21",     "Maximale Leistung heute",           S::MaximumPowerToday,      T::Number,         0, "W",   nullptr },
    { "H22",     "Ertrag gestern",                    S::YieldYesterday,         T::Number,         2, "kWh", nullptr },
    { "H23",     "Maximale Leistung gestern",         S::MaximumPowerYesterday,  T::Number,         0, "W",   nullptr },
    { "HSDS",    "Tageszähler",                       S::DaySequence,            T::Number,         0, "",    nullptr },
};

constexpr std::size_t CatalogSize = sizeof(Catalog) / sizeof(Catalog[0]);

} // namespace

/******************************************************************************
 * Öffentliche Funktionen
 ******************************************************************************/
const FieldDefinition* FieldCatalog::find(const char* label)
{
    if (label == nullptr)
    {
        return nullptr;
    }

    // Labels sind laut Doku exakt definiert ("Alarm", "Relay" gemischt
    // geschrieben) -> exakter Vergleich.
    for (std::size_t i = 0; i < CatalogSize; ++i)
    {
        if (std::strcmp(Catalog[i].label, label) == 0)
        {
            return &Catalog[i];
        }
    }
    return nullptr;
}

std::size_t FieldCatalog::size()
{
    return CatalogSize;
}

const FieldDefinition& FieldCatalog::at(std::size_t index)
{
    return Catalog[index];
}

const char* FieldCatalog::codeText(const FieldDefinition& definition, int64_t code)
{
    const CodeTable* table = definition.codes;
    if (table == nullptr)
    {
        return nullptr;
    }
    for (std::size_t i = 0; i < table->count; ++i)
    {
        if (table->entries[i].code == code)
        {
            return table->entries[i].text;
        }
    }
    return nullptr;
}
