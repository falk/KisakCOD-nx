#include "deko9_shader.h"

#include <mojoshader.h>
#include <compiler_iface.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace
{
// D3DDECLUSAGE values as MojoShader reports them in io_<usage>_<index>.
constexpr int kUsageTexcoord = 5;
constexpr int kUsageColor = 10;

bool Fail(std::string *error, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
bool Fail(std::string *error, const char *fmt, ...)
{
    if (error)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        *error = buf;
    }
    return false;
}

bool StartsWith(const std::string &line, const char *prefix)
{
    return line.compare(0, std::strlen(prefix), prefix) == 0;
}

// Varying location for a D3D usage/index pair, or -1 when unmapped.
int VaryingLocation(int usage, int index)
{
    if (usage == kUsageColor && index >= 0 && index < 2)
        return index;
    if (usage == kUsageTexcoord && index >= 0 && index < 8)
        return 2 + index;
    return -1;
}

// Rewrites one lookup form whose coordinate MojoShader truncated:
// "<call>(<sampler>, X.xy<tail>" -> "<helper>(<sampler>, X<tail>". D3D9
// compares against X.z (texld/texldl on a depth texture), so the helper
// needs the whole source register X back.
bool RewriteTruncated(std::string *line, const std::string &call, const std::string &helper,
                      const std::string &tail, std::string *error)
{
    size_t at;
    while ((at = line->find(call)) != std::string::npos)
    {
        const size_t coord = at + call.size();
        const size_t dot = line->find(".xy", coord);
        if (dot == std::string::npos)
            return Fail(error, "unparsed shadow lookup: %s", line->c_str());
        const std::string reg = line->substr(coord, dot - coord);
        std::string expectTail = tail;
        const size_t pos = expectTail.find('@');
        if (pos != std::string::npos)
            expectTail.replace(pos, 1, reg);
        if (reg.empty() || reg.find_first_of(" (),") != std::string::npos ||
            line->compare(dot + 3, expectTail.size(), expectTail))
            return Fail(error, "unparsed shadow lookup: %s", line->c_str());
        line->replace(at, dot + 3 + expectTail.size() - at,
                      helper + call.substr(call.find('(') + 1) + reg + expectTail);
    }
    return true;
}

// D3D9 float semantics the GLSL built-ins do not give (DXVK emulates the
// same): nrm of a zero vector is 0 and pow(0, 0) is 1 (0 * inf = 0), and a
// guarded rcp/rsq stays finite when the hardware flushes a denormal operand
// to zero. MojoShader's own zero guards compare against exact 0 only.
const char kD3D9MathPrelude[] =
    "vec2 deko9_nrm(vec2 v) { return v * min(inversesqrt(dot(v, v)), 1e38); }\n"
    "vec3 deko9_nrm(vec3 v) { return v * min(inversesqrt(dot(v, v)), 1e38); }\n"
    "vec4 deko9_nrm(vec4 v) { return v * min(inversesqrt(dot(v, v)), 1e38); }\n"
    "float deko9_pow(float a, float b) { return exp2(b == 0.0 ? 0.0 : b * log2(a)); }\n"
    "vec2 deko9_pow(vec2 a, vec2 b) { return exp2(mix(b * log2(a), vec2(0.0), equal(b, vec2(0.0)))); }\n"
    "vec3 deko9_pow(vec3 a, vec3 b) { return exp2(mix(b * log2(a), vec3(0.0), equal(b, vec3(0.0)))); }\n"
    "vec4 deko9_pow(vec4 a, vec4 b) { return exp2(mix(b * log2(a), vec4(0.0), equal(b, vec4(0.0)))); }\n"
    "float deko9_rsq(float x) { return min(inversesqrt(x), 1e38); }\n"
    "float deko9_rcp(float x) { return clamp(1.0 / x, -1e38, 1e38); }\n";

void RewriteD3D9Math(std::string *line)
{
    auto replaceAll = [line](const std::string &from, const std::string &to) {
        for (size_t at = 0; (at = line->find(from, at)) != std::string::npos; at += to.size())
            line->replace(at, from.size(), to);
    };
    replaceAll("normalize(", "deko9_nrm(");
    replaceAll("pow(abs(", "deko9_pow(abs(");
    replaceAll("? FLT_MAX : inversesqrt(", "? FLT_MAX : deko9_rsq(");
    // "? FLT_MAX : 1.0 / X)" with X one register component.
    const std::string rcp = "? FLT_MAX : 1.0 / ";
    for (size_t at = 0; (at = line->find(rcp, at)) != std::string::npos;)
    {
        const size_t operand = at + rcp.size();
        const size_t close = line->find(')', operand);
        if (close == std::string::npos)
            break;
        const std::string reg = line->substr(operand, close - operand);
        const std::string to = "? FLT_MAX : deko9_rcp(" + reg + ")";
        line->replace(at, close - at, to);
        at += to.size();
    }
}

// Rewrites the lookups of one depth-compare (shadow) sampler to comparison
// helpers. Gradient forms are not mapped and fail.
bool RewriteShadowCalls(std::string *line, const char *sampler, std::string *error)
{
    const std::string name = std::string(sampler) + ",";
    const std::string proj = "texture2DProj(" + name;
    size_t at;
    while ((at = line->find(proj)) != std::string::npos)
        line->replace(at, proj.size(), "deko9_shadowProj(" + name);
    // texldl: texture2DLod(s, X.xy, X.w)
    if (!RewriteTruncated(line, "texture2DLod(" + name + " ", "deko9_shadowLod(", ", @.w)", error))
        return false;
    // texld: texture2D(s, X.xy)
    if (!RewriteTruncated(line, "texture2D(" + name + " ", "deko9_shadow(", ")", error))
        return false;
    for (const char *other : {"texture2DGrad(", "texture2DProjGrad("})
    {
        if (line->find(std::string(other) + name) != std::string::npos)
            return Fail(error, "unsupported shadow lookup %s%s", other, sampler);
    }
    return true;
}
} // namespace

namespace
{
// r_shadowFilter 1 (Deko9_TranslateShader shadowFilter). A pre-pass over the
// MojoShader output finds the retail PCF filters of the depth-compare
// samplers: lookups whose coordinates are offset taps around one base,
//   ps_rT = (V.wwww * [-]Y) + V;   (sm3 spot lookups: V an interpolated input)
//   ps_rT = ps_rB + [-]K;          (sm3 sun cascades: base ps_rB, constant K)
// grouped by sampler, lookup form and base (register value version), and
// only when every offset appears with both signs (a symmetric kernel whose
// average position is the base). Each such lookup is moved to the base, so
// the group's fetches become identical (merged by the compiler into one
// hardware 2x2-PCF fetch). A register base is snapshotted into a fresh
// variable before the group's first tap (the taps may overwrite it).
// Analysis is per straight-line segment: any line with braces or control
// flow ends the segment, so taps and lookups always share one block.
class ShadowCenterRewrite
{
public:
    ShadowCenterRewrite(uint32_t samplerMask, const std::string &source) : m_mask(samplerMask)
    {
        for (int &tap : m_tapOf)
            tap = -1;
        if (!m_mask)
            return;
        size_t pos = 0, index = 0;
        while (pos < source.size())
        {
            size_t end = source.find('\n', pos);
            if (end == std::string::npos)
                end = source.size();
            Scan(index++, source.substr(pos, end - pos));
            pos = end + 1;
        }
        EndSegment();
    }

    // Line `index` of the MojoShader output (0-based), rewritten in place;
    // returns the number of lookups moved (0 or 1).
    uint32_t Apply(size_t index, std::string *line) const
    {
        uint32_t moved = 0;
        std::string prefix;
        for (const Edit &edit : m_edits)
        {
            if (edit.line != index)
                continue;
            if (edit.from.empty())
            {
                prefix += edit.to; // base snapshot before the group's first tap
                continue;
            }
            const size_t at = line->find(edit.from);
            if (at == std::string::npos)
                continue;
            line->replace(at, edit.from.size(), edit.to);
            ++moved;
        }
        line->insert(0, prefix);
        return moved;
    }

private:
    struct Tap
    {
        std::string base;   // "V:ps_v5" or "R:<reg>@<version>"
        std::string offset; // offset expression without its sign (+ version)
        bool negative;
        size_t line;
        int baseReg; // register base, or -1
    };
    struct Lookup
    {
        size_t line;
        std::string key; // base | sampler | form
        std::string from, pattern;
        int tap;
    };
    struct Edit
    {
        size_t line;
        std::string from, to; // from empty: insert `to` before the line
    };

    static std::string Trim(const std::string &line)
    {
        const size_t first = line.find_first_not_of(" \t");
        return first == std::string::npos ? std::string() : line.substr(first);
    }
    bool IsInput(const std::string &name) const
    {
        for (const std::string &input : m_inputs)
        {
            if (input == name)
                return true;
        }
        return false;
    }
    std::string Versioned(const char *expr) const
    {
        int reg = 0;
        if (std::sscanf(expr, "ps_r%d", &reg) == 1 && reg >= 0 && reg < 64)
            return std::string(expr) + "@" + std::to_string(m_version[reg]);
        return expr;
    }
    void EndSegment()
    {
        std::vector<std::string> keys;
        for (const Lookup &lookup : m_lookups)
        {
            bool seen = false;
            for (const std::string &key : keys)
                seen |= key == lookup.key;
            if (!seen)
                keys.push_back(lookup.key);
        }
        for (const std::string &key : keys)
        {
            std::vector<const Lookup *> group;
            for (const Lookup &lookup : m_lookups)
            {
                if (lookup.key == key)
                    group.push_back(&lookup);
            }
            // Symmetric: per offset expression, as many + as - taps.
            bool symmetric = group.size() >= 2;
            for (const Lookup *a : group)
            {
                int balance = 0;
                for (const Lookup *b : group)
                {
                    if (m_taps[b->tap].offset == m_taps[a->tap].offset)
                        balance += m_taps[b->tap].negative ? -1 : 1;
                }
                symmetric = symmetric && balance == 0;
            }
            if (!symmetric)
                continue;
            const Tap &first = m_taps[group.front()->tap];
            std::string base;
            if (first.baseReg >= 0)
            {
                size_t firstLine = first.line;
                for (const Lookup *lookup : group)
                    firstLine = std::min(firstLine, m_taps[lookup->tap].line);
                base = "deko9_sc" + std::to_string(m_snapshots++);
                m_edits.push_back({firstLine, std::string(),
                                   "\tvec4 " + base + " = ps_r" + std::to_string(first.baseReg) + ";\n"});
            }
            else
            {
                base = first.base.substr(2);
            }
            for (const Lookup *lookup : group)
            {
                std::string to = lookup->pattern;
                for (size_t at; (at = to.find('@')) != std::string::npos;)
                    to.replace(at, 1, base);
                m_edits.push_back({lookup->line, lookup->from, to});
            }
        }
        m_lookups.clear();
        m_taps.clear();
        for (int &tap : m_tapOf)
            tap = -1;
    }
    // Parses a lookup through a 2D sampler on a register: the sampler, the
    // coordinate register and the call text with the register as '@'.
    static bool ParseLookup(const std::string &body, int *sampler, int *coord, std::string *pattern)
    {
        int dst = 0, s = 0, c = 0, c2 = 0, n = 0;
        char mask[8];
        char text[64];
        const size_t eq = body.find(" = ");
        if (eq == std::string::npos)
            return false;
        const std::string call = body.substr(eq + 3);
        if (std::sscanf(body.c_str(), "ps_r%d%n", &dst, &n) != 1 ||
            !(body[(size_t)n] == ' ' || (std::sscanf(body.c_str() + n, ".%7[xyzw]", mask) == 1)))
            return false;
        n = 0;
        if (std::sscanf(call.c_str(), "texture2DProj(ps_s%d, ps_r%d);%n", &s, &c, &n) == 2 && n == (int)call.size())
            std::snprintf(text, sizeof(text), "texture2DProj(ps_s%d, @)", s);
        else if ((n = 0, std::sscanf(call.c_str(), "texture2D(ps_s%d, ps_r%d.xy);%n", &s, &c, &n)) == 2 &&
                 n == (int)call.size())
            std::snprintf(text, sizeof(text), "texture2D(ps_s%d, @.xy)", s);
        else if ((n = 0, std::sscanf(call.c_str(), "texture2DLod(ps_s%d, ps_r%d.xy, ps_r%d.w);%n", &s, &c, &c2, &n)) ==
                     3 &&
                 n == (int)call.size() && c == c2)
            std::snprintf(text, sizeof(text), "texture2DLod(ps_s%d, @.xy, @.w)", s);
        else
            return false;
        *sampler = s;
        *coord = c;
        *pattern = text;
        return true;
    }
    void Scan(size_t index, const std::string &line)
    {
        const std::string body = Trim(line);
        int a = 0, n = 0;
        char name[2], target[32];
        if (std::sscanf(body.c_str(), "#define ps_%1[vt]%d io_%*d_%*d%n", name, &a, &n) == 2 && n == (int)body.size())
        {
            std::snprintf(target, sizeof(target), "ps_%s%d", name, a);
            m_inputs.push_back(target);
            return;
        }
        if (body.empty() || body[0] == '#')
            return;
        if (body.find('{') != std::string::npos || body.find('}') != std::string::npos ||
            body.compare(0, 2, "if") == 0 || body.compare(0, 4, "else") == 0 || body.compare(0, 3, "for") == 0 ||
            body.compare(0, 5, "while") == 0 || body.compare(0, 2, "do") == 0)
        {
            EndSegment();
            return;
        }
        int sampler = -1, coord = -1;
        std::string pattern;
        if (ParseLookup(body, &sampler, &coord, &pattern) && sampler >= 0 && sampler < 32 &&
            ((m_mask >> sampler) & 1u) && coord >= 0 && coord < 64 && m_tapOf[coord] >= 0)
        {
            std::string from = pattern;
            const std::string reg = "ps_r" + std::to_string(coord);
            for (size_t at; (at = from.find('@')) != std::string::npos;)
                from.replace(at, 1, reg);
            const int tap = m_tapOf[coord];
            m_lookups.push_back({index, m_taps[tap].base + "|" + pattern, from, pattern, tap});
        }
        // Register writes: taps record their base and offset; every write
        // (whole or partial) makes a new value version of the register.
        int reg = 0;
        n = 0;
        if (std::sscanf(body.c_str(), "ps_r%d%n", &reg, &n) != 1 || reg < 0 || reg >= 64)
            return;
        const size_t eq = body.find(" = ", (size_t)n);
        if (eq == std::string::npos || (body[(size_t)n] != ' ' && body[(size_t)n] != '.') ||
            body.find_first_of("=(", (size_t)n) != eq + 1)
            return;
        const bool whole = body[(size_t)n] == ' ';
        char v1[16], v2[16], off[24];
        int baseReg = -1, tap = -1;
        int m = 0;
        if (whole &&
            std::sscanf(body.c_str(), "ps_r%*d = (%15[a-z_0-9].wwww * %23[-a-z_0-9.]) + %15[a-z_0-9];%n", v1, off, v2,
                        &m) == 3 &&
            m == (int)body.size() && !std::strcmp(v1, v2) && IsInput(v1))
        {
            const bool negative = off[0] == '-';
            m_taps.push_back({std::string("V:") + v1, Versioned(off + (negative ? 1 : 0)), negative, index, -1});
            tap = (int)m_taps.size() - 1;
        }
        else if (whole && (m = 0, std::sscanf(body.c_str(), "ps_r%*d = ps_r%d + %23[-a-z_0-9.];%n", &baseReg, off,
                                              &m)) == 2 &&
                 m == (int)body.size() && baseReg >= 0 && baseReg < 64)
        {
            const bool negative = off[0] == '-';
            m_taps.push_back({"R:" + std::to_string(baseReg) + "@" + std::to_string(m_version[baseReg]),
                              Versioned(off + (negative ? 1 : 0)), negative, index, baseReg});
            tap = (int)m_taps.size() - 1;
        }
        ++m_version[reg];
        m_tapOf[reg] = tap;
    }

    uint32_t m_mask;
    std::vector<std::string> m_inputs;
    std::vector<Tap> m_taps;
    std::vector<Lookup> m_lookups;
    std::vector<Edit> m_edits;
    int m_version[64] = {};
    int m_tapOf[64];
    uint32_t m_snapshots = 0;
};
} // namespace

uint64_t Deko9_HashBytecode(const void *bytecode, size_t bytes)
{
    uint64_t hash = 0xcbf29ce484222325ull;
    const uint8_t *p = static_cast<const uint8_t *>(bytecode);
    for (size_t i = 0; i < bytes; ++i)
        hash = (hash ^ p[i]) * 0x100000001b3ull;
    return hash;
}

bool Deko9_TranslateShader(const void *bytecode, size_t bytes, uint32_t shadowSamplerMask,
                           std::string *glsl, Deko9ShaderInfo *info, std::string *error,
                           const uint8_t *instanceRegs, uint32_t instanceRegCount, bool earlyFragmentTests,
                           uint32_t shadowFilter)
{
    if (shadowFilter >= DEKO9_SHADOW_FILTER_MODES)
        return Fail(error, "shadow filter mode %u", shadowFilter);
    if (instanceRegCount > DEKO9_MAX_INSTANCE_REGS || (instanceRegCount && !instanceRegs))
        return Fail(error, "instance layout of %u registers", instanceRegCount);
    *info = Deko9ShaderInfo{};
    glsl->clear();
    const MOJOSHADER_parseData *parsed = MOJOSHADER_parse(
        MOJOSHADER_PROFILE_GLSLES3, "main", static_cast<const unsigned char *>(bytecode),
        static_cast<unsigned int>(bytes), nullptr, 0, nullptr, 0, nullptr, nullptr, nullptr);
    if (!parsed)
        return Fail(error, "MojoShader returned null");
    struct Free
    {
        const MOJOSHADER_parseData *p;
        ~Free() { MOJOSHADER_freeParseData(p); }
    } freeParsed{parsed};
    if (parsed->error_count)
        return Fail(error, "MojoShader: %s", parsed->errors[0].error);
    if (!parsed->output || parsed->output_len <= 0)
        return Fail(error, "MojoShader produced no output");

    bool vertex;
    if (parsed->shader_type == MOJOSHADER_TYPE_VERTEX)
        vertex = true;
    else if (parsed->shader_type == MOJOSHADER_TYPE_PIXEL)
        vertex = false;
    else
        return Fail(error, "unsupported shader type %d", (int)parsed->shader_type);
    info->stage = vertex ? DEKO9_STAGE_VERTEX : DEKO9_STAGE_PIXEL;
    if (instanceRegCount && !vertex)
        return Fail(error, "instance layout on a pixel shader");
    if (earlyFragmentTests && vertex)
        return Fail(error, "early fragment tests on a vertex shader");
    const uint32_t instanceBase = DEKO9_MAX_VS_INPUTS - instanceRegCount;
    const char *prefix = vertex ? "vs" : "ps";
    const uint32_t regLimit = vertex ? DEKO9_VS_CONST_REGS : DEKO9_PS_CONST_REGS;

    for (int i = 0; i < parsed->uniform_count; ++i)
    {
        const MOJOSHADER_uniform &u = parsed->uniforms[i];
        if (u.type != MOJOSHADER_UNIFORM_FLOAT)
            return Fail(error, "unsupported %s constant type %d", prefix, (int)u.type);
        const int count = u.array_count ? u.array_count : 1;
        if (u.index < 0 || (uint32_t)(u.index + count) > regLimit)
            return Fail(error, "%s constant c%d out of range", prefix, u.index);
        if ((uint32_t)(u.index + count) > info->constRegs)
            info->constRegs = (uint32_t)(u.index + count);
    }
    for (int i = 0; i < parsed->sampler_count; ++i)
    {
        const MOJOSHADER_sampler &s = parsed->samplers[i];
        if (s.index < 0 || s.index >= (int)DEKO9_MAX_SAMPLERS)
            return Fail(error, "%s sampler s%d out of range", prefix, s.index);
        switch (s.type)
        {
        case MOJOSHADER_SAMPLER_2D: info->samplerDim[s.index] = DEKO9_SAMPLER_2D; break;
        case MOJOSHADER_SAMPLER_VOLUME: info->samplerDim[s.index] = DEKO9_SAMPLER_3D; break;
        case MOJOSHADER_SAMPLER_CUBE: info->samplerDim[s.index] = DEKO9_SAMPLER_CUBE; break;
        default: return Fail(error, "unsupported sampler type %d", (int)s.type);
        }
        info->samplerMask |= 1u << s.index;
    }
    if (shadowSamplerMask & ~info->samplerMask)
        return Fail(error, "shadow mask 0x%x names undeclared samplers", shadowSamplerMask);
    if (vertex)
    {
        for (int i = 0; i < parsed->attribute_count; ++i)
        {
            // MojoShader's attribute index is the usage index; the input
            // register is only in the variable name.
            const MOJOSHADER_attribute &a = parsed->attributes[i];
            int reg = -1, consumed = 0;
            if (!a.name || std::sscanf(a.name, "vs_v%d%n", &reg, &consumed) != 1 ||
                a.name[consumed] || reg < 0 || reg >= (int)DEKO9_MAX_VS_INPUTS)
                return Fail(error, "vs input %s not a v register", a.name ? a.name : "(null)");
            info->inputMask |= 1u << reg;
            info->inputUsage[reg] = (uint8_t)a.usage;
            info->inputUsageIndex[reg] = (uint8_t)a.index;
        }
    }

    if (instanceRegCount && (info->inputMask >> instanceBase))
        return Fail(error, "instance attributes (locations %u..15) overlap vs inputs 0x%x", instanceBase,
                    info->inputMask);

    std::string out;
    out.reserve((size_t)parsed->output_len + 1024);
    const std::string source(parsed->output, (size_t)parsed->output_len);
    if (!vertex)
    {
        // MojoShader emits texkill as `discard` and oDepth as gl_FragDepth.
        info->kills = source.find("discard") != std::string::npos;
        info->writesDepth = source.find("gl_FragDepth") != std::string::npos;
        if (earlyFragmentTests && info->writesDepth)
            return Fail(error, "early fragment tests on a shader that writes depth");
    }
    size_t pos = 0;
    bool sawVersion = false;
    bool pendingMain = false;
    std::string mainPrologue;
    // D3D9 saturates oD0/oD1 (COLOR outputs) of vs_1_x/vs_2_x; MojoShader
    // does not (DXVK: dxso_compiler.cpp clamps them). Such shaders get their
    // body renamed and a main() that clamps every declared io_10_N after it.
    const bool saturateColors = vertex && parsed->major_ver < 3;
    uint32_t colorOutputs = 0;
    char expect[160];
    const ShadowCenterRewrite shadowCenter(shadowFilter == 1 && !vertex ? shadowSamplerMask : 0u, source);
    size_t lineIndex = 0;
    while (pos < source.size())
    {
        size_t end = source.find('\n', pos);
        if (end == std::string::npos)
            end = source.size();
        std::string line = source.substr(pos, end - pos);
        pos = end + 1;
        const size_t thisLine = lineIndex++;

        if (line == "#version 300 es")
        {
            out += "#version 460\n";
            out += kD3D9MathPrelude;
            // D3D9 guarantees that the same position math yields the same
            // depth in every shader (depth prepass, then LEQUAL/EQUAL lit
            // and light passes); DXVK declares position invariant for this.
            // Without it the compiler may contract the math differently per
            // program, and later passes lose the depth test in patches.
            if (vertex)
                out += "invariant gl_Position;\n";
            if (earlyFragmentTests)
                out += "layout(early_fragment_tests) in;\n";
            if (shadowSamplerMask)
                out += "vec4 deko9_shadowProj(sampler2DShadow s, vec4 c) "
                       "{ return vec4(textureProj(s, c)); }\n"
                       "vec4 deko9_shadow(sampler2DShadow s, vec4 c) "
                       "{ return vec4(texture(s, c.xyz)); }\n"
                       "vec4 deko9_shadowLod(sampler2DShadow s, vec4 c, float lod) "
                       "{ return vec4(textureLod(s, c.xyz, lod)); }\n";
            sawVersion = true;
            continue;
        }
        if (StartsWith(line, "precision "))
            continue;

        int n = 0, m = 0, k = 0, consumed = 0;
        std::snprintf(expect, sizeof(expect), "uniform vec4 %s_uniforms_vec4[%%d];%%n", prefix);
        if (std::sscanf(line.c_str(), expect, &n, &consumed) == 1 && consumed == (int)line.size())
        {
            std::snprintf(expect, sizeof(expect),
                          "layout(std140, binding = 0) uniform Deko9%sConsts "
                          "{ vec4 %s_c_file[%u]; };\n",
                          vertex ? "Vs" : "Ps", prefix, regLimit);
            out += expect;
            continue;
        }
        std::snprintf(expect, sizeof(expect), "#define %s_c%%d %s_uniforms_vec4[%%d]%%n", prefix,
                      prefix);
        if (std::sscanf(line.c_str(), expect, &n, &m, &consumed) == 2 &&
            consumed == (int)line.size())
        {
            if (n < 0 || (uint32_t)n >= regLimit)
                return Fail(error, "%s_c%d out of range", prefix, n);
            // Instanced variant: this register comes from an instance-rate
            // attribute (the same float4 the constant file would hold).
            uint32_t slot = instanceRegCount;
            for (uint32_t i = 0; i < instanceRegCount; ++i)
            {
                if (instanceRegs[i] == (uint32_t)n)
                    slot = i;
            }
            if (slot < instanceRegCount)
                std::snprintf(expect, sizeof(expect),
                              "layout(location = %u) in vec4 deko9_inst%u;\n#define %s_c%d deko9_inst%u\n",
                              DEKO9_MAX_VS_INPUTS - instanceRegCount + slot, slot, prefix, n, slot);
            else
                std::snprintf(expect, sizeof(expect), "#define %s_c%d %s_c_file[%d]\n", prefix, n, prefix, n);
            out += expect;
            continue;
        }
        if (vertex && std::sscanf(line.c_str(), "in vec4 vs_v%d;%n", &n, &consumed) == 1 &&
            consumed == (int)line.size())
        {
            if (n < 0 || n >= (int)DEKO9_MAX_VS_INPUTS || !(info->inputMask & (1u << n)))
                return Fail(error, "vs input v%d not declared", n);
            std::snprintf(expect, sizeof(expect), "layout(location = %d) in vec4 vs_v%d;\n", n, n);
            out += expect;
            continue;
        }
        const char *ioFormat = vertex ? "out highp vec4 io_%d_%d;%n" : "in highp vec4 io_%d_%d;%n";
        if (std::sscanf(line.c_str(), ioFormat, &n, &m, &consumed) == 2 &&
            consumed == (int)line.size())
        {
            const int location = VaryingLocation(n, m);
            if (location < 0)
                return Fail(error, "unmapped %s varying usage=%d index=%d", prefix, n, m);
            if (vertex && n == 10 && m >= 0 && m < 32)
                colorOutputs |= 1u << m;
            info->varyingMask |= 1u << location;
            std::snprintf(expect, sizeof(expect), "layout(location = %d) %s", location,
                          line.c_str());
            out += expect;
            out += '\n';
            continue;
        }
        // ps_1_x texture registers that the shader also writes: MojoShader
        // emits "vec4 ps_tN = io_5_N;" without declaring the input, and a
        // global may not be initialized from an input in GLSL 4.60. Declare
        // the TEXCOORDN input and copy it at the top of main().
        if (!vertex && std::sscanf(line.c_str(), "vec4 ps_t%d = io_%d_%d;%n", &n, &m, &k,
                                   &consumed) == 3 &&
            consumed == (int)line.size())
        {
            const int location = VaryingLocation(m, k);
            if (location < 0 || m != kUsageTexcoord || k != n)
                return Fail(error, "unmapped texture register copy: %s", line.c_str());
            info->varyingMask |= 1u << location;
            std::snprintf(expect, sizeof(expect),
                          "layout(location = %d) in highp vec4 io_%d_%d;\nvec4 ps_t%d;\n",
                          location, m, n, n);
            out += expect;
            std::snprintf(expect, sizeof(expect), "\tps_t%d = io_%d_%d;\n", n, m, n);
            mainPrologue += expect;
            continue;
        }
        if (line == "{" && pendingMain)
        {
            out += "{\n";
            out += mainPrologue;
            pendingMain = false;
            continue;
        }
        if (line == "void main()")
        {
            pendingMain = true;
            if (saturateColors)
                line = "void deko9_main()";
        }
        char samplerType[32];
        if (std::sscanf(line.c_str(), "uniform %31s %*[a-z]_s%d;%n", samplerType, &n, &consumed) == 2 &&
            consumed == (int)line.size() && StartsWith(samplerType, "sampler"))
        {
            if (n < 0 || n >= (int)DEKO9_MAX_SAMPLERS || !(info->samplerMask & (1u << n)))
                return Fail(error, "%s sampler s%d not declared", prefix, n);
            const bool shadow = (shadowSamplerMask >> n) & 1u;
            if (shadow && std::strcmp(samplerType, "sampler2D"))
                return Fail(error, "shadow compare on %s s%d", samplerType, n);
            std::snprintf(expect, sizeof(expect), "layout(binding = %d) uniform %s %s_s%d;\n", n,
                          shadow ? "sampler2DShadow" : samplerType, prefix, n);
            out += expect;
            continue;
        }
        if (line.find("uniforms_") != std::string::npos)
            return Fail(error, "unmapped constant access: %s", line.c_str());
        if (StartsWith(line, "in ") || StartsWith(line, "out ") || StartsWith(line, "uniform "))
            return Fail(error, "unmapped declaration: %s", line.c_str());
        if (!vertex)
            info->shadowCenterTaps += shadowCenter.Apply(thisLine, &line);
        for (uint32_t s = 0; s < DEKO9_MAX_SAMPLERS; ++s)
        {
            if (!((shadowSamplerMask >> s) & 1u))
                continue;
            char sampler[16];
            std::snprintf(sampler, sizeof(sampler), "%s_s%u", prefix, s);
            if (!RewriteShadowCalls(&line, sampler, error))
                return false;
        }
        RewriteD3D9Math(&line);
        out += line;
        out += '\n';
    }
    if (pendingMain)
        return Fail(error, "MojoShader output lacks the main() body");
    if (saturateColors)
    {
        out += "void main()\n{\n    deko9_main();\n";
        for (uint32_t m = 0; m < 32; ++m)
        {
            if (!(colorOutputs & (1u << m)))
                continue;
            std::snprintf(expect, sizeof(expect), "    io_10_%u = clamp(io_10_%u, 0.0, 1.0);\n", m, m);
            out += expect;
        }
        out += "}\n";
    }
    if (!sawVersion)
        return Fail(error, "MojoShader output lacks #version 300 es");
    *glsl = std::move(out);
    return true;
}

bool Deko9_CompileDksh(Deko9Stage stage, const std::string &glsl, std::vector<uint8_t> *dksh,
                       std::string *error)
{
    static std::mutex s_uamLock;
    std::lock_guard<std::mutex> lock(s_uamLock);
    DekoCompiler compiler(stage == DEKO9_STAGE_VERTEX ? pipeline_stage_vertex
                                                      : pipeline_stage_fragment);
    if (!compiler.CompileGlsl(glsl.c_str()))
        return Fail(error, "UAM rejected the translated %s shader",
                    stage == DEKO9_STAGE_VERTEX ? "vertex" : "pixel");
    void *data = nullptr;
    uint32_t size = 0;
    if (!compiler.OutputDkshToMemory(&data, &size))
        return Fail(error, "UAM could not allocate the DKSH image");
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    dksh->assign(bytes, bytes + size);
    std::free(data);
    if (size < 4 || std::memcmp(dksh->data(), "DKSH", 4))
        return Fail(error, "UAM output is not DKSH");
    return true;
}

bool Deko9_ScanDksh(const uint8_t *dksh, size_t size, Deko9DkshInfo *info, std::string *error)
{
    // DkshHeader: magic, header_sz, control_sz, code_sz, programs_off,
    // num_programs; DkshProgramHeader (64 bytes): type, entrypoint,
    // num_gprs, constbuf1_off, constbuf1_sz, per_warp_scratch_sz, ...
    uint32_t hdr[6];
    if (!dksh || size < sizeof(hdr))
        return Fail(error, "DKSH too small");
    std::memcpy(hdr, dksh, sizeof(hdr));
    if (hdr[0] != 0x48534b44u)
        return Fail(error, "not a DKSH image");
    const uint32_t controlSize = hdr[2], codeSize = hdr[3], programsOff = hdr[4], numPrograms = hdr[5];
    if (numPrograms != 1 || (uint64_t)programsOff + 24 > controlSize || (uint64_t)controlSize + codeSize > size)
        return Fail(error, "malformed DKSH header (programs=%u control=%u code=%u size=%zu)", numPrograms,
                    controlSize, codeSize, size);
    uint32_t prog[6];
    std::memcpy(prog, dksh + programsOff, sizeof(prog));
    *info = {};
    info->programType = prog[0];
    info->entrypoint = prog[1];
    info->numGprs = prog[2];
    info->constbuf1Off = prog[3];
    info->constbuf1Size = prog[4];
    info->codeSize = codeSize;
    // frag.early_fragment_tests: byte 25 of the program header (after six
    // words and has_table_3d1); deko3d programs register 0x084 from it.
    if (info->programType == 1 && (uint64_t)programsOff + 26 <= controlSize)
        info->earlyFragmentTests = dksh[programsOff + 25] != 0;
    const uint8_t *code = dksh + controlSize;
    // Graphics programs carry the 0x50-byte shader program header first;
    // instruction groups (sched + 3 instructions) start right after it.
    // Compute (type 5) programs have none.
    const uint32_t sph = info->programType == 5 ? 0 : 0x50;
    uint32_t end = codeSize;
    if (info->constbuf1Size && info->constbuf1Off > info->entrypoint && info->constbuf1Off < end)
        end = info->constbuf1Off; // the compiler constbuf is data, not code
    if ((uint64_t)info->entrypoint + sph + 8 > end)
        return Fail(error, "DKSH entrypoint 0x%x outside the code section (%u bytes)", info->entrypoint, codeSize);
    uint32_t pos = info->entrypoint + sph, index = 0;
    for (; pos + 8 <= end; pos += 8, ++index)
    {
        if (index % 4 == 0)
            continue; // scheduling word
        uint64_t w;
        std::memcpy(&w, code + pos, 8);
        if (!w)
            continue; // padding past the program
        const uint32_t top = (uint32_t)(w >> 56);
        if ((top >= 0x48 && top <= 0x4f) || (top >= 0x51 && top <= 0x53))
            info->slotMask |= 1u << ((w >> 34) & 0x1f);
        else if ((w >> 51) == 0x1df2) // LDC
            info->slotMask |= 1u << ((w >> 36) & 0x1f);
        ++info->instructions;
    }
    return true;
}

uint32_t Deko9_BoundCbufSlots(const Deko9DkshInfo &info)
{
    uint32_t mask = (1u << 0) | (1u << 2);
    if (info.constbuf1Size)
        mask |= 1u << 1;
    if (info.programType == 1)
        mask |= 1u << 3;
    return mask;
}

bool Deko9_DkshStats(const uint8_t *dksh, size_t size, Deko9DkshStats *out)
{
    *out = {};
    uint32_t header[6];
    if (size < sizeof(header))
        return false;
    std::memcpy(header, dksh, sizeof(header));
    const uint32_t controlSize = header[2], codeSize = header[3], programsOffset = header[4];
    if (header[0] != 0x48534b44 || header[5] != 1 || (uint64_t)programsOffset + 24 > size ||
        (uint64_t)controlSize + codeSize > size)
        return false;
    uint32_t program[6]; // type, entrypoint, numGprs, constbuf1Offset, constbuf1Size, scratch
    std::memcpy(program, dksh + programsOffset, sizeof(program));
    const uint64_t begin = (uint64_t)controlSize + program[1] + 0x50;
    const uint64_t end = (uint64_t)controlSize + (program[4] ? program[3] : codeSize);
    if (begin > end || end > size)
        return false;
    out->gprs = program[2];
    const uint64_t words = (end - begin) / 8;
    uint32_t instrs = 0, nops = 0, trailingNops = 0;
    for (uint64_t i = 0; i < words; ++i)
    {
        if (!(i % 4))
            continue; // scheduling control word
        uint64_t w;
        std::memcpy(&w, dksh + begin + i * 8, 8);
        if (w == 0xe2400fffff07000full || w == 0xe2400fffff87000full)
            break; // BRA to self: end-of-program padding
        const uint32_t top = (uint32_t)(w >> 48);
        if (top == 0x50b0)
        {
            ++nops;
            ++trailingNops;
            continue;
        }
        ++instrs;
        trailingNops = 0;
        // Maxwell SM5x opcode forms (from the NAK sm50 opcode table).
        if ((top & 0xff00) == 0xe000)
            ++out->ipa; // IPA (0xe0xx; the branch/exit forms are 0xe2xx/0xe3xx)
        else if ((top & 0xfff8) == 0x5080)
            ++out->mufu; // MUFU
        else if ((top & 0xfe00) == 0xd800 || (top & 0xfe00) == 0xda00 || // TEXS, TLDS
                 (top & 0xffc0) == 0xdf00 ||                             // TLD4S
                 (top & 0xffc0) == 0x0380 || (top & 0xfff8) == 0xdeb8 || // TEX
                 (top & 0xfff8) == 0xdc38 || (top & 0xfff8) == 0xdd38 || // TLD
                 (top & 0xfff8) == 0xc838 || (top & 0xfff8) == 0xdef8 || // TLD4
                 (top & 0xfff8) == 0xde38 || (top & 0xfff8) == 0xde78 || // TXD
                 (top & 0xfff8) == 0xdf48 || (top & 0xfff8) == 0xdf50 || // TXQ
                 (top & 0xfff8) == 0xdf58 || (top & 0xfff8) == 0xdf60)   // TMML
            ++out->tex;
    }
    out->instrs = instrs;
    out->nops = nops - trailingNops;
    return instrs > 0;
}
