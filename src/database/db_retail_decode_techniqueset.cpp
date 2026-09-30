#include "db_retail_decode_techniqueset.h"

#include "db_retail_wire.h"
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_dvars.h>

#include <cstring>
#include <cstdlib>
#include <memory>

namespace
{
constexpr uint32_t kInlineRef = 0xffffffffu;

// Ported verbatim (logic, not allocation) from switch_sp_bootstrap.cpp's
// DecodeFastfileToken/ReadBlockString: resolve a wire reference against the
// FS reader's own per-block buffers, matching db_retail_wire.h's own
// documented contract for RetailWireTokenDecodeBlocks ("The canonical
// bootstrap still receives blocks from the FS-owned reader, so it uses this
// adapter without creating a second decoder").
bool DecodeFastfileToken(FsRetailFastfileReader *reader, uint32_t encoded, uint32_t span,
                         uint32_t allowedBlockMask, RetailWireToken *token)
{
    if (!reader || !token)
        return false;
    XBlock blocks[9]{};
    for (uint32_t block = 0; block < 9; ++block)
        blocks[block] = {const_cast<uint8_t *>(FS_RetailFastfileBlockData(reader, block)),
                         FS_RetailFastfileBlockSize(reader, block)};
    return RetailWireTokenDecodeBlocks(blocks, {encoded}, span, allowedBlockMask, token);
}

bool ReadBlockString(FsRetailFastfileReader *reader, uint32_t nameRef, char *buffer,
                     uint32_t bufferSize)
{
    RetailWireToken token{};
    if (!nameRef || bufferSize == 0 ||
        !DecodeFastfileToken(reader, nameRef, 0, 1u << 4, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET)
    {
        Com_Printf(0, "ReadBlockString(techset): decode failed nameRef=0x%08x\n", nameRef);
        return false;
    }
    if (token.offset >= FS_RetailFastfileBlockCursor(reader, token.block))
    {
        Com_Printf(0, "ReadBlockString(techset): offset=%u >= cursor=%u block=%u\n",
                   token.offset, FS_RetailFastfileBlockCursor(reader, token.block), token.block);
        return false;
    }
    // Exact-span read: request only the bytes actually mirrored
    // (offset..cursor), never a full buffer -- a name near the end of a
    // block made the old full-buffer read fail even though the string
    // was fully present.
    const uint32_t available =
        FS_RetailFastfileBlockCursor(reader, token.block) - token.offset;
    const uint32_t readBytes = available < bufferSize - 1 ? available : bufferSize - 1;
    if (FS_ReadRetailFastfileBlock(reader, token.block, token.offset,
                                   reinterpret_cast<uint8_t *>(buffer),
                                   readBytes) != FS_RETAIL_FF_WIRE_OK)
    {
        Com_Printf(0, "ReadBlockString(techset): block read failed offset=%u block=%u\n",
                   token.offset, token.block);
        return false;
    }
    // The name must end inside the bytes read: a name longer than the buffer
    // would otherwise be truncated into another asset's identity, and one
    // running into the unwritten mirror past the cursor would leave the rest
    // of the buffer uninitialized. Both fail loudly.
    if (!std::memchr(buffer, 0, readBytes))
    {
        Com_Printf(0, "ReadBlockString(techset): name at offset=%u is unterminated or longer than %u bytes\n",
                   token.offset, bufferSize - 1);
        return false;
    }
    buffer[bufferSize - 1] = '\0';
    if (!buffer[0])
    {
        Com_Printf(0, "ReadBlockString(techset): empty string offset=%u block=%u\n", token.offset,
                   token.block);
        return false;
    }
    // A resolved name must be printable ASCII: a block-4 back-reference
    // that lands on non-name data would register a garbage-named asset
    // (and let later by-name bindings silently miss), so reject it loudly
    // instead.
    for (const char *p = buffer; *p; ++p)
    {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c >= 0x7f)
            return false;
    }
    return true;
}

// Canonical identity of one inline shader/decl record: the encoding of the
// record's own stream offset. An alias slot carries exactly this encoding
// (see ReadRetailFastfileInlineName), so identity comparison is exact.
bool EncodeRecordIdentity(uint32_t recordOffset, uint32_t *encoded)
{
    RetailPtr32 token{};
    if (!recordOffset || !encoded || !RetailWireTokenEncode(4, recordOffset, &token))
        return false;
    *encoded = token.encoded;
    return true;
}

// Pass scalars only; shader, vertex-declaration, argument, and
// technique-name pointers stay null, matching the bootstrap proof exactly
// -- shader/vertex-declaration upload belongs to the renderer resource seam
// (G1), not this decode step.
MaterialTechnique *WidenInlineTechnique(RetailZoneLoadSession *session,
                                         FsRetailFastfileReader *reader, uint32_t techniqueRef,
                                         uint32_t *outWireToken = nullptr,
                                         RetailWireTechniqueCache *cache = nullptr)
{
    if (techniqueRef != kInlineRef)
    {
        Com_Printf(0, "WidenInlineTechnique: techniqueRef 0x%08x != kInlineRef\n", techniqueRef);
        return nullptr;
    }
    FsRetailFastfileMaterialTechnique wire;
    FsRetailFastfileWireResult wireRes = FS_ReadRetailFastfileMaterialTechniquePrefix(reader, techniqueRef, &wire);
    if (wireRes != FS_RETAIL_FF_WIRE_OK)
    {
        return nullptr;
    }
    // Exact stream identity: the FS prefix reader consumes the 8-byte
    // technique header and records passOffset immediately after it, so the
    // record starts exactly 8 bytes earlier by construction (the Android
    // oracle confirms header and passes are contiguous). Alias slots carry
    // the canonical encoding of this same offset -- never a proximity
    // guess -- so encode it canonically or fail.
    if (outWireToken)
    {
        if (wire.passOffset < 8u || !EncodeRecordIdentity(wire.passOffset - 8u, outWireToken))
        {
            Com_Printf(0, "WidenInlineTechnique: cannot encode identity for passOffset=%u\n",
                       wire.passOffset);
            return nullptr;
        }
    }
    if (!wire.passCount || wire.passCount > 64)
    {
        return nullptr;
    }

    FsRetailFastfileMaterialPass passes[64];
    FsRetailFastfileMaterialShaderArgument args[512];
    wireRes = FS_ReadRetailFastfileMaterialTechnique(reader, &wire, passes, 64, args, 512);
    if (wireRes != FS_RETAIL_FF_WIRE_OK)
    {
        Com_Printf(0, "WidenInlineTechnique: Read failed res=%d\n", wireRes);
        return nullptr;
    }

    const std::size_t bytes = sizeof(MaterialTechnique) + (wire.passCount - 1) * sizeof(MaterialPass);
    MaterialTechnique *technique = static_cast<MaterialTechnique *>(
        RetailZoneLoadSessionAlloc(session, bytes, alignof(MaterialTechnique)));
    if (!technique)
        return nullptr;
    std::memset(technique, 0, bytes);
    technique->flags = wire.flags;
    technique->passCount = wire.passCount;

    char techName[64]{};
    if (wire.nameRef)
    {
        // A streamed technique usually carries its trailing inline name,
        // but the retail linker pools duplicate technique names: the first
        // occurrence streams inline while repeats carry an absolute
        // back-reference into the linker's own block-4 image, whose layout
        // our mirror does not reproduce byte-for-byte (proven live: inline
        // techniques whose names resolve, side by side with pooled refs
        // that land on unwritten mirror memory). A pooled name is therefore
        // widened as null and COUNTED, never guessed -- technique selection
        // runs on slot indices, while the upload-time name-prefix dispatch
        // (rb_uploadshaders) is gated by the preflight, which fails
        // loudly on a null name reaching a drawn technique.
        if (!ReadBlockString(reader, wire.nameRef, techName, sizeof(techName)))
            techName[0] = '\0';
        if (techName[0])
        {
            const std::size_t tlen = std::strlen(techName) + 1;
            char *tCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, tlen, 1));
            if (!tCopy)
                return nullptr;
            std::memcpy(tCopy, techName, tlen);
            technique->name = tCopy;
        }
    }

    uint32_t currentArgIndex = 0;
    for (uint16_t pass = 0; pass < wire.passCount; ++pass)
    {
        const FsRetailFastfileMaterialPass &wirePass = passes[pass];
        MaterialPass &destPass = technique->passArray[pass];

        destPass.perPrimArgCount = wirePass.perPrimArgCount;
        destPass.perObjArgCount = wirePass.perObjArgCount;
        destPass.stableArgCount = wirePass.stableArgCount;
        destPass.customSamplerFlags = wirePass.customSamplerFlags;

        // 1. MaterialShaderArgument array
        const uint32_t totalArgs = wirePass.argsRef ? wirePass.argCount : 0;
        if (totalArgs > 0)
        {
            MaterialShaderArgument *passArgs = static_cast<MaterialShaderArgument *>(
                RetailZoneLoadSessionAlloc(session, sizeof(MaterialShaderArgument) * totalArgs, alignof(MaterialShaderArgument)));
            destPass.args = passArgs;
            if (passArgs)
            {
                std::memset(passArgs, 0, sizeof(MaterialShaderArgument) * totalArgs);
                for (uint32_t a = 0; a < totalArgs; ++a)
                {
                    const FsRetailFastfileMaterialShaderArgument &wireArg = args[currentArgIndex++];
                    passArgs[a].type = wireArg.type;
                    passArgs[a].dest = wireArg.dest;
                    if (wireArg.type == 1 || wireArg.type == 7)
                    {
                        // literalOffset arrives straight off the wire and was
                        // used as a raw index into block 4 with no bounds check,
                        // unlike every other wire offset this file resolves
                        // (ReadBlockString validates against the block cursor
                        // before reading).  An unexpected offset therefore read
                        // 16 bytes from arbitrary guest memory.  Bound it by the
                        // block size and let an out-of-range literal fall
                        // through to the s_zero default the code below already
                        // supplies for unreadable literals.  Subtraction rather
                        // than "literalOffset + 16" so the check cannot itself
                        // overflow.  NOTE: this is a memory-safety fix found
                        // while chasing the code_post_gfx walk spin under Eden;
                        // it did NOT stop that spin, so it is not that bug.
                        const uint32_t literalBlockSize = FS_RetailFastfileBlockSize(reader, 4);
                        const bool literalInBounds =
                            wireArg.literalOffset <= literalBlockSize &&
                            (literalBlockSize - wireArg.literalOffset) >= 16u;
                        if (wireArg.literalOffset && literalInBounds && FS_RetailFastfileBlockData(reader, 4))
                        {
                            float *lit = static_cast<float *>(
                                RetailZoneLoadSessionAlloc(session, 16, alignof(float)));
                            if (lit)
                            {
                                std::memcpy(lit, FS_RetailFastfileBlockData(reader, 4) + wireArg.literalOffset, 16);
                                passArgs[a].u.literalConst = lit;
                            }
                        }
                        if (!passArgs[a].u.literalConst)
                        {
                            static const float s_zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                            passArgs[a].u.literalConst = s_zero;
                        }
                    }
                    else if (wireArg.type == 3 || wireArg.type == 5)
                    {
                        std::memcpy(&passArgs[a].u.codeConst, &wireArg.valueRef, sizeof(passArgs[a].u.codeConst));
                    }
                    else if (wireArg.type == 4)
                    {
                        passArgs[a].u.codeSampler = static_cast<MaterialTextureSource>(wireArg.valueRef);
                    }
                    else
                    {
                        passArgs[a].u.nameHash = wireArg.valueRef;
                    }
                }
            }
            else
            {
                destPass.perPrimArgCount = 0;
                destPass.perObjArgCount = 0;
                destPass.stableArgCount = 0;
            }
        }
        else
        {
            destPass.perPrimArgCount = 0;
            destPass.perObjArgCount = 0;
            destPass.stableArgCount = 0;
            destPass.args = nullptr;
        }

        // 2. Vertex Declaration: a null slot means no declaration; an
        // alias resolves by exact record identity or fails loudly; an
        // inline record widens or fails. A silent null for a non-null slot
        // would leave the renderer binding garbage.
        MaterialVertexDeclaration *decl = nullptr;
        if (wirePass.vertexDeclRef && wirePass.vertexDeclRef != kInlineRef)
        {
            RetailWireToken token{};
            if (!DecodeFastfileToken(reader, wirePass.vertexDeclRef, 0, 1u << 4, &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET)
            {
                Com_Printf(0, "WidenInlineTechnique: bad vertexDecl alias 0x%08x\n",
                           wirePass.vertexDeclRef);
                return nullptr;
            }
            decl = cache ? cache->FindVertexDecl(token.encoded) : nullptr;
            if (!decl)
            {
                // Forward shared declaration: defer like technique
                // aliases; the post-pass patches the widened pass.
                if (!cache || cache->deferredSubCount >= 1024)
                {
                    Com_Printf(0, "WidenInlineTechnique: vertexDecl alias 0x%08x not deferrable\n",
                               wirePass.vertexDeclRef);
                    return nullptr;
                }
                RetailWireTechniqueCache::DeferredSubAlias &sub =
                    cache->deferredSubs[cache->deferredSubCount++];
                sub.pass = &destPass;
                sub.kind = 0;
                sub.reference = wirePass.vertexDeclRef;
            }
        }
        else if (wirePass.vertexDeclRef == kInlineRef)
        {
            const uint32_t declSize = 100;
            const uint32_t block4Size = FS_RetailFastfileBlockSize(reader, 4);
            if (!wirePass.vertexDeclOffset || !FS_RetailFastfileBlockData(reader, 4) ||
                wirePass.vertexDeclOffset > block4Size ||
                block4Size - wirePass.vertexDeclOffset < declSize)
            {
                Com_Printf(0, "WidenInlineTechnique: vertexDecl body out of range off=%u size=%u\n",
                           wirePass.vertexDeclOffset, block4Size);
                return nullptr;
            }
            decl = static_cast<MaterialVertexDeclaration *>(
                RetailZoneLoadSessionAlloc(session, sizeof(MaterialVertexDeclaration), alignof(MaterialVertexDeclaration)));
            if (!decl)
                return nullptr;
            std::memset(decl, 0, sizeof(*decl));
            const uint8_t *wireDecl = FS_RetailFastfileBlockData(reader, 4) + wirePass.vertexDeclOffset;
            decl->streamCount = wireDecl[0];
            decl->hasOptionalSource = (wireDecl[1] != 0);
            decl->isLoaded = false;
            std::memcpy(decl->routing.data, wireDecl + 4, sizeof(decl->routing.data));
            Load_BuildVertexDecl(&decl);
            if (cache)
            {
                uint32_t declToken = 0;
                if (!EncodeRecordIdentity(wirePass.vertexDeclOffset, &declToken) ||
                    !cache->InsertVertexDecl(declToken, decl))
                {
                    Com_Printf(0, "WidenInlineTechnique: cannot index vertexDecl off=%u\n",
                               wirePass.vertexDeclOffset);
                    return nullptr;
                }
            }
        }
        destPass.vertexDecl = decl;

        // 3. Vertex Shader: null slot means no shader; an alias resolves
        // by exact record identity or fails; an inline record widens
        // (program bytes included when the slot carries them) or fails. A
        // name slot that is present but unreadable fails: unlike the
        // technique-set name there is no memo to consult, so silence would
        // bind the wrong program.
        MaterialVertexShader *vs = nullptr;
        if (wirePass.vertexShader.headerRef && wirePass.vertexShader.headerRef != kInlineRef)
        {
            RetailWireToken token{};
            if (!DecodeFastfileToken(reader, wirePass.vertexShader.headerRef, 0, 1u << 4,
                                     &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET)
            {
                Com_Printf(0, "WidenInlineTechnique: bad vertexShader alias 0x%08x\n",
                           wirePass.vertexShader.headerRef);
                return nullptr;
            }
            vs = cache ? cache->FindVertexShader(token.encoded) : nullptr;
            if (!vs)
            {
                if (!cache || cache->deferredSubCount >= 1024)
                {
                    Com_Printf(0, "WidenInlineTechnique: vertexShader alias 0x%08x not deferrable\n",
                               wirePass.vertexShader.headerRef);
                    return nullptr;
                }
                RetailWireTechniqueCache::DeferredSubAlias &sub =
                    cache->deferredSubs[cache->deferredSubCount++];
                sub.pass = &destPass;
                sub.kind = 1;
                sub.reference = wirePass.vertexShader.headerRef;
            }
        }
        else if (wirePass.vertexShader.headerRef == kInlineRef)
        {
            const uint32_t block4Size = FS_RetailFastfileBlockSize(reader, 4);
            if (!wirePass.vertexShader.recordOffset || !FS_RetailFastfileBlockData(reader, 4))
            {
                Com_Printf(0, "WidenInlineTechnique: inline vertexShader has no record\n");
                return nullptr;
            }
            vs = static_cast<MaterialVertexShader *>(
                RetailZoneLoadSessionAlloc(session, sizeof(MaterialVertexShader), alignof(MaterialVertexShader)));
            if (!vs)
                return nullptr;
            std::memset(vs, 0, sizeof(*vs));
            if (wirePass.vertexShader.nameRef)
            {
                char sname[64]{};
                // Pooled shader names use the same linker pooling as
                // technique names: widen null and count, never guess (see
                // the technique-name contract above; upload consumes
                // bytecode, never names).
                if (ReadBlockString(reader, wirePass.vertexShader.nameRef, sname, sizeof(sname)))
                {
                    const std::size_t slen = std::strlen(sname) + 1;
                    char *scopy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, slen, 1));
                    if (!scopy)
                        return nullptr;
                    std::memcpy(scopy, sname, slen);
                    vs->name = scopy;
                }
                else if (cache)
                {
                    ++cache->nullShaderNames;
                }
            }

            const uint32_t vsBytes = static_cast<uint32_t>(wirePass.vertexShader.programSize) * 4u;
            if (wirePass.vertexShader.programRef && (!wirePass.vertexShader.programOffset || !vsBytes))
            {
                Com_Printf(0, "WidenInlineTechnique: inline vertexShader promises no program\n");
                return nullptr;
            }
            if (vsBytes)
            {
                if (wirePass.vertexShader.programOffset > block4Size ||
                    block4Size - wirePass.vertexShader.programOffset < vsBytes)
                {
                    Com_Printf(0, "WidenInlineTechnique: vertexShader program out of range\n");
                    return nullptr;
                }
                uint8_t *vsProg = static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(session, vsBytes, 4));
                if (!vsProg)
                    return nullptr;
                std::memcpy(vsProg, FS_RetailFastfileBlockData(reader, 4) + wirePass.vertexShader.programOffset, vsBytes);
                vs->prog.loadDef.program = vsProg;
            }
            vs->prog.loadDef.programSize = wirePass.vertexShader.programSize;
            vs->prog.loadDef.loadForRenderer = wirePass.vertexShader.loadForRenderer;
            Load_CreateMaterialVertexShader(&vs->prog.loadDef, vs);
            if (cache)
            {
                uint32_t vsToken = 0;
                if (!EncodeRecordIdentity(wirePass.vertexShader.recordOffset, &vsToken) ||
                    !cache->InsertVertexShader(vsToken, vs))
                {
                    Com_Printf(0, "WidenInlineTechnique: cannot index vertexShader\n");
                    return nullptr;
                }
            }
        }
        destPass.vertexShader = vs;

        // 4. Pixel Shader: same exact contract as the vertex shader.
        MaterialPixelShader *ps = nullptr;
        if (wirePass.pixelShader.headerRef && wirePass.pixelShader.headerRef != kInlineRef)
        {
            RetailWireToken token{};
            if (!DecodeFastfileToken(reader, wirePass.pixelShader.headerRef, 0, 1u << 4,
                                     &token) ||
                token.kind != RETAIL_WIRE_TOKEN_OFFSET)
            {
                Com_Printf(0, "WidenInlineTechnique: bad pixelShader alias 0x%08x\n",
                           wirePass.pixelShader.headerRef);
                return nullptr;
            }
            ps = cache ? cache->FindPixelShader(token.encoded) : nullptr;
            if (!ps)
            {
                if (!cache || cache->deferredSubCount >= 1024)
                {
                    Com_Printf(0, "WidenInlineTechnique: pixelShader alias 0x%08x not deferrable\n",
                               wirePass.pixelShader.headerRef);
                    return nullptr;
                }
                RetailWireTechniqueCache::DeferredSubAlias &sub =
                    cache->deferredSubs[cache->deferredSubCount++];
                sub.pass = &destPass;
                sub.kind = 2;
                sub.reference = wirePass.pixelShader.headerRef;
            }
        }
        else if (wirePass.pixelShader.headerRef == kInlineRef)
        {
            const uint32_t block4Size = FS_RetailFastfileBlockSize(reader, 4);
            if (!wirePass.pixelShader.recordOffset || !FS_RetailFastfileBlockData(reader, 4))
            {
                Com_Printf(0, "WidenInlineTechnique: inline pixelShader has no record\n");
                return nullptr;
            }
            ps = static_cast<MaterialPixelShader *>(
                RetailZoneLoadSessionAlloc(session, sizeof(MaterialPixelShader), alignof(MaterialPixelShader)));
            if (!ps)
                return nullptr;
            std::memset(ps, 0, sizeof(*ps));
            if (wirePass.pixelShader.nameRef)
            {
                char sname[64]{};
                if (ReadBlockString(reader, wirePass.pixelShader.nameRef, sname, sizeof(sname)))
                {
                    const std::size_t slen = std::strlen(sname) + 1;
                    char *scopy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, slen, 1));
                    if (!scopy)
                        return nullptr;
                    std::memcpy(scopy, sname, slen);
                    ps->name = scopy;
                }
                else if (cache)
                {
                    ++cache->nullShaderNames;
                }
            }

            const uint32_t psBytes = static_cast<uint32_t>(wirePass.pixelShader.programSize) * 4u;
            if (wirePass.pixelShader.programRef && (!wirePass.pixelShader.programOffset || !psBytes))
            {
                Com_Printf(0, "WidenInlineTechnique: inline pixelShader promises no program\n");
                return nullptr;
            }
            if (psBytes)
            {
                if (wirePass.pixelShader.programOffset > block4Size ||
                    block4Size - wirePass.pixelShader.programOffset < psBytes)
                {
                    Com_Printf(0, "WidenInlineTechnique: pixelShader program out of range\n");
                    return nullptr;
                }
                uint8_t *psProg = static_cast<uint8_t *>(RetailZoneLoadSessionAlloc(session, psBytes, 4));
                if (!psProg)
                    return nullptr;
                std::memcpy(psProg, FS_RetailFastfileBlockData(reader, 4) + wirePass.pixelShader.programOffset, psBytes);
                ps->prog.loadDef.program = psProg;
            }
            ps->prog.loadDef.programSize = wirePass.pixelShader.programSize;
            ps->prog.loadDef.loadForRenderer = wirePass.pixelShader.loadForRenderer;
            Load_CreateMaterialPixelShader(&ps->prog.loadDef, ps);
            if (cache)
            {
                uint32_t psToken = 0;
                if (!EncodeRecordIdentity(wirePass.pixelShader.recordOffset, &psToken) ||
                    !cache->InsertPixelShader(psToken, ps))
                {
                    Com_Printf(0, "WidenInlineTechnique: cannot index pixelShader\n");
                    return nullptr;
                }
            }
        }
        destPass.pixelShader = ps;
    }
    return technique;
}
} // namespace

bool RetailWireTechniqueCacheResolveDeferred(RetailWireTechniqueCache *cache,
                                             FsRetailFastfileReader *reader)
{
    if (!cache)
        return false;
    // Fixpoint for chains (A defers to B's slot, B itself deferred):
    // each pass binds whatever is ready; no progress with leftovers
    // means genuinely dangling references.
    for (uint32_t pass = 0; pass <= cache->deferredCount; ++pass)
    {
        bool progress = false;
        for (uint32_t i = 0; i < cache->deferredCount; ++i)
        {
            RetailWireTechniqueCache::DeferredAlias &entry = cache->deferred[i];
            if (!entry.techniqueSet || entry.techniqueSet->techniques[entry.slot] != nullptr)
                continue;
            MaterialTechnique *target = cache->Find(entry.reference);
            if (!target)
                continue;
            entry.techniqueSet->techniques[entry.slot] = target;
            ++cache->deferredResolvedCount;
            progress = true;
        }
        if (!progress)
            break;
    }
    // Canonical empty technique bound below for the linker-fill case:
    // vestigial slots (EMISSIVE/EMISSIVE_SHADOW/DEBUG_BUMPMAP and similar)
    // whose reference names block-4 bytes the zone's own stream never wrote.
    // The proof is the reader's block-4 write high-water mark: block memory is
    // zero-initialized at open and every streamed byte is written at the
    // reader's cursor, so an offset at or beyond FS_RetailFastfileBlockCursor
    // (reader, 4) is provably unwritten filler -- no loader ever placed real
    // content there. Anything below that cursor is inside streamed zone data:
    // it is either a real record the loader failed to widen or unrelated
    // bytes, and binding it empty would hide missing rendering, so it fails
    // loudly instead (ts/dangle's in-bounds offset 4 is exactly that case).
    // passCount 0 means the pass loop below draws nothing for it.
    static MaterialTechnique s_emptyTechnique = []() {
        MaterialTechnique t{};
        t.name = "$empty";
        t.flags = 0;
        t.passCount = 0;
        return t;
    }();
    bool missing = false;
    for (uint32_t i = 0; i < cache->deferredCount; ++i)
    {
        const RetailWireTechniqueCache::DeferredAlias &entry = cache->deferred[i];
        if (entry.techniqueSet && entry.techniqueSet->techniques[entry.slot] == nullptr)
        {
            RetailWireToken token{};
            if (reader && DecodeFastfileToken(reader, entry.reference, 0, 1u << 4, &token) &&
                token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4 &&
                token.offset >= FS_RetailFastfileBlockCursor(reader, 4))
            {
                entry.techniqueSet->techniques[entry.slot] = &s_emptyTechnique;
                ++cache->deferredResolvedCount;
                Com_Printf(0, "RetailWireTechniqueCacheResolveDeferred: set '%s' slot %u ref 0x%08x "
                              "unwritten block4 offset=%u >= cursor=%u -- binding canonical empty technique\n",
                           entry.setName, entry.slot, entry.reference, token.offset,
                           FS_RetailFastfileBlockCursor(reader, 4));
                continue;
            }
            Com_Printf(0, "RetailWireTechniqueCacheResolveDeferred: set '%s' slot %u ref 0x%08x still missing\n",
                       entry.setName, entry.slot, entry.reference);
            missing = true;
        }
    }
    // Same vestigial linker-fill class as the technique slots above, one
    // level down: a pass inside an otherwise-real, already-widened
    // technique whose vertexDecl/vertexShader/pixelShader sub-slot aliases
    // dead block-4 filler no loader ever places real content into. Bind an
    // inert stub (zero streams / null D3D program, i.e. draws-nothing /
    // fixed-function-nothing if ever actually bound) instead of leaving the
    // pass with a dangling null -- under the same unwritten-past-the-cursor
    // proof as the technique-slot case above, so a sub-slot that points into
    // streamed zone data still fails loudly.
    static MaterialVertexDeclaration s_emptyVertexDecl{};
    static MaterialVertexShader s_emptyVertexShader = []() {
        MaterialVertexShader vs{};
        vs.name = "$empty";
        return vs;
    }();
    static MaterialPixelShader s_emptyPixelShader = []() {
        MaterialPixelShader ps{};
        ps.name = "$empty";
        return ps;
    }();
    for (uint32_t i = 0; i < cache->deferredSubCount; ++i)
    {
        RetailWireTechniqueCache::DeferredSubAlias &entry = cache->deferredSubs[i];
        if (!entry.pass)
            continue;
        bool bound = false;
        if (entry.kind == 0)
        {
            MaterialVertexDeclaration *decl = cache->FindVertexDecl(entry.reference);
            if (decl)
            {
                entry.pass->vertexDecl = decl;
                ++cache->deferredSubResolvedCount;
                bound = true;
            }
        }
        else if (entry.kind == 1)
        {
            MaterialVertexShader *vs = cache->FindVertexShader(entry.reference);
            if (vs)
            {
                entry.pass->vertexShader = vs;
                ++cache->deferredSubResolvedCount;
                bound = true;
            }
        }
        else if (entry.kind == 2)
        {
            MaterialPixelShader *ps = cache->FindPixelShader(entry.reference);
            if (ps)
            {
                entry.pass->pixelShader = ps;
                ++cache->deferredSubResolvedCount;
                bound = true;
            }
        }
        if (!bound)
        {
            RetailWireToken token{};
            if (reader && DecodeFastfileToken(reader, entry.reference, 0, 1u << 4, &token) &&
                token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4 &&
                token.offset >= FS_RetailFastfileBlockCursor(reader, 4))
            {
                if (entry.kind == 0)
                    entry.pass->vertexDecl = &s_emptyVertexDecl;
                else if (entry.kind == 1)
                    entry.pass->vertexShader = &s_emptyVertexShader;
                else
                    entry.pass->pixelShader = &s_emptyPixelShader;
                ++cache->deferredSubResolvedCount;
                Com_Printf(0, "RetailWireTechniqueCacheResolveDeferred: sub kind %u ref 0x%08x "
                              "unwritten block4 offset=%u >= cursor=%u -- binding canonical empty stub\n",
                           entry.kind, entry.reference, token.offset,
                           FS_RetailFastfileBlockCursor(reader, 4));
                continue;
            }
            Com_Printf(0, "RetailWireTechniqueCacheResolveDeferred: sub kind %u ref 0x%08x still missing\n",
                       entry.kind, entry.reference);
            missing = true;
        }
    }
    return !missing;
}

bool RetailWalkLiveLoadTechniqueSet(RetailZoneLoadSession *session, FsRetailFastfileReader *reader,
                                    uint32_t headerRef, XAssetHeader *header,
                                    RetailWireTechniqueCache *cache)
{
    if (!session || !session->active || !reader || !header)
        return false;
    *header = {};

    // RetailWireTechniqueCache is ~385KB (five fixed-capacity tables sized
    // for killhouse's own need -- see the header). An unconditional
    // stack local of that size blew the stack of every caller on entry,
    // including DB_Thread's constrained background-loading stack, before
    // this function's own `if (!cache)` check ever ran -- a real crash,
    // not a hypothetical: production always passes a real cache (this
    // fallback exists only for callers -- tests, alternate decoders -- that
    // don't), so heap-allocate it lazily instead, only when actually
    // reached.
    std::unique_ptr<RetailWireTechniqueCache> fallbackCache;
    if (!cache)
    {
        fallbackCache = std::make_unique<RetailWireTechniqueCache>();
        cache = fallbackCache.get();
    }

    FsRetailFastfileTechniqueSet wireSet;
    FsRetailFastfileWireResult prefixRes = FS_ReadRetailFastfileTechniqueSetPrefix(reader, headerRef, &wireSet);
    if (prefixRes != FS_RETAIL_FF_WIRE_OK)
    {
        Com_Printf(0, "RetailWalkLiveLoadTechniqueSet: FS_ReadRetailFastfileTechniqueSetPrefix failed res=%d\n", prefixRes);
        return false;
    }
    char name[64]{};
    if (wireSet.nameWasInline)
    {
        if (!ReadBlockString(reader, wireSet.nameRef, name, sizeof(name)))
        {
            Com_Printf(0, "RetailWalkLiveLoadTechniqueSet: unreadable inline name nameRef=0x%08x\n",
                       wireSet.nameRef);
            return false;
        }
    }
    else if (wireSet.nameRef)
    {
        // Shared name: the slot is an encoded pointer to an already-decoded
        // name (DB_ConvertOffsetToPointer), resolved through the canonical
        // block-4 read or the exact token memo -- never guessed.
        if (cache)
        {
            const char *known = cache->FindString(wireSet.nameRef);
            if (known)
            {
                std::strncpy(name, known, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';
            }
        }
        if (!name[0] && !ReadBlockString(reader, wireSet.nameRef, name, sizeof(name)))
        {
            Com_Printf(0, "RetailWalkLiveLoadTechniqueSet: unresolvable shared name nameRef=0x%08x\n",
                       wireSet.nameRef);
            return false;
        }
        if (cache && name[0])
            cache->RecordString(wireSet.nameRef, name);
    }
    else
    {
        Com_Printf(0, "RetailWalkLiveLoadTechniqueSet: technique set has no name\n");
        return false;
    }

    MaterialTechniqueSet *techniqueSet = static_cast<MaterialTechniqueSet *>(
        RetailZoneLoadSessionAlloc(session, sizeof(MaterialTechniqueSet), alignof(MaterialTechniqueSet)));
    const std::size_t nameBytes = std::strlen(name) + 1;
    char *nameCopy = static_cast<char *>(RetailZoneLoadSessionAlloc(session, nameBytes, 1));
    if (!techniqueSet || !nameCopy)
        return false;
    std::memset(techniqueSet, 0, sizeof(*techniqueSet));
    std::memcpy(nameCopy, name, nameBytes);
    techniqueSet->name = nameCopy;
    techniqueSet->worldVertFormat = wireSet.worldVertFormat;
    techniqueSet->remappedTechniqueSet = techniqueSet;

    for (uint32_t slot = 0; slot < 34; ++slot)
    {
        if (!wireSet.techniqueRefs[slot])
            continue;
        if (wireSet.techniqueRefs[slot] == kInlineRef)
        {
            uint32_t wireToken = 0;
            techniqueSet->techniques[slot] =
                WidenInlineTechnique(session, reader, wireSet.techniqueRefs[slot], &wireToken, cache);
            if (!techniqueSet->techniques[slot])
            {
                Com_Printf(0, "RetailWalkLiveLoadTechniqueSet: slot %u WidenInlineTechnique failed\n", slot);
                return false;
            }
            if (wireToken && !cache->Insert(wireToken, techniqueSet->techniques[slot]))
            {
                Com_Printf(0, "RetailWalkLiveLoadTechniqueSet: slot %u cannot index identity\n", slot);
                return false;
            }
            if (!techniqueSet->techniques[slot]->name)
                ++cache->nullTechniqueNames;
        }
        else
        {
            // Alias slot: the reference names an already-decoded technique
            // by exact stream identity (the ord-1222 retail shape: one
            // inline technique with later slots aliasing it). Anything the
            // cache cannot resolve exactly fails loudly -- substituting the
            // primary slot, an earlier slot, or the most recent entry would
            // silently bind the wrong program.
            MaterialTechnique *aliased = cache->Find(wireSet.techniqueRefs[slot]);
            if (!aliased)
            {
                // Forward (or not-yet-widened) target: the real loader
                // resolves offsets order-independently, so defer to the
                // post-directory pass instead of failing. Anything still
                // missing then is genuinely dangling and fails loudly
                // there. The slot stays null until then; nothing consumes
                // the set before the post-pass runs.
                if (cache->deferredCount >= 1024)
                {
                    Com_Printf(0, "RetailWalkLiveLoadTechniqueSet: deferred table full at slot %u\n",
                               slot);
                    return false;
                }
                RetailWireTechniqueCache::DeferredAlias &deferred =
                    cache->deferred[cache->deferredCount++];
                deferred.techniqueSet = techniqueSet;
                deferred.slot = slot;
                deferred.reference = wireSet.techniqueRefs[slot];
                std::strncpy(deferred.setName, name, sizeof(deferred.setName) - 1);
                deferred.setName[sizeof(deferred.setName) - 1] = '\0';
                techniqueSet->techniques[slot] = nullptr;
                continue;
            }
            techniqueSet->techniques[slot] = aliased;
        }
    }

    const XAssetHeader registered =
        RetailZoneLoadSessionRegister(session, ASSET_TYPE_TECHNIQUE_SET, {techniqueSet});
    if (!registered.techniqueSet)
    {
        Com_Printf(0, "RetailWalkLiveLoadTechniqueSet: Register failed\n");
        return false;
    }
    *header = registered;
    return true;
}
