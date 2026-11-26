#include "saige/render/instance_culler.hpp"

#include "saige/shaders/scene_cull_instances.spv.hpp"

#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace saige::render {

namespace {

template <size_t N>
std::span<const uint32_t> words(const uint32_t (&array)[N])
{
    return {array, N};
}

struct CullParams {
    float planes[6][4];
    float sphere[4];
    uint32_t counts[4];
};
static_assert(sizeof(CullParams) == 128);

} // namespace

InstanceCuller::InstanceCuller(rhi::Device& device, GpuResourceRegistry& registry, const InstanceCullerSettings& settings)
    : device_(device), registry_(registry), max_instances_(settings.max_instances), host_visible_outputs_(settings.host_visible_outputs)
{
    if (settings.max_instances < 1 || settings.max_instances > (1u << 20)) {
        throw std::invalid_argument("InstanceCuller: max_instances must be 1 to 1048576");
    }
    if (!device.info().compute) {
        throw std::runtime_error("InstanceCuller: the device does not support compute");
    }
    try {
        shader_ = device_.create_shader({rhi::ShaderStage::Compute, words(saige::shaders::scene_cull_instances_cs_main), "CSMain", "SceneCullInstances.CS"});
        rhi::BindGroupLayoutDesc ld;
        for (uint32_t binding = 0; binding < 4; ++binding) {
            rhi::BindGroupLayoutEntry e;
            e.binding = binding;
            e.type = binding == 0 ? rhi::BindingType::UniformBuffer : rhi::BindingType::StorageBuffer;
            e.visibility = rhi::StageMask::Compute;
            ld.entries.push_back(e);
        }
        ld.debug_name = "InstanceCullLayout";
        layout_ = device_.create_bind_group_layout(ld);
        rhi::ComputePipelineDesc pd;
        pd.compute_shader = shader_;
        pd.bind_group_layouts = {layout_};
        pd.debug_name = "InstanceCull";
        pipeline_ = device_.create_compute_pipeline(pd);

        const uint64_t rows_bytes = uint64_t{max_instances_} * 3u * 16u;
        rhi::BufferDesc bd;
        bd.size = 256;
        bd.usage = rhi::BufferUsage::Uniform;
        bd.memory = rhi::MemoryLocation::CpuToGpu;
        bd.debug_name = "InstanceCullParams";
        params_ = registry_.create_buffer(bd);
        bd.size = rows_bytes;
        bd.usage = rhi::BufferUsage::Storage;
        bd.debug_name = "InstanceCullInput";
        in_rows_ = registry_.create_buffer(bd);
        bd.memory = host_visible_outputs_ ? rhi::MemoryLocation::GpuToCpu : rhi::MemoryLocation::GpuOnly;
        bd.debug_name = "InstanceCullOutput";
        out_rows_ = registry_.create_buffer(bd);
        bd.size = 256;
        bd.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::Indirect;
        bd.memory = rhi::MemoryLocation::CpuToGpu;  // five words: host-visible so the host can initialise the count and a test can read it
        bd.debug_name = "InstanceCullArgs";
        args_ = registry_.create_buffer(bd);

        BindGroupBinding b[4];
        b[0].binding = 0;
        b[0].buffer = params_;
        b[0].size = sizeof(CullParams);
        b[1].binding = 1;
        b[1].buffer = in_rows_;
        b[1].size = rows_bytes;
        b[2].binding = 2;
        b[2].buffer = out_rows_;
        b[2].size = rows_bytes;
        b[3].binding = 3;
        b[3].buffer = args_;
        b[3].size = 20;
        group_ = registry_.create_bind_group(layout_, std::span<const BindGroupBinding>(b, 4), "InstanceCull");
    } catch (...) {
        release();
        throw;
    }
}

InstanceCuller::~InstanceCuller() { release(); }

void InstanceCuller::release() noexcept
{
    group_.reset();
    args_.reset();
    out_rows_.reset();
    in_rows_.reset();
    params_.reset();
    if (pipeline_.is_valid()) device_.destroy(pipeline_);
    pipeline_ = {};
    if (layout_.is_valid()) device_.destroy(layout_);
    layout_ = {};
    if (shader_.is_valid()) device_.destroy(shader_);
    shader_ = {};
}

void InstanceCuller::cull(rhi::CommandList& cmd, const Frustum& frustum, std::span<const Mat4> worlds, const CullSphere& sphere, uint32_t index_count)
{
    if (worlds.size() > max_instances_) {
        throw std::invalid_argument("InstanceCuller::cull: more instances than max_instances");
    }
    if (!std::isfinite(sphere.radius) || sphere.radius < 0.0f || !std::isfinite(sphere.center.x) || !std::isfinite(sphere.center.y) || !std::isfinite(sphere.center.z)) {
        throw std::invalid_argument("InstanceCuller::cull: the bounding sphere must be finite with a radius >= 0");
    }
    // Host writes first (they may throw while mapping): the arguments start as an empty draw, the pass fills in the instance count.
    {
        std::span<std::byte> a = device_.map(args_.handle());
        const rhi::DrawIndexedIndirectCommand initial{index_count, 0, 0, 0, 0};
        std::memcpy(a.data(), &initial, sizeof(initial));
    }
    if (worlds.empty()) {
        return;
    }
    CullParams p{};
    for (size_t i = 0; i < 6; ++i) {
        p.planes[i][0] = frustum.planes[i].normal.x;
        p.planes[i][1] = frustum.planes[i].normal.y;
        p.planes[i][2] = frustum.planes[i].normal.z;
        p.planes[i][3] = frustum.planes[i].d;
    }
    p.sphere[0] = sphere.center.x;
    p.sphere[1] = sphere.center.y;
    p.sphere[2] = sphere.center.z;
    p.sphere[3] = sphere.radius;
    p.counts[0] = static_cast<uint32_t>(worlds.size());
    {
        std::span<std::byte> pm = device_.map(params_.handle());
        std::memcpy(pm.data(), &p, sizeof(p));
        std::span<std::byte> in = device_.map(in_rows_.handle());
        float* rows = reinterpret_cast<float*>(in.data());
        for (size_t i = 0; i < worlds.size(); ++i) {
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 4; ++c) rows[i * 12u + static_cast<size_t>(r * 4 + c)] = worlds[i].at(r, c);
            }
        }
    }
    cmd.bind_pipeline(pipeline_);
    cmd.bind_group(0, group_.handle());
    cmd.dispatch((static_cast<uint32_t>(worlds.size()) + 63u) / 64u, 1, 1);
    // The results are read by the vertex stage (rows) and the indirect draw (arguments); the pass wrote both from the compute stage.
    rhi::BufferBarrier rows_barrier;
    rows_barrier.buffer = out_rows_.handle();
    rows_barrier.before = {rhi::PipelineStage::ComputeShader, rhi::Access::ShaderStorageWrite, rhi::TextureLayout::Undefined};
    rows_barrier.after = {rhi::PipelineStage::VertexShader | rhi::PipelineStage::Host, rhi::Access::ShaderStorageRead | rhi::Access::HostRead, rhi::TextureLayout::Undefined};
    rhi::BufferBarrier args_barrier;
    args_barrier.buffer = args_.handle();
    args_barrier.before = {rhi::PipelineStage::ComputeShader, rhi::Access::ShaderStorageWrite, rhi::TextureLayout::Undefined};
    args_barrier.after = {rhi::PipelineStage::DrawIndirect | rhi::PipelineStage::Host, rhi::Access::IndirectCommandRead | rhi::Access::HostRead, rhi::TextureLayout::Undefined};
    const rhi::BufferBarrier barriers[2] = {rows_barrier, args_barrier};
    cmd.barrier({}, barriers);
}

rhi::DrawIndexedIndirectCommand InstanceCuller::arguments_after_wait() const
{
    rhi::DrawIndexedIndirectCommand out{};
    const std::span<std::byte> a = device_.map(args_.handle());
    std::memcpy(&out, a.data(), sizeof(out));
    return out;
}

std::vector<std::array<float, 12>> InstanceCuller::visible_rows_after_wait() const
{
    if (!host_visible_outputs_) throw std::logic_error("InstanceCuller::visible_rows_after_wait: the outputs are not host visible (InstanceCullerSettings::host_visible_outputs)");
    const rhi::DrawIndexedIndirectCommand args = arguments_after_wait();
    const uint32_t n = std::min(args.instance_count, max_instances_);
    const std::span<std::byte> m = device_.map(out_rows_.handle());
    std::vector<std::array<float, 12>> out(n);
    for (uint32_t i = 0; i < n; ++i) std::memcpy(out[i].data(), m.data() + size_t{i} * 48u, 48u);
    return out;
}

} // namespace saige::render
