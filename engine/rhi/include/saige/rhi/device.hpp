// SAIGE RHI — device and command list interfaces implemented by each backend (null, Vulkan, ...).
//
// Design: docs/rendering/rhi-design.md. 0.1 scope: buffers, textures, SPIR-V shaders, graphics pipelines with
// push constants, dynamic rendering, synchronization2-style barriers, swapchains, and texture readback.
// Bindless descriptors, compute and ray tracing extend this interface in later phases.
#pragma once

#include "saige/rhi/barrier.hpp"
#include "saige/rhi/format.hpp"
#include "saige/rhi/handle.hpp"
#include "saige/rhi/resource_desc.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace saige::rhi {

enum class Backend : uint8_t { Null, Vulkan };

// ---------------------------------------------------------------------------------------------------------------------
// Shaders and pipelines
// ---------------------------------------------------------------------------------------------------------------------

enum class ShaderStage : uint8_t { Vertex, Pixel, Compute };

struct ShaderDesc {
    ShaderStage stage = ShaderStage::Vertex;
    std::span<const uint32_t> spirv;  ///< SPIR-V words (copied by the device).
    std::string entry_point = "main";
    const char* debug_name = nullptr;
};

enum class PrimitiveTopology : uint8_t { TriangleList, TriangleStrip, LineList };
enum class CullMode : uint8_t { None, Front, Back };
enum class FrontFace : uint8_t { CounterClockwise, Clockwise };
enum class IndexType : uint8_t { Uint16, Uint32 };

struct VertexAttribute {
    uint32_t location = 0;
    Format format = Format::Unknown;
    uint32_t offset = 0;
};

/// One interleaved, per-vertex binding (binding 0). An empty layout means no vertex input.
struct VertexLayout {
    uint32_t stride = 0;
    std::vector<VertexAttribute> attributes;
};

struct DepthState {
    bool test = false;
    bool write = false;
    CompareOp compare = CompareOp::Greater;  ///< Reverse-Z: greater is closer.
};

struct GraphicsPipelineDesc {
    ShaderHandle vertex_shader;
    ShaderHandle pixel_shader;
    VertexLayout vertex_layout;
    PrimitiveTopology topology = PrimitiveTopology::TriangleList;
    CullMode cull_mode = CullMode::Back;
    FrontFace front_face = FrontFace::CounterClockwise;
    DepthState depth;
    std::vector<Format> color_formats;
    Format depth_format = Format::Unknown;
    uint32_t push_constant_size = 0;  ///< Bytes visible to vertex and pixel stages (<= 128).
    const char* debug_name = nullptr;
};

// ---------------------------------------------------------------------------------------------------------------------
// Rendering commands
// ---------------------------------------------------------------------------------------------------------------------

enum class AttachmentLoadOp : uint8_t { Load, Clear, DontCare };
enum class AttachmentStoreOp : uint8_t { Store, DontCare };

struct ClearColor {
    float r = 0.0f, g = 0.0f, b = 0.0f, a = 1.0f;
};

struct ColorAttachment {
    TextureHandle texture;
    AttachmentLoadOp load = AttachmentLoadOp::Clear;
    AttachmentStoreOp store = AttachmentStoreOp::Store;
    ClearColor clear;
};

struct DepthAttachment {
    TextureHandle texture;
    AttachmentLoadOp load = AttachmentLoadOp::Clear;
    AttachmentStoreOp store = AttachmentStoreOp::DontCare;
    float clear_depth = 0.0f;  ///< Reverse-Z far plane.
};

struct Rect2D {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

/// (x, y) is the top-left corner in framebuffer pixels, like Rect2D. Backends map clip space so that NDC +Y is
/// up on screen on every API (Vulkan does this with a negative-height viewport), so projection matrices
/// never need backend-specific flips.
struct Viewport {
    float x = 0.0f;
    float y = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float min_depth = 0.0f;
    float max_depth = 1.0f;
};

struct RenderingDesc {
    std::vector<ColorAttachment> colors;
    std::optional<DepthAttachment> depth;
    Rect2D area;
};

/// Records GPU work for one frame. Obtained from Device::begin_frame(); valid until Device::end_frame().
class CommandList {
public:
    virtual ~CommandList() = default;

    virtual void barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers = {}) = 0;
    virtual void begin_rendering(const RenderingDesc& desc) = 0;
    virtual void end_rendering() = 0;
    virtual void set_viewport(const Viewport& viewport) = 0;
    virtual void set_scissor(const Rect2D& rect) = 0;
    virtual void bind_pipeline(PipelineHandle pipeline) = 0;
    virtual void push_constants(std::span<const std::byte> data, uint32_t offset = 0) = 0;
    virtual void bind_vertex_buffer(BufferHandle buffer, uint64_t offset = 0) = 0;
    virtual void bind_index_buffer(BufferHandle buffer, IndexType type, uint64_t offset = 0) = 0;
    virtual void draw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0,
                      uint32_t first_instance = 0) = 0;
    virtual void draw_indexed(uint32_t index_count, uint32_t instance_count = 1, uint32_t first_index = 0,
                              int32_t vertex_offset = 0, uint32_t first_instance = 0) = 0;
    /// Copies mip 0 / layer 0 of `texture` (in TransferSrc layout) into `buffer`, tightly packed rows.
    virtual void copy_texture_to_buffer(TextureHandle texture, BufferHandle buffer) = 0;
    virtual void begin_debug_label(const char* name) = 0;
    virtual void end_debug_label() = 0;

    /// Pushes a trivially copyable struct (layout shared with the shader, size checked by the caller's static_assert).
    template <typename T>
        requires std::is_trivially_copyable_v<T>
    void push_constant_struct(const T& value, uint32_t offset = 0)
    {
        const std::span<const std::byte> bytes(reinterpret_cast<const std::byte*>(&value), sizeof(T));
        push_constants(bytes, offset);
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Presentation
// ---------------------------------------------------------------------------------------------------------------------

/// Native window handles, borrowed from the platform layer. The RHI never owns the window.
/// Core's `saige::NativeWindowHandle` maps onto this 1:1 (the adapter converts it).
struct NativeSurface {
    enum class System : uint8_t { Headless, Win32, Xlib };
    System system = System::Headless;
    void* display = nullptr;   ///< HINSTANCE | Display*
    uintptr_t window = 0;      ///< HWND | Window
};

struct SwapchainDesc {
    NativeSurface surface;
    uint32_t width = 0;
    uint32_t height = 0;
    bool vsync = true;
    Format preferred_format = Format::BGRA8_SRGB;
    uint32_t min_image_count = 3;
    const char* debug_name = nullptr;
};

enum class AcquireStatus : uint8_t {
    Success,
    OutOfDate,  ///< Recreate the swapchain (resize) before rendering.
    NotReady,   ///< Zero-sized or otherwise unpresentable; skip this frame.
};

struct AcquireResult {
    AcquireStatus status = AcquireStatus::NotReady;
    TextureHandle texture;  ///< Valid on Success. Owned by the swapchain.
};

// ---------------------------------------------------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------------------------------------------------

struct DeviceDesc {
    bool enable_validation = false;   ///< Request API validation (Vulkan: VK_LAYER_KHRONOS_validation).
    bool require_validation = false;  ///< Fail creation instead of continuing when validation is unavailable.
    bool require_presentation = false;  ///< Fail creation if swapchains are unsupported.
    uint32_t frames_in_flight = 2;
    std::string application_name = "SAIGE";
};

struct DeviceInfo {
    Backend backend = Backend::Null;
    std::string device_name;
    std::string driver_name;
    uint32_t api_version_major = 0;
    uint32_t api_version_minor = 0;
    bool validation_enabled = false;
    bool presentation_supported = false;
    bool software_device = false;  ///< CPU implementation (e.g. lavapipe).
};

struct FrameResult {
    bool presented = false;
    bool swapchain_out_of_date = false;  ///< Recreate before the next frame.
};

/// GPU device. Single-threaded: create, record and submit on one thread (0.1 contract, D-08).
///
/// Frame protocol:
///   CommandList& cmd = device.begin_frame();          // waits until the frame slot's GPU work is done
///   AcquireResult img = device.acquire_next_image(sc);  // optional, once per swapchain per frame
///   ... record into cmd ...
///   FrameResult r = device.end_frame();               // submits, presents acquired images
class Device {
public:
    virtual ~Device() = default;

    [[nodiscard]] virtual const DeviceInfo& info() const noexcept = 0;

    // Resources. Creation validates descriptions and throws std::invalid_argument on invalid input,
    // std::runtime_error on backend failure. Destruction is deferred until the GPU no longer uses the object.
    virtual BufferHandle create_buffer(const BufferDesc& desc, std::span<const std::byte> initial_data = {}) = 0;
    virtual TextureHandle create_texture(const TextureDesc& desc) = 0;
    /// Synchronously uploads the full contents of `texture` (every mip and layer) and leaves it in
    /// `sampled_texture_state`. `data` is tightly packed, mip level outermost, then array layer, then rows, and must
    /// be exactly `compute_texture_data_size(desc)` bytes. The texture needs `Sampled | TransferDst` usage, one sample
    /// and a 2D dimension; anything else throws std::invalid_argument. Must not be called between begin_frame() and
    /// end_frame(). Intended for load time (0.2 loads synchronously; a streaming path comes with measurements).
    virtual void upload_texture(TextureHandle texture, std::span<const std::byte> data) = 0;
    /// Samplers are immutable and cheap to share; callers may deduplicate with `hash_sampler_desc`.
    virtual SamplerHandle create_sampler(const SamplerDesc& desc) = 0;
    virtual ShaderHandle create_shader(const ShaderDesc& desc) = 0;
    virtual PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc) = 0;
    virtual void destroy(BufferHandle handle) = 0;
    virtual void destroy(TextureHandle handle) = 0;
    virtual void destroy(SamplerHandle handle) = 0;
    virtual void destroy(ShaderHandle handle) = 0;
    virtual void destroy(PipelineHandle handle) = 0;

    /// Persistent CPU mapping of a CpuToGpu or GpuToCpu buffer. Empty span for GpuOnly buffers.
    [[nodiscard]] virtual std::span<std::byte> map(BufferHandle handle) = 0;
    /// Description of a live texture (by value: never a pointer into backend storage, which resource creation
    /// may reallocate). nullopt for invalid or destroyed handles.
    [[nodiscard]] virtual std::optional<TextureDesc> texture_desc(TextureHandle handle) const = 0;

    // Presentation
    virtual SwapchainHandle create_swapchain(const SwapchainDesc& desc) = 0;
    /// Recreates the swapchain images at the new size. A zero extent is allowed (acquire returns NotReady).
    virtual void resize_swapchain(SwapchainHandle handle, uint32_t width, uint32_t height) = 0;
    virtual void destroy(SwapchainHandle handle) = 0;
    [[nodiscard]] virtual Format swapchain_format(SwapchainHandle handle) const = 0;

    // Frames
    virtual CommandList& begin_frame() = 0;
    virtual AcquireResult acquire_next_image(SwapchainHandle handle) = 0;
    virtual FrameResult end_frame() = 0;
    /// Blocks until all submitted GPU work has completed.
    virtual void wait_idle() = 0;
    [[nodiscard]] virtual uint64_t frame_number() const noexcept = 0;

    /// Validation errors reported by the backend since creation (always 0 without validation).
    [[nodiscard]] virtual uint32_t validation_error_count() const noexcept = 0;
};

} // namespace saige::rhi
