/******************************************************************************
 * @file LinkMonitor.cpp
 ******************************************************************************/

#include "LinkMonitor.hpp"

LinkMonitor::LinkMonitor()
{
    reset();
}

void LinkMonitor::reset()
{
    m_total         = Counters();
    m_current       = Counters();
    m_lastErrorMs   = 0;
    m_hasErrors     = false;
    m_unstable      = false;
    m_unstableCount = 0;
}

void LinkMonitor::recordChecksumError(uint32_t nowMs)
{
    ++m_total.checksumErrors;
    ++m_current.checksumErrors;
    onError(nowMs);
}

void LinkMonitor::recordDiscardedFrame(uint32_t nowMs)
{
    ++m_total.discardedFrames;
    ++m_current.discardedFrames;
    onError(nowMs);
}

void LinkMonitor::recordOutage(uint32_t nowMs)
{
    ++m_total.outages;
    ++m_current.outages;
    onError(nowMs);
}

/******************************************************************************
 * Nach jedem Fehler: Schwellen prüfen.
 ******************************************************************************/
void LinkMonitor::onError(uint32_t nowMs)
{
    m_lastErrorMs = nowMs;
    m_hasErrors   = true;

    const uint32_t dataErrors = m_current.checksumErrors + m_current.discardedFrames;

    const bool reached =
        (m_config.outageThreshold > 0 && m_current.outages >= m_config.outageThreshold) ||
        (m_config.errorThreshold  > 0 && dataErrors        >= m_config.errorThreshold);

    if (reached && !m_unstable)
    {
        m_unstable = true;
        ++m_unstableCount;
    }
}

/******************************************************************************
 * Erholung prüfen.
 ******************************************************************************/
void LinkMonitor::update(uint32_t nowMs, bool deviceConnected)
{
    if (!m_hasErrors)
    {
        return;
    }

    if (!deviceConnected)
    {
        // Gerät getrennt (z. B. Kabel ab): Die fehlerfreie Zeit beginnt erst,
        // wenn es wieder da ist. Daher "letzter Fehler" = jetzt.
        m_lastErrorMs = nowMs;
        return;
    }

    if (m_config.recoveryMs == 0)
    {
        return;   // Erholung abgeschaltet
    }

    if (static_cast<uint32_t>(nowMs - m_lastErrorMs) >= m_config.recoveryMs)
    {
        m_current   = Counters();
        m_hasErrors = false;
        m_unstable  = false;
    }
}
