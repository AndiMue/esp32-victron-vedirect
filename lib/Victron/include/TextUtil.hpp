/******************************************************************************
 * @file TextUtil.hpp
 *
 * Kleine Text-Hilfsfunktionen, die von mehreren Klassen genutzt werden.
 ******************************************************************************/

#pragma once

namespace TextUtil
{

/******************************************************************************
 * Vergleich zweier nullterminierter Texte ohne Beachtung der
 * Groß-/Kleinschreibung (nur ASCII). Beispiele:
 *   "0xa057" == "0xA057"   (PID)
 *   "On"     == "ON"       (alte BMV senden "On"/"Off")
 ******************************************************************************/
inline bool equalsIgnoreCase(const char* a, const char* b)
{
    if (a == nullptr || b == nullptr)
    {
        return false;
    }

    for (; *a != '\0' && *b != '\0'; ++a, ++b)
    {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 'a' + 'A');
        if (ca != cb)
        {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

} // namespace TextUtil
