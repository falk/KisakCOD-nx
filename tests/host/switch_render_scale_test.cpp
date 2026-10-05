// Host proof for the scene render scale rules (src/gfx_d3d/r_render_scale.h):
// every combination of r_dynres, r_renderScale (at layout creation and at
// runtime) and r_taau maps to the expected layout, mode, frame scale and
// upscaler. The negative control models the previous rule, where TAAU and
// the scene layout required r_dynres, and checks that it fails the new
// expectation "fixed 0.67 + r_taau 1 => TAAU".

#include "src/gfx_d3d/r_render_scale.h"

#include <cmath>
#include <cstdio>

namespace
{
int g_failures;

void Check(bool ok, const char *what, const char *detail = "")
{
    std::printf("%s:RENDER_SCALE_%s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok)
        ++g_failures;
}

using render_scale::Mode;
using render_scale::Upscaler;

struct Row
{
    bool dynres;        // r_dynres when the targets are created
    float startScale;   // r_renderScale when the targets are created
    float runtimeScale; // r_renderScale on a later frame
    bool held;          // the controller held (an A/B tour)
    bool taau;
    // expected
    bool layout;
    Mode mode;
    float frameScale; // controller pick is 0.6 in this table
    Upscaler upscaler;
};

constexpr float kController = 0.6f;

const Row kRows[] = {
    // native: the defaults; r_taau has nothing to resolve
    {false, 1.0f, 1.0f, false, true, false, Mode::Native, 1.0f, Upscaler::None},
    {false, 1.0f, 1.0f, false, false, false, Mode::Native, 1.0f, Upscaler::None},
    // started native, a lower r_renderScale waits for vid_restart
    {false, 1.0f, 0.67f, false, true, false, Mode::Native, 1.0f, Upscaler::None},
    {false, 1.0f, 0.67f, false, false, false, Mode::Native, 1.0f, Upscaler::None},
    // fixed S, controller off
    {false, 0.67f, 0.67f, false, true, true, Mode::Fixed, 0.67f, Upscaler::Taau},
    {false, 0.67f, 0.67f, false, false, true, Mode::Fixed, 0.67f, Upscaler::Spatial},
    {false, 0.5f, 0.5f, false, true, true, Mode::Fixed, 0.5f, Upscaler::Taau},
    {false, 0.25f, 0.25f, false, false, true, Mode::Fixed, 0.25f, Upscaler::Spatial},
    // fixed, changed at runtime between fixed values (the A/B scale sweep)
    {false, 0.67f, 0.875f, false, true, true, Mode::Fixed, 0.875f, Upscaler::Taau},
    {false, 0.67f, 0.5f, false, false, true, Mode::Fixed, 0.5f, Upscaler::Spatial},
    // fixed layout raised to 1 at runtime: TAAU as anti-aliasing only, or a copy
    {false, 0.67f, 1.0f, false, true, true, Mode::Fixed, 1.0f, Upscaler::Taau},
    {false, 0.67f, 1.0f, false, false, true, Mode::Fixed, 1.0f, Upscaler::Spatial},
    // adaptive: r_renderScale is not read while the controller drives
    {true, 1.0f, 1.0f, false, true, true, Mode::Adaptive, kController, Upscaler::Taau},
    {true, 1.0f, 1.0f, false, false, true, Mode::Adaptive, kController, Upscaler::Spatial},
    {true, 0.5f, 0.8f, false, true, true, Mode::Adaptive, kController, Upscaler::Taau},
    // adaptive held by a tour: r_renderScale sets the size again
    {true, 1.0f, 0.75f, true, true, true, Mode::Fixed, 0.75f, Upscaler::Taau},
    {true, 1.0f, 0.75f, true, false, true, Mode::Fixed, 0.75f, Upscaler::Spatial},
};

const char *Name(Upscaler u)
{
    return render_scale::UpscalerName(u);
}

// The previous rule: only r_dynres created the scene layout and TAAU
// required it; r_renderScale (then r_dynresForceScale) pinned a size only
// on top of it.
bool OldTaauActive(bool dynres, bool taau)
{
    return taau && dynres;
}

} // namespace

int main()
{
    int index = 0;
    for (const Row &r : kRows)
    {
        char detail[192];
        const bool layout = render_scale::SceneLayoutFor(r.dynres, r.startScale);
        const Mode mode = render_scale::ModeFor(layout, r.dynres && !r.held);
        const float scale = render_scale::FrameScale(mode, r.runtimeScale, kController);
        const Upscaler up = render_scale::UpscalerFor(mode, r.taau);
        std::snprintf(detail, sizeof(detail),
                      "row=%d r_dynres=%d start=%.3f runtime=%.3f held=%d r_taau=%d -> layout=%d mode=%s "
                      "scale=%.3f upscaler=%s (want %d %s %.3f %s)",
                      index, r.dynres, r.startScale, r.runtimeScale, r.held, r.taau, layout,
                      render_scale::ModeName(mode), scale, Name(up), r.layout, render_scale::ModeName(r.mode),
                      r.frameScale, Name(r.upscaler));
        Check(layout == r.layout && mode == r.mode && std::fabs(scale - r.frameScale) < 1e-6f && up == r.upscaler,
              "TRUTH_TABLE", detail);
        ++index;
    }

    // Exhaustive: whatever the inputs, TAAU runs exactly on scene-layout
    // frames with r_taau on, and native frames always render at scale 1.
    const float scales[] = {0.25f, 0.5f, 0.67f, 0.875f, 0.999f, 1.0f};
    int combos = 0, bad = 0;
    for (int dynres = 0; dynres < 2; ++dynres)
        for (float start : scales)
            for (float runtime : scales)
                for (int held = 0; held < 2; ++held)
                    for (int taau = 0; taau < 2; ++taau)
                    {
                        ++combos;
                        const bool layout = render_scale::SceneLayoutFor(dynres, start);
                        const Mode mode = render_scale::ModeFor(layout, dynres && !held);
                        const float scale = render_scale::FrameScale(mode, runtime, kController);
                        const Upscaler up = render_scale::UpscalerFor(mode, taau);
                        const bool wantLayout = dynres || start < 1.0f;
                        const bool ok = layout == wantLayout && (up == Upscaler::Taau) == (layout && taau) &&
                                        (mode != Mode::Native || (scale == 1.0f && up == Upscaler::None)) &&
                                        (mode != Mode::Fixed || scale == runtime) &&
                                        (mode != Mode::Adaptive || (scale == kController && dynres && !held));
                        bad += ok ? 0 : 1;
                    }
    char detail[64];
    std::snprintf(detail, sizeof(detail), "combos=%d bad=%d", combos, bad);
    Check(bad == 0 && combos == 288, "EXHAUSTIVE", detail);

    // Clamp: out-of-range scales never reach the targets.
    Check(render_scale::FrameScale(Mode::Fixed, 0.1f, 1.0f) == render_scale::kMinScale &&
              render_scale::FrameScale(Mode::Fixed, 2.0f, 1.0f) == 1.0f &&
              render_scale::FrameScale(Mode::Adaptive, 1.0f, 0.0f) == render_scale::kMinScale,
          "CLAMP");

    // Negative control: fixed 0.67 with r_taau 1 and the controller off.
    const bool layout = render_scale::SceneLayoutFor(false, 0.67f);
    const bool now = render_scale::UpscalerFor(render_scale::ModeFor(layout, false), true) == Upscaler::Taau;
    const bool old = OldTaauActive(false, true);
    std::snprintf(detail, sizeof(detail), "new_taau=%d old_taau=%d", now, old);
    Check(now && !old, "NEGATIVE_CONTROL_OLD_GATING_FAILS", detail);

    std::printf("%s:RENDER_SCALE failures=%d\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
