# Retail fastfile asset loading: model, bugs found, and the cross-zone wall

Engineering writeup of the retail COD4 fastfile (`.ff`) asset loader, as
exercised against real `killhouse.ff` data while registering the native
GfxWorld. It is organized from the asset perspective and grounds each claim
in source.

Primary files:

| File | Role |
| --- | --- |
| `src/database/db_retail_wire.cpp/.h` | Block cursors, the reference-token decoder, pool-index decode |
| `src/universal/com_files.cpp/.h` | The FS-owned live reader (`FsRetailFastfileReader`): block buffers, inline names, material/image bodies, image pool |
| `src/database/db_retail_walk.cpp/.h` | The walk-only reader plus the live-load dispatch (`RetailWalkLoadZoneAssets`) |
| `src/database/db_retail_zone.cpp` | `RetailZoneLoadSession` transaction (alloc, register, abort) |
| `src/database/db_registry.cpp` | `DB_RetailZoneBegin`/`DB_RetailZoneEnd`, the real zone registry |
| `src/database/db_retail_decode_*.cpp` | Per-family widening (material, image, techniqueset, world, ...) |
| `src/database/db_retail_decode_world.cpp/.h` | The GfxWorld decoder (`RetailWalkLiveLoadGfxWorld`) |

---

## 1. The retail fastfile asset model

### 1.1 Nine blocks

A fastfile decompresses into nine independent byte blocks, each with its own
cursor. `RetailWireBlocks` (`db_retail_wire.h`) is the only production cursor
over them:

```c
struct RetailWireBlocks
{
    XZoneMemory *zone;
    uint32_t cursor[9];
    uint32_t activeBlock;
    uint32_t stackIndex;
    uint32_t stack[32];
};
```

Roles observed so far, matching the Android/`db_load.cpp` load points:

- **Block 0** — temp/scratch. Asset *roots* stream here (a 36-byte `GfxImage`
  body, an 80-byte `Material` body, a 16-byte `GfxLightDef` body, the 732-byte
  `GfxWorld` root). Rewound after each asset (`RetailWireBlocksRewind(&session->wire, 0, bodyStart)`).
- **Block 1** — runtime/native expansion. Allocated and zero-filled, but
  consumes *zero* file bytes (`RetailZoneLoadSessionExpandRuntime`,
  `db_retail_walk.cpp`). Used for arrays the engine fills at runtime.
- **Blocks 2/3** — delayed streams, present in the format, unimplemented here
  by design (`RetailWalkOpenDirectory` rejects them).
- **Block 4** — the persistent "virtual" block. Everything a native asset can
  still point at after load lives here: the XAsset directory itself, every
  inline name string, texture-def tables, constant tables, state-bits tables,
  and `DB_InsertPointer` reserved slots. **All alias resolution happens in
  block 4.** OAT's own IW3 loader agrees: `INSERT_BLOCK = XFILE_BLOCK_VIRTUAL`
  = index 4.
- **Blocks 7/8** — packed vertex and index geometry (`db_retail_walk.cpp:1875`,
  `Load_XSurface`).

### 1.2 The reference token: `((block << 28) | offset) + 1`

Every 32-bit pointer field in the wire is a token, never a pointer. The
canonical decode is `RetailWireTokenDecodeBlocks` (`db_retail_wire.cpp:154`),
and it is the *only* implementation — every family decoder calls it through a
thin local adapter (`db_retail_decode_{image,material,techniqueset,world}.cpp`
each define a 3-line `DecodeFastfileToken` forwarder):

```c
const uint32_t adjusted = encoded.encoded - 1u;
decoded.block  = adjusted >> 28;
decoded.offset = adjusted & 0x0fffffffu;
```

Four kinds (`RetailWireTokenKind`):

| Encoded value | Kind | Meaning |
| --- | --- | --- |
| `0` | `NULL` | No reference |
| `0xffffffff` | `INLINE` | The referenced body follows immediately in the stream |
| `0xfffffffe` | `INSERT` | Reserve a 4-byte slot in block 4 (`DB_InsertPointer`), *then* the body follows |
| anything else | `OFFSET` | A `(block, offset)` location; subtract 1 first |

The one-based encoding exists so that `0` can mean null without colliding with
block 0 offset 0. `RetailWireTokenEncode` is the inverse and is what
write-back paths use.

Pseudo-block **15** is not a wire block at all: it is this port's
representation of the engine's `DB_AddXAsset` pool table
(`RETAIL_FASTFILE_POOL_BLOCK`, `com_files.h:187`;
`RetailWirePoolIndexDecode`, `db_retail_wire.cpp:213`). A pool reference is
`((15 << 28) | poolIndex) + 1`.

### 1.3 Inline widening vs. alias resolution

Consider a `Material`'s texture slots — the mechanism that most of these
findings turn on. `FS_ReadRetailFastfileMaterial`
(`com_files.cpp:1740`) streams the 80-byte root, then, if
`textureTableRef == INLINE`, reads `textureCount * 12` bytes into block 4 at
`tableOffset` and walks each 12-byte `MaterialTextureDef`. Bytes 8..11 of each
def are the image slot, at absolute block-4 offset `defOffset + 8`:

- **`0`** — no image.
- **`INLINE` / `INSERT`** — the `GfxImage` body follows in the stream. The
  `INSERT` form first reserves its own aligned 4-byte block-4 slot. The image
  is read (`FS_ReadRetailFastfileImage`), then — critically — the *pooled
  reference is written back into the texture-def slot's own block-4 bytes*:

  ```c
  uint32_t poolRef = 0;
  if (FS_RetailFastfileRegisterImagePoolSlot(reader, inlineImage.nameRef, imageSlotOffset,
                                             &poolRef) != FS_RETAIL_FF_WIRE_OK)
      return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
  ```

- **anything else** — an *alias*: `DB_ConvertOffsetToAlias`. The value decodes
  to a block-4 location that is expected to *already hold a pool reference*,
  written there by whichever earlier occurrence declared that image inline.
  `FS_RetailFastfileResolveImageAlias` (`com_files.cpp:2408`) re-reads those 4
  bytes and accepts them only if they form a valid pool reference.

This is a self-modifying-buffer trick. The zone's block-4 buffer is both data
and a resolution table: the *first* declaration of a shared image patches its
own on-disk slot, and every later reference to that same image is literally a
pointer to that slot. The consequence, which drives sections 3 and 5 below:
**if the declaring occurrence never performs the write-back, every alias to it
fails**, even though the wire data is perfectly well-formed.

### 1.4 The image pool

`FsRetailFastfileReader` carries a flat table:

```c
uint32_t imagePoolCount;
uint32_t imagePoolNameRefs[RETAIL_FASTFILE_IMAGE_POOL_MAX];
```

`FS_RetailFastfileRegisterImagePoolSlot(reader, nameRef, slotOffset, &poolRef)`
appends `nameRef` (itself a block-4 token pointing at the image's name string),
computes `poolRef = ((15 << 28) | index) + 1`, patches it into block 4 at
`slotOffset`, and returns it. `FS_RetailFastfileImagePoolNameRef` walks it back
to a name, which `ResolveImageRef` then looks up via `DB_FindXAssetHeader`.

The pool is per-reader (i.e. per-zone-load), so it is a *within-zone* aliasing
mechanism only.

> **Observation from the code, not a finding from a run:**
> `FS_RetailFastfileResolveImageAlias` and `FS_RetailFastfileImagePoolNameRef`
> test the top nibble of the *raw* value (`imageRef >> 28`) but compute the
> offset from `imageRef - 1`. These disagree only at the exact boundary value
> `0x40000000` (raw nibble 4, decoded block 3). Not implicated in anything
> below.

### 1.5 This port's own extensions

Two resolution paths exist that are *not* in `db_load.cpp`, added earlier for
cross-zone stub references the original 32-bit engine resolved differently:

- **The comma-stub directory path.** A `techniqueSetRef` (or image ref) can
  decode to an offset that lands *inside this zone's own asset directory* —
  specifically at `directoryOffset + entry*8 + 4`, the `header` half of
  directory entry `entry`. That entry's registered name is then used
  (`ResolveTechniqueSetRef`, `db_retail_decode_material.cpp:95`;
  `ResolveImageRef`, same file line 152). Names beginning with `,` are stubs:
  the leading comma is stripped and the *real* asset is looked up globally via
  `DB_FindXAssetHeader`, i.e. in an **earlier-loaded zone**. `stubNames[]` is
  built during the directory loop in `RetailWalkLoadZoneAssets`
  (`db_retail_walk.cpp:5077-5240`).
- **The plain-name path.** `ReadBlockString(reader, ref, ...)` — treat the
  token's target as a NUL-terminated name string and look it up.

Acceptance is gated by a `strict` flag on the decoders
(`db_retail_decode_material.h`, `db_retail_decode_image.h`): tolerant mode
permits `$white`/`$default` fallbacks and preserves the existing menu boot
path; **acceptance proofs must run strict**, where an unresolved reference
fails loudly.

### 1.6 Two readers, one stream

There are deliberately **two independent readers** over the same file:

- The **walk-only reader** — `db_retail_walk.cpp`'s `ReadRetail*Body`
  functions, driven through `RetailZoneLoadSessionReadStream` into
  `session->zoneMemory->blocks[]`. It consumes every wire byte exactly, but
  constructs nothing and registers nothing (`RETAIL_WALK_WALKED_DEFERRED`).
- The **live reader** — `com_files.cpp`'s `FS_ReadRetailFastfile*` functions,
  reading into `reader->blockData[]`, used by the families that get real
  native registration.

They share a single decompression stream, so the dispatch loop hands the
stream back and forth via `SyncSessionToReader` / `SyncReaderToSession`
(`db_retail_walk.cpp:237-276`), which copy only *forward-advancing* block-4
ranges between the two buffers. Calling a live reader from inside a walk-only
path would double-consume the stream. This constraint shaped the
`$identitynormalmap` fix (section 3).

### 1.7 Zone / session / registry

`RetailWalkLoadZoneAssets` opens a `RetailZoneLoadSession`, which calls
`DB_RetailZoneBegin` (`db_registry.cpp:2390`) to claim a `g_zoneIndex` and a
native arena. Every widened asset is allocated from the session arena and
registered through the real database owners (`RetailZoneLoadSessionRegister`
→ `DB_AddXAsset`), tagged with the owning zone index. On any failure the
session calls `RetailZoneLoadSessionAbort` → `DB_RetailZoneEnd`. Section 6 is
about what that means.

---

## 2. Five real-data bugs in the GfxWorld decoder

`db_retail_decode_world.cpp` passed its synthetic `KILLHOUSE_WORLD_REGISTRY`
fixture but had never been driven against the real `killhouse.ff` GfxWorld
body. Driving it (via `switch_retail_boot_test.cpp`'s `m9a_bounded_load`
section with staged real zones) found five bugs. All five are invisible to a
hand-built fixture because the fixture had one element per array, zeroed
records, and no shared/nested assets.

### 2.1 Interleaved vs. bulk record consumption (probes, lightmaps, materialMemory, DPVS surfaces)

**Bug.** Four arrays were decoded as "consume one fixed-size record, then
immediately widen its nested image/material pointer", per element.

**Why that's wrong.** The reference loaders in `db_load.cpp`
(`Load_GfxReflectionProbeArray`, `Load_GfxLightmapArrayArray`,
`Load_MaterialMemoryArray`, `Load_GfxSurfaceArray`) issue a *single*
`Load_Stream(recordSize * count)` for the entire array, and only then loop
resolving pointers. If element *i* carries an `INLINE` image, its body follows
in the stream — so widening it mid-loop consumes bytes that are actually
element *i+1*'s still-unread fixed record. With a one-element fixture this is
unobservable; with real data every array desyncs from element 1 onward.

**Fix.** Bulk-read into a raw staging buffer, then resolve in a second pass.
E.g. reflection probes (`db_retail_decode_world.cpp:756`):

```c
// Load_GfxReflectionProbeArray bulk-reads every fixed 16-byte record
// (Load_Stream(16*count)) before any element's image pointer is
// resolved: an inline image on probe[i] must not eat probe[i+1]'s
// still-unread record bytes, so consume the whole array first and
// only then widen each slot.
if (!RetailWorldConsume(&context, rawProbes, probeCount * 16u))
    return false;
```

Record sizes: reflection probe 16, lightmap 8, materialMemory 8, GfxSurface 48,
GfxStaticModelDrawInst 76.

### 2.2 Wrong record shape for `dpvsInstsRef`

**Bug.** `GfxWorldDpvsStatic::smodelInsts` (root offset 656) was decoded as
76-byte `GfxStaticModelDrawInst` records, each with an XModel widen.

**Truth.** That field is the plain 28-byte `GfxStaticModelInst`
(`Load_GfxStaticModelInstArray`) — bounds data only, **no XModel pointer at
all**. The XModel pointer lives exclusively on the separate
`smodelDrawInsts` / `dpvsDrawInstsRef` field (root offset 668), at record
offset +56 of the 76-byte struct.

**Fix.** `dpvsInstsRef` now consumes `dpvsSmodelCount * 28` with no widen
(`db_retail_decode_world.cpp:1147`). `dpvsDrawInstsRef` keeps its own 76-byte
records and got the bulk-then-resolve fix from 2.1
(`db_retail_decode_world.cpp:1194`).

Both fields are sized by the *same* count (`dpvsSmodelCount`), which is why
the wrong shape didn't trip a count check — it just read 76 bytes where 28
were serialized, desyncing everything after.

### 2.3 `GfxBrushModel` required all-zero records

**Bug.** The `models` array required each 56-byte record to be all-zero, and
failed loudly otherwise. (The fixture's records were zeroed, so this passed.)

**Truth.** `Load_GfxBrushModelArray` is a plain `Load_Stream` with no
per-element follow-up: the struct is bounds floats plus surface-range
`uint16`s, entirely pointer-free, so the ILP32 wire layout already equals the
native layout. A real world's brush models are never all-zero.

**Fix** (`db_retail_decode_world.cpp:945`):

```c
static_assert(sizeof(GfxBrushModel) == 56,
              "GfxBrushModel must match the 56-byte wire record exactly");
...
uint8_t record[56]{};
if (!RetailWorldConsume(&context, record, sizeof(record)))
    return false;
std::memcpy(&models[model], record, sizeof(record));
```

### 2.4 Undersized native arena

**Bug.** `kRetailZoneNativeArenaBytes` in `RetailWalkLoadZoneAssets`
(`db_retail_walk.cpp:5038`) was a fixed 16 MB.

**Measured.** Real killhouse registered lightmap/probe pixel payload pushed
arena peak to **~27 MB by ordinal 772 of 1684** — i.e. it would have run out
less than halfway through even if nothing else were wrong.

**Fix.** Raised to 96 MB, under the rule "replace fixed capacities only where
real zones require it."

### 2.5 Stub table never threaded into world-nested materials

**Bug.** `RetailWalkLiveLoadGfxWorld` hardcoded zeros for the zone's
ordinal-indexed stub table when calling `RetailWorldWidenMaterial`. So any
material nested inside the world that used the comma-stub directory-offset
technique-set binding (section 1.5) — the same binding standalone Material and
MenuList assets already used successfully — could never resolve.

**Fix.** `RetailWorldLoadContext` (`db_retail_decode_world.h:73`) now carries
`directoryOffset`, `directoryBytes`, `stubNames`, `stubCount`, `techCache`,
populated from `RetailWalkLoadZoneAssets`'s own directory loop and passed
through to every nested `RetailWidenMaterialFromWire` call
(`db_retail_decode_world.cpp:257`).

---

## 3. `$identitynormalmap`: a declaring occurrence inside a deferred asset

### Symptom

Real `killhouse.ff` reached its second `materialMemory` material,
`wc/caulk_shadow`, and failed one texture slot:

```
imageRef=0x400080ed  ->  block 4, offset 33004   (0x400080ed - 1 = 0x400080ec)
```

`FS_RetailFastfileResolveImageAlias` found no pool reference at 33004, so the
alias declined and the material failed in strict mode.

### What it should resolve to

Ground truth via the OAT `Unlinker` oracle (`--gdt` dump of `killhouse.ff`,
per the port's "OAT as the asset oracle" practice):
`materials/wc/caulk_shadow.json` has exactly 2 textures —

- `colorMap: "caulk_shadow"` (tex[1]) — the material's own inline image, which
  already widened correctly;
- `normalMap: "$identitynormalmap"` (tex[0]) — the failing slot.

`$identitynormalmap` is a genuine engine stock image: `R_InitImages`
(`src/gfx_d3d/r_image.cpp`) registers it via `Image_LoadIdentityNormalMap` →
`Image_LoadSolid(0x80,0x80,0xFF,0x80)`, a 1×1 `D3DFMT_A8R8G8B8` texture. It is
also a real on-disk file, `images/$identitynormalmap.iwi` in `main/iw_00.iwd`
(the procedural generator is the fallback, not the sole source). **143 of
killhouse's own materials reference it** — the standard "no real normal map"
default.

### Why the alias had nothing to read

Per-ordinal `FS_RetailFastfileBlockCursor(reader, 4)` snapshots in
`RetailWalkLoadZoneAssets`'s directory loop showed block-4 offset 33004 falls
inside **ordinal 12's** own block-4 footprint (cursor 30894 → 38144), and
ordinal 12 has `type=3` (XModel).

That is the actual mechanism, not a coincidence. XModel ordinal 12 has a
surface whose material is the *first inline declarer* of `$identitynormalmap`
in this zone; that material's texture-def slot sits at exactly block-4 offset
33004 — precisely the address `wc/caulk_shadow`'s alias later computes.

But XModel is `walked_deferred` by design. `ReadRetailXModelBody`
(`db_retail_walk.cpp`, its `numSurfs` per-surface material loop) routes each
surface material through the *walk-only* chain
`ReadRetailAssetSlotBody` → `ReadRetailMaterialBody` →
`ReadRetailMaterialTable`. That chain consumes every wire byte correctly
(byte-accurate against OAT — which is why nothing ever desynced), but it never
calls the live registration path, so it never performed the pool-ref
write-back at offset 33004.

Cross-check against OAT ruled out any encoding mismatch:
`ZoneLoading/Zone/Stream/ZoneInputStream.cpp`'s
`ConvertOffsetToAliasLookup` / `SetInsertedPointerAliasLookup` use the
identical block+offset keying, with `INSERT_BLOCK = XFILE_BLOCK_VIRTUAL` (4)
and `OFFSET_BLOCK_BIT_COUNT = 4` for IW3 — matching our decode exactly.

### The fix

The constraint: the walk-only and live readers are deliberately separate
implementations over one stream (section 1.6), so the walk-only path *cannot*
just call the live image reader — that would double-consume the inline image
body's bytes. The fix therefore exposes what the walk-only reader already
parsed, rather than re-parsing:

1. **`RetailWalkDirectoryRecord::resolvedNameRef`** (`db_retail_walk.h:37`) —
   `ReadRetailImageBody` (`db_retail_walk.cpp:643`) now records the walked
   image's name using the exact encoding the live side's
   `ReadRetailFastfileInlineName` would have produced:

   ```c
   const uint32_t nameStart = session->wire.cursor[4];
   if (!ReadInlineRetailString(session, reader, &record->nameBytes))
       return false;
   record->resolvedNameRef = ((4u << 28) | nameStart) + 1u;
   ```

   For a non-inline name it passes the existing alias reference through
   unchanged; 0 for anonymous. `ReadRetailAssetSlotBody` propagates it to the
   caller's record for the `RETAIL_WALK_NESTED_IMAGE` case
   (`db_retail_walk.cpp:232`).

2. **`FS_RetailFastfileRegisterImagePoolSlot`** (`com_files.h:657`,
   `com_files.cpp:2391`) — extracted the "pool a name + patch the pool ref
   into block 4" half of `FS_ReadRetailFastfileMaterial`'s inline branch into
   a reusable exported function; that branch now calls it too.

3. **`ReadRetailMaterialTable`** (`db_retail_walk.cpp:836-869`) — after
   walking an inline/insert texture-def image slot (non-water, i.e. semantic
   ≠ 11), it computes that slot's own block-4 offset, calls the new function,
   **and mirrors the same 4-byte patch into `session->zoneMemory->blocks[4]`**
   as well as the reader's buffer. Both writes are required: `SyncSessionToReader`
   only copies forward-advancing ranges, so patching only the session buffer
   could be stomped, and patching only the reader's buffer would leave the
   walk-only reader's own later reads of that slot stale.

   It is best-effort: a full pool or an out-of-range slot silently preserves
   today's behavior (a later alias fails) rather than failing the walk over a
   capacity concern unrelated to walk-only correctness.

4. **`RETAIL_FASTFILE_IMAGE_POOL_MAX` 256 → 8192** (`com_files.h:195`) — the
   walk-only reader now pools every inline/insert texture it walks, not just
   live-loaded materials'. 256 was exhausted around ordinal 600 of 1684.
   Measured 684 entries after 771/1684 ordinals (extrapolating to ~1500 for
   the zone); 8192 is `RetailWireTechniqueCache`'s 2048-entry table doubled
   twice, for headroom.

5. **Seeded the stock image in the test**
   (`switch_retail_boot_test.cpp:1166-1185`, using `StubRegisterStockAsset`
   from `switch_retail_zone_stubs_test.cpp:349` with `zoneIndex = 0` — the
   "not owned by any tracked zone" sentinel, immune to any `DB_RetailZoneEnd`
   sweep): a structurally accurate `$identitynormalmap` `GfxImage` header
   (`MAPTYPE_2D`, 1×1×1, no renderer resource).

   This is **not** a missing-dependency substitution. `$identitynormalmap` is
   exactly what the wire data resolves to; the real boot always runs
   `R_InitImages()` before any SP zone load, and this database-only test (no
   live D3D9 device) never runs it. Seeding reproduces that one real
   prerequisite rather than skipping it.

### Result

`RetailWorld: material widened 'wc/caulk_shadow' tex=2`, and the bounded load
proceeded through several more `materialMemory` materials
(`wc/ch_brick_wall_04`, `wc/ch_brick_wall_08`, ...). A full `./test host` run
stayed green everywhere except the next, further-along failure.

---

## 4. LightDef: implementing `DB_ConvertOffsetToAlias` for a non-material `GfxImage*`

After killhouse's next blocker turned out to be a cross-zone technique-set
stub (`ResolveTechniqueSetRef: entry=373 stub=,wc_l_sm_b0c0n0s0`), the boot
test was extended to bounded-live-load the real prerequisite zones first
(`code_post_gfx.ff`, then `ui.ff`, then `common.ff` — the real boot order), via the `boundedLoadEarlierZone` lambda at
`switch_retail_boot_test.cpp:1206-1238`.

That immediately exposed a second bug: `code_post_gfx.ff` fails at ordinal
1227 (a LightDef). `RetailWalkLiveLoadLightDef` (`db_retail_walk.cpp:4853`)
only ever supported an **inline/insert** attenuation image, and failed loudly
on every other form.

`db_load.cpp`'s real path is `Load_GfxLightDef` → `Load_GfxLightImage` →
`Load_GfxImagePtr`, and a non-inline `GfxImage*` is **always** resolved by
`DB_ConvertOffsetToAlias` — the same mechanism material texture-def slots use
(section 1.3). It is applied uniformly to every `GfxImage*` field, not just
material texture defs; this port had only ever implemented it in the material
path.

**Fix:**

- Extracted the mechanism from `FS_ReadRetailFastfileMaterial`'s inline branch
  into exported **`FS_RetailFastfileResolveImageAlias`** (`com_files.h:675`,
  `com_files.cpp:2408`); the material branch now calls it too
  (`com_files.cpp:1819`), removing the duplication.
- Wired it into `ResolveImageRef` as the *first* thing tried, ahead of this
  port's own comma-stub-directory and plain-name extensions
  (`db_retail_decode_material.cpp:171-187`). This also required promoting
  `ResolveImageRef` out of the anonymous namespace into a declared export
  (`db_retail_decode_material.h:83`).
- Threaded `directoryOffset` / `directoryBytes` / `stubNames` / `stubCount`
  into `RetailWalkLiveLoadLightDef` so its non-inline branch calls
  `ResolveImageRef(..., strict=true)` (`db_retail_walk.cpp:4917`) instead of
  failing outright.

---

## 5. Unresolved: `falloff_linear`'s 2-byte offset discrepancy

With section 4's fix in place, `code_post_gfx.ff` **still** fails at ordinal
1227, on the same reference. This is the one open item.

### The discrepancy

```
imageRef      = 0x40036511      (samplerState = 98)
decodes to      block 4, offset 222480      (0x40036511 - 1 = 0x40036510)
raw bytes there = mid-string ASCII "lloff_linear"
real string     = "falloff_linear", starting at offset 222478
```

`FS_RetailFastfileResolveImageAlias` correctly declines (those bytes are not a
pool reference), and the plain-name fallback then reads the same wrong offset.
The gap is exactly **2 bytes**, precise and reproducible — not a coincidental
overlap with unrelated data the way the XModel case in section 3 initially
looked.

The OAT oracle confirms `falloff_linear` is a real, standalone top-level Image
asset in `code_post_gfx.ff` (`image, falloff_linear` in the `--list` dump),
listed immediately before the failing lightdefs (`light_dynamic`,
`light_point_linear`) — so it should already be registered by ordinal 1227.

### Ruled out

- **Field layout.** A full root-record dump was added
  (`RetailWalkLiveLoadLightDef: root nameRef=... imageRef=... raw='...'`,
  `db_retail_walk.cpp:4869`) and the raw 16 bytes are byte-for-byte consistent
  with our reads. Confirmed against `db_load.cpp` *and* against OAT's
  generated loader `ZoneCode/Game/IW3/XAssets/gfxlightdef/gfxlightdef_iw3_load_db.cpp`
  (`FillStruct_GfxLightDef`: `name`@0, `attenuation`{`image`@0, `samplerState`@4}@4,
  `lmapLookupStart`@12). The file really does store that value at that
  position.
- **The token-decode formula.** `(value - 1) & 0x0FFFFFFF` with the block
  nibble is `RetailWireTokenDecodeBlocks`'s central, single implementation,
  used successfully by every other reference, and matches
  OAT's `OFFSET_BLOCK_BIT_COUNT = 4` / `INSERT_BLOCK = 4` for IW3.
- **The INSERT-slot theory.** A top-level asset *can* be declared `INSERT` at
  directory level (`RetailWalkLoadZoneAssets`'s generic walked-deferred
  fallback reserves a 4-byte block-4 slot for exactly that form). But
  `falloff_linear`'s own name text is genuinely present in block 4, which only
  happens via the live inline path (`RetailWalkLiveLoadImage` →
  `ReadRetailFastfileInlineName`) — so its header is `kInlineReference`, not
  `INSERT`. Theory ruled out *for this asset*.

  Separately noted as a real correctness gap in its own right: the bounded
  dispatch's `ASSET_TYPE_IMAGE` branch only live-loads
  `header == kInlineReference`; an `INSERT`-form top-level image falls
  silently through to walked-deferred (slot reserved, never registered, never
  named) instead of failing loudly.

### The architectural difference found in OAT

Reading `ZoneInputStream::ConvertOffsetToAliasLookup`'s full body surfaced a
structural difference from this port worth recording:

- **OAT never re-reads file bytes for this resolution.** It consults two
  in-memory hash maps (`m_alias_redirect_lookup`, `m_pointer_redirect_lookup`)
  keyed by the exact `(block, offset)` integer, populated only when an earlier
  `Load` call explicitly registered that key via
  `SetInsertedPointerAliasLookup` / `AddPointerLookup`. OAT's "declaring
  occurrence" bookkeeping is a **host-side table entry**.
- **This port re-reads raw bytes** from `reader->blockData[4]` at the computed
  offset, banking on the real 32-bit engine's declaring occurrence having
  patched that in-memory location in place.

Both models agree for a material texture-def slot (that slot *is* the texture
def's own on-disk position — confirmed working, `wc/caulk_shadow`). But
offset 222480 lands 2 bytes inside `falloff_linear`'s own name string
(222478-222492, hex-dump confirmed), which is not a position this port ever
writes a pool ref to: a **standalone** top-level image's registration
(`RetailWalkLiveLoadImage`) performs no block-4 write-back at all today — that
mechanism exists only for material texture-def slots.

So the pool-alias path can only work here if the real engine *also* pools
plain top-level image registrations through the identical block-4 write-back
for any `GfxImage*` field. Not confirmed either way.

### The alignment observation (reopens "coincidental overlap")

- **222480 is 4-byte aligned. 222478 is not.**
- Every size/cursor computation in this codebase aligns to 4
  (`(cursor + 3) & ~3` throughout `db_retail_walk.cpp` and `com_files.cpp`).
- But `ReadRetailFastfileInlineName` (`com_files.cpp`) — the function that
  actually wrote `falloff_linear`'s bytes — does **not** align: it packs name
  bytes back-to-back with zero padding. That matches the observed byte-exact
  adjacency between the preceding string's NUL (222477) and `falloff_linear`'s
  first byte (222478).

If some other, still-unidentified reserved/aligned slot legitimately lands at
222480 for an unrelated reason, it would fall inside this unaligned name
purely by position — exactly the shape of the XModel false lead in section 3.
Not proven either way.

### One caveated data point

OAT's fallback raw dump for LightDef (it has no dedicated JSON dumper for this
type) shows, for **both** `light_dynamic` and `light_point_linear`, the bytes
`62 66 61 6c 6c 6f 66 66 5f 6c 69 6e 65 61 72 00` = `"bfalloff_linear\0"` — a
leading `b` that neither `--list`'s name output nor our decode ever showed.
Identical across both lightdefs, so not per-asset noise, but this is OAT's
internal raw-dump format for an unexported type; most likely a leading
type/flag byte from whatever raw struct region it wrote, not fastfile-level
data. Recorded, not solid enough to build a fix on.

### Suggested next steps

Either:

1. Instrument `RetailWalkLiveLoadImage` / `ReadRetailFastfileInlineName` to
   log the exact block-4 range every top-level image's name write touches
   across a full `code_post_gfx.ff` walk, and check whether *any* of those
   ranges bracket 222480 — not just the immediately-preceding one this
   investigation focused on; or
2. Read OAT's actual raw-asset-dump code (`ObjWriting`, wherever the
   "no dedicated dumper" fallback lives) to determine what the leading `b`
   byte is, which would settle whether it means anything.

---

## 6. The architectural discovery: zone rollback is all-or-nothing

This is the load-bearing finding, and it invalidates the whole
"tolerate partial prerequisite loads" strategy.

### Evidence trail

`wc_l_sm_b0c0n0s0` — killhouse's original blocking technique-set stub — is
**not** in `code_post_gfx.ff`. The OAT oracle places it in `common.ff`
(`techniqueset, wc_l_sm_b0c0n0s0`). So the M9a section was extended to
bounded-load all three prerequisites (`boundedLoadEarlierZone` covering
`code_post_gfx.ff` 1639 assets, `ui.ff` 35, `common.ff` 6502), each
independently gated on staging.

Running all three produced:

```
common.ff ordinal 3993:  ResolveTechniqueSetRef: entry=3992 stub=,2d
```

— a comma-stub needing `code_post_gfx.ff`'s `2d` technique set. But the log
proves `code_post_gfx.ff` **did** successfully resolve and use that exact `2d`
technique set earlier in its own run (ordinal 2, `ui_cursor` material,
comma-stub entry=1 → `2d`, widened successfully). By the time `common.ff`
looked for it, it was gone.

### Root cause

`code_post_gfx.ff`'s own bounded load fails later, at its unrelated ordinal
1227 LightDef bug (section 5). A failed `RetailWalkLoadZoneAssets` calls
`RetailZoneLoadSessionAbort` on the way out (`db_retail_walk.cpp:4294`,
`:4309`, `:4325`, `:4352`, `:4363`, `:4374`), which calls `DB_RetailZoneEnd`:

```c
bool RetailZoneLoadSessionAbort(RetailZoneLoadSession *session)
{
    ...
    if (session->zoneIndex && !DB_RetailZoneEnd(session->zoneIndex))
        return false;
    ...
}
```

`DB_RetailZoneEnd` (`db_registry.cpp:2454`) then unconditionally sweeps
**every** registration owned by that zone — canonical entries included — via
the production `DB_UnloadXZone` path under the registry write lock, before
releasing PMem.

### This is correct behavior, not a bug

`RetailZoneLoadSessionAbort`'s whole job is a clean all-or-nothing rollback,
matching the real engine's `DB_LoadXFileInternal` "load the whole zone or none
of it" contract. The four-zone lifetime proof exists specifically to pin this
transactional guarantee (`retired_ptrs=0` — no pooled header may survive
pointing into freed zone memory). The stub reproduction in
`switch_retail_zone_stubs_test.cpp` sweeps the same way.

### The consequence

**A prerequisite zone's registrations only persist for a later zone to alias
against if that prerequisite zone's own bounded load succeeds completely, with
zero failures anywhere in its own ~1600-6500 assets.**

Partial/tolerant bounded loading of a prerequisite zone can *never* leave
anything behind, by construction — no matter how many individual asset bugs
get fixed piecemeal, a single remaining failure anywhere in the zone erases
all of it.

---

## 7. What this means for remaining scope

Resolving killhouse's cross-zone comma-stubs is not "fix `falloff_linear` and
move on." It requires `code_post_gfx.ff`, `ui.ff`, and `common.ff` to each
bounded-load with **zero** failures.

That very likely means finding and fixing every asset-family gap each of those
zones exercises that killhouse's own bounded load never happened to hit.
Killhouse's `materialMemory` reached the XModel-declared-image gap (section 3)
and the LightDef-alias gap (section 4) only because of its own specific asset
mix; three much larger, more varied zones will surface more, different gaps of
the same kind — format edge cases these fixes narrowed but did not
prove exhaustive.

Realistically a large undertaking, not a few more targeted patches.

**Recommended order** — treat each prerequisite zone as its own bounded-load
milestone, mirroring the scrutiny killhouse itself got:

1. **`ui.ff`** (35 assets, already fails at its own ordinal 2) — smallest, so
   the iteration loop is fastest.
2. **`code_post_gfx.ff`** (1639 assets, LightDef gap at ordinal 1227 already
   diagnosed to the exact reference; section 5).
3. **`common.ff`** (6502 assets, not yet diagnosed past its `,2d` symptom —
   which resolves for free once `code_post_gfx.ff` loads cleanly, since `2d`
   is declared there).

### Status summary

| Item | State |
| --- | --- |
| GfxWorld bulk-vs-interleaved consumption (4 arrays) | Fixed |
| `dpvsInstsRef` record shape (28 vs 76 bytes) | Fixed |
| `GfxBrushModel` all-zero requirement | Fixed |
| Native arena 16 MB → 96 MB | Fixed |
| World stub-table threading | Fixed |
| `$identitynormalmap` pool write-back from walk-only path | Fixed, verified |
| Image pool 256 → 8192 | Fixed |
| LightDef `DB_ConvertOffsetToAlias` | Implemented (`FS_RetailFastfileResolveImageAlias`) |
| `falloff_linear` 2-byte offset discrepancy | **Open** |
| Top-level `INSERT`-form Image silently deferred | **Open** (correctness gap, unexercised) |
| Standalone top-level Image block-4 write-back | **Unknown** — unconfirmed whether the real engine does this |
| Partial prerequisite-zone loading | **Impossible by construction** — needs zero-failure loads |
