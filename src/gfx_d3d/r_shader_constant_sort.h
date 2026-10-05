#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

// Insert registers and their value pointers together; equal registers retain
// insertion order. Invalid input must leave the block unchanged.
template <class Block>
bool R_InsertShaderConstant(Block &block, uint32_t dest, const float *value)
{
    const size_t capacity = sizeof(block.dest) / sizeof(block.dest[0]);
    static_assert(sizeof(block.value) / sizeof(block.value[0]) == capacity);
    if (block.count >= capacity || dest > UINT16_MAX || !value)
        return false;
    size_t index = block.count;
    while (index && block.dest[index - 1] > dest)
    {
        block.dest[index] = block.dest[index - 1];
        block.value[index] = block.value[index - 1];
        --index;
    }
    block.dest[index] = static_cast<uint16_t>(dest);
    block.value[index] = value;
    ++block.count;
    return true;
}

// Numeric values sort first; all NaNs compare equivalent. Signed zeros are
// equivalent too, giving material sorting a strict weak ordering.
inline int R_CompareShaderFloat(float a, float b)
{
    if (std::isnan(a))
        return std::isnan(b) ? 0 : 1;
    if (std::isnan(b))
        return -1;
    return a < b ? -1 : a > b ? 1 : 0;
}
