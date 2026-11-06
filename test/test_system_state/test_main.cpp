/******************************************************************************
 * @file test_main.cpp
 *
 * Unit-Tests für Stufe 4a: DeviceState, LinkMonitor, SystemEvaluator.
 *
 * Ausführen:
 *   pio test -e native     -> auf dem PC
 *   pio test -e esp32dev   -> auf dem ESP32
 ******************************************************************************/

#include <unity.h>

#include <cstring>
#include <vector>

#include "DeviceDetector.hpp"
#include "DeviceState.hpp"
#include "LinkMonitor.hpp"
#include "SystemState.hpp"

namespace
{

struct Field
{
    const char* label;
    const char* value;
};

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

/// Hauptblock des SmartShunts
VeDirectFrame shuntBlock(const char* soc = "1000", const char* alarm = "OFF", const char* ar = "0")
{
    return makeFrame({ { "PID", "0xC035" }, { "V", "12330" }, { "I", "-1500" }, { "P", "-18" },
                       { "CE", "-2500" }, { "SOC", soc }, { "TTG", "-1" },
                       { "Alarm", alarm }, { "AR", ar } });
}

/// Block des MPPT
VeDirectFrame mpptBlock(const char* err = "0")
{
    return makeFrame({ { "PID", "0xA057" }, { "V", "13500" }, { "VPV", "18600" }, { "PPV", "96" },
                       { "CS", "3" }, { "ERR", err }, { "H20", "12" } });
}

/// Kanal wie in der Firmware: Detector entscheidet, DeviceState speichert.
struct TestChannel
{
    DeviceDetector detector;
    DeviceState    state;
    LinkMonitor    link;

    void receive(const VeDirectFrame& frame, uint32_t now)
    {
        const DeviceDetector::Result r = detector.update(frame, now);
        if (r.event == DeviceDetector::Event::Changed)
        {
            state.clear();
        }
        state.setIdentity(detector.identity());
        if (r.origin != DeviceDetector::Origin::Unassigned)
        {
            state.apply(frame, now);
        }
    }

    void tick(uint32_t now)
    {
        if (detector.checkTimeout(now) == DeviceDetector::Event::Lost)
        {
            link.recordOutage(now);
            state.clear();
            state.setIdentity(detector.identity());
        }
        link.update(now, detector.identity().state == DeviceDetector::State::Identified);
    }

    void reset()
    {
        detector.reset();
        state.clear();
        link.reset();
    }
};

TestChannel g_ch[2];
SystemConfig g_config;

SystemState evaluate(uint32_t now)
{
    ChannelView views[2];
    for (int i = 0; i < 2; ++i)
    {
        views[i].state = &g_ch[i].state;
        views[i].link  = &g_ch[i].link;
    }
    return SystemEvaluator::evaluate(views, 2, g_config, now);
}

bool hasFault(const SystemState& s, uint32_t bit)
{
    return (s.faults & bit) != 0;
}

} // namespace

void setUp()
{
    g_ch[0].reset();
    g_ch[1].reset();
    g_config = SystemConfig();
}

void tearDown() {}

/******************************************************************************
 * DeviceState
 ******************************************************************************/

void test_device_state_stores_values_by_meaning()
{
    TestChannel& ch = g_ch[0];
    ch.receive(shuntBlock(), 1000);

    float v = 0.0f;
    TEST_ASSERT_TRUE(ch.state.freshValue(SemanticField::BatteryVoltage, 1000, 5000, v));
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 12.330f, v);
    TEST_ASSERT_TRUE(ch.state.freshValue(SemanticField::StateOfCharge, 1000, 5000, v));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, v);
    TEST_ASSERT_NULL(ch.state.entry(SemanticField::PvPower));   // nie empfangen
}

void test_device_state_freshness_and_overflow()
{
    TestChannel& ch = g_ch[0];
    ch.receive(shuntBlock(), 0xFFFFF000u);

    TEST_ASSERT_TRUE(ch.state.isFresh(SemanticField::BatteryVoltage, 0x00000100u, 5000));   // 4352 ms
    TEST_ASSERT_FALSE(ch.state.isFresh(SemanticField::BatteryVoltage, 0x00000400u, 5000));  // 5120 ms
    // Zeitstempel "aus der Zukunft" (now vor dem Kopieren gelesen) -> Alter 0
    TEST_ASSERT_EQUAL_UINT32(0, DeviceState::ageMs(1010, 1000));
}

void test_device_state_dashes_are_not_fresh()
{
    TestChannel& ch = g_ch[0];
    ch.receive(shuntBlock("---"), 1000);
    TEST_ASSERT_FALSE(ch.state.isFresh(SemanticField::StateOfCharge, 1000, 5000));
    TEST_ASSERT_NOT_NULL(ch.state.entry(SemanticField::StateOfCharge));   // empfangen, aber ungültig
}

/// Ausfall: alle Werte gelöscht ("--" statt alter Werte)
void test_device_state_cleared_on_loss()
{
    TestChannel& ch = g_ch[0];
    ch.receive(shuntBlock(), 1000);
    ch.tick(6001);   // Timeout
    TEST_ASSERT_FALSE(ch.state.isIdentified());
    TEST_ASSERT_NULL(ch.state.entry(SemanticField::BatteryVoltage));
}

/// Gerätewechsel: Werte des alten Geräts verschwinden
void test_device_state_cleared_on_change()
{
    TestChannel& ch = g_ch[0];
    ch.receive(shuntBlock(), 1000);
    ch.receive(mpptBlock(), 1500);
    TEST_ASSERT_NULL(ch.state.entry(SemanticField::StateOfCharge));
    TEST_ASSERT_NOT_NULL(ch.state.entry(SemanticField::PvPower));
}

/// Nicht zuordenbare Blöcke werden nicht übernommen
void test_device_state_ignores_unassigned_blocks()
{
    TestChannel& ch = g_ch[0];
    ch.receive(makeFrame({ { "H7", "8235" } }), 1000);   // H-Block vor PID
    TEST_ASSERT_NULL(ch.state.entry(SemanticField::MinimumBatteryVoltage));
}

/******************************************************************************
 * LinkMonitor
 ******************************************************************************/

void test_link_single_outage_is_unstable()
{
    LinkMonitor link;
    link.recordOutage(1000);
    TEST_ASSERT_TRUE(link.isUnstable());
    TEST_ASSERT_EQUAL_UINT32(1, link.total().outages);
}

void test_link_three_data_errors_are_unstable()
{
    LinkMonitor link;
    link.recordChecksumError(1000);
    link.recordDiscardedFrame(2000);
    TEST_ASSERT_FALSE(link.isUnstable());
    link.recordChecksumError(500000);   // kein Zeitfenster: auch viel später zählt
    TEST_ASSERT_TRUE(link.isUnstable());
}

void test_link_recovers_after_two_minutes_while_connected()
{
    LinkMonitor link;
    link.recordOutage(1000);
    link.update(120999, true);
    TEST_ASSERT_TRUE(link.isUnstable());
    link.update(121000, true);   // 2 min fehlerfrei
    TEST_ASSERT_FALSE(link.isUnstable());
    TEST_ASSERT_EQUAL_UINT32(0, link.current().outages);
    TEST_ASSERT_EQUAL_UINT32(1, link.total().outages);   // Gesamtzähler bleibt
    TEST_ASSERT_EQUAL_UINT32(1, link.unstableCount());
}

/// Kabel ab und nie wieder angesteckt: keine Erholung
void test_link_no_recovery_while_disconnected()
{
    LinkMonitor link;
    link.recordOutage(1000);
    link.update(600000, false);   // 10 min getrennt
    TEST_ASSERT_TRUE(link.isUnstable());
    link.update(700000, false);   // letzter Aufruf noch getrennt
    link.update(700001, true);    // wieder verbunden: Erholzeit läuft ab 700000
    TEST_ASSERT_TRUE(link.isUnstable());
    link.update(819999, true);
    TEST_ASSERT_TRUE(link.isUnstable());
    link.update(820000, true);    // 2 min nach dem Wiederverbinden
    TEST_ASSERT_FALSE(link.isUnstable());
}

/// Einzelne Datenfehler "heilen" ebenfalls nach der Erholzeit
void test_link_sporadic_errors_heal()
{
    LinkMonitor link;
    link.recordChecksumError(1000);
    link.recordChecksumError(2000);
    link.update(122000, true);                // geheilt
    link.recordChecksumError(130000);         // nur 1 seit Erholung
    TEST_ASSERT_FALSE(link.isUnstable());
    TEST_ASSERT_EQUAL_UINT32(3, link.total().checksumErrors);
}

void test_link_recovery_disabled()
{
    LinkMonitor link;
    LinkMonitor::Config config;
    config.recoveryMs = 0;
    link.setConfig(config);
    link.recordOutage(1000);
    link.update(100000000, true);
    TEST_ASSERT_TRUE(link.isUnstable());
}

/******************************************************************************
 * SystemEvaluator
 ******************************************************************************/

/// Normalfall: Shunt an UART2, MPPT an UART1 (Rollen unabhängig vom UART)
void test_system_normal_operation_swapped_uarts()
{
    g_ch[0].receive(mpptBlock(), 1000);
    g_ch[1].receive(shuntBlock(), 1000);

    const SystemState s = evaluate(1500);
    TEST_ASSERT_EQUAL(1, s.batteryChannel);
    TEST_ASSERT_EQUAL(0, s.pvChannel);
    TEST_ASSERT_EQUAL_UINT32(0, s.faults);
    TEST_ASSERT_TRUE(s.severity == Severity::None);

    TEST_ASSERT_TRUE(s.batteryVoltage.valid);
    TEST_ASSERT_TRUE(s.batteryVoltage.source == ValueSource::BatteryMonitor);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 12.330f, s.batteryVoltage.value);   // vom Shunt, nicht 13,5 V
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, -1.5f, s.batteryCurrent.value);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, s.stateOfCharge.value);
    TEST_ASSERT_TRUE(s.timeToGoInfinite);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 96.0f, s.pvPower.value);
    TEST_ASSERT_EQUAL(3, s.chargerState);
    TEST_ASSERT_EQUAL_STRING("Bulk (Konstantstrom)", s.chargerStateText);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.12f, s.yieldToday.value);
}

/// Shunt fehlt: kritisch, Spannung ersatzweise vom MPPT (gekennzeichnet)
void test_system_shunt_missing_uses_mppt_voltage()
{
    g_ch[0].receive(mpptBlock(), 1000);

    const SystemState s = evaluate(1500);
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::BatteryMonitorMissing));
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::SocNotAvailable));
    TEST_ASSERT_TRUE(s.severity == Severity::Critical);
    TEST_ASSERT_EQUAL(-1, s.batteryChannel);

    TEST_ASSERT_TRUE(s.batteryVoltage.valid);
    TEST_ASSERT_TRUE(s.batteryVoltage.source == ValueSource::PvCharger);
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 13.5f, s.batteryVoltage.value);
    TEST_ASSERT_FALSE(s.stateOfCharge.valid);
    TEST_ASSERT_FALSE(s.batteryCurrent.valid);   // Strom NICHT vom MPPT
}

/// Shunt fällt aus: nach Timeout kritisch, Werte "--"
void test_system_shunt_lost()
{
    g_ch[1].receive(shuntBlock(), 1000);
    TEST_ASSERT_EQUAL_UINT32(0, evaluate(1500).faults & SystemFault::BatteryMonitorMissing);

    g_ch[1].tick(6001);
    const SystemState s = evaluate(6001);
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::BatteryMonitorMissing));
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::BatteryLinkUnstable) == false);   // kein Shunt-Kanal mehr
    TEST_ASSERT_FALSE(s.batteryVoltage.valid);
}

/// Zwei Shunts: kritisch, keine Batteriewerte
void test_system_two_battery_monitors()
{
    g_ch[0].receive(shuntBlock(), 1000);
    g_ch[1].receive(shuntBlock(), 1000);

    const SystemState s = evaluate(1500);
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::MultipleBatteryMonitors));
    TEST_ASSERT_TRUE(s.severity == Severity::Critical);
    TEST_ASSERT_EQUAL(-1, s.batteryChannel);
    TEST_ASSERT_FALSE(s.stateOfCharge.valid);
}

/// Shunt nicht synchronisiert (SOC "---")
void test_system_soc_not_available()
{
    g_ch[0].receive(shuntBlock("---"), 1000);
    const SystemState s = evaluate(1500);
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::SocNotAvailable));
    TEST_ASSERT_TRUE(s.severity == Severity::Critical);
    TEST_ASSERT_TRUE(s.batteryVoltage.valid);   // Spannung trotzdem da
}

void test_system_battery_alarm()
{
    g_ch[0].receive(shuntBlock("500", "ON", "0"), 1000);
    TEST_ASSERT_TRUE(hasFault(evaluate(1500), SystemFault::BatteryAlarm));

    g_ch[0].receive(shuntBlock("500", "OFF", "4"), 2000);   // AR = Niedriger SOC
    TEST_ASSERT_TRUE(hasFault(evaluate(2500), SystemFault::BatteryAlarm));
}

/// MPPT optional: fehlt -> nur Info; als erforderlich eingestellt -> Warnung
void test_system_mppt_missing_info_or_warning()
{
    g_ch[0].receive(shuntBlock(), 1000);

    SystemState s = evaluate(1500);
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::PvChargerMissing));
    TEST_ASSERT_TRUE(s.severity == Severity::Info);

    g_config.requirePvCharger = true;
    s = evaluate(1500);
    TEST_ASSERT_TRUE(s.severity == Severity::Warning);
}

void test_system_mppt_error()
{
    g_ch[0].receive(shuntBlock(), 1000);
    g_ch[1].receive(mpptBlock("33"), 1000);
    const SystemState s = evaluate(1500);
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::PvChargerError));
    TEST_ASSERT_EQUAL(33, s.pvErrorCode);
    TEST_ASSERT_TRUE(s.severity == Severity::Warning);
}

void test_system_link_unstable()
{
    g_ch[0].receive(shuntBlock(), 1000);
    g_ch[0].link.recordChecksumError(1000);
    g_ch[0].link.recordChecksumError(1100);
    g_ch[0].link.recordDiscardedFrame(1200);
    const SystemState s = evaluate(1500);
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::BatteryLinkUnstable));
}

void test_system_unknown_device()
{
    g_ch[0].receive(shuntBlock(), 1000);
    g_ch[1].receive(makeFrame({ { "PID", "0xFFFF" }, { "V", "1" } }), 1000);
    const SystemState s = evaluate(1500);
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::UnknownDevice));
    TEST_ASSERT_EQUAL(0, s.batteryChannel);
}

/// Veraltete Werte (Block kam, aber zu lange her) gelten als ungültig
void test_system_stale_values_invalid()
{
    g_ch[0].receive(shuntBlock(), 1000);
    const SystemState s = evaluate(7000);   // 6 s alt, Detector noch nicht abgelaufen geprüft
    TEST_ASSERT_FALSE(s.stateOfCharge.valid);
    TEST_ASSERT_TRUE(hasFault(s, SystemFault::SocNotAvailable));
}

void test_fault_texts_and_severities_defined()
{
    for (unsigned bit = 0; bit < SystemFault::BitCount; ++bit)
    {
        const uint32_t mask = 1u << bit;
        TEST_ASSERT_TRUE(std::strcmp(SystemEvaluator::faultText(mask), "?") != 0);
        TEST_ASSERT_TRUE(SystemEvaluator::severityOf(mask, g_config) != Severity::None);
    }
}

/******************************************************************************
 * Testlauf
 ******************************************************************************/
int runAllTests()
{
    UNITY_BEGIN();
    RUN_TEST(test_device_state_stores_values_by_meaning);
    RUN_TEST(test_device_state_freshness_and_overflow);
    RUN_TEST(test_device_state_dashes_are_not_fresh);
    RUN_TEST(test_device_state_cleared_on_loss);
    RUN_TEST(test_device_state_cleared_on_change);
    RUN_TEST(test_device_state_ignores_unassigned_blocks);
    RUN_TEST(test_link_single_outage_is_unstable);
    RUN_TEST(test_link_three_data_errors_are_unstable);
    RUN_TEST(test_link_recovers_after_two_minutes_while_connected);
    RUN_TEST(test_link_no_recovery_while_disconnected);
    RUN_TEST(test_link_sporadic_errors_heal);
    RUN_TEST(test_link_recovery_disabled);
    RUN_TEST(test_system_normal_operation_swapped_uarts);
    RUN_TEST(test_system_shunt_missing_uses_mppt_voltage);
    RUN_TEST(test_system_shunt_lost);
    RUN_TEST(test_system_two_battery_monitors);
    RUN_TEST(test_system_soc_not_available);
    RUN_TEST(test_system_battery_alarm);
    RUN_TEST(test_system_mppt_missing_info_or_warning);
    RUN_TEST(test_system_mppt_error);
    RUN_TEST(test_system_link_unstable);
    RUN_TEST(test_system_unknown_device);
    RUN_TEST(test_system_stale_values_invalid);
    RUN_TEST(test_fault_texts_and_severities_defined);
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
