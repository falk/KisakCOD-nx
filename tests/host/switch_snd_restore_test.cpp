// Host ASan/UBSan proof for the sound savegame round trip (SND_Save ->
// SND_Restore, src/sound/snd.cpp) on LP64 with every object above 4 GiB.
//
// Hardware (checkpoint reload after death):
// G_LoadGame died in "SND_GetAliasWithOffset: could not find sound alias
// '<16 bytes>'", the 16 bytes being two identical {goalvolume, goalrate}
// float pairs.  SND_Save writes the whole g_snd.background[SND_TRACK_COUNT]
// array (40 bytes) but SND_Restore read one 8-byte record, so the 3D-channel
// loop read the ambient-track goals as the first saved alias name.  It only
// shows when an ambient track has a nonzero goal (both ambient tracks
// fading), which is why earlier load proofs passed.
//
// This runs the real SND_Save/SND_Restore over a byte stream with one live
// 3D channel (alias pair + script and subtitle length notifies), fading
// ambient tracks and a music goal, and requires: every alias the restore
// looks up is a saved alias name, the stream is consumed exactly, the music
// goal is restored, the live ambient goals are left alone, and the script
// notify's entity number round-trips through the 8-byte data slot.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>

#include "src/sound/snd_local.h"
#include "src/universal/com_sndalias.h"

namespace
{
std::vector<uint8_t> g_bytes;
size_t g_readPos;
std::vector<std::string> g_lookups;
snd_alias_t *g_aliases[2];

[[noreturn]] void Fail(const char *stage, const char *detail = "")
{
    std::fprintf(stderr, "FAIL:SND_RESTORE_LP64 stage=%s %s\n", stage, detail);
    std::exit(1);
}
} // namespace

void Com_Error(errorParm_t, const char *fmt, ...)
{
    char text[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    Fail("com-error", text);
}

void Com_Printf(int, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
}

void Com_PrintError(int, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void MyAssertHandler(const char *file, int line, int, const char *fmt, ...)
{
    char text[256];
    std::snprintf(text, sizeof(text), "file=%s line=%d fmt=%s", file, line, fmt);
    Fail("assert", text);
}

// Byte-stream MemoryFile (the segment/compression layer is memfile.cpp's and
// is covered elsewhere; this proof is about what the sound archive writes).
void MemFile_WriteData(MemoryFile *, int byteCount, const void *p)
{
    const uint8_t *bytes = static_cast<const uint8_t *>(p);
    g_bytes.insert(g_bytes.end(), bytes, bytes + byteCount);
}

void MemFile_WriteCString(MemoryFile *memFile, const char *string)
{
    MemFile_WriteData(memFile, (int)std::strlen(string) + 1, string);
}

void MemFile_ReadData(MemoryFile *, int byteCount, uint8_t *p)
{
    if (g_readPos + (size_t)byteCount > g_bytes.size())
        Fail("read-past-end");
    std::memcpy(p, g_bytes.data() + g_readPos, (size_t)byteCount);
    g_readPos += (size_t)byteCount;
}

double MemFile_ReadFloat(MemoryFile *memFile)
{
    float value;
    MemFile_ReadData(memFile, 4, reinterpret_cast<uint8_t *>(&value));
    return value;
}

const char *MemFile_ReadCString(MemoryFile *)
{
    const char *s = reinterpret_cast<const char *>(g_bytes.data() + g_readPos);
    const void *end = std::memchr(s, 0, g_bytes.size() - g_readPos);
    if (!end)
        Fail("cstring-past-end");
    g_readPos += std::strlen(s) + 1;
    return s;
}

// Alias database: two aliases, offset 0 in their lists.
int SND_GetAliasOffset(const snd_alias_t *) { return 0; }
snd_alias_t *SND_GetAliasWithOffset(const char *name, int offset)
{
    g_lookups.emplace_back(name);
    for (snd_alias_t *alias : g_aliases)
    {
        if (!std::strcmp(alias->aliasName, name) && offset == 0)
            return alias;
    }
    char text[160];
    std::snprintf(text, sizeof(text), "name_bytes=%zu offset=%d", std::strlen(name), offset);
    Fail("unknown-alias", text);
}

// Driver side: one busy 3D channel (8), free stream channel 40.
bool SND_Is3DChannelFree(int index) { return index != 8; }
bool SND_Is2DChannelFree(int) { return true; }
bool SND_IsStreamChannelFree(int) { return true; }
int SND_Get3DChannelLength(int) { return 1000; }
int SND_Get2DChannelLength(int) { return 0; }
int SND_GetStreamChannelLength(int) { return 0; }
int SND_Get3DChannelPlaybackRate(int) { return 44100; }
int SND_Get2DChannelPlaybackRate(int) { return 0; }
void SND_Get3DChannelSaveInfo(int, snd_save_3D_sample_t *info)
{
    *info = {};
    info->fraction = 0.25f;
    info->pitch = 1.0f;
    info->volume = 0.5f;
}
void SND_Get2DChannelSaveInfo(int, snd_save_2D_sample_t *info) { *info = {}; }
void SND_GetStreamChannelSaveInfo(int, snd_save_stream_t *info) { *info = {}; }
void SND_SaveEq(MemoryFile *memFile) { MemFile_WriteCString(memFile, "eq"); }
void SND_RestoreEq(MemoryFile *memFile)
{
    if (std::strcmp(MemFile_ReadCString(memFile), "eq"))
        Fail("eq-marker");
}
// Reachable from SND_Restore's start paths; this fixture's channel is a
// streamed alias on a 3D slot, so a start is never taken.
int SND_StartAlias3DSample(SndStartAliasInfo *, int *) { Fail("unexpected-start-3d"); }
int SND_StartAlias2DSample(SndStartAliasInfo *, int *) { Fail("unexpected-start-2d"); }
int SND_StartAliasStreamOnChannel(SndStartAliasInfo *, int) { Fail("unexpected-start-stream"); }
void SND_Set2DChannelFromSaveInfo(int, snd_save_2D_sample_t *) { Fail("unexpected-set-2d"); }
void SND_SetStreamChannelFromSaveInfo(int, snd_save_stream_t *) { Fail("unexpected-set-stream"); }
void SND_SetStreamChannelPlaybackRate(int, int) { Fail("unexpected-stream-rate"); }
void SND_StopStreamChannel(int) { Fail("unexpected-stream-stop"); }
void CG_GetSoundEntityOrientation(SndEntHandle, float *, float (*)[3]) { Fail("unexpected-ent-orientation"); }
void Com_DPrintf(int, const char *, ...) {}
int I_stricmp(const char *a, const char *b) { return strcasecmp(a, b); }
char *va(const char *format, ...)
{
    static char text[256];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    return text;
}
void Vec3Copy(const float *a, float *b) { b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; }
void Vec3Sub(const float *a, const float *b, float *c) { for (int i = 0; i < 3; ++i) c[i] = a[i] - b[i]; }
void Vec3Mad(const float *a, float s, const float *b, float *c) { for (int i = 0; i < 3; ++i) c[i] = a[i] + s * b[i]; }
float Vec3LengthSq(const float *v) { return v[0] * v[0] + v[1] * v[1] + v[2] * v[2]; }
int g_roomtype = -1;
void SND_SetRoomtype(int roomtype) { g_roomtype = roomtype; }

int main()
{
#if UINTPTR_MAX == UINT32_MAX
    Fail("host-not-lp64");
#else
    SoundFile *file = static_cast<SoundFile *>(std::calloc(1, sizeof(SoundFile)));
    file->type = SAT_STREAMED; // not SAT_LOADED: the restore decodes, then skips the start
    file->exists = 1;
    const char *names[2] = { "amb_cargo_creak", "cargoship_sub_line" };
    for (int i = 0; i < 2; ++i)
    {
        g_aliases[i] = static_cast<snd_alias_t *>(std::calloc(1, sizeof(snd_alias_t)));
        g_aliases[i]->aliasName = strdup(names[i]);
        g_aliases[i]->soundFile = file;
        g_aliases[i]->flags = SAT_STREAMED << 6;
    }
    if (reinterpret_cast<uintptr_t>(g_aliases[0]) <= UINT32_MAX ||
        reinterpret_cast<uintptr_t>(g_aliases[0]->aliasName) <= UINT32_MAX)
        Fail("fixture-below-4g");

    g_snd.Initialized2d = 1;
    g_snd.max_3D_channels = 1;
    g_snd.max_2D_channels = 0;
    g_snd.max_stream_channels = 1;
    g_snd.effect = &g_snd.envEffects[0];
    g_snd.envEffects[1].active = 1;
    g_snd.envEffects[1].roomtype = 7;

    snd_channel_info_t &chan = g_snd.chaninfo[8];
    chan.alias0 = g_aliases[0];
    chan.alias1 = g_aliases[0];
    chan.system = SASYS_CGAME;
    chan.sndEnt.field.entIndex = 321;
    chan.entchannel = 5;
    chan.basevolume = 0.75f;
    chan.lerp = 0.0f;
    chan.lengthNotifyInfo.count = 2;
    chan.lengthNotifyInfo.id[0] = SndLengthNotify_Script;
    chan.lengthNotifyInfo.data[0] = reinterpret_cast<void *>(uintptr_t{ 0x2b7 });
    chan.lengthNotifyInfo.id[1] = SndLengthNotify_Subtitle;
    chan.lengthNotifyInfo.data[1] = g_aliases[1];

    // A real in-game state: music fading, both ambient tracks of the active pair
    // at the same nonzero goal (the 16 bytes in the Com_Error), the other
    // pair idle and one leftover goal.
    const snd_background_info_t saved[SND_TRACK_COUNT] = {
        { 0.9f, 0.0005f }, { 0.452f, 0.000226f }, { 0.452f, 0.000226f }, { 0.0f, 0.0f }, { 0.3f, -0.001f },
    };
    std::memcpy(g_snd.background, saved, sizeof(saved));

    MemoryFile memFile{};
    SND_Save(&memFile);
    const size_t written = g_bytes.size();

    // Load into a later run: the live ambient goals belong to cgame's
    // current ambient and must survive the restore.
    const snd_background_info_t live[SND_TRACK_COUNT] = {
        { 0.0f, 0.0f }, { 0.61f, 0.0f }, { 0.61f, 0.0f }, { 0.2f, 0.0f }, { 0.0f, 0.0f },
    };
    std::memcpy(g_snd.background, live, sizeof(live));
    std::memset(&g_snd.chaninfo[8], 0, sizeof(g_snd.chaninfo[8]));
    g_readPos = 0;
    SND_Restore(&memFile);

    if (g_readPos != written)
    {
        char text[96];
        std::snprintf(text, sizeof(text), "read=%zu written=%zu", g_readPos, written);
        Fail("stream-not-consumed", text);
    }
    // alias0, alias1, the subtitle notify's alias.
    if (g_lookups.size() != 3 || g_lookups[0] != names[0] || g_lookups[1] != names[0] || g_lookups[2] != names[1])
        Fail("alias-lookups");
    if (std::memcmp(&g_snd.background[SND_TRACK_MUSIC], &saved[SND_TRACK_MUSIC], sizeof(saved[0])))
        Fail("music-goal-not-restored");
    for (int track = SND_TRACK_AMBIENT_PRIMARY_0; track < SND_TRACK_COUNT; ++track)
    {
        if (std::memcmp(&g_snd.background[track], &live[track], sizeof(live[0])))
            Fail("live-ambient-goal-overwritten");
    }
    if (g_roomtype != 7)
        Fail("roomtype");
    std::printf("PASS:SND_RESTORE_LP64 bytes=%zu lookups=%zu alias=%p\n", written, g_lookups.size(),
                (void *)g_aliases[0]);
    return 0;
#endif
}
