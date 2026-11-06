/******************************************************************************
 * @file DeviceState.hpp
 *
 * Aktueller Zustand des Geräts an EINEM Kanal: alle zuletzt empfangenen
 * Werte, abgelegt nach fachlicher Bedeutung (SemanticField), jeweils mit
 * Zeitpunkt der letzten Aktualisierung.
 *
 * Regeln:
 *   - Übernommen werden nur Blöcke, die zum erkannten Gerät gehören
 *     (DeviceDetector::Origin::Own oder ::Inherited). Das entscheidet der
 *     Aufrufer (UART-Task).
 *   - Bei Ausfall oder Gerätewechsel wird mit clear() ALLES gelöscht.
 *     Angezeigt wird dann "--", nie ein veralteter Wert
 *     (Entscheidung Projektinhaber 06.11.2026).
 *   - Ein Wert ist nur "frisch", wenn
 *       * das Gerät erkannt ist,
 *       * der Wert gültig ist (ValueStatus::Valid, also z. B. nicht "---"),
 *       * die letzte Aktualisierung höchstens maxAgeMs zurückliegt.
 *
 * Zeitwerte: millis(). Die Differenz wird vorzeichenlos gebildet
 * (überlaufsicher). nowMs muss NACH dem Kopieren des Zustands gelesen
 * werden; ein "Zeitstempel aus der Zukunft" wird als Alter 0 behandelt.
 *
 * Reines C++ ohne Arduino -> auf dem PC testbar.
 ******************************************************************************/

#pragma once

#include <cstddef>
#include <cstdint>

#include "DeviceDetector.hpp"
#include "MeasuredValue.hpp"
#include "SemanticField.hpp"
#include "VeDirectFrame.hpp"

class DeviceState
{
public:

    static constexpr std::size_t SlotCount =
        static_cast<std::size_t>(SemanticField::Count);

    /// Ein gespeicherter Wert.
    struct Entry
    {
        VeDirectField raw;              ///< Rohfeld wie empfangen (für Anzeige/Text)
        MeasuredValue value;            ///< umgerechneter Wert inkl. Status
        uint32_t      updatedMs = 0;    ///< millis() der letzten Aktualisierung
        bool          present   = false;///< seit letztem clear() empfangen?
    };

    DeviceState();

    /// Erkanntes Gerät des Kanals übernehmen (nach jedem DeviceDetector-Aufruf).
    void setIdentity(const DeviceDetector::Identity& identity) { m_identity = identity; }

    const DeviceDetector::Identity& identity() const { return m_identity; }

    bool isIdentified() const
    {
        return m_identity.state == DeviceDetector::State::Identified;
    }

    /// Alle bekannten Felder eines Blocks übernehmen (unbekannte Labels
    /// werden ignoriert).
    void apply(const VeDirectFrame& frame, uint32_t nowMs);

    /// Alle Werte und das Gerät löschen (Ausfall / Gerätewechsel).
    void clear();

    /// Gespeicherter Eintrag oder nullptr, wenn (noch) nicht empfangen.
    const Entry* entry(SemanticField field) const;

    /// Ist der Wert frisch (siehe Regeln oben)?
    bool isFresh(SemanticField field, uint32_t nowMs, uint32_t maxAgeMs) const;

    /// Frischen physikalischen Wert holen (z. B. 12.327 V). false = ungültig.
    bool freshValue(SemanticField field, uint32_t nowMs, uint32_t maxAgeMs, float& out) const;

    /// Frischen ganzzahligen Rohwert holen (z. B. Code, Bitmaske). false = ungültig.
    bool freshRaw(SemanticField field, uint32_t nowMs, uint32_t maxAgeMs, int64_t& out) const;

    /// Alter eines Zeitstempels (überlaufsicher, Zukunft -> 0).
    static uint32_t ageMs(uint32_t timestampMs, uint32_t nowMs);

private:

    DeviceDetector::Identity m_identity;
    Entry                    m_entries[SlotCount];
};
