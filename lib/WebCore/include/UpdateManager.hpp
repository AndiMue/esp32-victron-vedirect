/******************************************************************************
 * @file UpdateManager.hpp
 *
 * Updates über die WebUI (Stufe W2), Teil der WebUI-Basis (lib/WebCore).
 *
 * Drei Arten (Projektinhaber 08.11.2026):
 *   Firmware  firmware.bin           -> freier App-Bereich, dann Neustart
 *   WebUi     webui_*.tar  (web/...) -> ersetzt die WebUI-Dateien, kein Neustart
 *   Package   package_*.tar (firmware.bin + web/...) -> beides, dann Neustart
 *
 * Ablauf: Die Daten kommen in Stücken beim Hochladen (begin/write/end).
 *   - Firmware wird direkt in den freien App-Bereich geschrieben (Update).
 *     Es wird nur ein gültiges ESP32-Programm angenommen (Kopf 0xE9).
 *   - TAR wird während des Hochladens entpackt: web/... -> /_new/...
 *     Erst wenn alles vollständig und fehlerfrei ist, ersetzt /_new die
 *     bisherigen WebUI-Dateien. Ein Abbruch lässt die alte WebUI intakt.
 *
 * Probestart mit Rückfall (Projektinhaber 08.11.2026):
 *   Nach einem Firmware-Update startet die neue Firmware "auf Probe".
 *   checkTrialBoot() zählt die Starts. Läuft sie MaxTrialBoots Mal nicht
 *   StableAfterMs lang stabil (confirmStable() nicht erreicht), wird beim
 *   nächsten Start auf die vorherige Firmware zurückgeschaltet.
 *   Hinweis: Auch MaxTrialBoots schnelle Resets von Hand lösen das aus.
 *   (Der Rückfall des Herstellers ist im Arduino-Paket nicht aktiviert.)
 *
 * Nur aus EINER Task verwenden (Web-Task); checkTrialBoot() in setup().
 ******************************************************************************/

#pragma once

#include <Arduino.h>
#include <FS.h>

class UpdateManager
{
public:

    enum class Kind : uint8_t { Firmware, WebUi, Package };
    enum class State : uint8_t { Idle, Running, Success, Failed };

    static constexpr uint8_t  MaxTrialBoots = 3;
    static constexpr uint32_t StableAfterMs = 60000;

    // ---- Probestart (Rückfall) ----

    /// Ganz früh in setup() aufrufen. Schaltet ggf. auf die vorherige
    /// Firmware zurück und startet neu (kehrt dann nicht zurück).
    static void checkTrialBoot();

    /// Nach StableAfterMs Laufzeit aufrufen: neue Firmware gilt als gut.
    static void confirmStable();

    /// Wurde zuletzt auf die vorherige Firmware zurückgeschaltet?
    static bool rolledBack();

    /// Läuft die aktuelle Firmware noch auf Probe?
    static bool onTrial();

    /// Wievielter Start auf Probe (1..MaxTrialBoots), 0 = nicht auf Probe.
    static uint32_t trialBoots();

    // ---- Hochladen ----
    void begin(Kind kind);
    void write(const uint8_t* data, size_t length);
    void end();
    void abort(const char* reason);

    State         state() const          { return m_state; }
    bool          running() const        { return m_state == State::Running; }
    const String& message() const        { return m_message; }
    bool          restartRequired() const { return m_restartRequired; }

private:

    // TAR-Verarbeitung
    void tarWrite(const uint8_t* data, size_t length);
    void tarHeaderComplete();
    void tarEntryData(const uint8_t* data, size_t length);
    void tarEntryEnd();

    // Firmware
    bool firmwareBegin(size_t size);
    bool firmwareWrite(const uint8_t* data, size_t length);
    bool firmwareEnd();

    // WebUI-Dateien
    bool swapWebUi();
    void fail(const String& message);

    Kind   m_kind  = Kind::Firmware;
    State  m_state = State::Idle;
    String m_message;
    bool   m_restartRequired = false;

    // Firmware-Zustand
    bool   m_fwStarted  = false;
    bool   m_fwFirstByteChecked = false;
    size_t m_fwWritten  = 0;

    // TAR-Zustand
    enum class TarPhase : uint8_t { Header, Data, Padding, Done };
    TarPhase m_phase = TarPhase::Header;
    uint8_t  m_header[512];
    size_t   m_headerFill = 0;
    size_t   m_entryRemaining = 0;
    size_t   m_paddingRemaining = 0;
    enum class Target : uint8_t { Skip, File, Firmware };
    Target   m_target = Target::Skip;
    File     m_file;
    bool     m_sawFirmware = false;
    bool     m_sawIndex    = false;
    uint16_t m_fileCount   = 0;
};
