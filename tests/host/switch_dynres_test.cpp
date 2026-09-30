// Host proof for the dynamic render resolution controller
// (src/gfx_d3d/r_dynres_controller.h, r_dynres): synthetic GPU-time
// sequences run through a frame loop that models the real pipeline (the
// size chosen for frame N is what frame N renders at; its GPU time arrives
// two frames later, tagged with that size). Checks:
//   - quantisation: widths are multiples of 16, heights of 8, the top level
//     is exactly the output size, min/max clamp;
//   - a light load stays at full size, never changes;
//   - a pixel-bound heavy load converges below the budget and then holds
//     (no oscillation) for thousands of frames;
//   - fixed + per-pixel cost (the realistic shape) converges without
//     oscillating, also with noise;
//   - a spike drops fast (within a few frames) and recovers slowly;
//   - an impossible load pins the minimum, never below; a load that only
//     fits at a size above maxScale pins maxScale;
//   - stale samples (the old size, arriving late) never drive a decision;
//   - runtime reconfiguration (r_dynresMin/Max) clamps the level.

#include "src/gfx_d3d/r_dynres_controller.h"

#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <vector>

namespace
{
int g_failures;

void Check(bool ok, const char *what, const char *detail = "")
{
    std::printf("%s:DYNRES_%s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok)
        ++g_failures;
}

struct Run
{
    std::vector<dynres::Size> sizes; // per frame
    std::vector<float> gpu;          // per frame, GPU ms of that frame
    uint64_t changes = 0;
};

// Frame loop: the controller's size renders frame N; its sample arrives at
// frame N + latency.
Run Simulate(dynres::Controller &ctl, int frames, const std::function<float(int frame, dynres::Size)> &gpuMs,
             int latency = 2)
{
    Run run;
    std::deque<std::pair<float, dynres::Size>> inFlight;
    for (int f = 0; f < frames; ++f)
    {
        const dynres::Size size = ctl.Current();
        const float ms = gpuMs(f, size);
        run.sizes.push_back(size);
        run.gpu.push_back(ms);
        inFlight.push_back({ms, size});
        if ((int)inFlight.size() > latency)
        {
            ctl.Sample(inFlight.front().first, inFlight.front().second);
            inFlight.pop_front();
        }
    }
    run.changes = ctl.changes;
    return run;
}

int CountChanges(const Run &run, int from, int to)
{
    int n = 0;
    for (int f = from + 1; f < to && f < (int)run.sizes.size(); ++f)
        n += run.sizes[f] != run.sizes[f - 1];
    return n;
}

float Scale(const dynres::Config &c, dynres::Size s) { return (float)s.width / (float)c.maxWidth; }

uint32_t Hash(uint32_t v)
{
    v ^= v >> 16;
    v *= 0x7feb352du;
    v ^= v >> 15;
    v *= 0x846ca68bu;
    return v ^ (v >> 16);
}

} // namespace

int main()
{
    dynres::Config cfg; // 1280x720, 0.75..1.0, 15.5 ms
    char detail[256];

    // 1. Quantisation and clamps.
    {
        bool ok = true;
        for (int level = 1; level <= dynres::MaxLevel(cfg); ++level)
        {
            const dynres::Size s = dynres::SizeForLevel(cfg, level);
            if (level < dynres::MaxLevel(cfg))
                ok = ok && s.width % 16 == 0 && s.height % 8 == 0 &&
                     std::fabs((double)s.height - s.width * 9.0 / 16.0) <= 4.0;
            ok = ok && s.width <= 1280 && s.height <= 720;
        }
        const dynres::Size top = dynres::SizeForLevel(cfg, dynres::MaxLevel(cfg));
        const dynres::Size min = dynres::SizeForLevel(cfg, dynres::MinLevel(cfg));
        const dynres::Size s875 = dynres::SizeForLevel(cfg, dynres::LevelForScale(cfg, 0.875f));
        std::snprintf(detail, sizeof(detail), "top=%dx%d min=%dx%d 0.875=%dx%d", top.width, top.height, min.width,
                      min.height, s875.width, s875.height);
        Check(ok && top.width == 1280 && top.height == 720 && min.width == 960 && min.height == 544 &&
                  s875.width == 1120 && s875.height == 632,
              "QUANTISE", detail);
        dynres::Config odd = cfg;
        odd.maxWidth = 1920;
        odd.maxHeight = 1080;
        const dynres::Size d = dynres::SizeForLevel(odd, dynres::LevelForScale(odd, 0.6667f));
        std::snprintf(detail, sizeof(detail), "1080p 2/3 = %dx%d", d.width, d.height);
        Check(d.width == 1280 && d.height == 720, "QUANTISE_1080P", detail);
    }

    // 2. Light load (CPU-bound frame): full size, no change.
    {
        dynres::Controller ctl;
        ctl.Reset(cfg, dynres::MaxLevel(cfg));
        const Run run = Simulate(ctl, 3000, [](int, dynres::Size s) { return 11.0f * s.Pixels() / 921600.0f; });
        std::snprintf(detail, sizeof(detail), "changes=%llu final=%dx%d", (unsigned long long)run.changes,
                      run.sizes.back().width, run.sizes.back().height);
        Check(run.changes == 0 && run.sizes.back().width == 1280, "LIGHT_LOAD_STAYS_FULL", detail);
    }

    // 3. Pixel-bound heavy load: 20 ms at 720p, time proportional to pixels.
    //    Converges to a size under the budget, then holds.
    {
        dynres::Controller ctl;
        ctl.Reset(cfg, dynres::MaxLevel(cfg));
        const Run run = Simulate(ctl, 6000, [](int, dynres::Size s) { return 20.0f * s.Pixels() / 921600.0f; });
        int settled = -1;
        for (int f = 0; f < 6000; ++f)
            if (run.gpu[f] <= cfg.budgetMs && CountChanges(run, f, 6000) == 0)
            {
                settled = f;
                break;
            }
        const float scale = Scale(cfg, run.sizes.back());
        std::snprintf(detail, sizeof(detail), "settled_at_frame=%d scale=%.4f gpu=%.2fms changes=%llu drops=%llu raises=%llu",
                      settled, scale, run.gpu.back(), (unsigned long long)run.changes,
                      (unsigned long long)ctl.drops, (unsigned long long)ctl.raises);
        // Fits: 20 s^2 <= 15.5 -> s <= 0.880; the raise line (0.9 x budget
        // on the next size) keeps it one or two levels lower.
        Check(settled >= 0 && settled < 600 && run.gpu.back() <= cfg.budgetMs && scale >= 0.80f && scale <= 0.881f &&
                  CountChanges(run, 1000, 6000) == 0,
              "PIXEL_BOUND_CONVERGES", detail);
    }

    // 4. Fixed + per-pixel cost with +-0.6 ms noise: 7 ms fixed + 10 ms at
    //    720p (fits at s^2 <= 0.85). Few changes over 100 s, time under
    //    budget in the steady state except noise.
    {
        dynres::Controller ctl;
        ctl.Reset(cfg, dynres::MaxLevel(cfg));
        const Run run = Simulate(ctl, 6000, [](int f, dynres::Size s) {
            const float noise = ((Hash((uint32_t)f) & 0xffff) / 65535.0f - 0.5f) * 1.2f;
            return 7.0f + 10.0f * s.Pixels() / 921600.0f + noise;
        });
        const int late = CountChanges(run, 1200, 6000);
        int over = 0;
        double sum = 0;
        for (int f = 1200; f < 6000; ++f)
        {
            over += run.gpu[f] > cfg.budgetMs;
            sum += Scale(cfg, run.sizes[f]);
        }
        std::snprintf(detail, sizeof(detail),
                      "changes_after_20s=%d total_changes=%llu over_budget_frames=%d mean_scale=%.3f backoff=%d", late,
                      (unsigned long long)run.changes, over, sum / 4800.0, ctl.Backoff());
        // A change at most every ~10 s on average once settled.
        Check(late <= 10 && over < 4800 / 50 && sum / 4800.0 >= 0.80, "FIXED_PLUS_PIXEL_NOISY_STABLE", detail);
    }

    // 5. Spike: 12 ms at 720p (fits), then 24 ms at 720p for 120 frames (fits at the minimum),
    //    then back. Drops within 3 samples of the spike reaching the
    //    controller; recovers to full size only gradually (no single jump).
    {
        dynres::Controller ctl;
        ctl.Reset(cfg, dynres::MaxLevel(cfg));
        const int spikeStart = 600, spikeEnd = 720;
        const Run run = Simulate(ctl, 4000, [&](int f, dynres::Size s) {
            const float k = (f >= spikeStart && f < spikeEnd) ? 24.0f : 12.0f;
            return 2.0f + (k - 2.0f) * s.Pixels() / 921600.0f;
        });
        int firstDrop = -1;
        for (int f = spikeStart; f < spikeEnd; ++f)
            if (run.sizes[f].width < 1280)
            {
                firstDrop = f;
                break;
            }
        int minW = 1280, recovered = -1, maxRaiseStep = 0;
        for (int f = spikeStart; f < 4000; ++f)
        {
            minW = std::min(minW, run.sizes[f].width);
            if (f > spikeEnd && recovered < 0 && run.sizes[f].width == 1280)
                recovered = f;
            if (f > spikeEnd)
                maxRaiseStep = std::max(maxRaiseStep, run.sizes[f].width - run.sizes[f - 1].width);
        }
        int overInSpike = 0;
        for (int f = spikeStart + 20; f < spikeEnd; ++f)
            overInSpike += run.gpu[f] > cfg.budgetMs;
        std::snprintf(detail, sizeof(detail),
                      "first_drop_frame=%d (spike at %d) min_width=%d over_budget_in_spike_after_20=%d "
                      "recovered_frame=%d max_raise_step=%dpx",
                      firstDrop, spikeStart, minW, overInSpike, recovered, maxRaiseStep);
        Check(firstDrop >= 0 && firstDrop <= spikeStart + 2 + 3 && minW >= 960 && overInSpike == 0 &&
                  recovered > spikeEnd + 3 * cfg.raiseFrames && recovered < 4000 && maxRaiseStep <= 32,
              "SPIKE_FAST_DROP_SLOW_RECOVERY", detail);
    }

    // 6. Clamps: impossible load pins the minimum; a load that would fit
    //    only above r_dynresMax pins the maximum.
    {
        dynres::Controller ctl;
        ctl.Reset(cfg, dynres::MaxLevel(cfg));
        const Run heavy = Simulate(ctl, 2000, [](int, dynres::Size s) { return 60.0f * s.Pixels() / 921600.0f; });
        int belowMin = 0;
        for (const dynres::Size &s : heavy.sizes)
            belowMin += s.width < 960;
        std::snprintf(detail, sizeof(detail), "final=%dx%d below_min=%d", heavy.sizes.back().width,
                      heavy.sizes.back().height, belowMin);
        Check(heavy.sizes.back().width == 960 && belowMin == 0, "CLAMP_MIN", detail);

        dynres::Config capped = cfg;
        capped.maxScale = 0.9f;
        dynres::Controller ctl2;
        ctl2.Reset(capped, dynres::MinLevel(capped));
        const Run light = Simulate(ctl2, 4000, [](int, dynres::Size s) { return 8.0f * s.Pixels() / 921600.0f; });
        int above = 0;
        for (const dynres::Size &s : light.sizes)
            above += s.width > 1152;
        std::snprintf(detail, sizeof(detail), "final=%dx%d above_max=%d", light.sizes.back().width,
                      light.sizes.back().height, above);
        Check(light.sizes.back().width == 1152 && above == 0, "CLAMP_MAX", detail);
    }

    // 7. Stale samples: after a drop, the late samples of the old size
    //    (still over budget) must not cause a second drop.
    {
        dynres::Controller ctl;
        ctl.Reset(cfg, dynres::MaxLevel(cfg));
        const dynres::Size full = ctl.Current();
        ctl.Sample(17.0f, full); // drop
        const int afterFirst = ctl.Level();
        ctl.Sample(17.0f, full); // late, old size
        ctl.Sample(17.0f, full);
        std::snprintf(detail, sizeof(detail), "level %d -> %d, after stale %d, ignored=%llu", dynres::MaxLevel(cfg),
                      afterFirst, ctl.Level(), (unsigned long long)ctl.ignored);
        Check(afterFirst < dynres::MaxLevel(cfg) && ctl.Level() == afterFirst && ctl.ignored == 2,
              "STALE_SAMPLES_IGNORED", detail);
    }

    // 8. Runtime limits: raising r_dynresMin above the current size moves
    //    the level up at once; lowering r_dynresMax moves it down.
    {
        dynres::Controller ctl;
        ctl.Reset(cfg, dynres::MinLevel(cfg));
        dynres::Config c2 = cfg;
        c2.minScale = 0.875f;
        const bool moved = ctl.Configure(c2);
        const int w1 = ctl.Current().width;
        dynres::Config c3 = c2;
        c3.maxScale = 0.875f;
        c3.minScale = 0.75f;
        ctl.Reset(c3, dynres::MaxLevel(c3));
        const int w2 = ctl.Current().width;
        std::snprintf(detail, sizeof(detail), "min->0.875 width=%d, max 0.875 reset width=%d", w1, w2);
        Check(moved && w1 == 1120 && w2 == 1120, "RUNTIME_LIMITS", detail);
    }

    // 9. Oscillation stress: a load exactly at the raise/drop boundary
    //    (fixed 9 ms + 7 ms per 720p, noise +-1 ms, occasional 3-frame
    //    spikes) over 10 minutes: bounded number of changes thanks to the
    //    hysteresis and the raise backoff.
    {
        dynres::Controller ctl;
        ctl.Reset(cfg, dynres::MaxLevel(cfg));
        const int frames = 36000;
        const Run run = Simulate(ctl, frames, [](int f, dynres::Size s) {
            const float noise = ((Hash((uint32_t)f * 7u + 3u) & 0xffff) / 65535.0f - 0.5f) * 2.0f;
            const float spike = (f % 1800) < 3 ? 6.0f : 0.0f;
            return 9.0f + 7.0f * s.Pixels() / 921600.0f + noise + spike;
        });
        const int changes = CountChanges(run, 0, frames);
        std::snprintf(detail, sizeof(detail), "changes_in_10min=%d drops=%llu raises=%llu backoff=%d final=%dx%d",
                      changes, (unsigned long long)ctl.drops, (unsigned long long)ctl.raises, ctl.Backoff(),
                      run.sizes.back().width, run.sizes.back().height);
        // Spikes every 30 s force drops (20 of them); each costs at most a
        // drop and a later raise pair or two.
        Check(changes <= 120, "BOUNDARY_NO_OSCILLATION", detail);
    }

    std::printf("%s:DYNRES_CONTROLLER\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
