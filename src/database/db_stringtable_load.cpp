#include <universal/q_shared.h>
#include "database.h"

void __cdecl Load_ScriptStringCustom(uint16_t *var)
{
    // 32-bit wire convention: the custom string-list slot carries the value
    // in the low 16 bits of the serialized 32-bit slot.  The W2 wire walker
    // owns the authoritative decode; this keeps the widened load byte-faithful.
    *var = static_cast<uint16_t>(
        reinterpret_cast<uintptr_t>(varXAssetList->stringList.strings[*var]));
}

void __cdecl Mark_ScriptStringCustom(uint16_t *var)
{
    if (*var)
        SL_AddUser(*var, 4u);
}

