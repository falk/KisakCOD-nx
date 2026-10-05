#pragma once
#include <stdint.h>
#include <qcommon/qcommon.h>
#include <port/switch_perf.h>
#include "r_buffers.h"
#include "r_gfx.h"

// Skinned vertex cache fallback for R_LockSkinnedCache. When the cache cannot
// be locked this frame (the pool buffer is null after a device or video
// restart, or the lock is not 16-byte aligned), gfxBuf.skinCache is cleared
// so R_SkinSceneDObjModels skins the frame's DObjs into the temp skin buffer
// and they are drawn uncached instead of dropped. Every such frame is a
// fault: it counts a skincache_skip perf event and prints an error on the
// first and every 300th frame, so a cache that never returns is visible.

inline uint32_t &R_SkinnedCacheSkips()
{
    static uint32_t skips;
    return skips;
}

// Frames that fell back to the temp skin buffer.
inline uint32_t R_SkinnedCacheSkipCount()
{
    return R_SkinnedCacheSkips();
}

inline void R_SkinnedCacheFallBack(const char *reason)
{
    gfxBuf.skinCache = false;
    if (R_SkinnedCacheSkips()++ % 300 == 0)
        Com_PrintError(CON_CHANNEL_GFX, "R_LockSkinnedCache: %s; skinned models use the temp skin buffer (%u frames)\n",
                       reason, R_SkinnedCacheSkips());
    SwitchPerf_AddEvent(SWITCH_PERF_EV_SKIN_CACHE_SKIPPED, 1);
}

// True when the cache can be locked. A lost device keeps the retail
// behaviour (no lock, no skinning into the cache, nothing reported).
inline bool R_SkinnedCacheLockable(bool deviceLost, const GfxVertexBufferState *cacheVb)
{
    if (deviceLost)
        return false;
    if (!cacheVb || !cacheVb->buffer)
    {
        R_SkinnedCacheFallBack("skinned cache vertex buffer is null");
        return false;
    }
    return true;
}

// True (after falling back) when the locked address cannot hold the cache.
inline bool R_SkinnedCacheLockMisaligned(const void *lockAddr)
{
    if (((uintptr_t)lockAddr & 0xF) == 0)
        return false;
    R_SkinnedCacheFallBack("skinned cache lock is not 16-byte aligned");
    return true;
}
