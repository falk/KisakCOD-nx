// DOF blur-fraction clamp (rb_dof_fraction.h, used by
// RB_ApplyMergedPostEffects) and the retail lerp constants derived from it,
// over dynamic-res scene heights and the dvar range: the clamped fractions
// must keep 0 < small < medium < 1, every lerp constant must be finite and
// bounded, and at 480 lines and above the clamp must leave the retail values
// bit-identical.
#include <gfx_d3d/rb_dof_fraction.h>
#include <cmath>
#include <cstdio>
#include <cstring>

// RB_GetDepthOfFieldBlurFraction's retail formula: radii are authored at 480
// lines and scale with the scene height.
static float BlurFraction(float pixelRadiusAtSceneRes, int sceneHeight, float nearBlur, float dofBias)
{
    float normalizedRadius = pixelRadiusAtSceneRes * 480.0f / (double)sceneHeight;
    float fraction = normalizedRadius / nearBlur;
    return pow(fraction, dofBias);
}

// RB_ApplyMergedPostEffects' retail CONST_SRC_CODE_DOF_LERP_SCALE / _BIAS.
static void LerpConstants(float smallFrac, float mediumFrac, float scale[4], float bias[4])
{
    scale[0] = -1.0f / smallFrac;
    scale[1] = -1.0f / (mediumFrac - smallFrac);
    scale[2] = -1.0f / (1.0f - mediumFrac);
    scale[3] = 1.0f / (1.0f - mediumFrac);
    bias[0] = 1.0f;
    bias[1] = mediumFrac / (mediumFrac - smallFrac);
    bias[2] = 1.0f / (1.0f - mediumFrac);
    bias[3] = -mediumFrac / (1.0f - mediumFrac);
}

static int s_checks, s_failures;
static void Check(bool ok, const char *what, int height, float nearBlur, float bias)
{
    ++s_checks;
    if (!ok)
    {
        ++s_failures;
        std::printf("FAIL:DOF_FRACTION %s height=%d nearBlur=%g bias=%g\n", what, height, nearBlur, bias);
    }
}

int main()
{
    const int heights[] = {240, 288, 360, 420, 480, 720};
    const float nearBlurs[] = {4.0f, 4.7325f, 6.0f, 10.0f};
    const float biases[] = {0.1f, 0.5f, 1.0f, 3.0f};
    // Bound on any lerp constant: medium <= 0.99 caps the 1 - medium terms at
    // 100, and the small/medium gap terms stay below this on the grid.
    const float bound = 1.0e4f;
    float maxAbs = 0.0f;
    int clampedBelow480 = 0, altChangedAt480 = 0, invalidUnclamped = 0;
    for (int height : heights)
        for (float nearBlur : nearBlurs)
            for (float bias : biases)
            {
                const float rawSmall = BlurFraction(1.4f, height, nearBlur, bias);
                const float rawMedium = BlurFraction(3.5999999f, height, nearBlur, bias);
                float smallFrac = rawSmall, mediumFrac = rawMedium;
                invalidUnclamped += !RB_DofBlurFractionsValid(rawSmall, rawMedium);
                RB_ClampDofBlurFractions(&smallFrac, &mediumFrac);
                Check(RB_DofBlurFractionsValid(smallFrac, mediumFrac), "0 < small < medium < 1", height, nearBlur, bias);

                float scale[4], biasK[4];
                LerpConstants(smallFrac, mediumFrac, scale, biasK);
                for (int i = 0; i < 4; ++i)
                {
                    Check(std::isfinite(scale[i]) && std::fabs(scale[i]) <= bound, "lerp scale finite and bounded", height, nearBlur, bias);
                    Check(std::isfinite(biasK[i]) && std::fabs(biasK[i]) <= bound, "lerp bias finite and bounded", height, nearBlur, bias);
                    maxAbs = std::fmax(maxAbs, std::fmax(std::fabs(scale[i]), std::fabs(biasK[i])));
                }

                if (height >= 480)
                {
                    // Retail path: no clamp. Values and constants bit-identical.
                    float rawScale[4], rawBias[4];
                    LerpConstants(rawSmall, rawMedium, rawScale, rawBias);
                    Check(smallFrac == rawSmall && mediumFrac == rawMedium, "clamp changed a >=480 fraction", height, nearBlur, bias);
                    Check(!std::memcmp(scale, rawScale, sizeof(scale)) && !std::memcmp(biasK, rawBias, sizeof(biasK)),
                          "clamp changed a >=480 lerp constant", height, nearBlur, bias);
                    // The proposed small = min(small, 0.8 * medium) would not be retail-neutral.
                    if (std::fmin(rawSmall, 0.8f * rawMedium) != rawSmall)
                        ++altChangedAt480;
                }
                else if (smallFrac != rawSmall || mediumFrac != rawMedium)
                {
                    ++clampedBelow480;
                }
            }

    // Negative control: without the clamp some sub-480 cases break the
    // ordering the constants divide by (the retail assert).
    Check(clampedBelow480 > 0 && invalidUnclamped > 0, "clamp never needed below 480 lines", 0, 0.0f, 0.0f);
    std::printf("dof fraction: unclamped invalid=%d, clamped below 480=%d, min(small,0.8*medium) would change %d cases at >=480, max |constant|=%g\n",
                invalidUnclamped, clampedBelow480, altChangedAt480, maxAbs);
    std::printf("dof fraction: %s (%d checks, %d failures)\n", s_failures ? "FAIL" : "PASS", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
