// The cinematic movie rectangle (Switch FFmpeg cinematic path): before a
// decoded frame is uploaded it must draw opaque black through the white
// material, never the see-through cinematic material; once a frame exists it
// draws the cinematic material modulated by white.
#include <port/switch_cinematic_plane.h>
#include <cstdio>

struct Material
{
    int id;
};

static int s_failures;
static void Check(bool ok, const char *what)
{
    std::printf("%s %s\n", ok ? "ok" : "FAIL:CINEMATIC_PLANE", what);
    s_failures += !ok;
}

int main()
{
    Material cinematic{1}, white{2};
    const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float opaqueWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    const CinematicPlaneDraw pending = Cinematic_MoviePlaneDraw(false, &cinematic, &white, black, opaqueWhite);
    Check(pending.material == &white && pending.color == black, "no uploaded frame: white material, opaque black");
    Check(pending.color[3] == 1.0f, "no uploaded frame: opaque");

    const CinematicPlaneDraw playing = Cinematic_MoviePlaneDraw(true, &cinematic, &white, black, opaqueWhite);
    Check(playing.material == &cinematic && playing.color == opaqueWhite, "uploaded frame: cinematic material, white");

    std::printf("cinematic plane: %s (%d failures)\n", s_failures ? "FAIL" : "PASS", s_failures);
    return s_failures ? 1 : 0;
}
