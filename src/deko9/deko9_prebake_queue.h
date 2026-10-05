#pragma once

// Job queue of the load-time shader variant prebake (pure logic, no deko3d
// or libnx calls, so ./test host drives it with fake shaders and threads:
// switch_deko9_hazards_test.cpp).
//
// The loading thread plans the variants, then it and one build thread (if
// any) build them without the device lock while the loading thread installs
// each result under a short device lock as it arrives. Every shader a job
// names is pinned (one reference per shader object, ShaderPins) from before
// the first build until after the last install: an engine release during
// the unlocked build only drops the engine's reference, and the shader is
// destroyed after the prebake.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace deko9
{

// One reference on each distinct object (AddRef/Release), dropped when the
// pins go out of scope.
template <typename Object> class ShaderPins
{
public:
    ShaderPins() = default;
    ShaderPins(const ShaderPins &) = delete;
    ShaderPins &operator=(const ShaderPins &) = delete;
    ~ShaderPins()
    {
        for (Object *object : m_objects)
            object->Release();
    }
    // The caller guarantees `object` is alive now; it stays alive until
    // the pins are destroyed.
    void Pin(Object *object)
    {
        if (!object || !m_seen.insert(object).second)
            return;
        object->AddRef();
        m_objects.push_back(object);
    }
    size_t Count() const { return m_objects.size(); }

private:
    std::vector<Object *> m_objects;
    std::unordered_set<Object *> m_seen;
};

// Jobs are claimed in order by whoever builds; finished indices queue for
// the installing thread. `build(job)` runs without the device lock and
// returns whether it produced code; `clock()` is in ns.
template <typename Job, typename Build, typename Clock> class PrebakeQueue
{
public:
    PrebakeQueue(std::vector<Job> &jobs, Build build, Clock clock) : m_jobs(jobs), m_build(build), m_clock(clock) {}

    // Builds one job; false when none is left.
    bool BuildOne()
    {
        const uint32_t i = m_next.fetch_add(1, std::memory_order_relaxed);
        if (i >= m_jobs.size())
            return false;
        const uint64_t t0 = m_clock();
        m_jobs[i].ok = m_build(m_jobs[i]);
        m_buildNs.fetch_add(m_clock() - t0, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> guard(m_lock);
            m_finished.push_back(i);
        }
        m_ready.notify_one();
        return true;
    }

    // Build thread body: everything that is left.
    void BuildAll()
    {
        while (BuildOne())
        {
        }
    }

    // Installing thread: `install(job)` for every job in completion order
    // (the caller takes the device lock inside). While no result is waiting
    // and jobs are still unclaimed, this thread builds one itself, so a
    // build thread that never runs (a lower priority starved on its core)
    // cannot stall the load; it only waits once every job is claimed, and
    // calls `drain()` once then, before the first such wait (the device
    // raises the build thread to its own priority there, so a job the
    // build thread holds also finishes).
    template <typename Install, typename Drain> void InstallAll(Install &&install, Drain &&drain)
    {
        bool drained = false;
        for (size_t installed = 0; installed < m_jobs.size(); ++installed)
        {
            uint32_t index = 0;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> guard(m_lock);
                    if (!m_finished.empty() || AllClaimed())
                    {
                        if (m_finished.empty() && !drained)
                        {
                            drained = true;
                            guard.unlock();
                            drain();
                            guard.lock();
                        }
                        m_ready.wait(guard, [&] { return !m_finished.empty(); });
                        index = m_finished.front();
                        m_finished.pop_front();
                        break;
                    }
                }
                BuildOne();
            }
            install(m_jobs[index]);
        }
    }
    template <typename Install> void InstallAll(Install &&install)
    {
        InstallAll(install, [] {});
    }

    uint64_t BuildNs() const { return m_buildNs.load(std::memory_order_relaxed); }

private:
    bool AllClaimed() const { return m_next.load(std::memory_order_relaxed) >= m_jobs.size(); }

    std::vector<Job> &m_jobs;
    Build m_build;
    Clock m_clock;
    std::atomic<uint32_t> m_next{0};
    std::mutex m_lock;
    std::condition_variable m_ready;
    std::deque<uint32_t> m_finished;
    std::atomic<uint64_t> m_buildNs{0};
};

template <typename Job, typename Build, typename Clock>
PrebakeQueue<Job, Build, Clock> MakePrebakeQueue(std::vector<Job> &jobs, Build build, Clock clock)
{
    return PrebakeQueue<Job, Build, Clock>(jobs, build, clock);
}

} // namespace deko9
