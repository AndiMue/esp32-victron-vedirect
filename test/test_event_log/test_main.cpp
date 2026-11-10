/******************************************************************************
 * @file test_main.cpp
 *
 * Unit-Tests für EventLog (W4a Schritt 4, Ereignisliste im Reiter "Fehler").
 *
 * Ausführen:
 *   pio test -e native     -> auf dem PC
 *   pio test -e esp32dev   -> auf dem ESP32
 ******************************************************************************/

#include <unity.h>

#include <cstdio>
#include <cstring>

#include "EventLog.hpp"

namespace
{
EventLog g_log;   // ca. 5 KB, nicht auf den Stack
}

void setUp()    { g_log.clear(); }
void tearDown() {}

/// Leere Liste (läuft als erster Test, daher auch noch keine Nummer)
void test_empty()
{
    TEST_ASSERT_EQUAL_UINT32(0, g_log.size());
    TEST_ASSERT_EQUAL_UINT32(0, g_log.lastSeq());
}

/// Neuester Eintrag steht vorne, Nummern laufen hoch
void test_order_newest_first()
{
    const uint32_t seqBefore = g_log.lastSeq();
    g_log.add(100, Severity::Info, "eins");
    g_log.add(200, Severity::Warning, "zwei");
    g_log.add(300, Severity::Critical, "drei");

    TEST_ASSERT_EQUAL_UINT32(3, g_log.size());
    TEST_ASSERT_EQUAL_STRING("drei", g_log.newest(0).text);
    TEST_ASSERT_EQUAL_STRING("zwei", g_log.newest(1).text);
    TEST_ASSERT_EQUAL_STRING("eins", g_log.newest(2).text);
    TEST_ASSERT_EQUAL_UINT32(seqBefore + 3, g_log.newest(0).seq);
    TEST_ASSERT_EQUAL_UINT32(300, g_log.newest(0).timeMs);
    TEST_ASSERT_TRUE(g_log.newest(0).severity == Severity::Critical);
    TEST_ASSERT_EQUAL_UINT32(seqBefore + 3, g_log.lastSeq());
}

/// Volle Liste: ältester Eintrag wird überschrieben
void test_overwrite_oldest()
{
    const uint32_t seqBefore = g_log.lastSeq();   // Nummern laufen über clear() hinweg weiter
    char text[16];
    for (unsigned i = 1; i <= EventLog::Capacity + 5; ++i)
    {
        std::snprintf(text, sizeof(text), "e%u", i);
        g_log.add(i, Severity::Info, text);
    }
    TEST_ASSERT_EQUAL_UINT32(EventLog::Capacity, g_log.size());
    std::snprintf(text, sizeof(text), "e%u", static_cast<unsigned>(EventLog::Capacity + 5));
    TEST_ASSERT_EQUAL_STRING(text, g_log.newest(0).text);
    TEST_ASSERT_EQUAL_STRING("e6", g_log.newest(EventLog::Capacity - 1).text);   // e1..e5 überschrieben
    TEST_ASSERT_EQUAL_UINT32(seqBefore + EventLog::Capacity + 5, g_log.lastSeq());
}

/// Zu langer Text wird gekürzt und bleibt nullterminiert
void test_truncate_ascii()
{
    char text[200];
    std::memset(text, 'x', sizeof(text) - 1);
    text[sizeof(text) - 1] = '\0';
    g_log.add(1, Severity::Info, text);
    TEST_ASSERT_EQUAL_UINT32(EventLog::TextSize - 1, std::strlen(g_log.newest(0).text));
}

/// Kürzen nie mitten in einem Umlaut (UTF-8, 2 Bytes)
void test_truncate_utf8_boundary()
{
    // TextSize-2 Bytes 'x', dann "ü" (0xC3 0xBC): das "ü" passt nicht mehr
    // ganz hinein und muss komplett entfallen.
    char text[EventLog::TextSize + 8];
    std::memset(text, 'x', EventLog::TextSize - 2);
    std::strcpy(text + EventLog::TextSize - 2, "\xC3\xBC" "ende");
    g_log.add(1, Severity::Info, text);
    TEST_ASSERT_EQUAL_UINT32(EventLog::TextSize - 2, std::strlen(g_log.newest(0).text));
}

/// nullptr als Text ist erlaubt (leerer Text)
void test_null_text()
{
    g_log.add(1, Severity::Info, nullptr);
    TEST_ASSERT_EQUAL_STRING("", g_log.newest(0).text);
}

/// clear() leert die Liste, Nummern laufen weiter
void test_clear_keeps_sequence()
{
    const uint32_t seqBefore = g_log.lastSeq();
    g_log.add(1, Severity::Info, "a");
    g_log.add(2, Severity::Info, "b");
    g_log.clear();
    TEST_ASSERT_EQUAL_UINT32(0, g_log.size());
    g_log.add(3, Severity::Info, "c");
    TEST_ASSERT_EQUAL_UINT32(seqBefore + 3, g_log.newest(0).seq);
}

int runTests()
{
    UNITY_BEGIN();
    RUN_TEST(test_empty);
    RUN_TEST(test_order_newest_first);
    RUN_TEST(test_overwrite_oldest);
    RUN_TEST(test_truncate_ascii);
    RUN_TEST(test_truncate_utf8_boundary);
    RUN_TEST(test_null_text);
    RUN_TEST(test_clear_keeps_sequence);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>
void setup()
{
    delay(2000);   // Zeit für den seriellen Monitor
    runTests();
}
void loop() {}
#else
int main()
{
    return runTests();
}
#endif
