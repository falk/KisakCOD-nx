#pragma once

#include "../universal/com_files.h"
#include "db_retail_zone.h"
#include "db_retail_walk.h"

struct snd_alias_list_t;
struct snd_alias_t;
struct RetailWorldLoadContext;

// B2 snd_alias_list_t identity slice: widen one inline alias
// list body field by field into a transaction-owned native
// snd_alias_list_t and register it through the existing
// Load_snd_alias_list_Asset database owner.
//
// Wire contract (mirrors db_retail_walk.cpp's ReadRetailSndAliasListBody /
// ReadRetailSndAliasBody, which stay the independent byte oracle): a 12-byte
// root in the temp block (name XString, head ref, count), then the name and
// the inline array of 92-byte ILP32 snd_alias_t records in block 4, with
// each alias's sound file, falloff curve, and speaker map consumed
// byte-exactly by the walk reader exactly as the reference Load_snd_alias_t
// does.
//
// Widened now (real, never synthesized): the list name/count, every alias
// record's four name strings and scalar runs, and the nested graph --
// SoundFile (streamed dir/name or embedded LoadedSound info), SndCurve
// (filename/knots), and SpeakerMap (default flag/name/channel maps) -- from
// the exact mirror offsets the walk reader streamed. Embedded audio payload
// bytes deliberately stay in the zone's wire block (copying them exhausted
// the 100MB native arena on the real four-zone load): the LoadedSound keeps
// its wire info/data_len and a null data pointer, so identity/ownership is
// exact and no playback/streaming is claimed. Null/alias nested slots bind
// null; every still-deferred non-null object is counted so a consumer that
// reaches one fails loudly instead of playing silence.
//
// `worldContext` may be null. `slotOffset` records the widened result in the
// zone-scoped nested table when nonzero so weapon snd_alias_list references
// (inline insert slots or directory header slots) resolve to the registered
// list. `registeredLoadedSoundsOut`/`registeredSndCurvesOut` (both optional)
// receive how many nested LoadedSound/SndCurve owners this list registered
// through the original Load_LoadedSoundAsset/Load_SndCurveAsset chain.
//
// `deferredBreakdownOut` (optional) classifies every still-deferred non-null
// reference so a caller can tell which original side effect is missing
// instead of only counting them.
struct RetailSoundDeferredBreakdown
{
    uint32_t nameUnclassified;   // non-null name ref that is not a decodable token
    uint32_t nameBlock0;         // decodable offset token into the temp block
    uint32_t nameOtherBlock;     // decodable offset token into blocks 1..8 except 4
    uint32_t namePool;           // pseudo-block-15 pool index
    uint32_t soundFileAlias;     // alias-form SoundFile the widener did not bind
    uint32_t curveAlias;         // alias-form SndCurve the widener did not bind
    uint32_t speakerMapAlias;    // alias-form SpeakerMap the widener did not bind
    uint32_t headAlias;          // count==0 with a non-inline head reference
    uint32_t Total() const
    {
        return nameUnclassified + nameBlock0 + nameOtherBlock + namePool +
               soundFileAlias + curveAlias + speakerMapAlias + headAlias;
    }
};

bool RetailWalkLiveLoadSndAliasList(RetailZoneLoadSession *session,
                                    FsRetailFastfileReader *reader,
                                    RetailWorldLoadContext *worldContext,
                                    XAssetHeader *out,
                                    RetailWalkDirectoryRecord *record,
                                    uint32_t *deferredNestedOut,
                                    uint32_t slotOffset,
                                    uint32_t *registeredLoadedSoundsOut = nullptr,
                                    uint32_t *registeredSndCurvesOut = nullptr,
                                    RetailSoundDeferredBreakdown *deferredBreakdownOut = nullptr);

// B2 nested graph widening: given one alias's captured wire offsets (bodies
// already streamed into the session mirror by the walk reader), build the
// native SoundFile (streamed dir/name or embedded LoadedSound with its audio
// bytes copied), SndCurve (filename/knotCount/knots), and SpeakerMap
// (isDefault/name/channel maps) and attach them to `alias`. Null/alias slots
// leave the corresponding pointer null. No playback/streaming is claimed.
//
// `registerNestedOwners` restores the original nested-pointer call chain
// (`Load_SoundFileRef`->`Load_LoadedSoundPtr`->`Load_LoadedSoundAsset` and
// `Load_SndCurvePtr`->`Load_SndCurveAsset`, db_load.cpp:1555/1623): each
// widened inline/insert LoadedSound/SndCurve is registered through the zone
// session owner and the *registered* header (which may be an older canonical
// entry on a same-name override) is what the alias graph binds. Walk-only
// and synthetic-mirror callers pass false. When the out counters are
// non-null they receive how many of each owner were registered.
bool RetailSoundWidenAliasGraphs(RetailZoneLoadSession *session,
                                 const RetailWalkSndAliasOffsets &offsets,
                                 snd_alias_t *alias,
                                 bool registerNestedOwners = false,
                                 uint32_t *registeredLoadedSoundsOut = nullptr,
                                 uint32_t *registeredSndCurvesOut = nullptr);

// B1 SndCurve small root: consumes one inline 72-byte SndCurve root + its
// filename XString through the walk oracle, widens filename/knotCount/knots
// into the native SndCurve, and registers through Load_SndCurveAsset.
bool RetailWalkLiveLoadSndCurve(RetailZoneLoadSession *session,
                                FsRetailFastfileReader *reader, XAssetHeader *out);
