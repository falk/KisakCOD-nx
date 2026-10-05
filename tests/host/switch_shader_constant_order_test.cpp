#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <numeric>
#include <random>
#include <stdexcept>
#include <universal/q_shared.h>
#include <gfx_d3d/r_material.h>
#include <gfx_d3d/r_shader_constant_sort.h>
#include <qcommon/com_error.h>

// Compile the production insertion, literal gathering and pixel-constant
// comparison with the real engine types and host error handlers.
struct Drop {};
void Com_Error(errorParm_t code, const char *, ...)
{
    if (code != ERR_DROP)
        std::abort();
    throw Drop{};
}
void MyAssertHandler(const char *, int, int, const char *, ...)
{
    throw std::runtime_error("unexpected material assertion");
}
char *va(const char *, ...)
{
    throw std::runtime_error("unexpected missing material constant");
}
#include "shader-constant-production.inc"

static unsigned checks;
static void Check(bool ok, const char *message)
{
    ++checks;
    if (!ok)
        throw std::runtime_error(message);
}

static void Insertions()
{
    std::array<std::array<float, 4>, 16> values{};
    std::array<unsigned, 16> order{};
    std::iota(order.begin(), order.end(), 0);
    std::mt19937 rng(73);
    for (unsigned run = 0; run < 1002; ++run)
    {
        if (run == 1)
            std::reverse(order.begin(), order.end());
        else if (run > 1)
            std::shuffle(order.begin(), order.end(), rng);
        GfxShaderConstantBlock block{};
        // Unused slots must not affect the ordering of initialized entries.
        std::fill(std::begin(block.dest), std::end(block.dest), UINT16_MAX);
        for (unsigned index : order)
            R_RegisterShaderConst(index * 4096, values[index].data(), &block);
        Check(block.count == 16, "count changed");
        for (unsigned i = 0; i < 16; ++i)
            Check(block.dest[i] == i * 4096 && block.value[i] == values[i].data(),
                  "register/value ordering");
        const GfxShaderConstantBlock before = block;
        bool dropped = false;
        try { R_RegisterShaderConst(0, values[0].data(), &block); }
        catch (const Drop &) { dropped = true; }
        Check(dropped, "full block accepted");
        Check(block.count == before.count &&
              std::equal(std::begin(block.dest), std::end(block.dest), before.dest) &&
              std::equal(std::begin(block.value), std::end(block.value), before.value),
              "overflow changed block");
    }
    GfxShaderConstantBlock block{};
    for (unsigned i = 0; i < 3; ++i)
        R_RegisterShaderConst(7, values[i].data(), &block);
    for (unsigned i = 0; i < 3; ++i)
        Check(block.value[i] == values[i].data(), "duplicate stability");
    R_RegisterShaderConst(UINT16_MAX, values[3].data(), &block);
    Check(block.dest[3] == UINT16_MAX, "maximum register rejected");
    for (unsigned invalid = 0; invalid < 2; ++invalid)
    {
        const GfxShaderConstantBlock before = block;
        bool dropped = false;
        try { R_RegisterShaderConst(invalid ? 0 : 65536, invalid ? nullptr : values[0].data(), &block); }
        catch (const Drop &) { dropped = true; }
        Check(dropped, "invalid input accepted");
        Check(block.count == before.count &&
              std::equal(std::begin(block.dest), std::end(block.dest), before.dest) &&
              std::equal(std::begin(block.value), std::end(block.value), before.value),
              "invalid input changed block");
    }
}

struct Fixture
{
    Material material{};
    MaterialPass pass{};
    std::array<MaterialShaderArgument, 2> args{};
    std::array<std::array<float, 4>, 2> values{};
    Fixture()
    {
        pass.stableArgCount = 2;
        pass.args = args.data();
        for (unsigned i = 0; i < 2; ++i)
        {
            args[i].type = MTL_ARG_LITERAL_PIXEL_CONST;
            args[i].dest = static_cast<uint16_t>(2 - i);
            args[i].u.literalConst = values[i].data();
        }
    }
};

static int Compare(Fixture &a, Fixture &b)
{
    const Material *materials[] = {&a.material, &b.material};
    const MaterialPass *passes[] = {&a.pass, &b.pass};
    return R_ComparePixelConsts(materials, passes);
}

static void Comparisons()
{
    // The input sequence encodes expected numeric order, followed by the NaN
    // equivalence class; signed zero and both NaN signs share ranks.
    const float inf = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float denorm = std::numeric_limits<float>::denorm_min();
    const float values[] = {-inf, -1, -denorm, -0.0f, 0.0f, denorm, 1, inf, nan, -nan};
    const unsigned ranks[] = {0, 1, 2, 3, 3, 4, 5, 6, 7, 7};
    Fixture a, b, c;
    for (unsigned component = 0; component < 4; ++component)
    {
        for (unsigned i = 0; i < 10; ++i)
        for (unsigned j = 0; j < 10; ++j)
        for (unsigned k = 0; k < 10; ++k)
        {
            a.values[1][component] = values[i];
            b.values[1][component] = values[j];
            c.values[1][component] = values[k];
            const int ab = Compare(a, b), bc = Compare(b, c), ac = Compare(a, c);
            const int expected = ranks[i] < ranks[j] ? -1 : ranks[i] > ranks[j] ? 1 : 0;
            Check(ab == expected, "float ordering");
            Check(ab == -Compare(b, a), "comparison antisymmetry");
            Check(Compare(a, a) == 0, "comparison reflexivity");
            Check(!(ab < 0 && bc < 0) || ac < 0, "ordering transitivity");
            Check(!(ab == 0 && bc == 0) || ac == 0, "equivalence transitivity");
        }
        a.values[1][component] = b.values[1][component] = c.values[1][component] = 0;
    }
    // Permuted literal arguments describe the same material constants.
    a.values[0] = {1, 2, 3, 4};
    a.values[1] = {5, 6, 7, 8};
    b.values = a.values;
    std::swap(b.args[0], b.args[1]);
    Check(Compare(a, b) == 0, "literal permutation comparison");
    // Code-register keys take precedence over literal values.
    a.args[0].type = b.args[0].type = MTL_ARG_CODE_PIXEL_CONST;
    a.args[0].u.codeConst.index = 1;
    b.args[0].u.codeConst.index = 2;
    Check(Compare(a, b) < 0 && Compare(b, a) > 0, "code-register ordering");
    b.args[0].u.codeConst.index = 1;
    b.pass.stableArgCount = 1;
    Check(Compare(a, b) > 0 && Compare(b, a) < 0, "literal count ordering");
}

int main()
{
    try
    {
        Insertions();
        Comparisons();
        std::printf("PASS:SHADER_CONSTANT_ORDER production checks=%u\n", checks);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::fprintf(stderr, "FAIL:SHADER_CONSTANT_ORDER %s\n", error.what());
        return 1;
    }
}
