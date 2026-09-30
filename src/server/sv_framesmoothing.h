#pragma once

// Retail PC SP server frame-rate smoothing (iw3sp 0x5c8390) budget
// arithmetic, kept pure so a host test can drive it.
//
// Retail keeps two 10-entry rings indexed by a frame counter, each entry
// already divided by 10 so a ring's sum is an average:
//   frameDelta[i]  counter ticks between successive calls (the displayed
//                  frame period)
//   slack[i]       how far the last extra server slice missed its budget
// After R_SyncGpu returns, the time left in an average frame is
//   budget = avgFrame - (now - lastTime) - max(avgSlack, 0)
// and a positive budget runs G_RunFrame(SV_FRAME_DO_SMOOTHING, now + budget).
// All values are the low 32 bits of the counter and wrap like retail's
// 32-bit arithmetic; divisions truncate toward zero (retail's
// imul 0x66666667 / sar 2 / sign fix).

#include <stdint.h>

#define SV_FRAME_SMOOTHING_HISTORY 10

struct SvFrameSmoothing
{
    int32_t lastTime;                                // 0x014bfb34
    int32_t frameIndex;                              // 0x014bfb38
    int32_t frameDelta[SV_FRAME_SMOOTHING_HISTORY];  // 0x014bfb3c
    int32_t slack[SV_FRAME_SMOOTHING_HISTORY];       // 0x014bfb64
};

static inline int32_t SV_FrameSmoothing_Wrap(uint32_t v)
{
    return (int32_t)v;
}

// Time (counter ticks) available for an extra slice at `now`; <= 0 means none.
static inline int32_t SV_FrameSmoothing_Budget(const SvFrameSmoothing *s, int32_t now)
{
    uint32_t slackSum = 0;
    uint32_t deltaSum = 0;
    for (int i = 0; i < SV_FRAME_SMOOTHING_HISTORY; ++i)
    {
        slackSum += (uint32_t)s->slack[i];
        deltaSum += (uint32_t)s->frameDelta[i];
    }
    if (SV_FrameSmoothing_Wrap(slackSum) < 0)
        slackSum = 0;
    return SV_FrameSmoothing_Wrap((uint32_t)s->lastTime - (uint32_t)now - slackSum + deltaSum);
}

// Record one call: `now` is the counter after R_SyncGpu (the budget base),
// `budget` what SV_FrameSmoothing_Budget returned for it, `end` the counter
// after the optional slice.
static inline void SV_FrameSmoothing_Record(SvFrameSmoothing *s, int32_t now, int32_t budget, int32_t end)
{
    // Retail uses a signed modulo; the counter never gets near 2^31 frames.
    const int slot = (int)((uint32_t)s->frameIndex % SV_FRAME_SMOOTHING_HISTORY);
    if (budget > 0)
    {
        // |budget - elapsed| / 10: how far the slice missed its cap.
        const int32_t miss = SV_FrameSmoothing_Wrap((uint32_t)now - (uint32_t)end + (uint32_t)budget);
        const uint32_t magnitude = miss < 0 ? 0u - (uint32_t)miss : (uint32_t)miss;
        s->slack[slot] = (int32_t)(magnitude / 10u);
    }
    else
    {
        uint32_t slackSum = 0;
        for (int i = 0; i < SV_FRAME_SMOOTHING_HISTORY; ++i)
            slackSum += (uint32_t)s->slack[i];
        if (SV_FrameSmoothing_Wrap(slackSum) < 0)
            slackSum = 0;
        const int32_t over = SV_FrameSmoothing_Wrap(slackSum + (uint32_t)budget);
        s->slack[slot] = over < 0 ? over / 10 : 0;
    }
    s->frameDelta[slot] = SV_FrameSmoothing_Wrap((uint32_t)end - (uint32_t)s->lastTime) / 10;
    s->lastTime = end;
    s->frameIndex = SV_FrameSmoothing_Wrap((uint32_t)s->frameIndex + 1u);
}
