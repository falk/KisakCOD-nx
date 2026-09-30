#ifndef KISAK_SP 
#error This file is for SinglePlayer only 
#endif

#include <universal/q_shared.h>
#include "scr_readwrite.h"
#include "scr_main.h"
#include "scr_memorytree.h"
#include "scr_vm.h"
#include "scr_parsetree.h"
#include <game/savedevice.h>
#include <universal/com_files.h>  // FS_Write for SaveMemory_SaveWriteImmediate

static unsigned int g_idHistoryIndex;
static short idHistory[16];

// VariableStackBuffer's payload is native pointer-width.  The save stream is
// still the original byte-oriented format, but the in-memory walk must use
// the same stride as VM_ArchiveStack/VM_UnarchiveStack2 on LP64.
static constexpr size_t kScriptStackValueSize = 1 + sizeof(uintptr_t);
static constexpr size_t kScriptStackHeaderSize = offsetof(VariableStackBuffer, buf);

void __cdecl WriteByte(
    unsigned __int8 b,
    MemoryFile *memFile)
{
    MemFile_WriteData(memFile, 1, &b);
}

void __cdecl WriteShort(unsigned __int16 i, MemoryFile *memFile)
{
    MemFile_WriteData(memFile, 2, &i);
}

void __cdecl WriteString(unsigned __int16 str, MemoryFile *memFile)
{
    const char *v3; // r3

    v3 = SL_ConvertToString(str);
    MemFile_WriteCString(memFile, v3);
}

void __cdecl SafeWriteString(unsigned __int16 str, MemoryFile *memFile)
{
    unsigned int v3; // r30
    const char *v4; // r3
    _BYTE v5[8]; // [sp+50h] [-20h] BYREF

    v3 = str;
    if (str)
    {
        v5[0] = 1;
        MemFile_WriteData(memFile, 1, v5);
        v4 = SL_ConvertToString(v3);
        MemFile_WriteCString(memFile, v4);
    }
    else
    {
        v5[0] = 0;
        MemFile_WriteData(memFile, 1, v5);
    }
}

int __cdecl Scr_ReadString(MemoryFile *memFile)
{
    const char *CString; // r3
    const char *v2; // r11

    CString = MemFile_ReadCString(memFile);
    v2 = CString;
    while (*(unsigned __int8 *)v2++)
        ;
    return (unsigned __int16)SL_GetStringOfSize(CString, 0, v2 - CString, MT_TYPE_SCRIPT_STRING);
}

int __cdecl Scr_ReadOptionalString(MemoryFile *memFile)
{
    const char *CString; // r3
    const char *v4; // r11
    _BYTE v6[16]; // [sp+50h] [-20h] BYREF

    MemFile_ReadData(memFile, 1, v6);
    if (!v6[0])
        return 0;
    CString = MemFile_ReadCString(memFile);
    v4 = CString;
    while (*(unsigned __int8 *)v4++)
        ;
    return (unsigned __int16)SL_GetStringOfSize(CString, 0, v4 - CString, MT_TYPE_SCRIPT_STRING);
}

void __cdecl WriteInt(int i, MemoryFile *memFile)
{
    MemFile_WriteData(memFile, 4, &i);
}

void WriteFloat(float f, MemoryFile *memFile)
{
    iassert(!IS_NAN(f));
    MemFile_WriteData(memFile, 4, &f);
}

void __cdecl WriteVector(float *v, MemoryFile *memFile)
{
    iassert(v);
    iassert(!IS_NAN(v[0]) && !IS_NAN(v[1]) && !IS_NAN(v[2]));
    
    WriteFloat(v[0], memFile);
    WriteFloat(v[1], memFile);
    WriteFloat(v[2], memFile);
}

const float *__cdecl Scr_ReadVec3(MemoryFile *memFile)
{
    float vec[3]; // [sp+58h] [-48h] BYREF

    vec[0] = MemFile_ReadFloat(memFile);
    vec[1] = MemFile_ReadFloat(memFile);
    vec[2] = MemFile_ReadFloat(memFile);

    nanassertvec3(vec);

    return Scr_AllocVector(vec);
}

void __cdecl WriteCodepos(const char *pos, MemoryFile *memFile)
{
    int v4; // r11
    int v5; // [sp+50h] [-20h] BYREF

    if (pos)
    {
        if (!Scr_IsInOpcodeMemory(pos))
            MyAssertHandler(
                "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
                128,
                0,
                "%s",
                "!pos || Scr_IsInOpcodeMemory( pos )");
        v4 = pos - scrVarPub.programBuffer;
    }
    else
    {
        v4 = -1;
    }
    v5 = v4;
    MemFile_WriteData(memFile, 4, &v5);
}

const char *__cdecl Scr_ReadCodepos(MemoryFile *memFile)
{
    int v1; // r31
    const char *v2; // r31
    const char *result; // r3
    int v4; // [sp+50h] [-30h] BYREF

    MemFile_ReadData(memFile, 4, (unsigned char*)&v4);
    v1 = v4;
    if (v4 < -1)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 141, 0, "%s", "offset >= -1");
    if (v1 < 0)
        return 0;
    v2 = &scrVarPub.programBuffer[v1];
    result = v2;
    if (v2)
    {
        if (!Scr_IsInOpcodeMemory(v2))
            MyAssertHandler(
                "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
                143,
                0,
                "%s",
                "!pos || Scr_IsInOpcodeMemory( pos )");
        return v2;
    }
    return result;
}

unsigned int Scr_CheckIdHistory(unsigned int index)
{
    unsigned int result; // r3
    unsigned int v3; // r11
    __int16 *v4; // r10
    int v5; // r9
    __int16 *v6; // r11
    unsigned int v7; // r10
    int v8; // r9

    result = 1;
    v3 = g_idHistoryIndex + 1;
    if (g_idHistoryIndex + 1 >= 0x10)
    {
    LABEL_6:
        v6 = idHistory;
        v7 = 0;
        while (1)
        {
            v8 = (unsigned __int16)*v6;
            if (index == v8 + 1)
                break;
            if (index == v8)
                return ++result;
            ++v7;
            ++v6;
            result += 2;
            if (v7 > g_idHistoryIndex)
                return 0;
        }
    }
    else
    {
        v4 = &idHistory[v3];
        while (1)
        {
            v5 = (unsigned __int16)*v4;
            if (index == v5 + 1)
                break;
            if (index == v5)
                return ++result;
            ++v3;
            ++v4;
            result += 2;
            if (v3 >= 0x10)
                goto LABEL_6;
        }
    }
    return result;
}

void WriteId(unsigned int id, unsigned int opcode, MemoryFile *memFile)
{
    unsigned int v3; // r31
    unsigned __int16 v6; // r29
    unsigned int v7; // r31
    int v8; // r10
    _WORD v9[32]; // [sp+50h] [-40h] BYREF

    v3 = id;
    if (!scrVarPub.saveIdMap[id] && id)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            188,
            0,
            "%s",
            "scrVarPub.saveIdMap[id] || !id");
    if ((opcode & 0xFFFFFFF8) != 0)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            189,
            0,
            "%s\n\t(opcode) = %i",
            "(!(opcode & ~7))",
            opcode);
    v6 = scrVarPub.saveIdMap[v3];
    v7 = 8 * Scr_CheckIdHistory(v6);
    if (v7 >= 0x100)
        v7 = 0;
    if ((v7 & 7) != 0)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            195,
            0,
            "%s\n\t(historyIndex) = %i",
            "(!(historyIndex & 7))",
            v7);

    v9[0] = (v9[0] & 0xFF00) | (uint8_t)(v7 + opcode);
    MemFile_WriteData(memFile, 1, v9);
    if (!v7)
    {
        v9[0] = v6;
        MemFile_WriteData(memFile, 2, v9);
    }
    v8 = ((_BYTE)g_idHistoryIndex - 1) & 0xF;
    idHistory[g_idHistoryIndex] = v6;
    g_idHistoryIndex = v8;
}

unsigned int Scr_ReadId(MemoryFile *memFile, unsigned int opcode)
{
    unsigned int v2; // r11
    unsigned int v3; // r29
    unsigned int v4; // r30
    unsigned int result; // r3
    __int16 v6; // [sp+50h] [-30h] BYREF

    if (opcode >> 3)
    {
        v2 = g_idHistoryIndex;
        v3 = *(unsigned __int16 *)((char *)idHistory + ((2 * ((((opcode >> 3) + 1) >> 1) + g_idHistoryIndex)) & 0x1E))
            + ((opcode >> 3) & 1);
    }
    else
    {
        MemFile_ReadData(memFile, 2, (unsigned char*)&v6);
        v3 = v6;
        v2 = g_idHistoryIndex;
    }
    v4 = scrVarPub.saveIdMapRev[v3];
    if (scrVarPub.saveIdMapRev[v3])
    {
        AddRefToObject(scrVarPub.saveIdMapRev[v3]);
        v2 = g_idHistoryIndex;
    }
    result = v4;
    idHistory[v2] = v3;
    g_idHistoryIndex = ((_BYTE)v2 - 1) & 0xF;
    return result;
}

void __cdecl WriteStack(const VariableStackBuffer *stackBuf, MemoryFile *memFile)
{
    int v4; // r30
    __int16 v5; // r11
    const char *buf; // r31
    unsigned int v7; // r3
    __int16 v8; // r30
    uintptr_t v9; // r4
    _WORD v10[24]; // [sp+50h] [-30h] BYREF

    iassert(stackBuf);
    v10[0] = stackBuf->size;
    v4 = v10[0];
    MemFile_WriteData(memFile, 2, v10);
    WriteCodepos(stackBuf->pos, memFile);
    WriteId(stackBuf->localId, 0, memFile);

    v10[0] = (v10[0] & 0xFF00) | (uint8_t)stackBuf->time;
    MemFile_WriteData(memFile, 1, v10);
    v5 = v4;
    buf = stackBuf->buf;
    if (v4)
    {
        do
        {
            v7 = (unsigned __int8)*buf;
            v8 = v5 - 1;
            v9 = *reinterpret_cast<const uintptr_t *>(buf + 1);
            buf += kScriptStackValueSize;
            DoSaveEntryInternal(v7, reinterpret_cast<VariableUnion *>(v9), memFile);
            v5 = v8;
        } while (v8);
    }
}

VariableStackBuffer *__cdecl Scr_ReadStack(MemoryFile *memFile)
{
    __int16 v2; // r27
    int v3; // r28
    int v4; // r31
    unsigned __int16 v5; // r29
    VariableStackBuffer *v6; // r31
    char *v7; // r29
    __int16 v8; // r11
    __int16 v9; // r28
    unsigned __int8 v11[8]; // [sp+50h] [-40h] BYREF
    VariableValue v12; // [sp+58h] [-38h] BYREF

    MemFile_ReadData(memFile, 2, v11);
    v2 = *(_WORD *)v11;
    v3 = *(unsigned __int16 *)v11;
    v4 = static_cast<int>(kScriptStackHeaderSize +
                          kScriptStackValueSize * *(unsigned __int16 *)v11);
    v5 = static_cast<unsigned __int16>(v4);
    if (v4 != v5)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            268,
            0,
            "%s",
            "bufLen == (unsigned short)bufLen");
    v6 = static_cast<VariableStackBuffer *>(MT_Alloc(v4, MT_TYPE_THREAD));
    ++scrVarPub.numScriptThreads;
    v6->size = v2;
    v6->bufLen = v5;
    v6->pos = Scr_ReadCodepos(memFile);
    MemFile_ReadData(memFile, 1, v11);
    v6->localId = Scr_ReadId(memFile, v11[0]);
    MemFile_ReadData(memFile, 1, v11);
    v7 = v6->buf;
    v6->time = v11[0];
    if (v3)
    {
        v8 = v3;
        do
        {
            v9 = v8 - 1;
            Scr_DoLoadEntryInternal(&v12, memFile);
            v8 = v9;
            *v7 = static_cast<unsigned char>(v12.type);
            *reinterpret_cast<uintptr_t *>(v7 + 1) = v12.u.pointerValue;
            v7 += kScriptStackValueSize;
        } while (v9);
    }
    return v6;
}

void __cdecl Scr_DoLoadEntryInternal(VariableValue *value, MemoryFile *memFile)
{
    unsigned int v4; // r4
    int v5; // r4
    const char *v6; // r3
    _BYTE v7[4]; // [sp+50h] [-20h] BYREF
    VariableUnion v8; // [sp+54h] [-1Ch] BYREF

    MemFile_ReadData(memFile, 1, v7);
    v4 = v7[0];
    if ((v7[0] & 7) != 0)
    {
        value->type = VAR_POINTER;
        value->u.intValue = Scr_ReadId(memFile, v4);
    }
    else
    {
        v5 = v7[0] >> 3;
        value->type = (Vartype_t)v5;
        switch (v5)
        {
        case VAR_UNDEFINED:
        case VAR_PRECODEPOS:
            return;
        case VAR_STRING:
        case VAR_ISTRING:
            value->u.intValue = (unsigned __int16)Scr_ReadString(memFile);
            break;
        case VAR_VECTOR:
            value->u.pointerValue = reinterpret_cast<uintptr_t>(Scr_ReadVec3(memFile));
            break;
        case VAR_FLOAT:
            value->u.floatValue = MemFile_ReadFloat(memFile);
            break;
        case VAR_INTEGER:
        case VAR_ANIMATION:
            MemFile_ReadData(memFile, 4, (unsigned char*)&v8);
            value->u = v8;
            break;
        case VAR_CODEPOS:
        case VAR_FUNCTION:
            value->u.pointerValue = reinterpret_cast<uintptr_t>(Scr_ReadCodepos(memFile));
            break;
        case VAR_STACK:
            value->u.pointerValue = reinterpret_cast<uintptr_t>(Scr_ReadStack(memFile));
            break;
        default:
            if (!alwaysfails)
            {
                v6 = va("unknown type %i", v5);
                MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 501, 1, v6);
            }
            break;
        }
    }
}

int __cdecl Scr_DoLoadEntry(VariableValue *value, bool isArray, MemoryFile *memFile)
{

    int result;
    unsigned __int8 byte0;
    unsigned __int8 byte1;
    unsigned __int8 byte2;
    unsigned int header = 0;
    unsigned __int16 header2 = 0;
    int header4 = 0;

    Scr_DoLoadEntryInternal(value, memFile);
    if (isArray)
    {
        MemFile_ReadData(memFile, 1, (unsigned char *)&header);
        unsigned int tag = header & 0xFF;
        switch (tag & 7)
        {
        case 0:
            result = 0x800000;
            break;
        case 1:
        {
            unsigned int byteRead = 0;
            MemFile_ReadData(memFile, 1, (unsigned char *)&byteRead);
            result = (int)(__int8)byteRead + 0x800000;
            break;
        }
        case 2:
            header2 = 0;
            MemFile_ReadData(memFile, 2, (unsigned char *)&header2);
            result = (int)(__int16)header2 + 0x800000;
            break;
        case 3:
            header4 = 0;
            MemFile_ReadData(memFile, 4, (unsigned char *)&header4);
            result = header4 + 0x800000;
            break;
        case 4:
            result = (unsigned __int16)Scr_ReadString(memFile);
            break;
        case 5:
            result = Scr_ReadId(memFile, tag) + 0x10000;
            break;
        default:
            if (!alwaysfails)
                MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 539, 1, "bad case");
            result = 0;
            break;
        }
    }
    else
    {

        MemFile_ReadData(memFile, 1, &byte0);
        MemFile_ReadData(memFile, 1, &byte1);
        MemFile_ReadData(memFile, 1, &byte2);
        return (((unsigned int)byte2) << 16) | (((unsigned int)byte1) << 8) | byte0;
    }
    return result;
}

void __cdecl AddSaveObjectInternal(unsigned int parentId)
{
    if (parentId)
    {
        if (!scrVarPub.saveIdMap[parentId])
        {
            scrVarPub.saveIdMap[parentId] = ++scrVarPub.savecount;
            *(unsigned __int16 *)((char *)scrVarPub.saveIdMapRev + __ROL4__(scrVarPub.savecount, 1)) = parentId;
        }
    }
}

unsigned int __cdecl Scr_ConvertThreadFromLoad(unsigned __int16 handle)
{
    int v2; // r30
    unsigned int v3; // r30

    if (!handle)
        return 0;
    v2 = handle;
    if (!scrVarPub.saveIdMapRev[v2])
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            746,
            0,
            "%s",
            "scrVarPub.saveIdMapRev[handle]");
    v3 = scrVarPub.saveIdMapRev[v2];
    ++scrVarPub.ext_threadcount;
    AddRefToObject(v3);
    if (scrVarDebugPub)
        ++scrVarDebugPub->extRefCount[v3];
    return v3;
}

void __cdecl Scr_DoLoadObjectInfo(unsigned __int16 parentId, MemoryFile *memFile)
{


    unsigned int v2;
    VariableValueInternal *parentValue;
    int v5;
    bool v12;
    int v13;
    unsigned int v14;
    VariableValueInternal *entryValue;
    int v18;
    int type;
    int v20;
    unsigned int header;       // 1-byte read scratch
    unsigned int header4;      // 4-byte read scratch
    unsigned __int16 header2;  // 2-byte read scratch
    VariableValue value;

    v2 = parentId;
    parentValue = &scrVarGlob.variableList[parentId + 1];

    iassert((parentValue->w.status & VAR_STAT_MASK) == VAR_STAT_EXTERNAL);
    iassert(IsObject(parentValue));

    header = 0;
    MemFile_ReadData(memFile, 1, (unsigned char *)&header);
    unsigned int headerByte = header & 0xFF;

    switch (headerByte & 7)
    {
    case 1:
        v5 = VAR_THREAD;
        parentValue->u.o.u.size = Scr_ReadId(memFile, headerByte);
        break;
    case 2:
        v5 = VAR_NOTIFY_THREAD;
        parentValue->u.o.u.entnum = Scr_ReadId(memFile, headerByte);
        iassert(!(parentValue->w.notifyName & VAR_NAME_HIGH_MASK));
        parentValue->w.type |= (Scr_ReadOptionalString(memFile) << 8) & 0xFFFF00;
        break;
    case 3:
        v5 = VAR_TIME_THREAD;
        parentValue->u.o.u.entnum = Scr_ReadId(memFile, headerByte);
        iassert(!(parentValue->w.waitTime & VAR_NAME_HIGH_MASK));
        header4 = 0;
        MemFile_ReadData(memFile, 4, (unsigned char *)&header4);
        parentValue->w.type |= header4 << 8;
        break;
    case 4:
    {
        v5 = VAR_CHILD_THREAD;
        parentValue->u.o.u.size = Scr_ReadId(memFile, headerByte);
        unsigned int header_b = 0;
        MemFile_ReadData(memFile, 1, (unsigned char *)&header_b);
        unsigned int byte2 = header_b & 0xFF;
        iassert(!(parentValue->w.parentLocalId & VAR_NAME_HIGH_MASK));
        parentValue->w.type |= Scr_ReadId(memFile, byte2) << 8;
        break;
    }
    case 5:
        v5 = VAR_DEAD_ENTITY;
        parentValue->u.o.u.size = Scr_ReadId(memFile, headerByte);
        break;
    default:
        v5 = headerByte >> 3;
        if (v5 == VAR_ENTITY)
        {
            header2 = 0;
            MemFile_ReadData(memFile, 2, (unsigned char *)&header2);
            parentValue->u.o.u.size = header2;
            if (parentValue->w.classnum & VAR_NAME_HIGH_MASK)
                MyAssertHandler(
                    "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
                    827,
                    0,
                    "%s",
                    "!(parentValue->w.classnum & VAR_NAME_HIGH_MASK)");
            header2 = 0;
            MemFile_ReadData(memFile, 2, (unsigned char *)&header2);
            parentValue->w.type |= ((int)(__int16)header2) << 8;
        }
        else if (v5 == VAR_ARRAY)
        {
            parentValue->u.o.u.size = 0;
        }
        break;
    }
    if ((v5 & ~VAR_MASK) != 0)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 837, 0, "%s", "!(type & ~VAR_MASK)");
    v12 = v5 == VAR_ARRAY;
    parentValue->w.type = parentValue->w.type & 0xFFFFFFE0 | v5;

    header2 = 0;
    MemFile_ReadData(memFile, 2, (unsigned char *)&header2);
    if (header2)
    {
        v13 = header2;
        do
        {
            v14 = Scr_DoLoadEntry(&value, v12, memFile);
            entryValue = &scrVarGlob.variableList[GetVariable(v2, v14) + VARIABLELIST_CHILD_BEGIN];
            if (v12)
            {
                VariableValue val = Scr_GetArrayIndexValue(v14);
                RemoveRefToValue(val.type, val.u);
            }
            v18 = entryValue->w.status & 0x60;
            if (!v18 || v18 == 96)
                MyAssertHandler(
                    "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
                    856,
                    0,
                    "%s",
                    "(entryValue->w.status & VAR_STAT_MASK) != VAR_STAT_FREE && (entryValue->w.status & VAR_STAT_MASK) != VAR_STAT_EXTERNAL");
            iassert((entryValue->w.type & VAR_MASK) == VAR_UNDEFINED);
            type = value.type;
            iassert(!(value.type & ~VAR_MASK));
            iassert(!(entryValue->w.type & VAR_MASK));
            --v13;
            v20 = type | entryValue->w.type;
            // Preserve native pointers (vector/codepos/stack) on LP64; the
            // old int-only copy truncated the loaded union before use.
            entryValue->u.u = value.u;
            entryValue->w.type = v20;
        } while (v13);
    }
}

void __cdecl Scr_ReadGameEntry(MemoryFile *memFile)
{
    int type; // r28
    unsigned int gameId; // r11
    VariableUnion v4; // r8
    VariableValue v5; // [sp+50h] [-30h] BYREF

    if (scrVarPub.gameId)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 910, 0, "%s", "!scrVarPub.gameId");
    scrVarPub.gameId = AllocValue();
    Scr_DoLoadEntryInternal(&v5, memFile);
    type = v5.type;
    if ((v5.type & ~VAR_MASK) != 0)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            915,
            0,
            "%s",
            "!(tempValue.type & ~VAR_MASK)");
    gameId = scrVarPub.gameId;
    v4.intValue = (int)v5.u.intValue;
    scrVarGlob.variableList[gameId + VARIABLELIST_CHILD_BEGIN].w.type |= type;
    scrVarGlob.variableList[gameId + VARIABLELIST_CHILD_BEGIN].u.u = v4;
}

static void Scr_RemoveElementValue(Scr_WatchElement_s *element)
{
    Scr_WatchElement_s *i; // r31

    if (element->valueDefined)
    {
        element->valueDefined = 0;
        RemoveRefToValue(element->value.type, element->value.u);
    }
    for (i = element->childHead; i; i = i->next)
        Scr_RemoveElementValue(i);
}

void Scr_RemoveElementValues()
{
    Scr_WatchElement_s *i; // r30
    Scr_WatchElement_s *j; // r31

    for (i = scrDebuggerGlob.scriptWatch.elementHead; i; i = i->next)
    {
        if (i->valueDefined)
        {
            i->valueDefined = 0;
            RemoveRefToValue(i->value.type, i->value.u);
        }
        for (j = i->childHead; j; j = j->next)
            Scr_RemoveElementValue(j);
        if (!i->breakpoint)
        {
            if (!i->expr.exprHead)
                MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_debugger.cpp", 8570, 0, "%s", "expr->exprHead");
            Scr_ClearDebugExpr(i->expr.exprHead);
        }
    }
}

static void Scr_AddDebuggerRefs()
{
    if (scrVarPub.developer)
    {
        if (scrVmPub.function_count)
            MyAssertHandler(
                "c:\\trees\\cod3\\cod3src\\src\\script\\scr_debugger.cpp",
                8605,
                0,
                "%s",
                "!scrVmPub.function_count");
        scrDebuggerGlob.scriptWatch.localId = 0;
        iassert(!scrVarPub.evaluate);
        scrVarPub.evaluate = 1;
        //Scr_ScriptWatch::Evaluate(&scrDebuggerGlob.scriptWatch);
        scrDebuggerGlob.scriptWatch.Evaluate();
        //Scr_ScriptWatch::UpdateBreakpoints(&scrDebuggerGlob.scriptWatch, 1);
        scrDebuggerGlob.scriptWatch.UpdateBreakpoints(true);
        iassert(scrVarPub.evaluate);
        scrVarPub.evaluate = 0;
    }
}

static void Scr_RemoveDebuggerRefs()
{
    if (scrVarPub.developer)
    {
        if (!Scr_IsStackClear())
            MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_debugger.cpp", 8582, 0, "%s", "Scr_IsStackClear()");
        if (scrVmPub.function_count)
            MyAssertHandler(
                "c:\\trees\\cod3\\cod3src\\src\\script\\scr_debugger.cpp",
                8584,
                0,
                "%s",
                "!scrVmPub.function_count");
        scrDebuggerGlob.scriptWatch.localId = 0;
        if (scrVarPub.evaluate)
            MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_debugger.cpp", 8587, 0, "%s", "!scrVarPub.evaluate");
        scrVarPub.evaluate = 1;
        //Scr_ScriptWatch::UpdateBreakpoints(&scrDebuggerGlob.scriptWatch, 0);
        scrDebuggerGlob.scriptWatch.UpdateBreakpoints(false);
        Scr_RemoveElementValues();
        if (!scrVarPub.evaluate)
            MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_debugger.cpp", 8593, 0, "%s", "scrVarPub.evaluate");
        scrVarPub.evaluate = 0;
        if (!Scr_IsStackClear())
            MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_debugger.cpp", 8596, 0, "%s", "Scr_IsStackClear()");
    }
}

void __cdecl Scr_SaveShutdown(bool savegame)
{
    char v2; // r20
    int v4; // r30
    const char *v7; // r31

    v2 = 0;
    if (scrVarDebugPub)
    {
        // variableList records are native-sized on LP64; walking them with the
        // old 16-byte p_w stride landed inside records and produced false
        // cyclic-leak diagnostics.  saveIdMap[v4] and varUsage[v4 + 1] name
        // the same variableList[v4 + 1] entry as the retail walk did.
        v4 = 1;
        do
        {
            VariableValueInternal *entryValue = &scrVarGlob.variableList[v4 + 1];
            if ((entryValue->w.type & 0x60) != 0)
            {
                if (!IsObject(entryValue))
                    MyAssertHandler(
                        "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
                        968,
                        0,
                        "%s",
                        "IsObject( entryValue )");
                v7 = scrVarDebugPub->varUsage[v4 + 1];
                if (!v7)
                    MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 970, 0, "%s", "pos");
                if (!scrVarPub.saveIdMap[v4] && (entryValue->w.type & VAR_MASK) != VAR_ARRAY)
                {
                    if (!v2)
                    {
                        v2 = 1;
                        Com_Printf(CON_CHANNEL_PARSERSCRIPT, "\n****script variable cyclic leak*****\n");
                    }
                    {
                        static int s_scrLeakDiag;
                        if (s_scrLeakDiag < 8)
                        {
                            ++s_scrLeakDiag;
                            
                        }
                    }
                    Scr_PrintPrevCodePos(CON_CHANNEL_PARSERSCRIPT, (char*)v7, 0);
                }
            }
            ++v4;
        } while (v4 <= 0x7FFF);
    }
    Scr_AddDebuggerRefs();
    if (v2)
    {
        Com_Printf(CON_CHANNEL_PARSERSCRIPT, "************************************\n");
        if (savegame)
            Com_Error(ERR_DROP, "Script variable leak due to cyclic usage (see console for details)");
        else
            Com_Printf(CON_CHANNEL_PARSERSCRIPT, "script variable leak due to cyclic usage\n");
    }
}

void __cdecl Scr_LoadPre(int sys, MemoryFile *memFile)
{
    unsigned __int16 savecount; // r11
    unsigned int v4; // r30
    unsigned __int16 *v5; // r29
    unsigned int v6; // r3
    unsigned int v7; // r29
    unsigned __int16 *v8; // r30
    unsigned int v9; // r30
    unsigned int Id; // r3
    unsigned int v11; // r30
    unsigned int v12; // r3
    unsigned int v13; // r30
    unsigned int v14; // r3
    unsigned int v15; // r30
    unsigned int v16; // r3
    unsigned int v17; // r30
    unsigned int v18; // r3
    int v19; // r29
    unsigned __int16 *p_entArrayId; // r31
    unsigned __int16 v21; // r10
    scrVarDebugPub_t *v22; // r11
    unsigned __int8 v23[96]; // [sp+50h] [-60h] BYREF

    if (sys != 1)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 1008, 0, "%s", "sys == SCR_SYS_GAME");
    if (scrVarPub.varUsagePos)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 1011, 0, "%s", "!scrVarPub.varUsagePos");
    scrVarPub.varUsagePos = "<save game variable>";
    memset(scrVmDebugPub.profileEnable, 0, sizeof(scrVmDebugPub.profileEnable));
    MemFile_ReadData(memFile, 4, v23);
    scrVarPub.time = *(unsigned int *)v23;
    MemFile_ReadData(memFile, 2, v23);
    scrVarPub.savecount = *(_WORD *)v23;
    Com_Memset(scrVarPub.saveIdMap, 0, 0x10000);
    Com_Memset(scrVarPub.saveIdMapRev, 0, 0x10000);
    memset(idHistory, 0, 32);
    savecount = scrVarPub.savecount;
    g_idHistoryIndex = 0;
    v4 = 1;
    if (scrVarPub.savecount)
    {
        v5 = &scrVarPub.saveIdMapRev[1];
        do
        {
            v6 = AllocObject();
            *v5++ = v6;
            scrVarPub.saveIdMap[v6] = v4++;
            savecount = scrVarPub.savecount;
        } while (v4 <= scrVarPub.savecount);
    }
    v7 = 1;
    if (savecount)
    {
        v8 = &scrVarPub.saveIdMapRev[1];
        do
        {
            Scr_DoLoadObjectInfo(*v8, memFile);
            ++v7;
            ++v8;
        } while (v7 <= scrVarPub.savecount);
    }
    Scr_ReadGameEntry(memFile);
    MemFile_ReadData(memFile, 1, v23);
    v9 = v23[0];
    if (scrVarPub.levelId)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 1042, 0, "%s", "!scrVarPub.levelId");
    Id = Scr_ReadId(memFile, v9);
    scrVarPub.levelId = Id;
    if (scrVarDebugPub)
        ++scrVarDebugPub->extRefCount[Id];
    MemFile_ReadData(memFile, 1, v23);
    v11 = v23[0];
    if (scrVarPub.animId)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 1050, 0, "%s", "!scrVarPub.animId");
    v12 = Scr_ReadId(memFile, v11);
    scrVarPub.animId = v12;
    if (scrVarDebugPub)
        ++scrVarDebugPub->extRefCount[v12];
    MemFile_ReadData(memFile, 1, v23);
    v13 = v23[0];
    if (scrVarPub.timeArrayId)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 1058, 0, "%s", "!scrVarPub.timeArrayId");
    v14 = Scr_ReadId(memFile, v13);
    scrVarPub.timeArrayId = v14;
    if (scrVarDebugPub)
        ++scrVarDebugPub->extRefCount[v14];
    MemFile_ReadData(memFile, 1, v23);
    v15 = v23[0];
    if (scrVarPub.pauseArrayId)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            1066,
            0,
            "%s",
            "!scrVarPub.pauseArrayId");
    v16 = Scr_ReadId(memFile, v15);
    scrVarPub.pauseArrayId = v16;
    if (scrVarDebugPub)
        ++scrVarDebugPub->extRefCount[v16];
    MemFile_ReadData(memFile, 1, v23);
    v17 = v23[0];
    if (scrVarPub.freeEntList)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 1074, 0, "%s", "!scrVarPub.freeEntList");
    v18 = Scr_ReadId(memFile, v17);
    scrVarPub.freeEntList = v18;
    if (scrVarDebugPub)
        ++scrVarDebugPub->extRefCount[v18];
    v19 = 4;
    p_entArrayId = &g_classMap[0].entArrayId;
    for (v19 = 0; v19 < 4; ++v19)
    {
        // scr_classStruct_t widened on LP64 (its name pointer grew), so the
        // ILP32 +6 uint16 stride walked into the following entries' name
        // pointer and loaded class 1..3 roots out of unrelated memory.  Walk
        // the array natively so every class entity array is read back.
        const uint16_t entArrayId = g_classMap[v19].entArrayId;
        if (GetArraySize(entArrayId))
            MyAssertHandler(
                "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
                1083,
                0,
                "%s",
                "!GetArraySize( g_classMap[classnum].entArrayId )");
        if (scrVarDebugPub)
            --scrVarDebugPub->extRefCount[entArrayId];
        RemoveRefToObject(entArrayId);
        MemFile_ReadData(memFile, 1, v23);
        v21 = Scr_ReadId(memFile, v23[0]);
        v22 = scrVarDebugPub;
        g_classMap[v19].entArrayId = v21;
        if (v22)
            ++v22->extRefCount[v21];
    }
}

static void Scr_AddDebugExprValueRefCount(unsigned __int16 *refCount, sval_u *val)
{
    if (val->type == 81)
        ++refCount[val[1].type];
}

static void Scr_AddDebugExprRefCount(unsigned __int16 *refCount, sval_u *debugExprHead)
{
    sval_u *i; // r31

    for (i = debugExprHead; i; i = (sval_u *)i->type)
        Scr_AddDebugExprValueRefCount(refCount, i + 1);
}

static void Scr_AddDebugRefCountChildren(Scr_WatchElement_s *element, unsigned __int16 *refCount)
{
    Scr_WatchElement_s *i; // r31

    if (element->valueDefined && element->value.type == VAR_POINTER)
        ++refCount[element->value.u.intValue];
    for (i = element->childHead; i; i = i->next)
        Scr_AddDebugRefCountChildren(i, refCount);
}

static void Scr_AddDebugRefCount(unsigned __int16 *refCount)
{
    Scr_WatchElement_s *i; // r31

    for (i = scrDebuggerGlob.scriptWatch.elementHead; i; i = i->next)
    {
        if (!i->breakpoint)
        {
            if (!i->expr.exprHead)
                MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_debugger.cpp", 7231, 0, "%s", "expr->exprHead");
            Scr_AddDebugExprRefCount(refCount, (sval_u*)i->expr.exprHead);
        }
        Scr_AddDebugRefCountChildren(i, refCount);
    }
}

static bool Scr_IsVariableBreakpoint(unsigned int id)
{
    Scr_WatchElementDoubleNode_t **variableBreakpoints; // r11

    variableBreakpoints = scrDebuggerGlob.variableBreakpoints;
    if (!scrDebuggerGlob.variableBreakpoints)
    {
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_debugger.cpp",
            7193,
            0,
            "%s",
            "scrDebuggerGlob.variableBreakpoints");
        variableBreakpoints = scrDebuggerGlob.variableBreakpoints;
    }
    return variableBreakpoints[id] != 0;
}

// The leak check is the only walker of the whole variable list, and it is the
// only thing CheckReferences (called from Scr_LoadShutdown, i.e. the savegame
// load path alone) does.  Every index it feeds to refCount[] must be a real
// variable id: anything else is a write far outside scrVarDebugPub_t, which is
// how a load-time death in here corrupts something else instead of failing.
// The counters are gated on the same diag flag the save/load stage markers use
// (G_LoadMainState copies com_diagMarkers into it), which keeps this TU free of
// a dvar dependency the host proofs would have to satisfy.
bool scrLoadDiagEnabled;

static bool ScrLoadDiagOn()
{
    return scrLoadDiagEnabled;
}

struct ScrRefCountDiag
{
    unsigned int outOfRange;
    unsigned int maxIndex;
    unsigned int byCase[0x20];
    unsigned int arrayWalkSteps;
    unsigned int arrayWalkBailed;
};

static ScrRefCountDiag g_scrRefCountDiag;

static void ScrRefCountAdd(unsigned int index, unsigned int refCase)
{
    ++g_scrRefCountDiag.byCase[refCase & 0x1F];
    if (index >= 0x8000u)
    {
        if (++g_scrRefCountDiag.outOfRange <= 8)
        {
            Com_Printf(
                0,
                "KISAK_SAVE_REFCOUNT out_of_range index=%u case=0x%x\n",
                index,
                refCase);
        }
        return;
    }
    if (index > g_scrRefCountDiag.maxIndex)
        g_scrRefCountDiag.maxIndex = index;
    ++scrVarDebugPub->refCount[index];
}

static void CheckReferenceRange(unsigned int begin, unsigned int end)
{
    if ((int)(end - begin) <= 1)
        return;

    for (unsigned int parentType = 1; (int)parentType < (int)(end - begin); ++parentType)
    {
        VariableValueInternal *entry = &scrVarGlob.variableList[begin + parentType];
        unsigned int status = entry->w.status;
        if ((status & 0x60) == 0)
            continue;

        switch (status & VAR_MASK)
        {
        case VAR_POINTER:
            ScrRefCountAdd(entry->u.u.intValue, 1);
            break;

        case VAR_STACK:
        {
            VariableStackBuffer *sb = entry->u.u.stackValue;
            if (!sb->localId)
                MyAssertHandler(
                    "c:\\trees\\cod3\\cod3src\\src\\script\\scr_variable.cpp",
                    271,
                    0,
                    "%s",
                    "entryValue->u.u.stackValue->localId");
            ScrRefCountAdd(sb->localId, 0xA);
            unsigned __int8 *p = (unsigned __int8 *)&sb->buf[0];
            unsigned int count = sb->size;
            while (count)
            {
                unsigned int entryType = *p;
                uintptr_t entryVal = *reinterpret_cast<const uintptr_t *>(p + 1);
                p += kScriptStackValueSize;
                --count;
                if (entryType == VAR_POINTER)
                    ScrRefCountAdd(static_cast<unsigned int>(entryVal), 1);
            }
            break;
        }

        case VAR_THREAD:
        case VAR_NOTIFY_THREAD:
        case VAR_TIME_THREAD:
        case VAR_DEAD_ENTITY:
            ScrRefCountAdd(entry->u.o.u.size, status & 0x1F);
            break;

        case VAR_CHILD_THREAD:
            ScrRefCountAdd(GetParentLocalId(parentType), 0x11);
            ScrRefCountAdd(entry->u.o.u.size, 0x11);
            break;

        case VAR_ARRAY:
            // A sibling chain that points back into itself would spin here
            // forever; a chain that points outside the child list would read
            // unrelated records.  Both are bounded and reported.
            for (unsigned int i = FindFirstSibling(parentType); i; i = FindNextSibling(i))
            {
                if (++g_scrRefCountDiag.arrayWalkSteps > 0x20000u)
                {
                    ++g_scrRefCountDiag.arrayWalkBailed;
                    if (g_scrRefCountDiag.arrayWalkBailed <= 8)
                        Com_Printf(
                            0,
                            "KISAK_SAVE_REFCOUNT array_walk_bailed id=%u sibling=%u\n",
                            parentType,
                            i);
                    break;
                }
                VariableValueInternal *child = &scrVarGlob.variableList[i + VARIABLELIST_CHILD_BEGIN];
                if (IsObject(child))
                    MyAssertHandler(
                        "c:\\trees\\cod3\\cod3src\\src\\script\\scr_variable.cpp",
                        261,
                        0,
                        "%s",
                        "!IsObject( entryValue2 )");

                VariableValue val = Scr_GetArrayIndexValue(child->w.status >> 8);
                if (val.type == VAR_POINTER)
                    ScrRefCountAdd(val.u.intValue, 0x16); // array index that is an object id
            }
            break;

        default:
            break;
        }
    }
}

static int CheckReferences()
{
    if (!scrVarDebugPub || scrStringDebugGlob && scrStringDebugGlob->ignoreLeaks)
        return 1;
    const bool diagOn = ScrLoadDiagOn();
    if (diagOn)
        Com_Printf(0, "KISAK_SAVE_STAGE checkrefs=begin\n");
    memset(&g_scrRefCountDiag, 0, sizeof(g_scrRefCountDiag));
    memcpy(scrVarDebugPub->refCount, scrVarDebugPub->extRefCount, sizeof(scrVarDebugPub->refCount));
    if (diagOn)
        Com_Printf(0, "KISAK_SAVE_STAGE checkrefs=copy_done\n");
    Scr_AddDebugRefCount(scrVarDebugPub->refCount);
    if (diagOn)
        Com_Printf(0, "KISAK_SAVE_STAGE checkrefs=debugrefs_done\n");
    CheckReferenceRange(1u, 0x8001u);
    if (diagOn)
        Com_Printf(0, "KISAK_SAVE_STAGE checkrefs=range_parents_done\n");
    CheckReferenceRange(0x8002u, 0x18000u);
    if (diagOn)
        Com_Printf(0, "KISAK_SAVE_STAGE checkrefs=range_children_done\n");
    if (scrVarPub.developer && scrDebuggerGlob.variableBreakpoints)
    {
        // The retail loop wrote the refCount[id] bias through the
        // varUsage + 0x70002 byte alias, which lands inside varUsage on
        // LP64; address refCount by its real id index instead.
        //
        // Scr_ShutdownDebuggerMain (called by this load's Scr_ShutdownSystem)
        // frees the breakpoint table and zeroes the pointer, so the bias only
        // exists when the debugger actually kept one: it is the same table the
        // -1 bias is derived from, and with no table there are no variable
        // breakpoints to account for.
        for (unsigned int index = 1; index <= 0x7FFF; ++index)
        {
            if (Scr_IsVariableBreakpoint(index + 1))
                ++scrVarDebugPub->refCount[index];
        }
    }
    for (unsigned int entryIndex = 2; entryIndex <= 0x8000; ++entryIndex)
    {
        // The retail walk stepped 16 bytes through an ILP32 record.  On LP64
        // the records are native-sized, and refCount[entryIndex - 1] is the
        // same slot the old varUsage + 0x70002 byte alias addressed
        // (refCount[id], with parent id = entryIndex - 1).
        VariableValueInternal *entry = &scrVarGlob.variableList[entryIndex];
        if ((entry->w.status & 0x60) == 0
            || (entry->w.type & 0x1Fu) < 0xE
            || (scrVarDebugPub->refCount[entryIndex - 1]
                && scrVarDebugPub->refCount[entryIndex - 1]
                    == (unsigned __int16)(entry->u.next + 1)))
            continue;
        if (diagOn)
            Com_Printf(
                0,
                "KISAK_SAVE_STAGE checkrefs=mismatch id=%u refcount=%u ref=%u type=0x%x\n",
                entryIndex - 1,
                scrVarDebugPub->refCount[entryIndex - 1],
                entry->u.next,
                entry->w.type);
        return 0;
    }
    if (diagOn)
    {
        Com_Printf(
            0,
            "KISAK_SAVE_STAGE checkrefs=done case1=%u caseA=%u caseThr=%u caseChld=%u caseArr=%u caseArrVals=%u maxIndex=%u outOfRange=%u arraySteps=%u arrayBailed=%u\n",
            g_scrRefCountDiag.byCase[1],
            g_scrRefCountDiag.byCase[0xA],
            g_scrRefCountDiag.byCase[0xE] + g_scrRefCountDiag.byCase[0xF]
                + g_scrRefCountDiag.byCase[0x10] + g_scrRefCountDiag.byCase[0x13],
            g_scrRefCountDiag.byCase[0x11],
            g_scrRefCountDiag.byCase[0x15],
            g_scrRefCountDiag.byCase[0x16],
            g_scrRefCountDiag.maxIndex,
            g_scrRefCountDiag.outOfRange,
            g_scrRefCountDiag.arrayWalkSteps,
            g_scrRefCountDiag.arrayWalkBailed);
    }
    return 1;
}

// The removal walk frees every object the savegame load created, one call per
// saved id.  G_LoadMainState names its own stages, but a death inside this walk
// still prints nothing, so the walk reports its shape instead of every object:
// a type census, the first entries, and any entry whose saved id or status is
// not one this VM can hold.
struct ScrLoadWalkDiag
{
    unsigned int objects;
    unsigned int byType[0x20];
    unsigned int refZero;
    unsigned int anomalies;
};

static ScrLoadWalkDiag g_scrLoadWalkDiag;

static void ScrLoadShutdownDiag(bool enabled, const char *stage, unsigned int index)
{
    if (!enabled)
        return;
    if (index)
    {
        const unsigned int id = scrVarPub.saveIdMapRev[index];
        VariableValueInternal *entry =
            (id >= 1 && id < VARIABLELIST_PARENT_SIZE) ? &scrVarGlob.variableList[id + 1] : 0;
        const unsigned int type = entry ? (entry->w.type & 0x1F) : 0xFFFFFFFFu;
        const unsigned int status = entry ? (entry->w.status & VAR_STAT_MASK) : 0;
        const unsigned int ref = entry ? entry->u.next : 0;

        ++g_scrLoadWalkDiag.objects;
        if (type < 0x20)
            ++g_scrLoadWalkDiag.byType[type];
        if (entry && !ref)
            ++g_scrLoadWalkDiag.refZero;

        const bool anomaly = !entry || status != VAR_STAT_EXTERNAL
                             || type < VAR_THREAD || type > VAR_ARRAY || id >= VARIABLELIST_PARENT_SIZE;
        const bool named = index <= 8 || (index % 512) == 0;
        if ((anomaly || named) && g_scrLoadWalkDiag.anomalies < 16)
        {
            if (anomaly)
                ++g_scrLoadWalkDiag.anomalies;
            Com_Printf(
                0,
                "KISAK_SAVE_OBJ i=%u id=%u status=0x%x type=0x%x ref=%u sibling=%u%s\n",
                index,
                id,
                entry ? entry->w.status : 0,
                entry ? entry->w.type : 0,
                ref,
                entry ? entry->nextSibling : 0,
                anomaly ? " anomaly=1" : "");
        }
        return;
    }
    if (!stage[0])
        return;
    Com_Printf(0, "KISAK_SAVE_STAGE %s\n", stage);
}

static void ScrLoadWalkDiagCensus(bool enabled)
{
    if (!enabled)
        return;
    Com_Printf(
        0,
        "KISAK_SAVE_STAGE scrshutdown=census objects=%u refZero=%u anomalies=%u "
        "t14=%u t15=%u t16=%u t17=%u t18=%u t19=%u t20=%u t21=%u other=%u\n",
        g_scrLoadWalkDiag.objects,
        g_scrLoadWalkDiag.refZero,
        g_scrLoadWalkDiag.anomalies,
        g_scrLoadWalkDiag.byType[14],
        g_scrLoadWalkDiag.byType[15],
        g_scrLoadWalkDiag.byType[16],
        g_scrLoadWalkDiag.byType[17],
        g_scrLoadWalkDiag.byType[18],
        g_scrLoadWalkDiag.byType[19],
        g_scrLoadWalkDiag.byType[20],
        g_scrLoadWalkDiag.byType[21],
        g_scrLoadWalkDiag.objects
            - (g_scrLoadWalkDiag.byType[14] + g_scrLoadWalkDiag.byType[15]
               + g_scrLoadWalkDiag.byType[16] + g_scrLoadWalkDiag.byType[17]
               + g_scrLoadWalkDiag.byType[18] + g_scrLoadWalkDiag.byType[19]
               + g_scrLoadWalkDiag.byType[20] + g_scrLoadWalkDiag.byType[21]));
}

void __cdecl Scr_LoadShutdown()
{
    unsigned int v0; // r30
    unsigned __int16 *v1; // r31
    const bool diagOn = ScrLoadDiagOn();

    ScrLoadShutdownDiag(diagOn, "scrshutdown=begin", 0);
    if (diagOn)
    {
        Com_Printf(0, "KISAK_SAVE_STAGE scrshutdown=savecount savecount=%u\n", scrVarPub.savecount);
        // Which of the shutdown's branches are live: Scr_InitDebuggerSystem()
        // and the leak check below both key off these, and the port runs with
        // scrVarPub.developer set whenever com_logfile defaults to 1.
        Com_Printf(
            0,
            "KISAK_SAVE_STAGE scrshutdown=state developer=%d debugPub=%d ignoreLeaks=%d initedSystem=%d\n",
            scrVarPub.developer ? 1 : 0,
            scrVarDebugPub ? 1 : 0,
            (scrStringDebugGlob && scrStringDebugGlob->ignoreLeaks) ? 1 : 0,
            scrDebuggerGlob.debugger_inited_system ? 1 : 0);
    }
    v0 = 1;
    if (scrVarPub.savecount)
    {
        ScrLoadWalkProbeStart();
        v1 = &scrVarPub.saveIdMapRev[1];
        do
        {
            ScrLoadShutdownDiag(diagOn, 0, v0);
            RemoveRefToObject(*v1);
            ++v0;
            ++v1;
        } while (v0 <= scrVarPub.savecount);
    }
    ScrLoadShutdownDiag(diagOn, "scrshutdown=objects=done", 0);
    ScrLoadWalkDiagCensus(diagOn);
    if (diagOn)
        ScrLoadWalkProbeReport();
#if defined(__SWITCH__)
    // Same divergence Scr_InitGameVariable takes (scr_vm.cpp): the Switch SP
    // runtime has no remote script-debugger client, so this tail only needs the
    // debugger state the VM itself reads.  Running the full system init here
    // was wrong twice over: it re-initializes debugger UI state that the map
    // path deliberately leaves to Scr_InitDebuggerBootState, and its
    // ScriptList::LoadScriptPos walks the script-window array that this load's
    // own Scr_ShutdownSystem had just freed.
    Scr_InitDebuggerBootState();
#else
    Scr_InitDebuggerSystem();
#endif
    ScrLoadShutdownDiag(diagOn, "scrshutdown=debugger=done", 0);
    scrVarPub.varUsagePos = 0;
    if (!CheckReferences())
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 1115, 0, "%s", "CheckReferences()");
    ScrLoadShutdownDiag(diagOn, "scrshutdown=done", 0);
}

void __cdecl DoSaveEntryInternal(unsigned int type, VariableUnion *u, MemoryFile *memFile)
{
    unsigned int UsedSize; // r3
    unsigned int v7; // r3
    unsigned int v8; // r3
    unsigned int v9; // r3
    MemoryFile *v10; // r3
    double v11; // fp8
    double v12; // fp7
    double v13; // fp6
    double v14; // fp5
    double v15; // fp4
    double v16; // fp3
    double v17; // fp2
    double v18; // fp1
    unsigned int v19; // r3
    const char *v20; // r3
    unsigned int v21; // r3
    unsigned int v22; // r3
    unsigned int v23; // r3
    unsigned int v24; // r3
    unsigned int v25; // r3
    float v26; // [sp+8h] [-78h]
    float v27; // [sp+10h] [-70h]
    float v28; // [sp+18h] [-68h]
    float v29; // [sp+20h] [-60h]
    _BYTE v30[4]; // [sp+50h] [-30h] BYREF
    unsigned int v31[11]; // [sp+54h] [-2Ch] BYREF

    if (type != (unsigned __int8)type)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            1122,
            0,
            "%s",
            "type == (unsigned char)type");
    if (type == VAR_POINTER)
    {
        UsedSize = MemFile_GetUsedSize(memFile);
        //ProfMem_Begin("pointer", UsedSize);
        WriteId((unsigned int)(intptr_t)u, 1u, memFile);
        v7 = MemFile_GetUsedSize(memFile);
        //ProfMem_End(v7);
    }
    else
    {
        v8 = MemFile_GetUsedSize(memFile);
        //ProfMem_Begin("type", v8);
        v30[0] = 8 * type;
        MemFile_WriteData(memFile, 1, v30);
        v9 = MemFile_GetUsedSize(memFile);
        //ProfMem_End(v9);
        switch (type)
        {
        case VAR_UNDEFINED:
        case VAR_PRECODEPOS:
            return;
        case VAR_STRING:
        case VAR_ISTRING:
            v19 = MemFile_GetUsedSize(memFile);
            //ProfMem_Begin("string", v19);
            v20 = SL_ConvertToString((unsigned __int16)(intptr_t)u);
            MemFile_WriteCString(memFile, v20);
            v21 = MemFile_GetUsedSize(memFile);
            //ProfMem_End(v21);
            break;
        case VAR_VECTOR:
            WriteVector(&u->floatValue, memFile);
            break;
        case VAR_FLOAT:
            WriteFloat(*(float *)&u, memFile);
            break;
        case VAR_INTEGER:
            v22 = MemFile_GetUsedSize(memFile);
            //ProfMem_Begin("int", v22);
            v31[0] = (unsigned int)(uintptr_t)u;
            MemFile_WriteData(memFile, 4, v31);
            v23 = MemFile_GetUsedSize(memFile);
            //ProfMem_End(v23);
            break;
        case VAR_CODEPOS:
        case VAR_FUNCTION:
            WriteCodepos((const char *)u, memFile);
            break;
        case VAR_STACK:
            v24 = MemFile_GetUsedSize(memFile);
            //ProfMem_Begin("stack", v24);
            WriteStack((const VariableStackBuffer *)u, memFile);
            v25 = MemFile_GetUsedSize(memFile);
            //ProfMem_End(v25);
            break;
        case VAR_ANIMATION:
            v31[0] = (unsigned int)(uintptr_t)u;
            MemFile_WriteData(memFile, 4, v31);
            break;
        default:
            if (!alwaysfails)
                MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 1172, 0, "unknown type");
            break;
        }
    }
}

void __cdecl Scr_SaveSource(MemoryFile *memFile)
{
    MemFile_WriteData(memFile, 1, &scrVarPub.developer);
    MemFile_WriteData(memFile, 1, &scrVarPub.developer_script);
}

void __cdecl SaveMemory_SaveWriteImmediate(const void *buffer, unsigned int len, SaveImmediate *save)
{
    if (!save || !save->f || !buffer || !len)
        return;
    int handle = (int)(intptr_t)save->f;
    FS_Write((const char *)buffer, len, handle);
}

void __cdecl Scr_SaveSourceImmediate(SaveImmediate *save)
{
    if (!scrVarPub.developer)
        return;

    int archivedCount = 0;
    int countOut = 0;
    unsigned int sourceBufferLookupLen = scrParserPub.sourceBufferLookupLen;
    // LP64: index the records; the IDA form stepped a bool * by 44 (ILP32
    // sizeof(SourceBufferInfo)), but LP64 sizeof is 56, so it counted stray
    // bytes and the saved count disagreed with the entries written below.
    for (unsigned int k = 0; k < sourceBufferLookupLen; ++k)
    {
        if (scrParserPub.sourceBufferLookup[k].archive)
            countOut = ++archivedCount;
    }
    SaveMemory_SaveWriteImmediate(&countOut, 4u, save);

    unsigned int i = 0;
    if (scrParserPub.sourceBufferLookupLen)
    {
        int idx = 0;
        do
        {
            SourceBufferInfo *info = &scrParserPub.sourceBufferLookup[idx];
            if (scrParserPub.sourceBufferLookup[idx].archive)
            {
                SaveMemory_SaveWriteImmediate(&info->len, 4u, save);
                int len = info->len;
                if (len > 0)
                    SaveMemory_SaveWriteImmediate(info->sourceBuf, (unsigned int)len, save);
            }
            ++i;
            ++idx;
        } while (i < scrParserPub.sourceBufferLookupLen);
    }
}

void __cdecl Scr_LoadSource(MemoryFile *memFile, void *fileHandle)
{
    SaveSourceBufferInfo *saveSourceBufferLookup; // r3
    signed int v5; // r25
    int v6; // r27
    SaveSourceBufferInfo *v7; // r31
    int *p_len; // r30
    int len; // r3
    char *v10; // r3
    int v11; // r4
    char v12; // [sp+50h] [-50h] BYREF

    if (scrParserGlob.saveSourceBufferLookup)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            1236,
            0,
            "%s",
            "!scrParserGlob.saveSourceBufferLookup");
    MemFile_ReadData(memFile, 1, (unsigned char*)&v12);
    MemFile_ReadData(memFile, 1, (unsigned char*)&scrVarPub.developer_script);
    if (v12)
    {
        const int bytesRead = ReadFromDevice(
            &scrParserGlob.saveSourceBufferLookupLen, 4, fileHandle);
        if (bytesRead != 4 || scrParserGlob.saveSourceBufferLookupLen == 0)
        {
            scrParserGlob.saveSourceBufferLookupLen = 0;
            scrParserGlob.saveSourceBufferLookup = 0;
            return;
        }
        // LP64: SaveSourceBufferInfo holds a pointer (16 bytes, not 8).
        saveSourceBufferLookup = (SaveSourceBufferInfo *)Hunk_AllocDebugMem(
            sizeof(SaveSourceBufferInfo) * scrParserGlob.saveSourceBufferLookupLen,
            "Scr_LoadSource");
        scrParserGlob.saveSourceBufferLookup = saveSourceBufferLookup;
        v5 = scrParserGlob.saveSourceBufferLookupLen - 1;
        if ((signed int)(scrParserGlob.saveSourceBufferLookupLen - 1) >= 0)
        {
            v6 = v5;
            while (1)
            {
                v7 = &saveSourceBufferLookup[v6];
                p_len = &saveSourceBufferLookup[v6].len;
                ReadFromDevice(p_len, 4, fileHandle);
                len = v7->len;
                if (len <= 0)
                {
                    v7->sourceBuf = 0;
                }
                else
                {
                    v10 = (char *)Hunk_AllocDebugMem(len, "Scr_LoadSource");
                    v11 = *p_len;
                    v7->sourceBuf = v10;
                    ReadFromDevice(v10, v11, fileHandle);
                }
                --v5;
                --v6;
                if (v5 < 0)
                    break;
                saveSourceBufferLookup = scrParserGlob.saveSourceBufferLookup;
            }
        }
    }
}

void __cdecl Scr_SkipSource(MemoryFile *memFile, void *fileHandle)
{
    int i; // r30
    _BYTE v5[4]; // [sp+50h] [-30h] BYREF
    int v6; // [sp+54h] [-2Ch] BYREF
    int v7[4]; // [sp+58h] [-28h] BYREF

    MemFile_ReadData(memFile, 1, v5);
    MemFile_ReadData(memFile, 1, (unsigned char*)&scrVarPub.developer_script);
    if (v5[0])
    {
        if (fileHandle)
        {
            v6 = 0;
            const int bytesRead = ReadFromDevice(&v6, 4, fileHandle);
            if (bytesRead != 4 || v6 <= 0)
                return;
            for (i = v6 - 1; i >= 0; --i)
            {
                ReadFromDevice(v7, 4, fileHandle);
                if (v7[0] > 0)
                    ReadFromDevice(0, v7[0], fileHandle);
            }
        }
    }
}

void __cdecl AddSaveStackInternal(const VariableStackBuffer *stackBuf)
{
    int localId; // r7
    unsigned __int16 size; // r11
    const char *buf; // r31
    unsigned int v4; // r3
    unsigned __int16 v5; // r30
    uintptr_t v6; // r4

    localId = stackBuf->localId;
    if (stackBuf->localId && !scrVarPub.saveIdMap[localId])
    {
        scrVarPub.saveIdMap[localId] = ++scrVarPub.savecount;
        *(unsigned __int16 *)((char *)scrVarPub.saveIdMapRev + __ROL4__(scrVarPub.savecount, 1)) = localId;
    }
    size = stackBuf->size;
    buf = stackBuf->buf;
    if (size)
    {
        do
        {
            v4 = (unsigned __int8)*buf;
            v5 = size - 1;
            v6 = *reinterpret_cast<const uintptr_t *>(buf + 1);
            buf += kScriptStackValueSize;
            AddSaveEntryInternal(v4, reinterpret_cast<const VariableStackBuffer *>(v6));
            size = v5;
        } while (v5);
    }
}

void __cdecl AddSaveEntryInternal(unsigned int type, const VariableStackBuffer *u)
{
    if (type == VAR_POINTER)
    {
        if (u && !scrVarPub.saveIdMap[(unsigned int)(intptr_t)u])
        {
            scrVarPub.saveIdMap[(unsigned int)(intptr_t)u] = ++scrVarPub.savecount;
            *(unsigned __int16 *)((char *)scrVarPub.saveIdMapRev + __ROL4__(scrVarPub.savecount, 1)) = (unsigned __int16)(intptr_t)u;
        }
    }
    else if (type == VAR_STACK)
    {
        AddSaveStackInternal(u);
    }
}

// local variable allocation has failed, the output may be wrong!
void __cdecl DoSaveEntry(VariableValue *value, VariableValue *name, bool isArray, MemoryFile *memFile)
{
    unsigned int UsedSize; // r3
    unsigned int v9; // r3
    unsigned int v10; // r3
    unsigned int v11; // r4
    unsigned int v12; // r3
    unsigned int v13; // r3
    unsigned int v14; // r3
    VariableValue *ArrayIndexValue; // r3 OVERLAPPED
    unsigned int v16; // r3
    int v17; // r30
    unsigned int v18; // r3
    unsigned int v19; // r3
    unsigned int v20; // r3
    unsigned int v21; // r3
    unsigned int v22; // r3
    const char *v23; // r3
    unsigned int v24; // r3
    unsigned int v25; // r3
    unsigned int v26; // r3
    unsigned int v27[2]; // [sp+50h] [-40h] BYREF
    __int64 v28; // [sp+58h] [-38h]

    if (!value)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 391, 0, "%s", "value");
    UsedSize = MemFile_GetUsedSize(memFile);
    //ProfMem_Begin("DoSaveEntry", UsedSize);
    v9 = MemFile_GetUsedSize(memFile);
    //ProfMem_Begin("DoSaveEntryInternal", v9);
    DoSaveEntryInternal(value->type,
                        reinterpret_cast<VariableUnion *>(value->u.pointerValue), memFile);
    v10 = MemFile_GetUsedSize(memFile);
    //ProfMem_End(v10);
    if (!isArray)
    {
        v12 = MemFile_GetUsedSize(memFile);
        //ProfMem_Begin("non-array", v12);
        if (((unsigned int)(intptr_t)name & 0xFF000000) != 0)
            MyAssertHandler(
                "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
                402,
                0,
                "%s\n\t(name) = %i",
                "(!(name & 0xFF000000))",
                name);
        v27[0] = (v27[0] & 0xFFFFFF00) | ((uint8_t)(intptr_t)name);
        MemFile_WriteData(memFile, 1, v27);
        v27[0] = (v27[0] & 0xFFFFFF00) | (((uint32_t)(intptr_t)name >> 8) & 0xFF);
        MemFile_WriteData(memFile, 1, v27);
        v27[0] = (v27[0] & 0xFFFFFF00) | (((uint32_t)(intptr_t)name >> 16) & 0xFF);
        MemFile_WriteData(memFile, 1, v27);
        v13 = MemFile_GetUsedSize(memFile);
        //ProfMem_End(v13);
        v14 = MemFile_GetUsedSize(memFile);
        //ProfMem_End(v14);
        return;
    }
    VariableValue arrVal = Scr_GetArrayIndexValue((unsigned int)(intptr_t)name);
    if (arrVal.type == VAR_POINTER)
    {
        WriteId(arrVal.u.intValue, 5u, memFile);
        return;
    }
    if (arrVal.type == VAR_STRING)
    {
        v27[0] = (v27[0] & 0xFFFFFF00) | 4;
        MemFile_WriteData(memFile, 1, v27);
        v23 = SL_ConvertToString((unsigned int)arrVal.u.intValue & 0xFFFF);
        MemFile_WriteCString(memFile, v23);
        return;
    }
    if (arrVal.type != VAR_INTEGER)
    {
        if (!alwaysfails)
            MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 449, 1, "bad case");
        return;
    }
    v17 = arrVal.u.intValue;
    if (v17)
    {
        if (v17 < -128 || v17 >= 128)
        {
            if (v17 < -32768 || v17 >= 0x8000)
            {
                v27[0] = (v27[0] & 0xFFFFFF00) | 3;
                MemFile_WriteData(memFile, 1, v27);
                v27[0] = v17;
                MemFile_WriteData(memFile, 4, v27);
            }
            else
            {
                v27[0] = (v27[0] & 0xFFFFFF00) | 2;
                MemFile_WriteData(memFile, 1, v27);
                v27[0] = (v27[0] & 0xFFFF0000) | ((uint32_t)v17 & 0xFFFF);
                MemFile_WriteData(memFile, 2, v27);
            }
        }
        else
        {
            v27[0] = (v27[0] & 0xFFFFFF00) | 1;
            MemFile_WriteData(memFile, 1, v27);
            v27[0] = (v27[0] & 0xFFFFFF00) | ((uint32_t)v17 & 0xFF);
            MemFile_WriteData(memFile, 1, v27);
        }
    }
    else
    {
        v27[0] = v27[0] & 0xFFFFFF00;
        MemFile_WriteData(memFile, 1, v27);
    }
}

void __cdecl AddSaveObjectChildren(unsigned int parentId)
{
    VariableValueInternal *parentValue; // r23
    int parentType; // r24
    unsigned int i; // r29
    VariableValueInternal *entryValue; // r30
    unsigned int v6; // r4
    int v7; // r2
    VariableValueInternal_u u; // r3
    int v9; // r11
    VariableValueInternal_w w; // r11
    int v11; // r9
    int size; // r9

    parentValue = &scrVarGlob.variableList[parentId + 1];

    iassert((parentValue->w.status & VAR_STAT_MASK) != VAR_STAT_FREE);
    iassert(IsObject(parentValue));
    parentType = parentValue->w.type & 0x1F;
    for (i = FindLastSibling(parentId); i; i = FindPrevSibling(i))
    {
        // hash.id names the backing child slot.  The old expression was a
        // byte offset (id * 16) from the ILP32 layout; on LP64 each
        // VariableValueInternal is wider and that lands in the middle of an
        // unrelated record.
        entryValue = &scrVarGlob.variableList[
            scrVarGlob.variableList[i + VARIABLELIST_CHILD_BEGIN].hash.id +
            VARIABLELIST_CHILD_BEGIN];
        iassert(!IsObject(entryValue));
        if (parentType == VAR_ARRAY)
        {
            VariableValue arrVal = Scr_GetArrayIndexValue((unsigned int)entryValue->w.status >> 8);
            if (arrVal.type == VAR_POINTER
                && arrVal.u.intValue
                && !scrVarPub.saveIdMap[arrVal.u.intValue])
            {
                scrVarPub.saveIdMap[arrVal.u.intValue] = ++scrVarPub.savecount;
                scrVarPub.saveIdMapRev[scrVarPub.savecount] = (unsigned __int16)arrVal.u.intValue;
            }
        }
        u = entryValue->u;
        v9 = entryValue->w.type & 0x1F;
        if (v9 == VAR_POINTER)
        {
            if (u.u.intValue && !scrVarPub.saveIdMap[u.u.intValue])
            {
                scrVarPub.saveIdMap[u.u.intValue] = ++scrVarPub.savecount;
                scrVarPub.saveIdMapRev[scrVarPub.savecount] = (unsigned __int16)u.u.intValue;
            }
        }
        else if (v9 == VAR_STACK)
        {
            AddSaveStackInternal(u.u.stackValue);
        }
    }
    switch (parentType)
    {
    case VAR_THREAD:
    case VAR_NOTIFY_THREAD:
    case VAR_TIME_THREAD:
    case VAR_DEAD_ENTITY:
        goto LABEL_24;
    case VAR_CHILD_THREAD:
        w = parentValue->w;
        v11 = (unsigned __int16)((unsigned int)w.status >> 8);
        if ((unsigned __int16)((unsigned int)w.status >> 8) && !scrVarPub.saveIdMap[v11])
        {
            scrVarPub.saveIdMap[v11] = ++scrVarPub.savecount;
            *(unsigned __int16 *)((char *)scrVarPub.saveIdMapRev + __ROL4__(scrVarPub.savecount, 1)) = v11;
        }
    LABEL_24:
        size = parentValue->u.o.u.size;
        if (parentValue->u.o.u.size)
        {
            if (!scrVarPub.saveIdMap[size])
            {
                scrVarPub.saveIdMap[size] = ++scrVarPub.savecount;
                *(unsigned __int16 *)((char *)scrVarPub.saveIdMapRev + __ROL4__(scrVarPub.savecount, 1)) = size;
            }
        }
        break;
    default:
        return;
    }
}

void __cdecl AddSaveObject(unsigned int parentId)
{
    unsigned __int16 savecount; // r11
    int v2; // r29
    unsigned __int16 *v3; // r30

    savecount = scrVarPub.savecount;
    v2 = scrVarPub.savecount;
    if (parentId && !scrVarPub.saveIdMap[parentId])
    {
        ++scrVarPub.savecount;
        scrVarPub.saveIdMap[parentId] = v2 + 1;
        *(unsigned __int16 *)((char *)scrVarPub.saveIdMapRev + __ROL4__(scrVarPub.savecount, 1)) = parentId;
        savecount = scrVarPub.savecount;
    }
    if (v2 < savecount)
    {
        v3 = &scrVarPub.saveIdMapRev[v2];
        do
        {
            ++v3;
            ++v2;
            AddSaveObjectChildren(*v3);
        } while (v2 < scrVarPub.savecount);
    }
}

void __cdecl DoSaveObjectInfo(unsigned int parentId, MemoryFile *memFile)
{
    VariableValueInternal *v4; // r31
    int v5; // r30
    unsigned int UsedSize; // r3
    unsigned int v7; // r3
    int v8; // r4
    bool v9; // r26
    unsigned int v10; // r3
    __int16 v11; // r31
    unsigned int i; // r3
    unsigned int j; // r30
    VariableValueInternal *v14; // r31
    int v15; // r11
    VariableValueInternal_w w; // r11
    unsigned int v17; // r3
    VariableValue v18[12]; // [sp+50h] [-60h] BYREF

    v4 = &scrVarGlob.variableList[parentId + 1];
    if ((v4->w.status & 0x60) != 0x60)
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            655,
            0,
            "%s",
            "(parentValue->w.status & VAR_STAT_MASK) == VAR_STAT_EXTERNAL");
    if (!IsObject(v4))
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 656, 0, "%s", "IsObject( parentValue )");
    v5 = v4->w.type & 0x1F;
    switch (v5)
    {
    case VAR_THREAD:
        WriteId(v4->u.o.u.size, 1u, memFile);
        goto LABEL_14;
    case VAR_NOTIFY_THREAD:
        UsedSize = MemFile_GetUsedSize(memFile);
        //ProfMem_Begin("VAR_NOTIFY_THREAD", UsedSize);
        WriteId(v4->u.o.u.size, 2u, memFile);
        SafeWriteString((unsigned int)v4->w.status >> 8, memFile);
        v7 = MemFile_GetUsedSize(memFile);
        //ProfMem_End(v7);
        goto LABEL_14;
    case VAR_TIME_THREAD:
        WriteId(v4->u.o.u.size, 3u, memFile);
        v8 = 4;
        v18[0].u.intValue = (unsigned int)v4->w.status >> 8;
        goto LABEL_13;
    case VAR_CHILD_THREAD:
        WriteId(v4->u.o.u.size, 4u, memFile);
        WriteId((unsigned __int16)((unsigned int)v4->w.status >> 8), 0, memFile);
        goto LABEL_14;
    case VAR_DEAD_ENTITY:
        WriteId(v4->u.o.u.size, 5u, memFile);
        goto LABEL_14;
    case VAR_ENTITY:
        v18[0].u.intValue = (v18[0].u.intValue & 0xFFFFFF00) | (uint8_t)(-96);
        MemFile_WriteData(memFile, 1, v18);
        v18[0].u.intValue = (v18[0].u.intValue & 0xFFFF0000) | (v4->u.o.u.size & 0xFFFF);
        MemFile_WriteData(memFile, 2, v18);
        v8 = 2;
        v18[0].u.intValue = (v18[0].u.intValue & 0xFFFF0000) | (((unsigned int)v4->w.status >> 8) & 0xFFFF);
        goto LABEL_13;
    default:
        v8 = 1;
        v18[0].u.intValue = (v18[0].u.intValue & 0xFFFFFF00) | ((uint32_t)(8 * v5) & 0xFF);
    LABEL_13:
        MemFile_WriteData(memFile, v8, v18);
    LABEL_14:
        v9 = v5 == VAR_ARRAY;
        v10 = MemFile_GetUsedSize(memFile);
        //ProfMem_Begin("children", v10);
        v11 = 0;
        for (i = FindFirstSibling(parentId); i; i = FindNextSibling(i))
            ++v11;
        v18[0].u.intValue = (v18[0].u.intValue & 0xFFFF0000) | ((uint32_t)v11 & 0xFFFF);
        MemFile_WriteData(memFile, 2, v18);
        for (j = FindLastSibling(parentId); j; j = FindPrevSibling(j))
        {
            v14 = &scrVarGlob.variableList[
                scrVarGlob.variableList[j + VARIABLELIST_CHILD_BEGIN].hash.id +
                VARIABLELIST_CHILD_BEGIN];
            v15 = v14->w.status & 0x60;
            if (!v15 || v15 == 96)
                MyAssertHandler(
                    "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
                    715,
                    0,
                    "%s",
                    "(entryValue->w.status & VAR_STAT_MASK) != VAR_STAT_FREE && (entryValue->w.status & VAR_STAT_MASK) != VAR_STAT_EXTERNAL");
            if (IsObject(v14))
                MyAssertHandler(
                    "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
                    716,
                    0,
                    "%s",
                    "!IsObject( entryValue )");
            w = v14->w;
            v18[0].u = v14->u.u;
            v18[0].type = (Vartype_t)(w.type & 0x1F);
            DoSaveEntry(v18, (VariableValue *)((unsigned int)w.status >> 8), v9, memFile);
        }
        v17 = MemFile_GetUsedSize(memFile);
        //ProfMem_End(v17);
        return;
    }
}

int __cdecl Scr_ConvertThreadToSave(unsigned __int16 handle)
{
    int v1; // r31
    int v3; // r31

    v1 = handle;
    if (!handle)
        return 0;
    AddSaveObject(handle);
    v3 = v1;
    if (!scrVarPub.saveIdMap[v3])
        MyAssertHandler(
            "c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp",
            736,
            0,
            "%s",
            "scrVarPub.saveIdMap[handle]");
    return scrVarPub.saveIdMap[v3];
}

void __cdecl WriteGameEntry(MemoryFile *memFile)
{
    DoSaveEntryInternal(
        scrVarGlob.variableList[scrVarPub.gameId + VARIABLELIST_CHILD_BEGIN].w.type & 0x1F,
        reinterpret_cast<VariableUnion *>(scrVarGlob.variableList[
            scrVarPub.gameId + VARIABLELIST_CHILD_BEGIN].u.u.pointerValue),
        memFile);
}

void __cdecl Scr_SavePost(MemoryFile *memFile)
{
    unsigned int UsedSize; // r3
    unsigned int v3; // r29
    unsigned __int16 *v4; // r28
    unsigned int v5; // r3
    int v6; // r30
    unsigned __int16 *p_entArrayId; // r29
    unsigned int v8[12]; // [sp+50h] [-30h] BYREF

    memset(idHistory, 0, 0x20u);
    g_idHistoryIndex = 0;
    v8[0] = scrVarPub.time;
    MemFile_WriteData(memFile, 4, v8);
    v8[0] = (v8[0] & 0xFFFF0000) | ((uint32_t)scrVarPub.savecount & 0xFFFF);
    MemFile_WriteData(memFile, 2, v8);
    UsedSize = MemFile_GetUsedSize(memFile);
    //ProfMem_Begin("DoSaveObjectInfo", UsedSize);
    v3 = 1;
    if (scrVarPub.savecount)
    {
        v4 = &scrVarPub.saveIdMapRev[1];
        do
        {
            DoSaveObjectInfo(*v4, memFile);
            ++v3;
            ++v4;
        } while (v3 <= scrVarPub.savecount);
    }
    v5 = MemFile_GetUsedSize(memFile);
    //ProfMem_End(v5);
    DoSaveEntryInternal(
        scrVarGlob.variableList[scrVarPub.gameId + VARIABLELIST_CHILD_BEGIN].w.type & VAR_MASK,
        reinterpret_cast<VariableUnion *>(scrVarGlob.variableList[
            scrVarPub.gameId + VARIABLELIST_CHILD_BEGIN].u.u.pointerValue),
        memFile);
    WriteId(scrVarPub.levelId, 0, memFile);
    WriteId(scrVarPub.animId, 0, memFile);
    WriteId(scrVarPub.timeArrayId, 0, memFile);
    WriteId(scrVarPub.pauseArrayId, 0, memFile);
    WriteId(scrVarPub.freeEntList, 0, memFile);
    v6 = 4;
    p_entArrayId = &g_classMap[0].entArrayId;
    // Native class stride: the LP64 scr_classStruct_t is not 6 uint16 wide.
    for (v6 = 0; v6 < 4; ++v6)
        WriteId(g_classMap[v6].entArrayId, 0, memFile);
}

void __cdecl AddSaveStack(const VariableStackBuffer *stackBuf)
{
    int size; // r9
    CONST char *buf; // r31
    int v4; // r10
    __int16 v5; // r29
    uintptr_t v6; // r3

    AddSaveObject(stackBuf->localId);
    size = stackBuf->size;
    buf = stackBuf->buf;
    if (size)
    {
        do
        {
            v4 = (unsigned __int8)*buf;
            v5 = size - 1;
            v6 = *reinterpret_cast<const uintptr_t *>(buf + 1);
            buf += kScriptStackValueSize;
            if (v4 == VAR_POINTER)
            {
                AddSaveObject(static_cast<unsigned int>(v6));
            }
            else if (v4 == VAR_STACK)
            {
                AddSaveStack(reinterpret_cast<const VariableStackBuffer *>(v6));
            }
            //LOWORD(size) = v5;
            size = (size & 0xFFFF0000) | ((uint32_t)v5 & 0xFFFF);

        } while (v5);
    }
}

void __cdecl AddSaveEntry(unsigned int type, const VariableStackBuffer *u)
{
    if (type == VAR_POINTER)
    {
        AddSaveObject((unsigned int)(intptr_t)u);
    }
    else if (type == VAR_STACK)
    {
        AddSaveStack(u);
    }
}

void __cdecl Scr_SavePre(int sys)
{
    int v2; // r30
    unsigned __int16 *p_entArrayId; // r29
    VariableValueInternal *v4; // r11
    const VariableStackBuffer *stackValue; // r3
    int v6; // r11

    if (!scrVarPub.timeArrayId)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 871, 0, "%s", "scrVarPub.timeArrayId");
    if (!CheckReferences())
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 874, 0, "%s", "CheckReferences()");
    if (sys != 1)
        MyAssertHandler("c:\\trees\\cod3\\cod3src\\src\\script\\scr_readwrite.cpp", 876, 0, "%s", "sys == SCR_SYS_GAME");
    Scr_RemoveDebuggerRefs();
    Com_Memset(scrVarPub.saveIdMap, 0, 0x10000);
    Com_Memset(scrVarPub.saveIdMapRev, 0, 0x10000);
    scrVarPub.savecount = 0;
    AddSaveObject(scrVarPub.levelId);
    AddSaveObject(scrVarPub.animId);
    AddSaveObject(scrVarPub.timeArrayId);
    AddSaveObject(scrVarPub.pauseArrayId);
    AddSaveObject(scrVarPub.freeEntList);
    v2 = 4;
    p_entArrayId = &g_classMap[0].entArrayId;
    // Native class stride: rooting only class 0 leaves the pathnode/hudelem/
    // vehiclenode entity objects unreachable, so the save's cyclic-leak check
    // treats every one of their entities as leaked.
    for (v2 = 0; v2 < 4; ++v2)
        AddSaveObject(g_classMap[v2].entArrayId);
    v4 = &scrVarGlob.variableList[scrVarPub.gameId + VARIABLELIST_CHILD_BEGIN];
    stackValue = v4->u.u.stackValue;
    v6 = v4->w.type & VAR_MASK;
    if (v6 == VAR_POINTER)
    {
        AddSaveObject((unsigned int)(intptr_t)stackValue);
    }
    else if (v6 == VAR_STACK)
    {
        AddSaveStack(stackValue);
    }
}
