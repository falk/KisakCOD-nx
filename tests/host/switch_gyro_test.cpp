// Host test for the pure gyro-aiming math in src/port/switch_gyro.h:
// low-pass smoothing, deadzone, the sensitivity/invert/delta conversion and
// the apply-gate decision. No libnx dependency (compiled without
// __SWITCH__, same as the production header sees on host).

#include "src/port/switch_gyro.h"

#include <math.h>
#include <stdio.h>

static int g_failures;

#define CHECK(cond)                                                         \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static int NearlyEqual(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

static void TestLowPass()
{
    SwitchGyroFilterState state;
    Switch_GyroFilterReset(&state);
    float x, y, z;

    // First sample always passes straight through (no history to blend).
    Switch_GyroLowPass(&state, 1.0f, 2.0f, 3.0f, 0.016f, 30.0f, &x, &y, &z);
    CHECK(NearlyEqual(x, 1.0f, 1e-6f));
    CHECK(NearlyEqual(y, 2.0f, 1e-6f));
    CHECK(NearlyEqual(z, 3.0f, 1e-6f));

    // cutoffHz <= 0 disables smoothing: every subsequent sample also passes
    // straight through even after a big jump.
    Switch_GyroLowPass(&state, 10.0f, 0.0f, 0.0f, 0.016f, 0.0f, &x, &y, &z);
    CHECK(NearlyEqual(x, 10.0f, 1e-6f));

    // A positive cutoff partially tracks a step: strictly between the old
    // and new value, and monotonically converges toward it over repeated
    // identical samples.
    Switch_GyroFilterReset(&state);
    Switch_GyroLowPass(&state, 0.0f, 0.0f, 0.0f, 0.016f, 5.0f, &x, &y, &z);
    Switch_GyroLowPass(&state, 1.0f, 0.0f, 0.0f, 0.016f, 5.0f, &x, &y, &z);
    CHECK(x > 0.0f && x < 1.0f);
    float prev = x;
    for (int i = 0; i < 200; ++i)
        Switch_GyroLowPass(&state, 1.0f, 0.0f, 0.0f, 0.016f, 5.0f, &x, &y, &z);
    CHECK(x > prev);
    CHECK(NearlyEqual(x, 1.0f, 0.01f));

    // dtSeconds <= 0 also passes through unfiltered (no time to integrate).
    Switch_GyroFilterReset(&state);
    Switch_GyroLowPass(&state, 0.0f, 0.0f, 0.0f, 0.016f, 5.0f, &x, &y, &z);
    Switch_GyroLowPass(&state, 5.0f, 0.0f, 0.0f, 0.0f, 5.0f, &x, &y, &z);
    CHECK(NearlyEqual(x, 5.0f, 1e-6f));
}

static void TestDeadzone()
{
    float pitch, yaw;

    // Below the deadzone radius: zeroed entirely.
    Switch_GyroApplyDeadzone(0.1f, 0.1f, 1.0f, &pitch, &yaw);
    CHECK(pitch == 0.0f && yaw == 0.0f);

    // Exactly at the radius: still zeroed (<=).
    Switch_GyroApplyDeadzone(1.0f, 0.0f, 1.0f, &pitch, &yaw);
    CHECK(pitch == 0.0f && yaw == 0.0f);

    // Above it: direction preserved, magnitude reduced by the deadzone.
    Switch_GyroApplyDeadzone(2.0f, 0.0f, 1.0f, &pitch, &yaw);
    CHECK(NearlyEqual(pitch, 1.0f, 1e-5f));
    CHECK(NearlyEqual(yaw, 0.0f, 1e-5f));

    // Diagonal: same magnitude rule, split proportionally between the axes.
    Switch_GyroApplyDeadzone(3.0f, 4.0f, 2.0f, &pitch, &yaw); // magnitude 5
    const float expectedScale = (5.0f - 2.0f) / 5.0f;
    CHECK(NearlyEqual(pitch, 3.0f * expectedScale, 1e-4f));
    CHECK(NearlyEqual(yaw, 4.0f * expectedScale, 1e-4f));

    // Zero deadzone: passthrough.
    Switch_GyroApplyDeadzone(0.01f, 0.02f, 0.0f, &pitch, &yaw);
    CHECK(NearlyEqual(pitch, 0.01f, 1e-6f));
    CHECK(NearlyEqual(yaw, 0.02f, 1e-6f));
}

static void TestComputeDelta()
{
    SwitchGyroConfig cfg;
    float yawDeg, pitchDeg;

    cfg = SwitchGyroConfig{};
    cfg.enable = SWITCH_GYRO_ENABLE_OFF;
    cfg.sensitivityYaw = 1.0f;
    CHECK(!Switch_GyroComputeDelta(&cfg, 1.0f, 1.0f, 0.016f, &yawDeg, &pitchDeg));
    CHECK(yawDeg == 0.0f && pitchDeg == 0.0f);

    cfg.enable = SWITCH_GYRO_ENABLE_ALWAYS;
    CHECK(!Switch_GyroComputeDelta(&cfg, 1.0f, 1.0f, 0.0f, &yawDeg, &pitchDeg));

    // No deadzone, sensitivity 1, no invert: yaw delta = angularVelYaw(rad/s)
    // * 180/pi * dt * sensitivity.
    cfg = SwitchGyroConfig{};
    cfg.enable = SWITCH_GYRO_ENABLE_ALWAYS;
    cfg.sensitivityYaw = 1.0f;
    cfg.deadzoneDegPerSec = 0.0f;
    CHECK(Switch_GyroComputeDelta(&cfg, 0.0f, 1.0f, 1.0f, &yawDeg, &pitchDeg));
    CHECK(NearlyEqual(yawDeg, 57.29578f, 0.01f));
    CHECK(NearlyEqual(pitchDeg, 0.0f, 1e-4f));

    // Sensitivity scales linearly.
    cfg.sensitivityYaw = 2.0f;
    CHECK(Switch_GyroComputeDelta(&cfg, 0.0f, 1.0f, 1.0f, &yawDeg, &pitchDeg));
    CHECK(NearlyEqual(yawDeg, 2.0f * 57.29578f, 0.02f));

    // Invert flips the sign.
    cfg.invertYaw = 1;
    CHECK(Switch_GyroComputeDelta(&cfg, 0.0f, 1.0f, 1.0f, &yawDeg, &pitchDeg));
    CHECK(yawDeg < 0.0f);

    // Pitch sensitivity fallback: sensitivityPitch <= 0 derives from
    // sensitivityYaw * ySensitivityScale.
    cfg = SwitchGyroConfig{};
    cfg.enable = SWITCH_GYRO_ENABLE_ALWAYS;
    cfg.sensitivityYaw = 1.0f;
    cfg.sensitivityPitch = 0.0f;
    cfg.ySensitivityScale = 2.0f;
    CHECK(Switch_GyroComputeDelta(&cfg, 1.0f, 0.0f, 1.0f, &yawDeg, &pitchDeg));
    CHECK(NearlyEqual(pitchDeg, 2.0f * 57.29578f, 0.02f));

    // An explicit positive sensitivityPitch overrides the fallback.
    cfg.sensitivityPitch = 3.0f;
    CHECK(Switch_GyroComputeDelta(&cfg, 1.0f, 0.0f, 1.0f, &yawDeg, &pitchDeg));
    CHECK(NearlyEqual(pitchDeg, 3.0f * 57.29578f, 0.02f));

    // Deadzone reaches through to the delta: a rate under the deadzone
    // produces exactly zero, even with high sensitivity.
    cfg = SwitchGyroConfig{};
    cfg.enable = SWITCH_GYRO_ENABLE_ALWAYS;
    cfg.sensitivityYaw = 100.0f;
    cfg.deadzoneDegPerSec = 5.0f;
    // 1 deg/s (~0.01745 rad/s) is under the 5 deg/s deadzone.
    CHECK(Switch_GyroComputeDelta(&cfg, 0.0f, 0.01745f, 1.0f, &yawDeg, &pitchDeg));
    CHECK(yawDeg == 0.0f);
}

static void TestShouldApply()
{
    SwitchGyroConfig cfg = SwitchGyroConfig{};

    cfg.enable = SWITCH_GYRO_ENABLE_OFF;
    CHECK(!Switch_GyroShouldApply(&cfg, 1, 0, 0, 1, 0));

    cfg.enable = SWITCH_GYRO_ENABLE_ALWAYS;
    CHECK(Switch_GyroShouldApply(&cfg, 1, 0, 0, 0, 0));
    CHECK(!Switch_GyroShouldApply(&cfg, 0, 0, 0, 0, 0)); // menu/not gameplay
    CHECK(!Switch_GyroShouldApply(&cfg, 1, 1, 0, 0, 0)); // frozen (cinematic/pause)
    CHECK(!Switch_GyroShouldApply(&cfg, 1, 0, 1, 0, 0)); // dead
    CHECK(!Switch_GyroShouldApply(&cfg, 1, 0, 0, 0, 1)); // ratchet held

    cfg.enable = SWITCH_GYRO_ENABLE_ADS_ONLY;
    CHECK(!Switch_GyroShouldApply(&cfg, 1, 0, 0, 0, 0)); // not aiming
    CHECK(Switch_GyroShouldApply(&cfg, 1, 0, 0, 1, 0));  // aiming
}

int main()
{
    TestLowPass();
    TestDeadzone();
    TestComputeDelta();
    TestShouldApply();
    if (g_failures)
    {
        printf("FAIL:SWITCH_GYRO failures=%d\n", g_failures);
        return 1;
    }
    printf("PASS:SWITCH_GYRO\n");
    return 0;
}
