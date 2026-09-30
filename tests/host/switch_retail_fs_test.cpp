#include "src/universal/com_files.h"
#include "src/qcommon/qcommon.h"

#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__SWITCH__) && !defined(KISAK_RETAIL_FS_PROOF_HOST)
#include <switch.h>
#endif

static int RetailFsRun(int argc, char **argv)
{
    const char *root = argc > 1 ? argv[1] : "sdmc:/switch/kisakcod/retail-source-proof";
    FS_InitRetailSource(root);

    if (!fs_basepath || std::strcmp(fs_basepath->current.string, root) != 0 ||
        fs_basepath->current.string == root)
        return 1;

    dvar_s *game = const_cast<dvar_s *>(fs_gameDirVar);
    char acceptedGame[] = "mods/proof";
    Dvar_SetString(game, acceptedGame);
    acceptedGame[5] = 'X';
    if (std::strcmp(game->current.string, "mods/proof") != 0 || game->current.string == acceptedGame ||
        !game->modified)
        return 6;
    Dvar_ClearModified(game);
    char rejectedPlain[] = "main";
    Dvar_SetString(game, rejectedPlain);
    char rejectedTraversal[] = "mods/../proof";
    Dvar_SetString(game, rejectedTraversal);
    if (std::strcmp(game->current.string, "mods/proof") != 0 || game->modified)
        return 7;
    char rejectedGame[4096];
    std::memset(rejectedGame, 'x', sizeof(rejectedGame) - 1);
    rejectedGame[sizeof(rejectedGame) - 1] = '\0';
    for (int i = 0; i < 300; ++i)
        Dvar_SetString(game, rejectedGame);
    if (std::strcmp(game->current.string, "mods/proof") != 0 || game->modified)
        return 11;

    int file = 0;
    const uint32_t iwiSize = FS_FOpenFileRead("images/logo_cod2.iwi", &file);
    if (!file || iwiSize != 11 || !FS_IsFileInZip(file))
        return 2;
    unsigned char iwi[11];
    if (FS_Read(iwi, sizeof(iwi), file) != sizeof(iwi) || std::memcmp(iwi, "fixture-iwi", sizeof(iwi)) != 0)
        return 3;
    const int staleFile = file;
    FS_FCloseFile(file);

    // FS_Seek's zip branch skips forward by discarding through a real
    // buffer. The decompiled form passed NULL to unzReadCurrentFile; zlib
    // 1.3.1 rejects that (Z_STREAM_ERROR) so the skip silently did nothing,
    // which broke streamed MP3/WAV decode init. Engine origin convention:
    // 0 = current, 1 = end, 2 = start. Probe holds byte i == i.
    int probe = 0;
    const uint32_t probeSize = FS_FOpenFileRead("images/seek_probe.iwi", &probe);
    unsigned char probeBytes[4] = {0, 0, 0, 0};
    if (!probe || probeSize != 64 || !FS_IsFileInZip(probe))
        return 12;
    if (FS_Read(probeBytes, sizeof(probeBytes), probe) != sizeof(probeBytes) ||
        probeBytes[0] != 0 || probeBytes[1] != 1 || probeBytes[2] != 2 || probeBytes[3] != 3)
        return 13;
    if (FS_Seek(probe, 32, 0) != 0 || FS_FTell(probe) != 36 ||
        FS_Read(probeBytes, sizeof(probeBytes), probe) != sizeof(probeBytes) ||
        probeBytes[0] != 36 || probeBytes[1] != 37 || probeBytes[2] != 38 || probeBytes[3] != 39)
        return 14;
    if (FS_Seek(probe, 0, 2) != 0 || FS_FTell(probe) != 0 ||
        FS_Read(probeBytes, sizeof(probeBytes), probe) != sizeof(probeBytes) ||
        probeBytes[0] != 0 || probeBytes[3] != 3)
        return 15;
    if (FS_Seek(probe, -8, 1) != 0 || FS_FTell(probe) != 56 ||
        FS_Read(probeBytes, sizeof(probeBytes), probe) != sizeof(probeBytes) ||
        probeBytes[0] != 56 || probeBytes[3] != 59)
        return 16;
    FS_FCloseFile(probe);

    file = staleFile;
    if (FS_FOpenRetailFileRead("../invalid.ff", &file) != static_cast<uint32_t>(-1) || file != 0)
        return 8;
    file = staleFile;
    FsRetailFastfileReader *fastfile = 0;
    if (FS_OpenRetailFastfile("../invalid.ff", &fastfile) != FS_RETAIL_FF_MISSING || fastfile)
        return 9;

    if (FS_OpenRetailFastfile("zone/english/code_post_gfx.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile ||
        FS_RetailFastfileXFileSize(fastfile) != 92 || FS_RetailFastfileXFileExternalSize(fastfile) != 7 ||
        FS_RetailFastfileBlockTotal(fastfile) != 92 || FS_RetailFastfileBlockSize(fastfile, 4) != 40)
        return 4;
    FsRetailFastfileAssetList assetList;
    FsRetailFastfileAsset assets[2];
    uint32_t scriptRefs[2];
    if (FS_ReadRetailFastfileAssetList(fastfile, &assetList, assets, 1, scriptRefs, 2) !=
        FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL || assetList.assetCount != 2)
        return 12;
    FS_CloseRetailFastfile(fastfile);
    if (FS_OpenRetailFastfile("zone/english/code_post_gfx.ff", &fastfile) != FS_RETAIL_FF_OK)
        return 13;
    if (FS_ReadRetailFastfileAssetList(fastfile, &assetList, assets, 2, scriptRefs, 2) != FS_RETAIL_FF_WIRE_OK ||
        assetList.scriptStringCount != 2 || assetList.assetCount != 2 ||
        assetList.assetsRef != 0xffffffffu || assetList.decodedCount != 2 ||
        scriptRefs[0] != 0x40000009u || scriptRefs[1] != 0x4000000du ||
        assets[0].type != 4 || assets[0].header != 0x11111111u ||
        assets[1].type != 6 || assets[1].header != 0x22222222u)
    {
        std::fprintf(stderr, "wire: sc=%u ac=%u ar=%08x dc=%u sr=%08x/%08x a=%u/%08x,%u/%08x\\n",
            assetList.scriptStringCount, assetList.assetCount, assetList.assetsRef, assetList.decodedCount,
            scriptRefs[0], scriptRefs[1], assets[0].type, assets[0].header, assets[1].type, assets[1].header);
        return 10;
    }
    uint8_t directory[16];
    if (FS_ReadRetailFastfileBlock(fastfile, 4, 16, directory, sizeof(directory)) != FS_RETAIL_FF_WIRE_OK ||
        directory[0] != 4 || directory[8] != 6)
        return 14;
    FS_CloseRetailFastfile(fastfile);

    if (FS_OpenRetailFastfile("zone/english/rawfile.ff", &fastfile) != FS_RETAIL_FF_OK ||
        FS_RetailFastfileXFileSize(fastfile) != 67 || FS_RetailFastfileBlockSize(fastfile, 0) != 12 ||
        FS_RetailFastfileBlockSize(fastfile, 4) != 39)
        return 15;
    FsRetailFastfileAsset rawAsset;
    const FsRetailFastfileWireResult rawListResult =
        FS_ReadRetailFastfileAssetList(fastfile, &assetList, &rawAsset, 1, 0, 0);
    if (rawListResult != FS_RETAIL_FF_WIRE_OK || rawAsset.type != 31 || rawAsset.header != 0xffffffffu)
    {
        std::fprintf(stderr, "raw list: result=%d type=%u header=%08x\\n", rawListResult, rawAsset.type, rawAsset.header);
        return 16;
    }
    FsRetailFastfileRawFile rawFile;
    if (FS_ReadRetailFastfileRawFile(fastfile, rawAsset.header, &rawFile) != FS_RETAIL_FF_WIRE_OK ||
        rawFile.nameRef != 0x40000009u || rawFile.len != 5 || rawFile.bufferRef != 0x40000022u)
        return 17;
    char rawName[25];
    char rawBuffer[6];
    if (FS_ReadRetailFastfileBlock(fastfile, 4, 8, reinterpret_cast<uint8_t *>(rawName), sizeof(rawName)) != FS_RETAIL_FF_WIRE_OK ||
        FS_ReadRetailFastfileBlock(fastfile, 4, 33, reinterpret_cast<uint8_t *>(rawBuffer), sizeof(rawBuffer)) != FS_RETAIL_FF_WIRE_OK ||
        std::strcmp(rawName, "synthetic/raw_inline.txt") != 0 || std::strcmp(rawBuffer, "hello") != 0)
        return 18;
    FS_CloseRetailFastfile(fastfile);

    if (FS_OpenRetailFastfile("zone/english/missing.ff", &fastfile) != FS_RETAIL_FF_MISSING || fastfile ||
        FS_OpenRetailFastfile("zone/english/wrong.ff", &fastfile) != FS_RETAIL_FF_WRONG_MAGIC || fastfile ||
        FS_OpenRetailFastfile("zone/english/version.ff", &fastfile) != FS_RETAIL_FF_WRONG_VERSION || fastfile ||
        FS_OpenRetailFastfile("zone/english/truncated.ff", &fastfile) != FS_RETAIL_FF_TRUNCATED || fastfile ||
        FS_OpenRetailFastfile("zone/english/zlib-truncated.ff", &fastfile) != FS_RETAIL_FF_ZLIB_TRUNCATED || fastfile ||
        FS_OpenRetailFastfile("zone/english/xfile-truncated.ff", &fastfile) != FS_RETAIL_FF_XFILE_HEADER_TRUNCATED || fastfile ||
        FS_OpenRetailFastfile("zone/english/over-cap.ff", &fastfile) != FS_RETAIL_FF_BLOCK_TOTAL_OVER_CAP || fastfile)
        return 5;
    return 0;
}

static uint32_t g_retailRawFileCount;
static bool g_retailDirectoryProof;
static bool g_retailTechniqueProof;
static bool g_retailMaterialImageProof;
static bool g_retailUiClosureProof;

// Writes one synthetic fastfile built from stored (uncompressed) deflate
// blocks so the payload bytes are readable in the source: container magic +
// version 5 + a zlib stream of [44-byte XFile header][payload].
static bool WriteSyntheticFastfile(
    const char *path,
    const std::vector<uint8_t> &payload,
    uint32_t xfileSize,
    uint32_t externalSize,
    const uint32_t (&blockSizes)[9])
{
    std::vector<uint8_t> file;
    const uint8_t magic[12] = {'I', 'W', 'f', 'f', 'u', '1', '0', '0', 5, 0, 0, 0};
    file.insert(file.end(), magic, magic + sizeof(magic));
    file.push_back(0x78);
    file.push_back(0x01);
    std::vector<uint8_t> stream;
    auto le32 = [&stream](uint32_t v) {
        stream.push_back(static_cast<uint8_t>(v));
        stream.push_back(static_cast<uint8_t>(v >> 8));
        stream.push_back(static_cast<uint8_t>(v >> 16));
        stream.push_back(static_cast<uint8_t>(v >> 24));
    };
    le32(xfileSize);
    le32(externalSize);
    for (uint32_t block = 0; block < 9; ++block)
        le32(blockSizes[block]);
    stream.insert(stream.end(), payload.begin(), payload.end());
    // adler32 of the stream payload
    uint32_t a = 1, b = 0;
    for (uint8_t byte : stream)
    {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    // one final stored block carrying the whole stream
    file.push_back(0x01);
    const uint32_t len = static_cast<uint32_t>(stream.size());
    file.push_back(static_cast<uint8_t>(len));
    file.push_back(static_cast<uint8_t>(len >> 8));
    file.push_back(static_cast<uint8_t>(~len));
    file.push_back(static_cast<uint8_t>(~len >> 8));
    file.insert(file.end(), stream.begin(), stream.end());
    file.push_back(static_cast<uint8_t>(adler >> 24));
    file.push_back(static_cast<uint8_t>(adler >> 16));
    file.push_back(static_cast<uint8_t>(adler >> 8));
    file.push_back(static_cast<uint8_t>(adler));
    FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    const size_t written = std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
    return written == file.size();
}

static void AppendLe32(std::vector<uint8_t> &v, uint32_t value)
{
    v.push_back(static_cast<uint8_t>(value));
    v.push_back(static_cast<uint8_t>(value >> 8));
    v.push_back(static_cast<uint8_t>(value >> 16));
    v.push_back(static_cast<uint8_t>(value >> 24));
}

static void AppendString(std::vector<uint8_t> &v, const char *s)
{
    v.insert(v.end(), reinterpret_cast<const uint8_t *>(s),
             reinterpret_cast<const uint8_t *>(s) + std::strlen(s) + 1);
}

// Builds a synthetic zone holding one empty comma technique-set stub and
// four materials exercising the Android HandleAssetSlot/LoadGfxImagePtr
// semantics: synth_a loads an inline image (-1 def slot, -2 texture union
// with a DB_InsertPointer slot), synth_b aliases synth_a's image through
// synth_a's texture-def image slot, synth_c loads an inline image through a
// -2 def slot (its own insert slot), and synth_d aliases synth_c's image
// through that insert slot.  Directory: [0] techset ,synthetic/2d
// [1] material synth_a  [2] material synth_b  [3] material synth_c
// [4] material synth_d.
static bool BuildMaterialImageFixture(const char *path)
{
    std::vector<uint8_t> payload; // stream content after the XAssetList
    // XAssetList: 0 strings, 5 assets, inline directory
    AppendLe32(payload, 0);
    AppendLe32(payload, 0);
    AppendLe32(payload, 5);
    AppendLe32(payload, 0xffffffffu);
    // directory (lands at block-4 offset 0, 40 bytes)
    AppendLe32(payload, 5);
    AppendLe32(payload, 0xffffffffu);
    for (int i = 0; i < 4; ++i)
    {
        AppendLe32(payload, 4);
        AppendLe32(payload, 0xffffffffu);
    }
    // Block-4 layout the walk produces (hardcoded, matching walk order):
    //   40..53  stub name ",synthetic/2d"
    //   54..61  synth_a name
    //   64..75  synth_a texture table (def image slot at 72)
    //   76..87  synth_a image name "synth_image"
    //   88..91  synth_a texture-union insert slot (DB_InsertPointer)
    //   92..99  synth_a stateBits
    //   100..107 synth_b name; 108..119 synth_b table
    //   120..127 synth_c name; 128..139 synth_c table
    //   140..143 synth_c def-slot insert slot
    //   144..157 synth_c image name "synth_c_image"
    //   158..165 synth_d name; 168..179 synth_d table
    const uint32_t techNameRef = ((4u << 28) | 40u) + 1u;
    const uint32_t aliasDefSlotRef = ((4u << 28) | 72u) + 1u;     // synth_a def image slot
    const uint32_t aliasInsertSlotRef = ((4u << 28) | 140u) + 1u; // synth_c def insert slot

    // [0] techset stub body lands in temp block 0, its inline name at 40
    std::vector<uint8_t> techset(148, 0);
    techset[0] = techset[1] = techset[2] = techset[3] = 0xff;
    payload.insert(payload.end(), techset.begin(), techset.end());
    AppendString(payload, ",synthetic/2d");
    // material synth_a: 80-byte body; techniqueSet ref names the stub,
    // textureTable inline, stateBits inline
    std::vector<uint8_t> material(80, 0);
    material[0] = material[1] = material[2] = material[3] = 0xff;
    material[58] = 1;  // textureCount
    material[59] = 0;  // constantCount
    material[60] = 1;  // stateBitsCount
    material[64] = static_cast<uint8_t>(techNameRef >> 24);
    material[65] = static_cast<uint8_t>(techNameRef >> 16);
    material[66] = static_cast<uint8_t>(techNameRef >> 8);
    material[67] = static_cast<uint8_t>(techNameRef);
    material[68] = material[69] = material[70] = material[71] = 0xff;
    material[76] = material[77] = material[78] = material[79] = 0xff;
    payload.insert(payload.end(), material.begin(), material.end());
    AppendString(payload, "synth_a");
    // texture def: nameHash + nameStart/nameEnd + sampler + semantic + imgRef
    AppendLe32(payload, 0x12345678);
    payload.push_back('c');
    payload.push_back('p');
    payload.push_back(0xe2);
    payload.push_back(0);
    AppendLe32(payload, 0xffffffffu); // inline image
    // image body: 36 bytes, name inline, texture slot -2 (insert, LE)
    std::vector<uint8_t> image(36, 0);
    image[4] = 0xfe;
    image[5] = image[6] = image[7] = 0xff;                // -2 insert
    image[24] = 16;                                       // width
    image[26] = 16;                                       // height
    image[30] = 3;                                        // category
    image[32] = image[33] = image[34] = image[35] = 0xff; // name inline
    payload.insert(payload.end(), image.begin(), image.end());
    AppendString(payload, "synth_image");
    // loadDef: levelCount, flags, dims[3], format, resourceSize
    payload.push_back(1);
    payload.push_back(2);
    payload.push_back(16);
    payload.push_back(0);
    payload.push_back(16);
    payload.push_back(0);
    payload.push_back(1);
    payload.push_back(0);
    AppendLe32(payload, 0x31545844);
    AppendLe32(payload, 0);
    // state bits table: one 8-byte entry
    AppendLe32(payload, 0x11223344);
    AppendLe32(payload, 0x55667788);
    // material synth_b: aliases synth_a's image through synth_a's
    // texture-def image slot (DB_ConvertOffsetToAlias copies the pooled
    // reference the walk wrote there).
    std::vector<uint8_t> materialB(80, 0);
    materialB[0] = materialB[1] = materialB[2] = materialB[3] = 0xff;
    materialB[58] = 1;
    materialB[59] = 0;
    materialB[60] = 0;
    materialB[64] = static_cast<uint8_t>(techNameRef >> 24);
    materialB[65] = static_cast<uint8_t>(techNameRef >> 16);
    materialB[66] = static_cast<uint8_t>(techNameRef >> 8);
    materialB[67] = static_cast<uint8_t>(techNameRef);
    materialB[68] = materialB[69] = materialB[70] = materialB[71] = 0xff;
    payload.insert(payload.end(), materialB.begin(), materialB.end());
    AppendString(payload, "synth_b");
    AppendLe32(payload, 0x87654321);
    payload.push_back('c');
    payload.push_back('p');
    payload.push_back(0xe2);
    payload.push_back(0);
    AppendLe32(payload, aliasDefSlotRef);
    // material synth_c: inline image through a -2 def slot, so the walk
    // reserves a dedicated insert slot in block 4 before the image body.
    std::vector<uint8_t> materialC(80, 0);
    materialC[0] = materialC[1] = materialC[2] = materialC[3] = 0xff;
    materialC[58] = 1;
    materialC[59] = 0;
    materialC[60] = 0;
    materialC[64] = static_cast<uint8_t>(techNameRef >> 24);
    materialC[65] = static_cast<uint8_t>(techNameRef >> 16);
    materialC[66] = static_cast<uint8_t>(techNameRef >> 8);
    materialC[67] = static_cast<uint8_t>(techNameRef);
    materialC[68] = materialC[69] = materialC[70] = materialC[71] = 0xff;
    payload.insert(payload.end(), materialC.begin(), materialC.end());
    AppendString(payload, "synth_c");
    AppendLe32(payload, 0x2468ace0);
    payload.push_back('c');
    payload.push_back('p');
    payload.push_back(0xe2);
    payload.push_back(0);
    AppendLe32(payload, 0xfffffffeu); // -2 def slot: insert-token image
    // image body: 36 bytes, texture union slot 0 (no loadDef follows)
    std::vector<uint8_t> imageC(36, 0);
    imageC[24] = 8;                                        // width
    imageC[26] = 8;                                        // height
    imageC[30] = 3;                                        // category
    imageC[32] = imageC[33] = imageC[34] = imageC[35] = 0xff; // name inline
    payload.insert(payload.end(), imageC.begin(), imageC.end());
    AppendString(payload, "synth_c_image");
    // material synth_d: aliases synth_c's image through the def insert slot
    // the walk fills with the pooled reference.
    std::vector<uint8_t> materialD(80, 0);
    materialD[0] = materialD[1] = materialD[2] = materialD[3] = 0xff;
    materialD[58] = 1;
    materialD[59] = 0;
    materialD[60] = 0;
    materialD[64] = static_cast<uint8_t>(techNameRef >> 24);
    materialD[65] = static_cast<uint8_t>(techNameRef >> 16);
    materialD[66] = static_cast<uint8_t>(techNameRef >> 8);
    materialD[67] = static_cast<uint8_t>(techNameRef);
    materialD[68] = materialD[69] = materialD[70] = materialD[71] = 0xff;
    payload.insert(payload.end(), materialD.begin(), materialD.end());
    AppendString(payload, "synth_d");
    AppendLe32(payload, 0x13579bdf);
    payload.push_back('c');
    payload.push_back('p');
    payload.push_back(0xe2);
    payload.push_back(0);
    AppendLe32(payload, aliasInsertSlotRef);

    uint32_t blockSizes[9] = {160, 0, 0, 0, static_cast<uint32_t>(payload.size()), 0, 0, 0, 0};
    const uint32_t xfileSize = 44u + static_cast<uint32_t>(payload.size());
    return WriteSyntheticFastfile(path, payload, xfileSize, 0, blockSizes);
}

// Decodes the synthetic material/image fixture through the production
// walkers and checks the Android loader's pool/slot-write/alias semantics.
static int RetailMaterialImageRun(const char *root)
{
    std::string fixturePath = std::string(root) + "/zone/english/material_image.ff";
    if (!BuildMaterialImageFixture(fixturePath.c_str()))
        return 60;
    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = 0;
    FsRetailFastfileAssetList list;
    static FsRetailFastfileAsset assets[5];
    if (FS_OpenRetailFastfile("zone/english/material_image.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile)
        return 61;
    if (FS_ReadRetailFastfileAssetList(fastfile, &list, assets, 5, 0, 0) != FS_RETAIL_FF_WIRE_OK ||
        list.assetCount != 5)
        return 62;
    const uint32_t synthAPoolRef = ((RETAIL_FASTFILE_POOL_BLOCK << 28) | 0u) + 1u;
    const uint32_t synthCPoolRef = ((RETAIL_FASTFILE_POOL_BLOCK << 28) | 1u) + 1u;
    // [0] empty comma techset stub
    FsRetailFastfileTechniqueSet techset;
    if (assets[0].type != 5 ||
        FS_ReadRetailFastfileTechniqueSetPrefix(fastfile, assets[0].header, &techset) !=
            FS_RETAIL_FF_WIRE_OK)
        return 63;
    char name[64];
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (techset.nameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
        FS_RETAIL_FF_WIRE_OK)
        return 64;
    if (std::strcmp(name, ",synthetic/2d") != 0)
        return 65;
    // [1] material synth_a with an inline image
    FsRetailFastfileMaterial material;
    static FsRetailFastfileTextureDef textureDefs[4];
    if (assets[1].type != 4 ||
        FS_ReadRetailFastfileMaterial(fastfile, assets[1].header, &material, textureDefs, 4) !=
            FS_RETAIL_FF_WIRE_OK)
        return 66;
    if (material.textureCount != 1 || material.stateBitsCount != 1 ||
        material.textureTableRef >> 28 != 4 || material.stateBitsTableRef >> 28 != 4)
        return 67;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (material.nameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
        FS_RETAIL_FF_WIRE_OK)
        return 68;
    if (std::strcmp(name, "synth_a") != 0)
        return 69;
    // The inline load pools the image and writes the pooled reference back
    // into the texture-def image slot.
    if (!textureDefs[0].imageWasInline || textureDefs[0].imageRef != synthAPoolRef ||
        textureDefs[0].inlineImage.textureInsertOffset != 88 ||
        !textureDefs[0].inlineImage.haveLoadDef)
        return 70;
    // DB_InsertPointer: the texture-union insert slot holds the block-0
    // loadDef reference (LoadGfxTextureLoad writes the slot value).
    uint8_t slot[4];
    if (FS_ReadRetailFastfileBlock(fastfile, 4, 88, slot, 4) != FS_RETAIL_FF_WIRE_OK)
        return 70;
    if (static_cast<uint32_t>(slot[0] | slot[1] << 8 | slot[2] << 16 | slot[3] << 24) !=
        (((0u << 28) | 36u) + 1u))
        return 70;
    // [2] material synth_b: alias through the def image slot resolves to the
    // same pooled image.
    FsRetailFastfileMaterial materialB;
    if (assets[2].type != 4 ||
        FS_ReadRetailFastfileMaterial(fastfile, assets[2].header, &materialB, textureDefs, 4) !=
            FS_RETAIL_FF_WIRE_OK)
        return 71;
    if (materialB.textureCount != 1 || textureDefs[0].imageWasInline ||
        textureDefs[0].imageRef != synthAPoolRef)
        return 72;
    // [3] material synth_c: -2 def slot reserves its own insert slot and the
    // walk fills it with the pooled reference.
    FsRetailFastfileMaterial materialC;
    if (assets[3].type != 4 ||
        FS_ReadRetailFastfileMaterial(fastfile, assets[3].header, &materialC, textureDefs, 4) !=
            FS_RETAIL_FF_WIRE_OK)
        return 73;
    if (!textureDefs[0].imageWasInline || textureDefs[0].imageRef != synthCPoolRef ||
        textureDefs[0].inlineImage.haveLoadDef)
        return 74;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, 140, slot, 4) != FS_RETAIL_FF_WIRE_OK)
        return 75;
    if (static_cast<uint32_t>(slot[0] | slot[1] << 8 | slot[2] << 16 | slot[3] << 24) != synthCPoolRef)
        return 75;
    // [4] material synth_d: alias through the def insert slot.
    FsRetailFastfileMaterial materialD;
    if (assets[4].type != 4 ||
        FS_ReadRetailFastfileMaterial(fastfile, assets[4].header, &materialD, textureDefs, 4) !=
            FS_RETAIL_FF_WIRE_OK)
        return 76;
    if (textureDefs[0].imageWasInline || textureDefs[0].imageRef != synthCPoolRef)
        return 77;
    // Pool identity: exactly two loaded images; each pooled reference
    // resolves to its decoded name.
    if (FS_RetailFastfileImagePoolCount(fastfile) != 2)
        return 78;
    uint32_t poolNameRef = 0;
    if (FS_RetailFastfileImagePoolNameRef(fastfile, synthAPoolRef, &poolNameRef) !=
            FS_RETAIL_FF_WIRE_OK ||
        FS_ReadRetailFastfileBlock(fastfile, 4, (poolNameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
            FS_RETAIL_FF_WIRE_OK ||
        std::strcmp(name, "synth_image") != 0)
        return 79;
    if (FS_RetailFastfileImagePoolNameRef(fastfile, synthCPoolRef, &poolNameRef) !=
            FS_RETAIL_FF_WIRE_OK ||
        FS_ReadRetailFastfileBlock(fastfile, 4, (poolNameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
            FS_RETAIL_FF_WIRE_OK ||
        std::strcmp(name, "synth_c_image") != 0)
        return 80;
    // Canonical one-pass layout: the reader mirrors every streamed block-4
    // byte at its serialized (linker) offset, so an alias resolves directly
    // against the declaring slot's own serialized offset -- no engine-to-
    // reader coordinate translation exists or is needed. Both alias shapes
    // must resolve with no anchor installed: the texture-def declaration
    // slot (72) and the DB_InsertPointer insert slot (140).
    const uint32_t aliasDefSlotRef = ((4u << 28) | 72u) + 1u;
    const uint32_t aliasInsertSlotRef = ((4u << 28) | 140u) + 1u;
    uint32_t directPoolRef = 0;
    if (FS_RetailFastfileResolveImageAlias(fastfile, aliasDefSlotRef,
                                           &directPoolRef) != FS_RETAIL_FF_WIRE_OK ||
        directPoolRef != synthAPoolRef)
        return 81;
    if (FS_RetailFastfileResolveImageAlias(fastfile, aliasInsertSlotRef,
                                           &directPoolRef) != FS_RETAIL_FF_WIRE_OK ||
        directPoolRef != synthCPoolRef)
        return 82;
    FS_CloseRetailFastfile(fastfile);
    g_retailMaterialImageProof = true;
    return 0;
}

// Regression fixture for three Android stream semantics that are easy to
// miss in a bounded reader: block-0 rewinds between RawFiles, Material tables
// are independent, and a zero MaterialPass args slot consumes no bytes even
// when its serialized counts are non-zero.
static int RetailWireRegressionRun(const char *root)
{
    const std::string zoneRoot = std::string(root) + "/zone/english/";
    uint32_t blockSizes[9] = {};

    // Two RawFiles share a 12-byte temp block.  The second body can only load
    // if the first body's block-0 allocation was rewound.
    std::vector<uint8_t> payload;
    AppendLe32(payload, 0); AppendLe32(payload, 0);
    AppendLe32(payload, 2); AppendLe32(payload, 0xffffffffu);
    for (uint32_t i = 0; i < 2; ++i)
    {
        AppendLe32(payload, 31); AppendLe32(payload, 0xffffffffu);
    }
    for (const char *text : {"A", "B"})
    {
        AppendLe32(payload, 0xffffffffu);
        AppendLe32(payload, 1);
        AppendLe32(payload, 0xffffffffu);
        AppendString(payload, text);
        AppendString(payload, text);
    }
    blockSizes[0] = 12;
    blockSizes[4] = 24;
    if (!WriteSyntheticFastfile((zoneRoot + "raw_rewind.ff").c_str(), payload,
                                44u + static_cast<uint32_t>(payload.size()), 0, blockSizes))
        return 110;

    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = nullptr;
    FsRetailFastfileAssetList list = {};
    FsRetailFastfileAsset rawAssets[2] = {};
    if (FS_OpenRetailFastfile("zone/english/raw_rewind.ff", &fastfile) != FS_RETAIL_FF_OK ||
        FS_ReadRetailFastfileAssetList(fastfile, &list, rawAssets, 2, nullptr, 0) != FS_RETAIL_FF_WIRE_OK)
        return 111;
    FsRetailFastfileRawFile raw = {};
    for (uint32_t i = 0; i < 2; ++i)
        if (FS_ReadRetailFastfileRawFile(fastfile, rawAssets[i].header, &raw) != FS_RETAIL_FF_WIRE_OK ||
            raw.len != 1)
            return 112;
    FS_CloseRetailFastfile(fastfile);

    // A Material with no texture table still owns an inline constant table
    // and state-bits table.  Both must stream in the Android field order.
    payload.clear();
    AppendLe32(payload, 0); AppendLe32(payload, 0);
    AppendLe32(payload, 1); AppendLe32(payload, 0xffffffffu);
    AppendLe32(payload, 4); AppendLe32(payload, 0xffffffffu);
    std::vector<uint8_t> material(80, 0);
    material[0] = material[1] = material[2] = material[3] = 0xff;
    material[59] = 1;
    material[60] = 1;
    material[72] = material[73] = material[74] = material[75] = 0xff;
    material[76] = material[77] = material[78] = material[79] = 0xff;
    payload.insert(payload.end(), material.begin(), material.end());
    AppendString(payload, "m");
    for (uint32_t i = 0; i < 8; ++i) AppendLe32(payload, 0x11110000u + i);
    AppendLe32(payload, 0x12345678u); AppendLe32(payload, 0x9abcdef0u);
    std::memset(blockSizes, 0, sizeof(blockSizes));
    blockSizes[0] = 80;
    blockSizes[4] = 56;
    if (!WriteSyntheticFastfile((zoneRoot + "material_tables.ff").c_str(), payload,
                                44u + static_cast<uint32_t>(payload.size()), 0, blockSizes))
        return 113;
    FsRetailFastfileAsset materialAsset = {};
    if (FS_OpenRetailFastfile("zone/english/material_tables.ff", &fastfile) != FS_RETAIL_FF_OK ||
        FS_ReadRetailFastfileAssetList(fastfile, &list, &materialAsset, 1, nullptr, 0) != FS_RETAIL_FF_WIRE_OK)
        return 114;
    FsRetailFastfileMaterial decodedMaterial = {};
    if (FS_ReadRetailFastfileMaterial(fastfile, materialAsset.header, &decodedMaterial, nullptr, 0) !=
            FS_RETAIL_FF_WIRE_OK ||
        decodedMaterial.textureTableRef != 0 || decodedMaterial.constantTableOffset != 16 ||
        decodedMaterial.stateBitsOffset != 48 ||
        decodedMaterial.constantTableRef != 0x40000011u ||
        decodedMaterial.stateBitsTableRef != 0x40000031u)
        return 115;
    uint8_t stateBits[8] = {};
    if (FS_ReadRetailFastfileBlock(fastfile, 4, decodedMaterial.stateBitsOffset, stateBits,
                                   sizeof(stateBits)) != FS_RETAIL_FF_WIRE_OK ||
        stateBits[0] != 0x78 || stateBits[4] != 0xf0)
        return 116;
    FS_CloseRetailFastfile(fastfile);

    // One technique pass advertises one argument by count but carries a null
    // args slot.  Android leaves the following technique name untouched.
    payload.clear();
    AppendLe32(payload, 0); AppendLe32(payload, 0);
    AppendLe32(payload, 1); AppendLe32(payload, 0xffffffffu);
    AppendLe32(payload, 5); AppendLe32(payload, 0xffffffffu);
    std::vector<uint8_t> techset(148, 0);
    techset[0] = techset[1] = techset[2] = techset[3] = 0xff;
    techset[12] = techset[13] = techset[14] = techset[15] = 0xff;
    payload.insert(payload.end(), techset.begin(), techset.end());
    AppendString(payload, "set");
    AppendLe32(payload, 0xffffffffu);
    payload.push_back(0); payload.push_back(0); payload.push_back(1); payload.push_back(0);
    std::vector<uint8_t> pass(20, 0);
    pass[12] = 1;
    payload.insert(payload.end(), pass.begin(), pass.end());
    AppendString(payload, "t");
    std::memset(blockSizes, 0, sizeof(blockSizes));
    blockSizes[0] = 148;
    blockSizes[4] = 42;
    if (!WriteSyntheticFastfile((zoneRoot + "null_args.ff").c_str(), payload,
                                44u + static_cast<uint32_t>(payload.size()), 0, blockSizes))
        return 117;
    FsRetailFastfileAsset techniqueAsset = {};
    if (FS_OpenRetailFastfile("zone/english/null_args.ff", &fastfile) != FS_RETAIL_FF_OK ||
        FS_ReadRetailFastfileAssetList(fastfile, &list, &techniqueAsset, 1, nullptr, 0) != FS_RETAIL_FF_WIRE_OK)
        return 118;
    FsRetailFastfileTechniqueSet decodedSet = {};
    FsRetailFastfileMaterialTechnique technique = {};
    FsRetailFastfileMaterialPass decodedPass = {};
    FsRetailFastfileMaterialShaderArgument argument = {};
    if (FS_ReadRetailFastfileTechniqueSetPrefix(fastfile, techniqueAsset.header, &decodedSet) !=
            FS_RETAIL_FF_WIRE_OK ||
        FS_ReadRetailFastfileMaterialTechniquePrefix(fastfile, decodedSet.techniqueRefs[0], &technique) !=
            FS_RETAIL_FF_WIRE_OK ||
        FS_ReadRetailFastfileMaterialTechnique(fastfile, &technique, &decodedPass, 1, &argument, 1) !=
            FS_RETAIL_FF_WIRE_OK ||
        decodedPass.argCount != 1 || decodedPass.argsRef != 0 || technique.nameRef != 0x40000029u)
        return 119;
    char techniqueName[2] = {};
    if (FS_ReadRetailFastfileBlock(fastfile, 4, 40, reinterpret_cast<uint8_t *>(techniqueName), 2) !=
            FS_RETAIL_FF_WIRE_OK || std::strcmp(techniqueName, "t") != 0)
        return 120;
    FS_CloseRetailFastfile(fastfile);
    return 0;
}

// Builds a synthetic zone holding one inline menulist whose two menu slots
// are both inline.  synth_main exercises the nested walk - strings in field
// order, a two-node key-handler chain whose slots are non-zero non-token
// values (the engine allocates for any non-zero), a two-entry visibleExp
// with a VAL_STRING operand - and carries no items; synth_second carries an
// items pointer array whose bodies are deliberately absent (the reader must
// stop after the array).
static bool BuildMenuListFixture(const char *path)
{
    std::vector<uint8_t> payload;
    // XAssetList: 0 strings, 1 asset, inline directory
    AppendLe32(payload, 0);
    AppendLe32(payload, 0);
    AppendLe32(payload, 1);
    AppendLe32(payload, 0xffffffffu);
    // directory (block-4 offset 0): [0] menulist inline
    AppendLe32(payload, 20);
    AppendLe32(payload, 0xffffffffu);
    // menuList body: name inline, 2 menus, menus array inline
    AppendLe32(payload, 0xffffffffu);
    AppendLe32(payload, 2);
    AppendLe32(payload, 0xffffffffu);
    AppendString(payload, "synthetic/menus.txt"); // block-4 offset 8..27
    // menus pointer array (block-4 offset 28): both inline
    AppendLe32(payload, 0xffffffffu);
    AppendLe32(payload, 0xffffffffu);

    float rect[4] = {0.0f, 1.0f, 2.0f, 3.0f};
    // menu[0] body: name inline, font/onOpen inline, onKey non-zero (any
    // value allocates), visibleExp inline entries, no items
    std::vector<uint8_t> menu(284, 0);
    menu[0] = menu[1] = menu[2] = menu[3] = 0xff;                 // name
    for (int i = 0; i < 4; ++i)
    {
        uint32_t bits;
        std::memcpy(&bits, &rect[i], 4);
        menu[4 + i * 4] = static_cast<uint8_t>(bits);
        menu[5 + i * 4] = static_cast<uint8_t>(bits >> 8);
        menu[6 + i * 4] = static_cast<uint8_t>(bits >> 16);
        menu[7 + i * 4] = static_cast<uint8_t>(bits >> 24);
    }
    menu[160] = 1;                                                // fullScreen
    menu[156] = menu[157] = menu[158] = menu[159] = 0xff;         // font
    menu[196] = menu[197] = menu[198] = menu[199] = 0xff;         // onOpen
    menu[208] = 0xef; menu[209] = 0xbe; menu[210] = 0xad; menu[211] = 0xde; // onKey non-zero
    menu[212] = 2;                                                // visibleExp entries
    menu[216] = 1;                                                // visibleExp slot non-zero
    payload.insert(payload.end(), menu.begin(), menu.end());
    AppendString(payload, "synth_main");       // window name (block-4 36..46)
    AppendString(payload, "synth_font");       // font (47..57)
    AppendString(payload, "synth_open");       // onOpen (58..68)
    // key-handler nodes at aligned block-4 offsets 72 and 96
    AppendLe32(payload, 1); AppendLe32(payload, 0xffffffffu); AppendLe32(payload, 0xffffffffu);
    AppendString(payload, "synth_act1");
    AppendLe32(payload, 2); AppendLe32(payload, 0xffffffffu); AppendLe32(payload, 0);
    AppendString(payload, "synth_act2");
    // visibleExp entries array at 120: slot0 inline, slot1 null
    AppendLe32(payload, 0xffffffffu);
    AppendLe32(payload, 0);
    // entry0 body at 128: operator type 1, VAL_STRING operand inline
    AppendLe32(payload, 1);
    AppendLe32(payload, 2);
    AppendLe32(payload, 0xffffffffu);
    AppendString(payload, "synth_str");

    // menu[1] body: name inline, 2 items, items array inline
    std::vector<uint8_t> second(284, 0);
    second[0] = second[1] = second[2] = second[3] = 0xff;          // name
    second[164] = 2;                                               // itemCount
    second[280] = second[281] = second[282] = second[283] = 0xff;  // items
    payload.insert(payload.end(), second.begin(), second.end());
    AppendString(payload, "synth_second");
    AppendLe32(payload, 0xffffffffu); // item[0] inline (body absent on purpose)
    AppendLe32(payload, 0);           // item[1] null

    uint32_t blockSizes[9] = {320, 0, 0, 0, static_cast<uint32_t>(payload.size()), 0, 0, 0, 0};
    const uint32_t xfileSize = 44u + static_cast<uint32_t>(payload.size());
    return WriteSyntheticFastfile(path, payload, xfileSize, 0, blockSizes);
}

static bool BuildItemDefFixture(const char *path)
{
    std::vector<uint8_t> payload;
    AppendLe32(payload, 0); AppendLe32(payload, 0); AppendLe32(payload, 1);
    AppendLe32(payload, 0xffffffffu);
    // The directory is block 4 and streams immediately after XAssetList.
    // The item body follows it, exactly as FS_ReadRetailFastfileAssetList and
    // FS_ReadRetailFastfileItemDef consume it.
    AppendLe32(payload, 20); AppendLe32(payload, 0xffffffffu);
    const size_t body = payload.size();
    payload.resize(body + 372, 0);
    auto put = [&payload, body](size_t off, uint32_t value) {
        payload[body + off + 0] = static_cast<uint8_t>(value);
        payload[body + off + 1] = static_cast<uint8_t>(value >> 8);
        payload[body + off + 2] = static_cast<uint8_t>(value >> 16);
        payload[body + off + 3] = static_cast<uint8_t>(value >> 24);
    };
    put(0, 0xffffffffu); put(52, 0xffffffffu); put(180, 4); put(184, 2);
    put(224, 0xffffffffu); // text
    for (size_t off = 236; off <= 272; off += 4) put(off, 0xffffffffu);
    put(276, 0xffffffffu); // onKey
    put(280, 0xffffffffu); // enableDvar
    put(300, 0xffffffffu); // editFieldDef pointer
    put(308, 1); put(312, 0xffffffffu); // visibleExp: one inline entry
    const char *strings[] = {"item", "group", "text", "enterText", "exitText",
        "enter", "exit", "action", "accept", "focus", "leave", "dvar", "test"};
    for (const char *s : strings) AppendString(payload, s);
    // Alignment moves the wire block cursor, not the compressed-stream
    // position: the next body begins immediately in the source stream.
    AppendLe32(payload, 7); AppendLe32(payload, 0xffffffffu); AppendLe32(payload, 0);
    AppendString(payload, "keyAction");
    AppendString(payload, "enable");
    payload.resize(payload.size() + 32, 0); // editFieldDef_t
    AppendLe32(payload, 0xffffffffu); // visibleExp entry pointer array
    AppendLe32(payload, 1); AppendLe32(payload, 2); AppendLe32(payload, 0xffffffffu);
    AppendString(payload, "expression");
    // Cursor alignment leaves holes in block 4 while the source stream stays
    // packed; reserve those destination holes in the declared block size.
    const uint32_t blockSizes[9] = {0, 0, 0, 0,
        static_cast<uint32_t>(payload.size() - 16u + 8u), 0, 0, 0, 0};
    return WriteSyntheticFastfile(path, payload, 44u + static_cast<uint32_t>(payload.size()),
                                  0, blockSizes);
}

// This is a bounded cursor/order proof for the Android Load_itemDef_t
// sequence.  It is intentionally not a UI checkpoint: native widening and
// focus handling are not covered here.
static int RetailItemDefRun(const char *root)
{
    const std::string path = std::string(root) + "/zone/english/itemdef.ff";
    if (!BuildItemDefFixture(path.c_str()))
        return 108;
    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = 0;
    FsRetailFastfileAssetList list = {};
    FsRetailFastfileAsset asset = {};
    if (FS_OpenRetailFastfile("zone/english/itemdef.ff", &fastfile) != FS_RETAIL_FF_OK ||
        FS_ReadRetailFastfileAssetList(fastfile, &list, &asset, 1, 0, 0) != FS_RETAIL_FF_WIRE_OK ||
        asset.type != 20 || asset.header != 0xffffffffu)
    {
        if (fastfile) FS_CloseRetailFastfile(fastfile);
        return 109;
    }
    FsRetailFastfileItemDef item = {};
    FsRetailFastfileItemKeyHandler handlers[2] = {};
    FsRetailFastfileExpressionEntry entries[2] = {};
    const FsRetailFastfileWireResult result = FS_ReadRetailFastfileItemDef(
        fastfile, asset.header, &item, handlers, 2, entries, 2);
    if (result != FS_RETAIL_FF_WIRE_OK || item.type != 4 || item.dataType != 2 ||
        item.handlerCount != 1 || item.expressionEntryCount != 1 ||
        item.typeDataOffset == 0 || handlers[0].key != 7 ||
        entries[0].type != 1 || entries[0].dataType != 2)
    {
        std::fprintf(stderr, "itemdef result=%d type=%d data=%d handlers=%u entries=%u typeoff=%u key=%d entry=%d/%u\\n",
            result, item.type, item.dataType, item.handlerCount, item.expressionEntryCount,
            item.typeDataOffset, handlers[0].key, entries[0].type, entries[0].dataType);
        FS_CloseRetailFastfile(fastfile);
        return 110;
    }
    char value[16] = {};
    const bool validName = FS_ReadRetailFastfileBlock(
        fastfile, 4, (item.nameRef - 1u) & 0x0fffffffu,
        reinterpret_cast<uint8_t *>(value), sizeof(value)) == FS_RETAIL_FF_WIRE_OK &&
        std::strcmp(value, "item") == 0;
    const bool validExpression = FS_ReadRetailFastfileBlock(
        fastfile, 4, (entries[0].operandRef - 1u) & 0x0fffffffu,
        reinterpret_cast<uint8_t *>(value), sizeof("expression")) == FS_RETAIL_FF_WIRE_OK &&
        std::strcmp(value, "expression") == 0;
    FS_CloseRetailFastfile(fastfile);
    return validName && validExpression ? 0 : 111;
}

// Decodes the synthetic menulist fixture through the production walkers.
static int RetailMenuListRun(const char *root)
{
    std::string fixturePath = std::string(root) + "/zone/english/menulist.ff";
    if (!BuildMenuListFixture(fixturePath.c_str()))
        return 90;
    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = 0;
    FsRetailFastfileAssetList list;
    FsRetailFastfileAsset assets[1];
    if (FS_OpenRetailFastfile("zone/english/menulist.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile)
        return 91;
    if (FS_ReadRetailFastfileAssetList(fastfile, &list, assets, 1, 0, 0) != FS_RETAIL_FF_WIRE_OK ||
        assets[0].type != 20 || assets[0].header != 0xffffffffu)
        return 92;
    FsRetailFastfileMenuList menuList;
    static uint32_t menuRefs[2];
    if (FS_ReadRetailFastfileMenuList(fastfile, assets[0].header, &menuList, menuRefs, 2) !=
        FS_RETAIL_FF_WIRE_OK)
        return 93;
    char name[32];
    if (menuList.menuCount != 2 || menuList.menusRef >> 28 != 4 ||
        menuRefs[0] != 0xffffffffu || menuRefs[1] != 0xffffffffu)
        return 94;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (menuList.nameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(name, "synthetic/menus.txt") != 0)
        return 95;

    FsRetailFastfileMenu menu;
    static FsRetailFastfileItemKeyHandler handlers[8];
    static FsRetailFastfileExpressionEntry entries[8];
    if (FS_ReadRetailFastfileMenuPrefix(fastfile, menuRefs[0], &menu, handlers, 8, entries, 8) !=
        FS_RETAIL_FF_WIRE_OK)
        return 96;
    if (menu.itemCount != 0 || menu.itemsRef != 0 || menu.fullScreen != 1 ||
        menu.rect[2] != 2.0f || menu.rect[3] != 3.0f || menu.handlerCount != 2 ||
        menu.visibleExp.numEntries != 2 || menu.expressionEntryCount != 2)
        return 97;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (menu.nameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(name, "synth_main") != 0)
        return 98;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (menu.fontRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(name, "synth_font") != 0)
        return 98;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (menu.onOpenRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(name, "synth_open") != 0)
        return 98;
    if (handlers[0].key != 1 || handlers[1].key != 2)
        return 99;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (handlers[0].actionRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(name, "synth_act1") != 0)
        return 99;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (handlers[1].actionRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(name, "synth_act2") != 0)
        return 99;
    if (entries[0].type != 1 || entries[0].dataType != 2 || entries[1].type != 0)
        return 100;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (entries[0].operandRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(name, "synth_str") != 0)
        return 100;

    FsRetailFastfileMenu second;
    if (FS_ReadRetailFastfileMenuPrefix(fastfile, menuRefs[1], &second, handlers, 8, entries, 8) !=
        FS_RETAIL_FF_WIRE_OK)
        return 101;
    if (second.itemCount != 2 || second.itemsRef >> 28 != 4)
        return 102;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (second.nameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(name), sizeof(name) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(name, "synth_second") != 0)
        return 102;
    uint32_t itemSlots[2] = {0, 0};
    if (FS_ReadRetailFastfileBlock(fastfile, 4, second.itemsOffset,
                                   reinterpret_cast<uint8_t *>(itemSlots), sizeof(itemSlots)) !=
        FS_RETAIL_FF_WIRE_OK)
        return 103;
    if (itemSlots[0] != 0xffffffffu || itemSlots[1] != 0)
        return 103;
    FS_CloseRetailFastfile(fastfile);

    // Capacity bounds fail loudly on a fresh walk.
    if (FS_OpenRetailFastfile("zone/english/menulist.ff", &fastfile) != FS_RETAIL_FF_OK ||
        FS_ReadRetailFastfileAssetList(fastfile, &list, assets, 1, 0, 0) != FS_RETAIL_FF_WIRE_OK ||
        FS_ReadRetailFastfileMenuList(fastfile, assets[0].header, &menuList, menuRefs, 2) !=
            FS_RETAIL_FF_WIRE_OK)
        return 104;
    if (FS_ReadRetailFastfileMenuPrefix(fastfile, menuRefs[0], &menu, handlers, 1, entries, 8) !=
        FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL)
        return 105;
    FS_CloseRetailFastfile(fastfile);
    if (FS_OpenRetailFastfile("zone/english/menulist.ff", &fastfile) != FS_RETAIL_FF_OK ||
        FS_ReadRetailFastfileAssetList(fastfile, &list, assets, 1, 0, 0) != FS_RETAIL_FF_WIRE_OK ||
        FS_ReadRetailFastfileMenuList(fastfile, assets[0].header, &menuList, menuRefs, 2) !=
            FS_RETAIL_FF_WIRE_OK)
        return 106;
    if (FS_ReadRetailFastfileMenuPrefix(fastfile, menuRefs[0], &menu, handlers, 8, entries, 1) !=
        FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL)
        return 107;
    FS_CloseRetailFastfile(fastfile);

    return 0;
}

// Real-root verification: walks ui.ff entries 0..10 plus the entry-11
// menulist through the production readers.  The menulist walk covers its
// directory (56 inline menus) and the first two menu bodies: main (no
// items) and main_text (75 items), stopping at main_text's items pointer
// array - item bodies are the next seam.
static int RetailUiClosureRun(const char *root)
{
    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = 0;
    static FsRetailFastfileAsset assets[35];
    FsRetailFastfileAssetList list;
    if (FS_OpenRetailFastfile("zone/english/ui.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile)
        return 80;
    if (FS_ReadRetailFastfileAssetList(fastfile, &list, assets, 35, 0, 0) != FS_RETAIL_FF_WIRE_OK ||
        list.assetCount != 35 || list.scriptStringCount != 0)
        return 81;
    uint32_t materials = 0;
    uint32_t techsets = 0;
    // Entries 0..10 are walked; entry 11 (the menulist) is reached
    // explicitly so the boundary condition is actually exercised.
    for (uint32_t index = 0; index <= 11; ++index)
    {
        if (assets[index].header != 0xffffffffu)
            return 82;
        if (assets[index].type == 5)
        {
            FsRetailFastfileTechniqueSet techset;
            if (FS_ReadRetailFastfileTechniqueSetPrefix(fastfile, assets[index].header, &techset) !=
                FS_RETAIL_FF_WIRE_OK)
                return 83;
            ++techsets;
            continue;
        }
        if (assets[index].type == 4)
        {
            FsRetailFastfileMaterial material;
            static FsRetailFastfileTextureDef textureDefs[8];
            if (FS_ReadRetailFastfileMaterial(fastfile, assets[index].header, &material,
                                              textureDefs, 8) != FS_RETAIL_FF_WIRE_OK)
                return 84;
            if (material.textureCount != 1 || material.textureTableRef >> 28 != 4)
                return 85;
            ++materials;
            continue;
        }
        // entry 11: the menulist - reached explicitly, then walked below
        if (index == 11 && assets[index].type == 20)
            break;
        return 86;
    }
    // entry 11 menulist (W5): name, 56 inline menu slots, menu[0] main and
    // menu[1] main_text prefixes; the walk stops at main_text's items.
    FsRetailFastfileMenuList menuList;
    static uint32_t menuRefs[56];
    if (FS_ReadRetailFastfileMenuList(fastfile, assets[11].header, &menuList, menuRefs, 56) !=
        FS_RETAIL_FF_WIRE_OK)
        return 88;
    char menuName[160];
    if (menuList.menuCount != 56 || menuList.menusRef >> 28 != 4)
        return 88;
    for (uint32_t m = 0; m < 56; ++m)
        if (menuRefs[m] != 0xffffffffu)
            return 89;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (menuList.nameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(menuName), sizeof(menuName) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(menuName, "ui/menus.txt") != 0)
        return 89;

    FsRetailFastfileMenu menu;
    static FsRetailFastfileItemKeyHandler menuHandlers[8];
    static FsRetailFastfileExpressionEntry menuEntries[64];
    if (FS_ReadRetailFastfileMenuPrefix(fastfile, menuRefs[0], &menu, menuHandlers, 8,
                                        menuEntries, 64) != FS_RETAIL_FF_WIRE_OK)
        return 90;
    if (menu.itemCount != 0 || menu.itemsRef != 0 || menu.fullScreen != 1 ||
        menu.rect[2] != 640.0f || menu.rect[3] != 480.0f)
        return 91;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (menu.nameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(menuName), sizeof(menuName) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(menuName, "main") != 0)
        return 91;
    char onOpen[512];
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (menu.onOpenRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(onOpen), sizeof(onOpen) - 1) !=
        FS_RETAIL_FF_WIRE_OK || !std::strstr(onOpen, "\"open\" \"main_text\""))
        return 92;

    if (FS_ReadRetailFastfileMenuPrefix(fastfile, menuRefs[1], &menu, menuHandlers, 8,
                                        menuEntries, 64) != FS_RETAIL_FF_WIRE_OK)
        return 93;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, (menu.nameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(menuName), sizeof(menuName) - 1) !=
        FS_RETAIL_FF_WIRE_OK || std::strcmp(menuName, "main_text") != 0)
        return 94;
    if (menu.itemCount != 75 || menu.itemsRef >> 28 != 4)
        return 94;
    uint32_t firstItemSlot = 0;
    if (FS_ReadRetailFastfileBlock(fastfile, 4, menu.itemsOffset,
                                   reinterpret_cast<uint8_t *>(&firstItemSlot), 4) !=
        FS_RETAIL_FF_WIRE_OK || firstItemSlot != 0xffffffffu)
        return 95;
    if (materials != 7 || techsets != 4)
        return 87;
    FS_CloseRetailFastfile(fastfile);
    g_retailUiClosureProof = true;
    return 0;
}

// Reusable fast diagnostic (no emulator or device build needed): dump every one of
// ui.ff's own top-level materials and their texture-table wire fields
// exactly as FS_ReadRetailFastfileMaterial/FS_ReadRetailFastfileImage decode
// them, straight off the real staged fastfile. Use this to compare against
// what the Switch-side live-load path (db_retail_decode_material.cpp)
// reports for the same material, without a device or emulator round trip.
static int RetailMaterialDumpRun(const char *root)
{
    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = 0;
    static FsRetailFastfileAsset assets[35];
    FsRetailFastfileAssetList list;
    if (FS_OpenRetailFastfile("zone/english/ui.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile)
    {
        std::fprintf(stderr, "FAIL: could not open zone/english/ui.ff under %s\n", root);
        return 80;
    }
    if (FS_ReadRetailFastfileAssetList(fastfile, &list, assets, 35, 0, 0) != FS_RETAIL_FF_WIRE_OK)
    {
        std::fprintf(stderr, "FAIL: FS_ReadRetailFastfileAssetList\n");
        FS_CloseRetailFastfile(fastfile);
        return 81;
    }
    std::printf("assetCount=%u scriptStringCount=%u\n", list.assetCount, list.scriptStringCount);
    for (uint32_t index = 0; index < list.assetCount; ++index)
    {
        if (assets[index].type != 4 || assets[index].header != 0xffffffffu)
            continue;
        FsRetailFastfileMaterial material;
        FsRetailFastfileTextureDef textureDefs[8] = {};
        const FsRetailFastfileWireResult matResult =
            FS_ReadRetailFastfileMaterial(fastfile, assets[index].header, &material, textureDefs, 8);
        if (matResult != FS_RETAIL_FF_WIRE_OK)
        {
            std::printf("entry[%u]: FS_ReadRetailFastfileMaterial FAILED result=%d\n", index, matResult);
            continue;
        }
        if (material.textureCount > 8)
        {
            std::printf("entry[%u]: textureCount=%u exceeds dump capacity (8), skipping texture dump\n",
                        index, material.textureCount);
            continue;
        }
        char matName[64] = {};
        FS_ReadRetailFastfileBlock(fastfile, 4, (material.nameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(matName), sizeof(matName) - 1);
        std::printf("entry[%u]: material='%s' textureCount=%u constantCount=%u stateBitsCount=%u\n",
                    index, matName, material.textureCount, material.constantCount, material.stateBitsCount);
        for (uint32_t t = 0; t < material.textureCount; ++t)
        {
            const FsRetailFastfileTextureDef &def = textureDefs[t];
            std::printf("  tex[%u]: nameHash=0x%08x samplerState=%u semantic=%u imageRef=0x%08x "
                        "imageWasInline=%u\n",
                        t, def.nameHash, def.samplerState, def.semantic, def.imageRef, def.imageWasInline);
            if (def.imageWasInline)
            {
                const FsRetailFastfileImage &img = def.inlineImage;
                char imgName[64] = {};
                FS_ReadRetailFastfileBlock(fastfile, 4, (img.nameRef - 1) & 0x0fffffffu,
                                           reinterpret_cast<uint8_t *>(imgName), sizeof(imgName) - 1);
                std::printf("    inlineImage: nameRef=0x%08x name='%s' mapType=%u wh=%ux%u depth=%u "
                            "category=%u delayLoad=%u haveLoadDef=%u loadDefDims=%ux%ux%u fmt=%u "
                            "resourceSize=%u textureRef=0x%08x textureInsertOffset=%u\n",
                            img.nameRef, imgName, img.mapType, img.width, img.height, img.depth,
                            img.category, img.delayLoadPixels, img.haveLoadDef, img.loadDefDimensions[0],
                            img.loadDefDimensions[1], img.loadDefDimensions[2], img.loadDefFormat,
                            img.loadDefResourceSize, img.textureRef, img.textureInsertOffset);
            }
        }
    }
    FS_CloseRetailFastfile(fastfile);
    return 0;
}

// Reusable fast diagnostic: walk main_text's 75 items via the already-correct
// sequential ItemDef reader (FS_ReadRetailFastfileItemDef already handles
// per-item INLINE materials directly -- see its backgroundWasInline
// handling), dumping each item's name and, for inline backgrounds, the
// embedded material's texture-table wire fields. This reaches territory no
// existing proof walks (RetailUiClosureRun stops right after confirming
// main_text's item array header).
static void DumpInlineMaterialTextures(FsRetailFastfileReader *fastfile,
                                       const FsRetailFastfileMaterial &material,
                                       const FsRetailFastfileTextureDef *textureDefs,
                                       const char *indent)
{
    if (material.textureCount > 8)
    {
        std::printf("%stextureCount=%u exceeds capacity (8)\n", indent, material.textureCount);
        return;
    }
    for (uint32_t t = 0; t < material.textureCount; ++t)
    {
        const FsRetailFastfileTextureDef &def = textureDefs[t];
        std::printf("%stex[%u]: imageRef=0x%08x imageWasInline=%u\n", indent, t, def.imageRef,
                    def.imageWasInline);
        if (def.imageWasInline)
        {
            const FsRetailFastfileImage &img = def.inlineImage;
            char imgName[64] = {};
            FS_ReadRetailFastfileBlock(fastfile, 4, (img.nameRef - 1) & 0x0fffffffu,
                                       reinterpret_cast<uint8_t *>(imgName), sizeof(imgName) - 1);
            std::printf("%s  inlineImage: name='%s' nameRef=0x%08x nameWasInline=%u wh=%ux%u fmt=%u "
                        "resourceSize=%u textureRef=0x%08x\n",
                        indent, imgName, img.nameRef, img.nameWasInline, img.width, img.height,
                        img.loadDefFormat, img.loadDefResourceSize, img.textureRef);
            if (!img.nameWasInline)
            {
                // nameWasInline=0 means the serialized name slot is a block-4
                // reference into the *linker's* block-4 image.  Our reader only
                // mirrors the block-4 bytes it walks, so that offset resolves
                // against different bytes (usually unwritten -> empty name).
                // See RetailWidenImageFromWire's material-name fallback.
                std::printf("%s  (name is an unresolvable block-4 reference; "
                            "materialNameRef=0x%08x)\n", indent, material.nameRef);
            }
        }
    }
}

static int RetailUiItemsRun(const char *root)
{
    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = 0;
    static FsRetailFastfileAsset assets[35];
    FsRetailFastfileAssetList list;
    if (FS_OpenRetailFastfile("zone/english/ui.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile)
    {
        std::fprintf(stderr, "FAIL: could not open zone/english/ui.ff under %s\n", root);
        return 80;
    }
    if (FS_ReadRetailFastfileAssetList(fastfile, &list, assets, 35, 0, 0) != FS_RETAIL_FF_WIRE_OK)
        return 81;
    // Sequentially consume entries 0..10 exactly like RetailUiClosureRun so
    // the stream cursor is correctly positioned at entry 11's MenuList body.
    for (uint32_t index = 0; index <= 10; ++index)
    {
        if (assets[index].header != 0xffffffffu)
        {
            std::fprintf(stderr, "FAIL: entry[%u] header not inline\n", index);
            return 82;
        }
        if (assets[index].type == 5)
        {
            FsRetailFastfileTechniqueSet techset;
            if (FS_ReadRetailFastfileTechniqueSetPrefix(fastfile, assets[index].header, &techset) !=
                FS_RETAIL_FF_WIRE_OK)
                return 83;
        }
        else if (assets[index].type == 4)
        {
            FsRetailFastfileMaterial material;
            static FsRetailFastfileTextureDef textureDefs[8];
            if (FS_ReadRetailFastfileMaterial(fastfile, assets[index].header, &material, textureDefs, 8) !=
                FS_RETAIL_FF_WIRE_OK)
                return 84;
        }
        else
        {
            std::fprintf(stderr, "FAIL: entry[%u] unexpected type=%u\n", index, assets[index].type);
            return 86;
        }
    }
    FsRetailFastfileMenuList menuList;
    static uint32_t menuRefs[56];
    if (FS_ReadRetailFastfileMenuList(fastfile, assets[11].header, &menuList, menuRefs, 56) !=
        FS_RETAIL_FF_WIRE_OK)
        return 88;
    FsRetailFastfileMenu menu;
    static FsRetailFastfileItemKeyHandler menuHandlers[8];
    static FsRetailFastfileExpressionEntry menuEntries[64];
    // menu[0]="main" (no items); menu[1]="main_text" (75 items) per
    // RetailUiClosureRun's already-proven layout for this pinned ui.ff.
    if (FS_ReadRetailFastfileMenuPrefix(fastfile, menuRefs[0], &menu, menuHandlers, 8, menuEntries, 64) !=
        FS_RETAIL_FF_WIRE_OK)
        return 90;
    if (FS_ReadRetailFastfileMenuPrefix(fastfile, menuRefs[1], &menu, menuHandlers, 8, menuEntries, 64) !=
        FS_RETAIL_FF_WIRE_OK)
        return 93;
    std::printf("main_text: itemCount=%d itemsOffset=%u\n", menu.itemCount, menu.itemsOffset);
    for (int32_t i = 0; i < menu.itemCount; ++i)
    {
        uint32_t itemSlot = 0;
        if (FS_ReadRetailFastfileBlock(fastfile, 4, menu.itemsOffset + i * 4u,
                                       reinterpret_cast<uint8_t *>(&itemSlot), 4) != FS_RETAIL_FF_WIRE_OK)
        {
            std::printf("item[%d]: FAILED to read item slot\n", i);
            return 96;
        }
        FsRetailFastfileItemDef item;
        static FsRetailFastfileItemKeyHandler itemHandlers[8];
        static FsRetailFastfileExpressionEntry itemEntries[64];
        const FsRetailFastfileWireResult itemResult = FS_ReadRetailFastfileItemDef(
            fastfile, itemSlot, &item, itemHandlers, 8, itemEntries, 64);
        if (itemResult != FS_RETAIL_FF_WIRE_OK)
        {
            std::printf("item[%d]: FS_ReadRetailFastfileItemDef FAILED result=%d\n", i, itemResult);
            return 97;
        }
        char itemName[64] = {};
        FS_ReadRetailFastfileBlock(fastfile, 4, (item.nameRef - 1) & 0x0fffffffu,
                                   reinterpret_cast<uint8_t *>(itemName), sizeof(itemName) - 1);
        if (!item.backgroundWasInline && !item.backgroundRef)
        {
            std::printf("item[%d]: name='%s' type=%d style=%d rect=[%.1f, %.1f, %.1f, %.1f] fore=[%.2f,%.2f,%.2f,%.2f] back=[%.2f,%.2f,%.2f,%.2f] background=none\n",
                        i, itemName, item.type, item.style, item.rect[0], item.rect[1], item.rect[2], item.rect[3],
                        item.foreColor[0], item.foreColor[1], item.foreColor[2], item.foreColor[3],
                        item.backColor[0], item.backColor[1], item.backColor[2], item.backColor[3]);
        }
        else if (item.backgroundWasInline)
        {
            char matName[64] = {};
            FS_ReadRetailFastfileBlock(fastfile, 4, (item.backgroundInlineMaterial.nameRef - 1) & 0x0fffffffu,
                                       reinterpret_cast<uint8_t *>(matName), sizeof(matName) - 1);
            std::printf("item[%d]: name='%s' type=%d style=%d rect=[%.1f, %.1f, %.1f, %.1f] fore=[%.2f,%.2f,%.2f,%.2f] back=[%.2f,%.2f,%.2f,%.2f] background=INLINE material='%s'\n",
                        i, itemName, item.type, item.style, item.rect[0], item.rect[1], item.rect[2], item.rect[3],
                        item.foreColor[0], item.foreColor[1], item.foreColor[2], item.foreColor[3],
                        item.backColor[0], item.backColor[1], item.backColor[2], item.backColor[3], matName);
        }
        else if (item.backgroundRef)
        {
            char bgName[64] = {};
            const bool named = FS_ReadRetailFastfileBlock(
                fastfile, 4, (item.backgroundRef - 1) & 0x0fffffffu,
                reinterpret_cast<uint8_t *>(bgName), sizeof(bgName) - 1) == FS_RETAIL_FF_WIRE_OK;
            std::printf("item[%d]: name='%s' type=%d style=%d rect=[%.1f, %.1f, %.1f, %.1f] fore=[%.2f,%.2f,%.2f,%.2f] back=[%.2f,%.2f,%.2f,%.2f] background=REF 0x%08x name='%s'\n",
                        i, itemName, item.type, item.style, item.rect[0], item.rect[1], item.rect[2], item.rect[3],
                        item.foreColor[0], item.foreColor[1], item.foreColor[2], item.foreColor[3],
                        item.backColor[0], item.backColor[1], item.backColor[2], item.backColor[3],
                        item.backgroundRef, named ? bgName : "?");
        }
        std::printf("  place: halign=%d valign=%d textAlignMode=%d textalign=[%.2f, %.2f] textscale=%.3f font=%d alignment=%d staticFlags=0x%x border=%d borderSize=%.1f\n",
                    item.rectHorzAlign, item.rectVertAlign, item.textAlignMode, item.textalignx, item.textaligny,
                    item.textscale, item.fontEnum, item.alignment, item.staticFlags, item.border, item.borderSize);
        for (int s = 0; s < 8; ++s)
        {
            if (item.statements[s].numEntries)
            {
                std::printf("  stmt[%d]: numEntries=%d ref=0x%x\n", s, item.statements[s].numEntries, item.statements[s].entriesRef);
            }
        }
        if (item.expressionEntryCount)
        {
            std::printf("  total entries=%d\n", item.expressionEntryCount);
            for (uint32_t e = 0; e < item.expressionEntryCount; ++e)
            {
                if (itemEntries[e].type == 0)
                    std::printf("    [%u] OP %d\n", e, itemEntries[e].dataType);
                else if (itemEntries[e].dataType == 2)
                {
                    char strVal[64] = {};
                    uint32_t off = (itemEntries[e].operandRef - 1) & 0x0fffffffu;
                    if (itemEntries[e].operandRef)
                        FS_ReadRetailFastfileBlock(fastfile, 4, off,
                                                   reinterpret_cast<uint8_t *>(strVal), sizeof(strVal) - 1);
                    std::printf("    [%u] VAL_STRING ref=0x%x (off=0x%x) '%s'\n", e, itemEntries[e].operandRef, off, strVal);
                    if (off == 0x2584 || off == 0x2200)
                    {
                        uint8_t dump[32] = {};
                        FS_ReadRetailFastfileBlock(fastfile, 4, off, dump, sizeof(dump));
                        std::printf("      hex @ 0x%x: ", off);
                        for (int d = 0; d < 16; ++d) std::printf("%02x ", dump[d]);
                        std::printf("  ascii: '%.16s'\n", dump);
                    }
                }
                else
                    std::printf("    [%u] VAL_%d val=%d\n", e, itemEntries[e].dataType, itemEntries[e].operandRef);
            }
        }
    }
    uint32_t val262c = 0, val2808 = 0;
    FS_ReadRetailFastfileBlock(fastfile, 4, 0x262c, reinterpret_cast<uint8_t *>(&val262c), 4);
    FS_ReadRetailFastfileBlock(fastfile, 4, 0x2808, reinterpret_cast<uint8_t *>(&val2808), 4);
    char name262c[64] = {}, name2808[64] = {};
    if (val262c)
        FS_ReadRetailFastfileBlock(fastfile, 4, (val262c - 1) & 0x0fffffffu, reinterpret_cast<uint8_t *>(name262c), sizeof(name262c) - 1);
    if (val2808)
        FS_ReadRetailFastfileBlock(fastfile, 4, (val2808 - 1) & 0x0fffffffu, reinterpret_cast<uint8_t *>(name2808), sizeof(name2808) - 1);
    std::printf("SLOT 0x262c: val=0x%x name='%s'\n", val262c, name262c);
    std::printf("SLOT 0x2808: val=0x%x name='%s'\n", val2808, name2808);
    FS_CloseRetailFastfile(fastfile);
    return 0;
}

// Killhouse container/directory census: facts available before any body
// parsing -- nine declared block sizes, direct XAsset count/order/type/
// header class, and per-type counts across all 33 IDs. Consumes no bodies,
// registers nothing; nested closure and final body cursors stay unknown.
static int RetailZoneCensusRun(const char *root, const char *zoneRel, const char *tag)
{
    static const char *kTypeNames[33] = {
        "xmodelpieces", "physpreset", "xanim", "xmodel", "material", "techset",
        "image", "sound", "sndcurve", "loaded_sound", "col_map_sp", "col_map_mp",
        "com_map", "game_map_sp", "game_map_mp", "map_ents", "gfx_map", "lightdef",
        "ui_map", "font", "menufile", "menu", "localize", "weapon",
        "snddriverglobals", "fx", "impactfx", "aitype", "mptype", "character",
        "xmodelalias", "rawfile", "stringtable",
    };
    if (!zoneRel || !zoneRel[0] || !tag || !tag[0])
        return 84;
    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = 0;
    if (FS_OpenRetailFastfile(zoneRel, &fastfile) != FS_RETAIL_FF_OK || !fastfile)
    {
        std::fprintf(stderr, "FAIL: could not open %s under %s\n", zoneRel, root);
        return 80;
    }
    FsRetailFastfileAssetList list;
    if (FS_ReadRetailFastfileAssetList(fastfile, &list, 0, 0, 0, 0) != FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL ||
        !list.assetCount || list.assetCount > (1u << 20) || list.scriptStringCount > (1u << 20))
    {
        std::fprintf(stderr, "FAIL: %s directory preflight\n", zoneRel);
        FS_CloseRetailFastfile(fastfile);
        return 81;
    }
    const uint32_t assetCount = list.assetCount;
    const uint32_t scriptStringCount = list.scriptStringCount;
    // The reader is a forward-only zlib stream: the preflight consumed the
    // list prefix, so reopen before the full read (same pattern as
    // RetailDirectoryRun).
    FS_CloseRetailFastfile(fastfile);
    fastfile = 0;
    if (FS_OpenRetailFastfile(zoneRel, &fastfile) != FS_RETAIL_FF_OK || !fastfile)
    {
        std::fprintf(stderr, "FAIL: could not reopen %s under %s\n", zoneRel, root);
        return 81;
    }
    std::vector<FsRetailFastfileAsset> assets(assetCount);
    std::vector<uint32_t> scriptRefs(scriptStringCount);
    const FsRetailFastfileWireResult result = FS_ReadRetailFastfileAssetList(
        fastfile, &list, assets.data(), static_cast<uint32_t>(assets.size()),
        scriptRefs.empty() ? 0 : scriptRefs.data(), static_cast<uint32_t>(scriptRefs.size()));
    if (result != FS_RETAIL_FF_WIRE_OK || list.decodedCount != assets.size())
    {
        std::fprintf(stderr, "FAIL: %s directory read result=%d decoded=%u expected=%u\n",
                     zoneRel, result, list.decodedCount, static_cast<uint32_t>(assets.size()));
        FS_CloseRetailFastfile(fastfile);
        return 82;
    }
    uint32_t perType[33] = {};
    uint32_t nInline = 0, nInsert = 0, nNull = 0, nAlias = 0;
    uint64_t orderHash = 0xcbf29ce484222325ull;
    for (const FsRetailFastfileAsset &asset : assets)
    {
        if (asset.type >= 33)
        {
            std::fprintf(stderr, "FAIL: %s directory entry type=%u out of range\n", zoneRel, asset.type);
            FS_CloseRetailFastfile(fastfile);
            return 83;
        }
        ++perType[asset.type];
        uint64_t cls;
        if (asset.header == 0xffffffffu) { ++nInline; cls = 0; }
        else if (asset.header == 0xfffffffeu) { ++nInsert; cls = 1; }
        else if (asset.header == 0u) { ++nNull; cls = 2; }
        else { ++nAlias; cls = 3; }
        orderHash ^= asset.type; orderHash *= 0x100000001b3ull;
        orderHash ^= cls; orderHash *= 0x100000001b3ull;
    }
    std::printf("%s ff=%s\n", tag, zoneRel);
    std::printf("%s scriptStrings=%u assets=%u decoded=%u\n", tag,
                list.scriptStringCount, list.assetCount, list.decodedCount);
    std::printf("%s blocks=9", tag);
    for (uint32_t b = 0; b < 9; ++b)
        std::printf(" b%u=%u", b, FS_RetailFastfileBlockSize(fastfile, b));
    std::printf("\n");
    std::printf("%s headerClass inline=%u insert=%u null=%u alias=%u\n", tag,
                nInline, nInsert, nNull, nAlias);
    for (uint32_t t = 0; t < 33; ++t)
        std::printf("%s type id=%u name=%s count=%u\n", tag, t, kTypeNames[t], perType[t]);
    std::printf("%s orderHash=0x%016llx\n", tag, (unsigned long long)orderHash);
    std::printf("%s nested=unknown cursors=unknown\n", tag);
    FS_CloseRetailFastfile(fastfile);
    return 0;
}

static int RetailKillhouseCensusRun(const char *root)
{
    return RetailZoneCensusRun(root, "zone/english/killhouse.ff", "KILLHOUSE_CENSUS");
}

// M9a load-order analysis: dump every directory ordinal as `ord type
// header-class` in serialized order using the production directory reader.
// Bodies are never consumed; nothing is registered. The bounded loader
// design (which world-closure types precede/follow the GfxWorld root,
// where the direct lightdef sits, how techsets interleave) reads this,
// never guesses it.
static int RetailZoneOrderRun(const char *root, const char *zoneRel)
{
    if (!zoneRel || !zoneRel[0])
        return 84;
    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = 0;
    if (FS_OpenRetailFastfile(zoneRel, &fastfile) != FS_RETAIL_FF_OK || !fastfile)
    {
        std::fprintf(stderr, "FAIL: could not open %s under %s\n", zoneRel, root);
        return 80;
    }
    FsRetailFastfileAssetList list;
    if (FS_ReadRetailFastfileAssetList(fastfile, &list, 0, 0, 0, 0) != FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL ||
        !list.assetCount || list.assetCount > (1u << 20) || list.scriptStringCount > (1u << 20))
    {
        std::fprintf(stderr, "FAIL: %s directory preflight\n", zoneRel);
        FS_CloseRetailFastfile(fastfile);
        return 81;
    }
    const uint32_t assetCount = list.assetCount;
    const uint32_t scriptStringCount = list.scriptStringCount;
    FS_CloseRetailFastfile(fastfile);
    fastfile = 0;
    if (FS_OpenRetailFastfile(zoneRel, &fastfile) != FS_RETAIL_FF_OK || !fastfile)
    {
        std::fprintf(stderr, "FAIL: could not reopen %s under %s\n", zoneRel, root);
        return 81;
    }
    std::vector<FsRetailFastfileAsset> assets(assetCount);
    std::vector<uint32_t> scriptRefs(scriptStringCount);
    const FsRetailFastfileWireResult result = FS_ReadRetailFastfileAssetList(
        fastfile, &list, assets.data(), static_cast<uint32_t>(assets.size()),
        scriptRefs.empty() ? 0 : scriptRefs.data(), static_cast<uint32_t>(scriptRefs.size()));
    if (result != FS_RETAIL_FF_WIRE_OK || list.decodedCount != assets.size())
    {
        std::fprintf(stderr, "FAIL: %s directory read\n", zoneRel);
        FS_CloseRetailFastfile(fastfile);
        return 82;
    }
    for (uint32_t i = 0; i < assetCount; ++i)
    {
        const char *cls;
        if (assets[i].header == 0xffffffffu) cls = "inline";
        else if (assets[i].header == 0xfffffffeu) cls = "insert";
        else if (assets[i].header == 0u) cls = "null";
        else cls = "alias";
        std::printf("ZONE_ORDER ord=%u type=%u class=%s\n", i, assets[i].type, cls);
    }
    FS_CloseRetailFastfile(fastfile);
    return 0;
}

static int RetailDirectoryRun(const char *root)
{
    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = 0;
    FsRetailFastfileAssetList list;
    if (FS_OpenRetailFastfile("zone/english/code_post_gfx.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile)
        return 30;
    const FsRetailFastfileWireResult preflight =
        FS_ReadRetailFastfileAssetList(fastfile, &list, 0, 0, 0, 0);
    FS_CloseRetailFastfile(fastfile);
    if (preflight != FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL || !list.assetCount ||
        list.assetCount > (1u << 20) || list.scriptStringCount > (1u << 20))
        return 31;

    std::vector<FsRetailFastfileAsset> assets(list.assetCount);
    std::vector<uint32_t> scriptRefs(list.scriptStringCount);
    if (FS_OpenRetailFastfile("zone/english/code_post_gfx.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile)
        return 32;
    const FsRetailFastfileWireResult result = FS_ReadRetailFastfileAssetList(
        fastfile, &list, assets.data(), static_cast<uint32_t>(assets.size()),
        scriptRefs.empty() ? 0 : scriptRefs.data(), static_cast<uint32_t>(scriptRefs.size()));
    FS_CloseRetailFastfile(fastfile);
    if (result != FS_RETAIL_FF_WIRE_OK || list.decodedCount != assets.size())
        return 33;
    uint32_t rawFileCount = 0;
    for (const FsRetailFastfileAsset &asset : assets)
        if (asset.type == 31)
            ++rawFileCount;
    if (rawFileCount != 76)
        return 34;
    g_retailRawFileCount = rawFileCount;
    g_retailDirectoryProof = true;
    return 0;
}

static int RetailTechniquePrefixRun(const char *root)
{
    FS_InitRetailSource(root);
    FsRetailFastfileReader *fastfile = 0;
    FsRetailFastfileAssetList list;
    if (FS_OpenRetailFastfile("zone/english/code_post_gfx.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile)
        return 40;
    const FsRetailFastfileWireResult preflight = FS_ReadRetailFastfileAssetList(fastfile, &list, 0, 0, 0, 0);
    FS_CloseRetailFastfile(fastfile);
    if (preflight != FS_RETAIL_FF_WIRE_OUTPUT_TOO_SMALL || !list.assetCount || list.assetCount > (1u << 20) ||
        list.scriptStringCount > (1u << 20))
        return 41;
    std::vector<FsRetailFastfileAsset> assets(list.assetCount);
    std::vector<uint32_t> scriptRefs(list.scriptStringCount);
    if (FS_OpenRetailFastfile("zone/english/code_post_gfx.ff", &fastfile) != FS_RETAIL_FF_OK || !fastfile)
        return 42;
    const FsRetailFastfileWireResult result = FS_ReadRetailFastfileAssetList(
        fastfile, &list, assets.data(), static_cast<uint32_t>(assets.size()),
        scriptRefs.empty() ? 0 : scriptRefs.data(), static_cast<uint32_t>(scriptRefs.size()));
    if (result != FS_RETAIL_FF_WIRE_OK || assets[0].type != 5 || assets[0].header != 0xffffffffu)
    {
        FS_CloseRetailFastfile(fastfile);
        return 43;
    }
    FsRetailFastfileTechniqueSet techniqueSet;
    const FsRetailFastfileWireResult prefixResult =
        FS_ReadRetailFastfileTechniqueSetPrefix(fastfile, assets[0].header, &techniqueSet);
    char name[7];
    const FsRetailFastfileWireResult nameResult = FS_ReadRetailFastfileBlock(
        fastfile, 4, (techniqueSet.nameRef - 1) & 0x0fffffffu, reinterpret_cast<uint8_t *>(name), sizeof(name));
    uint32_t inlineTechniqueRef = 0;
    for (uint32_t ref : techniqueSet.techniqueRefs)
        if (ref == 0xffffffffu)
        {
            inlineTechniqueRef = ref;
            break;
        }
    FsRetailFastfileMaterialTechnique technique;
    const FsRetailFastfileWireResult techniqueResult =
        FS_ReadRetailFastfileMaterialTechniquePrefix(fastfile, inlineTechniqueRef, &technique);
    uint8_t firstPass[20] = {};
    const FsRetailFastfileWireResult passResult = FS_ReadRetailFastfileBlock(
        fastfile, 4, technique.passOffset, firstPass, sizeof(firstPass));
    FS_CloseRetailFastfile(fastfile);
    if (prefixResult != FS_RETAIL_FF_WIRE_OK || techniqueSet.nameRef >> 28 != 4 ||
        nameResult != FS_RETAIL_FF_WIRE_OK || std::strcmp(name, "sm2/2d") != 0)
        return 44;
    if (!inlineTechniqueRef || techniqueResult != FS_RETAIL_FF_WIRE_OK || !technique.passCount ||
        passResult != FS_RETAIL_FF_WIRE_OK)
        return 45;
    g_retailTechniqueProof = true;
    return 0;
}

int main(int argc, char **argv)
{
    const int result = argc == 3 && std::strcmp(argv[1], "--retail-directory") == 0
        ? RetailDirectoryRun(argv[2]) : argc == 3 && std::strcmp(argv[1], "--retail-technique-prefix") == 0
        ? RetailTechniquePrefixRun(argv[2])
        : argc == 3 && std::strcmp(argv[1], "--retail-material-image") == 0
        ? RetailMaterialImageRun(argv[2])
        : argc == 3 && std::strcmp(argv[1], "--retail-menulist") == 0
        ? RetailMenuListRun(argv[2])
        : argc == 3 && std::strcmp(argv[1], "--retail-itemdef") == 0
        ? RetailItemDefRun(argv[2])
        : argc == 3 && std::strcmp(argv[1], "--retail-wire-regressions") == 0
        ? RetailWireRegressionRun(argv[2])
        : argc == 3 && std::strcmp(argv[1], "--retail-ui-closure") == 0
        ? RetailUiClosureRun(argv[2])
        : argc == 3 && std::strcmp(argv[1], "--retail-material-dump") == 0
        ? RetailMaterialDumpRun(argv[2])
        : argc == 3 && std::strcmp(argv[1], "--retail-ui-items") == 0
        ? RetailUiItemsRun(argv[2])
        : argc == 3 && std::strcmp(argv[1], "--retail-killhouse-census") == 0
        ? RetailKillhouseCensusRun(argv[2])
        : argc == 4 && std::strcmp(argv[1], "--retail-zone-census") == 0
        ? RetailZoneCensusRun(argv[2], argv[3], "ZONE_CENSUS")
        : argc == 4 && std::strcmp(argv[1], "--retail-zone-order") == 0
        ? RetailZoneOrderRun(argv[2], argv[3])
        : RetailFsRun(argc, argv);
#if defined(__SWITCH__) && !defined(KISAK_RETAIL_FS_PROOF_HOST)
    consoleInit(NULL);
    if (!result)
        std::printf(g_retailTechniqueProof ? "PASS:RETAIL_TECHNIQUE_PREFIX_PROOF sm2/2d\n" :
            g_retailDirectoryProof ? "PASS:RETAIL_FF_DIRECTORY_PROOF rawfile=%u\n" :
            "PASS:RETAIL_FS_SOURCE_PROOF\n", g_retailRawFileCount);
    else
        std::printf("FAIL:RETAIL_FS_SOURCE_PROOF:%d\n", result);
    std::printf("Synthetic fixture only. Press + to return.\n");
    consoleUpdate(NULL);
    PadState pad;
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);
    while (appletMainLoop())
    {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        svcSleepThread(100000000);
    }
    consoleExit(NULL);
#endif
    return result;
}
