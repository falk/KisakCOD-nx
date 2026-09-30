#include "db_retail_decode_font.h"

#include <cstring>

namespace
{
constexpr uint32_t kInline = 0xffffffffu;
constexpr uint32_t kGlyphBytes = 24u;

uint32_t Le32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

bool InlineString(RetailZoneLoadSession *session, uint32_t ref, const char **out)
{
    if (ref != kInline)
        return false;
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

bool LoadFont(RetailZoneLoadSession *session, XAssetType type, bool, void *, XAssetHeader *header)
{
    Font_s *font = nullptr;
    if (type != ASSET_TYPE_FONT || !header || RetailDecodeFont(session, &font) != RETAIL_FONT_DECODE_OK)
        return false;
    *header = {font};
    return true;
}
}

RetailFontDecodeResult RetailDecodeFont(RetailZoneLoadSession *session, Font_s **out)
{
    if (!session || !session->active || !out)
        return RETAIL_FONT_DECODE_BAD_ARGUMENT;
    *out = nullptr;
    uint8_t wire[24];
    if (!RetailWireBlocksRead(&session->wire, 0, wire, sizeof(wire)))
        return RETAIL_FONT_DECODE_BAD_ROOT;
    Font_s *font = static_cast<Font_s *>(RetailZoneLoadSessionAlloc(session, sizeof(*font), alignof(Font_s)));
    if (!font)
        return RETAIL_FONT_DECODE_OUT_OF_ARENA;
    std::memset(font, 0, sizeof(*font));
    if (!InlineString(session, Le32(wire), &font->fontName))
        return RETAIL_FONT_DECODE_BAD_NAME;
    const uint32_t glyphCount = Le32(wire + 8);
    if (glyphCount > 4096)
        return RETAIL_FONT_DECODE_BAD_GLYPHS;
    XAssetHeader material{}, glow{};
    uint32_t ignored = 0;
    if (RetailZoneLoadSessionDispatchAsset(session, ASSET_TYPE_MATERIAL, Le32(wire + 12), &material, &ignored) != RETAIL_ZONE_ASSET_OK ||
        RetailZoneLoadSessionDispatchAsset(session, ASSET_TYPE_MATERIAL, Le32(wire + 16), &glow, &ignored) != RETAIL_ZONE_ASSET_OK)
        return RETAIL_FONT_DECODE_BAD_MATERIAL;
    font->pixelHeight = static_cast<int32_t>(Le32(wire + 4));
    font->glyphCount = static_cast<int32_t>(glyphCount);
    font->material = material.material;
    font->glowMaterial = glow.material;
    if (glyphCount)
    {
        if (Le32(wire + 20) != kInline || glyphCount > UINT32_MAX / kGlyphBytes)
            return RETAIL_FONT_DECODE_BAD_GLYPHS;
        Glyph *glyphs = static_cast<Glyph *>(RetailZoneLoadSessionAlloc(session, glyphCount * sizeof(Glyph), alignof(Glyph)));
        if (!glyphs || !RetailWireBlocksRead(&session->wire, 4, glyphs, glyphCount * kGlyphBytes))
            return RETAIL_FONT_DECODE_BAD_GLYPHS;
        font->glyphs = glyphs;
    }
    *out = font;
    return RETAIL_FONT_DECODE_OK;
}

bool RetailZoneInstallFontDecoder(RetailZoneLoadSession *session)
{
    return RetailZoneLoadSessionSetAssetLoader(session, ASSET_TYPE_FONT, LoadFont, nullptr);
}

const char *RetailFontDecodeResultName(RetailFontDecodeResult result)
{
    static const char *const names[] = {"ok", "bad_argument", "bad_root", "bad_name", "bad_material", "bad_glyphs", "out_of_arena"};
    return result >= RETAIL_FONT_DECODE_OK && result <= RETAIL_FONT_DECODE_OUT_OF_ARENA ? names[result] : "invalid_result";
}
