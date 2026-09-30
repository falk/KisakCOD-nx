// Retail fastfile reader: split out of com_files.cpp (seam 9). See
// db_retail_fastfile.h for why this lives under src/database/retail/.

#include "db_retail_fastfile.h"

#include <universal/q_shared.h>
#include <universal/com_files.h>
#include <universal/com_memory.h>
#include <universal/retail_asset_trace.h>
#include <qcommon/com_fileaccess.h>
#include <qcommon/qcommon.h>
#include <qcommon/files.h>
#include <zlib/zlib.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace
{
// Flash-era compressed-input window. The 4KB value is an HDD-era choice
// (small reads avoided disturbing readahead); a level load pulls ~70-135MB
// of deflate through this buffer, so 256KB cuts the syscall count by ~64x
// and lets the SD/eMMC layer do full-size sequential transfers.
const uint32_t RETAIL_FASTFILE_INPUT_BYTES = 256 * 1024;
const uint32_t RETAIL_FASTFILE_XFILE_HEADER_BYTES = 44;
const uint64_t RETAIL_FASTFILE_BLOCK_CAP = 768ull * 1024 * 1024;
const uint32_t RETAIL_FASTFILE_ASSET_COUNT_CAP = 1u << 20;
const uint32_t RETAIL_FASTFILE_INLINE_REF = 0xffffffffu;
const uint32_t RETAIL_FASTFILE_INSERT_REF = 0xfffffffeu;

uint32_t ReadRetailFastfileLe32(const uint8_t *data)
{
    return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

float ReadRetailFastfileLeFloat(const uint8_t *data)
{
    const uint32_t bits = ReadRetailFastfileLe32(data);
    float value;
    Com_Memcpy(&value, &bits, sizeof(value));
    return value;
}
}

struct FsRetailFastfileReader
{
    int file;
    z_stream stream;
    uint8_t input[RETAIL_FASTFILE_INPUT_BYTES];
    uint32_t xfileSize;
    uint32_t xfileExternalSize;
    uint32_t blockSizes[9];
    uint64_t blockTotal;
    uint64_t wireBytes;
    // On-disk (compressed) size of the fastfile and the compressed bytes
    // consumed so far: the retail loadbar's internal measure (see
    // FS_GetRetailLoadProgress).
    uint64_t fileBytes;
    uint64_t fileBytesRead;
    uint8_t *blockData[9];
    uint32_t blockCursor[9];
    // Pooled-image identity (the Android loader's DB_AddXAsset pseudo-block
    // 15): every inline image load appends the block-4 reference of its
    // decoded name; texture-def image slots and insert slots are overwritten
    // with the pooled reference so later alias references can copy it.
    uint32_t imagePoolCount;
    uint32_t imagePoolNameRefs[RETAIL_FASTFILE_IMAGE_POOL_MAX];
    int zlibResult;
    bool sourceExhausted;
    // A string scan may read a bounded chunk and hand the tail back: the
    // inflate stream is forward-only, so unread bytes are queued here and
    // drained by the next FS_ReadRetailFastfile call. At most one chunk's
    // tail (see ReadInlineRetailStringInBlock) is ever pending.
    uint8_t pushback[512];
    uint32_t pushbackLen;
    uint32_t pushbackPos;
    // Quake-style loading readout (see FS_GetRetailLoadStatus): basename of
    // the file being streamed plus the last published percent bucket.
    char loadLabel[64];
    uint32_t loadPercentShown;
};

// Loading-status cell shared with the frame path. Written only by the
// database thread (one zone walks at a time); read by the main/render
// thread. The numeric fields are atomic so a reader cannot miss the active
// transition or observe a torn progress value. The filename is published
// before active=1 and remains stable for the lifetime of that publication.
static char g_retailLoadFile[64];
static std::atomic<uint32_t> g_retailLoadPercent{0};
static std::atomic<uint32_t> g_retailLoadActive{0};
// Retail loadbar counters (retail DB_LoadXFileInternal sizes the bar from the
// zone file's on-disk size + XFile.externalSize and DB_ReadXFile counts the
// compressed chunks read).  Sized when a walk opens a fastfile, advanced as
// compressed input is consumed, and *held* when the walk closes, so the bar
// keeps its fill for the rest of the mission load; cleared only by
// FS_ResetRetailLoadProgress (DB_ResetZoneSize), so a new load starts empty
// instead of showing the previous zone's full bar.
static std::atomic<uint64_t> g_retailLoadFileBytes{0};
static std::atomic<uint64_t> g_retailLoadFileRead{0};
static std::atomic<uint32_t> g_retailLoadExternalBytes{0};
static std::atomic<uint32_t> g_retailLoadSized{0};

static void RetailLoadPublish(const char *file, uint32_t percent, uint32_t active)
{
    if (file)
        I_strncpyz(g_retailLoadFile, file, sizeof(g_retailLoadFile));
    g_retailLoadPercent.store(percent, std::memory_order_release);
    g_retailLoadActive.store(active, std::memory_order_release);
}

static void RetailLoadPublishBytes(const FsRetailFastfileReader *reader)
{
    g_retailLoadFileRead.store(reader->fileBytesRead, std::memory_order_release);
}

void __cdecl FS_ResetRetailLoadProgress()
{
    g_retailLoadSized.store(0, std::memory_order_release);
    g_retailLoadFileBytes.store(0, std::memory_order_release);
    g_retailLoadFileRead.store(0, std::memory_order_release);
    g_retailLoadExternalBytes.store(0, std::memory_order_release);
}

bool __cdecl FS_GetRetailLoadProgress(uint64_t *readBytes, uint64_t *fileBytes, uint32_t *externalBytes)
{
    if (!g_retailLoadSized.load(std::memory_order_acquire))
        return false;
    if (readBytes)
        *readBytes = g_retailLoadFileRead.load(std::memory_order_acquire);
    if (fileBytes)
        *fileBytes = g_retailLoadFileBytes.load(std::memory_order_acquire);
    if (externalBytes)
        *externalBytes = g_retailLoadExternalBytes.load(std::memory_order_acquire);
    return true;
}

bool __cdecl FS_GetRetailLoadStatus(char *fileOut, uint32_t fileOutSize, uint32_t *percentOut)
{
    if (!g_retailLoadActive.load(std::memory_order_acquire))
        return false;
    if (fileOut && fileOutSize)
        I_strncpyz(fileOut, g_retailLoadFile, fileOutSize);
    if (percentOut)
        *percentOut = g_retailLoadPercent.load(std::memory_order_acquire);
    return true;
}

static FsRetailFastfileWireResult ReadRetailFastfileInlineName(
    FsRetailFastfileReader *reader, uint32_t *ref);
static bool ReadRetailFastfileBlock4CString(FsRetailFastfileReader *reader);

// Writes a 32-bit little-endian value into block memory (the loader's
// WriteSlot: pointer slots are fixed up in place, never reinterpreted as
// host pointers).
static void RetailFastfileWriteSlot(
    FsRetailFastfileReader *reader, uint32_t block, uint32_t offset, uint32_t value)
{
    reader->blockData[block][offset] = static_cast<uint8_t>(value);
    reader->blockData[block][offset + 1] = static_cast<uint8_t>(value >> 8);
    reader->blockData[block][offset + 2] = static_cast<uint8_t>(value >> 16);
    reader->blockData[block][offset + 3] = static_cast<uint8_t>(value >> 24);
}

// Streams one NUL-terminated string into block 4 at its cursor and leaves
// the cursor just past the NUL. Chunked like the walk's
// ReadInlineRetailStringInBlock: one inflate call per 64 bytes instead of one
// per character, with the look-ahead past the NUL handed back through
// FS_RetailFastfileUnread. Only the string's own bytes are written to the
// block, so nothing at or past the cursor is touched. Fails (cursor at the
// block end or the stream end) exactly where the byte-at-a-time loop did:
// no NUL before the block is full, or the stream ends first.
static bool ReadRetailFastfileBlock4CString(FsRetailFastfileReader *reader)
{
    constexpr uint32_t kChunkBytes = 64;
    uint8_t chunk[kChunkBytes];
    for (;;)
    {
        const uint32_t cursor = reader->blockCursor[4];
        if (cursor >= reader->blockSizes[4])
            return false;
        const uint32_t room = reader->blockSizes[4] - cursor;
        const uint32_t want = room < kChunkBytes ? room : kChunkBytes;
        const uint32_t got = FS_ReadRetailFastfile(reader, chunk, want);
        if (!got)
            return false;
        const uint8_t *nul = static_cast<const uint8_t *>(memchr(chunk, 0, got));
        const uint32_t take = nul ? static_cast<uint32_t>(nul - chunk) + 1u : got;
        Com_Memcpy(reader->blockData[4] + cursor, chunk, take);
        reader->blockCursor[4] = cursor + take;
        if (nul)
        {
            if (take < got)
                FS_RetailFastfileUnread(reader, chunk + take, got - take);
            return true;
        }
    }
}

uint32_t __cdecl FS_ReadRetailFastfile(FsRetailFastfileReader *reader, uint8_t *buffer, uint32_t len)
{
    uint32_t read = 0;
    // Serve any bytes a string scan pushed back before touching the inflate
    // stream. FS_RetailFastfileUnread un-counted them from wireBytes, so every
    // byte handed to a caller is counted exactly once, when it is consumed.
    if (reader->pushbackPos < reader->pushbackLen)
    {
        const uint32_t queued = reader->pushbackLen - reader->pushbackPos;
        const uint32_t take = queued < len ? queued : len;
        Com_Memcpy(buffer, reader->pushback + reader->pushbackPos, take);
        reader->pushbackPos += take;
        read = take;
        if (reader->pushbackPos >= reader->pushbackLen)
        {
            reader->pushbackLen = 0;
            reader->pushbackPos = 0;
        }
    }
    while (read < len)
    {
        if (!reader->stream.avail_in)
        {
            const uint32_t inputBytes = FS_Read(reader->input, sizeof(reader->input), reader->file);
            if (!inputBytes || inputBytes == static_cast<uint32_t>(-1))
            {
                reader->sourceExhausted = true;
                break;
            }
            reader->stream.next_in = reader->input;
            reader->stream.avail_in = inputBytes;
            reader->fileBytesRead += inputBytes;
            if (reader->fileBytes)
                RetailLoadPublishBytes(reader);
        }

        reader->stream.next_out = buffer + read;
        reader->stream.avail_out = len - read;
        const int result = inflate(&reader->stream, Z_NO_FLUSH);
        reader->zlibResult = result;
        read = len - reader->stream.avail_out;
        if (result != Z_OK && result != Z_STREAM_END)
            break;
        if (result == Z_STREAM_END)
            break;
    }
    reader->wireBytes += read;
    if (reader->xfileSize && reader->loadLabel[0])
    {
        uint32_t percent = static_cast<uint32_t>(reader->wireBytes * 100u / reader->xfileSize);
        if (percent > 100u)
            percent = 100u;
        if (percent != reader->loadPercentShown)
        {
            reader->loadPercentShown = percent;
            RetailLoadPublish(reader->loadLabel, percent, 1);
        }
    }
    return read;
}

void __cdecl FS_RetailFastfileUnread(FsRetailFastfileReader *reader, const uint8_t *bytes,
                                     uint32_t len)
{
    if (!reader || !bytes || !len)
        return;
    // Queue, never replace: a read can stop mid-pushback (a chunk window
    // smaller than the pending run, common near the stream end), so the new
    // tail must land *before* whatever is still pending. Dropping the
    // pending bytes would lose stream data and desynchronize the walk.
    const uint32_t pending = reader->pushbackLen - reader->pushbackPos;
    if (len > sizeof(reader->pushback) - pending)
    {
        // Unreachable with the string reader's 64-byte chunks (pending is at
        // most one chunk tail); a larger request is a programming error and
        // must not silently corrupt the stream.
        Com_Error(ERR_FATAL, "FS_RetailFastfileUnread pushback overflow (%u + %u)",
                  len, pending);
        return;
    }
    if (pending)
        memmove(reader->pushback + len, reader->pushback + reader->pushbackPos, pending);
    Com_Memcpy(reader->pushback, bytes, len);
    reader->pushbackLen = len + pending;
    reader->pushbackPos = 0;
    // The bytes go back to the stream: wireBytes is the consumed position, so
    // bounds checks against the xfile size (and the walk's string-scan stream
    // room) never see a chunk's look-ahead as consumed.
    reader->wireBytes -= len;
}

uint32_t __cdecl FS_RetailFastfileXFileSize(const FsRetailFastfileReader *reader)
{
    return reader ? reader->xfileSize : 0;
}

// TEMPORARY diagnosis: decompressed stream bytes consumed so far.
uint64_t __cdecl FS_RetailFastfileWireBytes(const FsRetailFastfileReader *reader)
{
    return reader ? reader->wireBytes : 0;
}

const char *__cdecl FS_RetailFastfileLoadLabel(const FsRetailFastfileReader *reader)
{
    return reader ? reader->loadLabel : "";
}

uint32_t __cdecl FS_RetailFastfileXFileExternalSize(const FsRetailFastfileReader *reader)
{
    return reader ? reader->xfileExternalSize : 0;
}

uint32_t __cdecl FS_RetailFastfileBlockSize(const FsRetailFastfileReader *reader, uint32_t block)
{
    return reader && block < 9 ? reader->blockSizes[block] : 0;
}

const uint8_t *__cdecl FS_RetailFastfileBlockData(const FsRetailFastfileReader *reader, uint32_t block)
{
    return reader && block < 9 ? reader->blockData[block] : nullptr;
}

uint8_t *__cdecl FS_RetailFastfileBlockDataMutable(FsRetailFastfileReader *reader, uint32_t block)
{
    return reader && block < 9 ? reader->blockData[block] : nullptr;
}

#ifdef KISAK_RETAIL_FS_PROOF_HOST
// retained-pointer generalization: the set of readers whose transient blocks are
// still live.  FS_OpenRetailFastfile adds, FS_CloseRetailFastfile removes
// (before the Z_Free).  Host proof links use this to audit that no
// registered asset retained a reader-owned pointer -- the exact contract
// documented on FS_RetailFastfileBlockData.  Never compiled into Switch.
namespace
{
constexpr uint32_t kRetailFastfileTrackedReaderCap = 64;
const FsRetailFastfileReader *g_retailFastfileTrackedReaders[kRetailFastfileTrackedReaderCap];
uint32_t g_retailFastfileTrackedReaderCount = 0;
// A cap overflow would make the audit blind, so it must fail loudly: the
// query below then reports every pointer as transient.
bool g_retailFastfileTrackedReaderOverflow = false;

void RetailFastfileTrackReader(const FsRetailFastfileReader *reader)
{
    if (!reader)
        return;
    if (g_retailFastfileTrackedReaderCount >= kRetailFastfileTrackedReaderCap)
    {
        if (!g_retailFastfileTrackedReaderOverflow)
            Com_Printf(0, "FS_RetailFastfileTrackReader: tracking table full (%u); "
                          "reader-provenance audit will fail loudly\n",
                       kRetailFastfileTrackedReaderCap);
        g_retailFastfileTrackedReaderOverflow = true;
        return;
    }
    g_retailFastfileTrackedReaders[g_retailFastfileTrackedReaderCount++] = reader;
}

void RetailFastfileUntrackReader(const FsRetailFastfileReader *reader)
{
    for (uint32_t i = 0; i < g_retailFastfileTrackedReaderCount; ++i)
    {
        if (g_retailFastfileTrackedReaders[i] == reader)
        {
            g_retailFastfileTrackedReaders[i] =
                g_retailFastfileTrackedReaders[--g_retailFastfileTrackedReaderCount];
            return;
        }
    }
}

// sweep #3 poison accounting (host proof links only).
uint32_t g_retailFastfilePoisonBlocks = 0;
uint64_t g_retailFastfilePoisonBytes = 0;
uint32_t g_retailFastfilePoisonFailures = 0;
// pointer-provenance generalization: the close-time audit hook (null
// unless a host proof installs one).
FsRetailFastfileTransientAuditHook g_retailFastfileTransientAuditHook = nullptr;
} // namespace

void __cdecl FS_RetailFastfileSetTransientAuditHook(FsRetailFastfileTransientAuditHook hook)
{
    g_retailFastfileTransientAuditHook = hook;
}

bool __cdecl FS_RetailFastfilePointerIsReaderTransient(const void *pointer)
{
    if (g_retailFastfileTrackedReaderOverflow)
        return true;
    if (!pointer)
        return false;
    const uintptr_t value = reinterpret_cast<uintptr_t>(pointer);
    for (uint32_t i = 0; i < g_retailFastfileTrackedReaderCount; ++i)
    {
        const FsRetailFastfileReader *reader = g_retailFastfileTrackedReaders[i];
        for (uint32_t block = 0; block < 9; ++block)
        {
            const uint8_t *base = reader->blockData[block];
            const uint32_t size = reader->blockSizes[block];
            if (!base || !size)
                continue;
            const uintptr_t start = reinterpret_cast<uintptr_t>(base);
            if (value >= start && value < start + size)
                return true;
        }
    }
    return false;
}

uint32_t __cdecl FS_RetailFastfileTrackedReaderCount(void)
{
    return g_retailFastfileTrackedReaderCount;
}

void __cdecl FS_RetailFastfilePoisonStats(uint32_t *blocks, uint64_t *bytes,
                                          uint32_t *failures)
{
    if (blocks)
        *blocks = g_retailFastfilePoisonBlocks;
    if (bytes)
        *bytes = g_retailFastfilePoisonBytes;
    if (failures)
        *failures = g_retailFastfilePoisonFailures;
}
#endif

uint32_t __cdecl FS_RetailFastfileBlockCursor(const FsRetailFastfileReader *reader, uint32_t block)
{
    return reader && block < 9 ? reader->blockCursor[block] : 0;
}

void __cdecl FS_RetailFastfileSetBlockCursor(FsRetailFastfileReader *reader, uint32_t block, uint32_t cursor)
{
    if (reader && block < 9)
        reader->blockCursor[block] = cursor;
}

uint64_t __cdecl FS_RetailFastfileBlockTotal(const FsRetailFastfileReader *reader)
{
    return reader ? reader->blockTotal : 0;
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileAssetList(
    FsRetailFastfileReader *reader,
    FsRetailFastfileAssetList *list,
    FsRetailFastfileAsset *assets,
    uint32_t assetCapacity,
    uint32_t *scriptStringRefs,
    uint32_t scriptStringCapacity)
{
    uint8_t wireList[16];
    if (!reader || !list)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    Com_Memset(list, 0, sizeof(*list));
    if (FS_ReadRetailFastfile(reader, wireList, sizeof(wireList)) != sizeof(wireList))
        return FS_RETAIL_FF_WIRE_LIST_TRUNCATED;

    list->scriptStringCount = ReadRetailFastfileLe32(wireList);
    list->scriptStringsRef = ReadRetailFastfileLe32(wireList + 4);
    list->assetCount = ReadRetailFastfileLe32(wireList + 8);
    list->assetsRef = ReadRetailFastfileLe32(wireList + 12);
    if (list->scriptStringCount > RETAIL_FASTFILE_ASSET_COUNT_CAP)
        return FS_RETAIL_FF_WIRE_ASSET_COUNT_OVER_CAP;
    if (list->scriptStringCount)
    {
        if (list->scriptStringsRef != RETAIL_FASTFILE_INLINE_REF)
            return FS_RETAIL_FF_WIRE_ASSETS_NOT_INLINE;
        if (!scriptStringRefs || scriptStringCapacity < list->scriptStringCount)
            return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
        reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
        const uint64_t refsBytes = static_cast<uint64_t>(list->scriptStringCount) * 4;
        if (refsBytes > 0xffffffffu || reader->blockCursor[4] + refsBytes > reader->blockSizes[4])
            return FS_RETAIL_FF_WIRE_DIRECTORY_TRUNCATED;
        uint8_t *refTable = reader->blockData[4] + reader->blockCursor[4];
        if (FS_ReadRetailFastfile(reader, refTable, static_cast<uint32_t>(refsBytes)) != refsBytes)
            return FS_RETAIL_FF_WIRE_LIST_TRUNCATED;
        const uint32_t refsOffset = reader->blockCursor[4];
        reader->blockCursor[4] = refsOffset + static_cast<uint32_t>(refsBytes);
        for (uint32_t i = 0; i < list->scriptStringCount; ++i)
        {
            const uint32_t slotOffset = refsOffset + i * 4;
            const uint32_t ref = ReadRetailFastfileLe32(reader->blockData[4] + slotOffset);
            if (ref != RETAIL_FASTFILE_INLINE_REF)
                scriptStringRefs[i] = ref;
            else
            {
                const uint32_t stringOffset = reader->blockCursor[4];
                if (!ReadRetailFastfileBlock4CString(reader))
                    return FS_RETAIL_FF_WIRE_LIST_TRUNCATED;
                scriptStringRefs[i] = ((4u << 28) | stringOffset) + 1;
            }
        }
    }
    if (list->assetCount > RETAIL_FASTFILE_ASSET_COUNT_CAP)
        return FS_RETAIL_FF_WIRE_ASSET_COUNT_OVER_CAP;
    if (!list->assetCount)
        return FS_RETAIL_FF_WIRE_OK;
    if (list->assetsRef != RETAIL_FASTFILE_INLINE_REF)
        return FS_RETAIL_FF_WIRE_ASSETS_NOT_INLINE;
    if (!assets || assetCapacity < list->assetCount)
        return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;

    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
    const uint64_t directoryBytes = static_cast<uint64_t>(list->assetCount) * 8;
    if (directoryBytes > 0xffffffffu ||
        (reader->xfileSize && reader->wireBytes - RETAIL_FASTFILE_XFILE_HEADER_BYTES + directoryBytes > reader->xfileSize) ||
        reader->blockCursor[4] + directoryBytes > reader->blockSizes[4])
        return FS_RETAIL_FF_WIRE_DIRECTORY_TRUNCATED;
    // One read for the whole directory, straight into its block-4 slot (the
    // bounds above cover it), instead of one 8-byte inflate call per entry.
    // decodedCount still counts only complete entries on a short stream.
    uint8_t *directory = reader->blockData[4] + reader->blockCursor[4];
    const uint32_t got = FS_ReadRetailFastfile(reader, directory, static_cast<uint32_t>(directoryBytes));
    const uint32_t complete = got / 8u;
    for (uint32_t i = 0; i < complete; ++i)
    {
        assets[i].type = ReadRetailFastfileLe32(directory + i * 8u);
        assets[i].header = ReadRetailFastfileLe32(directory + i * 8u + 4u);
    }
    reader->blockCursor[4] += complete * 8u;
    list->decodedCount = complete;
    return got == directoryBytes ? FS_RETAIL_FF_WIRE_OK : FS_RETAIL_FF_WIRE_DIRECTORY_TRUNCATED;
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileRawFile(
    FsRetailFastfileReader *reader, uint32_t headerRef, FsRetailFastfileRawFile *rawfile)
{
    uint8_t wireRawFile[12];
    if (!reader || !rawfile)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    Com_Memset(rawfile, 0, sizeof(*rawfile));
    rawfile->headerRef = headerRef;
    if (!headerRef)
        return FS_RETAIL_FF_WIRE_OK;
    if (headerRef != RETAIL_FASTFILE_INLINE_REF)
        return FS_RETAIL_FF_WIRE_BODY_NOT_INLINE;

    const uint32_t tempStart = reader->blockCursor[0];
    reader->blockCursor[0] = (reader->blockCursor[0] + 3u) & ~3u;
    if (reader->blockCursor[0] + sizeof(wireRawFile) > reader->blockSizes[0] ||
        FS_ReadRetailFastfile(reader, wireRawFile, sizeof(wireRawFile)) != sizeof(wireRawFile))
    {
        reader->blockCursor[0] = tempStart;
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    }
    Com_Memcpy(reader->blockData[0] + reader->blockCursor[0], wireRawFile, sizeof(wireRawFile));
    // Block 0 is the engine's temporary asset-body block.  The enclosing
    // Load_XAssetHeader push/pop rewinds it after every asset; retain that
    // behavior even though this bounded reader exposes each body separately.
    reader->blockCursor[0] = tempStart;
    rawfile->nameRef = ReadRetailFastfileLe32(wireRawFile);
    rawfile->len = static_cast<int32_t>(ReadRetailFastfileLe32(wireRawFile + 4));
    rawfile->bufferRef = ReadRetailFastfileLe32(wireRawFile + 8);

    if (rawfile->nameRef)
    {
        if (rawfile->nameRef != RETAIL_FASTFILE_INLINE_REF)
            return FS_RETAIL_FF_WIRE_BODY_NOT_INLINE;
        const uint32_t nameOffset = reader->blockCursor[4];
        if (!ReadRetailFastfileBlock4CString(reader))
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        rawfile->nameRef = ((4u << 28) | nameOffset) + 1;
    }
    if (!rawfile->bufferRef)
        return FS_RETAIL_FF_WIRE_OK;
    if (rawfile->len < 0)
        return FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH;
    const uint64_t bytes = static_cast<uint64_t>(rawfile->len) + 1;
    if (bytes > 0xffffffffu || reader->blockCursor[4] + bytes > reader->blockSizes[4] ||
        FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4], static_cast<uint32_t>(bytes)) != bytes)
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    const uint32_t bufferOffset = reader->blockCursor[4];
    reader->blockCursor[4] += static_cast<uint32_t>(bytes);
    rawfile->bufferRef = ((4u << 28) | bufferOffset) + 1;
    return FS_RETAIL_FF_WIRE_OK;
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileTechniqueSetPrefix(
    FsRetailFastfileReader *reader, uint32_t headerRef, FsRetailFastfileTechniqueSet *techniqueSet)
{
    uint8_t wireTechniqueSet[148];
    if (!reader || !techniqueSet)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    Com_Memset(techniqueSet, 0, sizeof(*techniqueSet));
    techniqueSet->headerRef = headerRef;
    if (headerRef != RETAIL_FASTFILE_INLINE_REF)
        return FS_RETAIL_FF_WIRE_BODY_NOT_INLINE;
    const uint32_t tempStart = reader->blockCursor[0];
    reader->blockCursor[0] = (reader->blockCursor[0] + 3u) & ~3u;
    if (reader->blockCursor[0] + sizeof(wireTechniqueSet) > reader->blockSizes[0] ||
        FS_ReadRetailFastfile(reader, wireTechniqueSet, sizeof(wireTechniqueSet)) != sizeof(wireTechniqueSet))
    {
        reader->blockCursor[0] = tempStart;
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    }
    Com_Memcpy(reader->blockData[0] + reader->blockCursor[0], wireTechniqueSet, sizeof(wireTechniqueSet));
    reader->blockCursor[0] = tempStart;
    techniqueSet->nameRef = ReadRetailFastfileLe32(wireTechniqueSet);
    techniqueSet->nameWasInline = (techniqueSet->nameRef == RETAIL_FASTFILE_INLINE_REF);
    techniqueSet->worldVertFormat = wireTechniqueSet[4];
    techniqueSet->hasBeenUploaded = wireTechniqueSet[5];
    techniqueSet->unused = wireTechniqueSet[6];
    techniqueSet->remappedTechniqueSetRef = ReadRetailFastfileLe32(wireTechniqueSet + 8);
    for (uint32_t i = 0; i < 34; ++i)
        techniqueSet->techniqueRefs[i] = ReadRetailFastfileLe32(wireTechniqueSet + 12 + i * 4);
    return ReadRetailFastfileInlineName(reader, &techniqueSet->nameRef);
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileMaterialTechniquePrefix(
    FsRetailFastfileReader *reader, uint32_t headerRef, FsRetailFastfileMaterialTechnique *technique)
{
    uint8_t wireTechnique[8];
    if (!reader || !technique)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    Com_Memset(technique, 0, sizeof(*technique));
    technique->headerRef = headerRef;
    if (headerRef != RETAIL_FASTFILE_INLINE_REF)
        return FS_RETAIL_FF_WIRE_BODY_NOT_INLINE;
    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
    if (reader->blockCursor[4] + sizeof(wireTechnique) > reader->blockSizes[4] ||
        FS_ReadRetailFastfile(reader, wireTechnique, sizeof(wireTechnique)) != sizeof(wireTechnique))
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    Com_Memcpy(reader->blockData[4] + reader->blockCursor[4], wireTechnique, sizeof(wireTechnique));
    reader->blockCursor[4] += sizeof(wireTechnique);
    technique->nameRef = ReadRetailFastfileLe32(wireTechnique);
    technique->flags = static_cast<uint16_t>(wireTechnique[4] | wireTechnique[5] << 8);
    technique->passCount = static_cast<uint16_t>(wireTechnique[6] | wireTechnique[7] << 8);
    if (technique->passCount > 64)
        return FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH;
    const uint32_t bytes = technique->passCount * 20u;
    if (reader->blockCursor[4] + bytes > reader->blockSizes[4] ||
        FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4], bytes) != bytes)
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    technique->passOffset = reader->blockCursor[4];
    reader->blockCursor[4] += bytes;
    return FS_RETAIL_FF_WIRE_OK;
}

static FsRetailFastfileWireResult ReadRetailFastfileInlineName(
    FsRetailFastfileReader *reader, uint32_t *ref)
{
    if (!*ref)
        return FS_RETAIL_FF_WIRE_OK;
    if (*ref != RETAIL_FASTFILE_INLINE_REF)
    {
        // Shared string: the slot is an encoded pointer to an already
        // decoded name (DB_ConvertOffsetToPointer); no stream bytes belong
        // to it and the reference resolves through block 4.
        return FS_RETAIL_FF_WIRE_OK;
    }
    const uint32_t offset = reader->blockCursor[4];
    if (!ReadRetailFastfileBlock4CString(reader))
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    *ref = ((4u << 28) | offset) + 1;
    return FS_RETAIL_FF_WIRE_OK;
}

static FsRetailFastfileWireResult ReadRetailFastfileMaterialShader(
    FsRetailFastfileReader *reader, uint32_t ref,
    FsRetailFastfileMaterialShader *shader)
{
    uint8_t wire[16];
    Com_Memset(shader, 0, sizeof(*shader));
    shader->headerRef = ref;
    if (!ref)
        return FS_RETAIL_FF_WIRE_OK;
    if (ref != RETAIL_FASTFILE_INLINE_REF)
    {
        // Shared shader body: the slot is an encoded pointer to an already
        // decoded shader struct (DB_ConvertOffsetToPointer).  It consumes no
        // stream bytes; the record keeps the reference for later widening.
        shader->nameRef = ref;
        return FS_RETAIL_FF_WIRE_OK;
    }
    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
    if (reader->blockCursor[4] + sizeof(wire) > reader->blockSizes[4] ||
        FS_ReadRetailFastfile(reader, wire, sizeof(wire)) != sizeof(wire))
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    shader->recordOffset = reader->blockCursor[4];
    Com_Memcpy(reader->blockData[4] + reader->blockCursor[4], wire, sizeof(wire));
    reader->blockCursor[4] += sizeof(wire);
    shader->nameRef = ReadRetailFastfileLe32(wire);
    shader->programRef = ReadRetailFastfileLe32(wire + 8);
    shader->programSize = static_cast<uint16_t>(wire[12] | wire[13] << 8);
    shader->loadForRenderer = static_cast<uint16_t>(wire[14] | wire[15] << 8);
    FsRetailFastfileWireResult result = ReadRetailFastfileInlineName(reader, &shader->nameRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    if (!shader->programRef)
        return FS_RETAIL_FF_WIRE_OK;
    // The engine reads the program bytes for any non-zero program slot
    // (Load_GfxVertexShaderLoadDef allocates unconditionally); the stored
    // value carries no reference meaning.
    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
    const uint64_t bytes = static_cast<uint64_t>(shader->programSize) * 4;
    if (reader->blockCursor[4] + bytes > reader->blockSizes[4] ||
        FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4],
                              static_cast<uint32_t>(bytes)) != bytes)
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    shader->programOffset = reader->blockCursor[4];
    reader->blockCursor[4] += static_cast<uint32_t>(bytes);
    return FS_RETAIL_FF_WIRE_OK;
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileMaterialTechnique(
    FsRetailFastfileReader *reader, FsRetailFastfileMaterialTechnique *technique,
    FsRetailFastfileMaterialPass *passes, uint32_t passCapacity,
    FsRetailFastfileMaterialShaderArgument *args, uint32_t argCapacity)
{
    if (!reader || !technique)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    if (technique->passCount && (!passes || passCapacity < technique->passCount))
        return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
    const uint64_t passBytes = static_cast<uint64_t>(technique->passCount) * 20u;
    if (static_cast<uint64_t>(technique->passOffset) + passBytes > reader->blockSizes[4])
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    uint32_t argTotal = 0;
    for (uint32_t i = 0; i < technique->passCount; ++i)
    {
        const uint8_t *wire = reader->blockData[4] + technique->passOffset + i * 20u;
        const uint32_t count = ReadRetailFastfileLe32(wire + 16)
            ? wire[12] + wire[13] + wire[14]
            : 0;
        if (argTotal > 0xffffffffu - count)
            return FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH;
        argTotal += count;
    }
    if (argTotal && (!args || argCapacity < argTotal))
        return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
    // The prefix already consumed the pass array from the stream into block 4
    // (blockCursor[4] sits right after it, where the nested inline data
    // follows), so the records are read from the block copy - never from the
    // stream again.
    uint32_t argIndex = 0;
    for (uint32_t i = 0; i < technique->passCount; ++i)
    {
        const uint8_t *record = reader->blockData[4] + technique->passOffset + i * 20u;
        uint8_t wire[20];
        Com_Memcpy(wire, record, sizeof(wire));
        FsRetailFastfileMaterialPass *pass = &passes[i];
        Com_Memset(pass, 0, sizeof(*pass));
        pass->vertexDeclRef = ReadRetailFastfileLe32(wire);
        pass->argsRef = ReadRetailFastfileLe32(wire + 16);
        pass->perPrimArgCount = wire[12]; pass->perObjArgCount = wire[13];
        pass->stableArgCount = wire[14]; pass->customSamplerFlags = wire[15];
        pass->argCount = pass->perPrimArgCount + pass->perObjArgCount + pass->stableArgCount;
        if (pass->vertexDeclRef == RETAIL_FASTFILE_INLINE_REF)
        {
            reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
            if (reader->blockCursor[4] + 100u > reader->blockSizes[4] ||
                FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4], 100) != 100)
                return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
            pass->vertexDeclOffset = reader->blockCursor[4];
            reader->blockCursor[4] += 100;
        }
        FsRetailFastfileWireResult result = ReadRetailFastfileMaterialShader(
            reader, ReadRetailFastfileLe32(wire + 4), &pass->vertexShader);
        if (result != FS_RETAIL_FF_WIRE_OK) return result;
        result = ReadRetailFastfileMaterialShader(reader, ReadRetailFastfileLe32(wire + 8), &pass->pixelShader);
        if (result != FS_RETAIL_FF_WIRE_OK) return result;
        if (pass->argsRef)
        {
            // The engine treats any non-zero args slot as an inline argument
            // array (Load_MaterialPass allocates unconditionally); the slot
            // value itself carries no meaning.
            reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
            pass->argsOffset = reader->blockCursor[4];
            for (uint32_t a = 0; a < pass->argCount; ++a)
            {
                // Mirror every 8-byte arg record into block 4 like the
                // linker does (Android AllocStreamPos): argument records
                // are stream bytes like any other, and later absolute
                // tokens (technique aliases, pooled names) address them.
                // A stack-only copy silently shifts every subsequent
                // mirror offset (caught live: intra-set aliases missing
                // after arg-bearing techniques).
                if (reader->blockCursor[4] + 8u > reader->blockSizes[4] ||
                    FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4],
                                          8) != 8)
                    return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
                const uint8_t *raw = reader->blockData[4] + reader->blockCursor[4];
                reader->blockCursor[4] += 8;
                FsRetailFastfileMaterialShaderArgument *arg = &args[argIndex++];
                arg->type = static_cast<uint16_t>(raw[0] | raw[1] << 8);
                arg->dest = static_cast<uint16_t>(raw[2] | raw[3] << 8);
                arg->valueRef = ReadRetailFastfileLe32(raw + 4);
                arg->literalOffset = 0;
                if ((arg->type == 1 || arg->type == 7) && arg->valueRef == RETAIL_FASTFILE_INLINE_REF)
                {
                    // Only the inline token streams a literal float[4]; a
                    // null slot streams nothing and any other value is an
                    // encoded pointer to a shared constant (no stream bytes).
                    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
                    if (reader->blockCursor[4] + 16u > reader->blockSizes[4] ||
                        FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4], 16) != 16)
                        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
                    arg->literalOffset = reader->blockCursor[4];
                    reader->blockCursor[4] += 16;
                }
            }
        }
    }
    return ReadRetailFastfileInlineName(reader, &technique->nameRef);
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileBlock(
    const FsRetailFastfileReader *reader, uint32_t block, uint32_t offset,
    uint8_t *buffer, uint32_t bytes)
{
    if (!reader || block >= 9 || (!buffer && bytes))
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    if (static_cast<uint64_t>(offset) + bytes > reader->blockSizes[block])
        return FS_RETAIL_FF_WIRE_DIRECTORY_TRUNCATED;
    if (bytes)
        Com_Memcpy(buffer, reader->blockData[block] + offset, bytes);
    return FS_RETAIL_FF_WIRE_OK;
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileImage(
    FsRetailFastfileReader *reader, uint32_t headerRef, FsRetailFastfileImage *image)
{
    uint8_t wire[36];
    if (!reader || !image)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    Com_Memset(image, 0, sizeof(*image));
    image->headerRef = headerRef;
    if (!headerRef)
        return FS_RETAIL_FF_WIRE_OK;
    if (headerRef != RETAIL_FASTFILE_INLINE_REF)
        return FS_RETAIL_FF_WIRE_BODY_NOT_INLINE;
    const uint32_t tempStart = reader->blockCursor[0];
    reader->blockCursor[0] = (reader->blockCursor[0] + 3u) & ~3u;
    if (reader->blockCursor[0] + sizeof(wire) > reader->blockSizes[0] ||
        FS_ReadRetailFastfile(reader, wire, sizeof(wire)) != sizeof(wire))
    {
        reader->blockCursor[0] = tempStart;
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    }
    Com_Memcpy(reader->blockData[0] + reader->blockCursor[0], wire, sizeof(wire));
    reader->blockCursor[0] = tempStart;
    image->mapType = ReadRetailFastfileLe32(wire);
    image->textureRef = ReadRetailFastfileLe32(wire + 4);
    image->picmip[0] = wire[8];
    image->picmip[1] = wire[9];
    image->noPicmip = wire[10];
    image->semantic = wire[11];
    image->track = wire[12];
    image->cardMemory[0] = ReadRetailFastfileLe32(wire + 16);
    image->cardMemory[1] = ReadRetailFastfileLe32(wire + 20);
    image->width = static_cast<uint16_t>(wire[24] | wire[25] << 8);
    image->height = static_cast<uint16_t>(wire[26] | wire[27] << 8);
    image->depth = static_cast<uint16_t>(wire[28] | wire[29] << 8);
    image->category = wire[30];
    image->delayLoadPixels = wire[31];
    image->nameRef = ReadRetailFastfileLe32(wire + 32);
    image->nameWasInline = (image->nameRef == RETAIL_FASTFILE_INLINE_REF) ? 1u : 0u;
    FsRetailFastfileWireResult result = ReadRetailFastfileInlineName(reader, &image->nameRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    if (image->textureRef != RETAIL_FASTFILE_INLINE_REF && image->textureRef != RETAIL_FASTFILE_INSERT_REF)
        return FS_RETAIL_FF_WIRE_OK;
    // The 36-byte body mirror stays at the aligned temp-block offset for the
    // whole load (the Android temp block rewinds per asset, not per field).
    const uint32_t bodyOffset = (tempStart + 3u) & ~3u;
    const uint32_t loadDefOffset = bodyOffset + sizeof(wire);
    if (image->textureRef == RETAIL_FASTFILE_INSERT_REF)
    {
        // The linker's insert optimization reserves 4 bytes of block 4 for
        // the final pointer without writing stream bytes (DB_InsertPointer).
        reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
        if (reader->blockCursor[4] + 4u > reader->blockSizes[4])
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        image->textureInsertOffset = reader->blockCursor[4];
        reader->blockCursor[4] += 4;
    }
    uint8_t wireLoadDef[16];
    if (loadDefOffset + sizeof(wireLoadDef) > reader->blockSizes[0] ||
        FS_ReadRetailFastfile(reader, wireLoadDef, sizeof(wireLoadDef)) != sizeof(wireLoadDef))
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    // LoadGfxTextureLoad: the loadDef lives in the temp block right after the
    // body, and the texture slot (plus the reserved insert slot) receives the
    // block-0 loadDef reference.
    Com_Memcpy(reader->blockData[0] + loadDefOffset, wireLoadDef, sizeof(wireLoadDef));
    RetailFastfileWriteSlot(reader, 0, bodyOffset + 4, ((0u << 28) | loadDefOffset) + 1u);
    if (image->textureInsertOffset)
        RetailFastfileWriteSlot(
            reader, 4, image->textureInsertOffset, ((0u << 28) | loadDefOffset) + 1u);
    image->haveLoadDef = 1;
    image->loadDefLevelCount = wireLoadDef[0];
    image->loadDefFlags = wireLoadDef[1];
    for (uint32_t i = 0; i < 3; ++i)
        image->loadDefDimensions[i] =
            static_cast<uint16_t>(wireLoadDef[2 + i * 2] | wireLoadDef[3 + i * 2] << 8);
    image->loadDefFormat = ReadRetailFastfileLe32(wireLoadDef + 8);
    image->loadDefResourceSize = ReadRetailFastfileLe32(wireLoadDef + 12);
    if (image->loadDefResourceSize)
    {
        const uint32_t pixelOffset = loadDefOffset + sizeof(wireLoadDef);
        if (pixelOffset + image->loadDefResourceSize > reader->blockSizes[0] ||
            FS_ReadRetailFastfile(reader, reader->blockData[0] + pixelOffset,
                                  image->loadDefResourceSize) != image->loadDefResourceSize)
        {
            reader->blockCursor[0] = tempStart;
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        }
        image->pixelDataOffset = pixelOffset;
    }
    reader->blockCursor[0] = tempStart;
    return FS_RETAIL_FF_WIRE_OK;
}

// Load_water_t order, mirrored from the walk-only reader's
// ReadRetailWaterBody (db_retail_walk.cpp): the semantic-11 arm of the
// MaterialTextureDef image/water union. An inline slot streams a 68-byte
// root (H0 at +4 sized N*M complex floats, wTerm at +8 sized N*M floats,
// M at +12, N at +16, image at +64); the -2 insert form carries no stream
// bytes. Any other non-null form is an already-streamed alias. The
// material texture loop previously decoded every inline slot as a GfxImage
// body, so a water material's 68-byte root was eaten as a 36-byte image
// root and its float/image bytes desynchronized everything downstream
// (killhouse ord 772: 12,298 bytes skipped at wc/kh_water_mud, cascading
// through verts/vld/sunflare/shadowGeom into the hull-count failure).
static FsRetailFastfileWireResult ReadRetailFastfileWater(
    FsRetailFastfileReader *reader, uint32_t reference, FsRetailFastfileTextureDef *def)
{
    uint8_t wire[68];
    if (!reader || !def)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    const bool inlineBody = reference == RETAIL_FASTFILE_INLINE_REF;
    uint32_t waterOffset = (reference - 1u) & 0x0fffffffu;
    if (inlineBody)
    {
        reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
        waterOffset = reader->blockCursor[4];
        if (waterOffset > reader->blockSizes[4] ||
            reader->blockSizes[4] - waterOffset < sizeof(wire) ||
            FS_ReadRetailFastfile(reader, reader->blockData[4] + waterOffset,
                                  sizeof(wire)) != sizeof(wire))
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        reader->blockCursor[4] += sizeof(wire);
    }
    else if (((reference - 1u) >> 28) != 4u ||
             waterOffset > reader->blockCursor[4] ||
             reader->blockCursor[4] - waterOffset < sizeof(wire))
    {
        return FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED;
    }
    const uint8_t *water = reader->blockData[4] + waterOffset;
    const uint32_t h0Ref = ReadRetailFastfileLe32(water + 4);
    const uint32_t wTermRef = ReadRetailFastfileLe32(water + 8);
    const uint32_t m = ReadRetailFastfileLe32(water + 12);
    const uint32_t n = ReadRetailFastfileLe32(water + 16);
    const uint32_t imageRef = ReadRetailFastfileLe32(water + 64);
    def->water.reference = ((4u << 28) | waterOffset) + 1u;
    def->water.m = m;
    def->water.n = n;
    Com_Memcpy(&def->water.floatTime, water, sizeof(float));
    Com_Memcpy(def->water.parameters, water + 20, sizeof(def->water.parameters));
    const uint32_t refs[2] = {h0Ref, wTermRef};
    const uint64_t sizes[2] = {(uint64_t)n * m * 8u, (uint64_t)n * m * 4u};
    for (uint32_t side = 0; side < 2; ++side)
    {
        if (!refs[side])
            continue;
        if (refs[side] != RETAIL_FASTFILE_INLINE_REF)
        {
            // Array aliases address their complete mirrored block-4 span.
            // Asset-pool references cannot name frequency-array storage.
            const uint32_t block = (refs[side] - 1u) >> 28;
            const uint32_t off = (refs[side] - 1u) & 0x0fffffffu;
            if (block != 4u)
                return FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH;
            if (off > reader->blockCursor[4] ||
                sizes[side] > reader->blockCursor[4] - off)
                return FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH;
            continue;
        }
        if (!inlineBody)
            return FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED;
        if (sizes[side] > UINT32_MAX)
            return FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH;
        reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
        const uint32_t arrayOffset = reader->blockCursor[4];
        if (reader->blockCursor[4] + sizes[side] > reader->blockSizes[4] ||
            FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4],
                                  (uint32_t)sizes[side]) != sizes[side])
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        reader->blockCursor[4] += (uint32_t)sizes[side];
        RetailFastfileWriteSlot(reader, 4, waterOffset + 4u + side * 4u,
                                ((4u << 28) | arrayOffset) + 1u);
    }
    def->water.h0Ref = ReadRetailFastfileLe32(water + 4);
    def->water.wTermRef = ReadRetailFastfileLe32(water + 8);
    const uint32_t imageSlotOffset = waterOffset + 64u;
    if (!imageRef)
    {
        def->imageRef = 0;
        return FS_RETAIL_FF_WIRE_OK;
    }
    if (imageRef == RETAIL_FASTFILE_INLINE_REF || imageRef == RETAIL_FASTFILE_INSERT_REF)
    {
        if (!inlineBody)
            return FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED;
        uint32_t imageInsertOffset = 0;
        if (imageRef == RETAIL_FASTFILE_INSERT_REF &&
            FS_RetailFastfileReserveBlock4Slot(reader, &imageInsertOffset) !=
                FS_RETAIL_FF_WIRE_OK)
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        FsRetailFastfileImage inlineImage;
        Com_Memset(&inlineImage, 0, sizeof(inlineImage));
        const FsRetailFastfileWireResult imageRes =
            FS_ReadRetailFastfileImage(reader, RETAIL_FASTFILE_INLINE_REF, &inlineImage);
        if (imageRes != FS_RETAIL_FF_WIRE_OK)
            return imageRes;
        uint32_t poolRef = 0;
        if (FS_RetailFastfileRegisterImagePoolSlot(reader, inlineImage.nameRef, imageSlotOffset,
                                                   &poolRef) != FS_RETAIL_FF_WIRE_OK)
            return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
        if (imageInsertOffset)
            RetailFastfileWriteSlot(reader, 4, imageInsertOffset, poolRef);
        def->imageRef = poolRef;
        def->imageWasInline = 1;
        def->inlineImage = inlineImage;
        return FS_RETAIL_FF_WIRE_OK;
    }
    uint32_t targetRef = 0;
    const FsRetailFastfileWireResult aliasRes =
        FS_RetailFastfileResolveImageAlias(reader, imageRef, &targetRef);
    if (aliasRes == FS_RETAIL_FF_WIRE_OK)
    {
        RetailFastfileWriteSlot(reader, 4, imageSlotOffset, targetRef);
        def->imageRef = targetRef;
        return FS_RETAIL_FF_WIRE_OK;
    }
    if (aliasRes == FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED)
    {
        def->imageRef = imageRef;
        return FS_RETAIL_FF_WIRE_OK;
    }
    return aliasRes;
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileMaterial(
    FsRetailFastfileReader *reader, uint32_t headerRef, FsRetailFastfileMaterial *material,
    FsRetailFastfileTextureDef *textureDefs, uint32_t textureDefCapacity)
{
    uint8_t wire[80];
    if (!reader || !material)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    Com_Memset(material, 0, sizeof(*material));
    material->headerRef = headerRef;
    if (!headerRef)
        return FS_RETAIL_FF_WIRE_OK;
    if (headerRef != RETAIL_FASTFILE_INLINE_REF && headerRef != RETAIL_FASTFILE_INSERT_REF)
        return FS_RETAIL_FF_WIRE_BODY_NOT_INLINE;
    uint32_t materialInsertOffset = 0;
    if (headerRef == RETAIL_FASTFILE_INSERT_REF)
    {
        reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
        if (reader->blockCursor[4] + 4u > reader->blockSizes[4])
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        materialInsertOffset = reader->blockCursor[4];
        reader->blockCursor[4] += 4;
    }
    const uint32_t tempStart = reader->blockCursor[0];
    reader->blockCursor[0] = (reader->blockCursor[0] + 3u) & ~3u;
    if (reader->blockCursor[0] + sizeof(wire) > reader->blockSizes[0] ||
        FS_ReadRetailFastfile(reader, wire, sizeof(wire)) != sizeof(wire))
    {
        reader->blockCursor[0] = tempStart;
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    }
    Com_Memcpy(reader->blockData[0] + reader->blockCursor[0], wire, sizeof(wire));
    reader->blockCursor[0] = tempStart;
    material->nameRef = ReadRetailFastfileLe32(wire);
    material->nameWasInline = (material->nameRef == RETAIL_FASTFILE_INLINE_REF);
    material->gameFlags = wire[4];
    material->sortKey = wire[5];
    material->textureAtlasRowCount = wire[6];
    material->textureAtlasColumnCount = wire[7];
    material->drawSurf = static_cast<uint64_t>(ReadRetailFastfileLe32(wire + 8)) |
        (static_cast<uint64_t>(ReadRetailFastfileLe32(wire + 12)) << 32);
    material->surfaceTypeBits = ReadRetailFastfileLe32(wire + 16);
    material->hashIndex = static_cast<uint16_t>(wire[20] | wire[21] << 8);
    Com_Memcpy(material->stateBitsEntry, wire + 24, sizeof(material->stateBitsEntry));
    material->textureCount = wire[58];
    material->constantCount = wire[59];
    material->stateBitsCount = wire[60];
    material->stateFlags = wire[61];
    material->cameraRegion = wire[62];
    material->techniqueSetRef = ReadRetailFastfileLe32(wire + 64);
    material->textureTableRef = ReadRetailFastfileLe32(wire + 68);
    material->constantTableRef = ReadRetailFastfileLe32(wire + 72);
    material->stateBitsTableRef = ReadRetailFastfileLe32(wire + 76);
    FsRetailFastfileWireResult result = ReadRetailFastfileInlineName(reader, &material->nameRef);
    RETAIL_ASSET_TRACE(0, "FS_ReadRetailFastfileMaterial: insertOff=0x%x nameRef=%x (res=%d), texCnt=%u, constCnt=%u, stateCnt=%u\n",
                       materialInsertOffset, material->nameRef, result, material->textureCount,
                       material->constantCount, material->stateBitsCount);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    if (materialInsertOffset)
        RetailFastfileWriteSlot(reader, 4, materialInsertOffset, material->nameRef);
    // The Android/engine loader treats the texture, constant, and state-bits
    // tables as independent slots.  A missing or shared texture table must
    // not prevent a later inline constant/state-bits table from streaming.
    if (material->textureTableRef == RETAIL_FASTFILE_INLINE_REF)
    {
        if (material->textureCount && (!textureDefs || textureDefCapacity < material->textureCount))
            return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
        reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
        const uint32_t tableOffset = reader->blockCursor[4];
        const uint32_t tableBytes = material->textureCount * 12u;
        if (reader->blockCursor[4] + tableBytes > reader->blockSizes[4] ||
            FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4], tableBytes) != tableBytes)
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        reader->blockCursor[4] += tableBytes;
        material->textureTableRef = ((4u << 28) | tableOffset) + 1;
        for (uint32_t i = 0; i < material->textureCount; ++i)
        {
            const uint32_t defOffset = tableOffset + i * 12u;
            const uint8_t *wireDef = reader->blockData[4] + defOffset;
            FsRetailFastfileTextureDef *def = &textureDefs[i];
            Com_Memset(def, 0, sizeof(*def));
            def->nameHash = ReadRetailFastfileLe32(wireDef);
            def->nameStart = static_cast<char>(wireDef[4]);
            def->nameEnd = static_cast<char>(wireDef[5]);
            def->samplerState = wireDef[6];
            def->semantic = wireDef[7];
            const uint32_t imageSlotOffset = defOffset + 8u;
            const uint32_t imageSlotValue = ReadRetailFastfileLe32(wireDef + 8);
            RETAIL_ASSET_TRACE(0, "FS_ReadRetailFastfileMaterial tex[%u]: slotVal=0x%08x cur4=%u\n",
                               i, imageSlotValue, reader->blockCursor[4]);
            def->imageRef = imageSlotValue;
            if (!imageSlotValue)
                continue;
            if (def->semantic == 11)
            {
                result = ReadRetailFastfileWater(reader, imageSlotValue, def);
                if (result != FS_RETAIL_FF_WIRE_OK)
                    return result;
                RetailFastfileWriteSlot(reader, 4, imageSlotOffset, def->water.reference);
            }
            else if (imageSlotValue == RETAIL_FASTFILE_INLINE_REF || imageSlotValue == RETAIL_FASTFILE_INSERT_REF)
            {
                // LoadGfxImagePtr/HandleAssetSlot: the image body follows in the
                // stream.  A -2 texture-def slot first reserves its own 4-byte
                // DB_InsertPointer slot in block 4.
                uint32_t defInsertOffset = 0;
                if (imageSlotValue == RETAIL_FASTFILE_INSERT_REF)
                {
                    const FsRetailFastfileWireResult reserveResult =
                        FS_RetailFastfileReserveBlock4Slot(reader, &defInsertOffset);
                    if (reserveResult != FS_RETAIL_FF_WIRE_OK)
                        return reserveResult;
                }
                FsRetailFastfileImage inlineImage;
                result = FS_ReadRetailFastfileImage(reader, RETAIL_FASTFILE_INLINE_REF, &inlineImage);
                if (result != FS_RETAIL_FF_WIRE_OK)
                    return result;
                // DB_AddXAsset: pool the loaded image and write the pooled
                // reference back into the texture-def slot (and the reserved
                // insert slot) so later alias references copy it.
                uint32_t poolRef = 0;
                if (FS_RetailFastfileRegisterImagePoolSlot(reader, inlineImage.nameRef, imageSlotOffset,
                                                           &poolRef) != FS_RETAIL_FF_WIRE_OK)
                    return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
                if (defInsertOffset)
                    RetailFastfileWriteSlot(reader, 4, defInsertOffset, poolRef);
                def->imageRef = poolRef;
                def->imageWasInline = 1;
                def->inlineImage = inlineImage;
            }
            else
            {
                // DB_ConvertOffsetToAlias: copy the referenced block-4 slot's
                // value.  The target must be a slot an earlier walk already
                // filled with a pooled image reference; anything else is an
                // unsupported form and fails loudly.
                uint32_t targetRef = 0;
                const FsRetailFastfileWireResult aliasResult =
                    FS_RetailFastfileResolveImageAlias(reader, imageSlotValue, &targetRef);
                if (aliasResult == FS_RETAIL_FF_WIRE_OK)
                {
                    RetailFastfileWriteSlot(reader, 4, imageSlotOffset, targetRef);
                    def->imageRef = targetRef;
                }
                else if (aliasResult == FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED)
                {
                    def->imageRef = imageSlotValue;
                }
                else
                {
                    return aliasResult;
                }
            }
        }
    }
    if (material->constantTableRef == RETAIL_FASTFILE_INLINE_REF)
    {
        reader->blockCursor[4] = (reader->blockCursor[4] + 15u) & ~15u;
        material->constantTableOffset = reader->blockCursor[4];
        const uint32_t constantBytes = material->constantCount * 32u;
        if (reader->blockCursor[4] + constantBytes > reader->blockSizes[4] ||
            FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4], constantBytes) != constantBytes)
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        reader->blockCursor[4] += constantBytes;
        material->constantTableRef = ((4u << 28) | material->constantTableOffset) + 1;
    }
    else if (material->constantCount && (material->constantTableRef >> 28) == 4u)
    {
        material->constantTableOffset = (material->constantTableRef - 1u) & 0x0fffffffu;
    }
    if (material->stateBitsTableRef == RETAIL_FASTFILE_INLINE_REF)
    {
        reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
        material->stateBitsOffset = reader->blockCursor[4];
        const uint32_t stateBitsBytes = material->stateBitsCount * 8u;
        if (reader->blockCursor[4] + stateBitsBytes > reader->blockSizes[4] ||
            FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4], stateBitsBytes) != stateBitsBytes)
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        reader->blockCursor[4] += stateBitsBytes;
        material->stateBitsTableRef = ((4u << 28) | material->stateBitsOffset) + 1;
    }
    else if (material->stateBitsCount && (material->stateBitsTableRef >> 28) == 4u)
    {
        material->stateBitsOffset = (material->stateBitsTableRef - 1u) & 0x0fffffffu;
    }
    return FS_RETAIL_FF_WIRE_OK;
}

// Walks one serialized statement_s (Load_Statement): the entries pointer
// array streams first (the engine allocates for any non-zero slot), then one
// 12-byte expressionEntry body per non-zero slot; only a type != 0 operand
// with dataType == VAL_STRING carries an inline string.
static FsRetailFastfileWireResult ReadRetailFastfileStatement(
    FsRetailFastfileReader *reader,
    FsRetailFastfileStatement *statement,
    FsRetailFastfileExpressionEntry *entries,
    uint32_t entryCapacity,
    uint32_t *entryCount)
{
    if (!statement->entriesRef)
        return FS_RETAIL_FF_WIRE_OK;
    if (statement->numEntries > RETAIL_FASTFILE_EXPRESSION_ENTRY_MAX)
        return FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH;
    if (statement->numEntries &&
        (!entries || *entryCount + statement->numEntries > entryCapacity))
        return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
    const uint64_t arrayBytes = static_cast<uint64_t>(statement->numEntries) * 4u;
    if (static_cast<uint64_t>(reader->blockCursor[4]) + arrayBytes > reader->blockSizes[4] ||
        FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4],
                              static_cast<uint32_t>(arrayBytes)) != arrayBytes)
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    const uint32_t arrayOffset = reader->blockCursor[4];
    reader->blockCursor[4] += static_cast<uint32_t>(arrayBytes);
    for (uint32_t i = 0; i < statement->numEntries; ++i)
    {
        const uint32_t slotValue = ReadRetailFastfileLe32(reader->blockData[4] + arrayOffset + i * 4u);
        FsRetailFastfileExpressionEntry *entry = &entries[*entryCount];
        ++*entryCount;
        Com_Memset(entry, 0, sizeof(*entry));
        if (!slotValue)
            continue;
        reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
        uint8_t wire[12];
        if (reader->blockCursor[4] + sizeof(wire) > reader->blockSizes[4] ||
            FS_ReadRetailFastfile(reader, wire, sizeof(wire)) != sizeof(wire))
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        Com_Memcpy(reader->blockData[4] + reader->blockCursor[4], wire, sizeof(wire));
        reader->blockCursor[4] += sizeof(wire);
        entry->type = static_cast<int32_t>(ReadRetailFastfileLe32(wire));
        entry->dataType = ReadRetailFastfileLe32(wire + 4);
        entry->operandRef = ReadRetailFastfileLe32(wire + 8);
        if (entry->type != 0 && entry->dataType == 2u /* VAL_STRING */)
        {
            const FsRetailFastfileWireResult result =
                ReadRetailFastfileInlineName(reader, &entry->operandRef);
            if (result != FS_RETAIL_FF_WIRE_OK)
                return result;
        }
    }
    return FS_RETAIL_FF_WIRE_OK;
}

// Walks one serialized ItemKeyHandler chain (Load_ItemKeyHandler): nodes
// stream inline as 12-byte {key, action, next} records linked by their next
// slot until a null terminator.
static FsRetailFastfileWireResult ReadRetailFastfileKeyHandlerChain(
    FsRetailFastfileReader *reader,
    uint32_t handlerRef,
    FsRetailFastfileItemKeyHandler *handlers,
    uint32_t handlerCapacity,
    uint32_t *handlerCount)
{
    *handlerCount = 0;
    uint32_t ref = handlerRef;
    while (ref)
    {
        if (*handlerCount >= RETAIL_FASTFILE_KEY_HANDLER_MAX)
            return FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH;
        if (!handlers || *handlerCount >= handlerCapacity)
            return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
        reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
        uint8_t wire[12];
        if (reader->blockCursor[4] + sizeof(wire) > reader->blockSizes[4] ||
            FS_ReadRetailFastfile(reader, wire, sizeof(wire)) != sizeof(wire))
            return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
        Com_Memcpy(reader->blockData[4] + reader->blockCursor[4], wire, sizeof(wire));
        reader->blockCursor[4] += sizeof(wire);
        FsRetailFastfileItemKeyHandler *handler = &handlers[*handlerCount];
        handler->key = static_cast<int32_t>(ReadRetailFastfileLe32(wire));
        handler->actionRef = ReadRetailFastfileLe32(wire + 4);
        const uint32_t nextRef = ReadRetailFastfileLe32(wire + 8);
        const FsRetailFastfileWireResult result =
            ReadRetailFastfileInlineName(reader, &handler->actionRef);
        if (result != FS_RETAIL_FF_WIRE_OK)
            return result;
        ref = nextRef;
        ++*handlerCount;
    }
    return FS_RETAIL_FF_WIRE_OK;
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileMenuList(
    FsRetailFastfileReader *reader, uint32_t headerRef,
    FsRetailFastfileMenuList *menuList, uint32_t *menuRefs, uint32_t menuRefCapacity)
{
    uint8_t wire[12];
    if (!reader || !menuList)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    Com_Memset(menuList, 0, sizeof(*menuList));
    menuList->headerRef = headerRef;
    if (!headerRef)
        return FS_RETAIL_FF_WIRE_OK;
    if (headerRef != RETAIL_FASTFILE_INLINE_REF)
        return FS_RETAIL_FF_WIRE_BODY_NOT_INLINE;
    const uint32_t tempStart = reader->blockCursor[0];
    reader->blockCursor[0] = (reader->blockCursor[0] + 3u) & ~3u;
    if (reader->blockCursor[0] + sizeof(wire) > reader->blockSizes[0] ||
        FS_ReadRetailFastfile(reader, wire, sizeof(wire)) != sizeof(wire))
    {
        reader->blockCursor[0] = tempStart;
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    }
    Com_Memcpy(reader->blockData[0] + reader->blockCursor[0], wire, sizeof(wire));
    reader->blockCursor[0] = tempStart;
    menuList->nameRef = ReadRetailFastfileLe32(wire);
    menuList->menuCount = ReadRetailFastfileLe32(wire + 4);
    menuList->menusRef = ReadRetailFastfileLe32(wire + 8);
    FsRetailFastfileWireResult result = ReadRetailFastfileInlineName(reader, &menuList->nameRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    if (!menuList->menusRef)
        return FS_RETAIL_FF_WIRE_OK;
    if (menuList->menuCount > RETAIL_FASTFILE_MENU_MAX)
        return FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH;
    if (menuList->menuCount && (!menuRefs || menuRefCapacity < menuList->menuCount))
        return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
    const uint64_t arrayBytes = static_cast<uint64_t>(menuList->menuCount) * 4u;
    if (static_cast<uint64_t>(reader->blockCursor[4]) + arrayBytes > reader->blockSizes[4] ||
        FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4],
                              static_cast<uint32_t>(arrayBytes)) != arrayBytes)
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    menuList->menusOffset = reader->blockCursor[4];
    if (menuList->menuCount)
        Com_Memcpy(menuRefs, reader->blockData[4] + menuList->menusOffset,
                   static_cast<size_t>(arrayBytes));
    reader->blockCursor[4] += static_cast<uint32_t>(arrayBytes);
    menuList->menusRef = ((4u << 28) | menuList->menusOffset) + 1;
    return FS_RETAIL_FF_WIRE_OK;
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileMenuPrefix(
    FsRetailFastfileReader *reader, uint32_t menuRef, FsRetailFastfileMenu *menu,
    FsRetailFastfileItemKeyHandler *handlers, uint32_t handlerCapacity,
    FsRetailFastfileExpressionEntry *entries, uint32_t entryCapacity)
{
    uint8_t wire[284];
    if (!reader || !menu)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    Com_Memset(menu, 0, sizeof(*menu));
    menu->headerRef = menuRef;
    if (!menuRef)
        return FS_RETAIL_FF_WIRE_OK;
    if (menuRef != RETAIL_FASTFILE_INLINE_REF)
        return FS_RETAIL_FF_WIRE_BODY_NOT_INLINE;
    const uint32_t tempStart = reader->blockCursor[0];
    reader->blockCursor[0] = (reader->blockCursor[0] + 3u) & ~3u;
    if (reader->blockCursor[0] + sizeof(wire) > reader->blockSizes[0] ||
        FS_ReadRetailFastfile(reader, wire, sizeof(wire)) != sizeof(wire))
    {
        reader->blockCursor[0] = tempStart;
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    }
    Com_Memcpy(reader->blockData[0] + reader->blockCursor[0], wire, sizeof(wire));
    reader->blockCursor[0] = tempStart;
    // windowDef_t (156 bytes)
    menu->nameRef = ReadRetailFastfileLe32(wire);
    for (uint32_t i = 0; i < 4; ++i)
        menu->rect[i] = ReadRetailFastfileLeFloat(wire + 4 + i * 4);
    menu->rectHorzAlign = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 20));
    menu->rectVertAlign = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 24));
    for (uint32_t i = 0; i < 4; ++i)
        menu->rectClient[i] = ReadRetailFastfileLeFloat(wire + 28 + i * 4);
    menu->rectClientHorzAlign = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 44));
    menu->rectClientVertAlign = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 48));
    menu->groupRef = ReadRetailFastfileLe32(wire + 52);
    menu->style = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 56));
    menu->border = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 60));
    menu->ownerDraw = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 64));
    menu->ownerDrawFlags = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 68));
    menu->borderSize = ReadRetailFastfileLeFloat(wire + 72);
    menu->staticFlags = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 76));
    menu->dynamicFlags = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 80));
    menu->nextTime = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 84));
    for (uint32_t i = 0; i < 4; ++i)
        menu->foreColor[i] = ReadRetailFastfileLeFloat(wire + 88 + i * 4);
    for (uint32_t i = 0; i < 4; ++i)
        menu->backColor[i] = ReadRetailFastfileLeFloat(wire + 104 + i * 4);
    for (uint32_t i = 0; i < 4; ++i)
        menu->borderColor[i] = ReadRetailFastfileLeFloat(wire + 120 + i * 4);
    for (uint32_t i = 0; i < 4; ++i)
        menu->outlineColor[i] = ReadRetailFastfileLeFloat(wire + 136 + i * 4);
    menu->backgroundRef = ReadRetailFastfileLe32(wire + 152);
    // menuDef_t tail
    menu->fontRef = ReadRetailFastfileLe32(wire + 156);
    menu->fullScreen = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 160));
    menu->itemCount = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 164));
    menu->fontIndex = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 168));
    menu->cursorItem = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 172));
    menu->fadeCycle = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 176));
    menu->fadeClamp = ReadRetailFastfileLeFloat(wire + 180);
    menu->fadeAmount = ReadRetailFastfileLeFloat(wire + 184);
    menu->fadeInAmount = ReadRetailFastfileLeFloat(wire + 188);
    menu->blurRadius = ReadRetailFastfileLeFloat(wire + 192);
    menu->onOpenRef = ReadRetailFastfileLe32(wire + 196);
    menu->onCloseRef = ReadRetailFastfileLe32(wire + 200);
    menu->onESCRef = ReadRetailFastfileLe32(wire + 204);
    menu->onKeyRef = ReadRetailFastfileLe32(wire + 208);
    menu->visibleExp.numEntries = ReadRetailFastfileLe32(wire + 212);
    menu->visibleExp.entriesRef = ReadRetailFastfileLe32(wire + 216);
    menu->allowedBindingRef = ReadRetailFastfileLe32(wire + 220);
    menu->soundNameRef = ReadRetailFastfileLe32(wire + 224);
    menu->imageTrack = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 228));
    for (uint32_t i = 0; i < 4; ++i)
        menu->focusColor[i] = ReadRetailFastfileLeFloat(wire + 232 + i * 4);
    for (uint32_t i = 0; i < 4; ++i)
        menu->disableColor[i] = ReadRetailFastfileLeFloat(wire + 248 + i * 4);
    menu->rectXExp.numEntries = ReadRetailFastfileLe32(wire + 264);
    menu->rectXExp.entriesRef = ReadRetailFastfileLe32(wire + 268);
    menu->rectYExp.numEntries = ReadRetailFastfileLe32(wire + 272);
    menu->rectYExp.entriesRef = ReadRetailFastfileLe32(wire + 276);
    menu->itemsRef = ReadRetailFastfileLe32(wire + 280);
    if (menu->itemCount < 0 ||
        static_cast<uint32_t>(menu->itemCount) > RETAIL_FASTFILE_MENU_ITEM_MAX)
        return FS_RETAIL_FF_WIRE_BODY_INVALID_LENGTH;

    // Nested data streams in field order (Load_windowDef / LoadMenuDef).
    FsRetailFastfileWireResult result = ReadRetailFastfileInlineName(reader, &menu->nameRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    result = ReadRetailFastfileInlineName(reader, &menu->groupRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    menu->backgroundWasInline = 0;
    if (menu->backgroundRef == RETAIL_FASTFILE_INLINE_REF ||
        menu->backgroundRef == RETAIL_FASTFILE_INSERT_REF)
    {
        result = FS_ReadRetailFastfileMaterial(reader, menu->backgroundRef, &menu->backgroundInlineMaterial,
                                               menu->backgroundInlineTextures, 8);
        if (result != FS_RETAIL_FF_WIRE_OK)
            return result;
        menu->backgroundWasInline = 1;
        menu->backgroundRef = menu->backgroundInlineMaterial.nameRef;
    }
    result = ReadRetailFastfileInlineName(reader, &menu->fontRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    result = ReadRetailFastfileInlineName(reader, &menu->onOpenRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    result = ReadRetailFastfileInlineName(reader, &menu->onCloseRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    result = ReadRetailFastfileInlineName(reader, &menu->onESCRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    result = ReadRetailFastfileKeyHandlerChain(
        reader, menu->onKeyRef, handlers, handlerCapacity, &menu->handlerCount);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    result = ReadRetailFastfileStatement(
        reader, &menu->visibleExp, entries, entryCapacity, &menu->expressionEntryCount);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    result = ReadRetailFastfileInlineName(reader, &menu->allowedBindingRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    result = ReadRetailFastfileInlineName(reader, &menu->soundNameRef);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    result = ReadRetailFastfileStatement(
        reader, &menu->rectXExp, entries, entryCapacity, &menu->expressionEntryCount);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    result = ReadRetailFastfileStatement(
        reader, &menu->rectYExp, entries, entryCapacity, &menu->expressionEntryCount);
    if (result != FS_RETAIL_FF_WIRE_OK)
        return result;
    if (!menu->itemsRef)
        return FS_RETAIL_FF_WIRE_OK;
    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
    const uint64_t arrayBytes = static_cast<uint64_t>(menu->itemCount) * 4u;
    if (static_cast<uint64_t>(reader->blockCursor[4]) + arrayBytes > reader->blockSizes[4] ||
        FS_ReadRetailFastfile(reader, reader->blockData[4] + reader->blockCursor[4],
                              static_cast<uint32_t>(arrayBytes)) != arrayBytes)
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    menu->itemsOffset = reader->blockCursor[4];
    reader->blockCursor[4] += static_cast<uint32_t>(arrayBytes);
    menu->itemsRef = ((4u << 28) | menu->itemsOffset) + 1;
    return FS_RETAIL_FF_WIRE_OK;
}

static FsRetailFastfileWireResult ReadRetailFastfileItemTypeData(
    FsRetailFastfileReader *reader, int32_t type, uint32_t *typeDataRef,
    uint32_t *typeDataOffset)
{
    if (!typeDataRef || !*typeDataRef)
        return FS_RETAIL_FF_WIRE_OK;
    if (type == 0xD) // enumDvarName is the one XString-backed union form
        return ReadRetailFastfileInlineName(reader, typeDataRef);

    uint32_t bytes = 0;
    switch (type)
    {
    case 6: bytes = 340; break; // listBoxDef_s
    case 4: case 9: case 0x10: case 0x12: case 0xB: case 0xE:
    case 0xA: case 0: case 0x11: bytes = 32; break; // editFieldDef_s
    case 0xC: bytes = 392; break; // multiDef_s
    default:
        return FS_RETAIL_FF_WIRE_OK;
    }
    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
    const uint32_t offset = reader->blockCursor[4];
    if (static_cast<uint64_t>(offset) + bytes > reader->blockSizes[4] ||
        FS_ReadRetailFastfile(reader, reader->blockData[4] + offset, bytes) != bytes)
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    reader->blockCursor[4] += bytes;
    if (typeDataOffset)
        *typeDataOffset = offset;
    if (typeDataRef && *typeDataRef == RETAIL_FASTFILE_INLINE_REF)
        *typeDataRef = ((4u << 28) | offset) + 1u;

    // The two nested pointer-bearing forms have their own XString fields.
    if (type == 6)
    {
        uint8_t *slotPtr = reader->blockData[4] + offset + 288;
        uint32_t ref = ReadRetailFastfileLe32(slotPtr);
        if (ref)
        {
            FsRetailFastfileWireResult result = ReadRetailFastfileInlineName(reader, &ref);
            if (result != FS_RETAIL_FF_WIRE_OK)
                return result;
            slotPtr[0] = static_cast<uint8_t>(ref & 0xFF);
            slotPtr[1] = static_cast<uint8_t>((ref >> 8) & 0xFF);
            slotPtr[2] = static_cast<uint8_t>((ref >> 16) & 0xFF);
            slotPtr[3] = static_cast<uint8_t>((ref >> 24) & 0xFF);
        }
    }
    else if (type == 0xC)
    {
        for (uint32_t i = 0; i < 64; ++i)
        {
            uint8_t *slotPtr = reader->blockData[4] + offset + i * 4;
            uint32_t ref = ReadRetailFastfileLe32(slotPtr);
            if (ref)
            {
                FsRetailFastfileWireResult result = ReadRetailFastfileInlineName(reader, &ref);
                if (result != FS_RETAIL_FF_WIRE_OK)
                    return result;
                slotPtr[0] = static_cast<uint8_t>(ref & 0xFF);
                slotPtr[1] = static_cast<uint8_t>((ref >> 8) & 0xFF);
                slotPtr[2] = static_cast<uint8_t>((ref >> 16) & 0xFF);
                slotPtr[3] = static_cast<uint8_t>((ref >> 24) & 0xFF);
            }
        }
    }
    return FS_RETAIL_FF_WIRE_OK;
}

FsRetailFastfileWireResult __cdecl FS_ReadRetailFastfileItemDef(
    FsRetailFastfileReader *reader, uint32_t itemRef, FsRetailFastfileItemDef *item,
    FsRetailFastfileItemKeyHandler *handlers, uint32_t handlerCapacity,
    FsRetailFastfileExpressionEntry *entries, uint32_t entryCapacity)
{
    uint8_t wire[372];
    if (!reader || !item)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    Com_Memset(item, 0, sizeof(*item));
    item->headerRef = itemRef;
    if (!itemRef)
        return FS_RETAIL_FF_WIRE_OK;
    if (itemRef != RETAIL_FASTFILE_INLINE_REF)
        return FS_RETAIL_FF_WIRE_BODY_NOT_INLINE;
    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
    if (reader->blockCursor[4] + sizeof(wire) > reader->blockSizes[4] ||
        FS_ReadRetailFastfile(reader, wire, sizeof(wire)) != sizeof(wire))
    {
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    }
    Com_Memcpy(reader->blockData[4] + reader->blockCursor[4], wire, sizeof(wire));
    reader->blockCursor[4] += sizeof(wire);

    item->nameRef = ReadRetailFastfileLe32(wire + 0);
    for (uint32_t i = 0; i < 4; ++i)
        item->rect[i] = ReadRetailFastfileLeFloat(wire + 4 + i * 4);
    item->rectHorzAlign = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 20));
    item->rectVertAlign = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 24));
    for (uint32_t i = 0; i < 4; ++i)
        item->rectClient[i] = ReadRetailFastfileLeFloat(wire + 28 + i * 4);
    item->rectClientHorzAlign = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 44));
    item->rectClientVertAlign = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 48));
    item->groupRef = ReadRetailFastfileLe32(wire + 52);
    item->style = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 56));
    item->border = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 60));
    item->ownerDraw = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 64));
    item->ownerDrawFlags = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 68));
    item->borderSize = ReadRetailFastfileLeFloat(wire + 72);
    item->staticFlags = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 76));
    item->dynamicFlags = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 80));
    item->nextTime = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 84));
    for (uint32_t i = 0; i < 4; ++i)
        item->foreColor[i] = ReadRetailFastfileLeFloat(wire + 88 + i * 4);
    for (uint32_t i = 0; i < 4; ++i)
        item->backColor[i] = ReadRetailFastfileLeFloat(wire + 104 + i * 4);
    for (uint32_t i = 0; i < 4; ++i)
        item->borderColor[i] = ReadRetailFastfileLeFloat(wire + 120 + i * 4);
    for (uint32_t i = 0; i < 4; ++i)
        item->outlineColor[i] = ReadRetailFastfileLeFloat(wire + 136 + i * 4);
    item->backgroundRef = ReadRetailFastfileLe32(wire + 152);
    // windowDef_t is 156 bytes and rectDef_s is 24 bytes on the retail wire.
    // Keep these offsets tied to Android's itemDef_s rather than host layout.
    item->type = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 180));
    item->dataType = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 184));
    item->alignment = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 188));
    item->fontEnum = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 192));
    item->textAlignMode = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 196));
    item->textalignx = ReadRetailFastfileLeFloat(wire + 200);
    item->textaligny = ReadRetailFastfileLeFloat(wire + 204);
    item->textscale = ReadRetailFastfileLeFloat(wire + 208);
    item->textStyle = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 212));
    item->gameMsgWindowIndex = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 216));
    item->gameMsgWindowMode = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 220));
    item->textRef = ReadRetailFastfileLe32(wire + 224);
    item->itemFlags = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 228));
    item->parentRef = ReadRetailFastfileLe32(wire + 232);
    uint32_t *refs[] = {&item->mouseEnterTextRef, &item->mouseExitTextRef,
        &item->mouseEnterRef, &item->mouseExitRef, &item->actionRef,
        &item->onAcceptRef, &item->onFocusRef, &item->leaveFocusRef,
        &item->dvarRef, &item->dvarTestRef, &item->onKeyRef};
    for (uint32_t i = 0; i < 11; ++i)
        *refs[i] = ReadRetailFastfileLe32(wire + 236 + i * 4);
    item->enableDvarRef = ReadRetailFastfileLe32(wire + 280);
    item->dvarFlags = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 284));
    item->focusSoundRef = ReadRetailFastfileLe32(wire + 288);
    item->special = ReadRetailFastfileLeFloat(wire + 292);
    item->cursorPos = ReadRetailFastfileLe32(wire + 296);
    item->typeDataRef = ReadRetailFastfileLe32(wire + 300);
    item->imageTrack = static_cast<int32_t>(ReadRetailFastfileLe32(wire + 304));
    for (uint32_t i = 0; i < 8; ++i)
    {
        item->statements[i].numEntries = ReadRetailFastfileLe32(wire + 308 + i * 8);
        item->statements[i].entriesRef = ReadRetailFastfileLe32(wire + 312 + i * 8);
    }
    // Com_Printf(0, "FS_ReadRetailFastfileItemDef: type=%d bg=0x%x snd=0x%x typeData=0x%x\n",
    //            item->type, item->backgroundRef, item->focusSoundRef, item->typeDataRef);

    FsRetailFastfileWireResult result = ReadRetailFastfileInlineName(reader, &item->nameRef);
    if (result != FS_RETAIL_FF_WIRE_OK) return result;
    result = ReadRetailFastfileInlineName(reader, &item->groupRef);
    if (result != FS_RETAIL_FF_WIRE_OK) return result;
    item->backgroundWasInline = 0;
    if (item->backgroundRef == RETAIL_FASTFILE_INLINE_REF ||
        item->backgroundRef == RETAIL_FASTFILE_INSERT_REF)
    {
        result = FS_ReadRetailFastfileMaterial(reader, item->backgroundRef, &item->backgroundInlineMaterial,
                                               item->backgroundInlineTextures, 8);
        if (result != FS_RETAIL_FF_WIRE_OK)
        {
            Com_Printf(0, "FS_ReadRetailFastfileItemDef: FS_ReadRetailFastfileMaterial failed res=%d\n", result);
            return result;
        }
        item->backgroundWasInline = 1;
        item->backgroundRef = item->backgroundInlineMaterial.nameRef;
    }
    result = ReadRetailFastfileInlineName(reader, &item->textRef);
    if (result != FS_RETAIL_FF_WIRE_OK) return result;
    for (uint32_t i = 0; i < 10; ++i)
    {
        result = ReadRetailFastfileInlineName(reader, refs[i]);
        if (result != FS_RETAIL_FF_WIRE_OK) return result;
    }
    result = ReadRetailFastfileKeyHandlerChain(reader, item->onKeyRef, handlers,
                                               handlerCapacity, &item->handlerCount);
    if (result != FS_RETAIL_FF_WIRE_OK) return result;
    result = ReadRetailFastfileInlineName(reader, &item->enableDvarRef);
    if (result != FS_RETAIL_FF_WIRE_OK) return result;
    if (item->focusSoundRef == RETAIL_FASTFILE_INLINE_REF ||
        item->focusSoundRef == RETAIL_FASTFILE_INSERT_REF)
    {
        Com_Printf(0, "FS_ReadRetailFastfileItemDef: item focusSoundRef=0x%x is INLINE/INSERT\n", item->focusSoundRef);
        return FS_RETAIL_FF_WIRE_UNSUPPORTED_FORM;
    }
    result = ReadRetailFastfileItemTypeData(reader, item->type, &item->typeDataRef,
                                            &item->typeDataOffset);
    if (result != FS_RETAIL_FF_WIRE_OK)
    {
        Com_Printf(0, "FS_ReadRetailFastfileItemDef: ReadRetailFastfileItemTypeData failed res=%d type=%d typeDataRef=0x%x\n",
                   result, item->type, item->typeDataRef);
        return result;
    }
    item->expressionEntryCount = 0;
    for (uint32_t i = 0; i < 8; ++i)
    {
        result = ReadRetailFastfileStatement(reader, &item->statements[i], entries,
                                             entryCapacity, &item->expressionEntryCount);
        if (result != FS_RETAIL_FF_WIRE_OK) return result;
    }
    return FS_RETAIL_FF_WIRE_OK;
}

uint32_t __cdecl FS_RetailFastfileImagePoolCount(const FsRetailFastfileReader *reader)
{
    return reader ? reader->imagePoolCount : 0;
}

FsRetailFastfileWireResult __cdecl FS_RetailFastfileImagePoolNameRef(
    const FsRetailFastfileReader *reader, uint32_t poolRef, uint32_t *nameRef)
{
    if (!reader || !nameRef)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    if ((poolRef >> 28) != RETAIL_FASTFILE_POOL_BLOCK)
        return FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED;
    const uint32_t poolIndex = (poolRef - 1u) & 0x0fffffffu;
    if (poolIndex >= reader->imagePoolCount)
        return FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED;
    *nameRef = reader->imagePoolNameRefs[poolIndex];
    return FS_RETAIL_FF_WIRE_OK;
}

FsRetailFastfileWireResult __cdecl FS_RetailFastfileRegisterImagePoolSlot(
    FsRetailFastfileReader *reader, uint32_t nameRef, uint32_t slotOffset, uint32_t *outPoolRef)
{
    if (!reader || !outPoolRef)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    if (slotOffset + 4u > reader->blockSizes[4])
        return FS_RETAIL_FF_WIRE_DIRECTORY_TRUNCATED;
    if (reader->imagePoolCount >= RETAIL_FASTFILE_IMAGE_POOL_MAX)
        return FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL;
    const uint32_t poolIndex = reader->imagePoolCount++;
    reader->imagePoolNameRefs[poolIndex] = nameRef;
    const uint32_t poolRef = ((RETAIL_FASTFILE_POOL_BLOCK << 28) | poolIndex) + 1u;
    RetailFastfileWriteSlot(reader, 4, slotOffset, poolRef);
    *outPoolRef = poolRef;
    return FS_RETAIL_FF_WIRE_OK;
}

FsRetailFastfileWireResult __cdecl FS_RetailFastfileResolveImageAlias(
    FsRetailFastfileReader *reader, uint32_t imageRef, uint32_t *outAliasedRef)
{
    if (!reader || !outAliasedRef)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    if ((imageRef >> 28) != 4u)
        return FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED;
    const uint32_t aliasOffset = (imageRef - 1u) & 0x0fffffffu;
    if (aliasOffset + 4u > reader->blockSizes[4])
        return FS_RETAIL_FF_WIRE_DIRECTORY_TRUNCATED;
    const uint32_t targetRef = ReadRetailFastfileLe32(reader->blockData[4] + aliasOffset);
    const uint32_t poolIndex = (targetRef - 1u) & 0x0fffffffu;
    if ((targetRef >> 28) == RETAIL_FASTFILE_POOL_BLOCK &&
        poolIndex < reader->imagePoolCount)
    {
        *outAliasedRef = targetRef;
        return FS_RETAIL_FF_WIRE_OK;
    }
    return FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED;
}

FsRetailFastfileWireResult __cdecl FS_RetailFastfilePatchImagePoolSlot(
    FsRetailFastfileReader *reader, uint32_t poolRef, uint32_t slotOffset)
{
    if (!reader)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    const uint32_t poolIndex = (poolRef - 1u) & 0x0fffffffu;
    if ((poolRef >> 28) != RETAIL_FASTFILE_POOL_BLOCK ||
        poolIndex >= reader->imagePoolCount)
        return FS_RETAIL_FF_WIRE_ALIAS_UNRESOLVED;
    if (slotOffset + 4u > reader->blockSizes[4])
        return FS_RETAIL_FF_WIRE_DIRECTORY_TRUNCATED;
    RetailFastfileWriteSlot(reader, 4, slotOffset, poolRef);
    return FS_RETAIL_FF_WIRE_OK;
}

FsRetailFastfileWireResult __cdecl FS_RetailFastfileReserveBlock4Slot(
    FsRetailFastfileReader *reader, uint32_t *outOffset)
{
    if (!reader || !outOffset)
        return FS_RETAIL_FF_WIRE_READER_MISSING;
    reader->blockCursor[4] = (reader->blockCursor[4] + 3u) & ~3u;
    if (reader->blockCursor[4] + 4u > reader->blockSizes[4])
        return FS_RETAIL_FF_WIRE_BODY_TRUNCATED;
    *outOffset = reader->blockCursor[4];
    reader->blockCursor[4] += 4;
    return FS_RETAIL_FF_WIRE_OK;
}

void __cdecl FS_CloseRetailFastfile(FsRetailFastfileReader *reader)
{
    if (!reader)
        return;
#ifdef KISAK_RETAIL_FS_PROOF_HOST
    // pointer-provenance generalization: audit zone-lifetime storage
    // while this reader's blocks are still live and tracked -- the exact
    // close moment a retained transient pointer (the retained-pointer class) starts to
    // dangle. Runs before untracking so the scan can see this reader.
    if (g_retailFastfileTransientAuditHook)
        g_retailFastfileTransientAuditHook(reader);
    RetailFastfileUntrackReader(reader);
#endif
    RetailLoadPublish(nullptr, 0, 0);
    inflateEnd(&reader->stream);
    FS_FCloseFile(reader->file);
    for (uint32_t block = 0; block < 9; ++block)
    {
        if (!reader->blockData[block])
            continue;
        // sweep #3: poison every
        // transient block before it is freed. A registered asset that
        // retained a raw pointer into one of them (the retained-pointer shape) then reads
        // recognizable 0xDD at its use point instead of freed-heap contents,
        // turning a delayed PrintWaitedError/crash into an immediate,
        // recognizable failure. The host proof counters below verify the
        // poison actually landed on each close.
        const uint32_t size = reader->blockSizes[block];
        if (size)
        {
            Com_Memset(reader->blockData[block], 0xDD, size);
#ifdef KISAK_RETAIL_FS_PROOF_HOST
            ++g_retailFastfilePoisonBlocks;
            g_retailFastfilePoisonBytes += size;
            if (reader->blockData[block][0] != 0xDD ||
                reader->blockData[block][size / 2u] != 0xDD ||
                reader->blockData[block][size - 1u] != 0xDD)
                ++g_retailFastfilePoisonFailures;
#endif
        }
        Z_Free(reader->blockData[block], 0);
    }
    Z_Free(reader, 0);
}

FsRetailFastfileResult __cdecl FS_OpenRetailFastfile(const char *filename, FsRetailFastfileReader **reader)
{
    static const uint8_t magic[8] = {'I', 'W', 'f', 'f', 'u', '1', '0', '0'};
    uint8_t containerHeader[12];
    uint8_t xfileHeader[RETAIL_FASTFILE_XFILE_HEADER_BYTES];
    FsRetailFastfileReader *state;

    if (!reader)
        return FS_RETAIL_FF_MISSING;
    *reader = NULL;
    state = static_cast<FsRetailFastfileReader *>(Z_Malloc(sizeof(*state), __FILE__, __LINE__));
    Com_Memset(state, 0, sizeof(*state));
    const uint32_t fileLen = FS_FOpenRetailFileRead(filename, &state->file);
    if (fileLen == static_cast<uint32_t>(-1))
    {
        Com_Printf(0, "FS_OpenRetailFastfile: FS_FOpenRetailFileRead('%s') failed\n", filename);
        Z_Free(state, 0);
        return FS_RETAIL_FF_MISSING;
    }
    const uint32_t readBytes = FS_Read(containerHeader, sizeof(containerHeader), state->file);
    if (readBytes != sizeof(containerHeader))
    {
        Com_Printf(0, "FS_OpenRetailFastfile: FS_Read returned %u != %zu (fileLen=%u file=%d)\n",
                   readBytes, sizeof(containerHeader), fileLen, state->file);
        FS_FCloseFile(state->file);
        Z_Free(state, 0);
        return FS_RETAIL_FF_TRUNCATED;
    }
    if (memcmp(containerHeader, magic, sizeof(magic)) != 0)
    {
        FS_FCloseFile(state->file);
        Z_Free(state, 0);
        return FS_RETAIL_FF_WRONG_MAGIC;
    }
    const uint32_t version = ReadRetailFastfileLe32(containerHeader + 8);
    if (version != 5)
    {
        FS_FCloseFile(state->file);
        Z_Free(state, 0);
        return FS_RETAIL_FF_WRONG_VERSION;
    }

    if (inflateInit(&state->stream) != Z_OK)
    {
        FS_FCloseFile(state->file);
        Z_Free(state, 0);
        return FS_RETAIL_FF_ZLIB_INIT;
    }
    const uint32_t xfileBytes = FS_ReadRetailFastfile(state, xfileHeader, sizeof(xfileHeader));
    if (xfileBytes != sizeof(xfileHeader))
    {
        const int zlibResult = state->zlibResult;
        const bool sourceExhausted = state->sourceExhausted;
        FS_CloseRetailFastfile(state);
        if (zlibResult != Z_OK && zlibResult != Z_STREAM_END)
            return FS_RETAIL_FF_ZLIB_ERROR;
        return zlibResult == Z_STREAM_END ? FS_RETAIL_FF_XFILE_HEADER_TRUNCATED :
            (sourceExhausted ? FS_RETAIL_FF_ZLIB_TRUNCATED : FS_RETAIL_FF_ZLIB_ERROR);
    }
    state->xfileSize = ReadRetailFastfileLe32(xfileHeader);
    state->xfileExternalSize = ReadRetailFastfileLe32(xfileHeader + 4);
    for (uint32_t block = 0; block < 9; ++block)
    {
        state->blockSizes[block] = ReadRetailFastfileLe32(xfileHeader + 8 + block * 4);
        state->blockTotal += state->blockSizes[block];
    }
    if (state->blockTotal > RETAIL_FASTFILE_BLOCK_CAP)
    {
        FS_CloseRetailFastfile(state);
        return FS_RETAIL_FF_BLOCK_TOTAL_OVER_CAP;
    }
    // Mirror only the blocks this FS-level reader itself writes: block 0 (the
    // temp asset-body block its bounded readers stream roots into) and block
    // 4 (inline names, directory, technique/material/image bodies, pool
    // slots). The walk streams every other block straight into zone memory
    // and never reads a reader mirror of it, so allocating, zeroing and
    // poisoning those (blocks 1/7/8: 21 MB on cargoship, 30 MB on killhouse)
    // was pure transient cost. blockSizes stays exact for every block.
    for (uint32_t block = 0; block < 9; ++block)
    {
        if (state->blockSizes[block] && (block == 0 || block == 4))
        {
            state->blockData[block] = static_cast<uint8_t *>(Z_Malloc(state->blockSizes[block], __FILE__, __LINE__));
            Com_Memset(state->blockData[block], 0, state->blockSizes[block]);
        }
    }
#ifdef KISAK_RETAIL_FS_PROOF_HOST
    // Track only now that every failure path above has already released the
    // state; FS_CloseRetailFastfile untracks before the transient blocks are
    // freed, so a pointer into them is detectable while they are live.
    RetailFastfileTrackReader(state);
#endif
    *reader = state;
    // Publish the Quake-style loading readout: basename only, percent
    // follows from FS_ReadRetailFastfile as the stream is consumed.
    const char *label = filename;
    for (const char *slash = filename; *slash; ++slash)
        if (*slash == '/' || *slash == '\\')
            label = slash + 1;
    I_strncpyz(state->loadLabel, label, sizeof(state->loadLabel));
    state->loadPercentShown = 0;
    RetailLoadPublish(state->loadLabel, 0, 1);
    // Size the retail loadbar for this zone (retail: DB_LoadXFileInternal
    // right after reading the XFile header).
    state->fileBytes = fileLen;
    state->fileBytesRead += sizeof(containerHeader);
    g_retailLoadFileBytes.store(state->fileBytes, std::memory_order_release);
    g_retailLoadExternalBytes.store(state->xfileExternalSize, std::memory_order_release);
    RetailLoadPublishBytes(state);
    g_retailLoadSized.store(1, std::memory_order_release);
    return FS_RETAIL_FF_OK;
}
