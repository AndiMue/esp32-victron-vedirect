/******************************************************************************
 * @file ValueConverter.cpp
 ******************************************************************************/

#include "ValueConverter.hpp"

#include <cstring>

#include "FieldCatalog.hpp"

namespace
{

/// Victron sendet "---", wenn ein Wert nicht verfügbar ist
/// (z. B. SOC/CE bei nicht synchronisiertem Shunt, T ohne Sensor).
bool isNotAvailable(const char* text)
{
    return std::strcmp(text, "---") == 0;
}

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

int hexDigitValue(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/******************************************************************************
 * Dezimalzahl mit optionalem Vorzeichen lesen. Der GESAMTE Text muss eine
 * Zahl sein ("12a" ist ungültig). Bereich: int32 (reicht für alle Felder).
 ******************************************************************************/
bool parseDecimal(const char* text, int64_t& result)
{
    const char* p = text;
    bool negative = false;

    if (*p == '-' || *p == '+')
    {
        negative = (*p == '-');
        ++p;
    }

    if (!isDigit(*p))
    {
        return false;
    }

    int64_t value = 0;
    for (; *p != '\0'; ++p)
    {
        if (!isDigit(*p))
        {
            return false;
        }
        value = value * 10 + (*p - '0');
        if (value > 2147483648LL)   // außerhalb int32
        {
            return false;
        }
    }

    value = negative ? -value : value;
    if (value > 2147483647LL)
    {
        return false;
    }

    result = value;
    return true;
}

/******************************************************************************
 * Hex-Zahl im Format "0x1234" (Präfix Pflicht, 1..8 Ziffern) lesen.
 ******************************************************************************/
bool parseHex(const char* text, int64_t& result)
{
    if (text[0] != '0' || (text[1] != 'x' && text[1] != 'X'))
    {
        return false;
    }

    const char* p = text + 2;
    std::size_t digits = 0;
    int64_t value = 0;

    for (; *p != '\0'; ++p, ++digits)
    {
        const int d = hexDigitValue(*p);
        if (d < 0 || digits >= 8)
        {
            return false;
        }
        value = value * 16 + d;
    }

    if (digits == 0)
    {
        return false;
    }

    result = value;
    return true;
}

/// Vergleich ohne Beachtung der Groß-/Kleinschreibung (Doku empfiehlt das
/// für ON/OFF, da alte BMV "On"/"Off" senden).
bool equalsIgnoreCase(const char* a, const char* b)
{
    for (; *a != '\0' && *b != '\0'; ++a, ++b)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 'a' + 'A');
        if (ca != cb)
        {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

/******************************************************************************
 * FW: optional EIN Buchstabe (Release Candidate), dann 1..4 Ziffern.
 * "419" / "0419" -> 419 (= v4.19), "C208" -> 208.
 ******************************************************************************/
bool parseFirmware(const char* text, int64_t& result)
{
    const char* p = text;
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z'))
    {
        ++p;
    }

    const std::size_t length = std::strlen(p);
    if (length == 0 || length > 4)
    {
        return false;
    }
    return parseDecimal(p, result) && p[0] != '-' && p[0] != '+';
}

/******************************************************************************
 * FWE: 3..6 Zeichen. Die letzten zwei Zeichen sind die Kennung
 * ("FF" = offizielles Release, sonst Beta-Nummer), davor die Version.
 * "0208FF" / "208FF" -> 208 (= v2.08).
 ******************************************************************************/
bool parseFirmwareExt(const char* text, int64_t& result)
{
    const std::size_t length = std::strlen(text);
    if (length < 3 || length > 6)
    {
        return false;
    }

    if (hexDigitValue(text[length - 2]) < 0 || hexDigitValue(text[length - 1]) < 0)
    {
        return false;
    }

    int64_t version = 0;
    for (std::size_t i = 0; i < length - 2; ++i)
    {
        if (!isDigit(text[i]))
        {
            return false;
        }
        version = version * 10 + (text[i] - '0');
    }

    result = version;
    return true;
}

/// 10^n für n = 0..9
int64_t powerOfTen(uint8_t n)
{
    int64_t result = 1;
    while (n-- > 0)
    {
        result *= 10;
    }
    return result;
}

} // namespace

/******************************************************************************
 * Umrechnung
 ******************************************************************************/
MeasuredValue ValueConverter::convert(const VeDirectField& field)
{
    return convert(field.label.data(), field.value.data());
}

MeasuredValue ValueConverter::convert(const char* label, const char* value)
{
    MeasuredValue result;

    result.definition = FieldCatalog::find(label);
    if (result.definition == nullptr)
    {
        result.status = ValueStatus::UnknownField;
        return result;
    }

    const FieldDefinition& def = *result.definition;

    // Text wird nie interpretiert, ist also immer gültig.
    if (def.type == ValueType::Text)
    {
        result.status = ValueStatus::Valid;
        return result;
    }

    // "---" ist bei allen nicht-Text-Feldern "nicht verfügbar", niemals 0.
    if (isNotAvailable(value))
    {
        result.status = ValueStatus::NotAvailable;
        return result;
    }

    bool ok = false;
    int64_t raw = 0;

    switch (def.type)
    {
        case ValueType::Number:
        case ValueType::TimeToGo:
        case ValueType::Code:
        case ValueType::BitmaskDecimal:
            ok = parseDecimal(value, raw);
            break;

        case ValueType::ProductId:
        case ValueType::BitmaskHex:
            ok = parseHex(value, raw);
            break;

        case ValueType::OnOff:
            if (equalsIgnoreCase(value, "ON"))       { raw = 1; ok = true; }
            else if (equalsIgnoreCase(value, "OFF")) { raw = 0; ok = true; }
            break;

        case ValueType::Firmware:
            ok = parseFirmware(value, raw);
            break;

        case ValueType::FirmwareExt:
            ok = parseFirmwareExt(value, raw);
            break;

        case ValueType::Text:
            break;   // oben behandelt
    }

    if (!ok)
    {
        result.status = ValueStatus::Invalid;
        return result;
    }

    result.raw = raw;

    if (def.type == ValueType::TimeToGo && raw == -1)
    {
        // Laut Doku: Batterie wird nicht entladen -> Restlaufzeit unendlich.
        result.status = ValueStatus::Infinite;
        return result;
    }

    if (def.type == ValueType::Number || def.type == ValueType::TimeToGo)
    {
        result.value = static_cast<float>(raw) / static_cast<float>(powerOfTen(def.decimals));
    }
    else if (def.type == ValueType::Firmware || def.type == ValueType::FirmwareExt)
    {
        result.value = static_cast<float>(raw) / 100.0f;   // 419 -> 4.19
    }
    else
    {
        result.value = static_cast<float>(raw);
    }

    result.status = ValueStatus::Valid;
    return result;
}
