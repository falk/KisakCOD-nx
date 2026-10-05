#ifndef SWITCH_TEXT_INPUT_H
#define SWITCH_TEXT_INPUT_H

// Text-entry helpers for the software keyboard path: turning what the player
// typed into a value a menu field accepts, and picking a default profile name
// when no keyboard can be shown. Pure; the applet call is in
// switch_ui_menus.cpp.

#include <stddef.h>
#include <string.h>
#include <stdio.h>

static inline bool SwitchTextInput_IsFilenameChar(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '-';
}

// Copies the characters a profile directory name may contain (spaces become
// '_', everything else is dropped) up to maxChars. Returns the new length.
static inline int SwitchTextInput_SanitizeFilename(const char *in, char *out, size_t outSize, int maxChars)
{
    size_t n = 0;
    if (!outSize)
        return 0;
    for (const char *p = in; p && *p && n + 1 < outSize && (int)n < maxChars; ++p)
    {
        unsigned char c = (unsigned char)*p;
        if (c == ' ')
            c = '_';
        if (SwitchTextInput_IsFilenameChar(c))
            out[n++] = (char)c;
    }
    out[n] = 0;
    return (int)n;
}

// "Player", "Player2", ... the first not already in `existing` (compared
// without case), clipped to maxChars.
static inline void SwitchTextInput_UniqueName(const char *base, const char *const *existing, int count,
                                              char *out, size_t outSize, int maxChars)
{
    for (int suffix = 1; suffix < 1000; ++suffix)
    {
        char digits[8] = "";
        if (suffix > 1)
            snprintf(digits, sizeof(digits), "%d", suffix);
        const size_t digitLen = strlen(digits);
        char candidate[64];
        size_t keep = strlen(base);
        if ((int)(keep + digitLen) > maxChars)
            keep = maxChars > (int)digitLen ? (size_t)maxChars - digitLen : 0;
        snprintf(candidate, sizeof(candidate), "%.*s%s", (int)keep, base, digits);

        bool taken = false;
        for (int i = 0; i < count && !taken; ++i)
        {
            const char *a = candidate;
            const char *b = existing[i];
            while (*a && *b && ((*a | 0x20) == (*b | 0x20)))
                ++a, ++b;
            taken = !*a && !*b;
        }
        if (!taken)
        {
            snprintf(out, outSize, "%s", candidate);
            return;
        }
    }
    snprintf(out, outSize, "%s", base);
}

#endif
