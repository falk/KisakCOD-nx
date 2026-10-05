#pragma once

// Depth-of-field blur-fraction clamp for RB_ApplyMergedPostEffects, applied
// to the small and medium fractions RB_GetDepthOfFieldBlurFraction returns
// before the DOF lerp constants are derived from them. Pure, so the clamp can
// be checked against every scene height and dvar combination on the host.

// The DOF lerp constants divide by small, medium - small and 1 - medium, so they
// need 0 < small < medium < 1. A scene below 480 lines with a small nearBlur
// can push both fractions past 1; at 480 lines and above (nearBlur >= 4,
// r_dof_bias >= 0.1) medium <= 0.9895 and the clamp never changes a value.
inline void RB_ClampDofBlurFractions(float *smallFrac, float *mediumFrac)
{
    if (*mediumFrac >= 0.99f)
        *mediumFrac = 0.99f;
    if (*smallFrac >= *mediumFrac)
        *smallFrac = *mediumFrac * 0.5f;
}

inline bool RB_DofBlurFractionsValid(float smallFrac, float mediumFrac)
{
    return 0.0f < smallFrac && smallFrac < mediumFrac && mediumFrac < 1.0f;
}
