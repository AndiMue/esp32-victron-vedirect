/******************************************************************************
 * @file SettingsStore.cpp
 ******************************************************************************/

#include "SettingsStore.hpp"

#if defined(ESP32)

#include <Preferences.h>

namespace
{

/// Öffnet einen Namensraum für die Dauer eines Zugriffs (RAII).
class Namespace
{
public:
    Namespace(const char* ns, bool readOnly) { m_ok = m_prefs.begin(ns, readOnly); }
    ~Namespace() { if (m_ok) m_prefs.end(); }

    bool ok() const          { return m_ok; }
    Preferences& prefs()     { return m_prefs; }

    /// Nur lesen, wenn der Schlüssel existiert (sonst Fehlermeldung der
    /// Bibliothek auf Serial vermeiden).
    bool has(const char* key) { return m_ok && m_prefs.isKey(key); }

private:
    Preferences m_prefs;
    bool        m_ok = false;
};

} // namespace

String SettingsStore::getString(const char* ns, const char* key, const char* defaultValue)
{
    Namespace n(ns, true);
    return n.has(key) ? n.prefs().getString(key, defaultValue) : String(defaultValue);
}

bool SettingsStore::putString(const char* ns, const char* key, const String& value)
{
    Namespace n(ns, false);
    return n.ok() && n.prefs().putString(key, value) == value.length();
}

bool SettingsStore::getBool(const char* ns, const char* key, bool defaultValue)
{
    Namespace n(ns, true);
    return n.has(key) ? n.prefs().getBool(key, defaultValue) : defaultValue;
}

bool SettingsStore::putBool(const char* ns, const char* key, bool value)
{
    Namespace n(ns, false);
    return n.ok() && n.prefs().putBool(key, value) > 0;
}

uint32_t SettingsStore::getUInt(const char* ns, const char* key, uint32_t defaultValue)
{
    Namespace n(ns, true);
    return n.has(key) ? n.prefs().getUInt(key, defaultValue) : defaultValue;
}

bool SettingsStore::putUInt(const char* ns, const char* key, uint32_t value)
{
    Namespace n(ns, false);
    return n.ok() && n.prefs().putUInt(key, value) > 0;
}

bool SettingsStore::remove(const char* ns, const char* key)
{
    Namespace n(ns, false);
    return n.ok() && (!n.prefs().isKey(key) || n.prefs().remove(key));
}

bool SettingsStore::clear(const char* ns)
{
    Namespace n(ns, false);
    return n.ok() && n.prefs().clear();
}

#else
#error "SettingsStore: ESP8266-Variante folgt in einem eigenen Schritt (siehe Claude.md)"
#endif
