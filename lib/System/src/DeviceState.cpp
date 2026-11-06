/******************************************************************************
 * @file DeviceState.cpp
 ******************************************************************************/

#include "DeviceState.hpp"

#include "ValueConverter.hpp"

DeviceState::DeviceState()
{
    clear();
}

void DeviceState::clear()
{
    m_identity = DeviceDetector::Identity();
    for (std::size_t i = 0; i < SlotCount; ++i)
    {
        m_entries[i] = Entry();
    }
}

void DeviceState::apply(const VeDirectFrame& frame, uint32_t nowMs)
{
    for (std::size_t i = 0; i < frame.fieldCount; ++i)
    {
        const VeDirectField& field = frame.fields[i];
        const MeasuredValue  value = ValueConverter::convert(field);

        // Unbekannte Labels haben keine Bedeutung -> nicht speichern
        if (value.definition == nullptr)
        {
            continue;
        }

        const std::size_t slot = static_cast<std::size_t>(value.definition->semantic);
        if (slot == 0 || slot >= SlotCount)   // 0 = SemanticField::Unknown
        {
            continue;
        }

        Entry& entry    = m_entries[slot];
        entry.raw       = field;
        entry.value     = value;
        entry.updatedMs = nowMs;
        entry.present   = true;
    }
}

const DeviceState::Entry* DeviceState::entry(SemanticField field) const
{
    const std::size_t slot = static_cast<std::size_t>(field);
    if (slot == 0 || slot >= SlotCount || !m_entries[slot].present)
    {
        return nullptr;
    }
    return &m_entries[slot];
}

uint32_t DeviceState::ageMs(uint32_t timestampMs, uint32_t nowMs)
{
    const uint32_t age = nowMs - timestampMs;   // vorzeichenlos -> überlaufsicher
    // Liegt der Zeitstempel (knapp) in der Zukunft, ergibt sich ein riesiger
    // Wert. Das passiert, wenn nowMs VOR dem Kopieren gelesen wurde.
    return age > 0x7FFFFFFFu ? 0u : age;
}

bool DeviceState::isFresh(SemanticField field, uint32_t nowMs, uint32_t maxAgeMs) const
{
    if (!isIdentified())
    {
        return false;
    }
    const Entry* e = entry(field);
    return e != nullptr &&
           e->value.isValid() &&
           ageMs(e->updatedMs, nowMs) <= maxAgeMs;
}

bool DeviceState::freshValue(SemanticField field, uint32_t nowMs, uint32_t maxAgeMs, float& out) const
{
    if (!isFresh(field, nowMs, maxAgeMs))
    {
        return false;
    }
    out = entry(field)->value.value;
    return true;
}

bool DeviceState::freshRaw(SemanticField field, uint32_t nowMs, uint32_t maxAgeMs, int64_t& out) const
{
    if (!isFresh(field, nowMs, maxAgeMs))
    {
        return false;
    }
    out = entry(field)->value.raw;
    return true;
}
