/******************************************************************************
 * @file main.cpp
 *
 * ESP32 Victron VE.Direct
 *   Stufe 1: prüfsummengesicherter Empfang
 *   Stufe 2: Geräteerkennung pro Kanal (DeviceDetector)
 *   Stufe 3: Klarnamen und physikalische Werte
 *
 * Aufbau (FreeRTOS-Tasks):
 *
 *   UART1 ─► Reader ─► Parser ─► DeviceDetector ─┐   uartTask (Core 0)
 *                                                  ├─► Queue ─► loggerTask ─► Serial (UART0)
 *   UART2 ─► Reader ─► Parser ─► DeviceDetector ─┘   uartTask (Core 1)
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

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "AppConfig.hpp"
#include "DeviceCatalog.hpp"
#include "DeviceDetector.hpp"
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

constexpr uint32_t UartTaskStack   = 4096;
constexpr uint32_t LoggerTaskStack = 6144;   ///< printf/snprintf mit Formatierung braucht etwas mehr

/// Spaltenbreiten der Feldausgabe (in Zeichen, nicht Bytes – Umlaute!)
constexpr size_t NameColumnWidth  = 34;
constexpr size_t ValueColumnWidth = 32;

/// UART-Tasks haben höhere Priorität als der Logger: Empfang geht vor Ausgabe.
constexpr UBaseType_t UartTaskPriority   = 2;
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

/// Empfangspuffer der Logger-Task (global statt auf dem Stack).
LogMessage g_incoming;

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
    if (kind == LogKind::Frame)
    {
        channel.status.lastValidFrameMs = now;
        channel.status.hasValidFrame    = true;
    }
    const ChannelStatus snapshot = channel.status;
    portEXIT_CRITICAL(&g_statusLock);

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
        publish(channel, LogKind::DeviceLost, now);
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

    for (;;)
    {
        // Timeout laufend aus der Konfiguration übernehmen
        // (kann später z. B. über die WebUI geändert werden).
        channel.detector.setTimeoutMs(g_config.deviceTimeoutMs.load());

        // Ausfall auch dann erkennen, wenn gar keine Bytes mehr kommen
        // (z. B. Stecker abgezogen, Kabelbruch).
        checkDeviceTimeout(channel, millis());

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

                    publish(channel, LogKind::Frame, now, result.origin, &frame);
                    break;
                }

                case VeDirectTextParser::Event::ChecksumError:
                    publish(channel, LogKind::ChecksumError, now);
                    break;

                default:
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
    const uint32_t now = millis();

    for (size_t i = 0; i < ChannelCount; ++i)
    {
        const Channel&      channel = g_channels[i];
        const ChannelStatus status  = readStatus(channel);

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

        Serial.printf("letzter gültiger Block vor %lu ms\n",
                      static_cast<unsigned long>(now - status.lastValidFrameMs));
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
        if (xQueueReceive(g_logQueue, &g_incoming, pdMS_TO_TICKS(250)) == pdTRUE)
        {
            switch (g_incoming.kind)
            {
                case LogKind::Frame:
                    printFrame(g_incoming);
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

        if (millis() - lastStatusMs >= StatusIntervalMs)
        {
            lastStatusMs = millis();
            printStatusReport();
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
    Serial.begin(DebugBaudrate);
    delay(200);   // USB-Seriell kurz stabilisieren lassen

    Serial.println();
    Serial.println("--------------------------------------------------");
    Serial.println("Victron VE.Direct Reader - Stufe 2+3 (Geräteerkennung, Klarnamen)");
    Serial.println("UART1: RX 16 / TX 17    UART2: RX 26 / TX 33");
    Serial.printf("Geräte-Timeout: %lu ms\n",
                  static_cast<unsigned long>(g_config.deviceTimeoutMs.load()));
    Serial.printf("sizeof(VeDirectFrame) = %u Byte, sizeof(LogMessage) = %u Byte\n",
                  static_cast<unsigned>(sizeof(VeDirectFrame)),
                  static_cast<unsigned>(sizeof(LogMessage)));
    Serial.println("Hinweis: Der erste Block je Kanal wird zur Synchronisation verworfen.");
    Serial.println("--------------------------------------------------");

    g_logQueue = xQueueCreate(LogQueueLength, sizeof(LogMessage));
    if (g_logQueue == nullptr)
    {
        Serial.println("FEHLER: Log-Queue konnte nicht angelegt werden (Speicher?)");
        for (;;)
        {
            delay(1000);
        }
    }

    for (size_t i = 0; i < ChannelCount; ++i)
    {
        g_channels[i].reader.begin();
    }

    // Logger zuerst starten, damit er bereit ist, bevor Nachrichten kommen.
    xTaskCreatePinnedToCore(loggerTask, "Logger", LoggerTaskStack,
                            nullptr, LoggerTaskPriority, nullptr, 1);

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
