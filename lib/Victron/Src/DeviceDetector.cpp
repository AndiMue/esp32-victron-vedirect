/******************************************************************************
 * @file DeviceDetector.cpp
 *
 * Beschreibung der Regeln siehe DeviceDetector.hpp.
 ******************************************************************************/

#include "DeviceDetector.hpp"

#include <cstring>

#include "DeviceCatalog.hpp"
#include "TextUtil.hpp"

/******************************************************************************
 * Konstruktor / Reset
 ******************************************************************************/
DeviceDetector::DeviceDetector()
{
    reset();
}

void DeviceDetector::reset()
{
    m_identity      = Identity();
    m_previous      = Identity();
    m_stats         = Statistics();
    m_lostLastPidMs = 0;
    m_wasLost       = false;
}

/******************************************************************************
 * Zu lange kein Block mit PID?
 *
 * Vorzeichenlose Differenz: funktioniert auch beim Überlauf von millis().
 ******************************************************************************/
bool DeviceDetector::isTimedOut(uint32_t nowMs) const
{
    return m_identity.state == State::Identified &&
           static_cast<uint32_t>(nowMs - m_identity.lastPidMs) > m_timeoutMs;
}

/******************************************************************************
 * Timeout prüfen
 ******************************************************************************/
DeviceDetector::Event DeviceDetector::checkTimeout(uint32_t nowMs)
{
    if (!isTimedOut(nowMs))
    {
        return Event::None;
    }

    // Gerät verloren: alten Stand für Meldungen merken, Kanal zurücksetzen.
    m_previous      = m_identity;
    m_lostLastPidMs = m_identity.lastPidMs;
    m_wasLost       = true;
    m_identity      = Identity();

    ++m_stats.lostCount;
    return Event::Lost;
}

/******************************************************************************
 * Gerät anhand der PID festlegen.
 ******************************************************************************/
void DeviceDetector::identify(const char* productId, uint32_t nowMs)
{
    m_identity.state = State::Identified;

    std::strncpy(m_identity.productId, productId, sizeof(m_identity.productId) - 1);
    m_identity.productId[sizeof(m_identity.productId) - 1] = '\0';

    m_identity.device    = DeviceCatalog::find(productId);   // nullptr = unbekannt
    m_identity.lastPidMs = nowMs;
}

/******************************************************************************
 * Gültigen Block auswerten
 ******************************************************************************/
DeviceDetector::Result DeviceDetector::update(const VeDirectFrame& frame, uint32_t nowMs)
{
    Result result;

    const VeDirectField* pid = frame.find("PID");

    /**************************************************************************
     * Fall 1: Block mit PID
     **************************************************************************/
    if (pid != nullptr)
    {
        const char* productId = pid->value.data();
        result.origin = Origin::Own;

        if (m_identity.state == State::Identified && isTimedOut(nowMs))
        {
            // Sicherheitsnetz, falls checkTimeout() nicht vorher aufgerufen
            // wurde: wie ein Ausfall mit sofortiger Wiedererkennung behandeln.
            checkTimeout(nowMs);
        }

        if (m_identity.state == State::Unknown)
        {
            // Erstmalige Erkennung oder Wiedererkennung nach Ausfall
            if (m_wasLost)
            {
                m_stats.lastOutageMs = static_cast<uint32_t>(nowMs - m_lostLastPidMs);
                m_wasLost = false;
            }
            identify(productId, nowMs);
            ++m_stats.identifiedCount;
            result.event = Event::Identified;
        }
        else if (TextUtil::equalsIgnoreCase(m_identity.productId, productId))
        {
            // Gleiches Gerät: nur Zeitstempel auffrischen
            m_identity.lastPidMs = nowMs;
        }
        else
        {
            // Andere PID: Gerät wurde gewechselt
            m_previous = m_identity;
            identify(productId, nowMs);
            ++m_stats.changedCount;
            result.event = Event::Changed;
        }

        return result;
    }

    /**************************************************************************
     * Fall 2: Block ohne PID
     *
     * Nur einem erkannten, nicht abgelaufenen Batteriemonitor zuordnen.
     * Bei unbekanntem Gerät (device == nullptr) ist die Kategorie unklar
     * -> nicht zuordenbar.
     **************************************************************************/
    const bool isBatteryMonitor =
        m_identity.state == State::Identified &&
        m_identity.device != nullptr &&
        m_identity.device->category == DeviceCategory::BatteryMonitor;

    if (isBatteryMonitor && !isTimedOut(nowMs))
    {
        result.origin = Origin::Inherited;
    }
    else
    {
        result.origin = Origin::Unassigned;
        ++m_stats.unassignedFrames;
    }

    return result;
}
