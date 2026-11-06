/******************************************************************************
 * @file test_main.cpp
 *
 * Unit-Tests für LoadController (Stufe 4b, Abschaltlogik).
 *
 * Ausführen:
 *   pio test -e native     -> auf dem PC
 *   pio test -e esp32dev   -> auf dem ESP32
 *
 * Schwellen Camper (Projektinhaber 06.11.2026):
 *   R1 Wechselrichter: gesperrt < 25 %, aus < 15 %, frei ab 30 %
 *   R2 Licht:          aus < 15 %, frei ab 20 %
 *   R3 Kühlschrank:    aus <  5 %, frei ab 10 %
 ******************************************************************************/

#include <unity.h>

#include "LoadController.hpp"

namespace
{

/// Systemzustand mit gültigem SOC
SystemState withSoc(float soc)
{
    SystemState s;
    s.stateOfCharge.valid  = true;
    s.stateOfCharge.value  = soc;
    s.stateOfCharge.source = ValueSource::BatteryMonitor;
    s.batteryChannel       = 0;
    return s;
}

/// Systemzustand ohne Shunt (Spannung nur vom MPPT)
SystemState withoutShunt()
{
    SystemState s;
    s.faults = SystemFault::BatteryMonitorMissing | SystemFault::SocNotAvailable;
    s.batteryVoltage.valid  = true;
    s.batteryVoltage.value  = 13.3f;
    s.batteryVoltage.source = ValueSource::PvCharger;
    return s;
}

LoadController g_ctl;

bool isOn(RelayId id)     { return g_ctl.status(id).on; }
bool isLocked(RelayId id) { return g_ctl.status(id).locked; }

const CommandSource Local = CommandSource::Local;
const CommandSource Mqtt  = CommandSource::Mqtt;

} // namespace

void setUp()
{
    g_ctl.setConfig(LoadControllerConfig::camper());
    g_ctl.start(0);
}

void tearDown() {}

/******************************************************************************
 * Start
 ******************************************************************************/

/// Nach Neustart: Wechselrichter aus, Licht und Kühlschrank an.
void test_startup_state()
{
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
    TEST_ASSERT_TRUE(isOn(RelayId::Light));
    TEST_ASSERT_TRUE(isOn(RelayId::Fridge));
    TEST_ASSERT_FALSE(g_ctl.overrideActive());
}

/// Auch ohne Shunt nach dem Start: Kleinverbraucher bleiben an.
void test_startup_without_shunt_keeps_small_loads()
{
    g_ctl.update(withoutShunt(), 1000);
    TEST_ASSERT_TRUE(isOn(RelayId::Light));
    TEST_ASSERT_TRUE(isOn(RelayId::Fridge));
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
}

/******************************************************************************
 * R1 Wechselrichter
 ******************************************************************************/

void test_inverter_switch_on_and_off_by_command()
{
    const SystemState s = withSoc(80.0f);
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Local, s, 1000) == CommandResult::Accepted);
    TEST_ASSERT_TRUE(isOn(RelayId::Main));
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, false, Local, s, 2000) == CommandResult::Accepted);
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
}

/// Camper: MQTT darf den Wechselrichter nicht schalten.
void test_inverter_source_not_allowed()
{
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Mqtt, withSoc(80.0f), 1000) == CommandResult::RejectedSource);
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
}

/// Unter 25 %: Einschalten gesperrt.
void test_inverter_locked_below_25()
{
    g_ctl.update(withSoc(24.0f), 1000);
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Local, withSoc(24.0f), 1000) == CommandResult::RejectedLocked);
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
}

/// Laufender Wechselrichter läuft unter 25 % weiter, aus erst unter 15 %.
void test_running_inverter_keeps_running_until_15()
{
    g_ctl.request(RelayId::Main, true, Local, withSoc(50.0f), 1000);

    g_ctl.update(withSoc(20.0f), 2000);
    TEST_ASSERT_TRUE(isOn(RelayId::Main));
    TEST_ASSERT_TRUE(isLocked(RelayId::Main));   // aber wieder einschalten wäre gesperrt

    g_ctl.update(withSoc(14.9f), 3000);
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
    TEST_ASSERT_EQUAL_STRING("SOC unter Abschaltschwelle", g_ctl.status(RelayId::Main).reason);
}

/// Hysterese: zwischen 25 und 30 % bleibt die Sperre bestehen, ab 30 % frei.
void test_inverter_hysteresis()
{
    g_ctl.update(withSoc(20.0f), 1000);
    TEST_ASSERT_TRUE(isLocked(RelayId::Main));

    g_ctl.update(withSoc(27.0f), 2000);
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Local, withSoc(27.0f), 2000) == CommandResult::RejectedLocked);

    g_ctl.update(withSoc(30.0f), 3000);
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Local, withSoc(30.0f), 3000) == CommandResult::Accepted);
}

/// Camper: Nach Schutzabschaltung schaltet der Wechselrichter NICHT
/// selbst wieder ein, sondern braucht einen neuen Tastendruck.
void test_inverter_no_auto_restart_in_camper()
{
    g_ctl.request(RelayId::Main, true, Local, withSoc(50.0f), 1000);
    g_ctl.update(withSoc(10.0f), 2000);
    g_ctl.update(withSoc(90.0f), 100000);
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
}

/// Shunt fehlt (Spannung nur vom MPPT): Wechselrichter aus und gesperrt.
void test_inverter_off_without_shunt()
{
    g_ctl.request(RelayId::Main, true, Local, withSoc(50.0f), 1000);
    g_ctl.update(withoutShunt(), 2000);
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
    TEST_ASSERT_EQUAL_STRING("kein gültiger SOC", g_ctl.status(RelayId::Main).reason);
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Local, withoutShunt(), 3000) == CommandResult::RejectedLocked);

    // Kleinverbraucher laufen weiter
    TEST_ASSERT_TRUE(isOn(RelayId::Light));
    TEST_ASSERT_TRUE(isOn(RelayId::Fridge));
}

/// Ohne vorheriges update(): Befehl darf nicht vor gültigem SOC einschalten.
void test_inverter_command_before_first_update_without_soc()
{
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Local, SystemState(), 100) == CommandResult::RejectedLocked);
}

/******************************************************************************
 * R2 Licht, R3 Kühlschrank (automatisch)
 ******************************************************************************/

void test_light_off_below_15_on_from_20_after_min_off_time()
{
    g_ctl.update(withSoc(14.9f), 1000);
    TEST_ASSERT_FALSE(isOn(RelayId::Light));
    TEST_ASSERT_TRUE(isOn(RelayId::Fridge));

    g_ctl.update(withSoc(19.9f), 100000);
    TEST_ASSERT_FALSE(isOn(RelayId::Light));   // Hysterese

    g_ctl.update(withSoc(20.0f), 30000 + 1000);   // frei, aber erst 30 s aus
    TEST_ASSERT_FALSE(isOn(RelayId::Light));

    g_ctl.update(withSoc(20.0f), 61000);           // 60 s aus -> wieder ein
    TEST_ASSERT_TRUE(isOn(RelayId::Light));
    TEST_ASSERT_EQUAL_STRING("SOC wieder ausreichend", g_ctl.status(RelayId::Light).reason);
}

void test_fridge_off_below_5_on_from_10()
{
    g_ctl.update(withSoc(4.9f), 1000);
    TEST_ASSERT_FALSE(isOn(RelayId::Fridge));
    TEST_ASSERT_FALSE(isOn(RelayId::Light));

    g_ctl.update(withSoc(9.0f), 120000);
    TEST_ASSERT_FALSE(isOn(RelayId::Fridge));
    g_ctl.update(withSoc(10.0f), 120000);
    TEST_ASSERT_TRUE(isOn(RelayId::Fridge));
    TEST_ASSERT_FALSE(isOn(RelayId::Light));   // Licht erst ab 20 %
}

/// Wurde das Licht wegen niedrigem SOC ausgeschaltet und fällt danach der
/// Shunt aus, bleibt es aus (keine Freigabe ohne gültigen SOC).
void test_light_stays_off_when_shunt_lost_afterwards()
{
    g_ctl.update(withSoc(10.0f), 1000);
    g_ctl.update(withoutShunt(), 200000);
    TEST_ASSERT_FALSE(isOn(RelayId::Light));
}

/// Automatische Relais sind nicht per Befehl schaltbar.
void test_automatic_relay_rejects_commands()
{
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Light, false, Local, withSoc(50.0f), 1000) == CommandResult::RejectedNotManual);
}

/// millis()-Überlauf bei der Mindest-Auszeit
void test_min_off_time_overflow()
{
    g_ctl.update(withSoc(10.0f), 0xFFFFF000u);
    g_ctl.update(withSoc(50.0f), 0x00000100u);    // 4352 ms später
    TEST_ASSERT_FALSE(isOn(RelayId::Light));
    g_ctl.update(withSoc(50.0f), 0x0000E000u);    // > 60 s später
    TEST_ASSERT_TRUE(isOn(RelayId::Light));
}

/******************************************************************************
 * Override
 ******************************************************************************/

void test_override_bypasses_all_locks()
{
    g_ctl.update(withoutShunt(), 1000);
    g_ctl.setOverride(true, withoutShunt(), 1000);
    TEST_ASSERT_TRUE(g_ctl.overrideActive());

    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Local, withoutShunt(), 1500) == CommandResult::Accepted);
    TEST_ASSERT_TRUE(isOn(RelayId::Main));

    // auch bei sehr niedrigem SOC bleibt alles an
    g_ctl.update(withSoc(1.0f), 2000);
    TEST_ASSERT_TRUE(isOn(RelayId::Main));
    TEST_ASSERT_TRUE(isOn(RelayId::Light));
    TEST_ASSERT_TRUE(isOn(RelayId::Fridge));
}

void test_override_end_applies_rules_immediately()
{
    g_ctl.setOverride(true, withSoc(3.0f), 1000);
    g_ctl.request(RelayId::Main, true, Local, withSoc(3.0f), 1000);

    g_ctl.setOverride(false, withSoc(3.0f), 2000);
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
    TEST_ASSERT_FALSE(isOn(RelayId::Light));
    TEST_ASSERT_FALSE(isOn(RelayId::Fridge));
    TEST_ASSERT_EQUAL_STRING("Override beendet", g_ctl.status(RelayId::Fridge).reason);
}

/// Neustart beendet den Override (Normalbetrieb).
void test_restart_ends_override()
{
    g_ctl.setOverride(true, withSoc(50.0f), 1000);
    g_ctl.start(5000);
    TEST_ASSERT_FALSE(g_ctl.overrideActive());
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
}

/******************************************************************************
 * Balkonkraftwerk
 ******************************************************************************/

void test_balcony_profile()
{
    g_ctl.setConfig(LoadControllerConfig::balcony());
    g_ctl.start(0);

    TEST_ASSERT_FALSE(isOn(RelayId::Light));    // R2/R3 deaktiviert
    TEST_ASSERT_FALSE(isOn(RelayId::Fridge));
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Local, withSoc(80.0f), 100) == CommandResult::RejectedSource);
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Light, true, Mqtt, withSoc(80.0f), 100) == CommandResult::RejectedDisabled);

    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Mqtt, withSoc(80.0f), 1000) == CommandResult::Accepted);
    TEST_ASSERT_TRUE(isOn(RelayId::Main));
}

/// BKW: Wunsch bleibt bestehen -> nach Schutzabschaltung automatisch wieder an.
void test_balcony_remembers_request()
{
    g_ctl.setConfig(LoadControllerConfig::balcony());
    g_ctl.start(0);

    g_ctl.request(RelayId::Main, true, Mqtt, withSoc(80.0f), 1000);
    g_ctl.update(withSoc(14.0f), 2000);
    TEST_ASSERT_FALSE(isOn(RelayId::Main));

    g_ctl.update(withSoc(28.0f), 3000);         // noch gesperrt (Hysterese)
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
    g_ctl.update(withSoc(30.0f), 4000);
    TEST_ASSERT_TRUE(isOn(RelayId::Main));
    TEST_ASSERT_EQUAL_STRING("gespeicherter Wunsch ausgeführt", g_ctl.status(RelayId::Main).reason);
}

/// BKW: Einschaltbefehl bei zu niedrigem SOC wird gespeichert.
void test_balcony_deferred_request()
{
    g_ctl.setConfig(LoadControllerConfig::balcony());
    g_ctl.start(0);

    g_ctl.update(withSoc(20.0f), 1000);
    TEST_ASSERT_TRUE(g_ctl.request(RelayId::Main, true, Mqtt, withSoc(20.0f), 1000) == CommandResult::Deferred);
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
    g_ctl.update(withSoc(31.0f), 2000);
    TEST_ASSERT_TRUE(isOn(RelayId::Main));

    // MQTT-Befehl "aus" löscht den Wunsch
    g_ctl.request(RelayId::Main, false, Mqtt, withSoc(31.0f), 3000);
    g_ctl.update(withSoc(31.0f), 4000);
    TEST_ASSERT_FALSE(isOn(RelayId::Main));
}

void test_result_texts_defined()
{
    TEST_ASSERT_EQUAL_STRING("ausgeführt", LoadController::resultText(CommandResult::Accepted));
    TEST_ASSERT_TRUE(LoadController::resultText(CommandResult::RejectedDisabled)[0] != '?');
}

/******************************************************************************
 * Testlauf
 ******************************************************************************/
int runAllTests()
{
    UNITY_BEGIN();
    RUN_TEST(test_startup_state);
    RUN_TEST(test_startup_without_shunt_keeps_small_loads);
    RUN_TEST(test_inverter_switch_on_and_off_by_command);
    RUN_TEST(test_inverter_source_not_allowed);
    RUN_TEST(test_inverter_locked_below_25);
    RUN_TEST(test_running_inverter_keeps_running_until_15);
    RUN_TEST(test_inverter_hysteresis);
    RUN_TEST(test_inverter_no_auto_restart_in_camper);
    RUN_TEST(test_inverter_off_without_shunt);
    RUN_TEST(test_inverter_command_before_first_update_without_soc);
    RUN_TEST(test_light_off_below_15_on_from_20_after_min_off_time);
    RUN_TEST(test_fridge_off_below_5_on_from_10);
    RUN_TEST(test_light_stays_off_when_shunt_lost_afterwards);
    RUN_TEST(test_automatic_relay_rejects_commands);
    RUN_TEST(test_min_off_time_overflow);
    RUN_TEST(test_override_bypasses_all_locks);
    RUN_TEST(test_override_end_applies_rules_immediately);
    RUN_TEST(test_restart_ends_override);
    RUN_TEST(test_balcony_profile);
    RUN_TEST(test_balcony_remembers_request);
    RUN_TEST(test_balcony_deferred_request);
    RUN_TEST(test_result_texts_defined);
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
