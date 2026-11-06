/******************************************************************************
 * @file RelayDriver.hpp
 *
 * Gibt die Sollzustände des LoadController auf die Relais-GPIOs aus
 * (Stufe 4c).
 *
 * Hardware (Stand 06.11.2026, siehe Claude.md):
 *   - "2 Relay Module" mit Optokoppler, AKTIV LOW (IN = Low -> Relais an)
 *   - Modul-VCC an 3,3 V, JD-VCC an 5 V, Jumper ab
 *   - 10 kΩ Pull-up von 3,3 V an jedem IN
 *
 * Sicherer Zustand:
 *   - Beim Booten, Reset oder Absturz sind die GPIOs hochohmig. Der Pull-up
 *     hält IN auf High -> alle Relais AUS.
 *   - begin() setzt den Pegel ZUERST auf "aus" und schaltet DANACH auf
 *     Ausgang. So entsteht beim Initialisieren kein kurzer Schaltimpuls.
 *   - Hängt die Firmware, löst der Watchdog einen Neustart aus (main.cpp)
 *     -> GPIOs wieder hochohmig -> Relais aus.
 ******************************************************************************/

#pragma once

#include <Arduino.h>

#include "LoadController.hpp"

/// Anschluss eines Relais.
struct RelayPin
{
    uint8_t gpio;
    bool    activeLow;   ///< true: Low = Relais an
};

class RelayDriver
{
public:

    explicit RelayDriver(const RelayPin (&pins)[RelayCount])
    {
        for (size_t i = 0; i < RelayCount; ++i)
        {
            m_pins[i] = pins[i];
            m_on[i]   = false;
        }
    }

    /// Alle Relais sicher AUS und als Ausgang konfigurieren.
    /// So früh wie möglich in setup() aufrufen.
    void begin()
    {
        for (size_t i = 0; i < RelayCount; ++i)
        {
            // 1. Ausgangspegel "aus" vorbelegen (wirkt, sobald Ausgang aktiv)
            digitalWrite(m_pins[i].gpio, levelFor(i, false));
            // 2. erst jetzt auf Ausgang schalten
            pinMode(m_pins[i].gpio, OUTPUT);
            // 3. sicherheitshalber noch einmal setzen
            digitalWrite(m_pins[i].gpio, levelFor(i, false));
            m_on[i] = false;
        }
    }

    /// Sollzustände des LoadController ausgeben.
    void apply(const LoadController& controller)
    {
        for (size_t i = 0; i < RelayCount; ++i)
        {
            const bool on = controller.status(static_cast<RelayId>(i)).on;
            if (on != m_on[i])
            {
                digitalWrite(m_pins[i].gpio, levelFor(i, on));
                m_on[i] = on;
            }
        }
    }

    uint8_t gpio(size_t index) const      { return m_pins[index].gpio; }
    bool    activeLow(size_t index) const { return m_pins[index].activeLow; }

private:

    uint8_t levelFor(size_t index, bool on) const
    {
        return (on != m_pins[index].activeLow) ? HIGH : LOW;
    }

    RelayPin m_pins[RelayCount];
    bool     m_on[RelayCount];
};
