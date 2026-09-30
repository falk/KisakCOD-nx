// Host test (ASan/UBSan) for the pure parts of deko9 baked per-pass state and
// instanced draws (src/deko9/deko9_baked.h): raster key normalization and
// completeness, the baked-unit cache and its invalidation, instance layouts
// and run splitting. Run by ./test host (deko9_baked_sanitizer_check).

#include "src/deko9/deko9_baked.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <vector>

namespace
{

int g_failures;

void Check(bool ok, const char *name)
{
    if (!ok)
    {
        std::printf("FAIL:DEKO9_BAKED %s\n", name);
        ++g_failures;
    }
}

using deko9::BuildRasterKey;
using deko9::DepthTarget;
using deko9::RasterKey;

// D3D9 defaults (Device::ResetState).
void Defaults(uint32_t rs[256])
{
    std::memset(rs, 0, 256 * sizeof(uint32_t));
    rs[deko9::kRsZEnable] = 1;
    rs[deko9::kRsFillMode] = 3;
    rs[deko9::kRsZWriteEnable] = 1;
    rs[deko9::kRsSrcBlend] = 2;
    rs[deko9::kRsDestBlend] = 1;
    rs[deko9::kRsCullMode] = 3;
    rs[deko9::kRsZFunc] = 4;
    rs[deko9::kRsAlphaFunc] = 8;
    rs[deko9::kRsStencilFail] = rs[deko9::kRsStencilZFail] = rs[deko9::kRsStencilPass] = 1;
    rs[deko9::kRsStencilFunc] = 8;
    rs[deko9::kRsStencilMask] = rs[deko9::kRsStencilWriteMask] = 0xffffffff;
    rs[deko9::kRsCcwStencilFail] = rs[deko9::kRsCcwStencilZFail] = rs[deko9::kRsCcwStencilPass] = 1;
    rs[deko9::kRsCcwStencilFunc] = 8;
    rs[deko9::kRsColorWriteEnable] = rs[deko9::kRsColorWriteEnable1] = rs[deko9::kRsColorWriteEnable2] =
        rs[deko9::kRsColorWriteEnable3] = 0xf;
    rs[deko9::kRsBlendOp] = rs[deko9::kRsBlendOpAlpha] = 1;
    rs[deko9::kRsSrcBlendAlpha] = 2;
    rs[deko9::kRsDestBlendAlpha] = 1;
    rs[deko9::kRsBlendFactor] = 0xffffffff;
}

uint32_t FloatBits(float f)
{
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    return bits;
}

const DepthTarget kD24S8{true, true, false};
const DepthTarget kD16{true, false, true};
const DepthTarget kNoDepth{};

void TestRasterKeyNormalization()
{
    uint32_t rs[256];
    Defaults(rs);
    const RasterKey base = BuildRasterKey(rs, kD24S8);

    // Disabled features ignore their inputs.
    uint32_t t[256];
    std::memcpy(t, rs, sizeof(t));
    t[deko9::kRsStencilFail] = 5;
    t[deko9::kRsStencilRef] = 0x42;
    t[deko9::kRsCcwStencilFunc] = 3;
    Check(BuildRasterKey(t, kD24S8) == base, "stencil off ignores stencil ops/ref");
    std::memcpy(t, rs, sizeof(t));
    t[deko9::kRsSrcBlend] = 5;
    t[deko9::kRsBlendFactor] = 0x12345678;
    t[deko9::kRsSrcBlendAlpha] = 7;
    Check(BuildRasterKey(t, kD24S8) == base, "blend off ignores factors/op/constant");
    std::memcpy(t, rs, sizeof(t));
    t[deko9::kRsAlphaRef] = 0x80;
    t[deko9::kRsAlphaFunc] = 7;
    Check(BuildRasterKey(t, kD24S8) == base, "alpha test off ignores func/ref");
    std::memcpy(t, rs, sizeof(t));
    t[deko9::kRsCcwStencilPass] = 4;
    t[deko9::kRsStencilEnable] = 1;
    uint32_t u[256];
    std::memcpy(u, t, sizeof(u));
    u[deko9::kRsCcwStencilPass] = 6;
    Check(BuildRasterKey(t, kD24S8) == BuildRasterKey(u, kD24S8), "one-sided stencil ignores CCW ops");
    u[deko9::kRsTwoSidedStencilMode] = 1;
    t[deko9::kRsTwoSidedStencilMode] = 1;
    Check(BuildRasterKey(t, kD24S8) != BuildRasterKey(u, kD24S8), "two-sided stencil keys CCW ops");

    // Enabled features key their inputs.
    std::memcpy(t, rs, sizeof(t));
    t[deko9::kRsStencilEnable] = 1;
    const RasterKey stencil = BuildRasterKey(t, kD24S8);
    Check(stencil != base, "stencil enable changes the key");
    t[deko9::kRsStencilRef] = 0x42;
    Check(BuildRasterKey(t, kD24S8) != stencil, "stencil ref keyed when enabled");
    Check(BuildRasterKey(t, kD16) == BuildRasterKey(rs, kD16), "stencil ignored on a depth format without stencil");
    Check(BuildRasterKey(t, kNoDepth) == BuildRasterKey(rs, kNoDepth), "stencil ignored without a depth target");

    // Depth test needs a depth target; zfunc is recorded regardless.
    Check(BuildRasterKey(rs, kNoDepth) != base, "no depth target disables the depth test");
    std::memcpy(t, rs, sizeof(t));
    t[deko9::kRsZFunc] = 2;
    Check(BuildRasterKey(t, kNoDepth) != BuildRasterKey(rs, kNoDepth), "zfunc keyed even with the test off");
    t[deko9::kRsZWriteEnable] = 0;
    t[deko9::kRsZFunc] = 4;
    Check(BuildRasterKey(t, kNoDepth) == BuildRasterKey(rs, kNoDepth), "depth write ignored without depth test");

    // Bias: signed zero is no bias; the D16 unit matters only with a bias.
    std::memcpy(t, rs, sizeof(t));
    t[deko9::kRsDepthBias] = FloatBits(-0.0f);
    t[deko9::kRsSlopeScaleDepthBias] = FloatBits(-0.0f);
    Check(BuildRasterKey(t, kD24S8) == base, "-0 bias equals no bias");
    Check(BuildRasterKey(rs, kD16).flags == BuildRasterKey(rs, kD24S8).flags, "D16 flag only with a bias");
    t[deko9::kRsDepthBias] = FloatBits(1.0f / 65536);
    Check(BuildRasterKey(t, kD16) != BuildRasterKey(t, kD24S8), "bias unit differs D16 vs D24");

    // Render-state dvars (r_polygonOffsetBias etc.) arrive as new bias
    // values: a different key, never a stale hit.
    uint32_t v[256];
    std::memcpy(v, t, sizeof(v));
    v[deko9::kRsDepthBias] = FloatBits(2.0f / 65536);
    Check(BuildRasterKey(v, kD24S8) != BuildRasterKey(t, kD24S8), "polygon offset dvar change re-keys");

    // Color-write masks use their low 4 bits only (dkColorWriteStateSetMask).
    std::memcpy(t, rs, sizeof(t));
    t[deko9::kRsColorWriteEnable2] = 0xff;
    Check(BuildRasterKey(t, kD24S8) == base, "color write mask low nibble");

    // Out-of-range enums stay distinguishable from valid ones.
    std::memcpy(t, rs, sizeof(t));
    t[deko9::kRsCullMode] = 0x100 + 3;
    Check(BuildRasterKey(t, kD24S8) != base, "out-of-range cull mode not aliased");
}

// Completeness: every render state BuildRasterKey reads is flagged by
// IsRasterRenderState (so SetRenderState dirties the raster state), and no
// other state changes the key.
void TestRasterKeyCompleteness()
{
    uint32_t rs[256];
    Defaults(rs);
    // Enable every feature so each input is live.
    rs[deko9::kRsStencilEnable] = 1;
    rs[deko9::kRsTwoSidedStencilMode] = 1;
    rs[deko9::kRsAlphaTestEnable] = 1;
    rs[deko9::kRsAlphaBlendEnable] = 1;
    rs[deko9::kRsSeparateAlphaBlendEnable] = 1;
    const RasterKey base = BuildRasterKey(rs, kD24S8);
    uint32_t flagged = 0;
    for (uint32_t state = 0; state < 256; ++state)
    {
        // Mutations: flip low bits of an enum/mask, toggle a boolean, a
        // non-zero float bias.
        bool changes = false;
        const uint32_t mutations[3] = {rs[state] ^ 0x5u, rs[state] ? 0u : 1u, FloatBits(0.25f)};
        for (uint32_t m : mutations)
        {
            uint32_t t[256];
            std::memcpy(t, rs, sizeof(t));
            t[state] = m;
            changes |= BuildRasterKey(t, kD24S8) != base;
        }
        const bool isRaster = deko9::IsRasterRenderState(state);
        flagged += isRaster;
        if (changes && !isRaster)
        {
            char name[96];
            std::snprintf(name, sizeof(name), "state %u changes the key but is not a raster state", state);
            Check(false, name);
        }
        if (!changes && isRaster)
        {
            char name[96];
            std::snprintf(name, sizeof(name), "raster state %u does not reach the key", state);
            Check(false, name);
        }
    }
    Check(flagged == 36, "36 raster render states");
}

struct TestRasterUnit
{
    RasterKey key;
};

struct TestUnit
{
    deko9::ProgramKey key;
    int payload;
};

void TestCache()
{
    deko9::BakedCache<deko9::ProgramKey, TestUnit> cache;
    std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint64_t, uint32_t>, TestUnit *> reference;
    std::mt19937 rng(12345);
    for (int i = 0; i < 5000; ++i)
    {
        deko9::ProgramKey key{(uint32_t)(rng() % 40 + 1), (uint32_t)(rng() % 40 + 41), (uint32_t)(rng() % 10 + 81),
                              (uint32_t)(rng() % 4), (uint64_t)(rng() % 3), (uint32_t)(rng() % 2)};
        auto tuple = std::make_tuple(key.vs, key.ps, key.decl, key.psShadowMask, key.instance, key.psEarlyZ);
        TestUnit *found = cache.Find(key);
        auto it = reference.find(tuple);
        if ((found != nullptr) != (it != reference.end()) || (found && found != it->second))
        {
            Check(false, "cache find matches the reference map");
            break;
        }
        if (!found)
        {
            auto unit = std::make_unique<TestUnit>();
            unit->key = key;
            unit->payload = i;
            reference[tuple] = cache.Insert(std::move(unit));
        }
    }
    Check(cache.Size() == reference.size(), "cache size");
    // Units keep their address across growth (the device keeps pointers).
    bool stable = true;
    for (const auto &entry : reference)
        stable &= cache.Find(entry.second->key) == entry.second;
    Check(stable, "unit pointers stable across rehash");

    // Shader / declaration release: purge by id, others stay findable.
    // (Snapshot keys and payloads first: removed units are freed.)
    std::vector<std::pair<deko9::ProgramKey, int>> snapshot;
    for (const auto &entry : reference)
        snapshot.push_back({entry.second->key, entry.second->payload});
    reference.clear();
    const uint32_t releasedShader = 7, releasedDecl = 85;
    const uint32_t removed = cache.RemoveIf(
        [&](const TestUnit &u) { return u.key.Uses(releasedShader) || u.key.Uses(releasedDecl); });
    uint32_t expected = 0;
    bool survivorsOk = true;
    for (const auto &entry : snapshot)
    {
        const deko9::ProgramKey &key = entry.first;
        const bool gone = key.Uses(releasedShader) || key.Uses(releasedDecl);
        expected += gone;
        if (!gone)
            survivorsOk &= cache.Find(key) != nullptr && cache.Find(key)->payload == entry.second;
        else
            survivorsOk &= cache.Find(key) == nullptr;
    }
    Check(removed == expected && removed > 0, "purge removes exactly the released object's units");
    Check(survivorsOk, "purge keeps every other unit");
    // Device reset.
    cache.Clear();
    bool allGone = cache.Size() == 0;
    for (const auto &entry : snapshot)
        allGone &= cache.Find(entry.first) == nullptr;
    Check(allGone, "reset clears every unit");

    // Raster cache with real keys.
    deko9::BakedCache<RasterKey, TestRasterUnit> raster;
    uint32_t rs[256];
    Defaults(rs);
    auto unit = std::make_unique<TestRasterUnit>();
    unit->key = BuildRasterKey(rs, kD24S8);
    raster.Insert(std::move(unit));
    Check(raster.Find(BuildRasterKey(rs, kD24S8)) != nullptr, "raster key hit");
    rs[deko9::kRsCullMode] = 1;
    Check(raster.Find(BuildRasterKey(rs, kD24S8)) == nullptr, "raster key miss after a state change");
}

void TestInstanceLayout()
{
    deko9::InstanceLayout layout;
    Check(deko9::AddInstanceRegs(&layout, 8, 4), "add world matrix rows");
    Check(deko9::AddInstanceRegs(&layout, 4, 4), "add a second matrix");
    Check(deko9::AddInstanceRegs(&layout, 10, 2), "overlapping rows dedupe");
    Check(layout.count == 8, "8 unique registers");
    bool sorted = true;
    for (uint32_t i = 1; i < layout.count; ++i)
        sorted &= layout.regs[i - 1] < layout.regs[i];
    Check(sorted && layout.regs[0] == 4 && layout.regs[7] == 11, "sorted register list");
    const deko9::InstanceLayout before = layout;
    Check(!deko9::AddInstanceRegs(&layout, 20, 9), "overflow past 16 registers rejected");
    Check(layout == before, "rejected add leaves the layout unchanged");
    Check(!deko9::AddInstanceRegs(&layout, 254, 4), "register >= 256 rejected");
    Check(layout == before, "rejected add leaves the layout unchanged (range)");
    deko9::InstanceLayout other;
    deko9::AddInstanceRegs(&other, 4, 8);
    Check(other == layout && other.Hash() == layout.Hash(), "same registers, same layout and hash");
    deko9::InstanceLayout third;
    deko9::AddInstanceRegs(&third, 4, 7);
    Check(third != layout && third.Hash() != layout.Hash(), "different registers, different hash");
    Check(deko9::InstanceLayout{}.Hash() == 0, "empty layout hash is 0 (not instanced)");
}

void TestInstanceRuns()
{
    const int probes[] = {3, 3, 3, 1, 1, 3, 2, 2, 2, 2};
    std::vector<std::pair<uint32_t, uint32_t>> runs;
    const uint32_t n = deko9::ForEachInstanceRun(
        10, [&](uint32_t i) { return probes[i]; }, [&](uint32_t first, uint32_t count) { runs.push_back({first, count}); });
    const std::vector<std::pair<uint32_t, uint32_t>> expected = {{0, 3}, {3, 2}, {5, 1}, {6, 4}};
    Check(n == 4 && runs == expected, "runs split where the group changes, order kept");
    runs.clear();
    deko9::ForEachInstanceRun(
        0, [&](uint32_t) { return 0; }, [&](uint32_t first, uint32_t count) { runs.push_back({first, count}); });
    Check(runs.empty(), "no instances, no runs");
    // Every instance lands in exactly one run, in order.
    std::mt19937 rng(7);
    for (int trial = 0; trial < 200; ++trial)
    {
        std::vector<int> group(rng() % 50);
        for (int &g : group)
            g = rng() % 3;
        uint32_t next = 0;
        bool ok = true;
        deko9::ForEachInstanceRun(
            (uint32_t)group.size(), [&](uint32_t i) { return group[i]; },
            [&](uint32_t first, uint32_t count) {
                ok &= first == next && count > 0;
                for (uint32_t i = first; i < first + count; ++i)
                    ok &= group[i] == group[first];
                ok &= first + count == group.size() || group[first + count] != group[first];
                next = first + count;
            });
        ok &= next == group.size();
        if (!ok)
        {
            Check(false, "random run partition");
            break;
        }
    }
}

// Early-Z variant eligibility (RasterAllowsEarlyZ): only draws that test
// depth and write neither depth nor stencil.
void TestEarlyZ()
{
    uint32_t rs[256];
    DepthTarget depth;
    depth.present = true;
    depth.stencil = true;
    Defaults(rs);
    Check(!deko9::RasterAllowsEarlyZ(rs, depth), "early-Z: depth write on -> not allowed");
    rs[deko9::kRsZWriteEnable] = 0;
    Check(deko9::RasterAllowsEarlyZ(rs, depth), "early-Z: test on, write off -> allowed");
    DepthTarget none;
    Check(!deko9::RasterAllowsEarlyZ(rs, none), "early-Z: no depth target -> not allowed");
    rs[deko9::kRsZEnable] = 0;
    Check(!deko9::RasterAllowsEarlyZ(rs, depth), "early-Z: depth test off -> not allowed");
    rs[deko9::kRsZEnable] = 1;
    // Stencil: keep-only ops (D3D9 defaults) are fine; any writing op is not,
    // unless the write mask is zero or the target has no stencil.
    rs[deko9::kRsStencilEnable] = 1;
    Check(deko9::RasterAllowsEarlyZ(rs, depth), "early-Z: stencil test with KEEP ops -> allowed");
    rs[deko9::kRsStencilPass] = 3; // REPLACE
    Check(!deko9::RasterAllowsEarlyZ(rs, depth), "early-Z: stencil REPLACE on pass -> not allowed");
    rs[deko9::kRsStencilWriteMask] = 0;
    Check(deko9::RasterAllowsEarlyZ(rs, depth), "early-Z: stencil write mask 0 -> allowed");
    rs[deko9::kRsStencilWriteMask] = 0xff;
    DepthTarget noStencil = depth;
    noStencil.stencil = false;
    Check(deko9::RasterAllowsEarlyZ(rs, noStencil), "early-Z: stencil ops without stencil bits -> allowed");
    rs[deko9::kRsStencilPass] = 1;
    rs[deko9::kRsStencilZFail] = 7; // INCR
    Check(!deko9::RasterAllowsEarlyZ(rs, depth), "early-Z: stencil INCR on z-fail -> not allowed");
    rs[deko9::kRsStencilZFail] = 1;
    rs[deko9::kRsTwoSidedStencilMode] = 1;
    rs[deko9::kRsCcwStencilFail] = 2; // ZERO
    Check(!deko9::RasterAllowsEarlyZ(rs, depth), "early-Z: two-sided back-face ZERO -> not allowed");
    rs[deko9::kRsTwoSidedStencilMode] = 0;
    Check(deko9::RasterAllowsEarlyZ(rs, depth), "early-Z: back-face ops ignored when one-sided");
    rs[deko9::kRsStencilEnable] = 0;
    rs[deko9::kRsStencilFail] = 3;
    Check(deko9::RasterAllowsEarlyZ(rs, depth), "early-Z: stencil ops ignored with stencil off");
    // Key identity: the variant choice is part of the program key.
    deko9::ProgramKey a{1, 2, 3, 0, 0, 0}, b{1, 2, 3, 0, 0, 1};
    Check(a != b && deko9::HashKey(a) != deko9::HashKey(b), "early-Z: program keys differ by variant");
}

} // namespace

int main()
{
    TestRasterKeyNormalization();
    TestRasterKeyCompleteness();
    TestCache();
    TestInstanceLayout();
    TestInstanceRuns();
    TestEarlyZ();
    std::printf("%s:DEKO9_BAKED\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
