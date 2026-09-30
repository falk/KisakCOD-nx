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
    return 0;
#endif
}
