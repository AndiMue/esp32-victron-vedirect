/******************************************************************************
 * @file VeDirectHex.hpp
 *
 * VE.Direct HEX-Protokoll (W4b): Anfragen bauen und Antworten zerlegen.
 * Quellen: docs/VeDirect-Doku/Victron_BMV-7xx-SmartShunt-HEX-protocol_Rev7.pdf
 *          docs/VeDirect-Doku/Victron_BlueSolar-SmartSolar-MPPT-HEX-protocol_Rev18.pdf
 *
 * Aufbau einer Nachricht (ASCII, Hex-Ziffern groß):
 *
 *   ':' BEFEHL(1 Ziffer) DATEN(je Byte 2 Ziffern) PRÜFSUMME(2 Ziffern) '\n'
 *
 *   Befehl + alle Datenbytes + Prüfsumme ergeben modulo 256 genau 0x55.
 *   Zahlen sind Little Endian. Beispiel Ping: ":154\n" (1 + 0x54 = 0x55).
 *
 * SICHERHEIT (Projektinhaber 10.11.2026, W4b = NUR LESEN):
 *   Diese Bibliothek kann ausschließlich LESENDE Anfragen bauen:
 *   Ping, Firmware-Version, Produkt-ID und Get (Register lesen).
 *   Befehle zum Schreiben (Set), Neustart (Restart) oder Bootloader gibt es
 *   hier bewusst NICHT. Ein Victron-Gerät kann damit nicht verstellt werden.
 *   Schreiben ist frühestens W4c (MPPT, derzeit leer) bzw. W4d (Shunt) und
 *   braucht eine eigene Freigabe.
 *
 * Empfang (VeDirectHexParser):
 *   Byteweise mit denselben Bytes füttern wie den Text-Parser. Er sammelt nur
 *   Zeilen von ':' bis '\n', alles andere (Textprotokoll) wird ignoriert.
 *   Antworten und asynchrone Meldungen (":A…", MPPT ab FW 1.17) landen
 *   gleichermaßen in message().
 *
 * Reines C++ ohne Arduino -> auf dem PC testbar.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

namespace VeHex
{

/******************************************************************************
 * Antwort-Codes (erste Ziffer einer empfangenen Nachricht)
 ******************************************************************************/
enum class Response : uint8_t
{
    Done    = 0x1,   ///< Antwort auf Firmware-Version / Produkt-ID
    Unknown = 0x3,   ///< Gerät kennt den Befehl nicht
    Error   = 0x4,   ///< Rahmenfehler (Nutzdaten 0xAAAA)
    Ping    = 0x5,   ///< Antwort auf Ping (Version + Firmware-Typ)
    Get     = 0x7,   ///< Antwort auf Get (Register, Flags, Wert)
    Set     = 0x8,   ///< Antwort auf Set (wird von uns nie ausgelöst)
    Async   = 0xA    ///< Ungefragte Meldung, Aufbau wie Get
};

/// Flags in Get/Set/Async-Antworten.
constexpr uint8_t FlagUnknownId      = 0x01;   ///< Register gibt es nicht
constexpr uint8_t FlagNotSupported   = 0x02;   ///< (nur bei Set) schreibgeschützt
constexpr uint8_t FlagParameterError = 0x04;   ///< Wert ungültig; bei Historie: Tag ohne Daten

/// Größter erwarteter Inhalt (Datenbytes ohne Prüfsumme): Register (2) +
/// Flags (1) + Historien-Datensatz (34) = 37. Etwas Reserve.
constexpr std::size_t MaxPayload = 48;

/******************************************************************************
 * Anfragen (nur lesend!)
 ******************************************************************************/
struct Request
{
    char        text[12] = {};   ///< z. B. ":70010003E\n" (mit '\n', nullterminiert)
    std::size_t length   = 0;    ///< Zeichen ohne Nullterminator
};

Request ping();                         ///< ":154\n"
Request appVersion();                   ///< ":352\n"
Request productId();                    ///< ":451\n"
Request get(uint16_t registerId);       ///< ":7" + Register (LE) + Flags 00 + Prüfsumme

/******************************************************************************
 * Empfangene Nachricht
 ******************************************************************************/
struct Message
{
    uint8_t     code   = 0;              ///< Antwort-Code (siehe Response)
    uint8_t     data[MaxPayload] = {};   ///< Datenbytes ohne Prüfsumme
    std::size_t length = 0;

    Response response() const { return static_cast<Response>(code); }

    /// Get-, Set- oder Async-Antwort mit Register und Flags?
    bool hasRegister() const;

    uint16_t registerId() const;          ///< nur wenn hasRegister()
    uint8_t  flags() const;               ///< nur wenn hasRegister()
    const uint8_t* value() const;         ///< Wert hinter Register und Flags
    std::size_t    valueLength() const;
};

/******************************************************************************
 * Byteweiser Empfänger
 ******************************************************************************/
class VeDirectHexParser
{
public:

    enum class Event : uint8_t
    {
        None,            ///< nichts Neues
        MessageReady,    ///< gültige Nachricht -> message()
        ChecksumError,   ///< Zeile vollständig, Prüfsumme falsch
        Malformed        ///< Zeile ungültig (falsches Zeichen, ungerade Länge, zu lang)
    };

    struct Statistics
    {
        uint32_t messages       = 0;
        uint32_t checksumErrors = 0;
        uint32_t malformed      = 0;
    };

    Event process(uint8_t byte);

    const Message&    message()    const { return m_message; }
    const Statistics& statistics() const { return m_stats; }

    void reset();

private:

    Event finish();
    Event fail(Event event);

    bool        m_active    = false;   ///< zwischen ':' und '\n'
    bool        m_invalid   = false;   ///< Zeile schon als ungültig erkannt
    bool        m_haveCode  = false;
    bool        m_highNibble = true;   ///< nächste Ziffer = oberes Halbbyte
    uint8_t     m_code      = 0;
    uint8_t     m_pending   = 0;
    uint8_t     m_bytes[MaxPayload + 1] = {};   ///< Daten + Prüfsumme
    std::size_t m_count     = 0;
    std::size_t m_digits    = 0;       ///< empfangene Ziffern (inkl. Befehl)

    Message    m_message;
    Statistics m_stats;
};

/******************************************************************************
 * Werte lesen (Little Endian)
 ******************************************************************************/
uint32_t readUnsigned(const uint8_t* data, std::size_t bytes);   ///< 1..4 Bytes
int32_t  readSigned(const uint8_t* data, std::size_t bytes);     ///< 1..4 Bytes, Vorzeichen erweitert

/// Ping-Antwort zerlegen: raw = 0x4401 -> version 0x0401 ("4.01"),
/// type 1 (0 Bootloader, 1 Anwendung, 2 Tester, 3 Release Candidate).
void decodePing(uint16_t raw, uint16_t& version, uint8_t& type);

/******************************************************************************
 * MPPT-Historie (Register 0x104F gesamt, 0x1050..0x106E Tage)
 ******************************************************************************/
/// Fähigkeiten des MPPT (un32). Bit 0 = echter Lastausgang vorhanden.
/// (Bit 19 = "virtueller" Lastausgang über den TX-Pin – den nutzen wir
/// für die Kommunikation, daher für die Anzeige nicht relevant.)
constexpr uint16_t RegCapabilities  = 0x0140;
constexpr uint32_t CapLoadOutput    = 0x00000001;

constexpr uint16_t RegHistoryTotal = 0x104F;
constexpr uint16_t RegHistoryToday = 0x1050;   ///< 0x1050 + n = vor n Tagen
constexpr uint8_t  HistoryDays     = 31;       ///< 0x1050..0x106E

/// Ein Tag. "Ungültig" = Feld nicht verfügbar (z. B. Verbrauch ohne Lastausgang).
struct HistoryDay
{
    bool     valid              = false;   ///< false = Tag (noch) ohne Daten
    uint32_t yieldWh            = 0;       ///< Ertrag in Wh (Gerät: 0,01 kWh = 10 Wh)
    bool     consumedValid      = false;
    uint32_t consumedWh         = 0;
    uint16_t batteryMaxCentiV   = 0;       ///< 0,01 V
    bool     batteryMinValid    = false;   ///< Finding 12.11.2026: MPPT FW 1.59 meldet oft 0 = nicht erfasst
    uint16_t batteryMinCentiV   = 0;
    uint8_t  errors[4]          = {};      ///< [0] = jüngster Fehlercode (siehe ERR)
    uint16_t bulkMin            = 0;
    uint16_t absorptionMin      = 0;
    uint16_t floatMin           = 0;
    uint32_t powerMaxW          = 0;
    uint16_t batteryCurrentMaxDeciA = 0;   ///< 0,1 A
    uint16_t panelMaxCentiV     = 0;
    uint16_t sequence           = 0;       ///< Tagesnummer (läuft bis 364, dann 0)
};

/// Tagesdatensatz (34 Byte) zerlegen. flags = Flags der Antwort; ist
/// FlagParameterError gesetzt, hat der Tag keine Daten (valid = false).
/// false = Datensatz zu kurz/ungültig.
bool decodeHistoryDay(const uint8_t* value, std::size_t length, uint8_t flags, HistoryDay& out);

struct HistoryTotal
{
    uint8_t  errors[4]          = {};
    uint32_t userYieldWh        = 0;       ///< Gesamtertrag (rücksetzbar)
    uint32_t systemYieldWh      = 0;       ///< Gesamtertrag (System)
    uint16_t panelMaxCentiV     = 0;
    uint16_t batteryMaxCentiV   = 0;
    uint8_t  daysAvailable      = 0;
    bool     batteryMinValid    = false;   ///< erst ab MPPT-FW 1.17
    uint16_t batteryMinCentiV   = 0;
};

/// Gesamtdatensatz (19 Byte ab FW 1.16, 34 Byte ab FW 1.17) zerlegen.
bool decodeHistoryTotal(const uint8_t* value, std::size_t length, HistoryTotal& out);

} // namespace VeHex
