/******************************************************************************
 * @file test_main.cpp
 *
 * Unit-Tests für VeDirectTextParser (Unity).
 *
 * Ausführen:
 *   pio test -e native     -> auf dem PC (benötigt GCC/G++)
 *   pio test -e esp32dev   -> auf dem ESP32 (Board per USB angeschlossen)
 *
 * Die Testdaten werden im Test selbst erzeugt, inklusive korrekter
 * VE.Direct-Prüfsumme. So bleiben die Tests lesbar und es müssen keine
 * binären Prüfsummen von Hand berechnet werden.
 ******************************************************************************/

#include <unity.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "VeDirectTextParser.hpp"

using Event = VeDirectTextParser::Event;

namespace
{

/******************************************************************************
 * Hilfsfunktionen zum Erzeugen von Testdaten
 ******************************************************************************/

struct Field
{
    const char* label;
    const char* value;
};

/// Summe aller Bytes Modulo 256.
uint8_t byteSum(const std::string& data)
{
    uint8_t sum = 0;
    for (char c : data)
    {
        sum = static_cast<uint8_t>(sum + static_cast<uint8_t>(c));
    }
    return sum;
}

/// Prüfsummenbyte anhängen, sodass die Summe des Blocks 0 ergibt.
std::string appendChecksum(std::string block)
{
    block += static_cast<char>(static_cast<uint8_t>(0u - byteSum(block)));
    return block;
}

/// Blockrumpf im Format echter Geräte: "\r\nLABEL\tWERT" ... "\r\nChecksum\t"
std::string deviceBody(const std::vector<Field>& fields)
{
    std::string s;
    for (const Field& f : fields)
    {
        s += "\r\n";
        s += f.label;
        s += '\t';
        s += f.value;
    }
    s += "\r\nChecksum\t";
    return s;
}

/// Vollständiger Block im Format echter Geräte (mit gültiger Prüfsumme).
std::string deviceBlock(const std::vector<Field>& fields)
{
    return appendChecksum(deviceBody(fields));
}

/// Block im Format des Arduino-Simulators: "LABEL\tWERT\r\n" ...
/// "Checksum\t<X>\r\n". Das abschließende "\r\n" zählt dort zur Prüfsumme.
std::string simulatorBlock(const std::vector<Field>& fields)
{
    std::string s;
    for (const Field& f : fields)
    {
        s += f.label;
        s += '\t';
        s += f.value;
        s += "\r\n";
    }
    s += "Checksum\t";
    const uint8_t sum = static_cast<uint8_t>(byteSum(s) + '\r' + '\n');
    s += static_cast<char>(static_cast<uint8_t>(0u - sum));
    s += "\r\n";
    return s;
}

/// Block erzeugen, dessen Prüfsummenbyte genau "target" ist. Dazu wird ein
/// zusätzliches Feld "PAD" mit passend gewählten Buchstaben eingefügt.
std::string blockWithChecksumByte(uint8_t target)
{
    for (unsigned count = 1; count <= 12; ++count)
    {
        const std::string prefix = "\r\nPID\t0xA057\r\nPAD\t";
        const std::string suffix = "\r\nChecksum\t";

        // Benötigte Summe der PAD-Zeichen (mod 256)
        const uint8_t base   = static_cast<uint8_t>(byteSum(prefix) + byteSum(suffix));
        const unsigned need  = static_cast<uint8_t>(0u - base - target);

        // Mit "count" Zeichen aus 'A'..'Z' erreichbar: [65*count, 90*count]
        for (unsigned padSum = need; padSum <= 90u * count; padSum += 256u)
        {
            if (padSum < 65u * count)
            {
                continue;
            }

            std::string pad(count, 'A');
            unsigned rest = padSum - 65u * count;
            for (unsigned i = 0; i < count && rest > 0; ++i)
            {
                const unsigned add = rest > 25u ? 25u : rest;
                pad[i] = static_cast<char>('A' + add);
                rest -= add;
            }

            const std::string block = appendChecksum(prefix + pad + suffix);
            if (static_cast<uint8_t>(block.back()) == target)
            {
                return block;
            }
        }
    }
    return std::string();   // nicht erreichbar
}

/// Ergebnis beim Einspeisen mehrerer Bytes.
struct FeedResult
{
    int ready          = 0;
    int checksumErrors = 0;
    int discarded      = 0;
};

FeedResult feed(VeDirectTextParser& parser, const std::string& data)
{
    FeedResult result;
    for (char c : data)
    {
        switch (parser.parse(static_cast<uint8_t>(c)))
        {
            case Event::FrameReady:     ++result.ready;          break;
            case Event::ChecksumError:  ++result.checksumErrors; break;
            case Event::FrameDiscarded: ++result.discarded;      break;
            case Event::None:                                    break;
        }
    }
    return result;
}

/// Wert eines Feldes im zuletzt gültigen Frame (nullptr, falls nicht vorhanden).
const char* valueOf(const VeDirectTextParser& parser, const char* label)
{
    const VeDirectField* field = parser.frame().find(label);
    return field ? field->value.data() : nullptr;
}

/// Ein Parser für alle Tests (global, um den Stack auf dem ESP32 zu schonen).
VeDirectTextParser g_parser;

/// Parser synchronisieren: Der erste Block nach reset() wird verworfen.
void synchronize()
{
    feed(g_parser, deviceBlock({ { "PID", "0x0000" } }));
}

} // namespace

/******************************************************************************
 * Unity: vor jedem Test frischer Parser
 ******************************************************************************/
void setUp()
{
    g_parser.reset();
}

void tearDown()
{
}

/******************************************************************************
 * Tests
 ******************************************************************************/

/// Der erste Block nach dem Start wird ohne Fehlermeldung verworfen,
/// der zweite identische Block wird angenommen.
void test_first_block_is_used_for_synchronization()
{
    const std::string block = deviceBlock({ { "PID", "0xA057" }, { "V", "13300" } });

    FeedResult r = feed(g_parser, block);
    TEST_ASSERT_EQUAL(0, r.ready);
    TEST_ASSERT_EQUAL(0, r.checksumErrors);
    TEST_ASSERT_EQUAL(0, r.discarded);

    r = feed(g_parser, block);
    TEST_ASSERT_EQUAL(1, r.ready);
}

/// Typischer MPPT-Block: alle Felder als Rohtext, kein "Checksum"-Feld.
void test_valid_mppt_block()
{
    synchronize();

    const FeedResult r = feed(g_parser, deviceBlock({
        { "PID", "0xA057" }, { "FW", "159" }, { "SER#", "HQ25419R9R3" },
        { "V", "13300" }, { "I", "-120" }, { "VPV", "18500" },
        { "PPV", "72" }, { "CS", "3" } }));

    TEST_ASSERT_EQUAL(1, r.ready);
    TEST_ASSERT_EQUAL(8, static_cast<int>(g_parser.frame().fieldCount));
    TEST_ASSERT_TRUE(g_parser.frame().checksumValid);
    TEST_ASSERT_EQUAL_STRING("0xA057", valueOf(g_parser, "PID"));
    TEST_ASSERT_EQUAL_STRING("HQ25419R9R3", valueOf(g_parser, "SER#"));
    TEST_ASSERT_EQUAL_STRING("-120", valueOf(g_parser, "I"));
    TEST_ASSERT_NULL(valueOf(g_parser, "Checksum"));
    TEST_ASSERT_EQUAL_UINT32(1, g_parser.statistics().framesOk);
}

/// Ein verfälschtes Byte -> Prüfsummenfehler, alter Frame bleibt erhalten.
void test_checksum_error_keeps_previous_frame()
{
    synchronize();
    feed(g_parser, deviceBlock({ { "V", "12000" } }));
    TEST_ASSERT_EQUAL_STRING("12000", valueOf(g_parser, "V"));

    std::string broken = deviceBlock({ { "V", "13000" } });
    broken[5] = '9';   // "\r\nV\t13000" -> "\r\nV\t19000", Prüfsumme passt nicht mehr

    const FeedResult r = feed(g_parser, broken);
    TEST_ASSERT_EQUAL(0, r.ready);
    TEST_ASSERT_EQUAL(1, r.checksumErrors);
    TEST_ASSERT_EQUAL_STRING("12000", valueOf(g_parser, "V"));
    TEST_ASSERT_EQUAL_UINT32(1, g_parser.statistics().checksumErrors);

    // Danach läuft der Empfang normal weiter.
    TEST_ASSERT_EQUAL(1, feed(g_parser, deviceBlock({ { "V", "13100" } })).ready);
}

/// Das Prüfsummenbyte darf jeden Wert haben, auch Steuerzeichen und ':'.
/// Danach muss der Parser weiterhin synchron sein.
void test_checksum_byte_may_be_control_character()
{
    const uint8_t targets[] = { '\n', '\r', '\t', ':', 0x00, 0xFF };

    synchronize();

    for (uint8_t target : targets)
    {
        const std::string block = blockWithChecksumByte(target);
        TEST_ASSERT_FALSE_MESSAGE(block.empty(), "Testdaten nicht erzeugbar");
        TEST_ASSERT_EQUAL_UINT8(target, static_cast<uint8_t>(block.back()));

        TEST_ASSERT_EQUAL_MESSAGE(1, feed(g_parser, block).ready,
                                  "Block mit Sonder-Pruefsumme nicht erkannt");
        TEST_ASSERT_EQUAL_MESSAGE(1, feed(g_parser, deviceBlock({ { "V", "1" } })).ready,
                                  "Folgeblock nicht erkannt (Synchronitaet verloren)");
    }
    TEST_ASSERT_EQUAL_UINT32(0, g_parser.statistics().checksumErrors);
}

/// Asynchrone HEX-Nachricht mitten in einem Wert und am Zeilenanfang:
/// wird übersprungen, zählt nicht zur Prüfsumme, Werte bleiben korrekt.
void test_async_hex_message_inside_block()
{
    synchronize();

    const std::string hex = ":A0102000543\n";
    const std::string block = deviceBlock({ { "PID", "0xA057" }, { "V", "13300" }, { "I", "500" } });

    // Position 1: mitten im Wert "13300" (nach "133")
    std::string withHex = block;
    withHex.insert(withHex.find("13300") + 3, hex);
    // Position 2: direkt nach dem "\r\n" vor "I"
    withHex.insert(withHex.find("\r\nI\t") + 2, hex);

    const FeedResult r = feed(g_parser, withHex);
    TEST_ASSERT_EQUAL(1, r.ready);
    TEST_ASSERT_EQUAL_STRING("13300", valueOf(g_parser, "V"));
    TEST_ASSERT_EQUAL_STRING("500", valueOf(g_parser, "I"));
    TEST_ASSERT_EQUAL_UINT32(2, g_parser.statistics().hexMessagesSkipped);
}

/// HEX-Nachricht zwischen zwei Blöcken stört nicht.
void test_hex_message_between_blocks()
{
    synchronize();

    const std::string data = ":A0102000543\n" + deviceBlock({ { "V", "13300" } });
    TEST_ASSERT_EQUAL(1, feed(g_parser, data).ready);
    TEST_ASSERT_EQUAL_UINT32(1, g_parser.statistics().hexMessagesSkipped);
}

/// Format des Arduino-Simulators ("...\r\n" am Zeilenende) wird ebenfalls
/// korrekt erkannt (der erste Block dient der Synchronisation).
void test_simulator_format()
{
    const std::string block = simulatorBlock({
        { "PID", "0xA057" }, { "V", "13400" }, { "PPV", "72" } });

    const FeedResult r = feed(g_parser, block + block + block);
    TEST_ASSERT_EQUAL(2, r.ready);
    TEST_ASSERT_EQUAL(0, r.checksumErrors);
    TEST_ASSERT_EQUAL(3, static_cast<int>(g_parser.frame().fieldCount));
    TEST_ASSERT_EQUAL_STRING("0xA057", valueOf(g_parser, "PID"));
}

/// SmartShunt sendet zwei Blöcke; der zweite hat keine PID.
/// Die PID des ersten Blocks darf NICHT im zweiten Frame auftauchen.
void test_second_block_without_pid_has_no_stale_fields()
{
    synchronize();

    feed(g_parser, deviceBlock({ { "PID", "0xC035" }, { "V", "13300" }, { "SOC", "---" } }));
    TEST_ASSERT_EQUAL_STRING("0xC035", valueOf(g_parser, "PID"));
    TEST_ASSERT_EQUAL_STRING("---", valueOf(g_parser, "SOC"));   // Rohwert bleibt erhalten

    const FeedResult r = feed(g_parser, deviceBlock({ { "H1", "-1234" }, { "H2", "0" } }));
    TEST_ASSERT_EQUAL(1, r.ready);
    TEST_ASSERT_EQUAL(2, static_cast<int>(g_parser.frame().fieldCount));
    TEST_ASSERT_NULL(valueOf(g_parser, "PID"));
    TEST_ASSERT_NULL(valueOf(g_parser, "SOC"));
}

/// Wert mit genau 32 Zeichen ist erlaubt, 33 Zeichen verwerfen den Block.
void test_value_length_limit()
{
    synchronize();

    const std::string max32(32, 'x');
    TEST_ASSERT_EQUAL(1, feed(g_parser, deviceBlock({ { "BMV", max32.c_str() } })).ready);
    TEST_ASSERT_EQUAL_STRING(max32.c_str(), valueOf(g_parser, "BMV"));

    const std::string tooLong(33, 'x');
    const FeedResult r = feed(g_parser, deviceBlock({ { "BMV", tooLong.c_str() } }));
    TEST_ASSERT_EQUAL(0, r.ready);
    TEST_ASSERT_EQUAL(1, r.discarded);

    // Folgeblock wieder ok
    TEST_ASSERT_EQUAL(1, feed(g_parser, deviceBlock({ { "V", "1" } })).ready);
}

/// Mehr Felder als VeDirectFrame::MaxFields -> Block verworfen.
void test_too_many_fields_discard_block()
{
    synchronize();

    std::vector<std::string> labels;
    std::vector<Field> fields;
    for (size_t i = 0; i <= VeDirectFrame::MaxFields; ++i)
    {
        labels.push_back("F" + std::to_string(i));
    }
    for (const std::string& l : labels)
    {
        fields.push_back({ l.c_str(), "1" });
    }

    const FeedResult r = feed(g_parser, deviceBlock(fields));
    TEST_ASSERT_EQUAL(0, r.ready);
    TEST_ASSERT_EQUAL(1, r.discarded);
}

/// Zeile ohne Tabulator -> Formatfehler.
void test_line_without_tab_discards_block()
{
    synchronize();

    const std::string block = appendChecksum("\r\nV\t13300\r\nKAPUTT\r\nI\t1\r\nChecksum\t");
    const FeedResult r = feed(g_parser, block);
    TEST_ASSERT_EQUAL(0, r.ready);
    TEST_ASSERT_EQUAL(1, r.discarded);
}

/// Fund aus dem Praxistest: Beim Einstecken entsteht ein 0x00 vor "PID".
/// Die Prüfsumme stimmt trotzdem (0x00 zählt nicht) -> Block muss verworfen
/// werden, sonst wäre das Label unsichtbar.
void test_null_byte_in_label_discards_block()
{
    synchronize();

    std::string body = "\r\n";
    body += '\0';                                   // Störung beim Einstecken
    body += "PID\t0xA057\r\nV\t13400\r\nChecksum\t";
    const std::string block = appendChecksum(body);

    const FeedResult r = feed(g_parser, block);
    TEST_ASSERT_EQUAL(0, r.ready);
    TEST_ASSERT_EQUAL(0, r.checksumErrors);          // Prüfsumme ist formal gültig!
    TEST_ASSERT_EQUAL(1, r.discarded);

    // Der nächste saubere Block wird normal angenommen.
    TEST_ASSERT_EQUAL(1, feed(g_parser, deviceBlock({ { "PID", "0xA057" } })).ready);
    TEST_ASSERT_EQUAL_STRING("0xA057", valueOf(g_parser, "PID"));
}

/// 0x00 mitten in einem Wert: würde "13400" zu "13" abschneiden
/// (13 mV statt 13,4 V) -> Block verwerfen.
void test_null_byte_in_value_discards_block()
{
    synchronize();
    feed(g_parser, deviceBlock({ { "V", "12000" } }));   // gültiger Vorwert

    std::string body = "\r\nV\t13";
    body += '\0';
    body += "400\r\nChecksum\t";

    const FeedResult r = feed(g_parser, appendChecksum(body));
    TEST_ASSERT_EQUAL(0, r.ready);
    TEST_ASSERT_EQUAL(1, r.discarded);
    TEST_ASSERT_EQUAL_STRING("12000", valueOf(g_parser, "V"));   // alter Wert bleibt
}

/// Weitere unzulässige Bytes: Steuerzeichen, Tabulator im Wert, Bytes >= 0x7F.
void test_non_printable_bytes_discard_block()
{
    synchronize();

    const char* const bodies[] =
    {
        "\r\nV\t13\x01" "400\r\nChecksum\t",   // Steuerzeichen im Wert
        "\r\nV\t13\t400\r\nChecksum\t",        // Tabulator im Wert
        "\r\nV\xFF\t13400\r\nChecksum\t",      // 0xFF im Label
        "\r\nV\t134\x7F" "00\r\nChecksum\t",   // DEL im Wert
    };

    for (const char* body : bodies)
    {
        const FeedResult r = feed(g_parser, appendChecksum(body));
        TEST_ASSERT_EQUAL_MESSAGE(1, r.discarded, "Block mit unzulaessigem Byte nicht verworfen");
        TEST_ASSERT_EQUAL(0, r.ready);
    }
}

/// Leerzeichen und typische Sonderzeichen in Werten bleiben erlaubt.
void test_printable_special_characters_are_allowed()
{
    synchronize();

    const FeedResult r = feed(g_parser, deviceBlock({
        { "BMV", "SmartShunt 500A/50mV" }, { "SER#", "HQ25419R9R3" },
        { "AR", "0" }, { "OR", "0x00000001" }, { "I", "-120" }, { "SOC", "---" } }));

    TEST_ASSERT_EQUAL(1, r.ready);
    TEST_ASSERT_EQUAL_STRING("SmartShunt 500A/50mV", valueOf(g_parser, "BMV"));
}

/// Nach reset() ist wieder eine Synchronisation nötig, Zähler sind 0.
void test_reset_requires_resynchronization()
{
    synchronize();
    feed(g_parser, deviceBlock({ { "V", "1" } }));
    TEST_ASSERT_EQUAL_UINT32(1, g_parser.statistics().framesOk);

    g_parser.reset();
    TEST_ASSERT_EQUAL_UINT32(0, g_parser.statistics().framesOk);
    TEST_ASSERT_EQUAL(0, static_cast<int>(g_parser.frame().fieldCount));
    TEST_ASSERT_EQUAL(0, feed(g_parser, deviceBlock({ { "V", "2" } })).ready);
    TEST_ASSERT_EQUAL(1, feed(g_parser, deviceBlock({ { "V", "3" } })).ready);
}

/******************************************************************************
 * Testlauf
 ******************************************************************************/
int runAllTests()
{
    UNITY_BEGIN();
    RUN_TEST(test_first_block_is_used_for_synchronization);
    RUN_TEST(test_valid_mppt_block);
    RUN_TEST(test_checksum_error_keeps_previous_frame);
    RUN_TEST(test_checksum_byte_may_be_control_character);
    RUN_TEST(test_async_hex_message_inside_block);
    RUN_TEST(test_hex_message_between_blocks);
    RUN_TEST(test_simulator_format);
    RUN_TEST(test_second_block_without_pid_has_no_stale_fields);
    RUN_TEST(test_value_length_limit);
    RUN_TEST(test_too_many_fields_discard_block);
    RUN_TEST(test_line_without_tab_discards_block);
    RUN_TEST(test_null_byte_in_label_discards_block);
    RUN_TEST(test_null_byte_in_value_discards_block);
    RUN_TEST(test_non_printable_bytes_discard_block);
    RUN_TEST(test_printable_special_characters_are_allowed);
    RUN_TEST(test_reset_requires_resynchronization);
    return UNITY_END();
}

#ifdef ARDUINO
// ---- Ausführung auf dem ESP32 ---------------------------------------------
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
// ---- Ausführung auf dem PC -------------------------------------------------
int main()
{
    return runAllTests();
}
#endif
