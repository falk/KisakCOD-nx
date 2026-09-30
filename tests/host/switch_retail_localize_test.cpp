// Wire proof: LocalizeEntry is value then name in the 32-bit wire root.
// This fixture uses the production wire and decoder implementations; it does
// not stand up a replacement asset registry.
#include <cstdio>
#include <cstring>

#include <database/db_retail_decode_small.h>

// xanim.h retains this static dispatch table even though this narrow decoder
// never executes a collision trace.  The real engine owns both functions.
void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

namespace
{
bool Check(bool condition, const char *stage)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL:RETAIL_LOCALIZE_DECODE_PROOF stage=%s\n", stage);
        return false;
    }
    return true;
}

void WriteLe32(uint8_t *destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}
} // namespace

int main()
{
    uint8_t root[8]{};
    uint8_t strings[64]{};
    uint8_t nativeStorage[256]{};
    XZoneMemory zone{};
    zone.blocks[0] = {root, sizeof(root)};
    zone.blocks[4] = {strings, sizeof(strings)};
    const char value[] = "Retail localized value";
    const char name[] = "MENU_TEST_RETAIL_KEY";
    std::memcpy(strings, value, sizeof(value));
    std::memcpy(strings + sizeof(value), name, sizeof(name));
    WriteLe32(root, 0xffffffffu);
    WriteLe32(root + 4, 0xffffffffu);

    RetailZoneLoadSession session{};
    session.active = true;
    session.zoneMemory = &zone;
    if (!Check(RetailWireBlocksInit(&session.wire, &zone), "wire_init") ||
        !Check(RetailNativeArenaInit(&session.arena, nativeStorage, sizeof(nativeStorage)), "arena_init"))
        return 1;

    LocalizeEntry *entry = nullptr;
    if (!Check(RetailDecodeLocalizeEntry(&session, &entry) == RETAIL_LOCALIZE_DECODE_OK, "decode") ||
        !Check(entry && !std::strcmp(entry->value, value) && !std::strcmp(entry->name, name), "order") ||
        !Check(session.wire.cursor[0] == sizeof(root), "root_cursor") ||
        !Check(session.wire.cursor[4] == sizeof(value) + sizeof(name), "string_cursor") ||
        !Check(reinterpret_cast<const uint8_t *>(entry->value) >= nativeStorage &&
               reinterpret_cast<const uint8_t *>(entry->name) >= nativeStorage, "native_ownership"))
        return 1;

    RetailWireBlocksRewind(&session.wire, 0, 0);
    RetailWireBlocksRewind(&session.wire, 4, 0);
    WriteLe32(root, 0);
    if (!Check(RetailDecodeLocalizeEntry(&session, &entry) ==
                   RETAIL_LOCALIZE_DECODE_UNSUPPORTED_VALUE_REFERENCE,
               "null_value_rejected"))
        return 1;
    std::puts("PASS:RETAIL_LOCALIZE_DECODE_PROOF value_then_name=1 cursor=exact");
    return 0;
}
