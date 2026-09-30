// Host test for two hot-path swaps picked from a PGO profile:
//
//  - MSG_ReadBits / MSG_WriteBits (src/qcommon/msg.cpp, bodies now in
//    src/qcommon/msg_bits.h): rewritten to consume/produce up to 8 bits per
//    iteration from/to the current byte instead of 1 bit per iteration.
//    Profile ranks: MSG_ReadBits #4, 1.33% / 12,035,079 calls; MSG_WriteBits
//    #11, 1.12% / 12,208,061 calls.
//  - Com_Memset (src/universal/com_shared.cpp): was `_copyDWord` (a scalar
//    4-byte-store loop; #2 in the profile, 2.48% / 869,879,635 line
//    executions) plus a byte/word tail fixup, now `memset()`.
//
// msg_bits.h is a pure header (like src/server/sv_framesmoothing.h), so it
// is included directly below: MSG_ReadBits_Impl/MSG_WriteBits_Impl are the
// literal production bodies, not a re-typed copy. ReferenceReadBits/
// ReferenceWriteBits are byte-for-byte transcriptions of the original
// msg.cpp bodies (one bit per iteration) as they were before this change.
// Com_Memset lives in com_shared.cpp, which pulls in the whole engine, so
// ReferenceComMemset below is a transcription of the byte-pattern-fill body
// it replaced, checked against libc memset() directly.
//
// Every caller of Com_Memset in the SP game build passes a literal byte
// value (grepped: 0, 1, 0x1u, 176, 255, 0xDD), so only val 0..255 is
// exercised here -- that is the domain in which the swap is claimed to be
// behaviour-preserving.

#include "src/qcommon/msg_bits.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

#define CHECK(cond)                                                         \
    do                                                                      \
    {                                                                       \
        if (!(cond))                                                        \
        {                                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                                   \
        }                                                                   \
    } while (0)

static uint32_t g_rng = 0x9e3779b9u;
static uint32_t NextRand()
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}
// Uniform in [0, n); NextRand()'s low bits are decent enough for a test PRNG
// over the small ranges used here.
static uint32_t NextRandRange(uint32_t n)
{
    return n ? (NextRand() % n) : 0u;
}

// ---------------------------------------------------------------------
// MSG_ReadBits / MSG_WriteBits reference: transcription of the original
// msg.cpp bodies (one bit consumed/produced per loop iteration).
// ---------------------------------------------------------------------

static int ReferenceReadBits(msg_t *msg, unsigned int bits)
{
    int result = 0;
    int v11;
    int i = 0;
    for (; i < (int)bits; result |= v11)
    {
        int v6 = msg->bit & 7;
        if (!v6)
        {
            int readcount = msg->readcount;
            if (readcount >= msg->cursize)
            {
                result = -1;
                msg->overflowed = 1;
                return result;
            }
            msg->bit = 8 * readcount;
            msg->readcount = readcount + 1;
        }
        int bit = msg->bit;
        int v9 = bit + 1;
        unsigned int v10 = msg->data[bit >> 3];
        msg->bit = v9;
        v11 = ((v10 >> v6) & 1) << i++;
    }
    return result;
}

static void ReferenceWriteBits(msg_t *msg, int value, unsigned int bits)
{
    unsigned int v3 = bits;
    if (msg->maxsize - msg->cursize >= 4)
    {
        for (; v3; ++msg->bit)
        {
            --v3;
            int v6 = msg->bit & 7;
            if (!v6)
            {
                int cursize = msg->cursize;
                msg->bit = 8 * cursize;
                msg->data[cursize] = 0;
                ++msg->cursize;
            }
            if ((value & 1) != 0)
                msg->data[msg->bit >> 3] |= 1 << v6;
            value >>= 1;
        }
    }
    else
    {
        msg->overflowed = 1;
    }
}

// ---------------------------------------------------------------------
// Com_Memset reference: transcription of the original body (_copyDWord
// pattern fill for count >= 8, hand DWORD/WORD/BYTE fixup below that).
// ---------------------------------------------------------------------

static void RefCopyDWord(uint32_t *dest, uint32_t constant, uint32_t count)
{
    for (unsigned i = 0; i < count; i++)
        dest[i] = constant;
}

static void ReferenceComMemset(void *dest_p, int val, size_t count)
{
    uint32_t *dest = (uint32_t *)dest_p;
    uint32_t *v3;
    int v4, v5, v6;
    char *v7;

    if (count >= 8)
    {
        RefCopyDWord(dest, (uint32_t)(val | (val << 8) | ((val | (val << 8)) << 16)),
                     (uint32_t)(count / 4));
        if ((count & 3) != 0)
        {
            v7 = (char *)dest + (count & ~(size_t)3);
            if ((count & 3u) < 2)
            {
                if ((count & 3) != 0)
                    *v7 = (char)val;
            }
            else
            {
                *(uint16_t *)v7 = (uint16_t)(val | ((uint16_t)val << 8));
                if ((count & 3) != 2)
                    v7[2] = (char)val;
            }
        }
    }
    else
    {
        v3 = dest;
        v4 = val;
        v4 = (v4 & ~0xFF00) | ((val & 0xFF) << 8); // BYTE1(v4) = val
        v5 = (uint16_t)v4 + (v4 << 16);
        v6 = (int)count;
        if (count >= 4)
        {
            *dest = (uint32_t)v5;
            v3 = dest + 1;
            v6 = (int)count - 4;
        }
        if (v6 >= 2)
        {
            *(uint16_t *)v3 = (uint16_t)v5;
            v3 = (uint32_t *)((char *)v3 + 2);
            v6 -= 2;
        }
        if (v6)
            *(unsigned char *)v3 = (unsigned char)v5;
    }
}

// ---------------------------------------------------------------------
// MSG_ReadBits / MSG_WriteBits fuzz
// ---------------------------------------------------------------------

struct MsgHarness
{
    unsigned char data[128];
    msg_t m;
};

static void InitHarness(MsgHarness &h)
{
    memset(&h, 0, sizeof(h));
    h.m.data = h.data;
}

static void CompareMsgState(const msg_t &ref, const msg_t &impl,
                             const unsigned char *refData, const unsigned char *implData,
                             size_t dataLen, const char *where)
{
    if (ref.overflowed != impl.overflowed || ref.cursize != impl.cursize ||
        ref.readcount != impl.readcount || ref.bit != impl.bit ||
        memcmp(refData, implData, dataLen) != 0)
    {
        fprintf(stderr,
                "FAIL %s: state mismatch (overflowed %d/%d cursize %d/%d "
                "readcount %d/%d bit %d/%d data %s)\n",
                where, ref.overflowed, impl.overflowed, ref.cursize, impl.cursize,
                ref.readcount, impl.readcount, ref.bit, impl.bit,
                memcmp(refData, implData, dataLen) == 0 ? "match" : "DIFFER");
        ++g_failures;
    }
}

static void RunReadTrial()
{
    const int bufLen = (int)NextRandRange(97); // 0..96
    unsigned char content[128];
    for (int i = 0; i < 128; ++i)
        content[i] = (unsigned char)NextRand();

    MsgHarness refH, implH;
    InitHarness(refH);
    InitHarness(implH);
    memcpy(refH.data, content, sizeof(content));
    memcpy(implH.data, content, sizeof(content));
    refH.m.cursize = implH.m.cursize = bufLen;
    refH.m.maxsize = implH.m.maxsize = bufLen;

    // A reachable (readcount, bit) pair: readcount counts bytes already
    // opened; bit is either exactly byte-aligned at 8*readcount (nothing
    // pending) or somewhere inside the byte at index readcount-1 that was
    // opened but not yet fully consumed. readcount may exceed cursize (to
    // exercise the overflow path) but only paired with the aligned case, so
    // no out-of-bounds byte is ever addressed before the boundary check
    // fires.
    int readcount = (int)NextRandRange((uint32_t)bufLen + 3);
    int offset = (int)NextRandRange(9); // 0..8; 8 == aligned
    int bit;
    if (readcount == 0 || offset == 8)
    {
        bit = 8 * readcount;
    }
    else
    {
        if (readcount > bufLen)
            readcount = bufLen;
        bit = readcount == 0 ? 0 : 8 * (readcount - 1) + offset;
    }
    refH.m.readcount = implH.m.readcount = readcount;
    refH.m.bit = implH.m.bit = bit;

    const int steps = 1 + (int)NextRandRange(12);
    for (int s = 0; s < steps; ++s)
    {
        unsigned int bits;
        // Bias toward the interesting edges: 0, 32, and small counts, on
        // top of the general 0..32 spread.
        switch (NextRandRange(4))
        {
        case 0: bits = 0; break;
        case 1: bits = 32; break;
        default: bits = NextRandRange(33); break;
        }
        int rref = ReferenceReadBits(&refH.m, bits);
        int rimpl = MSG_ReadBits_Impl(&implH.m, bits);
        CHECK(rref == rimpl);
        CompareMsgState(refH.m, implH.m, refH.data, implH.data, sizeof(refH.data), "read");
    }
}

static void RunWriteTrial()
{
    const int maxsize = (int)NextRandRange(41); // 0..40
    const int cursize = (int)NextRandRange((uint32_t)maxsize + 1);
    unsigned char content[128];
    for (int i = 0; i < 128; ++i)
        content[i] = (unsigned char)NextRand();

    MsgHarness refH, implH;
    InitHarness(refH);
    InitHarness(implH);
    memcpy(refH.data, content, sizeof(content));
    memcpy(implH.data, content, sizeof(content));
    refH.m.maxsize = implH.m.maxsize = maxsize;
    refH.m.cursize = implH.m.cursize = cursize;

    int offset = (int)NextRandRange(9); // 0..8; 8 == aligned
    int bit = (cursize == 0 || offset == 8) ? 8 * cursize : 8 * (cursize - 1) + offset;
    refH.m.bit = implH.m.bit = bit;

    const int steps = 1 + (int)NextRandRange(12);
    for (int s = 0; s < steps; ++s)
    {
        unsigned int bits;
        switch (NextRandRange(4))
        {
        case 0: bits = 0; break;
        case 1: bits = 32; break;
        default: bits = NextRandRange(33); break;
        }
        int value;
        switch (NextRandRange(6))
        {
        case 0: value = 0; break;
        case 1: value = -1; break;
        case 2: value = (int)0x80000000u; break;
        case 3: value = 0x7FFFFFFF; break;
        default: value = (int)NextRand(); break;
        }
        ReferenceWriteBits(&refH.m, value, bits);
        MSG_WriteBits_Impl(&implH.m, value, bits);
        CompareMsgState(refH.m, implH.m, refH.data, implH.data, sizeof(refH.data), "write");
    }
}

// Sanity beyond pure equivalence: encode a sequence of known values with the
// production writer, decode them back with the production reader, and check
// round-trip.
static void RunRoundTrip()
{
    static const unsigned int widths[] = {0, 1, 3, 7, 8, 9, 15, 16, 17, 24, 25, 31, 32};
    for (uint32_t trial = 0; trial < 200; ++trial)
    {
        MsgHarness h;
        InitHarness(h);
        h.m.maxsize = sizeof(h.data);

        int values[64];
        unsigned int bits[64];
        int n = 1 + (int)NextRandRange(64);
        int written = 0;
        for (int i = 0; i < n; ++i)
        {
            unsigned int w = widths[NextRandRange(sizeof(widths) / sizeof(widths[0]))];
            // Stop before the buffer's capacity runs out (WriteBits_Impl's
            // own headroom gate is conservative -- it can refuse a bit
            // count that would technically still fit -- so stop a little
            // earlier still, rather than depend on exactly matching it).
            if ((size_t)(h.m.bit + (int)w + 32) / 8 >= sizeof(h.data))
                break;
            uint32_t mask = w == 32 ? 0xFFFFFFFFu : ((1u << w) - 1u);
            int value = (int)(NextRand() & mask);
            MSG_WriteBits_Impl(&h.m, value, w);
            CHECK(!h.m.overflowed);
            bits[written] = w;
            values[written] = value;
            ++written;
        }
        h.m.cursize = (h.m.bit + 7) / 8;
        h.m.bit = 0;
        h.m.readcount = 0;
        for (int i = 0; i < written; ++i)
        {
            uint32_t mask = bits[i] == 32 ? 0xFFFFFFFFu : ((1u << bits[i]) - 1u);
            int got = MSG_ReadBits_Impl(&h.m, bits[i]);
            CHECK(!h.m.overflowed);
            CHECK(((uint32_t)got & mask) == ((uint32_t)values[i] & mask));
        }
    }
}

// ---------------------------------------------------------------------
// Com_Memset fuzz
// ---------------------------------------------------------------------

static void RunMemsetTrials()
{
    unsigned char refBuf[80];
    unsigned char newBuf[80];
    for (uint32_t trial = 0; trial < 4000; ++trial)
    {
        size_t count = NextRandRange(sizeof(refBuf) + 1);
        int val = (int)NextRandRange(256); // every caller in the game build
                                            // passes a literal byte value
        for (size_t i = 0; i < sizeof(refBuf); ++i)
            refBuf[i] = newBuf[i] = (unsigned char)NextRand();

        ReferenceComMemset(refBuf, val, count);
        memset(newBuf, val, count);
        if (memcmp(refBuf, newBuf, sizeof(refBuf)) != 0)
        {
            fprintf(stderr, "FAIL memset trial=%u count=%zu val=%d\n", trial, count, val);
            ++g_failures;
        }
    }
    // Exhaustive small counts x every byte value, since the original code
    // branches differently below/above count==8/4/2.
    for (int count = 0; count <= 16; ++count)
    {
        for (int val = 0; val <= 255; ++val)
        {
            unsigned char a[24], b[24];
            for (int i = 0; i < 24; ++i)
                a[i] = b[i] = (unsigned char)(i * 37 + 11);
            ReferenceComMemset(a, val, (size_t)count);
            memset(b, val, (size_t)count);
            CHECK(memcmp(a, b, sizeof(a)) == 0);
        }
    }
}

int main()
{
    for (uint32_t i = 0; i < 3000; ++i)
        RunReadTrial();
    for (uint32_t i = 0; i < 3000; ++i)
        RunWriteTrial();
    RunRoundTrip();
    RunMemsetTrials();

    if (g_failures)
    {
        printf("FAIL:MSG_BITS_MEMSET failures=%d\n", g_failures);
        return 1;
    }
    printf("PASS:MSG_BITS_MEMSET\n");
    return 0;
}
