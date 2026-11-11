/******************************************************************************
 * @file test_main.cpp
 *
 * Unit-Tests für den RTC-Minitreiber (lib/RtcPcf8523, W4e Schritt 1):
 * Datum <-> Unix-Zeit, BCD, PCF8523-Register.
 *
 * Ausführen:
 *   pio test -e native     -> auf dem PC
 ******************************************************************************/

#include <unity.h>

#include "Pcf8523.hpp"
#include "RtcTime.hpp"
#include "RtcZones.hpp"

using Rtc::DateTime;

namespace
{
DateTime make(uint16_t y, uint8_t mo, uint8_t d, uint8_t h, uint8_t mi, uint8_t s)
{
    DateTime t;
    t.year = y; t.month = mo; t.day = d; t.hour = h; t.minute = mi; t.second = s;
    return t;
}
} // namespace

void setUp()    {}
void tearDown() {}

/******************************************************************************
 * Kalender
 ******************************************************************************/
void test_leap_years()
{
    TEST_ASSERT_TRUE(Rtc::isLeapYear(2000));    // durch 400 teilbar
    TEST_ASSERT_TRUE(Rtc::isLeapYear(2028));
    TEST_ASSERT_FALSE(Rtc::isLeapYear(2026));
    TEST_ASSERT_FALSE(Rtc::isLeapYear(2100));   // durch 100, nicht 400
    TEST_ASSERT_EQUAL_UINT8(29, Rtc::daysInMonth(2028, 2));
    TEST_ASSERT_EQUAL_UINT8(28, Rtc::daysInMonth(2026, 2));
    TEST_ASSERT_EQUAL_UINT8(0, Rtc::daysInMonth(2026, 13));
}

void test_validity()
{
    TEST_ASSERT_TRUE(make(2026, 11, 11, 17, 42, 5).isValid());
    TEST_ASSERT_TRUE(make(2028, 2, 29, 0, 0, 0).isValid());
    TEST_ASSERT_FALSE(make(2026, 2, 29, 0, 0, 0).isValid());   // kein Schaltjahr
    TEST_ASSERT_FALSE(make(2026, 4, 31, 0, 0, 0).isValid());
    TEST_ASSERT_FALSE(make(2026, 1, 1, 24, 0, 0).isValid());
    TEST_ASSERT_FALSE(make(1999, 12, 31, 0, 0, 0).isValid());
    TEST_ASSERT_FALSE(make(2100, 1, 1, 0, 0, 0).isValid());
}

void test_weekday()
{
    TEST_ASSERT_EQUAL_UINT8(6, Rtc::weekday(make(2000, 1, 1, 0, 0, 0)));     // Samstag
    TEST_ASSERT_EQUAL_UINT8(3, Rtc::weekday(make(2026, 11, 11, 12, 0, 0)));  // Mittwoch
    TEST_ASSERT_EQUAL_UINT8(2, Rtc::weekday(make(2028, 2, 29, 0, 0, 0)));    // Dienstag
}

/******************************************************************************
 * Unix-Zeit (Referenzwerte z. B. aus "date -u -d ... +%s")
 ******************************************************************************/
void test_to_unix_known_values()
{
    TEST_ASSERT_EQUAL_UINT32(946684800UL,  Rtc::toUnix(make(2000, 1, 1, 0, 0, 0)));
    TEST_ASSERT_EQUAL_UINT32(1700000000UL, Rtc::toUnix(make(2023, 11, 14, 22, 13, 20)));
    TEST_ASSERT_EQUAL_UINT32(1798761600UL, Rtc::toUnix(make(2027, 1, 1, 0, 0, 0)));
    TEST_ASSERT_EQUAL_UINT32(1835395200UL, Rtc::toUnix(make(2028, 2, 29, 0, 0, 0)));
    TEST_ASSERT_EQUAL_UINT32(0, Rtc::toUnix(make(2026, 2, 30, 0, 0, 0)));   // ungültig
}

void test_from_unix_roundtrip()
{
    const DateTime samples[] = {
        make(2000, 1, 1, 0, 0, 0),
        make(2026, 11, 11, 17, 42, 5),
        make(2028, 2, 29, 23, 59, 59),
        make(2028, 3, 1, 0, 0, 0),
        make(2099, 12, 31, 23, 59, 59),
    };
    for (const DateTime& s : samples)
    {
        DateTime back;
        TEST_ASSERT_TRUE(Rtc::fromUnix(Rtc::toUnix(s), back));
        TEST_ASSERT_EQUAL_UINT16(s.year, back.year);
        TEST_ASSERT_EQUAL_UINT8(s.month, back.month);
        TEST_ASSERT_EQUAL_UINT8(s.day, back.day);
        TEST_ASSERT_EQUAL_UINT8(s.hour, back.hour);
        TEST_ASSERT_EQUAL_UINT8(s.minute, back.minute);
        TEST_ASSERT_EQUAL_UINT8(s.second, back.second);
    }
}

void test_from_unix_out_of_range()
{
    DateTime t;
    TEST_ASSERT_FALSE(Rtc::fromUnix(946684799UL, t));   // 1999
    TEST_ASSERT_FALSE(Rtc::fromUnix(4102444800UL, t));  // 2100-01-01
}

/******************************************************************************
 * BCD und PCF8523-Register
 ******************************************************************************/
void test_bcd()
{
    TEST_ASSERT_EQUAL_HEX8(0x59, Rtc::toBcd(59));
    TEST_ASSERT_EQUAL_HEX8(0x00, Rtc::toBcd(0));
    TEST_ASSERT_EQUAL_UINT8(59, Rtc::fromBcd(0x59));
    TEST_ASSERT_EQUAL_UINT8(26, Rtc::fromBcd(0x26));
}

void test_pcf8523_encode()
{
    uint8_t r[Pcf8523Codec::TimeRegs] = {};
    TEST_ASSERT_TRUE(Pcf8523Codec::encode(make(2026, 11, 11, 17, 42, 5), r));
    const uint8_t expected[] = { 0x05, 0x42, 0x17, 0x11, 3, 0x11, 0x26 };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, r, 7);
    TEST_ASSERT_FALSE(Pcf8523Codec::encode(make(2026, 2, 30, 0, 0, 0), r));
}

void test_pcf8523_decode()
{
    const uint8_t r[] = { 0x05, 0x42, 0x17, 0x11, 3, 0x11, 0x26 };
    DateTime t;
    TEST_ASSERT_TRUE(Pcf8523Codec::decode(r, t));
    TEST_ASSERT_EQUAL_UINT16(2026, t.year);
    TEST_ASSERT_EQUAL_UINT8(11, t.month);
    TEST_ASSERT_EQUAL_UINT8(11, t.day);
    TEST_ASSERT_EQUAL_UINT8(17, t.hour);
    TEST_ASSERT_EQUAL_UINT8(42, t.minute);
    TEST_ASSERT_EQUAL_UINT8(5, t.second);
}

void test_pcf8523_oscillator_stopped_is_invalid()
{
    // Erster Start / Stromausfall ohne Knopfzelle: OS-Flag gesetzt
    const uint8_t r[] = { 0x80 | 0x05, 0x42, 0x17, 0x11, 3, 0x11, 0x26 };
    DateTime t = make(2030, 1, 1, 0, 0, 0);
    TEST_ASSERT_FALSE(Pcf8523Codec::decode(r, t));
    TEST_ASSERT_EQUAL_UINT16(2030, t.year);   // unverändert
}

void test_pcf8523_garbage_is_invalid()
{
    const uint8_t r[] = { 0x05, 0x42, 0x17, 0x31, 3, 0x02, 0x26 };   // 31. Februar
    DateTime t;
    TEST_ASSERT_FALSE(Pcf8523Codec::decode(r, t));
}

void test_pcf8523_control_registers()
{
    // Control_1: STOP und 12-h-Modus aus, Rest (z. B. CAP_SEL Bit 7) bleibt
    TEST_ASSERT_EQUAL_HEX8(0x80, Pcf8523Codec::runningControl1(0x80 | 0x20 | 0x08));
    // Control_3: Werkszustand PM=111 -> 000 (Umschaltung auf Knopfzelle an)
    TEST_ASSERT_EQUAL_HEX8(0x00, Pcf8523Codec::runningControl3(0xE0));
    TEST_ASSERT_EQUAL_HEX8(0x00, Pcf8523Codec::runningControl3(0xE7));
}

/******************************************************************************
 * Zeitzonen und NTP-Server (W4e Schritt 3)
 ******************************************************************************/
void test_time_zones()
{
    std::size_t n = 0;
    const Rtc::TimeZone* z = Rtc::timeZones(n);
    TEST_ASSERT_TRUE(n >= 2);
    TEST_ASSERT_EQUAL_STRING("berlin", z[0].key);                        // Standard
    TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", z[0].posix);
    TEST_ASSERT_EQUAL_STRING("UTC0", Rtc::timeZone("utc").posix);
    TEST_ASSERT_EQUAL_STRING("berlin", Rtc::timeZone("gibtsnicht").key); // Rückfall
    TEST_ASSERT_EQUAL_STRING("berlin", Rtc::timeZone(nullptr).key);
    for (std::size_t i = 0; i < n; ++i)
    {
        TEST_ASSERT_TRUE(z[i].label[0] != '\0');
        TEST_ASSERT_TRUE(z[i].posix[0] != '\0');
    }
}

void test_ntp_servers_and_host_names()
{
    std::size_t n = 0;
    const char* const* s = Rtc::ntpServers(n);
    TEST_ASSERT_EQUAL_STRING("pool.ntp.org", s[0]);                      // Standard
    for (std::size_t i = 0; i < n; ++i)
    {
        TEST_ASSERT_TRUE(Rtc::isValidHostName(s[i]));
    }
    TEST_ASSERT_TRUE(Rtc::isValidHostName("192.168.178.1"));
    TEST_ASSERT_FALSE(Rtc::isValidHostName(""));
    TEST_ASSERT_FALSE(Rtc::isValidHostName(nullptr));
    TEST_ASSERT_FALSE(Rtc::isValidHostName("pool ntp.org"));
    TEST_ASSERT_FALSE(Rtc::isValidHostName("-pool.ntp.org"));
    TEST_ASSERT_FALSE(Rtc::isValidHostName("http://pool.ntp.org"));
}

/******************************************************************************
 * Testlauf
 ******************************************************************************/
int runAllTests()
{
    UNITY_BEGIN();
    RUN_TEST(test_leap_years);
    RUN_TEST(test_validity);
    RUN_TEST(test_weekday);
    RUN_TEST(test_to_unix_known_values);
    RUN_TEST(test_from_unix_roundtrip);
    RUN_TEST(test_from_unix_out_of_range);
    RUN_TEST(test_bcd);
    RUN_TEST(test_pcf8523_encode);
    RUN_TEST(test_pcf8523_decode);
    RUN_TEST(test_pcf8523_oscillator_stopped_is_invalid);
    RUN_TEST(test_pcf8523_garbage_is_invalid);
    RUN_TEST(test_pcf8523_control_registers);
    RUN_TEST(test_time_zones);
    RUN_TEST(test_ntp_servers_and_host_names);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>

void setup()
{
    delay(2000);
    runAllTests();
}

void loop()
{
}
#else
int main()
{
    return runAllTests();
}
#endif
