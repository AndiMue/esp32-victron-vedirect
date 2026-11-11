/******************************************************************************
 * @file VeDirectHexReader.cpp
 *
 * Siehe VeDirectHexReader.hpp.
 ******************************************************************************/

#include "VeDirectHexReader.hpp"

/******************************************************************************
 * HexRegisterCache
 ******************************************************************************/
const HexRegisterCache::Entry* HexRegisterCache::find(uint16_t id) const
{
    for (std::size_t i = 0; i < m_count; ++i)
    {
        if (m_entries[i].id == id)
        {
            return &m_entries[i];
        }
    }
    return nullptr;
}

HexRegisterCache::Entry* HexRegisterCache::findOrAdd(uint16_t id)
{
    for (std::size_t i = 0; i < m_count; ++i)
    {
        if (m_entries[i].id == id)
        {
            return &m_entries[i];
        }
    }
    if (m_count >= Capacity)
    {
        return nullptr;
    }
    Entry& e = m_entries[m_count++];
    e = Entry();
    e.id = id;
    return &e;
}

bool HexRegisterCache::store(const VeHex::Message& message, uint32_t nowMs, bool onlyKnown)
{
    if (!message.hasRegister())
    {
        return false;
    }
    const VeHex::Response r = message.response();
    if (r != VeHex::Response::Get && r != VeHex::Response::Async)
    {
        return false;   // Set-Antworten lösen wir nie aus -> ignorieren
    }
    if (onlyKnown && find(message.registerId()) == nullptr)
    {
        return false;
    }
    Entry* e = findOrAdd(message.registerId());
    if (e == nullptr)
    {
        return false;
    }

    const uint8_t flags = message.flags();
    e->flags     = flags;
    e->updatedMs = nowMs;
    if (flags & VeHex::FlagUnknownId)          e->state = State::Unknown;
    else if (flags & VeHex::FlagParameterError) e->state = State::NoData;
    else if (flags == 0)                        e->state = State::Ok;
    else                                        e->state = State::Unknown;

    std::size_t n = message.valueLength();
    if (n > MaxValue)
    {
        n = MaxValue;
    }
    e->length = static_cast<uint8_t>(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        e->value[i] = message.value()[i];
    }
    return true;
}

void HexRegisterCache::markNoAnswer(uint16_t id, uint32_t nowMs)
{
    Entry* e = findOrAdd(id);
    if (e == nullptr)
    {
        return;
    }
    e->state     = State::NoAnswer;
    e->flags     = 0;
    e->length    = 0;
    e->updatedMs = nowMs;
}

/******************************************************************************
 * HexJobRunner
 ******************************************************************************/
bool HexJobRunner::start(const uint16_t* registers, std::size_t count, uint32_t nowMs)
{
    if (m_busy || count == 0 || count > MaxRegisters)
    {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i)
    {
        m_registers[i] = registers[i];
    }
    m_count        = count;
    m_index        = 0;
    m_busy         = true;
    m_waiting      = false;
    m_finishedFlag = false;
    m_attempts     = 0;
    m_failsInARow  = 0;
    m_result       = Result();
    m_result.requested = static_cast<uint16_t>(count);
    m_result.startedMs = nowMs;
    return true;
}

void HexJobRunner::finish(uint32_t nowMs, bool aborted)
{
    m_busy    = false;
    m_waiting = false;
    m_result.aborted    = aborted;
    m_result.finished   = true;
    m_result.finishedMs = nowMs;
    m_finishedFlag      = true;
}

bool HexJobRunner::finishedNow()
{
    const bool f = m_finishedFlag;
    m_finishedFlag = false;
    return f;
}

bool HexJobRunner::poll(uint32_t nowMs, VeHex::Request& request, HexRegisterCache& cache)
{
    if (!m_busy)
    {
        return false;
    }

    if (m_waiting)
    {
        if (static_cast<uint32_t>(nowMs - m_sentMs) < TimeoutMs)
        {
            return false;   // Antwort abwarten
        }
        if (m_attempts < MaxAttempts)
        {
            ++m_attempts;   // einmal wiederholen
            m_sentMs = nowMs;
            request  = VeHex::get(m_registers[m_index]);
            return true;
        }
        // Endgültig keine Antwort auf dieses Register
        cache.markNoAnswer(m_registers[m_index], nowMs);
        ++m_result.failed;
        m_waiting = false;
        ++m_index;
        if (++m_failsInARow >= MaxFailsInARow)
        {
            // Gerät antwortet gar nicht (z. B. Simulator nur mit Text, TX
            // nicht angeschlossen): Rest nicht mehr abfragen, damit die Leitung
            // nicht minutenlang belegt ist.
            finish(nowMs, true);
            return false;
        }
    }

    if (m_index >= m_count)
    {
        finish(nowMs, false);
        return false;
    }

    m_attempts = 1;
    m_waiting  = true;
    m_sentMs   = nowMs;
    request    = VeHex::get(m_registers[m_index]);
    return true;
}

bool HexJobRunner::onMessage(const VeHex::Message& message, uint32_t nowMs, HexRegisterCache& cache)
{
    if (!m_busy || !m_waiting || !message.hasRegister() ||
        message.response() != VeHex::Response::Get ||
        message.registerId() != m_registers[m_index])
    {
        return false;
    }
    cache.store(message, nowMs, false);
    ++m_result.answered;
    m_failsInARow = 0;
    m_waiting     = false;
    ++m_index;
    return true;   // nächste Anfrage beim nächsten poll()
}

/******************************************************************************
 * Registerlisten
 ******************************************************************************/
namespace HexJobs
{

namespace
{
const uint16_t ShuntRegisters[] =
{
    0x1000,   // Batteriekapazität (Ah)
    0x1001,   // Ladespannung (0,1 V)
    0x1002,   // Schweifstrom (0,1 %)
    0x1003,   // Ladeerkennungszeit (min)
    0x1004,   // Ladeeffizienz (%)
    0x1005,   // Peukert-Exponent (0,01)
    0x1006,   // Strom-Schwellwert (0,01 A)  (Projektinhaber 11.11.2026)
    0x1007,   // Durchschnittliche Restlaufzeit, TTG Delta T (min)
    0x1008,   // Entladeboden (0,1 %)
};

const uint16_t MpptRegisters[] =
{
    // Einstellungen zuerst (kurz), dann die Historie
    0xEDEF,   // Batteriespannung (V)
    0xEDF7,   // Absorptionsspannung (0,01 V)
    0xEDF6,   // Erhaltungsspannung (0,01 V)
    0xEDF0,   // max. Ladestrom (0,1 A)
    0xEDF1,   // Batterietyp
    0xEDF2,   // Temperaturkompensation (0,01 mV/K)
    0xEDAB,   // Lastausgang (nur Modelle mit Lastausgang, sonst "unbekannt")
    0x104F,   // Historie gesamt
    0x1050, 0x1051, 0x1052, 0x1053, 0x1054, 0x1055, 0x1056, 0x1057,
    0x1058, 0x1059, 0x105A, 0x105B, 0x105C, 0x105D, 0x105E, 0x105F,
    0x1060, 0x1061, 0x1062, 0x1063, 0x1064, 0x1065, 0x1066, 0x1067,
    0x1068, 0x1069, 0x106A, 0x106B, 0x106C, 0x106D, 0x106E,   // 31 Tage
};
} // namespace

const uint16_t* shunt(std::size_t& count)
{
    count = sizeof(ShuntRegisters) / sizeof(ShuntRegisters[0]);
    return ShuntRegisters;
}

const uint16_t* mppt(std::size_t& count)
{
    count = sizeof(MpptRegisters) / sizeof(MpptRegisters[0]);
    return MpptRegisters;
}

} // namespace HexJobs
