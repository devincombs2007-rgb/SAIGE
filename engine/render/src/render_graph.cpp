#include "saige/render/render_graph.hpp"

#include "saige/rhi/config.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>

namespace saige::render {

using rhi::Access;
using rhi::PipelineStage;
using rhi::ResourceState;
using rhi::TextureLayout;

namespace {

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) noexcept
{
    return (value + alignment - 1u) / alignment * alignment;
}

uint64_t default_texture_size(const rhi::TextureDesc& desc)
{
    return align_up(rhi::compute_texture_data_size(desc), 64u * 1024u);
}

uint64_t default_buffer_size(const rhi::BufferDesc& desc)
{
    return align_up(desc.size, 256u);
}

constexpr bool is_attachment_layout(TextureLayout l) noexcept
{
    return l == TextureLayout::ColorAttachment || l == TextureLayout::DepthStencilAttachment;
}

/// Merges two layouts requested for the same texture within one pass. Returns nullopt on a conflict
/// (e.g., sampling a texture while rendering to it).
std::optional<TextureLayout> merge_layouts(TextureLayout a, TextureLayout b) noexcept
{
    if (a == b) {
        return a;
    }
    const auto is_read_only_pair = [](TextureLayout x, TextureLayout y) {
        return x == TextureLayout::DepthStencilReadOnly && y == TextureLayout::ShaderReadOnly;
    };
    if (is_read_only_pair(a, b) || is_read_only_pair(b, a)) {
        return TextureLayout::DepthStencilReadOnly;  // depth test + sampling in the same pass is legal
    }
    if (is_attachment_layout(a) || is_attachment_layout(b) || a == TextureLayout::Present || b == TextureLayout::Present ||
        a == TextureLayout::Undefined || b == TextureLayout::Undefined) {
        return std::nullopt;
    }
    return TextureLayout::General;
}

rhi::TextureUsage infer_texture_usage(Access a) noexcept
{
    rhi::TextureUsage u = rhi::TextureUsage::None;
    if (rhi::has_any(a, Access::ColorAttachmentRead | Access::ColorAttachmentWrite)) {
        u |= rhi::TextureUsage::ColorAttachment;
    }
    if (rhi::has_any(a, Access::DepthStencilRead | Access::DepthStencilWrite)) {
        u |= rhi::TextureUsage::DepthStencilAttachment;
    }
    if (rhi::has_any(a, Access::ShaderSampledRead)) {
        u |= rhi::TextureUsage::Sampled;
    }
    if (rhi::has_any(a, Access::ShaderStorageRead | Access::ShaderStorageWrite)) {
        u |= rhi::TextureUsage::Storage;
    }
    if (rhi::has_any(a, Access::TransferRead)) {
        u |= rhi::TextureUsage::TransferSrc;
    }
    if (rhi::has_any(a, Access::TransferWrite)) {
        u |= rhi::TextureUsage::TransferDst;
    }
    return u;
}

rhi::BufferUsage infer_buffer_usage(Access a) noexcept
{
    rhi::BufferUsage u = rhi::BufferUsage::None;
    if (rhi::has_any(a, Access::VertexAttributeRead)) {
        u |= rhi::BufferUsage::Vertex;
    }
    if (rhi::has_any(a, Access::IndexRead)) {
        u |= rhi::BufferUsage::Index;
    }
    if (rhi::has_any(a, Access::IndirectCommandRead)) {
        u |= rhi::BufferUsage::Indirect;
    }
    if (rhi::has_any(a, Access::UniformRead)) {
        u |= rhi::BufferUsage::Uniform;
    }
    if (rhi::has_any(a, Access::ShaderStorageRead | Access::ShaderStorageWrite)) {
        u |= rhi::BufferUsage::Storage;
    }
    if (rhi::has_any(a, Access::TransferRead)) {
        u |= rhi::BufferUsage::TransferSrc;
    }
    if (rhi::has_any(a, Access::TransferWrite)) {
        u |= rhi::BufferUsage::TransferDst;
    }
    return u;
}

/// Synchronization state of one resource while barriers are being generated.
struct TrackedState {
    TextureLayout layout = TextureLayout::Undefined;
    PipelineStage write_stages = PipelineStage::None;
    Access write_access = Access::None;
    PipelineStage read_stages = PipelineStage::None;  // readers since the last write / layout change
    Access read_access = Access::None;
};

TrackedState tracked_from_initial(const ResourceState& s) noexcept
{
    TrackedState t;
    t.layout = s.layout;
    if (s.writes()) {
        t.write_stages = s.stages;
        t.write_access = rhi::write_access_only(s.access);
    } else {
        t.read_stages = s.stages;
        t.read_access = s.access;
    }
    return t;
}

/// Computes the barrier (if any) needed to move `t` to `desired`, and updates `t`.
std::optional<GraphBarrier> transition(uint32_t resource, bool is_texture, TrackedState& t,
                                       const ResourceState& desired, bool discard)
{
    const bool layout_change = is_texture && desired.layout != t.layout;
    const bool has_pending_write = t.write_stages != PipelineStage::None || t.write_access != Access::None;

    bool needed = false;
    if (desired.writes()) {
        // WAW and WAR hazards: wait for prior writers and readers.
        needed = layout_change || has_pending_write || t.read_stages != PipelineStage::None;
    } else {
        const bool covered = rhi::has_all(t.read_stages, desired.stages) && rhi::has_all(t.read_access, desired.access);
        needed = layout_change || (has_pending_write && !covered);
    }

    std::optional<GraphBarrier> barrier;
    if (needed) {
        GraphBarrier b;
        b.resource = resource;
        // Source scope includes prior readers so the dependency chains through earlier layout transitions.
        b.before.stages = t.write_stages | t.read_stages;
        b.before.access = t.write_access;
        b.before.layout = is_texture ? (discard ? TextureLayout::Undefined : t.layout) : TextureLayout::Undefined;
        b.after = desired;
        if (!is_texture) {
            b.after.layout = TextureLayout::Undefined;
        }
        barrier = b;
    }

    if (desired.writes()) {
        t.write_stages = desired.stages;
        t.write_access = rhi::write_access_only(desired.access);
        t.read_stages = PipelineStage::None;
        t.read_access = Access::None;
    } else if (layout_change) {
        t.read_stages = desired.stages;
        t.read_access = desired.access;
    } else {
        t.read_stages |= desired.stages;
        t.read_access |= desired.access;
    }
    if (is_texture) {
        t.layout = desired.layout;
    }
    return barrier;
}

std::string escape_dot(std::string_view s)
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

RGTexture PassBuilder::create_texture(std::string_view name, const rhi::TextureDesc& desc)
{
    RenderGraph::Resource res;
    res.name = std::string(name);
    res.kind = RenderGraph::ResourceKind::Texture;
    res.texture = desc;
    res.creator_pass = pass_index_;
    graph_.resources_.push_back(std::move(res));
    return RGTexture{static_cast<uint32_t>(graph_.resources_.size() - 1)};
}

RGBuffer PassBuilder::create_buffer(std::string_view name, const rhi::BufferDesc& desc)
{
    RenderGraph::Resource res;
    res.name = std::string(name);
    res.kind = RenderGraph::ResourceKind::Buffer;
    res.buffer = desc;
    res.creator_pass = pass_index_;
    graph_.resources_.push_back(std::move(res));
    return RGBuffer{static_cast<uint32_t>(graph_.resources_.size() - 1)};
}

void PassBuilder::write_color(RGTexture tex, LoadOp load)
{
    const bool preserve = load == LoadOp::Load;
    Access access = Access::ColorAttachmentWrite;
    if (preserve) {
        access |= Access::ColorAttachmentRead;
    }
    add_access(tex.id, true, {PipelineStage::ColorAttachmentOutput, access, TextureLayout::ColorAttachment}, !preserve);
}

void PassBuilder::write_depth(RGTexture tex, LoadOp load)
{
    const bool preserve = load == LoadOp::Load;
    Access access = Access::DepthStencilWrite;
    if (preserve) {
        access |= Access::DepthStencilRead;
    }
    add_access(tex.id, true,
              {PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests, access,
               TextureLayout::DepthStencilAttachment},
              !preserve);
}

void PassBuilder::read_depth(RGTexture tex)
{
    add_access(tex.id, true,
              {PipelineStage::EarlyFragmentTests | PipelineStage::LateFragmentTests, Access::DepthStencilRead,
               TextureLayout::DepthStencilReadOnly},
              false);
}

void PassBuilder::sample(RGTexture tex, PipelineStage shader_stages)
{
    add_access(tex.id, true, {shader_stages, Access::ShaderSampledRead, TextureLayout::ShaderReadOnly}, false);
}

void PassBuilder::read_storage(RGTexture tex, PipelineStage shader_stages)
{
    add_access(tex.id, true, {shader_stages, Access::ShaderStorageRead, TextureLayout::General}, false);
}

void PassBuilder::write_storage(RGTexture tex, PipelineStage shader_stages, bool full_overwrite)
{
    Access access = Access::ShaderStorageWrite;
    if (!full_overwrite) {
        access |= Access::ShaderStorageRead;
    }
    add_access(tex.id, true, {shader_stages, access, TextureLayout::General}, full_overwrite);
}

void PassBuilder::copy_src(RGTexture tex)
{
    add_access(tex.id, true, {PipelineStage::Transfer, Access::TransferRead, TextureLayout::TransferSrc}, false);
}

void PassBuilder::copy_dst(RGTexture tex, bool full_overwrite)
{
    add_access(tex.id, true, {PipelineStage::Transfer, Access::TransferWrite, TextureLayout::TransferDst}, full_overwrite);
}

void PassBuilder::read_buffer(RGBuffer buf, BufferReadKind kind, PipelineStage shader_stages)
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
        s = {shader_stages, Access::UniformRead, TextureLayout::Undefined};
        break;
    case BufferReadKind::Storage:
        s = {shader_stages, Access::ShaderStorageRead, TextureLayout::Undefined};
        break;
    }
    if (s.stages == PipelineStage::None) {
        graph_.add_error("Pass '" + graph_.passes_[pass_index_].name +
                         "': read_buffer(Uniform/Storage) requires shader stages");
        return;
    }
    add_access(buf.id, false, s, false);
}

void PassBuilder::write_storage(RGBuffer buf, PipelineStage shader_stages, bool full_overwrite)
{
    Access access = Access::ShaderStorageWrite;
    if (!full_overwrite) {
        access |= Access::ShaderStorageRead;
    }
    add_access(buf.id, false, {shader_stages, access, TextureLayout::Undefined}, full_overwrite);
}

void PassBuilder::copy_src(RGBuffer buf)
{
    add_access(buf.id, false, {PipelineStage::Transfer, Access::TransferRead, TextureLayout::Undefined}, false);
}

void PassBuilder::copy_dst(RGBuffer buf, bool full_overwrite)
{
    add_access(buf.id, false, {PipelineStage::Transfer, Access::TransferWrite, TextureLayout::Undefined}, full_overwrite);
}

void PassBuilder::access(RGTexture tex, const ResourceState& state, bool discard)
{
    add_access(tex.id, true, state, discard && state.writes());
}

void PassBuilder::access(RGBuffer buf, const ResourceState& state, bool discard)
{
    ResourceState s = state;
    s.layout = TextureLayout::Undefined;
    add_access(buf.id, false, s, discard && state.writes());
}

void PassBuilder::set_side_effects()
{
    graph_.passes_[pass_index_].side_effects = true;
}

void PassBuilder::add_access(uint32_t resource, bool expect_texture, const ResourceState& state, bool discard)
{
    RenderGraph::Pass& pass = graph_.passes_[pass_index_];
    if (resource >= graph_.resources_.size()) {
        graph_.add_error("Pass '" + pass.name + "': access to an invalid resource handle");
        return;
    }
    const RenderGraph::Resource& res = graph_.resources_[resource];
    const bool is_texture = res.kind == RenderGraph::ResourceKind::Texture;
    if (is_texture != expect_texture) {
        graph_.add_error("Pass '" + pass.name + "': resource '" + res.name + "' accessed as the wrong kind");
        return;
    }

    for (ResourceAccess& existing : pass.accesses) {
        if (existing.resource != resource) {
            continue;
        }
        existing.state.stages |= state.stages;
        existing.state.access |= state.access;
        existing.discard = existing.discard && discard;
        if (is_texture) {
            const std::optional<TextureLayout> merged = merge_layouts(existing.state.layout, state.layout);
            if (!merged) {
                graph_.add_error("Pass '" + pass.name + "': conflicting accesses to texture '" + res.name +
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
    const auto& res = graph_.resources_.at(tex.id);
    if (res.imported) {
        return res.physical_texture;
    }
    return tex.id < textures_.size() ? textures_[tex.id] : rhi::TextureHandle{};
}

rhi::BufferHandle PassContext::buffer(RGBuffer buf) const
{
    const auto& res = graph_.resources_.at(buf.id);
    if (res.imported) {
        return res.physical_buffer;
    }
    return buf.id < buffers_.size() ? buffers_[buf.id] : rhi::BufferHandle{};
}

const rhi::TextureDesc& PassContext::texture_desc(RGTexture tex) const
{
    return graph_.texture_desc(tex);
}

const rhi::BufferDesc& PassContext::buffer_desc(RGBuffer buf) const
{
    return graph_.buffer_desc(buf);
}

// =====================================================================================================================
// RenderGraph
// =====================================================================================================================

uint32_t RenderGraph::begin_pass(std::string_view name, rhi::QueueType queue, ExecuteFn execute)
{
    compiled_ = false;
    Pass pass;
    pass.name = std::string(name);
    pass.queue = queue;
    pass.execute = std::move(execute);
    passes_.push_back(std::move(pass));
    return static_cast<uint32_t>(passes_.size() - 1);
}

void RenderGraph::add_error(std::string message)
{
    setup_errors_.push_back(std::move(message));
}

RGTexture RenderGraph::import_texture(std::string_view name, const rhi::TextureDesc& desc, rhi::TextureHandle physical,
                                     const ResourceState& initial_state, std::optional<ResourceState> final_state)
{
    compiled_ = false;
    Resource res;
    res.name = std::string(name);
    res.kind = ResourceKind::Texture;
    res.imported = true;
    res.texture = desc;
    res.physical_texture = physical;
    res.initial_state = initial_state;
    res.final_state = final_state;
    resources_.push_back(std::move(res));
    return RGTexture{static_cast<uint32_t>(resources_.size() - 1)};
}

RGBuffer RenderGraph::import_buffer(std::string_view name, const rhi::BufferDesc& desc, rhi::BufferHandle physical,
                                   const ResourceState& initial_state, std::optional<ResourceState> final_state)
{
    compiled_ = false;
    Resource res;
    res.name = std::string(name);
    res.kind = ResourceKind::Buffer;
    res.imported = true;
    res.buffer = desc;
    res.physical_buffer = physical;
    res.initial_state = initial_state;
    res.initial_state.layout = TextureLayout::Undefined;
    res.final_state = final_state;
    if (res.final_state) {
        res.final_state->layout = TextureLayout::Undefined;
    }
    resources_.push_back(std::move(res));
    return RGBuffer{static_cast<uint32_t>(resources_.size() - 1)};
}

const CompileResult& RenderGraph::compile(const CompileOptions& options)
{
    result_ = CompileResult{};
    CompileResult& r = result_;
    r.errors = setup_errors_;

    const auto pass_count = static_cast<uint32_t>(passes_.size());
    const auto res_count = static_cast<uint32_t>(resources_.size());

    // ---------------------------------------------------------------------------------------------------------
    // 1. Data dependencies and culling
    // ---------------------------------------------------------------------------------------------------------
    std::vector<uint32_t> last_writer(res_count, invalid_resource);
    std::vector<std::vector<uint32_t>> deps(pass_count);
    std::vector<bool> is_root(pass_count, false);
    std::vector<std::pair<uint32_t, uint32_t>> uninitialized_reads;  // (pass, resource)

    for (uint32_t p = 0; p < pass_count; ++p) {
        const Pass& pass = passes_[p];
        is_root[p] = pass.side_effects;
        for (const ResourceAccess& a : pass.accesses) {
            if (!a.discard) {  // needs the previous contents
                if (last_writer[a.resource] != invalid_resource) {
                    deps[p].push_back(last_writer[a.resource]);
                } else if (!resources_[a.resource].imported) {
                    uninitialized_reads.emplace_back(p, a.resource);
                }
            }
            if (a.state.writes()) {
                last_writer[a.resource] = p;
                if (resources_[a.resource].imported) {
                    is_root[p] = true;
                }
            }
        }
    }

    std::vector<bool> needed(pass_count, false);
    std::vector<uint32_t> stack;
    for (uint32_t p = 0; p < pass_count; ++p) {
        if (is_root[p]) {
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

    std::vector<uint32_t> position(pass_count, invalid_resource);
    for (uint32_t p = 0; p < pass_count; ++p) {
        if (needed[p]) {
            position[p] = static_cast<uint32_t>(r.passes.size());
            r.passes.push_back(CompiledPass{p, {}});
        } else {
            r.culled_passes.push_back(p);
        }
    }

    for (const auto& [p, res] : uninitialized_reads) {
        if (needed[p]) {
            r.warnings.push_back("Pass '" + passes_[p].name + "' reads transient resource '" + resources_[res].name +
                                 "' before anything writes it (contents are undefined)");
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // 2. Lifetimes, usage inference and validation
    // ---------------------------------------------------------------------------------------------------------
    r.lifetimes.assign(res_count, ResourceLifetime{});
    std::vector<rhi::TextureUsage> tex_usage(res_count, rhi::TextureUsage::None);
    std::vector<rhi::BufferUsage> buf_usage(res_count, rhi::BufferUsage::None);

    for (uint32_t pos = 0; pos < r.passes.size(); ++pos) {
        for (const ResourceAccess& a : passes_[r.passes[pos].pass_index].accesses) {
            ResourceLifetime& life = r.lifetimes[a.resource];
            if (!life.used()) {
                life.first_pass = pos;
            }
            life.last_pass = pos;
            if (resources_[a.resource].kind == ResourceKind::Texture) {
                tex_usage[a.resource] |= infer_texture_usage(a.state.access);
            } else {
                buf_usage[a.resource] |= infer_buffer_usage(a.state.access);
            }
        }
    }

    for (uint32_t i = 0; i < res_count; ++i) {
        Resource& res = resources_[i];
        if (!r.lifetimes[i].used()) {
            continue;
        }
        if (res.kind == ResourceKind::Texture) {
            if (res.imported) {
                if (!rhi::has_all(res.texture.usage, tex_usage[i])) {
                    r.errors.push_back("Imported texture '" + res.name +
                                       "' is accessed in ways its usage flags do not allow");
                }
            } else {
                res.texture.usage |= tex_usage[i];
                if (const rhi::ValidationResult v = rhi::validate(res.texture); !v) {
                    r.errors.push_back("Transient texture '" + res.name + "': " + v.error);
                }
            }
        } else {
            if (res.imported) {
                if (!rhi::has_all(res.buffer.usage, buf_usage[i])) {
                    r.errors.push_back("Imported buffer '" + res.name +
                                       "' is accessed in ways its usage flags do not allow");
                }
            } else {
                res.buffer.usage |= buf_usage[i];
                if (const rhi::ValidationResult v = rhi::validate(res.buffer); !v) {
                    r.errors.push_back("Transient buffer '" + res.name + "': " + v.error);
                }
            }
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // 3. Transient memory aliasing (greedy interval assignment, best fit)
    // ---------------------------------------------------------------------------------------------------------
    r.allocations.assign(res_count, TransientAllocation{});
    {
        struct Slot {
            ResourceKind kind;
            uint64_t size;
            uint32_t last_use;
            uint32_t occupant;
        };
        std::vector<Slot> slots;
        std::vector<uint32_t> order;
        for (uint32_t i = 0; i < res_count; ++i) {
            if (!resources_[i].imported && r.lifetimes[i].used()) {
                order.push_back(i);
            }
        }
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
            return r.lifetimes[a].first_pass < r.lifetimes[b].first_pass;
        });

        for (uint32_t id : order) {
            const Resource& res = resources_[id];
            const ResourceLifetime& life = r.lifetimes[id];
            uint64_t size = 0;
            if (res.kind == ResourceKind::Texture) {
                size = options.texture_size ? options.texture_size(res.texture) : default_texture_size(res.texture);
            } else {
                size = options.buffer_size ? options.buffer_size(res.buffer) : default_buffer_size(res.buffer);
            }
            r.transient_bytes_unaliased += size;

            uint32_t chosen = invalid_resource;
            if (options.enable_aliasing) {
                uint32_t best_fit = invalid_resource;
                uint32_t largest = invalid_resource;
                for (uint32_t s = 0; s < slots.size(); ++s) {
                    const Slot& slot = slots[s];
                    if (slot.kind != res.kind || slot.last_use >= life.first_pass) {
                        continue;
                    }
                    if (slot.size >= size && (best_fit == invalid_resource || slot.size < slots[best_fit].size)) {
                        best_fit = s;
                    }
                    if (largest == invalid_resource || slot.size > slots[largest].size) {
                        largest = s;
                    }
                }
                chosen = best_fit != invalid_resource ? best_fit : largest;
            }

            TransientAllocation& alloc = r.allocations[id];
            alloc.size_bytes = size;
            if (chosen == invalid_resource) {
                chosen = static_cast<uint32_t>(slots.size());
                slots.push_back(Slot{res.kind, size, life.last_pass, id});
            } else {
                Slot& slot = slots[chosen];
                alloc.alias_predecessor = slot.occupant;
                slot.size = std::max(slot.size, size);
                slot.last_use = life.last_pass;
                slot.occupant = id;
            }
            alloc.slot = chosen;
        }

        for (const Slot& s : slots) {
            r.slot_sizes.push_back(s.size);
            r.transient_bytes_aliased += s.size;
        }
    }

    // ---------------------------------------------------------------------------------------------------------
    // 4. Barrier generation
    // ---------------------------------------------------------------------------------------------------------
    std::vector<TrackedState> tracked(res_count);
    for (uint32_t i = 0; i < res_count; ++i) {
        if (resources_[i].imported) {
            tracked[i] = tracked_from_initial(resources_[i].initial_state);
        }
    }

    for (uint32_t pos = 0; pos < r.passes.size(); ++pos) {
        CompiledPass& cp = r.passes[pos];
        for (const ResourceAccess& a : passes_[cp.pass_index].accesses) {
            const Resource& res = resources_[a.resource];
            const bool is_texture = res.kind == ResourceKind::Texture;
            TrackedState& t = tracked[a.resource];

            // First use of an aliased transient: wait for the previous occupant of the memory.
            if (!res.imported && r.lifetimes[a.resource].first_pass == pos) {
                const uint32_t pred = r.allocations[a.resource].alias_predecessor;
                if (pred != invalid_resource) {
                    const TrackedState& pt = tracked[pred];
                    t.write_stages = pt.write_stages | pt.read_stages;
                    t.write_access = pt.write_access;
                    t.layout = TextureLayout::Undefined;
                }
            }

            if (auto b = transition(a.resource, is_texture, t, a.state, a.discard)) {
                cp.barriers_before.push_back(*b);
            }
        }
    }

    for (uint32_t i = 0; i < res_count; ++i) {
        const Resource& res = resources_[i];
        if (res.imported && res.final_state) {
            if (auto b = transition(i, res.kind == ResourceKind::Texture, tracked[i], *res.final_state, false)) {
                r.final_barriers.push_back(*b);
            }
        }
    }

    r.ok = r.errors.empty();
    compiled_ = true;
    return r;
}

void RenderGraph::execute(const ExecuteContext& ctx)
{
    SAIGE_RHI_ASSERT(compiled_ && result_.ok, "RenderGraph::execute requires a successful compile()");
    if (!compiled_ || !result_.ok) {
        return;
    }

    std::vector<rhi::TextureBarrier> tex_barriers;
    std::vector<rhi::BufferBarrier> buf_barriers;

    auto resolve_and_submit = [&](const std::vector<GraphBarrier>& barriers) {
        if (barriers.empty() || !ctx.submit_barriers) {
            return;
        }
        tex_barriers.clear();
        buf_barriers.clear();
        for (const GraphBarrier& gb : barriers) {
            const Resource& res = resources_[gb.resource];
            if (res.kind == ResourceKind::Texture) {
                rhi::TextureBarrier tb;
                tb.texture = res.imported ? res.physical_texture
                                          : (gb.resource < ctx.transient_textures.size() ? ctx.transient_textures[gb.resource]
                                                                                        : rhi::TextureHandle{});
                tb.before = gb.before;
                tb.after = gb.after;
                tex_barriers.push_back(tb);
            } else {
                rhi::BufferBarrier bb;
                bb.buffer = res.imported ? res.physical_buffer
                                         : (gb.resource < ctx.transient_buffers.size() ? ctx.transient_buffers[gb.resource]
                                                                                      : rhi::BufferHandle{});
                bb.before = gb.before;
                bb.after = gb.after;
                buf_barriers.push_back(bb);
            }
        }
        ctx.submit_barriers(tex_barriers, buf_barriers);
    };

    for (const CompiledPass& cp : result_.passes) {
        resolve_and_submit(cp.barriers_before);
        Pass& pass = passes_[cp.pass_index];
        if (pass.execute) {
            PassContext pc(*this, ctx.command_list, pass.name, ctx.transient_textures, ctx.transient_buffers);
            pass.execute(pc);
        }
    }
    resolve_and_submit(result_.final_barriers);
}

void RenderGraph::reset()
{
    passes_.clear();
    resources_.clear();
    setup_errors_.clear();
    result_ = CompileResult{};
    compiled_ = false;
}

std::string RenderGraph::to_graphviz() const
{
    std::vector<bool> culled(passes_.size(), false);
    if (compiled_) {
        for (uint32_t p : result_.culled_passes) {
            culled[p] = true;
        }
    }

    std::string out = "digraph RenderGraph {\n  rankdir=LR;\n  node [fontname=\"Helvetica\", fontsize=10];\n";
    char line[512];
    for (uint32_t p = 0; p < passes_.size(); ++p) {
        std::snprintf(line, sizeof(line),
                      "  p%u [shape=box, style=\"rounded,filled%s\", fillcolor=\"%s\", label=\"%s\"];\n", p,
                      culled[p] ? ",dashed" : "", culled[p] ? "#eeeeee" : "#cfe2ff",
                      escape_dot(passes_[p].name).c_str());
        out += line;
    }
    for (uint32_t i = 0; i < resources_.size(); ++i) {
        const Resource& res = resources_[i];
        std::string detail;
        if (res.kind == ResourceKind::Texture) {
            std::snprintf(line, sizeof(line), "%ux%u %s", res.texture.width, res.texture.height,
                          std::string(rhi::format_name(res.texture.format)).c_str());
        } else {
            std::snprintf(line, sizeof(line), "%" PRIu64 " bytes", res.buffer.size);
        }
        detail = line;
        std::snprintf(line, sizeof(line), "  r%u [shape=%s, style=filled, fillcolor=\"%s\", label=\"%s\\n%s\"];\n", i,
                      res.kind == ResourceKind::Texture ? "ellipse" : "cylinder",
                      res.imported ? "#ffe8a3" : "#d1f2d1", escape_dot(res.name).c_str(), escape_dot(detail).c_str());
        out += line;
    }
    for (uint32_t p = 0; p < passes_.size(); ++p) {
        for (const ResourceAccess& a : passes_[p].accesses) {
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
