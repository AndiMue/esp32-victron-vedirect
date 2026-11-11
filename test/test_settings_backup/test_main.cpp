/******************************************************************************
 * @file test_main.cpp
 *
 * Unit-Tests für SettingsBackup (W5 Schritt 1): Sicherung schreiben,
 * einlesen, prüfen.
 *
 * Ausführen:
 *   pio test -e native     -> auf dem PC
 ******************************************************************************/

#include <unity.h>

#include <string>

#include "SettingsBackup.hpp"

namespace
{

BackupData sample()
{
    BackupData d;
    d.firmware = "26.11.11-1803";
    d.created  = "11.11.2026 18:30:00";
    d.balcony  = false;
    // Strategie C: immer alle wirksamen Werte; R1/R3 = Voreinstellung, R2 geändert
    const LoadControllerConfig preset = LoadControllerConfig::camper();
    for (std::size_t i = 0; i < RelayCount; ++i)
    {
        d.relays[i] = RelayLimits::from(preset.relays[i]);
    }
    d.relays[1].lockBelowSoc = 20.5f;
    d.relays[1].offBelowSoc  = 18.0f;
    d.relays[1].releaseAtSoc = 25.0f;
    d.relays[1].minOffTimeMs = 120000;
    d.relays[1].startupRequest  = true;
    d.relays[1].rememberRequest = true;
    d.tz  = "utc";
    d.ntp = "fritz.box";
    d.mqttEnabled  = true;
    d.mqttHost     = "192.168.178.10";
    d.mqttPort     = 1884;
    d.mqttUser     = "camper";
    d.mqttBase     = "vedirect";
    d.mqttInterval = 30;
    d.mqttClient   = "";
    d.wifiSsid     = "Mein \"WLAN\" äöü";   // Sonderzeichen, Umlaute
    d.apAlways     = true;
    d.hostname     = "ve-direct";
    return d;
}

/// Gültige Sicherung als Text mit ersetztem Ausschnitt.
std::string replaced(const std::string& from, const std::string& to)
{
    std::string s = backupToJson(sample());
    const std::size_t pos = s.find(from);
    TEST_ASSERT_TRUE_MESSAGE(pos != std::string::npos, from.c_str());
    s.replace(pos, from.size(), to);
    return s;
}

void expectRejected(const std::string& text, const char* contains)
{
    BackupData out;
    std::string error;
    TEST_ASSERT_FALSE(backupFromJson(text.c_str(), out, error));
    TEST_ASSERT_TRUE_MESSAGE(error.find(contains) != std::string::npos, error.c_str());
}

} // namespace

void setUp()    {}
void tearDown() {}

void test_roundtrip()
{
    const std::string text = backupToJson(sample());
    BackupData d;
    std::string error;
    TEST_ASSERT_TRUE_MESSAGE(backupFromJson(text.c_str(), d, error), error.c_str());

    TEST_ASSERT_FALSE(d.balcony);
    TEST_ASSERT_EQUAL_FLOAT(25.0f, d.relays[0].lockBelowSoc);   // Voreinstellung R1
    TEST_ASSERT_EQUAL_FLOAT(15.0f, d.relays[0].offBelowSoc);
    TEST_ASSERT_FALSE(d.relays[0].startupRequest);              // R1 nach Neustart aus
    TEST_ASSERT_TRUE(d.relays[2].startupRequest);               // R3 nach Neustart ein
    TEST_ASSERT_EQUAL_FLOAT(20.5f, d.relays[1].lockBelowSoc);
    TEST_ASSERT_EQUAL_FLOAT(18.0f, d.relays[1].offBelowSoc);
    TEST_ASSERT_EQUAL_FLOAT(25.0f, d.relays[1].releaseAtSoc);
    TEST_ASSERT_EQUAL_UINT32(120000, d.relays[1].minOffTimeMs);
    TEST_ASSERT_TRUE(d.relays[1].startupRequest);
    TEST_ASSERT_TRUE(d.relays[1].rememberRequest);
    TEST_ASSERT_EQUAL_STRING("utc", d.tz.c_str());
    TEST_ASSERT_EQUAL_STRING("fritz.box", d.ntp.c_str());
    TEST_ASSERT_TRUE(d.mqttEnabled);
    TEST_ASSERT_EQUAL_STRING("192.168.178.10", d.mqttHost.c_str());
    TEST_ASSERT_EQUAL_UINT32(1884, d.mqttPort);
    TEST_ASSERT_EQUAL_STRING("camper", d.mqttUser.c_str());
    TEST_ASSERT_EQUAL_UINT32(30, d.mqttInterval);
    TEST_ASSERT_EQUAL_STRING("Mein \"WLAN\" äöü", d.wifiSsid.c_str());
    TEST_ASSERT_TRUE(d.apAlways);
    TEST_ASSERT_EQUAL_STRING("ve-direct", d.hostname.c_str());
    TEST_ASSERT_EQUAL_STRING("26.11.11-1803", d.firmware.c_str());
}

void test_no_passwords_in_file()
{
    const std::string text = backupToJson(sample());
    TEST_ASSERT_TRUE(text.find("pass") == std::string::npos);
    TEST_ASSERT_TRUE(text.find("devpw") == std::string::npos);
}

void test_file_is_readable_text()
{
    const std::string text = backupToJson(sample());
    TEST_ASSERT_TRUE(text.find("\"format\": \"vedirect-backup\"") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("\"relay2.lock\": 20.5") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("\"relay1.lock\": 25.0") != std::string::npos);   // auch Voreinstellung sichtbar
    TEST_ASSERT_TRUE(text.find("\"relay1.startup\": false") != std::string::npos);
    TEST_ASSERT_TRUE(text.find("custom") == std::string::npos);                 // kein Merker
}

void test_unknown_keys_ignored()
{
    BackupData d;
    std::string error;
    const std::string text = replaced("\"format\"", "\"zukunft.neu\": [1, 2],\n  \"format\"");
    // Listen gehören nicht zum Format -> abgelehnt; ein einfacher unbekannter Wert dagegen nicht
    TEST_ASSERT_FALSE(backupFromJson(text.c_str(), d, error));
    const std::string text2 = replaced("\"format\"", "\"zukunft.neu\": 42,\n  \"format\"");
    TEST_ASSERT_TRUE_MESSAGE(backupFromJson(text2.c_str(), d, error), error.c_str());
}

void test_rejects_wrong_format_or_version()
{
    expectRejected("", "Sicherung");
    expectRejected("[1,2]", "keine Sicherung");
    expectRejected(replaced("vedirect-backup", "anderes-geraet"), "keine Sicherung");
    expectRejected(replaced("\"version\": 1", "\"version\": 2"), "neueren Firmware");
    expectRejected(replaced("\"installation\": \"camper\"", "\"installation\": \"boot\""), "Anlage");
}

void test_rejects_bad_relay_values()
{
    expectRejected(replaced("\"relay2.off\": 18.0", "\"relay2.off\": 30.0"), "R2");      // Aus > Sperre
    expectRejected(replaced("\"relay2.minOffS\": 120", "\"relay2.minOffS\": 99999"), "minOffS");
    expectRejected(replaced("\"relay2.startup\": true", "\"relay2.startup\": \"ja\""), "relay2.startup");
    expectRejected(replaced("\"relay2.release\": 25.0,", ""), "relay2.release");          // fehlt
}

void test_rejects_bad_clock_mqtt_wifi()
{
    expectRejected(replaced("\"clock.tz\": \"utc\"", "\"clock.tz\": \"mars\""), "Zeitzone");
    expectRejected(replaced("\"clock.ntp\": \"fritz.box\"", "\"clock.ntp\": \"http://x\""), "NTP");
    expectRejected(replaced("\"mqtt.port\": 1884", "\"mqtt.port\": 70000"), "mqtt.port");
    expectRejected(replaced("\"mqtt.interval\": 30", "\"mqtt.interval\": 0"), "mqtt.interval");
    expectRejected(replaced("\"mqtt.host\": \"192.168.178.10\"", "\"mqtt.host\": \"\""), "Broker");
    expectRejected(replaced("\"mqtt.base\": \"vedirect\"", "\"mqtt.base\": \"ve/#\""), "Topic");
    expectRejected(replaced("\"wifi.ssid\": \"Mein \\\"WLAN\\\" äöü\"",
                            "\"wifi.ssid\": \"123456789012345678901234567890123\""), "WLAN-Name");
}

void test_rejects_broken_json()
{
    expectRejected(replaced("\n}\n", ""), "fehlerhaft");                  // Ende fehlt
    expectRejected(replaced("\"clock.tz\": \"utc\"", "\"clock.tz\" \"utc\""), "fehlerhaft");
    expectRejected(backupToJson(sample()) + "x", "fehlerhaft");
}

void test_failed_import_leaves_output_unchanged()
{
    BackupData d = sample();
    d.tz = "berlin";
    std::string error;
    TEST_ASSERT_FALSE(backupFromJson("{\"format\":\"vedirect-backup\"}", d, error));
    TEST_ASSERT_EQUAL_STRING("berlin", d.tz.c_str());
}

int runAllTests()
{
    UNITY_BEGIN();
    RUN_TEST(test_roundtrip);
    RUN_TEST(test_no_passwords_in_file);
    RUN_TEST(test_file_is_readable_text);
    RUN_TEST(test_unknown_keys_ignored);
    RUN_TEST(test_rejects_wrong_format_or_version);
    RUN_TEST(test_rejects_bad_relay_values);
    RUN_TEST(test_rejects_bad_clock_mqtt_wifi);
    RUN_TEST(test_rejects_broken_json);
    RUN_TEST(test_failed_import_leaves_output_unchanged);
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
