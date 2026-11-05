/******************************************************************************
 * @file test_main.cpp
 *
 * Unit-Tests für DeviceDetector (Unity).
 *
 * Ausführen:
 *   pio test -e native     -> auf dem PC
 *   pio test -e esp32dev   -> auf dem ESP32
 *
 * Zeiten werden als feste millis()-Werte vorgegeben; es wird nicht
 * gewartet. Standard-Timeout: 5000 ms.
 ******************************************************************************/

#include <unity.h>

#include <cstring>
#include <vector>

#include "DeviceDetector.hpp"

using Detector = DeviceDetector;
using Event    = DeviceDetector::Event;
using Origin   = DeviceDetector::Origin;
using State    = DeviceDetector::State;

namespace
{

struct Field
{
    const char* label;
    const char* value;
};

/// Gültigen Frame bauen (wie ihn der Parser liefern würde).
VeDirectFrame makeFrame(const std::vector<Field>& fields)
{
    VeDirectFrame frame;
    for (const Field& f : fields)
    {
        VeDirectField& target = frame.fields[frame.fieldCount++];
        std::strncpy(target.label.data(), f.label, VeDirectField::LabelSize - 1);
        std::strncpy(target.value.data(), f.value, VeDirectField::ValueSize - 1);
    }
    frame.checksumValid = true;
    return frame;
}

/// Typische Blöcke der Projektgeräte
const VeDirectFrame& shuntMainBlock()
{
    static const VeDirectFrame frame = makeFrame({ { "PID", "0xC035" }, { "V", "12330" }, { "SOC", "1000" } });
    return frame;
}

const VeDirectFrame& shuntHistoryBlock()
{
    static const VeDirectFrame frame = makeFrame({ { "H1", "0" }, { "H2", "0" }, { "H7", "8235" } });
    return frame;
}

const VeDirectFrame& mpptBlock()
{
    static const VeDirectFrame frame = makeFrame({ { "PID", "0xA057" }, { "V", "13400" }, { "PPV", "96" } });
    return frame;
}

Detector g_detector;

} // namespace

void setUp()
{
    g_detector.reset();
    g_detector.setTimeoutMs(Detector::DefaultTimeoutMs);
}

void tearDown() {}

/******************************************************************************
 * Tests
 ******************************************************************************/

void test_initially_unknown()
{
    TEST_ASSERT_TRUE(g_detector.identity().state == State::Unknown);
    TEST_ASSERT_EQUAL_UINT32(5000, g_detector.timeoutMs());
    TEST_ASSERT_TRUE(g_detector.checkTimeout(100000) == Event::None);   // ohne Gerät kein Ausfall
}

/// H-Block vor der ersten PID: nicht zuordenbar.
void test_block_without_pid_before_identification_is_unassigned()
{
    const Detector::Result r = g_detector.update(shuntHistoryBlock(), 1000);
    TEST_ASSERT_TRUE(r.origin == Origin::Unassigned);
    TEST_ASSERT_TRUE(r.event == Event::None);
    TEST_ASSERT_EQUAL_UINT32(1, g_detector.statistics().unassignedFrames);
}

/// SmartShunt: PID-Block erkennt, H-Block wird geerbt.
void test_shunt_identified_and_history_block_inherited()
{
    Detector::Result r = g_detector.update(shuntMainBlock(), 1000);
    TEST_ASSERT_TRUE(r.event == Event::Identified);
    TEST_ASSERT_TRUE(r.origin == Origin::Own);
    TEST_ASSERT_TRUE(g_detector.identity().state == State::Identified);
    TEST_ASSERT_EQUAL_STRING("0xC035", g_detector.identity().productId);
    TEST_ASSERT_NOT_NULL(g_detector.identity().device);
    TEST_ASSERT_EQUAL_STRING("SmartShunt 500A/50mV", g_detector.identity().device->name);

    r = g_detector.update(shuntHistoryBlock(), 1500);
    TEST_ASSERT_TRUE(r.origin == Origin::Inherited);
    TEST_ASSERT_TRUE(r.event == Event::None);

    // Gleiche PID erneut: kein neues Ereignis
    r = g_detector.update(shuntMainBlock(), 2000);
    TEST_ASSERT_TRUE(r.event == Event::None);
    TEST_ASSERT_EQUAL_UINT32(1, g_detector.statistics().identifiedCount);
    TEST_ASSERT_EQUAL_UINT32(2000, g_detector.identity().lastPidMs);
}

/// MPPT: Blöcke ohne PID werden NICHT zugeordnet (kein Batteriemonitor).
void test_block_without_pid_not_inherited_by_mppt()
{
    g_detector.update(mpptBlock(), 1000);
    TEST_ASSERT_TRUE(g_detector.identity().device->category == DeviceCategory::Mppt);

    const Detector::Result r = g_detector.update(shuntHistoryBlock(), 1200);
    TEST_ASSERT_TRUE(r.origin == Origin::Unassigned);
    TEST_ASSERT_EQUAL_UINT32(1, g_detector.statistics().unassignedFrames);
}

/// Unbekannte PID: erkannt als "unbekanntes Gerät", H-Block nicht zuordenbar.
void test_unknown_pid_is_identified_as_unknown_device()
{
    const Detector::Result r = g_detector.update(makeFrame({ { "PID", "0xFFFF" }, { "V", "1" } }), 1000);
    TEST_ASSERT_TRUE(r.event == Event::Identified);
    TEST_ASSERT_TRUE(g_detector.identity().state == State::Identified);
    TEST_ASSERT_NULL(g_detector.identity().device);
    TEST_ASSERT_EQUAL_STRING("0xFFFF", g_detector.identity().productId);

    TEST_ASSERT_TRUE(g_detector.update(shuntHistoryBlock(), 1100).origin == Origin::Unassigned);
}

/// Timeout: genau einmal "Lost", Grenze exakt bei timeoutMs.
void test_timeout_reports_lost_once()
{
    g_detector.update(shuntMainBlock(), 1000);

    TEST_ASSERT_TRUE(g_detector.checkTimeout(6000) == Event::None);   // genau 5000 ms: noch ok
    TEST_ASSERT_TRUE(g_detector.checkTimeout(6001) == Event::Lost);
    TEST_ASSERT_TRUE(g_detector.identity().state == State::Unknown);
    TEST_ASSERT_EQUAL_STRING("0xC035", g_detector.previousIdentity().productId);
    TEST_ASSERT_EQUAL_UINT32(1, g_detector.statistics().lostCount);

    TEST_ASSERT_TRUE(g_detector.checkTimeout(7000) == Event::None);   // nicht wiederholt
    TEST_ASSERT_EQUAL_UINT32(1, g_detector.statistics().lostCount);
}

/// Nur Blöcke MIT PID halten die Erkennung aufrecht.
void test_history_blocks_alone_do_not_keep_device_alive()
{
    g_detector.update(shuntMainBlock(), 1000);
    g_detector.update(shuntHistoryBlock(), 3000);
    g_detector.update(shuntHistoryBlock(), 5500);
    TEST_ASSERT_TRUE(g_detector.checkTimeout(6001) == Event::Lost);
}

/// Wiedererkennung nach Ausfall mit Dauer des Ausfalls (Wackelkontakt).
void test_reidentification_after_loss_measures_outage()
{
    g_detector.update(shuntMainBlock(), 1000);
    g_detector.checkTimeout(6500);                       // Ausfall

    const Detector::Result r = g_detector.update(shuntMainBlock(), 13000);
    TEST_ASSERT_TRUE(r.event == Event::Identified);
    TEST_ASSERT_EQUAL_UINT32(12000, g_detector.statistics().lastOutageMs);   // 1000 -> 13000
    TEST_ASSERT_EQUAL_UINT32(2, g_detector.statistics().identifiedCount);
    TEST_ASSERT_EQUAL_UINT32(1, g_detector.statistics().lostCount);
}

/// Andere PID: sofortiger Gerätewechsel.
void test_pid_change_reports_changed()
{
    g_detector.update(shuntMainBlock(), 1000);

    const Detector::Result r = g_detector.update(mpptBlock(), 1500);
    TEST_ASSERT_TRUE(r.event == Event::Changed);
    TEST_ASSERT_EQUAL_STRING("0xA057", g_detector.identity().productId);
    TEST_ASSERT_EQUAL_STRING("0xC035", g_detector.previousIdentity().productId);
    TEST_ASSERT_EQUAL_UINT32(1, g_detector.statistics().changedCount);
}

/// PID-Vergleich ohne Groß-/Kleinschreibung: kein falscher Gerätewechsel.
void test_pid_case_insensitive()
{
    g_detector.update(mpptBlock(), 1000);
    const Detector::Result r = g_detector.update(makeFrame({ { "PID", "0xa057" } }), 1500);
    TEST_ASSERT_TRUE(r.event == Event::None);
    TEST_ASSERT_EQUAL_UINT32(0, g_detector.statistics().changedCount);
}

/// Überlauf von millis() nach ca. 49 Tagen.
void test_millis_overflow()
{
    g_detector.update(shuntMainBlock(), 0xFFFFF000u);
    TEST_ASSERT_TRUE(g_detector.checkTimeout(0x00000100u) == Event::None);   // 4352 ms später
    TEST_ASSERT_TRUE(g_detector.update(shuntHistoryBlock(), 0x00000200u).origin == Origin::Inherited);
    TEST_ASSERT_TRUE(g_detector.checkTimeout(0x00000400u) == Event::Lost);   // 5120 ms später
}

/// Timeout ist zur Laufzeit änderbar (später über WebUI).
void test_timeout_configurable()
{
    g_detector.setTimeoutMs(10000);
    g_detector.update(shuntMainBlock(), 1000);
    TEST_ASSERT_TRUE(g_detector.checkTimeout(8000) == Event::None);
    TEST_ASSERT_TRUE(g_detector.checkTimeout(11001) == Event::Lost);
}

/// Sicherheitsnetz: abgelaufene Erkennung erbt keine Blöcke, auch wenn
/// checkTimeout() vergessen wurde.
void test_expired_identity_does_not_inherit()
{
    g_detector.update(shuntMainBlock(), 1000);
    TEST_ASSERT_TRUE(g_detector.update(shuntHistoryBlock(), 7000).origin == Origin::Unassigned);
}

/// reset(): alles zurück, Timeout bleibt.
void test_reset()
{
    g_detector.setTimeoutMs(8000);
    g_detector.update(shuntMainBlock(), 1000);
    g_detector.reset();
    TEST_ASSERT_TRUE(g_detector.identity().state == State::Unknown);
    TEST_ASSERT_EQUAL_UINT32(0, g_detector.statistics().identifiedCount);
    TEST_ASSERT_EQUAL_UINT32(8000, g_detector.timeoutMs());
}

/******************************************************************************
 * Testlauf
 ******************************************************************************/
int runAllTests()
{
    UNITY_BEGIN();
    RUN_TEST(test_initially_unknown);
    RUN_TEST(test_block_without_pid_before_identification_is_unassigned);
    RUN_TEST(test_shunt_identified_and_history_block_inherited);
    RUN_TEST(test_block_without_pid_not_inherited_by_mppt);
    RUN_TEST(test_unknown_pid_is_identified_as_unknown_device);
    RUN_TEST(test_timeout_reports_lost_once);
    RUN_TEST(test_history_blocks_alone_do_not_keep_device_alive);
    RUN_TEST(test_reidentification_after_loss_measures_outage);
    RUN_TEST(test_pid_change_reports_changed);
    RUN_TEST(test_pid_case_insensitive);
    RUN_TEST(test_millis_overflow);
    RUN_TEST(test_timeout_configurable);
    RUN_TEST(test_expired_identity_does_not_inherit);
    RUN_TEST(test_reset);
    return UNITY_END();
}

#ifdef ARDUINO
#include <Arduino.h>

void setup()
{
    delay(2000);   // Zeit für den Testmonitor, sich zu verbinden
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
