/******************************************************************************
 * @file EventLog.cpp
 *
 * Beschreibung siehe EventLog.hpp.
 ******************************************************************************/

#include "EventLog.hpp"

#include <cstring>

namespace
{

/// text nach dest kopieren (max. size-1 Bytes). Wird gekürzt, dann an einer
/// UTF-8-Zeichengrenze: Folgebytes (10xxxxxx) eines angeschnittenen Zeichens
/// werden samt Startbyte entfernt.
void copyUtf8(char* dest, std::size_t size, const char* text)
{
    if (text == nullptr)
    {
        dest[0] = '\0';
        return;
    }
    std::size_t len = std::strlen(text);
    if (len < size)
    {
        std::memcpy(dest, text, len + 1);
        return;
    }

    len = size - 1;
    // Liegt die Schnittstelle in einem Mehrbyte-Zeichen? Dann zurück bis vor
    // dessen Startbyte (das Byte an Position len ist ein Folgebyte).
    while (len > 0 && (static_cast<unsigned char>(text[len]) & 0xC0) == 0x80)
    {
        --len;
    }
    std::memcpy(dest, text, len);
    dest[len] = '\0';
}

} // namespace

void EventLog::add(uint32_t nowMs, Severity severity, const char* text)
{
    Entry& e   = m_entries[m_next];
    e.seq      = ++m_seq;
    e.timeMs   = nowMs;
    e.severity = severity;
    copyUtf8(e.text, TextSize, text);

    m_next = (m_next + 1) % Capacity;
    if (m_count < Capacity)
    {
        ++m_count;
    }
}

const EventLog::Entry& EventLog::newest(std::size_t index) const
{
    // m_next zeigt hinter den neuesten Eintrag
    const std::size_t pos = (m_next + Capacity - 1 - index) % Capacity;
    return m_entries[pos];
}

void EventLog::clear()
{
    for (Entry& e : m_entries)
    {
        e = Entry();
    }
    m_next  = 0;
    m_count = 0;
    // m_seq läuft weiter: Nummern bleiben eindeutig
}
