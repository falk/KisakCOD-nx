// Reload-idempotency proof for retail zone loads (PMem retention fix).
//
// Retail zones begin with flags=0, so no freeFlags unload ever retires
// them: before this fix every R_Init reload of an already-resident
// graphics zone leaked another full zone's PMem (blocks + native arena)
// until the run died in `PMem_Alloc: Need more bytes of ram`.  Zone bytes
// are deterministic from the file, so RetailWalkLoadZoneAssets now skips a
// reload when a live zone with the same name AND the same load policy
// already exists (see DB_RetailZoneFindLive): same slot, same
// registrations, zero new bytes.
//
// This test drives the real RetailWalkLoadZoneAssets (same production TU
// the boot proof links, same host stub registry) over the tiny
// rawfile_walk.ff / stringtable.ff fixtures and proves:
//   1. first full load registers and reports alreadyLoaded=false;
//   2. second identical load succeeds with alreadyLoaded=true, the SAME
//      zone slot, no new zone high-water, no new registrations, no new
//      same-name overrides, and the identical canonical header;
//   3. the same file under the other policy (bounded) is NOT skipped -- a
//      bounded-partial zone must never satisfy a full request -- and loads
//      fresh (RawFile registers under both policies); a repeat bounded
//      load then skips;
//   4. ending the first zone retires liveness: the next full load is fresh
//      again (no stale skip through a reused slot).
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <database/database.h>
#include <database/db_retail_walk.h>
#include <database/db_retail_zone.h>

// Pulls in xanim/r_bsp statics the linked production TUs reference; never
// executed here (same set as switch_retail_boot_test.cpp).
void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

// From switch_retail_zone_stubs_test.cpp.
bool StubZoneIsLive(uint32_t zoneIndex);
XAssetHeader StubFindXAssetHeader(XAssetType type, const char *name);
uint32_t StubOverrideCount(void);
uint32_t StubRegisteredCount(void);
uint32_t StubZoneHighWater(void);
bool DB_RetailZoneEnd(uint32_t zoneIndex);

namespace
{
bool Check(bool condition, const char *stage)
{
    if (!condition)
        std::fprintf(stderr, "FAIL:RETAIL_ZONE_RELOAD_PROOF stage=%s\n", stage);
    return condition;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 2)
    {
        std::fprintf(stderr, "usage: %s <fixture-root>\n", argv[0]);
        return 1;
    }
    FS_InitRetailSource(argv[1]);

    // 1. Fresh full load registers the RawFile and reports a real load.
    RetailWalkLoadZoneResult first{};
    if (!Check(RetailWalkLoadZoneAssets("zone/english/rawfile_walk.ff", &first) ==
                       RETAIL_WALK_LOAD_OK &&
                   first.code == RETAIL_WALK_LOAD_OK,
               "first_load_ok") ||
        !Check(!first.alreadyLoaded, "first_load_not_skipped") ||
        !Check(first.assetCount == 1 && first.registeredRawFileCount == 1,
               "first_load_counts"))
        return 1;
    const uint32_t firstZone = first.zoneIndex;
    const uint32_t highWater = StubZoneHighWater();
    const uint32_t overrides = StubOverrideCount();
    const uint32_t registered = StubRegisteredCount();
    if (!Check(firstZone != 0 && StubZoneIsLive(firstZone), "first_load_live"))
        return 1;
    const XAssetHeader firstHeader = StubFindXAssetHeader(ASSET_TYPE_RAWFILE, "raw");
    if (!Check(firstHeader.rawfile != nullptr &&
                   firstHeader.rawfile->len == 5 &&
                   !std::strcmp(firstHeader.rawfile->buffer, "hello"),
               "first_load_value"))
        return 1;

    // 2. Identical reload is a no-op success on the same slot: no new zone,
    // no new registration, no override event, same canonical header.
    RetailWalkLoadZoneResult second{};
    if (!Check(RetailWalkLoadZoneAssets("zone/english/rawfile_walk.ff", &second) ==
                       RETAIL_WALK_LOAD_OK &&
                   second.code == RETAIL_WALK_LOAD_OK,
               "second_load_ok") ||
        !Check(second.alreadyLoaded, "second_load_skipped") ||
        !Check(second.zoneIndex == firstZone, "second_load_same_slot") ||
        !Check(StubZoneHighWater() == highWater, "second_load_no_new_zone") ||
        !Check(StubOverrideCount() == overrides, "second_load_no_override") ||
        !Check(StubRegisteredCount() == registered, "second_load_no_reregister"))
        return 1;
    const XAssetHeader secondHeader = StubFindXAssetHeader(ASSET_TYPE_RAWFILE, "raw");
    if (!Check(secondHeader.rawfile == firstHeader.rawfile, "second_load_same_header"))
        return 1;

    // 3. Same file under the other policy must NOT skip: a bounded-partial
    // zone must never satisfy a full request (and vice versa), so the
    // policy mismatch loads fresh.  RawFile itself registers under both
    // policies (the bounded spmap script chain reads .gsc exclusively
    // through RawFile assets); the policy gate is about the zone slot,
    // not the family.  A repeat bounded load then skips.
    RetailWalkLoadZoneResult bounded{};
    if (!Check(RetailWalkLoadZoneAssets("zone/english/rawfile_walk.ff", &bounded, false,
                                       true) == RETAIL_WALK_LOAD_OK &&
                   bounded.code == RETAIL_WALK_LOAD_OK,
               "bounded_load_ok") ||
        !Check(!bounded.alreadyLoaded, "bounded_load_not_skipped") ||
        !Check(bounded.zoneIndex != 0 && bounded.zoneIndex != firstZone,
               "bounded_load_fresh_slot") ||
        !Check(bounded.registeredRawFileCount == 1, "bounded_load_registered"))
        return 1;
    const uint32_t boundedZone = bounded.zoneIndex;
    RetailWalkLoadZoneResult boundedAgain{};
    if (!Check(RetailWalkLoadZoneAssets("zone/english/rawfile_walk.ff", &boundedAgain, false,
                                       true) == RETAIL_WALK_LOAD_OK &&
                   boundedAgain.code == RETAIL_WALK_LOAD_OK,
               "bounded_again_ok") ||
        !Check(boundedAgain.alreadyLoaded && boundedAgain.zoneIndex == boundedZone,
               "bounded_again_skipped"))
        return 1;

    // 4. Retiring the first zone retires liveness: the next full load is
    // fresh again (guards against a stale skip through a reused slot), and
    // the bounded resident is untouched by the unload.
    if (!Check(DB_RetailZoneEnd(firstZone), "end_first") ||
        !Check(!StubZoneIsLive(firstZone), "end_first_retired") ||
        !Check(StubZoneIsLive(boundedZone), "end_first_bounded_untouched"))
        return 1;
    RetailWalkLoadZoneResult third{};
    if (!Check(RetailWalkLoadZoneAssets("zone/english/rawfile_walk.ff", &third) ==
                       RETAIL_WALK_LOAD_OK &&
                   third.code == RETAIL_WALK_LOAD_OK,
               "third_load_ok") ||
        !Check(!third.alreadyLoaded, "third_load_not_skipped"))
        return 1;
    const XAssetHeader thirdHeader = StubFindXAssetHeader(ASSET_TYPE_RAWFILE, "raw");
    if (!Check(thirdHeader.rawfile != nullptr &&
                   thirdHeader.rawfile->len == 5 &&
                   !std::strcmp(thirdHeader.rawfile->buffer, "hello"),
               "third_load_value"))
        return 1;
    if (!Check(DB_RetailZoneEnd(third.zoneIndex), "end_third") ||
        !Check(DB_RetailZoneEnd(boundedZone), "end_bounded"))
        return 1;

    std::printf("PASS:RETAIL_ZONE_RELOAD_PROOF first=%u bounded=%u\n", firstZone, boundedZone);
    return 0;
}
