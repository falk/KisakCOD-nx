#pragma once

// Stall evidence. Threads stamp a per-thread breadcrumb (one relaxed counter
// bump); a watchdog thread notices when the main frame counter stops
// advancing and writes every breadcrumb plus the GPU black box to the log
// ring. Nothing here takes a lock a stuck thread could hold.

#include <atomic>
#include <cstdint>

enum CrumbSlot
{
    CRUMB_MAIN_FRAME,
    CRUMB_BACKEND,
    CRUMB_WORKER0,
    CRUMB_WORKER1,
    CRUMB_SOUND_MIX,
    CRUMB_STREAM,
    CRUMB_DATABASE,
    CRUMB_GPU_HB,
    CRUMB_COUNT
};

struct CrumbCell
{
    alignas(64) std::atomic<uint32_t> count{0};
    std::atomic<uint32_t> site{0};
};

inline CrumbCell g_crumbs[CRUMB_COUNT];

// `site` says where in the loop the thread last was (1 = waiting, 2 = working).
inline void Watchdog_Crumb(CrumbSlot slot, uint32_t site = 0)
{
    CrumbCell &c = g_crumbs[slot];
    c.site.store(site, std::memory_order_relaxed);
    c.count.fetch_add(1, std::memory_order_relaxed);
}

// Fires once when the value stops changing for thresholdMs; re-arms when it
// moves again.
class StallTrigger
{
public:
    explicit StallTrigger(uint64_t thresholdMs) : m_threshold(thresholdMs) {}

    bool Sample(uint64_t value, uint64_t nowMs)
    {
        if (!m_seen || value != m_value)
        {
            m_seen = true;
            m_value = value;
            m_since = nowMs;
            m_fired = false;
            return false;
        }
        if (!m_fired && nowMs - m_since >= m_threshold)
        {
            m_fired = true;
            return true;
        }
        return false;
    }

    void SetThreshold(uint64_t thresholdMs) { m_threshold = thresholdMs; }
    uint64_t StalledMs(uint64_t nowMs) const { return m_seen ? nowMs - m_since : 0; }

private:
    uint64_t m_threshold;
    uint64_t m_value = 0, m_since = 0;
    bool m_seen = false, m_fired = false;
};

#ifdef __cplusplus
extern "C" {
#endif
void Switch_WatchdogStart(void);
#ifdef __cplusplus
}
#endif
