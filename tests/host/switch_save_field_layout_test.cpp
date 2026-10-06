// Host ASan/UBSan proof for the native save-field conversion.
//
// WriteField1 must walk native pointer-sized fields and use native array
// strides.  In particular, the old ILP32 sentient stride (116) turns a valid
// pointer into a bogus index on LP64, while writing only four bytes leaves a
// stale high half in the serialized pointer slot.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "src/game/g_save.h"
#include "src/game/g_main.h"
#include "src/game/g_scr_main.h"
#include "src/game/game_public.h"
#include "src/game/savememory.h"

gentity_s g_entities[MAX_GENTITIES]{};
level_locals_t level{};
scr_data_t g_scr_data{};

void Com_Error(errorParm_t, const char *, ...)
{
    std::fprintf(stderr, "unexpected Com_Error in save-field proof\n");
    std::abort();
}

void MyAssertHandler(const char *, int, int, const char *, ...)
{
    std::fprintf(stderr, "unexpected MyAssertHandler in save-field proof\n");
    std::abort();
}

void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
bool EntHandle::isDefined() const { return false; }
int32_t EntHandle::entnum() { return 0; }
bool SentientHandle::isDefined() const { return false; }
sentient_s *SentientHandle::sentient() const { return nullptr; }
int Scr_ConvertThreadToSave(unsigned short value) { return value; }
int Path_SaveIndex(const pathnode_t *) { return 0; }
XAnim_s *XAnimGetAnims(const XAnimTree_s *) { return nullptr; }
int Scr_GetAnimsIndex(const XAnim_s *) { return 0; }

// In-memory stand-ins for the save stream: the production G_SaveHudElems /
// G_LoadHudElems call these with their raw block.
game_hudelem_s g_hudelems[MAX_HUDELEMS_TOTAL];
static unsigned char g_stream[1 << 17];
static size_t g_streamWrite, g_streamRead;
void SaveMemory_SaveWrite(const void *buffer, int len, SaveGame *)
{
    std::memcpy(g_stream + g_streamWrite, buffer, (size_t)len);
    g_streamWrite += (size_t)len;
}
void SaveMemory_LoadRead(void *buffer, int len, SaveGame *)
{
    std::memcpy(buffer, g_stream + g_streamRead, (size_t)len);
    g_streamRead += (size_t)len;
}

static int hudelemRoundTrip()
{
    static_assert(sizeof(game_hudelem_s) == 172, "retail hud element record is 172 bytes");
    static_assert(MAX_HUDELEMS_TOTAL * sizeof(game_hudelem_s) == 44032, "retail hud pool block is 44032 bytes");
    static_assert(sizeof(game_hudelem_s) == sizeof(hudelem_s), "SP game_hudelem_s adds no fields");

    for (int i = 0; i < MAX_HUDELEMS_TOTAL; ++i)
    {
        auto *b = reinterpret_cast<unsigned char *>(&g_hudelems[i]);
        for (size_t k = 0; k < sizeof(game_hudelem_s); ++k)
            b[k] = (unsigned char)(i * 31 + k * 7 + 1);
    }
    static unsigned char expect[sizeof(g_hudelems)];
    std::memcpy(expect, g_hudelems, sizeof(expect));

    g_streamWrite = g_streamRead = 0;
    G_SaveHudElems(nullptr);
    if (g_streamWrite != 44032)
    {
        std::fprintf(stderr, "FAIL:SAVE_HUDELEM stage=written-size value=%zu\n", g_streamWrite);
        return 1;
    }
    std::memset(g_hudelems, 0, sizeof(g_hudelems));
    G_LoadHudElems(nullptr);
    if (g_streamRead != g_streamWrite || std::memcmp(expect, g_hudelems, sizeof(expect)))
    {
        std::fprintf(stderr, "FAIL:SAVE_HUDELEM stage=round-trip\n");
        return 1;
    }

    // Negative control: a reader one record short must be caught by the same
    // comparison (the last record stays zero).
    std::memset(g_hudelems, 0, sizeof(g_hudelems));
    std::memcpy(g_hudelems, g_stream, sizeof(g_hudelems) - sizeof(game_hudelem_s));
    if (!std::memcmp(expect, g_hudelems, sizeof(expect)))
    {
        std::fprintf(stderr, "FAIL:SAVE_HUDELEM stage=negative-control-not-detected\n");
        return 1;
    }
    std::printf("PASS:SAVE_HUDELEM record=%zu pool=%d bytes=%zu\n", sizeof(game_hudelem_s),
                (int)MAX_HUDELEMS_TOTAL, g_streamWrite);
    return 0;
}

int main()
{
#if UINTPTR_MAX == UINT32_MAX
    std::fprintf(stderr, "FAIL:SAVE_FIELD_LAYOUT stage=host-not-lp64\n");
    return 1;
#else
    static_assert(sizeof(uintptr_t) == 8, "the proof must exercise the LP64 ABI");
    static_assert(sizeof(sentient_s) == 144, "sentient native layout drifted");

    sentient_s sentients[MAX_SENTIENTS]{};
    level.sentients = sentients;

    alignas(gentity_s) unsigned char source[sizeof(gentity_s)]{};
    auto **slot = reinterpret_cast<sentient_s **>(source);
    *slot = &sentients[7];

    saveField_t field{0, SF_SENTIENT};
    WriteField1(&field, source, source);

    const auto encoded = *reinterpret_cast<uintptr_t *>(source);
    if (encoded != 8)
    {
        std::fprintf(stderr, "FAIL:SAVE_FIELD_LAYOUT stage=sentient-index value=%llu\n",
                     static_cast<unsigned long long>(encoded));
        return 1;
    }
    if (reinterpret_cast<uintptr_t *>(source)[0] >> 32 != 0)
    {
        std::fprintf(stderr, "FAIL:SAVE_FIELD_LAYOUT stage=sentient-high-half\n");
        return 1;
    }

    std::printf("PASS:SAVE_FIELD_LAYOUT abi=lp64 sentient_stride=%zu encoded=%llu slot=%zu\n",
                sizeof(sentient_s), static_cast<unsigned long long>(encoded), sizeof(uintptr_t));
    return hudelemRoundTrip();
#endif
}
