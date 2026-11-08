/******************************************************************************
 * @file UpdateManager.cpp
 *
 * Beschreibung siehe UpdateManager.hpp.
 ******************************************************************************/

#include "UpdateManager.hpp"

#include "SettingsStore.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(ESP32)
#include <LittleFS.h>
#include <Update.h>
#include <esp_ota_ops.h>
#else
#error "UpdateManager: ESP8266-Variante folgt in einem eigenen Schritt (siehe Claude.md)"
#endif

namespace
{

const char* const Ns          = "ota";
const char* const KeyTrial    = "trial";
const char* const KeyBoots    = "boots";
const char* const KeyRollback = "rolledback";

const char* const NewDir = "/_new";   ///< Zwischenordner für die neue WebUI

constexpr uint8_t EspImageMagic = 0xE9;

/// Oktalzahl aus einem TAR-Feld lesen.
size_t parseOctal(const uint8_t* field, size_t length)
{
    size_t value = 0;
    for (size_t i = 0; i < length; ++i)
    {
        const uint8_t c = field[i];
        if (c == 0 || c == ' ') { if (value != 0) break; else continue; }
        if (c < '0' || c > '7') break;
        value = value * 8 + (c - '0');
    }
    return value;
}

/// Alle Zwischenordner eines Pfads anlegen ("/a/b/c.txt" -> /a, /a/b).
void makeParentDirs(const String& path)
{
    int pos = 1;
    while ((pos = path.indexOf('/', pos)) > 0)
    {
        const String dir = path.substring(0, pos);
        if (!LittleFS.exists(dir))
        {
            LittleFS.mkdir(dir);
        }
        ++pos;
    }
}

/// Ein Verzeichniseintrag (Name ohne Pfad).
struct DirEntry
{
    String name;
    bool   isDir;
};

/// Einträge eines Ordners ZUERST sammeln. Löschen/Verschieben während des
/// Durchlaufens kann sonst Einträge überspringen.
std::vector<DirEntry> listDir(const String& path)
{
    std::vector<DirEntry> list;
    File dir = LittleFS.open(path);
    if (!dir || !dir.isDirectory())
    {
        return list;
    }
    File entry = dir.openNextFile();
    while (entry)
    {
        const String full = entry.name();
        list.push_back({ full.substring(full.lastIndexOf('/') + 1), entry.isDirectory() });
        entry.close();
        entry = dir.openNextFile();
    }
    dir.close();
    return list;
}

String joinPath(const String& dir, const String& name)
{
    return (dir == "/" ? String("") : dir) + "/" + name;
}

/// Ordner rekursiv löschen.
void removeRecursive(const String& path)
{
    if (!LittleFS.exists(path))
    {
        return;
    }
    for (const DirEntry& e : listDir(path))
    {
        const String child = joinPath(path, e.name);
        if (e.isDir) removeRecursive(child); else LittleFS.remove(child);
    }
    LittleFS.rmdir(path);
}

/// Inhalt von "from" nach "to" verschieben (rekursiv).
bool moveTree(const String& from, const String& to)
{
    bool ok = true;
    for (const DirEntry& e : listDir(from))
    {
        const String src = joinPath(from, e.name);
        const String dst = joinPath(to, e.name);
        if (e.isDir)
        {
            if (!LittleFS.exists(dst)) LittleFS.mkdir(dst);
            ok = moveTree(src, dst) && ok;
        }
        else
        {
            if (LittleFS.exists(dst)) LittleFS.remove(dst);
            ok = LittleFS.rename(src, dst) && ok;
        }
    }
    return ok;
}

} // namespace

/******************************************************************************
 * Probestart / Rückfall
 ******************************************************************************/
void UpdateManager::checkTrialBoot()
{
    if (!SettingsStore::getBool(Ns, KeyTrial, false))
    {
        return;
    }

    const uint32_t boots = SettingsStore::getUInt(Ns, KeyBoots, 0) + 1;
    SettingsStore::putUInt(Ns, KeyBoots, boots);

    if (boots <= MaxTrialBoots)
    {
        return;   // weiter auf Probe
    }

    // Neue Firmware ist MaxTrialBoots Mal nicht stabil gelaufen:
    // auf den anderen App-Bereich (vorherige Firmware) zurückschalten.
    SettingsStore::putBool(Ns, KeyTrial, false);
    SettingsStore::putUInt(Ns, KeyBoots, 0);

    const esp_partition_t* previous = esp_ota_get_next_update_partition(nullptr);
    if (previous != nullptr && esp_ota_set_boot_partition(previous) == ESP_OK)
    {
        SettingsStore::putBool(Ns, KeyRollback, true);
        ESP.restart();
    }
    // Rückfall nicht möglich (z. B. kein gültiges altes Programm): weiterlaufen
}

void UpdateManager::confirmStable()
{
    if (SettingsStore::getBool(Ns, KeyTrial, false))
    {
        SettingsStore::putBool(Ns, KeyTrial, false);
        SettingsStore::putUInt(Ns, KeyBoots, 0);
    }
}

bool UpdateManager::rolledBack()
{
    return SettingsStore::getBool(Ns, KeyRollback, false);
}

bool UpdateManager::onTrial()
{
    return SettingsStore::getBool(Ns, KeyTrial, false);
}

uint32_t UpdateManager::trialBoots()
{
    return SettingsStore::getUInt(Ns, KeyBoots, 0);
}

/******************************************************************************
 * Hochladen
 ******************************************************************************/
void UpdateManager::begin(Kind kind)
{
    if (m_file) m_file.close();
    if (Update.isRunning()) Update.abort();

    m_kind  = kind;
    m_state = State::Running;
    m_message = "Upload läuft";
    m_restartRequired = false;

    m_fwStarted = false;
    m_fwFirstByteChecked = false;
    m_fwWritten = 0;

    m_phase = TarPhase::Header;
    m_headerFill = 0;
    m_entryRemaining = 0;
    m_paddingRemaining = 0;
    m_target = Target::Skip;
    m_sawFirmware = false;
    m_sawIndex = false;
    m_fileCount = 0;

    if (kind != Kind::Firmware)
    {
        removeRecursive(NewDir);   // Reste eines früheren Abbruchs
        LittleFS.mkdir(NewDir);
    }
}

void UpdateManager::fail(const String& message)
{
    if (m_state != State::Running) return;
    if (m_file) m_file.close();
    if (Update.isRunning()) Update.abort();
    removeRecursive(NewDir);
    m_state = State::Failed;
    m_message = message;
}

void UpdateManager::abort(const char* reason)
{
    fail(String("Abgebrochen: ") + reason);
}

void UpdateManager::write(const uint8_t* data, size_t length)
{
    if (m_state != State::Running) return;

    if (m_kind == Kind::Firmware)
    {
        if (!m_fwStarted && !firmwareBegin(UPDATE_SIZE_UNKNOWN)) return;
        firmwareWrite(data, length);
    }
    else
    {
        tarWrite(data, length);
    }
}

void UpdateManager::end()
{
    if (m_state != State::Running) return;

    if (m_kind == Kind::Firmware)
    {
        if (!m_fwStarted) { fail("Leere Datei"); return; }
        if (!firmwareEnd()) return;
        m_state = State::Success;
        m_message = "Firmware aktualisiert. Neustart …";
        m_restartRequired = true;
        return;
    }

    // ---- TAR ----
    if (m_phase == TarPhase::Data || m_phase == TarPhase::Padding ||
        (m_phase == TarPhase::Header && m_headerFill != 0))
    {
        fail("TAR-Datei unvollständig");
        return;
    }
    if (m_file) m_file.close();

    if (m_kind == Kind::Package && !m_sawFirmware)
    {
        fail("Paket enthält keine firmware.bin");
        return;
    }
    if (!m_sawIndex)
    {
        fail("Keine WebUI gefunden (web/index.html fehlt)");
        return;
    }

    // Erst die Firmware abschließen (prüft das Programm), dann die WebUI
    // austauschen. So bleibt bei einem Firmware-Fehler alles beim Alten.
    if (m_kind == Kind::Package && !firmwareEnd())
    {
        return;
    }
    if (!swapWebUi())
    {
        if (m_kind == Kind::Package)
        {
            // Firmware ist bereits übernommen -> trotzdem neu starten
            m_state = State::Failed;
            m_message = "Firmware aktualisiert, WebUI-Übernahme fehlgeschlagen. Neustart …";
            m_restartRequired = true;
            return;
        }
        fail("WebUI konnte nicht übernommen werden");
        return;
    }

    m_state = State::Success;
    if (m_kind == Kind::Package)
    {
        m_message = "Firmware und WebUI aktualisiert. Neustart …";
        m_restartRequired = true;
    }
    else
    {
        m_message = "WebUI aktualisiert (" + String(m_fileCount) + " Dateien). Seite neu laden.";
    }
}

/******************************************************************************
 * Firmware
 ******************************************************************************/
bool UpdateManager::firmwareBegin(size_t size)
{
    if (!Update.begin(size, U_FLASH))
    {
        fail(String("Firmware passt nicht in den App-Bereich: ") + Update.errorString());
        return false;
    }
    m_fwStarted = true;
    return true;
}

bool UpdateManager::firmwareWrite(const uint8_t* data, size_t length)
{
    if (length == 0) return true;

    if (!m_fwFirstByteChecked)
    {
        m_fwFirstByteChecked = true;
        if (data[0] != EspImageMagic)
        {
            fail("Keine gültige ESP32-Firmware (falsche Datei?)");
            return false;
        }
    }
    if (Update.write(const_cast<uint8_t*>(data), length) != length)
    {
        fail(String("Schreibfehler Firmware: ") + Update.errorString());
        return false;
    }
    m_fwWritten += length;
    return true;
}

bool UpdateManager::firmwareEnd()
{
    if (!Update.end(true))   // prüft das Programm und setzt den Startbereich
    {
        fail(String("Firmware ungültig: ") + Update.errorString());
        return false;
    }

    // Neue Firmware startet auf Probe (Rückfall möglich)
    SettingsStore::putBool(Ns, KeyTrial, true);
    SettingsStore::putUInt(Ns, KeyBoots, 0);
    SettingsStore::putBool(Ns, KeyRollback, false);
    return true;
}

/******************************************************************************
 * TAR (USTAR) während des Hochladens entpacken
 ******************************************************************************/
void UpdateManager::tarWrite(const uint8_t* data, size_t length)
{
    while (length > 0 && m_state == State::Running)
    {
        switch (m_phase)
        {
            case TarPhase::Header:
            {
                const size_t n = std::min(length, sizeof(m_header) - m_headerFill);
                std::memcpy(m_header + m_headerFill, data, n);
                m_headerFill += n;
                data += n;
                length -= n;
                if (m_headerFill == sizeof(m_header))
                {
                    m_headerFill = 0;
                    tarHeaderComplete();
                }
                break;
            }

            case TarPhase::Data:
            {
                const size_t n = std::min(length, m_entryRemaining);
                tarEntryData(data, n);
                m_entryRemaining -= n;
                data += n;
                length -= n;
                if (m_entryRemaining == 0)
                {
                    tarEntryEnd();
                }
                break;
            }

            case TarPhase::Padding:
            {
                const size_t n = std::min(length, m_paddingRemaining);
                m_paddingRemaining -= n;
                data += n;
                length -= n;
                if (m_paddingRemaining == 0)
                {
                    m_phase = TarPhase::Header;
                }
                break;
            }

            case TarPhase::Done:
                return;   // Rest (Füllblöcke am Ende) ignorieren
        }
    }
}

void UpdateManager::tarHeaderComplete()
{
    // Ende des Archivs: Block aus lauter Nullen
    bool allZero = true;
    for (size_t i = 0; i < sizeof(m_header); ++i)
    {
        if (m_header[i] != 0) { allZero = false; break; }
    }
    if (allZero)
    {
        m_phase = TarPhase::Done;
        return;
    }

    // Name = prefix (345..499) + "/" + name (0..99), jeweils nullterminiert
    char name[101] = {};
    char prefix[156] = {};
    std::memcpy(name, m_header, 100);
    std::memcpy(prefix, m_header + 345, 155);
    String path = prefix[0] ? String(prefix) + "/" + String(name) : String(name);
    if (path.startsWith("./")) path = path.substring(2);

    const size_t size = parseOctal(m_header + 124, 12);
    const char type = static_cast<char>(m_header[156]);

    m_entryRemaining   = size;
    m_paddingRemaining = (512 - (size % 512)) % 512;
    m_target = Target::Skip;

    if (type == '5')   // Ordner
    {
        if (path.startsWith("web/") && path.length() > 4)
        {
            String dir = String(NewDir) + "/" + path.substring(4);
            if (dir.endsWith("/")) dir.remove(dir.length() - 1);
            LittleFS.mkdir(dir);
        }
    }
    else if (type == '0' || type == '\0')   // normale Datei
    {
        if (path == "firmware.bin" && m_kind == Kind::WebUi)
        {
            // Finding 08.11.2026: Ein Paket unter "Nur WebUI" würde die
            // Firmware still ignorieren -> WebUI und Firmware passen dann
            // evtl. nicht zusammen. Deshalb klar ablehnen.
            fail("Das ist ein Paket (enthält Firmware) – bitte unter „Paket“ hochladen.");
            return;
        }
        if (path == "firmware.bin" && m_kind == Kind::Package)
        {
            m_sawFirmware = true;
            if (!firmwareBegin(size)) return;
            m_target = Target::Firmware;
        }
        else if (path.startsWith("web/") && path.length() > 4)
        {
            const String rel  = path.substring(4);
            const String file = String(NewDir) + "/" + rel;
            makeParentDirs(file);
            m_file = LittleFS.open(file, "w");
            if (!m_file)
            {
                fail("Datei kann nicht angelegt werden: " + rel);
                return;
            }
            if (rel == "index.html") m_sawIndex = true;
            ++m_fileCount;
            m_target = Target::File;
        }
    }

    if (size == 0)
    {
        tarEntryEnd();
    }
    else
    {
        m_phase = TarPhase::Data;
    }
}

void UpdateManager::tarEntryData(const uint8_t* data, size_t length)
{
    switch (m_target)
    {
        case Target::File:
            if (m_file.write(data, length) != length)
            {
                fail("Dateisystem voll oder Schreibfehler");
            }
            break;
        case Target::Firmware:
            firmwareWrite(data, length);
            break;
        case Target::Skip:
            break;
    }
}

void UpdateManager::tarEntryEnd()
{
    if (m_target == Target::File && m_file)
    {
        m_file.close();
    }
    m_target = Target::Skip;
    m_phase = (m_paddingRemaining > 0) ? TarPhase::Padding : TarPhase::Header;
}

/******************************************************************************
 * Neue WebUI übernehmen: alte Dateien löschen, /_new nach / verschieben
 ******************************************************************************/
bool UpdateManager::swapWebUi()
{
    // Alles außer /_new entfernen
    for (const DirEntry& e : listDir("/"))
    {
        const String path = joinPath("/", e.name);
        if (path == NewDir) continue;
        if (e.isDir) removeRecursive(path); else LittleFS.remove(path);
    }

    const bool ok = moveTree(NewDir, "/");
    removeRecursive(NewDir);
    return ok;
}
