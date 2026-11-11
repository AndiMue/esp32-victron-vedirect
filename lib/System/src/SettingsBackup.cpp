/******************************************************************************
 * @file SettingsBackup.cpp
 *
 * Siehe SettingsBackup.hpp.
 ******************************************************************************/

#include "SettingsBackup.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "RtcZones.hpp"   // Zeitzonen-Schlüssel, Prüfung von Hostnamen

namespace
{

const char* const FormatName = "vedirect-backup";
constexpr std::size_t MaxTextSize = 8192;   ///< größere Dateien sind sicher keine Sicherung

/******************************************************************************
 * Schreiben
 ******************************************************************************/
void appendEscaped(std::string& out, const std::string& s)
{
    out += '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20)
                {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                }
                else
                {
                    out += static_cast<char>(c);   // UTF-8 unverändert
                }
        }
    }
    out += '"';
}

class Writer
{
public:
    Writer() { m_out = "{"; }

    void str(const char* key, const std::string& value) { next(key); appendEscaped(m_out, value); }
    void boolean(const char* key, bool value)          { next(key); m_out += value ? "true" : "false"; }
    void number(const char* key, uint32_t value)        { next(key); m_out += std::to_string(value); }
    void decimal(const char* key, float value)
    {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(value));
        next(key);
        m_out += buf;
    }

    std::string finish() { return m_out + "\n}\n"; }

private:
    void next(const char* key)
    {
        m_out += m_first ? "\n  " : ",\n  ";
        m_first = false;
        appendEscaped(m_out, key);
        m_out += ": ";
    }
    std::string m_out;
    bool        m_first = true;
};

/******************************************************************************
 * Lesen: flaches JSON-Objekt { "schlüssel": wert, ... }
 * Werte: Text, Zahl, true/false, null. Verschachtelung wird abgelehnt.
 ******************************************************************************/
struct Value
{
    enum class Type : uint8_t { String, Number, Bool, Null } type = Type::Null;
    std::string text;
    double      number = 0;
    bool        boolean = false;
};

struct Entry
{
    std::string key;
    Value       value;
};

class Parser
{
public:
    explicit Parser(const char* text) : m_p(text) {}

    bool parse(std::vector<Entry>& out, std::string& error)
    {
        skipWs();
        if (*m_p != '{') { error = "Datei ist keine Sicherung (kein JSON-Objekt)"; return false; }
        ++m_p;
        skipWs();
        if (*m_p == '}') { ++m_p; return true; }
        for (;;)
        {
            Entry e;
            skipWs();
            if (!parseString(e.key)) { error = "Datei fehlerhaft (Schlüssel erwartet)"; return false; }
            skipWs();
            if (*m_p != ':') { error = "Datei fehlerhaft (':' erwartet)"; return false; }
            ++m_p;
            skipWs();
            if (!parseValue(e.value)) { error = "Datei fehlerhaft (Wert bei \"" + e.key + "\")"; return false; }
            out.push_back(e);
            skipWs();
            if (*m_p == ',') { ++m_p; continue; }
            if (*m_p == '}') { ++m_p; break; }
            error = "Datei fehlerhaft (',' oder '}' erwartet)";
            return false;
        }
        skipWs();
        if (*m_p != '\0') { error = "Datei fehlerhaft (Text nach dem Ende)"; return false; }
        return true;
    }

private:
    void skipWs()
    {
        while (*m_p == ' ' || *m_p == '\t' || *m_p == '\n' || *m_p == '\r') ++m_p;
    }

    static int hex(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool parseString(std::string& out)
    {
        if (*m_p != '"') return false;
        ++m_p;
        while (*m_p != '"')
        {
            const char c = *m_p;
            if (c == '\0' || static_cast<unsigned char>(c) < 0x20) return false;
            if (c != '\\') { out += c; ++m_p; continue; }
            ++m_p;
            switch (*m_p)
            {
                case '"':  out += '"';  break;
                case '\\': out += '\\'; break;
                case '/':  out += '/';  break;
                case 'n':  out += '\n'; break;
                case 'r':  out += '\r'; break;
                case 't':  out += '\t'; break;
                case 'b':  out += '\b'; break;
                case 'f':  out += '\f'; break;
                case 'u':
                {
                    unsigned code = 0;
                    for (int i = 1; i <= 4; ++i)
                    {
                        const int v = hex(m_p[i]);
                        if (v < 0) return false;
                        code = code * 16 + static_cast<unsigned>(v);
                    }
                    m_p += 4;
                    // als UTF-8 (Ersatzpaare werden nicht unterstützt)
                    if (code < 0x80) out += static_cast<char>(code);
                    else if (code < 0x800)
                    {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    else
                    {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    break;
                }
                default: return false;
            }
            ++m_p;
        }
        ++m_p;
        return true;
    }

    bool parseValue(Value& v)
    {
        if (*m_p == '"')
        {
            v.type = Value::Type::String;
            return parseString(v.text);
        }
        if (std::strncmp(m_p, "true", 4) == 0)  { v.type = Value::Type::Bool; v.boolean = true;  m_p += 4; return true; }
        if (std::strncmp(m_p, "false", 5) == 0) { v.type = Value::Type::Bool; v.boolean = false; m_p += 5; return true; }
        if (std::strncmp(m_p, "null", 4) == 0)  { v.type = Value::Type::Null; m_p += 4; return true; }
        if (*m_p == '-' || (*m_p >= '0' && *m_p <= '9'))
        {
            char* end = nullptr;
            v.number = std::strtod(m_p, &end);
            if (end == m_p) return false;
            v.type = Value::Type::Number;
            m_p = end;
            return true;
        }
        return false;   // Objekte/Listen: nicht Teil des Formats
    }

    const char* m_p;
};

/******************************************************************************
 * Zugriff mit Typprüfung
 ******************************************************************************/
class Reader
{
public:
    explicit Reader(const std::vector<Entry>& entries) : m_entries(entries) {}

    const Value* find(const std::string& key) const
    {
        for (const Entry& e : m_entries)
        {
            if (e.key == key) return &e.value;
        }
        return nullptr;
    }

    bool str(const std::string& key, std::string& out, std::string& error) const
    {
        const Value* v = find(key);
        if (v == nullptr || v->type != Value::Type::String)
        {
            error = "Eintrag \"" + key + "\" fehlt oder ist kein Text";
            return false;
        }
        out = v->text;
        return true;
    }

    bool boolean(const std::string& key, bool& out, std::string& error) const
    {
        const Value* v = find(key);
        if (v == nullptr || v->type != Value::Type::Bool)
        {
            error = "Eintrag \"" + key + "\" fehlt oder ist nicht true/false";
            return false;
        }
        out = v->boolean;
        return true;
    }

    bool number(const std::string& key, double& out, std::string& error) const
    {
        const Value* v = find(key);
        if (v == nullptr || v->type != Value::Type::Number)
        {
            error = "Eintrag \"" + key + "\" fehlt oder ist keine Zahl";
            return false;
        }
        out = v->number;
        return true;
    }

    /// Ganzzahl im Bereich min..max.
    bool integer(const std::string& key, uint32_t min, uint32_t max, uint32_t& out, std::string& error) const
    {
        double d = 0;
        if (!number(key, d, error)) return false;
        if (d != std::floor(d) || d < min || d > max)
        {
            error = "Eintrag \"" + key + "\" außerhalb " + std::to_string(min) + ".." + std::to_string(max);
            return false;
        }
        out = static_cast<uint32_t>(d);
        return true;
    }

private:
    const std::vector<Entry>& m_entries;
};

bool hasForbiddenTopicChars(const std::string& s)
{
    return s.find('#') != std::string::npos || s.find('+') != std::string::npos;
}

std::string relayKey(std::size_t i, const char* name)
{
    return "relay" + std::to_string(i + 1) + "." + name;
}

} // namespace

/******************************************************************************
 * Öffentliche Funktionen
 ******************************************************************************/
std::string backupToJson(const BackupData& d)
{
    Writer w;
    w.str("format", FormatName);
    w.number("version", BackupData::FormatVersion);
    w.str("firmware", d.firmware);
    w.str("created", d.created);
    w.str("installation", d.balcony ? "balcony" : "camper");
    for (std::size_t i = 0; i < RelayCount; ++i)
    {
        const RelayLimits& l = d.relays[i];
        w.decimal(relayKey(i, "lock").c_str(), l.lockBelowSoc);
        w.decimal(relayKey(i, "off").c_str(), l.offBelowSoc);
        w.decimal(relayKey(i, "release").c_str(), l.releaseAtSoc);
        w.number(relayKey(i, "minOffS").c_str(), l.minOffTimeMs / 1000);
        w.boolean(relayKey(i, "startup").c_str(), l.startupRequest);
        w.boolean(relayKey(i, "auto").c_str(), l.rememberRequest);
    }
    w.str("clock.tz", d.tz);
    w.str("clock.ntp", d.ntp);
    w.boolean("mqtt.enabled", d.mqttEnabled);
    w.str("mqtt.host", d.mqttHost);
    w.number("mqtt.port", d.mqttPort);
    w.str("mqtt.user", d.mqttUser);
    w.str("mqtt.base", d.mqttBase);
    w.number("mqtt.interval", d.mqttInterval);
    w.str("mqtt.client", d.mqttClient);
    w.str("wifi.ssid", d.wifiSsid);
    w.boolean("wifi.apAlways", d.apAlways);
    w.str("wifi.hostname", d.hostname);
    return w.finish();
}

bool backupFromJson(const char* text, BackupData& out, std::string& error)
{
    if (text == nullptr || std::strlen(text) > MaxTextSize)
    {
        error = "Datei zu groß oder leer";
        return false;
    }
    std::vector<Entry> entries;
    Parser parser(text);
    if (!parser.parse(entries, error))
    {
        return false;
    }
    const Reader r(entries);
    BackupData d;

    // ---- Kennung und Version ----
    std::string format;
    if (!r.str("format", format, error) || format != FormatName)
    {
        error = "Datei ist keine Sicherung dieses Geräts";
        return false;
    }
    uint32_t version = 0;
    if (!r.integer("version", 1, 1000, version, error)) return false;
    if (version > BackupData::FormatVersion)
    {
        error = "Sicherung stammt aus einer neueren Firmware (Format " + std::to_string(version) +
                "), bitte erst die Firmware aktualisieren";
        return false;
    }
    r.str("firmware", d.firmware, error);   // nur Information, darf fehlen
    r.str("created", d.created, error);
    error.clear();

    // ---- Anlage ----
    std::string inst;
    if (!r.str("installation", inst, error)) return false;
    if (inst != "camper" && inst != "balcony")
    {
        error = "Anlage muss camper oder balcony sein";
        return false;
    }
    d.balcony = inst == "balcony";

    // ---- Relais ----
    for (std::size_t i = 0; i < RelayCount; ++i)
    {
        double lock = 0, off = 0, rel = 0;
        uint32_t minOffS = 0;
        RelayLimits& l = d.relays[i];
        if (!r.number(relayKey(i, "lock"), lock, error) ||
            !r.number(relayKey(i, "off"), off, error) ||
            !r.number(relayKey(i, "release"), rel, error) ||
            !r.integer(relayKey(i, "minOffS"), 0, RelayLimits::MaxMinOffTimeMs / 1000, minOffS, error) ||
            !r.boolean(relayKey(i, "startup"), l.startupRequest, error) ||
            !r.boolean(relayKey(i, "auto"), l.rememberRequest, error))
        {
            return false;
        }
        // auf 0,1 % runden (wie im Gerät gespeichert)
        l.lockBelowSoc = static_cast<float>(std::floor(lock * 10 + 0.5) / 10);
        l.offBelowSoc  = static_cast<float>(std::floor(off * 10 + 0.5) / 10);
        l.releaseAtSoc = static_cast<float>(std::floor(rel * 10 + 0.5) / 10);
        l.minOffTimeMs = minOffS * 1000;
        const char* invalid = l.validate();
        if (invalid != nullptr)
        {
            error = "Relais R" + std::to_string(i + 1) + ": " + invalid;
            return false;
        }
    }

    // ---- Uhr ----
    if (!r.str("clock.tz", d.tz, error) || !r.str("clock.ntp", d.ntp, error)) return false;
    if (std::strcmp(Rtc::timeZone(d.tz.c_str()).key, d.tz.c_str()) != 0)
    {
        error = "Unbekannte Zeitzone \"" + d.tz + "\"";
        return false;
    }
    if (!Rtc::isValidHostName(d.ntp.c_str()))
    {
        error = "NTP-Server ungültig";
        return false;
    }

    // ---- MQTT (ohne Passwort) ----
    if (!r.boolean("mqtt.enabled", d.mqttEnabled, error) ||
        !r.str("mqtt.host", d.mqttHost, error) ||
        !r.integer("mqtt.port", 1, 65535, d.mqttPort, error) ||
        !r.str("mqtt.user", d.mqttUser, error) ||
        !r.str("mqtt.base", d.mqttBase, error) ||
        !r.integer("mqtt.interval", 1, 3600, d.mqttInterval, error) ||
        !r.str("mqtt.client", d.mqttClient, error))
    {
        return false;
    }
    if (d.mqttEnabled && !Rtc::isValidHostName(d.mqttHost.c_str()))
    {
        error = "MQTT-Broker ungültig";
        return false;
    }
    if (!d.mqttHost.empty() && !Rtc::isValidHostName(d.mqttHost.c_str()))
    {
        error = "MQTT-Broker ungültig";
        return false;
    }
    if (d.mqttBase.empty() || hasForbiddenTopicChars(d.mqttBase) || d.mqttBase.size() > 64 ||
        d.mqttUser.size() > 64 || d.mqttClient.size() > 64)
    {
        error = "MQTT-Basis-Topic, Benutzer oder Client-ID ungültig";
        return false;
    }

    // ---- WLAN (ohne Passwort) ----
    if (!r.str("wifi.ssid", d.wifiSsid, error) ||
        !r.boolean("wifi.apAlways", d.apAlways, error) ||
        !r.str("wifi.hostname", d.hostname, error))
    {
        return false;
    }
    if (d.wifiSsid.size() > 32)
    {
        error = "WLAN-Name zu lang (max. 32 Zeichen)";
        return false;
    }
    if (!d.hostname.empty() && !Rtc::isValidHostName(d.hostname.c_str()))
    {
        error = "Hostname ungültig";
        return false;
    }

    out = d;
    error.clear();
    return true;
}
