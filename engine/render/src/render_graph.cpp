#include "saige/render/render_graph.h"

#include "saige/rhi/config.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace saige::render {

using rhi::Access;
using rhi::PipelineStage;
using rhi::ResourceState;
using rhi::TextureLayout;

namespace {

constexpr uint64_t alignUp(uint64_t value, uint64_t alignment) noexcept
{
    return (value + alignment - 1u) / alignment * alignment;
}

uint64_t defaultTextureSize(const rhi::TextureDesc& desc)
{
    return alignUp(rhi::computeTextureDataSize(desc), 64u * 1024u);
}

uint64_t defaultBufferSize(const rhi::BufferDesc& desc)
{
    return alignUp(desc.size, 256u);
}

constexpr bool isAttachmentLayout(TextureLayout l) noexcept
{
    return l == TextureLayout::ColorAttachment || l == TextureLayout::DepthStencilAttachment;
}

/// Merges two layouts requested for the same texture within one pass. Returns nullopt on a conflict
/// (e.g., sampling a texture while rendering to it).
std::optional<TextureLayout> mergeLayouts(TextureLayout a, TextureLayout b) noexcept
{
    if (a == b) {
        return a;
    }
    const auto isReadOnlyPair = [](TextureLayout x, TextureLayout y) {
        return x == TextureLayout::DepthStencilReadOnly && y == TextureLayout::ShaderReadOnly;
    };
    if (isReadOnlyPair(a, b) || isReadOnlyPair(b, a)) {
        return TextureLayout::DepthStencilReadOnly;  // depth test + sampling in the same pass is legal
    }
    if (isAttachmentLayout(a) || isAttachmentLayout(b) || a == TextureLayout::Present || b == TextureLayout::Present ||
        a == TextureLayout::Undefined || b == TextureLayout::Undefined) {
        return std::nullopt;
    }
    return TextureLayout::General;
}

rhi::TextureUsage inferTextureUsage(Access a) noexcept
{
    rhi::TextureUsage u = rhi::TextureUsage::None;
    if (rhi::hasAny(a, Access::ColorAttachmentRead | Access::ColorAttachmentWrite)) {
        u |= rhi::TextureUsage::ColorAttachment;
    }
    if (rhi::hasAny(a, Access::DepthStencilRead | Access::DepthStencilWrite)) {
        u |= rhi::TextureUsage::DepthStencilAttachment;
    }
    if (rhi::hasAny(a, Access::ShaderSampledRead)) {
        u |= rhi::TextureUsage::Sampled;
    }
    if (rhi::hasAny(a, Access::ShaderStorageRead | Access::ShaderStorageWrite)) {
        u |= rhi::TextureUsage::Storage;
    }
    if (rhi::hasAny(a, Access::TransferRead)) {
        u |= rhi::TextureUsage::TransferSrc;
    }
    if (rhi::hasAny(a, Access::TransferWrite)) {
        u |= rhi::TextureUsage::TransferDst;
    }
    return u;
}

rhi::BufferUsage inferBufferUsage(Access a) noexcept
{
    rhi::BufferUsage u = rhi::BufferUsage::None;
    if (rhi::hasAny(a, Access::VertexAttributeRead)) {
        u |= rhi::BufferUsage::Vertex;
    }
    if (rhi::hasAny(a, Access::IndexRead)) {
        u |= rhi::BufferUsage::Index;
    }
    if (rhi::hasAny(a, Access::IndirectCommandRead)) {
        u |= rhi::BufferUsage::Indirect;
    }
    if (rhi::hasAny(a, Access::UniformRead)) {
        u |= rhi::BufferUsage::Uniform;
    }
    if (rhi::hasAny(a, Access::ShaderStorageRead | Access::ShaderStorageWrite)) {
        u |= rhi::BufferUsage::Storage;
    }
    if (rhi::hasAny(a, Access::TransferRead)) {
        u |= rhi::BufferUsage::TransferSrc;
    }
    if (rhi::hasAny(a, Access::TransferWrite)) {
        u |= rhi::BufferUsage::TransferDst;
    }
    return u;
}

/// Synchronization state of one resource while barriers are being generated.
struct TrackedState {
    TextureLayout layout = TextureLayout::Undefined;
    PipelineStage writeStages = PipelineStage::None;
    Access writeAccess = Access::None;
    PipelineStage readStages = PipelineStage::None;  // readers since the last write / layout change
    Access readAccess = Access::None;
};

TrackedState trackedFromInitial(const ResourceState& s) noexcept
{
    TrackedState t;
    t.layout = s.layout;
    if (s.writes()) {
        t.writeStages = s.stages;
        t.writeAccess = rhi::writeAccessOnly(s.access);
    } else {
        t.readStages = s.stages;
        t.readAccess = s.access;
    }
    return t;
}

/// Computes the barrier (if any) needed to move `t` to `desired`, and updates `t`.
std::optional<GraphBarrier> transition(uint32_t resource, bool isTexture, TrackedState& t,
                                       const ResourceState& desired, bool discard)
{
    const bool layoutChange = isTexture && desired.layout != t.layout;
    const bool hasPendingWrite = t.writeStages != PipelineStage::None || t.writeAccess != Access::None;

    bool needed = false;
    if (desired.writes()) {
        // WAW and WAR hazards: wait for prior writers and readers.
        needed = layoutChange || hasPendingWrite || t.readStages != PipelineStage::None;
    } else {
        const bool covered = rhi::hasAll(t.readStages, desired.stages) && rhi::hasAll(t.readAccess, desired.access);
        needed = layoutChange || (hasPendingWrite && !covered);
    }

    std::optional<GraphBarrier> barrier;
    if (needed) {
        GraphBarrier b;
        b.resource = resource;
        // Source scope includes prior readers so the dependency chains through earlier layout transitions.
        b.before.stages = t.writeStages | t.readStages;
        b.before.access = t.writeAccess;
        b.before.layout = isTexture ? (discard ? TextureLayout::Undefined : t.layout) : TextureLayout::Undefined;
        b.after = desired;
        if (!isTexture) {
            b.after.layout = TextureLayout::Undefined;
        }
        barrier = b;
    }

    if (desired.writes()) {
        t.writeStages = desired.stages;
        t.writeAccess = rhi::writeAccessOnly(desired.access);
        t.readStages = PipelineStage::None;
        t.readAccess = Access::None;
    } else if (layoutChange) {
        t.readStages = desired.stages;
        t.readAccess = desired.access;
    } else {
        t.readStages |= desired.stages;
        t.readAccess |= desired.access;
    }
    if (isTexture) {
        t.layout = desired.layout;
    }
    return barrier;
}

std::string escapeDot(std::string_view s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

} // namespace

// =====================================================================================================================
// PassBuilder
// =====================================================================================================================

RGTexture PassBuilder::createTexture(std::string_view name, const rhi::TextureDesc& desc)
{
    RenderGraph::Resource res;
    res.name = std::string(name);
    res.kind = RenderGraph::ResourceKind::Texture;
    res.texture = desc;
    res.creatorPass = m_passIndex;
    m_graph.m_resources.push_back(std::move(res));
    return RGTexture{static_cast<uint32_t>(m_graph.m_resources.size() - 1)};
}

RGBuffer PassBuilder::createBuffer(std::string_view name, const rhi::BufferDesc& desc)
{
    RenderGraph::Resource res;
    res.name = std::string(name);
    res.kind = RenderGraph::ResourceKind::Buffer;
    res.buffer = desc;
    res.creatorPass = m_passIndex;
    m_graph.m_resources.push_back(std::move(res));
    return RGBuffer{static_cast<uint32_t>(m_graph.m_resources.size() - 1)};
}

void PassBuilder::writeColor(RGTexture tex, LoadOp load)
{
    const bool preserve = load == LoadOp::Load;
    Access access = Access::ColorAttachmentWrite;
    if (preserve) {
        access |= Access::ColorAttachmentRead;
    }
    addAccess(tex.id, true, {PipelineStage::ColorAttachmentOutput, access, TextureLayout::ColorAttachment}, !preserve);
}

void PassBuilder::writeDepth(RGTexture tex, LoadOp load)
{
    const bool preserve = load == LoadOp::Load;
    Access access = Access::DepthStencilWrite;
    if (preserve) {
        access |= Access::DepthStencilRead;
    }
    addAccess(tex.id, true,
              {PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests, access,
               TextureLayout::DepthStencilAttachment},
              !preserve);
}

void PassBuilder::readDepth(RGTexture tex)
{
    addAccess(tex.id, true,
              {PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests, Access::DepthStencilRead,
               TextureLayout::DepthStencilReadOnly},
              false);
}

void PassBuilder::sample(RGTexture tex, PipelineStage shaderStages)
{
    addAccess(tex.id, true, {shaderStages, Access::ShaderSampledRead, TextureLayout::ShaderReadOnly}, false);
}

void PassBuilder::readStorage(RGTexture tex, PipelineStage shaderStages)
{
    addAccess(tex.id, true, {shaderStages, Access::ShaderStorageRead, TextureLayout::General}, false);
}

void PassBuilder::writeStorage(RGTexture tex, PipelineStage shaderStages, bool fullOverwrite)
{
    Access access = Access::ShaderStorageWrite;
    if (!fullOverwrite) {
        access |= Access::ShaderStorageRead;
    }
    addAccess(tex.id, true, {shaderStages, access, TextureLayout::General}, fullOverwrite);
}

void PassBuilder::copySrc(RGTexture tex)
{
    addAccess(tex.id, true, {PipelineStage::Transfer, Access::TransferRead, TextureLayout::TransferSrc}, false);
}

void PassBuilder::copyDst(RGTexture tex, bool fullOverwrite)
{
    addAccess(tex.id, true, {PipelineStage::Transfer, Access::TransferWrite, TextureLayout::TransferDst}, fullOverwrite);
}

void PassBuilder::readBuffer(RGBuffer buf, BufferReadKind kind, PipelineStage shaderStages)
{
    ResourceState s;
    switch (kind) {
    case BufferReadKind::Vertex:
        s = {PipelineStage::VertexInput, Access::VertexAttributeRead, TextureLayout::Undefined};
        break;
    case BufferReadKind::Index:
        s = {PipelineStage::VertexInput, Access::IndexRead, TextureLayout::Undefined};
        break;
    case BufferReadKind::Indirect:
        s = {PipelineStage::DrawIndirect, Access::IndirectCommandRead, TextureLayout::Undefined};
        break;
    case BufferReadKind::Uniform:
        s = {shaderStages, Access::UniformRead, TextureLayout::Undefined};
        break;
    case BufferReadKind::Storage:
        s = {shaderStages, Access::ShaderStorageRead, TextureLayout::Undefined};
        break;
    }
    if (s.stages == PipelineStage::None) {
        m_graph.addError("Pass '" + m_graph.m_passes[m_passIndex].name +
                         "': readBuffer(Uniform/Storage) requires shader stages");
        return;
    }
    addAccess(buf.id, false, s, false);
}

void PassBuilder::writeStorage(RGBuffer buf, PipelineStage shaderStages, bool fullOverwrite)
{
    Access access = Access::ShaderStorageWrite;
    if (!fullOverwrite) {
        access |= Access::ShaderStorageRead;
    }
    addAccess(buf.id, false, {shaderStages, access, TextureLayout::Undefined}, fullOverwrite);
}

void PassBuilder::copySrc(RGBuffer buf)
{
    addAccess(buf.id, false, {PipelineStage::Transfer, Access::TransferRead, TextureLayout::Undefined}, false);
}

void PassBuilder::copyDst(RGBuffer buf, bool fullOverwrite)
{
    addAccess(buf.id, false, {PipelineStage::Transfer, Access::TransferWrite, TextureLayout::Undefined}, fullOverwrite);
}

void PassBuilder::access(RGTexture tex, const ResourceState& state, bool discard)
{
    addAccess(tex.id, true, state, discard && state.writes());
}

void PassBuilder::access(RGBuffer buf, const ResourceState& state, bool discard)
{
    ResourceState s = state;
    s.layout = TextureLayout::Undefined;
    addAccess(buf.id, false, s, discard && state.writes());
}

void PassBuilder::setSideEffects()
{
    m_graph.m_passes[m_passIndex].sideEffects = true;
}

void PassBuilder::addAccess(uint32_t resource, bool expectTexture, const ResourceState& state, bool discard)
{
    RenderGraph::Pass& pass = m_graph.m_passes[m_passIndex];
    if (resource >= m_graph.m_resources.size()) {
        m_graph.addError("Pass '" + pass.name + "': access to an invalid resource handle");
        return;
    }
    const RenderGraph::Resource& res = m_graph.m_resources[resource];
    const bool isTexture = res.kind == RenderGraph::ResourceKind::Texture;
    if (isTexture != expectTexture) {
        m_graph.addError("Pass '" + pass.name + "': resource '" + res.name + "' accessed as the wrong kind");
        return;
    }

    for (ResourceAccess& existing : pass.accesses) {
        if (existing.resource != resource) {
            continue;
        }
        existing.state.stages |= state.stages;
        existing.state.access |= state.access;
        existing.discard = existing.discard && discard;
        if (isTexture) {
            const std::optional<TextureLayout> merged = mergeLayouts(existing.state.layout, state.layout);
            if (!merged) {
                m_graph.addError("Pass '" + pass.name + "': conflicting accesses to texture '" + res.name +
                                 "' (e.g., sampling a texture that is also an attachment)");
                return;
            }
            existing.state.layout = *merged;
        }
        return;
    }
    pass.accesses.push_back({resource, state, discard});
}

// =====================================================================================================================
// PassContext
// =====================================================================================================================

rhi::TextureHandle PassContext::texture(RGTexture tex) const
{
    const auto& res = m_graph.m_resources.at(tex.id);
    if (res.imported) {
        return res.physicalTexture;
    }
    return tex.id < m_textures.size() ? m_textures[tex.id] : rhi::TextureHandle{};
}

rhi::BufferHandle PassContext::buffer(RGBuffer buf) const
{
    const auto& res = m_graph.m_resources.at(buf.id);
    if (res.imported) {
        return res.physicalBuffer;
    }
    return buf.id < m_buffers.size() ? m_buffers[buf.id] : rhi::BufferHandle{};
}

const rhi::TextureDesc& PassContext::textureDesc(RGTexture tex) const
{
    return m_graph.textureDesc(tex);
}

const rhi::BufferDesc& PassContext::bufferDesc(RGBuffer buf) const
{
    return m_graph.bufferDesc(buf);
}

// =====================================================================================================================
// RenderGraph
// =====================================================================================================================

uint32_t RenderGraph::beginPass(std::string_view name, rhi::QueueType queue, ExecuteFn execute)
{
    m_compiled = false;
    Pass pass;
    pass.name = std::string(name);
    pass.queue = queue;
    pass.execute = std::move(execute);
    m_passes.push_back(std::move(pass));
    return static_cast<uint32_t>(m_passes.size() - 1);
}

void RenderGraph::addError(std::string message)
{
    m_setupErrors.push_back(std::move(message));
}

RGTexture RenderGraph::importTexture(std::string_view name, const rhi::TextureDesc& desc, rhi::TextureHandle physical,
                                     const ResourceState& initialState, std::optional<ResourceState> finalState)
{
    m_compiled = false;
    Resource res;
    res.name = std::string(name);
    res.kind = ResourceKind::Texture;
    res.imported = true;
    res.texture = desc;
    res.physicalTexture = physical;
    res.initialState = initialState;
    res.finalState = finalState;
    m_resources.push_back(std::move(res));
    return RGTexture{static_cast<uint32_t>(m_resources.size() - 1)};
}

RGBuffer RenderGraph::importBuffer(std::string_view name, const rhi::BufferDesc& desc, rhi::BufferHandle physical,
                                   const ResourceState& initialState, std::optional<ResourceState> finalState)
{
    m_compiled = false;
    Resource res;
    res.name = std::string(name);
    res.kind = ResourceKind::Buffer;
    res.imported = true;
    res.buffer = desc;
    res.physicalBuffer = physical;
    res.initialState = initialState;
    res.initialState.layout = TextureLayout::Undefined;
    res.finalState = finalState;
    if (res.finalState) {
        res.finalState->layout = TextureLayout::Undefined;
    }
    m_resources.push_back(std::move(res));
    return RGBuffer{static_cast<uint32_t>(m_resources.size() - 1)};
}

const CompileResult& RenderGraph::compile(const CompileOptions& options)
{
    m_result = CompileResult{};
    CompileResult& r = m_result;
    r.errors = m_setupErrors;

    const auto passCount = static_cast<uint32_t>(m_passes.size());
    const auto resCount = static_cast<uint32_t>(m_resources.size());

    // ---------------------------------------------------------------------------------------------------------
    // 1. Data dependencies and culling
    // ---------------------------------------------------------------------------------------------------------
    std::vector<uint32_t> lastWriter(resCount, kInvalidResource);
    std::vector<std::vector<uint32_t>> deps(passCount);
    std::vector<bool> isRoot(passCount, false);
    std::vector<std::pair<uint32_t, uint32_t>> uninitializedReads;  // (pass, resource)

    for (uint32_t p = 0; p < passCount; ++p) {
        const Pass& pass = m_passes[p];
        isRoot[p] = pass.sideEffects;
        for (const ResourceAccess& a : pass.accesses) {
            if (!a.discard) {  // needs the previous contents
                if (lastWriter[a.resource] != kInvalidResource) {
                    deps[p].push_back(lastWriter[a.resource]);
                } else if (!m_resources[a.resource].imported) {
                    uninitializedReads.emplace_back(p, a.resource);
                }
            }
            if (a.state.writes()) {
                lastWriter[a.resource] = p;
                if (m_resources[a.resource].imported) {
                    isRoot[p] = true;
                }
            }
        }
    }

    std::vector<bool> needed(passCount, false);
    std::vector<uint32_t> stack;
    for (uint32_t p = 0; p < passCount; ++p) {
        if (isRoot[p]) {
            needed[p] = true;
            stack.push_back(p);
        }
    }
    while (!stack.empty()) {
        const uint32_t p = stack.back();
        stack.pop_back();
        for (uint32_t d : deps[p]) {
            if (!needed[d]) {
                needed[d] = true;
                stack.push_back(d);
            }
        }
    }

    std::vector<uint32_t> position(passCount, kInvalidResource);
    for (uint32_t p = 0; p < passCount; ++p) {
        if (needed[p]) {
            position[p] = static_cast<uint32_t>(r.passes.size());
            r.passes.push_back(CompiledPass{p, {}});
        } else {
            r.culledPasses.push_back(p);
        }
    }

    for (const auto& [p, res] : uninitializedReads) {
        if (needed[p]) {
            r.warnings.push_back("Pass '" + m_passes[p].name + "' reads transient resource '" + m_resources[res].name +
                                 "' before anything writes it (contents are undefined)");
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // 2. Lifetimes, usage inference and validation
    // ---------------------------------------------------------------------------------------------------------
    r.lifetimes.assign(resCount, ResourceLifetime{});
    std::vector<rhi::TextureUsage> texUsage(resCount, rhi::TextureUsage::None);
    std::vector<rhi::BufferUsage> bufUsage(resCount, rhi::BufferUsage::None);

    for (uint32_t pos = 0; pos < r.passes.size(); ++pos) {
        for (const ResourceAccess& a : m_passes[r.passes[pos].passIndex].accesses) {
            ResourceLifetime& life = r.lifetimes[a.resource];
            if (!life.used()) {
                life.firstPass = pos;
            }
            life.lastPass = pos;
            if (m_resources[a.resource].kind == ResourceKind::Texture) {
                texUsage[a.resource] |= inferTextureUsage(a.state.access);
            } else {
                bufUsage[a.resource] |= inferBufferUsage(a.state.access);
            }
        }
    }

    for (uint32_t i = 0; i < resCount; ++i) {
        Resource& res = m_resources[i];
        if (!r.lifetimes[i].used()) {
            continue;
        }
        if (res.kind == ResourceKind::Texture) {
            if (res.imported) {
                if (!rhi::hasAll(res.texture.usage, texUsage[i])) {
                    r.errors.push_back("Imported texture '" + res.name +
                                       "' is accessed in ways its usage flags do not allow");
                }
            } else {
                res.texture.usage |= texUsage[i];
                if (const rhi::ValidationResult v = rhi::validate(res.texture); !v) {
                    r.errors.push_back("Transient texture '" + res.name + "': " + v.error);
                }
            }
        } else {
            if (res.imported) {
                if (!rhi::hasAll(res.buffer.usage, bufUsage[i])) {
                    r.errors.push_back("Imported buffer '" + res.name +
                                       "' is accessed in ways its usage flags do not allow");
                }
            } else {
                res.buffer.usage |= bufUsage[i];
                if (const rhi::ValidationResult v = rhi::validate(res.buffer); !v) {
                    r.errors.push_back("Transient buffer '" + res.name + "': " + v.error);
                }
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // 3. Transient memory aliasing (greedy interval assignment, best fit)
    // ---------------------------------------------------------------------------------------------------------
    r.allocations.assign(resCount, TransientAllocation{});
    {
        struct Slot {
            ResourceKind kind;
            uint64_t size;
            uint32_t lastUse;
            uint32_t occupant;
        };
        std::vector<Slot> slots;
        std::vector<uint32_t> order;
        for (uint32_t i = 0; i < resCount; ++i) {
            if (!m_resources[i].imported && r.lifetimes[i].used()) {
                order.push_back(i);
            }
        }
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return r.lifetimes[a].firstPass < r.lifetimes[b].firstPass;
        });

        for (uint32_t id : order) {
            const Resource& res = m_resources[id];
            const ResourceLifetime& life = r.lifetimes[id];
            uint64_t size = 0;
            if (res.kind == ResourceKind::Texture) {
                size = options.textureSize ? options.textureSize(res.texture) : defaultTextureSize(res.texture);
            } else {
                size = options.bufferSize ? options.bufferSize(res.buffer) : defaultBufferSize(res.buffer);
            }
            r.transientBytesUnaliased += size;

            uint32_t chosen = kInvalidResource;
            if (options.enableAliasing) {
                uint32_t bestFit = kInvalidResource;
                uint32_t largest = kInvalidResource;
                for (uint32_t s = 0; s < slots.size(); ++s) {
                    const Slot& slot = slots[s];
                    if (slot.kind != res.kind || slot.lastUse >= life.firstPass) {
                        continue;
                    }
                    if (slot.size >= size && (bestFit == kInvalidResource || slot.size < slots[bestFit].size)) {
                        bestFit = s;
                    }
                    if (largest == kInvalidResource || slot.size > slots[largest].size) {
                        largest = s;
                    }
                }
                chosen = bestFit != kInvalidResource ? bestFit : largest;
            }

            TransientAllocation& alloc = r.allocations[id];
            alloc.sizeBytes = size;
            if (chosen == kInvalidResource) {
                chosen = static_cast<uint32_t>(slots.size());
                slots.push_back(Slot{res.kind, size, life.lastPass, id});
            } else {
                Slot& slot = slots[chosen];
                alloc.aliasPredecessor = slot.occupant;
                slot.size = std::max(slot.size, size);
                slot.lastUse = life.lastPass;
                slot.occupant = id;
            }
            alloc.slot = chosen;
        }

        for (const Slot& s : slots) {
            r.slotSizes.push_back(s.size);
            r.transientBytesAliased += s.size;
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // 4. Barrier generation
    // ---------------------------------------------------------------------------------------------------------
    std::vector<TrackedState> tracked(resCount);
    for (uint32_t i = 0; i < resCount; ++i) {
        if (m_resources[i].imported) {
            tracked[i] = trackedFromInitial(m_resources[i].initialState);
        }
    }

    for (uint32_t pos = 0; pos < r.passes.size(); ++pos) {
        CompiledPass& cp = r.passes[pos];
        for (const ResourceAccess& a : m_passes[cp.passIndex].accesses) {
            const Resource& res = m_resources[a.resource];
            const bool isTexture = res.kind == ResourceKind::Texture;
            TrackedState& t = tracked[a.resource];

            // First use of an aliased transient: wait for the previous occupant of the memory.
            if (!res.imported && r.lifetimes[a.resource].firstPass == pos) {
                const uint32_t pred = r.allocations[a.resource].aliasPredecessor;
                if (pred != kInvalidResource) {
                    const TrackedState& pt = tracked[pred];
                    t.writeStages = pt.writeStages | pt.readStages;
                    t.writeAccess = pt.writeAccess;
                    t.layout = TextureLayout::Undefined;
                }
            }

            if (auto b = transition(a.resource, isTexture, t, a.state, a.discard)) {
                cp.barriersBefore.push_back(*b);
            }
        }
    }

    for (uint32_t i = 0; i < resCount; ++i) {
        const Resource& res = m_resources[i];
        if (res.imported && res.finalState) {
            if (auto b = transition(i, res.kind == ResourceKind::Texture, tracked[i], *res.finalState, false)) {
                r.finalBarriers.push_back(*b);
            }
        }
    }

    r.ok = r.errors.empty();
    m_compiled = true;
    return r;
}

void RenderGraph::execute(const ExecuteContext& ctx)
{
    SAIGE_RHI_ASSERT(m_compiled && m_result.ok, "RenderGraph::execute requires a successful compile()");
    if (!m_compiled || !m_result.ok) {
        return;
    }

    std::vector<rhi::TextureBarrier> texBarriers;
    std::vector<rhi::BufferBarrier> bufBarriers;

    auto resolveAndSubmit = [&](const std::vector<GraphBarrier>& barriers) {
        if (barriers.empty() || !ctx.submitBarriers) {
            return;
        }
        texBarriers.clear();
        bufBarriers.clear();
        for (const GraphBarrier& gb : barriers) {
            const Resource& res = m_resources[gb.resource];
            if (res.kind == ResourceKind::Texture) {
                rhi::TextureBarrier tb;
                tb.texture = res.imported ? res.physicalTexture
                                          : (gb.resource < ctx.transientTextures.size() ? ctx.transientTextures[gb.resource]
                                                                                        : rhi::TextureHandle{});
                tb.before = gb.before;
                tb.after = gb.after;
                texBarriers.push_back(tb);
            } else {
                rhi::BufferBarrier bb;
                bb.buffer = res.imported ? res.physicalBuffer
                                         : (gb.resource < ctx.transientBuffers.size() ? ctx.transientBuffers[gb.resource]
                                                                                      : rhi::BufferHandle{});
                bb.before = gb.before;
                bb.after = gb.after;
                bufBarriers.push_back(bb);
            }
        }
        ctx.submitBarriers(texBarriers, bufBarriers);
    };

    for (const CompiledPass& cp : m_result.passes) {
        resolveAndSubmit(cp.barriersBefore);
        Pass& pass = m_passes[cp.passIndex];
        if (pass.execute) {
            PassContext pc(*this, ctx.commandList, pass.name, ctx.transientTextures, ctx.transientBuffers);
            pass.execute(pc);
        }
    }
    resolveAndSubmit(m_result.finalBarriers);
}

void RenderGraph::reset()
{
    m_passes.clear();
    m_resources.clear();
    m_setupErrors.clear();
    m_result = CompileResult{};
    m_compiled = false;
}

std::string RenderGraph::toGraphviz() const
{
    std::vector<bool> culled(m_passes.size(), false);
    if (m_compiled) {
        for (uint32_t p : m_result.culledPasses) {
            culled[p] = true;
        }
    }

    std::string out = "digraph RenderGraph {\n  rankdir=LR;\n  node [fontname=\"Helvetica\", fontsize=10];\n";
    char line[512];
    for (uint32_t p = 0; p < m_passes.size(); ++p) {
        std::snprintf(line, sizeof(line),
                      "  p%u [shape=box, style=\"rounded,filled%s\", fillcolor=\"%s\", label=\"%s\"];\n", p,
                      culled[p] ? ",dashed" : "", culled[p] ? "#eeeeee" : "#cfe2ff",
                      escapeDot(m_passes[p].name).c_str());
        out += line;
    }
    for (uint32_t i = 0; i < m_resources.size(); ++i) {
        const Resource& res = m_resources[i];
        std::string detail;
        if (res.kind == ResourceKind::Texture) {
            std::snprintf(line, sizeof(line), "%ux%u %s", res.texture.width, res.texture.height,
                          std::string(rhi::formatName(res.texture.format)).c_str());
        } else {
            std::snprintf(line, sizeof(line), "%" PRIu64 " bytes", res.buffer.size);
        }
        detail = line;
        std::snprintf(line, sizeof(line), "  r%u [shape=%s, style=filled, fillcolor=\"%s\", label=\"%s\\n%s\"];\n", i,
                      res.kind == ResourceKind::Texture ? "ellipse" : "cylinder",
                      res.imported ? "#ffe8a3" : "#d1f2d1", escapeDot(res.name).c_str(), escapeDot(detail).c_str());
        out += line;
    }
    for (uint32_t p = 0; p < m_passes.size(); ++p) {
        for (const ResourceAccess& a : m_passes[p].accesses) {
            const bool reads = !a.discard;
            if (reads) {
                std::snprintf(line, sizeof(line), "  r%u -> p%u [color=\"#2b6cb0\"];\n", a.resource, p);
                out += line;
            }
            if (a.state.writes()) {
                std::snprintf(line, sizeof(line), "  p%u -> r%u [color=\"#c53030\"];\n", p, a.resource);
                out += line;
            }
        }
    }
    out += "}\n";
    return out;
}

} // namespace saige::render
