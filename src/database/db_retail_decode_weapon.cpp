#include "db_retail_decode_weapon.h"
#include "../universal/retail_asset_trace.h"

#include "db_retail_walk.h"
#include "db_retail_wire.h"
#include "db_retail_decode_fx.h"
#include "db_retail_decode_world.h"
#include "database.h"
#include "../xanim/xanim.h"
#include "../script/scr_stringlist.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

// B7 WeaponDef widening. Every byte-count formula below mirrors
// ReadRetailWeaponDefBody (db_retail_walk.cpp) exactly, in the same stream
// order; the walk-only reader stays the independent oracle. The 2168-byte
// root is parsed from an explicit byte buffer, never cast (LP64 rule:
// native WeaponDef carries 8-byte pointers, so wire offsets never equal
// native offsets -- the native side of every run below is offsetof-derived).
namespace
{
constexpr uint32_t kInlineRef = 0xffffffffu;
constexpr uint32_t kInsertRef = 0xfffffffeu;
constexpr uint32_t kRootBytes = 2168;
constexpr uint32_t kMaxOwnedSL = 8 + 16 + 16;

uint32_t ReadLe32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint16_t ReadLe16(const uint8_t *p)
{
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8));
}

// ILP32 wire offsets (db_load.cpp Load_WeaponDef order, reconciled against
// the walk reader's slot reads and the native field order: the three agree
// to the byte and sum to exactly 2168).
constexpr uint32_t kWInternalName = 0;
constexpr uint32_t kWDisplayName = 4;
constexpr uint32_t kWOverlayName = 8;
constexpr uint32_t kWGunXModel = 12;
constexpr uint32_t kWHandXModel = 76;
constexpr uint32_t kWXAnims = 80;
constexpr uint32_t kWModeName = 212;
constexpr uint32_t kWHideTags = 216;
constexpr uint32_t kWNotetrackKeys = 232;
constexpr uint32_t kWNotetrackValues = 264;
constexpr uint32_t kWViewFlash = 332;
constexpr uint32_t kWWorldFlash = 336;
constexpr uint32_t kWSounds = 340;
constexpr uint32_t kWSoundCount = 45;
constexpr uint32_t kWBounceSound = 520;
constexpr uint32_t kWBounceCount = 29;
constexpr uint32_t kWShellEject = 524;
constexpr uint32_t kWReticleMats = 540;
constexpr uint32_t kWWorldModel = 700;
constexpr uint32_t kWClipModels = 764;
constexpr uint32_t kWHudMats = 780;
constexpr uint32_t kWAmmoName = 804;
constexpr uint32_t kWClipName = 812;
constexpr uint32_t kWSharedAmmoCapName = 832;
constexpr uint32_t kWOverlayMats = 1072;
constexpr uint32_t kWKillMats = 1304;
constexpr uint32_t kWAltWeaponName = 1340;
constexpr uint32_t kWProjectileModel = 1412;
constexpr uint32_t kWProjFx = 1420;
constexpr uint32_t kWProjSounds = 1432;
constexpr uint32_t kWTrailFx = 1704;
constexpr uint32_t kWIgnitionFx = 1732;
constexpr uint32_t kWIgnitionSound = 1736;
constexpr uint32_t kWGraphNames = 1900;
constexpr uint32_t kWKnots0 = 1908;
constexpr uint32_t kWOrigKnots0 = 1916;
constexpr uint32_t kWKnots1 = 1912;
constexpr uint32_t kWOrigKnots1 = 1920;
constexpr uint32_t kWKnotCount0 = 1924;
constexpr uint32_t kWKnotCount1 = 1928;
constexpr uint32_t kWUseHintString = 2012;
constexpr uint32_t kWDropHintString = 2016;
constexpr uint32_t kWScript = 2036;
constexpr uint32_t kWFireRumble = 2152;
constexpr uint32_t kWMeleeRumble = 2156;

// One reference Load_* step: wire offset of the 4-byte slot (or array
// base), operation kind, and count (array length, or 1 for singles; for
// KNOTS the shared per-pair count lives at countOffset instead), plus the
// native destination (compiler-derived base offset, per-element stride).
enum RetailWeaponOpKind : uint8_t
{
    RETAIL_WEAPON_OP_STR,
    RETAIL_WEAPON_OP_STR_ARRAY,
    RETAIL_WEAPON_OP_SCRIPT_ARRAY,
    RETAIL_WEAPON_OP_XMODEL_ARRAY,
    RETAIL_WEAPON_OP_XMODEL,
    RETAIL_WEAPON_OP_FX,
    RETAIL_WEAPON_OP_MATERIAL,
    RETAIL_WEAPON_OP_SOUND,
    RETAIL_WEAPON_OP_SOUND_ARRAY,
    RETAIL_WEAPON_OP_KNOTS,
};

struct RetailWeaponOp
{
    uint32_t wireOffset;
    RetailWeaponOpKind kind;
    uint16_t count;
    uint32_t countOffset;
    size_t nativeBase;
    uint32_t nativeStride;
};

// Scalar runs: {wire offset, native offset (compiler-derived), bytes}.
// Covers every root byte that is not a pointer slot or an interned u16
// run; all members are 4-byte ints/enums/floats or plain float spans, so a
// verbatim copy widens correctly. Sums with the 80 u16 bytes and the 628
// pointer-slot bytes to exactly 2168 (see header).
struct WeaponScalarRun
{
    uint32_t wire;
    size_t native;
    uint32_t bytes;
};

constexpr WeaponScalarRun kWeaponScalarRuns[] = {
    {296, offsetof(WeaponDef, playerAnimType), 36},
    {548, offsetof(WeaponDef, iReticleCenterSize), 152},
    // Wire 788 is the ammoCounterIcon pointer slot (a MATERIAL op below), so
    // this cannot be one 20-byte verbatim copy: on LP64 that would shift the
    // remaining 4-byte fields into the pointer's native padding/halves and
    // leave ammoCounterClip/iStartAmmo at zero (no HUD ammo counter at all).
    // Split around the slot: hudIconRatio alone, then the three scalars that
    // follow it on the wire (ammoCounterIconRatio, ammoCounterClip,
    // iStartAmmo) straight into their native offsets.
    {784, offsetof(WeaponDef, hudIconRatio), 4},
    {792, offsetof(WeaponDef, ammoCounterIconRatio), 12},
    {808, offsetof(WeaponDef, iAmmoIndex), 4},
    {816, offsetof(WeaponDef, iClipIndex), 16},
    {836, offsetof(WeaponDef, iSharedAmmoCapIndex), 12},
    {848, offsetof(WeaponDef, playerDamage), 224},
    {1080, offsetof(WeaponDef, overlayReticle), 224},
    {1308, offsetof(WeaponDef, killIconRatio), 8},
    {1320, offsetof(WeaponDef, dpadIconRatio), 20},
    {1344, offsetof(WeaponDef, altWeaponIndex), 68},
    {1416, offsetof(WeaponDef, projExplosion), 4},
    {1424, offsetof(WeaponDef, projExplosionEffectForceNormalUp), 4},
    {1440, offsetof(WeaponDef, bProjImpactExplode), 28},
    {1468, offsetof(WeaponDef, parallelBounce), 116},
    {1584, offsetof(WeaponDef, perpendicularBounce), 116},
    {1708, offsetof(WeaponDef, vProjectileColor), 12},
    {1720, offsetof(WeaponDef, guidedMissileType), 12},
    {1740, offsetof(WeaponDef, fAdsAimPitch), 160},
    {1924, offsetof(WeaponDef, accuracyGraphKnotCount), 16},
    {1940, offsetof(WeaponDef, iPositionReloadTransTime), 72},
    {2020, offsetof(WeaponDef, iUseHintStringIndex), 16},
    {2040, offsetof(WeaponDef, fOOPosAnimLength), 8},
    {2048, offsetof(WeaponDef, minDamage), 28},
    {2076, offsetof(WeaponDef, locationDamageMultipliers), 76},
    {2160, offsetof(WeaponDef, adsDofStart), 8},
};

struct WeaponDecodeContext
{
    RetailZoneLoadSession *session = nullptr;
    FsRetailFastfileReader *reader = nullptr;
    RetailWeaponDecodeResult result = RETAIL_WEAPON_DECODE_OK;
    const char *assetName = "";
    // Per-occurrence SL_* IDs interned by this body (hideTags[8] +
    // notetrack keys/values[16+16]); unwound on failure, owned by the
    // registered asset on success (RetailWeaponFree on unload).
    uint16_t ownedSL[kMaxOwnedSL]{};
    uint32_t ownedCount = 0;

    void Fail(RetailWeaponDecodeResult code, const char *stage)
    {
        if (result == RETAIL_WEAPON_DECODE_OK)
        {
            result = code;
            Com_Printf(0, "RetailWalkLiveLoadWeaponDef: '%s' %s failed (%s)\n",
                       assetName ? assetName : "(null)", stage,
                       RetailWeaponDecodeResultName(code));
        }
    }

    bool Ok() const { return result == RETAIL_WEAPON_DECODE_OK; }

    bool Stream(uint32_t block, uint32_t bytes, uint32_t alignment, const char *stage)
    {
        if (!RetailZoneLoadSessionReadStream(session, reader, block, bytes, alignment))
        {
            Fail(RETAIL_WEAPON_DECODE_TRUNCATED, stage);
            return false;
        }
        return true;
    }

    void *Alloc(std::size_t bytes, std::size_t alignment, const char *stage)
    {
        void *memory = RetailZoneLoadSessionAlloc(session, bytes, alignment);
        if (!memory)
            Fail(RETAIL_WEAPON_DECODE_OUT_OF_ARENA, stage);
        return memory;
    }
};

// Resolve one u16 wire index into this zone's script-string table (same
// table B3 builds: the open streams count u32 slots up front, inline
// strings pack back-to-back after them). Shares B3's cache in
// session->scriptStringOffsets when an XAnim already built it.
bool BuildWeaponScriptTable(WeaponDecodeContext *context)
{
    RetailZoneLoadSession *session = context->session;
    if (session->scriptStringOffsets)
        return true;
    const uint32_t count = session->scriptStringCount;
    if (!count || count > 65536)
    {
        context->Fail(RETAIL_WEAPON_DECODE_SCRIPT_TABLE_MISSING, "script table count");
        return false;
    }
    const XBlock &block = session->zoneMemory->blocks[4];
    if (count > block.size / 4u)
    {
        context->Fail(RETAIL_WEAPON_DECODE_SCRIPT_TABLE_MISSING, "script table slots");
        return false;
    }
    uint32_t *offsets = static_cast<uint32_t *>(
        RetailZoneLoadSessionAlloc(session, static_cast<std::size_t>(count) * 4u, 4));
    if (!offsets)
    {
        context->Fail(RETAIL_WEAPON_DECODE_OUT_OF_ARENA, "script table index");
        return false;
    }
    for (uint32_t i = 0; i < count; ++i)
        offsets[i] = UINT32_MAX;
    uint32_t pos = count * 4u;
    XBlock blocks[9]{};
    for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
        blocks[blockIndex] = session->zoneMemory->blocks[blockIndex];
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t slot = ReadLe32(block.data + i * 4u);
        if (slot == kInlineRef)
        {
            uint32_t end = pos;
            while (end < block.size && block.data[end] != 0)
                ++end;
            if (end >= block.size)
            {
                context->Fail(RETAIL_WEAPON_DECODE_SCRIPT_TABLE_MISSING, "script string bytes");
                return false;
            }
            offsets[i] = pos;
            pos = end + 1;
        }
        else if (slot != 0)
        {
            RetailWireToken token{};
            RetailPtr32 encoded{};
            encoded.encoded = slot;
            if (RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) &&
                token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4 &&
                token.offset < block.size)
            {
                uint32_t end = token.offset;
                bool printable = true;
                while (end < block.size && block.data[end] != 0)
                {
                    const unsigned char c = block.data[end];
                    if (c < 0x20 || c >= 0x7f)
                    {
                        printable = false;
                        break;
                    }
                    ++end;
                }
                if (printable && end < block.size)
                    offsets[i] = token.offset;
            }
        }
    }
    session->scriptStringOffsets = offsets;
    {
        static int s_tableLog = 0;
        if (s_tableLog < 4)
        {
            ++s_tableLog;
            uint32_t resolved = 0, zero = 0, unresolved = 0;
            for (uint32_t i = 0; i < count; ++i)
            {
                if (offsets[i] != UINT32_MAX)
                    ++resolved;
                else if (ReadLe32(block.data + i * 4u) == 0)
                    ++zero;
                else
                    ++unresolved;
            }
            Com_Printf(0, "WPN_SCRIPT_TABLE count=%u resolved=%u zero=%u unresolved=%u\n",
                       count, resolved, zero, unresolved);
        }
    }
    return true;
}

const char *ZoneWeaponScriptString(WeaponDecodeContext *context, uint32_t wireIndex,
                                   const char *stage)
{
    // No index-0 special case: 0 is the first table string (unused slots
    // in real zones point at an empty first entry, which resolves to ""
    // and binds 0 downstream -- the engine's own null convention).
    if (!BuildWeaponScriptTable(context))
        return nullptr;
    RetailZoneLoadSession *session = context->session;
    if (wireIndex >= session->scriptStringCount)
    {
        context->Fail(RETAIL_WEAPON_DECODE_INDEX_OOB, stage);
        return nullptr;
    }
    const uint32_t offset = session->scriptStringOffsets[wireIndex];
    if (offset == UINT32_MAX)
    {
        // A raw null entry is the engine's null ScriptString (index 0 is a
        // real, loadable entry -- "No index-0 special case" above -- and an
        // unused slot is a declared null, which binds 0 without interning).
        // Only a non-null entry that failed table reconstruction is a
        // genuine miss.
        if (ReadLe32(session->zoneMemory->blocks[4].data + wireIndex * 4u) == 0)
            return "";
        static int s_missLog = 0;
        if (s_missLog < 24)
        {
            ++s_missLog;
            Com_Printf(0, "WPN_SCRIPT_MISS idx=%u raw=0x%08x count=%u stage=%s\n", wireIndex,
                       ReadLe32(session->zoneMemory->blocks[4].data + wireIndex * 4u),
                       session->scriptStringCount, stage);
        }
        context->Fail(RETAIL_WEAPON_DECODE_SCRIPT_STRING_MISSING, stage);
        return nullptr;
    }
    const XBlock &block = session->zoneMemory->blocks[4];
    if (offset >= block.size)
    {
        context->Fail(RETAIL_WEAPON_DECODE_SCRIPT_STRING_MISSING, stage);
        return nullptr;
    }
    return reinterpret_cast<const char *>(block.data + offset);
}

// Intern one resolved table string with exactly one per-occurrence
// reference (B3 rule). An empty resolution binds 0 (the null ScriptString,
// never refcounted) instead of interning.
bool InternWeaponScriptString(WeaponDecodeContext *context, const char *str, uint16_t *out,
                              const char *stage)
{
    if (!str || !str[0])
    {
        *out = 0;
        return true;
    }
    const uint32_t found = SL_FindString(str);
    uint32_t id = 0;
    if (found)
    {
        SL_AddRefToString(found);
        id = found;
    }
    else
    {
        // Engine-table exhaustion is a resource failure, not a data gap:
        // the walk reader never interns, so this stays zone-fatal.
        id = SL_GetString(str, 4u);
        if (!id || id >= 0x10000u)
        {
            context->Fail(RETAIL_WEAPON_DECODE_OUT_OF_ARENA, stage);
            return false;
        }
    }
    if (context->ownedCount >= kMaxOwnedSL)
    {
        SL_RemoveRefToString(id);
        context->Fail(RETAIL_WEAPON_DECODE_OUT_OF_ARENA, stage);
        return false;
    }
    context->ownedSL[context->ownedCount++] = static_cast<uint16_t>(id);
    *out = static_cast<uint16_t>(id);
    return true;
}

// Stream one inline-or-alias XString: the -1 form carries its bytes here,
// any other nonzero value names an absolute block-4 offset, 0 binds null.
// allowEmpty accepts a bare NUL as "" (display strings); the asset name
// itself must be non-empty to hash for lookup.
bool StreamWeaponString(WeaponDecodeContext *context, uint32_t nameRef, const char **out,
                        const char *stage, bool allowEmpty)
{
    *out = nullptr;
    if (!nameRef)
        return true;
    const uint32_t stringStart = context->session->wire.cursor[4];
    if (nameRef == kInlineRef)
    {
        constexpr uint32_t kMaxNameBytes = 4096;
        for (;;)
        {
            if (context->session->wire.cursor[4] - stringStart >= kMaxNameBytes ||
                !context->Stream(4, 1, 1, stage))
                return false;
            if (context->session->zoneMemory->blocks[4]
                    .data[context->session->wire.cursor[4] - 1] == 0)
                break;
        }
    }
    else
    {
        XBlock blocks[9]{};
        for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
            blocks[blockIndex] = context->session->zoneMemory->blocks[blockIndex];
        RetailWireToken token{};
        RetailPtr32 encoded{};
        encoded.encoded = nameRef;
        if (!RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) ||
            token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        {
            // Names an offset the stream cannot supply; the walk reader
            // fails there too, so this stays zone-fatal.
            context->Fail(RETAIL_WEAPON_DECODE_TRUNCATED, stage);
            return false;
        }
        const XBlock &block = context->session->zoneMemory->blocks[4];
        // No printability gate: retail names carry high bytes verbatim
        // (OAT: m4_silencer's ammoName is "5.56 x 45 mm nato" with a
        // non-ASCII separator); the reference streams them unchecked and
        // the walk reader resolves aliases without one. Bounds and
        // termination are still enforced.
        uint32_t end = token.offset;
        while (end < block.size && block.data[end] != 0)
            ++end;
        if (end >= block.size || (!allowEmpty && end == token.offset))
        {
            context->Fail(RETAIL_WEAPON_DECODE_BAD_NAME, stage);
            return false;
        }
        const std::size_t length = static_cast<std::size_t>(end - token.offset) + 1u;
        char *copy = static_cast<char *>(context->Alloc(length, 1, stage));
        if (!context->Ok())
            return false;
        std::memcpy(copy, block.data + token.offset, length);
        *out = copy;
        return true;
    }
    const XBlock &block = context->session->zoneMemory->blocks[4];
    const std::size_t length =
        static_cast<std::size_t>(context->session->wire.cursor[4] - stringStart);
    if (length < 2 && !(allowEmpty && length == 1))
    {
        context->Fail(RETAIL_WEAPON_DECODE_BAD_NAME, stage);
        return false;
    }
    char *copy = static_cast<char *>(context->Alloc(length, 1, stage));
    if (!context->Ok())
        return false;
    std::memcpy(copy, block.data + stringStart, length);
    *out = copy;
    return true;
}
} // namespace

static constexpr RetailWeaponOp kRetailWeaponOps[] = {
    {kWInternalName, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, szInternalName), 0},
    {kWDisplayName, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, szDisplayName), 0},
    {kWOverlayName, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, szOverlayName), 0},
    {kWGunXModel, RETAIL_WEAPON_OP_XMODEL_ARRAY, 16, 0, offsetof(WeaponDef, gunXModel), 8},
    {kWHandXModel, RETAIL_WEAPON_OP_XMODEL, 1, 0, offsetof(WeaponDef, handXModel), 0},
    {kWXAnims, RETAIL_WEAPON_OP_STR_ARRAY, 33, 0, offsetof(WeaponDef, szXAnims), 8},
    {kWModeName, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, szModeName), 0},
    {kWHideTags, RETAIL_WEAPON_OP_SCRIPT_ARRAY, 8, 0, offsetof(WeaponDef, hideTags), 2},
    {kWNotetrackKeys, RETAIL_WEAPON_OP_SCRIPT_ARRAY, 16, 0, offsetof(WeaponDef, notetrackSoundMapKeys), 2},
    {kWNotetrackValues, RETAIL_WEAPON_OP_SCRIPT_ARRAY, 16, 0, offsetof(WeaponDef, notetrackSoundMapValues), 2},
    {kWViewFlash, RETAIL_WEAPON_OP_FX, 1, 0, offsetof(WeaponDef, viewFlashEffect), 0},
    {kWWorldFlash, RETAIL_WEAPON_OP_FX, 1, 0, offsetof(WeaponDef, worldFlashEffect), 0},
    {kWSounds, RETAIL_WEAPON_OP_SOUND, kWSoundCount, 0, offsetof(WeaponDef, pickupSound), 8},
    {kWBounceSound, RETAIL_WEAPON_OP_SOUND_ARRAY, kWBounceCount, 0, offsetof(WeaponDef, bounceSound), 0},
    {kWShellEject, RETAIL_WEAPON_OP_FX, 4, 0, offsetof(WeaponDef, viewShellEjectEffect), 8},
    {kWReticleMats, RETAIL_WEAPON_OP_MATERIAL, 2, 0, offsetof(WeaponDef, reticleCenter), 8},
    {kWWorldModel, RETAIL_WEAPON_OP_XMODEL_ARRAY, 16, 0, offsetof(WeaponDef, worldModel), 8},
    {kWClipModels, RETAIL_WEAPON_OP_XMODEL, 4, 0, offsetof(WeaponDef, worldClipModel), 8},
    {kWHudMats, RETAIL_WEAPON_OP_MATERIAL, 1, 0, offsetof(WeaponDef, hudIcon), 0},
    {kWHudMats + 8, RETAIL_WEAPON_OP_MATERIAL, 1, 0, offsetof(WeaponDef, ammoCounterIcon), 0},
    {kWAmmoName, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, szAmmoName), 0},
    {kWClipName, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, szClipName), 0},
    {kWSharedAmmoCapName, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, szSharedAmmoCapName), 0},
    {kWOverlayMats, RETAIL_WEAPON_OP_MATERIAL, 2, 0, offsetof(WeaponDef, overlayMaterial), 8},
    {kWKillMats, RETAIL_WEAPON_OP_MATERIAL, 1, 0, offsetof(WeaponDef, killIcon), 0},
    {kWKillMats + 12, RETAIL_WEAPON_OP_MATERIAL, 1, 0, offsetof(WeaponDef, dpadIcon), 0},
    {kWAltWeaponName, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, szAltWeaponName), 0},
    {kWProjectileModel, RETAIL_WEAPON_OP_XMODEL, 1, 0, offsetof(WeaponDef, projectileModel), 0},
    {kWProjFx, RETAIL_WEAPON_OP_FX, 1, 0, offsetof(WeaponDef, projExplosionEffect), 0},
    {kWProjFx + 8, RETAIL_WEAPON_OP_FX, 1, 0, offsetof(WeaponDef, projDudEffect), 0},
    {kWProjSounds, RETAIL_WEAPON_OP_SOUND, 2, 0, offsetof(WeaponDef, projExplosionSound), 8},
    {kWTrailFx, RETAIL_WEAPON_OP_FX, 1, 0, offsetof(WeaponDef, projTrailEffect), 0},
    {kWIgnitionFx, RETAIL_WEAPON_OP_FX, 1, 0, offsetof(WeaponDef, projIgnitionEffect), 0},
    {kWIgnitionSound, RETAIL_WEAPON_OP_SOUND, 1, 0, offsetof(WeaponDef, projIgnitionSound), 0},
    {kWGraphNames, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, accuracyGraphName), 0},
    {kWKnots0, RETAIL_WEAPON_OP_KNOTS, 1, kWKnotCount0, offsetof(WeaponDef, accuracyGraphKnots), 8},
    {kWOrigKnots0, RETAIL_WEAPON_OP_KNOTS, 1, kWKnotCount0, offsetof(WeaponDef, originalAccuracyGraphKnots), 8},
    {kWGraphNames + 4, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, accuracyGraphName) + 8, 0},
    {kWKnots1, RETAIL_WEAPON_OP_KNOTS, 1, kWKnotCount1, offsetof(WeaponDef, accuracyGraphKnots) + 8, 8},
    {kWOrigKnots1, RETAIL_WEAPON_OP_KNOTS, 1, kWKnotCount1, offsetof(WeaponDef, originalAccuracyGraphKnots) + 8, 8},
    {kWUseHintString, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, szUseHintString), 0},
    {kWDropHintString, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, dropHintString), 0},
    {kWScript, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, szScript), 0},
    {kWFireRumble, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, fireRumble), 0},
    {kWMeleeRumble, RETAIL_WEAPON_OP_STR, 1, 0, offsetof(WeaponDef, meleeImpactRumble), 0},
};

// Build-time table checks (doc checklist: the operation count and offsets
// stay pinned to the reference 98 Load_* steps): the grouped entries
// expand to exactly 98 steps (same-kind contiguous runs count per slot,
// true arrays count once), and every wire offset is in-range and unique.
// Stream ORDER matches the walk reader by construction (the table follows
// reference order, including the one inversion: accuracyGraphName[1]
// @1904 streams after the first knot pair); order agreement is enforced
// at runtime by the proof's live-vs-walk cursor check.
constexpr uint32_t RetailWeaponExpandedSteps()
{
    uint32_t steps = 0;
    for (size_t i = 0; i < sizeof(kRetailWeaponOps) / sizeof(kRetailWeaponOps[0]); ++i)
    {
        switch (kRetailWeaponOps[i].kind)
        {
        case RETAIL_WEAPON_OP_STR_ARRAY:
        case RETAIL_WEAPON_OP_SCRIPT_ARRAY:
        case RETAIL_WEAPON_OP_XMODEL_ARRAY:
        case RETAIL_WEAPON_OP_SOUND_ARRAY:
        case RETAIL_WEAPON_OP_KNOTS:
            steps += 1;
            break;
        default:
            steps += kRetailWeaponOps[i].count;
            break;
        }
    }
    return steps;
}

constexpr uint32_t RetailWeaponOpCount()
{
    return static_cast<uint32_t>(sizeof(kRetailWeaponOps) / sizeof(kRetailWeaponOps[0]));
}

constexpr bool RetailWeaponOpsSound()
{
    const size_t n = sizeof(kRetailWeaponOps) / sizeof(kRetailWeaponOps[0]);
    for (size_t i = 0; i < n; ++i)
    {
        if (kRetailWeaponOps[i].wireOffset >= kRootBytes)
            return false;
        for (size_t j = i + 1; j < n; ++j)
        {
            if (kRetailWeaponOps[j].wireOffset == kRetailWeaponOps[i].wireOffset)
                return false;
        }
    }
    return true;
}

// Mechanical guard for the ammoCounterClip class of bug: a scalar run is a
// verbatim wire->native widening copy, so an op (pointer/asset) slot inside
// one is both overwritten and shifts every scalar after it into the native
// pointer's padding/halves.  The same holds natively: a run must not cover
// bytes another op writes.  Keep both directions disjoint at compile time
// so a future table edit cannot silently drop a field (the HUD ammo counter
// fields were exactly this).
constexpr bool RetailWeaponOpSlotsClearOfScalarRuns()
{
    for (const RetailWeaponOp &op : kRetailWeaponOps)
    {
        uint32_t slots = op.count;
        uint32_t slotBytes = 4;
        if (op.kind == RETAIL_WEAPON_OP_SCRIPT_ARRAY)
            slotBytes = 2; // interned u16 script-string IDs, packed
        else if (op.kind == RETAIL_WEAPON_OP_SOUND_ARRAY || op.kind == RETAIL_WEAPON_OP_KNOTS)
            slots = 1;
        for (uint32_t i = 0; i < slots; ++i)
        {
            const uint32_t wire = op.wireOffset + i * slotBytes;
            for (const WeaponScalarRun &run : kWeaponScalarRuns)
            {
                if (wire >= run.wire && wire < run.wire + run.bytes)
                    return false;
            }
        }
    }
    return true;
}

constexpr bool RetailWeaponScalarRunsClearOfOpNatives()
{
    for (const WeaponScalarRun &run : kWeaponScalarRuns)
    {
        const size_t runStart = run.native;
        const size_t runEnd = run.native + run.bytes;
        for (const RetailWeaponOp &op : kRetailWeaponOps)
        {
            size_t element = sizeof(void *);
            size_t stride = op.nativeStride;
            size_t count = op.count;
            switch (op.kind)
            {
            case RETAIL_WEAPON_OP_SCRIPT_ARRAY:
                element = 2;
                stride = 2;
                break;
            case RETAIL_WEAPON_OP_SOUND_ARRAY:
            case RETAIL_WEAPON_OP_KNOTS:
                count = 1;
                stride = sizeof(void *);
                break;
            default:
                if (!stride)
                    stride = sizeof(void *);
                break;
            }
            const size_t opStart = op.nativeBase;
            const size_t opEnd = op.nativeBase + stride * (count - 1) + element;
            if (runStart < opEnd && opStart < runEnd)
                return false;
        }
    }
    return true;
}

static_assert(RetailWeaponExpandedSteps() == 98,
              "kRetailWeaponOps must expand to the 98 Load_WeaponDef steps");
static_assert(RetailWeaponOpsSound(),
              "kRetailWeaponOps wire offsets must be in-range and unique");
static_assert(RetailWeaponOpSlotsClearOfScalarRuns(),
              "a wire op slot falls inside a scalar widening run; split the run around it");
static_assert(RetailWeaponScalarRunsClearOfOpNatives(),
              "a scalar widening run overlaps a native op destination; split the run");
const uint32_t kRetailWeaponOpCount =
    static_cast<uint32_t>(sizeof(kRetailWeaponOps) / sizeof(kRetailWeaponOps[0]));

// Native LP64 layout pins for the widened pointer leaves (wire sizes stay
// frozen above as byte offsets; these guard the arena shapes instead).
static_assert(sizeof(void *) == 8);
static_assert(sizeof(WeaponDef::hideTags) == 16);
static_assert(sizeof(WeaponDef::notetrackSoundMapKeys) == 32);
static_assert(sizeof(WeaponDef::parallelBounce) == 29 * 4);

const char *RetailWeaponDecodeResultName(RetailWeaponDecodeResult result)
{
    static const char *const names[] = {
        "ok", "bad_argument", "bad_root", "bad_name", "count_mismatch",
        "index_oob", "script_table_missing", "script_string_missing",
        "unresolved_reference", "truncated", "out_of_arena",
        "registration_failed",
    };
    return result >= RETAIL_WEAPON_DECODE_OK && result <= RETAIL_WEAPON_DECODE_REGISTRATION_FAILED ?
        names[result] : "invalid_result";
}

void RetailWeaponFree(WeaponDef *weapon)
{
    if (!weapon)
        return;
    for (uint32_t i = 0; i < 8; ++i)
        if (weapon->hideTags[i])
            SL_RemoveRefToString(weapon->hideTags[i]);
    for (uint32_t i = 0; i < 16; ++i)
    {
        if (weapon->notetrackSoundMapKeys[i])
            SL_RemoveRefToString(weapon->notetrackSoundMapKeys[i]);
        if (weapon->notetrackSoundMapValues[i])
            SL_RemoveRefToString(weapon->notetrackSoundMapValues[i]);
    }
}

// deferred weapon sound references. A weapon body streams before the
// snd_alias_list_t it names (retail killhouse: usp @458 names
// weap_usp45_fire_npc later in the same zone; common's turret/vehicle
// weapons name killhouse-owned sounds). The original Load_SndAliasCustom
// resolves by name at stream time through DB_FindXAssetHeader, whose miss
// path manufactures the per-name engine default (`null`) -- a placeholder
// baked into the weapon. Instead, record the slot and copy the name here
// (never keep the reader's block-4 pointer: it is freed while the zone
// stays live), then re-resolve after every zone load exactly like
// the material-alias pass. The final zone load in the P6 route must
// leave zero outstanding; a name no loaded zone declares stays visible
// there instead of being defaulted.
namespace
{
constexpr uint32_t kDeferredWeaponSoundCapacity = 2048;
constexpr uint32_t kDeferredWeaponSoundNameBytes = 96;
struct DeferredWeaponSound
{
    void **slot;
    uint32_t zoneIndex;
    char name[kDeferredWeaponSoundNameBytes];
};
DeferredWeaponSound g_deferredWeaponSounds[kDeferredWeaponSoundCapacity]{};
uint32_t g_deferredWeaponSoundCount = 0;
uint32_t g_deferredWeaponSoundOverflow = 0;
} // namespace

bool RetailDeferWeaponSound(void **slot, const char *name, uint32_t zoneIndex)
{
    if (!slot || !name || !name[0])
        return false;
    for (uint32_t i = 0; i < g_deferredWeaponSoundCount; ++i)
    {
        DeferredWeaponSound &entry = g_deferredWeaponSounds[i];
        if (!entry.name[0])
            continue;
        if (entry.slot == slot && !std::strcmp(entry.name, name))
            return true;
    }
    if (g_deferredWeaponSoundCount >= kDeferredWeaponSoundCapacity)
    {
        ++g_deferredWeaponSoundOverflow;
        return false;
    }
    DeferredWeaponSound &entry = g_deferredWeaponSounds[g_deferredWeaponSoundCount++];
    entry.slot = slot;
    entry.zoneIndex = zoneIndex;
    std::strncpy(entry.name, name, kDeferredWeaponSoundNameBytes - 1);
    entry.name[kDeferredWeaponSoundNameBytes - 1] = '\0';
    return true;
}

uint32_t RetailWeaponDeferredSoundCount()
{
    uint32_t outstanding = 0;
    for (uint32_t i = 0; i < g_deferredWeaponSoundCount; ++i)
        if (g_deferredWeaponSounds[i].name[0])
            ++outstanding;
    return outstanding;
}

uint32_t RetailWeaponDeferredSoundOverflow()
{
    return g_deferredWeaponSoundOverflow;
}

uint32_t RetailResolveDeferredWeaponSounds()
{
    const uint32_t outstandingBefore = RetailWeaponDeferredSoundCount();
    uint32_t resolved = 0;
    uint32_t unresolved = 0;
    const char *firstName = nullptr;
    for (uint32_t i = 0; i < g_deferredWeaponSoundCount; ++i)
    {
        DeferredWeaponSound &entry = g_deferredWeaponSounds[i];
        if (!entry.name[0])
            continue;
        const XAssetHeader found =
            DB_FindXAssetHeaderNoDefault(ASSET_TYPE_SOUND, entry.name);
        if (found.sound && found.data)
        {
            *entry.slot = found.data;
            entry.name[0] = '\0';
            entry.slot = nullptr;
            ++resolved;
            continue;
        }
        ++unresolved;
        if (!firstName)
            firstName = entry.name;
    }
    if (outstandingBefore || resolved)
    {
        Com_Printf(0, "KILLHOUSE_WEAPON_SOUND_PATCH outstanding_before=%u resolved=%u "
                      "unresolved=%u outstanding=%u first='%s'\n",
                   outstandingBefore, resolved, unresolved,
                   RetailWeaponDeferredSoundCount(),
                   firstName ? firstName : "");
    }
    return unresolved;
}

void RetailWeaponZoneUnloaded(uint32_t zoneIndex)
{
    if (zoneIndex == 0)
        return;
    for (uint32_t i = 0; i < g_deferredWeaponSoundCount; ++i)
    {
        DeferredWeaponSound &entry = g_deferredWeaponSounds[i];
        if (entry.name[0] && entry.zoneIndex == zoneIndex)
        {
            entry.name[0] = '\0';
            entry.slot = nullptr;
        }
    }
}

// Resolve one shared (alias-form) 29-entry WeaponDef bounce pointer array
// from the pristine zone mirror. Retail common.ff's smoke/flash grenades use
// this shape: the root slot is a block-4 token naming the array a declarer
// streamed inline earlier, so the mirror holds 29 entry slots followed, in
// entry order, by the inner pointer slot and name bytes of every -1 entry
// (the walk reader's ReadRetailXStringPtrSlot layout). Null entries stay
// null; a non-null entry this resolver cannot name or bind fails and reports
// its index/name to the caller (loud defer, never a silent null). A
// direct-name-offset entry (not a pointer slot) is accepted as the fallback
// shape. With deferUnresolved (the live load path), a well-formed entry
// whose list is not registered yet is recorded for the after-zone pass
// instead of failing, so a later-declared bounce alias still binds the real
// list. Returns false for a malformed/OOB alias or an unbound entry.
bool RetailWeaponResolveBounceArray(RetailZoneLoadSession *session, uint32_t aliasRef,
                                    void **bounce, uint32_t *unresolvedIndex,
                                    const char **unresolvedName, bool deferUnresolved)
{
    if (unresolvedIndex)
        *unresolvedIndex = UINT32_MAX;
    if (unresolvedName)
        *unresolvedName = nullptr;
    if (!session || !session->active || !bounce || !aliasRef || aliasRef == kInlineRef ||
        aliasRef == kInsertRef || !session->zoneMemory)
        return false;
    const XBlock &block = session->zoneMemory->blocks[4];
    const uint8_t *block4 = block.data;
    const uint32_t block4Size = block.size;
    const uint32_t base = (aliasRef - 1u) & 0x0fffffffu;
    if (!block4 || base > block4Size || block4Size - base < kWBounceCount * 4u)
        return false;
    XBlock blocks[9]{};
    for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
        blocks[blockIndex] = session->zoneMemory->blocks[blockIndex];
    auto stringAt = [&](uint32_t ref, uint32_t inlineStart) -> const char *
    {
        uint32_t off = 0;
        if (ref == kInlineRef)
            off = inlineStart;
        else
        {
            RetailWireToken token{};
            RetailPtr32 encoded{};
            encoded.encoded = ref;
            if (!RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
                return nullptr;
            off = token.offset;
        }
        if (off >= block4Size)
            return nullptr;
        uint32_t end = off;
        while (end < block4Size && block4[end] != 0)
            ++end;
        if (end >= block4Size)
            return nullptr;
        return reinterpret_cast<const char *>(block4 + off);
    };
    auto aliasTarget = [&](uint32_t ref) -> const char *
    {
        const uint32_t off = (ref - 1u) & 0x0fffffffu;
        if (off >= block4Size)
            return nullptr;
        const uint32_t inner = ReadLe32(block4 + off);
        if (inner == kInlineRef)
            return stringAt(kInlineRef,
                            off + 4u < block4Size ? off + 4u : block4Size);
        return stringAt(inner, 0);
    };
    uint32_t cursor = base + kWBounceCount * 4u;
    for (uint32_t i = 0; i < kWBounceCount; ++i)
    {
        const uint32_t entry = ReadLe32(block4 + base + i * 4u);
        if (!entry)
            continue;
        const char *soundName = nullptr;
        if (entry == kInlineRef)
        {
            // ReadRetailXStringPtrSlot uses the engine's 4-byte stream
            // alignment before each inner pointer slot; the preceding
            // inline string is byte-sized and may leave the cursor
            // unaligned.
            cursor = (cursor + 3u) & ~3u;
            if (static_cast<uint64_t>(cursor) + 4u > block4Size)
                return false;
            const uint32_t innerRef = ReadLe32(block4 + cursor);
            cursor += 4u;
            if (innerRef == kInlineRef)
            {
                uint32_t end = cursor;
                while (end < block4Size && block4[end] != 0)
                    ++end;
                if (end >= block4Size)
                    return false;
                soundName = reinterpret_cast<const char *>(block4 + cursor);
                cursor = end + 1u;
            }
            else
            {
                // An inline bounce entry's XStringPtr may itself be a
                // DB_ConvertOffsetToAlias slot. Follow that slot first;
                // only the direct-name shape falls back to reading the
                // token as a string offset.
                soundName = aliasTarget(innerRef);
                if (!soundName)
                    soundName = stringAt(innerRef, 0);
            }
        }
        else
        {
            soundName = aliasTarget(entry);
            if (!soundName || !soundName[0])
                soundName = stringAt(entry, 0);
        }
        if (!soundName || !soundName[0])
        {
            if (unresolvedIndex)
                *unresolvedIndex = i;
            return false;
        }
        const XAssetHeader found =
            deferUnresolved ? DB_FindXAssetHeaderNoDefault(ASSET_TYPE_SOUND, soundName)
                            : DB_FindXAssetHeader(ASSET_TYPE_SOUND, soundName);
        if (!found.sound)
        {
            // live path: a declared entry whose owner streams later is
            // deferred to the after-zone pass (the same contract as the
            // single sound slots), never the engine default entry. The
            // synthetic pointer-class proof keeps the old strict contract so
            // its miss=loud expectation stays meaningful.
            if (deferUnresolved &&
                RetailDeferWeaponSound(&bounce[i], soundName, session->zoneIndex))
                continue;
            if (unresolvedIndex)
                *unresolvedIndex = i;
            if (unresolvedName)
                *unresolvedName = soundName;
            return false;
        }
        bounce[i] = found.data;
    }
    return true;
}

RetailWeaponDecodeResult RetailWalkLiveLoadWeaponDef(RetailZoneLoadSession *session,
                                                         FsRetailFastfileReader *reader,
                                                         RetailWorldLoadContext *worldContext,
                                                         uint32_t headerRef, XAssetHeader *header,
                                                         RetailWalkDirectoryRecord *record,
                                                         RetailWalkDirectoryResult *summary)
{
    if (!session || !session->active || !reader || !header || !record || !summary)
        return RETAIL_WEAPON_DECODE_BAD_ARGUMENT;
    *header = XAssetHeader{};
    if (headerRef != kInlineRef)
    {
        Com_Printf(0, "RetailWalkLiveLoadWeaponDef: unsupported header 0x%08x\n", headerRef);
        return RETAIL_WEAPON_DECODE_BAD_ROOT;
    }
    WeaponDecodeContext context;
    context.session = session;
    context.reader = reader;

    // Temp-block root: consume 2168 bytes, snapshot them, rewind (B3's
    // temp-scope contract).
    const uint32_t bodyStart = (session->wire.cursor[0] + 3u) & ~3u;
    if (!context.Stream(0, kRootBytes, 4, "root"))
        return context.result;
    uint8_t root[kRootBytes]{};
    std::memcpy(root, session->zoneMemory->blocks[0].data + bodyStart, kRootBytes);
    RetailWireBlocksRewind(&session->wire, 0, bodyStart);

    auto decodeBody = [&]() -> RetailWeaponDecodeResult {
    // Transaction-owned native root, zeroed first (the reference never
    // streams originalAccuracyGraphKnotCount, so zero is its value).
    WeaponDef *def = static_cast<WeaponDef *>(
        context.Alloc(sizeof(WeaponDef), alignof(WeaponDef), "def"));
    if (!context.Ok())
        return context.result;
    std::memset(def, 0, sizeof(*def));

    // Deferral state: first DEFER-class cause wins. Unlike FATAL results
    // it never touches context.result, so accounting continues to the end
    // of the body and the zone keeps a byte-exact walked-deferred asset
    // instead of aborting.
    RetailWeaponDecodeResult deferCode = RETAIL_WEAPON_DECODE_OK;
    char deferStage[96]{};
    auto noteDefer = [&](RetailWeaponDecodeResult code, const char *stage) {
        if (deferCode == RETAIL_WEAPON_DECODE_OK)
        {
            deferCode = code;
            std::snprintf(deferStage, sizeof(deferStage), "%s", stage);
            Com_Printf(0, "RetailWalkLiveLoadWeaponDef: '%s' %s deferred (%s)\n",
                       context.assetName ? context.assetName : "(null)", stage,
                       RetailWeaponDecodeResultName(code));
        }
    };

    // Scalar runs: verbatim 4-byte/int/float copies, wire side explicit,
    // native side compiler-derived.
    for (size_t run = 0; run < sizeof(kWeaponScalarRuns) / sizeof(kWeaponScalarRuns[0]); ++run)
    {
        std::memcpy(reinterpret_cast<uint8_t *>(def) + kWeaponScalarRuns[run].native,
                    root + kWeaponScalarRuns[run].wire, kWeaponScalarRuns[run].bytes);
    }

    // Asset name first: every later message names it. An empty name
    // defers (the walk accounts nameless bodies fine; registration needs
    // a hashable name).
    const char *assetName = nullptr;
    if (!StreamWeaponString(&context, ReadLe32(root + kWInternalName), &assetName,
                            "szInternalName", false))
        return context.result;
    if (!assetName || !assetName[0])
    {
        noteDefer(RETAIL_WEAPON_DECODE_BAD_NAME, "szInternalName");
        context.result = RETAIL_WEAPON_DECODE_OK;
    }
    else
    {
        context.assetName = assetName;
        def->szInternalName = assetName;
    }

    // The reference steps in exact walk/stream order (op 0 was the name
    // above). Block-4 bytes are consumed in this order only, so the live
    // cursor always agrees with the walk-only reader's. Null asset slots
    // bind null; anything else reports UNRESOLVED_REFERENCE for the
    // caller to defer (no live owner before B2/B4/B6).
    char stage[96]{};
    // B2: Load_snd_alias_list_name fields are XStringPtr slots -- an inline
    // form streams a 4-byte inner pointer slot and the name bytes, an alias
    // form points at an already-loaded pointer slot. Both reduce to the
    // exact name bytes the walk reader landed; the list itself is bound by
    // name through the registry (the top-level SOUND branch registered it).
    const uint8_t *soundBlock4 = session->zoneMemory->blocks[4].data;
    const uint32_t soundBlock4Size = session->zoneMemory->blocks[4].size;
    auto SoundStringAt = [&](uint32_t ref, uint32_t inlineStart) -> const char *
    {
        if (!ref)
            return nullptr;
        uint32_t off = 0;
        if (ref == kInlineRef)
            off = inlineStart;
        else
        {
            RetailWireToken token{};
            RetailPtr32 encoded{};
            encoded.encoded = ref;
            XBlock blocks[9]{};
            for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
                blocks[blockIndex] = session->zoneMemory->blocks[blockIndex];
            if (!RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
                return nullptr;
            off = token.offset;
        }
        if (off >= soundBlock4Size)
            return nullptr;
        uint32_t end = off;
        while (end < soundBlock4Size && soundBlock4[end] != 0)
            ++end;
        if (end >= soundBlock4Size)
            return nullptr;
        return reinterpret_cast<const char *>(soundBlock4 + off);
    };
    auto SoundAliasTargetString = [&](uint32_t aliasRef) -> const char *
    {
        if (!aliasRef || aliasRef == kInlineRef || aliasRef == kInsertRef)
            return nullptr;
        const uint32_t off = (aliasRef - 1u) & 0x0fffffffu;
        if (off >= soundBlock4Size)
            return nullptr;
        const uint32_t inner = ReadLe32(soundBlock4 + off);
        if (inner == kInlineRef)
            return SoundStringAt(kInlineRef,
                                 off + 4u < soundBlock4Size ? off + 4u : soundBlock4Size);
        return SoundStringAt(inner, 0);
    };
    // a sound slot whose declarer is later in the zone order (retail
    // killhouse usp @458 names weap_usp45_fire_npc, declared later in the
    // same zone; common's turret weapons name killhouse-owned sounds) must
    // end up pointing at the real snd_alias_list_t. The original
    // Load_SndAliasCustom looks the name up with DB_FindXAssetHeader at
    // stream time; a miss there manufactures the per-name engine default
    // (`null`) instead -- a placeholder baked into every weapon. Record the
    // (slot, name) pair instead and let the after-zone pass bind the real
    // list as soon as its owner zone has streamed; a name that never arrives
    // stays outstanding and the P6 verifier rejects it (never a default).
    auto BindSound = [&](const char *name, void **dest) -> bool
    {
        if (!name || !name[0])
            return false;
        const XAssetHeader found = DB_FindXAssetHeaderNoDefault(ASSET_TYPE_SOUND, name);
        if (found.sound)
        {
            *dest = found.data;
            return true;
        }
        return RetailDeferWeaponSound(dest, name, session->zoneIndex);
    };
    for (uint32_t op = 1; op < RetailWeaponOpCount(); ++op)
    {
        const RetailWeaponOp &step = kRetailWeaponOps[op];
        uint8_t *native = reinterpret_cast<uint8_t *>(def) + step.nativeBase;
        switch (step.kind)
        {
        case RETAIL_WEAPON_OP_STR:
        case RETAIL_WEAPON_OP_STR_ARRAY:
            for (uint32_t i = 0; i < step.count; ++i)
            {
                const char *value = nullptr;
                if (!StreamWeaponString(&context, ReadLe32(root + step.wireOffset + i * 4u),
                                        &value, "string", true))
                    return context.result;
                // Null slots stay null; inline/alias forms (even "") keep
                // their bytes verbatim -- only the asset name itself must
                // be non-empty (checked once, before the loop).
                *reinterpret_cast<const char **>(native + i * step.nativeStride) = value;
            }
            break;
        case RETAIL_WEAPON_OP_SCRIPT_ARRAY:
            for (uint32_t i = 0; i < step.count; ++i)
            {
                const uint32_t wireIndex = ReadLe16(root + step.wireOffset + i * 2u);
                const char *str = ZoneWeaponScriptString(&context, wireIndex, "script array");
                if (!str)
                {
                    // Table gaps defer (the walk ignores indices); arena
                    // exhaustion stays fatal.
                    if (context.result == RETAIL_WEAPON_DECODE_OUT_OF_ARENA)
                        return context.result;
                    noteDefer(RETAIL_WEAPON_DECODE_SCRIPT_STRING_MISSING, "script array");
                    context.result = RETAIL_WEAPON_DECODE_OK;
                    continue;
                }
                uint16_t id = 0;
                if (!InternWeaponScriptString(&context, str, &id, "script array"))
                    return context.result;
                *reinterpret_cast<uint16_t *>(native + i * step.nativeStride) = id;
            }
            break;
        case RETAIL_WEAPON_OP_XMODEL_ARRAY:
        case RETAIL_WEAPON_OP_XMODEL:
        case RETAIL_WEAPON_OP_FX:
        case RETAIL_WEAPON_OP_MATERIAL:
        {
            const char *kindName = step.kind == RETAIL_WEAPON_OP_XMODEL ||
                                   step.kind == RETAIL_WEAPON_OP_XMODEL_ARRAY ? "xmodel" :
                                   step.kind == RETAIL_WEAPON_OP_FX ? "fx" : "material";
            const XAssetType assetType =
                step.kind == RETAIL_WEAPON_OP_XMODEL ||
                        step.kind == RETAIL_WEAPON_OP_XMODEL_ARRAY
                    ? ASSET_TYPE_XMODEL
                    : step.kind == RETAIL_WEAPON_OP_FX ? ASSET_TYPE_FX : ASSET_TYPE_MATERIAL;
            for (uint32_t i = 0; i < step.count; ++i)
            {
                const uint32_t slot = ReadLe32(root + step.wireOffset + i * 4u);
                if (!slot)
                {
                    *reinterpret_cast<void **>(native + i * step.nativeStride) = nullptr;
                    continue;
                }
                // Inline and insert bodies still need byte-exact
                // accounting for the rest of the zone: consume through
                // the walk helpers (forward-only, exactly as the walk
                // reader would). Alias forms consume nothing in either
                // mode.
                if (slot == kInlineRef || slot == kInsertRef)
                {
                    // B4/G2: an inline/insert Material body (weapon-embedded
                    // reticle/hud/clip materials) widens through the same
                    // world material widener the nested world path uses;
                    // an insert's block-4 slot is recorded so later aliases
                    // resolve through the slot table.
                    if (assetType == ASSET_TYPE_MATERIAL)
                    {
                        Material *material = nullptr;
                        const uint32_t insertSlot =
                            slot == kInsertRef ? ((session->wire.cursor[4] + 3u) & ~3u) : 0u;
                        if (!worldContext ||
                            !RetailWorldWidenMaterial(worldContext, slot, &material, insertSlot))
                        {
                            context.Fail(RETAIL_WEAPON_DECODE_TRUNCATED, "nested material body");
                            return RETAIL_WEAPON_DECODE_TRUNCATED;
                        }
                        *reinterpret_cast<void **>(native + i * step.nativeStride) = material;
                        continue;
                    }
                    // B6: an inline/insert FX body is widened live (identity
                    // + scalar elem graph) instead of merely consumed, and an
                    // insert's 4-byte block-4 slot is recorded so later
                    // aliases resolve to the registered effect.
                    if (assetType == ASSET_TYPE_FX)
                    {
                        const uint32_t insertSlot =
                            slot == kInsertRef ? ((session->wire.cursor[4] + 3u) & ~3u) : 0u;
                        if (slot == kInsertRef &&
                            !RetailWireBlocksAlloc(&session->wire, 4, 4, 4))
                        {
                            context.Fail(RETAIL_WEAPON_DECODE_TRUNCATED, "fx insert slot");
                            return RETAIL_WEAPON_DECODE_TRUNCATED;
                        }
                        RetailWalkDirectoryRecord fxRecord{};
                        uint32_t fxDeferred = 0;
                        XAssetHeader resolvedFx{};
                        if (!RetailWalkLiveLoadFxEffectDef(session, reader, worldContext,
                                                           &resolvedFx, &fxRecord, &fxDeferred,
                                                           insertSlot))
                        {
                            context.Fail(RETAIL_WEAPON_DECODE_TRUNCATED, "nested fx body");
                            return RETAIL_WEAPON_DECODE_TRUNCATED;
                        }
                        record->nestedBodyBytes +=
                            fxRecord.bodyBytes + fxRecord.nestedBodyBytes;
                        *reinterpret_cast<void **>(native + i * step.nativeStride) =
                            resolvedFx.data;
                        continue;
                    }
                    // B4: an inline/insert XModel body widens live through
                    // the shared zone XModel widener. Beyond binding the
                    // weapon's own model pointer, this records the model's
                    // materialHandles slots in the zone nested table -- the
                    // slot a later weapon material alias points at (retail
                    // fraggrenade: weapon -> nested XModel materialHandles
                    // slot -> material body). An insert body's block-4 slot
                    // is reserved and recorded inside the widener.
                    if (assetType == ASSET_TYPE_XMODEL)
                    {
                        XModel *model = nullptr;
                        if (!worldContext ||
                            !RetailWorldWidenXModel(worldContext, slot, &model))
                        {
                            context.Fail(RETAIL_WEAPON_DECODE_TRUNCATED,
                                         "nested xmodel body");
                            return RETAIL_WEAPON_DECODE_TRUNCATED;
                        }
                        *reinterpret_cast<void **>(native + i * step.nativeStride) = model;
                        continue;
                    }
                }
                else
                {
                    // Alias reference: resolve through the zone's nested
                    // widened table and the registry exactly like the
                    // world decoder's own nested refs. Dependency order
                    // guarantees the target was widened earlier in this
                    // zone (or an earlier zone); a null target slot binds
                    // null exactly like the original's copied value.
                    XAssetHeader resolved{};
                    if (worldContext &&
                        RetailWorldResolveNestedAlias(worldContext, slot, assetType, &resolved))
                    {
                        *reinterpret_cast<void **>(native + i * step.nativeStride) =
                            resolved.data;
                        continue;
                    }
                }
                std::snprintf(stage, sizeof(stage),
                              "%s slot %u @%u value=0x%08x (alias unresolved)",
                              kindName, i, step.wireOffset + i * 4u, slot);
                noteDefer(RETAIL_WEAPON_DECODE_UNRESOLVED_REFERENCE, stage);
            }
            break;
        }
        case RETAIL_WEAPON_OP_SOUND:
            for (uint32_t i = 0; i < step.count; ++i)
            {
                const uint32_t slot = ReadLe32(root + step.wireOffset + i * 4u);
                void **dest = reinterpret_cast<void **>(native + i * step.nativeStride);
                if (!slot)
                {
                    *dest = nullptr;
                    continue;
                }
                // Inline names still stream for alignment (both modes
                // agree); alias forms consume nothing.
                const char *soundName = nullptr;
                if (slot == kInlineRef)
                {
                    if (!RetailZoneLoadSessionReadStream(session, reader, 4, 4, 4))
                    {
                        context.Fail(RETAIL_WEAPON_DECODE_TRUNCATED, "sound name slot");
                        return RETAIL_WEAPON_DECODE_TRUNCATED;
                    }
                    const uint32_t inner = session->wire.cursor[4] - 4;
                    const uint32_t innerRef = ReadLe32(soundBlock4 + inner);
                    const uint32_t stringStart = session->wire.cursor[4];
                    uint32_t bytes = 0;
                    if (!ReadRetailXString(session, reader, innerRef, &bytes))
                    {
                        context.Fail(RETAIL_WEAPON_DECODE_TRUNCATED, "sound name");
                        return RETAIL_WEAPON_DECODE_TRUNCATED;
                    }
                    soundName = SoundStringAt(innerRef, stringStart);
                }
                else
                {
                    soundName = SoundAliasTargetString(slot);
                }
                if (BindSound(soundName, dest))
                    continue;
                std::snprintf(stage, sizeof(stage),
                              "sound slot %u @%u value=0x%08x name=%s (no live list)",
                              i, step.wireOffset + i * 4u, slot,
                              soundName ? soundName : "(unresolved)");
                noteDefer(RETAIL_WEAPON_DECODE_UNRESOLVED_REFERENCE, stage);
            }
            break;
        case RETAIL_WEAPON_OP_SOUND_ARRAY:
        {
            // Bounce array: one slot holding 29 names when inline (only
            // -1 streams; insert/alias forms consume nothing, exactly
            // like the walk reader).
            const uint32_t slot = ReadLe32(root + step.wireOffset);
            void **nativeArray = reinterpret_cast<void **>(native);
            if (!slot)
            {
                *nativeArray = nullptr;
                break;
            }
            void **bounce = static_cast<void **>(context.Alloc(
                static_cast<std::size_t>(kWBounceCount) * sizeof(void *), alignof(void *),
                "bounce"));
            if (!context.Ok())
                return context.result;
            std::memset(bounce, 0, static_cast<std::size_t>(kWBounceCount) * sizeof(void *));
            *nativeArray = bounce;
            if (slot == kInlineRef)
            {
                // Mirror the walk reader exactly: the inner base is the
                // post-stream cursor minus the array extent (streaming
                // aligns up, so a pre-stream cursor would misread).
                if (!context.Stream(4, kWBounceCount * 4u, 4, "bounce"))
                    return context.result;
                const uint32_t innerBase =
                    session->wire.cursor[4] - kWBounceCount * 4u;
                for (uint32_t i = 0; i < kWBounceCount; ++i)
                {
                    const uint32_t entry = ReadLe32(soundBlock4 + innerBase + i * 4u);
                    const char *soundName = nullptr;
                    if (entry == kInlineRef)
                    {
                        if (!RetailZoneLoadSessionReadStream(session, reader, 4, 4, 4))
                        {
                            context.Fail(RETAIL_WEAPON_DECODE_TRUNCATED, "bounce name slot");
                            return RETAIL_WEAPON_DECODE_TRUNCATED;
                        }
                        const uint32_t inner = session->wire.cursor[4] - 4;
                        const uint32_t innerRef = ReadLe32(soundBlock4 + inner);
                        const uint32_t stringStart = session->wire.cursor[4];
                        uint32_t bytes = 0;
                        if (!ReadRetailXString(session, reader, innerRef, &bytes))
                        {
                            context.Fail(RETAIL_WEAPON_DECODE_TRUNCATED, "bounce name");
                            return RETAIL_WEAPON_DECODE_TRUNCATED;
                        }
                        soundName = SoundStringAt(innerRef, stringStart);
                    }
                    else if (entry)
                    {
                        soundName = SoundAliasTargetString(entry);
                    }
                    if (soundName && soundName[0])
                    {
                        // Inline bounce entries bind only lists already
                        // registered at stream time; a missing entry keeps
                        // its declared null (the fixture/real shapes with
                        // genuinely absent bounce aliases) but never creates
                        // the engine default entry. The 48 single
                        // sound slots above are the strict forward-reference
                        // seam.
                        const XAssetHeader found =
                            DB_FindXAssetHeaderNoDefault(ASSET_TYPE_SOUND, soundName);
                        if (found.sound)
                            bounce[i] = found.data;
                    }
                }
                break;
            }
            // Alias form (smoke/flash grenades in retail common.ff): the root
            // slot names a shared 29-entry pointer array in block 4 that the
            // declarer streamed inline earlier. Resolve each entry from the
            // pristine mirror; a non-null entry that still misses is a loud
            // defer, never a silent null. A null entry stays null.
            uint32_t failedIndex = UINT32_MAX;
            const char *failedName = nullptr;
            if (!RetailWeaponResolveBounceArray(session, slot, bounce, &failedIndex,
                                                &failedName, true))
            {
                std::snprintf(stage, sizeof(stage),
                              "bounce[%u] @%u value=0x%08x name=%s (alias array unresolved)",
                              failedIndex == UINT32_MAX ? 0u : failedIndex,
                              step.wireOffset, slot,
                              failedName ? failedName : "(unresolved)");
                noteDefer(RETAIL_WEAPON_DECODE_UNRESOLVED_REFERENCE, stage);
            }
            break;
        }
        case RETAIL_WEAPON_OP_KNOTS:
        {
            const uint32_t slot = ReadLe32(root + step.wireOffset);
            const uint32_t knotCount = ReadLe32(root + step.countOffset);
            float (*knots)[2] = nullptr;
            if (slot && knotCount)
            {
                if (knotCount > 0x10000u)
                {
                    context.Fail(RETAIL_WEAPON_DECODE_COUNT_MISMATCH, "knots");
                    return RETAIL_WEAPON_DECODE_COUNT_MISMATCH;
                }
                const size_t bytes = static_cast<size_t>(knotCount) * 8u;
                float *raw = static_cast<float *>(context.Alloc(bytes, 4, "knots"));
                if (!context.Ok())
                    return context.result;
                if (slot == kInlineRef)
                {
                    if (!context.Stream(4, static_cast<uint32_t>(bytes), 4, "knots"))
                        return context.result;
                }
                // A shared accuracy-graph knot array is addressed by its
                // block-4 data offset directly (verified live: the target
                // holds the float pairs, not a pointer slot), so the same
                // arena copy is the exact shared array.
                uint32_t dataOffset = 0;
                if (slot == kInlineRef)
                    dataOffset = session->wire.cursor[4] - static_cast<uint32_t>(bytes);
                else
                    dataOffset = (slot - 1u) & 0x0fffffffu;
                if (static_cast<uint64_t>(dataOffset) + bytes >
                    session->zoneMemory->blocks[4].size)
                {
                    context.Fail(RETAIL_WEAPON_DECODE_TRUNCATED, "knots span");
                    return RETAIL_WEAPON_DECODE_TRUNCATED;
                }
                const uint8_t *wire = session->zoneMemory->blocks[4].data + dataOffset;
                for (uint32_t i = 0; i < knotCount * 2u; ++i)
                {
                    float value = 0.0f;
                    std::memcpy(&value, wire + i * 4u, 4);
                    raw[i] = value;
                }
                knots = reinterpret_cast<float (*)[2]>(raw);
            }
            *reinterpret_cast<float (**)[2]>(native) = knots;
            break;
        }
        default:
            context.Fail(RETAIL_WEAPON_DECODE_BAD_ARGUMENT, "op");
            return RETAIL_WEAPON_DECODE_BAD_ARGUMENT;
        }
    }

    // Deferred bodies are fully accounted but never committed: consumers
    // of the missing asset still fail loudly by name.
    if (deferCode != RETAIL_WEAPON_DECODE_OK)
        return deferCode;

    // Atomic commit through the existing real database owner. The pooled
    // header is required: callers must use it, never the transaction.
    XAssetHeader tx{};
    tx.weapon = def;
    Load_WeaponDefAsset(&tx);
    if (!tx.weapon)
    {
        context.Fail(RETAIL_WEAPON_DECODE_REGISTRATION_FAILED, "register");
        return RETAIL_WEAPON_DECODE_REGISTRATION_FAILED;
    }
    RETAIL_ASSET_TRACE(0, "RetailWalkLiveLoadWeaponDef: '%s' ok\n", assetName);
    *header = tx;
    return RETAIL_WEAPON_DECODE_OK;
    };
    const RetailWeaponDecodeResult code = decodeBody();
    if (code != RETAIL_WEAPON_DECODE_OK)
    {
        // Unwind interned script strings. Cursors stay: accounting and
        // widening shared one forward pass, so a deferred body is already
        // fully accounted and a fatal one aborts the zone.
        for (uint32_t i = 0; i < context.ownedCount; ++i)
            SL_RemoveRefToString(context.ownedSL[i]);
        context.ownedCount = 0;
    }
    return code;
}
