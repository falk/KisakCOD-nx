#include "db_retail_decode_render.h"
#include <cstring>
namespace { uint32_t ReadLe32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); } }
RetailTechniqueSetDecodeResult DB_RetailDecodeTechniqueSet(RetailZoneLoadSession *session, uint32_t headerRef, const char *expectedName, MaterialTechniqueSet **out)
{
    if (!session || !session->active || !expectedName || !expectedName[0] || !out) return RETAIL_TECHNIQUE_SET_BAD_ARGUMENT;
    *out = nullptr;
    if (headerRef != 0xffffffffu) return RETAIL_TECHNIQUE_SET_UNSUPPORTED_REFERENCE;
    uint8_t wire[148];
    if (!RetailWireBlocksAlign(&session->wire, 4) || !RetailWireBlocksRead(&session->wire, 0, wire, sizeof(wire))) return RETAIL_TECHNIQUE_SET_TRUNCATED;
    const uint32_t nameCursor = session->wire.cursor[4];
    RetailWireBlocksRewind(&session->wire, 4, 0);
    char name[64] = {};
    uint32_t length = 0;
    for (;;) { if (length + 1 >= sizeof(name) || !RetailWireBlocksRead(&session->wire, 4, &name[length], 1)) { RetailWireBlocksRewind(&session->wire, 4, nameCursor); return RETAIL_TECHNIQUE_SET_TRUNCATED; } if (!name[length++]) break; }
    RetailWireBlocksRewind(&session->wire, 4, nameCursor);
    if (std::strcmp(name, expectedName) != 0) return RETAIL_TECHNIQUE_SET_UNSUPPORTED_REFERENCE;
    auto *set = static_cast<MaterialTechniqueSet *>(RetailZoneLoadSessionAlloc(session, sizeof(MaterialTechniqueSet), alignof(MaterialTechniqueSet)));
    if (!set) return RETAIL_TECHNIQUE_SET_ALLOCATION_FAILED;
    std::memset(set, 0, sizeof(*set));
    set->name = static_cast<const char *>(RetailZoneLoadSessionAlloc(session, length, alignof(char)));
    if (!set->name) return RETAIL_TECHNIQUE_SET_ALLOCATION_FAILED;
    std::memcpy(const_cast<char *>(set->name), name, length);
    set->worldVertFormat = wire[4];
    for (uint32_t slot = 0; slot < 34; ++slot) if (ReadLe32(wire + 12 + slot * 4) != 0) return RETAIL_TECHNIQUE_SET_UNSUPPORTED_REFERENCE;
    XAssetHeader registered = RetailZoneLoadSessionRegister(session, ASSET_TYPE_TECHNIQUE_SET, {set});
    if (!registered.techniqueSet) return RETAIL_TECHNIQUE_SET_REGISTRATION_FAILED;
    *out = registered.techniqueSet;
    return RETAIL_TECHNIQUE_SET_OK;
}
