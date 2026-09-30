#include "db_retail_decode_small.h"

#include <cstring>

namespace
{
constexpr uint32_t kInlineReference = 0xffffffffu;

uint32_t ReadLe32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

RetailLocalizeDecodeResult CopyInlineString(RetailZoneLoadSession *session, uint32_t reference,
                                            bool name, const char **output)
{
    if (reference != kInlineReference)
    {
        RetailWireToken token{};
        if (!RetailWireTokenDecode(&session->wire, {reference}, 0, 1u << 4, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET)
        {
            return name ? RETAIL_LOCALIZE_DECODE_UNSUPPORTED_NAME_REFERENCE :
                          RETAIL_LOCALIZE_DECODE_UNSUPPORTED_VALUE_REFERENCE;
        }
        const XBlock &block = session->zoneMemory->blocks[token.block];
        if (token.offset >= block.size)
            return name ? RETAIL_LOCALIZE_DECODE_UNTERMINATED_NAME :
                          RETAIL_LOCALIZE_DECODE_UNTERMINATED_VALUE;
        const char *source = reinterpret_cast<const char *>(block.data + token.offset);
        const uint32_t remaining = block.size - token.offset;
        uint32_t length = 0;
        while (length < remaining && source[length])
            ++length;
        if (length == remaining)
            return name ? RETAIL_LOCALIZE_DECODE_UNTERMINATED_NAME :
                          RETAIL_LOCALIZE_DECODE_UNTERMINATED_VALUE;
        if (name && length == 0)
            return RETAIL_LOCALIZE_DECODE_EMPTY_NAME;

        char *copy = static_cast<char *>(RetailNativeArenaAlloc(&session->arena, length + 1, 1));
        if (!copy)
            return RETAIL_LOCALIZE_DECODE_OUT_OF_ARENA;
        std::memcpy(copy, source, length + 1);
        *output = copy;
        return RETAIL_LOCALIZE_DECODE_OK;
    }

    const XBlock &block = session->zoneMemory->blocks[4];
    const uint32_t cursor = session->wire.cursor[4];
    if (cursor >= block.size)
        return name ? RETAIL_LOCALIZE_DECODE_UNTERMINATED_NAME :
                      RETAIL_LOCALIZE_DECODE_UNTERMINATED_VALUE;
    const uint8_t *source = block.data + cursor;
    const uint32_t remaining = block.size - cursor;
    uint32_t length = 0;
    while (length < remaining && source[length])
        ++length;
    if (length == remaining)
        return name ? RETAIL_LOCALIZE_DECODE_UNTERMINATED_NAME :
                      RETAIL_LOCALIZE_DECODE_UNTERMINATED_VALUE;
    if (name && length == 0)
        return RETAIL_LOCALIZE_DECODE_EMPTY_NAME;

    char *copy = static_cast<char *>(RetailNativeArenaAlloc(&session->arena, length + 1, 1));
    if (!copy)
        return RETAIL_LOCALIZE_DECODE_OUT_OF_ARENA;
    std::memcpy(copy, source, length + 1);
    if (!RetailWireBlocksAlloc(&session->wire, 4, length + 1, 1))
        return name ? RETAIL_LOCALIZE_DECODE_UNTERMINATED_NAME :
                      RETAIL_LOCALIZE_DECODE_UNTERMINATED_VALUE;
    *output = copy;
    return RETAIL_LOCALIZE_DECODE_OK;
}

bool LoadLocalizeEntry(RetailZoneLoadSession *session, XAssetType type, bool, void *,
                       XAssetHeader *header)
{
    if (type != ASSET_TYPE_LOCALIZE_ENTRY || !header)
        return false;
    LocalizeEntry *entry = nullptr;
    if (RetailDecodeLocalizeEntry(session, &entry) != RETAIL_LOCALIZE_DECODE_OK)
        return false;
    *header = {entry};
    return true;
}
} // namespace

RetailLocalizeDecodeResult RetailDecodeLocalizeEntry(RetailZoneLoadSession *session,
                                                     LocalizeEntry **entry)
{
    if (!session || !session->active || !entry)
        return RETAIL_LOCALIZE_DECODE_BAD_ARGUMENT;
    *entry = nullptr;

    uint8_t wire[8];
    if (!RetailWireBlocksRead(&session->wire, 0, wire, sizeof(wire)))
        return RETAIL_LOCALIZE_DECODE_BAD_ROOT;

    LocalizeEntry *native = static_cast<LocalizeEntry *>(
        RetailNativeArenaAlloc(&session->arena, sizeof(*native), alignof(LocalizeEntry)));
    if (!native)
        return RETAIL_LOCALIZE_DECODE_OUT_OF_ARENA;
    std::memset(native, 0, sizeof(*native));

    RetailLocalizeDecodeResult result =
        CopyInlineString(session, ReadLe32(wire), false, &native->value);
    if (result != RETAIL_LOCALIZE_DECODE_OK)
        return result;
    result = CopyInlineString(session, ReadLe32(wire + 4), true, &native->name);
    if (result != RETAIL_LOCALIZE_DECODE_OK)
        return result;
    *entry = native;
    return RETAIL_LOCALIZE_DECODE_OK;
}

bool RetailZoneInstallLocalizeEntryDecoder(RetailZoneLoadSession *session)
{
    return RetailZoneLoadSessionSetAssetLoader(session, ASSET_TYPE_LOCALIZE_ENTRY,
                                               LoadLocalizeEntry, nullptr);
}

const char *RetailLocalizeDecodeResultName(RetailLocalizeDecodeResult result)
{
    static const char *const names[] = {
        "ok", "bad_argument", "bad_root", "unsupported_value_reference",
        "unsupported_name_reference", "unterminated_value", "unterminated_name",
        "empty_name", "out_of_arena",
    };
    return result >= RETAIL_LOCALIZE_DECODE_OK && result <= RETAIL_LOCALIZE_DECODE_OUT_OF_ARENA ?
        names[result] : "invalid_result";
}
