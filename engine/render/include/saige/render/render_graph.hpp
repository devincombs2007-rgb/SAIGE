// SAIGE Render — frame render graph: declarative passes, automatic culling, barriers and transient aliasing.
//
// Design: docs/rendering/render-architecture.md §4.
//
// Usage per frame:
//   RenderGraph graph;
//   RGTexture backbuffer = graph.import_texture("Backbuffer", desc, swapchain_tex, initial_state, present_state);
//   RGTexture hdr;
//   graph.add_pass("Lighting", rhi::QueueType::Graphics,
//       [&](PassBuilder& b) { hdr = b.create_texture("HDR", hdr_desc); b.write_color(hdr, LoadOp::Clear); },
//       [=](PassContext& ctx) { /* record commands */ });
//   graph.add_pass("Tonemap", ..., [&](PassBuilder& b) { b.sample(hdr, PS); b.write_color(backbuffer, LoadOp::DontCare); }, ...);
//   const CompileResult& r = graph.compile();
//   graph.execute(execute_context);
//
// Semantics:
// - Passes execute in declaration order (v1; async-compute scheduling comes later).
// - Passes whose results are never consumed are culled. Roots are passes that write an imported
//   resource, or that call `PassBuilder::set_side_effects()`.
// - Transient resources get their usage flags inferred from their accesses, and are assigned to
//   aliased memory slots when their lifetimes don't overlap.
// - Barriers are derived from declared accesses (Vulkan synchronization2 semantics).
#pragma once

#include "saige/rhi/barrier.hpp"
#include "saige/rhi/handle.hpp"
#include "saige/rhi/resource_desc.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace saige::rhi {
class CommandList;  // defined by the RHI device layer (Phase 1)
}

namespace saige::render {

inline constexpr uint32_t invalid_resource = 0xFFFFFFFFu;

/// Virtual texture handle, valid only within the graph that created it.
struct RGTexture {
    uint32_t id = invalid_resource;
    [[nodiscard]] constexpr bool is_valid() const noexcept { return id != invalid_resource; }
    friend constexpr bool operator==(RGTexture, RGTexture) noexcept = default;
};

/// Virtual buffer handle, valid only within the graph that created it.
struct RGBuffer {
    uint32_t id = invalid_resource;
    [[nodiscard]] constexpr bool is_valid() const noexcept { return id != invalid_resource; }
    friend constexpr bool operator==(RGBuffer, RGBuffer) noexcept = default;
};

enum class LoadOp : uint8_t {
    Load,      ///< Preserve existing contents (read-modify-write).
    Clear,     ///< Contents are fully overwritten.
    DontCare,  ///< Contents are fully overwritten (or don't matter).
};

enum class BufferReadKind : uint8_t { Vertex, Index, Indirect, Uniform, Storage };

/// One resource access declared by a pass.
struct ResourceAccess {
    uint32_t resource = invalid_resource;
    rhi::ResourceState state;
    bool discard = false;  ///< A write that fully overwrites the resource (previous contents not needed).
};

class RenderGraph;
class PassContext;

/// Collects a pass's resource declarations during setup.
class PassBuilder {
public:
    // -- Resource creation (transient: lives for this frame only) -----------------------------------------------
    /// `desc.usage` may be left as None: usage is inferred from the accesses.
    RGTexture create_texture(std::string_view name, const rhi::TextureDesc& desc);
    RGBuffer create_buffer(std::string_view name, const rhi::BufferDesc& desc);

    // -- Texture accesses -----------------------------------------------------------------------------------------
    void write_color(RGTexture tex, LoadOp load = LoadOp::Load);
    void write_depth(RGTexture tex, LoadOp load = LoadOp::Load);
    void read_depth(RGTexture tex);  ///< Depth test without writes (read-only depth attachment).
    void sample(RGTexture tex, rhi::PipelineStage shader_stages);
    void read_storage(RGTexture tex, rhi::PipelineStage shader_stages);
    void write_storage(RGTexture tex, rhi::PipelineStage shader_stages, bool full_overwrite = false);
    void copy_src(RGTexture tex);
    void copy_dst(RGTexture tex, bool full_overwrite = true);

    // -- Buffer accesses ------------------------------------------------------------------------------------------
    void read_buffer(RGBuffer buf, BufferReadKind kind, rhi::PipelineStage shader_stages = rhi::PipelineStage::None);
    void write_storage(RGBuffer buf, rhi::PipelineStage shader_stages, bool full_overwrite = false);
    void copy_src(RGBuffer buf);
    void copy_dst(RGBuffer buf, bool full_overwrite = true);

    // -- Escape hatches -------------------------------------------------------------------------------------------
    void access(RGTexture tex, const rhi::ResourceState& state, bool discard = false);
    void access(RGBuffer buf, const rhi::ResourceState& state, bool discard = false);

    /// The pass must never be culled (readbacks, debug output, GPU timestamps).
    void set_side_effects();

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& graph, uint32_t pass_index) : graph_(graph), pass_index_(pass_index) {}
    void add_access(uint32_t resource, bool expect_texture, const rhi::ResourceState& state, bool discard);

    RenderGraph& graph_;
    uint32_t pass_index_;
};

/// Handed to a pass's execute callback.
class PassContext {
public:
    [[nodiscard]] rhi::TextureHandle texture(RGTexture tex) const;
    [[nodiscard]] rhi::BufferHandle buffer(RGBuffer buf) const;
    [[nodiscard]] const rhi::TextureDesc& texture_desc(RGTexture tex) const;
    [[nodiscard]] const rhi::BufferDesc& buffer_desc(RGBuffer buf) const;
    /// May be null (null backend, unit tests).
    [[nodiscard]] rhi::CommandList* command_list() const noexcept { return command_list_; }
    [[nodiscard]] std::string_view pass_name() const noexcept { return pass_name_; }

private:
    friend class RenderGraph;
    PassContext(const RenderGraph& graph, rhi::CommandList* cmd, std::string_view name,
                std::span<const rhi::TextureHandle> textures, std::span<const rhi::BufferHandle> buffers)
        : graph_(graph), command_list_(cmd), pass_name_(name), textures_(textures), buffers_(buffers)
    {
    }

    const RenderGraph& graph_;
    rhi::CommandList* command_list_;
    std::string_view pass_name_;
    std::span<const rhi::TextureHandle> textures_;
    std::span<const rhi::BufferHandle> buffers_;
};

// ---------------------------------------------------------------------------------------------------------------------
// Compilation output
// ---------------------------------------------------------------------------------------------------------------------

/// A barrier on a virtual resource, resolved to an rhi::TextureBarrier / rhi::BufferBarrier at execute time.
struct GraphBarrier {
    uint32_t resource = invalid_resource;
    rhi::ResourceState before;
    rhi::ResourceState after;
};

struct CompiledPass {
    uint32_t pass_index = 0;  ///< Index in declaration order.
    std::vector<GraphBarrier> barriers_before;
};

struct ResourceLifetime {
    uint32_t first_pass = invalid_resource;  ///< Position in CompileResult::passes (execution order).
    uint32_t last_pass = invalid_resource;
    [[nodiscard]] constexpr bool used() const noexcept { return first_pass != invalid_resource; }
};

/// Placement of a transient resource in aliased memory.
struct TransientAllocation {
    uint32_t slot = invalid_resource;              ///< Memory slot shared by non-overlapping resources.
    uint64_t size_bytes = 0;                        ///< Estimated (or backend-reported) memory size.
    uint32_t alias_predecessor = invalid_resource;  ///< Resource that occupied this slot before (if any).
};

struct CompileResult {
    bool ok = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    std::vector<CompiledPass> passes;   ///< Surviving passes in execution order.
    std::vector<uint32_t> culled_passes;  ///< Declaration indices of culled passes.
    std::vector<GraphBarrier> final_barriers;  ///< Transitions of imported resources to their final states.

    std::vector<ResourceLifetime> lifetimes;          ///< Indexed by resource id.
    std::vector<TransientAllocation> allocations;     ///< Indexed by resource id (slot invalid for imported/unused).
    std::vector<uint64_t> slot_sizes;                  ///< Size of each aliased memory slot.
    uint64_t transient_bytes_unaliased = 0;             ///< Sum of all used transient resources.
    uint64_t transient_bytes_aliased = 0;               ///< Sum of slot sizes.
};

/// Options affecting compilation.
struct CompileOptions {
    /// Memory size estimate for transient textures. The Vulkan backend can supply exact sizes from
    /// vk_get_device_image_memory_requirements (Vulkan 1.3). The default estimate is the packed data size
    /// rounded up to 64 KiB.
    std::function<uint64_t(const rhi::TextureDesc&)> texture_size;
    std::function<uint64_t(const rhi::BufferDesc&)> buffer_size;
    bool enable_aliasing = true;
};

/// Resources supplied by the executor.
struct ExecuteContext {
    rhi::CommandList* command_list = nullptr;
    /// Physical handles for transient resources, indexed by resource id (may be empty in tests).
    /// Imported resources always use the handle given at import.
    std::span<const rhi::TextureHandle> transient_textures;
    std::span<const rhi::BufferHandle> transient_buffers;
    /// Receives each pass's barriers before its execute callback runs, and the final barriers at the end.
    std::function<void(std::span<const rhi::TextureBarrier>, std::span<const rhi::BufferBarrier>)> submit_barriers;
};

// ---------------------------------------------------------------------------------------------------------------------
// RenderGraph
// ---------------------------------------------------------------------------------------------------------------------

class RenderGraph {
public:
    using ExecuteFn = std::function<void(PassContext&)>;

    RenderGraph() = default;
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    /// Declares a pass. `setup(PassBuilder&)` runs immediately. `execute(PassContext&)` runs in `execute()`.
    template <typename SetupFn>
    uint32_t add_pass(std::string_view name, rhi::QueueType queue, SetupFn&& setup, ExecuteFn execute = {})
    {
        const uint32_t index = begin_pass(name, queue, std::move(execute));
        PassBuilder builder(*this, index);
        setup(builder);
        return index;
    }

    /// Imports an externally owned texture (swapchain image, history buffer, persistent atlas).
    /// `final_state`: the state the texture must be left in after the graph (e.g., Present).
    RGTexture import_texture(std::string_view name, const rhi::TextureDesc& desc, rhi::TextureHandle physical,
                            const rhi::ResourceState& initial_state,
                            std::optional<rhi::ResourceState> final_state = std::nullopt);
    RGBuffer import_buffer(std::string_view name, const rhi::BufferDesc& desc, rhi::BufferHandle physical,
                          const rhi::ResourceState& initial_state,
                          std::optional<rhi::ResourceState> final_state = std::nullopt);

    const CompileResult& compile(const CompileOptions& options = {});
    [[nodiscard]] const CompileResult& compiled() const noexcept { return result_; }

    /// Runs surviving passes in order, emitting barriers through `ctx.submit_barriers`. Requires a
    /// successful compile().
    void execute(const ExecuteContext& ctx);

    /// Clears all passes and resources for the next frame (keeps allocations).
    void reset();

    /// Graphviz DOT dump of passes, resources and dependencies (culled passes shown dashed).
    [[nodiscard]] std::string to_graphviz() const;

    [[nodiscard]] uint32_t pass_count() const noexcept { return static_cast<uint32_t>(passes_.size()); }
    [[nodiscard]] uint32_t resource_count() const noexcept { return static_cast<uint32_t>(resources_.size()); }
    [[nodiscard]] std::string_view pass_name(uint32_t pass_index) const { return passes_.at(pass_index).name; }
    [[nodiscard]] std::string_view resource_name(uint32_t resource) const { return resources_.at(resource).name; }
    /// Final description of a texture (transient usage flags are filled in after compile()).
    [[nodiscard]] const rhi::TextureDesc& texture_desc(RGTexture tex) const { return resources_.at(tex.id).texture; }
    [[nodiscard]] const rhi::BufferDesc& buffer_desc(RGBuffer buf) const { return resources_.at(buf.id).buffer; }
    [[nodiscard]] bool is_imported(uint32_t resource) const { return resources_.at(resource).imported; }

private:
    friend class PassBuilder;
    friend class PassContext;

    enum class ResourceKind : uint8_t { Texture, Buffer };

    struct Resource {
        std::string name;
        ResourceKind kind = ResourceKind::Texture;
        bool imported = false;
        rhi::TextureDesc texture;
        rhi::BufferDesc buffer;
        rhi::TextureHandle physical_texture;
        rhi::BufferHandle physical_buffer;
        rhi::ResourceState initial_state;
        std::optional<rhi::ResourceState> final_state;
        uint32_t creator_pass = invalid_resource;
    };

    struct Pass {
        std::string name;
        rhi::QueueType queue = rhi::QueueType::Graphics;
        ExecuteFn execute;
        std::vector<ResourceAccess> accesses;  // merged: at most one entry per resource
        bool side_effects = false;
    };

    uint32_t begin_pass(std::string_view name, rhi::QueueType queue, ExecuteFn execute);
    void add_error(std::string message);

    std::vector<Pass> passes_;
    std::vector<Resource> resources_;
    std::vector<std::string> setup_errors_;
    CompileResult result_;
    bool compiled_ = false;
};

} // namespace saige::render
