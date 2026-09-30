// Host probe for a retail savegame file, using the engine's own SaveHeader.
//
// Every load starts with three checks (savememory.cpp:499/507/533): version
// 287, bodySize <= 1572864, and a full-body read.  This probe answers those
// three plus the segment table, the trailer, and any further save records in
// the same file, so a save the loader would reject is identified without a
// device or emulator run.
//
// Usage: switch_save_file_probe <file.svg> [more.svg ...]
// Exit:  1 if any file is structurally unloadable, 0 otherwise.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "src/client/client.h"

namespace
{

constexpr uint32_t kSaveBodyLimit = 1572864u;
constexpr int kExpectedVersion = 287;
// memfile.cpp:158 SAVE_SEGMENT_COUNT (file-local there).
constexpr int kSaveSegmentCount = 8;

std::string Trimmed(const char *text, size_t size)
{
    size_t length = 0;
    while (length < size && text[length])
        ++length;
    return std::string(text, length);
}

bool ReadWholeFile(const char *path, std::vector<unsigned char> &out)
{
    FILE *f = std::fopen(path, "rb");
    if (!f)
        return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < 0)
    {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t got = out.empty() ? 0u : std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

}  // namespace

int main(int argc, char **argv)
{
    // Line-buffer stdout even though it is redirected to a regular file (the
    // libc default there is fully-buffered): the FAIL/PASS line below goes to
    // stderr, which is unbuffered, and a fully-buffered stdout can otherwise
    // flush a multi-hundred-byte line out of order relative to it, tearing
    // that line in the combined log (`prog >>log 2>&1`) with no data lost,
    // just reordered -- confusing but not a real failure.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <save.svg> [more.svg ...]\n", argv[0]);
        return 2;
    }

    size_t files = 0;
    size_t unloadable = 0;

    for (int i = 1; i < argc; ++i)
    {
        std::vector<unsigned char> bytes;
        if (!ReadWholeFile(argv[i], bytes))
        {
            std::printf("SAVE_FILE_PROBE path=%s error=unreadable\n", argv[i]);
            ++files;
            ++unloadable;
            continue;
        }

        ++files;
        const size_t size = bytes.size();
        const size_t headerBytes = sizeof(SaveHeader);
        if (size < headerBytes)
        {
            std::printf("SAVE_FILE_PROBE path=%s bytes=%zu error=shorter-than-header headerBytes=%zu\n",
                        argv[i], size, headerBytes);
            ++unloadable;
            continue;
        }

        SaveHeader header;
        std::memcpy(&header, bytes.data(), sizeof(header));

        const char *verdict = "ok";
        if (header.saveVersion != kExpectedVersion)
            verdict = "bad-version";
        else if (header.bodySize < 0 || static_cast<uint32_t>(header.bodySize) > kSaveBodyLimit)
            verdict = "body-too-large";
        else if (size < headerBytes + static_cast<size_t>(header.bodySize))
            verdict = "truncated-body";

        const long long trailer = static_cast<long long>(size) - static_cast<long long>(headerBytes)
                                  - static_cast<long long>(header.bodySize < 0 ? 0 : header.bodySize);

        // Segment walk: each segment starts with a u32 total length that
        // includes its own four bytes (memfile.cpp:187-192/292); the chain must
        // land exactly on bodySize or the loader's MoveToSegment walks off.
        // This and the extra-record scan below must finish before the
        // verdict is printed, or the printed line can say "ok" for a file the
        // summary still counts unloadable (that happened for real on
        // ambush.svg before this fix).
        // True once the three header checks above pass: bodySize is then
        // trustworthy as the trailer's start offset, whatever the segment
        // walk below goes on to find inside the body.
        const bool bodyBoundsValid = std::strcmp(verdict, "ok") == 0;

        int segment = 0;
        size_t segmentsCovered = 0;
        if (bodyBoundsValid)
        {
            size_t offset = 0;
            const size_t bodyEnd = static_cast<size_t>(header.bodySize);
            const unsigned char *body = bytes.data() + headerBytes;
            while (offset < bodyEnd)
            {
                if (offset + 4 > bodyEnd)
                {
                    verdict = "segment-header-past-body";
                    break;
                }
                uint32_t length = 0;
                std::memcpy(&length, body + offset, sizeof(length));
                if (length < 4 || offset + length > bodyEnd)
                {
                    verdict = "segment-length-out-of-range";
                    break;
                }
                offset += length;
                ++segment;
                if (segment > kSaveSegmentCount)
                {
                    verdict = "too-many-segments";
                    break;
                }
            }
            if (std::strcmp(verdict, "ok") == 0 && offset != bodyEnd)
                verdict = "segments-do-not-cover-body";
            segmentsCovered = offset;
        }

        // A second save record appended after this one's trailer means the
        // writer appended instead of replacing (rename/copy failure), which
        // makes every later read parse a stale record. Scan only the trailer
        // (the bytes past headerBytes+bodySize), never the body: the body is
        // arbitrary game state (entity names, script strings, checksums, ...)
        // and a 4-byte-aligned (version==287, plausible bodySize) pattern
        // turns up in it by pure coincidence on real saves -- that is exactly
        // what happened to ambush.svg's body around offset 479220 (a
        // "trigger" entity-name string plus adjacent numeric fields), which
        // is why this used to scan from headerBytes across the whole file.
        size_t extraRecords = 0;
        std::vector<std::pair<size_t, uint32_t>> extraRecordHits;
        if (bodyBoundsValid)
        {
            const size_t trailerStart = headerBytes + static_cast<size_t>(header.bodySize);
            for (size_t offset = trailerStart; offset + 8 <= size; offset += 4)
            {
                uint32_t version = 0;
                uint32_t bodySize = 0;
                std::memcpy(&version, bytes.data() + offset, sizeof(version));
                std::memcpy(&bodySize, bytes.data() + offset + 4, sizeof(bodySize));
                if (version == kExpectedVersion && bodySize > 0 && bodySize <= kSaveBodyLimit)
                {
                    ++extraRecords;
                    extraRecordHits.emplace_back(offset, bodySize);
                }
            }
            if (extraRecords)
                verdict = "extra-save-records";
        }

        std::printf(
            "SAVE_FILE_PROBE path=%s verdict=%s bytes=%zu headerBytes=%zu version=%d bodySize=%d "
            "trailer=%lld map='%s' campaign='%s' desc='%s' file='%s' build='%s' health=%d skill=%d "
            "internal=%d demo=%d scriptChecksum=%d gameCheckSum=0x%08x saveCheckSum=0x%08x "
            "scrCheckSum=%u,%u,%u\n",
            argv[i], verdict, size, headerBytes, header.saveVersion, header.bodySize, trailer,
            Trimmed(header.mapName, sizeof(header.mapName)).c_str(),
            Trimmed(header.campaign, sizeof(header.campaign)).c_str(),
            Trimmed(header.description, sizeof(header.description)).c_str(),
            Trimmed(header.filename, sizeof(header.filename)).c_str(),
            Trimmed(header.buildNumber, sizeof(header.buildNumber)).c_str(),
            header.health, header.skill, header.internalSave ? 1 : 0, header.demoPlayback ? 1 : 0,
            header.isUsingScriptChecksum ? 1 : 0,
            static_cast<unsigned>(header.gameCheckSum), static_cast<unsigned>(header.saveCheckSum),
            static_cast<unsigned>(header.scrCheckSum[0]), static_cast<unsigned>(header.scrCheckSum[1]),
            static_cast<unsigned>(header.scrCheckSum[2]));

        if (bodyBoundsValid)
            std::printf("SAVE_FILE_PROBE_SEGMENTS path=%s segments=%d covered=%zu bodySize=%d\n",
                        argv[i], segment, segmentsCovered, header.bodySize);

        for (const auto &hit : extraRecordHits)
            std::printf("SAVE_FILE_PROBE_RECORD path=%s offset=%zu bodySize=%u\n",
                        argv[i], hit.first, hit.second);

        std::printf("SAVE_FILE_PROBE_TRAILER path=%s trailer=%lld extraRecords=%zu\n",
                    argv[i], trailer, extraRecords);

        if (std::strcmp(verdict, "ok") != 0)
            ++unloadable;
    }

    std::printf("SAVE_FILE_PROBE_SUMMARY files=%zu unloadable=%zu\n", files, unloadable);
    if (unloadable != 0)
    {
        std::fprintf(stderr, "FAIL:SAVE_FILE_PROBE files=%zu unloadable=%zu\n", files, unloadable);
        return 1;
    }
    std::printf("PASS:SAVE_FILE_PROBE files=%zu unloadable=0\n", files);
    return 0;
}
