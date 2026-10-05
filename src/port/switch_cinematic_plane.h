#pragma once

struct Material;

// What the movie rectangle draws. Until a decoded frame is uploaded the
// cinematic code images hold black luma and gray chroma with zero alpha, so
// the cinematic material would be see-through; draw opaque black with the
// white material instead, and the cinematic material once a frame exists.
struct CinematicPlaneDraw
{
    Material *material;
    const float *color;
};

inline CinematicPlaneDraw Cinematic_MoviePlaneDraw(bool haveUploadedFrame, Material *cinematicMaterial,
                                                   Material *whiteMaterial, const float *opaqueBlack,
                                                   const float *opaqueWhite)
{
    if (!haveUploadedFrame)
        return { whiteMaterial, opaqueBlack };
    return { cinematicMaterial, opaqueWhite };
}
