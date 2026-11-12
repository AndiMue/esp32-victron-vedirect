/******************************************************************************
 * @file main.cpp
 *
 * ESP32 Victron VE.Direct
 *   Stufe 1: prüfsummengesicherter Empfang
 *   Stufe 2: Geräteerkennung pro Kanal (DeviceDetector)
 *   Stufe 3: Klarnamen und physikalische Werte
 *
 *   Stufe 4a: Gerätezustand, Verbindungsüberwachung, Systemzustand, Fehlerwort
 *
 * Aufbau (FreeRTOS-Tasks):
 *
 *   UART1 ─► Reader ─► Parser ─► DeviceDetector ─┐   uartTask (Core 0)
 *                                                  ├─► Queue ─► loggerTask ─► Serial (UART0)
 *   UART2 ─► Reader ─► Parser ─► DeviceDetector ─┘   uartTask (Core 1)
 *
 *   Zusätzlich je Kanal: DeviceState (Werte) + LinkMonitor (Verbindung),
 *   geschützt durch einen Mutex. Die systemTask (1×/s) kopiert beide Kanäle
 *   und berechnet daraus SystemState + Fehlerwort (SystemEvaluator).
 *
 * Grundregeln:
 *   - NUR die loggerTask schreibt auf Serial (UART0). Die UART-Tasks geben
 *     nie selbst etwas aus. So können sich die Ausgaben beider Kanäle nicht
 *     vermischen, und eine blockierende Debug-Ausgabe bremst den
 *     VE.Direct-Empfang nicht aus.
 *   - Ausgegeben werden ausschließlich Blöcke mit gültiger Prüfsumme.
 *   - Die Kanäle heißen bewusst "UART1"/"UART2" und nicht "MPPT"/"Shunt":
 *     Jedes Gerät funktioniert an jedem UART, die Erkennung erfolgt per PID.
 *
 * Pinbelegung (bestätigt):
 *   UART1: RX GPIO16, TX GPIO17
 *   UART2: RX GPIO26, TX GPIO33
 ******************************************************************************/

#include <Arduino.h>

#include <cstdarg>
#include <cstring>

#include "esp_system.h"     // esp_reset_reason()
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <LittleFS.h>

#include "BuildVersion.h"   // FW_VERSION, erzeugt von tools/build_version.py
#include "MqttManager.hpp"
#include "SettingsStore.hpp"
#include "RelayDriver.hpp"
#include "StatusDisplay.hpp"
#include "UpdateManager.hpp"
#include "WebUi.hpp"
#include "WifiManager.hpp"

#include <Wire.h>
#include <sys/time.h>
#include "esp_sntp.h"       // W4e: NTP (im ESP32-Framework enthalten)
#include "Pcf8523.hpp"      // W4e: RTC-Minitreiber (lib/RtcPcf8523)
#include "RtcZones.hpp"

#include "AppConfig.hpp"
#include "DeviceCatalog.hpp"
#include "DeviceDetector.hpp"
#include "DeviceState.hpp"
#include "LinkMonitor.hpp"
#include "EventLog.hpp"
#include "FieldCatalog.hpp"
#include "SettingsBackup.hpp"
#include "LoadController.hpp"
#include "SystemState.hpp"
#include "ValueConverter.hpp"
#include "ValueFormatter.hpp"
#include "VeDirectFrame.hpp"
#include "VeDirectHex.hpp"
#include "VeDirectHexReader.hpp"
#include "VeDirectReader.hpp"
#include "VeDirectTextParser.hpp"

/******************************************************************************
 * Zentrale Einstellungen (siehe AppConfig.hpp). Später per WebUI änderbar.
 ******************************************************************************/
AppConfig g_config;

namespace
{

/******************************************************************************
 * Feste Einstellungen
 ******************************************************************************/
constexpr uint32_t DebugBaudrate     = 115200; ///< UART0 / USB-Monitor
constexpr uint32_t StatusIntervalMs  = 5000;   ///< Statuszeile alle 5 s
constexpr UBaseType_t LogQueueLength = 8;      ///< Puffer für Logger-Nachrichten
constexpr UBaseType_t CommandQueueLength = 4;  ///< Schaltbefehle an die systemTask

constexpr uint32_t SystemIntervalMs = 1000;   ///< Auswertung des Systemzustands

/// Watchdog: Reagiert die systemTask oder eine UART-Task so lange nicht,
/// startet der ESP32 neu -> GPIOs hochohmig -> alle Relais AUS.
constexpr uint32_t WatchdogTimeoutS = 10;

/// WebUI (Stufe W1)
const char* const DeviceName      = "VE.Direct Steuergerät";
const char* const FirmwareVersion = FW_VERSION;   ///< JJ.MM.TT-HHMM des Builds
const char* const DefaultHostname = "ve-direct";   ///< http://ve-direct.local
const char* const ApPrefix        = "VE-Direct";   ///< AP-Name: VE-Direct-XXXX
constexpr uint32_t WebTaskStack   = 8192;
constexpr UBaseType_t WebTaskPriority = 1;

/// Netzwerk-Reset-Taste (Pinplan: Reserve-Eingang IO35, reiner Eingang ohne
/// internen Pull-up -> externer 10-kΩ-Pull-up nach 3,3 V nötig, Taste nach GND).
/// Beim Einschalten ResetHoldMs gedrückt halten und dann loslassen ->
/// WLAN + Gerätepasswort werden zurückgesetzt (neues Startpasswort, siehe
/// Display). Wird nicht losgelassen (Pin dauerhaft Low): kein Reset.
constexpr uint8_t  ResetButtonPin       = 35;
constexpr uint32_t ResetHoldMs          = 10000;
constexpr uint32_t ResetReleaseWindowMs = 5000;

/// Relais-Anschlüsse (Pinplan 06.11.2026), Module aktiv Low.
const RelayPin RelayPins[RelayCount] =
{
    { 25, true },   // R1 Wechselrichter / DC-DC
    { 27, true },   // R2 Licht
    { 32, true },   // R3 Kühlschrank
};

// W4a: UART- und System-Task von 4 auf 6 KB, da sie jetzt über addEvent()
// vsnprintf aufrufen (braucht selbst gut 1 KB Stack)
constexpr uint32_t UartTaskStack    = 6144;
constexpr uint32_t SystemTaskStack  = 6144;
constexpr uint32_t DisplayTaskStack = 6144;   ///< Grafik/Schriften
constexpr uint32_t LoggerTaskStack = 6144;   ///< printf/snprintf mit Formatierung braucht etwas mehr

/// Spaltenbreiten der Feldausgabe (in Zeichen, nicht Bytes – Umlaute!)
constexpr size_t NameColumnWidth  = 34;
constexpr size_t ValueColumnWidth = 32;

/// UART-Tasks haben höhere Priorität als der Logger: Empfang geht vor Ausgabe.
constexpr UBaseType_t UartTaskPriority   = 2;
constexpr UBaseType_t SystemTaskPriority = 1;
constexpr UBaseType_t DisplayTaskPriority = 1;
constexpr UBaseType_t LoggerTaskPriority = 1;

/******************************************************************************
 * Diagnosedaten eines Kanals.
 *
 * Wird von der UART-Task geschrieben und von der Logger-Task gelesen.
 * Zugriff daher nur innerhalb von portENTER_CRITICAL(&g_statusLock).
 ******************************************************************************/
struct ChannelStatus
{
    VeDirectTextParser::Statistics parser;    ///< Zähler des Parsers
    DeviceDetector::Identity       device;    ///< aktuell erkanntes Gerät
    DeviceDetector::Statistics     detector;  ///< Erkennungen, Ausfälle ...
    uint32_t lastValidFrameMs = 0;            ///< millis() des letzten gültigen Blocks
    bool     hasValidFrame    = false;        ///< mindestens ein gültiger Block?
    uint32_t queueDrops       = 0;            ///< Nachrichten verloren (Queue voll)
    bool     linkUnstable     = false;        ///< LinkMonitor: Verbindung instabil?
    VeHex::VeDirectHexParser::Statistics hex; ///< W4b: HEX-Empfang
    uint32_t hexRequests      = 0;            ///< W4b: gesendete HEX-Anfragen
    HexJobRunner::Result hexJob;              ///< W4b: letzte/laufende automatische Abfrage
    bool     hexBusy          = false;        ///< W4b: Abfrage läuft gerade
};

/******************************************************************************
 * Nachricht von einer UART-Task an die Logger-Task.
 ******************************************************************************/
enum class LogKind : uint8_t
{
    Frame,            ///< gültiger Block, "frame" und "origin" sind gefüllt
    ChecksumError,    ///< Block mit falscher Prüfsumme verworfen
    FrameDiscarded,   ///< Block mit Format-/Größenfehler verworfen
    DeviceIdentified, ///< Gerät erkannt (erstmals oder nach Ausfall)
    DeviceChanged,    ///< anderes Gerät am Kanal ("previousDevice" = altes)
    DeviceLost,       ///< Timeout ("previousDevice" = verlorenes Gerät)
    HexSent,          ///< W4b: HEX-Anfrage gesendet ("hexRequest")
    HexMessage,       ///< W4b: gültige HEX-Nachricht ("hex")
    HexError,         ///< W4b: HEX-Zeile mit Prüfsummen- oder Formatfehler
    HexJobDone        ///< W4b: automatische Abfrage beendet (status.hexJob)
};

struct LogMessage
{
    LogKind                  kind = LogKind::Frame;
    uint8_t                  channelIndex = 0;
    uint32_t                 timestampMs = 0;
    ChannelStatus            status;          ///< Stand zum Zeitpunkt der Nachricht
    DeviceDetector::Origin   origin = DeviceDetector::Origin::Unassigned;
    DeviceDetector::Identity previousDevice;  ///< nur bei DeviceChanged/DeviceLost
    VeDirectFrame            frame;           ///< nur bei LogKind::Frame gültig
    VeHex::Message           hex;             ///< W4b: nur bei LogKind::HexMessage
    VeHex::Request           hexRequest;      ///< W4b: nur bei LogKind::HexSent
    int32_t                  hexAfterMs = -1; ///< W4b: Zeit seit der letzten Anfrage (-1 = keine)
};

/******************************************************************************
 * Ein VE.Direct-Kanal: UART + Parser + Geräteerkennung + Status.
 ******************************************************************************/
struct Channel
{
    Channel(uint8_t index, const char* name,
            HardwareSerial& serial, uint8_t rxPin, uint8_t txPin)
        : index(index), name(name), reader(serial, rxPin, txPin)
    {
    }

    const uint8_t       index;
    const char* const   name;
    VeDirectReader      reader;
    VeDirectTextParser  parser;     ///< nur von der eigenen UART-Task benutzt
    DeviceDetector      detector;   ///< nur von der eigenen UART-Task benutzt
    ChannelStatus       status;     ///< geschützt durch g_statusLock
    LogMessage          outgoing;   ///< Sendepuffer der UART-Task
                                    ///< (global statt auf dem Stack, ~1,8 KB)

    // ---- W4b: HEX-Protokoll (nur lesen) ----
    VeHex::VeDirectHexParser hexParser;     ///< nur von der eigenen UART-Task benutzt
    VeHex::Request      hexPending;         ///< nächste Anfrage, geschützt durch g_statusLock
    bool                hexPendingSet = false;   ///< geschützt durch g_statusLock
    uint32_t            hexSentMs     = 0;  ///< nur UART-Task
    bool                hexSent       = false;   ///< nur UART-Task

    // ---- W4b Schritt 3: automatische Abfragen ----
    HexRegisterCache    hexCache;                ///< Antworten, geschützt durch stateMutex (~2 KB)
    HexJobRunner        hexJob;                  ///< nur UART-Task
    bool                hexAutoPending = false;  ///< nur UART-Task: nach Erkennung lesen
    uint32_t            hexAutoAtMs   = 0;       ///< nur UART-Task: frühester Zeitpunkt dafür
    bool                hexRefreshRequested = false;   ///< geschützt durch g_statusLock
    uint32_t            hexLastRxMs   = 0;       ///< nur UART-Task: letzte gültige HEX-Nachricht
    uint32_t            hexJobEndMs   = 0;       ///< nur UART-Task

    // ---- Stufe 4a: geschützt durch stateMutex ----
    DeviceState         deviceState;            ///< Werte des Geräts (~5 KB)
    LinkMonitor         link;                   ///< Verbindungsqualität
    SemaphoreHandle_t   stateMutex = nullptr;   ///< in setup() angelegt
};

/******************************************************************************
 * RAII-Sperre für deviceState/link eines Kanals.
 * Mutex statt Spinlock, da hier einige KB kopiert werden.
 ******************************************************************************/
class StateLock
{
public:
    explicit StateLock(SemaphoreHandle_t mutex) : m_mutex(mutex)
    {
        xSemaphoreTake(m_mutex, portMAX_DELAY);
    }
    ~StateLock()
    {
        xSemaphoreGive(m_mutex);
    }
    StateLock(const StateLock&) = delete;
    StateLock& operator=(const StateLock&) = delete;

private:
    SemaphoreHandle_t m_mutex;
};

/******************************************************************************
 * Globale Objekte
 ******************************************************************************/
HardwareSerial Uart1(1);
HardwareSerial Uart2(2);

Channel g_channels[] =
{
    Channel(0, "UART1", Uart1, 16, 17),
    Channel(1, "UART2", Uart2, 26, 33),
};

constexpr size_t ChannelCount = sizeof(g_channels) / sizeof(g_channels[0]);

QueueHandle_t g_logQueue = nullptr;

/// Schützt alle ChannelStatus-Strukturen (sehr kurze Kopiervorgänge).
portMUX_TYPE g_statusLock = portMUX_INITIALIZER_UNLOCKED;

/******************************************************************************
 * W4a Schritt 4: Ereignisliste (Reiter "Fehler" der WebUI)
 *
 * Wird aus mehreren Tasks beschrieben (UART-Tasks: Geräte erkannt/
 * ausgefallen; systemTask: Relais, Befehle, Fehler) und von der webTask
 * gelesen -> eigener Spinlock, nur kurze Kopien darin.
 ******************************************************************************/
EventLog     g_events;
portMUX_TYPE g_eventLock = portMUX_INITIALIZER_UNLOCKED;

/// Ereignis eintragen (printf-Format, Text wird ggf. gekürzt).
void addEvent(Severity severity, const char* format, ...)
{
    char text[EventLog::TextSize];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    const uint32_t now = millis();
    portENTER_CRITICAL(&g_eventLock);
    g_events.add(now, severity, text);
    portEXIT_CRITICAL(&g_eventLock);
}

/// Grund des letzten Neustarts (esp_reset_reason) als Klartext + Schwere.
const char* resetReasonText(esp_reset_reason_t reason, Severity& severity)
{
    severity = Severity::Info;
    switch (reason)
    {
        case ESP_RST_POWERON:   return "Einschalten (Stromversorgung)";
        case ESP_RST_EXT:       return "Reset von außen (EN-Taste)";
        case ESP_RST_SW:        return "Neustart durch die Software (z. B. Update, Neustart-Knopf)";
        case ESP_RST_DEEPSLEEP: return "Aufwachen aus dem Tiefschlaf";
        case ESP_RST_SDIO:      return "Reset über SDIO";
        default: break;
    }
    severity = Severity::Critical;
    switch (reason)
    {
        case ESP_RST_PANIC:     return "Absturz (Software-Fehler)";
        case ESP_RST_INT_WDT:   return "Watchdog: Programm hing (Interrupt)";
        case ESP_RST_TASK_WDT:  return "Watchdog: Programm hing (Task)";
        case ESP_RST_WDT:       return "Watchdog: Programm hing";
        case ESP_RST_BROWNOUT:  return "Unterspannung der Versorgung (Brownout)";
        default:                return "unbekannt";
    }
}

esp_reset_reason_t g_resetReason = ESP_RST_UNKNOWN;   ///< in setup() gesetzt

/******************************************************************************
 * W4e: Echtzeituhr. Vorerst nur in setup() und der Logger-Task (Monitor-
 * Befehl "uhr") benutzt; I²C ist nicht threadsicher.
 ******************************************************************************/
constexpr uint8_t RtcSdaPin = 21;
constexpr uint8_t RtcSclPin = 22;
Pcf8523 g_rtc(Wire);
bool    g_rtcFound = false;

/******************************************************************************
 * W4e Schritt 3: Uhrdienst (Projektinhaber 11.11.2026)
 *
 *   - Systemuhr des ESP32 läuft in UTC; Anzeige über die Zeitzone (TZ).
 *   - Stellen: beim Start aus der RTC; NTP nach WLAN-Verbindung, dann 1×/Tag;
 *     Browserzeit NUR, wenn noch keine gültige Zeit vorhanden ist.
 *   - Nach NTP/Browser wird die RTC nachgestellt.
 *   - I²C (RTC) wird aus mehreren Tasks benutzt -> g_i2cMutex.
 ******************************************************************************/
const char* const NvsClock = "clock";

enum class ClockSource : uint8_t { None = 0, Rtc = 1, Ntp = 2, Browser = 3 };

SemaphoreHandle_t     g_i2cMutex = nullptr;          ///< in setup() angelegt
std::atomic<uint8_t>  g_clockSource{0};               ///< ClockSource
std::atomic<uint32_t> g_clockSetAtUnix{0};            ///< letzte Stellung (UTC)
std::atomic<bool>     g_ntpSyncPending{false};        ///< SNTP-Callback -> webTask
char   g_ntpServer[64] = "pool.ntp.org";  ///< SNTP merkt sich nur den Zeiger -> fester Puffer
String g_tzKey         = "berlin";        ///< nur setup()/webTask

constexpr uint32_t ClockValidAfterUnix = 1704067200UL;   ///< 2024-01-01: davor "nicht gestellt"
constexpr uint32_t NtpIntervalMs       = 24UL * 3600UL * 1000UL;   ///< 1× pro Tag

/// I²C-Sperre (RAII). Wartet höchstens 200 ms; ok() = Sperre erhalten.
class I2cLock
{
public:
    I2cLock() : m_ok(g_i2cMutex != nullptr && xSemaphoreTake(g_i2cMutex, pdMS_TO_TICKS(200)) == pdTRUE) {}
    ~I2cLock() { if (m_ok) xSemaphoreGive(g_i2cMutex); }
    bool ok() const { return m_ok; }
    I2cLock(const I2cLock&) = delete;
    I2cLock& operator=(const I2cLock&) = delete;
private:
    bool m_ok;
};

uint32_t clockNowUnix()  { return static_cast<uint32_t>(time(nullptr)); }
bool     clockValid()    { return clockNowUnix() >= ClockValidAfterUnix; }

const char* clockSourceKey(ClockSource s)
{
    switch (s)
    {
        case ClockSource::Rtc:     return "rtc";
        case ClockSource::Ntp:     return "ntp";
        case ClockSource::Browser: return "browser";
        default:                   return "none";
    }
}

/// Systemuhr stellen (UTC) und Quelle merken.
void setSystemClock(uint32_t unix, ClockSource source)
{
    struct timeval tv;
    tv.tv_sec  = static_cast<time_t>(unix);
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    g_clockSource.store(static_cast<uint8_t>(source));
    g_clockSetAtUnix.store(unix);
}

/// RTC auf die aktuelle Systemzeit stellen (nach NTP/Browser).
bool writeRtcFromSystem()
{
    if (!g_rtcFound)
    {
        return false;
    }
    Rtc::DateTime t;
    if (!Rtc::fromUnix(clockNowUnix(), t))
    {
        return false;
    }
    I2cLock lock;
    return lock.ok() && g_rtc.write(t);
}

/// Zeitzone anwenden (POSIX-Regel in die Umgebung der C-Bibliothek).
void applyTimeZone()
{
    setenv("TZ", Rtc::timeZone(g_tzKey.c_str()).posix, 1);
    tzset();
}

/// Einstellungen laden (setup(), vor dem Start der Tasks).
void loadClockSettings()
{
    g_tzKey = SettingsStore::getString(NvsClock, "tz", "berlin");
    const String server = SettingsStore::getString(NvsClock, "ntp", "pool.ntp.org");
    if (Rtc::isValidHostName(server.c_str()))
    {
        std::strncpy(g_ntpServer, server.c_str(), sizeof(g_ntpServer) - 1);
        g_ntpServer[sizeof(g_ntpServer) - 1] = '\0';
    }
    applyTimeZone();
}

/// SNTP meldet einen erfolgreichen Abgleich (läuft in der lwIP-Task:
/// nur Merker setzen, kein I²C hier).
void onNtpSync(struct timeval*)
{
    g_ntpSyncPending.store(true);
}

/// NTP (neu) starten. Nur aus der webTask, nachdem das WLAN gestartet ist.
void startNtp()
{
    sntp_stop();   // Zeiger auf g_ntpServer darf beim Ändern nicht benutzt werden
    sntp_set_time_sync_notification_cb(onNtpSync);
    sntp_set_sync_interval(NtpIntervalMs);
    configTzTime(Rtc::timeZone(g_tzKey.c_str()).posix, g_ntpServer);
}

/// In der webTask-Schleife: NTP-Ergebnis übernehmen (RTC nachstellen, Ereignis).
void clockLoop()
{
    if (!g_ntpSyncPending.exchange(false))
    {
        return;
    }
    const ClockSource before = static_cast<ClockSource>(g_clockSource.load());
    g_clockSource.store(static_cast<uint8_t>(ClockSource::Ntp));
    g_clockSetAtUnix.store(clockNowUnix());
    const bool rtcOk = writeRtcFromSystem();
    if (before != ClockSource::Ntp)
    {
        // Nur beim Wechsel der Quelle melden, nicht bei jedem täglichen Abgleich
        addEvent(Severity::Info, "Uhrzeit per NTP gestellt (%s)%s", g_ntpServer,
                 g_rtcFound ? (rtcOk ? ", RTC nachgestellt" : ", RTC-Schreiben fehlgeschlagen") : "");
    }
}

/// Browserzeit übernehmen – nur, wenn noch keine gültige Zeit vorhanden ist.
/// true = übernommen.
bool setClockFromBrowser(uint32_t unix)
{
    if (clockValid() || unix < ClockValidAfterUnix)
    {
        return false;
    }
    setSystemClock(unix, ClockSource::Browser);
    const bool rtcOk = writeRtcFromSystem();
    addEvent(Severity::Info, "Uhrzeit vom Browser übernommen (kein NTP/RTC)%s",
             g_rtcFound ? (rtcOk ? ", RTC gestellt" : ", RTC-Schreiben fehlgeschlagen") : "");
    return true;
}

/// Lokale Zeit als "TT.MM.JJJJ hh:mm:ss" (leer, wenn ungültig).
String formatLocalTime(uint32_t unix)
{
    if (unix < ClockValidAfterUnix)
    {
        return String();
    }
    const time_t t = static_cast<time_t>(unix);
    struct tm lt;
    localtime_r(&t, &lt);
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%02d.%02d.%04d %02d:%02d:%02d", lt.tm_mday, lt.tm_mon + 1,
                  lt.tm_year + 1900, lt.tm_hour, lt.tm_min, lt.tm_sec);
    return String(buf);
}

/// Empfangspuffer der Logger-Task (global statt auf dem Stack).
LogMessage g_incoming;

/// Kopien der Kanalzustände für die systemTask (global statt auf dem Stack).
DeviceState g_stateSnapshot[ChannelCount];
LinkMonitor g_linkSnapshot[ChannelCount];

/// Zuletzt berechneter Systemzustand, geschützt durch g_statusLock
/// (klein, Kopie in Mikrosekunden).
SystemState g_systemState;

SystemState readSystemState()
{
    portENTER_CRITICAL(&g_statusLock);
    const SystemState copy = g_systemState;
    portEXIT_CRITICAL(&g_statusLock);
    return copy;
}

/******************************************************************************
 * Stufe 4b: Abschaltlogik
 *
 * Befehle (Taster/Touch, WebUI, MQTT – vorerst der serielle Monitor) werden
 * per Queue an die systemTask geschickt. Nur sie verändert den
 * LoadController. Ergebnis und Relais-Sollzustände stellt sie als
 * ControlSnapshot bereit (geschützt durch g_statusLock).
 ******************************************************************************/
enum class CommandKind : uint8_t
{
    Relay,      ///< Relais ein/aus
    Override,   ///< Override ein/aus
    Limits,     ///< W4a: änderbare Werte eines Relais setzen (WebUI)
    ResetLimits ///< W4a: Relais auf die Voreinstellung der Anlage (alle oder eins, siehe allRelays)
};

struct ControlCommand
{
    CommandKind   kind   = CommandKind::Relay;
    RelayId       relay  = RelayId::Main;
    bool          on     = false;
    CommandSource source = CommandSource::Local;
    uint32_t      ticket = 0;   ///< W4a: laufende Nummer (WebUI wartet auf genau diesen Befehl), 0 = keine
    RelayLimits   limits;       ///< W4a: nur bei CommandKind::Limits
    bool          allRelays = true;   ///< W4a: ResetLimits für alle Relais (sonst nur "relay")
};

struct ControlSnapshot
{
    RelayStatus    relays[RelayCount];
    const char*    names[RelayCount]   = {};
    const char*    shortNames[RelayCount] = {};
    bool           enabled[RelayCount] = {};
    RelayConfig    configs[RelayCount];        ///< W4a: Schwellen für die Anzeige (WebUI)
    bool           overrideActive      = false;
    uint32_t       commandCount        = 0;    ///< bearbeitete Befehle
    ControlCommand lastCommand;
    CommandResult  lastResult          = CommandResult::Accepted;
};

QueueHandle_t   g_commandQueue = nullptr;
LoadController  g_loadController;      ///< nur in der systemTask verwenden!
RelayDriver     g_relays(RelayPins);   ///< nur in setup() und der systemTask verwenden!
ControlSnapshot g_control;             ///< geschützt durch g_statusLock

/******************************************************************************
 * Stufe W1: WLAN und WebUI (nur in der webTask verwenden!)
 ******************************************************************************/
WifiManager g_wifi;
WebUi       g_webUi;
MqttManager g_mqtt;   ///< W3: loop/publish nur in der mqttTask; Konfiguration/Status threadsicher
NetStatus   g_netStatus;   ///< Kopie für Logger/Display, geschützt durch g_statusLock

NetStatus readNetStatus()
{
    portENTER_CRITICAL(&g_statusLock);
    const NetStatus copy = g_netStatus;
    portEXIT_CRITICAL(&g_statusLock);
    return copy;
}

ControlSnapshot readControl()
{
    portENTER_CRITICAL(&g_statusLock);
    const ControlSnapshot copy = g_control;
    portEXIT_CRITICAL(&g_statusLock);
    return copy;
}

/// Aktuelle Einstellungen der Verbindungsüberwachung aus AppConfig.
LinkMonitor::Config makeLinkConfig()
{
    LinkMonitor::Config config;
    config.errorThreshold  = g_config.linkErrorThreshold.load();
    config.outageThreshold = g_config.linkOutageThreshold.load();
    config.recoveryMs      = g_config.linkRecoveryMs.load();
    return config;
}

/// Aktuelle Einstellungen für die Systemauswertung aus AppConfig.
SystemConfig makeSystemConfig()
{
    SystemConfig config;
    config.requireBatteryMonitor = g_config.requireBatteryMonitor.load();
    config.requirePvCharger      = g_config.requirePvCharger.load();
    config.valueMaxAgeMs         = g_config.deviceTimeoutMs.load();
    return config;
}

/******************************************************************************
 * Status eines Kanals threadsicher kopieren.
 ******************************************************************************/
ChannelStatus readStatus(const Channel& channel)
{
    portENTER_CRITICAL(&g_statusLock);
    const ChannelStatus copy = channel.status;
    portEXIT_CRITICAL(&g_statusLock);
    return copy;
}

/******************************************************************************
 * Status aktualisieren und eine Nachricht an den Logger senden.
 *
 * Nicht blockierend: Ist die Queue voll, wird die Nachricht verworfen und
 * gezählt. Der Empfang darf nie wegen der Ausgabe stocken.
 * Nur aus der UART-Task des Kanals aufrufen.
 ******************************************************************************/
void publish(Channel& channel, LogKind kind, uint32_t now,
             DeviceDetector::Origin origin = DeviceDetector::Origin::Unassigned,
             const VeDirectFrame* frame = nullptr)
{
    portENTER_CRITICAL(&g_statusLock);
    channel.status.parser   = channel.parser.statistics();
    channel.status.device   = channel.detector.identity();
    channel.status.detector = channel.detector.statistics();
    channel.status.linkUnstable = channel.link.isUnstable();   // eigene Task -> Lesen ohne Mutex ok
    channel.status.hex      = channel.hexParser.statistics();
    channel.status.hexJob   = channel.hexJob.result();
    channel.status.hexBusy  = channel.hexJob.busy();
    if (kind == LogKind::Frame)
    {
        channel.status.lastValidFrameMs = now;
        channel.status.hasValidFrame    = true;
    }
    const ChannelStatus snapshot = channel.status;
    portEXIT_CRITICAL(&g_statusLock);

    // W4a: Geräteereignisse in die Ereignisliste (WebUI, Reiter "Fehler")
    if (kind == LogKind::DeviceIdentified || kind == LogKind::DeviceChanged ||
        kind == LogKind::DeviceLost)
    {
        const DeviceDetector::Identity& cur  = channel.detector.identity();
        const DeviceDetector::Identity& prev = channel.detector.previousIdentity();
        const char* curName  = cur.device  ? cur.device->name  : cur.productId;
        const char* prevName = prev.device ? prev.device->name : prev.productId;
        if (kind == LogKind::DeviceIdentified)
        {
            addEvent(Severity::Info, "%s: %s erkannt", channel.name, curName);
        }
        else if (kind == LogKind::DeviceChanged)
        {
            addEvent(Severity::Warning, "%s: Gerät gewechselt von %s zu %s", channel.name, prevName, curName);
        }
        else
        {
            addEvent(Severity::Warning, "%s: %s ausgefallen (keine Daten mehr)", channel.name, prevName);
        }
    }

    LogMessage& msg   = channel.outgoing;
    msg.kind           = kind;
    msg.channelIndex   = channel.index;
    msg.timestampMs    = now;
    msg.status         = snapshot;
    msg.origin         = origin;
    msg.previousDevice = channel.detector.previousIdentity();

    if (frame != nullptr)
    {
        msg.frame = *frame;
    }
    else
    {
        msg.frame.clear();
    }

    if (xQueueSend(g_logQueue, &msg, 0) != pdTRUE)
    {
        portENTER_CRITICAL(&g_statusLock);
        ++channel.status.queueDrops;
        portEXIT_CRITICAL(&g_statusLock);
    }
}

/******************************************************************************
 * W4b: HEX-Protokoll (nur lesen) in der UART-Task
 ******************************************************************************/
/// HEX-Ereignis an den Logger melden (Nachricht, Fehler oder gesendete Anfrage).
void publishHex(Channel& channel, LogKind kind, uint32_t now)
{
    channel.outgoing.hexAfterMs = channel.hexSent
        ? static_cast<int32_t>(now - channel.hexSentMs) : -1;
    if (kind == LogKind::HexMessage)
    {
        channel.outgoing.hex = channel.hexParser.message();
    }
    publish(channel, kind, now);
}

/// Wartende HEX-Anfrage (von einer anderen Task hinterlegt) senden.
void sendPendingHex(Channel& channel)
{
    bool pending = false;
    portENTER_CRITICAL(&g_statusLock);
    if (channel.hexPendingSet)
    {
        channel.outgoing.hexRequest = channel.hexPending;
        channel.hexPendingSet = false;
        ++channel.status.hexRequests;
        pending = true;
    }
    portEXIT_CRITICAL(&g_statusLock);

    if (!pending)
    {
        return;
    }
    const VeHex::Request& r = channel.outgoing.hexRequest;
    channel.reader.write(r.text, r.length);
    const uint32_t now = millis();
    channel.hexSentMs = now;
    channel.hexSent   = true;
    publish(channel, LogKind::HexSent, now);
}

/// HEX-Anfrage für einen Kanal hinterlegen (aus beliebiger Task).
/// Die UART-Task sendet sie beim nächsten Durchlauf. Eine noch nicht
/// gesendete Anfrage wird ersetzt.
void requestHex(size_t channelIndex, const VeHex::Request& request)
{
    Channel& channel = g_channels[channelIndex];
    portENTER_CRITICAL(&g_statusLock);
    channel.hexPending    = request;
    channel.hexPendingSet = true;
    portEXIT_CRITICAL(&g_statusLock);
}

/// HEX-Register neu lesen lassen (aus beliebiger Task, z. B. WebUI-Reiter,
/// Knopf, serieller Monitor). Die UART-Task startet die Abfrage, sobald das
/// Gerät erkannt ist und keine andere Abfrage läuft.
void requestHexRefresh(size_t channelIndex)
{
    Channel& channel = g_channels[channelIndex];
    portENTER_CRITICAL(&g_statusLock);
    channel.hexRefreshRequested = true;
    portEXIT_CRITICAL(&g_statusLock);
}

/// W4b: Nach einer Abfrage noch so lange einen Ausfall nicht melden, wenn
/// das Gerät auf HEX antwortet (ältere MPPT-Firmware < 1.53 kann die
/// Textblöcke während HEX-Verkehr verzögern; Shunt FW 4.20: keine Pause).
constexpr uint32_t HexGraceMs      = 3000;
constexpr uint32_t HexAliveMs      = 2000;   ///< "antwortet": letzte HEX-Nachricht so jung

/// Automatische Abfrage erst so lange nach dem Erkennen starten.
/// Finding 11.11.2026: Direkt nach dem Erkennen lieferte der SmartShunt
/// (FW 4.20) für 0x1001 (Ladespannung) zweimal 0 statt 140 (14,0 V); später
/// gelesen stimmte der Wert. Ursache am Gerät unklar, Abhilfe: kurz warten.
constexpr uint32_t HexAutoDelayMs  = 5000;

/// Geräteausfall prüfen und ggf. melden.
void checkDeviceTimeout(Channel& channel, uint32_t now)
{
    // Gerät beantwortet gerade HEX-Anfragen -> es ist da, auch wenn die
    // Textblöcke kurz pausieren. Gilt nur während/kurz nach einer Abfrage.
    const bool hexPhase = channel.hexJob.busy() ||
                          static_cast<uint32_t>(now - channel.hexJobEndMs) < HexGraceMs;
    const bool hexAlive = channel.hexLastRxMs != 0 &&
                          static_cast<uint32_t>(now - channel.hexLastRxMs) < HexAliveMs;
    if (hexPhase && hexAlive)
    {
        return;
    }

    if (channel.detector.checkTimeout(now) == DeviceDetector::Event::Lost)
    {
        channel.hexJob.cancel();   // Abfrage für ein verlorenes Gerät beenden
        {
            StateLock lock(channel.stateMutex);
            channel.link.recordOutage(now);
            channel.deviceState.clear();   // "--" statt veralteter Werte
            channel.deviceState.setIdentity(channel.detector.identity());
            channel.hexCache.clear();      // Einstellungen/Historie gehören zum alten Gerät
        }
        publish(channel, LogKind::DeviceLost, now);
    }
}

/// Automatische Abfrage starten, wenn fällig (nach Erkennung oder auf Anforderung).
void startHexJobIfDue(Channel& channel, uint32_t now)
{
    bool requested = false;
    portENTER_CRITICAL(&g_statusLock);
    requested = channel.hexRefreshRequested;
    portEXIT_CRITICAL(&g_statusLock);

    const bool autoDue = channel.hexAutoPending &&
                         static_cast<int32_t>(now - channel.hexAutoAtMs) >= 0;
    if (!(requested || autoDue) || channel.hexJob.busy())
    {
        return;
    }
    const DeviceDetector::Identity& id = channel.detector.identity();
    if (id.state != DeviceDetector::State::Identified || id.device == nullptr)
    {
        return;   // warten, bis das Gerät erkannt ist (Anforderung bleibt stehen)
    }

    std::size_t count = 0;
    const uint16_t* regs = nullptr;
    if (id.device->category == DeviceCategory::BatteryMonitor) regs = HexJobs::shunt(count);
    else if (id.device->category == DeviceCategory::Mppt)      regs = HexJobs::mppt(count);

    const bool started = regs != nullptr && channel.hexJob.start(regs, count, now);

    // Anforderung erledigt; "läuft" sofort sichtbar machen (die WebUI fragt
    // "angefordert || läuft" ab und darf dazwischen keine Lücke sehen)
    portENTER_CRITICAL(&g_statusLock);
    channel.hexRefreshRequested = false;
    channel.status.hexBusy      = started;
    portEXIT_CRITICAL(&g_statusLock);
    channel.hexAutoPending = false;
}

/// Laufende Abfrage weiterführen: nächste Anfrage senden, Ende melden.
void pollHexJob(Channel& channel, uint32_t now)
{
    VeHex::Request request;
    bool send = false;
    {
        StateLock lock(channel.stateMutex);   // poll() kann den Cache ändern (keine Antwort)
        send = channel.hexJob.poll(now, request, channel.hexCache);
    }
    if (send)
    {
        channel.reader.write(request.text, request.length);
        portENTER_CRITICAL(&g_statusLock);
        ++channel.status.hexRequests;
        portEXIT_CRITICAL(&g_statusLock);
    }

    if (channel.hexJob.finishedNow())
    {
        channel.hexJobEndMs = now;
        const HexJobRunner::Result& r = channel.hexJob.result();
        if (r.aborted)
        {
            addEvent(Severity::Info, "%s: Gerät antwortet nicht auf HEX-Anfragen "
                     "(Einstellungen/Historie nicht verfügbar)", channel.name);
        }
        else if (r.failed > 0)
        {
            addEvent(Severity::Info, "%s: HEX-Abfrage, %u von %u Werten ohne Antwort", channel.name,
                     static_cast<unsigned>(r.failed), static_cast<unsigned>(r.requested));
        }
        publish(channel, LogKind::HexJobDone, now);
    }
}

/// Gültige HEX-Nachricht verarbeiten: Antwort der Abfrage, ungefragte
/// Meldung (aktualisiert bekannte Register) oder Diagnose-Antwort (Monitor).
void handleHexMessage(Channel& channel, uint32_t now)
{
    const VeHex::Message& m = channel.hexParser.message();
    channel.hexLastRxMs = now;

    bool consumed = false;
    {
        StateLock lock(channel.stateMutex);
        consumed = channel.hexJob.onMessage(m, now, channel.hexCache);
        if (!consumed)
        {
            channel.hexCache.store(m, now, true);
        }
    }
    if (!consumed)
    {
        // Antworten der automatischen Abfrage nicht einzeln ausgeben (bis zu
        // 39 Zeilen), nur Diagnose-Antworten und ungefragte Meldungen.
        publishHex(channel, LogKind::HexMessage, now);
    }
}

/// Gültigen Block in den Gerätezustand übernehmen (Stufe 4a).
void applyFrame(Channel& channel, const VeDirectFrame& frame,
                const DeviceDetector::Result& result, uint32_t now)
{
    StateLock lock(channel.stateMutex);

    if (result.event == DeviceDetector::Event::Changed)
    {
        // Anderes Gerät: Werte des alten Geräts dürfen nicht stehen bleiben
        channel.deviceState.clear();
    }
    channel.deviceState.setIdentity(channel.detector.identity());

    // Nur Blöcke des erkannten Geräts übernehmen
    if (result.origin == DeviceDetector::Origin::Own ||
        result.origin == DeviceDetector::Origin::Inherited)
    {
        channel.deviceState.apply(frame, now);
    }
}

/******************************************************************************
 * UART-Task: Bytes lesen, parsen, Gerät erkennen, Ergebnisse melden.
 *
 * Gibt selbst NICHTS auf Serial aus.
 ******************************************************************************/
void uartTask(void* parameter)
{
    Channel& channel = *static_cast<Channel*>(parameter);

    esp_task_wdt_add(nullptr);   // UART-Task vom Watchdog überwachen

    for (;;)
    {
        esp_task_wdt_reset();

        // Timeout laufend aus der Konfiguration übernehmen
        // (kann später z. B. über die WebUI geändert werden).
        channel.detector.setTimeoutMs(g_config.deviceTimeoutMs.load());

        // Ausfall auch dann erkennen, wenn gar keine Bytes mehr kommen
        // (z. B. Stecker abgezogen, Kabelbruch).
        checkDeviceTimeout(channel, millis());

        // Verbindungsüberwachung: Einstellungen übernehmen, Erholung prüfen
        {
            StateLock lock(channel.stateMutex);
            channel.link.setConfig(makeLinkConfig());
            channel.link.update(millis(),
                                channel.detector.identity().state == DeviceDetector::State::Identified);
        }

        // W4b: hinterlegte Diagnose-Anfrage senden, automatische Abfrage
        // starten/weiterführen (nur lesende Befehle)
        sendPendingHex(channel);
        startHexJobIfDue(channel, millis());
        pollHexJob(channel, millis());

        while (channel.reader.available())
        {
            const uint8_t byte = static_cast<uint8_t>(channel.reader.read());

            // W4b: Dieselben Bytes auch auf HEX-Nachrichten prüfen (":…\n").
            // Der Text-Parser überspringt diese Zeilen selbst.
            const VeHex::VeDirectHexParser::Event hexEvent = channel.hexParser.process(byte);
            if (hexEvent == VeHex::VeDirectHexParser::Event::MessageReady)
            {
                handleHexMessage(channel, millis());
            }
            else if (hexEvent != VeHex::VeDirectHexParser::Event::None)
            {
                publishHex(channel, LogKind::HexError, millis());
            }

            const VeDirectTextParser::Event event = channel.parser.parse(byte);

            if (event == VeDirectTextParser::Event::None)
            {
                continue;
            }

            const uint32_t now = millis();

            switch (event)
            {
                case VeDirectTextParser::Event::FrameReady:
                {
                    checkDeviceTimeout(channel, now);   // vor update(), siehe DeviceDetector

                    const VeDirectFrame& frame = channel.parser.frame();
                    const DeviceDetector::Result result = channel.detector.update(frame, now);

                    // Erst das Ereignis, dann den Block melden
                    if (result.event == DeviceDetector::Event::Identified)
                    {
                        publish(channel, LogKind::DeviceIdentified, now);
                        channel.hexAutoPending = true;   // W4b: einmal nach Erkennung lesen
                        channel.hexAutoAtMs    = now + HexAutoDelayMs;
                    }
                    else if (result.event == DeviceDetector::Event::Changed)
                    {
                        publish(channel, LogKind::DeviceChanged, now);
                        channel.hexJob.cancel();
                        {
                            StateLock lock(channel.stateMutex);
                            channel.hexCache.clear();   // Werte des alten Geräts verwerfen
                        }
                        channel.hexAutoPending = true;
                        channel.hexAutoAtMs    = now + HexAutoDelayMs;
                    }

                    applyFrame(channel, frame, result, now);
                    publish(channel, LogKind::Frame, now, result.origin, &frame);
                    break;
                }

                case VeDirectTextParser::Event::ChecksumError:
                {
                    StateLock lock(channel.stateMutex);
                    channel.link.recordChecksumError(now);
                }
                    publish(channel, LogKind::ChecksumError, now);
                    break;

                default:
                {
                    StateLock lock(channel.stateMutex);
                    channel.link.recordDiscardedFrame(now);
                }
                    publish(channel, LogKind::FrameDiscarded, now);
                    break;
            }
        }

        // UART-FIFO/Puffer füllt sich mit ~2 Byte/ms (19200 Baud);
        // der Standard-RX-Puffer (256 Byte) reicht bei 1 Tick Pause locker.
        vTaskDelay(1);
    }
}

/******************************************************************************
 * System-Task: Systemzustand und Fehlerwort berechnen (1×/s).
 *
 * Kopiert die Kanalzustände unter Mutex, liest DANACH die Zeit und wertet
 * aus. In Stufe 4b läuft hier zusätzlich die Abschaltlogik.
 ******************************************************************************/
/// Relais-Sollzustände für Logger (später Display/WebUI) veröffentlichen.
void publishControl(const ControlCommand* command, CommandResult result)
{
    ControlSnapshot snapshot;
    for (size_t i = 0; i < RelayCount; ++i)
    {
        const RelayId id = static_cast<RelayId>(i);
        snapshot.relays[i]  = g_loadController.status(id);
        snapshot.names[i]   = g_loadController.config().relays[i].name;
        snapshot.shortNames[i] = g_loadController.config().relays[i].shortName;
        snapshot.enabled[i] = g_loadController.config().relays[i].enabled;
        snapshot.configs[i] = g_loadController.config().relays[i];
    }
    snapshot.overrideActive = g_loadController.overrideActive();

    portENTER_CRITICAL(&g_statusLock);
    snapshot.commandCount = g_control.commandCount;
    snapshot.lastCommand  = g_control.lastCommand;
    snapshot.lastResult   = g_control.lastResult;
    if (command != nullptr)
    {
        ++snapshot.commandCount;
        snapshot.lastCommand = *command;
        snapshot.lastResult  = result;
    }
    g_control = snapshot;
    portEXIT_CRITICAL(&g_statusLock);
}

/// Systemzustand neu berechnen (Kanäle kopieren, auswerten, speichern).
SystemState evaluateSystem()
{
    ChannelView views[ChannelCount];

    for (size_t i = 0; i < ChannelCount; ++i)
    {
        {
            StateLock lock(g_channels[i].stateMutex);
            g_stateSnapshot[i] = g_channels[i].deviceState;
            g_linkSnapshot[i]  = g_channels[i].link;
        }
        views[i].state = &g_stateSnapshot[i];
        views[i].link  = &g_linkSnapshot[i];
    }

    // Zeit ERST NACH dem Kopieren lesen (sonst Überlauf der Altersrechnung)
    const uint32_t now = millis();
    const SystemState state =
        SystemEvaluator::evaluate(views, ChannelCount, makeSystemConfig(), now);

    portENTER_CRITICAL(&g_statusLock);
    g_systemState = state;
    portEXIT_CRITICAL(&g_statusLock);

    return state;
}

/******************************************************************************
 * W4a Schritt 4: Ereignisse der systemTask
 ******************************************************************************/
/// Fehler erst nach dieser Zeit protokollieren: Beim Start sind die Geräte
/// noch nicht erkannt ("fehlt" wäre nur Startrauschen).
constexpr uint32_t FaultLogGraceMs = 10000;

const char* sourceText(CommandSource source)
{
    switch (source)
    {
        case CommandSource::Local: return "lokal";
        case CommandSource::WebUi: return "WebUI";
        case CommandSource::Mqtt:  return "MQTT";
    }
    return "?";
}

/// Relais-Zustand merken (vor einer Änderung).
void captureRelays(bool on[RelayCount])
{
    for (size_t i = 0; i < RelayCount; ++i)
    {
        on[i] = g_loadController.status(static_cast<RelayId>(i)).on;
    }
}

/// Geänderte Relais protokollieren. command = auslösender Befehl oder nullptr
/// (Automatik: Schutzabschaltung, Freigabe, Start).
void logRelayChanges(const bool before[RelayCount], const ControlCommand* command)
{
    for (size_t i = 0; i < RelayCount; ++i)
    {
        const RelayStatus& st = g_loadController.status(static_cast<RelayId>(i));
        if (st.on == before[i])
        {
            continue;
        }
        const char* name = g_loadController.config().relays[i].name;
        if (command != nullptr)
        {
            addEvent(Severity::Info, "R%u %s %s (Befehl %s)", static_cast<unsigned>(i + 1), name,
                     st.on ? "EIN" : "AUS", sourceText(command->source));
        }
        else
        {
            // Automatisch AUS = Schutzabschaltung -> Warnung
            addEvent(st.on ? Severity::Info : Severity::Warning, "R%u %s %s: %s",
                     static_cast<unsigned>(i + 1), name, st.on ? "EIN" : "AUS", st.reason);
        }
    }
}

/// Befehlsergebnis protokollieren, soweit es nicht schon als Relaisänderung
/// erscheint (abgelehnt, gespeichert, Override).
void logCommand(const ControlCommand& command, CommandResult result)
{
    if (command.kind == CommandKind::Override)
    {
        addEvent(command.on ? Severity::Warning : Severity::Info, "Override %s (Befehl %s)",
                 command.on ? "EINGESCHALTET – Schutzsperren aufgehoben" : "ausgeschaltet",
                 sourceText(command.source));
        return;
    }
    if (command.kind == CommandKind::ResetLimits && !command.allRelays)
    {
        const size_t i = static_cast<size_t>(command.relay);
        if (result != CommandResult::Accepted)
        {
            addEvent(Severity::Warning, "R%u Voreinstellung nicht übernommen: %s",
                     static_cast<unsigned>(i + 1), LoadController::resultText(result));
            return;
        }
        addEvent(Severity::Info, "R%u %s auf Voreinstellung zurückgesetzt (Befehl %s)",
                 static_cast<unsigned>(i + 1), g_loadController.config().relays[i].name,
                 sourceText(command.source));
        return;
    }
    if (command.kind == CommandKind::ResetLimits)
    {
        addEvent(Severity::Info, "Relais-Einstellungen auf Voreinstellung zurückgesetzt (Befehl %s)",
                 sourceText(command.source));
        return;
    }
    if (command.kind == CommandKind::Limits)
    {
        const size_t i = static_cast<size_t>(command.relay);
        const RelayLimits& l = command.limits;
        if (result != CommandResult::Accepted)
        {
            addEvent(Severity::Warning, "R%u Einstellungen nicht übernommen: %s",
                     static_cast<unsigned>(i + 1), LoadController::resultText(result));
            return;
        }
        // Kurz halten: Ereignistexte sind auf 95 Byte begrenzt
        addEvent(Severity::Info, "R%u %s geändert: Sperre %g, Aus %g, Frei %g %%, %lu s, Start %s, Auto %s",
                 static_cast<unsigned>(i + 1), g_loadController.config().relays[i].name,
                 l.lockBelowSoc, l.offBelowSoc, l.releaseAtSoc,
                 static_cast<unsigned long>(l.minOffTimeMs / 1000),
                 l.startupRequest ? "ein" : "aus", l.rememberRequest ? "ja" : "nein");
        return;
    }
    if (result == CommandResult::Accepted)
    {
        return;   // erscheint als Relaisänderung (oder war schon so)
    }
    const size_t i = static_cast<size_t>(command.relay);
    addEvent(result == CommandResult::Deferred ? Severity::Info : Severity::Warning,
             "R%u %s %s (Befehl %s): %s", static_cast<unsigned>(i + 1),
             g_loadController.config().relays[i].name, command.on ? "ein" : "aus",
             sourceText(command.source), LoadController::resultText(result));
}

/// Fehlerbits vergleichen und Aufgetreten/Behoben protokollieren.
void logFaultChanges(uint32_t faults, uint32_t& previous, bool& initialized)
{
    if (millis() < FaultLogGraceMs)
    {
        return;
    }
    const SystemConfig cfg = makeSystemConfig();
    if (!initialized)
    {
        previous = 0;   // beim ersten Mal alle aktiven Fehler melden
        initialized = true;
    }
    const uint32_t changed = faults ^ previous;
    for (unsigned bit = 0; bit < SystemFault::BitCount; ++bit)
    {
        const uint32_t mask = 1u << bit;
        if ((changed & mask) == 0)
        {
            continue;
        }
        if (faults & mask)
        {
            addEvent(SystemEvaluator::severityOf(mask, cfg), "Fehler: %s", SystemEvaluator::faultText(mask));
        }
        else
        {
            addEvent(Severity::Info, "Behoben: %s", SystemEvaluator::faultText(mask));
        }
    }
    previous = faults;
}

/******************************************************************************
 * W4a Schritt 5: Einstellungen im Flash (NVS)
 *
 *   Namensraum "app"    : "install" = Anlage (0 Camper, 1 Balkonkraftwerk)
 *   Namensraum "relays" : je Relais n (1..3) die änderbaren Werte, nur wenn
 *                         in der WebUI geändert ("rn.set" = true). SOC-Werte
 *                         in Zehntel-Prozent, Auszeit in s.
 * Fehlt ein Eintrag, gilt die Voreinstellung der Anlage. Beim Wechsel der
 * Anlage und bei "Werkseinstellung" wird "relays" gelöscht.
 ******************************************************************************/
const char* const NvsApp    = "app";
const char* const NvsRelays = "relays";

/// Voreinstellung der aktuell gewählten Anlage.
LoadControllerConfig presetConfig()
{
    return g_config.installation.load() == Installation::Balcony
               ? LoadControllerConfig::balcony()
               : LoadControllerConfig::camper();
}

/// NVS-Schlüssel "r<n>.<name>" (n = 1..3) bauen. Höchstens 15 Zeichen (NVS).
String relayKey(size_t index, const char* name)
{
    return "r" + String(static_cast<unsigned>(index + 1)) + "." + name;
}

uint32_t socToTenths(float soc) { return static_cast<uint32_t>(soc * 10.0f + 0.5f); }

/// Gespeicherte Werte eines Relais lesen. false = keine gespeichert.
bool loadRelayLimits(size_t index, const RelayLimits& preset, RelayLimits& out)
{
    if (!SettingsStore::getBool(NvsRelays, relayKey(index, "set").c_str(), false))
    {
        return false;
    }
    out = preset;
    out.lockBelowSoc    = SettingsStore::getUInt(NvsRelays, relayKey(index, "lock").c_str(),
                                                 socToTenths(preset.lockBelowSoc)) / 10.0f;
    out.offBelowSoc     = SettingsStore::getUInt(NvsRelays, relayKey(index, "off").c_str(),
                                                 socToTenths(preset.offBelowSoc)) / 10.0f;
    out.releaseAtSoc    = SettingsStore::getUInt(NvsRelays, relayKey(index, "rel").c_str(),
                                                 socToTenths(preset.releaseAtSoc)) / 10.0f;
    out.minOffTimeMs    = SettingsStore::getUInt(NvsRelays, relayKey(index, "minoff").c_str(),
                                                 preset.minOffTimeMs / 1000) * 1000;
    out.startupRequest  = SettingsStore::getBool(NvsRelays, relayKey(index, "start").c_str(),
                                                 preset.startupRequest);
    out.rememberRequest = SettingsStore::getBool(NvsRelays, relayKey(index, "auto").c_str(),
                                                 preset.rememberRequest);
    return true;
}

/// Werte eines Relais speichern (nur aus der webTask aufrufen).
bool saveRelayLimits(size_t index, const RelayLimits& l)
{
    bool ok = true;
    ok &= SettingsStore::putUInt(NvsRelays, relayKey(index, "lock").c_str(),   socToTenths(l.lockBelowSoc));
    ok &= SettingsStore::putUInt(NvsRelays, relayKey(index, "off").c_str(),    socToTenths(l.offBelowSoc));
    ok &= SettingsStore::putUInt(NvsRelays, relayKey(index, "rel").c_str(),    socToTenths(l.releaseAtSoc));
    ok &= SettingsStore::putUInt(NvsRelays, relayKey(index, "minoff").c_str(), l.minOffTimeMs / 1000);
    ok &= SettingsStore::putBool(NvsRelays, relayKey(index, "start").c_str(),  l.startupRequest);
    ok &= SettingsStore::putBool(NvsRelays, relayKey(index, "auto").c_str(),   l.rememberRequest);
    ok &= SettingsStore::putBool(NvsRelays, relayKey(index, "set").c_str(),    true);   // zuletzt
    return ok;
}

/// Startkonfiguration: Voreinstellung der Anlage + gespeicherte Änderungen.
/// Ungültige gespeicherte Werte werden verworfen (Voreinstellung, Warnung).
LoadControllerConfig loadRelayConfig()
{
    LoadControllerConfig config = presetConfig();
    for (size_t i = 0; i < RelayCount; ++i)
    {
        RelayConfig& r = config.relays[i];
        if (!r.enabled)
        {
            continue;
        }
        RelayLimits stored;
        if (!loadRelayLimits(i, RelayLimits::from(r), stored))
        {
            continue;
        }
        if (stored.validate() != nullptr)
        {
            addEvent(Severity::Warning, "R%u %s: gespeicherte Einstellungen ungültig, Voreinstellung verwendet",
                     static_cast<unsigned>(i + 1), r.name);
            continue;
        }
        stored.applyTo(r);
    }
    return config;
}

/// Anlage aus dem NVS lesen (in setup(), vor dem Start der Tasks).
void loadInstallation()
{
    const uint32_t v = SettingsStore::getUInt(NvsApp, "install", 0);
    g_config.installation.store(v == 1 ? Installation::Balcony : Installation::Camper);
}

/******************************************************************************
 * System-Task: 1×/s Systemzustand + Abschaltlogik; Befehle sofort.
 ******************************************************************************/
void checkMemoryHealth();   // W5: definiert bei g_tasks (Speicher-Überwachung)

void systemTask(void*)
{
    // Relais-Voreinstellung je Anlage plus in der WebUI geänderte Werte (NVS);
    // Neustart = Normalbetrieb (kein Override, Zustand nach Neustart je Relais)
    g_loadController.setConfig(loadRelayConfig());
    bool relaysBefore[RelayCount] = {};   // nach dem Einschalten ist alles aus
    g_loadController.start(millis());
    g_relays.apply(g_loadController);   // Startzustand ausgeben (R2/R3 ein)
    logRelayChanges(relaysBefore, nullptr);

    uint32_t loggedFaults = 0;
    bool     faultsLogged = false;
    publishControl(nullptr, CommandResult::Accepted);

    esp_task_wdt_add(nullptr);          // diese Task vom Watchdog überwachen

    SystemState state;
    TickType_t  nextRun = xTaskGetTickCount();

    for (;;)
    {
        esp_task_wdt_reset();

        // Bis zur nächsten Auswertung auf Befehle warten
        const TickType_t nowTick = xTaskGetTickCount();
        const TickType_t wait = (static_cast<int32_t>(nextRun - nowTick) > 0) ? (nextRun - nowTick) : 0;

        ControlCommand command;
        if (xQueueReceive(g_commandQueue, &command, wait) == pdTRUE)
        {
            const uint32_t now = millis();
            CommandResult result = CommandResult::Accepted;
            captureRelays(relaysBefore);

            if (command.kind == CommandKind::Override)
            {
                g_loadController.setOverride(command.on, state, now);
            }
            else if (command.kind == CommandKind::Limits || command.kind == CommandKind::ResetLimits)
            {
                if (command.kind == CommandKind::Limits)
                {
                    result = g_loadController.setLimits(command.relay, command.limits);
                }
                else if (!command.allRelays)
                {
                    // Ein Relais auf die Voreinstellung der Anlage, Zustand bleibt
                    const size_t i = static_cast<size_t>(command.relay);
                    result = g_loadController.setLimits(command.relay,
                                                        RelayLimits::from(presetConfig().relays[i]));
                }
                else
                {
                    // Werkseinstellung: Voreinstellung der Anlage, Zustand bleibt
                    const LoadControllerConfig preset = presetConfig();
                    for (size_t i = 0; i < RelayCount; ++i)
                    {
                        if (preset.relays[i].enabled)
                        {
                            g_loadController.setLimits(static_cast<RelayId>(i),
                                                       RelayLimits::from(preset.relays[i]));
                        }
                    }
                }
                // Neue Schwellen sofort anwenden; Folgen (z. B. Abschaltung)
                // erscheinen als automatische Relaisänderung mit Grund.
                publishControl(&command, result);
                logCommand(command, result);
                g_loadController.update(state, now);
                g_relays.apply(g_loadController);
                logRelayChanges(relaysBefore, nullptr);
                publishControl(nullptr, CommandResult::Accepted);
                continue;
            }
            else
            {
                result = g_loadController.request(command.relay, command.on,
                                                  command.source, state, now);
            }
            g_relays.apply(g_loadController);   // sofort schalten
            publishControl(&command, result);
            logCommand(command, result);
            logRelayChanges(relaysBefore, &command);
            continue;   // weiter warten bis zur nächsten Auswertung
        }

        nextRun += pdMS_TO_TICKS(SystemIntervalMs);

        state = evaluateSystem();
        logFaultChanges(state.faults, loggedFaults, faultsLogged);
        checkMemoryHealth();   // W5: 1×/min, Warnung bei knappem Speicher
        captureRelays(relaysBefore);
        g_loadController.update(state, millis());
        g_relays.apply(g_loadController);
        logRelayChanges(relaysBefore, nullptr);
        publishControl(nullptr, CommandResult::Accepted);
    }
}

/******************************************************************************
 * Display-Task (Stufe 4c-2): E-Paper mit den wichtigsten Werten.
 *
 * Eigene Task, weil ein Bildaufbau bis zu ca. 2 s dauert. Nicht vom
 * Watchdog überwacht: Ein hängendes Display darf die Steuerung nicht
 * neu starten.
 ******************************************************************************/
StatusDisplay g_statusDisplay;   ///< nur in der displayTask verwenden!

DisplayData makeDisplayData()
{
    DisplayData data;
    data.system = readSystemState();

    const ControlSnapshot control = readControl();
    for (size_t i = 0; i < RelayCount; ++i)
    {
        data.relayOn[i]        = control.relays[i].on;
        data.relayEnabled[i]   = control.enabled[i];
        data.relayShortName[i] = control.shortNames[i];
    }
    data.overrideActive = control.overrideActive;

    // Ersteinrichtung: AP-Name und Startpasswort anzeigen, solange das
    // Gerätepasswort nicht geändert wurde und der AP aktiv ist.
    const NetStatus net = readNetStatus();
    data.wifiConnected  = net.staConnected;
    data.wifiRssi       = net.rssi;
    data.apActive       = net.apActive;
    const MqttConfig mqttCfg = g_mqtt.config();
    data.mqttConfigured = mqttCfg.enabled && mqttCfg.host.length() > 0;
    data.mqttConnected  = g_mqtt.status().state == MqttState::Connected;
    data.updateRunning  = g_webUi.updateRunning();

    data.showSetup = net.apActive && net.defaultPassword && net.displayPassword[0] != '\0';
    if (data.showSetup)
    {
        std::strncpy(data.setupSsid, net.apSsid, sizeof(data.setupSsid) - 1);
        std::strncpy(data.setupPassword, net.displayPassword, sizeof(data.setupPassword) - 1);
    }
    return data;
}

/******************************************************************************
 * Web-Task (Stufe W1): WLAN-Verwaltung und Webserver.
 *
 * Nicht vom Watchdog überwacht: WLAN-Probleme dürfen die Steuerung nicht
 * neu starten. Schreibt nicht auf Serial (Meldungen über g_netStatus an
 * den Logger).
 ******************************************************************************/
void registerProjectApi();   // W4a, definiert nach den JSON-Bausteinen (MQTT-Abschnitt)

void webTask(void*)
{
    // Dateisystem für die WebUI-Dateien (true = bei leerem/defektem
    // Dateisystem formatieren; Dateien dann per "uploadfs" aufspielen)
    LittleFS.begin(true);

    g_wifi.begin(DefaultHostname, ApPrefix);
    g_webUi.setMqtt(&g_mqtt);   // W3: MQTT-Reiter und /api/mqtt
    registerProjectApi();       // W4a: Projektseiten (/api/live, ...)
    g_webUi.begin(g_wifi, DeviceName, FirmwareVersion);
    startNtp();   // W4e: SNTP wartet selbst, bis das WLAN Internet hat

    bool stableConfirmed = false;

    for (;;)
    {
        const uint32_t now = millis();
        g_wifi.loop(now);
        g_webUi.loop(now);
        clockLoop();   // W4e: NTP-Abgleich übernehmen, RTC nachstellen

        // W2: Nach 60 s stabilem Lauf gilt eine neue Firmware als gut
        // (sonst Rückfall auf die vorherige, siehe UpdateManager)
        if (!stableConfirmed && now >= UpdateManager::StableAfterMs)
        {
            UpdateManager::confirmStable();
            stableConfirmed = true;
        }

        const NetStatus status = g_wifi.status();
        portENTER_CRITICAL(&g_statusLock);
        g_netStatus = status;
        portEXIT_CRITICAL(&g_statusLock);

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

/******************************************************************************
 * MQTT-Task (Stufe W3)
 *
 * Topics (Basis-Topic einstellbar, Standard "vedirect"), Projektinhaber
 * 09.11.2026: getrennte JSON je Bereich, alle retained:
 *   <base>/status           online / offline (Letzter Wille)
 *   <base>/battery          {"soc":..,"voltage":..,"current":..,"power":.., ...}
 *   <base>/pv               {"power":..,"voltage":..,"state":"..", ...}
 *   <base>/relays           {"r1":"ON","r2":..,"r3":..,"override":"OFF", ...}
 *   <base>/system           {"faults":"0x..","severity":"..","shunt":"ok", ...}
 *   <base>/command-result   Rückmeldung zu Befehlen (nicht retained)
 * Befehle (ON/OFF, auch 1/0, true/false):
 *   <base>/relay/r1-set, r2-set, r3-set, <base>/override-set
 * Ungültige Werte werden als null gesendet (nie 0).
 *
 * Eigene Task: Ein Verbindungsversuch zu einem nicht erreichbaren Broker
 * blockiert einige Sekunden. Nicht vom Watchdog überwacht.
 ******************************************************************************/
constexpr uint32_t    MqttTaskStack    = 6144;
constexpr UBaseType_t MqttTaskPriority = 1;

/******************************************************************************
 * W5: Übersicht der Tasks für die Speicher-Diagnose (Monitor "speicher").
 * Handles werden in setup() beim Anlegen eingetragen (Reihenfolge fest).
 ******************************************************************************/
struct TaskInfo
{
    const char*  name;
    uint32_t     stackBytes;
    TaskHandle_t handle;
};

TaskInfo g_tasks[] =
{
    { "Logger",  LoggerTaskStack,  nullptr },
    { "System",  SystemTaskStack,  nullptr },
    { "Display", DisplayTaskStack, nullptr },
    { "Web",     WebTaskStack,     nullptr },
    { "Mqtt",    MqttTaskStack,    nullptr },
    { "UART1",   UartTaskStack,    nullptr },
    { "UART2",   UartTaskStack,    nullptr },
};

/******************************************************************************
 * W5: Speicher-Überwachung ohne Zuschauer (Projektinhaber 11.11.2026).
 * Schleichende Verschlechterung soll auch ohne Monitor auffallen:
 * Warnung in der Ereignisliste, wenn der kleinste freie Heap seit Start
 * unter HeapWarnBytes fällt oder eine Task weniger als StackWarnBytes
 * Stack-Reserve hatte. Je Ursache nur EINMAL (Werte sind "seit Start").
 ******************************************************************************/
constexpr uint32_t HeapWarnBytes   = 30 * 1024;
constexpr uint32_t StackWarnBytes  = 1024;
constexpr uint32_t MemoryCheckMs   = 60000;

/// Knappste Stack-Reserve aller Tasks (Byte); name = betroffene Task.
uint32_t minStackReserve(const char*& name)
{
    uint32_t best = UINT32_MAX;
    name = "";
    for (const TaskInfo& t : g_tasks)
    {
        if (t.handle == nullptr) continue;
        const uint32_t freeMin = uxTaskGetStackHighWaterMark(t.handle);
        if (freeMin < best)
        {
            best = freeMin;
            name = t.name;
        }
    }
    return best == UINT32_MAX ? 0 : best;
}

/// Zusatz für /api/status: kleinster freier Heap seit Start, knappste Stack-Reserve.
String memoryStatusJson()
{
    const char* task = "";
    const uint32_t stack = minStackReserve(task);
    return ",\"memory\":{\"minFreeHeap\":" + String(ESP.getMinFreeHeap()) +
           ",\"minStack\":" + String(stack) +
           ",\"minStackTask\":" + WebUi::jsonString(task) +
           ",\"heapWarn\":" + String(HeapWarnBytes) +
           ",\"stackWarn\":" + String(StackWarnBytes) + "}";
}

/// Aus der systemTask aufrufen (1×/s); prüft nur alle MemoryCheckMs.
void checkMemoryHealth()
{
    static uint32_t lastMs    = 0;
    static bool     heapWarned = false;
    static bool     stackWarned[sizeof(g_tasks) / sizeof(g_tasks[0])] = {};

    const uint32_t now = millis();
    if (now - lastMs < MemoryCheckMs)
    {
        return;
    }
    lastMs = now;

    const uint32_t minHeap = ESP.getMinFreeHeap();
    if (!heapWarned && minHeap < HeapWarnBytes)
    {
        heapWarned = true;
        addEvent(Severity::Warning, "Arbeitsspeicher knapp: kleinster freier Wert seit Start %lu KB (Grenze %lu KB)",
                 static_cast<unsigned long>(minHeap / 1024), static_cast<unsigned long>(HeapWarnBytes / 1024));
    }
    for (size_t i = 0; i < sizeof(g_tasks) / sizeof(g_tasks[0]); ++i)
    {
        if (g_tasks[i].handle == nullptr || stackWarned[i]) continue;
        const uint32_t freeMin = uxTaskGetStackHighWaterMark(g_tasks[i].handle);
        if (freeMin < StackWarnBytes)
        {
            stackWarned[i] = true;
            addEvent(Severity::Warning, "Stack der Task %s knapp: Reserve min. %lu Byte von %lu",
                     g_tasks[i].name, static_cast<unsigned long>(freeMin),
                     static_cast<unsigned long>(g_tasks[i].stackBytes));
        }
    }
}

/// Messwert als JSON-Zahl, ungültig = null.
String jsonNumber(const Reading& r, unsigned decimals)
{
    if (!r.valid)
    {
        return String("null");
    }
    if (decimals == 0)
    {
        // String(float, 0) liefert " 0" mit führendem Leerzeichen (Finding 09.11.2026)
        return String(static_cast<long>(lroundf(r.value)));
    }
    return String(r.value, decimals);
}

const char* onOff(bool on) { return on ? "ON" : "OFF"; }

const char* linkText(const SystemState& s, uint32_t missingMask, uint32_t unstableMask)
{
    if (s.faults & missingMask)  return "defekt";
    if (s.faults & unstableMask) return "instabil";
    return "ok";
}

String buildBatteryJson(const SystemState& s)
{
    const char* source = !s.batteryVoltage.valid ? "none"
                       : (s.batteryVoltage.source == ValueSource::PvCharger ? "mppt" : "shunt");
    String j = "{";
    j += "\"soc\":"               + jsonNumber(s.stateOfCharge, 1);
    j += ",\"voltage\":"          + jsonNumber(s.batteryVoltage, 3);
    j += ",\"voltageSource\":\""  + String(source) + "\"";
    j += ",\"current\":"          + jsonNumber(s.batteryCurrent, 3);
    j += ",\"power\":"            + jsonNumber(s.batteryPower, 0);
    j += ",\"consumedAh\":"       + jsonNumber(s.consumedAmpHours, 3);
    j += ",\"timeToGoMin\":"      + jsonNumber(s.timeToGo, 0);
    j += ",\"timeToGoInfinite\":" + String(s.timeToGoInfinite ? "true" : "false");
    j += "}";
    return j;
}

String buildPvJson(const SystemState& s)
{
    String j = "{";
    j += "\"power\":"          + jsonNumber(s.pvPower, 0);
    j += ",\"voltage\":"       + jsonNumber(s.pvVoltage, 3);
    j += ",\"yieldTodayKWh\":" + jsonNumber(s.yieldToday, 2);
    j += ",\"state\":"         + (s.chargerStateText ? WebUi::jsonString(s.chargerStateText) : String("null"));
    j += ",\"stateCode\":"     + (s.chargerState >= 0 ? String(s.chargerState) : String("null"));
    j += ",\"errorCode\":"     + (s.pvErrorCode >= 0 ? String(s.pvErrorCode) : String("null"));
    j += "}";
    return j;
}

String buildRelaysJson(const ControlSnapshot& c)
{
    String j = "{";
    for (size_t i = 0; i < RelayCount; ++i)
    {
        const String key = "r" + String(i + 1);
        if (i > 0) j += ",";
        j += "\"" + key + "\":\"" + (c.enabled[i] ? onOff(c.relays[i].on) : "DISABLED") + "\"";
        j += ",\"" + key + "Name\":"   + WebUi::jsonString(c.names[i] ? c.names[i] : "");
        j += ",\"" + key + "Locked\":" + String(c.relays[i].locked ? "true" : "false");
        j += ",\"" + key + "Wanted\":" + String(c.relays[i].requested ? "true" : "false");
        j += ",\"" + key + "Reason\":" + WebUi::jsonString(c.relays[i].reason ? c.relays[i].reason : "");
    }
    j += ",\"override\":\"" + String(onOff(c.overrideActive)) + "\"";
    j += "}";
    return j;
}

String buildSystemJson(const SystemState& s, const NetStatus& net)
{
    char faults[12];
    snprintf(faults, sizeof(faults), "0x%08lX", static_cast<unsigned long>(s.faults));

    const char* severity = "none";
    switch (s.severity)
    {
        case Severity::Info:     severity = "info";     break;
        case Severity::Warning:  severity = "warning";  break;
        case Severity::Critical: severity = "critical"; break;
        default: break;
    }

    String texts = "[";
    bool first = true;
    for (unsigned bit = 0; bit < SystemFault::BitCount; ++bit)
    {
        const uint32_t mask = 1u << bit;
        if (s.faults & mask)
        {
            if (!first) texts += ",";
            texts += WebUi::jsonString(SystemEvaluator::faultText(mask));
            first = false;
        }
    }
    texts += "]";

    String j = "{";
    j += "\"faults\":\""    + String(faults) + "\"";
    j += ",\"severity\":\"" + String(severity) + "\"";
    j += ",\"faultTexts\":" + texts;
    j += ",\"shunt\":\""    + String(linkText(s, SystemFault::BatteryMonitorMissing | SystemFault::MultipleBatteryMonitors,
                                               SystemFault::BatteryLinkUnstable)) + "\"";
    j += ",\"mppt\":\""     + String(linkText(s, SystemFault::PvChargerMissing | SystemFault::MultiplePvChargers,
                                               SystemFault::PvLinkUnstable)) + "\"";
    j += ",\"firmware\":\"" + String(FirmwareVersion) + "\"";
    j += ",\"uptimeS\":"    + String(millis() / 1000);
    j += ",\"rssi\":"       + String(net.rssi);
    j += "}";
    return j;
}

/******************************************************************************
 * W4a: Projekt-Endpunkte der WebUI (laufen in der webTask)
 *
 *   GET  /api/live      {"battery":{..},"pv":{..},"relays":{..},"system":{..},
 *                        "limits":[{"lock":25,"off":15,"release":30,"requiresSoc":true},..]}
 *                       battery/pv/relays/system = gleiche Inhalte wie die
 *                       MQTT-Topics, damit WebUI, MQTT und Anleitung
 *                       übereinstimmen. limits = Schwellen je Relais.
 *   POST /api/relay     relay=1..3, on=ON|OFF -> {"ok":..,"result":"..","live":{..}}
 *   POST /api/override  on=ON|OFF             -> {"ok":..,"result":"..","live":{..}}
 *
 * Schaltbefehle laufen wie bei MQTT über die Befehlsqueue (Quelle WebUi);
 * nur die systemTask schaltet. Der Handler wartet max. CommandWaitMs auf
 * genau seinen Befehl (ticket) und meldet das Ergebnis direkt zurück.
 *
 * Login prüft WebUi. Daten nur über Kopien (readSystemState() usw.) lesen.
 ******************************************************************************/
constexpr uint32_t CommandWaitMs = 500;
std::atomic<uint32_t> g_nextTicket{1};

int parseOnOff(String payload);   // definiert weiter unten (MQTT)

/// Schwellen je Relais als JSON-Array.
String buildLimitsJson(const ControlSnapshot& c)
{
    String j = "[";
    for (size_t i = 0; i < RelayCount; ++i)
    {
        const RelayConfig& r = c.configs[i];
        if (i > 0) j += ",";
        j += "{\"lock\":"        + String(r.lockBelowSoc, 1);
        j += ",\"off\":"         + String(r.offBelowSoc, 1);
        j += ",\"release\":"     + String(r.releaseAtSoc, 1);
        j += ",\"requiresSoc\":" + String(r.requiresSoc ? "true" : "false");
        j += ",\"minOffS\":"     + String(r.minOffTimeMs / 1000);
        j += "}";
    }
    j += "]";
    return j;
}

/// Gesamtes Live-JSON für die Übersicht.
String buildLiveJson()
{
    const SystemState     state   = readSystemState();
    const ControlSnapshot control = readControl();
    const NetStatus       net     = readNetStatus();
    String out;
    out.reserve(1800);
    out  = "{\"battery\":" + buildBatteryJson(state);
    out += ",\"pv\":"      + buildPvJson(state);
    out += ",\"relays\":"  + buildRelaysJson(control);
    out += ",\"system\":"  + buildSystemJson(state, net);
    out += ",\"limits\":"  + buildLimitsJson(control);
    out += "}";
    return out;
}

/// Befehl an die systemTask senden und auf das Ergebnis warten.
/// Rückgabe: Ergebnistext; ok = ausgeführt oder gespeichert.
String sendWebCommand(ControlCommand command, bool& ok)
{
    command.source = CommandSource::WebUi;
    command.ticket = g_nextTicket.fetch_add(1);
    ok = false;
    if (xQueueSend(g_commandQueue, &command, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return "abgelehnt: Gerät ausgelastet, bitte erneut versuchen";
    }
    const uint32_t start = millis();
    while (millis() - start < CommandWaitMs)
    {
        const ControlSnapshot c = readControl();
        if (c.lastCommand.ticket == command.ticket)
        {
            if (command.kind == CommandKind::Override)
            {
                ok = true;
                return "ausgeführt";
            }
            if (command.kind == CommandKind::ResetLimits && command.allRelays)
            {
                ok = true;
                return "ausgeführt";
            }
            ok = c.lastResult == CommandResult::Accepted || c.lastResult == CommandResult::Deferred;
            return LoadController::resultText(c.lastResult);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return "keine Rückmeldung (Befehl wird ggf. noch ausgeführt)";
}

String commandResponse(bool ok, const String& result)
{
    return "{\"ok\":" + String(ok ? "true" : "false") +
           ",\"result\":" + WebUi::jsonString(result) +
           ",\"live\":" + buildLiveJson() + "}";
}

/// Zusatz für /api/status (alle Reiter): Anzahl aktiver Fehler und Schwere,
/// für die Markierung am Reiter "Fehler" und das Banner bei kritischen Fehlern.
String buildStatusExtension()
{
    const SystemState s = readSystemState();
    unsigned count = 0;
    for (unsigned bit = 0; bit < SystemFault::BitCount; ++bit)
    {
        if (s.faults & (1u << bit)) ++count;
    }
    const char* severity = "none";
    switch (s.severity)
    {
        case Severity::Info:     severity = "info";     break;
        case Severity::Warning:  severity = "warning";  break;
        case Severity::Critical: severity = "critical"; break;
        default: break;
    }
    // W4e: "clock.valid" -> die WebUI schickt die Browserzeit, wenn false
    return "\"faults\":{\"count\":" + String(count) + ",\"severity\":\"" + severity + "\"}" +
           ",\"clock\":{\"valid\":" + String(clockValid() ? "true" : "false") +
           ",\"local\":" + WebUi::jsonString(formatLocalTime(clockNowUnix())) +   // leer, wenn ungültig
           ",\"source\":\"" + clockSourceKey(static_cast<ClockSource>(g_clockSource.load())) + "\"}" +
           memoryStatusJson();   // W5: Speicher-Überwachung (System → Info)
}

/******************************************************************************
 * W4e Schritt 3: Uhr (System → Gerät → Uhr)
 ******************************************************************************/
String buildClockJson()
{
    bool batteryLow = false;
    if (g_rtcFound)
    {
        I2cLock lock;
        batteryLow = lock.ok() && g_rtc.batteryLow();
    }
    const uint32_t now   = clockNowUnix();
    const uint32_t setAt = g_clockSetAtUnix.load();

    String j;
    j.reserve(900);
    j  = "{\"valid\":"   + String(clockValid() ? "true" : "false");
    j += ",\"local\":"   + WebUi::jsonString(formatLocalTime(now));
    j += ",\"source\":\"" + String(clockSourceKey(static_cast<ClockSource>(g_clockSource.load()))) + "\"";
    j += ",\"setAgoS\":" + (setAt >= ClockValidAfterUnix && now >= setAt ? String(now - setAt) : String("null"));
    j += ",\"tz\":"      + WebUi::jsonString(Rtc::timeZone(g_tzKey.c_str()).key);
    j += ",\"ntp\":"     + WebUi::jsonString(g_ntpServer);
    j += ",\"rtc\":{\"found\":" + String(g_rtcFound ? "true" : "false");
    j += ",\"chip\":"    + WebUi::jsonString(g_rtc.name());
    j += ",\"batteryLow\":" + String(batteryLow ? "true" : "false") + "}";

    std::size_t n = 0;
    const Rtc::TimeZone* zones = Rtc::timeZones(n);
    j += ",\"zones\":[";
    for (std::size_t i = 0; i < n; ++i)
    {
        if (i > 0) j += ",";
        j += "{\"key\":" + WebUi::jsonString(zones[i].key) + ",\"label\":" + WebUi::jsonString(zones[i].label) + "}";
    }
    const char* const* servers = Rtc::ntpServers(n);
    j += "],\"servers\":[";
    for (std::size_t i = 0; i < n; ++i)
    {
        if (i > 0) j += ",";
        j += WebUi::jsonString(servers[i]);
    }
    j += "]}";
    return j;
}

/******************************************************************************
 * W4a Schritt 3: GET /api/device?role=shunt|mppt
 *
 * Alle empfangenen Felder des Geräts in Klartext (Name, formatierter Wert
 * mit Einheit, Alter), dazu Gerät, UART und Verbindungsstatistik.
 * Felder H1..H23 sind als "history" markiert (Reiter "Historie"), alle
 * anderen erscheinen in den Reitern "Shunt"/"MPPT".
 *
 * Die Kopien liegen statisch (nur die webTask ruft das auf): DeviceState ist
 * ca. 5 KB groß und gehört nicht auf den Stack.
 ******************************************************************************/
DeviceState g_webDeviceCopy;   ///< nur webTask
LinkMonitor g_webLinkCopy;     ///< nur webTask

/// VE.Direct-Historienfeld? (H1..H23, nicht z. B. "HSDS")
bool isHistoryLabel(const char* label)
{
    return label[0] == 'H' && label[1] >= '0' && label[1] <= '9';
}

String buildDeviceJson(bool shunt)
{
    const SystemState state = readSystemState();
    const int index = shunt ? state.batteryChannel : state.pvChannel;
    if (index < 0 || index >= static_cast<int>(ChannelCount))
    {
        return "{\"present\":false}";
    }
    Channel& channel = g_channels[index];

    {
        StateLock lock(channel.stateMutex);
        g_webDeviceCopy = channel.deviceState;
        g_webLinkCopy   = channel.link;
    }
    const ChannelStatus status = readStatus(channel);
    const uint32_t now = millis();   // erst NACH dem Kopieren lesen

    const DeviceDetector::Identity& id = g_webDeviceCopy.identity();
    String j;
    j.reserve(4096);
    j  = "{\"present\":true";
    j += ",\"uart\":"   + WebUi::jsonString(channel.name);
    j += ",\"device\":" + WebUi::jsonString(id.device ? id.device->name : "unbekanntes Gerät");
    j += ",\"pid\":"    + WebUi::jsonString(id.productId);
    j += ",\"lastFrameAgoMs\":" + String(status.hasValidFrame ? DeviceState::ageMs(status.lastValidFrameMs, now) : 0);

    // Verbindung
    const LinkMonitor::Counters& total = g_webLinkCopy.total();
    j += ",\"link\":{";
    j += "\"unstable\":"        + String(g_webLinkCopy.isUnstable() ? "true" : "false");
    j += ",\"unstableCount\":"  + String(g_webLinkCopy.unstableCount());
    j += ",\"checksumErrors\":" + String(total.checksumErrors);
    j += ",\"discarded\":"      + String(total.discardedFrames);
    j += ",\"outages\":"        + String(total.outages);
    j += ",\"lastOutageMs\":"   + String(status.detector.lastOutageMs);
    j += ",\"queueDrops\":"     + String(status.queueDrops);
    j += "}";

    // Felder in der Reihenfolge der fachlichen Bedeutung
    j += ",\"fields\":[";
    bool first = true;
    char buffer[ValueFormatter::BufferSize];
    for (size_t s = 0; s < DeviceState::SlotCount; ++s)
    {
        const DeviceState::Entry* e = g_webDeviceCopy.entry(static_cast<SemanticField>(s));
        if (e == nullptr || !e->present)
        {
            continue;
        }
        const char* label = e->raw.label.data();
        ValueFormatter::format(e->raw, e->value, buffer, sizeof(buffer));
        if (!first) j += ",";
        first = false;
        j += "{\"label\":"  + WebUi::jsonString(label);
        j += ",\"name\":"   + WebUi::jsonString(ValueFormatter::name(e->value));
        j += ",\"value\":"  + WebUi::jsonString(buffer);
        j += ",\"history\":" + String(isHistoryLabel(label) ? "true" : "false");
        j += ",\"ageS\":"   + String(DeviceState::ageMs(e->updatedMs, now) / 1000);
        j += "}";
    }
    j += "]}";
    return j;
}

/******************************************************************************
 * W4a Schritt 4: GET /api/faults
 *
 * {"uptimeS":..,"reset":{"text":"..","severity":".."},
 *  "active":[{"text":"..","severity":".."}],
 *  "pvError":"..."|null, "batteryAlarm":"..."|null,
 *  "events":[{"seq":..,"ageS":..,"severity":"..","text":".."}]}   (neueste zuerst)
 ******************************************************************************/
EventLog g_webEventsCopy;   ///< nur webTask (ca. 5 KB, nicht auf den Stack)

const char* severityKey(Severity s)
{
    switch (s)
    {
        case Severity::Critical: return "critical";
        case Severity::Warning:  return "warning";
        case Severity::Info:     return "info";
        default:                 return "none";
    }
}

/// Ein Feld eines Kanals formatiert (z. B. Alarmgrund), "" wenn nicht vorhanden.
String formatChannelField(int index, SemanticField field)
{
    if (index < 0 || index >= static_cast<int>(ChannelCount))
    {
        return "";
    }
    Channel& channel = g_channels[index];
    {
        StateLock lock(channel.stateMutex);
        g_webDeviceCopy = channel.deviceState;
    }
    const DeviceState::Entry* e = g_webDeviceCopy.entry(field);
    if (e == nullptr || !e->present)
    {
        return "";
    }
    char buffer[ValueFormatter::BufferSize];
    ValueFormatter::format(e->raw, e->value, buffer, sizeof(buffer));
    return String(buffer);
}

String buildFaultsJson()
{
    const SystemState state = readSystemState();
    const SystemConfig cfg  = makeSystemConfig();

    portENTER_CRITICAL(&g_eventLock);
    g_webEventsCopy = g_events;
    portEXIT_CRITICAL(&g_eventLock);
    const uint32_t now = millis();

    String j;
    j.reserve(6000);
    j = "{\"uptimeS\":" + String(now / 1000);

    Severity resetSeverity = Severity::Info;
    const char* resetText = resetReasonText(g_resetReason, resetSeverity);
    j += ",\"reset\":{\"text\":" + WebUi::jsonString(resetText) +
         ",\"severity\":\"" + severityKey(resetSeverity) + "\"}";

    j += ",\"active\":[";
    bool first = true;
    for (unsigned bit = 0; bit < SystemFault::BitCount; ++bit)
    {
        const uint32_t mask = 1u << bit;
        if ((state.faults & mask) == 0) continue;
        if (!first) j += ",";
        first = false;
        j += "{\"text\":" + WebUi::jsonString(SystemEvaluator::faultText(mask)) +
             ",\"severity\":\"" + severityKey(SystemEvaluator::severityOf(mask, cfg)) + "\"}";
    }
    j += "]";

    // Meldungen der Geräte selbst (nur wenn ungleich "kein Fehler/Alarm")
    const String pvError = state.pvErrorCode > 0 ? formatChannelField(state.pvChannel, SemanticField::ErrorCode) : String("");
    const String alarm   = (state.faults & SystemFault::BatteryAlarm)
                               ? formatChannelField(state.batteryChannel, SemanticField::AlarmReason) : String("");
    j += ",\"pvError\":"      + (pvError.length() ? WebUi::jsonString(pvError) : String("null"));
    j += ",\"batteryAlarm\":" + (alarm.length()   ? WebUi::jsonString(alarm)   : String("null"));

    // W4e Schritt 4: Mit gültiger Uhr echte Zeitpunkte. Die Ereignisse kennen
    // nur die Laufzeit (millis); daraus wird zurückgerechnet. Das stimmt auch
    // für Ereignisse, die VOR dem Stellen der Uhr lagen (z. B. der Start).
    const bool     clockOk = clockValid();
    const uint32_t nowUnix = clockNowUnix();
    j += ",\"bootTime\":" + (clockOk ? WebUi::jsonString(formatLocalTime(nowUnix - now / 1000)) : String("null"));

    j += ",\"events\":[";
    for (size_t i = 0; i < g_webEventsCopy.size(); ++i)
    {
        const EventLog::Entry& e = g_webEventsCopy.newest(i);
        const uint32_t ageS = DeviceState::ageMs(e.timeMs, now) / 1000;
        if (i > 0) j += ",";
        j += "{\"seq\":"       + String(e.seq);
        j += ",\"ageS\":"      + String(ageS);
        j += ",\"time\":"      + (clockOk ? WebUi::jsonString(formatLocalTime(nowUnix - ageS)) : String("null"));
        j += ",\"severity\":\"" + String(severityKey(e.severity)) + "\"";
        j += ",\"text\":"      + WebUi::jsonString(e.text);
        j += "}";
    }
    j += "]}";
    return j;
}

/******************************************************************************
 * W4a Schritt 5: Einstellungen (Reiter "Einstellungen")
 ******************************************************************************/
/// Änderbare Werte als JSON-Felder (ohne Klammern).
String limitsFields(const RelayLimits& l)
{
    String j;
    j += "\"lock\":"     + String(l.lockBelowSoc, 1);
    j += ",\"off\":"     + String(l.offBelowSoc, 1);
    j += ",\"release\":" + String(l.releaseAtSoc, 1);
    j += ",\"minOffS\":" + String(l.minOffTimeMs / 1000);
    j += ",\"startup\":" + String(l.startupRequest ? "true" : "false");
    j += ",\"auto\":"    + String(l.rememberRequest ? "true" : "false");
    return j;
}

bool sameLimits(const RelayLimits& a, const RelayLimits& b)
{
    return socToTenths(a.lockBelowSoc) == socToTenths(b.lockBelowSoc) &&
           socToTenths(a.offBelowSoc)  == socToTenths(b.offBelowSoc)  &&
           socToTenths(a.releaseAtSoc) == socToTenths(b.releaseAtSoc) &&
           a.minOffTimeMs / 1000 == b.minOffTimeMs / 1000 &&
           a.startupRequest == b.startupRequest && a.rememberRequest == b.rememberRequest;
}

/// GET /api/settings: Anlage + je Relais aktuelle Werte und Voreinstellung.
String buildSettingsJson()
{
    const ControlSnapshot      control = readControl();
    const LoadControllerConfig preset  = presetConfig();
    String j;
    j.reserve(1200);
    j  = "{\"installation\":\"";
    j += g_config.installation.load() == Installation::Balcony ? "balcony" : "camper";
    j += "\",\"maxMinOffS\":" + String(RelayLimits::MaxMinOffTimeMs / 1000);
    j += ",\"relays\":[";
    for (size_t i = 0; i < RelayCount; ++i)
    {
        const RelayConfig& r   = control.configs[i];
        const RelayLimits  cur = RelayLimits::from(r);
        const RelayLimits  def = RelayLimits::from(preset.relays[i]);
        if (i > 0) j += ",";
        j += "{\"name\":"         + WebUi::jsonString(r.name);
        j += ",\"enabled\":"      + String(r.enabled ? "true" : "false");
        j += ",\"requiresSoc\":"  + String(r.requiresSoc ? "true" : "false");
        j += ",\"forceOffWithoutSoc\":" + String(r.forceOffWithoutSoc ? "true" : "false");
        j += ",\"custom\":"       + String(sameLimits(cur, def) ? "false" : "true");
        j += "," + limitsFields(cur);
        j += ",\"preset\":{" + limitsFields(def) + "}}";
    }
    j += "]}";
    return j;
}

/// Zahl aus einem Formularfeld (Dezimalkomma erlaubt). false = keine Zahl.
bool parseNumber(String text, float& out)
{
    text.trim();
    text.replace(',', '.');
    if (text.length() == 0 || text.length() > 8)
    {
        return false;
    }
    int dots = 0;
    for (size_t i = 0; i < text.length(); ++i)
    {
        const char c = text[i];
        if (c == '.') { if (++dots > 1) return false; }
        else if (c < '0' || c > '9') return false;
    }
    out = text.toFloat();
    return true;
}

String settingsResponse(bool ok, const String& result)
{
    return "{\"ok\":" + String(ok ? "true" : "false") +
           ",\"result\":" + WebUi::jsonString(result) +
           ",\"settings\":" + buildSettingsJson() + "}";
}

/// POST /api/settings/relay: relay=1..3, lock, off, release (%), minOffS (s),
/// startup=ON/OFF, auto=ON/OFF.
int handleRelaySettings(const ApiRequest& req, String& out)
{
    const long relay = req.arg("relay").toInt();
    if (relay < 1 || relay > static_cast<long>(RelayCount))
    {
        out = "{\"ok\":false,\"error\":\"relay muss 1..3 sein\"}";
        return 400;
    }
    float lock = 0, off = 0, release = 0, minOff = 0;
    const int startup = parseOnOff(req.arg("startup"));
    const int autoOn  = parseOnOff(req.arg("auto"));
    if (!parseNumber(req.arg("lock"), lock) || !parseNumber(req.arg("off"), off) ||
        !parseNumber(req.arg("release"), release) || !parseNumber(req.arg("minOffS"), minOff) ||
        startup < 0 || autoOn < 0)
    {
        out = "{\"ok\":false,\"error\":\"Bitte alle Felder mit gültigen Zahlen ausfüllen\"}";
        return 400;
    }

    RelayLimits l;
    l.lockBelowSoc    = socToTenths(lock) / 10.0f;   // auf 0,1 % runden (wie gespeichert)
    l.offBelowSoc     = socToTenths(off) / 10.0f;
    l.releaseAtSoc    = socToTenths(release) / 10.0f;
    l.minOffTimeMs    = (minOff > RelayLimits::MaxMinOffTimeMs / 1000.0f)
                            ? RelayLimits::MaxMinOffTimeMs + 1000   // -> validate() lehnt ab
                            : static_cast<uint32_t>(minOff + 0.5f) * 1000;
    l.startupRequest  = startup == 1;
    l.rememberRequest = autoOn == 1;

    const char* error = l.validate();
    if (error != nullptr)
    {
        out = "{\"ok\":false,\"error\":" + WebUi::jsonString(error) + "}";
        return 400;
    }

    ControlCommand command;
    command.kind   = CommandKind::Limits;
    command.relay  = static_cast<RelayId>(relay - 1);
    command.limits = l;
    bool ok = false;
    String result = sendWebCommand(command, ok);
    if (ok && !saveRelayLimits(static_cast<size_t>(relay - 1), l))
    {
        ok = false;
        result = "übernommen, aber Speichern im Flash fehlgeschlagen (gilt bis zum Neustart)";
    }
    else if (ok)
    {
        result = "gespeichert";
    }
    out = settingsResponse(ok, result);
    return 200;
}

/******************************************************************************
 * W5: Sicherung / Wiederherstellung aller Einstellungen
 *
 * Die Namensräume und Schlüssel von WLAN ("net") und MQTT ("mqtt") gehören
 * zu lib/WebCore (WifiManager.cpp, MqttManager.cpp) und MÜSSEN mit den
 * dortigen Konstanten übereinstimmen. Passwörter ("pass", "devpw") werden
 * weder gelesen noch geschrieben (Projektinhaber: bleiben auf dem Gerät).
 ******************************************************************************/
const char* const NvsNet  = "net";    // = WifiManager.cpp
const char* const NvsMqtt = "mqtt";   // = MqttManager.cpp

std::string toStd(const String& s) { return std::string(s.c_str()); }

/// Aktuelle Einstellungen aus dem Flash einsammeln.
BackupData collectBackup()
{
    BackupData d;
    d.firmware = FirmwareVersion;
    d.created  = toStd(formatLocalTime(clockNowUnix()));
    d.balcony  = SettingsStore::getUInt(NvsApp, "install", 0) == 1;

    const LoadControllerConfig preset = d.balcony ? LoadControllerConfig::balcony()
                                                  : LoadControllerConfig::camper();
    for (size_t i = 0; i < RelayCount; ++i)
    {
        // Strategie C: immer die wirksamen Werte (gespeicherte oder Voreinstellung)
        const RelayLimits def = RelayLimits::from(preset.relays[i]);
        if (!loadRelayLimits(i, def, d.relays[i]))
        {
            d.relays[i] = def;
        }
    }

    d.tz  = toStd(g_tzKey);
    d.ntp = g_ntpServer;

    d.mqttEnabled  = SettingsStore::getBool(NvsMqtt, "enabled", false);
    d.mqttHost     = toStd(SettingsStore::getString(NvsMqtt, "host"));
    d.mqttPort     = SettingsStore::getUInt(NvsMqtt, "port", 1883);
    d.mqttUser     = toStd(SettingsStore::getString(NvsMqtt, "user"));
    d.mqttBase     = toStd(SettingsStore::getString(NvsMqtt, "base", "vedirect"));
    d.mqttInterval = SettingsStore::getUInt(NvsMqtt, "interval", 10);
    d.mqttClient   = toStd(SettingsStore::getString(NvsMqtt, "client"));

    d.wifiSsid = toStd(SettingsStore::getString(NvsNet, "ssid"));
    d.apAlways = SettingsStore::getBool(NvsNet, "apalways", false);
    d.hostname = toStd(SettingsStore::getString(NvsNet, "host"));
    return d;
}

/// Geprüfte Sicherung in den Flash schreiben (danach Neustart!).
/// Passwörter bleiben unverändert. false = Schreibfehler (teilweise geschrieben).
bool applyBackup(const BackupData& d)
{
    bool ok = true;
    ok &= SettingsStore::putUInt(NvsApp, "install", d.balcony ? 1 : 0);

    // Strategie C: nur Werte speichern, die von der Voreinstellung der
    // (wiederhergestellten) Anlage abweichen; sonst gilt die Voreinstellung.
    SettingsStore::clear(NvsRelays);
    const LoadControllerConfig preset = d.balcony ? LoadControllerConfig::balcony()
                                                  : LoadControllerConfig::camper();
    for (size_t i = 0; i < RelayCount; ++i)
    {
        if (preset.relays[i].enabled && !sameLimits(d.relays[i], RelayLimits::from(preset.relays[i])))
        {
            ok &= saveRelayLimits(i, d.relays[i]);
        }
    }

    ok &= SettingsStore::putString(NvsClock, "tz", d.tz.c_str());
    ok &= SettingsStore::putString(NvsClock, "ntp", d.ntp.c_str());

    ok &= SettingsStore::putBool(NvsMqtt, "enabled", d.mqttEnabled);
    ok &= SettingsStore::putString(NvsMqtt, "host", d.mqttHost.c_str());
    ok &= SettingsStore::putUInt(NvsMqtt, "port", d.mqttPort);
    ok &= SettingsStore::putString(NvsMqtt, "user", d.mqttUser.c_str());
    ok &= SettingsStore::putString(NvsMqtt, "base", d.mqttBase.c_str());
    ok &= SettingsStore::putUInt(NvsMqtt, "interval", d.mqttInterval);
    ok &= SettingsStore::putString(NvsMqtt, "client", d.mqttClient.c_str());

    if (d.wifiSsid.empty())
    {
        SettingsStore::remove(NvsNet, "ssid");
    }
    else
    {
        ok &= SettingsStore::putString(NvsNet, "ssid", d.wifiSsid.c_str());
    }
    ok &= SettingsStore::putBool(NvsNet, "apalways", d.apAlways);
    if (d.hostname.empty())
    {
        SettingsStore::remove(NvsNet, "host");
    }
    else
    {
        ok &= SettingsStore::putString(NvsNet, "host", d.hostname.c_str());
    }
    return ok;
}

/// Kurzfassung für die Vorschau: Liste [["Bereich","Inhalt"],...] als JSON.
String backupSummaryJson(const BackupData& d)
{
    auto row = [](const String& k, const String& v) {
        return "[" + WebUi::jsonString(k) + "," + WebUi::jsonString(v) + "]";
    };
    const LoadControllerConfig preset = d.balcony ? LoadControllerConfig::balcony()
                                                  : LoadControllerConfig::camper();
    String j = "[";
    j += row("Gesichert", String(d.created.empty() ? "(ohne Datum)" : d.created.c_str()) +
                          ", Firmware " + d.firmware.c_str());
    j += "," + row("Anlage", d.balcony ? "Balkonkraftwerk" : "Camper");
    for (size_t i = 0; i < RelayCount; ++i)
    {
        String v;
        if (!preset.relays[i].enabled)      v = "deaktiviert";
        else if (sameLimits(d.relays[i], RelayLimits::from(preset.relays[i])))
                                            v = "Voreinstellung";
        else
        {
            const RelayLimits& l = d.relays[i];
            v = "Sperre " + String(l.lockBelowSoc, 1) + " %, Aus " + String(l.offBelowSoc, 1) +
                " %, Frei " + String(l.releaseAtSoc, 1) + " %, Auszeit " + String(l.minOffTimeMs / 1000) +
                " s, Start " + (l.startupRequest ? "ein" : "aus") + ", autom. " + (l.rememberRequest ? "ja" : "nein");
            v.replace('.', ',');
        }
        j += "," + row("R" + String(i + 1) + " " + preset.relays[i].name, v);
    }
    j += "," + row("Uhr", String(Rtc::timeZone(d.tz.c_str()).label) + ", NTP " + d.ntp.c_str());
    j += "," + row("MQTT", d.mqttEnabled ? String("ein, ") + d.mqttHost.c_str() + ":" + String(d.mqttPort) +
                                               ", Topic " + d.mqttBase.c_str() + ", alle " + String(d.mqttInterval) + " s"
                                         : String("aus"));
    j += "," + row("WLAN", d.wifiSsid.empty() ? String("nicht eingerichtet")
                                              : String(d.wifiSsid.c_str()) + (d.apAlways ? ", Access Point immer an" : ""));
    j += "," + row("Passwörter", "bleiben unverändert (sind nicht in der Datei)");
    return j + "]";
}

/// POST /api/backup/check und /restore: Datei prüfen (und ggf. übernehmen).
int handleBackupUpload(const ApiRequest& req, String& out, bool restore)
{
    const String text = req.arg("data");
    BackupData d;
    std::string error;
    if (!backupFromJson(text.c_str(), d, error))
    {
        out = "{\"ok\":false,\"error\":" + WebUi::jsonString(String(error.c_str())) + "}";
        return 400;
    }
    if (!restore)
    {
        out = "{\"ok\":true,\"summary\":" + backupSummaryJson(d) + "}";
        return 200;
    }
    const bool ok = applyBackup(d);
    addEvent(ok ? Severity::Warning : Severity::Critical,
             ok ? "Einstellungen aus Sicherung wiederhergestellt, Neustart (Befehl WebUI)"
                : "Wiederherstellen: Speichern im Flash teilweise fehlgeschlagen");
    g_webUi.requestRestart(1500);
    out = "{\"ok\":" + String(ok ? "true" : "false") + ",\"result\":" +
          WebUi::jsonString(ok ? "wiederhergestellt, Gerät startet neu"
                               : "Speichern teilweise fehlgeschlagen, Gerät startet neu – Einstellungen prüfen") + "}";
    return 200;
}

/******************************************************************************
 * W4b Schritt 4: HEX-Daten für die WebUI (MPPT-Historie; Einstellungen folgen)
 ******************************************************************************/
HexRegisterCache g_webHexCache;   ///< Kopie für die webTask (~2 KB, nicht auf den Stack)

/// Kanal zu einer Rolle ("shunt"/"mppt"), -1 = Gerät nicht vorhanden.
int roleChannel(bool shunt)
{
    const SystemState state = readSystemState();
    const int index = shunt ? state.batteryChannel : state.pvChannel;
    return (index >= 0 && index < static_cast<int>(ChannelCount)) ? index : -1;
}

const char* hexStateKey(const HexRegisterCache::Entry* e)
{
    if (e == nullptr) return "missing";   // (noch) nicht gelesen
    switch (e->state)
    {
        case HexRegisterCache::State::Ok:       return "ok";
        case HexRegisterCache::State::NoData:   return "nodata";
        case HexRegisterCache::State::Unknown:  return "unknown";
        case HexRegisterCache::State::NoAnswer: return "noanswer";
    }
    return "missing";
}

/// Zahl mit Nachkommastellen als JSON (Rohwert / divisor).
String jsonScaled(uint32_t raw, float divisor, unsigned decimals)
{
    return String(raw / divisor, decimals);
}

/// MPPT-Fehlercodes (jüngster zuerst) als Textliste, 0 = kein Fehler -> weglassen.
String jsonErrorList(const uint8_t errors[4])
{
    static const FieldDefinition* err = FieldCatalog::find("ERR");
    String j = "[";
    bool first = true;
    for (int i = 0; i < 4; ++i)
    {
        if (errors[i] == 0) continue;
        const char* text = err ? FieldCatalog::codeText(*err, errors[i]) : nullptr;
        if (!first) j += ",";
        first = false;
        j += WebUi::jsonString(text ? String(text) + " (" + errors[i] + ")"
                                    : "Fehler " + String(errors[i]));
    }
    return j + "]";
}

/// MPPT-Historie aus dem Zwischenspeicher (g_webHexCache) als JSON.
String buildHistoryJson()
{
    String j;
    j.reserve(9000);

    // Gesamt
    const HexRegisterCache::Entry* t = g_webHexCache.find(VeHex::RegHistoryTotal);
    VeHex::HistoryTotal total;
    const bool totalOk = t && t->state == HexRegisterCache::State::Ok &&
                         VeHex::decodeHistoryTotal(t->value, t->length, total);
    j += "{\"total\":{\"state\":\"" + String(totalOk ? "ok" : hexStateKey(t)) + "\"";
    if (totalOk)
    {
        j += ",\"userYieldKWh\":"   + jsonScaled(total.userYieldWh, 1000.0f, 2);
        j += ",\"systemYieldKWh\":" + jsonScaled(total.systemYieldWh, 1000.0f, 2);
        j += ",\"panelMaxV\":"      + jsonScaled(total.panelMaxCentiV, 100.0f, 2);
        j += ",\"batteryMaxV\":"    + jsonScaled(total.batteryMaxCentiV, 100.0f, 2);
        j += ",\"batteryMinV\":"    + (total.batteryMinValid ? jsonScaled(total.batteryMinCentiV, 100.0f, 2)
                                                             : String("null"));
        j += ",\"daysAvailable\":"  + String(total.daysAvailable);
        j += ",\"errors\":"         + jsonErrorList(total.errors);
    }
    j += "},\"days\":[";

    // Tage: 0 = heute, 1 = gestern, ...
    for (uint8_t ago = 0; ago < VeHex::HistoryDays; ++ago)
    {
        const HexRegisterCache::Entry* e = g_webHexCache.find(VeHex::RegHistoryToday + ago);
        VeHex::HistoryDay d;
        bool ok = false;
        if (e && (e->state == HexRegisterCache::State::Ok || e->state == HexRegisterCache::State::NoData))
        {
            ok = VeHex::decodeHistoryDay(e->value, e->length, e->flags, d);
        }
        if (ago > 0) j += ",";
        j += "{\"ago\":" + String(ago);
        if (!ok || !d.valid)
        {
            j += ",\"state\":\"" + String(ok ? "nodata" : hexStateKey(e)) + "\"}";
            continue;
        }
        j += ",\"state\":\"ok\"";
        j += ",\"yieldKWh\":"      + jsonScaled(d.yieldWh, 1000.0f, 2);
        j += ",\"consumedKWh\":"   + (d.consumedValid ? jsonScaled(d.consumedWh, 1000.0f, 2) : String("null"));
        j += ",\"batteryMaxV\":"   + jsonScaled(d.batteryMaxCentiV, 100.0f, 2);
        j += ",\"batteryMinV\":"   + (d.batteryMinValid ? jsonScaled(d.batteryMinCentiV, 100.0f, 2)
                                                            : String("null"));   // 0 = nicht erfasst
        j += ",\"bulkMin\":"       + String(d.bulkMin);
        j += ",\"absorptionMin\":" + String(d.absorptionMin);
        j += ",\"floatMin\":"      + String(d.floatMin);
        j += ",\"powerMaxW\":"     + String(d.powerMaxW);
        j += ",\"currentMaxA\":"   + jsonScaled(d.batteryCurrentMaxDeciA, 10.0f, 1);
        j += ",\"panelMaxV\":"     + jsonScaled(d.panelMaxCentiV, 100.0f, 2);
        j += ",\"errors\":"        + jsonErrorList(d.errors);
        j += "}";
    }
    j += "]}";
    return j;
}

/******************************************************************************
 * W4b Schritt 5: Geräteeinstellungen (nur lesen) – Anzeige
 *
 * Namen beim Shunt wie in VictronConnect (Projektinhaber 11.11.2026).
 * Umrechnung laut Victron-HEX-Doku; Shunt-Werte am 11.11.2026 mit
 * VictronConnect verglichen (alle passend).
 ******************************************************************************/
enum class HexFormat : uint8_t
{
    Number,          ///< Rohwert / divisor, decimals Nachkommastellen
    BatteryVoltage,  ///< MPPT 0xEDEF: 0 = automatisch, sonst Volt
    BatteryType,     ///< MPPT 0xEDF1: Code -> Text
    LoadOutput       ///< MPPT 0xEDAB: Code (untere 4 Bit) -> Text
};

struct HexSetting
{
    uint16_t    reg;
    const char* name;
    HexFormat   format;
    float       divisor;
    uint8_t     decimals;
    bool        isSigned;
    const char* unit;
};

const HexSetting ShuntSettings[] =
{
    { 0x1000, "Batteriekapazität",                  HexFormat::Number, 1,   0, false, "Ah"   },
    { 0x1001, "Spannung wenn aufgeladen",           HexFormat::Number, 10,  1, false, "V"    },
    { 0x1002, "Schweifstrom",                       HexFormat::Number, 10,  1, false, "%"    },
    { 0x1003, "Zeit für Ladezustandserkennung",     HexFormat::Number, 1,   0, false, "min"  },
    { 0x1004, "Ladewirkungsgrad",                   HexFormat::Number, 1,   0, false, "%"    },
    { 0x1005, "Peukert-Exponent",                   HexFormat::Number, 100, 2, false, ""     },
    { 0x1006, "Strom-Schwellwert",                  HexFormat::Number, 100, 2, false, "A"    },
    { 0x1007, "Durchschnittliche Restlaufzeit",     HexFormat::Number, 1,   0, false, "min"  },
    { 0x1008, "Entladungsboden",                    HexFormat::Number, 10,  1, false, "%"    },
};

const HexSetting MpptSettings[] =
{
    { 0xEDEF, "Batteriespannung",                   HexFormat::BatteryVoltage, 1, 0, false, "V" },
    { 0xEDF7, "Absorptionsspannung",                HexFormat::Number, 100, 2, false, "V"    },
    { 0xEDF6, "Erhaltungsspannung",                 HexFormat::Number, 100, 2, false, "V"    },
    { 0xEDF0, "Max. Ladestrom",                     HexFormat::Number, 10,  1, false, "A"    },
    { 0xEDF1, "Batterietyp",                        HexFormat::BatteryType, 1, 0, false, ""  },
    { 0xEDF2, "Temperaturkompensation",             HexFormat::Number, 100, 2, true,  "mV/K" },
    { 0xEDAB, "Lastausgang",                        HexFormat::LoadOutput, 1, 0, false, ""   },
};

/// Batterietyp (MPPT mit Drehschalter, z. B. 100/50). Die Victron-Doku
/// (Rev 18) nummeriert hier nicht eindeutig -> Code immer mit anzeigen.
const char* batteryTypeText(uint32_t code)
{
    switch (code)
    {
        case 1:   return "GEL Victron Long Life (14,1 V)";
        case 2:   return "GEL Victron Deep Discharge (14,3 V)";
        case 3:   return "GEL Victron Deep Discharge (14,4 V)";
        case 4:   return "AGM Victron Deep Discharge (14,7 V)";
        case 5:   return "Röhrenplatte, Zyklus 1 (14,9 V)";
        case 6:   return "Röhrenplatte, Zyklus 2 (15,1 V)";
        case 7:   return "Röhrenplatte, Zyklus 3 (15,3 V)";
        case 8:   return "LiFePO4 (14,2 V)";
        case 255: return "benutzerdefiniert";
    }
    return nullptr;
}

const char* loadOutputText(uint32_t code)
{
    switch (code & 0x0F)
    {
        case 0: return "aus";
        case 1: return "automatisch (BatteryLife)";
        case 2: return "Alternative 1 (aus < 11,1 V, ein > 13,1 V)";
        case 3: return "Alternative 2 (aus < 11,8 V, ein > 14,0 V)";
        case 4: return "immer ein (ohne Entladeschutz)";
        case 5: return "benutzerdefiniert 1";
        case 6: return "benutzerdefiniert 2";
        case 7: return "Automatic Energy Selector";
    }
    return nullptr;
}

/// Wert eines Registers als Anzeigetext (Dezimalkomma).
String formatHexSetting(const HexSetting& s, const HexRegisterCache::Entry& e)
{
    const uint32_t raw = VeHex::readUnsigned(e.value, e.length);
    switch (s.format)
    {
        case HexFormat::BatteryVoltage:
            return raw == 0 ? String("automatisch") : String(raw) + " V";
        case HexFormat::BatteryType:
        {
            const char* t = batteryTypeText(raw);
            return t ? String(t) + " (Typ " + String(raw) + ")" : "Typ " + String(raw);
        }
        case HexFormat::LoadOutput:
        {
            const char* t = loadOutputText(raw);
            return t ? String(t) : "Code " + String(raw);
        }
        case HexFormat::Number:
        default:
        {
            const float v = (s.isSigned ? static_cast<float>(VeHex::readSigned(e.value, e.length))
                                        : static_cast<float>(raw)) / s.divisor;
            String text = String(v, static_cast<unsigned int>(s.decimals));
            text.trim();   // Finding 12.11.2026: Arduino füllt bei 0 Nachkommastellen links auf (" 3")
            text.replace('.', ',');
            return s.unit[0] ? text + " " + s.unit : text;
        }
    }
}

/// Einstellungen als JSON-Liste [{name, value, state}] aus g_webHexCache.
/// Register, die das Gerät nicht kennt (z. B. Lastausgang ohne Lastausgang),
/// werden weggelassen.
String buildSettingsListJson(bool shunt)
{
    const HexSetting* list  = shunt ? ShuntSettings : MpptSettings;
    const size_t      count = shunt ? sizeof(ShuntSettings) / sizeof(ShuntSettings[0])
                                    : sizeof(MpptSettings) / sizeof(MpptSettings[0]);
    String j = "[";
    bool first = true;
    for (size_t i = 0; i < count; ++i)
    {
        const HexRegisterCache::Entry* e = g_webHexCache.find(list[i].reg);
        if (e != nullptr && e->state == HexRegisterCache::State::Unknown)
        {
            continue;
        }
        // Finding 12.11.2026: Der SmartSolar 100/50 beantwortet 0xEDAB, obwohl
        // er keinen Lastausgang hat (nur "virtuell" über den TX-Pin, den wir
        // selbst nutzen). Lastausgang nur zeigen, wenn Fähigkeit Bit 0 gesetzt.
        if (list[i].format == HexFormat::LoadOutput)
        {
            const HexRegisterCache::Entry* cap = g_webHexCache.find(VeHex::RegCapabilities);
            const bool hasLoad = cap != nullptr && cap->state == HexRegisterCache::State::Ok &&
                                 (VeHex::readUnsigned(cap->value, cap->length) & VeHex::CapLoadOutput) != 0;
            if (!hasLoad)
            {
                continue;
            }
        }
        const bool ok = e != nullptr && e->state == HexRegisterCache::State::Ok && e->length > 0;
        if (!first) j += ",";
        first = false;
        j += "{\"name\":" + WebUi::jsonString(list[i].name);
        j += ",\"state\":\"" + String(ok ? "ok" : hexStateKey(e)) + "\"";
        j += ",\"value\":" + (ok ? WebUi::jsonString(formatHexSetting(list[i], *e)) : String("null"));
        j += "}";
    }
    return j + "]";
}

/// GET /api/hex?role=shunt|mppt: Zustand der Abfrage + gelesene Daten.
String buildHexJson(bool shunt)
{
    const int index = roleChannel(shunt);
    if (index < 0)
    {
        return "{\"present\":false}";
    }
    Channel& channel = g_channels[index];
    {
        StateLock lock(channel.stateMutex);
        g_webHexCache = channel.hexCache;
    }
    bool pending = false;
    portENTER_CRITICAL(&g_statusLock);
    pending = channel.hexRefreshRequested;
    const ChannelStatus status = channel.status;
    portEXIT_CRITICAL(&g_statusLock);
    const uint32_t now = millis();

    const HexJobRunner::Result& r = status.hexJob;
    String j;
    j.reserve(9500);
    j  = "{\"present\":true";
    j += ",\"uart\":"     + WebUi::jsonString(channel.name);
    j += ",\"device\":"   + WebUi::jsonString(status.device.device ? status.device.device->name : "unbekanntes Gerät");
    j += ",\"pending\":"  + String(pending ? "true" : "false");
    j += ",\"busy\":"     + String(status.hexBusy ? "true" : "false");
    j += ",\"job\":{\"finished\":" + String(r.finished ? "true" : "false");
    j += ",\"aborted\":"   + String(r.aborted ? "true" : "false");
    j += ",\"requested\":" + String(r.requested);
    j += ",\"answered\":"  + String(r.answered);
    j += ",\"failed\":"    + String(r.failed);
    j += ",\"ageS\":"      + String(r.finished ? (now - r.finishedMs) / 1000 : 0);
    j += "}";
    // W4e: heutiges Datum (Ortszeit des Geräts) -> WebUI zeigt die
    // Historientage mit Datum statt "vor n Tagen"
    if (clockValid())
    {
        const time_t t = static_cast<time_t>(clockNowUnix());
        struct tm lt;
        localtime_r(&t, &lt);
        j += ",\"today\":{\"y\":" + String(lt.tm_year + 1900) + ",\"m\":" + String(lt.tm_mon + 1) +
             ",\"d\":" + String(lt.tm_mday) + "}";
    }
    j += ",\"settings\":" + buildSettingsListJson(shunt);
    if (!shunt)
    {
        j += ",\"history\":" + buildHistoryJson();
    }
    j += "}";
    return j;
}

void registerProjectApi()
{
    g_webUi.setStatusExtension(buildStatusExtension);

    g_webUi.addApi("/api/faults", WebUi::Method::Get,
                   [](const ApiRequest&, String& out) {
                       out = buildFaultsJson();
                       return 200;
                   });

    g_webUi.addApi("/api/device", WebUi::Method::Get,
                   [](const ApiRequest& req, String& out) {
                       const String role = req.arg("role");
                       if (role != "shunt" && role != "mppt")
                       {
                           out = "{\"ok\":false,\"error\":\"role muss shunt oder mppt sein\"}";
                           return 400;
                       }
                       out = buildDeviceJson(role == "shunt");
                       return 200;
                   });

    g_webUi.addApi("/api/live", WebUi::Method::Get,
                   [](const ApiRequest&, String& out) {
                       out = buildLiveJson();
                       return 200;
                   });

    g_webUi.addApi("/api/relay", WebUi::Method::Post,
                   [](const ApiRequest& req, String& out) {
                       const long relay = req.arg("relay").toInt();
                       const int  value = parseOnOff(req.arg("on"));
                       if (relay < 1 || relay > static_cast<long>(RelayCount) || value < 0)
                       {
                           out = "{\"ok\":false,\"error\":\"relay muss 1..3 und on ON/OFF sein\"}";
                           return 400;
                       }
                       ControlCommand command;
                       command.kind  = CommandKind::Relay;
                       command.relay = static_cast<RelayId>(relay - 1);
                       command.on    = value == 1;
                       bool ok = false;
                       const String result = sendWebCommand(command, ok);
                       out = commandResponse(ok, result);
                       return 200;
                   });

    g_webUi.addApi("/api/override", WebUi::Method::Post,
                   [](const ApiRequest& req, String& out) {
                       const int value = parseOnOff(req.arg("on"));
                       if (value < 0)
                       {
                           out = "{\"ok\":false,\"error\":\"on muss ON oder OFF sein\"}";
                           return 400;
                       }
                       ControlCommand command;
                       command.kind = CommandKind::Override;
                       command.on   = value == 1;
                       bool ok = false;
                       const String result = sendWebCommand(command, ok);
                       out = commandResponse(ok, result);
                       return 200;
                   });

    // ---- W5: Sicherung / Wiederherstellung ----
    g_webUi.addApi("/api/backup", WebUi::Method::Get,
                   [](const ApiRequest&, String& out) {
                       out = backupToJson(collectBackup()).c_str();
                       return 200;
                   });
    g_webUi.addApi("/api/backup/check", WebUi::Method::Post,
                   [](const ApiRequest& req, String& out) { return handleBackupUpload(req, out, false); });
    g_webUi.addApi("/api/backup/restore", WebUi::Method::Post,
                   [](const ApiRequest& req, String& out) { return handleBackupUpload(req, out, true); });

    // ---- W4e Schritt 3: Uhr ----
    g_webUi.addApi("/api/clock", WebUi::Method::Get,
                   [](const ApiRequest&, String& out) {
                       out = buildClockJson();
                       return 200;
                   });

    // Zeitzone und NTP-Server speichern und sofort anwenden
    g_webUi.addApi("/api/clock", WebUi::Method::Post,
                   [](const ApiRequest& req, String& out) {
                       String tz = req.arg("tz");
                       String ntp = req.arg("ntp");
                       ntp.trim();
                       if (std::strcmp(Rtc::timeZone(tz.c_str()).key, tz.c_str()) != 0)
                       {
                           out = "{\"ok\":false,\"error\":\"Unbekannte Zeitzone\"}";
                           return 400;
                       }
                       if (!Rtc::isValidHostName(ntp.c_str()))
                       {
                           out = "{\"ok\":false,\"error\":\"NTP-Server: nur Name oder IP-Adresse (Buchstaben, Ziffern, Punkt, Bindestrich)\"}";
                           return 400;
                       }
                       const bool okTz  = SettingsStore::putString(NvsClock, "tz", tz);
                       const bool okNtp = SettingsStore::putString(NvsClock, "ntp", ntp);
                       const bool ntpChanged = ntp != g_ntpServer;
                       g_tzKey = tz;
                       applyTimeZone();
                       if (ntpChanged)
                       {
                           sntp_stop();   // vor dem Ändern des Puffers (SNTP hält nur den Zeiger)
                           std::strncpy(g_ntpServer, ntp.c_str(), sizeof(g_ntpServer) - 1);
                           g_ntpServer[sizeof(g_ntpServer) - 1] = '\0';
                       }
                       startNtp();    // neuer Server/Zeitzone, sofortiger Abgleich
                       addEvent(Severity::Info, "Uhr-Einstellungen geändert: %s, NTP %s",
                                Rtc::timeZone(g_tzKey.c_str()).label, g_ntpServer);
                       out = "{\"ok\":" + String(okTz && okNtp ? "true" : "false") +
                             ",\"result\":" + WebUi::jsonString(okTz && okNtp ? "gespeichert"
                                                                     : "übernommen, aber Speichern im Flash fehlgeschlagen") +
                             ",\"clock\":" + buildClockJson() + "}";
                       return 200;
                   });

    // Browserzeit (automatisch von der WebUI, nur wenn keine gültige Zeit)
    g_webUi.addApi("/api/time", WebUi::Method::Post,
                   [](const ApiRequest& req, String& out) {
                       const uint32_t unix = static_cast<uint32_t>(strtoul(req.arg("unix").c_str(), nullptr, 10));
                       const bool applied = setClockFromBrowser(unix);
                       out = "{\"ok\":true,\"applied\":" + String(applied ? "true" : "false") + "}";
                       return 200;
                   });

    // ---- W4b Schritt 4: HEX-Daten (nur lesen) ----
    g_webUi.addApi("/api/hex", WebUi::Method::Get,
                   [](const ApiRequest& req, String& out) {
                       const String role = req.arg("role");
                       if (role != "shunt" && role != "mppt")
                       {
                           out = "{\"ok\":false,\"error\":\"role muss shunt oder mppt sein\"}";
                           return 400;
                       }
                       out = buildHexJson(role == "shunt");
                       return 200;
                   });

    // Neu einlesen (Reiter geöffnet oder Knopf). Startet, sobald das Gerät
    // erkannt ist und keine Abfrage läuft. Kein automatisches Wiederholen.
    g_webUi.addApi("/api/hex/refresh", WebUi::Method::Post,
                   [](const ApiRequest& req, String& out) {
                       const String role = req.arg("role");
                       if (role != "shunt" && role != "mppt")
                       {
                           out = "{\"ok\":false,\"error\":\"role muss shunt oder mppt sein\"}";
                           return 400;
                       }
                       const int index = roleChannel(role == "shunt");
                       if (index < 0)
                       {
                           out = "{\"ok\":false,\"error\":\"Gerät nicht erkannt\"}";
                           return 200;
                       }
                       requestHexRefresh(static_cast<size_t>(index));
                       out = "{\"ok\":true}";
                       return 200;
                   });

    // ---- W4a Schritt 5: Einstellungen ----
    g_webUi.addApi("/api/settings", WebUi::Method::Get,
                   [](const ApiRequest&, String& out) {
                       out = buildSettingsJson();
                       return 200;
                   });

    g_webUi.addApi("/api/settings/relay", WebUi::Method::Post, handleRelaySettings);

    // Relais auf die Voreinstellung der Anlage (ohne Neustart).
    // Ohne Parameter alle Relais, mit relay=1..3 nur dieses eine.
    g_webUi.addApi("/api/settings/defaults", WebUi::Method::Post,
                   [](const ApiRequest& req, String& out) {
                       ControlCommand command;
                       command.kind = CommandKind::ResetLimits;
                       if (req.hasArg("relay"))
                       {
                           const long relay = req.arg("relay").toInt();
                           if (relay < 1 || relay > static_cast<long>(RelayCount))
                           {
                               out = "{\"ok\":false,\"error\":\"relay muss 1..3 sein\"}";
                               return 400;
                           }
                           command.allRelays = false;
                           command.relay     = static_cast<RelayId>(relay - 1);
                       }
                       bool ok = false;
                       String result = sendWebCommand(command, ok);
                       if (ok && command.allRelays)
                       {
                           SettingsStore::clear(NvsRelays);
                           result = "Voreinstellung wiederhergestellt";
                       }
                       else if (ok)
                       {
                           // Nur die Schlüssel dieses Relais löschen -> beim Start Voreinstellung
                           const size_t i = static_cast<size_t>(command.relay);
                           const char* keys[] = { "set", "lock", "off", "rel", "minoff", "start", "auto" };
                           for (const char* k : keys)
                           {
                               SettingsStore::remove(NvsRelays, relayKey(i, k).c_str());
                           }
                           result = "Voreinstellung wiederhergestellt";
                       }
                       out = settingsResponse(ok, result);
                       return 200;
                   });

    // Anlage wechseln: speichern, Relais-Änderungen löschen, Neustart
    g_webUi.addApi("/api/settings/installation", WebUi::Method::Post,
                   [](const ApiRequest& req, String& out) {
                       const String value = req.arg("value");
                       if (value != "camper" && value != "balcony")
                       {
                           out = "{\"ok\":false,\"error\":\"value muss camper oder balcony sein\"}";
                           return 400;
                       }
                       const bool balcony = value == "balcony";
                       if (balcony == (g_config.installation.load() == Installation::Balcony))
                       {
                           out = "{\"ok\":true,\"restart\":false,\"result\":\"unverändert\"}";
                           return 200;
                       }
                       if (!SettingsStore::putUInt(NvsApp, "install", balcony ? 1 : 0))
                       {
                           out = "{\"ok\":false,\"error\":\"Speichern im Flash fehlgeschlagen\"}";
                           return 500;
                       }
                       SettingsStore::clear(NvsRelays);
                       addEvent(Severity::Warning, "Anlage geändert auf %s, Neustart (Befehl WebUI)",
                                balcony ? "Balkonkraftwerk" : "Camper");
                       g_webUi.requestRestart(1500);
                       out = "{\"ok\":true,\"restart\":true,\"result\":\"gespeichert, Gerät startet neu\"}";
                       return 200;
                   });
}

/// "ON"/"OFF", "1"/"0", "true"/"false" (Groß/klein egal). -1 = ungültig.
int parseOnOff(String payload)
{
    payload.trim();
    payload.toUpperCase();
    if (payload == "ON"  || payload == "1" || payload == "TRUE"  || payload == "EIN") return 1;
    if (payload == "OFF" || payload == "0" || payload == "FALSE" || payload == "AUS") return 0;
    return -1;
}

/// Eingehender MQTT-Befehl -> Befehls-Queue der systemTask (Quelle MQTT).
void onMqttMessage(const String& subtopic, const String& payload)
{
    const int value = parseOnOff(payload);
    ControlCommand command;
    command.source = CommandSource::Mqtt;

    if (subtopic == "override-set")
    {
        command.kind = CommandKind::Override;
    }
    else if (subtopic == "relay/r1-set") { command.kind = CommandKind::Relay; command.relay = RelayId::Main; }
    else if (subtopic == "relay/r2-set") { command.kind = CommandKind::Relay; command.relay = RelayId::Light; }
    else if (subtopic == "relay/r3-set") { command.kind = CommandKind::Relay; command.relay = RelayId::Fridge; }
    else
    {
        return;   // fremdes Topic
    }

    if (value < 0)
    {
        g_mqtt.publish("command-result",
                       "{\"command\":" + WebUi::jsonString(subtopic) +
                       ",\"value\":" + WebUi::jsonString(payload) +
                       ",\"result\":\"abgelehnt: Wert muss ON oder OFF sein\"}", false);
        return;
    }
    command.on = (value == 1);
    xQueueSend(g_commandQueue, &command, 0);
}

void mqttTask(void*)
{
    // Client-ID: ve-direct-XXXXXX (aus der MAC, eindeutig je Gerät)
    char clientId[32];
    snprintf(clientId, sizeof(clientId), "%s-%06llX", DefaultHostname,
             static_cast<unsigned long long>(ESP.getEfuseMac() & 0xFFFFFF));
    g_mqtt.begin(clientId);
    g_mqtt.onMessage(onMqttMessage);
    g_mqtt.subscribe("relay/r1-set");
    g_mqtt.subscribe("relay/r2-set");
    g_mqtt.subscribe("relay/r3-set");
    g_mqtt.subscribe("override-set");

    uint32_t lastFullMs     = 0;
    uint32_t lastConnects   = 0;
    uint32_t lastSwitchSum  = 0;
    bool     lastOverride   = false;
    uint32_t lastFaults     = 0;
    uint32_t lastCommandCnt = readControl().commandCount;

    for (;;)
    {
        const uint32_t now = millis();
        const NetStatus net = readNetStatus();
        g_mqtt.loop(now, net.staConnected);

        if (g_mqtt.connected())
        {
            const SystemState     state   = readSystemState();
            const ControlSnapshot control = readControl();
            const MqttStatus      mst     = g_mqtt.status();

            uint32_t switchSum = 0;
            for (size_t i = 0; i < RelayCount; ++i) switchSum += control.relays[i].switchCount;

            const bool reconnected = mst.connectCount != lastConnects;
            const bool due = reconnected ||
                             static_cast<uint32_t>(now - lastFullMs) >= g_mqtt.intervalMs();
            const bool controlChanged = switchSum != lastSwitchSum ||
                                        control.overrideActive != lastOverride;
            const bool faultsChanged  = state.faults != lastFaults;

            if (due)
            {
                g_mqtt.publish("battery", buildBatteryJson(state), true);
                g_mqtt.publish("pv",      buildPvJson(state), true);
                lastFullMs   = now;
                lastConnects = mst.connectCount;
            }
            if (due || controlChanged)
            {
                g_mqtt.publish("relays", buildRelaysJson(control), true);
                lastSwitchSum = switchSum;
                lastOverride  = control.overrideActive;
            }
            if (due || faultsChanged)
            {
                g_mqtt.publish("system", buildSystemJson(state, net), true);
                lastFaults = state.faults;
            }

            // Rückmeldung zu MQTT-Befehlen
            if (control.commandCount != lastCommandCnt)
            {
                lastCommandCnt = control.commandCount;
                if (control.lastCommand.source == CommandSource::Mqtt)
                {
                    const String cmd = control.lastCommand.kind == CommandKind::Override
                        ? String("override-set")
                        : "relay/r" + String(static_cast<unsigned>(control.lastCommand.relay) + 1) + "-set";
                    const char* result = control.lastCommand.kind == CommandKind::Override
                        ? "ausgeführt" : LoadController::resultText(control.lastResult);
                    g_mqtt.publish("command-result",
                                   "{\"command\":\"" + cmd + "\",\"value\":\"" +
                                   onOff(control.lastCommand.on) + "\",\"result\":" +
                                   WebUi::jsonString(result) + "}", false);
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/// Netzwerk-Reset-Taste prüfen (nur in setup(), vor dem Start des WLAN).
void checkResetButton()
{
    pinMode(ResetButtonPin, INPUT);   // IO35: kein interner Pull-up möglich

    if (digitalRead(ResetButtonPin) != LOW)
    {
        return;   // nicht gedrückt
    }

    // Fehlersicher (08.11.2026): Reset erst beim LOSLASSEN nach ResetHoldMs.
    // Ein dauerhaft auf Low liegender Pin (fehlender Pull-up, Kabelbruch,
    // klemmende Taste) löst so NIE einen Reset aus.
    Serial.printf("[RESET] Taste an IO%u gedrückt: %lu s halten, dann loslassen = Netzwerk-Reset\n",
                  static_cast<unsigned>(ResetButtonPin),
                  static_cast<unsigned long>(ResetHoldMs / 1000));

    const uint32_t start = millis();
    while (millis() - start < ResetHoldMs)
    {
        if (digitalRead(ResetButtonPin) != LOW)
        {
            Serial.println("[RESET] Taste zu früh losgelassen, kein Reset.");
            return;
        }
        delay(50);
    }

    Serial.println("[RESET] Jetzt loslassen (innerhalb von 5 s) ...");
    const uint32_t releaseStart = millis();
    while (millis() - releaseStart < ResetReleaseWindowMs)
    {
        if (digitalRead(ResetButtonPin) != LOW)
        {
            WifiManager::factoryResetNetwork();
            Serial.println("[RESET] Netzwerk-Einstellungen gelöscht: WLAN und Gerätepasswort "
                           "zurückgesetzt. Neues Startpasswort siehe Display/Monitor.");
            return;
        }
        delay(50);
    }

    Serial.printf("[RESET] WARNUNG: IO%u liegt dauerhaft auf Low (Pull-up fehlt? Taste klemmt?). "
                  "Kein Reset ausgeführt.\n", static_cast<unsigned>(ResetButtonPin));
}

void displayTask(void*)
{
    // Kurz warten, damit erste Messwerte vorliegen
    vTaskDelay(pdMS_TO_TICKS(3000));

    g_statusDisplay.begin(makeDisplayData(), millis());

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        g_statusDisplay.update(makeDisplayData(), millis());
    }
}

/******************************************************************************
 * Ausgabe-Hilfsfunktionen (nur in der Logger-Task verwenden!)
 ******************************************************************************/

/// Zeitstempel als Sekunden mit Millisekunden ausgeben, z. B. "12.345s".
void printTimestamp(uint32_t ms)
{
    Serial.printf("%6lu.%03lus",
                  static_cast<unsigned long>(ms / 1000),
                  static_cast<unsigned long>(ms % 1000));
}

/// Dauer in Sekunden mit einer Nachkommastelle, z. B. "7,2 s".
void printSeconds(uint32_t ms)
{
    Serial.printf("%lu,%lu s",
                  static_cast<unsigned long>(ms / 1000),
                  static_cast<unsigned long>((ms % 1000) / 100));
}

/// Kanalname und Zeitstempel am Zeilenanfang.
void printLinePrefix(const LogMessage& msg)
{
    Serial.printf("[%s] ", g_channels[msg.channelIndex].name);
    printTimestamp(msg.timestampMs);
    Serial.print("  ");
}

/// Gerät ausgeben: "SmartShunt 500A/50mV (0xC035)" bzw. "unbekanntes Gerät (0x1234)".
void printDevice(const DeviceDetector::Identity& device)
{
    if (device.state != DeviceDetector::State::Identified)
    {
        Serial.print("kein Gerät erkannt");
        return;
    }
    Serial.printf("%s (%s)",
                  device.device != nullptr ? device.device->name : "unbekanntes Gerät",
                  device.productId);
}

/// Wert ausgeben; nicht druckbare Zeichen erscheinen als <0xNN>.
void printRawValue(const char* value)
{
    for (const char* p = value; *p != '\0'; ++p)
    {
        const uint8_t c = static_cast<uint8_t>(*p);
        if (c >= 32 && c <= 126)
        {
            Serial.write(c);
        }
        else
        {
            Serial.printf("<0x%02X>", c);
        }
    }
}

/// Anzahl der Zeichen eines UTF-8-Texts (Umlaute zählen als 1 Zeichen).
size_t utf8Length(const char* text)
{
    size_t count = 0;
    for (const char* p = text; *p != '\0'; ++p)
    {
        // Folgebytes einer UTF-8-Sequenz haben das Muster 10xxxxxx
        if ((static_cast<uint8_t>(*p) & 0xC0) != 0x80)
        {
            ++count;
        }
    }
    return count;
}

/// Text ausgeben und mit Leerzeichen auf "width" Zeichen auffüllen
/// (mindestens ein Leerzeichen Abstand).
void printPadded(const char* text, size_t width)
{
    Serial.print(text);
    const size_t length = utf8Length(text);
    for (size_t i = length; i < width; ++i)
    {
        Serial.write(' ');
    }
    if (length >= width)
    {
        Serial.write(' ');
    }
}

/******************************************************************************
 * Kopfzeile eines Blocks: Kanal, Zeit, Herkunft/Gerät, Zähler.
 ******************************************************************************/
void printFrameHeader(const LogMessage& msg)
{
    printLinePrefix(msg);

    switch (msg.origin)
    {
        case DeviceDetector::Origin::Own:
            printDevice(msg.status.device);
            break;

        case DeviceDetector::Origin::Inherited:
            printDevice(msg.status.device);
            Serial.print(" - Folgeblock ohne PID");
            break;

        case DeviceDetector::Origin::Unassigned:
            Serial.print("nicht zuordenbarer Block ohne PID");
            if (msg.status.device.state == DeviceDetector::State::Identified)
            {
                Serial.print(" (Kanalgerät ");
                printDevice(msg.status.device);
                Serial.print(" ist kein Batteriemonitor)");
            }
            else
            {
                Serial.print(" (noch kein Gerät erkannt)");
            }
            break;
    }

    Serial.printf("  |  Block #%lu OK  Felder=%u  Pruefsumme=0x%02X\n",
                  static_cast<unsigned long>(msg.status.parser.framesOk),
                  static_cast<unsigned>(msg.frame.fieldCount),
                  msg.frame.checksum);
}

/******************************************************************************
 * Gültigen Block ausgeben, eine Zeile je Feld:
 *
 *   Klarname          umgerechneter Wert          Label = Rohwert
 *   Batteriespannung  12,327 V                    V = 12327
 ******************************************************************************/
void printFrame(const LogMessage& msg)
{
    printFrameHeader(msg);

    char text[ValueFormatter::BufferSize];

    for (size_t i = 0; i < msg.frame.fieldCount; ++i)
    {
        const VeDirectField& field = msg.frame.fields[i];
        const MeasuredValue  value = ValueConverter::convert(field);

        Serial.print("    ");
        printPadded(ValueFormatter::name(value), NameColumnWidth);
        printPadded(ValueFormatter::format(field, value, text, sizeof(text)),
                    ValueColumnWidth);

        Serial.printf("%s = ", field.label.data());
        printRawValue(field.value.data());
        Serial.println();
    }
}

/// Verworfenen Block melden (Inhalt wird nicht ausgegeben).
void printError(const LogMessage& msg)
{
    printLinePrefix(msg);

    if (msg.kind == LogKind::ChecksumError)
    {
        Serial.printf("!! Pruefsummenfehler, Block verworfen (gesamt: %lu)\n",
                      static_cast<unsigned long>(msg.status.parser.checksumErrors));
    }
    else
    {
        Serial.printf("!! Formatfehler (zu lang/zu viele Felder), Block verworfen (gesamt: %lu)\n",
                      static_cast<unsigned long>(msg.status.parser.framesDiscarded));
    }
}

/// Ereignis der Geräteerkennung melden.
void printDeviceEvent(const LogMessage& msg)
{
    const DeviceDetector::Statistics& stats = msg.status.detector;

    printLinePrefix(msg);

    switch (msg.kind)
    {
        case LogKind::DeviceIdentified:
            Serial.print(">> Gerät erkannt: ");
            printDevice(msg.status.device);
            if (stats.lostCount > 0)
            {
                // Wiedererkennung: Dauer des Ausfalls hilft bei der Suche
                // nach Wackelkontakten.
                Serial.print("  (wieder da nach Ausfall von ");
                printSeconds(stats.lastOutageMs);
                Serial.printf(", Ausfälle gesamt: %lu)",
                              static_cast<unsigned long>(stats.lostCount));
            }
            Serial.println();
            break;

        case LogKind::DeviceChanged:
            Serial.print(">> Gerätewechsel: ");
            printDevice(msg.previousDevice);
            Serial.print("  ->  ");
            printDevice(msg.status.device);
            Serial.println();
            break;

        case LogKind::DeviceLost:
            Serial.print("!! Gerät verloren: ");
            printDevice(msg.previousDevice);
            Serial.print("  (seit mehr als ");
            printSeconds(g_config.deviceTimeoutMs.load());
            Serial.printf(" kein Block mit PID, Ausfälle gesamt: %lu)\n",
                          static_cast<unsigned long>(stats.lostCount));
            break;

        default:
            break;
    }
}

/******************************************************************************
 * W4b Schritt 2: HEX-Diagnose im seriellen Monitor
 ******************************************************************************/
/// Ungefragte HEX-Meldungen (":A…") anzeigen? Der MPPT sendet nach einer
/// Anfrage ggf. 2 min lang viele davon ("hex async aus" zum Abschalten).
bool g_showHexAsync = false;   // Standard aus (12.11.2026): echter MPPT sendet 2 min lang sekündlich

/// Abstand zum letzten gültigen Textblock (zeigt, ob Textblöcke pausieren).
void printTextGap(const LogMessage& msg)
{
    if (msg.status.hasValidFrame)
    {
        Serial.printf("; letzter Textblock vor %lu ms",
                      static_cast<unsigned long>(msg.timestampMs - msg.status.lastValidFrameMs));
    }
}

void printHex(const LogMessage& msg)
{
    if (msg.kind == LogKind::HexMessage && msg.hex.response() == VeHex::Response::Async && !g_showHexAsync)
    {
        return;
    }
    printLinePrefix(msg);

    if (msg.kind == LogKind::HexSent)
    {
        // Anfrage ohne das abschließende '\n' ausgeben
        Serial.print("HEX -> ");
        Serial.write(reinterpret_cast<const uint8_t*>(msg.hexRequest.text),
                     msg.hexRequest.length > 0 ? msg.hexRequest.length - 1 : 0);
        printTextGap(msg);
        Serial.println();
        return;
    }

    if (msg.kind == LogKind::HexError)
    {
        Serial.printf("!! HEX-Zeile fehlerhaft (gesamt: Prüfsumme %lu, Format %lu)",
                      static_cast<unsigned long>(msg.status.hex.checksumErrors),
                      static_cast<unsigned long>(msg.status.hex.malformed));
        printTextGap(msg);
        Serial.println();
        return;
    }

    const VeHex::Message& m = msg.hex;
    Serial.print("HEX <- ");
    switch (m.response())
    {
        case VeHex::Response::Ping:
        {
            uint16_t version = 0;
            uint8_t  type    = 0;
            VeHex::decodePing(static_cast<uint16_t>(VeHex::readUnsigned(m.data, 2)), version, type);
            static const char* const types[] = { "Bootloader", "Anwendung", "Tester", "Release Candidate" };
            Serial.printf("Ping: Firmware %X.%02X (%s)", version >> 8, version & 0xFF, types[type & 3]);
            break;
        }
        case VeHex::Response::Done:
            Serial.printf("Antwort: 0x%04lX", static_cast<unsigned long>(VeHex::readUnsigned(m.data, m.length)));
            break;
        case VeHex::Response::Unknown:
            Serial.print("Befehl unbekannt");
            break;
        case VeHex::Response::Error:
            Serial.printf("Fehler 0x%04lX (0xAAAA = Rahmenfehler)",
                          static_cast<unsigned long>(VeHex::readUnsigned(m.data, m.length)));
            break;
        case VeHex::Response::Get:
        case VeHex::Response::Set:
        case VeHex::Response::Async:
        {
            const char* name = m.response() == VeHex::Response::Get ? "Get"
                             : m.response() == VeHex::Response::Set ? "Set" : "Async (ungefragt)";
            if (!m.hasRegister())
            {
                Serial.printf("%s ohne Register (Länge %u)", name, static_cast<unsigned>(m.length));
                break;
            }
            Serial.printf("%s 0x%04X Flags %02X Wert", name, m.registerId(), m.flags());
            for (size_t i = 0; i < m.valueLength(); ++i)
            {
                Serial.printf(" %02X", m.value()[i]);
            }
            if (m.valueLength() >= 1 && m.valueLength() <= 4)
            {
                Serial.printf(" (= %lu)", static_cast<unsigned long>(VeHex::readUnsigned(m.value(), m.valueLength())));
            }
            if (m.flags() & VeHex::FlagUnknownId)      Serial.print(" [Register unbekannt]");
            if (m.flags() & VeHex::FlagParameterError) Serial.print(" [Parameterfehler / keine Daten]");
            break;
        }
        default:
            Serial.printf("Code %X, %u Byte", m.code, static_cast<unsigned>(m.length));
            break;
    }
    if (msg.hexAfterMs >= 0 && m.response() != VeHex::Response::Async)
    {
        Serial.printf("; %ld ms nach der Anfrage", static_cast<long>(msg.hexAfterMs));
    }
    printTextGap(msg);
    Serial.println();
}

/// Ende einer automatischen Abfrage (W4b Schritt 3).
void printHexJobDone(const LogMessage& msg)
{
    const HexJobRunner::Result& r = msg.status.hexJob;
    printLinePrefix(msg);
    if (r.aborted)
    {
        Serial.printf("!! HEX-Abfrage abgebrochen: Gerät antwortet nicht (%u von %u Registern versucht)\n",
                      static_cast<unsigned>(r.answered + r.failed), static_cast<unsigned>(r.requested));
        return;
    }
    Serial.printf("HEX-Abfrage fertig: %u von %u Registern beantwortet, %u ohne Antwort, Dauer %lu ms "
                  "(Anzeige: hex cache)\n",
                  static_cast<unsigned>(r.answered), static_cast<unsigned>(r.requested),
                  static_cast<unsigned>(r.failed),
                  static_cast<unsigned long>(r.finishedMs - r.startedMs));
}

/// Kopie des Zwischenspeichers für die Ausgabe (nicht auf den Stack, ~2 KB).
HexRegisterCache g_printHexCache;

/// Zwischenspeicher eines Kanals ausgeben ("hex cache").
void printHexCache(size_t index)
{
    Channel& channel = g_channels[index];
    {
        StateLock lock(channel.stateMutex);
        g_printHexCache = channel.hexCache;
    }
    const uint32_t now = millis();
    Serial.printf("[%s] HEX-Zwischenspeicher: %u Register\n", channel.name,
                  static_cast<unsigned>(g_printHexCache.size()));
    static const char* const states[] = { "ok", "keine Daten", "unbekannt", "keine Antwort" };
    for (size_t i = 0; i < g_printHexCache.size(); ++i)
    {
        const HexRegisterCache::Entry& e = g_printHexCache.at(i);
        Serial.printf("  0x%04X %-13s vor %3lu s ", e.id, states[static_cast<uint8_t>(e.state) & 3],
                      static_cast<unsigned long>((now - e.updatedMs) / 1000));
        if (e.length >= 1 && e.length <= 4)
        {
            Serial.printf("= %lu (", static_cast<unsigned long>(VeHex::readUnsigned(e.value, e.length)));
        }
        else if (e.length > 0)
        {
            Serial.print("(");
        }
        for (size_t b = 0; b < e.length; ++b)
        {
            Serial.printf(b == 0 ? "%02X" : " %02X", e.value[b]);
        }
        Serial.println(e.length > 0 ? ")" : "");
    }
}

/// Periodische Statuszeile je Kanal, auch wenn ein Kanal keine Daten liefert.
void printStatusReport()
{
    for (size_t i = 0; i < ChannelCount; ++i)
    {
        const Channel&      channel = g_channels[i];
        const ChannelStatus status  = readStatus(channel);

        // Zeit ERST NACH dem Kopieren lesen: Sonst kann der Zeitstempel im
        // Status jünger sein als "now", und die vorzeichenlose Differenz
        // läuft über (z. B. -6 ms -> 4294967290 ms).
        const uint32_t now = millis();

        Serial.printf("[STATUS] %s: ", channel.name);
        printDevice(status.device);

        Serial.printf("  OK=%lu  Pruefsummenfehler=%lu  Verworfen=%lu  HEX=%lu  "
                      "Ausfälle=%lu  nicht zuordenbar=%lu  Queue-Verluste=%lu  ",
                      static_cast<unsigned long>(status.parser.framesOk),
                      static_cast<unsigned long>(status.parser.checksumErrors),
                      static_cast<unsigned long>(status.parser.framesDiscarded),
                      static_cast<unsigned long>(status.parser.hexMessagesSkipped),
                      static_cast<unsigned long>(status.detector.lostCount),
                      static_cast<unsigned long>(status.detector.unassignedFrames),
                      static_cast<unsigned long>(status.queueDrops));

        if (!status.hasValidFrame)
        {
            Serial.println("** noch kein gültiger Block seit Start **");
            continue;
        }

        Serial.printf("letzter gültiger Block vor %lu ms  Verbindung=%s\n",
                      static_cast<unsigned long>(now - status.lastValidFrameMs),
                      status.linkUnstable ? "INSTABIL" : "stabil");
    }
}

/// Messwert mit Dezimalkomma und Einheit ausgeben, ungültig als "--".
void printReading(const Reading& reading, int decimals, const char* unit)
{
    if (!reading.valid)
    {
        Serial.print("--");
        return;
    }

    char text[24];
    snprintf(text, sizeof(text), "%.*f", decimals, static_cast<double>(reading.value));
    for (char* p = text; *p != '\0'; ++p)
    {
        if (*p == '.')
        {
            *p = ',';   // deutsches Dezimalkomma
        }
    }
    Serial.printf("%s %s", text, unit);
}

/******************************************************************************
 * Systemzeile (Stufe 4a): Batterie, PV, Fehlerwort.
 *
 *   [SYSTEM] Batterie (UART2): 12,330 V  0,000 A  0 W  SOC 100,0 %  TTG unendlich
 *            | PV (UART1): 96 W  18,600 V  Bulk  | Fehler 0x00000000: keine
 ******************************************************************************/
void printSystemReport()
{
    const SystemState state = readSystemState();

    // ---- Batterie ----
    Serial.print("[SYSTEM] Batterie");
    if (state.batteryChannel >= 0)
    {
        Serial.printf(" (%s): ", g_channels[state.batteryChannel].name);
        printReading(state.batteryVoltage, 3, "V");
        Serial.print("  ");
        printReading(state.batteryCurrent, 3, "A");
        Serial.print("  ");
        printReading(state.batteryPower, 0, "W");
        Serial.print("  SOC ");
        printReading(state.stateOfCharge, 1, "%");
        Serial.print("  TTG ");
        if (state.timeToGoInfinite)
        {
            Serial.print("unendlich");
        }
        else
        {
            printReading(state.timeToGo, 0, "min");
        }
    }
    else
    {
        Serial.print(": --");
        if (state.batteryVoltage.valid && state.batteryVoltage.source == ValueSource::PvCharger)
        {
            // Ersatzwert, deutlich gekennzeichnet (nicht für die Steuerung!)
            Serial.print("  (Spannung vom MPPT: ");
            printReading(state.batteryVoltage, 3, "V");
            Serial.print(")");
        }
    }

    // ---- PV ----
    Serial.print("  |  PV");
    if (state.pvChannel >= 0)
    {
        Serial.printf(" (%s): ", g_channels[state.pvChannel].name);
        printReading(state.pvPower, 0, "W");
        Serial.print("  ");
        printReading(state.pvVoltage, 3, "V");
        Serial.printf("  %s", state.chargerStateText != nullptr ? state.chargerStateText : "--");
    }
    else
    {
        Serial.print(": --");
    }

    // ---- Fehlerwort ----
    Serial.printf("  |  Fehler 0x%08lX", static_cast<unsigned long>(state.faults));
    if (state.faults == 0)
    {
        Serial.println(": keine");
        return;
    }

    Serial.printf(" %s: ", SystemEvaluator::severityText(state.severity));
    bool first = true;
    for (unsigned bit = 0; bit < SystemFault::BitCount; ++bit)
    {
        const uint32_t mask = 1u << bit;
        if ((state.faults & mask) != 0)
        {
            Serial.printf("%s%s", first ? "" : ", ", SystemEvaluator::faultText(mask));
            first = false;
        }
    }
    Serial.println();
}

/******************************************************************************
 * Stufe 4b: Ausgabe der Relais-Sollzustände
 *
 * Die Relais werden NOCH NICHT geschaltet (kommt in 4c). Angezeigt wird,
 * was geschaltet WÜRDE.
 ******************************************************************************/

/// Stand der zuletzt ausgegebenen Schaltvorgänge/Befehle (nur Logger-Task).
uint32_t g_printedSwitchCount[RelayCount] = {};
uint32_t g_printedCommandCount = 0;
bool     g_printedOverride = false;

/// Kompakte Relaiszeile alle 5 s.
void printRelayReport(const ControlSnapshot& control)
{
    Serial.print("[RELAIS] ");
    for (size_t i = 0; i < RelayCount; ++i)
    {
        const RelayStatus& r = control.relays[i];
        Serial.printf("%sR%u %s: ", i == 0 ? "" : "  |  ", static_cast<unsigned>(i + 1), control.names[i]);
        if (!control.enabled[i])
        {
            Serial.print("deaktiviert");
            continue;
        }
        Serial.print(r.on ? "EIN" : "AUS");
        if (r.locked)
        {
            Serial.print(" (gesperrt)");
        }
    }
    Serial.println(control.overrideActive ? "  |  !! OVERRIDE AKTIV !!" : "  |  Override: aus");
}

/// Schaltvorgänge, Override-Wechsel und Befehlsergebnisse sofort melden.
void printControlChanges(const ControlSnapshot& control)
{
    const SystemState state = readSystemState();

    for (size_t i = 0; i < RelayCount; ++i)
    {
        const RelayStatus& r = control.relays[i];
        if (r.switchCount == g_printedSwitchCount[i])
        {
            continue;
        }
        g_printedSwitchCount[i] = r.switchCount;

        Serial.printf("[RELAIS] R%u %s -> %s (%s, SOC ",
                      static_cast<unsigned>(i + 1), control.names[i],
                      r.on ? "EIN" : "AUS", r.reason);
        printReading(state.stateOfCharge, 1, "%");
        Serial.println(")");
    }

    if (control.overrideActive != g_printedOverride)
    {
        g_printedOverride = control.overrideActive;
        Serial.println(control.overrideActive
                           ? "[RELAIS] !! OVERRIDE AKTIV: alle Sperren aufgehoben !!"
                           : "[RELAIS] Override beendet, Normalbetrieb");
    }

    if (control.commandCount != g_printedCommandCount)
    {
        g_printedCommandCount = control.commandCount;
        if (control.lastCommand.kind == CommandKind::Relay)
        {
            Serial.printf("[BEFEHL] R%u %s: %s\n",
                          static_cast<unsigned>(control.lastCommand.relay) + 1,
                          control.lastCommand.on ? "ein" : "aus",
                          LoadController::resultText(control.lastResult));
        }
    }
}

/******************************************************************************
 * Befehle über den seriellen Monitor (zum Testen ohne Display)
 *
 *   r1 ein | r1 aus | override ein | override aus | hilfe
 *
 * Quelle ist "Local" (wie Taster/Touch). Groß-/Kleinschreibung egal.
 ******************************************************************************/
char   g_inputLine[48] = {};   // W4e: 48 statt 32 ("uhr stellen JJJJ-MM-TT hh:mm:ss")
size_t g_inputLength   = 0;

/// Vollständige Blockausgabe (jedes Feld) ein/aus. Standard: aus, damit
/// der Monitor übersichtlich bleibt. Nur in der Logger-Task verwendet.
bool g_showFrames = false;

void printHelp()
{
    Serial.println("[HILFE] Befehle: r1|r2|r3 ein|aus | override ein|aus | "
                   "bloecke ein|aus | hex (Diagnose) | uhr | speicher | hilfe");
}

/// "speicher": Arbeitsspeicher und Stack-Reserve je Task (W5).
/// Stack-Reserve = kleinster jemals freier Rest seit dem Start ("Hochwassermarke");
/// unter ca. 1 KB wird es eng (Absturz durch Stack-Überlauf möglich).
void printMemory()
{
    Serial.printf("[SPEICHER] Heap frei %lu Byte, kleinster Wert seit Start %lu Byte, "
                  "größter freier Block %lu Byte\n",
                  static_cast<unsigned long>(ESP.getFreeHeap()),
                  static_cast<unsigned long>(ESP.getMinFreeHeap()),
                  static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    for (const TaskInfo& t : g_tasks)
    {
        if (t.handle == nullptr)
        {
            continue;
        }
        // ESP-IDF: Rückgabe in Byte (Stackgröße wird dort in Byte angegeben)
        const uint32_t freeMin = uxTaskGetStackHighWaterMark(t.handle);
        Serial.printf("[SPEICHER] Task %-8s Stack %5lu Byte, Reserve min. %5lu Byte (%2lu %%)%s\n",
                      t.name, static_cast<unsigned long>(t.stackBytes), static_cast<unsigned long>(freeMin),
                      static_cast<unsigned long>(freeMin * 100 / t.stackBytes),
                      freeMin < 1024 ? "  <-- KNAPP" : "");
    }
}

/// "uhr" (anzeigen) bzw. "uhr stellen JJJJ-MM-TT hh:mm:ss" (UTC!). W4e Schritt 2.
void handleClockCommand(const char* args)
{
    // Systemuhr (W4e Schritt 3): Zeit, Quelle, Zeitzone
    const uint32_t now = clockNowUnix();
    Serial.printf("[UHR] System: %s (Quelle: %s, Zeitzone %s, NTP-Server %s)\n",
                  clockValid() ? formatLocalTime(now).c_str() : "nicht gestellt",
                  clockSourceKey(static_cast<ClockSource>(g_clockSource.load())),
                  g_tzKey.c_str(), g_ntpServer);

    I2cLock lock;   // RTC wird auch aus der webTask benutzt
    if (!lock.ok())
    {
        Serial.println("[UHR] I²C gerade belegt, bitte erneut versuchen");
        return;
    }

    if (!g_rtcFound)
    {
        // Evtl. erst nachträglich angeschlossen: noch einmal suchen
        g_rtcFound = g_rtc.begin();
        if (!g_rtcFound)
        {
            Serial.println("[UHR] RTC nicht gefunden (I²C GPIO 21 SDA / 22 SCL, Adresse 0x68)");
            return;
        }
        Serial.println("[UHR] RTC jetzt gefunden");
    }

    unsigned y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (std::sscanf(args, " stellen %u-%u-%u %u:%u:%u", &y, &mo, &d, &h, &mi, &s) == 6)
    {
        Rtc::DateTime t;
        t.year = static_cast<uint16_t>(y); t.month = static_cast<uint8_t>(mo); t.day = static_cast<uint8_t>(d);
        t.hour = static_cast<uint8_t>(h);  t.minute = static_cast<uint8_t>(mi); t.second = static_cast<uint8_t>(s);
        if (!t.isValid())
        {
            Serial.println("[UHR] ungültiges Datum (2000..2099, Format JJJJ-MM-TT hh:mm:ss, UTC)");
            return;
        }
        Serial.println(g_rtc.write(t) ? "[UHR] gestellt (UTC)" : "[UHR] Schreiben fehlgeschlagen");
    }
    else if (args[0] != '\0')
    {
        Serial.println("[UHR] Befehle: uhr | uhr stellen JJJJ-MM-TT hh:mm:ss (UTC)");
        return;
    }

    Rtc::DateTime t;
    if (g_rtc.read(t))
    {
        Serial.printf("[UHR] %s: %04u-%02u-%02u %02u:%02u:%02u UTC, Unix %lu%s\n", g_rtc.name(),
                      t.year, t.month, t.day, t.hour, t.minute, t.second,
                      static_cast<unsigned long>(Rtc::toUnix(t)),
                      g_rtc.batteryLow() ? ", Knopfzelle schwach/fehlt" : ", Knopfzelle ok");
    }
    else
    {
        Serial.printf("[UHR] %s: Zeit UNGÜLTIG (noch nie gestellt oder Stromausfall ohne Knopfzelle)%s\n",
                      g_rtc.name(), g_rtc.batteryLow() ? ", Knopfzelle schwach/fehlt" : "");
    }
}

void printHexHelp()
{
    Serial.println("[HEX] Befehle (nur lesen): hex ping [1|2] | hex version [1|2] | "
                   "hex pid [1|2] | hex get <Register> [1|2] | hex async ein|aus | "
                   "hex lesen [1|2] | hex cache [1|2]");
    Serial.println("[HEX] 1 = UART1, 2 = UART2, ohne Angabe beide. Register hex, z. B. "
                   "hex get 1000 (Shunt: Kapazität), hex get edf0 (MPPT: max. Ladestrom), "
                   "hex get 104f (MPPT: Historie gesamt)");
}

/// "hex …" auswerten. args = Rest der Zeile (Kleinbuchstaben, beginnt mit ' ' oder leer).
void handleHexCommand(const char* args)
{
    char word[12]  = {};
    char param[12] = {};
    char target[4] = {};
    const int n = std::sscanf(args, "%11s %11s %3s", word, param, target);
    if (n < 1)
    {
        printHexHelp();
        return;
    }

    // "hex lesen [1|2]" (automatische Abfrage neu starten), "hex cache [1|2]"
    if (std::strcmp(word, "lesen") == 0 || std::strcmp(word, "cache") == 0)
    {
        const bool read = word[0] == 'l';
        bool any = false;
        for (size_t i = 0; i < ChannelCount; ++i)
        {
            if (param[0] == '\0' || (param[0] == static_cast<char>('1' + i) && param[1] == '\0'))
            {
                any = true;
                if (read)
                {
                    requestHexRefresh(i);
                    Serial.printf("[HEX] %s: Abfrage angefordert\n", g_channels[i].name);
                }
                else
                {
                    printHexCache(i);
                }
            }
        }
        if (!any)
        {
            Serial.println("[HEX] Kanal muss 1 oder 2 sein (oder weglassen = beide)");
        }
        return;
    }

    if (std::strcmp(word, "async") == 0)
    {
        g_showHexAsync = std::strcmp(param, "aus") != 0 && std::strcmp(param, "off") != 0;
        Serial.printf("[HEX] Ungefragte Meldungen (:A) werden %s\n",
                      g_showHexAsync ? "angezeigt" : "NICHT mehr angezeigt");
        return;
    }

    VeHex::Request request;
    const char* channelArg = param;   // bei ping/version/pid ist der 2. Parameter der Kanal
    if (std::strcmp(word, "ping") == 0)         request = VeHex::ping();
    else if (std::strcmp(word, "version") == 0) request = VeHex::appVersion();
    else if (std::strcmp(word, "pid") == 0)     request = VeHex::productId();
    else if (std::strcmp(word, "get") == 0)
    {
        const char* hex = (param[0] == '0' && param[1] == 'x') ? param + 2 : param;
        char* end = nullptr;
        const unsigned long reg = std::strtoul(hex, &end, 16);
        if (hex[0] == '\0' || end == nullptr || *end != '\0' || reg > 0xFFFF)
        {
            Serial.println("[HEX] Register fehlt oder ungültig (Hex-Zahl 0..FFFF)");
            return;
        }
        request    = VeHex::get(static_cast<uint16_t>(reg));
        channelArg = target;
    }
    else
    {
        printHexHelp();
        return;
    }

    bool sent = false;
    for (size_t i = 0; i < ChannelCount; ++i)
    {
        const bool wanted = channelArg[0] == '\0' ||
                            (channelArg[0] == static_cast<char>('1' + i) && channelArg[1] == '\0');
        if (wanted)
        {
            requestHex(i, request);
            sent = true;
        }
    }
    if (!sent)
    {
        Serial.println("[HEX] Kanal muss 1 oder 2 sein (oder weglassen = beide)");
    }
}

/// Eine vollständige Eingabezeile auswerten.
void handleCommandLine(char* line)
{
    // in Kleinbuchstaben wandeln
    for (char* p = line; *p != '\0'; ++p)
    {
        if (*p >= 'A' && *p <= 'Z')
        {
            *p = static_cast<char>(*p - 'A' + 'a');
        }
    }

    Serial.printf("> %s\n", line);

    // ---- TESTFUNKTION (08.11.2026), SPÄTER ENTFERNEN ----
    // "absturz": löst absichtlich einen Absturz aus (Panic -> Neustart,
    // Absturzbericht im Core-Dump-Bereich). Zum Testen des Rückfalls nach
    // einem Update: dreimal innerhalb von 60 s nach dem Start auslösen.
    // Bewusst NICHT gespeichert/automatisch, damit auch die Firmware, auf die
    // zurückgeschaltet wird, nicht in eine Absturzschleife gerät.
    if (std::strcmp(line, "absturz") == 0)
    {
        Serial.printf("[TEST] Absturz wird ausgelöst (Laufzeit %lu s) ...\n",
                      static_cast<unsigned long>(millis() / 1000));
        Serial.flush();
        delay(100);
        abort();
    }

    // ---- Anzeige-Befehle (direkt in der Logger-Task) ----
    if (std::strcmp(line, "bloecke ein") == 0 || std::strcmp(line, "blöcke ein") == 0)
    {
        g_showFrames = true;
        Serial.println("[AUSGABE] Blockausgabe EIN (jedes Feld, Klarname + Rohwert)");
        return;
    }
    if (std::strcmp(line, "bloecke aus") == 0 || std::strcmp(line, "blöcke aus") == 0)
    {
        g_showFrames = false;
        Serial.println("[AUSGABE] Blockausgabe AUS (nur Status, Ereignisse, Befehle)");
        return;
    }

    // ---- W5: Speicher-Diagnose ----
    if (std::strcmp(line, "speicher") == 0)
    {
        printMemory();
        return;
    }

    // ---- W4e: Echtzeituhr ----
    if (std::strncmp(line, "uhr", 3) == 0 && (line[3] == ' ' || line[3] == '\0'))
    {
        handleClockCommand(line + 3);
        return;
    }

    // ---- W4b Schritt 2: HEX-Diagnose (nur lesende Anfragen) ----
    if (std::strncmp(line, "hex", 3) == 0 && (line[3] == ' ' || line[3] == '\0'))
    {
        handleHexCommand(line + 3);
        return;
    }

    ControlCommand command;
    command.source = CommandSource::Local;

    // "r1 ein", "r2 aus", ... (r1..r3; ein/an/on bzw. aus/off)
    const bool relayCmd = line[0] == 'r' && line[1] >= '1' && line[1] <= '3' && line[2] == ' ';
    const char* arg = relayCmd ? line + 3 : "";
    const bool argOn  = std::strcmp(arg, "ein") == 0 || std::strcmp(arg, "an") == 0 || std::strcmp(arg, "on") == 0;
    const bool argOff = std::strcmp(arg, "aus") == 0 || std::strcmp(arg, "off") == 0;

    if (relayCmd && (argOn || argOff))
    {
        command.kind  = CommandKind::Relay;
        command.relay = static_cast<RelayId>(line[1] - '1');
        command.on    = argOn;
    }
    else if (std::strcmp(line, "override ein") == 0 || std::strcmp(line, "override on") == 0)
    {
        command.kind = CommandKind::Override; command.on = true;
    }
    else if (std::strcmp(line, "override aus") == 0 || std::strcmp(line, "override off") == 0)
    {
        command.kind = CommandKind::Override; command.on = false;
    }
    else
    {
        printHelp();
        return;
    }

    if (xQueueSend(g_commandQueue, &command, 0) != pdTRUE)
    {
        Serial.println("[BEFEHL] verworfen: Befehls-Queue voll");
    }
}

/// Zeichen vom seriellen Monitor sammeln (nicht blockierend).
void readSerialInput()
{
    while (Serial.available() > 0)
    {
        const char c = static_cast<char>(Serial.read());
        if (c == '\r' || c == '\n')
        {
            if (g_inputLength > 0)
            {
                g_inputLine[g_inputLength] = '\0';
                handleCommandLine(g_inputLine);
                g_inputLength = 0;
            }
        }
        else if (g_inputLength < sizeof(g_inputLine) - 1)
        {
            g_inputLine[g_inputLength++] = c;
        }
    }
}

/******************************************************************************
 * Stufe W1: WLAN-Meldungen
 ******************************************************************************/
const char* netStateText(NetState state)
{
    switch (state)
    {
        case NetState::AccessPoint: return "nur Access Point (kein WLAN eingerichtet)";
        case NetState::Connecting:  return "verbinde mit WLAN";
        case NetState::Connected:   return "mit WLAN verbunden";
        case NetState::Fallback:    return "WLAN nicht erreichbar, Access Point aktiv";
    }
    return "?";
}

/// Zuletzt gemeldeter Netzwerkzustand (nur Logger-Task).
bool      g_netPrinted = false;
NetStatus g_netLast;

/// WLAN-Zustand ausgeben (bei Änderung sofort, sonst mit dem Statusbericht).
void printNetwork(const NetStatus& n)
{
    Serial.printf("[WLAN] %s", netStateText(n.state));
    if (n.staConnected)
    {
        Serial.printf("  |  %s  IP %s  %d dBm  ->  http://%s.local",
                      n.staSsid, IPAddress(n.staIp).toString().c_str(),
                      static_cast<int>(n.rssi), n.hostname);
    }
    if (!n.staConnected && n.lastFailReason != 0)
    {
        Serial.printf("  |  Fehler: %s (Code %u)", WifiManager::reasonText(n.lastFailReason),
                      static_cast<unsigned>(n.lastFailReason));
    }
    if (n.apActive)
    {
        Serial.printf("  |  AP \"%s\"  IP %s  Geräte: %u",
                      n.apSsid, IPAddress(n.apIp).toString().c_str(),
                      static_cast<unsigned>(n.apClients));
        if (n.defaultPassword)
        {
            // Nur am USB-Monitor (physischer Zugriff) und solange es das
            // Startpasswort ist; gilt für AP und WebUI-Login (admin).
            Serial.printf("  Startpasswort: %s", n.displayPassword);
        }
    }
    Serial.println();
}

/// MQTT-Zustand bei Änderung melden (nur Logger-Task).
MqttState g_mqttPrintedState = MqttState::Disabled;
bool      g_mqttPrinted = false;

uint32_t g_mqttPrintedRx = 0;

void printMqttChanges()
{
    const MqttStatus st = g_mqtt.status();

    // Empfangene Nachrichten (Befehle) protokollieren
    if (st.receiveCount != g_mqttPrintedRx)
    {
        g_mqttPrintedRx = st.receiveCount;
        Serial.printf("[MQTT] empfangen: %s = %s\n", st.lastRxTopic, st.lastRxPayload);
    }

    if (g_mqttPrinted && st.state == g_mqttPrintedState)
    {
        return;
    }
    g_mqttPrinted = true;
    g_mqttPrintedState = st.state;

    const MqttConfig cfg = g_mqtt.config();
    switch (st.state)
    {
        case MqttState::Disabled:
            Serial.println("[MQTT] aus (nicht eingerichtet oder ausgeschaltet)");
            break;
        case MqttState::NoNetwork:
            Serial.println("[MQTT] wartet auf WLAN-Verbindung");
            break;
        case MqttState::Connecting:
            Serial.printf("[MQTT] verbinde mit %s:%u ...\n", cfg.host.c_str(), static_cast<unsigned>(cfg.port));
            break;
        case MqttState::Connected:
            Serial.printf("[MQTT] verbunden mit %s:%u, Basis-Topic \"%s\"\n",
                          cfg.host.c_str(), static_cast<unsigned>(cfg.port), cfg.baseTopic.c_str());
            break;
        case MqttState::Error:
            Serial.printf("[MQTT] Fehler: %s (neuer Versuch folgt)\n", st.lastError);
            break;
    }
}

void printNetworkChanges()
{
    const NetStatus n = readNetStatus();
    const bool changed = !g_netPrinted ||
                         n.state != g_netLast.state ||
                         n.apActive != g_netLast.apActive ||
                         n.staConnected != g_netLast.staConnected ||
                         n.staIp != g_netLast.staIp ||
                         n.defaultPassword != g_netLast.defaultPassword ||
                         n.lastFailReason != g_netLast.lastFailReason;
    if (changed && n.apSsid[0] != '\0')   // erst ausgeben, wenn WLAN gestartet ist
    {
        printNetwork(n);
        g_netLast = n;
        g_netPrinted = true;
    }
}

/******************************************************************************
 * Logger-Task: einzige Stelle, die auf Serial (UART0) schreibt.
 ******************************************************************************/
void loggerTask(void*)
{
    uint32_t lastStatusMs = millis();

    for (;;)
    {
        // Mit Timeout warten, damit der Statusbericht auch dann kommt,
        // wenn kein Kanal Daten liefert.
        if (xQueueReceive(g_logQueue, &g_incoming, pdMS_TO_TICKS(100)) == pdTRUE)
        {
            switch (g_incoming.kind)
            {
                case LogKind::Frame:
                    if (g_showFrames)   // Standard: aus ("bloecke ein")
                    {
                        printFrame(g_incoming);
                    }
                    break;

                case LogKind::ChecksumError:
                case LogKind::FrameDiscarded:
                    printError(g_incoming);
                    break;

                case LogKind::DeviceIdentified:
                case LogKind::DeviceChanged:
                case LogKind::DeviceLost:
                    printDeviceEvent(g_incoming);
                    break;

                case LogKind::HexSent:
                case LogKind::HexMessage:
                case LogKind::HexError:
                    printHex(g_incoming);
                    break;

                case LogKind::HexJobDone:
                    printHexJobDone(g_incoming);
                    break;
            }
        }

        readSerialInput();

        const ControlSnapshot control = readControl();
        printControlChanges(control);
        printNetworkChanges();
        printMqttChanges();

        if (millis() - lastStatusMs >= StatusIntervalMs)
        {
            lastStatusMs = millis();
            printStatusReport();
            printSystemReport();
            printRelayReport(control);
            if (g_netPrinted)
            {
                printNetwork(readNetStatus());
            }
        }
    }
}

} // namespace

/******************************************************************************
 * setup(): Initialisierung
 *
 * Das Banner wird VOR dem Start der Tasks ausgegeben, damit es nicht mit
 * Empfangsdaten vermischt wird.
 ******************************************************************************/
void setup()
{
    // ALLERERSTE Aktion: Relais in den sicheren Zustand (aus), noch vor
    // allem anderen. Bis hierher hält der Pull-up die Relais aus.
    g_relays.begin();

    Serial.begin(DebugBaudrate);
    delay(200);   // USB-Seriell kurz stabilisieren lassen

    Serial.println();
    Serial.println("--------------------------------------------------");
    Serial.println("Victron VE.Direct Reader - Stufe W3 (WLAN, WebUI, Updates, MQTT)");
    Serial.printf("Firmware: %s\n", FirmwareVersion);

    // W2: Probestart nach Update prüfen. Lief die neue Firmware zu oft nicht
    // stabil, wird hier auf die vorherige zurückgeschaltet (Neustart).
    UpdateManager::checkTrialBoot();

    // W4a Schritt 5: gewählte Anlage (Camper/Balkonkraftwerk) aus dem NVS,
    // bevor die systemTask die Relais-Voreinstellung lädt
    loadInstallation();

    // W4e: Echtzeituhr (PCF8523 an I²C, GPIO 21 SDA / 22 SCL). Zeitzone und
    // NTP-Server aus dem NVS; gültige RTC-Zeit stellt sofort die Systemuhr.
    g_i2cMutex = xSemaphoreCreateMutex();
    loadClockSettings();
    Wire.begin(RtcSdaPin, RtcSclPin);
    g_rtcFound = g_rtc.begin();
    if (g_rtcFound)
    {
        Rtc::DateTime t;
        const bool valid = g_rtc.read(t);
        if (valid)
        {
            setSystemClock(Rtc::toUnix(t), ClockSource::Rtc);
            Serial.printf("RTC %s: %04u-%02u-%02u %02u:%02u:%02u UTC%s\n", g_rtc.name(), t.year, t.month, t.day,
                          t.hour, t.minute, t.second, g_rtc.batteryLow() ? "  (Knopfzelle schwach/fehlt)" : "");
        }
        else
        {
            Serial.printf("RTC %s gefunden, Zeit UNGÜLTIG (noch nie gestellt oder Stromausfall ohne Knopfzelle)\n",
                          g_rtc.name());
        }
    }
    else
    {
        Serial.println("RTC: nicht gefunden (I²C GPIO 21/22) – Zeit nur über NTP/Browser");
    }

    // W4a: Grund des Neustarts als erstes Ereignis (Absturz, Watchdog,
    // Brownout = kritisch)
    {
        g_resetReason = esp_reset_reason();
        Severity severity = Severity::Info;
        const char* reason = resetReasonText(g_resetReason, severity);
        addEvent(severity, "Gerät gestartet: %s (Firmware %s)", reason, FirmwareVersion);
        Serial.printf("Neustart-Grund: %s\n", reason);
        if (UpdateManager::rolledBack())
        {
            addEvent(Severity::Warning, "Neue Firmware lief nicht stabil, vorherige Firmware wiederhergestellt");
        }
    }

    if (UpdateManager::onTrial())
    {
        Serial.printf("[UPDATE] Neue Firmware läuft auf Probe: Start %lu von %u "
                      "(bestätigt nach 60 s stabilem Lauf, sonst Rückfall)\n",
                      static_cast<unsigned long>(UpdateManager::trialBoots()),
                      static_cast<unsigned>(UpdateManager::MaxTrialBoots));
    }
    if (UpdateManager::rolledBack())
    {
        Serial.println("[UPDATE] ACHTUNG: Die zuletzt aufgespielte Firmware lief nicht stabil, "
                       "es wurde auf die vorherige zurückgeschaltet.");
    }
    for (size_t i = 0; i < RelayCount; ++i)
    {
        Serial.printf("Relais R%u: GPIO%u, %s\n", static_cast<unsigned>(i + 1),
                      static_cast<unsigned>(g_relays.gpio(i)),
                      g_relays.activeLow(i) ? "aktiv Low" : "aktiv High");
    }
    Serial.printf("Anlage: %s\n", g_config.installation.load() == Installation::Balcony
                                      ? "Balkonkraftwerk" : "Camper");
    Serial.println("Befehle im Monitor: r1|r2|r3 ein|aus | override ein|aus | "
                   "bloecke ein | bloecke aus | hilfe");
    Serial.println("Blockausgabe ist AUS (einschalten mit: bloecke ein)");
    Serial.println("UART1: RX 16 / TX 17    UART2: RX 26 / TX 33");
    Serial.printf("Geräte-Timeout: %lu ms\n",
                  static_cast<unsigned long>(g_config.deviceTimeoutMs.load()));
    Serial.printf("sizeof(VeDirectFrame) = %u Byte, sizeof(LogMessage) = %u Byte\n",
                  static_cast<unsigned>(sizeof(VeDirectFrame)),
                  static_cast<unsigned>(sizeof(LogMessage)));
    Serial.println("Hinweis: Der erste Block je Kanal wird zur Synchronisation verworfen.");
    Serial.println("--------------------------------------------------");

    g_logQueue     = xQueueCreate(LogQueueLength, sizeof(LogMessage));
    g_commandQueue = xQueueCreate(CommandQueueLength, sizeof(ControlCommand));
    if (g_logQueue == nullptr || g_commandQueue == nullptr)
    {
        Serial.println("FEHLER: Log-Queue konnte nicht angelegt werden (Speicher?)");
        for (;;)
        {
            delay(1000);
        }
    }

    for (size_t i = 0; i < ChannelCount; ++i)
    {
        g_channels[i].stateMutex = xSemaphoreCreateMutex();
        if (g_channels[i].stateMutex == nullptr)
        {
            Serial.println("FEHLER: Mutex konnte nicht angelegt werden (Speicher?)");
            for (;;)
            {
                delay(1000);
            }
        }
        g_channels[i].reader.begin();
    }

    // Netzwerk-Reset-Taste (vor dem Start des WLAN)
    checkResetButton();

    // Watchdog: Neustart (panic), wenn eine überwachte Task hängt
    esp_task_wdt_init(WatchdogTimeoutS, true);

    // Logger zuerst starten, damit er bereit ist, bevor Nachrichten kommen.
    // Handles in g_tasks merken (W5: Monitor-Befehl "speicher")
    xTaskCreatePinnedToCore(loggerTask, "Logger", LoggerTaskStack,
                            nullptr, LoggerTaskPriority, &g_tasks[0].handle, 1);

    // Systemauswertung (Stufe 4a), 1×/s
    xTaskCreatePinnedToCore(systemTask, "System", SystemTaskStack,
                            nullptr, SystemTaskPriority, &g_tasks[1].handle, 0);

    // E-Paper (Stufe 4c-2)
    xTaskCreatePinnedToCore(displayTask, "Display", DisplayTaskStack,
                            nullptr, DisplayTaskPriority, &g_tasks[2].handle, 1);

    // WLAN + WebUI (Stufe W1)
    xTaskCreatePinnedToCore(webTask, "Web", WebTaskStack,
                            nullptr, WebTaskPriority, &g_tasks[3].handle, 0);

    // MQTT (Stufe W3)
    xTaskCreatePinnedToCore(mqttTask, "Mqtt", MqttTaskStack,
                            nullptr, MqttTaskPriority, &g_tasks[4].handle, 0);

    // Je ein Kern pro UART-Kanal (wie bisher).
    xTaskCreatePinnedToCore(uartTask, "UART1", UartTaskStack,
                            &g_channels[0], UartTaskPriority, &g_tasks[5].handle, 0);
    xTaskCreatePinnedToCore(uartTask, "UART2", UartTaskStack,
                            &g_channels[1], UartTaskPriority, &g_tasks[6].handle, 1);
}

/******************************************************************************
 * loop(): bleibt leer.
 *
 * Die gesamte Arbeit erledigen die Tasks. Die Pause verhindert, dass die
 * Arduino-loopTask unnötig Rechenzeit auf Core 1 verbraucht.
 ******************************************************************************/
void loop()
{
    vTaskDelay(pdMS_TO_TICKS(1000));
}
