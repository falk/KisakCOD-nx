#pragma once

// Retail fastfile decoding begins before archived dvars/config are reliably
// available. Keep dense per-asset success narration compiled out by default;
// define KISAK_RETAIL_ASSET_TRACE for a diagnostic build. Errors and zone-level
// milestones continue to use Com_Printf directly.
#if defined(KISAK_RETAIL_ASSET_TRACE)
#define RETAIL_ASSET_TRACE(...) Com_Printf(__VA_ARGS__)
#else
#define RETAIL_ASSET_TRACE(...) ((void)0)
#endif
