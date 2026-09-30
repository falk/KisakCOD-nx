// Host proofs for the task/hot-engine2 CPU hot-path slice: every rewritten
// routine against a verbatim copy of the code it replaced, over randomized
// inputs. Built and run by ./test host under
// ASan/UBSan (hot_engine2_check), then for AArch64 under qemu.

#include <universal/name_ptr_cache.h>
#include <universal/q_stricmp.h>
#include <platform/switch/switch_wake_flag.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <algorithm>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static int g_failures;
#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            if (g_failures++ < 20)                                                                                    \
            {                                                                                                         \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                      \
                std::printf(__VA_ARGS__);                                                                             \
                std::printf("\n");                                                                                    \
            }                                                                                                         \
        }                                                                                                             \
    } while (0)

// ---- 1. I_stricmp / I_strnicmp ---------------------------------------------

namespace ref
{
static bool I_isupper(int c)
{
    return c >= 'A' && c <= 'Z';
}
// Verbatim from universal/q_shared.cpp before this slice.
static int I_strnicmp(const char *s0, const char *s1, int n)
{
    int c1;
    int c0;

    do
    {
        c0 = *(uint8_t *)s0;
        c1 = *(uint8_t *)s1;
        ++s0;
        ++s1;
        if (!n--)
            return 0;
        if (c0 != c1)
        {
            if (I_isupper(c0))
                c0 += 32;
            if (I_isupper(c1))
                c1 += 32;
            if (c0 != c1)
                return 2 * (c0 >= c1) - 1;
        }
    } while (c0);
    return 0;
}
static int I_stricmp(const char *s0, const char *s1)
{
    return I_strnicmp(s0, s1, 0x7FFFFFFF);
}
} // namespace ref

static std::string RandomName(std::mt19937 &rng, int maxLen)
{
    // Mostly identifier characters with case variants, plus the fold
    // boundaries ('@', '[', '`', '{'), '/', '\\' and high bytes.
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_/\\@[`{ .";
    std::string s;
    const int len = (int)(rng() % (unsigned)(maxLen + 1));
    for (int i = 0; i < len; ++i)
    {
        const unsigned r = rng() % 100;
        if (r < 3)
            s.push_back((char)(0x80 + rng() % 0x80));
        else
            s.push_back(alphabet[rng() % (sizeof(alphabet) - 1)]);
    }
    return s;
}

static std::string Mutate(std::mt19937 &rng, std::string s)
{
    // Case flips everywhere, plus sometimes one changed/removed/added byte.
    for (char &c : s)
    {
        if (rng() % 3 == 0)
        {
            if (c >= 'a' && c <= 'z')
                c = (char)(c - 32);
            else if (c >= 'A' && c <= 'Z')
                c = (char)(c + 32);
        }
    }
    switch (rng() % 5)
    {
    case 0:
        if (!s.empty())
            s[rng() % s.size()] = (char)(1 + rng() % 255);
        break;
    case 1:
        if (!s.empty())
            s.erase(rng() % s.size(), 1);
        break;
    case 2:
        s.insert(s.begin() + (long)(rng() % (s.size() + 1)), (char)(1 + rng() % 255));
        break;
    default:
        break;
    }
    return s;
}

static int Sign(int v)
{
    return (v > 0) - (v < 0);
}

static void TestStricmp()
{
    std::mt19937 rng(0x51c0de);
    long checks = 0;
    for (int iter = 0; iter < 400000; ++iter)
    {
        const std::string a = RandomName(rng, 40);
        const std::string b = (rng() % 4) ? Mutate(rng, a) : RandomName(rng, 40);
        const int want = ref::I_stricmp(a.c_str(), b.c_str());
        const int got = qstr::Icmp(a.c_str(), b.c_str());
        CHECK(want == got, "stricmp(\"%s\", \"%s\") ref %d new %d", a.c_str(), b.c_str(), want, got);
        const int ns[] = {0, 1, 2, 3, 7, (int)(rng() % 48), 64, 0x7FFFFFFF, -1};
        for (int n : ns)
        {
            const int wn = ref::I_strnicmp(a.c_str(), b.c_str(), n);
            const int gn = qstr::Nicmp(a.c_str(), b.c_str(), n);
            CHECK(wn == gn, "strnicmp(\"%s\", \"%s\", %d) ref %d new %d", a.c_str(), b.c_str(), n, wn, gn);
            ++checks;
        }
        // Symmetry and exact -1/0/+1 values (callers test `< 0` and `!`).
        CHECK(got >= -1 && got <= 1 && Sign(got) == -Sign(qstr::Icmp(b.c_str(), a.c_str())), "sign");
        ++checks;
    }
    // Every byte pair once, as one-character strings.
    for (int x = 0; x < 256; ++x)
    {
        for (int y = 0; y < 256; ++y)
        {
            const char a[2] = {(char)x, 0};
            const char b[2] = {(char)y, 0};
            CHECK(ref::I_stricmp(a, b) == qstr::Icmp(a, b), "byte pair %d %d", x, y);
            CHECK(ref::I_strnicmp(a, b, 1) == qstr::Nicmp(a, b, 1), "byte pair n1 %d %d", x, y);
            checks += 2;
        }
    }
    std::printf("stricmp: %ld checks\n", checks);
}

// ---- 2. Material_HasAnyFogableTechnique's "2d" name test ---------------------
// (r_rendercmds.cpp). RB_DrawText now asks it once per string instead of per
// glyph; the materials are `const Material *const` / set once before the
// glyph loop, so the compiler holds that part. Here: the name test without
// the redundant strcmp.

static void TestFogable2dName()
{
    std::mt19937 rng(0x2d2d);
    long checks = 0;
    const char *fixed[] = {"", "2", "d", "2d", "2D", "d2", "22d", "2dd", "sm2/2d", "2d_font", "wc_l_sm", "mc_l_2d"};
    for (const char *n : fixed)
    {
        const bool want = std::strcmp(n, "2d") == 0 || std::strstr(n, "2d") != nullptr;
        CHECK(want == (std::strstr(n, "2d") != nullptr), "2d name \"%s\"", n);
        ++checks;
    }
    for (int iter = 0; iter < 200000; ++iter)
    {
        std::string n;
        const int len = (int)(rng() % 12);
        for (int i = 0; i < len; ++i)
            n.push_back("2dD_ls/"[rng() % 7]);
        const bool want = std::strcmp(n.c_str(), "2d") == 0 || std::strstr(n.c_str(), "2d") != nullptr;
        CHECK(want == (std::strstr(n.c_str(), "2d") != nullptr), "2d name \"%s\"", n.c_str());
        ++checks;
    }
    std::printf("fogable 2d name: %ld checks\n", checks);
}

// ---- 3. NamePtrCache (universal/name_ptr_cache.h) ---------------------------
// A model registry with the semantics of the two users: an ordered chain
// searched case-insensitively, first match wins (DB hash chains, where a
// case-variant duplicate or an override in front can exist), entries that
// are added in front, removed (freed) and renamed, and a generation bumped
// by every mutation (db_registry.cpp's write-locked sections). Queries come
// from a small pool of stable name buffers whose contents are sometimes
// rewritten in place (a freed and reused config string). The memoized
// lookup, written exactly as Com_FindSoundAlias_FastFile / Dvar_
// FindMalleableVar use it, must return what the plain chain walk returns
// after every step; ASan catches a hit that touched a freed entry.

struct ModelEntry
{
    char *name;
    int id;
};

struct ModelRegistry
{
    std::vector<ModelEntry *> chain;
    uint32_t generation = 1;
    bool bumpOnMutation = true;

    void Bump()
    {
        if (bumpOnMutation && ++generation == 0)
            generation = 1;
    }
    ModelEntry *Slow(const char *name) const
    {
        for (ModelEntry *e : chain)
        {
            if (!qstr::Icmp(e->name, name))
                return e;
        }
        return nullptr;
    }
    static char *Dup(const std::string &s)
    {
        char *p = new char[s.size() + 1];
        std::memcpy(p, s.c_str(), s.size() + 1);
        return p;
    }
    ~ModelRegistry()
    {
        for (ModelEntry *e : chain)
        {
            delete[] e->name;
            delete e;
        }
    }
};

static const char *ModelName(ModelEntry *e)
{
    return e->name;
}

static void TestNamePtrCache(bool uniqueNames, bool expectFailures)
{
    std::mt19937 rng(uniqueNames ? 0xd7a2 : 0x5a1a5);
    ModelRegistry reg;
    reg.bumpOnMutation = !expectFailures;
    NamePtrCache<ModelEntry, 64> cache; // small: forces slot collisions
    std::vector<std::string> vocabulary;
    for (int i = 0; i < 40; ++i)
        vocabulary.push_back(RandomName(rng, 10) + "x");
    auto variant = [&](const std::string &s) { return (rng() % 2) ? Mutate(rng, s) : s; };
    auto caseVariant = [&](std::string s) {
        // Mostly the canonical spelling (as in the game), sometimes a case
        // variant (which must then miss the memo or match correctly).
        if (rng() % 8)
            return s;
        for (char &c : s)
        {
            if ((c >= 'a' && c <= 'z' && rng() % 2) || (c >= 'A' && c <= 'Z' && rng() % 2))
                c = (char)(c ^ 0x20);
        }
        return s;
    };
    // Stable query buffers (config strings / literals).
    std::vector<std::vector<char>> queries(24, std::vector<char>(64, 0));
    auto setQuery = [&](std::vector<char> &q, const std::string &s) {
        std::memset(q.data(), 0, q.size());
        std::memcpy(q.data(), s.c_str(), std::min(s.size(), q.size() - 1));
    };
    for (auto &q : queries)
        setQuery(q, caseVariant(vocabulary[rng() % vocabulary.size()]));
    int nextId = 1;
    long lookups = 0, hits = 0, mismatches = 0;
    std::vector<ModelEntry *> graveyard;
    std::vector<char *> graveyardNames;
    for (int step = 0; step < 300000; ++step)
    {
        const unsigned op = rng() % 1000;
        if (op < 8)
        {
            // Add in front (an override / a newly linked entry).
            std::string n = caseVariant(vocabulary[rng() % vocabulary.size()]);
            if (uniqueNames && reg.Slow(n.c_str()))
                continue;
            reg.chain.insert(reg.chain.begin(), new ModelEntry{ModelRegistry::Dup(n), nextId++});
            reg.Bump();
        }
        else if (op < 12 && !reg.chain.empty())
        {
            if (uniqueNames)
                continue; // dvars are never unlinked
            const size_t i = rng() % reg.chain.size();
            // The negative control keeps removed objects alive: without
            // generation bumps its memo reads them, which it must catch by
            // value rather than by crashing.
            if (expectFailures)
            {
                graveyard.push_back(reg.chain[i]);
            }
            else
            {
                delete[] reg.chain[i]->name;
                delete reg.chain[i];
            }
            reg.chain.erase(reg.chain.begin() + (long)i);
            reg.Bump();
        }
        else if (op < 16 && !reg.chain.empty())
        {
            // Rename to a case variant through a new allocation
            // (Dvar_ReinterpretDvar; DB clones). The old name is freed.
            ModelEntry *e = reg.chain[rng() % reg.chain.size()];
            char *old = e->name;
            e->name = ModelRegistry::Dup(caseVariant(old));
            if (expectFailures)
                graveyardNames.push_back(old);
            else
                delete[] old;
            reg.Bump();
        }
        else if (op < 40)
        {
            // A query buffer is reused for another string.
            setQuery(queries[rng() % queries.size()], caseVariant(variant(vocabulary[rng() % vocabulary.size()])));
        }
        else if (op < 42 && uniqueNames)
        {
            cache.Clear(); // Dvar_Shutdown
        }
        else
        {
            const char *name = queries[rng() % queries.size()].data();
            const uint32_t gen = uniqueNames ? 1u : reg.generation;
            ModelEntry *got = nullptr;
            bool hit = false;
            if (gen)
            {
                got = cache.Find(name, gen, ModelName);
                hit = got != nullptr;
            }
            if (!hit)
            {
                got = reg.Slow(name);
                if (got && gen)
                    cache.Store(name, gen, got);
            }
            ModelEntry *want = reg.Slow(name);
            ++lookups;
            hits += hit;
            if (want != got)
            {
                ++mismatches;
                if (!expectFailures)
                    CHECK(want == got, "cache step %d \"%s\": want id %d got id %d (hit %d)", step, name,
                          want ? want->id : 0, got ? got->id : 0, (int)hit);
            }
        }
    }
    for (ModelEntry *e : graveyard)
    {
        delete[] e->name;
        delete e;
    }
    for (char *n : graveyardNames)
        delete[] n;
    if (expectFailures)
    {
        // Negative control: without the generation bump the memo must be
        // caught returning stale answers, or this test proves nothing.
        CHECK(mismatches > 0, "negative control: a memo without generation bumps was not caught");
        std::printf("name cache negative control: %ld stale answers caught\n", mismatches);
    }
    else
    {
        CHECK(hits > lookups / 10, "cache hit rate too low to exercise hits: %ld of %ld", hits, lookups);
        std::printf("name cache (%s): %ld lookups, %ld hits\n", uniqueNames ? "dvar model" : "db model", lookups,
                    hits);
    }
}

// ---- 4. deko9 ApplyTextures slot iteration (deko9_draw.cpp) ----------------
// The scan over [0, limit) testing samplerMask became a walk over the set
// bits of samplerMask & ((1 << limit) - 1). Both loops, with the body's
// `continue` (the cached-slot path) taken at random, must visit the same
// slots in the same order.

static void TestSamplerSlotWalk()
{
    std::mt19937 rng(0xde609);
    long checks = 0;
    for (int iter = 0; iter < 200000; ++iter)
    {
        const uint32_t mask = (iter < 65536) ? (uint32_t)iter : (uint32_t)rng();
        const uint32_t limit = (rng() % 2) ? 4u : 16u; // vertex / fragment stage
        const uint32_t skipSeed = (uint32_t)rng();
        std::vector<uint32_t> want, got;
        uint32_t wantCount = 0, gotCount = 0;
        for (uint32_t s = 0; s < limit; ++s)
        {
            if (!(mask & (1u << s)))
                continue;
            want.push_back(s);
            wantCount = s + 1;
            if (skipSeed & (1u << s))
                continue;
            want.push_back(100 + s);
        }
        for (uint32_t pending = mask & ((1u << limit) - 1u); pending; pending &= pending - 1u)
        {
            const uint32_t s = (uint32_t)__builtin_ctz(pending);
            got.push_back(s);
            gotCount = s + 1;
            if (skipSeed & (1u << s))
                continue;
            got.push_back(100 + s);
        }
        CHECK(want == got && wantCount == gotCount, "slot walk mask %08x limit %u", mask, limit);
        ++checks;
    }
    std::printf("sampler slot walk: %ld checks\n", checks);
}

// ---- 5. WakeFlag (platform/switch/switch_wake_flag.h) ----------------------------
// Replaces Sys_WaitStartDatabase's 1 ms sleep-poll of an atomic flag. Same
// contract as the polled flag / the retail auto-reset event: a Wait returns
// only after a Notify, consumes it, and notifies before a Wait coalesce.

using HostWakeFlag = WakeFlag<std::mutex, std::condition_variable_any>;

static void TestWakeFlag()
{
    long checks = 0;
    // Ping-pong: every notify is consumed exactly once, none is lost.
    {
        HostWakeFlag flag, ack;
        std::atomic<int> consumed{0};
        const int rounds = 20000;
        std::thread waiter([&] {
            for (int i = 0; i < rounds; ++i)
            {
                flag.Wait();
                consumed.fetch_add(1);
                ack.Notify();
            }
        });
        std::mt19937 rng(0x3a4e);
        for (int i = 0; i < rounds; ++i)
        {
            if (rng() % 16 == 0)
                std::this_thread::yield();
            flag.Notify();
            ack.Wait();
            CHECK(consumed.load() == i + 1, "wake round %d consumed %d", i, consumed.load());
            ++checks;
        }
        waiter.join();
    }
    // Coalescing and no spurious return: three notifies wake one Wait; the
    // next Wait blocks until a new notify.
    {
        HostWakeFlag flag;
        flag.Notify();
        flag.Notify();
        flag.Notify();
        flag.Wait(); // returns at once
        std::atomic<bool> returned{false};
        std::thread waiter([&] {
            flag.Wait();
            returned.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(!returned.load(), "Wait returned without a notify after coalesced notifies were consumed");
        flag.Notify();
        waiter.join();
        CHECK(returned.load(), "Wait did not return after notify");
        checks += 2;
    }
    std::printf("wake flag: %ld checks\n", checks);
}

int main()
{
    TestWakeFlag();
    TestSamplerSlotWalk();
    TestStricmp();
    TestFogable2dName();
    TestNamePtrCache(false, false);
    TestNamePtrCache(true, false);
    TestNamePtrCache(false, true);
    if (g_failures)
    {
        std::printf("FAIL:HOT_ENGINE2 %d failures\n", g_failures);
        return 1;
    }
    std::printf("PASS:HOT_ENGINE2\n");
    return 0;
}
