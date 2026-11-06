/******************************************************************************
 * @file ValueFormatter.cpp
 ******************************************************************************/

#include "ValueFormatter.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "DeviceCatalog.hpp"
#include "FieldCatalog.hpp"

namespace
{

/******************************************************************************
 * Kleiner Hilfspuffer: hängt Text an, ohne jemals überzulaufen.
 ******************************************************************************/
class TextBuffer
{
public:
    TextBuffer(char* buffer, std::size_t size)
        : m_buffer(buffer), m_size(size), m_length(0)
    {
        if (m_size > 0)
        {
            m_buffer[0] = '\0';
        }
    }

    void append(const char* format, ...)
    {
        if (m_length + 1 >= m_size)
        {
            return;   // voll
        }

        va_list args;
        va_start(args, format);
        const int written = std::vsnprintf(m_buffer + m_length,
                                           m_size - m_length, format, args);
        va_end(args);

        if (written > 0)
        {
            m_length += static_cast<std::size_t>(written);
            if (m_length >= m_size)
            {
                m_length = m_size - 1;   // gekürzt
            }
        }
    }

    bool empty() const { return m_length == 0; }

private:
    char*       m_buffer;
    std::size_t m_size;
    std::size_t m_length;
};

/******************************************************************************
 * Festkommazahl mit Dezimalkomma: raw=12327, decimals=3 -> "12,327";
 * raw=-120, decimals=3 -> "-0,120".
 ******************************************************************************/
void appendFixedPoint(TextBuffer& out, int64_t raw, uint8_t decimals)
{
    const bool negative = raw < 0;
    const unsigned long magnitude =
        static_cast<unsigned long>(negative ? -raw : raw);   // |raw| <= 2^31

    if (decimals == 0)
    {
        out.append("%s%lu", negative ? "-" : "", magnitude);
        return;
    }

    unsigned long divisor = 1;
    for (uint8_t i = 0; i < decimals; ++i)
    {
        divisor *= 10;
    }

    out.append("%s%lu,%0*lu", negative ? "-" : "",
               magnitude / divisor, static_cast<int>(decimals),
               magnitude % divisor);
}

/// Einheit mit Leerzeichen anhängen (falls vorhanden).
void appendUnit(TextBuffer& out, const char* unit)
{
    if (unit != nullptr && unit[0] != '\0')
    {
        out.append(" %s", unit);
    }
}

/// Bitmaske als Liste ausgeben: "Unterspannung, Niedriger SOC".
void appendBitmask(TextBuffer& out, const CodeTable* table, int64_t raw)
{
    const unsigned long bits = static_cast<unsigned long>(raw);
    if (bits == 0)
    {
        out.append("keiner");
        return;
    }

    unsigned long known = 0;
    bool first = true;

    if (table != nullptr)
    {
        for (std::size_t i = 0; i < table->count; ++i)
        {
            const unsigned long bit = static_cast<unsigned long>(table->entries[i].code);
            if ((bits & bit) != 0)
            {
                out.append("%s%s", first ? "" : ", ", table->entries[i].text);
                known |= bit;
                first = false;
            }
        }
    }

    const unsigned long unknown = bits & ~known;
    if (unknown != 0)
    {
        out.append("%sunbekannte Bits 0x%08lX", first ? "" : ", ", unknown);
    }
}

/// "v4.19" aus 419
void appendVersion(TextBuffer& out, int64_t version)
{
    out.append("v%ld.%02ld", static_cast<long>(version / 100),
               static_cast<long>(version % 100));
}

} // namespace

/******************************************************************************
 * Klarname
 ******************************************************************************/
const char* ValueFormatter::name(const MeasuredValue& value)
{
    return value.definition != nullptr ? value.definition->name
                                       : "(unbekanntes Feld)";
}

/******************************************************************************
 * Wert als Text
 ******************************************************************************/
const char* ValueFormatter::format(const VeDirectField& field,
                                   const MeasuredValue& value,
                                   char* buffer,
                                   std::size_t size)
{
    TextBuffer out(buffer, size);

    switch (value.status)
    {
        case ValueStatus::UnknownField: out.append("-");                return buffer;
        case ValueStatus::NotAvailable: out.append("nicht verfügbar");  return buffer;
        case ValueStatus::Invalid:      out.append("ungültiger Wert");  return buffer;
        case ValueStatus::Infinite:     out.append("unendlich (keine Entladung)"); return buffer;
        case ValueStatus::Valid:        break;
    }

    const FieldDefinition& def = *value.definition;
    const char* text = field.value.data();

    switch (def.type)
    {
        case ValueType::Number:
        case ValueType::TimeToGo:
            appendFixedPoint(out, value.raw, def.decimals);
            appendUnit(out, def.unit);
            break;

        case ValueType::OnOff:
            out.append(value.raw != 0 ? "EIN" : "AUS");
            break;

        case ValueType::Text:
            out.append("%s", text);
            break;

        case ValueType::ProductId:
        {
            const DeviceInfo* device = DeviceCatalog::find(text);
            if (device != nullptr)
            {
                out.append("%s", device->name);
            }
            else
            {
                out.append("unbekanntes Gerät");
            }
            break;
        }

        case ValueType::Code:
        {
            const char* codeText = FieldCatalog::codeText(def, value.raw);
            if (codeText != nullptr)
            {
                out.append("%s", codeText);
            }
            else
            {
                out.append("unbekannter Code %ld", static_cast<long>(value.raw));
            }
            break;
        }

        case ValueType::BitmaskDecimal:
        case ValueType::BitmaskHex:
            appendBitmask(out, def.codes, value.raw);
            break;

        case ValueType::Firmware:
            appendVersion(out, value.raw);
            // Führender Buchstabe = Release Candidate (z. B. "C208")
            if ((text[0] >= 'A' && text[0] <= 'Z') || (text[0] >= 'a' && text[0] <= 'z'))
            {
                out.append(" (Release Candidate %c)", text[0]);
            }
            break;

        case ValueType::FirmwareExt:
        {
            appendVersion(out, value.raw);
            // Letzte zwei Zeichen: "FF" = Release, sonst Beta-Nummer
            const std::size_t length = std::strlen(text);
            const char* suffix = text + length - 2;
            if (!(suffix[0] == 'F' || suffix[0] == 'f') ||
                !(suffix[1] == 'F' || suffix[1] == 'f'))
            {
                out.append("-beta-%s", suffix);
            }
            break;
        }
    }

    return buffer;
}
