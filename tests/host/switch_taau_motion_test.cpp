// Host proof for r_taau's per-object previous-frame table
// (src/gfx_d3d/r_taau_motion_table.h): first sight, continuity, skipped
// frames, layout change, teleport, a key seen twice in a frame, the rigid
// cap, despawned keys freeing their slots, and a full table; and the motion
// pass's per-draw range check (src/deko9/deko9_taau.h TaauMotionDrawFits):
// offsets past the buffer and a last vertex past it are skipped draws.

#include "src/deko9/deko9_taau.h"
#include "src/gfx_d3d/r_taau_motion_table.h"

#include <cstdio>
#include <memory>
#include <vector>

namespace
{
int g_failures;

void Check(bool ok, const char *what, const char *detail = "")
{
    std::printf("%s:TAAU_MOTION_%s %s\n", ok ? "PASS" : "FAIL", what, detail);
    if (!ok)
        ++g_failures;
}

struct MotionPlacement
{
    float quat[4];
    float origin[3];
    float scale;
};
using taau::MotionState;
using MotionTable = taau::MotionTable<MotionPlacement>;

MotionPlacement Placement(float x)
{
    MotionPlacement p{};
    p.quat[3] = 1.0f;
    p.origin[0] = x;
    p.scale = 1.0f;
    return p;
}
} // namespace

int main()
{
    auto table = std::make_unique<MotionTable>();
    int keys[400];
    const float origin[3] = {100, 0, 0}, walk[3] = {106, 0, 0}, jump[3] = {400, 0, 0};

    {
        MotionPlacement rigid[2] = {Placement(1), Placement(2)};
        const MotionTable::Prev a = table->Note(&keys[0], 10, 77, origin, 4096, rigid, 2);
        MotionPlacement next[2] = {Placement(3), Placement(4)};
        const MotionTable::Prev b = table->Note(&keys[0], 11, 77, walk, 8192, next, 2);
        Check(a.state == MotionState::None && b.state == MotionState::Prev && b.skinBase == 4096 &&
                  b.rigidCount == 2 && next[0].origin[0] == 1 && next[1].origin[0] == 2,
              "CONTINUITY");
        MotionPlacement again[2] = {Placement(5), Placement(6)};
        const MotionTable::Prev c = table->Note(&keys[0], 11, 77, walk, 0, again, 2);
        MotionPlacement after[2] = {Placement(7), Placement(8)};
        const MotionTable::Prev d = table->Note(&keys[0], 12, 77, walk, 0, after, 2);
        Check(c.state == MotionState::None && again[0].origin[0] == 5 && d.state == MotionState::Prev &&
                  d.skinBase == 8192 && after[0].origin[0] == 3,
              "TWICE_IN_A_FRAME");
    }
    {
        MotionPlacement r[1] = {Placement(0)};
        table->Note(&keys[1], 20, 5, origin, 0, r, 1);
        const MotionTable::Prev skipped = table->Note(&keys[1], 22, 5, origin, 0, r, 1);
        const MotionTable::Prev layout = table->Note(&keys[1], 23, 6, origin, 0, r, 1);
        const MotionTable::Prev teleport = table->Note(&keys[1], 24, 6, jump, 0, r, 1);
        const MotionTable::Prev settled = table->Note(&keys[1], 25, 6, jump, 0, r, 1);
        Check(skipped.state == MotionState::None && layout.state == MotionState::Reject &&
                  teleport.state == MotionState::Reject && settled.state == MotionState::Prev,
              "SKIP_LAYOUT_TELEPORT");
    }
    {
        std::vector<MotionPlacement> many(MotionTable::kMaxRigid + 8, Placement(9));
        table->Note(&keys[2], 30, 1, origin, 0, many.data(), (uint32_t)many.size());
        const MotionTable::Prev p = table->Note(&keys[2], 31, 1, origin, 0, many.data(), (uint32_t)many.size());
        Check(p.state == MotionState::Prev && p.rigidCount == MotionTable::kMaxRigid, "RIGID_CAP");
    }
    {
        // Fill every slot with live keys at frame 100: one more is refused.
        auto full = std::make_unique<MotionTable>();
        MotionPlacement r[1] = {Placement(0)};
        uint32_t refused = 0;
        for (uint32_t i = 0; i < MotionTable::kSlots; ++i)
            full->Note(&keys[i], 100, 1, origin, 0, r, 1);
        const MotionTable::Prev extra = full->Note(&keys[MotionTable::kSlots], 100, 1, origin, 0, r, 1);
        const MotionTable::Prev extra2 = full->Note(&keys[MotionTable::kSlots], 101, 1, origin, 0, r, 1);
        refused += extra2.state == MotionState::None;
        // Two frames later the old keys are stale: a new key takes a slot and
        // has continuity in the next frame; an old key starts fresh.
        full->Note(&keys[MotionTable::kSlots + 1], 102, 1, origin, 0, r, 1);
        const MotionTable::Prev reused = full->Note(&keys[MotionTable::kSlots + 1], 103, 1, origin, 0, r, 1);
        const MotionTable::Prev old = full->Note(&keys[5], 103, 1, origin, 0, r, 1);
        Check(extra.state == MotionState::None && refused == 1 && reused.state == MotionState::Prev &&
                  old.state == MotionState::None,
              "DESPAWN_AND_FULL");
    }
    {
        const uint64_t a = MotionTable::Mix(MotionTable::Mix(0, 1), 2), b = MotionTable::Mix(MotionTable::Mix(0, 2), 1);
        Check(a != b, "SIGNATURE_ORDER");
    }
    {
        // A 1000-vertex surface (32-byte vertices) in a 64 KB vertex buffer,
        // 3000 indices in a 4096-index buffer; previous positions in a second
        // buffer of the same size.
        using deko9::TaauMotionFit;
        const uint32_t stride = 32, verts = 1000, vbSize = 65536, ib = 4096;
        const auto fits = [&](uint32_t first, uint32_t count, uint32_t vbOffset, uint32_t prevOffset, uint32_t n) {
            return deko9::TaauMotionDrawFits(ib, first, count, vbSize, vbOffset, vbSize, prevOffset, n, stride);
        };
        // The check the pass made before: only the first byte of each offset.
        const auto oldFits = [&](uint32_t first, uint32_t count, uint32_t vbOffset, uint32_t prevOffset) {
            return (uint64_t)first + count <= ib && vbOffset < vbSize && prevOffset < vbSize;
        };
        const uint32_t lastFit = vbSize - verts * stride; // 33536: the surface ends at the buffer's end
        const TaauMotionFit valid = fits(0, 3000, 0, 4096, verts);
        const TaauMotionFit atEnd = fits(1096, 3000, lastFit, lastFit, verts);
        const TaauMotionFit offsetPast = fits(0, 3000, vbSize + 64, 0, verts);
        const TaauMotionFit maxVertexPast = fits(0, 3000, lastFit + stride, 0, verts);
        const TaauMotionFit prevPast = fits(0, 3000, 0, 60000, verts);
        const TaauMotionFit indexPast = fits(1097, 3000, 0, 0, verts);
        const TaauMotionFit noVertices = fits(0, 3000, 0, 0, 0);
        const TaauMotionFit wrap = fits(0, 3000, 0xffffff00u, 0, verts);
        const bool oldAcceptsOverrun = oldFits(0, 3000, lastFit + stride, 0) && oldFits(0, 3000, 0, 60000);
        char detail[200];
        std::snprintf(detail, sizeof(detail),
                      "valid=%s at_end=%s offset_past=%s max_vertex_past=%s prev_past=%s index_past=%s "
                      "no_vertices=%s wrap=%s old_accepts_overrun=%d",
                      deko9::TaauMotionFitName(valid), deko9::TaauMotionFitName(atEnd),
                      deko9::TaauMotionFitName(offsetPast), deko9::TaauMotionFitName(maxVertexPast),
                      deko9::TaauMotionFitName(prevPast), deko9::TaauMotionFitName(indexPast),
                      deko9::TaauMotionFitName(noVertices), deko9::TaauMotionFitName(wrap), (int)oldAcceptsOverrun);
        Check(valid == TaauMotionFit::Ok && atEnd == TaauMotionFit::Ok && offsetPast == TaauMotionFit::VertexRange &&
                  maxVertexPast == TaauMotionFit::VertexRange && prevPast == TaauMotionFit::PrevVertexRange &&
                  indexPast == TaauMotionFit::IndexRange && noVertices == TaauMotionFit::VertexRange &&
                  wrap == TaauMotionFit::VertexRange && oldAcceptsOverrun,
              "DRAW_RANGE", detail);
    }
    std::printf("%s:TAAU_MOTION\n", g_failures ? "FAIL" : "PASS");
    return g_failures ? 1 : 0;
}
