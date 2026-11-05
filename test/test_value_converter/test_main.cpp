/******************************************************************************
 * @file test_main.cpp
 *
 * Unit-Tests für FieldCatalog, ValueConverter, ValueFormatter und
 * DeviceCatalog (Unity).
 *
 * Ausführen:
 *   pio test -e native     -> auf dem PC
 *   pio test -e esp32dev   -> auf dem ESP32
 *
 * Die erwarteten Werte stammen aus "VE.Direct Protocol" 3.34 und aus
 * echten Ausgaben der Projektgeräte (SmartShunt 0xC035, MPPT 0xA057).
 ******************************************************************************/

#include <unity.h>

#include <cstring>
#include <string>

#include "DeviceCatalog.hpp"
#include "FieldCatalog.hpp"
#include "ValueConverter.hpp"
#include "ValueFormatter.hpp"

namespace
{

/// Feld aus Label und Wert bauen (wie es der Parser liefern würde).
VeDirectField makeField(const char* label, const char* value)
{
    VeDirectField field;
    std::strncpy(field.label.data(), label, VeDirectField::LabelSize - 1);
    std::strncpy(field.value.data(), value, VeDirectField::ValueSize - 1);
    return field;
}

/// Umrechnen + formatieren in einem Schritt.
std::string formatted(const char* label, const char* value)
{
    const VeDirectField field = makeField(label, value);
    const MeasuredValue measured = ValueConverter::convert(field);
    char buffer[ValueFormatter::BufferSize];
    return ValueFormatter::format(field, measured, buffer, sizeof(buffer));
}

ValueStatus statusOf(const char* label, const char* value)
{
    return ValueConverter::convert(label, value).status;
}

} // namespace

void setUp() {}
void tearDown() {}

/******************************************************************************
 * FieldCatalog
 ******************************************************************************/

/// Jedes Label darf nur einmal vorkommen; Pflichtangaben vorhanden.
void test_catalog_labels_unique_and_complete()
{
    for (std::size_t i = 0; i < FieldCatalog::size(); ++i)
    {
        const FieldDefinition& a = FieldCatalog::at(i);
        TEST_ASSERT_NOT_NULL(a.label);
        TEST_ASSERT_NOT_NULL(a.name);
        TEST_ASSERT_NOT_NULL(a.unit);
        TEST_ASSERT_TRUE_MESSAGE(std::strlen(a.label) <= 8, a.label);   // Doku: max. 8 Zeichen

        const bool needsTable = a.type == ValueType::Code ||
                                a.type == ValueType::BitmaskDecimal ||
                                a.type == ValueType::BitmaskHex;
        TEST_ASSERT_TRUE_MESSAGE(needsTable == (a.codes != nullptr), a.label);

        for (std::size_t j = i + 1; j < FieldCatalog::size(); ++j)
        {
            TEST_ASSERT_TRUE_MESSAGE(std::strcmp(a.label, FieldCatalog::at(j).label) != 0, a.label);
        }
    }
}

/// Alle Labels, die die Projektgeräte tatsächlich senden, sind bekannt.
void test_catalog_knows_all_fields_of_project_devices()
{
    const char* shunt[] = { "PID", "V", "I", "P", "CE", "SOC", "TTG", "Alarm", "AR",
                            "BMV", "FW", "MON", "H1", "H2", "H3", "H4", "H5", "H6",
                            "H7", "H8", "H9", "H10", "H11", "H12", "H15", "H16", "H17", "H18" };
    const char* mppt[]  = { "PID", "FW", "SER#", "V", "I", "VPV", "PPV", "CS", "MPPT",
                            "OR", "ERR", "LOAD", "IL", "H19", "H20", "H21", "H22", "H23", "HSDS" };

    for (const char* label : shunt) TEST_ASSERT_NOT_NULL_MESSAGE(FieldCatalog::find(label), label);
    for (const char* label : mppt)  TEST_ASSERT_NOT_NULL_MESSAGE(FieldCatalog::find(label), label);

    TEST_ASSERT_NULL(FieldCatalog::find("XYZ"));
    TEST_ASSERT_NULL(FieldCatalog::find("v"));   // Labels sind exakt
}

/******************************************************************************
 * Zahlen und Einheiten
 ******************************************************************************/
void test_number_scaling()
{
    TEST_ASSERT_EQUAL_STRING("12,327 V",  formatted("V", "12327").c_str());     // mV -> V
    TEST_ASSERT_EQUAL_STRING("-0,120 A",  formatted("I", "-120").c_str());      // mA -> A
    TEST_ASSERT_EQUAL_STRING("-12,500 A", formatted("I", "-12500").c_str());
    TEST_ASSERT_EQUAL_STRING("0,007 A",   formatted("IL", "7").c_str());        // IL ist mA
    TEST_ASSERT_EQUAL_STRING("100,0 %",   formatted("SOC", "1000").c_str());    // Promille -> %
    TEST_ASSERT_EQUAL_STRING("95,3 %",    formatted("SOC", "953").c_str());
    TEST_ASSERT_EQUAL_STRING("-1,234 Ah", formatted("H1", "-1234").c_str());    // mAh -> Ah
    TEST_ASSERT_EQUAL_STRING("0,03 kWh",  formatted("H19", "3").c_str());       // 0,01 kWh
    TEST_ASSERT_EQUAL_STRING("96 W",      formatted("PPV", "96").c_str());
    TEST_ASSERT_EQUAL_STRING("25 °C",     formatted("T", "25").c_str());
    TEST_ASSERT_EQUAL_STRING("0",         formatted("H4", "0").c_str());        // ohne Einheit
    TEST_ASSERT_EQUAL_STRING("14,412 V",  formatted("H8", "14412").c_str());

    const MeasuredValue v = ValueConverter::convert("V", "12327");
    TEST_ASSERT_TRUE(v.isValid());
    TEST_ASSERT_TRUE(v.raw == 12327);   // kein INT64-Makro: auf dem ESP32 ist Unity ohne 64-Bit-Support
    TEST_ASSERT_FLOAT_WITHIN(0.0005f, 12.327f, v.value);
    TEST_ASSERT_TRUE(v.definition->semantic == SemanticField::BatteryVoltage);
}

/// "---" wird NIE zu 0 – entscheidend für die spätere SOC-Relaislogik.
void test_dashes_mean_not_available()
{
    TEST_ASSERT_TRUE(statusOf("SOC", "---") == ValueStatus::NotAvailable);
    TEST_ASSERT_TRUE(statusOf("CE",  "---") == ValueStatus::NotAvailable);
    TEST_ASSERT_TRUE(statusOf("TTG", "---") == ValueStatus::NotAvailable);
    TEST_ASSERT_TRUE(statusOf("T",   "---") == ValueStatus::NotAvailable);
    TEST_ASSERT_FALSE(ValueConverter::convert("SOC", "---").isValid());
    TEST_ASSERT_EQUAL_STRING("nicht verfügbar", formatted("SOC", "---").c_str());
}

void test_time_to_go()
{
    TEST_ASSERT_TRUE(statusOf("TTG", "-1") == ValueStatus::Infinite);
    TEST_ASSERT_EQUAL_STRING("unendlich (keine Entladung)", formatted("TTG", "-1").c_str());
    TEST_ASSERT_EQUAL_STRING("125 min", formatted("TTG", "125").c_str());
}

/// Fehlerhafte Rohtexte werden als ungültig erkannt, nicht teilweise gelesen.
void test_invalid_raw_text()
{
    TEST_ASSERT_TRUE(statusOf("V", "12a") == ValueStatus::Invalid);
    TEST_ASSERT_TRUE(statusOf("V", "")    == ValueStatus::Invalid);
    TEST_ASSERT_TRUE(statusOf("V", "-")   == ValueStatus::Invalid);
    TEST_ASSERT_TRUE(statusOf("V", "99999999999") == ValueStatus::Invalid);
    TEST_ASSERT_TRUE(statusOf("OR", "00000001")   == ValueStatus::Invalid);   // "0x" fehlt
    TEST_ASSERT_TRUE(statusOf("OR", "0x")         == ValueStatus::Invalid);
    TEST_ASSERT_TRUE(statusOf("LOAD", "MAYBE")    == ValueStatus::Invalid);
    TEST_ASSERT_EQUAL_STRING("ungültiger Wert", formatted("V", "12a").c_str());
}

void test_unknown_field()
{
    TEST_ASSERT_TRUE(statusOf("XYZ", "1") == ValueStatus::UnknownField);
    const MeasuredValue v = ValueConverter::convert("XYZ", "1");
    TEST_ASSERT_EQUAL_STRING("(unbekanntes Feld)", ValueFormatter::name(v));
    TEST_ASSERT_EQUAL_STRING("-", formatted("XYZ", "1").c_str());
}

/******************************************************************************
 * ON/OFF, Text
 ******************************************************************************/
void test_on_off_case_insensitive()
{
    TEST_ASSERT_EQUAL_STRING("EIN", formatted("LOAD", "ON").c_str());
    TEST_ASSERT_EQUAL_STRING("AUS", formatted("Alarm", "OFF").c_str());
    TEST_ASSERT_EQUAL_STRING("EIN", formatted("Relay", "On").c_str());   // alte BMV
    TEST_ASSERT_EQUAL_STRING("AUS", formatted("Relay", "off").c_str());
}

void test_text_fields()
{
    TEST_ASSERT_EQUAL_STRING("HQ25419R9R3", formatted("SER#", "HQ25419R9R3").c_str());
    TEST_ASSERT_EQUAL_STRING("SmartShunt 500A/50mV", formatted("BMV", "SmartShunt 500A/50mV").c_str());
    TEST_ASSERT_EQUAL_STRING("---", formatted("SER#", "---").c_str());   // Text bleibt Text
}

/******************************************************************************
 * Codes und Bitmasken
 ******************************************************************************/
void test_codes()
{
    TEST_ASSERT_EQUAL_STRING("Aus",                  formatted("CS", "0").c_str());
    TEST_ASSERT_EQUAL_STRING("Bulk (Konstantstrom)", formatted("CS", "3").c_str());
    TEST_ASSERT_EQUAL_STRING("Float (Erhaltung)",    formatted("CS", "5").c_str());
    TEST_ASSERT_EQUAL_STRING("Externe Steuerung",    formatted("CS", "252").c_str());
    TEST_ASSERT_EQUAL_STRING("unbekannter Code 99",  formatted("CS", "99").c_str());
    TEST_ASSERT_EQUAL_STRING("Kein Fehler",          formatted("ERR", "0").c_str());
    TEST_ASSERT_EQUAL_STRING("PV-Eingangsspannung zu hoch", formatted("ERR", "33").c_str());
    TEST_ASSERT_EQUAL_STRING("MPP-Tracker aktiv",    formatted("MPPT", "2").c_str());
    TEST_ASSERT_EQUAL_STRING("Batteriemonitor",      formatted("MON", "0").c_str());
    TEST_ASSERT_EQUAL_STRING("Solarladegerät",       formatted("MON", "-9").c_str());
}

void test_bitmasks()
{
    TEST_ASSERT_EQUAL_STRING("keiner",                 formatted("OR", "0x00000000").c_str());
    TEST_ASSERT_EQUAL_STRING("Keine Eingangsleistung", formatted("OR", "0x00000001").c_str());
    TEST_ASSERT_EQUAL_STRING("Keine Eingangsleistung, Ausgeschaltet (Schalter)",
                             formatted("OR", "0x00000003").c_str());
    TEST_ASSERT_EQUAL_STRING("unbekannte Bits 0x00001000", formatted("OR", "0x00001000").c_str());
    TEST_ASSERT_EQUAL_STRING("keiner",                       formatted("AR", "0").c_str());
    TEST_ASSERT_EQUAL_STRING("Unterspannung, Niedriger SOC", formatted("AR", "5").c_str());   // Doku-Beispiel
    TEST_ASSERT_EQUAL_STRING("BLE abschaltbar",              formatted("CAP_BLE", "0x00000001").c_str());
}

/// Sehr lange Bitmaskentexte werden sicher gekürzt (kein Pufferüberlauf).
void test_formatter_truncates_safely()
{
    const VeDirectField field = makeField("AR", "16383");   // alle 14 Bits
    const MeasuredValue value = ValueConverter::convert(field);
    char small[16];
    std::memset(small, 'X', sizeof(small));
    ValueFormatter::format(field, value, small, sizeof(small));
    TEST_ASSERT_EQUAL_CHAR('\0', small[sizeof(small) - 1]);
    TEST_ASSERT_EQUAL(15, static_cast<int>(std::strlen(small)));
}

/******************************************************************************
 * Firmware
 ******************************************************************************/
void test_firmware_versions()
{
    TEST_ASSERT_EQUAL_STRING("v4.19", formatted("FW", "0419").c_str());   // SmartShunt
    TEST_ASSERT_EQUAL_STRING("v1.59", formatted("FW", "159").c_str());    // MPPT
    TEST_ASSERT_EQUAL_STRING("v2.08 (Release Candidate C)", formatted("FW", "C208").c_str());
    TEST_ASSERT_EQUAL_STRING("v2.08", formatted("FWE", "0208FF").c_str());
    TEST_ASSERT_EQUAL_STRING("v2.08", formatted("FWE", "208FF").c_str());
    TEST_ASSERT_EQUAL_STRING("v2.08-beta-01", formatted("FWE", "020801").c_str());
    TEST_ASSERT_TRUE(statusOf("FW", "12345") == ValueStatus::Invalid);
}

/******************************************************************************
 * Produkt-ID / DeviceCatalog
 ******************************************************************************/
void test_product_ids()
{
    TEST_ASSERT_EQUAL_STRING("SmartShunt 500A/50mV",   formatted("PID", "0xC035").c_str());
    TEST_ASSERT_EQUAL_STRING("SmartSolar MPPT 100/50", formatted("PID", "0xA057").c_str());
    TEST_ASSERT_EQUAL_STRING("SmartShunt 500A/50mV",   formatted("PID", "0xA389").c_str());
    TEST_ASSERT_EQUAL_STRING("BlueSolar MPPT 75/15",   formatted("PID", "0xA042").c_str());
    TEST_ASSERT_EQUAL_STRING("unbekanntes Gerät",      formatted("PID", "0xFFFF").c_str());

    // Groß-/Kleinschreibung egal
    const DeviceInfo* mppt = DeviceCatalog::find("0xa057");
    TEST_ASSERT_NOT_NULL(mppt);
    TEST_ASSERT_TRUE(mppt->category == DeviceCategory::Mppt);

    // Korrigierte PIDs (früher fälschlich ohne "A")
    TEST_ASSERT_NULL(DeviceCatalog::find("0x389"));
    TEST_ASSERT_TRUE(DeviceCatalog::find("0xC035")->category == DeviceCategory::BatteryMonitor);
}

/******************************************************************************
 * Testlauf
 ******************************************************************************/
int runAllTests()
{
    UNITY_BEGIN();
    RUN_TEST(test_catalog_labels_unique_and_complete);
    RUN_TEST(test_catalog_knows_all_fields_of_project_devices);
    RUN_TEST(test_number_scaling);
    RUN_TEST(test_dashes_mean_not_available);
    RUN_TEST(test_time_to_go);
    RUN_TEST(test_invalid_raw_text);
    RUN_TEST(test_unknown_field);
    RUN_TEST(test_on_off_case_insensitive);
    RUN_TEST(test_text_fields);
    RUN_TEST(test_codes);
    RUN_TEST(test_bitmasks);
    RUN_TEST(test_formatter_truncates_safely);
    RUN_TEST(test_firmware_versions);
    RUN_TEST(test_product_ids);
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
