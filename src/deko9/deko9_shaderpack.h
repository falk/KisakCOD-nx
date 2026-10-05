#pragma once

// deko3d renderer shader pipeline pack: on a cache hit, creating a shader
// needs no MojoShader translation, no per-variant SD `fopen`, and no SD I/O
// under the device lock -- the old per-file `v<N>-<hash>-<mask>[...].dksh`
// scheme did all three while holding the lock the loading
// screen present also takes.
//
// One file per cache version holds every compiled DKSH variant: a header
// (version + translator pins), a sorted index of
// {ShaderVariantKey -> offset, size, checksum} and a records blob of
// {Deko9ShaderInfo, DKSH bytes}. A caller reads the whole file once
// (outside any lock) and calls Load(); a hit is a pure in-memory lookup
// (Find()) that needs no translation and no file I/O. A miss is handled by
// the caller as before (translate + compile) and handed back with
// Append(), which only grows an in-memory delta; Serialize() folds the
// delta into the pack and returns the bytes to write back -- once per
// flush (map load end), never per shader.
//
// Any inconsistency (short file, bad magic, version/pin mismatch, a
// record's checksum not matching its bytes, an index entry pointing
// outside the records blob, or an unsorted index) fails Load() loudly
// (returns false with *error set) instead of using the bad data: the
// caller then starts from an empty pack -- every shader retranslates this
// boot, exactly like a cold cache -- rather than risking a corrupt or
// stale DKSH reaching the GPU. This is the whole point of the invariant:
// corruption or a version drift must never be silently misused.

#include "deko9_shader.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace deko9
{

// Bump whenever the translator output, its preludes, or the MojoShader/UAM
// pins change: a pack from another version (or built by another pin) must
// never be loaded. Kept in lockstep with deko9_resources.cpp's
// kCacheVersion.
constexpr uint32_t kShaderPackVersion = 6;
// Free-form identifiers for the vendored translator/compiler. There is no
// upstream version macro to key off, so these are bumped by hand alongside
// kShaderPackVersion whenever MojoShader or UAM is upgraded; a stale pack
// then fails the pin check above translator output silently drifting.
constexpr char kShaderPackMojoPin[] = "mojoshader-1";
constexpr char kShaderPackUamPin[] = "uam-1";

// Identifies one compiled variant of one bytecode program: ShaderBase::Hash
// plus every ShaderVariant selector in deko9_internal.h that changes the
// translated GLSL (deko9_shader.h's Deko9_TranslateShader parameters).
struct ShaderVariantKey
{
    uint64_t bytecodeHash = 0;
    uint32_t shadowMask = 0;
    uint64_t instanceHash = 0;       // InstanceLayout::Hash(); 0 = not instanced
    uint32_t shadowFilter = 0;       // 0 = retail translation
    uint32_t shadowFilterVersion = 0; // DEKO9_SHADOW_FILTER_VERSION when shadowFilter != 0, else 0
    uint32_t shaderOpt = 0;           // r_deko9ShaderOpt bits
    uint32_t shaderOptVersion = 0;    // DEKO9_SHADER_OPT_VERSION when shaderOpt != 0, else 0
    uint8_t earlyZ = 0;

    bool operator==(const ShaderVariantKey &o) const
    {
        return bytecodeHash == o.bytecodeHash && shadowMask == o.shadowMask &&
               instanceHash == o.instanceHash && shadowFilter == o.shadowFilter &&
               shadowFilterVersion == o.shadowFilterVersion && shaderOpt == o.shaderOpt &&
               shaderOptVersion == o.shaderOptVersion && earlyZ == o.earlyZ;
    }
    bool operator<(const ShaderVariantKey &o) const
    {
        if (bytecodeHash != o.bytecodeHash)
            return bytecodeHash < o.bytecodeHash;
        if (shadowMask != o.shadowMask)
            return shadowMask < o.shadowMask;
        if (instanceHash != o.instanceHash)
            return instanceHash < o.instanceHash;
        if (shadowFilter != o.shadowFilter)
            return shadowFilter < o.shadowFilter;
        if (shadowFilterVersion != o.shadowFilterVersion)
            return shadowFilterVersion < o.shadowFilterVersion;
        if (shaderOpt != o.shaderOpt)
            return shaderOpt < o.shaderOpt;
        if (shaderOptVersion != o.shaderOptVersion)
            return shaderOptVersion < o.shaderOptVersion;
        return earlyZ < o.earlyZ;
    }
};

// One resolved record: the translated shader's declared interface plus its
// compiled DKSH, exactly what ShaderBase/ShaderVariant need to skip
// translation and compilation on a hit.
struct ShaderPackRecord
{
    ShaderVariantKey key;
    Deko9ShaderInfo info{};
    std::vector<uint8_t> dksh;
};

namespace shaderpack_detail
{
constexpr uint32_t kMagic = 0x39304b44u; // "DK09" little-endian

// Deliberately not packed: every read of these goes through memcpy into a
// properly-aligned local (never a reinterpret_cast over the raw file
// buffer, whose base is not guaranteed aligned for a uint64_t member), so
// natural alignment costs nothing and keeps AArch64 (strict alignment)
// honest. The writer and reader are always the same binary, so padding
// bytes (whatever they hold) round-trip identically either way.
struct FileHeader
{
    uint32_t magic;
    uint32_t version;
    char mojoPin[16];
    char uamPin[16];
    uint32_t recordCount;
};
struct FileIndexEntry
{
    ShaderVariantKey key;
    uint64_t offset;   // from the start of the records blob
    uint32_t size;     // record payload bytes (info + dkshSize + dksh)
    uint32_t checksum; // FNV-1a over the record payload bytes
};

inline uint32_t Fnv1a(const void *data, size_t size)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint32_t h = 0x811c9dc5u;
    for (size_t i = 0; i < size; ++i)
    {
        h ^= p[i];
        h *= 0x01000193u;
    }
    return h;
}

inline void SetPin(char (&field)[16], const char *pin)
{
    std::memset(field, 0, sizeof(field));
    std::strncpy(field, pin, sizeof(field) - 1);
}
} // namespace shaderpack_detail

class ShaderPack
{
public:
    explicit ShaderPack(uint32_t version = kShaderPackVersion, const char *mojoPin = kShaderPackMojoPin,
                        const char *uamPin = kShaderPackUamPin)
        : m_version(version)
    {
        shaderpack_detail::SetPin(m_mojoPin, mojoPin);
        shaderpack_detail::SetPin(m_uamPin, uamPin);
    }

    // Parses a whole pack file already read into memory. An empty buffer is
    // a legitimate cold start (returns true, an empty pack). Anything else
    // that fails a structural check returns false with *error set and
    // leaves the pack empty: the caller must treat that as "no cache" (a
    // full rebuild this boot), never as partially trusted data.
    bool Load(const std::vector<uint8_t> &file, std::string *error)
    {
        // Parsed into a local vector and only swapped into m_records on
        // full success: a truncated/corrupt/unsorted file must leave the
        // pack exactly as empty as it started, never partially populated.
        m_records.clear();
        m_pending.clear();
        if (file.empty())
            return true;
        std::vector<ShaderPackRecord> records;
        if (!ParseFile(file, &records, error))
            return false;
        m_records = std::move(records);
        return true;
    }

    // Pure in-memory lookup: no translation, no file I/O. Checks the
    // loaded records first, then the not-yet-flushed delta (Append()),
    // so a hit is exact even before the next flush.
    const ShaderPackRecord *Find(const ShaderVariantKey &key) const
    {
        auto it = std::lower_bound(m_records.begin(), m_records.end(), key,
                                   [](const ShaderPackRecord &r, const ShaderVariantKey &k) { return r.key < k; });
        if (it != m_records.end() && it->key == key)
            return &*it;
        for (const auto &p : m_pending)
            if (p.key == key)
                return &p;
        return nullptr;
    }

    // Adds a freshly compiled variant to the in-memory delta. A repeat of
    // an already-known key (loaded or already pending) is ignored, so a
    // second miss for the same key never duplicates the record.
    void Append(const ShaderVariantKey &key, const Deko9ShaderInfo &info, const std::vector<uint8_t> &dksh)
    {
        if (Find(key))
            return;
        m_pending.push_back(ShaderPackRecord{key, info, dksh});
    }

    bool Dirty() const { return !m_pending.empty(); }
    size_t RecordCount() const { return m_records.size() + m_pending.size(); }

    // Folds the delta into the loaded records and serializes the whole
    // pack (sorted index + records blob) to a file buffer, ready to write
    // to disk. Clears the delta: call once per flush, not per shader.
    std::vector<uint8_t> Serialize()
    {
        using namespace shaderpack_detail;
        for (auto &p : m_pending)
            m_records.push_back(std::move(p));
        m_pending.clear();
        std::sort(m_records.begin(), m_records.end(),
                  [](const ShaderPackRecord &a, const ShaderPackRecord &b) { return a.key < b.key; });
        std::vector<uint8_t> recordsBlob;
        std::vector<FileIndexEntry> index;
        index.reserve(m_records.size());
        for (const auto &r : m_records)
        {
            const size_t start = recordsBlob.size();
            AppendRecord(recordsBlob, r);
            FileIndexEntry e{};
            e.key = r.key;
            e.offset = start;
            e.size = (uint32_t)(recordsBlob.size() - start);
            e.checksum = Fnv1a(recordsBlob.data() + start, e.size);
            index.push_back(e);
        }
        FileHeader header{};
        header.magic = kMagic;
        header.version = m_version;
        std::memcpy(header.mojoPin, m_mojoPin, sizeof(m_mojoPin));
        std::memcpy(header.uamPin, m_uamPin, sizeof(m_uamPin));
        header.recordCount = (uint32_t)index.size();
        std::vector<uint8_t> out(sizeof(header) + index.size() * sizeof(FileIndexEntry) + recordsBlob.size());
        uint8_t *w = out.data();
        std::memcpy(w, &header, sizeof(header));
        w += sizeof(header);
        if (!index.empty())
        {
            std::memcpy(w, index.data(), index.size() * sizeof(FileIndexEntry));
            w += index.size() * sizeof(FileIndexEntry);
        }
        if (!recordsBlob.empty())
            std::memcpy(w, recordsBlob.data(), recordsBlob.size());
        return out;
    }

private:
    static bool Fail(std::string *error, const char *what)
    {
        if (error)
            *error = what;
        return false;
    }

    // Parses the whole file into `out`; never touches m_records/m_pending,
    // so a failure partway through leaves Load() free to discard `out` and
    // change nothing.
    bool ParseFile(const std::vector<uint8_t> &file, std::vector<ShaderPackRecord> *out, std::string *error)
    {
        using namespace shaderpack_detail;
        if (file.size() < sizeof(FileHeader))
            return Fail(error, "truncated header");
        FileHeader header;
        std::memcpy(&header, file.data(), sizeof(header));
        if (header.magic != kMagic)
            return Fail(error, "bad magic");
        if (header.version != m_version || std::memcmp(header.mojoPin, m_mojoPin, sizeof(m_mojoPin)) ||
            std::memcmp(header.uamPin, m_uamPin, sizeof(m_uamPin)))
            return Fail(error, "version or translator pin mismatch");
        const size_t indexBytes = (size_t)header.recordCount * sizeof(FileIndexEntry);
        if (sizeof(header) + indexBytes > file.size())
            return Fail(error, "truncated index");
        const uint8_t *indexBase = file.data() + sizeof(header);
        const uint8_t *records = file.data() + sizeof(header) + indexBytes;
        const size_t recordsSize = file.size() - sizeof(header) - indexBytes;
        out->reserve(header.recordCount);
        for (uint32_t i = 0; i < header.recordCount; ++i)
        {
            // memcpy into a properly-aligned local: indexBase + i * sizeof
            // entry is not guaranteed aligned for the entry's uint64_t
            // members (AArch64 faults on a misaligned load of one).
            FileIndexEntry e;
            std::memcpy(&e, indexBase + (size_t)i * sizeof(FileIndexEntry), sizeof(FileIndexEntry));
            if (e.offset > recordsSize || e.size > recordsSize - e.offset)
                return Fail(error, "index entry out of range");
            const uint8_t *payload = records + e.offset;
            if (Fnv1a(payload, e.size) != e.checksum)
                return Fail(error, "corrupt record (checksum mismatch)");
            if (!DecodeRecord(e.key, payload, e.size, out, error))
                return false;
        }
        // The index must be strictly sorted (Find() binary-searches it);
        // an unsorted or duplicate-key file is corrupt the same way a bad
        // checksum is.
        for (size_t i = 1; i < out->size(); ++i)
            if (!((*out)[i - 1].key < (*out)[i].key))
                return Fail(error, "index not strictly sorted");
        return true;
    }

    static bool DecodeRecord(const ShaderVariantKey &key, const uint8_t *payload, size_t size,
                             std::vector<ShaderPackRecord> *out, std::string *error)
    {
        if (size < sizeof(Deko9ShaderInfo) + sizeof(uint32_t))
            return Fail(error, "truncated record");
        ShaderPackRecord rec;
        rec.key = key;
        std::memcpy(&rec.info, payload, sizeof(Deko9ShaderInfo));
        uint32_t dkshSize;
        std::memcpy(&dkshSize, payload + sizeof(Deko9ShaderInfo), sizeof(dkshSize));
        if (sizeof(Deko9ShaderInfo) + sizeof(uint32_t) + (size_t)dkshSize != size)
            return Fail(error, "record size mismatch");
        rec.dksh.assign(payload + sizeof(Deko9ShaderInfo) + sizeof(uint32_t), payload + size);
        out->push_back(std::move(rec));
        return true;
    }

    static void AppendRecord(std::vector<uint8_t> &blob, const ShaderPackRecord &r)
    {
        const size_t at = blob.size();
        blob.resize(at + sizeof(Deko9ShaderInfo) + sizeof(uint32_t) + r.dksh.size());
        uint8_t *w = blob.data() + at;
        std::memcpy(w, &r.info, sizeof(Deko9ShaderInfo));
        w += sizeof(Deko9ShaderInfo);
        const uint32_t dkshSize = (uint32_t)r.dksh.size();
        std::memcpy(w, &dkshSize, sizeof(dkshSize));
        w += sizeof(dkshSize);
        if (dkshSize)
            std::memcpy(w, r.dksh.data(), dkshSize);
    }

    uint32_t m_version;
    char m_mojoPin[16];
    char m_uamPin[16];
    std::vector<ShaderPackRecord> m_records; // sorted by key once Load()/Serialize() return
    std::vector<ShaderPackRecord> m_pending; // delta since Load(), not yet in m_records
};

} // namespace deko9
