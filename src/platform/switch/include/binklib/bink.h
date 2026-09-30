// Switch Bink type shim.
//
// The Bink AV codec is a licensed Win32 library excluded from the Horizon
// boot closure (binklib/bink.h cannot compile off Windows).  The renderer and
// client declarations still carry Bink types, so this shim mirrors the exact
// shape of the committed deps/binklib/bink.h definitions with fixed-width
// types.  No Bink functions are declared: any real codec use must fail loudly
// at link, never silently.
#pragma once

#ifndef __BINKH__
#define __BINKH__

#define BINKMAJORVERSION 1
#define BINKMINORVERSION 9
#define BINKSUBVERSION 9
#define BINKVERSION "1.9i"
#define BINKDATE    "2008-10-31"

#include <cstdint>

typedef int32_t BINKS32;
typedef uint32_t BINKU32;

struct BINK;
typedef struct BINK *HBINK;

struct BINKPLANE
{
    BINKS32 Allocate;
    void *Buffer;
    BINKU32 BufferPitch;
};

struct BINKFRAMEPLANESET
{
    BINKPLANE YPlane;
    BINKPLANE cRPlane;
    BINKPLANE cBPlane;
    BINKPLANE APlane;
};

#define BINKMAXFRAMEBUFFERS 2

struct BINKFRAMEBUFFERS
{
    BINKS32 TotalFrames;
    BINKU32 YABufferWidth;
    BINKU32 YABufferHeight;
    BINKU32 cRcBBufferWidth;
    BINKU32 cRcBBufferHeight;
    BINKU32 FrameNum;
    BINKFRAMEPLANESET Frames[BINKMAXFRAMEBUFFERS];
};

struct BINKREALTIME
{
    BINKU32 FrameNum;
    BINKU32 FrameRate;
    BINKU32 FrameRateDiv;
    BINKU32 Frames;
    BINKU32 FramesTime;
    BINKU32 FramesVideoDecompTime;
    BINKU32 FramesAudioDecompTime;
    BINKU32 FramesReadTime;
    BINKU32 FramesIdleReadTime;
    BINKU32 FramesThreadReadTime;
    BINKU32 FramesBlitTime;
    BINKU32 ReadBufferSize;
    BINKU32 ReadBufferUsed;
    BINKU32 FramesDataRate;
};

#endif // __BINKH__
