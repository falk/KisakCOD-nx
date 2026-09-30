#pragma once

// Retail fastfile reader: split out of com_files.{cpp,h} (seam 9) so
// com_files keeps only the FS-root and file-open hooks. A later slice
// merges this token handling into db_retail_wire for one cursor.

#include <cstdint>

// Quake-style loading readout: the database thread publishes the retail
// fastfile it is currently streaming (basename, e.g. "ui.ff") plus
// decompression progress 0..100 while a retail fastfile is open. Single
// writer (the DB thread walks one zone at a time); readers on the main or
// render thread may observe one frame of filename/percent skew, which is
// benign for a status line, so this is intentionally lock-free. Percent only
// changes ~100 times per file, so consumers cannot be spammed by
// construction. Returns false when no retail load is in flight.
bool __cdecl FS_GetRetailLoadStatus(char *fileOut, uint32_t fileOutSize, uint32_t *percentOut);

// Retail loadbar source on Switch, where db_file_load's classic counters
// (g_totalSize/g_loadedSize, written by the Win32 overlapped reader) are never
// populated.  For the most recent fastfile a retail walk opened since the last
// FS_ResetRetailLoadProgress: the compressed bytes consumed, the file's
// on-disk size and its XFile.externalSize -- the same quantities retail's
// DB_GetLoadedFraction divides.  Held after the walk closes.  Returns false
// when no walk has opened since the reset (retail: bar not sized yet).
bool __cdecl FS_GetRetailLoadProgress(uint64_t *readBytes, uint64_t *fileBytes, uint32_t *externalBytes);
// DB_ResetZoneSize's Switch half: forget the previous zone's sizing.
void __cdecl FS_ResetRetailLoadProgress();

enum FsRetailFastfileResult
{
    FS_RETAIL_FF_OK = 0,
    FS_RETAIL_FF_MISSING,
    FS_RETAIL_FF_TRUNCATED,
    FS_RETAIL_FF_WRONG_MAGIC,
    FS_RETAIL_FF_WRONG_VERSION,
    FS_RETAIL_FF_ZLIB_INIT,
    FS_RETAIL_FF_ZLIB_ERROR,
    FS_RETAIL_FF_ZLIB_TRUNCATED,
    FS_RETAIL_FF_XFILE_HEADER_TRUNCATED,
    FS_RETAIL_FF_BLOCK_TOTAL_OVER_CAP,
};

enum FsRetailFastfileWireResult
{
    FS_RETAIL_FF_WIRE_OK = 0,
    FS_RETAIL_FF_WIRE_READER_MISSING,
    FS_RETAIL_FF_WIRE_LIST_TRUNCATED,
    FS_RETAIL_FF_WIRE_ASSET_COUNT_OVER_CAP,
    FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL,
    FS_RETAIL_FF_WIRE_ASSETS_NOT_INLINE,
    FS_RETAIL_FF_WIRE_DIRECTORY_TRUNCATED,
    FS_RETAIL_FF_WIRE_BODY_NOT_INLINE,
    FS_RETAIL_FF_WIRE_BODY_TRUNCATED,
    FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH,
    FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED,
    FS_RETAIL_FF_WIRE_UNSUPPORTED_FORM,
};

// Pooled-asset reference encoding (the Android loader's DB_AddXAsset
// pseudo-block 15): ((RETAIL_FASTFILE_POOL_BLOCK << 28) | poolIndex) + 1.
#define RETAIL_FASTFILE_POOL_BLOCK 15u
// The walk-only reader pools every inline/insert texture it walks (the
// XModel-declared shared images), not just live-loaded materials' own; a
// real map zone needs on the order of 1500 entries. 8192 keeps generous headroom (matching
// RetailWireTechniqueCache's own 2048-entry technique table, doubled) per
// the rule to raise fixed capacities only where real zones require it.
#define RETAIL_FASTFILE_IMAGE_POOL_MAX 8192u

// Menu wire bounds (engine limits and loader sanity caps): UiContext keeps
// Menus[640], and menu/item/statement counts beyond these caps cannot come
// from a real zone build.
#define RETAIL_FASTFILE_MENU_MAX 640u
#define RETAIL_FASTFILE_MENU_ITEM_MAX 4096u
#define RETAIL_FASTFILE_EXPRESSION_ENTRY_MAX 256u
#define RETAIL_FASTFILE_KEY_HANDLER_MAX 256u

struct FsRetailFastfileAsset
{
    uint32_t type;
    uint32_t header;
};

struct FsRetailFastfileAssetList
{
    uint32_t scriptStringCount;
    uint32_t scriptStringsRef;
    uint32_t assetCount;
    uint32_t assetsRef;
    uint32_t decodedCount;
};

struct FsRetailFastfileRawFile
{
    uint32_t headerRef;
    uint32_t nameRef;
    int32_t len;
    uint32_t bufferRef;
};

struct FsRetailFastfileTechniqueSet
{
    uint32_t headerRef;
    uint32_t nameRef;
    uint8_t worldVertFormat;
    uint8_t hasBeenUploaded;
    uint8_t unused;
    uint8_t nameWasInline;
    uint32_t remappedTechniqueSetRef;
    uint32_t techniqueRefs[34];
};

struct FsRetailFastfileMaterialTechnique
{
    uint32_t headerRef;
    uint32_t nameRef;
    uint16_t flags;
    uint16_t passCount;
    uint32_t passOffset;
};

struct FsRetailFastfileMaterialShader
{
    uint32_t headerRef;
    uint32_t nameRef;
    uint32_t programRef;
    uint16_t programSize;
    uint16_t loadForRenderer;
    uint32_t programOffset;
    // Block-4 stream offset of this shader's own 16-byte record header
    // (inline form only; 0 for alias/null slots). An alias slot carries the
    // canonical encoding of this same offset, so decoders match aliases by
    // exact identity instead of guessing by proximity.
    uint32_t recordOffset;
};

struct FsRetailFastfileMaterialShaderArgument
{
    uint16_t type;
    uint16_t dest;
    uint32_t valueRef;
    uint32_t literalOffset;
};

struct FsRetailFastfileMaterialPass
{
    uint32_t vertexDeclRef;
    uint32_t vertexDeclOffset;
    FsRetailFastfileMaterialShader vertexShader;
    FsRetailFastfileMaterialShader pixelShader;
    uint8_t perPrimArgCount;
    uint8_t perObjArgCount;
    uint8_t stableArgCount;
    uint8_t customSamplerFlags;
    uint32_t argsRef;
    uint32_t argsOffset;
    uint32_t argCount;
};

// Serialized GfxImage body (36 wire bytes).  The texture slot keeps the
// original 32-bit value semantics: 0 = none, 0xffffffff = inline loadDef
// follows, 0xfffffffe = inline loadDef follows plus a 4-byte insert slot
// reserved in block 4 (DB_InsertPointer; the walker writes the block-0
// loadDef reference there, mirroring the Android LoadGfxTextureLoad).
struct FsRetailFastfileImage
{
    uint32_t headerRef;
    uint32_t nameRef;
    uint32_t textureRef;
    uint32_t textureInsertOffset;
    uint32_t mapType;
    uint8_t picmip[2];
    uint8_t noPicmip;
    uint8_t semantic;
    uint8_t track;
    uint32_t cardMemory[2];
    uint16_t width;
    uint16_t height;
    uint16_t depth;
    uint8_t category;
    uint8_t delayLoadPixels;
    uint8_t haveLoadDef;
    uint8_t loadDefLevelCount;
    uint8_t loadDefFlags;
    uint16_t loadDefDimensions[3];
    uint32_t loadDefFormat;
    uint32_t loadDefResourceSize;
    // Not a wire field: the block-0 byte offset FS_ReadRetailFastfileImage
    // already staged loadDefResourceSize raw pixel bytes at (right after the
    // 16-byte loadDef mirror). Lets RetailWidenImageFromWire copy the pixels
    // it already read off the stream instead of leaving them stranded.
    uint32_t pixelDataOffset;
    // Not a wire field: 1 when the serialized name slot was the inline token
    // (0xffffffff, i.e. the string bytes follow in the stream), 0 when it was
    // an already-resolved block-4 back-reference to a previously decoded
    // string (Load_XString's DB_ConvertOffsetToPointer branch).
    uint8_t nameWasInline;
};

// One serialized MaterialTextureDef (12 wire bytes).  The image slot follows
// the Android HandleAssetSlot/LoadGfxImagePtr semantics: 0 = none,
// 0xffffffff/0xfffffffe = inline image body follows (the -2 form first
// reserves a 4-byte DB_InsertPointer slot in block 4), anything else is an
// alias to a block-4 slot an earlier walk already filled.  After decoding,
// imageRef holds the pooled-image reference for both inline loads and
// resolved aliases; the walker writes that reference back into the wire
// slot (and any reserved insert slot), exactly like DB_AddXAsset +
// DB_ConvertOffsetToAlias.
struct FsRetailFastfileWater
{
    uint32_t reference;
    uint32_t h0Ref;
    uint32_t wTermRef;
    uint32_t m;
    uint32_t n;
    float floatTime;
    // Lx, Lz, gravity, windvel, winddir[2], amplitude, codeConstant[4].
    float parameters[11];
};

struct FsRetailFastfileTextureDef
{
    uint32_t nameHash;
    char nameStart;
    char nameEnd;
    uint8_t samplerState;
    uint8_t semantic;
    uint32_t imageRef;
    // When imageRef was an inline token, retain the already-decoded body for
    // the owning material walker.  imageRef becomes the pooled-image
    // reference assigned by this walk.
    uint8_t imageWasInline;
    FsRetailFastfileImage inlineImage;
    // Semantic 11 owns a water object, whose image is only one nested field.
    FsRetailFastfileWater water;
};

// Serialized Material body (80 wire bytes).  Pointer slots keep their 32-bit
// encoded values: techniqueSet/constantTable/stateBitsTable preserve the raw
// ref (0, inline token, or encoded alias/reference), textureTable is decoded
// inline when present and textureDefs receives its entries.
struct FsRetailFastfileMaterial
{
    uint32_t headerRef;
    uint32_t nameRef;
    uint8_t gameFlags;
    uint8_t sortKey;
    uint8_t textureAtlasRowCount;
    uint8_t textureAtlasColumnCount;
    uint64_t drawSurf;
    uint32_t surfaceTypeBits;
    uint16_t hashIndex;
    uint8_t stateBitsEntry[34];
    uint8_t textureCount;
    uint8_t constantCount;
    uint8_t stateBitsCount;
    uint8_t stateFlags;
    uint8_t cameraRegion;
    uint32_t techniqueSetRef;
    uint32_t textureTableRef;
    uint32_t constantTableRef;
    uint32_t stateBitsTableRef;
    uint32_t constantTableOffset;
    uint32_t stateBitsOffset;
    uint8_t nameWasInline;
};

// Serialized statement_s (8 wire bytes) and expressionEntry (12 wire
// bytes).  Entries stream as a numEntries pointer array followed by one
// 12-byte body per non-zero slot; only a type != 0 operand with
// dataType == VAL_STRING (2) carries an inline string.
struct FsRetailFastfileStatement
{
    uint32_t numEntries;
    uint32_t entriesRef;
};

struct FsRetailFastfileExpressionEntry
{
    int32_t type;
    uint32_t dataType;
    uint32_t operandRef;
};

// Serialized ItemKeyHandler chain node (12 wire bytes): a linked list
// streamed inline until a null next slot.
struct FsRetailFastfileItemKeyHandler
{
    int32_t key;
    uint32_t actionRef;
};

// Serialized MenuList body (12 wire bytes) {name, menuCount, menus}.  The
// menus pointer array streams as menuCount 4-byte asset-style slots; every
// retail ui.ff slot is the inline token, so the caller walks each menu body
// in order through FS_ReadRetailFastfileMenuPrefix.
struct FsRetailFastfileMenuList
{
    uint32_t headerRef;
    uint32_t nameRef;
    uint32_t menuCount;
    uint32_t menusRef;
    uint32_t menusOffset;
};

// Serialized menuDef_t prefix (284 wire bytes): windowDef_t (156 bytes:
// name, rect, rectClient, group, style scalars, four colors, background
// material slot) plus the menu fields through the items pointer.  String and
// asset slots keep their 32-bit encoded values; the reader walks the
// onKey chain and the visibleExp/rectXExp/rectYExp entries and stops after
// the items pointer array - item bodies belong to the item walk.
struct FsRetailFastfileMenu
{
    uint32_t headerRef;
    // windowDef_t
    uint32_t nameRef;
    float rect[4];
    int32_t rectHorzAlign;
    int32_t rectVertAlign;
    float rectClient[4];
    int32_t rectClientHorzAlign;
    int32_t rectClientVertAlign;
    uint32_t groupRef;
    int32_t style;
    int32_t border;
    int32_t ownerDraw;
    int32_t ownerDrawFlags;
    float borderSize;
    int32_t staticFlags;
    int32_t dynamicFlags;
    int32_t nextTime;
    float foreColor[4];
    float backColor[4];
    float borderColor[4];
    float outlineColor[4];
    uint32_t backgroundRef;
    // menuDef_t tail
    uint32_t fontRef;
    int32_t fullScreen;
    int32_t itemCount;
    int32_t fontIndex;
    int32_t cursorItem;
    int32_t fadeCycle;
    float fadeClamp;
    float fadeAmount;
    float fadeInAmount;
    float blurRadius;
    uint32_t onOpenRef;
    uint32_t onCloseRef;
    uint32_t onESCRef;
    uint32_t onKeyRef;
    uint32_t handlerCount;
    FsRetailFastfileStatement visibleExp;
    uint32_t allowedBindingRef;
    uint32_t soundNameRef;
    int32_t imageTrack;
    float focusColor[4];
    float disableColor[4];
    FsRetailFastfileStatement rectXExp;
    FsRetailFastfileStatement rectYExp;
    uint32_t expressionEntryCount;
    uint32_t itemsRef;
    uint32_t itemsOffset;
    // When backgroundRef was an inline/insert token, the reader widens the embedded
    // Material body here instead of collapsing it to just its name -- an anonymous
    // inline background material is never a named zone asset, so a later by-name
    // lookup (ResolveMaterialRef) can never find it and silently substitutes the
    // default material. See FS_ReadRetailFastfileMenu.
    uint8_t backgroundWasInline;
    FsRetailFastfileMaterial backgroundInlineMaterial;
    FsRetailFastfileTextureDef backgroundInlineTextures[8];
};

// Serialized itemDef_t body (372 bytes on the retail SP wire).  Pointer
// fields remain 32-bit encoded refs; the reader consumes nested XStrings,
// key handlers, type-data payloads, and statements in Load_itemDef_t order.
struct FsRetailFastfileItemDef
{
    uint32_t headerRef;
    uint32_t nameRef;
    float rect[4];
    int32_t rectHorzAlign;
    int32_t rectVertAlign;
    float rectClient[4];
    int32_t rectClientHorzAlign;
    int32_t rectClientVertAlign;
    uint32_t groupRef;
    int32_t style;
    int32_t border;
    int32_t ownerDraw;
    int32_t ownerDrawFlags;
    float borderSize;
    int32_t staticFlags;
    int32_t dynamicFlags;
    int32_t nextTime;
    float foreColor[4];
    float backColor[4];
    float borderColor[4];
    float outlineColor[4];
    uint32_t backgroundRef;
    int32_t type;
    int32_t dataType;
    int32_t alignment;
    int32_t fontEnum;
    int32_t textAlignMode;
    float textalignx;
    float textaligny;
    float textscale;
    int32_t textStyle;
    int32_t gameMsgWindowIndex;
    int32_t gameMsgWindowMode;
    uint32_t textRef;
    int32_t itemFlags;
    uint32_t parentRef;
    uint32_t mouseEnterTextRef;
    uint32_t mouseExitTextRef;
    uint32_t mouseEnterRef;
    uint32_t mouseExitRef;
    uint32_t actionRef;
    uint32_t onAcceptRef;
    uint32_t onFocusRef;
    uint32_t leaveFocusRef;
    uint32_t dvarRef;
    uint32_t dvarTestRef;
    uint32_t onKeyRef;
    uint32_t enableDvarRef;
    int32_t dvarFlags;
    uint32_t focusSoundRef;
    float special;
    uint32_t cursorPos;
    uint32_t typeDataRef;
    int32_t imageTrack;
    FsRetailFastfileStatement statements[8];
    uint32_t handlerCount;
    uint32_t expressionEntryCount;
    uint32_t typeDataOffset;
    // See FsRetailFastfileMenu.backgroundWasInline: same reasoning for item backgrounds.
    uint8_t backgroundWasInline;
    FsRetailFastfileMaterial backgroundInlineMaterial;
    FsRetailFastfileTextureDef backgroundInlineTextures[8];
};

struct FsRetailFastfileReader;

uint32_t __cdecl FS_FOpenRetailFileRead(const char *filename, int *file);
// A successful reader owns its FS handle until FS_CloseRetailFastfile.
FsRetailFastfileResult __cdecl FS_OpenRetailFastfile(const char *filename, FsRetailFastfileReader **reader);
uint32_t __cdecl FS_ReadRetailFastfile(FsRetailFastfileReader *reader, uint8_t *buffer, uint32_t len);
// Hand the tail of the last read back to the stream: the bytes are already
// inflated (and in the block mirror), so they are queued and served by the
// next FS_ReadRetailFastfile call. Used by chunked string scans, which must
// stop the caller-visible cursor exactly after the NUL while only paying one
// inflate call per chunk. Never mix with an outstanding unread.
void __cdecl FS_RetailFastfileUnread(FsRetailFastfileReader *reader, const uint8_t *bytes,
                                     uint32_t len);
uint32_t __cdecl FS_RetailFastfileXFileSize(const FsRetailFastfileReader *reader);
// TEMPORARY diagnosis: decompressed stream bytes consumed so far.
uint64_t __cdecl FS_RetailFastfileWireBytes(const FsRetailFastfileReader *reader);
// Basename of the file being streamed (Quake-style loading readout label),
// for zone-keyed cross-phase lookups. Never null; may be empty.
const char *__cdecl FS_RetailFastfileLoadLabel(const FsRetailFastfileReader *reader);
uint32_t __cdecl FS_RetailFastfileXFileExternalSize(const FsRetailFastfileReader *reader);
uint32_t __cdecl FS_RetailFastfileBlockSize(const FsRetailFastfileReader *reader, uint32_t block);
// Read-only wire-block view for the one canonical retail token decoder.  This
// remains FS-owned storage; callers must not retain or widen its address.
// Only blocks 0 and 4 are mirrored (the blocks this reader writes); every
// other block returns null with its exact FS_RetailFastfileBlockSize, so a
// token with a nonzero span into it must be decoded against zone memory.
const uint8_t *__cdecl FS_RetailFastfileBlockData(const FsRetailFastfileReader *reader, uint32_t block);
uint8_t *__cdecl FS_RetailFastfileBlockDataMutable(FsRetailFastfileReader *reader, uint32_t block);
#ifdef KISAK_RETAIL_FS_PROOF_HOST
// retained-pointer generalization provenance aid, compiled only into host proof
// links: true when `pointer` lies inside any currently-open reader's
// transient block storage, i.e. when retaining it past the reader's close
// would dangle (the ownership contract above).  The host registration
// double audits every registered asset's name pointer with this, so the
// bug class (a retained reader pointer) fails host-side instead of surfacing as a guest
// timeout or invalid access.
bool __cdecl FS_RetailFastfilePointerIsReaderTransient(const void *pointer);
uint32_t __cdecl FS_RetailFastfileTrackedReaderCount(void);
// sweep #3: FS_CloseRetailFastfile
// memsets every transient block to 0xDD before Z_Free, so a retained
// reader-owned pointer (the retained-pointer shape) reads recognizable poison at its use
// point instead of delayed freed-heap garbage. These counters observe the
// poison landing on every close: `blocks`/`bytes` are totals, and `failures`
// is nonzero if a poisoned span did not read back 0xDD at start/middle/end.
void __cdecl FS_RetailFastfilePoisonStats(uint32_t *blocks, uint64_t *bytes,
                                          uint32_t *failures);
// pointer-provenance generalization: host proofs install an audit that
// FS_CloseRetailFastfile invokes at the top, while the closing reader's
// blocks are still live and tracked. The audit calls
// FS_RetailFastfilePointerIsReaderTransient over zone-lifetime storage to
// find any raw pointer field -- not just a registration name -- that would
// dangle after this close (the retained-pointer class generalized across all 33 asset
// types). The hook must not open or close readers itself; Switch never
// compiles this.
typedef void (*FsRetailFastfileTransientAuditHook)(const FsRetailFastfileReader *reader);
void __cdecl FS_RetailFastfileSetTransientAuditHook(FsRetailFastfileTransientAuditHook hook);
#endif
uint32_t __cdecl FS_RetailFastfileBlockCursor(const FsRetailFastfileReader *reader, uint32_t block);
void __cdecl FS_RetailFastfileSetBlockCursor(FsRetailFastfileReader *reader, uint32_t block, uint32_t cursor);
uint64_t __cdecl FS_RetailFastfileBlockTotal(const FsRetailFastfileReader *reader);
// Reads the Android-derived XAssetList wire header and its inline 8-byte
// directory. The caller owns assets and supplies its capacity.
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileAssetList(
    FsRetailFastfileReader *reader,
    FsRetailFastfileAssetList *list,
    FsRetailFastfileAsset *assets,
    uint32_t assetCapacity,
    uint32_t *scriptStringRefs,
    uint32_t scriptStringCapacity);
// Decodes one inline 12-byte serialized RawFile body after its XAsset
// directory entry. Inline name and buffer data remain in block 4 and are
// returned as 32-bit encoded refs.
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileRawFile(
    FsRetailFastfileReader *reader,
    uint32_t headerRef,
    FsRetailFastfileRawFile *rawfile);
// Decodes the fixed 148-byte serialized TechniqueSet and its inline name.
// Nested MaterialTechnique bodies deliberately remain owned by the next walk.
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileTechniqueSetPrefix(
    FsRetailFastfileReader *reader,
    uint32_t headerRef,
    FsRetailFastfileTechniqueSet *techniqueSet);
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileMaterialTechniquePrefix(
    FsRetailFastfileReader *reader,
    uint32_t headerRef,
    FsRetailFastfileMaterialTechnique *technique);
// Walks the inline pass payload selected by MaterialTechniquePrefix.  All
// pointers remain encoded refs; inline payload offsets are block-4 offsets.
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileMaterialTechnique(
    FsRetailFastfileReader *reader,
    FsRetailFastfileMaterialTechnique *technique,
    FsRetailFastfileMaterialPass *passes,
    uint32_t passCapacity,
    FsRetailFastfileMaterialShaderArgument *args,
    uint32_t argCapacity);
// Decodes one inline 36-byte serialized GfxImage body after the owning asset:
// inline name, then the loadDef (16 bytes plus resourceSize payload bytes)
// when the texture slot is the inline/insert token.  Reference and insert
// slots stay encoded refs.
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileImage(
    FsRetailFastfileReader *reader,
    uint32_t headerRef,
    FsRetailFastfileImage *image);
// Decodes one inline 80-byte serialized Material body and, when present, its
// inline texture table (textureDefs receives textureCount entries; inline
// image bodies nested in the table are decoded through
// FS_ReadRetailFastfileImage).  techniqueSet, constantTable, and
// stateBitsTable slots stay encoded refs.
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileMaterial(
    FsRetailFastfileReader *reader,
    uint32_t headerRef,
    FsRetailFastfileMaterial *material,
    FsRetailFastfileTextureDef *textureDefs,
    uint32_t textureDefCapacity);
// Pooled-image identity (the Android loader's per-zone asset pool): every
// inline image load appends one entry; texture-def imageRef values (inline
// loads and resolved aliases alike) are pooled references into it.
uint32_t __cdecl FS_RetailFastfileImagePoolCount(const FsRetailFastfileReader *reader);
FsRetailFastfileWireResult __cdecl FS_RetailFastfileImagePoolNameRef(
    const FsRetailFastfileReader *reader,
    uint32_t poolRef,
    uint32_t *nameRef);
// Pools `nameRef` (a resolved name reference, same encoding
// FS_RetailFastfileImagePoolNameRef returns) as a new image-pool entry and
// patches the pool reference into block 4 at `slotOffset` -- the same
// write-back FS_ReadRetailFastfileMaterial's own inline-texture branch
// performs, exposed so a caller that already resolved a name through some
// other reader (e.g. the walk-only reader widening a deferred asset's own
// nested material) can make that name available to a later alias without
// re-reading the image body through this reader. Returns the computed pool
// reference in `outPoolRef` so the caller can mirror the same 4 bytes into
// any other buffer mirroring this reader's block 4. Fails (without
// touching the pool or block 4) if the pool is full or slotOffset is out
// of range for block 4.
FsRetailFastfileWireResult __cdecl FS_RetailFastfileRegisterImagePoolSlot(
    FsRetailFastfileReader *reader,
    uint32_t nameRef,
    uint32_t slotOffset,
    uint32_t *outPoolRef);
// DB_ConvertOffsetToAlias's real semantic for a non-inline GfxImage pointer
// (db_load.cpp's Load_GfxImagePtr, used uniformly for every GfxImage* field,
// not just a material texture-def slot): the reference is never a name --
// it is a block-4 (block,offset) pair whose raw 4 bytes, once the declaring
// occurrence has widened, already hold a pooled image reference. Reads
// those raw bytes and returns them in `outAliasedRef` only if they already
// form a valid pool reference (top nibble RETAIL_FASTFILE_POOL_BLOCK, index
// within this reader's current pool); returns
// FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED (leaving `outAliasedRef` untouched)
// if `imageRef` is not itself a block-4 offset, or the value sitting there
// is not (yet) a valid pool reference. Callers needing a name (not just a
// pool ref) should follow a successful call with
// FS_RetailFastfileImagePoolNameRef on the returned `outAliasedRef`.
FsRetailFastfileWireResult __cdecl FS_RetailFastfileResolveImageAlias(
    FsRetailFastfileReader *reader,
    uint32_t imageRef,
    uint32_t *outAliasedRef);
FsRetailFastfileWireResult __cdecl FS_RetailFastfilePatchImagePoolSlot(
    FsRetailFastfileReader *reader,
    uint32_t poolRef,
    uint32_t slotOffset);
// DB_InsertPointer's block-4 half (db_load.cpp: every -2/INSERT GfxImage*
// field reserves this before its inline body is read, so a later
// DB_ConvertOffsetToAlias reference elsewhere in the same zone has
// somewhere to find the resolved pool ref once this declaring occurrence
// finishes widening -- not just a material texture-def slot's own
// position, which already carries this without needing a extra
// reservation). Aligns to 4 bytes, reserves 4 bytes at the aligned
// position, and returns that offset in `outOffset`. Callers must still
// call FS_RetailFastfileRegisterImagePoolSlot(reader, resolvedNameRef,
// *outOffset, &poolRef) once the inline body is widened, to actually pool
// it and patch this reserved slot.
FsRetailFastfileWireResult __cdecl FS_RetailFastfileReserveBlock4Slot(
    FsRetailFastfileReader *reader,
    uint32_t *outOffset);
// Decodes one inline 12-byte serialized MenuList body: inline name, then the
// menuCount-sized menu pointer array (menuRefs receives the raw asset-style
// slots).  Menu bodies are walked separately, in slot order, through
// FS_ReadRetailFastfileMenuPrefix so the shared stream cursor stays exact.
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileMenuList(
    FsRetailFastfileReader *reader,
    uint32_t headerRef,
    FsRetailFastfileMenuList *menuList,
    uint32_t *menuRefs,
    uint32_t menuRefCapacity);
// Decodes one inline 284-byte serialized menuDef_t body in field order:
// window strings, menu strings, the ItemKeyHandler chain (handlers), the
// visibleExp/rectXExp/rectYExp entries (entries, filled sequentially), and
// finally the items pointer array.  The walk stops right after that array:
// item bodies are the next seam.  An inline/insert window background
// material is an unsupported form in this slice and fails loudly; null and
// reference backgrounds stay encoded.
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileMenuPrefix(
    FsRetailFastfileReader *reader,
    uint32_t menuRef,
    FsRetailFastfileMenu *menu,
    FsRetailFastfileItemKeyHandler *handlers,
    uint32_t handlerCapacity,
    FsRetailFastfileExpressionEntry *entries,
    uint32_t entryCapacity);
// Decodes one inline 372-byte serialized itemDef_t body.  This is the
// continuation immediately after a menu's item pointer array; unsupported
// inline material/background forms fail loudly rather than guessing.
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileItemDef(
    FsRetailFastfileReader *reader, uint32_t itemRef,
    FsRetailFastfileItemDef *item,
    FsRetailFastfileItemKeyHandler *handlers, uint32_t handlerCapacity,
    FsRetailFastfileExpressionEntry *entries, uint32_t entryCapacity);
FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileBlock(
    const FsRetailFastfileReader *reader,
    uint32_t block,
    uint32_t offset,
    uint8_t *buffer,
    uint32_t bytes);
void __cdecl FS_CloseRetailFastfile(FsRetailFastfileReader *reader);
