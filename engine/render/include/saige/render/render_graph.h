// SAIGE Render — frame render graph: declarative passes, automatic culling, barriers and transient aliasing.
//
// Design: docs/rendering/render-architecture.md §4.
//
// Usage per frame:
//   RenderGraph graph;
//   RGTexture backbuffer = graph.importTexture("Backbuffer", desc, swapchainTex, initialState, presentState);
//   RGTexture hdr;
//   graph.addPass("Lighting", rhi::QueueType::Graphics,
//       [&](PassBuilder& b) { hdr = b.createTexture("HDR", hdrDesc); b.writeColor(hdr, LoadOp::Clear); },
//       [=](PassContext& ctx) { /* record commands */ });
//   graph.addPass("Tonemap", ..., [&](PassBuilder& b) { b.sample(hdr, PS); b.writeColor(backbuffer, LoadOp::DontCare); }, ...);
//   const CompileResult& r = graph.compile();
//   graph.execute(executeContext);
//
// Semantics:
// - Passes execute in declaration order (v1; async-compute scheduling comes later).
// - Passes whose results are never consumed are culled. Roots are passes that write an imported
//   resource, or that call `PassBuilder::setSideEffects()`.
// - Transient resources get their usage flags inferred from their accesses, and are assigned to
//   aliased memory slots when their lifetimes don't overlap.
// - Barriers are derived from declared accesses (Vulkan synchronization2 semantics).
#pragma once

#include "saige/rhi/barrier.h"
#include "saige/rhi/handle.h"
#include "saige/rhi/resource_desc.h"

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

inline constexpr uint32_t kInvalidResource = 0xFFFFFFFFu;

/// Virtual texture handle, valid only within the graph that created it.
struct RGTexture {
    uint32_t id = kInvalidResource;
    [[nodiscard]] constexpr bool isValid() const noexcept { return id != kInvalidResource; }
    friend constexpr bool operator==(RGTexture, RGTexture) noexcept = default;
};

/// Virtual buffer handle, valid only within the graph that created it.
struct RGBuffer {
    uint32_t id = kInvalidResource;
    [[nodiscard]] constexpr bool isValid() const noexcept { return id != kInvalidResource; }
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
    uint32_t resource = kInvalidResource;
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
    RGTexture createTexture(std::string_view name, const rhi::TextureDesc& desc);
    RGBuffer createBuffer(std::string_view name, const rhi::BufferDesc& desc);

    // -- Texture accesses -----------------------------------------------------------------------------------------
    void writeColor(RGTexture tex, LoadOp load = LoadOp::Load);
    void writeDepth(RGTexture tex, LoadOp load = LoadOp::Load);
    void readDepth(RGTexture tex);  ///< Depth test without writes (read-only depth attachment).
    void sample(RGTexture tex, rhi::PipelineStage shaderStages);
    void readStorage(RGTexture tex, rhi::PipelineStage shaderStages);
    void writeStorage(RGTexture tex, rhi::PipelineStage shaderStages, bool fullOverwrite = false);
    void copySrc(RGTexture tex);
    void copyDst(RGTexture tex, bool fullOverwrite = true);

    // -- Buffer accesses ------------------------------------------------------------------------------------------
    void readBuffer(RGBuffer buf, BufferReadKind kind, rhi::PipelineStage shaderStages = rhi::PipelineStage::None);
    void writeStorage(RGBuffer buf, rhi::PipelineStage shaderStages, bool fullOverwrite = false);
    void copySrc(RGBuffer buf);
    void copyDst(RGBuffer buf, bool fullOverwrite = true);

    // -- Escape hatches -------------------------------------------------------------------------------------------
    void access(RGTexture tex, const rhi::ResourceState& state, bool discard = false);
    void access(RGBuffer buf, const rhi::ResourceState& state, bool discard = false);

    /// The pass must never be culled (readbacks, debug output, GPU timestamps).
    void setSideEffects();

private:
    friend class RenderGraph;
    PassBuilder(RenderGraph& graph, uint32_t passIndex) : m_graph(graph), m_passIndex(passIndex) {}
    void addAccess(uint32_t resource, bool expectTexture, const rhi::ResourceState& state, bool discard);

    RenderGraph& m_graph;
    uint32_t m_passIndex;
};

/// Handed to a pass's execute callback.
class PassContext {
public:
    [[nodiscard]] rhi::TextureHandle texture(RGTexture tex) const;
    [[nodiscard]] rhi::BufferHandle buffer(RGBuffer buf) const;
    [[nodiscard]] const rhi::TextureDesc& textureDesc(RGTexture tex) const;
    [[nodiscard]] const rhi::BufferDesc& bufferDesc(RGBuffer buf) const;
    /// May be null (null backend, unit tests).
    [[nodiscard]] rhi::CommandList* commandList() const noexcept { return m_commandList; }
    [[nodiscard]] std::string_view passName() const noexcept { return m_passName; }

private:
    friend class RenderGraph;
    PassContext(const RenderGraph& graph, rhi::CommandList* cmd, std::string_view name,
                std::span<const rhi::TextureHandle> textures, std::span<const rhi::BufferHandle> buffers)
        : m_graph(graph), m_commandList(cmd), m_passName(name), m_textures(textures), m_buffers(buffers)
    {
    }

    const RenderGraph& m_graph;
    rhi::CommandList* m_commandList;
    std::string_view m_passName;
    std::span<const rhi::TextureHandle> m_textures;
    std::span<const rhi::BufferHandle> m_buffers;
};

// ---------------------------------------------------------------------------------------------------------------------
// Compilation output
// ---------------------------------------------------------------------------------------------------------------------

/// A barrier on a virtual resource, resolved to an rhi::TextureBarrier / rhi::BufferBarrier at execute time.
struct GraphBarrier {
    uint32_t resource = kInvalidResource;
    rhi::ResourceState before;
    rhi::ResourceState after;
};

struct CompiledPass {
    uint32_t passIndex = 0;  ///< Index in declaration order.
    std::vector<GraphBarrier> barriersBefore;
};

struct ResourceLifetime {
    uint32_t firstPass = kInvalidResource;  ///< Position in CompileResult::passes (execution order).
    uint32_t lastPass = kInvalidResource;
    [[nodiscard]] constexpr bool used() const noexcept { return firstPass != kInvalidResource; }
};

/// Placement of a transient resource in aliased memory.
struct TransientAllocation {
    uint32_t slot = kInvalidResource;              ///< Memory slot shared by non-overlapping resources.
    uint64_t sizeBytes = 0;                        ///< Estimated (or backend-reported) memory size.
    uint32_t aliasPredecessor = kInvalidResource;  ///< Resource that occupied this slot before (if any).
};

struct CompileResult {
    bool ok = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;

    std::vector<CompiledPass> passes;   ///< Surviving passes in execution order.
    std::vector<uint32_t> culledPasses;  ///< Declaration indices of culled passes.
    std::vector<GraphBarrier> finalBarriers;  ///< Transitions of imported resources to their final states.

    std::vector<ResourceLifetime> lifetimes;          ///< Indexed by resource id.
    std::vector<TransientAllocation> allocations;     ///< Indexed by resource id (slot invalid for imported/unused).
    std::vector<uint64_t> slotSizes;                  ///< Size of each aliased memory slot.
    uint64_t transientBytesUnaliased = 0;             ///< Sum of all used transient resources.
    uint64_t transientBytesAliased = 0;               ///< Sum of slot sizes.
};

/// Options affecting compilation.
struct CompileOptions {
    /// Memory size estimate for transient textures. The Vulkan backend can supply exact sizes from
    /// vkGetDeviceImageMemoryRequirements (Vulkan 1.3). The default estimate is the packed data size
    /// rounded up to 64 KiB.
    std::function<uint64_t(const rhi::TextureDesc&)> textureSize;
    std::function<uint64_t(const rhi::BufferDesc&)> bufferSize;
    bool enableAliasing = true;
};

/// Resources supplied by the executor.
struct ExecuteContext {
    rhi::CommandList* commandList = nullptr;
    /// Physical handles for transient resources, indexed by resource id (may be empty in tests).
    /// Imported resources always use the handle given at import.
    std::span<const rhi::TextureHandle> transientTextures;
    std::span<const rhi::BufferHandle> transientBuffers;
    /// Receives each pass's barriers before its execute callback runs, and the final barriers at the end.
    std::function<void(std::span<const rhi::TextureBarrier>, std::span<const rhi::BufferBarrier>)> submitBarriers;
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
    uint32_t addPass(std::string_view name, rhi::QueueType queue, SetupFn&& setup, ExecuteFn execute = {})
    {
        const uint32_t index = beginPass(name, queue, std::move(execute));
        PassBuilder builder(*this, index);
        setup(builder);
        return index;
    }

    /// Imports an externally owned texture (swapchain image, history buffer, persistent atlas).
    /// `finalState`: the state the texture must be left in after the graph (e.g., Present).
    RGTexture importTexture(std::string_view name, const rhi::TextureDesc& desc, rhi::TextureHandle physical,
                            const rhi::ResourceState& initialState,
                            std::optional<rhi::ResourceState> finalState = std::nullopt);
    RGBuffer importBuffer(std::string_view name, const rhi::BufferDesc& desc, rhi::BufferHandle physical,
                          const rhi::ResourceState& initialState,
                          std::optional<rhi::ResourceState> finalState = std::nullopt);

    const CompileResult& compile(const CompileOptions& options = {});
    [[nodiscard]] const CompileResult& compiled() const noexcept { return m_result; }

    /// Runs surviving passes in order, emitting barriers through `ctx.submitBarriers`. Requires a
    /// successful compile().
    void execute(const ExecuteContext& ctx);

    /// Clears all passes and resources for the next frame (keeps allocations).
    void reset();

    /// Graphviz DOT dump of passes, resources and dependencies (culled passes shown dashed).
    [[nodiscard]] std::string toGraphviz() const;

    [[nodiscard]] uint32_t passCount() const noexcept { return static_cast<uint32_t>(m_passes.size()); }
    [[nodiscard]] uint32_t resourceCount() const noexcept { return static_cast<uint32_t>(m_resources.size()); }
    [[nodiscard]] std::string_view passName(uint32_t passIndex) const { return m_passes.at(passIndex).name; }
    [[nodiscard]] std::string_view resourceName(uint32_t resource) const { return m_resources.at(resource).name; }
    /// Final description of a texture (transient usage flags are filled in after compile()).
    [[nodiscard]] const rhi::TextureDesc& textureDesc(RGTexture tex) const { return m_resources.at(tex.id).texture; }
    [[nodiscard]] const rhi::BufferDesc& bufferDesc(RGBuffer buf) const { return m_resources.at(buf.id).buffer; }
    [[nodiscard]] bool isImported(uint32_t resource) const { return m_resources.at(resource).imported; }

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
        rhi::TextureHandle physicalTexture;
        rhi::BufferHandle physicalBuffer;
        rhi::ResourceState initialState;
        std::optional<rhi::ResourceState> finalState;
        uint32_t creatorPass = kInvalidResource;
    };

    struct Pass {
        std::string name;
        rhi::QueueType queue = rhi::QueueType::Graphics;
        ExecuteFn execute;
        std::vector<ResourceAccess> accesses;  // merged: at most one entry per resource
        bool sideEffects = false;
    };

    uint32_t beginPass(std::string_view name, rhi::QueueType queue, ExecuteFn execute);
    void addError(std::string message);

    std::vector<Pass> m_passes;
    std::vector<Resource> m_resources;
    std::vector<std::string> m_setupErrors;
    CompileResult m_result;
    bool m_compiled = false;
};

} // namespace saige::render
