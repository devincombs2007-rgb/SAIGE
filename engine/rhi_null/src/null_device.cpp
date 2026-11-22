#include "saige/rhi_null/null_device.hpp"

#include "saige/rhi/deferred_release_queue.hpp"
#include "saige/rhi/handle_pool.hpp"

#include <array>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace saige::rhi {

namespace {

struct NullBuffer {
    BufferDesc desc;
    std::vector<std::byte> storage;  // only for host-visible buffers
};

struct NullTexture {
    TextureDesc desc;
    bool swapchain_image = false;
    uint64_t uploaded_bytes = 0;
};

struct NullSampler {
    SamplerDesc desc;
};

struct NullBindGroupLayout {
    BindGroupLayoutDesc desc;
};

struct NullBindGroup {
    BindGroupLayoutHandle layout;
    // Only the resources the layout actually binds (extraneous handles in an entry are ignored, as in validation).
    std::vector<TextureHandle> textures;
    std::vector<SamplerHandle> samplers;
    std::vector<BufferHandle> buffers;
};

struct NullShader {
    ShaderDesc desc;
    std::vector<uint32_t> spirv;
};

struct NullPipeline {
    GraphicsPipelineDesc desc;
};

struct NullSwapchain {
    SwapchainDesc desc;
    std::vector<TextureHandle> images;
    uint32_t next_image = 0;
    uint32_t recreate_count = 0;
    bool force_out_of_date = false;
};

constexpr uint32_t spirv_magic = 0x07230203u;
constexpr uint32_t max_push_constant_size = 128;

} // namespace

class NullCommandList;

struct NullDevice::Impl {
    DeviceInfo info;
    DeviceDesc desc;
    HandlePool<NullBuffer, BufferTag> buffers;
    HandlePool<NullTexture, TextureTag> textures;
    HandlePool<NullSampler, SamplerTag> samplers;
    HandlePool<NullBindGroupLayout, BindGroupLayoutTag> bind_group_layouts;
    HandlePool<NullBindGroup, BindGroupTag> bind_groups;
    HandlePool<NullShader, ShaderTag> shaders;
    HandlePool<NullPipeline, PipelineTag> pipelines;
    HandlePool<NullSwapchain, SwapchainTag> swapchains;
    DeferredReleaseQueue<std::function<void()>> pending;
    std::vector<std::string> messages;
    uint32_t errors = 0;
    uint64_t frame = 0;  // number of frames ended
    bool in_frame = false;
    uint32_t presents = 0;
    std::vector<SwapchainHandle> acquired_this_frame;
    std::unique_ptr<NullCommandList> cmd;
    std::vector<RecordedCommand> last_commands;

    /// A group does not own what it references. Binding one whose resources were destroyed is an error.
    bool group_resources_alive(const NullBindGroup& g) const
    {
        for (const TextureHandle t : g.textures) {
            if (!textures.contains(t)) return false;
        }
        for (const SamplerHandle s : g.samplers) {
            if (!samplers.contains(s)) return false;
        }
        for (const BufferHandle b : g.buffers) {
            if (!buffers.contains(b)) return false;
        }
        return true;
    }

    uint32_t creation_failure_countdown = 0;  // fail_creation_after

    void maybe_fail_creation(const char* what)
    {
        if (creation_failure_countdown != 0 && --creation_failure_countdown == 0) {
            throw std::runtime_error(std::string("NullDevice: injected failure creating ") + what);
        }
    }

    void error(std::string message)
    {
        std::cerr << "[saige.null] validation: " << message << '\n';
        messages.push_back(std::move(message));
        ++errors;
    }

    void create_swapchain_images(NullSwapchain& sc)
    {
        for (TextureHandle img : sc.images) {
            textures.release(img);
        }
        sc.images.clear();
        sc.next_image = 0;
        if (sc.desc.width == 0 || sc.desc.height == 0) {
            return;
        }
        const uint32_t count = sc.desc.min_image_count < 2 ? 2 : sc.desc.min_image_count;
        for (uint32_t i = 0; i < count; ++i) {
            TextureDesc td;
            td.format = sc.desc.preferred_format;
            td.width = sc.desc.width;
            td.height = sc.desc.height;
            td.usage = TextureUsage::ColorAttachment | TextureUsage::TransferDst;
            td.debug_name = "SwapchainImage";
            sc.images.push_back(textures.emplace(NullTexture{td, true}));
        }
    }
};

/// Records commands and validates their usage.
class NullCommandList final : public CommandList {
public:
    explicit NullCommandList(NullDevice::Impl& device) : device_(device) {}

    void reset()
    {
        commands_.clear();
        written_timestamps_ = 0;
        in_rendering_ = false;
        bound_pipeline_ = {};
        bound_groups_.fill({});
        vertex_bound_ = false;
        index_bound_ = false;
    }

    std::vector<RecordedCommand>& commands() { return commands_; }
    bool in_rendering() const { return in_rendering_; }

    void barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) override
    {
        check_frame("barrier");
        if (in_rendering_) {
            device_.error("barrier() inside begin_rendering()/end_rendering()");
        }
        for (const TextureBarrier& b : textures) {
            if (!device_.textures.contains(b.texture)) {
                device_.error("barrier() on an invalid texture");
            }
        }
        for (const BufferBarrier& b : buffers) {
            if (!device_.buffers.contains(b.buffer)) {
                device_.error("barrier() on an invalid buffer");
            }
        }
        RecordedCommand c = make(CommandType::Barrier);
        c.texture_barriers.assign(textures.begin(), textures.end());
        c.buffer_barriers.assign(buffers.begin(), buffers.end());
        commands_.push_back(std::move(c));
    }

    void begin_rendering(const RenderingDesc& desc) override
    {
        check_frame("begin_rendering");
        if (in_rendering_) {
            device_.error("begin_rendering() while already rendering");
        }
        if (desc.colors.empty() && !desc.depth) {
            device_.error("begin_rendering() without attachments");
        }
        for (const ColorAttachment& a : desc.colors) {
            const NullTexture* t = device_.textures.get(a.texture);
            if (t == nullptr) {
                device_.error("begin_rendering(): invalid color attachment");
            } else if (!has_any(t->desc.usage, TextureUsage::ColorAttachment)) {
                device_.error("begin_rendering(): color attachment lacks ColorAttachment usage");
            }
        }
        if (desc.depth) {
            const NullTexture* t = device_.textures.get(desc.depth->texture);
            if (t == nullptr || !has_any(t->desc.usage, TextureUsage::DepthStencilAttachment)) {
                device_.error("begin_rendering(): invalid depth attachment");
            }
        }
        if (desc.area.width == 0 || desc.area.height == 0) {
            device_.error("begin_rendering(): zero render area");
        }
        in_rendering_ = true;
        RecordedCommand c = make(CommandType::BeginRendering);
        c.rendering = desc;
        commands_.push_back(std::move(c));
    }

    void end_rendering() override
    {
        check_frame("end_rendering");
        if (!in_rendering_) {
            device_.error("end_rendering() without begin_rendering()");
        }
        in_rendering_ = false;
        commands_.push_back(make(CommandType::EndRendering));
    }

    void set_viewport(const Viewport& viewport) override
    {
        check_frame("set_viewport");
        if (viewport.width <= 0.0f || viewport.height <= 0.0f) {
            device_.error("set_viewport(): non-positive extent");
        }
        RecordedCommand c = make(CommandType::SetViewport);
        c.viewport = viewport;
        commands_.push_back(std::move(c));
    }

    void set_scissor(const Rect2D& rect) override
    {
        check_frame("set_scissor");
        RecordedCommand c = make(CommandType::SetScissor);
        c.scissor = rect;
        commands_.push_back(std::move(c));
    }

    void bind_pipeline(PipelineHandle pipeline) override
    {
        check_frame("bind_pipeline");
        if (!device_.pipelines.contains(pipeline)) {
            device_.error("bind_pipeline(): invalid pipeline");
        }
        if (pipeline != bound_pipeline_) {
            // Descriptor compatibility also depends on push constant ranges and every lower set, so a different
            // pipeline starts with nothing bound: the caller rebinds its groups (same-handle rebinds keep them).
            bound_groups_.fill({});
        }
        bound_pipeline_ = pipeline;
        RecordedCommand c = make(CommandType::BindPipeline);
        c.pipeline = pipeline;
        commands_.push_back(std::move(c));
    }

    void write_timestamp(uint32_t index) override
    {
        check_frame("write_timestamp");
        if (index >= max_gpu_timestamps) {
            device_.error("write_timestamp(): index out of range");
        } else if ((written_timestamps_ & (1u << index)) != 0) {
            device_.error("write_timestamp(): index " + std::to_string(index) + " was already written in this frame");
        } else {
            written_timestamps_ |= 1u << index;
        }
        RecordedCommand c = make(CommandType::Timestamp);
        c.count = index;
        commands_.push_back(std::move(c));
    }

    void bind_group(uint32_t set, BindGroupHandle group) override
    {
        check_frame("bind_group");
        const NullPipeline* p = device_.pipelines.get(bound_pipeline_);
        const NullBindGroup* g = device_.bind_groups.get(group);
        // Like the Vulkan backend, an invalid bind changes nothing: the previously bound group (if any) stays.
        bool valid = false;
        if (p == nullptr) {
            device_.error("bind_group() without a bound pipeline");
        } else if (set >= p->desc.bind_group_layouts.size()) {
            device_.error("bind_group(): the bound pipeline has no descriptor set " + std::to_string(set));
        } else if (g == nullptr) {
            device_.error("bind_group(): invalid bind group");
        } else if (g->layout != p->desc.bind_group_layouts[set]) {
            device_.error("bind_group(): the group's layout differs from the pipeline's layout for set " + std::to_string(set));
        } else if (!device_.group_resources_alive(*g)) {
            device_.error("bind_group(): the group refers to a resource that has been destroyed; rebuild the group");
        } else {
            valid = true;
        }
        if (valid) {
            bound_groups_[set] = group;
        }
        RecordedCommand c = make(CommandType::BindGroup);
        c.group = group;
        c.count = set;
        commands_.push_back(std::move(c));
    }

    void push_constants(std::span<const std::byte> data, uint32_t offset) override
    {
        check_frame("push_constants");
        const NullPipeline* p = device_.pipelines.get(bound_pipeline_);
        if (p == nullptr) {
            device_.error("push_constants() without a bound pipeline");
        } else if (offset + data.size() > p->desc.push_constant_size) {
            device_.error("push_constants() exceeds the pipeline's push constant range");
        }
        RecordedCommand c = make(CommandType::PushConstants);
        c.push_data.assign(data.begin(), data.end());
        c.push_offset = offset;
        commands_.push_back(std::move(c));
    }

    void bind_vertex_buffer(BufferHandle buffer, uint64_t /*offset*/) override
    {
        check_frame("bind_vertex_buffer");
        const NullBuffer* b = device_.buffers.get(buffer);
        if (b == nullptr || !has_any(b->desc.usage, BufferUsage::Vertex)) {
            device_.error("bind_vertex_buffer(): invalid buffer or missing Vertex usage");
        }
        vertex_bound_ = true;
        RecordedCommand c = make(CommandType::BindVertexBuffer);
        c.buffer = buffer;
        commands_.push_back(std::move(c));
    }

    void bind_index_buffer(BufferHandle buffer, IndexType /*type*/, uint64_t /*offset*/) override
    {
        check_frame("bind_index_buffer");
        const NullBuffer* b = device_.buffers.get(buffer);
        if (b == nullptr || !has_any(b->desc.usage, BufferUsage::Index)) {
            device_.error("bind_index_buffer(): invalid buffer or missing Index usage");
        }
        index_bound_ = true;
        RecordedCommand c = make(CommandType::BindIndexBuffer);
        c.buffer = buffer;
        commands_.push_back(std::move(c));
    }

    void draw(uint32_t vertex_count, uint32_t instance_count, uint32_t /*first_vertex*/,
              uint32_t /*first_instance*/) override
    {
        check_draw("draw");
        RecordedCommand c = make(CommandType::Draw);
        c.count = vertex_count;
        c.instance_count = instance_count;
        c.pipeline = bound_pipeline_;
        commands_.push_back(std::move(c));
    }

    void draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t /*first_index*/,
                      int32_t /*vertex_offset*/, uint32_t /*first_instance*/) override
    {
        check_draw("draw_indexed");
        if (!index_bound_) {
            device_.error("draw_indexed() without an index buffer");
        }
        RecordedCommand c = make(CommandType::DrawIndexed);
        c.count = index_count;
        c.instance_count = instance_count;
        c.pipeline = bound_pipeline_;
        commands_.push_back(std::move(c));
    }

    void copy_texture_to_buffer(TextureHandle texture, BufferHandle buffer) override
    {
        check_frame("copy_texture_to_buffer");
        if (in_rendering_) {
            device_.error("copy_texture_to_buffer() inside rendering");
        }
        const NullTexture* t = device_.textures.get(texture);
        const NullBuffer* b = device_.buffers.get(buffer);
        if (t == nullptr || b == nullptr) {
            device_.error("copy_texture_to_buffer(): invalid texture or buffer");
        } else {
            if (!has_any(t->desc.usage, TextureUsage::TransferSrc)) {
                device_.error("copy_texture_to_buffer(): texture lacks TransferSrc usage");
            }
            if (!has_any(b->desc.usage, BufferUsage::TransferDst)) {
                device_.error("copy_texture_to_buffer(): buffer lacks TransferDst usage");
            }
            const SurfaceLayout layout = compute_surface_layout(t->desc.format, t->desc.width, t->desc.height);
            if (b->desc.size < layout.slice_pitch) {
                device_.error("copy_texture_to_buffer(): buffer too small");
            }
        }
        RecordedCommand c = make(CommandType::CopyTextureToBuffer);
        c.texture = texture;
        c.buffer = buffer;
        commands_.push_back(std::move(c));
    }

    void begin_debug_label(const char* name) override
    {
        RecordedCommand c = make(CommandType::BeginDebugLabel);
        c.label = name != nullptr ? name : "";
        commands_.push_back(std::move(c));
    }

    void end_debug_label() override { commands_.push_back(make(CommandType::EndDebugLabel)); }

private:
    static RecordedCommand make(CommandType type)
    {
        RecordedCommand c;
        c.type = type;
        return c;
    }

    void check_frame(const char* what)
    {
        if (!device_.in_frame) {
            device_.error(std::string(what) + "() outside begin_frame()/end_frame()");
        }
    }

    void check_draw(const char* what)
    {
        check_frame(what);
        if (!in_rendering_) {
            device_.error(std::string(what) + "() outside begin_rendering()/end_rendering()");
        }
        const NullPipeline* p = device_.pipelines.get(bound_pipeline_);
        if (p == nullptr) {
            device_.error(std::string(what) + "() without a bound pipeline");
        } else {
            if (!p->desc.vertex_layout.attributes.empty() && !vertex_bound_) {
                device_.error(std::string(what) + "() without a vertex buffer for a pipeline with vertex input");
            }
            for (std::size_t set = 0; set < p->desc.bind_group_layouts.size(); ++set) {
                const NullBindGroup* g = device_.bind_groups.get(bound_groups_[set]);
                if (g == nullptr || g->layout != p->desc.bind_group_layouts[set]) {
                    device_.error(std::string(what) + "() without a compatible bind group at set " + std::to_string(set));
                }
            }
        }
    }

    NullDevice::Impl& device_;
    std::vector<RecordedCommand> commands_;
    uint32_t written_timestamps_ = 0;
    bool in_rendering_ = false;
    PipelineHandle bound_pipeline_;
    bool vertex_bound_ = false;
    bool index_bound_ = false;
    std::array<BindGroupHandle, max_bind_groups_per_pipeline> bound_groups_{};
};

// =====================================================================================================================

NullDevice::NullDevice(const DeviceDesc& desc) : impl_(std::make_unique<Impl>())
{
    impl_->desc = desc;
    if (impl_->desc.frames_in_flight == 0) {
        impl_->desc.frames_in_flight = 1;
    }
    impl_->info.backend = Backend::Null;
    impl_->info.device_name = "SAIGE null device";
    impl_->info.driver_name = "saige-null";
    impl_->info.api_version_major = 1;
    impl_->info.validation_enabled = true;
    impl_->info.presentation_supported = true;
    impl_->info.software_device = true;
    impl_->cmd = std::make_unique<NullCommandList>(*impl_);
}

NullDevice::~NullDevice()
{
    impl_->pending.drain_all([](std::function<void()>&& fn) { fn(); });
}

const DeviceInfo& NullDevice::info() const noexcept
{
    return impl_->info;
}

BufferHandle NullDevice::create_buffer(const BufferDesc& desc, std::span<const std::byte> initial_data)
{
    impl_->maybe_fail_creation("buffer");
    if (const ValidationResult v = validate(desc); !v) {
        throw std::invalid_argument(std::string("create_buffer: ") + v.error);
    }
    if (initial_data.size() > desc.size) {
        throw std::invalid_argument("create_buffer: initial data larger than the buffer");
    }
    NullBuffer b{desc, {}};
    if (desc.memory != MemoryLocation::GpuOnly) {
        b.storage.assign(static_cast<std::size_t>(desc.size), std::byte{0});
        if (!initial_data.empty()) {
            std::memcpy(b.storage.data(), initial_data.data(), initial_data.size());
        }
    }
    return impl_->buffers.emplace(std::move(b));
}

TextureHandle NullDevice::create_texture(const TextureDesc& desc)
{
    impl_->maybe_fail_creation("texture");
    if (const ValidationResult v = validate(desc); !v) {
        throw std::invalid_argument(std::string("create_texture: ") + v.error);
    }
    return impl_->textures.emplace(NullTexture{desc, false, 0});
}

void NullDevice::upload_texture(TextureHandle texture, std::span<const std::byte> data)
{
    NullTexture* t = impl_->textures.get(texture);
    if (t == nullptr || t->swapchain_image) {
        throw std::invalid_argument("upload_texture: invalid texture handle");
    }
    if (impl_->in_frame) {
        throw std::logic_error("upload_texture: not allowed between begin_frame() and end_frame()");
    }
    if (const ValidationResult v = validate_texture_upload(t->desc, data.size()); !v) {
        throw std::invalid_argument(v.error);
    }
    t->uploaded_bytes = data.size();
}

SamplerHandle NullDevice::create_sampler(const SamplerDesc& desc)
{
    impl_->maybe_fail_creation("sampler");
    if (const ValidationResult v = validate(desc); !v) {
        throw std::invalid_argument(std::string("create_sampler: ") + v.error);
    }
    return impl_->samplers.emplace(NullSampler{desc});
}

BindGroupLayoutHandle NullDevice::create_bind_group_layout(const BindGroupLayoutDesc& desc)
{
    impl_->maybe_fail_creation("bind group layout");
    if (const ValidationResult v = validate(desc); !v) {
        throw std::invalid_argument(std::string("create_bind_group_layout: ") + v.error);
    }
    NullBindGroupLayout layout{desc};
    layout.desc.debug_name = nullptr;
    return impl_->bind_group_layouts.emplace(std::move(layout));
}

BindGroupHandle NullDevice::create_bind_group(BindGroupLayoutHandle layout, std::span<const BindGroupEntry> entries)
{
    impl_->maybe_fail_creation("bind group");
    const NullBindGroupLayout* l = impl_->bind_group_layouts.get(layout);
    if (l == nullptr) {
        throw std::invalid_argument("create_bind_group: invalid layout");
    }
    BindGroupResolver resolver;
    resolver.texture = [this](TextureHandle h) -> std::optional<TextureDesc> {
        const NullTexture* t = impl_->textures.get(h);
        return t != nullptr ? std::optional<TextureDesc>(t->desc) : std::nullopt;
    };
    resolver.sampler_exists = [this](SamplerHandle h) { return impl_->samplers.contains(h); };
    resolver.buffer = [this](BufferHandle h) -> std::optional<BufferDesc> {
        const NullBuffer* b = impl_->buffers.get(h);
        return b != nullptr ? std::optional<BufferDesc>(b->desc) : std::nullopt;
    };
    if (const ValidationResult v = validate(l->desc, entries, resolver); !v) {
        throw std::invalid_argument(std::string("create_bind_group: ") + v.error);
    }
    NullBindGroup group;
    group.layout = layout;
    for (const BindGroupLayoutEntry& expected : l->desc.entries) {
        for (const BindGroupEntry& e : entries) {
            if (e.binding != expected.binding) {
                continue;
            }
            if (expected.type == BindingType::SampledTexture) {
                group.textures.push_back(e.texture);
            } else if (expected.type == BindingType::Sampler) {
                group.samplers.push_back(e.sampler);
            } else {
                group.buffers.push_back(e.buffer);
            }
        }
    }
    return impl_->bind_groups.emplace(std::move(group));
}

void NullDevice::destroy(BindGroupLayoutHandle handle)
{
    if (!impl_->bind_group_layouts.contains(handle)) {
        impl_->error("destroy(): invalid bind group layout handle");
        return;
    }
    impl_->pending.enqueue([this, handle] { impl_->bind_group_layouts.release(handle); },
                           impl_->frame + impl_->desc.frames_in_flight);
}

void NullDevice::destroy(BindGroupHandle handle)
{
    if (!impl_->bind_groups.contains(handle)) {
        impl_->error("destroy(): invalid bind group handle");
        return;
    }
    impl_->pending.enqueue([this, handle] { impl_->bind_groups.release(handle); }, impl_->frame + impl_->desc.frames_in_flight);
}

void NullDevice::destroy(SamplerHandle handle)
{
    if (!impl_->samplers.contains(handle)) {
        impl_->error("destroy(): invalid sampler handle");
        return;
    }
    impl_->pending.enqueue([this, handle] { impl_->samplers.release(handle); }, impl_->frame + impl_->desc.frames_in_flight);
}

uint64_t NullDevice::uploaded_bytes(TextureHandle texture) const noexcept
{
    const NullTexture* t = impl_->textures.get(texture);
    return t != nullptr ? t->uploaded_bytes : 0;
}

ShaderHandle NullDevice::create_shader(const ShaderDesc& desc)
{
    impl_->maybe_fail_creation("shader");
    if (desc.spirv.size() < 5 || desc.spirv[0] != spirv_magic) {
        throw std::invalid_argument("create_shader: not a SPIR-V module");
    }
    if (desc.entry_point.empty()) {
        throw std::invalid_argument("create_shader: empty entry point");
    }
    NullShader s{desc, std::vector<uint32_t>(desc.spirv.begin(), desc.spirv.end())};
    s.desc.spirv = s.spirv;
    return impl_->shaders.emplace(std::move(s));
}

PipelineHandle NullDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc)
{
    impl_->maybe_fail_creation("pipeline");
    const NullShader* vs = impl_->shaders.get(desc.vertex_shader);
    const NullShader* ps = impl_->shaders.get(desc.pixel_shader);
    if (vs == nullptr || vs->desc.stage != ShaderStage::Vertex) {
        throw std::invalid_argument("create_graphics_pipeline: invalid vertex shader");
    }
    if (ps == nullptr || ps->desc.stage != ShaderStage::Pixel) {
        throw std::invalid_argument("create_graphics_pipeline: invalid pixel shader");
    }
    if (desc.color_formats.empty() && desc.depth_format == Format::Unknown) {
        throw std::invalid_argument("create_graphics_pipeline: no attachment formats");
    }
    if (desc.push_constant_size > max_push_constant_size || desc.push_constant_size % 4 != 0) {
        throw std::invalid_argument("create_graphics_pipeline: push constant size must be a multiple of 4 and <= 128");
    }
    if ((desc.depth.test || desc.depth.write) && !has_depth(desc.depth_format)) {
        throw std::invalid_argument("create_graphics_pipeline: depth state without a depth format");
    }
    if (desc.bind_group_layouts.size() > max_bind_groups_per_pipeline) {
        throw std::invalid_argument("create_graphics_pipeline: too many bind group layouts");
    }
    std::vector<const BindGroupLayoutDesc*> layout_descs;
    for (const BindGroupLayoutHandle layout : desc.bind_group_layouts) {
        const NullBindGroupLayout* l = impl_->bind_group_layouts.get(layout);
        if (l == nullptr) {
            throw std::invalid_argument("create_graphics_pipeline: invalid bind group layout");
        }
        layout_descs.push_back(&l->desc);
    }
    if (const ValidationResult v = validate_pipeline_bindings(layout_descs); !v) {
        throw std::invalid_argument(std::string("create_graphics_pipeline: ") + v.error);
    }
    if (const ValidationResult v = validate_blend_state(desc.blend, desc.color_formats); !v) {
        throw std::invalid_argument(std::string("create_graphics_pipeline: ") + v.error);
    }
    return impl_->pipelines.emplace(NullPipeline{desc});
}

void NullDevice::destroy(BufferHandle handle)
{
    if (!impl_->buffers.contains(handle)) {
        impl_->error("destroy(): invalid buffer handle");
        return;
    }
    impl_->pending.enqueue([this, handle] { impl_->buffers.release(handle); }, impl_->frame + impl_->desc.frames_in_flight);
}

void NullDevice::destroy(TextureHandle handle)
{
    const NullTexture* t = impl_->textures.get(handle);
    if (t == nullptr || t->swapchain_image) {
        impl_->error("destroy(): invalid texture handle (or a swapchain image)");
        return;
    }
    impl_->pending.enqueue([this, handle] { impl_->textures.release(handle); }, impl_->frame + impl_->desc.frames_in_flight);
}

void NullDevice::destroy(ShaderHandle handle)
{
    if (!impl_->shaders.contains(handle)) {
        impl_->error("destroy(): invalid shader handle");
        return;
    }
    impl_->pending.enqueue([this, handle] { impl_->shaders.release(handle); }, impl_->frame + impl_->desc.frames_in_flight);
}

void NullDevice::destroy(PipelineHandle handle)
{
    if (!impl_->pipelines.contains(handle)) {
        impl_->error("destroy(): invalid pipeline handle");
        return;
    }
    impl_->pending.enqueue([this, handle] { impl_->pipelines.release(handle); }, impl_->frame + impl_->desc.frames_in_flight);
}

std::span<std::byte> NullDevice::map(BufferHandle handle)
{
    NullBuffer* b = impl_->buffers.get(handle);
    if (b == nullptr) {
        impl_->error("map(): invalid buffer handle");
        return {};
    }
    return b->storage;
}

std::optional<TextureDesc> NullDevice::texture_desc(TextureHandle handle) const
{
    const NullTexture* t = impl_->textures.get(handle);
    return t != nullptr ? std::optional<TextureDesc>(t->desc) : std::nullopt;
}

void NullDevice::fail_creation_after(uint32_t n) noexcept
{
    impl_->creation_failure_countdown = n;
}

SwapchainHandle NullDevice::create_swapchain(const SwapchainDesc& desc)
{
    if (desc.surface.system == NativeSurface::System::Headless) {
        throw std::invalid_argument("create_swapchain: a window surface is required");
    }
    NullSwapchain sc{desc, {}, 0, 0, false};
    impl_->create_swapchain_images(sc);
    return impl_->swapchains.emplace(std::move(sc));
}

void NullDevice::resize_swapchain(SwapchainHandle handle, uint32_t width, uint32_t height)
{
    NullSwapchain* sc = impl_->swapchains.get(handle);
    if (sc == nullptr) {
        impl_->error("resize_swapchain(): invalid swapchain");
        return;
    }
    if (impl_->in_frame) {
        impl_->error("resize_swapchain() inside a frame");
    }
    sc->desc.width = width;
    sc->desc.height = height;
    ++sc->recreate_count;
    impl_->create_swapchain_images(*sc);
}

void NullDevice::destroy(SwapchainHandle handle)
{
    NullSwapchain* sc = impl_->swapchains.get(handle);
    if (sc == nullptr) {
        impl_->error("destroy(): invalid swapchain");
        return;
    }
    for (TextureHandle img : sc->images) {
        impl_->textures.release(img);
    }
    impl_->swapchains.release(handle);
}

Format NullDevice::swapchain_format(SwapchainHandle handle) const
{
    const NullSwapchain* sc = impl_->swapchains.get(handle);
    return sc != nullptr ? sc->desc.preferred_format : Format::Unknown;
}

CommandList& NullDevice::begin_frame()
{
    if (impl_->in_frame) {
        impl_->error("begin_frame() called twice without end_frame()");
    }
    impl_->in_frame = true;
    impl_->acquired_this_frame.clear();
    impl_->cmd->reset();
    return *impl_->cmd;
}

AcquireResult NullDevice::acquire_next_image(SwapchainHandle handle)
{
    if (!impl_->in_frame) {
        impl_->error("acquire_next_image() outside a frame");
    }
    NullSwapchain* sc = impl_->swapchains.get(handle);
    if (sc == nullptr) {
        impl_->error("acquire_next_image(): invalid swapchain");
        return {};
    }
    if (sc->force_out_of_date) {
        sc->force_out_of_date = false;
        return {AcquireStatus::OutOfDate, {}};
    }
    if (sc->images.empty()) {
        return {AcquireStatus::NotReady, {}};
    }
    for (SwapchainHandle h : impl_->acquired_this_frame) {
        if (h == handle) {
            impl_->error("acquire_next_image() twice for one swapchain in a frame");
        }
    }
    impl_->acquired_this_frame.push_back(handle);
    const TextureHandle img = sc->images[sc->next_image];
    sc->next_image = (sc->next_image + 1) % static_cast<uint32_t>(sc->images.size());
    return {AcquireStatus::Success, img};
}

FrameResult NullDevice::end_frame()
{
    FrameResult result;
    if (!impl_->in_frame) {
        impl_->error("end_frame() without begin_frame()");
        return result;
    }
    if (impl_->cmd->in_rendering()) {
        impl_->error("end_frame() while rendering is still open");
    }
    impl_->in_frame = false;
    impl_->last_commands = std::move(impl_->cmd->commands());
    impl_->cmd->reset();
    if (!impl_->acquired_this_frame.empty()) {
        result.presented = true;
        impl_->presents += static_cast<uint32_t>(impl_->acquired_this_frame.size());
    }
    ++impl_->frame;
    impl_->pending.collect(impl_->frame, [](std::function<void()>&& fn) { fn(); });
    return result;
}

void NullDevice::wait_idle()
{
    if (impl_->in_frame) {
        impl_->error("wait_idle() inside a frame");
    }
    impl_->pending.drain_all([](std::function<void()>&& fn) { fn(); });
}

uint64_t NullDevice::frame_number() const noexcept
{
    return impl_->frame;
}

uint32_t NullDevice::validation_error_count() const noexcept
{
    return impl_->errors;
}

const std::vector<RecordedCommand>& NullDevice::last_frame_commands() const noexcept
{
    return impl_->last_commands;
}

const std::vector<std::string>& NullDevice::validation_messages() const noexcept
{
    return impl_->messages;
}

std::optional<GpuTimestamps> NullDevice::completed_timestamps() const
{
    return std::nullopt;
}

DeviceMemoryStats NullDevice::memory_stats() const
{
    DeviceMemoryStats s;
    impl_->buffers.for_each([&](BufferHandle, NullBuffer& b) {
        s.allocated_bytes += b.desc.size;
        ++s.allocation_count;
    });
    impl_->textures.for_each([&](TextureHandle, NullTexture& t) {
        if (!t.swapchain_image) {
            s.allocated_bytes += compute_texture_data_size(t.desc);
            ++s.allocation_count;
        }
    });
    return s;
}

uint32_t NullDevice::live_resource_count() const noexcept
{
    uint32_t swapchain_images = 0;
    impl_->swapchains.for_each([&](SwapchainHandle, NullSwapchain& sc) {
        swapchain_images += static_cast<uint32_t>(sc.images.size());
    });
    return impl_->buffers.size() + impl_->textures.size() - swapchain_images + impl_->samplers.size() +
           impl_->bind_group_layouts.size() + impl_->bind_groups.size() + impl_->shaders.size() +
           impl_->pipelines.size();
}

uint32_t NullDevice::pending_destruction_count() const noexcept
{
    return static_cast<uint32_t>(impl_->pending.size());
}

Extent3D NullDevice::swapchain_extent(SwapchainHandle handle) const
{
    const NullSwapchain* sc = impl_->swapchains.get(handle);
    return sc != nullptr ? Extent3D{sc->desc.width, sc->desc.height, 1} : Extent3D{0, 0, 0};
}

uint32_t NullDevice::swapchain_recreate_count(SwapchainHandle handle) const
{
    const NullSwapchain* sc = impl_->swapchains.get(handle);
    return sc != nullptr ? sc->recreate_count : 0;
}

uint32_t NullDevice::present_count() const noexcept
{
    return impl_->presents;
}

void NullDevice::force_out_of_date(SwapchainHandle handle)
{
    if (NullSwapchain* sc = impl_->swapchains.get(handle)) {
        sc->force_out_of_date = true;
    }
}

} // namespace saige::rhi
