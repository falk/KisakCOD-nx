// Host test for the HD Rumble keyframe player in src/port/switch_rumble_hd.h.

#include "src/port/switch_rumble_hd.h"

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

static int Near(float a, float b, float eps)
{
    return fabsf(a - b) <= eps;
}

static int ActiveVoices(const SwitchRumbleHdPlayer *p)
{
    int n = 0;
    for (int i = 0; i < SWITCH_RUMBLE_HD_MAX_VOICES; ++i)
        n += p->voices[i].active;
    return n;
}

static void TestTableSane()
{
    for (int e = 0; e < SWITCH_RUMBLE_HD_EFFECT_COUNT; ++e)
    {
        const SwitchRumbleHdEffect *fx = &kSwitchRumbleHdEffects[e];
        CHECK(fx->keyCount >= 2 && fx->keyCount <= SWITCH_RUMBLE_HD_MAX_KEYS);
        CHECK(fx->keys[0].t == 0.0f);
        CHECK(fx->keys[fx->keyCount - 1].ampLow == 0.0f && fx->keys[fx->keyCount - 1].ampHigh == 0.0f);
        for (int k = 0; k < fx->keyCount; ++k)
        {
            const SwitchRumbleHdKey *key = &fx->keys[k];
            CHECK(k == 0 || key->t > fx->keys[k - 1].t);
            CHECK(key->ampLow >= 0.0f && key->ampLow <= 1.0f && key->ampHigh >= 0.0f && key->ampHigh <= 1.0f);
            CHECK(key->freqLow >= SWITCH_RUMBLE_HD_LOW_MIN_HZ && key->freqLow <= SWITCH_RUMBLE_HD_LOW_MAX_HZ);
            CHECK(key->freqHigh >= SWITCH_RUMBLE_HD_HIGH_MIN_HZ && key->freqHigh <= SWITCH_RUMBLE_HD_HIGH_MAX_HZ);
        }
    }
}

static void TestInterpolation()
{
    SwitchRumbleHdPlayer p;
    SwitchRumbleHdOut out;
    SwitchRumbleHd_Reset(&p);

    SwitchRumbleHd_Advance(&p, 0.016f, &out);
    CHECK(out.left.ampLow == 0.0f && out.right.ampHigh == 0.0f);

    // SHOTGUN: halfway between keys 0 (t0) and 1 (t0.03): amp and freq both lerp.
    SwitchRumbleHd_Trigger(&p, SWITCH_RUMBLE_HD_SHOTGUN, 1.0f, 0.0f);
    SwitchRumbleHd_Advance(&p, 0.015f, &out);
    CHECK(Near(out.left.ampLow, 0.70f, 1e-3f));
    CHECK(Near(out.left.freqLow, 80.0f, 1e-2f));
    CHECK(Near(out.left.freqHigh, 450.0f, 1e-2f));
    CHECK(Near(out.left.ampLow, out.right.ampLow, 1e-6f));

    // Frequency changes over time, not just amplitude.
    const float f0 = out.left.freqLow;
    SwitchRumbleHd_Advance(&p, 0.1f, &out);
    CHECK(out.left.freqLow < f0);

    SwitchRumbleHd_Advance(&p, 0.5f, &out);
    CHECK(out.left.ampLow == 0.0f);
    CHECK(ActiveVoices(&p) == 0);
}

static void TestAutoRetrigger()
{
    SwitchRumbleHdPlayer p;
    SwitchRumbleHdOut out;
    SwitchRumbleHd_Reset(&p);

    // First shot is the full effect; shots inside the window use the auto variant.
    SwitchRumbleHd_TriggerWeaponFire(&p, 2, 0);
    CHECK(p.voices[0].effect == SWITCH_RUMBLE_HD_SMG);
    SwitchRumbleHd_Advance(&p, 0.05f, &out);
    SwitchRumbleHd_TriggerWeaponFire(&p, 2, 0);
    CHECK(p.voices[1].active && p.voices[1].effect == SWITCH_RUMBLE_HD_SMG_AUTO);

    // Sustained fire restarts the auto voice instead of stacking, and the
    // output keeps pulsing (amplitude dips between shots, then spikes).
    float minAmp = 10.0f;
    float maxAmp = 0.0f;
    for (int shot = 0; shot < 20; ++shot)
    {
        SwitchRumbleHd_TriggerWeaponFire(&p, 2, 0);
        for (int f = 0; f < 4; ++f)
        {
            SwitchRumbleHd_Advance(&p, 0.0125f, &out);
            if (shot > 4)
            {
                if (out.left.ampHigh < minAmp)
                    minAmp = out.left.ampHigh;
                if (out.left.ampHigh > maxAmp)
                    maxAmp = out.left.ampHigh;
            }
        }
        CHECK(ActiveVoices(&p) <= 2);
    }
    CHECK(maxAmp > 0.2f);
    CHECK(minAmp < maxAmp * 0.6f);

    // After a pause, the next shot is a full single shot again.
    SwitchRumbleHd_Advance(&p, 0.5f, &out);
    SwitchRumbleHd_TriggerWeaponFire(&p, 2, 0);
    CHECK(p.voices[0].effect == SWITCH_RUMBLE_HD_SMG || p.voices[1].effect == SWITCH_RUMBLE_HD_SMG);

    // Class mapping.
    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_TriggerWeaponFire(&p, 0, 1);
    CHECK(p.voices[0].effect == SWITCH_RUMBLE_HD_SNIPER);
    SwitchRumbleHd_TriggerWeaponFire(&p, 3, 0);
    CHECK(p.voices[1].effect == SWITCH_RUMBLE_HD_SHOTGUN);
    SwitchRumbleHd_TriggerWeaponFire(&p, 6, 0);
    CHECK(p.voices[2].effect == SWITCH_RUMBLE_HD_LAUNCHER);
    SwitchRumbleHd_TriggerWeaponFire(&p, 4, 0);
    CHECK(p.voices[3].effect == SWITCH_RUMBLE_HD_PISTOL);
}

static void TestMixAndClamp()
{
    SwitchRumbleHdPlayer p;
    SwitchRumbleHdOut single;
    SwitchRumbleHdOut mixed;

    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_Trigger(&p, SWITCH_RUMBLE_HD_DAMAGE, 0.5f, 0.0f);
    SwitchRumbleHd_Advance(&p, 0.05f, &single);
    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_Trigger(&p, SWITCH_RUMBLE_HD_DAMAGE, 0.5f, 0.0f);
    SwitchRumbleHd_Trigger(&p, SWITCH_RUMBLE_HD_EXPLOSION, 0.5f, 0.0f);
    SwitchRumbleHd_Advance(&p, 0.05f, &mixed);
    // Concurrent effects add...
    CHECK(mixed.left.ampLow > single.left.ampLow);

    // ...but never past the cap, and frequencies stay in hardware range.
    SwitchRumbleHd_Reset(&p);
    for (int i = 0; i < 6; ++i)
        SwitchRumbleHd_Trigger(&p, SWITCH_RUMBLE_HD_LAUNCHER + (i % 4), 1.0f, 0.0f);
    SwitchRumbleHd_Advance(&p, 0.05f, &mixed);
    CHECK(mixed.left.ampLow <= SWITCH_RUMBLE_HD_MIX_CAP + 1e-6f);
    CHECK(mixed.left.ampHigh <= SWITCH_RUMBLE_HD_MIX_CAP + 1e-6f);
    CHECK(mixed.left.freqLow >= SWITCH_RUMBLE_HD_LOW_MIN_HZ && mixed.left.freqLow <= SWITCH_RUMBLE_HD_LOW_MAX_HZ);
    CHECK(mixed.left.freqHigh >= SWITCH_RUMBLE_HD_HIGH_MIN_HZ && mixed.left.freqHigh <= SWITCH_RUMBLE_HD_HIGH_MAX_HZ);

    // Frequency follows the dominant contributor.
    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_Trigger(&p, SWITCH_RUMBLE_HD_LAUNCHER, 1.0f, 0.0f);
    SwitchRumbleHd_Trigger(&p, SWITCH_RUMBLE_HD_THROW, 0.1f, 0.0f);
    SwitchRumbleHd_Advance(&p, 0.05f, &mixed);
    CHECK(Near(mixed.left.freqLow, 55.0f, 1.0f));

    // Voice pool is bounded: overflow steals instead of growing.
    SwitchRumbleHd_Reset(&p);
    for (int i = 0; i < 40; ++i)
        SwitchRumbleHd_Trigger(&p, SWITCH_RUMBLE_HD_PISTOL, 0.2f, 0.0f);
    CHECK(ActiveVoices(&p) == SWITCH_RUMBLE_HD_MAX_VOICES);

    // Intensity scales, 0 and disabled silence.
    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_Trigger(&p, SWITCH_RUMBLE_HD_SHOTGUN, 1.0f, 0.0f);
    SwitchRumbleHd_Advance(&p, 0.03f, &single);
    mixed = single;
    SwitchRumbleHd_ApplyIntensity(&mixed, 0.5f, 1);
    CHECK(Near(mixed.left.ampLow, single.left.ampLow * 0.5f, 1e-5f));
    SwitchRumbleHd_ApplyIntensity(&mixed, 1.0f, 0);
    CHECK(mixed.left.ampLow == 0.0f && mixed.right.ampHigh == 0.0f);
}

static void TestBalance()
{
    SwitchRumbleHdPlayer p;
    SwitchRumbleHdOut out;

    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_TriggerDamage(&p, 50, -1.0f);
    SwitchRumbleHd_Advance(&p, 0.05f, &out);
    CHECK(out.left.ampLow > out.right.ampLow * 2.0f);

    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_TriggerDamage(&p, 50, 1.0f);
    SwitchRumbleHd_Advance(&p, 0.05f, &out);
    CHECK(out.right.ampLow > out.left.ampLow * 2.0f);

    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_TriggerDamage(&p, 50, 0.0f);
    SwitchRumbleHd_Advance(&p, 0.05f, &out);
    CHECK(Near(out.left.ampLow, out.right.ampLow, 1e-6f));

    // More damage hits harder.
    SwitchRumbleHdOut big;
    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_TriggerDamage(&p, 100, 0.0f);
    SwitchRumbleHd_Advance(&p, 0.05f, &big);
    CHECK(big.left.ampLow > out.left.ampLow);
}

static void TestExplosionDistance()
{
    SwitchRumbleHdPlayer p;
    SwitchRumbleHdOut nearOut;
    SwitchRumbleHdOut farOut;

    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_TriggerExplosion(&p, 600.0f, 300.0f);
    CHECK(ActiveVoices(&p) == 0);
    SwitchRumbleHd_TriggerExplosion(&p, 10.0f, 0.0f);
    CHECK(ActiveVoices(&p) == 0);

    SwitchRumbleHd_TriggerExplosion(&p, 20.0f, 300.0f);
    SwitchRumbleHd_Advance(&p, 0.04f, &nearOut);
    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_TriggerExplosion(&p, 250.0f, 300.0f);
    SwitchRumbleHd_Advance(&p, 0.04f, &farOut);
    CHECK(nearOut.left.ampLow > farOut.left.ampLow);
    CHECK(farOut.left.ampLow > 0.0f);
}

static void TestStopAndSend()
{
    SwitchRumbleHdPlayer p;
    SwitchRumbleHdOut out;
    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_Trigger(&p, SWITCH_RUMBLE_HD_LAUNCHER, 1.0f, 0.0f);
    SwitchRumbleHd_Advance(&p, 0.05f, &out);
    CHECK(out.left.ampLow > 0.0f);
    SwitchRumbleHd_Reset(&p);
    SwitchRumbleHd_Advance(&p, 0.0f, &out);
    CHECK(out.left.ampLow == 0.0f && out.left.ampHigh == 0.0f && ActiveVoices(&p) == 0);

    SwitchRumbleHdBand a = {0.5f, 100.0f, 0.2f, 300.0f};
    SwitchRumbleHdBand b = a;
    SwitchRumbleHdBand silent = {0.0f, 100.0f, 0.0f, 300.0f};
    CHECK(!SwitchRumbleHd_ShouldSend(&a, &b, 16));
    CHECK(SwitchRumbleHd_ShouldSend(&a, &b, 100));
    b.ampLow = 0.6f;
    CHECK(SwitchRumbleHd_ShouldSend(&a, &b, 16));
    b = a;
    b.freqLow = 120.0f;
    CHECK(SwitchRumbleHd_ShouldSend(&a, &b, 16));
    CHECK(SwitchRumbleHd_ShouldSend(&a, &silent, 16));
    CHECK(!SwitchRumbleHd_ShouldSend(&silent, &silent, 5000));
}

int main()
{
    TestTableSane();
    TestInterpolation();
    TestAutoRetrigger();
    TestMixAndClamp();
    TestBalance();
    TestExplosionDistance();
    TestStopAndSend();
    if (g_failures)
    {
        printf("FAIL:SWITCH_RUMBLE_HD failures=%d\n", g_failures);
        return 1;
    }
    printf("PASS:SWITCH_RUMBLE_HD\n");
    return 0;
}
