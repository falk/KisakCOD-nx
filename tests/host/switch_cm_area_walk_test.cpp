// Host proof for src/qcommon/cm_area_walk.h (CM_AreaEntities_r's batched
// chain walk and vector box test). ./test runs it twice: on x86 under
// ASan/UBSan and, built for AArch64 (NEON box test), under qemu-aarch64.
//
// 1. CM_AreaBoxOverlaps against the retail six-compare expression, on
//    random boxes salted with shared edges, +/-0, infinities and NaNs.
// 2. The retail CM_AreaEntities_r (copied below over test-local sectors and
//    entities) against the same function rewritten on CM_WalkEntityChain +
//    CM_AreaBoxOverlaps as cm_world.cpp now is: identical list, count,
//    node/candidate counters and MAXCOUNT early exit on random trees.

#include <qcommon/cm_area_walk.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

namespace
{
int g_failures;

void Check(bool ok, const char *what)
{
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL:CM_AREA_WALK %s\n", what);
    }
}

float SpecialOr(std::mt19937 &rng, float v)
{
    switch (rng() % 16)
    {
    case 0:
        return std::numeric_limits<float>::quiet_NaN();
    case 1:
        return std::numeric_limits<float>::infinity();
    case 2:
        return -std::numeric_limits<float>::infinity();
    case 3:
        return 0.0f;
    case 4:
        return -0.0f;
    default:
        return v;
    }
}

void BoxTest()
{
    std::mt19937 rng(0xB0C5u);
    std::uniform_real_distribution<float> coord(-100.0f, 100.0f);
    int mismatches = 0, overlaps = 0;
    for (int i = 0; i < 200000; ++i)
    {
        float mins[3], maxs[3];
        float ent[8]; // absmin[3], absmax[3], then two trailing fields
        for (int a = 0; a < 3; ++a)
        {
            mins[a] = coord(rng);
            maxs[a] = mins[a] + std::fabs(coord(rng)) * 0.5f;
            ent[a] = coord(rng);
            ent[3 + a] = ent[a] + std::fabs(coord(rng)) * 0.5f;
            if (rng() % 8 == 0)
                ent[a] = maxs[a]; // touching edges
            if (rng() % 8 == 0)
                ent[3 + a] = mins[a];
            if (rng() % 4 == 0)
            {
                mins[a] = SpecialOr(rng, mins[a]);
                maxs[a] = SpecialOr(rng, maxs[a]);
                ent[a] = SpecialOr(rng, ent[a]);
                ent[3 + a] = SpecialOr(rng, ent[3 + a]);
            }
        }
        ent[6] = SpecialOr(rng, coord(rng)); // lanes past absmin/absmax must not matter
        ent[7] = SpecialOr(rng, coord(rng));
        const float *absmin = &ent[0], *absmax = &ent[3];
        const bool retail = maxs[0] >= absmin[0] && mins[0] <= absmax[0] && maxs[1] >= absmin[1] &&
                            mins[1] <= absmax[1] && maxs[2] >= absmin[2] && mins[2] <= absmax[2];
        const CmAreaQueryBox box = CM_MakeAreaQueryBox(mins, maxs);
        if (CM_AreaBoxOverlaps(box, absmin, absmax) != retail)
            ++mismatches;
        overlaps += retail;
    }
    std::printf("CM_AREA_WALK box cases=200000 overlaps=%d mismatches=%d\n", overlaps, mismatches);
    Check(mismatches == 0, "CM_AreaBoxOverlaps differs from the retail compare");
    Check(overlaps > 1000, "box cases barely overlap (weak coverage)");
}

// Test-local world: the fields CM_AreaEntities_r reads.
struct Sector
{
    int contentsEntities;
    uint16_t entities;
    float dist;
    uint16_t axis;
    uint16_t child[2];
};
struct SvEnt
{
    uint16_t next;
};
struct GEnt
{
    int contents;
    float absmin[3];
    float absmax[3];
    float trailing[3];
};
struct World
{
    std::vector<Sector> sectors;
    std::vector<SvEnt> svEnts;
    std::vector<GEnt> gents;
};
struct Parms
{
    const float *mins;
    const float *maxs;
    int *list;
    int count;
    int maxcount;
    int contentmask;
    int nodes;
    int tested;
    int maxcountHits;
};

// Retail CM_AreaEntities_r (cm_world.cpp before the batched walk).
void RetailArea(const World &w, uint32_t nodeIndex, Parms *ap)
{
    for (const Sector *node = &w.sectors[nodeIndex]; (node->contentsEntities & ap->contentmask) != 0;
         node = &w.sectors[nodeIndex])
    {
        ++ap->nodes;
        const SvEnt *svEnt;
        for (uint32_t entnum = node->entities; entnum; entnum = svEnt->next)
        {
            ++ap->tested;
            svEnt = &w.svEnts[entnum - 1];
            const GEnt *gcheck = &w.gents[svEnt - w.svEnts.data()];
            if ((ap->contentmask & gcheck->contents) != 0 && ap->maxs[0] >= gcheck->absmin[0] &&
                ap->mins[0] <= gcheck->absmax[0] && ap->maxs[1] >= gcheck->absmin[1] &&
                ap->mins[1] <= gcheck->absmax[1] && ap->maxs[2] >= gcheck->absmin[2] &&
                ap->mins[2] <= gcheck->absmax[2])
            {
                if (ap->count >= ap->maxcount)
                {
                    ++ap->maxcountHits;
                    return;
                }
                ap->list[ap->count] = (int)(svEnt - w.svEnts.data());
                ap->count++;
            }
        }
        if (node->dist >= ap->maxs[node->axis])
        {
            if (node->dist <= ap->mins[node->axis])
                return;
            nodeIndex = node->child[1];
        }
        else if (node->dist <= ap->mins[node->axis])
        {
            nodeIndex = node->child[0];
        }
        else
        {
            const uint32_t next = node->child[1];
            RetailArea(w, node->child[0], ap);
            nodeIndex = next;
        }
    }
}

// The same shape as the new cm_world.cpp CM_AreaEntities_r.
void BatchedArea(const World &w, uint32_t nodeIndex, Parms *ap, std::vector<uint32_t> *prefetched)
{
    const CmAreaQueryBox box = CM_MakeAreaQueryBox(ap->mins, ap->maxs);
    for (const Sector *node = &w.sectors[nodeIndex]; (node->contentsEntities & ap->contentmask) != 0;
         node = &w.sectors[nodeIndex])
    {
        int tested = 0;
        const bool completed = CM_WalkEntityChain<16>(
            node->entities, [&](uint32_t entnum) -> uint32_t { return w.svEnts[entnum - 1].next; },
            [&](uint32_t entnum) { prefetched->push_back(entnum); },
            [&](uint32_t entnum) -> bool {
                ++tested;
                const GEnt *gcheck = &w.gents[entnum - 1];
                if ((ap->contentmask & gcheck->contents) != 0 && CM_AreaBoxOverlaps(box, gcheck->absmin, gcheck->absmax))
                {
                    if (ap->count >= ap->maxcount)
                    {
                        ++ap->maxcountHits;
                        return false;
                    }
                    ap->list[ap->count] = (int)(entnum - 1);
                    ap->count++;
                }
                return true;
            });
        ++ap->nodes;
        ap->tested += tested;
        if (!completed)
            return;
        if (node->dist >= ap->maxs[node->axis])
        {
            if (node->dist <= ap->mins[node->axis])
                return;
            nodeIndex = node->child[1];
        }
        else if (node->dist <= ap->mins[node->axis])
        {
            nodeIndex = node->child[0];
        }
        else
        {
            const uint32_t next = node->child[1];
            BatchedArea(w, node->child[0], ap, prefetched);
            nodeIndex = next;
        }
    }
}

// A random kd-ish tree: sector 0 is the null sector (contents 0), 1 is the
// head, children point at higher indices; entities are spread over chains.
World RandomWorld(std::mt19937 &rng)
{
    World w;
    const int sectors = 2 + (int)(rng() % 60);
    const int ents = 1 + (int)(rng() % 400);
    std::uniform_real_distribution<float> coord(-500.0f, 500.0f), size(0.0f, 300.0f);
    w.sectors.resize(sectors + 1);
    w.sectors[0] = {};
    for (int s = 1; s <= sectors; ++s)
    {
        Sector &n = w.sectors[s];
        n.contentsEntities = (int)(rng() & 0x7);
        n.axis = (uint16_t)(rng() % 3);
        n.dist = coord(rng);
        for (int c = 0; c < 2; ++c)
            n.child[c] = (s * 2 + c <= sectors && rng() % 5) ? (uint16_t)(s * 2 + c) : 0;
        n.entities = 0;
    }
    w.svEnts.resize(ents);
    w.gents.resize(ents);
    for (int e = ents; e >= 1; --e) // push-front so chains mix index orders
    {
        const int s = 1 + (int)(rng() % sectors);
        w.svEnts[e - 1].next = w.sectors[s].entities;
        w.sectors[s].entities = (uint16_t)e;
        GEnt &g = w.gents[e - 1];
        g.contents = (int)(rng() & 0xF);
        for (int a = 0; a < 3; ++a)
        {
            g.absmin[a] = coord(rng);
            g.absmax[a] = g.absmin[a] + size(rng);
            g.trailing[a] = coord(rng);
        }
        if (rng() % 50 == 0)
            g.absmin[rng() % 3] = std::numeric_limits<float>::quiet_NaN();
    }
    return w;
}

// CM_UnlinkEntity's per-node contents rebuild: retail do/while chain loop
// against the batched walk (the walk must see every chain entry once, in
// order; the position-dependent shift makes order visible).
void ContentsRebuildTest()
{
    std::mt19937 rng(0xC047u);
    int mismatches = 0;
    for (int round = 0; round < 2000; ++round)
    {
        const World w = RandomWorld(rng);
        for (size_t s = 1; s < w.sectors.size(); ++s)
        {
            int retailContents = 0, batchedContents = 0, retailCount = 0, batchedCount = 0;
            if (w.sectors[s].entities)
            {
                const SvEnt *scan = &w.svEnts[w.sectors[s].entities - 1];
                for (;;)
                {
                    retailContents |= w.gents[scan - w.svEnts.data()].contents << (retailCount % 8);
                    ++retailCount;
                    if (!scan->next)
                        break;
                    scan = &w.svEnts[scan->next - 1];
                }
            }
            CM_WalkEntityChain<16>(
                w.sectors[s].entities, [&](uint32_t entnum) -> uint32_t { return w.svEnts[entnum - 1].next; },
                [](uint32_t) {},
                [&](uint32_t entnum) -> bool {
                    batchedContents |= w.gents[entnum - 1].contents << (batchedCount % 8);
                    ++batchedCount;
                    return true;
                });
            mismatches += retailContents != batchedContents || retailCount != batchedCount;
        }
    }
    Check(mismatches == 0, "batched contents rebuild differs from the retail chain loop");
}

// CM_SortNode's static-model loop: the caller relinks every visited entry
// into one of two child chains (push-front, as CM_AddStaticModelToNode) or
// keeps it. The lookahead must prefetch exactly the original chain's entries
// in order (each once, never past the end) and not disturb the walk.
void LookaheadTest()
{
    std::mt19937 rng(0x100Au);
    int bad = 0;
    for (int round = 0; round < 5000; ++round)
    {
        const int count = (int)(rng() % 60);
        std::vector<uint16_t> next(count + 1, 0);
        std::vector<uint32_t> original;
        uint32_t head = 0;
        for (int e = count; e >= 1; --e)
        {
            if (rng() % 4 == 0)
                continue; // not in this chain
            next[e] = (uint16_t)head;
            head = (uint32_t)e;
        }
        for (uint32_t e = head; e; e = next[e])
            original.push_back(e);
        uint32_t childHead[2] = {0, 0};
        std::vector<uint32_t> prefetched, visited;
        auto lookahead = CM_MakeChainLookahead<8>(
            head, [&](uint32_t e) -> uint32_t { return next[e]; }, [&](uint32_t e) { prefetched.push_back(e); });
        uint32_t prev = 0, keptHead = head;
        for (uint32_t e = head; e;)
        {
            lookahead.Advance();
            visited.push_back(e);
            const uint32_t following = next[e];
            const unsigned choice = rng() % 3;
            if (choice < 2)
            {
                next[e] = (uint16_t)childHead[choice]; // relink into a child
                childHead[choice] = e;
                if (prev)
                    next[prev] = (uint16_t)following;
                else
                    keptHead = following;
            }
            else
            {
                prev = e;
            }
            e = following;
        }
        (void)keptHead;
        bad += visited != original || prefetched != original;
    }
    Check(bad == 0, "chain lookahead prefetched off the original chain or disturbed the walk");
}

void AreaTest()
{
    std::mt19937 rng(0xA2EAu);
    int queries = 0, hits = 0, earlyExits = 0;
    for (int round = 0; round < 3000; ++round)
    {
        const World w = RandomWorld(rng);
        std::uniform_real_distribution<float> coord(-500.0f, 500.0f), size(0.0f, 1000.0f);
        float mins[3], maxs[3];
        for (int a = 0; a < 3; ++a)
        {
            mins[a] = coord(rng);
            maxs[a] = mins[a] + size(rng);
        }
        const int maxcount = (rng() % 3 == 0) ? (int)(rng() % 8) : 1024;
        std::vector<int> listA(1024, -1), listB(1024, -1);
        Parms a{mins, maxs, listA.data(), 0, maxcount, (int)(1 + rng() % 7), 0, 0, 0};
        Parms b = a;
        b.list = listB.data();
        std::vector<uint32_t> prefetched;
        RetailArea(w, 1, &a);
        BatchedArea(w, 1, &b, &prefetched);
        ++queries;
        hits += a.count;
        earlyExits += a.maxcountHits;
        const bool same = a.count == b.count && a.nodes == b.nodes && a.tested == b.tested &&
                          a.maxcountHits == b.maxcountHits && listA == listB;
        if (!same)
            std::printf("  round %d retail count=%d nodes=%d tested=%d batched count=%d nodes=%d tested=%d\n", round,
                        a.count, a.nodes, a.tested, b.count, b.nodes, b.tested);
        Check(same, "batched CM_AreaEntities_r differs from retail");
        for (uint32_t e : prefetched)
            Check(e >= 1 && e <= w.svEnts.size(), "prefetch outside the entity arrays");
    }
    std::printf("CM_AREA_WALK area queries=%d hits=%d maxcount_exits=%d\n", queries, hits, earlyExits);
    Check(hits > 1000 && earlyExits > 100, "area cases lack hits or MAXCOUNT exits (weak coverage)");
}
} // namespace

int main()
{
    BoxTest();
    AreaTest();
    ContentsRebuildTest();
    LookaheadTest();
    if (g_failures)
    {
        std::printf("CM_AREA_WALK failures=%d\n", g_failures);
        return 1;
    }
#if defined(__aarch64__)
    std::printf("PASS:CM_AREA_WALK neon\n");
#else
    std::printf("PASS:CM_AREA_WALK\n");
#endif
    return 0;
}
