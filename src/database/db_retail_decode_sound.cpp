#include "db_retail_decode_sound.h"

#include "db_retail_decode_world.h"
#include "db_retail_wire.h"
#include "database.h"

#include "../sound/snd_public.h"

#include <utility>
#include <vector>
#include <cstddef>
#include <cstdio>
#include <cstring>

// B2 snd_alias_list_t identity slice. ReadRetailSndAliasListBody (with its
// ReadRetailSndAliasBody per-record child) consumes the body byte-exactly
// and stays the independent oracle; this file widens the root and alias
// records it leaves in the session mirror into native structures and
// registers through the engine's existing Load_snd_alias_list_Asset owner.
namespace
{
constexpr uint32_t kInlineRef = 0xffffffffu;
constexpr uint32_t kAliasBytes = 92;

// ILP32 snd_alias_t offsets (native layout on the 32-bit reference, where
// Load_snd_alias_t's 92-byte record is read field by field).
constexpr uint32_t kWireAliasSequence = 20;
constexpr uint32_t kWireAliasEnvelopMin = 76;
constexpr uint32_t kWireAliasSoundFile = 16;
constexpr uint32_t kWireAliasVolumeCurve = 72;
constexpr uint32_t kWireAliasSpeakerMap = 88;
constexpr uint32_t kWireAliasNameSlots = 4;

// The scalar run sequence..startDelay (13 dwords) and the three envelope
// floats are same-size POD on both ABIs; the offsetof-derived native
// destinations below are the LP64 side of that mapping.
static_assert(offsetof(snd_alias_t, sequence) == 40, "snd_alias_t layout drift");
static_assert(offsetof(snd_alias_t, envelopMin) == 104, "snd_alias_t layout drift");
static_assert(offsetof(snd_alias_t, startDelay) == 88, "snd_alias_t layout drift");
static_assert(offsetof(snd_alias_t, soundFile) == 32, "snd_alias_t layout drift");

uint32_t ReadLe32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t InlineNameBytes(const uint8_t *block4, uint32_t block4Size, uint32_t offset)
{
    uint32_t end = offset;
    while (end < block4Size && block4[end] != 0)
        ++end;
    if (end >= block4Size)
        return 0;
    return end - offset + 1u;
}

// Copy one NUL-terminated wire string into arena storage; returns the copy
// or null (out->null) for a null/unresolvable reference.
char *CopyArenaString(RetailZoneLoadSession *session, const uint8_t *block4,
                      uint32_t block4Size, uint32_t offset, uint32_t *bytesOut)
{
    if (bytesOut)
        *bytesOut = 0;
    if (offset == UINT32_MAX || offset >= block4Size)
        return nullptr;
    const uint32_t bytes = InlineNameBytes(block4, block4Size, offset);
    if (!bytes)
        return nullptr;
    char *copy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, bytes, 1));
    if (!copy)
        return nullptr;
    std::memcpy(copy, block4 + offset, bytes);
    if (bytesOut)
        *bytesOut = bytes;
    return copy;
}

// Resolve the alias-list's own name reference the same way the walk reader
// does (inline bytes at the captured block-4 start, or an alias offset).
bool ResolveListName(RetailZoneLoadSession *session, uint32_t nameRef,
                     uint32_t inlineStart, const char **out, uint32_t *nameBytesOut)
{
    *out = nullptr;
    *nameBytesOut = 0;
    const uint8_t *block4 = session->zoneMemory->blocks[4].data;
    const uint32_t block4Size = session->zoneMemory->blocks[4].size;
    if (nameRef == kInlineRef)
    {
        const uint32_t bytes = InlineNameBytes(block4, block4Size, inlineStart);
        if (!bytes)
            return false;
        *out = reinterpret_cast<const char *>(block4 + inlineStart);
        *nameBytesOut = bytes;
        return true;
    }
    RetailWireToken token{};
    RetailPtr32 encoded{};
    encoded.encoded = nameRef;
    XBlock blocks[9]{};
    for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
        blocks[blockIndex] = session->zoneMemory->blocks[blockIndex];
    if (!RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4 ||
        token.offset >= block4Size)
        return false;
    const uint32_t bytes = InlineNameBytes(block4, block4Size, token.offset);
    if (!bytes)
        return false;
    *out = reinterpret_cast<const char *>(block4 + token.offset);
    *nameBytesOut = bytes;
    return true;
}

// Alias-form SoundFile/SpeakerMap resolution (Load_snd_alias_t's
// DB_ConvertOffsetToPointer branch): the encoded token addresses the
// already-streamed struct in block 4, whose widened object the live widener
// recorded under that exact offset. Returns null on any other shape, which
// the caller turns into a loud zone failure.
void *ResolveSoundBodyAlias(RetailZoneLoadSession *session,
                            RetailWorldLoadContext *worldContext, uint32_t ref,
                            uint8_t kind)
{
    if (!session || !worldContext || !ref)
        return nullptr;
    XBlock blocks[9]{};
    for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
        blocks[blockIndex] = session->zoneMemory->blocks[blockIndex];
    RetailWireToken token{};
    RetailPtr32 encoded{};
    encoded.encoded = ref;
    if (!RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != 4)
        return nullptr;
    return RetailWorldFindSoundBody(worldContext, token.offset, kind);
}

float ReadMirrorFloat(const uint8_t *p)
{
    float value = 0.0f;
    std::memcpy(&value, p, 4);
    return value;
}
} // namespace

// B2 nested graph widening: turn one alias's captured wire offsets (all
// bodies already streamed into the session mirror by the walk reader) into
// native SoundFile/SndCurve/SpeakerMap/LoadedSound objects. Null/alias slots
// leave the corresponding native pointer null. No playback or streaming is
// claimed here; this is identity/ownership of the decoded graph only.
bool RetailSoundWidenAliasGraphs(RetailZoneLoadSession *session,
                                 const RetailWalkSndAliasOffsets &offsets,
                                 snd_alias_t *alias,
                                 bool registerNestedOwners,
                                 uint32_t *registeredLoadedSoundsOut,
                                 uint32_t *registeredSndCurvesOut)
{
    if (!session || !session->active || !session->zoneMemory || !alias)
        return false;
    if (registeredLoadedSoundsOut)
        *registeredLoadedSoundsOut = 0;
    if (registeredSndCurvesOut)
        *registeredSndCurvesOut = 0;
    const uint8_t *block0 = session->zoneMemory->blocks[0].data;
    const uint32_t block0Size = session->zoneMemory->blocks[0].size;
    const uint8_t *block4 = session->zoneMemory->blocks[4].data;
    const uint32_t block4Size = session->zoneMemory->blocks[4].size;
    XBlock blocks[9]{};
    for (uint32_t blockIndex = 0; blockIndex < 9; ++blockIndex)
        blocks[blockIndex] = session->zoneMemory->blocks[blockIndex];

    auto mirrorString = [&](uint32_t ref, uint32_t inlineStart) -> char *
    {
        if (!ref)
            return nullptr;
        uint32_t blockIndex = 4;
        uint32_t off = 0;
        if (ref == kInlineRef)
        {
            off = inlineStart;
        }
        else
        {
            RetailWireToken token{};
            RetailPtr32 encoded{};
            encoded.encoded = ref;
            if (!RetailWireTokenDecodeBlocks(blocks, encoded, 0, 1u << 4, &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET)
                return nullptr;
            blockIndex = token.block;
            off = token.offset;
        }
        if (blockIndex >= 9)
            return nullptr;
        const uint8_t *data = session->zoneMemory->blocks[blockIndex].data;
        const uint32_t size = session->zoneMemory->blocks[blockIndex].size;
        if (!data || off >= size)
            return nullptr;
        uint32_t end = off;
        while (end < size && data[end] != 0)
            ++end;
        if (end >= size)
            return nullptr;
        const uint32_t len = end - off + 1u;
        char *copy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, len, 1));
        if (!copy)
            return nullptr;
        std::memcpy(copy, data + off, len);
        return copy;
    };

    if (offsets.soundFile != UINT32_MAX)
    {
        if (static_cast<uint64_t>(offsets.soundFile) + 12u > block4Size)
        {
            Com_Printf(0, "RetailSoundWidenAliasGraphs: soundFile span OOB off=%u\n",
                       offsets.soundFile);
            return false;
        }
        SoundFile *file = static_cast<SoundFile *>(
            RetailZoneLoadSessionAlloc(session, sizeof(SoundFile), alignof(SoundFile)));
        if (!file)
            return false;
        std::memset(file, 0, sizeof(*file));
        file->type = block4[offsets.soundFile];
        // Retail keeps the linker's `exists` byte (every streamed entry in the
        // shipped zones carries 1), rather than asserting it.
        file->exists = block4[offsets.soundFile + 1];
        if (file->type == 1)
        {
            // A null/alias embedded slot streams no LoadedSound; the native
            // SoundFile keeps its null pointer (a real engine state).
            if (offsets.loadedSoundRootBytes ||
                offsets.loadedSoundRoot != UINT32_MAX)
            {
                const uint8_t *root = offsets.loadedSoundRootBytes;
                if (!root)
                {
                    if (static_cast<uint64_t>(offsets.loadedSoundRoot) + 44u > block0Size)
                    {
                        Com_Printf(0, "RetailSoundWidenAliasGraphs: loadedSound root OOB off=%u\n",
                                   offsets.loadedSoundRoot);
                        return false;
                    }
                    root = block0 + offsets.loadedSoundRoot;
                }
                LoadedSound *loaded = static_cast<LoadedSound *>(RetailZoneLoadSessionAlloc(
                    session, sizeof(LoadedSound), alignof(LoadedSound)));
                if (!loaded)
                    return false;
                std::memset(loaded, 0, sizeof(*loaded));
                if (offsets.loadedSoundName != UINT32_MAX)
                    loaded->name = mirrorString(kInlineRef, offsets.loadedSoundName);
                loaded->sound.info.format = static_cast<int>(ReadLe32(root + 4));
                loaded->sound.info.data_len = ReadLe32(root + 12);
                loaded->sound.info.rate = ReadLe32(root + 16);
                loaded->sound.info.bits = static_cast<int>(ReadLe32(root + 20));
                loaded->sound.info.channels = static_cast<int>(ReadLe32(root + 24));
                loaded->sound.info.samples = ReadLe32(root + 28);
                loaded->sound.info.block_size = ReadLe32(root + 32);
                // The embedded audio payload stays in the zone's wire block:
                // copying every embedded sound into the native arena
                // exhausted it on a real multi-zone load. This widens identity/ownership only, so the LoadedSound
                // keeps its real info (format/rate/bits/channels/samples/
                // data_len) and a null data pointer; playback/streaming owns
                // the payload later. `loadedSoundData` is retained for that
                // later consumer.
                loaded->sound.data = nullptr;
                loaded->sound.info.data_ptr = nullptr;
                // the walk retained the payload in sound memory (PCM16,
                // ADPCM already decoded) when a sound backend is linked.
                if (offsets.loadedSoundRetained)
                {
                    loaded->sound.data = offsets.loadedSoundRetained;
                    loaded->sound.info.data_ptr = offsets.loadedSoundRetained;
                    loaded->sound.info.initial_ptr = offsets.loadedSoundRetained;
                    loaded->sound.info.data_len = offsets.loadedSoundRetainedLen;
                    loaded->sound.info.format = static_cast<int>(offsets.loadedSoundRetainedFormat);
                    loaded->sound.info.bits = static_cast<int>(offsets.loadedSoundRetainedBits);
                }
                file->u.loadSnd = loaded;
                if (registerNestedOwners)
                {
                    // Original Load_LoadedSoundAsset: DB_AddXAsset owns the
                    // object and the pointer slot is replaced by whatever it
                    // returned (first canonical entry on a same-name
                    // override). Registration failure is zone-fatal, never a
                    // silently unregistered alias pointer.
                    XAssetHeader registered{};
                    registered.loadSnd = loaded;
                    registered = RetailZoneLoadSessionRegister(
                        session, ASSET_TYPE_LOADED_SOUND, registered);
                    if (!registered.loadSnd)
                    {
                        Com_Printf(0, "RetailSoundWidenAliasGraphs: LoadedSound registration "
                                      "failed name='%s'\n",
                                   loaded->name ? loaded->name : "(null)");
                        return false;
                    }
                    file->u.loadSnd = registered.loadSnd;
                    if (registeredLoadedSoundsOut)
                        ++*registeredLoadedSoundsOut;
                }
            }
        }
        else
        {
            file->u.streamSnd.filename.info.raw.dir =
                mirrorString(ReadLe32(block4 + offsets.soundFile + 4u), offsets.soundFileDir);
            file->u.streamSnd.filename.info.raw.name =
                mirrorString(ReadLe32(block4 + offsets.soundFile + 8u), offsets.soundFileName);
        }
        alias->soundFile = file;
    }

    if (offsets.curveRootBytes || offsets.curveRoot != UINT32_MAX)
    {
        const uint8_t *root = offsets.curveRootBytes;
        if (!root)
        {
            if (static_cast<uint64_t>(offsets.curveRoot) + 72u > block0Size)
            {
                Com_Printf(0, "RetailSoundWidenAliasGraphs: curve root OOB off=%u\n",
                           offsets.curveRoot);
                return false;
            }
            root = block0 + offsets.curveRoot;
        }
        SndCurve *curve = static_cast<SndCurve *>(
            RetailZoneLoadSessionAlloc(session, sizeof(SndCurve), alignof(SndCurve)));
        if (!curve)
            return false;
        std::memset(curve, 0, sizeof(*curve));
        curve->filename = mirrorString(ReadLe32(root), offsets.curveName);
        curve->knotCount = static_cast<int>(ReadLe32(root + 4));
        for (uint32_t knot = 0; knot < 8; ++knot)
            for (uint32_t axis = 0; axis < 2; ++axis)
                curve->knots[knot][axis] =
                    ReadMirrorFloat(root + 8u + (knot * 2u + axis) * 4u);
        alias->volumeFalloffCurve = curve;
        if (registerNestedOwners)
        {
            // Original Load_SndCurveAsset: same registry ownership and
            // pointer-slot replacement as the LoadedSound form above.
            XAssetHeader registered{};
            registered.sndCurve = curve;
            registered = RetailZoneLoadSessionRegister(
                session, ASSET_TYPE_SOUND_CURVE, registered);
            if (!registered.sndCurve)
            {
                Com_Printf(0, "RetailSoundWidenAliasGraphs: SndCurve registration "
                              "failed name='%s'\n",
                           curve->filename ? curve->filename : "(null)");
                return false;
            }
            alias->volumeFalloffCurve = registered.sndCurve;
            if (registeredSndCurvesOut)
                ++*registeredSndCurvesOut;
        }
    }

    if (offsets.speakerMap != UINT32_MAX)
    {
        if (static_cast<uint64_t>(offsets.speakerMap) + 408u > block4Size)
        {
            Com_Printf(0, "RetailSoundWidenAliasGraphs: speakerMap span OOB off=%u\n",
                       offsets.speakerMap);
            return false;
        }
        const uint8_t *root = block4 + offsets.speakerMap;
        SpeakerMap *map = static_cast<SpeakerMap *>(
            RetailZoneLoadSessionAlloc(session, sizeof(SpeakerMap), alignof(SpeakerMap)));
        if (!map)
            return false;
        std::memset(map, 0, sizeof(*map));
        map->isDefault = root[0] != 0;
        map->name = mirrorString(ReadLe32(root + 4), offsets.speakerMapName);
        std::memcpy(map->channelMaps, root + 8, sizeof(map->channelMaps));
        alias->speakerMap = map;
    }
    return true;
}

bool RetailWalkLiveLoadSndCurve(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader, XAssetHeader *out)
{
    if (!session || !reader || !out || !session->active)
        return false;
    *out = XAssetHeader{};
    const uint32_t savedCursor0 = session->wire.cursor[0];
    RetailWalkDirectoryRecord consumed{};
    consumed.header = kInlineRef;
    RetailWalkDirectoryResult scratch{};
    uint32_t rootStart = 0;
    uint32_t nameStart = UINT32_MAX;
    if (!ReadRetailSndCurveBody(session, reader, &consumed, &scratch, &rootStart, &nameStart))
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadSndCurve: body consume failed\n");
        return false;
    }
    RetailWalkSndAliasOffsets offsets{};
    std::memset(&offsets, 0xFF, sizeof(offsets));
    offsets.loadedSoundRootBytes = nullptr;
    offsets.curveRootBytes = nullptr;
    offsets.loadedSoundRetained = nullptr;
    offsets.curveRoot = rootStart;
    offsets.curveName = nameStart;
    snd_alias_t scratchAlias{};
    // The directory header's own registration below is the original
    // Load_SndCurvePtr owner for this asset; the widener must not register
    // it a second time.
    if (!RetailSoundWidenAliasGraphs(session, offsets, &scratchAlias, false) ||
        !scratchAlias.volumeFalloffCurve)
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadSndCurve: widen failed\n");
        return false;
    }
    XAssetHeader tx{};
    tx.sndCurve = scratchAlias.volumeFalloffCurve;
    Load_SndCurveAsset(&tx);
    if (!tx.sndCurve)
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadSndCurve: registration failed for '%s'\n",
                   tx.sndCurve && tx.sndCurve->filename ? tx.sndCurve->filename : "(null)");
        return false;
    }
    *out = tx;
    RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
    return true;
}

bool RetailWalkLiveLoadSndAliasList(RetailZoneLoadSession *session,
                                    FsRetailFastfileReader *reader,
                                    RetailWorldLoadContext *worldContext,
                                    XAssetHeader *out,
                                    RetailWalkDirectoryRecord *record,
                                    uint32_t *deferredNestedOut,
                                    uint32_t slotOffset,
                                    uint32_t *registeredLoadedSoundsOut,
                                    uint32_t *registeredSndCurvesOut,
                                    RetailSoundDeferredBreakdown *deferredBreakdownOut)
{
    if (!session || !reader || !out || !session->active)
        return false;
    *out = XAssetHeader{};
    if (deferredNestedOut)
        *deferredNestedOut = 0;
    if (registeredLoadedSoundsOut)
        *registeredLoadedSoundsOut = 0;
    if (registeredSndCurvesOut)
        *registeredSndCurvesOut = 0;
    if (deferredBreakdownOut)
        *deferredBreakdownOut = RetailSoundDeferredBreakdown{};

    const uint32_t savedCursor0 = session->wire.cursor[0];
    const uint32_t block4Start = session->wire.cursor[4];

    RetailWalkDirectoryRecord consumed{};
    consumed.header = kInlineRef;
    RetailWalkDirectoryResult scratch{};
    uint32_t *nameOffsets = nullptr;
    RetailWalkSndAliasOffsets *aliasOffsets = nullptr;
    if (!ReadRetailSndAliasListBody(session, reader, &consumed, &scratch, &nameOffsets,
                                    &aliasOffsets))
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadSndAliasList: body consume failed\n");
        return false;
    }

    const uint32_t bodyStart = (savedCursor0 + 3u) & ~3u;
    const uint8_t *root = session->zoneMemory->blocks[0].data + bodyStart;
    const uint8_t *block4 = session->zoneMemory->blocks[4].data;
    const uint32_t block4Size = session->zoneMemory->blocks[4].size;
    const uint32_t nameRef = ReadLe32(root);
    const uint32_t headRef = ReadLe32(root + 4);
    const uint32_t aliasCount = ReadLe32(root + 8);

    const char *name = nullptr;
    uint32_t nameBytes = 0;
    if (!ResolveListName(session, nameRef, block4Start, &name, &nameBytes) || !name ||
        !name[0])
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadSndAliasList: unresolvable name ref=0x%08x\n", nameRef);
        return false;
    }

    uint32_t deferredNested = 0;
    snd_alias_t *aliases = nullptr;
    if (aliasCount)
    {
        if (headRef != kInlineRef || aliasCount > 0x10000u)
        {
            RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
            Com_Printf(0, "RetailWalkLiveLoadSndAliasList: '%s' head form unsupported "
                          "count=%u ref=0x%08x\n",
                       name, aliasCount, headRef);
            return false;
        }
        // The walk reader streamed the alias record array after the list
        // name, aligned to 4 (RetailZoneLoadSessionReadStream's alignment).
        const uint32_t listNameBytes = nameRef == kInlineRef ? nameBytes : 0u;
        const uint32_t headStart = (block4Start + listNameBytes + 3u) & ~3u;
        if (static_cast<uint64_t>(headStart) +
                static_cast<uint64_t>(aliasCount) * kAliasBytes >
            block4Size)
        {
            RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
            Com_Printf(0, "RetailWalkLiveLoadSndAliasList: '%s' alias span OOB\n", name);
            return false;
        }
        aliases = static_cast<snd_alias_t *>(RetailZoneLoadSessionAlloc(
            session, static_cast<std::size_t>(aliasCount) * sizeof(snd_alias_t),
            alignof(snd_alias_t)));
        if (!aliases)
        {
            RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
            return false;
        }
        std::memset(aliases, 0, static_cast<std::size_t>(aliasCount) * sizeof(snd_alias_t));
        // A name the wire references by offset is the same zone memory as the
        // inline copy it points back to, so retail hands every variant of an
        // alias one shared aliasName pointer.  The engine relies on that
        // identity: SND_ContinueLoopingSound matches a playing loop by
        // `aliasName ==`, and CL_PickSoundAlias picks a random variant each
        // frame, so per-variant copies restarted multi-variant loops (e.g.
        // emt_ac_metal_rattle, re-opening and decoding a WAV) every frame.
        // Copy each block-4 offset once per list and reuse the pointer.
        std::vector<std::pair<uint32_t, char *>> copiedNames;
        for (uint32_t alias = 0; alias < aliasCount; ++alias)
        {
            const uint8_t *wire = block4 + headStart + static_cast<std::size_t>(alias) * kAliasBytes;
            snd_alias_t *native = &aliases[alias];
            const uint32_t *offsets = nameOffsets ? nameOffsets + alias * kWireAliasNameSlots : nullptr;
            const char *wireNames[kWireAliasNameSlots]{};
            char **nativeNames[kWireAliasNameSlots] = {
                const_cast<char **>(&native->aliasName),
                const_cast<char **>(&native->subtitle),
                const_cast<char **>(&native->secondaryAliasName),
                const_cast<char **>(&native->chainAliasName),
            };
            for (uint32_t slot = 0; slot < kWireAliasNameSlots; ++slot)
            {
                if (offsets && offsets[slot] != UINT32_MAX)
                {
                    char *shared = nullptr;
                    for (const auto &copied : copiedNames)
                    {
                        if (copied.first == offsets[slot])
                        {
                            shared = copied.second;
                            break;
                        }
                    }
                    if (!shared)
                    {
                        shared = CopyArenaString(session, block4, block4Size, offsets[slot], nullptr);
                        if (shared)
                            copiedNames.emplace_back(offsets[slot], shared);
                    }
                    *nativeNames[slot] = shared;
                }
                else if (ReadLe32(wire + slot * 4u) != 0)
                {
                    ++deferredNested;
                    if (deferredBreakdownOut)
                    {
                        // Classify the deferred ref by what its encoded token
                        // actually addresses: the original DB_ConvertOffsetToPointer
                        // resolves every block, not only block 4.
                        const uint32_t ref = ReadLe32(wire + slot * 4u);
                        RetailWireToken token{};
                        RetailPtr32 encoded{};
                        encoded.encoded = ref;
                        uint32_t poolIndex = 0;
                        if (RetailWireTokenDecodeBlocks(session->zoneMemory->blocks, encoded, 0,
                                                        (1u << 9) - 1u, &token) &&
                            token.kind == RETAIL_WIRE_TOKEN_OFFSET)
                        {
                            if (token.block == 0)
                                ++deferredBreakdownOut->nameBlock0;
                            else if (token.block == 4)
                                ++deferredBreakdownOut->nameUnclassified; // offset lost by reader
                            else
                                ++deferredBreakdownOut->nameOtherBlock;
                        }
                        else if (RetailWirePoolIndexDecode(encoded, UINT32_MAX, &poolIndex))
                            ++deferredBreakdownOut->namePool;
                        else
                            ++deferredBreakdownOut->nameUnclassified;
                    }
                }
            }
            (void)wireNames;
            std::memcpy(reinterpret_cast<uint8_t *>(native) + offsetof(snd_alias_t, sequence),
                        wire + kWireAliasSequence, 52);
            std::memcpy(reinterpret_cast<uint8_t *>(native) + offsetof(snd_alias_t, envelopMin),
                        wire + kWireAliasEnvelopMin, 12);
            if (aliasOffsets)
            {
                // The walk reader already streamed every nested body this
                // alias owns; widen the native graph from its captured
                // offsets and register each inline/insert LoadedSound/
                // SndCurve through the original owners (Load_LoadedSound*
                // / Load_SndCurve*). A widen or registration failure is a
                // decoder defect (the walk accepted the same bytes), so it
                // stays zone-fatal.
                RetailWalkSndAliasOffsets &nested = aliasOffsets[alias];
                uint32_t aliasLoadedSounds = 0;
                uint32_t aliasSndCurves = 0;
                if (!RetailSoundWidenAliasGraphs(session, nested, native,
                                                 true, &aliasLoadedSounds, &aliasSndCurves))
                {
                    RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
                    Com_Printf(0, "RetailWalkLiveLoadSndAliasList: '%s' alias %u graph widen failed\n",
                               name, alias);
                    return false;
                }
                if (registeredLoadedSoundsOut)
                    *registeredLoadedSoundsOut += aliasLoadedSounds;
                if (registeredSndCurvesOut)
                    *registeredSndCurvesOut += aliasSndCurves;

                auto failAlias = [&](const char *what, uint32_t ref) -> bool
                {
                    RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
                    Com_Printf(0, "RetailWalkLiveLoadSndAliasList: '%s' alias %u %s ref=0x%08x "
                                  "unresolved\n",
                               name, alias, what, ref);
                    return false;
                };

                // Alias-form sibling references. Load_snd_alias_t binds these
                // through DB_ConvertOffsetToPointer (soundFile/speakerMap,
                // block-4 resident structs) or DB_ConvertOffsetToAlias
                // (curve/LoadedSound, whose temp-block roots the loader
                // reaches through the pointer field that owns them): the
                // pointer of the object an earlier load already wrote. The
                // ledger lookup is identity, never a second object, a
                // re-parse, or a tolerated null.
                const uint32_t rawSoundFile = ReadLe32(wire + kWireAliasSoundFile);
                const uint32_t rawCurve = ReadLe32(wire + kWireAliasVolumeCurve);
                const uint32_t rawSpeakerMap = ReadLe32(wire + kWireAliasSpeakerMap);
                if (rawSoundFile && !native->soundFile && rawSoundFile != kInlineRef)
                {
                    native->soundFile = static_cast<SoundFile *>(ResolveSoundBodyAlias(
                        session, worldContext, rawSoundFile,
                        RetailWorldLoadContext::RETAIL_SOUND_BODY_FILE));
                    if (!native->soundFile)
                        return failAlias("soundFile alias", rawSoundFile);
                }
                if (rawSpeakerMap && !native->speakerMap && rawSpeakerMap != kInlineRef)
                {
                    native->speakerMap = static_cast<SpeakerMap *>(ResolveSoundBodyAlias(
                        session, worldContext, rawSpeakerMap,
                        RetailWorldLoadContext::RETAIL_SOUND_BODY_SPEAKER_MAP));
                    if (!native->speakerMap)
                        return failAlias("speakerMap alias", rawSpeakerMap);
                }
                if (rawCurve && !native->volumeFalloffCurve && rawCurve != kInlineRef)
                {
                    XAssetHeader found{};
                    if (!worldContext ||
                        !RetailWorldResolveNestedAlias(worldContext, rawCurve,
                                                       ASSET_TYPE_SOUND_CURVE, &found) ||
                        !found.sndCurve)
                        return failAlias("curve alias", rawCurve);
                    native->volumeFalloffCurve = found.sndCurve;
                }
                // Embedded LoadedSound alias inside an inline SoundFile
                // (type 1): Load_LoadedSoundPtr's alias branch copies the
                // pointer an earlier load wrote into the owning field/slot.
                if (nested.soundFile != UINT32_MAX && native->soundFile &&
                    native->soundFile->type == 1 && !native->soundFile->u.loadSnd)
                {
                    const uint32_t rawLoaded = ReadLe32(block4 + nested.soundFile + 4u);
                    if (rawLoaded && rawLoaded != kInlineRef)
                    {
                        XAssetHeader found{};
                        if (!worldContext ||
                            !RetailWorldResolveNestedAlias(worldContext, rawLoaded,
                                                           ASSET_TYPE_LOADED_SOUND, &found) ||
                            !found.loadSnd)
                            return failAlias("embedded LoadedSound alias", rawLoaded);
                        native->soundFile->u.loadSnd = found.loadSnd;
                    }
                }

                // Record every address the original loader leaves holding
                // this alias's bound pointers, so a later alias-form sibling
                // (or a chain through this alias's own field) resolves to the
                // same object:
                //   - block-4 struct addresses: the struct itself
                //     (DB_ConvertOffsetToPointer);
                //   - block-0/registered roots: the owning 4-byte pointer
                //     field and any DB_InsertPointer slot the loader patched
                //     (DB_ConvertOffsetToAlias).
                if (worldContext)
                {
                    const uint32_t wireOffset = static_cast<uint32_t>(wire - block4);
                    if (native->soundFile &&
                        (!RetailWorldRecordSoundBody(
                             worldContext, wireOffset + kWireAliasSoundFile,
                             RetailWorldLoadContext::RETAIL_SOUND_BODY_FILE,
                             native->soundFile) ||
                         (nested.soundFile != UINT32_MAX &&
                          !RetailWorldRecordSoundBody(
                              worldContext, nested.soundFile,
                              RetailWorldLoadContext::RETAIL_SOUND_BODY_FILE,
                              native->soundFile))))
                        return failAlias("soundFile ledger", wireOffset + kWireAliasSoundFile);
                    if (native->speakerMap &&
                        (!RetailWorldRecordSoundBody(
                             worldContext, wireOffset + kWireAliasSpeakerMap,
                             RetailWorldLoadContext::RETAIL_SOUND_BODY_SPEAKER_MAP,
                             native->speakerMap) ||
                         (nested.speakerMap != UINT32_MAX &&
                          !RetailWorldRecordSoundBody(
                              worldContext, nested.speakerMap,
                              RetailWorldLoadContext::RETAIL_SOUND_BODY_SPEAKER_MAP,
                              native->speakerMap))))
                        return failAlias("speakerMap ledger", wireOffset + kWireAliasSpeakerMap);
                    if (native->volumeFalloffCurve)
                    {
                        XAssetHeader slot{};
                        slot.sndCurve = native->volumeFalloffCurve;
                        if (!RetailWorldRecordZoneSlot(worldContext,
                                                       wireOffset + kWireAliasVolumeCurve,
                                                       ASSET_TYPE_SOUND_CURVE, slot))
                            return failAlias("curve field", wireOffset + kWireAliasVolumeCurve);
                        if (nested.curveInsertSlot != UINT32_MAX &&
                            !RetailWorldRecordZoneSlot(worldContext, nested.curveInsertSlot,
                                                       ASSET_TYPE_SOUND_CURVE, slot))
                            return failAlias("curve insert slot", nested.curveInsertSlot);
                    }
                    if (native->soundFile && native->soundFile->u.loadSnd)
                    {
                        XAssetHeader slot{};
                        slot.loadSnd = native->soundFile->u.loadSnd;
                        if (nested.soundFile != UINT32_MAX &&
                            !RetailWorldRecordZoneSlot(worldContext, nested.soundFile + 4u,
                                                       ASSET_TYPE_LOADED_SOUND, slot))
                            return failAlias("LoadedSound field", nested.soundFile + 4u);
                        if (nested.loadedSoundInsertSlot != UINT32_MAX &&
                            !RetailWorldRecordZoneSlot(worldContext,
                                                       nested.loadedSoundInsertSlot,
                                                       ASSET_TYPE_LOADED_SOUND, slot))
                            return failAlias("LoadedSound insert slot",
                                             nested.loadedSoundInsertSlot);
                    }
                }

                // Anything still unbound after the original resolution chain
                // is a real deferral; keep it counted and classified so the
                // zero stays honest instead of silently disappearing.
                if (rawSoundFile && !native->soundFile)
                {
                    ++deferredNested;
                    if (deferredBreakdownOut)
                        ++deferredBreakdownOut->soundFileAlias;
                }
                if (rawCurve && !native->volumeFalloffCurve)
                {
                    ++deferredNested;
                    if (deferredBreakdownOut)
                        ++deferredBreakdownOut->curveAlias;
                }
                if (rawSpeakerMap && !native->speakerMap)
                {
                    ++deferredNested;
                    if (deferredBreakdownOut)
                        ++deferredBreakdownOut->speakerMapAlias;
                }
            }
            else
            {
                const uint32_t rawSoundFile = ReadLe32(wire + kWireAliasSoundFile);
                const uint32_t rawCurve = ReadLe32(wire + kWireAliasVolumeCurve);
                const uint32_t rawSpeakerMap = ReadLe32(wire + kWireAliasSpeakerMap);
                deferredNested += rawSoundFile != 0 ? 1u : 0u;
                deferredNested += rawCurve != 0 ? 1u : 0u;
                deferredNested += rawSpeakerMap != 0 ? 1u : 0u;
                if (deferredBreakdownOut)
                {
                    if (rawSoundFile)
                        ++deferredBreakdownOut->soundFileAlias;
                    if (rawCurve)
                        ++deferredBreakdownOut->curveAlias;
                    if (rawSpeakerMap)
                        ++deferredBreakdownOut->speakerMapAlias;
                }
            }
        }
    }
    else if (headRef && headRef != kInlineRef)
    {
        deferredNested += 1u;
        if (deferredBreakdownOut)
            ++deferredBreakdownOut->headAlias;
    }

    snd_alias_list_t *list = static_cast<snd_alias_list_t *>(
        RetailZoneLoadSessionAlloc(session, sizeof(snd_alias_list_t), alignof(snd_alias_list_t)));
    if (!list)
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        return false;
    }
    std::memset(list, 0, sizeof(*list));
    char *nameCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, nameBytes, 1));
    if (!nameCopy)
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        return false;
    }
    std::memcpy(nameCopy, name, nameBytes);
    list->aliasName = nameCopy;
    list->head = aliases;
    list->count = static_cast<int>(aliasCount);

    XAssetHeader tx{};
    tx.sound = list;
    Load_snd_alias_list_Asset(&tx);
    if (!tx.sound)
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadSndAliasList: '%s' registration failed\n", name);
        return false;
    }
    if (slotOffset && worldContext &&
        !RetailWorldRecordZoneSlot(worldContext, slotOffset, ASSET_TYPE_SOUND, tx))
    {
        RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
        Com_Printf(0, "RetailWalkLiveLoadSndAliasList: '%s' slot ledger full\n", name);
        return false;
    }

    if (record)
    {
        record->bodyBytes = consumed.bodyBytes;
        record->nameBytes = consumed.nameBytes;
        record->nestedBodyBytes = consumed.nestedBodyBytes;
        record->nestedReferenceCount = consumed.nestedReferenceCount;
    }
    if (deferredNestedOut)
        *deferredNestedOut = deferredNested;
    *out = tx;
    RetailWireBlocksRewind(&session->wire, 0, savedCursor0);
    return true;
}
