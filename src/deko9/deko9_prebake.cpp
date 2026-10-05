// Load-time shader variant prebake of the deko3d renderer (deko9): every
// variant a loaded material pass can select (deko9_variant_plan.h) is
// translated and compiled while the zone loads, without the device lock, so
// a draw only ever looks one up.

// libnx before the D3D9 headers: the vendored windows_base.h defines `interface`.
#include <switch.h>

#include "deko9_internal.h"
#include "deko9_native.h"
#include "deko9_prebake_queue.h"

#include <unordered_map>
#include <vector>

namespace deko9
{
namespace
{

constexpr size_t kBuildThreadStack = 0x40000; // UAM's front end recurses deeply

struct BuildJob
{
    ShaderBase *shader;
    VariantSelect select;
    std::vector<uint8_t> dksh;
    bool ok = false;
    bool packHit = false;
};

template <typename Queue> void BuildThreadMain(void *arg)
{
    static_cast<Queue *>(arg)->BuildAll();
}

} // namespace

bool Device::PrebakeVariants(const Deko9PassVariants *passes, uint32_t count, int cpuId, int priority,
                             Deko9PrebakeResult *out)
{
    Deko9PrebakeResult result{};
    const uint64_t start = LockClockNs();
    if (count && !passes)
        return false;
    // The jobs hold the shaders without the device lock across the build,
    // so each shader object is referenced until its last install: an engine
    // Release meanwhile only drops the engine's reference.
    ShaderPins<IUnknown> pins;
    for (uint32_t i = 0; i < count; ++i)
    {
        pins.Pin(passes[i].vs);
        pins.Pin(passes[i].ps);
    }
    PlanOptions options;
    {
        DeviceLockGuard lock(m_lock);
        options.earlyZ = m_earlyZ;
        options.shadowFilter = m_shadowFilter;
        options.shaderOpt = m_shaderOpt;
    }

    // The plan reads only immutable shader state (info after creation), so
    // it runs without the lock; a shader's plan id is its index + 1 here.
    VariantPlan plan;
    std::vector<ShaderBase *> shaders;
    std::unordered_map<const ShaderBase *, uint64_t> ids;
    const auto idOf = [&](ShaderBase *shader) -> uint64_t {
        if (!shader)
            return 0;
        const auto it = ids.emplace(shader, shaders.size() + 1);
        if (it.second)
            shaders.push_back(shader);
        return it.first->second;
    };
    for (uint32_t i = 0; i < count; ++i)
    {
        const Deko9PassVariants &pass = passes[i];
        ShaderBase *vs = pass.vs ? &static_cast<VertexShader *>(pass.vs)->shader : nullptr;
        ShaderBase *ps = pass.ps ? &static_cast<PixelShader *>(pass.ps)->shader : nullptr;
        if (!vs && !ps)
            continue;
        PassVariantDesc desc;
        desc.vsId = idOf(vs);
        desc.psId = idOf(ps);
        desc.vs = vs ? &vs->Info() : nullptr;
        desc.ps = ps ? &ps->Info() : nullptr;
        desc.depthSamplerMask = pass.depthSamplerMask;
        desc.alphaTest = pass.alphaTest != 0;
        if (pass.instanceRegCount)
            LayoutFromRegs(pass.instanceRegs, pass.instanceRegCount, &desc.instance);
        plan.Add(desc, options);
        ++result.passes;
    }
    const std::vector<PlannedVariant> &variants = plan.Finish();
    result.planned = (uint32_t)variants.size();

    std::vector<BuildJob> jobs;
    {
        DeviceLockGuard lock(m_lock);
        for (const PlannedVariant &v : variants)
        {
            ShaderBase *shader = shaders[v.shader - 1];
            if (shader->Find(v.select))
                ++result.present;
            else
                jobs.push_back(BuildJob{shader, v.select, {}, false, false});
        }
    }
    result.planUs = (LockClockNs() - start) / 1000;

    auto queue = MakePrebakeQueue(
        jobs, [this](BuildJob &job) { return job.shader->BuildCode(this, job.select, &job.dksh, &job.packHit); },
        [] { return LockClockNs(); });
    Thread thread{};
    bool threaded = false;
    if (!jobs.empty() && cpuId >= 0 &&
        R_SUCCEEDED(threadCreate(&thread, BuildThreadMain<decltype(queue)>, &queue, nullptr, kBuildThreadStack,
                                 priority, cpuId)))
    {
        threaded = R_SUCCEEDED(threadStart(&thread));
        if (!threaded)
            threadClose(&thread);
    }
    result.threads = threaded ? 1 : 0;

    // Install each result as it arrives (one short lock per variant: the
    // back end keeps drawing the loading screen in between). The caller
    // builds unclaimed jobs too, so a starved compile thread cannot stall
    // the load; once only the compile thread's job is left, that thread is
    // raised to the caller's priority so it finishes.
    const auto drain = [&] {
        s32 callerPriority = 0;
        if (threaded && R_SUCCEEDED(svcGetThreadPriority(&callerPriority, CUR_THREAD_HANDLE)) &&
            callerPriority < priority)
            svcSetThreadPriority(thread.handle, (u32)callerPriority);
    };
    queue.InstallAll([&](BuildJob &job) {
        bool ok = job.ok;
        if (ok)
        {
            DeviceLockGuard lock(m_lock);
            ok = job.shader->Install(this, job.select, job.dksh) != nullptr;
        }
        if (ok)
        {
            ++result.built;
            result.packHits += job.packHit ? 1 : 0;
        }
        else
        {
            ++result.failed;
        }
        std::vector<uint8_t>().swap(job.dksh);
    }, drain);
    if (threaded)
    {
        threadWaitForExit(&thread);
        threadClose(&thread);
    }
    result.buildUs = queue.BuildNs() / 1000;
    result.wallUs = (LockClockNs() - start) / 1000;
    Log("prebake passes=%u planned=%u present=%u built=%u packHits=%u failed=%u threads=%u planUs=%llu buildUs=%llu "
        "wallUs=%llu",
        result.passes, result.planned, result.present, result.built, result.packHits, result.failed, result.threads,
        (unsigned long long)result.planUs, (unsigned long long)result.buildUs, (unsigned long long)result.wallUs);
    if (out)
        *out = result;
    return true;
}

} // namespace deko9

bool Deko9_PrebakeVariants(IDirect3DDevice9 *device, const Deko9PassVariants *passes, uint32_t count, int cpuId,
                           int priority, Deko9PrebakeResult *result)
{
    if (!device)
        return false;
    return static_cast<deko9::Device *>(device)->PrebakeVariants(passes, count, cpuId, priority, result);
}
