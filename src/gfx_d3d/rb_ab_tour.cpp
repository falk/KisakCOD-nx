// In-engine A/B measurement: the fixed-viewpoint tour, the emissive
// material skip and the draw census glue (see rb_ab_tour.h).

#include "rb_ab_tour.h"


#include <deko9/deko9_native.h>
#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <qcommon/cmd.h>
#include <universal/com_files.h>

#include "r_dvars.h"
#include "r_dynres.h"
#include "r_init.h"
#include "rb_backend.h"
#include "r_material.h"
#include "r_scene.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// fx_draw.cpp: sprite screen-area census (fx_drawStats), reported per phase.
void FX_DrawStatsReset();
void FX_DrawStatsReport(const char *label);

// server/sv_public.h and client/client.h need the whole game headers.
bool __cdecl SV_Loaded();
bool __cdecl CL_IsLocalClientInGame(int32_t localClientNum);


bool g_emissiveSkipOn;
uint32_t g_emissiveSkipped;
bool g_drawCensusOn;

namespace
{
// Substrings of r_deko9SkipEmissive, re-parsed when the string changes.
std::string s_skipSource;
std::vector<std::string> s_skipTokens;
bool s_skipAll;
uint32_t s_autoFrames;
bool s_tourActive; // the tour owns census reports
// r_deko9DrawCensusPasses, parsed into a gpupass bit mask when it changes.
std::string s_passSource = "\x01";
uint32_t s_passMask;

std::vector<std::string> Split(const std::string &text, char sep)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size())
    {
        const size_t end = text.find(sep, start);
        const std::string part = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty())
            out.push_back(part);
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return out;
}

void ParseSkip(const char *text)
{
    if (s_skipSource == text)
        return;
    s_skipSource = text;
    s_skipTokens = Split(s_skipSource, ',');
    s_skipAll = false;
    for (const std::string &t : s_skipTokens)
        s_skipAll |= t == "*";
}

uint32_t ParsePasses(const char *text)
{
    if (s_passSource == text)
        return s_passMask;
    s_passSource = text;
    s_passMask = 0;
    for (const std::string &t : Split(s_passSource, ','))
    {
        uint32_t p = 0;
        for (; p < Deko9GpuPass_Count; ++p)
        {
            if (t == Deko9_GpuPassName(p))
                break;
        }
        if (p == Deko9GpuPass_Count)
        {
            // Unsupported input fails loudly; the census then measures all passes.
            Com_Printf(CON_CHANNEL_SYSTEM, "DRAW_CENSUS_FAIL unknown pass \"%s\" in r_deko9DrawCensusPasses\n",
                       t.c_str());
            continue;
        }
        s_passMask |= 1u << p;
    }
    if (!s_passMask)
        s_passMask = (1u << Deko9GpuPass_Count) - 1;
    return s_passMask;
}

void SceneSize(uint32_t *w, uint32_t *h)
{
    *w = gfxRenderTargets[R_RENDERTARGET_SCENE].width;
    *h = gfxRenderTargets[R_RENDERTARGET_SCENE].height;
}
} // namespace

bool RB_EmissiveListActive(const GfxDrawSurfListInfo *info)
{
    return info->viewInfo && info == &info->viewInfo->emissiveInfo;
}

bool RB_EmissiveSkipMaterial(const Material *material)
{
    if (s_skipAll)
        return true;
    for (const std::string &t : s_skipTokens)
    {
        if (t[0] == '=' ? !strcmp(material->info.name, t.c_str() + 1) : strstr(material->info.name, t.c_str()) != nullptr)
            return true;
    }
    return false;
}

void RB_DrawCensusLabel(const Material *material, const MaterialTechnique *technique, const MaterialPixelShader *ps,
                        const MaterialVertexShader *vs)
{
    Deko9_CensusLabel(dx.device, material ? material->info.name : nullptr, technique ? technique->name : nullptr,
                      ps ? ps->name : nullptr, vs ? vs->name : nullptr, ps ? (const void *)ps->prog.ps : nullptr);
}

void RB_DrawCensusLight(uint32_t lightIndex, uint32_t viewLights)
{
    Deko9_CensusLight(dx.device, lightIndex, viewLights);
}

void RB_AbTourBackendFrame()
{
    // Registered here, not with the renderer's dvars: an A/B knob of the black box.
    static const dvar_t *cmdChunkKB = Dvar_RegisterInt(
        "r_deko9CmdChunkKB", 0, DvarLimits(0, 4096), DVAR_NOFLAG,
        "deko3d renderer: command-memory chunk size in KiB (0 = 256). Every chunk switch starts a new GPFIFO entry: "
        "1-4 puts dozens into each list, 4096 keeps whole lists in one chunk. Diagnostics");
    Deko9_SetCmdChunkBytes(dx.device, (uint32_t)cmdChunkKB->current.integer << 10);
    static const dvar_t *cmdPoison = Dvar_RegisterBool(
        "r_deko9CmdPoison", false, DVAR_NOFLAG,
        "deko3d renderer: fill freed command memory and the words after each command chunk's end with words the "
        "GPU rejects that name the chunk and offset (a GPU pushbuffer fault then says whose memory it read), and "
        "keep freed chunks out of reuse for two lists. Diagnostics; costs CPU writes per list");
    Deko9_SetCmdPoison(dx.device, cmdPoison->current.enabled);
    static const dvar_t *cmdOwnerCheck = Dvar_RegisterBool(
        "r_deko9CmdOwnerCheck", true, DVAR_NOFLAG,
        "deko3d renderer: report (once, FAIL:DEKO9_CMD_THREAD) a command recorded by a thread that does not hold the "
        "device lock or into another thread's bake capture");
    Deko9_SetCmdOwnerCheck(dx.device, cmdOwnerCheck->current.enabled);
    const int mode = r_deko9DrawCensus ? r_deko9DrawCensus->current.integer : 0;
    Deko9_SetDrawCensus(dx.device, (uint32_t)mode, mode ? ParsePasses(r_deko9DrawCensusPasses->current.string) : 0u);
    g_drawCensusOn = mode != 0;
    ParseSkip(r_deko9SkipEmissive ? r_deko9SkipEmissive->current.string : "");
    g_emissiveSkipOn = !s_skipTokens.empty();
    if (!mode || s_tourActive)
    {
        s_autoFrames = 0;
        return;
    }
    const uint32_t period = (uint32_t)r_deko9DrawCensusFrames->current.integer;
    if (++s_autoFrames >= period)
    {
        uint32_t w, h;
        SceneSize(&w, &h);
        Deko9_CensusReport(dx.device, "auto", w, h);
        s_autoFrames = 0;
    }
}

// ---- tour ---------------------------------------------------------------------

namespace
{
struct TourSpot
{
    float v[6]; // x y z yaw pitch, turn (degrees per second of +left, negative = +right; 0 = still)
    bool here;  // "here": stay where the player is (no teleport)
};

struct TourGroup
{
    std::string name;
    std::string skip;               // r_deko9SkipEmissive value for the phase
    std::vector<std::string> dvars; // booleans set to 0 for the phase
    // "@name=value": any dvar set to value for the phase, restored after
    // (e.g. @r_texFilterAnisoMax=1 for a texture-filtering A/B).
    std::vector<std::pair<std::string, std::string>> sets;
    std::vector<std::string> saved; // values before the phase, parallel to sets
};

struct TourPhase
{
    uint32_t spot;
    int group; // -1: baseline, -2: census (baseline with r_deko9DrawCensus on), -3: shots
};

enum TourState
{
    TOUR_IDLE,
    TOUR_WAIT_MAP,
    TOUR_DELAY,
    TOUR_SETTLE,
    TOUR_MEASURE,
    TOUR_SHOTS,
    TOUR_DONE,
};

struct Tour
{
    TourState state = TOUR_IDLE;
    std::vector<TourSpot> spots;
    std::vector<TourGroup> groups;
    std::vector<TourPhase> phases;
    float delay = 40, settle = 3, measure = 8;
    int drawCensusMode = 0; // r_deko9DrawCensus at setup; on only in census phases
    uint32_t phase = 0;
    uint32_t stateStartMs = 0;
    uint32_t measureFrames = 0;
    uint64_t ns0[Deko9GpuPass_Count]{};
    uint64_t frames0 = 0;
    uint32_t skipped0 = 0;
    uint64_t draws0 = 0, drawCpuNs0 = 0; // Deko9Counters draw totals at BeginMeasure
    bool measurePaused = false;          // r_deko9EmissiveTourPaused: "pause" issued at BeginMeasure
    // r_deko9EmissiveTourShots at setup ("" = no shot phases): comma-separated
    // boolean dvars, toggled together.
    std::string shotsDvar;
    std::vector<std::string> shotsDvars;
    std::vector<std::string> shotsOn, shotsOff; // per dvar: the "on" and "off" values
    std::vector<std::string> shotsRestore;      // per dvar: the value at setup, set again after the shots
    // name=v1|v2|...: one shot per value instead of the on/off A/B (e.g. the
    // r_renderScale sweep); per dvar the value list (one entry = its on
    // value at every step), and the step script built at setup.
    std::vector<std::vector<std::string>> shotsValues;
    std::vector<std::string> shotSteps;
    uint32_t shotFrames = 0; // frames since the shot phase's current step began
    uint32_t shotStep = 0;
    bool shotPending = false; // RB_RequestScreenshot issued, not yet written
} s_tour;

bool ParseSpots(const char *text, std::vector<TourSpot> *out)
{
    out->clear();
    for (const std::string &spot : Split(text, '/'))
    {
        if (spot == "here")
        {
            TourSpot s{};
            s.here = true;
            out->push_back(s);
            continue;
        }
        const std::vector<std::string> f = Split(spot, ',');
        if (f.size() < 3 || f.size() > 6)
            return false;
        TourSpot s{};
        for (size_t i = 0; i < f.size(); ++i)
            s.v[i] = (float)atof(f[i].c_str());
        out->push_back(s);
    }
    return !out->empty();
}

bool ParseGroups(const char *text, std::vector<TourGroup> *out)
{
    out->clear();
    // "-": no skip groups (a census-only tour: census + baseline per spot;
    // the command line cannot carry an empty string).
    if (!strcmp(text, "-"))
        return true;
    for (const std::string &group : Split(text, '/'))
    {
        const size_t colon = group.find(':');
        if (colon == std::string::npos || !colon)
            return false;
        TourGroup g;
        g.name = group.substr(0, colon);
        std::string skip;
        for (const std::string &item : Split(group.substr(colon + 1), ','))
        {
            const size_t eq = item.find('=');
            if (item[0] == '@' && eq != std::string::npos && eq > 1)
                g.sets.emplace_back(item.substr(1, eq - 1), item.substr(eq + 1));
            else if (item[0] == '@')
                g.dvars.push_back(item.substr(1));
            else
                skip += (skip.empty() ? "" : ",") + item;
        }
        if (skip.empty() && g.dvars.empty() && g.sets.empty())
            return false;
        g.saved.resize(g.sets.size());
        g.skip = skip;
        out->push_back(g);
    }
    return true;
}

// The census phase, plus every A/B group phase under r_deko9DrawCensusGroups.
bool PhaseHasCensus(const TourPhase &p)
{
    return p.group == -2 || (p.group >= 0 && r_deko9DrawCensusGroups->current.enabled);
}

const char *PhaseGroupName(const TourPhase &p)
{
    if (p.group == -2)
        return "census";
    if (p.group == -3)
        return "shots";
    return p.group < 0 ? "none" : s_tour.groups[p.group].name.c_str();
}

void ApplyGroup(int group, bool on)
{
    if (group < 0)
        return;
    TourGroup &g = s_tour.groups[group];
    Dvar_SetStringByName("r_deko9SkipEmissive", on ? g.skip.c_str() : "");
    for (size_t i = 0; i < g.sets.size(); ++i)
    {
        const char *name = g.sets[i].first.c_str();
        if (!Dvar_FindVar(name))
        {
            Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_FAIL unknown dvar %s in group %s\n", name, g.name.c_str());
            continue;
        }
        if (on)
            g.saved[i] = Dvar_GetVariantString(name);
        Dvar_SetFromStringByName(name, on ? g.sets[i].second.c_str() : g.saved[i].c_str());
    }
    for (const std::string &d : g.dvars)
    {
        if (Dvar_FindVar(d.c_str()))
            Dvar_SetBoolByName(d.c_str(), !on);
        else
            Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_FAIL unknown dvar %s in group %s\n", d.c_str(), g.name.c_str());
    }
}

// A spot's turn: the view keeps turning (the +left/+right keys at
// cl_yawspeed) through its phases, so the A/B sees camera motion; the
// teleport at each phase start brings it back to the spot's yaw.
void SetTurn(float degreesPerSecond)
{
    Cbuf_AddText(0, "-left\n-right\n");
    if (degreesPerSecond != 0.0f)
        Cbuf_AddText(0, va("set cl_yawspeed %g\n%s\n", std::fabs(degreesPerSecond),
                           degreesPerSecond > 0.0f ? "+left" : "+right"));
}

// The dynamic-resolution controller would change the scene size with the very
// load an A/B changes and mask it, so a tour never leaves it running: every
// phase, baselines included, renders at r_renderScale (set to r_dynresMax
// when the controller is held).
void PinDynResScale()
{
    if (R_RenderScaleMode() != render_scale::Mode::Adaptive)
        return;
    Dvar_SetFloatByName("r_renderScale", r_dynresMax ? r_dynresMax->current.value : 1.0f);
    R_DynResHoldController();
    Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_PIN r_renderScale=%g\n", r_renderScale->current.value);
}

void StartPhase(uint32_t now)
{
    const TourPhase &p = s_tour.phases[s_tour.phase];
    const TourSpot &s = s_tour.spots[p.spot];
    ApplyGroup(p.group, true);
    PinDynResScale();
    if (!s_tour.phase || s_tour.phases[s_tour.phase - 1].spot != p.spot)
        SetTurn(s.v[5]);
    // The census serializes every bracket (timestamps), so it runs in its
    // own phase and never inflates the A/B timings -- unless
    // r_deko9DrawCensusGroups asks for draw counts per group phase.
    Dvar_SetIntByName("r_deko9DrawCensus", PhaseHasCensus(p) ? s_tour.drawCensusMode : 0);
    // Still paused at this spot from the previous phase: same frame, no teleport.
    if (!s.here && !s_tour.measurePaused)
        Cbuf_AddText(0, va("setviewpos %g %g %g %g %g\n", s.v[0], s.v[1], s.v[2], s.v[3], s.v[4]));
    Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_PHASE_BEGIN phase=%u/%u spot=%u org=%g,%g,%g,%g,%g turn=%g skip=%s\n",
               s_tour.phase + 1, (unsigned)s_tour.phases.size(), p.spot + 1, s.v[0], s.v[1], s.v[2], s.v[3], s.v[4],
               s.v[5], PhaseGroupName(p));
    s_tour.state = TOUR_SETTLE;
    s_tour.stateStartMs = now;
}

void BeginMeasure(uint32_t now)
{
    Deko9_GetGpuPassTotals(dx.device, s_tour.ns0, &s_tour.frames0);
    s_tour.skipped0 = g_emissiveSkipped;
    if (r_deko9EmissiveTourPaused && r_deko9EmissiveTourPaused->current.enabled && !s_tour.measurePaused)
    {
        Cbuf_AddText(0, "pause\n");
        s_tour.measurePaused = true;
    }
    Deko9Counters counters{};
    Deko9_GetCounters(dx.device, &counters);
    s_tour.draws0 = counters.drawsTotal;
    s_tour.drawCpuNs0 = counters.drawCpuNsTotal;
    // Drops what the census gathered while settling.
    Deko9_CensusReport(dx.device, nullptr, 0, 0);
    FX_DrawStatsReset();
    s_tour.measureFrames = 0;
    s_tour.state = TOUR_MEASURE;
    s_tour.stateStartMs = now;
}

void EndMeasure(uint32_t now)
{
    // Unpause only for a teleport to another spot: the phases of one spot
    // (census, baselines, every A/B group) then measure the very same paused
    // frame, so their draw counts and pixels compare exactly; a level whose
    // script keeps moving things otherwise drifts between phases by more
    // than the A/B.
    const bool nextSameSpot = s_tour.phase + 1 < s_tour.phases.size()
        && s_tour.phases[s_tour.phase + 1].spot == s_tour.phases[s_tour.phase].spot;
    if (s_tour.measurePaused && !nextSameSpot)
    {
        Cbuf_AddText(0, "pause\n"); // unpause for the next phase's teleport/settle
        s_tour.measurePaused = false;
    }
    uint64_t ns[Deko9GpuPass_Count], frames;
    Deko9_GetGpuPassTotals(dx.device, ns, &frames);
    const TourPhase &p = s_tour.phases[s_tour.phase];
    const double gpuFrames = frames > s_tour.frames0 ? (double)(frames - s_tour.frames0) : 1.0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < Deko9GpuPass_Count; ++i)
        total += ns[i] - s_tour.ns0[i];
    const double secs = (now - s_tour.stateStartMs) / 1000.0;
    char label[96];
    snprintf(label, sizeof(label), "spot%u/%s", p.spot + 1, PhaseGroupName(p));
    Com_Printf(CON_CHANNEL_SYSTEM,
               "EMISSIVE_PHASE phase=%u spot=%u skip=%s scale=%g frames=%llu fps=%.1f gpu=%.2fms emissive=%.2fms lit=%.2fms "
               "floatz=%.2fms postfx=%.2fms skipped_lists=%.1f (GPU ms per frame)\n",
               s_tour.phase + 1, p.spot + 1, PhaseGroupName(p), r_renderScale->current.value,
               (unsigned long long)(frames - s_tour.frames0), secs > 0 ? s_tour.measureFrames / secs : 0.0, total / 1e6 / gpuFrames,
               (ns[Deko9GpuPass_Emissive] - s_tour.ns0[Deko9GpuPass_Emissive]) / 1e6 / gpuFrames,
               (ns[Deko9GpuPass_Lit] - s_tour.ns0[Deko9GpuPass_Lit]) / 1e6 / gpuFrames,
               (ns[Deko9GpuPass_FloatZ] - s_tour.ns0[Deko9GpuPass_FloatZ]) / 1e6 / gpuFrames,
               (ns[Deko9GpuPass_PostFx] - s_tour.ns0[Deko9GpuPass_PostFx]) / 1e6 / gpuFrames,
               s_tour.measureFrames ? (double)(g_emissiveSkipped - s_tour.skipped0) / s_tour.measureFrames : 0.0);
    // Per-draw CPU cost of the phase: time inside deko9's Draw* entry points
    // over the draws recorded (A/B of per-draw fast-path dvars via @dvar groups).
    Deko9Counters counters{};
    Deko9_GetCounters(dx.device, &counters);
    const uint64_t draws = counters.drawsTotal - s_tour.draws0;
    const uint64_t drawNs = counters.drawCpuNsTotal - s_tour.drawCpuNs0;
    Com_Printf(CON_CHANNEL_SYSTEM,
               "PERDRAW_PHASE phase=%u spot=%u skip=%s draws/frame=%.0f drawCpu=%.3fms/frame ns/draw=%.0f\n",
               s_tour.phase + 1, p.spot + 1, PhaseGroupName(p),
               s_tour.measureFrames ? (double)draws / s_tour.measureFrames : 0.0,
               s_tour.measureFrames ? drawNs / 1e6 / s_tour.measureFrames : 0.0, draws ? (double)drawNs / draws : 0.0);
    {
        // Every pass (offline tooling joins the baselines with the
        // census phase); a separate line keeps the one above stable.
        char line[512];
        int len = snprintf(line, sizeof(line), "EMISSIVE_PHASE_PASSES phase=%u spot=%u skip=%s", s_tour.phase + 1,
                           p.spot + 1, PhaseGroupName(p));
        for (uint32_t i = 0; i < Deko9GpuPass_Count && len > 0 && len < (int)sizeof(line); ++i)
            len += snprintf(line + len, sizeof(line) - len, " %s=%.3f", Deko9_GpuPassName(i),
                            (ns[i] - s_tour.ns0[i]) / 1e6 / gpuFrames);
        Com_Printf(CON_CHANNEL_SYSTEM, "%s (GPU ms per frame)\n", line);
    }
    if (PhaseHasCensus(p))
    {
        uint32_t w, h;
        SceneSize(&w, &h);
        Deko9_CensusReport(dx.device, label, w, h);
    }
    FX_DrawStatsReport(label); // no-op unless fx_drawStats gathered frames
    ApplyGroup(p.group, false);
    // The controller stays held once the group's values are undone, before
    // the next phase's frames, not just at its start.
    PinDynResScale();
}

bool TourSetup()
{
    if (!ParseSpots(r_deko9EmissiveTourSpots->current.string, &s_tour.spots))
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_FAIL bad r_deko9EmissiveTourSpots \"%s\"\n",
                   r_deko9EmissiveTourSpots->current.string);
        return false;
    }
    if (!ParseGroups(r_deko9EmissiveTourGroups->current.string, &s_tour.groups))
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_FAIL bad r_deko9EmissiveTourGroups \"%s\"\n",
                   r_deko9EmissiveTourGroups->current.string);
        return false;
    }
    const std::vector<std::string> t = Split(r_deko9EmissiveTourTimes->current.string, ',');
    if (t.size() != 3)
    {
        Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_FAIL bad r_deko9EmissiveTourTimes \"%s\"\n",
                   r_deko9EmissiveTourTimes->current.string);
        return false;
    }
    s_tour.delay = (float)atof(t[0].c_str());
    s_tour.settle = (float)atof(t[1].c_str());
    s_tour.measure = (float)atof(t[2].c_str());
    s_tour.phases.clear();
    s_tour.drawCensusMode = r_deko9DrawCensus->current.integer;
    s_tour.shotsDvar = r_deko9EmissiveTourShots->current.string;
    s_tour.shotsDvars.clear();
    s_tour.shotsOn.clear();
    s_tour.shotsOff.clear();
    s_tour.shotsRestore.clear();
    s_tour.shotsValues.clear();
    size_t sweep = 0; // values in a name=v1|v2|... sweep (0 = on/off A/B)
    if (!s_tour.shotsDvar.empty())
        s_tour.shotsDvars = Split(s_tour.shotsDvar.c_str(), ',');
    for (std::string &d : s_tour.shotsDvars)
    {
        // name (a bool: on = 1, off = 0) or name=value (any type: on =
        // value, off = the value at setup).
        std::string on = "1", off = "0";
        const size_t eq = d.find('=');
        if (eq != std::string::npos)
        {
            on = d.substr(eq + 1);
            d = d.substr(0, eq);
        }
        if (!Dvar_FindVar(d.c_str()))
        {
            Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_FAIL unknown r_deko9EmissiveTourShots dvar %s\n", d.c_str());
            return false;
        }
        if (eq != std::string::npos)
            off = Dvar_GetVariantString(d.c_str());
        s_tour.shotsOn.push_back(on);
        s_tour.shotsOff.push_back(off);
        s_tour.shotsRestore.push_back(Dvar_GetVariantString(d.c_str()));
        std::vector<std::string> values = Split(on, '|');
        if (values.empty())
            values.push_back(on);
        if (values.size() > 1)
        {
            if (sweep && values.size() != sweep)
            {
                Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_FAIL r_deko9EmissiveTourShots sweeps of different "
                           "lengths (%zu vs %zu values)\n", sweep, values.size());
                return false;
            }
            sweep = values.size();
        }
        s_tour.shotsValues.push_back(values);
    }
    // Step script: "=1"/"=0" set every dvar on/off, "=v<i>" sets sweep
    // value i, anything else is a screenshot label.
    s_tour.shotSteps.clear();
    if (!sweep)
    {
        s_tour.shotSteps = {"=1", "on_a", "on_b", "=0", "off", "=1", "on_c"};
    }
    else
    {
        // One shot per value, then the first value again: the frozen frame
        // must reproduce (same check as on_a vs on_c).
        for (size_t i = 0; i <= sweep; ++i)
        {
            const size_t v = i < sweep ? i : 0;
            s_tour.shotSteps.push_back("=v" + std::to_string(v));
            std::string label = "v" + (sweep ? s_tour.shotsValues[0].size() > 1 ? s_tour.shotsValues[0][v]
                                                                                 : std::to_string(v)
                                             : std::string());
            for (size_t d = 1; d < s_tour.shotsValues.size(); ++d)
                if (s_tour.shotsValues[d].size() > 1)
                    label += "_" + s_tour.shotsValues[d][v];
            s_tour.shotSteps.push_back(i < sweep ? label : label + "_again");
        }
    }
    Dvar_SetIntByName("r_deko9DrawCensus", 0);
    PinDynResScale();
    for (uint32_t spot = 0; spot < s_tour.spots.size(); ++spot)
    {
        if (!s_tour.shotsDvar.empty())
            s_tour.phases.push_back({spot, -3});
        if (s_tour.drawCensusMode)
            s_tour.phases.push_back({spot, -2});
        s_tour.phases.push_back({spot, -1});
        for (int g = 0; g < (int)s_tour.groups.size(); ++g)
            s_tour.phases.push_back({spot, g});
        if (!s_tour.groups.empty())
            s_tour.phases.push_back({spot, -1}); // second baseline: drift check
    }
    const float secs = s_tour.phases.size() * (s_tour.settle + s_tour.measure);
    Com_Printf(CON_CHANNEL_SYSTEM,
               "EMISSIVE_TOUR spots=%u groups=%u phases=%u delay=%gs settle=%gs measure=%gs (~%.0fs after the delay)\n",
               (unsigned)s_tour.spots.size(), (unsigned)s_tour.groups.size(), (unsigned)s_tour.phases.size(),
               s_tour.delay, s_tour.settle, s_tour.measure, secs);
    for (uint32_t g = 0; g < s_tour.groups.size(); ++g)
    {
        std::string dvars;
        for (const std::string &d : s_tour.groups[g].dvars)
            dvars += (dvars.empty() ? "" : ",") + d;
        std::string sets;
        for (const auto &kv : s_tour.groups[g].sets)
            sets += (sets.empty() ? "" : ",") + kv.first + "=" + kv.second;
        Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_GROUP %s skip=%s dvars_off=%s dvars_set=%s\n",
                   s_tour.groups[g].name.c_str(), s_tour.groups[g].skip.empty() ? "-" : s_tour.groups[g].skip.c_str(),
                   dvars.empty() ? "-" : dvars.c_str(), sets.empty() ? "-" : sets.c_str());
    }
    return true;
}
} // namespace

void R_AbTourFrame()
{
    if (!r_deko9EmissiveTour || !dx.device)
        return;
    const uint32_t now = Sys_Milliseconds();
    switch (s_tour.state)
    {
    case TOUR_IDLE:
        if (!r_deko9EmissiveTour->current.enabled)
            return;
        if (!TourSetup())
        {
            s_tour.state = TOUR_DONE;
            return;
        }
        Dvar_SetBoolByName("r_deko9GpuPasses", true);
        s_tourActive = true;
        s_tour.state = TOUR_WAIT_MAP;
        return;
    case TOUR_WAIT_MAP:
        if (!SV_Loaded() || !CL_IsLocalClientInGame(0))
            return;
        s_tour.state = TOUR_DELAY;
        s_tour.stateStartMs = now;
        return;
    case TOUR_DELAY:
        if (now - s_tour.stateStartMs < (uint32_t)(s_tour.delay * 1000))
            return;
        // Toggles: sent once. god keeps the player alive; notarget keeps AI
        // from converging on a player parked in the open.
        Cbuf_AddText(0, "god\nnotarget\n");
        s_tour.phase = 0;
        StartPhase(now);
        return;
    case TOUR_SETTLE:
        if (now - s_tour.stateStartMs < (uint32_t)(s_tour.settle * 1000))
            return;
        if (s_tour.phases[s_tour.phase].group == -3)
        {
            s_tour.state = TOUR_SHOTS;
            s_tour.shotStep = 0;
            s_tour.shotFrames = 0;
            s_tour.stateStartMs = now;
            // The pause menu covers the scene on hardware; with
            // r_deko9EmissiveTourPaused 0 the shots are taken live.
            if (!s_tour.measurePaused && r_deko9EmissiveTourPaused && r_deko9EmissiveTourPaused->current.enabled)
            {
                Cbuf_AddText(0, "pause\n");
                s_tour.measurePaused = true;
            }
            return;
        }
        BeginMeasure(now);
        return;
    case TOUR_SHOTS:
    {
        // Paused pixel A/B of r_deko9EmissiveTourShots: after the pause
        // settles, screenshots <on_a, on_b> with the dvar on, <off> with it
        // off, <on_c> on again (offline tooling diffs them;
        // on_a vs on_b is the frozen-frame baseline).
        // One action per step, at least 2 s and 10 frames apart (4 s for the
        // pause to settle) and never while a capture is still pending: a dvar
        // change always lands whole rendered frames before the next capture.
        const uint32_t kStepCount = (uint32_t)s_tour.shotSteps.size();
        ++s_tour.shotFrames;
        if (s_tour.shotPending)
        {
            // Polled here, not by the console command's per-client-frame
            // report (CL frames stop while paused).
            bool ok = false;
            if (!RB_PollRequestedScreenshot(&ok))
                return;
            s_tour.shotPending = false;
            if (!ok)
                Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_FAIL screenshot not written\n");
        }
        if (s_tour.shotFrames < 10 || now - s_tour.stateStartMs < (s_tour.shotStep ? 2000u : 4000u))
            return;
        s_tour.shotFrames = 0;
        s_tour.stateStartMs = now;
        if (s_tour.shotStep < kStepCount)
        {
            const std::string &stepText = s_tour.shotSteps[s_tour.shotStep++];
            const char *step = stepText.c_str();
            if (step[0] == '=' && step[1] == 'v')
            {
                const size_t v = (size_t)atoi(step + 2);
                for (size_t i = 0; i < s_tour.shotsDvars.size(); ++i)
                {
                    const std::vector<std::string> &values = s_tour.shotsValues[i];
                    Dvar_SetFromStringByName(s_tour.shotsDvars[i].c_str(),
                                             values[values.size() > 1 ? v : 0].c_str());
                }
            }
            else if (step[0] == '=')
            {
                for (size_t i = 0; i < s_tour.shotsDvars.size(); ++i)
                    Dvar_SetFromStringByName(s_tour.shotsDvars[i].c_str(),
                                             (step[1] == '1' ? s_tour.shotsOn : s_tour.shotsOff)[i].c_str());
            }
            else
            {
                const dvar_t *map = Dvar_FindVar("mapname");
                char qpath[128], ospath[256];
                snprintf(qpath, sizeof(qpath), "screenshots/ezab_%s%u_%s.png", map ? map->current.string : "map",
                         s_tour.phases[s_tour.phase].spot + 1, step);
                FS_BuildOSPath(fs_homepath->current.string, fs_gameDirVar->current.string, qpath, ospath);
                FS_CreatePath(ospath);
                RB_RequestScreenshot(ospath);
                s_tour.shotPending = true;
                Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_SHOT %s\n", qpath);
            }
            return;
        }
        // Restore the value the run set (not "on"): the spot's census,
        // baseline and group phases follow and must measure the run's own
        // setting (a shot dvar left on made every later phase an A/B group).
        for (size_t i = 0; i < s_tour.shotsDvars.size(); ++i)
            Dvar_SetFromStringByName(s_tour.shotsDvars[i].c_str(), s_tour.shotsRestore[i].c_str());
        Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_SHOTS phase=%u spot=%u dvar=%s done\n", s_tour.phase + 1,
                   s_tour.phases[s_tour.phase].spot + 1, s_tour.shotsDvar.c_str());
        // The spot's census/baseline/group phases follow on the same paused
        // frame (see EndMeasure); unpause only when nothing follows here.
        if (s_tour.measurePaused && s_tour.phase + 1 < s_tour.phases.size()
            && s_tour.phases[s_tour.phase + 1].spot != s_tour.phases[s_tour.phase].spot)
        {
            Cbuf_AddText(0, "pause\n");
            s_tour.measurePaused = false;
        }
        if (++s_tour.phase < s_tour.phases.size())
        {
            StartPhase(now);
            return;
        }
        if (s_tour.measurePaused)
        {
            Cbuf_AddText(0, "pause\n");
            s_tour.measurePaused = false;
        }
        SetTurn(0.0f);
        Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_DONE phases=%u\n", (unsigned)s_tour.phases.size());
        s_tour.state = TOUR_DONE;
        s_tourActive = false;
        return;
    }
    case TOUR_MEASURE:
        ++s_tour.measureFrames;
        if (now - s_tour.stateStartMs < (uint32_t)(s_tour.measure * 1000))
            return;
        EndMeasure(now);
        if (++s_tour.phase < s_tour.phases.size())
        {
            StartPhase(now);
            return;
        }
        SetTurn(0.0f);
        Com_Printf(CON_CHANNEL_SYSTEM, "EMISSIVE_TOUR_DONE phases=%u\n", (unsigned)s_tour.phases.size());
        s_tour.state = TOUR_DONE;
        s_tourActive = false;
        return;
    case TOUR_DONE:
        return;
    }
}

