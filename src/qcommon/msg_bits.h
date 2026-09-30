#pragma once

#ifndef KISAK_SP
#error This File is SinglePlayer Only
#endif

#include "msg.h"
#include <universal/assertive.h>

// MSG_ReadBits/MSG_WriteBits bodies, kept in a pure header (like
// sv_framesmoothing.h) so a host test can drive the literal production code
// against a hand transcription of the original one-bit-per-iteration retail
// algorithm.  These consume/produce up to 8 bits per iteration -- one byte
// load/store instead of up to 32 bit-at-a-time ones -- while reproducing the
// original's observable behaviour exactly:
//
//  - The byte-boundary rule fires only when `msg->bit & 7 == 0` (about to
//    start a fresh byte).  Read: if `readcount >= cursize`, set
//    `overflowed = 1` and return -1 immediately, discarding any bits already
//    OR'd into the result this call (msg->bit/readcount changes already made
//    by earlier bytes *this* call are kept -- only the pending byte is never
//    consumed).  Otherwise `bit = 8 * readcount; ++readcount`.  Write: the
//    space check (`maxsize - cursize < 4`) is made once, up front, exactly
//    like retail; on failure `overflowed = 1` and nothing is written even
//    for `bits == 0`.  On success, starting a fresh byte zero-inits
//    `data[cursize]` and advances cursize.
//  - `bits == 0`: no iterations, read returns 0 and touches no state
//    (besides read's constant-time overflow gate happening per byte, which
//    for 0 bits never runs); write's up-front gate still applies.
//  - `bits == 32`: each chunk is 1..8 bits, so the widest single shift is by
//    24 (`chunk << i` on the last chunk of a 4-chunk, byte-aligned 32-bit
//    read) -- never by 32, so no UB.
//  - Every intermediate `msg->bit`/`msg->readcount`/`msg->cursize` value and
//    the final packed bytes are identical to the retail algorithm; only the
//    number of loop iterations changes (ceil(bits/8) worst case instead of
//    bits).

static inline int MSG_ReadBits_Impl(msg_t *msg, unsigned int bits)
{
    iassert((unsigned)bits <= 32);
    int result = 0;
    int i = 0;
    const int nbits = (int)bits;
    while (i < nbits)
    {
        int v6 = msg->bit & 7;
        if (!v6)
        {
            const int readcount = msg->readcount;
            if (readcount >= msg->cursize)
            {
                msg->overflowed = 1;
                return -1;
            }
            msg->bit = 8 * readcount;
            msg->readcount = readcount + 1;
        }
        const int bit = msg->bit;
        const int byteIndex = bit >> 3;
        const int remInByte = 8 - v6;
        const int remBits = nbits - i;
        const int n = remInByte < remBits ? remInByte : remBits;
        const unsigned int byteVal = msg->data[byteIndex];
        const unsigned int mask = (1u << n) - 1u;
        const unsigned int chunk = (byteVal >> v6) & mask;
        result |= (int)(chunk << i);
        i += n;
        msg->bit = bit + n;
    }
    return result;
}

static inline void MSG_WriteBits_Impl(msg_t *msg, int value, unsigned int bits)
{
    iassert((unsigned)bits <= 32);
    if (msg->maxsize - msg->cursize < 4)
    {
        msg->overflowed = 1;
        return;
    }
    unsigned int remaining = bits;
    while (remaining)
    {
        int v6 = msg->bit & 7;
        if (!v6)
        {
            const int cursize = msg->cursize;
            msg->bit = 8 * cursize;
            msg->data[cursize] = 0;
            msg->cursize = cursize + 1;
        }
        const int bit = msg->bit;
        const int byteIndex = bit >> 3;
        const unsigned int remInByte = (unsigned int)(8 - v6);
        const unsigned int n = remInByte < remaining ? remInByte : remaining;
        const unsigned int mask = (1u << n) - 1u;
        const unsigned int chunk = (unsigned int)value & mask;
        msg->data[byteIndex] = (unsigned __int8)(msg->data[byteIndex] | (chunk << v6));
        // Arithmetic right shift by n, matching n successive `value >>= 1`
        // steps of the original (only the low n bits already masked into
        // `chunk` matter; the sign-extension of the bits still to come must
        // match what repeated single-bit shifts would leave behind).
        value = value >> n;
        msg->bit = bit + (int)n;
        remaining -= n;
    }
}
