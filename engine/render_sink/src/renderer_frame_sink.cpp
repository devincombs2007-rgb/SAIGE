#include "saige/render_sink/renderer_frame_sink.hpp"

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

std::vector<SceneInstance> to_scene_instances(std::span<const saige::RenderInstance> instances)
{
    // Linear-space albedos; entity 0 (the sample's marker) gets the signature orange.
    static constexpr std::array<Float3, 6> palette{{{0.85f, 0.30f, 0.10f},
                                                    {0.15f, 0.45f, 0.85f},
                                                    {0.20f, 0.70f, 0.30f},
                                                    {0.80f, 0.70f, 0.15f},
                                                    {0.60f, 0.25f, 0.75f},
                                                    {0.85f, 0.85f, 0.85f}}};
    std::vector<SceneInstance> out;
    out.reserve(instances.size());
    for (const saige::RenderInstance& inst : instances) {
        SceneInstance s;
        s.position = {inst.position.x, inst.position.y, inst.position.z};
        s.color = palette[inst.entity.index % palette.size()];
        s.yaw_offset = static_cast<float>(inst.entity.index) * 0.7f;
        out.push_back(s);
    }
    return out;
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

void RendererFrameSink::submit(const saige::FrameView& frame)
{
    ++stats_.frames_submitted;
    instances_ = to_scene_instances(frame.instances);  // copy: the span is only valid during submit()
    stats_.last_instance_count = instances_.size();
    SceneFrame scene;
    scene.tick = frame.tick;
    scene.alpha = frame.alpha;
    scene.instances = instances_;
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
    (void)renderer_->render(cmd, frame, target);
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
    target.readback_buffer = readback_;
    (void)renderer_->render(cmd, frame, target);
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
