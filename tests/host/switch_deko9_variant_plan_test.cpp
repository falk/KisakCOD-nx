// deko3d renderer load-time variant plan (src/deko9/deko9_variant_plan.h).
//
// Always: the plan's selection rules and a seeded synthetic draw stream --
// every variant a draw selects must be in the set planned from the passes'
// load-time inputs, and removing any one key class (depth compare, early-Z,
// instancing) from the plan must make that check fail (negative controls).
//
// Census (optional, user game data outside the repository):
//   KISAK_DEKO_VARIANT_PASSES  pass list(s) written by the retail boot host's
//                              deko-variant-passes mode, ':'-separated, one
//                              per map ("label=path" names it);
//   KISAK_DEKO_SHADER_CORPUS   the <hash>.vs / <hash>.ps programs.
// Prints each map's planned variant count; with KISAK_DEKO_VARIANT_BUILD=1
// also translates and compiles every planned variant and reports the host
// time; with KISAK_DEKO_VARIANT_RECORDED=<log> checks that every
// "DEKO9 first bind" key of a recorded run is planned.

#include "src/deko9/deko9_variant_plan.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

void Check(bool ok, const char *what)
{
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL:DEKO9_VARIANT_PLAN %s\n", what);
    }
}

using namespace deko9;

Deko9ShaderInfo PixelInfo(uint32_t samplerMask, uint32_t cubeMask, bool kills, bool writesDepth)
{
    Deko9ShaderInfo info{};
    info.stage = DEKO9_STAGE_PIXEL;
    info.samplerMask = samplerMask;
    for (uint32_t s = 0; s < DEKO9_MAX_SAMPLERS; ++s)
        info.samplerDim[s] = (samplerMask >> s) & 1u ? ((cubeMask >> s) & 1u ? DEKO9_SAMPLER_CUBE : DEKO9_SAMPLER_2D)
                                                      : DEKO9_SAMPLER_NONE;
    info.kills = kills;
    info.writesDepth = writesDepth;
    return info;
}

Deko9ShaderInfo VertexInfo(uint32_t inputMask)
{
    Deko9ShaderInfo info{};
    info.stage = DEKO9_STAGE_VERTEX;
    info.inputMask = inputMask;
    return info;
}

void TestRules()
{
    Check(CompareSampler(true, DEKO9_SAMPLER_2D) && !CompareSampler(true, DEKO9_SAMPLER_CUBE) &&
              !CompareSampler(false, DEKO9_SAMPLER_2D),
          "compare: a depth texture on a 2D sampler only");
    const Deko9ShaderInfo kill = PixelInfo(1, 0, true, false), plain = PixelInfo(1, 0, false, false),
                          depth = PixelInfo(1, 0, true, true);
    Check(EarlyZShaderEligible(kill, false) && EarlyZShaderEligible(plain, true) &&
              !EarlyZShaderEligible(plain, false) && !EarlyZShaderEligible(depth, true),
          "early-Z: discards or alpha-tests, never writes depth");
    Check(VariantShadowFilter(DEKO9_STAGE_PIXEL, 0x10, 1) == 1 && VariantShadowFilter(DEKO9_STAGE_PIXEL, 0, 1) == 0 &&
              VariantShadowFilter(DEKO9_STAGE_VERTEX, 0x10, 1) == 0,
          "shadow filter: pixel shaders with a compare sampler only");
    const uint8_t regs[] = {9, 4, 5, 4};
    InstanceLayout layout;
    Check(LayoutFromRegs(regs, 4, &layout) && layout.count == 3 && layout.regs[0] == 4 && layout.regs[2] == 9,
          "layout: sorted, unique");
    Check(!LayoutFromRegs(regs, 0, &layout), "layout: empty list refused");

    // Two compare candidates (one 2D, one cube excluded) -> masks {0, 0x10}
    // x early-Z {0, 1}; a vertex shader whose inputs leave room for 4
    // instance registers -> ordinary + instanced.
    const Deko9ShaderInfo ps = PixelInfo(0x31, 0x20, true, false);
    const Deko9ShaderInfo vs = VertexInfo(0x0fff);
    PassVariantDesc pass;
    pass.vsId = 1;
    pass.psId = 2;
    pass.vs = &vs;
    pass.ps = &ps;
    pass.depthSamplerMask = 0x30;
    LayoutFromRegs(regs, 3, &pass.instance);
    VariantPlan plan;
    PlanOptions options;
    options.shadowFilter = 1;
    plan.Add(pass, options);
    const std::vector<PlannedVariant> &all = plan.Finish();
    uint32_t vsCount = 0, psCount = 0, filtered = 0;
    for (const PlannedVariant &v : all)
    {
        (v.stage == DEKO9_STAGE_VERTEX ? vsCount : psCount)++;
        filtered += v.select.shadowFilter != 0;
    }
    Check(vsCount == 2 && psCount == 4 && filtered == 2, "plan: 2 vertex + 4 pixel variants, filter on masked ones");
    // Three candidates enumerate all 8 subsets.
    const Deko9ShaderInfo ps3 = PixelInfo(0x0e, 0, false, false);
    pass.ps = &ps3;
    pass.depthSamplerMask = 0x0e;
    pass.instance = {};
    VariantPlan plan3;
    plan3.Add(pass, PlanOptions{});
    uint32_t masks = 0;
    for (const PlannedVariant &v : plan3.Finish())
        masks += v.stage == DEKO9_STAGE_PIXEL;
    Check(masks == 8, "plan: every subset of the compare candidates");
    // Instance layout overlapping the inputs is never planned.
    const Deko9ShaderInfo wide = VertexInfo(0xffff);
    pass.vs = &wide;
    LayoutFromRegs(regs, 3, &pass.instance);
    VariantPlan planWide;
    planWide.Add(pass, PlanOptions{});
    uint32_t inst = 0;
    for (const PlannedVariant &v : planWide.Finish())
        inst += v.select.instance.count != 0;
    Check(inst == 0, "plan: no instanced variant when the layout overlaps the inputs");
}

// ---- synthetic draw stream -----------------------------------------------------

enum KeyClass
{
    CLASS_COMPARE,
    CLASS_EARLYZ,
    CLASS_INSTANCE,
    CLASS_COUNT
};
const char *const kClassNames[CLASS_COUNT] = {"compare", "early-Z", "instance"};

bool InClass(const PlannedVariant &v, KeyClass c)
{
    switch (c)
    {
    case CLASS_COMPARE: return v.select.shadowMask != 0;
    case CLASS_EARLYZ: return v.select.earlyZ;
    default: return v.select.instance.count != 0;
    }
}

struct World
{
    std::vector<Deko9ShaderInfo> vsInfos, psInfos;
    std::vector<PassVariantDesc> passes;
};

World MakeWorld(std::mt19937 &rng, uint32_t passCount)
{
    World w;
    w.vsInfos.resize(passCount / 3 + 1);
    w.psInfos.resize(passCount / 2 + 1);
    for (Deko9ShaderInfo &vs : w.vsInfos)
        vs = VertexInfo((1u << (4 + rng() % 12)) - 1u); // v0..v3 at least, up to all 16
    for (Deko9ShaderInfo &ps : w.psInfos)
    {
        const uint32_t samplers = rng() & 0x3f;
        const uint32_t cube = (rng() & 0x3f) & samplers & ~0x30u;
        ps = PixelInfo(samplers, cube, rng() % 3 == 0, rng() % 8 == 0);
    }
    for (uint32_t i = 0; i < passCount; ++i)
    {
        PassVariantDesc pass;
        const uint32_t vsIndex = rng() % w.vsInfos.size(), psIndex = rng() % w.psInfos.size();
        pass.vsId = 1000 + vsIndex;
        pass.psId = 5000 + psIndex;
        pass.vs = &w.vsInfos[vsIndex];
        pass.ps = &w.psInfos[psIndex];
        // Shadow-map code samplers live at s4 (sun) / s5 (spot) in retail
        // lit techniques; a few passes bind one of them.
        const uint32_t shadow = rng() % 4;
        pass.depthSamplerMask = shadow == 1 ? 0x10u : shadow == 2 ? 0x20u : shadow == 3 ? 0x30u : 0u;
        pass.alphaTest = rng() % 4 == 0;
        if (rng() % 3 == 0)
        {
            uint8_t regs[16];
            const uint32_t count = 1 + rng() % 8;
            const uint32_t first = rng() % 32;
            for (uint32_t r = 0; r < count; ++r)
                regs[r] = (uint8_t)(first + r);
            LayoutFromRegs(regs, count, &pass.instance);
        }
        w.passes.push_back(pass);
    }
    return w;
}

// A draw of `pass` binds what the engine could bind for it: depth textures
// only on the pass's shadow-map samplers (each either the shadow map or a
// colour placeholder), the alpha test only where the material enables it,
// any raster state, an occlusion query sometimes, and the pass's instance
// layout when drawn instanced.
DrawVariantState RandomDraw(std::mt19937 &rng, const PassVariantDesc &pass)
{
    DrawVariantState d;
    d.depthBoundMask = pass.depthSamplerMask & (uint32_t)rng();
    d.alphaTest = pass.alphaTest && (rng() & 1u);
    d.rasterAllowsEarlyZ = rng() & 1u;
    d.occlusionOpen = rng() % 16 == 0;
    d.instanced = pass.instance.count && (rng() & 1u);
    d.instance = pass.instance;
    return d;
}

uint32_t CountMissing(const VariantPlan &plan, const std::vector<PlannedVariant> &requested)
{
    uint32_t missing = 0;
    for (const PlannedVariant &v : requested)
        missing += !plan.Contains(v);
    return missing;
}

void TestSyntheticStream()
{
    for (uint32_t seed = 1; seed <= 8; ++seed)
    {
        std::mt19937 rng(seed * 7919u);
        const World world = MakeWorld(rng, 400);
        for (int ezOn = 0; ezOn < 2; ++ezOn)
        {
            PlanOptions options;
            options.earlyZ = ezOn != 0;
            options.shadowFilter = seed & 1u;
            options.shaderOpt = (seed >> 1) & 1u;
            VariantPlan plan;
            for (const PassVariantDesc &pass : world.passes)
                plan.Add(pass, options);
            plan.Finish();
            std::vector<PlannedVariant> requested;
            uint32_t perClass[CLASS_COUNT] = {};
            for (uint32_t i = 0; i < 20000; ++i)
            {
                const PassVariantDesc &pass = world.passes[rng() % world.passes.size()];
                PlannedVariant vs, ps;
                DrawVariants(pass, RandomDraw(rng, pass), options, &vs, &ps);
                for (const PlannedVariant &v : {vs, ps})
                {
                    requested.push_back(v);
                    for (int c = 0; c < CLASS_COUNT; ++c)
                        perClass[c] += InClass(v, (KeyClass)c);
                }
            }
            char what[160];
            std::snprintf(what, sizeof(what), "seed %u ez %d: every requested variant is planned", seed, ezOn);
            Check(CountMissing(plan, requested) == 0, what);
            for (int c = 0; c < CLASS_COUNT; ++c)
            {
                if (c == CLASS_EARLYZ && !ezOn)
                {
                    std::snprintf(what, sizeof(what), "seed %u: r_deko9EarlyZ 0 requests no early-Z variant", seed);
                    Check(perClass[c] == 0, what);
                    continue;
                }
                std::snprintf(what, sizeof(what), "seed %u ez %d: the stream requests %s variants", seed, ezOn,
                              kClassNames[c]);
                Check(perClass[c] > 0, what);
                // Negative control: the same plan without this key class
                // must be caught missing what the stream requested.
                VariantPlan control;
                for (const PlannedVariant &v : plan.Variants())
                    if (!InClass(v, (KeyClass)c))
                        control.AddVariant(v);
                control.Finish();
                std::snprintf(what, sizeof(what), "seed %u ez %d: omitting %s variants is detected", seed, ezOn,
                              kClassNames[c]);
                Check(CountMissing(control, requested) > 0, what);
            }
        }
    }
}


#ifdef DEKO9_VARIANT_CENSUS
// ---- census over retail zones (optional) ----------------------------------------

struct CorpusShader
{
    bool ok = false;
    Deko9ShaderInfo info{};
    std::vector<uint8_t> bytes;
};

const CorpusShader &LoadCorpus(std::map<std::pair<uint64_t, int>, CorpusShader> &cache, const char *corpus,
                               uint64_t hash, Deko9Stage stage, uint32_t *missing)
{
    CorpusShader &entry = cache[{hash, (int)stage}];
    if (entry.ok || !entry.bytes.empty())
        return entry;
    char path[1024];
    std::snprintf(path, sizeof(path), "%s/%016llx.%s", corpus, (unsigned long long)hash,
                  stage == DEKO9_STAGE_PIXEL ? "ps" : "vs");
    FILE *file = std::fopen(path, "rb");
    if (!file)
    {
        ++*missing;
        entry.bytes.push_back(0); // remembered as missing
        return entry;
    }
    std::fseek(file, 0, SEEK_END);
    entry.bytes.resize((size_t)std::ftell(file));
    std::rewind(file);
    const bool read = std::fread(entry.bytes.data(), 1, entry.bytes.size(), file) == entry.bytes.size();
    std::fclose(file);
    std::string glsl, error;
    entry.ok = read && Deko9_TranslateShader(entry.bytes.data(), entry.bytes.size(), 0, &glsl, &entry.info, &error);
    if (!entry.ok)
        std::printf("DEKO9_VARIANT_CENSUS translate failed %s: %s\n", path, error.c_str());
    return entry;
}

uint32_t EnvU32(const char *name, uint32_t fallback)
{
    const char *v = std::getenv(name);
    return v ? (uint32_t)std::strtoul(v, nullptr, 0) : fallback;
}

// Every "first bind" key of a recorded run (vs/ps hash, mask, instance
// count, early-Z) must be planned for that map.
void CheckRecorded(const char *label, const VariantPlan &plan, const char *logPath)
{
    FILE *log = std::fopen(logPath, "r");
    if (!log)
    {
        Check(false, "census: recorded log unreadable");
        return;
    }
    char line[4096];
    uint32_t keys = 0, missing = 0;
    std::set<std::string> seen;
    while (std::fgets(line, sizeof(line), log))
    {
        const char *at = std::strstr(line, "DEKO9 first bind ");
        if (!at)
            continue;
        unsigned long long vs = 0, ps = 0;
        unsigned mask = 0, inst = 0, ez = 0;
        const char *v = std::strstr(at, " vs=");
        if (!v || std::sscanf(v, " vs=%llx ps=%llx mask=0x%x inst=%u ez=%u", &vs, &ps, &mask, &inst, &ez) != 5)
            continue;
        char key[128];
        std::snprintf(key, sizeof(key), "%llx %llx %x %u %u", vs, ps, mask, inst, ez);
        if (!seen.insert(key).second)
            continue;
        ++keys;
        bool vsFound = false, psFound = false;
        for (const PlannedVariant &p : plan.Variants())
        {
            if (p.stage == DEKO9_STAGE_VERTEX && p.shader == vs && p.select.instance.count == inst)
                vsFound = true;
            if (p.stage == DEKO9_STAGE_PIXEL && p.shader == ps && p.select.shadowMask == mask &&
                p.select.earlyZ == (ez != 0))
                psFound = true;
        }
        if (!vsFound || !psFound)
        {
            ++missing;
            std::printf("DEKO9_VARIANT_CENSUS map=%s recorded key not planned: %s%s%s", label, key,
                        vsFound ? "" : " (vs)", psFound ? "\n" : " (ps)\n");
        }
    }
    std::fclose(log);
    std::printf("DEKO9_VARIANT_CENSUS map=%s recorded_keys=%u recorded_missing=%u\n", label, keys, missing);
    Check(keys > 0, "census: the recorded log has first-bind keys");
    Check(missing == 0, "census: every recorded first-bind key is planned");
}

void RunCensus(const char *passLists, const char *corpus)
{
    PlanOptions options;
    options.earlyZ = EnvU32("KISAK_DEKO_VARIANT_EARLYZ", 1) != 0;
    options.shadowFilter = EnvU32("KISAK_DEKO_VARIANT_SHADOW_FILTER", 0);
    options.shaderOpt = EnvU32("KISAK_DEKO_VARIANT_SHADER_OPT", 0);
    const bool build = EnvU32("KISAK_DEKO_VARIANT_BUILD", 0) != 0;
    const uint32_t cap = EnvU32("KISAK_DEKO_VARIANT_CAP", 4096);
    const char *recordedMap = std::getenv("KISAK_DEKO_VARIANT_RECORDED_MAP");
    const char *recorded = std::getenv("KISAK_DEKO_VARIANT_RECORDED");
    std::map<std::pair<uint64_t, int>, CorpusShader> cache;
    std::string lists = passLists;
    size_t start = 0;
    while (start <= lists.size())
    {
        size_t end = lists.find(':', start);
        if (end == std::string::npos)
            end = lists.size();
        std::string item = lists.substr(start, end - start);
        start = end + 1;
        if (item.empty())
            continue;
        std::string label = item, path = item;
        const size_t eq = item.find('=');
        if (eq != std::string::npos)
            label = item.substr(0, eq), path = item.substr(eq + 1);
        FILE *file = std::fopen(path.c_str(), "r");
        if (!file)
        {
            Check(false, "census: pass list unreadable");
            continue;
        }
        VariantPlan plan;
        std::set<uint64_t> vsSet, psSet;
        uint32_t passes = 0, missing = 0, untranslated = 0;
        char line[512];
        while (std::fgets(line, sizeof(line), file))
        {
            unsigned long long vs = 0, ps = 0;
            unsigned depth = 0, atest = 0, inst = 0;
            char regs[128] = "";
            if (std::sscanf(line, "DEKO_VARIANT_PASS vs=%llx ps=%llx depth=0x%x atest=%u inst=%u:%127s", &vs, &ps,
                            &depth, &atest, &inst, regs) < 5)
                continue;
            ++passes;
            PassVariantDesc desc;
            desc.vsId = vs;
            desc.psId = ps;
            if (vs)
            {
                const CorpusShader &c = LoadCorpus(cache, corpus, vs, DEKO9_STAGE_VERTEX, &missing);
                desc.vs = c.ok ? &c.info : nullptr;
                untranslated += !c.ok;
                vsSet.insert(vs);
            }
            if (ps)
            {
                const CorpusShader &c = LoadCorpus(cache, corpus, ps, DEKO9_STAGE_PIXEL, &missing);
                desc.ps = c.ok ? &c.info : nullptr;
                untranslated += !c.ok;
                psSet.insert(ps);
            }
            desc.depthSamplerMask = depth;
            desc.alphaTest = atest != 0;
            uint8_t r[16];
            uint32_t n = 0;
            for (const char *p = regs; *p && n < 16;)
            {
                r[n++] = (uint8_t)std::strtoul(p, nullptr, 10);
                p = std::strchr(p, ',');
                if (!p)
                    break;
                ++p;
            }
            if (inst && n == inst)
                LayoutFromRegs(r, n, &desc.instance);
            plan.Add(desc, options);
        }
        std::fclose(file);
        const std::vector<PlannedVariant> &all = plan.Finish();
        uint32_t compare = 0, earlyZ = 0, instanced = 0, extra = 0;
        for (const PlannedVariant &v : all)
        {
            compare += v.select.shadowMask != 0;
            earlyZ += v.select.earlyZ;
            instanced += v.select.instance.count != 0;
            extra += v.select.shadowMask || v.select.earlyZ || v.select.instance.count;
        }
        std::printf("DEKO9_VARIANT_CENSUS map=%s passes=%u vs=%zu ps=%zu planned=%zu base=%zu extra=%u compare=%u "
                    "earlyz=%u instanced=%u corpus_missing=%u untranslated=%u\n",
                    label.c_str(), passes, vsSet.size(), psSet.size(), all.size(), all.size() - extra, extra, compare,
                    earlyZ, instanced, missing, untranslated);
        Check(passes > 0, "census: pass list is empty");
        Check(missing == 0 && untranslated == 0, "census: every pass shader is in the corpus and translates");
        Check(all.size() <= cap, "census: planned variants exceed the cap");
        if (build)
        {
            // The extra variants are what the prebake compiles on a cold
            // pack; base variants already compile at shader creation.
            double totalMs = 0, maxMs = 0;
            uint32_t built = 0, failed = 0;
            for (const PlannedVariant &v : all)
            {
                if (!(v.select.shadowMask || v.select.earlyZ || v.select.instance.count))
                    continue;
                const CorpusShader &c = cache[{v.shader, (int)v.stage}];
                std::string glsl, error;
                Deko9ShaderInfo info{};
                std::vector<uint8_t> dksh;
                const auto t0 = std::chrono::steady_clock::now();
                const bool ok = c.ok &&
                                Deko9_TranslateShader(c.bytes.data(), c.bytes.size(), v.select.shadowMask, &glsl, &info,
                                                      &error, v.select.instance.regs, v.select.instance.count,
                                                      v.select.earlyZ, v.select.shadowFilter, v.select.shaderOpt) &&
                                Deko9_CompileDksh(v.stage, glsl, &dksh, &error);
                const double ms =
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                totalMs += ms;
                maxMs = ms > maxMs ? ms : maxMs;
                if (ok)
                    ++built;
                else
                {
                    ++failed;
                    std::printf("DEKO9_VARIANT_CENSUS map=%s build failed %016llx: %s\n", label.c_str(),
                                (unsigned long long)v.shader, error.c_str());
                }
            }
            std::printf("DEKO9_VARIANT_CENSUS map=%s built=%u failed=%u host_ms=%.0f host_max_ms=%.1f\n",
                        label.c_str(), built, failed, totalMs, maxMs);
            Check(failed == 0, "census: every planned variant builds");
        }
        if (recorded && recordedMap && label == recordedMap)
            CheckRecorded(label.c_str(), plan, recorded);
    }
}
#endif

} // namespace

int main()
{
    TestRules();
    TestSyntheticStream();
#ifdef DEKO9_VARIANT_CENSUS
    const char *passLists = std::getenv("KISAK_DEKO_VARIANT_PASSES");
    const char *corpus = std::getenv("KISAK_DEKO_SHADER_CORPUS");
    if (passLists && corpus)
        RunCensus(passLists, corpus);
#endif
    if (g_failures)
    {
        std::printf("FAIL:DEKO9_VARIANT_PLAN %d check(s)\n", g_failures);
        return 1;
    }
    std::printf("PASS:DEKO9_VARIANT_PLAN rules, synthetic stream superset, per-class negative controls\n");
    return 0;
}
