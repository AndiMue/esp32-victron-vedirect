/******************************************************************************
 * @file RtcTime.cpp
 *
 * Siehe RtcTime.hpp.
 ******************************************************************************/

#include "RtcTime.hpp"

namespace Rtc
{

bool isLeapYear(uint16_t year)
{
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

uint8_t daysInMonth(uint16_t year, uint8_t month)
{
    static const uint8_t days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (month < 1 || month > 12)
    {
        return 0;
    }
    return (month == 2 && isLeapYear(year)) ? 29 : days[month - 1];
}

bool DateTime::isValid() const
{
    return year >= 2000 && year <= 2099 &&
           month >= 1 && month <= 12 &&
           day >= 1 && day <= daysInMonth(year, month) &&
           hour <= 23 && minute <= 59 && second <= 59;
}

namespace
{
/// Tage seit 2000-01-01 (gültiges Datum vorausgesetzt).
uint32_t daysSince2000(const DateTime& t)
{
    uint32_t days = 0;
    for (uint16_t y = 2000; y < t.year; ++y)
    {
        days += isLeapYear(y) ? 366 : 365;
    }
    for (uint8_t m = 1; m < t.month; ++m)
    {
        days += daysInMonth(t.year, m);
    }
    return days + (t.day - 1);
}
} // namespace

uint8_t weekday(const DateTime& t)
{
    // 2000-01-01 war ein Samstag (6)
    return static_cast<uint8_t>((daysSince2000(t) + 6) % 7);
}

uint32_t toUnix(const DateTime& t)
{
    if (!t.isValid())
    {
        return 0;
    }
    return Epoch2000 + daysSince2000(t) * 86400UL +
           t.hour * 3600UL + t.minute * 60UL + t.second;
}

bool fromUnix(uint32_t unix, DateTime& out)
{
    if (unix < Epoch2000)
    {
        return false;
    }
    uint32_t seconds = unix - Epoch2000;
    uint32_t days    = seconds / 86400UL;
    seconds %= 86400UL;

    DateTime t;
    t.hour   = static_cast<uint8_t>(seconds / 3600);
    t.minute = static_cast<uint8_t>((seconds % 3600) / 60);
    t.second = static_cast<uint8_t>(seconds % 60);

    uint16_t year = 2000;
    for (;;)
    {
        const uint16_t len = isLeapYear(year) ? 366 : 365;
        if (days < len) break;
        days -= len;
        if (++year > 2099) return false;
    }
    uint8_t month = 1;
    for (;;)
    {
        const uint8_t len = daysInMonth(year, month);
        if (days < len) break;
        days -= len;
        ++month;
    }
    t.year  = year;
    t.month = month;
    t.day   = static_cast<uint8_t>(days + 1);
    out = t;
    return true;
}

uint8_t toBcd(uint8_t value)
{
    return static_cast<uint8_t>(((value / 10) << 4) | (value % 10));
}

uint8_t fromBcd(uint8_t bcd)
{
    return static_cast<uint8_t>((bcd >> 4) * 10 + (bcd & 0x0F));
}

} // namespace Rtc
