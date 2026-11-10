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

#include "AppConfig.hpp"
#include "DeviceCatalog.hpp"
#include "DeviceDetector.hpp"
#include "DeviceState.hpp"
#include "LinkMonitor.hpp"
#include "EventLog.hpp"
#include "LoadController.hpp"
#include "SystemState.hpp"
#include "ValueConverter.hpp"
#include "ValueFormatter.hpp"
#include "VeDirectFrame.hpp"
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
    DeviceLost        ///< Timeout ("previousDevice" = verlorenes Gerät)
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

/// Geräteausfall prüfen und ggf. melden.
void checkDeviceTimeout(Channel& channel, uint32_t now)
{
    if (channel.detector.checkTimeout(now) == DeviceDetector::Event::Lost)
    {
        {
            StateLock lock(channel.stateMutex);
            channel.link.recordOutage(now);
            channel.deviceState.clear();   // "--" statt veralteter Werte
            channel.deviceState.setIdentity(channel.detector.identity());
        }
        publish(channel, LogKind::DeviceLost, now);
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

        while (channel.reader.available())
        {
            const uint8_t byte = static_cast<uint8_t>(channel.reader.read());
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
                    }
                    else if (result.event == DeviceDetector::Event::Changed)
                    {
                        publish(channel, LogKind::DeviceChanged, now);
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

    bool stableConfirmed = false;

    for (;;)
    {
        const uint32_t now = millis();
        g_wifi.loop(now);
        g_webUi.loop(now);

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
    return "\"faults\":{\"count\":" + String(count) + ",\"severity\":\"" + severity + "\"}";
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

    j += ",\"events\":[";
    for (size_t i = 0; i < g_webEventsCopy.size(); ++i)
    {
        const EventLog::Entry& e = g_webEventsCopy.newest(i);
        if (i > 0) j += ",";
        j += "{\"seq\":"       + String(e.seq);
        j += ",\"ageS\":"      + String(DeviceState::ageMs(e.timeMs, now) / 1000);
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
char   g_inputLine[32] = {};
size_t g_inputLength   = 0;

/// Vollständige Blockausgabe (jedes Feld) ein/aus. Standard: aus, damit
/// der Monitor übersichtlich bleibt. Nur in der Logger-Task verwendet.
bool g_showFrames = false;

void printHelp()
{
    Serial.println("[HILFE] Befehle: r1|r2|r3 ein|aus | override ein|aus | "
                   "bloecke ein|aus | hilfe");
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
    xTaskCreatePinnedToCore(loggerTask, "Logger", LoggerTaskStack,
                            nullptr, LoggerTaskPriority, nullptr, 1);

    // Systemauswertung (Stufe 4a), 1×/s
    xTaskCreatePinnedToCore(systemTask, "System", SystemTaskStack,
                            nullptr, SystemTaskPriority, nullptr, 0);

    // E-Paper (Stufe 4c-2)
    xTaskCreatePinnedToCore(displayTask, "Display", DisplayTaskStack,
                            nullptr, DisplayTaskPriority, nullptr, 1);

    // WLAN + WebUI (Stufe W1)
    xTaskCreatePinnedToCore(webTask, "Web", WebTaskStack,
                            nullptr, WebTaskPriority, nullptr, 0);

    // MQTT (Stufe W3)
    xTaskCreatePinnedToCore(mqttTask, "Mqtt", MqttTaskStack,
                            nullptr, MqttTaskPriority, nullptr, 0);

    // Je ein Kern pro UART-Kanal (wie bisher).
    xTaskCreatePinnedToCore(uartTask, "UART1", UartTaskStack,
                            &g_channels[0], UartTaskPriority, nullptr, 0);
    xTaskCreatePinnedToCore(uartTask, "UART2", UartTaskStack,
                            &g_channels[1], UartTaskPriority, nullptr, 1);
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
