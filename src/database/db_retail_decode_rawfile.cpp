#include "db_retail_decode_rawfile.h"

#include <cstring>

namespace
{
constexpr uint32_t kInline = 0xffffffffu;

uint32_t Le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool LoadRawFile(RetailZoneLoadSession *session, XAssetType type, bool, void *, XAssetHeader *header)
{
    RawFile *file = nullptr;
    if (type != ASSET_TYPE_RAWFILE || !header ||
        RetailDecodeRawFile(session, &file) != RETAIL_RAWFILE_DECODE_OK)
        return false;
    *header = {file};
    return true;
}
} // namespace

// Android Load_XString: only the -1 inline form carries a stream body; a
// zero reference is simply absent. Any other value is DB_ConvertOffsetToPointer
// semantics -- an alias to an already-streamed block-4 string (db_load.cpp
// Load_XString else-branch). Resolve it against the session's own persistent
// zone blocks, which hold every prior ordinal's inline strings by this point.
// Shared with db_retail_decode_world.cpp's MapEnts loader (see the header).
bool RetailDecodeInlineOrNullString(RetailZoneLoadSession *session, uint32_t ref, const char **out)
{
    *out = nullptr;
    if (!ref)
        return true;
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
    if (!session || !session->zoneMemory)
        return false;
    XBlock blocks[9]{};
    for (uint32_t block = 0; block < 9; ++block)
        blocks[block] = session->zoneMemory->blocks[block];
    RetailWireToken token{};
    RetailPtr32 encoded{};
    encoded.encoded = ref;
    if (!RetailWireTokenDecodeBlocks(blocks, encoded, 1, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        return false;
    const XBlock &block = session->zoneMemory->blocks[4];
    if (token.offset >= block.size)
        return false;
    const char *source = reinterpret_cast<const char *>(block.data + token.offset);
    uint32_t length = 0;
    while (token.offset + length < block.size && source[length])
    {
        const unsigned char c = static_cast<unsigned char>(source[length]);
        if (c < 0x20 || c > 0x7e)
            return false;
        ++length;
    }
    if (token.offset + length >= block.size || length == 0)
        return false;
    char *copy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, length + 1, 1));
    if (!copy)
        return false;
    std::memcpy(copy, source, length + 1);
    *out = copy;
    return true;
}

RetailRawFileDecodeResult RetailDecodeRawFile(RetailZoneLoadSession *session, RawFile **out)
{
    if (!session || !session->active || !out)
        return RETAIL_RAWFILE_DECODE_BAD_ARGUMENT;
    *out = nullptr;
    uint8_t wire[12];
    if (!RetailWireBlocksRead(&session->wire, 0, wire, sizeof(wire)))
        return RETAIL_RAWFILE_DECODE_BAD_ROOT;
    const uint32_t nameRef = Le32(wire);
    const int32_t length = static_cast<int32_t>(Le32(wire + 4));
    const uint32_t bufferRef = Le32(wire + 8);
    if (length < 0)
        return RETAIL_RAWFILE_DECODE_BAD_ROOT;
    RawFile *file =
        static_cast<RawFile *>(RetailZoneLoadSessionAlloc(session, sizeof(*file), alignof(RawFile)));
    if (!file)
        return RETAIL_RAWFILE_DECODE_OUT_OF_ARENA;
    std::memset(file, 0, sizeof(*file));
    if (!RetailDecodeInlineOrNullString(session, nameRef, &file->name))
        return RETAIL_RAWFILE_DECODE_BAD_NAME;
    file->len = length;
    // Android Load_RawFile: the wire buffer value is a presence marker, not
    // an encoded alias -- any nonzero value (not only -1) means "read
    // len + 1 raw bytes here", matching the F5 walker's own
    // ReadRetailRawFileBody contract.
    if (bufferRef)
    {
        const uint32_t bytes = static_cast<uint32_t>(length) + 1u;
        char *buffer = static_cast<char *>(RetailZoneLoadSessionAlloc(session, bytes, 1));
        if (!buffer || !RetailWireBlocksRead(&session->wire, 4, buffer, bytes))
            return RETAIL_RAWFILE_DECODE_BAD_BUFFER;
        file->buffer = buffer;
    }
    *out = file;
    return RETAIL_RAWFILE_DECODE_OK;
}

bool RetailZoneInstallRawFileDecoder(RetailZoneLoadSession *session)
{
    return RetailZoneLoadSessionSetAssetLoader(session, ASSET_TYPE_RAWFILE, LoadRawFile, nullptr);
}

const char *RetailRawFileDecodeResultName(RetailRawFileDecodeResult result)
{
    static const char *const names[] = {"ok", "bad_argument", "bad_root", "bad_name",
                                        "bad_buffer", "out_of_arena"};
    return result >= RETAIL_RAWFILE_DECODE_OK && result <= RETAIL_RAWFILE_DECODE_OUT_OF_ARENA ?
        names[result] : "invalid_result";
}
