/******************************************************************************
 * @file test_main.cpp
 *
 * Unit-Tests für HexRegisterCache / HexJobRunner (W4b Schritt 3).
 * Ein kleines simuliertes Gerät beantwortet die Anfragen (die echten
 * Simulatoren können noch kein HEX).
 *
 * Ausführen:
 *   pio test -e native     -> auf dem PC
 ******************************************************************************/

#include <unity.h>

#include <cstring>
#include <string>

#include "VeDirectHexReader.hpp"

using namespace VeHex;

namespace
{

/// Simuliertes Gerät: kennt einige Register, antwortet wie ein Victron-Gerät.
struct FakeDevice
{
    bool     answers   = true;    ///< false = antwortet nie (z. B. TX nicht verbunden)
    int      dropFirst = 0;       ///< so viele Anfragen verschlucken (Störung)
    uint16_t unknownId = 0xEDAB;  ///< dieses Register "gibt es nicht"
    uint16_t emptyDay  = 0x106E;  ///< dieser Historientag hat keine Daten
    int      requests  = 0;

    /// Anfrage zerlegen und ggf. Antwort als Nachricht liefern.
    bool respond(const Request& request, Message& answer)
    {
        ++requests;
        VeDirectHexParser p;
        Message q;
        for (std::size_t i = 0; i < request.length; ++i)
        {
            if (p.process(static_cast<uint8_t>(request.text[i])) == VeDirectHexParser::Event::MessageReady)
            {
                q = p.message();
            }
        }
        if (!answers || dropFirst-- > 0)
        {
            return false;
        }
        answer = Message();
        answer.code = static_cast<uint8_t>(Response::Get);
        answer.data[0] = q.data[0];
        answer.data[1] = q.data[1];
        const uint16_t id = q.registerId();
        if (id == unknownId)
        {
            answer.data[2] = FlagUnknownId;
            answer.length  = 3;
        }
        else if (id == emptyDay)
        {
            answer.data[2] = FlagParameterError;
            answer.length  = 3 + 34;
        }
        else
        {
            answer.data[2] = 0;
            answer.data[3] = static_cast<uint8_t>(id & 0xFF);   // Wert = Register (LE), leicht prüfbar
            answer.data[4] = static_cast<uint8_t>(id >> 8);
            answer.length  = 5;
        }
        return true;
    }
};

HexRegisterCache g_cache;
HexJobRunner     g_runner;

/// Lauf bis zum Ende simulieren (Zeit in 10-ms-Schritten). Liefert Dauer in ms.
uint32_t run(FakeDevice& dev, uint32_t start = 1000, uint32_t limitMs = 120000)
{
    uint32_t now = start;
    while (g_runner.busy() && now - start < limitMs)
    {
        Request req;
        if (g_runner.poll(now, req, g_cache))
        {
            Message answer;
            if (dev.respond(req, answer))
            {
                g_runner.onMessage(answer, now + 10, g_cache);   // Antwort nach 10 ms
            }
        }
        now += 10;
    }
    return now - start;
}

} // namespace

void setUp()
{
    g_cache.clear();
    g_runner = HexJobRunner();
}
void tearDown() {}

/******************************************************************************
 * Registerlisten
 ******************************************************************************/
void test_job_lists()
{
    std::size_t n = 0;
    const uint16_t* s = HexJobs::shunt(n);
    TEST_ASSERT_EQUAL_UINT32(9, n);
    TEST_ASSERT_EQUAL_HEX16(0x1000, s[0]);

    const uint16_t* m = HexJobs::mppt(n);
    TEST_ASSERT_EQUAL_UINT32(39, n);   // 7 Einstellungen + gesamt + 31 Tage
    TEST_ASSERT_TRUE(n <= HexJobRunner::MaxRegisters);
    TEST_ASSERT_EQUAL_HEX16(0x104F, m[7]);
    TEST_ASSERT_EQUAL_HEX16(0x1050, m[8]);
    TEST_ASSERT_EQUAL_HEX16(0x106E, m[n - 1]);
}

/******************************************************************************
 * Normaler Lauf
 ******************************************************************************/
void test_all_registers_answered()
{
    FakeDevice dev;
    std::size_t n = 0;
    const uint16_t* regs = HexJobs::shunt(n);
    TEST_ASSERT_TRUE(g_runner.start(regs, n, 1000));
    TEST_ASSERT_TRUE(g_runner.busy());

    const uint32_t duration = run(dev);
    TEST_ASSERT_FALSE(g_runner.busy());
    TEST_ASSERT_TRUE(g_runner.finishedNow());
    TEST_ASSERT_FALSE(g_runner.finishedNow());   // nur einmal

    const HexJobRunner::Result& r = g_runner.result();
    TEST_ASSERT_EQUAL_UINT16(9, r.requested);
    TEST_ASSERT_EQUAL_UINT16(9, r.answered);
    TEST_ASSERT_EQUAL_UINT16(0, r.failed);
    TEST_ASSERT_FALSE(r.aborted);
    TEST_ASSERT_TRUE(duration < 500);           // zügig, eine Anfrage nach der anderen
    TEST_ASSERT_EQUAL(9, dev.requests);

    const HexRegisterCache::Entry* e = g_cache.find(0x1000);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(HexRegisterCache::State::Ok, e->state);
    TEST_ASSERT_EQUAL_UINT32(0x1000, readUnsigned(e->value, e->length));
}

void test_unknown_register_and_empty_day()
{
    FakeDevice dev;
    std::size_t n = 0;
    const uint16_t* regs = HexJobs::mppt(n);
    TEST_ASSERT_TRUE(g_runner.start(regs, n, 0));
    run(dev, 0);

    TEST_ASSERT_EQUAL_UINT16(39, g_runner.result().answered);
    TEST_ASSERT_EQUAL(HexRegisterCache::State::Unknown, g_cache.find(0xEDAB)->state);   // kein Lastausgang
    TEST_ASSERT_EQUAL(HexRegisterCache::State::NoData,  g_cache.find(0x106E)->state);   // Tag ohne Daten
    TEST_ASSERT_EQUAL_UINT8(34, g_cache.find(0x106E)->length);
    TEST_ASSERT_EQUAL(HexRegisterCache::State::Ok,      g_cache.find(0x1050)->state);
}

/******************************************************************************
 * Zeitlimit, Wiederholung, Abbruch
 ******************************************************************************/
void test_retry_after_timeout()
{
    FakeDevice dev;
    dev.dropFirst = 1;   // erste Anfrage geht verloren
    const uint16_t regs[] = { 0x1000, 0x1001 };
    g_runner.start(regs, 2, 0);
    const uint32_t duration = run(dev, 0);

    TEST_ASSERT_EQUAL_UINT16(2, g_runner.result().answered);
    TEST_ASSERT_EQUAL_UINT16(0, g_runner.result().failed);
    TEST_ASSERT_EQUAL(3, dev.requests);                      // 1 Wiederholung
    TEST_ASSERT_TRUE(duration >= HexJobRunner::TimeoutMs);   // eine Wartezeit
}

void test_single_register_without_answer_continues()
{
    FakeDevice dev;
    dev.dropFirst = 2;   // erstes Register: beide Versuche verloren
    const uint16_t regs[] = { 0x1000, 0x1001, 0x1002 };
    g_runner.start(regs, 3, 0);
    run(dev, 0);

    const HexJobRunner::Result& r = g_runner.result();
    TEST_ASSERT_FALSE(r.aborted);
    TEST_ASSERT_EQUAL_UINT16(1, r.failed);
    TEST_ASSERT_EQUAL_UINT16(2, r.answered);
    TEST_ASSERT_EQUAL(HexRegisterCache::State::NoAnswer, g_cache.find(0x1000)->state);
    TEST_ASSERT_EQUAL(HexRegisterCache::State::Ok, g_cache.find(0x1001)->state);
}

void test_device_never_answers_aborts_quickly()
{
    FakeDevice dev;
    dev.answers = false;   // z. B. MPPT-Simulator (nur Text)
    std::size_t n = 0;
    const uint16_t* regs = HexJobs::mppt(n);
    g_runner.start(regs, n, 0);
    const uint32_t duration = run(dev, 0);

    const HexJobRunner::Result& r = g_runner.result();
    TEST_ASSERT_TRUE(r.aborted);
    TEST_ASSERT_TRUE(r.finished);
    TEST_ASSERT_EQUAL_UINT16(HexJobRunner::MaxFailsInARow, r.failed);
    TEST_ASSERT_EQUAL(HexJobRunner::MaxFailsInARow * HexJobRunner::MaxAttempts, dev.requests);
    TEST_ASSERT_TRUE(duration <= 2100);   // ca. 2 s, nicht 39 × 1 s
}

void test_start_rejected_while_busy_or_invalid()
{
    const uint16_t regs[] = { 0x1000 };
    TEST_ASSERT_FALSE(g_runner.start(regs, 0, 0));
    TEST_ASSERT_TRUE(g_runner.start(regs, 1, 0));
    TEST_ASSERT_FALSE(g_runner.start(regs, 1, 0));   // läuft schon
    g_runner.cancel();
    TEST_ASSERT_FALSE(g_runner.busy());
    TEST_ASSERT_FALSE(g_runner.finishedNow());       // Abbruch von außen = kein Ergebnis
    TEST_ASSERT_TRUE(g_runner.start(regs, 1, 0));
}

void test_wrong_register_answer_is_not_taken()
{
    const uint16_t regs[] = { 0x1000 };
    g_runner.start(regs, 1, 0);
    Request req;
    TEST_ASSERT_TRUE(g_runner.poll(0, req, g_cache));

    Message other;   // Antwort auf ein anderes Register (z. B. eigene Diagnose-Anfrage)
    other.code = static_cast<uint8_t>(Response::Get);
    other.data[0] = 0x01; other.data[1] = 0x10; other.data[2] = 0; other.length = 5;
    TEST_ASSERT_FALSE(g_runner.onMessage(other, 5, g_cache));
    TEST_ASSERT_TRUE(g_runner.busy());
}

/******************************************************************************
 * Cache und ungefragte Meldungen
 ******************************************************************************/
void test_async_updates_only_known_registers()
{
    Message async;
    async.code = static_cast<uint8_t>(Response::Async);
    async.data[0] = 0x50; async.data[1] = 0x10; async.data[2] = 0;   // 0x1050 (heute)
    async.data[3] = 0x7B; async.length = 4;

    TEST_ASSERT_FALSE(g_cache.store(async, 100, true));   // noch unbekannt -> ignorieren
    TEST_ASSERT_EQUAL_UINT32(0, g_cache.size());

    Message get = async;
    get.code = static_cast<uint8_t>(Response::Get);
    get.data[3] = 0x01;
    TEST_ASSERT_TRUE(g_cache.store(get, 200, false));

    TEST_ASSERT_TRUE(g_cache.store(async, 300, true));    // jetzt bekannt -> aktualisieren
    TEST_ASSERT_EQUAL_UINT8(0x7B, g_cache.find(0x1050)->value[0]);
    TEST_ASSERT_EQUAL_UINT32(300, g_cache.find(0x1050)->updatedMs);
    TEST_ASSERT_EQUAL_UINT32(1, g_cache.size());
}

void test_no_answer_clears_old_value()
{
    Message get;
    get.code = static_cast<uint8_t>(Response::Get);
    get.data[0] = 0x00; get.data[1] = 0x10; get.data[2] = 0; get.data[3] = 0x2C; get.data[4] = 0x01; get.length = 5;
    g_cache.store(get, 100, false);
    g_cache.markNoAnswer(0x1000, 200);
    const HexRegisterCache::Entry* e = g_cache.find(0x1000);
    TEST_ASSERT_EQUAL(HexRegisterCache::State::NoAnswer, e->state);
    TEST_ASSERT_EQUAL_UINT8(0, e->length);
}

void test_cache_capacity()
{
    Message get;
    get.code = static_cast<uint8_t>(Response::Get);
    get.length = 4;
    for (uint16_t i = 0; i < HexRegisterCache::Capacity + 5; ++i)
    {
        get.data[0] = static_cast<uint8_t>(i);
        get.data[1] = 0x20;
        g_cache.store(get, i, false);
    }
    TEST_ASSERT_EQUAL_UINT32(HexRegisterCache::Capacity, g_cache.size());
}

/******************************************************************************
 * Testlauf
 ******************************************************************************/
int runAllTests()
{
    UNITY_BEGIN();
    RUN_TEST(test_job_lists);
    RUN_TEST(test_all_registers_answered);
    RUN_TEST(test_unknown_register_and_empty_day);
    RUN_TEST(test_retry_after_timeout);
    RUN_TEST(test_single_register_without_answer_continues);
    RUN_TEST(test_device_never_answers_aborts_quickly);
    RUN_TEST(test_start_rejected_while_busy_or_invalid);
    RUN_TEST(test_wrong_register_answer_is_not_taken);
    RUN_TEST(test_async_updates_only_known_registers);
    RUN_TEST(test_no_answer_clears_old_value);
    RUN_TEST(test_cache_capacity);
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
