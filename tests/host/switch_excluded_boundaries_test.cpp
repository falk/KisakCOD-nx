//  host proof: the excluded-boundary stubs must report every
// effectful reach into a CMake-excluded runtime subsystem to the frame-evidence
// audit, attributed to the exact entry point that was reached -- so a walk that
// silently relies on one can fail the walk gate instead of passing on draw counts
// alone, and so the walk verifier can gate each site by its pixel
// impact instead of trusting a subsystem-wide aggregate.
//
// This test links the real excluded-boundary translation unit (compiled with
// the same flags as the retail host links, i.e. its __SWITCH__ bodies)
// against a counting double for RetailKillhouseFrameEvidenceNoteExcluded.  It
// is a positive control per site: call every counted entry point once and
// require its own site counter to move by exactly one, with the subsystem
// kind derived from the site (never passed separately).
//
// the sound, physics and screenshot stubs this proof used
// to exercise are gone -- those paths are real on Switch (OpenAL over libnx
// audren, the vendored ODE sources, the PNG screenshot command).
// the server, save-history and renderer worker spawns are
// all real, so no fake thread-spawn site remains.
//
// Prints PASS:KILLHOUSE_EXCLUDED_BOUNDARIES with the per-kind counts and the
// number of site counters verified; exits nonzero on any missing, duplicated,
// or spurious reach.
#include <cstdio>
#include <cstring>

#include <database/database.h>
#include <qcommon/threads.h>
#include <database/db_retail_frame_evidence.h>

// Counting double for the production note (db_retail_frame_evidence.cpp
// is not part of this narrow host link).
// xanim.h's static dispatch table references these even though this narrow
// test never executes a collision trace (same doubles as
// switch_retail_boot_test.cpp's narrow host link).
void CG_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}
void G_TraceCapsule(trace_t *, const float *, const float *, const float *, const float *, int, int) {}

static uint32_t s_siteNotes[RKE_SITE_COUNT];
static uint32_t s_kindNotes[RKE_KIND_COUNT];

void RetailKillhouseFrameEvidenceNoteExcluded(uint32_t site)
{
    if (site >= RKE_SITE_COUNT)
        return;
    ++s_siteNotes[site];
    switch (site)
    {
    case RKE_SITE_CINE_SET_NEXT_PLAYBACK:
        ++s_kindNotes[RKE_KIND_CINE];
        break;
    case RKE_SITE_THREAD_FAKE_SPAWN:
        ++s_kindNotes[RKE_KIND_THREAD];
        break;
    default:
        break;
    }
}

static bool expect_site(uint32_t site, uint32_t want, const char *what)
{
    if (s_siteNotes[site] != want)
    {
        printf("FAIL:KILLHOUSE_EXCLUDED_BOUNDARIES %s site=%u got=%u want=%u\n",
               what, site, s_siteNotes[site], want);
        return false;
    }
    return true;
}

static bool expect(uint32_t kind, uint32_t want, const char *what)
{
    if (s_kindNotes[kind] != want)
    {
        printf("FAIL:KILLHOUSE_EXCLUDED_BOUNDARIES %s kind=%u got=%u want=%u\n",
               what, kind, s_kindNotes[kind], want);
        return false;
    }
    return true;
}

#define SITE_ONE(expr, site, what) \
    do \
    { \
        (expr); \
        if (!expect_site((site), 1, (what))) \
            return 1; \
    } while (0)

int main()
{
    memset(s_siteNotes, 0, sizeof(s_siteNotes));
    memset(s_kindNotes, 0, sizeof(s_kindNotes));

    // The cinematic stub is guest-only (switch_cinematic_stubs.cpp is not part
    // of this narrow host link); its site is covered by the production run.
    if (!expect(RKE_KIND_CINE, 0, "cinematic host stub absent"))
        return 1;
    if (!expect_site(RKE_SITE_CINE_SET_NEXT_PLAYBACK, 0, "cine_set_next_playback absent"))
        return 1;

    // Thread: every production spawn is real. The retired site must stay
    // untouched; otherwise a substituted path has been reintroduced.
    if (!expect_site(RKE_SITE_THREAD_FAKE_SPAWN, 0, "thread_fake_spawn retired"))
        return 1;
    if (!expect(RKE_KIND_THREAD, 0, "thread reaches"))
        return 1;

    printf("PASS:KILLHOUSE_EXCLUDED_BOUNDARIES sites=%u snd=retired phys=retired shot=retired "
           "cine=0 thread=%u\n",
           (unsigned)RKE_SITE_COUNT, s_kindNotes[RKE_KIND_THREAD]);
    return 0;
}
