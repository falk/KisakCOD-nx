#pragma once

// Load-time shader variant prebake inputs (deko3d renderer): for each pass of
// a loaded material, the state a draw of it can bind that selects a compiled
// shader variant -- which pixel samplers can hold a hardware shadow map
// (depth compare), whether the material alpha-tests the pass (early-Z
// eligibility), and the instanced static-model register layout. Shared by
// the zone-load prebake (switch_shader_prebake.cpp) and the host census of
// retail zones, so both enumerate exactly the same passes.

#include <gfx_d3d/r_material.h>

#include <cstdint>

// GfxStateBits loadBits[0] bit that disables the alpha test (r_state.h
// GFXS0_ATEST_DISABLE; checked against it where both are visible).
constexpr uint32_t kPrebakeAtestDisable = 0x800;

// The vertex shader constant registers that vary between the models of an
// instanced rigid static-model draw: the pass's per-prim arguments, which
// must all be code vertex constants. False when the pass cannot be drawn
// instanced (no per-prim argument, another argument type, over 16 rows).
inline bool R_StaticModelInstanceRegs(const MaterialPass *pass, uint8_t *regs, uint32_t *regCount)
{
    constexpr uint32_t kMaxRegs = 16;
    uint32_t count = 0;
    for (uint32_t argIndex = 0; argIndex < pass->perPrimArgCount; ++argIndex)
    {
        const MaterialShaderArgument &arg = pass->args[argIndex];
        if (arg.type != MTL_ARG_CODE_VERTEX_CONST)
            return false;
        for (uint32_t row = 0; row < arg.u.codeConst.rowCount; ++row)
        {
            const uint32_t reg = arg.dest + row;
            if (count >= kMaxRegs || reg >= 256)
                return false;
            regs[count++] = (uint8_t)reg;
        }
    }
    *regCount = count;
    return count != 0;
}

// Pixel sampler registers the pass binds to the sun or spot shadow map: the
// only code images that are depth-format textures, and only with hardware
// shadow maps.
inline uint32_t R_PassShadowmapSamplers(const MaterialPass &pass)
{
    uint32_t mask = 0;
    const uint32_t argCount = pass.perPrimArgCount + pass.perObjArgCount + pass.stableArgCount;
    for (uint32_t i = 0; pass.args && i < argCount; ++i)
    {
        const MaterialShaderArgument &arg = pass.args[i];
        if (arg.type == MTL_ARG_CODE_PIXEL_SAMPLER && arg.dest < 16 &&
            (arg.u.codeSampler == TEXTURE_SRC_CODE_SHADOWMAP_SUN || arg.u.codeSampler == TEXTURE_SRC_CODE_SHADOWMAP_SPOT))
            mask |= 1u << arg.dest;
    }
    return mask;
}

// Whether the material's state bits for pass `passIndex` of technique
// `techType` enable the alpha test (R_SetupPass reads the same entry).
inline bool R_MaterialPassAlphaTest(const Material *material, int techType, uint32_t passIndex)
{
    if (!material->stateBitsTable)
        return false;
    const uint32_t entry = material->stateBitsEntry[techType];
    if (entry >= material->stateBitsCount || entry + passIndex >= material->stateBitsCount)
        return false;
    return (material->stateBitsTable[entry + passIndex].loadBits[0] & kPrebakeAtestDisable) == 0;
}

struct PrebakePassInputs
{
    const MaterialPass *pass;
    uint32_t depthSamplerMask;
    bool alphaTest;
    uint8_t instanceRegs[16];
    uint32_t instanceRegCount;
};

// Whether rigid static-model runs draw instanced: the draw side and the
// prebake share this one rule (r_portDebugChecks needs one draw per model).
bool R_StaticModelInstancingEnabled();

// Calls emit(const PrebakePassInputs &) for every pass of every technique
// of `techSet` drawn for `material`: the material's own technique set or the
// one the feature remap (r_specular, r_normal, hardware shadow maps, ...)
// selects for it, both drawn with the material's state bits.
// `hardwareShadowmap`: shadow maps are depth textures
// (gfxMetrics.hasHardwareShadowmap); `instancing`: rigid static models may
// draw instanced (R_StaticModelInstancingEnabled).
template <typename Emit>
void R_ForEachPrebakePass(const Material *material, const MaterialTechniqueSet *techSet, bool hardwareShadowmap,
                          bool instancing, Emit &&emit)
{
    if (!material || !techSet)
        return;
    for (int techType = 0; techType < TECHNIQUE_COUNT; ++techType)
    {
        const MaterialTechnique *technique = techSet->techniques[techType];
        if (!technique)
            continue;
        for (uint32_t p = 0; p < technique->passCount; ++p)
        {
            const MaterialPass &pass = technique->passArray[p];
            if (!pass.vertexShader && !pass.pixelShader)
                continue;
            PrebakePassInputs in{};
            in.pass = &pass;
            in.depthSamplerMask = hardwareShadowmap ? R_PassShadowmapSamplers(pass) : 0u;
            in.alphaTest = R_MaterialPassAlphaTest(material, techType, p);
            uint32_t regCount = 0;
            if (instancing && R_StaticModelInstanceRegs(&pass, in.instanceRegs, &regCount))
                in.instanceRegCount = regCount;
            emit(in);
        }
    }
}
