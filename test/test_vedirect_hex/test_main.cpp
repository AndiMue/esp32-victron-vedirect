/******************************************************************************
 * @file test_main.cpp
 *
 * Unit-Tests für VeDirectHex (W4b Schritt 1, HEX-Protokoll nur lesen).
 * Beispiele aus den Victron-Dokumenten (docs/VeDirect-Doku).
 *
 * Ausführen:
 *   pio test -e native     -> auf dem PC
 *   pio test -e esp32dev   -> auf dem ESP32
 ******************************************************************************/

#include <unity.h>

#include <cstring>
#include <string>

#include "VeDirectHex.hpp"

using namespace VeHex;

namespace
{

VeDirectHexParser g_parser;

/// Text Byte für Byte in den Parser; liefert das letzte Ereignis != None.
VeDirectHexParser::Event feed(const char* text)
{
    VeDirectHexParser::Event last = VeDirectHexParser::Event::None;
    for (const char* p = text; *p; ++p)
    {
        const VeDirectHexParser::Event e = g_parser.process(static_cast<uint8_t>(*p));
        if (e != VeDirectHexParser::Event::None)
        {
            last = e;
        }
    }
    return last;
}

/// Testhilfe: beliebige Nachricht (wie vom Gerät) mit Prüfsumme bauen.
std::string frame(uint8_t code, const uint8_t* data, std::size_t length)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string s = ":";
    s += hex[code];
    uint8_t sum = code;
    for (std::size_t i = 0; i < length; ++i)
    {
        s += hex[data[i] >> 4];
        s += hex[data[i] & 0x0F];
        sum = static_cast<uint8_t>(sum + data[i]);
    }
    const uint8_t check = static_cast<uint8_t>(0x55 - sum);
    s += hex[check >> 4];
    s += hex[check & 0x0F];
    s += '\n';
    return s;
}

void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (v >> (8 * i)) & 0xFF; }

} // namespace

void setUp()    { g_parser.reset(); }
void tearDown() {}

/******************************************************************************
 * Anfragen (Beispiele aus der Doku)
 ******************************************************************************/
void test_requests_match_documentation()
{
    TEST_ASSERT_EQUAL_STRING(":154\n", ping().text);
    TEST_ASSERT_EQUAL_STRING(":352\n", appVersion().text);
    TEST_ASSERT_EQUAL_STRING(":451\n", productId().text);
    TEST_ASSERT_EQUAL_STRING(":70010003E\n", get(0x1000).text);   // Batteriekapazität (Shunt)
    TEST_ASSERT_EQUAL_UINT32(11, get(0x1000).length);
}

void test_get_request_little_endian_and_checksum()
{
    // 0xEDF0 (MPPT max. Ladestrom): 7 + F0 + ED + 00 + Prüfsumme = 0x55
    TEST_ASSERT_EQUAL_STRING(":7F0ED0071\n", get(0xEDF0).text);
    // Selbstprüfung: eigene Anfrage wird vom eigenen Parser angenommen
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(get(0x1050).text));
    TEST_ASSERT_EQUAL_HEX16(0x1050, g_parser.message().registerId());
}

/******************************************************************************
 * Antworten (Beispiele aus der Doku)
 ******************************************************************************/
void test_ping_response()
{
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(":501440B\n"));
    const Message& m = g_parser.message();
    TEST_ASSERT_EQUAL(Response::Ping, m.response());
    TEST_ASSERT_EQUAL_UINT32(2, m.length);
    uint16_t version = 0;
    uint8_t  type    = 0;
    decodePing(static_cast<uint16_t>(readUnsigned(m.data, 2)), version, type);
    TEST_ASSERT_EQUAL_HEX16(0x0401, version);   // "4.01"
    TEST_ASSERT_EQUAL_UINT8(1, type);           // Anwendung
}

void test_product_id_response()
{
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(":181A330\n"));
    TEST_ASSERT_EQUAL(Response::Done, g_parser.message().response());
    TEST_ASSERT_EQUAL_HEX32(0xA381, readUnsigned(g_parser.message().data, 2));   // BMV-712
    TEST_ASSERT_FALSE(g_parser.message().hasRegister());
}

void test_get_response()
{
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(":7001000C80076\n"));
    const Message& m = g_parser.message();
    TEST_ASSERT_TRUE(m.hasRegister());
    TEST_ASSERT_EQUAL_HEX16(0x1000, m.registerId());
    TEST_ASSERT_EQUAL_HEX8(0, m.flags());
    TEST_ASSERT_EQUAL_UINT32(2, m.valueLength());
    TEST_ASSERT_EQUAL_UINT32(200, readUnsigned(m.value(), 2));   // 200 Ah
}

void test_set_response_with_parameter_error_flag()
{
    // Antwort des Geräts auf ein (fremdes) Set: Flag 0x04
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(":8001004010038\n"));
    TEST_ASSERT_EQUAL(Response::Set, g_parser.message().response());
    TEST_ASSERT_EQUAL_HEX8(FlagParameterError, g_parser.message().flags());
}

void test_error_and_unknown_responses()
{
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(":4AAAAFD\n"));
    TEST_ASSERT_EQUAL(Response::Error, g_parser.message().response());
    TEST_ASSERT_EQUAL_HEX32(0xAAAA, readUnsigned(g_parser.message().data, 2));

    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(":3020050\n"));
    TEST_ASSERT_EQUAL(Response::Unknown, g_parser.message().response());
}

/******************************************************************************
 * Fehlerfälle
 ******************************************************************************/
void test_checksum_error()
{
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::ChecksumError, feed(":7001000C80077\n"));
    TEST_ASSERT_EQUAL_UINT32(1, g_parser.statistics().checksumErrors);
    TEST_ASSERT_EQUAL_UINT32(0, g_parser.statistics().messages);
}

void test_odd_digit_count_is_malformed()
{
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::Malformed, feed(":7001000C8076\n"));
    TEST_ASSERT_EQUAL_UINT32(1, g_parser.statistics().malformed);
}

void test_invalid_character_is_malformed()
{
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::Malformed, feed(":7001000X80076\n"));
}

void test_too_long_is_malformed()
{
    std::string s = ":7";
    for (int i = 0; i < 60; ++i) s += "00";
    s += "\n";
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::Malformed, feed(s.c_str()));
}

void test_crlf_and_lowercase_tolerated()
{
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(":7001000c80076\r\n"));
    TEST_ASSERT_EQUAL_UINT32(200, readUnsigned(g_parser.message().value(), 2));
}

/******************************************************************************
 * Zusammenspiel mit dem Textprotokoll
 ******************************************************************************/
void test_text_protocol_is_ignored()
{
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::None,
                      feed("\r\nPID\t0xA389\r\nV\t13280\r\nI\t-1500\r\nChecksum\t"));
    TEST_ASSERT_EQUAL_UINT32(0, g_parser.statistics().malformed);
}

void test_colon_as_text_checksum_is_silently_ignored()
{
    // Prüfsummenbyte eines Textblocks = ':' -> danach beginnt der nächste Block
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::None,
                      feed("\r\nChecksum\t:\r\nPID\t0xA389\r\nV\t13280\r\n"));
    TEST_ASSERT_EQUAL_UINT32(0, g_parser.statistics().malformed);
    TEST_ASSERT_EQUAL_UINT32(0, g_parser.statistics().checksumErrors);
    // ... und eine danach folgende echte HEX-Nachricht wird erkannt
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(":501440B\n"));
}

void test_hex_message_between_text_lines()
{
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady,
                      feed("\r\nV\t13280:7001000C80076\n\r\nI\t-1500"));
    TEST_ASSERT_EQUAL_HEX16(0x1000, g_parser.message().registerId());
}

void test_async_message()
{
    // Asynchrone Meldung des MPPT (":A…", Aufbau wie Get), z. B. 0xEDD5 = 13,28 V
    const uint8_t d[] = { 0xD5, 0xED, 0x00, 0x30, 0x05 };   // 0x0530 = 1328
    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(frame(0xA, d, sizeof(d)).c_str()));
    const Message& m = g_parser.message();
    TEST_ASSERT_EQUAL(Response::Async, m.response());
    TEST_ASSERT_EQUAL_HEX16(0xEDD5, m.registerId());
    TEST_ASSERT_EQUAL_UINT32(1328, readUnsigned(m.value(), 2));
}

/******************************************************************************
 * Werte
 ******************************************************************************/
void test_read_signed()
{
    const uint8_t a[] = { 0xFF, 0xFF };          // -1 (sn16)
    const uint8_t b[] = { 0x18, 0xFC };          // -1000 (sn16)
    const uint8_t c[] = { 0x00, 0x80, 0x00 };    // +32768 (24 Bit, kein Vorzeichen)
    TEST_ASSERT_EQUAL_INT32(-1, readSigned(a, 2));
    TEST_ASSERT_EQUAL_INT32(-1000, readSigned(b, 2));
    TEST_ASSERT_EQUAL_INT32(32768, readSigned(c, 3));
}

/******************************************************************************
 * MPPT-Historie
 ******************************************************************************/
void test_history_day_record()
{
    uint8_t d[3 + 34] = {};
    put16(d, 0x1051);            // gestern
    d[2] = 0;                    // Flags
    uint8_t* v = d + 3;
    put32(v + 1, 123);           // 1,23 kWh
    put32(v + 5, 0xFFFFFFFF);    // Verbrauch n. v. (ohne Lastausgang)
    put16(v + 9, 1445);          // 14,45 V
    put16(v + 11, 1262);         // 12,62 V
    v[14] = 0; v[15] = 17;       // Fehler 17 (zweitjüngster)
    put16(v + 18, 95);
    put16(v + 20, 120);
    put16(v + 22, 300);
    put32(v + 24, 412);          // 412 W
    put16(v + 28, 287);          // 28,7 A
    put16(v + 30, 4210);         // 42,10 V
    put16(v + 32, 57);

    TEST_ASSERT_EQUAL(VeDirectHexParser::Event::MessageReady, feed(frame(0x7, d, sizeof(d)).c_str()));
    const Message& m = g_parser.message();
    TEST_ASSERT_EQUAL_HEX16(0x1051, m.registerId());

    HistoryDay day;
    TEST_ASSERT_TRUE(decodeHistoryDay(m.value(), m.valueLength(), m.flags(), day));
    TEST_ASSERT_TRUE(day.valid);
    TEST_ASSERT_EQUAL_UINT32(1230, day.yieldWh);
    TEST_ASSERT_FALSE(day.consumedValid);
    TEST_ASSERT_EQUAL_UINT16(1445, day.batteryMaxCentiV);
    TEST_ASSERT_EQUAL_UINT16(1262, day.batteryMinCentiV);
    TEST_ASSERT_EQUAL_UINT8(17, day.errors[1]);
    TEST_ASSERT_EQUAL_UINT16(95, day.bulkMin);
    TEST_ASSERT_EQUAL_UINT16(120, day.absorptionMin);
    TEST_ASSERT_EQUAL_UINT16(300, day.floatMin);
    TEST_ASSERT_EQUAL_UINT32(412, day.powerMaxW);
    TEST_ASSERT_EQUAL_UINT16(287, day.batteryCurrentMaxDeciA);
    TEST_ASSERT_EQUAL_UINT16(4210, day.panelMaxCentiV);
    TEST_ASSERT_EQUAL_UINT16(57, day.sequence);
}

void test_history_day_without_data()
{
    // Laut Doku: Tag ohne Daten -> leerer Datensatz mit Flag 0x04
    uint8_t d[3 + 34] = {};
    put16(d, 0x106E);
    d[2] = FlagParameterError;
    feed(frame(0x7, d, sizeof(d)).c_str());
    HistoryDay day;
    day.valid = true;
    TEST_ASSERT_TRUE(decodeHistoryDay(g_parser.message().value(), g_parser.message().valueLength(),
                                      g_parser.message().flags(), day));
    TEST_ASSERT_FALSE(day.valid);
}

void test_history_day_unknown_or_short()
{
    const uint8_t v[10] = {};
    HistoryDay day;
    TEST_ASSERT_FALSE(decodeHistoryDay(v, sizeof(v), 0, day));               // zu kurz
    TEST_ASSERT_FALSE(decodeHistoryDay(v, sizeof(v), FlagUnknownId, day));   // Register unbekannt
}

void test_history_total_fw116_and_fw117()
{
    uint8_t v[34] = {};
    v[2] = 2;                    // jüngster Fehler
    put32(v + 6, 15000);         // 150,00 kWh (Benutzer)
    put32(v + 10, 32150);        // 321,50 kWh (System)
    put16(v + 14, 4780);
    put16(v + 16, 1460);
    v[18] = 30;
    put16(v + 19, 1180);

    HistoryTotal t;
    TEST_ASSERT_TRUE(decodeHistoryTotal(v, 19, t));     // FW 1.16
    TEST_ASSERT_EQUAL_UINT32(150000, t.userYieldWh);
    TEST_ASSERT_EQUAL_UINT32(321500, t.systemYieldWh);
    TEST_ASSERT_EQUAL_UINT16(4780, t.panelMaxCentiV);
    TEST_ASSERT_EQUAL_UINT16(1460, t.batteryMaxCentiV);
    TEST_ASSERT_EQUAL_UINT8(30, t.daysAvailable);
    TEST_ASSERT_EQUAL_UINT8(2, t.errors[0]);
    TEST_ASSERT_FALSE(t.batteryMinValid);

    TEST_ASSERT_TRUE(decodeHistoryTotal(v, 34, t));     // FW 1.17+
    TEST_ASSERT_TRUE(t.batteryMinValid);
    TEST_ASSERT_EQUAL_UINT16(1180, t.batteryMinCentiV);

    TEST_ASSERT_FALSE(decodeHistoryTotal(v, 18, t));    // zu kurz
}

/******************************************************************************
 * Testlauf
 ******************************************************************************/
int runAllTests()
{
    UNITY_BEGIN();
    RUN_TEST(test_requests_match_documentation);
    RUN_TEST(test_get_request_little_endian_and_checksum);
    RUN_TEST(test_ping_response);
    RUN_TEST(test_product_id_response);
    RUN_TEST(test_get_response);
    RUN_TEST(test_set_response_with_parameter_error_flag);
    RUN_TEST(test_error_and_unknown_responses);
    RUN_TEST(test_checksum_error);
    RUN_TEST(test_odd_digit_count_is_malformed);
    RUN_TEST(test_invalid_character_is_malformed);
    RUN_TEST(test_too_long_is_malformed);
    RUN_TEST(test_crlf_and_lowercase_tolerated);
    RUN_TEST(test_text_protocol_is_ignored);
    RUN_TEST(test_colon_as_text_checksum_is_silently_ignored);
    RUN_TEST(test_hex_message_between_text_lines);
    RUN_TEST(test_async_message);
    RUN_TEST(test_read_signed);
    RUN_TEST(test_history_day_record);
    RUN_TEST(test_history_day_without_data);
    RUN_TEST(test_history_day_unknown_or_short);
    RUN_TEST(test_history_total_fw116_and_fw117);
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
