#pragma once
#include "switch_skin_motion_normals_prelude.h"
#define InterlockedCompareExchange(pointer, value, comparand) \
    __sync_val_compare_and_swap((pointer), (comparand), (value))
#define InterlockedExchangeAdd(pointer, value) __sync_fetch_and_add((pointer), (value))
