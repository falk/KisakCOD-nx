// Host test for src/port/switch_settings_hud.h, the upper-right settings
// readout formatter: default settings show no DEBUG line, probes show up in a
// DEBUG line, fixed and adaptive scale read differently, and the fingerprint
// is the one the run-profile tooling computes.
//
// Without arguments it runs the unit checks. `--keys` prints the fingerprint
// key list and `--hash MAP` reads `name=value` lines from stdin and prints the
// fingerprint; the parity script compares both against the Python tooling.

#include "src/port/switch_settings_hud.h"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <string>

static int g_failures;
static int g_checks;

#define CHECK(cond)                                                         \
    do                                                                      \
    {                                                                       \
        ++g_checks;                                                         \
        if (!(cond))                                                        \
        {                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

typedef std::map<std::string, std::string> Dvars;

static const char *Lookup(void *ctx, const char *name)
{
    Dvars *d = (Dvars *)ctx;
    auto it = d->find(name);
    return it == d->end() ? nullptr : it->second.c_str();
}

// The dvars of the `fps` profile at scale 0.75 with TAAU.
static Dvars FpsProfile()
{
    return {{"com_hardware", "1"}, {"performance", "1"}, {"snd_enableStream", "1"}, {"replay_autosave", "0"},
            {"switch_perfTrace", "0"}, {"r_deko9GpuPasses", "0"}, {"r_deko9FaultTrace", "0"}, {"r_deko9GpuMap", "0"},
            {"r_smp_backend", "1"}, {"sv_smp", "0"}, {"sm_enable", "1"}, {"r_shadowFilter", "0"},
            {"r_halfResParticles", "0"}, {"r_renderResolution", "1280x720"}, {"r_texFilterAnisoMax", "4"},
            {"r_vsync", "0"}, {"r_fastSkin", "1"}, {"r_preloadShaders", "1"}, {"ai_corpseCount", "10"},
            {"cg_drawFPS", "Simple"}, {"r_dynres", "0"}, {"r_renderScale", "0.75"}, {"r_dynresMin", "0.5"},
            {"r_dynresMax", "1"}, {"r_taau", "1"}, {"r_fsrMode", "sgsr"}, {"switch_pcSample", "0"},
            {"com_maxfps", "0"}, {"r_fsrSharpness", "0.2"}, {"r_depthPrepass", "0"}, {"r_distortion", "1"},
            {"r_aaSamples", "1"}, {"r_gamma", "0.80000001"}, {"r_deko9CmdPoison", "0"}, {"r_deko9DrawCensus", "0"},
            {"r_deko9Census", "0"}, {"fx_census", "0"}, {"developer", "0"}, {"com_diagMarkers", "0"},
            {"r_deko9SkipEmissive", ""}};
}

static SwHudText Format(Dvars &d, const char *map, int w, int h)
{
    SwHudInput in = {Lookup, &d, map, w, h};
    SwHudText t;
    SwHud_Format(in, t);
    return t;
}

static int RunChecks()
{
    {
        Dvars d = FpsProfile();
        SwHudText t = Format(d, "cargoship", 960, 540);
        CHECK(!t.hasDebug);
        CHECK(strcmp(t.line1, "scale fixed 0.75 960x540 taau  gamma 0.8") == 0);
        CHECK(strncmp(t.line2, "shadow sm1 filter0  fp ", 23) == 0);
        CHECK(strlen(t.hash) == 12);
        printf("%s\n%s\n", t.line1, t.line2);
    }
    {
        // fault-hunt: adaptive scale, poison and a fault trace of 16.
        Dvars d = FpsProfile();
        d["r_dynres"] = "1";
        d["r_renderScale"] = "1";
        d["r_deko9CmdPoison"] = "1";
        d["r_deko9FaultTrace"] = "16";
        SwHudText t = Format(d, "cargoship", 1024, 576);
        CHECK(t.hasDebug);
        CHECK(strcmp(t.debug, "DEBUG: poison faulttrace16 -> timing invalid") == 0);
        // Golden from run_profiles.py: fault-hunt on cargoship.
        CHECK(strcmp(t.hash, "027480c63a1f") == 0);
        CHECK(strcmp(t.line1, "scale adaptive 0.5-1 1024x576 taau  gamma 0.8") == 0);
        printf("%s\n%s\n%s\n", t.line1, t.line2, t.debug);
    }
    {
        Dvars d = FpsProfile();
        d["r_dynres"] = "0";
        d["r_renderScale"] = "1";
        SwHudText t = Format(d, "cargoship", 1280, 720);
        CHECK(strcmp(t.line1, "scale native taau  gamma 0.8") != 0);  // native has no upscaler
        CHECK(strcmp(t.line1, "scale native none  gamma 0.8") == 0);
    }
    {
        Dvars d = FpsProfile();
        d["r_taau"] = "0";
        d["r_fsrMode"] = "bilinear";
        d["performance"] = "0";
        d["r_deko9GpuPasses"] = "1";
        d["switch_perfTrace"] = "1";
        d["r_deko9Census"] = "1";
        d["r_gamma"] = "1.2";
        d["sm_enable"] = "0";
        d["r_shadowFilter"] = "1";
        SwHudText t = Format(d, "cargoship", 960, 540);
        CHECK(strstr(t.line1, "bilinear") != nullptr);
        CHECK(strstr(t.line1, "gamma 1.2") != nullptr);
        CHECK(strstr(t.line2, "shadow sm0 filter1") != nullptr);
        CHECK(strcmp(t.debug, "DEBUG: performance0 gpupasses perftrace census -> timing invalid") == 0);
    }
    {
        // The fingerprint follows the map and the effective values.
        Dvars d = FpsProfile();
        SwHudText a = Format(d, "cargoship", 960, 540), b = Format(d, "killhouse", 960, 540);
        CHECK(strcmp(a.hash, b.hash) != 0);
        d["r_shadowFilter"] = "1";
        CHECK(strcmp(Format(d, "cargoship", 960, 540).hash, a.hash) != 0);
        // A key the fingerprint does not cover leaves it alone.
        Dvars e = FpsProfile();
        e["r_gamma"] = "2";
        CHECK(strcmp(Format(e, "cargoship", 960, 540).hash, a.hash) == 0);
    }
    {
        char out[32];
        swhud::Normalize("0.750000", out, sizeof(out));
        CHECK(strcmp(out, "0.75") == 0);
        swhud::Normalize("1.0", out, sizeof(out));
        CHECK(strcmp(out, "1") == 0);
        swhud::Normalize(" Simple ", out, sizeof(out));
        CHECK(strcmp(out, "Simple") == 0);
        swhud::Normalize("1280x720", out, sizeof(out));
        CHECK(strcmp(out, "1280x720") == 0);
        swhud::Normalize("0.2", out, sizeof(out));
        CHECK(strcmp(out, "0.2") == 0);
    }
    printf("settings_hud: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && !strcmp(argv[1], "--keys"))
    {
        for (int i = 0; i < swhud::kFingerprintKeyCount; ++i)
            printf("%s\n", swhud::kFingerprintKeys[i]);
        return 0;
    }
    if (argc >= 3 && !strcmp(argv[1], "--hash"))
    {
        Dvars d;
        std::string line;
        while (std::getline(std::cin, line))
        {
            size_t eq = line.find('=');
            if (eq != std::string::npos)
                d[line.substr(0, eq)] = line.substr(eq + 1);
        }
        SwHudInput in = {Lookup, &d, argv[2], 0, 0};
        char hash[13];
        swhud::Fingerprint(in, hash);
        printf("%s\n", hash);
        return 0;
    }
    return RunChecks();
}
