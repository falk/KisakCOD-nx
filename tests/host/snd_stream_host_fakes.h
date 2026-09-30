#pragma once
// Host fakes for src/sound/snd_stream_openal.cpp (KISAK_SND_STREAM_HOST_TEST).
// Definitions live in switch_snd_stream_test.cpp.
#include <cstddef>
#include <cstdint>

// --- engine ---------------------------------------------------------------
enum errorParm_t
{
    ERR_FATAL = 0,
    ERR_DROP = 1,
};
void Com_Printf(int channel, const char *fmt, ...);
[[noreturn]] void Com_Error(errorParm_t code, const char *fmt, ...);
void I_strncpyz(char *dest, const char *src, int destsize);

// FS_* with the engine's contract: retail seek origins (0 = current,
// 1 = end, 2 = set), stream handles limited to 13, open returns the file size
// or 0xFFFFFFFF.
uint32_t FS_FOpenFileReadStream(const char *filename, int *file);
uint32_t FS_Read(uint8_t *buffer, uint32_t len, int h);
int FS_Seek(int f, int offset, int origin);
uint32_t FS_FTell(int f);
void FS_FCloseFile(int h);

// --- OpenAL subset ----------------------------------------------------------
typedef unsigned int ALuint;
typedef int ALint;
typedef int ALenum;
typedef int ALsizei;
typedef void ALvoid;

#define AL_NO_ERROR 0
#define AL_INVALID_NAME 0xA001
#define AL_INVALID_VALUE 0xA003
#define AL_INVALID_OPERATION 0xA004
#define AL_BUFFER 0x1009
#define AL_SOURCE_STATE 0x1010
#define AL_INITIAL 0x1011
#define AL_PLAYING 0x1012
#define AL_PAUSED 0x1013
#define AL_STOPPED 0x1014
#define AL_BUFFERS_QUEUED 0x1015
#define AL_BUFFERS_PROCESSED 0x1016
#define AL_SAMPLE_OFFSET 0x1025
#define AL_FORMAT_MONO16 0x1101
#define AL_FORMAT_STEREO16 0x1103

void alGenBuffers(ALsizei n, ALuint *buffers);
void alDeleteBuffers(ALsizei n, const ALuint *buffers);
void alBufferData(ALuint buffer, ALenum format, const ALvoid *data, ALsizei size, ALsizei freq);
void alSourceQueueBuffers(ALuint source, ALsizei n, const ALuint *buffers);
void alSourceUnqueueBuffers(ALuint source, ALsizei n, ALuint *buffers);
void alSourcei(ALuint source, ALenum param, ALint value);
void alGetSourcei(ALuint source, ALenum param, ALint *value);
void alSourcePlay(ALuint source);
void alSourcePause(ALuint source);
void alSourceStop(ALuint source);
ALenum alGetError(void);
