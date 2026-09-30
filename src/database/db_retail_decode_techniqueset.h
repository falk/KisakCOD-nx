#pragma once

#include "../universal/com_files.h"
#include "db_retail_zone.h"

// Widens one real TechniqueSet body read live from `reader` -- the same
// FS-owned reader db_retail_walk.cpp's directory walk already holds open --
// allocating through the zone's real native arena and registering through
// the zone's real asset table.  Unlike db_retail_decode_render.cpp's
// DB_RetailDecodeTechniqueSet (a narrow named-lookup proof that requires a
// pre-known name and rejects any technique set with a non-zero technique
// slot), this accepts empty, inline, and alias technique slots.
//
// Alias identity is exact, never heuristic: an inline technique's identity
// is the canonical encoding of its own stream record start
// (RetailWireTokenEncode(block 4, prefix offset), which the FS reader's
// own InlineName rewrite proves equals what alias slots carry -- see
// ReadRetailFastfileInlineName in com_files.cpp and the Android oracle's
// LoadMaterialTechnique contiguity), and an alias slot resolves only
// through the canonical token decoder against the techniques decoded
// earlier in the same cache. A miss fails loudly naming the slot and
// reference; there is no nearest-offset, bias, fallback-slot, or
// last-entry substitution. The same rule covers vertex declarations and
// vertex/pixel shaders (identity from their own record offsets).
// Technique-set and technique names resolve only through the canonical
// block-4 string read (plus the exact token->name memo for names already
// resolved this run); ps_/vs_ prefix guessing and sm2/-stripping are gone,
// and an unresolvable inline name fails instead of synthesizing one.
//
// Ported from switch_sp_bootstrap.cpp's hardware-proven LoadTechniqueSet/
// WidenInlineTechnique (PASS:RETAIL_MENU_GFX_PROOF against the real retail
// code_post_gfx.ff/ui.ff), adapted to allocate through
// RetailZoneLoadSessionAlloc and register through
// RetailZoneLoadSessionRegister instead of Hunk_Alloc/
// DB_RegisterMaterialTechniqueSet directly, so a later zone unload
// correctly reclaims this memory and untracks the asset -- the bootstrap's
// own allocation/registration choice bypasses the zone lifetime contract
// F2 established and must not be copied verbatim into production.
//
// Reference resolution (name and nested technique bodies) reads directly
// against `reader`'s own persistent per-block buffers via
// RetailWireTokenDecodeBlocks/FS_RetailFastfileBlockData, exactly like the
// bootstrap does -- this asset family's wire references are Disk32-style
// absolute-offset tokens, not the sequential inline-or-nothing sentinels
// Font/LocalizeEntry/RawFile/StringTable use, so it deliberately does not
// go through session->wire/RetailWireBlocksRead at all.

struct MaterialTechnique;
struct MaterialTechniqueSet;
struct MaterialPass;
struct MaterialVertexShader;
struct MaterialPixelShader;
struct MaterialVertexDeclaration;
struct MaterialTextureDef;
struct water_t;

struct RetailWireTechniqueEntry
{
    uint32_t token;
    MaterialTechnique *technique;
};

struct RetailWireTechniqueCache
{
    // Sized for the killhouse need (hundreds of techniques
    // across 238 sets, each shared by several aliasing sets).
    RetailWireTechniqueEntry entries[2048];
    uint32_t count = 0;
    // Techniques widened with a null name (pooled names, see
    // WidenInlineTechnique). Tallied here so the zone summary can report
    // it without changing every loader signature.
    uint32_t nullTechniqueNames = 0;
    // Vertex/pixel shaders widened with a null name (same pooled-string
    // class as technique names; upload uses bytecode, never names).
    uint32_t nullShaderNames = 0;
    // Deferred alias bindings (technique-slot aliases whose target has
    // not been widened yet at use time -- forward references are legal
    // zone data: the real loader resolves offsets order-independently).
    // Resolved after the directory completes; anything still missing
    // then fails loudly. Sized for the killhouse need (~300
    // across 238 sets); overflow fails loudly, never silently drops.
    struct DeferredAlias
    {
        MaterialTechniqueSet *techniqueSet = nullptr;
        uint32_t slot = 0;
        uint32_t reference = 0;
        char setName[64]{};
    };
    DeferredAlias deferred[1024]{};
    uint32_t deferredCount = 0;
    // Deferred aliases resolved by the post-pass (vs recorded).
    uint32_t deferredResolvedCount = 0;
    // Deferred vertex-declaration / vertex-shader / pixel-shader aliases:
    // sub-records shared across techniques (like techniques, order-
    // independent). The post-pass patches the already-widened passes.
    struct DeferredSubAlias
    {
        MaterialPass *pass = nullptr;
        // 0 = vertexDecl, 1 = vertexShader, 2 = pixelShader.
        uint32_t kind = 0;
        uint32_t reference = 0;
    };
    DeferredSubAlias deferredSubs[1024]{};
    uint32_t deferredSubCount = 0;
    uint32_t deferredSubResolvedCount = 0;

    struct VertexShaderEntry { uint32_t token; MaterialVertexShader *vs; };
    struct PixelShaderEntry { uint32_t token; MaterialPixelShader *ps; };
    struct VertexDeclEntry { uint32_t token; MaterialVertexDeclaration *decl; };

    // Per-zone alias-resolution caches. Sized for the
    // killhouse need (hundreds of distinct shaders/decls across 238
    // sets); insert beyond capacity fails loudly, never silently drops.
    VertexShaderEntry vsEntries[2048]{};
    uint32_t vsCount = 0;
    PixelShaderEntry psEntries[2048]{};
    uint32_t psCount = 0;
    VertexDeclEntry declEntries[1024]{};
    uint32_t declCount = 0;

    MaterialVertexShader *FindVertexShader(uint32_t token) const
    {
        if (!token) return nullptr;
        for (uint32_t i = 0; i < vsCount; ++i)
        {
            if (vsEntries[i].token == token)
                return vsEntries[i].vs;
        }
        return nullptr;
    }

    bool InsertVertexShader(uint32_t token, MaterialVertexShader *vs)
    {
        if (!token || !vs) return false;
        for (uint32_t i = 0; i < vsCount; ++i)
        {
            if (vsEntries[i].token == token)
            {
                vsEntries[i].vs = vs;
                return true;
            }
        }
        if (vsCount >= 2048) return false;
        vsEntries[vsCount++] = {token, vs};
        return true;
    }

    MaterialPixelShader *FindPixelShader(uint32_t token) const
    {
        if (!token) return nullptr;
        for (uint32_t i = 0; i < psCount; ++i)
        {
            if (psEntries[i].token == token)
                return psEntries[i].ps;
        }
        return nullptr;
    }

    bool InsertPixelShader(uint32_t token, MaterialPixelShader *ps)
    {
        if (!token || !ps) return false;
        for (uint32_t i = 0; i < psCount; ++i)
        {
            if (psEntries[i].token == token)
            {
                psEntries[i].ps = ps;
                return true;
            }
        }
        if (psCount >= 2048) return false;
        psEntries[psCount++] = {token, ps};
        return true;
    }

    MaterialVertexDeclaration *FindVertexDecl(uint32_t token) const
    {
        if (!token) return nullptr;
        for (uint32_t i = 0; i < declCount; ++i)
        {
            if (declEntries[i].token == token)
                return declEntries[i].decl;
        }
        return nullptr;
    }

    bool InsertVertexDecl(uint32_t token, MaterialVertexDeclaration *decl)
    {
        if (!token || !decl) return false;
        for (uint32_t i = 0; i < declCount; ++i)
        {
            if (declEntries[i].token == token)
            {
                declEntries[i].decl = decl;
                return true;
            }
        }
        if (declCount >= 1024) return false;
        declEntries[declCount++] = {token, decl};
        return true;
    }

    struct TokenStringEntry
    {
        uint32_t token;
        char str[64];
    };
    TokenStringEntry stringMap[256]{};
    uint32_t stringMapCount = 0;

    void RecordString(uint32_t token, const char *str)
    {
        if (!token || !str || !str[0] || stringMapCount >= 256)
            return;
        for (uint32_t i = 0; i < stringMapCount; ++i)
        {
            if (stringMap[i].token == token)
            {
                std::strncpy(stringMap[i].str, str, sizeof(stringMap[i].str) - 1);
                stringMap[i].str[sizeof(stringMap[i].str) - 1] = '\0';
                return;
            }
        }
        stringMap[stringMapCount].token = token;
        std::strncpy(stringMap[stringMapCount].str, str, sizeof(stringMap[stringMapCount].str) - 1);
        stringMap[stringMapCount].str[sizeof(stringMap[stringMapCount].str) - 1] = '\0';
        ++stringMapCount;
    }

    const char *FindString(uint32_t token) const
    {
        if (!token)
            return nullptr;
        for (uint32_t i = 0; i < stringMapCount; ++i)
        {
            if (stringMap[i].token == token)
                return stringMap[i].str;
        }
        return nullptr;
    }

    MaterialTechnique *Find(uint32_t token) const
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            if (entries[i].token == token)
                return entries[i].technique;
        }
        return nullptr;
    }

    bool Insert(uint32_t token, MaterialTechnique *tech)
    {
        if (!token || !tech)
            return false;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (entries[i].token == token)
            {
                entries[i].technique = tech;
                return true;
            }
        }
        if (count >= 2048)
            return false;
        entries[count++] = {token, tech};
        return true;
    }

    // Deferred texture-slot image bindings (same order-independence rule as
    // the technique aliases above, but one level down): a material texture
    // slot whose reference addresses block-4 bytes the stream has not
    // produced yet (a pool slot of a later declarer, or a later inline
    // image) cannot resolve at widen time -- the declaring occurrence
    // widens later in the same zone. The widener records the native table
    // slot and retries after the directory completes, when the mirror and
    // the image pool are whole; anything still missing then is genuinely
    // dangling and fails loudly. Sized for killhouse's world-material
    // closure (hundreds of nested materials); overflow fails loudly.
    struct DeferredImageSlot
    {
        MaterialTextureDef *table = nullptr;
        uint32_t texIndex = 0;
        uint32_t reference = 0;
        char materialName[64]{};
    };
    DeferredImageSlot deferredImages[2048]{};
    uint32_t deferredImageCount = 0;
    uint32_t deferredImageResolvedCount = 0;
    // Water aliases point at the water root, not its nested pooled image.
    struct WaterEntry { uint32_t reference; water_t *water; };
    WaterEntry waters[256]{};
    uint32_t waterCount = 0;
};

bool RetailWalkLiveLoadTechniqueSet(RetailZoneLoadSession *session,
                                     FsRetailFastfileReader *reader,
                                     uint32_t headerRef,
                                     XAssetHeader *header,
                                     RetailWireTechniqueCache *cache = nullptr);

// Resolve deferred technique-slot aliases after the directory completes
// (all targets widened by then, regardless of stream order). Iterates to
// fixpoint for chains; anything still missing is genuinely dangling and
// fails loudly naming the set, slot, and reference -- except references
// that decode to block 4 inside this zone's image bounds but were never
// widened (linker fill shared across vestigial slots, e.g. killhouse
// sm2/effect emissive+debug slots aliasing one body no loader places):
// those bind a canonical empty technique (passCount 0, draws nothing)
// loudly instead of failing. Returns false on any leftover or bad argument.
bool RetailWireTechniqueCacheResolveDeferred(RetailWireTechniqueCache *cache,
                                             FsRetailFastfileReader *reader);
