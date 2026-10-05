#pragma once

// r_taau per-object motion: what each animated model (keyed by its DObj)
// looked like last frame, so this frame's surfaces can name where they were.
//
// One entry per key records the frame it was last seen, a signature of its
// surface layout (which surfaces, in which order), its origin, where its
// skinned vertices start in that frame's skin cache, and the placements of
// its rigid surfaces. Note() returns what the previous frame left and
// records this frame's state:
//   Prev    seen in the previous frame with the same layout and a plausible
//           move: the previous skin base and rigid placements are valid;
//   Reject  seen in the previous frame but with another layout (a different
//           model in a reused DObj) or a jump past kTeleport: its pixels have
//           no usable history;
//   None    not seen in the previous frame (first sight, back in view): the
//           resolve reprojects its pixels by depth and clips the history.
// Entries not seen for two frames are free for reuse, so despawned keys
// cost nothing. Header-only and engine-free (host test:
// switch_taau_motion_test); `Placement` is the engine's GfxScaledPlacement.

#include <cstdint>
#include <cstring>
#include <functional>

namespace taau
{

enum class MotionState : uint8_t
{
    None,
    Prev,
    Reject,
};

template <typename Placement> class MotionTable
{
public:
    static constexpr uint32_t kSlots = 256;
    static constexpr uint32_t kMaxRigid = 64;
    static constexpr float kTeleport = 128.0f;

    struct Prev
    {
        MotionState state = MotionState::None;
        int32_t skinBase = -1;   // previous frame's first skinned byte, -1 none
        uint32_t rigidCount = 0; // valid entries of the caller's rigid array
    };

    void Clear() { std::memset(m_slots, 0, sizeof(m_slots)); }

    // `rigid` holds this frame's rigid placements in surface order (count
    // capped at kMaxRigid) and receives the previous frame's on Prev.
    Prev Note(const void *key, uint32_t frame, uint64_t signature, const float origin[3], int32_t skinBase,
              Placement *rigid, uint32_t rigidCount)
    {
        Prev out;
        if (!key || !frame)
            return out;
        if (rigidCount > kMaxRigid)
            rigidCount = kMaxRigid;
        Slot *slot = Find(key, frame);
        if (!slot)
            return out;
        if (slot->key == key && slot->frame == frame)
            return out; // seen twice this frame: keep the first record
        if (slot->key == key && slot->frame + 1 == frame)
        {
            const float dx = origin[0] - slot->origin[0], dy = origin[1] - slot->origin[1],
                        dz = origin[2] - slot->origin[2];
            if (slot->signature != signature || dx * dx + dy * dy + dz * dz > kTeleport * kTeleport)
                out.state = MotionState::Reject;
            else
            {
                out.state = MotionState::Prev;
                out.skinBase = slot->skinBase;
                out.rigidCount = slot->rigidCount < rigidCount ? slot->rigidCount : rigidCount;
            }
        }
        slot->key = key;
        slot->frame = frame;
        slot->signature = signature;
        std::memcpy(slot->origin, origin, sizeof(slot->origin));
        slot->skinBase = skinBase;
        // Swap: the slot keeps this frame's placements, the caller gets the
        // previous ones.
        for (uint32_t i = 0; i < rigidCount; ++i)
        {
            const Placement now = rigid[i];
            rigid[i] = slot->rigid[i];
            slot->rigid[i] = now;
        }
        slot->rigidCount = rigidCount;
        return out;
    }

    // Order-dependent hash of a layout, one value per surface.
    static uint64_t Mix(uint64_t hash, uint64_t value)
    {
        hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        return hash * 0xff51afd7ed558ccdull;
    }

private:
    struct Slot
    {
        const void *key;
        uint32_t frame;
        uint32_t rigidCount;
        uint64_t signature;
        float origin[3];
        int32_t skinBase;
        Placement rigid[kMaxRigid];
    };

    // The key's slot, else the first free one on its probe path (null when
    // the table is full of live keys).
    Slot *Find(const void *key, uint32_t frame)
    {
        uint64_t h = std::hash<const void *>{}(key);
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        Slot *free = nullptr;
        for (uint32_t i = 0; i < kSlots; ++i)
        {
            Slot *s = &m_slots[(h + i) % kSlots];
            if (s->key == key)
                return s;
            const bool live = s->key && s->frame + 2 > frame;
            if (!live && !free)
                free = s;
            if (!s->key)
                break;
        }
        if (free)
            std::memset(free, 0, sizeof(*free));
        return free;
    }

    Slot m_slots[kSlots] = {};
};

} // namespace taau
