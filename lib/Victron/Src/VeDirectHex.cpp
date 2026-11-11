/******************************************************************************
 * @file VeDirectHex.cpp
 *
 * Siehe VeDirectHex.hpp.
 ******************************************************************************/

#include "VeDirectHex.hpp"

namespace VeHex
{

namespace
{

const char HexDigits[] = "0123456789ABCDEF";

/// Ziffer -> Wert, -1 = keine Hex-Ziffer. Kleinbuchstaben werden toleriert,
/// obwohl Victron Großbuchstaben vorschreibt.
int hexValue(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/// Anfrage bauen. NUR intern und NUR für die lesenden Befehle unten
/// verwendet (bewusst nicht öffentlich, siehe "SICHERHEIT" im Header).
Request build(uint8_t command, const uint8_t* data, std::size_t length)
{
    Request r;
    std::size_t pos = 0;
    uint8_t sum = command;

    r.text[pos++] = ':';
    r.text[pos++] = HexDigits[command & 0x0F];
    for (std::size_t i = 0; i < length; ++i)
    {
        r.text[pos++] = HexDigits[data[i] >> 4];
        r.text[pos++] = HexDigits[data[i] & 0x0F];
        sum = static_cast<uint8_t>(sum + data[i]);
    }
    const uint8_t check = static_cast<uint8_t>(0x55 - sum);
    r.text[pos++] = HexDigits[check >> 4];
    r.text[pos++] = HexDigits[check & 0x0F];
    r.text[pos++] = '\n';
    r.text[pos]   = '\0';
    r.length      = pos;
    return r;
}

// Befehlscodes (nur die lesenden)
constexpr uint8_t CmdPing       = 0x1;
constexpr uint8_t CmdAppVersion = 0x3;
constexpr uint8_t CmdProductId  = 0x4;
constexpr uint8_t CmdGet        = 0x7;

} // namespace

/******************************************************************************
 * Anfragen
 ******************************************************************************/
Request ping()       { return build(CmdPing, nullptr, 0); }
Request appVersion() { return build(CmdAppVersion, nullptr, 0); }
Request productId()  { return build(CmdProductId, nullptr, 0); }

Request get(uint16_t registerId)
{
    const uint8_t data[3] = {
        static_cast<uint8_t>(registerId & 0xFF),   // Little Endian
        static_cast<uint8_t>(registerId >> 8),
        0x00                                       // Flags: laut Doku immer 0
    };
    return build(CmdGet, data, sizeof(data));
}

/******************************************************************************
 * Message
 ******************************************************************************/
bool Message::hasRegister() const
{
    const Response r = response();
    return (r == Response::Get || r == Response::Set || r == Response::Async) && length >= 3;
}

uint16_t Message::registerId() const
{
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint8_t Message::flags() const
{
    return data[2];
}

const uint8_t* Message::value() const
{
    return data + 3;
}

std::size_t Message::valueLength() const
{
    return length >= 3 ? length - 3 : 0;
}

/******************************************************************************
 * Empfänger
 ******************************************************************************/
void VeDirectHexParser::reset()
{
    m_active  = false;
    m_invalid = false;
    m_count   = 0;
    m_digits  = 0;
    m_stats   = Statistics();
}

VeDirectHexParser::Event VeDirectHexParser::fail(Event event)
{
    m_active = false;
    if (event == Event::ChecksumError) ++m_stats.checksumErrors;
    if (event == Event::Malformed)     ++m_stats.malformed;
    return event;
}

VeDirectHexParser::Event VeDirectHexParser::process(uint8_t byte)
{
    if (byte == ':')
    {
        // Beginn einer neuen Zeile. Eine unterbrochene vorherige Zeile ist
        // damit erledigt (wird nicht gezählt, z. B. ':' als Prüfsummenbyte
        // eines Textblocks).
        m_active     = true;
        m_invalid    = false;
        m_haveCode   = false;
        m_highNibble = true;
        m_count      = 0;
        m_digits     = 0;
        return Event::None;
    }

    if (!m_active)
    {
        return Event::None;   // Textprotokoll: nicht unsere Sache
    }

    if (byte == '\r')
    {
        return Event::None;   // falls ein Gerät "\r\n" sendet: ignorieren
    }

    if (byte == '\n')
    {
        return finish();
    }

    if (m_invalid)
    {
        return Event::None;   // Rest der Zeile bis '\n' überlesen
    }

    const int v = hexValue(byte);
    if (v < 0)
    {
        // Kein Hex-Zeichen. Typisch: ':' war das Prüfsummenbyte eines
        // Textblocks, danach folgt "\r\nPID\t...". Still verwerfen, wenn
        // noch fast nichts gesammelt wurde, sonst als ungültig zählen.
        if (m_digits <= 1)
        {
            m_active = false;
            return Event::None;
        }
        m_invalid = true;
        return Event::None;
    }

    ++m_digits;
    if (!m_haveCode)
    {
        m_code     = static_cast<uint8_t>(v);
        m_haveCode = true;
        return Event::None;
    }

    if (m_highNibble)
    {
        if (m_count >= sizeof(m_bytes))
        {
            m_invalid = true;   // zu lang
            return Event::None;
        }
        m_pending    = static_cast<uint8_t>(v << 4);
        m_highNibble = false;
    }
    else
    {
        m_bytes[m_count++] = static_cast<uint8_t>(m_pending | v);
        m_highNibble = true;
    }
    return Event::None;
}

VeDirectHexParser::Event VeDirectHexParser::finish()
{
    if (m_digits == 0)
    {
        m_active = false;   // ":" + Zeilenende: nichts, still verwerfen
        return Event::None;
    }
    // Mindestens Befehl + Prüfsumme, gerade Anzahl Datenziffern
    if (m_invalid || !m_highNibble || m_count < 1)
    {
        return fail(Event::Malformed);
    }

    uint8_t sum = m_code;
    for (std::size_t i = 0; i < m_count; ++i)
    {
        sum = static_cast<uint8_t>(sum + m_bytes[i]);
    }
    if (sum != 0x55)
    {
        return fail(Event::ChecksumError);
    }

    m_message.code   = m_code;
    m_message.length = m_count - 1;   // ohne Prüfsumme
    for (std::size_t i = 0; i < m_message.length; ++i)
    {
        m_message.data[i] = m_bytes[i];
    }
    m_active = false;
    ++m_stats.messages;
    return Event::MessageReady;
}

/******************************************************************************
 * Werte
 ******************************************************************************/
uint32_t readUnsigned(const uint8_t* data, std::size_t bytes)
{
    uint32_t v = 0;
    for (std::size_t i = 0; i < bytes && i < 4; ++i)
    {
        v |= static_cast<uint32_t>(data[i]) << (8 * i);
    }
    return v;
}

int32_t readSigned(const uint8_t* data, std::size_t bytes)
{
    if (bytes == 0)
    {
        return 0;
    }
    if (bytes > 4)
    {
        bytes = 4;
    }
    uint32_t v = readUnsigned(data, bytes);
    if (bytes < 4 && (v & (1u << (8 * bytes - 1))))
    {
        v |= ~((1u << (8 * bytes)) - 1);   // Vorzeichen erweitern
    }
    return static_cast<int32_t>(v);
}

void decodePing(uint16_t raw, uint16_t& version, uint8_t& type)
{
    type    = static_cast<uint8_t>(raw >> 14);
    version = static_cast<uint16_t>(raw & 0x0FFF);
}

/******************************************************************************
 * Historie
 ******************************************************************************/
namespace
{
constexpr uint32_t NotAvailable32 = 0xFFFFFFFFu;
}

bool decodeHistoryDay(const uint8_t* v, std::size_t length, uint8_t flags, HistoryDay& out)
{
    out = HistoryDay();
    if (flags & FlagParameterError)
    {
        return true;   // Tag ohne Daten (laut Doku "leerer Datensatz")
    }
    if (flags != 0 || length < 34)
    {
        return false;
    }

    out.valid            = true;
    out.yieldWh          = readUnsigned(v + 1, 4) * 10;
    const uint32_t consumed = readUnsigned(v + 5, 4);
    out.consumedValid    = consumed != NotAvailable32;
    out.consumedWh       = out.consumedValid ? consumed * 10 : 0;
    out.batteryMaxCentiV = static_cast<uint16_t>(readUnsigned(v + 9, 2));
    out.batteryMinCentiV = static_cast<uint16_t>(readUnsigned(v + 11, 2));
    for (int i = 0; i < 4; ++i)
    {
        out.errors[i] = v[14 + i];
    }
    out.bulkMin          = static_cast<uint16_t>(readUnsigned(v + 18, 2));
    out.absorptionMin    = static_cast<uint16_t>(readUnsigned(v + 20, 2));
    out.floatMin         = static_cast<uint16_t>(readUnsigned(v + 22, 2));
    out.powerMaxW        = readUnsigned(v + 24, 4);
    out.batteryCurrentMaxDeciA = static_cast<uint16_t>(readUnsigned(v + 28, 2));
    out.panelMaxCentiV   = static_cast<uint16_t>(readUnsigned(v + 30, 2));
    out.sequence         = static_cast<uint16_t>(readUnsigned(v + 32, 2));
    return true;
}

bool decodeHistoryTotal(const uint8_t* v, std::size_t length, HistoryTotal& out)
{
    out = HistoryTotal();
    if (length < 19)
    {
        return false;
    }
    for (int i = 0; i < 4; ++i)
    {
        out.errors[i] = v[2 + i];
    }
    out.userYieldWh      = readUnsigned(v + 6, 4) * 10;
    out.systemYieldWh    = readUnsigned(v + 10, 4) * 10;
    out.panelMaxCentiV   = static_cast<uint16_t>(readUnsigned(v + 14, 2));
    out.batteryMaxCentiV = static_cast<uint16_t>(readUnsigned(v + 16, 2));
    out.daysAvailable    = v[18];
    if (length >= 21)
    {
        out.batteryMinValid  = true;   // ab FW 1.17 (34 Byte)
        out.batteryMinCentiV = static_cast<uint16_t>(readUnsigned(v + 19, 2));
    }
    return true;
}

} // namespace VeHex
