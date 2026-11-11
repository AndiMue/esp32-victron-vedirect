/******************************************************************************
 * @file RtcZones.cpp
 *
 * Siehe RtcZones.hpp.
 ******************************************************************************/

#include "RtcZones.hpp"

#include <cstring>

namespace Rtc
{

namespace
{
// Regeln: EU-Sommerzeit letzter Sonntag im März bis letzter Sonntag im Oktober.
const TimeZone Zones[] =
{
    { "berlin",   "Deutschland (MEZ/MESZ)",           "CET-1CEST,M3.5.0,M10.5.0/3"    },
    { "utc",      "UTC (ohne Sommerzeit)",             "UTC0"                          },
    { "london",   "Großbritannien (GMT/BST)",          "GMT0BST,M3.5.0/1,M10.5.0"      },
    { "lisbon",   "Portugal (WEZ/WESZ)",               "WET0WEST,M3.5.0/1,M10.5.0"     },
    { "helsinki", "Osteuropa, z. B. Finnland (OEZ/OESZ)", "EET-2EEST,M3.5.0/3,M10.5.0/4" },
};

const char* const Servers[] =
{
    "pool.ntp.org",       // Standard (Projektinhaber 11.11.2026)
    "de.pool.ntp.org",
    "ptbtime1.ptb.de",    // Physikalisch-Technische Bundesanstalt
    "fritz.box",          // eigener Router (FRITZ!Box)
    "time.google.com",
};
} // namespace

const TimeZone* timeZones(std::size_t& count)
{
    count = sizeof(Zones) / sizeof(Zones[0]);
    return Zones;
}

const TimeZone& timeZone(const char* key)
{
    if (key != nullptr)
    {
        for (const TimeZone& z : Zones)
        {
            if (std::strcmp(z.key, key) == 0)
            {
                return z;
            }
        }
    }
    return Zones[0];
}

const char* const* ntpServers(std::size_t& count)
{
    count = sizeof(Servers) / sizeof(Servers[0]);
    return Servers;
}

bool isValidHostName(const char* name)
{
    if (name == nullptr)
    {
        return false;
    }
    const std::size_t len = std::strlen(name);
    if (len == 0 || len > 63 || name[0] == '.' || name[0] == '-')
    {
        return false;
    }
    for (std::size_t i = 0; i < len; ++i)
    {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

} // namespace Rtc
