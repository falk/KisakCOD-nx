#pragma once

// dkCmdBufBindVtxBuffers in deko3d reserves 5 command words per buffer but
// writes 6 (VertexArray::Start and VertexArrayLimit, 3 words each). Its
// reserve only starts a new chunk when fewer than size + 1 words are left,
// so a single-buffer call (reserve 5, so at least 6 words free) always fits,
// while an n-buffer call can write one word past the end of its command
// chunk: the low word of a vertex-stream limit address lands on the next
// chunk's first word, which the GPU rejects as a pushbuffer entry.
// Every vertex buffer is therefore bound with its own call.

#include <stdint.h>

namespace deko9
{

// Words dkCmdBufBindVtxBuffers writes and reserves per buffer.
constexpr uint32_t kVtxBindWordsPerBuffer = 6;
constexpr uint32_t kVtxBindReservePerBuffer = 5;

// deko3d CmdBufWriter::reserve: a call that reserves `size` words continues
// in the current chunk only when pos + size < end.
constexpr bool VtxBindReserveFits(uint32_t freeWords, uint32_t reserveWords)
{
    return reserveWords < freeWords;
}

// Words written past the chunk end by one call binding `buffers` buffers
// with `freeWords` left in the chunk (0 when the call moves to a new chunk
// or fits).
constexpr uint32_t VtxBindOverrun(uint32_t freeWords, uint32_t buffers)
{
    const uint32_t reserve = kVtxBindReservePerBuffer * buffers;
    const uint32_t writes = kVtxBindWordsPerBuffer * buffers;
    if (!VtxBindReserveFits(freeWords, reserve))
        return 0;
    return writes > freeWords ? writes - freeWords : 0;
}

#ifdef DEKO9_VTXBIND_DK
inline void BindVtxBuffers(DkCmdBuf cmd, uint32_t firstId, const DkBufExtents *buffers, uint32_t numBuffers)
{
    for (uint32_t i = 0; i < numBuffers; ++i)
        dkCmdBufBindVtxBuffers(cmd, firstId + i, &buffers[i], 1);
}
#endif

} // namespace deko9
