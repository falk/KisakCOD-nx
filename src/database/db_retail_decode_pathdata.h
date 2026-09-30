#pragma once

#include <database/db_retail_zone.h>

// Live GameWorldSp decoder for RetailWalkLoadZoneAssets: widens the retail
// GameWorldSp/PathData wire body into the engine's native gameWorldSp
// singleton (pathnode_t records, per-node pathlink_s arrays, base nodes,
// chain arrays, path visibility, and the recursive pathnode tree) instead of
// leaving path data absent.  A zone without real path data makes every
// authored pathnode entity an "extra node" (Com_PrintError through
// G_UpdateTrackExtraNodes), which fails the map-load error summary.
bool RetailZoneInstallGameWorldSpDecoder(RetailZoneLoadSession *session);
