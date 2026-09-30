#include "db_retail_decode_stringtable.h"

#include <cstring>

namespace
{
constexpr uint32_t kInline = 0xffffffffu;

uint32_t Le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool ResolveString(RetailZoneLoadSession *session, uint32_t ref, const char **out)
{
    *out = nullptr;
    if (!ref)
    {
        *out = "";
        return true;
    }
    if (ref == kInline)
    {
        const XBlock &block = session->zoneMemory->blocks[4];
        const uint32_t cursor = session->wire.cursor[4];
        if (cursor >= block.size)
            return false;
        const char *source = reinterpret_cast<const char *>(block.data + cursor);
        uint32_t length = 0;
        while (cursor + length < block.size && source[length])
            ++length;
        if (cursor + length == block.size)
            return false;
        char *copy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, length + 1, 1));
        if (!copy || !RetailWireBlocksAlloc(&session->wire, 4, length + 1, 1))
            return false;
        std::memcpy(copy, source, length + 1);
        *out = copy;
        return true;
    }

    RetailWireToken token{};
    if (!RetailWireTokenDecode(&session->wire, {ref}, 1, (1u << 9) - 1u, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET)
    {
        return false;
    }
    const XBlock &block = session->zoneMemory->blocks[token.block];
    if (token.offset >= block.size)
        return false;
    const char *source = reinterpret_cast<const char *>(block.data + token.offset);
    uint32_t length = 0;
    while (token.offset + length < block.size && source[length])
        ++length;
    if (token.offset + length == block.size)
        return false;
    char *copy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, length + 1, 1));
    if (!copy)
        return false;
    std::memcpy(copy, source, length + 1);
    *out = copy;
    return true;
}

bool LoadStringTable(RetailZoneLoadSession *session, XAssetType type, bool, void *,
                     XAssetHeader *header)
{
    StringTable *table = nullptr;
    if (type != ASSET_TYPE_STRINGTABLE || !header ||
        RetailDecodeStringTable(session, &table) != RETAIL_STRINGTABLE_DECODE_OK)
        return false;
    *header = {table};
    return true;
}
} // namespace

RetailStringTableDecodeResult RetailDecodeStringTable(RetailZoneLoadSession *session,
                                                       StringTable **out)
{
    if (!session || !session->active || !out)
        return RETAIL_STRINGTABLE_DECODE_BAD_ARGUMENT;
    *out = nullptr;
    uint8_t wire[16];
    if (!RetailWireBlocksRead(&session->wire, 4, wire, sizeof(wire)))
        return RETAIL_STRINGTABLE_DECODE_BAD_ROOT;
    const uint32_t nameRef = Le32(wire);
    const uint32_t columnCount = Le32(wire + 4);
    const uint32_t rowCount = Le32(wire + 8);
    const uint32_t valuesRef = Le32(wire + 12);
    if (columnCount && rowCount > UINT32_MAX / columnCount)
        return RETAIL_STRINGTABLE_DECODE_BAD_ROOT;
    const uint32_t count = rowCount * columnCount;

    StringTable *table = static_cast<StringTable *>(
        RetailZoneLoadSessionAlloc(session, sizeof(*table), alignof(StringTable)));
    if (!table)
        return RETAIL_STRINGTABLE_DECODE_OUT_OF_ARENA;
    std::memset(table, 0, sizeof(*table));
    if (!ResolveString(session, nameRef, &table->name))
        return RETAIL_STRINGTABLE_DECODE_BAD_NAME;
    table->columnCount = static_cast<int32_t>(columnCount);
    table->rowCount = static_cast<int32_t>(rowCount);

    // Android Load_StringTable only checks "nonzero" for the values slot,
    // not specifically -1, matching the F5 walker's own ReadRetailStringTableBody.
    if (valuesRef)
    {
        if (count > UINT32_MAX / 4u)
            return RETAIL_STRINGTABLE_DECODE_BAD_VALUES;
        // The live streamer 4-byte-aligns the whole pointer table (matching
        // F5's own ReadRetailStringTableBody), which can leave a short gap
        // after the name string; RetailWireBlocksRead never auto-aligns, so
        // replay that same alignment here or this decoder reads a table
        // that starts a few bytes earlier than where it was actually
        // written.
        const uint32_t tableStart = (session->wire.cursor[4] + 3u) & ~3u;
        if (tableStart > session->zoneMemory->blocks[4].size)
            return RETAIL_STRINGTABLE_DECODE_BAD_VALUES;
        RetailWireBlocksRewind(&session->wire, 4, tableStart);
        // The whole pointer table streams as one contiguous span before any
        // cell's string bytes; peek every ref first, exactly like the F5
        // walker's own raw table read, so a later cell's ResolveString
        // call never mistakes an unread ref slot for string data.
        uint32_t *refs = count ? static_cast<uint32_t *>(RetailZoneLoadSessionAlloc(
            session, count * sizeof(uint32_t), alignof(uint32_t))) : nullptr;
        if (count && !refs)
            return RETAIL_STRINGTABLE_DECODE_OUT_OF_ARENA;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint8_t cell[4];
            if (!RetailWireBlocksRead(&session->wire, 4, cell, sizeof(cell)))
                return RETAIL_STRINGTABLE_DECODE_BAD_VALUES;
            refs[i] = Le32(cell);
        }
        const char **values = count ? static_cast<const char **>(RetailZoneLoadSessionAlloc(
            session, count * sizeof(char *), alignof(char *))) : nullptr;
        if (count && !values)
            return RETAIL_STRINGTABLE_DECODE_OUT_OF_ARENA;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (!ResolveString(session, refs[i], &values[i]))
                return RETAIL_STRINGTABLE_DECODE_BAD_VALUES;
        }
        table->values = values;
    }
    *out = table;
    return RETAIL_STRINGTABLE_DECODE_OK;
}

bool RetailZoneInstallStringTableDecoder(RetailZoneLoadSession *session)
{
    return RetailZoneLoadSessionSetAssetLoader(session, ASSET_TYPE_STRINGTABLE, LoadStringTable,
                                               nullptr);
}

const char *RetailStringTableDecodeResultName(RetailStringTableDecodeResult result)
{
    static const char *const names[] = {"ok", "bad_argument", "bad_root", "bad_name",
                                        "bad_values", "out_of_arena"};
    return result >= RETAIL_STRINGTABLE_DECODE_OK && result <= RETAIL_STRINGTABLE_DECODE_OUT_OF_ARENA ?
        names[result] : "invalid_result";
}
