#pragma once

// Shader build accounting of the deko3d renderer (deko9): shader-pack
// lookups, MojoShader translations, UAM compiles, baked program units and
// first binds. A variant that misses the pack is translated and compiled at
// draw time with the device lock held, so every other thread's device call
// waits for it; these counters say how often and for how long.
//
// Window totals (not per frame) are appended to the `DEKO9 perf frames=60`
// line. A miss whose translate+compile time reaches kSlowCompileNs logs one
// line, at most kSlowLogsPerWindow per window; the rest are counted as
// slowUnlogged.
//
// Any thread may add (relaxed atomics). Pure C++ (host-testable:
// switch_deko9_fastpath_test.cpp).

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace deko9
{

struct ShaderBuildWindow
{
    uint64_t packHits, packMisses;
    uint64_t translates, translateNs;
    uint64_t compiles, compileNs, compileMaxNs; // per miss: translate + UAM, retail retry included
    uint64_t bakes, bakeNs, bakeMaxNs, bakesUnlocked;
    uint64_t firstBinds, firstBindBakeNs, firstBindBakeMaxNs;
    uint64_t slowCompiles, slowUnlogged;
    uint64_t drawBuilds; // variants a draw had to build (load-time prebake missed them)
};

class ShaderBuildStats
{
public:
    static constexpr uint64_t kSlowCompileNs = 5000000; // 5 ms
    static constexpr uint64_t kSlowLogsPerWindow = 4;

    void NotePackLookup(bool hit) { Add(hit ? m_packHits : m_packMisses, 1); }
    void NoteTranslate(uint64_t ns)
    {
        Add(m_translates, 1);
        Add(m_translateNs, ns);
    }
    // One pack miss built (translate + compile). True when the caller should
    // log it as a slow compile (over the threshold and within the window's
    // log budget).
    bool NoteCompile(uint64_t ns)
    {
        Add(m_compiles, 1);
        Add(m_compileNs, ns);
        Max(m_compileMaxNs, ns);
        if (ns < kSlowCompileNs)
            return false;
        if (m_slowCompiles.fetch_add(1, std::memory_order_relaxed) < kSlowLogsPerWindow)
            return true;
        Add(m_slowUnlogged, 1);
        return false;
    }
    // A new program unit baked (both variants looked up or built); `locked`
    // is whether the caller held the device lock throughout.
    void NoteBake(uint64_t ns, bool locked)
    {
        Add(m_bakes, 1);
        Add(m_bakeNs, ns);
        Max(m_bakeMaxNs, ns);
        if (!locked)
            Add(m_bakesUnlocked, 1);
    }
    // A variant built for a draw because no load-time prebake installed it.
    void NoteDrawBuild() { Add(m_drawBuilds, 1); }
    // A first bind of a variant; bakeNs is the device-lock time the same draw
    // spent baking its unit (0 when the unit already existed).
    void NoteFirstBind(uint64_t bakeNs)
    {
        Add(m_firstBinds, 1);
        Add(m_firstBindBakeNs, bakeNs);
        Max(m_firstBindBakeMaxNs, bakeNs);
    }

    ShaderBuildWindow Take()
    {
        ShaderBuildWindow w;
        w.packHits = Swap(m_packHits);
        w.packMisses = Swap(m_packMisses);
        w.translates = Swap(m_translates);
        w.translateNs = Swap(m_translateNs);
        w.compiles = Swap(m_compiles);
        w.compileNs = Swap(m_compileNs);
        w.compileMaxNs = Swap(m_compileMaxNs);
        w.bakes = Swap(m_bakes);
        w.bakeNs = Swap(m_bakeNs);
        w.bakeMaxNs = Swap(m_bakeMaxNs);
        w.bakesUnlocked = Swap(m_bakesUnlocked);
        w.firstBinds = Swap(m_firstBinds);
        w.firstBindBakeNs = Swap(m_firstBindBakeNs);
        w.firstBindBakeMaxNs = Swap(m_firstBindBakeMaxNs);
        w.slowCompiles = Swap(m_slowCompiles);
        w.slowUnlogged = Swap(m_slowUnlogged);
        w.drawBuilds = Swap(m_drawBuilds);
        return w;
    }

    // " spHits=.. spMisses=.. ... slowUnlogged=.. drawBuilds=.." (leading space), times in us.
    static int Format(const ShaderBuildWindow &w, char *out, size_t size)
    {
        return std::snprintf(out, size,
                             " spHits=%llu spMisses=%llu translates=%llu translateUs=%llu compiles=%llu compileUs=%llu "
                             "compileMaxUs=%llu bakes=%llu bakeUs=%llu bakeMaxUs=%llu bakesUnlocked=%llu firstBinds=%llu "
                             "firstBindBakeUs=%llu firstBindBakeMaxUs=%llu slowCompiles=%llu slowUnlogged=%llu drawBuilds=%llu",
                             U(w.packHits), U(w.packMisses), U(w.translates), U(w.translateNs / 1000), U(w.compiles),
                             U(w.compileNs / 1000), U(w.compileMaxNs / 1000), U(w.bakes), U(w.bakeNs / 1000),
                             U(w.bakeMaxNs / 1000), U(w.bakesUnlocked), U(w.firstBinds), U(w.firstBindBakeNs / 1000),
                             U(w.firstBindBakeMaxNs / 1000), U(w.slowCompiles), U(w.slowUnlogged),
                             U(w.drawBuilds));
    }

private:
    std::atomic<uint64_t> m_packHits{0}, m_packMisses{0}, m_translates{0}, m_translateNs{0};
    std::atomic<uint64_t> m_compiles{0}, m_compileNs{0}, m_compileMaxNs{0};
    std::atomic<uint64_t> m_bakes{0}, m_bakeNs{0}, m_bakeMaxNs{0}, m_bakesUnlocked{0};
    std::atomic<uint64_t> m_firstBinds{0}, m_firstBindBakeNs{0}, m_firstBindBakeMaxNs{0};
    std::atomic<uint64_t> m_slowCompiles{0}, m_slowUnlogged{0}, m_drawBuilds{0};

    static unsigned long long U(uint64_t v) { return (unsigned long long)v; }
    static void Add(std::atomic<uint64_t> &a, uint64_t v) { a.fetch_add(v, std::memory_order_relaxed); }
    static uint64_t Swap(std::atomic<uint64_t> &a) { return a.exchange(0, std::memory_order_relaxed); }
    static void Max(std::atomic<uint64_t> &a, uint64_t v)
    {
        uint64_t cur = a.load(std::memory_order_relaxed);
        while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed))
        {
        }
    }
};

} // namespace deko9
