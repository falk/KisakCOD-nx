#pragma once

// r_taau per-object motion, back end (r_taau_motion.cpp).

#include <deko9/deko9_taau.h>

#include <cstdint>
#include <vector>

struct GfxViewInfo;

struct RB_TaauMotionMatrices
{
    deko9::TaauMat viewProj;          // this frame, unjittered
    deko9::TaauMat viewProjDepthHack; // the same with the viewmodel's near clip
    deko9::TaauMat prevViewProj;      // the previous resolved frame, unjittered
    float viewOffset[3], prevViewOffset[3]; // skinned vertices are relative to these
};

// The view's moving DObj surfaces as motion draws; returns their count.
uint32_t RB_TaauBuildMotion(const GfxViewInfo *viewInfo, const RB_TaauMotionMatrices &m,
                            std::vector<deko9::TaauMotionDraw> *draws);
