#pragma once

// ASCII case-insensitive compares behind I_stricmp / I_strnicmp
// (q_shared.cpp). Header-only so switch_hot_engine2_test.cpp can check them
// against the retail loop over randomized inputs.
//
// Same results as the retail I_strnicmp: bytes compare as unsigned, only
// 'A'..'Z' fold (no locale, no table), and the sign is -1 / +1 of the first
// folded difference. I_stricmp no longer goes through the counted loop,
// a high-call-count hot path in a PGO profile, and equal bytes skip the
// fold entirely.

namespace qstr
{

inline int FoldAscii(int c)
{
    return (unsigned)(c - 'A') < 26u ? c + ('a' - 'A') : c;
}

inline int Icmp(const char *s0, const char *s1)
{
    const unsigned char *a = (const unsigned char *)s0;
    const unsigned char *b = (const unsigned char *)s1;
    for (;;)
    {
        int c0 = *a++;
        int c1 = *b++;
        if (c0 == c1)
        {
            if (!c0)
                return 0;
            continue;
        }
        // Different bytes that fold equal are two letters, never the end.
        c0 = FoldAscii(c0);
        c1 = FoldAscii(c1);
        if (c0 != c1)
            return c0 < c1 ? -1 : 1;
    }
}

// n counts compared characters like the retail `if (!n--) return 0`; a
// negative n is unbounded in practice there too (it would take ~2^31 equal
// characters to reach zero), which the unsigned counter keeps.
inline int Nicmp(const char *s0, const char *s1, int n)
{
    const unsigned char *a = (const unsigned char *)s0;
    const unsigned char *b = (const unsigned char *)s1;
    unsigned remaining = (unsigned)n;
    for (;;)
    {
        if (!remaining--)
            return 0;
        int c0 = *a++;
        int c1 = *b++;
        if (c0 == c1)
        {
            if (!c0)
                return 0;
            continue;
        }
        c0 = FoldAscii(c0);
        c1 = FoldAscii(c1);
        if (c0 != c1)
            return c0 < c1 ? -1 : 1;
    }
}

} // namespace qstr
