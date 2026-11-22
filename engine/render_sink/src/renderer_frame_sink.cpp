#include "saige/render_sink/renderer_frame_sink.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace saige::render {

namespace {

constexpr rhi::Format offscreen_format = rhi::Format::RGBA8_SRGB;

const rhi::ResourceState swapchain_acquired{rhi::PipelineStage::ColorAttachmentOutput, rhi::Access::None,
                                            rhi::TextureLayout::Undefined};
const rhi::ResourceState swapchain_present{rhi::PipelineStage::None, rhi::Access::None, rhi::TextureLayout::Present};

rhi::NativeSurface to_rhi_surface(const saige::NativeWindowHandle& handle)
{
    rhi::NativeSurface s;
    switch (handle.system) {
    case saige::WindowSystem::win32: s.system = rhi::NativeSurface::System::Win32; break;
    case saige::WindowSystem::headless: s.system = rhi::NativeSurface::System::Headless; break;
    }
    s.display = handle.display;
    s.window = handle.window;
    return s;
}

} // namespace

namespace {

SceneInstance to_scene_instance(const saige::RenderInstance& inst)
{
    // Linear-space albedos; entity 0 (the sample's marker) gets the signature orange.
    static constexpr std::array<Float3, 6> palette{{{0.85f, 0.30f, 0.10f},
                                                    {0.15f, 0.45f, 0.85f},
                                                    {0.20f, 0.70f, 0.30f},
                                                    {0.80f, 0.70f, 0.15f},
                                                    {0.60f, 0.25f, 0.75f},
                                                    {0.85f, 0.85f, 0.85f}}};
    SceneInstance s;
    s.position = {inst.position.x, inst.position.y, inst.position.z};
    s.color = palette[inst.entity.index % palette.size()];
    s.yaw_offset = static_cast<float>(inst.entity.index) * 0.7f;
    return s;
}

bool id_less(const saige::AssetId& a, const saige::AssetId& b) noexcept
{
    return a.high != b.high ? a.high < b.high : a.low < b.low;
}

} // namespace

std::vector<SceneInstance> to_scene_instances(std::span<const saige::RenderInstance> instances)
{
    std::vector<SceneInstance> out;
    out.reserve(instances.size());
    for (const saige::RenderInstance& inst : instances) {
        out.push_back(to_scene_instance(inst));
    }
    return out;
}

Camera to_render_camera(const saige::CameraView& camera) noexcept
{
    Camera c;
    c.position = {camera.position.x, camera.position.y, camera.position.z};
    c.rotation = {camera.rotation.x, camera.rotation.y, camera.rotation.z, camera.rotation.w};
    c.fov_y_radians = camera.fov_y_radians;
    c.near_plane = camera.near_plane;
    return c;
}

Mat4 to_render_matrix(const saige::WorldMatrix& matrix) noexcept
{
    Mat4 m;
    for (std::size_t i = 0; i < 16; ++i) {
        m.m[i] = matrix[i];
    }
    return m;
}

const GpuModel* StagedScene::find(saige::AssetId id) const noexcept
{
    const auto it = std::lower_bound(models_.begin(), models_.end(), id,
                                     [](const Entry& e, const saige::AssetId& key) { return id_less(e.id, key); });
    return (it != models_.end() && it->id == id) ? &it->model : nullptr;
}

RendererFrameSink::RendererFrameSink(std::unique_ptr<rhi::Device> device, const saige::IDesktopPlatform& platform,
                                     FrameSinkSettings settings)
    : device_(std::move(device)), platform_(&platform), settings_(settings)
{
    if (!device_) {
        throw std::invalid_argument("RendererFrameSink: null device");
    }
    const saige::NativeWindowHandle native = platform.native_window();
    if (native.system == saige::WindowSystem::headless) {
        throw std::invalid_argument("RendererFrameSink: the platform has no window to present to");
    }
    if (!device_->info().presentation_supported) {
        throw std::runtime_error("RendererFrameSink: the graphics device cannot present to windows");
    }
    const saige::SurfaceState surface = platform.surface();
    rhi::SwapchainDesc sd;
    sd.surface = to_rhi_surface(native);
    sd.width = surface.minimized ? 0 : surface.width_px;
    sd.height = surface.minimized ? 0 : surface.height_px;
    sd.vsync = settings_.vsync;
    sd.debug_name = "MainSwapchain";
    swapchain_ = device_->create_swapchain(sd);
    surface_revision_ = surface.revision;
    surface_drawable_ = sd.width > 0 && sd.height > 0;
    width_ = sd.width;
    height_ = sd.height;
    renderer_ = std::make_unique<SceneRenderer>(*device_, device_->swapchain_format(swapchain_), settings_.renderer);
}

RendererFrameSink::RendererFrameSink(std::unique_ptr<rhi::Device> device, uint32_t width, uint32_t height,
                                     FrameSinkSettings settings)
    : device_(std::move(device)), settings_(settings), width_(width), height_(height)
{
    if (!device_) {
        throw std::invalid_argument("RendererFrameSink: null device");
    }
    if (width == 0 || height == 0) {
        throw std::invalid_argument("RendererFrameSink: offscreen size must be nonzero");
    }
    rhi::TextureDesc td;
    td.format = offscreen_format;
    td.width = width;
    td.height = height;
    td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
    td.debug_name = "OffscreenColor";
    offscreen_color_ = device_->create_texture(td);
    rhi::BufferDesc bd;
    bd.size = uint64_t{width} * height * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryLocation::GpuToCpu;
    bd.debug_name = "OffscreenReadback";
    readback_ = device_->create_buffer(bd);
    renderer_ = std::make_unique<SceneRenderer>(*device_, offscreen_format, settings_.renderer);
}

RendererFrameSink::~RendererFrameSink()
{
    // D-18: release GPU work, swapchain and surface before the platform destroys its window.
    device_->wait_idle();
    active_ = StagedScene{};  // the scene's references go before the renderer's registry does
    renderer_.reset();
    if (swapchain_.is_valid()) {
        device_->destroy(swapchain_);
    }
    if (offscreen_color_.is_valid()) {
        device_->destroy(offscreen_color_);
    }
    if (readback_.is_valid()) {
        device_->destroy(readback_);
    }
    device_->wait_idle();
}

StagedScene RendererFrameSink::stage_scene(std::span<const SceneAssetRef> assets, const gfx::StageSettings& settings)
{
    for (std::size_t i = 0; i < assets.size(); ++i) {
        if (assets[i].asset == nullptr) {
            throw std::invalid_argument("stage_scene: an asset reference has no data");
        }
        if (assets[i].id.is_builtin()) {
            throw std::invalid_argument("stage_scene: the all-zero asset ID is reserved for the built-in mesh");
        }
        for (std::size_t j = i + 1; j < assets.size(); ++j) {
            if (assets[i].id == assets[j].id) {
                throw std::invalid_argument("stage_scene: duplicate asset ID");
            }
        }
    }
    StagedScene staged;
    for (const SceneAssetRef& ref : assets) {
        gfx::StageReport report;
        const std::string name = ref.name.empty() ? saige::format_asset_id(ref.id) : ref.name;
        StagedScene::Entry entry{ref.id, gfx::stage_model(*renderer_, *ref.asset, name, settings, &report)};
        staged.models_.push_back(std::move(entry));
        staged.report_.textures += report.textures;
        staged.report_.texture_bytes += report.texture_bytes;
        staged.report_.primitives += report.primitives;
        staged.report_.vertex_bytes += report.vertex_bytes;
        staged.report_.index_bytes += report.index_bytes;
        for (const std::string& w : report.warnings) {
            if (std::find(staged.report_.warnings.begin(), staged.report_.warnings.end(), w) == staged.report_.warnings.end()) {
                staged.report_.warnings.push_back(w);
            }
        }
    }
    std::sort(staged.models_.begin(), staged.models_.end(),
              [](const StagedScene::Entry& a, const StagedScene::Entry& b) { return id_less(a.id, b.id); });
    return staged;
}

void RendererFrameSink::activate_scene(StagedScene&& scene)
{
    active_ = std::move(scene);  // the previous scene's references are released here
}

void RendererFrameSink::submit(const saige::FrameView& frame)
{
    ++stats_.frames_submitted;
    // Copy what is retained: the FrameView is only valid during submit().
    instances_.clear();
    model_draws_.clear();
    for (const saige::RenderInstance& inst : frame.instances) {
        const GpuModel* model = nullptr;
        if (!inst.mesh.is_builtin()) {
            model = active_.find(inst.mesh);
            if (model == nullptr) {
                ++stats_.unresolved_instances;  // names an asset that is not staged: a fallback cube, never nothing
            }
        }
        if (model != nullptr) {
            Mat4 world = Mat4::identity();
            if (inst.world_matrix) {
                world = to_render_matrix(*inst.world_matrix);
            } else {  // no authoritative matrix: place the model at the instance position, unrotated
                world.at(0, 3) = inst.position.x;
                world.at(1, 3) = inst.position.y;
                world.at(2, 3) = inst.position.z;
            }
            model_draws_.push_back({model, world});
        } else {
            instances_.push_back(to_scene_instance(inst));
        }
    }
    stats_.last_instance_count = frame.instances.size();
    SceneFrame scene;
    scene.tick = frame.tick;
    scene.alpha = frame.alpha;
    scene.instances = instances_;
    scene.models = model_draws_;
    if (frame.camera) {
        camera_ = to_render_camera(*frame.camera);
        scene.camera = &camera_;  // a missing camera keeps the 0.1 follow camera
    }
    if (platform_ != nullptr) {
        submit_windowed(scene);
    } else {
        submit_offscreen(scene);
    }
}

void RendererFrameSink::sync_surface()
{
    const saige::SurfaceState surface = platform_->surface();
    const bool drawable = !surface.minimized && surface.width_px > 0 && surface.height_px > 0;
    if (surface.revision != surface_revision_ || recreate_pending_) {
        surface_revision_ = surface.revision;
        recreate_pending_ = false;
        const uint32_t w = drawable ? surface.width_px : 0u;
        const uint32_t h = drawable ? surface.height_px : 0u;
        if (w != width_ || h != height_ || drawable != surface_drawable_ || drawable) {
            device_->resize_swapchain(swapchain_, w, h);
            ++stats_.swapchain_recreations;
        }
        width_ = w;
        height_ = h;
    }
    surface_drawable_ = drawable;
}

void RendererFrameSink::submit_windowed(const SceneFrame& frame)
{
    sync_surface();
    if (!surface_drawable_) {
        ++stats_.frames_skipped;  // minimized or zero extent: defer presentation (contract)
        return;
    }
    rhi::CommandList& cmd = device_->begin_frame();
    const rhi::AcquireResult image = device_->acquire_next_image(swapchain_);
    if (image.status != rhi::AcquireStatus::Success) {
        (void)device_->end_frame();
        recreate_pending_ = image.status == rhi::AcquireStatus::OutOfDate;
        ++stats_.frames_skipped;
        // With a renderer-paced host (Core #12) nothing else sleeps while the window is drawable, so yield briefly
        // instead of spinning while the swapchain recovers.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return;
    }
    RenderTarget target;
    target.texture = image.texture;
    target.width = width_;
    target.height = height_;
    target.initial_state = swapchain_acquired;
    target.final_state = swapchain_present;
    const FrameStats fs = renderer_->render(cmd, frame, target);
    stats_.model_instances_drawn += fs.model_instances;
    stats_.model_instances_skipped += fs.model_instances_skipped;
    const rhi::FrameResult result = device_->end_frame();
    if (result.swapchain_out_of_date) {
        recreate_pending_ = true;
    }
    ++stats_.frames_rendered;
}

void RendererFrameSink::submit_offscreen(const SceneFrame& frame)
{
    rhi::CommandList& cmd = device_->begin_frame();
    RenderTarget target;
    target.texture = offscreen_color_;
    target.width = width_;
    target.height = height_;
    target.initial_state = offscreen_state_;
    target.readback_buffer = readback_;
    const FrameStats fs = renderer_->render(cmd, frame, target);
    offscreen_state_ = {rhi::PipelineStage::Transfer, rhi::Access::TransferRead, rhi::TextureLayout::TransferSrc};
    stats_.model_instances_drawn += fs.model_instances;
    stats_.model_instances_skipped += fs.model_instances_skipped;
    (void)device_->end_frame();
    has_frame_ = true;
    ++stats_.frames_rendered;
}

std::vector<uint8_t> RendererFrameSink::read_back_rgba()
{
    if (platform_ != nullptr || !has_frame_) {
        return {};
    }
    device_->wait_idle();
    const std::span<std::byte> bytes = device_->map(readback_);
    std::vector<uint8_t> out(size_t{width_} * height_ * 4);
    if (bytes.size() >= out.size()) {
        std::memcpy(out.data(), bytes.data(), out.size());
    }
    return out;
}

} // namespace saige::render
