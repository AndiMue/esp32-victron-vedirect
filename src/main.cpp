/******************************************************************************
 * @file main.cpp
 *
 * ESP32 Victron VE.Direct – Stufe 1: prüfsummengesicherter Rohdatenempfang
 *
 * Aufbau (FreeRTOS-Tasks):
 *
 *   UART1 ─► VeDirectReader ─► VeDirectTextParser ─┐   uartTask (Core 0)
 *                                                    ├─► Queue ─► loggerTask ─► Serial (UART0)
 *   UART2 ─► VeDirectReader ─► VeDirectTextParser ─┘   uartTask (Core 1)
 *
 * Grundregeln:
 *   - NUR die loggerTask schreibt auf Serial (UART0). Die UART-Tasks geben
 *     nie selbst etwas aus. So können sich die Ausgaben beider Kanäle nicht
 *     mehr Zeichen für Zeichen vermischen, und eine blockierende
 *     Debug-Ausgabe bremst den VE.Direct-Empfang nicht aus.
 *   - Ausgegeben werden ausschließlich Blöcke mit gültiger Prüfsumme,
 *     als Rohtext (Label = Wert). Interpretation folgt in späteren Stufen.
 *   - Die Kanäle heißen bewusst "UART1"/"UART2" und nicht "MPPT"/"Shunt":
 *     Später soll jedes Gerät an jedem UART funktionieren (Erkennung per PID).
 *
 * Pinbelegung (bestätigt):
 *   UART1: RX GPIO16, TX GPIO17
 *   UART2: RX GPIO26, TX GPIO33
 ******************************************************************************/

#include <Arduino.h>

#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "DeviceCatalog.hpp"
#include "ValueConverter.hpp"
#include "ValueFormatter.hpp"
#include "VeDirectFrame.hpp"
#include "VeDirectReader.hpp"
#include "VeDirectTextParser.hpp"

namespace
{

/******************************************************************************
 * Einstellungen
 ******************************************************************************/
constexpr uint32_t DebugBaudrate     = 115200; ///< UART0 / USB-Monitor
constexpr uint32_t StatusIntervalMs  = 5000;   ///< Statuszeile alle 5 s
constexpr uint32_t ChannelTimeoutMs  = 3000;   ///< ohne gültigen Block -> Warnung
constexpr UBaseType_t LogQueueLength = 6;      ///< Puffer für Logger-Nachrichten

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
    VeDirectTextParser::Statistics parser;   ///< Zähler des Parsers
    uint32_t lastValidFrameMs = 0;            ///< millis() des letzten gültigen Blocks
    bool     hasValidFrame    = false;        ///< mindestens ein gültiger Block?
    uint32_t queueDrops       = 0;            ///< Nachrichten verloren (Queue voll)
};

/******************************************************************************
 * Nachricht von einer UART-Task an die Logger-Task.
 ******************************************************************************/
enum class LogKind : uint8_t
{
    Frame,          ///< gültiger Block, "frame" ist gefüllt
    ChecksumError,  ///< Block mit falscher Prüfsumme verworfen
    FrameDiscarded  ///< Block mit Format-/Größenfehler verworfen
};

struct LogMessage
{
    LogKind       kind = LogKind::Frame;
    uint8_t       channelIndex = 0;
    uint32_t      timestampMs = 0;
    ChannelStatus status;          ///< Zählerstand zum Zeitpunkt der Nachricht
    VeDirectFrame frame;           ///< nur bei LogKind::Frame gültig
};

/******************************************************************************
 * Ein VE.Direct-Kanal: UART + eigener Parser + eigener Status.
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
    ChannelStatus       status;     ///< geschützt durch g_statusLock
    LogMessage          outgoing;   ///< Sendepuffer der UART-Task
                                    ///< (global statt auf dem Stack, ~1,7 KB)
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
 * UART-Task: Bytes lesen, parsen, Ergebnisse an den Logger melden.
 *
 * Gibt selbst NICHTS auf Serial aus.
 ******************************************************************************/
void uartTask(void* parameter)
{
    Channel& channel = *static_cast<Channel*>(parameter);

    for (;;)
    {
        while (channel.reader.available())
        {
            const uint8_t byte = static_cast<uint8_t>(channel.reader.read());
            const VeDirectTextParser::Event event = channel.parser.parse(byte);

            if (event == VeDirectTextParser::Event::None)
            {
                continue;
            }

            const uint32_t now = millis();

            // Status aktualisieren (Zähler des Parsers + Zeitstempel)
            portENTER_CRITICAL(&g_statusLock);
            channel.status.parser = channel.parser.statistics();
            if (event == VeDirectTextParser::Event::FrameReady)
            {
                channel.status.lastValidFrameMs = now;
                channel.status.hasValidFrame    = true;
            }
            const ChannelStatus snapshot = channel.status;
            portEXIT_CRITICAL(&g_statusLock);

            // Nachricht für den Logger vorbereiten
            LogMessage& msg = channel.outgoing;
            msg.channelIndex = channel.index;
            msg.timestampMs  = now;
            msg.status       = snapshot;

            switch (event)
            {
                case VeDirectTextParser::Event::FrameReady:
                    msg.kind  = LogKind::Frame;
                    msg.frame = channel.parser.frame();
                    break;
                case VeDirectTextParser::Event::ChecksumError:
                    msg.kind = LogKind::ChecksumError;
                    msg.frame.clear();
                    break;
                default:
                    msg.kind = LogKind::FrameDiscarded;
                    msg.frame.clear();
                    break;
            }

            // Nicht warten: Ist die Queue voll, wird die Nachricht verworfen
            // und gezählt. Der Empfang darf nie wegen der Ausgabe stocken.
            if (xQueueSend(g_logQueue, &msg, 0) != pdTRUE)
            {
                portENTER_CRITICAL(&g_statusLock);
                ++channel.status.queueDrops;
                portEXIT_CRITICAL(&g_statusLock);
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
 * Gerätename je Kanal (vorläufig, bis zur Geräteerkennung in Stufe 2).
 *
 * Blöcke mit PID legen das Gerät des Kanals fest. Blöcke ohne PID (z. B.
 * der H-Block des SmartShunts) werden dem zuletzt erkannten Gerät des
 * Kanals zugeordnet und im Kopf als "Folgeblock" gekennzeichnet.
 * Nur von der Logger-Task verwendet.
 ******************************************************************************/
char g_lastProductId[ChannelCount][VeDirectField::ValueSize] = {};

/// Kopfzeile eines Blocks: Kanal, Zeit, Gerät, Zähler.
void printFrameHeader(const LogMessage& msg)
{
    const Channel& channel = g_channels[msg.channelIndex];
    char* lastPid = g_lastProductId[msg.channelIndex];

    const VeDirectField* pidField = msg.frame.find("PID");
    if (pidField != nullptr)
    {
        std::strncpy(lastPid, pidField->value.data(), VeDirectField::ValueSize - 1);
        lastPid[VeDirectField::ValueSize - 1] = '\0';
    }

    Serial.printf("[%s] ", channel.name);
    printTimestamp(msg.timestampMs);
    Serial.print("  ");

    if (lastPid[0] == '\0')
    {
        Serial.print("Gerät unbekannt (noch keine PID empfangen)");
    }
    else
    {
        const DeviceInfo* device = DeviceCatalog::find(lastPid);
        Serial.printf("%s (%s)", device != nullptr ? device->name : "unbekanntes Gerät", lastPid);
        if (pidField == nullptr)
        {
            Serial.print(" - Folgeblock ohne PID");
        }
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
    const Channel& channel = g_channels[msg.channelIndex];

    Serial.printf("[%s] ", channel.name);
    printTimestamp(msg.timestampMs);

    if (msg.kind == LogKind::ChecksumError)
    {
        Serial.printf("  !! Pruefsummenfehler, Block verworfen (gesamt: %lu)\n",
                      static_cast<unsigned long>(msg.status.parser.checksumErrors));
    }
    else
    {
        Serial.printf("  !! Formatfehler (zu lang/zu viele Felder), Block verworfen (gesamt: %lu)\n",
                      static_cast<unsigned long>(msg.status.parser.framesDiscarded));
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

        Serial.printf("[STATUS] %s: OK=%lu  Pruefsummenfehler=%lu  Verworfen=%lu  "
                      "HEX=%lu  Queue-Verluste=%lu  ",
                      channel.name,
                      static_cast<unsigned long>(status.parser.framesOk),
                      static_cast<unsigned long>(status.parser.checksumErrors),
                      static_cast<unsigned long>(status.parser.framesDiscarded),
                      static_cast<unsigned long>(status.parser.hexMessagesSkipped),
                      static_cast<unsigned long>(status.queueDrops));

        if (!status.hasValidFrame)
        {
            Serial.println("** noch kein gueltiger Block seit Start **");
            continue;
        }

        const uint32_t age = now - status.lastValidFrameMs;
        Serial.printf("letzter gueltiger Block vor %lu ms",
                      static_cast<unsigned long>(age));

        if (age > ChannelTimeoutMs)
        {
            Serial.print("  ** KEINE DATEN (Timeout) **");
        }
        Serial.println();
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
            if (g_incoming.kind == LogKind::Frame)
            {
                printFrame(g_incoming);
            }
            else
            {
                printError(g_incoming);
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
    Serial.println("Victron VE.Direct Reader - Stufe 3 (Klarnamen, umgerechnete Werte)");
    Serial.println("UART1: RX 16 / TX 17    UART2: RX 26 / TX 33");
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
