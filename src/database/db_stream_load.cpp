#include <universal/q_shared.h>
#include "database.h"

#if defined(__SWITCH__)
#include "db_retail_wire.h"
#include <qcommon/com_error.h>
#endif


void __cdecl Load_Stream(bool atStreamStart, uint8_t *ptr, int32_t size)
{
    iassert(atStreamStart == (ptr == DB_GetStreamPos()));
    if (atStreamStart && size)
    {
        if (g_streamPosIndex - 1 < 3)
        {
            if (g_streamPosIndex == 1)
            {
                memset(ptr, 0, size);
            }
            else
            {
                bcassert(g_streamDelayIndex, ARRAY_COUNT(g_streamDelayArray));
                g_streamDelayArray[g_streamDelayIndex].ptr = ptr;
                g_streamDelayArray[g_streamDelayIndex++].size = size;
            }
        }
        else
        {
            DB_LoadXFileData(ptr, size);
        }
        DB_IncStreamPos(size);
    }
}

void __cdecl Load_DelayStream()
{
    uint32_t index; // [esp+4h] [ebp-8h]

    for (index = 0; index < g_streamDelayIndex; ++index)
        DB_LoadXFileData((unsigned char*)g_streamDelayArray[index].ptr, g_streamDelayArray[index].size);
}

void __cdecl DB_ConvertOffsetToAlias(uint32_t *data)
{
#if defined(__SWITCH__)
    // This legacy 32-bit loader is not an LP64 asset widener, but it still
    // sees retail encoded aliases while the normal SP source set is linked.
    // Keep its only supported scalar operation on the canonical token seam;
    // do not revive the old block-nibble/mask arithmetic here.
    iassert(data);
    RetailWireToken token{};
    if (!RetailWireTokenDecodeBlocks(g_streamZoneMem->blocks, {*data},
                                     sizeof(*data), (1u << 9) - 1u, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET)
    {
        Com_Error(ERR_FATAL, "DB_ConvertOffsetToAlias received an unsupported retail token on Switch");
        return;
    }
    memcpy(data, g_streamZoneMem->blocks[token.block].data + token.offset, sizeof(*data));
#else
    uint32_t offset; // [esp+0h] [ebp-8h]

    offset = *data;
    iassert((offset && (offset != -1) && (offset != -2)));
    *data = *(uint32_t *)&g_streamZoneMem->blocks[(offset - 1) >> 28].data[(offset - 1) & 0xFFFFFFF];
#endif
}

void __cdecl DB_ConvertOffsetToPointer(uint32_t *data)
{
#if defined(__SWITCH__)
    // Publishing an AArch64 address through this uint32_t API would truncate
    // it.  The production retail walkers widen field-by-field instead; any
    // remaining legacy caller is an unsupported asset family and must stop.
    (void)data;
    Com_Error(ERR_FATAL, "DB_ConvertOffsetToPointer is unsupported on Switch; use a retail native decoder");
#else
    *data = (uint32_t)&g_streamZoneMem->blocks[(uint32_t)(*data - 1) >> 28].data[(*data - 1) & 0xFFFFFFF];
#endif
}

void __cdecl Load_XStringCustom(char **str)
{
    uint8_t *pos; // [esp+0h] [ebp-8h]
    char *s; // [esp+4h] [ebp-4h]

    s = *str;
    for (pos = (uint8_t *)*str; ; ++pos)
    {
        DB_LoadXFileData(pos, 1u);
        if (!*pos)
            break;
    }
    DB_IncStreamPos(pos - (uint8_t *)s + 1);
}

void __cdecl Load_TempStringCustom(char **str)
{
    const char * string; // [esp+0h] [ebp-4h]

    Load_XStringCustom(str);
    if (*str)
        string = (const char*)SL_GetString(*str, 4u); // KISAKTODO: this seems way wrong but it's what the decomp is showing
    else
        string= 0;
    *str = (char *)string;
}
