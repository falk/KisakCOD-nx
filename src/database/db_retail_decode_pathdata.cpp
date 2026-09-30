#include <universal/q_shared.h>

#include <cstdint>
#include <cstring>

#include <database/database.h>
#include <database/db_retail_zone.h>
#include <game/g_bsp.h>
#include <script/scr_stringlist.h>

// Android Load_GameWorldSp / Load_PathData widen, live-decode half.
//
// The walk-only reader (ReadRetailGameWorldSpBody, db_retail_walk.cpp) proves
// the exact wire order and cursors; RetailWalkLiveLoadGameWorldSp replays
// that body through this decoder so the engine's native gameWorldSp singleton
// holds real path data.  Without it every authored pathnode entity looks like
// an extra node (G_UpdateTrackExtraNodes -> Com_PrintError), which trips the
// map-load error summary and leaves the UI on pregame_loaderror instead of
// starting the level.
namespace
{
constexpr uint32_t kInlineReference = 0xffffffffu;
constexpr uint32_t kRootBytes = 44;
constexpr uint32_t kNodeBytes = 128;
constexpr uint32_t kLinkBytes = 12;
constexpr uint32_t kBaseBytes = 16;
constexpr uint32_t kTreeBytes = 16;
constexpr uint32_t kMaxNodeCount = 0x2000;      // PATH_MAX_NODES
constexpr uint32_t kMaxTreeCount = 0x40000;
constexpr uint32_t kBlock4Mask = 1u << 4;

constexpr uint32_t kNodeLinkCountOffset = 62;
constexpr uint32_t kNodeLinksRefOffset = 64;
constexpr uint32_t kNodeDynamicOffset = 68;
constexpr uint32_t kNodeConstantBytes = 64;
constexpr uint32_t kNodeDynamicBytes = 32;

constexpr uint32_t kChainCountOffset = 16;
constexpr uint32_t kChainARefOffset = 20;
constexpr uint32_t kChainBRefOffset = 24;
constexpr uint32_t kVisBytesOffset = 28;
constexpr uint32_t kVisRefOffset = 32;
constexpr uint32_t kTreeCountOffset = 36;
constexpr uint32_t kTreeRefOffset = 40;

uint32_t Le32(const uint8_t *bytes)
{
    return static_cast<uint32_t>(bytes[0]) |
           (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) |
           (static_cast<uint32_t>(bytes[3]) << 24);
}

float LeFloat(const uint8_t *bytes)
{
    const uint32_t bits = Le32(bytes);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void *Alloc(RetailZoneLoadSession *session, std::size_t bytes, std::size_t alignment)
{
    return RetailZoneLoadSessionAlloc(session, bytes, alignment);
}

const uint8_t *Block(const RetailZoneLoadSession *session, uint32_t index)
{
    return session->zoneMemory->blocks[index].data;
}

bool TokenOffset(RetailZoneLoadSession *session, uint32_t reference, uint32_t span,
                 uint32_t block, uint32_t *offset)
{
    XBlock blocks[9]{};
    for (uint32_t i = 0; i < 9; ++i)
        blocks[i] = session->zoneMemory->blocks[i];
    RetailWireToken token{};
    RetailPtr32 encoded{};
    encoded.encoded = reference;
    if (!RetailWireTokenDecodeBlocks(blocks, encoded, span, 1u << block, &token) ||
        token.kind != RETAIL_WIRE_TOKEN_OFFSET || token.block != block)
        return false;
    *offset = token.offset;
    return true;
}

// Replay copy of one already-streamed block-4 span: the wrapper's walk pass
// consumed the reader to the same end, so the session cursor only needs the
// same alignment/advance without touching the file.
const uint8_t *ReserveBlock4(RetailZoneLoadSession *session, uint32_t bytes,
                             uint32_t alignment)
{
    return RetailWireBlocksAlloc(&session->wire, 4, bytes, alignment);
}

const char *ReserveInlineString(RetailZoneLoadSession *session)
{
    const uint32_t start = session->wire.cursor[4];
    const XBlock &block = session->zoneMemory->blocks[4];
    uint32_t end = start;
    while (end < block.size && block.data[end] != 0)
        ++end;
    if (end >= block.size)
        return nullptr;
    const uint32_t bytes = end - start + 1;
    const uint8_t *reserved = ReserveBlock4(session, bytes, 1);
    if (!reserved)
        return nullptr;
    return reinterpret_cast<const char *>(reserved);
}

const char *CopyString(RetailZoneLoadSession *session, const char *source)
{
    if (!source)
        return nullptr;
    const std::size_t bytes = std::strlen(source) + 1;
    char *copy = static_cast<char *>(Alloc(session, bytes, 1));
    if (!copy)
        return nullptr;
    std::memcpy(copy, source, bytes);
    return copy;
}

// Lazily build the XAssetList wire-index-to-string-bytes table (same contract
// as the XAnim decoder's BuildScriptTable).  Pathnode wire records address
// their five script strings by table index.
bool EnsureScriptTable(RetailZoneLoadSession *session)
{
    if (session->scriptStringOffsets)
        return true;
    const uint32_t count = session->scriptStringCount;
    if (!count || count > 65536)
        return false;
    const XBlock &block = session->zoneMemory->blocks[4];
    if (count > block.size / 4u)
        return false;
    uint32_t *offsets = static_cast<uint32_t *>(Alloc(session, count * 4u, 4));
    if (!offsets)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        offsets[i] = UINT32_MAX;

    XBlock blocks[9]{};
    for (uint32_t i = 0; i < 9; ++i)
        blocks[i] = session->zoneMemory->blocks[i];

    uint32_t pos = count * 4u;
    for (uint32_t i = 0; i < count; ++i)
    {
        const uint32_t slot = Le32(block.data + i * 4u);
        if (slot == kInlineReference)
        {
            uint32_t end = pos;
            while (end < block.size && block.data[end] != 0)
                ++end;
            if (end >= block.size)
                return false;
            offsets[i] = pos;
            pos = end + 1;
        }
        else if (slot != 0)
        {
            RetailWireToken token{};
            RetailPtr32 encoded{};
            encoded.encoded = slot;
            if (RetailWireTokenDecodeBlocks(blocks, encoded, 1, kBlock4Mask, &token) &&
                token.kind == RETAIL_WIRE_TOKEN_OFFSET && token.block == 4)
            {
                uint32_t end = token.offset;
                bool printable = true;
                while (end < block.size && block.data[end] != 0)
                {
                    const unsigned char c = block.data[end];
                    if (c < 0x20 || c >= 0x7f)
                    {
                        printable = false;
                        break;
                    }
                    ++end;
                }
                if (printable && end < block.size)
                    offsets[i] = token.offset;
            }
        }
    }
    session->scriptStringOffsets = offsets;
    return true;
}

// Intern one resolved table string through the real SL table, exactly like
// the legacy Load_ScriptStringCustom result (a u16 native string id).
bool ResolveScriptString(RetailZoneLoadSession *session, uint32_t wireIndex,
                         uint16_t *out)
{
    *out = 0;
    if (!wireIndex)
        return true;
    if (wireIndex >= session->scriptStringCount || !EnsureScriptTable(session))
        return false;
    const uint32_t offset = session->scriptStringOffsets[wireIndex];
    if (offset == UINT32_MAX)
        return false;
    const char *str = reinterpret_cast<const char *>(Block(session, 4) + offset);
    const uint32_t found = SL_FindString(str);
    uint32_t id = 0;
    if (found)
    {
        SL_AddRefToString(found);
        id = found;
    }
    else
    {
        id = SL_GetString(str, 4u);
        if (!id || id >= 0x10000u)
            return false;
    }
    *out = static_cast<uint16_t>(id);
    return true;
}

bool DecodeTree(RetailZoneLoadSession *session, uint32_t wireOffset,
                uint32_t *budget, pathnode_tree_t **out);

// Copy a wire leaf's u16 node list (inline or alias) into arena storage.
bool DecodeTreeNodeList(RetailZoneLoadSession *session, uint32_t count,
                        uint32_t dataRef, uint16_t **out)
{
    *out = nullptr;
    if (!dataRef || !count)
        return true;
    if (count > UINT32_MAX / 2u)
        return false;
    const uint32_t bytes = count * 2u;
    const uint8_t *source = nullptr;
    if (dataRef == kInlineReference)
    {
        source = ReserveBlock4(session, bytes, 2);
    }
    else
    {
        uint32_t offset = 0;
        if (!TokenOffset(session, dataRef, bytes, 4, &offset))
            return false;
        source = Block(session, 4) + offset;
    }
    if (!source)
        return false;
    uint16_t *nodes = static_cast<uint16_t *>(Alloc(session, bytes, 2));
    if (!nodes)
        return false;
    std::memcpy(nodes, source, bytes);
    *out = nodes;
    return true;
}

struct TreeDecodeSlot
{
    uint32_t wireOffset;
    pathnode_tree_t *node;
    bool inProgress;
    bool decoded;
};

struct TreeDecodeContext
{
    RetailZoneLoadSession *session;
    TreeDecodeSlot *slots;
    uint32_t slotCount;
    uint32_t slotCapacity;
    uint32_t budget;
};

TreeDecodeSlot *FindTreeSlot(TreeDecodeContext *ctx, uint32_t wireOffset)
{
    for (uint32_t i = 0; i < ctx->slotCount; ++i)
    {
        if (ctx->slots[i].wireOffset == wireOffset)
            return &ctx->slots[i];
    }
    return nullptr;
}

bool DecodeTreeAt(TreeDecodeContext *ctx, uint32_t wireOffset, pathnode_tree_t **out);

bool DecodeTreeInto(TreeDecodeContext *ctx, TreeDecodeSlot *slot)
{
    RetailZoneLoadSession *session = ctx->session;
    const XBlock &block = session->zoneMemory->blocks[4];
    if (slot->wireOffset > block.size || kTreeBytes > block.size - slot->wireOffset)
        return false;
    const uint8_t *wire = block.data + slot->wireOffset;
    pathnode_tree_t *native = slot->node;
    native->axis = static_cast<int32_t>(Le32(wire));
    native->dist = LeFloat(wire + 4);

    if (native->axis < 0)
    {
        const uint32_t count = Le32(wire + 8);
        const uint32_t dataRef = Le32(wire + 12);
        if (!DecodeTreeNodeList(session, count, dataRef, &native->u.s.nodes))
            return false;
        native->u.s.nodeCount = static_cast<int>(count);
        slot->decoded = true;
        slot->inProgress = false;
        return true;
    }

    for (uint32_t child = 0; child < 2; ++child)
    {
        const uint32_t childRef = Le32(wire + 8 + child * 4u);
        if (!childRef)
            continue;
        if (childRef == kInlineReference)
        {
            // The wire streams an inline child's 16-byte record at exactly
            // this point, so it is decoded here -- the same place the
            // walk-only reader (ReadRetailPathNodeTreeNode) and db_load's
            // Load_pathnode_tree_ptrArray consume it.
            const uint8_t *childWire = ReserveBlock4(session, kTreeBytes, 4);
            if (!childWire)
                return false;
            const uint32_t childOffset = static_cast<uint32_t>(childWire - block.data);
            if (!DecodeTreeAt(ctx, childOffset, &native->u.child[child]))
                return false;
            continue;
        }
        // Every other form aliases a record already present in block 4 -- in
        // practice an earlier slot of this same tree array (killhouse's real
        // tree).  It consumes no stream bytes and must NOT be decoded here:
        // the slot loop decodes the array in wire order, and decoding an
        // array record early would reserve that leaf's u16 node list out of
        // order, handing it to another leaf.  The pointer-table forms stay
        // unsupported and fail loudly rather than binding a fabricated
        // subtree.
        uint32_t childOffset = 0;
        if (!TokenOffset(session, childRef, kTreeBytes, 4, &childOffset))
        {
            Com_Printf(0, "RetailPathData: tree child ref=0x%08x is not a block-4 alias\n",
                       childRef);
            return false;
        }
        TreeDecodeSlot *target = FindTreeSlot(ctx, childOffset);
        if (!target)
        {
            Com_Printf(0,
                       "RetailPathData: tree child ref=0x%08x offset=%u is not a record of "
                       "this tree array\n",
                       childRef, childOffset);
            return false;
        }
        native->u.child[child] = target->node;
    }
    slot->decoded = true;
    slot->inProgress = false;
    return true;
}

// Resolve one wire tree offset (top-level array slot, streamed child, or an
// alias target) to exactly one native node, so aliased children share the
// widened node instead of duplicating or fabricating it.
bool DecodeTreeAt(TreeDecodeContext *ctx, uint32_t wireOffset, pathnode_tree_t **out)
{
    *out = nullptr;
    if (TreeDecodeSlot *existing = FindTreeSlot(ctx, wireOffset))
    {
        *out = existing->node;
        if (existing->decoded || existing->inProgress)
            return true;
        existing->inProgress = true;
        if (!DecodeTreeInto(ctx, existing))
        {
            Com_Printf(0, "RetailPathData: tree node offset=%u decode failed\n", wireOffset);
            return false;
        }
        *out = existing->node;
        return true;
    }
    if (!ctx->budget)
    {
        Com_Printf(0, "RetailPathData: tree budget exhausted offset=%u\n", wireOffset);
        return false;
    }
    --ctx->budget;
    if (ctx->slotCount >= ctx->slotCapacity)
    {
        Com_Printf(0, "RetailPathData: tree slot table full capacity=%u\n", ctx->slotCapacity);
        return false;
    }
    pathnode_tree_t *native = static_cast<pathnode_tree_t *>(
        Alloc(ctx->session, sizeof(pathnode_tree_t), alignof(pathnode_tree_t)));
    if (!native)
        return false;
    std::memset(native, 0, sizeof(*native));
    TreeDecodeSlot *slot = &ctx->slots[ctx->slotCount++];
    slot->wireOffset = wireOffset;
    slot->node = native;
    slot->inProgress = true;
    slot->decoded = false;
    if (!DecodeTreeInto(ctx, slot))
    {
        Com_Printf(0, "RetailPathData: tree node offset=%u decode failed\n", wireOffset);
        return false;
    }
    *out = native;
    return true;
}

bool DecodeNode(RetailZoneLoadSession *session, const uint8_t *wire, pathnode_t *native)
{
    std::memset(native, 0, sizeof(*native));
    // Native constant/dynamic offsets are wire-identical for their scalar
    // spans; only the trailing Links pointer changes width.
    std::memcpy(&native->constant, wire, kNodeConstantBytes);
    std::memcpy(&native->dynamic, wire + kNodeDynamicOffset, kNodeDynamicBytes);

    if (!ResolveScriptString(session, native->constant.targetname,
                             &native->constant.targetname) ||
        !ResolveScriptString(session, native->constant.script_linkName,
                             &native->constant.script_linkName) ||
        !ResolveScriptString(session, native->constant.script_noteworthy,
                             &native->constant.script_noteworthy) ||
        !ResolveScriptString(session, native->constant.target,
                             &native->constant.target) ||
        !ResolveScriptString(session, native->constant.animscript,
                             &native->constant.animscript))
        return false;

    const uint32_t linkCount = static_cast<uint32_t>(wire[kNodeLinkCountOffset]) |
                               (static_cast<uint32_t>(wire[kNodeLinkCountOffset + 1]) << 8);
    const uint32_t linksRef = Le32(wire + kNodeLinksRefOffset);
    native->constant.Links = nullptr;
    native->constant.totalLinkCount = static_cast<uint16_t>(linkCount);
    if (!linksRef)
        return true;
    // The proven walker consumes totalLinkCount*12 for every non-null Links
    // slot regardless of the reference form (the serialized array is inline;
    // only the unused 32-bit pointer value varies), so the native Links must
    // consume the same bytes.
    if (linkCount > UINT32_MAX / kLinkBytes)
        return false;
    const uint32_t bytes = linkCount * kLinkBytes;
    if (!bytes)
        return true;
    const uint8_t *source = ReserveBlock4(session, bytes, 4);
    if (!source)
    {
        Com_Printf(0, "RetailPathData: link array reserve failed cursor=%u links=%u\n",
                   session->wire.cursor[4], linkCount);
        return false;
    }
    pathlink_s *links = static_cast<pathlink_s *>(Alloc(session, bytes, 4));
    if (!links)
        return false;
    std::memcpy(links, source, bytes);
    native->constant.Links = links;
    return true;
}

bool LoadGameWorldSp(RetailZoneLoadSession *session, XAssetType type, bool insert,
                     void *, XAssetHeader *header)
{
    if (!session || !header || type != ASSET_TYPE_GAMEWORLD_SP || insert)
        return false;

    uint8_t root[kRootBytes]{};
    if (!RetailWireBlocksRead(&session->wire, 0, root, sizeof(root)))
    {
        Com_Printf(0, "RetailPathData: root unreadable\n");
        return false;
    }
    const uint32_t nameRef = Le32(root);
    const uint32_t nodeCount = Le32(root + 4);
    const uint32_t nodesRef = Le32(root + 8);
    const uint32_t baseRef = Le32(root + 12);
    const uint32_t chainCount = Le32(root + kChainCountOffset);
    const uint32_t chainRefs[2] = {Le32(root + kChainARefOffset), Le32(root + kChainBRefOffset)};
    const uint32_t visBytes = Le32(root + kVisBytesOffset);
    const uint32_t visRef = Le32(root + kVisRefOffset);
    const uint32_t treeCount = Le32(root + kTreeCountOffset);
    const uint32_t treeRef = Le32(root + kTreeRefOffset);

    if (nodeCount > kMaxNodeCount || treeCount > kMaxTreeCount)
    {
        Com_Printf(0, "RetailPathData: implausible counts nodes=%u trees=%u\n",
                   nodeCount, treeCount);
        return false;
    }
    if (nodeCount && nodesRef != kInlineReference)
    {
        Com_Printf(0, "RetailPathData: non-inline node array ref=0x%08x\n", nodesRef);
        return false;
    }
    if (baseRef && baseRef != kInlineReference)
    {
        Com_Printf(0, "RetailPathData: non-inline base node ref=0x%08x\n", baseRef);
        return false;
    }

    const char *name = nullptr;
    if (nameRef == kInlineReference)
    {
        name = CopyString(session, ReserveInlineString(session));
        if (!name)
        {
            Com_Printf(0, "RetailPathData: inline name unreadable\n");
            return false;
        }
    }
    else if (nameRef)
    {
        uint32_t offset = 0;
        if (!TokenOffset(session, nameRef, 1, 4, &offset))
        {
            Com_Printf(0, "RetailPathData: name alias 0x%08x undecodable\n", nameRef);
            return false;
        }
        name = CopyString(session, reinterpret_cast<const char *>(Block(session, 4) + offset));
        if (!name)
            return false;
    }

    pathnode_t *nodes = nullptr;
    if (nodeCount)
    {
        const uint8_t *wireNodes = ReserveBlock4(session, nodeCount * kNodeBytes, 4);
        if (!wireNodes)
        {
            Com_Printf(0, "RetailPathData: node array reserve failed cursor=%u count=%u\n",
                       session->wire.cursor[4], nodeCount);
            return false;
        }
        nodes = static_cast<pathnode_t *>(Alloc(session, nodeCount * sizeof(pathnode_t),
                                                alignof(pathnode_t)));
        if (!nodes)
            return false;
        for (uint32_t i = 0; i < nodeCount; ++i)
        {
            if (!DecodeNode(session, wireNodes + i * kNodeBytes, &nodes[i]))
            {
                Com_Printf(0, "RetailPathData: node %u decode failed\n", i);
                return false;
            }
        }
    }

    pathbasenode_t *basenodes = nullptr;
    if (baseRef && nodeCount)
    {
        basenodes = static_cast<pathbasenode_t *>(
            Alloc(session, nodeCount * kBaseBytes, 16));
        if (!basenodes)
            return false;
        std::memset(basenodes, 0, nodeCount * kBaseBytes);
    }

    uint16_t *chains[2] = {nullptr, nullptr};
    for (uint32_t chain = 0; chain < 2; ++chain)
    {
        const uint32_t ref = chainRefs[chain];
        if (!ref || !nodeCount)
            continue;
        if (nodeCount > UINT32_MAX / 2u)
            return false;
        // Android Load_UnsignedShortArray sizes both chain arrays by
        // nodeCount (the root's chainNodeCount is only the meaningful
        // prefix), so consume/replay exactly the walker's nodeCount*2.
        const uint32_t bytes = nodeCount * 2u;
        const uint8_t *source = nullptr;
        if (ref == kInlineReference)
        {
            source = ReserveBlock4(session, bytes, 2);
        }
        else
        {
            uint32_t offset = 0;
            if (!TokenOffset(session, ref, bytes, 4, &offset))
            {
                Com_Printf(0, "RetailPathData: chain %u alias undecodable\n", chain);
                return false;
            }
            source = Block(session, 4) + offset;
        }
        if (!source)
            return false;
        chains[chain] = static_cast<uint16_t *>(Alloc(session, bytes, 2));
        if (!chains[chain])
            return false;
        std::memcpy(chains[chain], source, bytes);
    }

    uint8_t *pathVis = nullptr;
    if (visRef && visBytes)
    {
        const uint8_t *source = nullptr;
        if (visRef == kInlineReference)
        {
            source = ReserveBlock4(session, visBytes, 1);
        }
        else
        {
            uint32_t offset = 0;
            if (!TokenOffset(session, visRef, visBytes, 4, &offset))
            {
                Com_Printf(0, "RetailPathData: vis alias 0x%08x undecodable\n", visRef);
                return false;
            }
            source = Block(session, 4) + offset;
        }
        if (!source)
            return false;
        pathVis = static_cast<uint8_t *>(Alloc(session, visBytes, 1));
        if (!pathVis)
            return false;
        std::memcpy(pathVis, source, visBytes);
    }

    pathnode_tree_t *nodeTree = nullptr;
    if (treeRef && treeCount)
    {
        if (treeRef != kInlineReference)
        {
            Com_Printf(0, "RetailPathData: non-inline tree array ref=0x%08x\n", treeRef);
            return false;
        }
        const uint32_t treeArrayBytes = treeCount * kTreeBytes;
        const uint8_t *wireTrees = ReserveBlock4(session, treeArrayBytes, 4);
        if (!wireTrees)
            return false;
        nodeTree = static_cast<pathnode_tree_t *>(
            Alloc(session, treeCount * sizeof(pathnode_tree_t), alignof(pathnode_tree_t)));
        if (!nodeTree)
            return false;
        std::memset(nodeTree, 0, treeCount * sizeof(pathnode_tree_t));
        // Same exact visit budget as the walker: every visited node consumed
        // 16 stream bytes, so the array plus the remaining block-4 span is an
        // upper bound that can neither loop forever nor false-reject a tree.
        // Alias targets resolve through the slot table instead of new visits.
        const XBlock &block = session->zoneMemory->blocks[4];
        uint32_t budget = treeCount;
        if (session->wire.cursor[4] < block.size)
            budget += (block.size - session->wire.cursor[4]) / kTreeBytes;
        constexpr uint32_t kExtraTreeSlots = 16384;
        const uint32_t slotCapacity =
            treeCount > UINT32_MAX - kExtraTreeSlots ? UINT32_MAX : treeCount + kExtraTreeSlots;
        TreeDecodeSlot *slots = static_cast<TreeDecodeSlot *>(
            Alloc(session, static_cast<std::size_t>(slotCapacity) * sizeof(TreeDecodeSlot),
                  alignof(TreeDecodeSlot)));
        if (!slots)
            return false;
        TreeDecodeContext ctx{session, slots, 0, slotCapacity, budget};
        const uint32_t treesStart = static_cast<uint32_t>(wireTrees - block.data);
        for (uint32_t i = 0; i < treeCount; ++i)
        {
            TreeDecodeSlot &slot = ctx.slots[ctx.slotCount++];
            slot.wireOffset = treesStart + i * kTreeBytes;
            slot.node = &nodeTree[i];
            slot.inProgress = false;
            slot.decoded = false;
        }
        for (uint32_t i = 0; i < treeCount; ++i)
        {
            pathnode_tree_t *decoded = nullptr;
            if (!DecodeTreeAt(&ctx, treesStart + i * kTreeBytes, &decoded))
            {
                Com_Printf(0, "RetailPathData: tree %u decode failed\n", i);
                return false;
            }
        }
    }

    GameWorldSp candidate{};
    candidate.name = name;
    candidate.path.nodeCount = nodeCount;
    candidate.path.nodes = nodes;
    candidate.path.basenodes = basenodes;
    candidate.path.chainNodeCount = chainCount;
    candidate.path.chainNodeForNode = chains[0];
    candidate.path.nodeForChainNode = chains[1];
    candidate.path.visBytes = static_cast<int>(visBytes);
    candidate.path.pathVis = pathVis;
    candidate.path.nodeTreeCount = static_cast<int>(treeCount);
    candidate.path.nodeTree = nodeTree;
    gameWorldSp = candidate;
    *header = {&gameWorldSp};
    return true;
}
} // namespace

bool RetailZoneInstallGameWorldSpDecoder(RetailZoneLoadSession *session)
{
    return RetailZoneLoadSessionSetAssetLoader(session, ASSET_TYPE_GAMEWORLD_SP,
                                               LoadGameWorldSp, nullptr);
}
