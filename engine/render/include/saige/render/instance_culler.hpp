// GPU frustum culling of the instances of one primitive (docs/rendering/gpu-culling.md): a compute pass that tests each instance's bounding sphere against the six frustum planes (the same
// planes and the same test as the CPU's `intersects_sphere`), compacts the surviving instances' world matrices into a buffer, and counts them into the instance-count word of indirect
// draw arguments, so that `draw_indexed_indirect` draws exactly the visible instances with no readback.
//
// What this is, and is not: a standalone, tested building block. It owns its buffers (input matrices, compacted output, arguments, parameters), a compute pipeline and a bind group.
// It is not wired into `ModelRenderer` yet (that needs the instanced vertex stage to read the compacted buffer, one cull per primitive per frame slot, and per-slot buffers).
// The order of the compacted instances is undefined. Matrices must be finite, non-degenerate affines (the caller validates, as the CPU path does before culling).
//
// Frame contract of this version: one `cull` per culler per frame, recorded outside a rendering scope, and the buffers are not versioned per frame in flight: the caller must not
// call `cull` again until the device has finished the frame that used the previous call's results (the tests wait for the device between uses). A frame-slot ring is the integration step.
#pragma once

#include "saige/render/camera.hpp"
#include "saige/render/gpu_resources.hpp"
#include "saige/render/math.hpp"
#include "saige/rhi/device.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace saige::render {

struct InstanceCullerSettings {
    uint32_t max_instances = 16384;  ///< 1 to 1048576; the capacity of the input and output buffers
    /// Make the compacted rows host-visible so a test or a tool can read them back (`DeviceInfo` has no buffer-to-buffer copy). Off in production: the rows then live in device-local memory.
    bool host_visible_outputs = false;
};

/// A bounding sphere in the primitive's own space, shared by all its instances (`GpuPrimitive::bounds_center`, `bounds_radius`). A radius of 0 is never culled.
struct CullSphere {
    Float3 center{0.0f, 0.0f, 0.0f};
    float radius = 0.0f;
};

class InstanceCuller {
public:
    /// Creates the buffers, the bind group layout and the compute pipeline. Throws std::invalid_argument for settings out of range, std::runtime_error if the device has no compute.
    InstanceCuller(rhi::Device& device, GpuResourceRegistry& registry, const InstanceCullerSettings& settings = {});
    ~InstanceCuller();
    InstanceCuller(const InstanceCuller&) = delete;
    InstanceCuller& operator=(const InstanceCuller&) = delete;

    /// Uploads `worlds` and the frustum, initialises the draw arguments to {index_count, 0, 0, 0, 0} and records the culling dispatch into `cmd` (outside a rendering scope), followed by the
    /// barriers that make the compacted rows visible to the vertex stage and the arguments to the indirect draw. Throws std::invalid_argument for more instances than `max_instances`, a
    /// negative or non-finite radius, or a non-finite sphere centre; nothing is recorded in that case. Zero instances records nothing but still initialises the arguments (an empty draw).
    void cull(rhi::CommandList& cmd, const Frustum& frustum, std::span<const Mat4> worlds, const CullSphere& sphere, uint32_t index_count);

    /// The compacted rows: three float4 per visible instance (the affine's rows), valid for `visible_count_after_wait()` instances. A storage buffer for the vertex stage.
    [[nodiscard]] rhi::BufferHandle rows() const noexcept { return out_rows_.handle(); }
    /// The draw arguments, laid out as `rhi::DrawIndexedIndirectCommand` (index_count, instance_count = the visible count, first_index 0, vertex_offset 0, first_instance 0).
    [[nodiscard]] rhi::BufferHandle arguments() const noexcept { return args_.handle(); }
    [[nodiscard]] uint32_t max_instances() const noexcept { return max_instances_; }

    /// Reads the arguments back from the host (they live in host-visible memory): valid only after the device has finished the frame that ran the cull (for example after `Device::wait_idle`).
    [[nodiscard]] rhi::DrawIndexedIndirectCommand arguments_after_wait() const;
    /// Reads the compacted rows back: only with `host_visible_outputs`, and only after the device has finished the cull. Each entry is {row0, row1, row2} of one visible instance.
    [[nodiscard]] std::vector<std::array<float, 12>> visible_rows_after_wait() const;

private:
    void release() noexcept;
    rhi::Device& device_;
    GpuResourceRegistry& registry_;
    uint32_t max_instances_;
    bool host_visible_outputs_;
    rhi::ShaderHandle shader_;
    rhi::BindGroupLayoutHandle layout_;
    rhi::PipelineHandle pipeline_;
    BufferRef params_, in_rows_, out_rows_, args_;
    BindGroupRef group_;
};

} // namespace saige::render
