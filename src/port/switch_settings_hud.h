#pragma once

// Upper-right settings readout: which performance and debug settings are in
// effect, so a screenshot says whether a run is a timing run or a debug run.
// Pure string logic over a dvar lookup, so it runs on the host.
//
// Line 1: scale mode, render size and upscaler, gamma.
// Line 2: shadow settings and a 12-hex fingerprint of the effective settings.
// Line 3 (only when a cost-adding probe is on): DEBUG: <flags> -> timing invalid.
//
// The fingerprint is the one the run-profile tooling prints: sha256 over
// "map=", the scale mode and upscaler, then every fingerprint key sorted
// case-insensitively as "key=value" with numbers normalised.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Returns the displayable value of dvar `name`, or null when it is not registered.
typedef const char *(*SwHudDvarFn)(void *ctx, const char *name);

struct SwHudInput
{
    SwHudDvarFn get;
    void *ctx;
    const char *map;
    int sceneWidth;
    int sceneHeight;
};

struct SwHudText
{
    char line1[256];
    char line2[256];
    char debug[192];
    char hash[13];
    bool hasDebug;
};

namespace swhud
{
// Every dvar the fingerprint covers (written by every profile, the scale
// mapping, and the dvars left at their code default that the drift check watches).
static const char *const kFingerprintKeys[] = {
    "com_hardware", "performance", "snd_enableStream", "replay_autosave", "switch_perfTrace",
    "r_deko9GpuPasses", "r_deko9FaultTrace", "r_deko9GpuMap", "r_smp_backend", "sv_smp",
    "sm_enable", "r_shadowFilter", "r_halfResParticles", "r_renderResolution", "r_texFilterAnisoMax",
    "r_vsync", "r_fastSkin", "r_preloadShaders", "ai_corpseCount", "cg_drawFPS",
    "r_dynres", "r_renderScale", "r_dynresMin", "r_dynresMax", "r_taau", "r_fsrMode",
    "switch_pcSample", "com_maxfps", "r_fsrSharpness", "r_depthPrepass", "r_distortion", "r_aaSamples",
};
static const int kFingerprintKeyCount = (int)(sizeof(kFingerprintKeys) / sizeof(kFingerprintKeys[0]));

// A probe or experiment that adds cost: shown when its value differs from the
// code default. withValue appends the value to the name (faulttrace16).
struct DebugFlag
{
    const char *dvar;
    const char *defaultValue;
    const char *token;
    bool withValue;
};
static const DebugFlag kDebugFlags[] = {
    {"performance", "1", "performance", true},
    {"r_deko9CmdPoison", "0", "poison", false},
    {"r_deko9FaultTrace", "0", "faulttrace", true},
    {"r_deko9DrawCensus", "0", "drawcensus", true},
    {"r_deko9GpuPasses", "0", "gpupasses", false},
    {"switch_perfTrace", "0", "perftrace", false},
    {"r_deko9GpuMap", "0", "gpumap", true},
    {"r_deko9Census", "0", "census", false},
    {"fx_census", "0", "fxcensus", true},
    {"r_deko9ZcullStats", "0", "zcull", false},
    {"r_deko9Verify", "0", "verify", false},
    {"r_deko9EmissiveTour", "0", "tour", false},
    {"r_deko9SkipEmissive", "", "skipemissive", false},
    {"r_dynresFakeGpuMs", "0", "fakegpu", true},
    {"com_diagMarkers", "0", "diagmarkers", false},
    {"switch_pcSample", "0", "pcsample", true},
    {"developer", "0", "developer", true},
};

struct Sha256
{
    uint32_t h[8];
    uint8_t buf[64];
    uint64_t total;
    uint32_t fill;
};

inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void Sha256Block(Sha256 &s, const uint8_t *p)
{
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | p[i * 4 + 3];
    for (int i = 16; i < 64; ++i)
    {
        uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4], f = s.h[5], g = s.h[6], hh = s.h[7];
    for (int i = 0; i < 64; ++i)
    {
        uint32_t t1 = hh + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
        uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d; s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += hh;
}

inline void Sha256Init(Sha256 &s)
{
    static const uint32_t init[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s.h, init, sizeof(init));
    s.total = 0;
    s.fill = 0;
}

inline void Sha256Update(Sha256 &s, const char *data, size_t n)
{
    for (size_t i = 0; i < n; ++i)
    {
        s.buf[s.fill++] = (uint8_t)data[i];
        if (s.fill == 64)
        {
            Sha256Block(s, s.buf);
            s.fill = 0;
        }
    }
    s.total += n;
}

// First 6 bytes of the digest as 12 lowercase hex digits.
inline void Sha256Hex12(Sha256 &s, char *out)
{
    uint64_t bits = s.total * 8;
    uint8_t pad = 0x80;
    Sha256Update(s, (const char *)&pad, 1);
    pad = 0;
    while (s.fill != 56)
        Sha256Update(s, (const char *)&pad, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; ++i)
        len[i] = (uint8_t)(bits >> (56 - 8 * i));
    for (int i = 0; i < 8; ++i)
        Sha256Update(s, (const char *)&len[i], 1);
    for (int i = 0; i < 6; ++i)
        snprintf(out + i * 2, 3, "%02x", (s.h[i / 4] >> (24 - 8 * (i % 4))) & 0xff);
    out[12] = 0;
}

// A value as the tooling compares it: surrounding blanks dropped; a plain
// decimal number becomes an integer when whole, else %.6g.
inline void Normalize(const char *in, char *out, size_t cap)
{
    while (*in == ' ' || *in == '\t')
        ++in;
    size_t n = strlen(in);
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\t' || in[n - 1] == '\n' || in[n - 1] == '\r'))
        --n;
    char tmp[64];
    if (n == 0 || n >= sizeof(tmp))
    {
        snprintf(out, cap, "%.*s", (int)n, in);
        return;
    }
    memcpy(tmp, in, n);
    tmp[n] = 0;
    // -?(\d+\.?\d*|\.\d+)([eE][-+]?\d+)?
    const char *p = tmp;
    if (*p == '-')
        ++p;
    bool ok = false;
    if (*p >= '0' && *p <= '9')
    {
        while (*p >= '0' && *p <= '9')
            ++p;
        if (*p == '.')
            ++p;
        while (*p >= '0' && *p <= '9')
            ++p;
        ok = true;
    }
    else if (*p == '.' && p[1] >= '0' && p[1] <= '9')
    {
        ++p;
        while (*p >= '0' && *p <= '9')
            ++p;
        ok = true;
    }
    if (ok && (*p == 'e' || *p == 'E'))
    {
        const char *q = p + 1;
        if (*q == '-' || *q == '+')
            ++q;
        if (*q >= '0' && *q <= '9')
        {
            while (*q >= '0' && *q <= '9')
                ++q;
            p = q;
        }
        else
            ok = false;
    }
    if (!ok || *p)
    {
        snprintf(out, cap, "%s", tmp);
        return;
    }
    double f = strtod(tmp, nullptr);
    if (f == (double)(long long)f && (f < 1e15 && f > -1e15))
        snprintf(out, cap, "%lld", (long long)f);
    else
        snprintf(out, cap, "%.6g", f);
}

struct Value
{
    char s[64];
    bool known;
};

inline Value Get(const SwHudInput &in, const char *name)
{
    Value v;
    const char *raw = in.get ? in.get(in.ctx, name) : nullptr;
    v.known = raw != nullptr;
    if (raw)
        Normalize(raw, v.s, sizeof(v.s));
    else
        snprintf(v.s, sizeof(v.s), "?");
    return v;
}

// Scale mode ("native", "fixed S", "adaptive MIN-MAX", "?") and the upscaler
// ("taau", an r_fsrMode name, "none" for native, "?").
inline void ScaleMode(const SwHudInput &in, char *mode, size_t modeCap, char *up, size_t upCap)
{
    Value dyn = Get(in, "r_dynres"), taau = Get(in, "r_taau"), fsr = Get(in, "r_fsrMode");
    Value rs = Get(in, "r_renderScale"), mn = Get(in, "r_dynresMin"), mx = Get(in, "r_dynresMax");
    snprintf(mode, modeCap, "?");
    snprintf(up, upCap, "?");
    if (!dyn.known)
        return;
    snprintf(up, upCap, "%s", strcmp(taau.s, "1") == 0 ? "taau" : fsr.s);
    if (strcmp(dyn.s, "1") == 0)
    {
        snprintf(mode, modeCap, "adaptive %s-%s", mn.s, mx.s);
        return;
    }
    if (strcmp(dyn.s, "0") != 0)
    {
        snprintf(mode, modeCap, "native (r_dynres %s)", dyn.s);
        snprintf(up, upCap, "none");
        return;
    }
    if (!rs.known)
    {
        snprintf(up, upCap, "?");
        return;
    }
    char *end = nullptr;
    double f = strtod(rs.s, &end);
    if (end != rs.s && *end == 0 && f >= 1.0)
    {
        snprintf(mode, modeCap, "native");
        snprintf(up, upCap, "none");
    }
    else
        snprintf(mode, modeCap, "fixed %s", rs.s);
}

inline int KeyCmp(const char *a, const char *b)
{
    for (;; ++a, ++b)
    {
        int ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca += 32;
        if (cb >= 'A' && cb <= 'Z')
            cb += 32;
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (!ca)
            return 0;
    }
}

inline void Fingerprint(const SwHudInput &in, char *out13)
{
    char mode[96], up[32];
    ScaleMode(in, mode, sizeof(mode), up, sizeof(up));
    char line[160];
    Sha256 s;
    Sha256Init(s);
    snprintf(line, sizeof(line), "map=%s\n", in.map ? in.map : "?");
    Sha256Update(s, line, strlen(line));
    snprintf(line, sizeof(line), "scale=%s\nupscaler=%s\n", mode, up);
    Sha256Update(s, line, strlen(line));
    const char *order[64];
    int n = kFingerprintKeyCount;
    for (int i = 0; i < n; ++i)
        order[i] = kFingerprintKeys[i];
    for (int i = 1; i < n; ++i)
    {
        const char *k = order[i];
        int j = i - 1;
        while (j >= 0 && KeyCmp(order[j], k) > 0)
        {
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = k;
    }
    for (int i = 0; i < n; ++i)
    {
        Value v = Get(in, order[i]);
        snprintf(line, sizeof(line), "%s=%s\n", order[i], v.s);
        Sha256Update(s, line, strlen(line));
    }
    Sha256Hex12(s, out13);
}
} // namespace swhud

inline void SwHud_Format(const SwHudInput &in, SwHudText &out)
{
    using namespace swhud;
    char mode[96], up[32];
    ScaleMode(in, mode, sizeof(mode), up, sizeof(up));
    Value gamma = Get(in, "r_gamma"), sm = Get(in, "sm_enable"), filt = Get(in, "r_shadowFilter");
    char size[24] = "";
    if (in.sceneWidth > 0 && in.sceneHeight > 0 && strncmp(mode, "native", 6) != 0)
        snprintf(size, sizeof(size), " %dx%d", in.sceneWidth, in.sceneHeight);
    snprintf(out.line1, sizeof(out.line1), "scale %s%s %s  gamma %s", mode, size, up, gamma.s);
    Fingerprint(in, out.hash);
    snprintf(out.line2, sizeof(out.line2), "shadow sm%s filter%s  fp %s", sm.s, filt.s, out.hash);

    out.debug[0] = 0;
    out.hasDebug = false;
    size_t len = 0;
    for (const DebugFlag &f : kDebugFlags)
    {
        Value v = Get(in, f.dvar);
        if (!v.known)
            continue;
        char def[64];
        Normalize(f.defaultValue, def, sizeof(def));
        if (strcmp(v.s, def) == 0)
            continue;
        if (!out.hasDebug)
        {
            len = (size_t)snprintf(out.debug, sizeof(out.debug), "DEBUG:");
            out.hasDebug = true;
        }
        char tok[96];
        snprintf(tok, sizeof(tok), f.withValue ? " %s%s" : " %s", f.token, v.s);
        if (len + strlen(tok) + 20 < sizeof(out.debug))
            len += (size_t)snprintf(out.debug + len, sizeof(out.debug) - len, "%s", tok);
    }
    if (out.hasDebug)
        snprintf(out.debug + len, sizeof(out.debug) - len, " -> timing invalid");
}

struct ScreenPlacement;
// Draws the lines at the upper-right corner below y; returns the next y.
float SwSettingsHud_Draw(const ScreenPlacement *scrPlace, float y);
