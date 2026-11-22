#include "saige/rhi_vulkan/vulkan_device.hpp"

#include "vk_api.hpp"

#include "saige/rhi/deferred_release_queue.hpp"
#include "saige/rhi/handle_pool.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <unordered_map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace saige::rhi {

namespace {

#if defined(SAIGE_RHI_VULKAN_TESTING)
std::atomic<uint32_t> g_process_validation_errors{0};  // all devices, including errors reported during destruction
std::atomic<uint32_t> g_memory_bookkeeping_fault{0};  // test hook: 1-based countdown to an injected failure; 0 = off

/// True exactly once, for the registration the test asked to fail.
bool consume_memory_bookkeeping_fault() noexcept
{
    uint32_t n = g_memory_bookkeeping_fault.load(std::memory_order_relaxed);
    while (n != 0) {
        if (g_memory_bookkeeping_fault.compare_exchange_weak(n, n - 1, std::memory_order_relaxed)) {
            return n == 1;
        }
    }
    return false;
}
#else
constexpr bool consume_memory_bookkeeping_fault() noexcept { return false; }  // production builds carry no injection point
#endif

using vk::Api;

constexpr const char* validation_layer = "VK_LAYER_KHRONOS_validation";
constexpr uint32_t max_push_constants = 128;

[[noreturn]] void fail(const std::string& what, VkResult result)
{
    throw std::runtime_error("Vulkan: " + what + " failed (VkResult " + std::to_string(static_cast<int>(result)) + ")");
}

void check(VkResult result, const char* what)
{
    if (result != VK_SUCCESS) {
        fail(what, result);
    }
}

// ----------------------------------------------------------------------------------------------- enum mapping

VkFormat to_vk(Format f)
{
    switch (f) {
    case Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
    case Format::R8_SNORM: return VK_FORMAT_R8_SNORM;
    case Format::R8_UINT: return VK_FORMAT_R8_UINT;
    case Format::R8_SINT: return VK_FORMAT_R8_SINT;
    case Format::RG8_UNORM: return VK_FORMAT_R8G8_UNORM;
    case Format::RG8_SNORM: return VK_FORMAT_R8G8_SNORM;
    case Format::RG8_UINT: return VK_FORMAT_R8G8_UINT;
    case Format::RG8_SINT: return VK_FORMAT_R8G8_SINT;
    case Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::RGBA8_SNORM: return VK_FORMAT_R8G8B8A8_SNORM;
    case Format::RGBA8_UINT: return VK_FORMAT_R8G8B8A8_UINT;
    case Format::RGBA8_SINT: return VK_FORMAT_R8G8B8A8_SINT;
    case Format::RGBA8_SRGB: return VK_FORMAT_R8G8B8A8_SRGB;
    case Format::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::BGRA8_SRGB: return VK_FORMAT_B8G8R8A8_SRGB;
    case Format::R16_UNORM: return VK_FORMAT_R16_UNORM;
    case Format::R16_SNORM: return VK_FORMAT_R16_SNORM;
    case Format::R16_UINT: return VK_FORMAT_R16_UINT;
    case Format::R16_SINT: return VK_FORMAT_R16_SINT;
    case Format::R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case Format::RG16_UNORM: return VK_FORMAT_R16G16_UNORM;
    case Format::RG16_SNORM: return VK_FORMAT_R16G16_SNORM;
    case Format::RG16_UINT: return VK_FORMAT_R16G16_UINT;
    case Format::RG16_SINT: return VK_FORMAT_R16G16_SINT;
    case Format::RG16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case Format::RGBA16_UNORM: return VK_FORMAT_R16G16B16A16_UNORM;
    case Format::RGBA16_SNORM: return VK_FORMAT_R16G16B16A16_SNORM;
    case Format::RGBA16_UINT: return VK_FORMAT_R16G16B16A16_UINT;
    case Format::RGBA16_SINT: return VK_FORMAT_R16G16B16A16_SINT;
    case Format::RGBA16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case Format::R32_UINT: return VK_FORMAT_R32_UINT;
    case Format::R32_SINT: return VK_FORMAT_R32_SINT;
    case Format::R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case Format::RG32_UINT: return VK_FORMAT_R32G32_UINT;
    case Format::RG32_SINT: return VK_FORMAT_R32G32_SINT;
    case Format::RG32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case Format::RGB32_UINT: return VK_FORMAT_R32G32B32_UINT;
    case Format::RGB32_SINT: return VK_FORMAT_R32G32B32_SINT;
    case Format::RGB32_FLOAT: return VK_FORMAT_R32G32B32_SFLOAT;
    case Format::RGBA32_UINT: return VK_FORMAT_R32G32B32A32_UINT;
    case Format::RGBA32_SINT: return VK_FORMAT_R32G32B32A32_SINT;
    case Format::RGBA32_FLOAT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case Format::RGB10A2_UNORM: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case Format::RGB10A2_UINT: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case Format::RG11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case Format::RGB9E5_UFLOAT: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case Format::D16_UNORM: return VK_FORMAT_D16_UNORM;
    case Format::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32_FLOAT: return VK_FORMAT_D32_SFLOAT;
    case Format::D32_FLOAT_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case Format::BC1_UNORM: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case Format::BC1_SRGB: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case Format::BC2_UNORM: return VK_FORMAT_BC2_UNORM_BLOCK;
    case Format::BC2_SRGB: return VK_FORMAT_BC2_SRGB_BLOCK;
    case Format::BC3_UNORM: return VK_FORMAT_BC3_UNORM_BLOCK;
    case Format::BC3_SRGB: return VK_FORMAT_BC3_SRGB_BLOCK;
    case Format::BC4_UNORM: return VK_FORMAT_BC4_UNORM_BLOCK;
    case Format::BC4_SNORM: return VK_FORMAT_BC4_SNORM_BLOCK;
    case Format::BC5_UNORM: return VK_FORMAT_BC5_UNORM_BLOCK;
    case Format::BC5_SNORM: return VK_FORMAT_BC5_SNORM_BLOCK;
    case Format::BC6H_UFLOAT: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    case Format::BC6H_SFLOAT: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case Format::BC7_UNORM: return VK_FORMAT_BC7_UNORM_BLOCK;
    case Format::BC7_SRGB: return VK_FORMAT_BC7_SRGB_BLOCK;
    case Format::Unknown:
    case Format::Count: break;
    }
    return VK_FORMAT_UNDEFINED;
}

Format from_vk_surface_format(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_B8G8R8A8_SRGB: return Format::BGRA8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return Format::RGBA8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM: return Format::BGRA8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return Format::RGBA8_UNORM;
    default: return Format::Unknown;
    }
}

VkPipelineStageFlags2 to_vk(PipelineStage s)
{
    if (s == PipelineStage::None) {
        return VK_PIPELINE_STAGE_2_NONE;
    }
    if (has_any(s, PipelineStage::AllCommands)) {
        return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    VkPipelineStageFlags2 r = 0;
    const auto map = [&](PipelineStage bit, VkPipelineStageFlags2 vk_bit) {
        if (has_any(s, bit)) {
            r |= vk_bit;
        }
    };
    map(PipelineStage::DrawIndirect, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT);
    map(PipelineStage::VertexInput, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT);
    map(PipelineStage::VertexShader, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT);
    map(PipelineStage::PixelShader, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT);
    map(PipelineStage::EarlyFragmentTests, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT);
    map(PipelineStage::LateFragmentTests, VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT);
    map(PipelineStage::ColorAttachmentOutput, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT);
    map(PipelineStage::ComputeShader, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    map(PipelineStage::Transfer, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT);
    map(PipelineStage::Host, VK_PIPELINE_STAGE_2_HOST_BIT);
    // Mesh/task/ray tracing/AS build stages need their extensions (later phases); be conservative meanwhile.
    if (has_any(s, PipelineStage::TaskShader | PipelineStage::MeshShader | PipelineStage::RayTracingShader |
                       PipelineStage::AccelerationStructureBuild)) {
        r |= VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    return r;
}

VkAccessFlags2 to_vk(Access a)
{
    VkAccessFlags2 r = 0;
    const auto map = [&](Access bit, VkAccessFlags2 vk_bit) {
        if (has_any(a, bit)) {
            r |= vk_bit;
        }
    };
    map(Access::IndirectCommandRead, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT);
    map(Access::IndexRead, VK_ACCESS_2_INDEX_READ_BIT);
    map(Access::VertexAttributeRead, VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
    map(Access::UniformRead, VK_ACCESS_2_UNIFORM_READ_BIT);
    map(Access::ShaderSampledRead, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    map(Access::ShaderStorageRead, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    map(Access::ShaderStorageWrite, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    map(Access::ColorAttachmentRead, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    map(Access::ColorAttachmentWrite, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    map(Access::DepthStencilRead, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);
    map(Access::DepthStencilWrite, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
    map(Access::TransferRead, VK_ACCESS_2_TRANSFER_READ_BIT);
    map(Access::TransferWrite, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    map(Access::HostRead, VK_ACCESS_2_HOST_READ_BIT);
    map(Access::HostWrite, VK_ACCESS_2_HOST_WRITE_BIT);
    return r;
}

VkImageLayout to_vk(TextureLayout l)
{
    switch (l) {
    case TextureLayout::Undefined: return VK_IMAGE_LAYOUT_UNDEFINED;
    case TextureLayout::General: return VK_IMAGE_LAYOUT_GENERAL;
    case TextureLayout::ColorAttachment: return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case TextureLayout::DepthStencilAttachment: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case TextureLayout::DepthStencilReadOnly: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    case TextureLayout::ShaderReadOnly: return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case TextureLayout::TransferSrc: return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case TextureLayout::TransferDst: return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case TextureLayout::Present: return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    }
    return VK_IMAGE_LAYOUT_UNDEFINED;
}

VkFilter to_vk(Filter f) { return f == Filter::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; }

VkSamplerMipmapMode to_vk(MipmapMode m)
{
    return m == MipmapMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
}

VkSamplerAddressMode to_vk(AddressMode a)
{
    switch (a) {
    case AddressMode::Repeat: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case AddressMode::MirroredRepeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case AddressMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case AddressMode::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case AddressMode::MirrorClampToEdge: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    }
    return VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

VkBorderColor to_vk(BorderColor b)
{
    switch (b) {
    case BorderColor::TransparentBlack: return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    case BorderColor::OpaqueBlack: return VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    case BorderColor::OpaqueWhite: return VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    }
    return VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
}

VkCompareOp to_vk(CompareOp op)
{
    switch (op) {
    case CompareOp::Never: return VK_COMPARE_OP_NEVER;
    case CompareOp::Less: return VK_COMPARE_OP_LESS;
    case CompareOp::Equal: return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessOrEqual: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater: return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual: return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterOrEqual: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always: return VK_COMPARE_OP_ALWAYS;
    }
    return VK_COMPARE_OP_ALWAYS;
}

VkImageAspectFlags aspect_of(Format f)
{
    if (has_depth(f)) {
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (has_stencil(f)) {
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        return aspect;
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

// ----------------------------------------------------------------------------------------------- resources

struct VkBufferRes {
    BufferDesc desc;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    std::byte* mapped = nullptr;
};

struct VkTextureRes {
    TextureDesc desc;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    bool swapchain_image = false;
};

struct VkSamplerRes {
    SamplerDesc desc;
    VkSampler sampler = VK_NULL_HANDLE;
};

struct VkBindGroupLayoutRes {
    BindGroupLayoutDesc desc;
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
};

struct VkBindGroupRes {
    BindGroupLayoutHandle layout_handle;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    // Non-owning references, only the ones the layout binds. Checked when the group is bound (see bind_group).
    std::vector<TextureHandle> textures;
    std::vector<SamplerHandle> samplers;
    std::vector<BufferHandle> buffers;
};

struct VkShaderRes {
    ShaderStage stage = ShaderStage::Vertex;
    std::string entry;
    VkShaderModule module = VK_NULL_HANDLE;
};

struct VkPipelineRes {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    uint32_t push_constant_size = 0;
    std::vector<BindGroupLayoutHandle> bind_group_layouts;  ///< set index = position
};

struct VkSwapchainRes {
    SwapchainDesc desc;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    Format format = Format::Unknown;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D extent{};
    std::vector<TextureHandle> images;
    std::vector<VkSemaphore> render_finished;  // one per image (signaled by the frame, waited by present)
};

struct FrameSlot {
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    std::vector<VkSemaphore> acquire_semaphores;  // one per swapchain acquired in this slot
    uint64_t timeline_value = 0;                  // signaled when this slot's last submission completes
    uint64_t frame_number = 0;                    // the frame this slot last recorded
    bool recorded = false;                        // the slot has been used (its timestamp queries hold a frame's results)
};

struct PendingPresent {
    SwapchainHandle swapchain;
    uint32_t image_index = 0;
    VkSemaphore acquire = VK_NULL_HANDLE;
};

} // namespace

// =====================================================================================================================

class VulkanDevice;

class VulkanCommandList final : public CommandList {
public:
    explicit VulkanCommandList(VulkanDevice& device) : device_(device) {}
    void begin(VkCommandBuffer cmd)
    {
        cmd_ = cmd;
        bound_layout_ = VK_NULL_HANDLE;
        bound_push_size_ = 0;
        written_timestamps_ = 0;
        bound_pipeline_ = {};
        bound_groups_.fill({});
    }

    void barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers) override;
    void begin_rendering(const RenderingDesc& desc) override;
    void end_rendering() override;
    void set_viewport(const Viewport& viewport) override;
    void set_scissor(const Rect2D& rect) override;
    void bind_pipeline(PipelineHandle pipeline) override;
    void write_timestamp(uint32_t index) override;
    void bind_group(uint32_t set, BindGroupHandle group) override;
    void push_constants(std::span<const std::byte> data, uint32_t offset) override;
    void bind_vertex_buffer(BufferHandle buffer, uint64_t offset) override;
    void bind_index_buffer(BufferHandle buffer, IndexType type, uint64_t offset) override;
    void draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) override;
    void draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset,
                      uint32_t first_instance) override;
    void copy_texture_to_buffer(TextureHandle texture, BufferHandle buffer) override;
    void begin_debug_label(const char* name) override;
    void end_debug_label() override;

private:
    void check_draw_state(const char* what) const;

    VulkanDevice& device_;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    uint32_t written_timestamps_ = 0;  // bit i: write_timestamp(i) was recorded in this frame (once per index)
    VkPipelineLayout bound_layout_ = VK_NULL_HANDLE;
    uint32_t bound_push_size_ = 0;
    PipelineHandle bound_pipeline_;
    std::array<BindGroupHandle, max_bind_groups_per_pipeline> bound_groups_{};
};

class VulkanDevice final : public Device {
public:
    explicit VulkanDevice(const DeviceDesc& desc);
    ~VulkanDevice() override;

    const DeviceInfo& info() const noexcept override { return info_; }

    BufferHandle create_buffer(const BufferDesc& desc, std::span<const std::byte> initial_data) override;
    TextureHandle create_texture(const TextureDesc& desc) override;
    void upload_texture(TextureHandle texture, std::span<const std::byte> data) override;
    SamplerHandle create_sampler(const SamplerDesc& desc) override;
    void destroy(SamplerHandle handle) override;
    BindGroupLayoutHandle create_bind_group_layout(const BindGroupLayoutDesc& desc) override;
    BindGroupHandle create_bind_group(BindGroupLayoutHandle layout, std::span<const BindGroupEntry> entries) override;
    void destroy(BindGroupLayoutHandle handle) override;
    void destroy(BindGroupHandle handle) override;
    ShaderHandle create_shader(const ShaderDesc& desc) override;
    PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc) override;
    void destroy(BufferHandle handle) override;
    void destroy(TextureHandle handle) override;
    void destroy(ShaderHandle handle) override;
    void destroy(PipelineHandle handle) override;
    std::span<std::byte> map(BufferHandle handle) override;
    std::optional<TextureDesc> texture_desc(TextureHandle handle) const override;

    SwapchainHandle create_swapchain(const SwapchainDesc& desc) override;
    void resize_swapchain(SwapchainHandle handle, uint32_t width, uint32_t height) override;
    void destroy(SwapchainHandle handle) override;
    Format swapchain_format(SwapchainHandle handle) const override;

    CommandList& begin_frame() override;
    AcquireResult acquire_next_image(SwapchainHandle handle) override;
    FrameResult end_frame() override;
    void wait_idle() override;
    uint64_t frame_number() const noexcept override { return frame_number_; }
    uint32_t validation_error_count() const noexcept override { return validation_errors_.load(); }
    std::optional<GpuTimestamps> completed_timestamps() const override;
    DeviceMemoryStats memory_stats() const override;

private:
    friend class VulkanCommandList;

    static VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                         VkDebugUtilsMessageTypeFlagsEXT types,
                                                         const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                         void* user);

    void create_instance();
    void pick_physical_device();
    void create_logical_device();
    void create_frame_resources();
    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const;
    VkDeviceMemory allocate(const VkMemoryRequirements& req, MemoryLocation location);
    void free_memory(VkDeviceMemory memory);
    void collect_timestamps(FrameSlot& slot);
    void probe_capabilities();
    void set_name(VkObjectType type, uint64_t handle, const char* name);
    VkDescriptorSet allocate_descriptor_set(VkDescriptorSetLayout layout, VkDescriptorPool& pool_out);
    bool group_resources_alive(const VkBindGroupRes& g) const;
    void immediate_submit(const std::function<void(VkCommandBuffer)>& record);
    uint64_t retire_value() const noexcept { return in_frame_ ? timeline_counter_ + 1 : timeline_counter_; }
    void defer(std::function<void()> fn) { pending_.enqueue(std::move(fn), retire_value()); }
    uint64_t completed_value() const;
    void collect_garbage();
    void build_swapchain(VkSwapchainRes& sc);
    void release_swapchain_images(VkSwapchainRes& sc);

    Api vk_;
    DeviceDesc desc_;
    DeviceInfo info_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_props_{};
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    bool surface_extensions_ = false;
    bool swapchain_extension_ = false;
    bool debug_utils_ = false;

    VkSemaphore timeline_ = VK_NULL_HANDLE;
    uint64_t timeline_counter_ = 0;
    std::vector<FrameSlot> slots_;
    uint32_t slot_index_ = 0;
    uint64_t frame_number_ = 0;
    // GPU timestamps: one block of max_gpu_timestamps queries per frame slot.
    VkQueryPool timestamp_pool_ = VK_NULL_HANDLE;
    bool timestamps_ = false;
    uint32_t selected_api_version_ = 0;                 // the chosen physical device's own apiVersion
    std::vector<std::string> selected_extensions_;      // the chosen device's advertised extensions
    uint32_t timestamp_valid_bits_ = 0;
    float timestamp_period_ns_ = 0.0f;
    std::optional<GpuTimestamps> completed_timestamps_;
    // Device memory the backend allocated, exact: every vkAllocateMemory/vkFreeMemory goes through allocate/free_memory.
    std::unordered_map<VkDeviceMemory, uint64_t> allocation_sizes_;
    uint64_t allocated_bytes_ = 0;
    bool memory_budget_ = false;
    bool in_frame_ = false;
    bool anisotropy_supported_ = false;
    std::vector<PendingPresent> presents_;
    VkCommandPool upload_pool_ = VK_NULL_HANDLE;

    HandlePool<VkBufferRes, BufferTag> buffers_;
    HandlePool<VkTextureRes, TextureTag> textures_;
    HandlePool<VkSamplerRes, SamplerTag> samplers_;
    HandlePool<VkBindGroupLayoutRes, BindGroupLayoutTag> bind_group_layouts_;
    HandlePool<VkBindGroupRes, BindGroupTag> bind_groups_;
    std::vector<VkDescriptorPool> descriptor_pools_;  ///< sets are allocated from the back; grows when it is full
    HandlePool<VkShaderRes, ShaderTag> shaders_;
    HandlePool<VkPipelineRes, PipelineTag> pipelines_;
    HandlePool<VkSwapchainRes, SwapchainTag> swapchains_;
    DeferredReleaseQueue<std::function<void()>> pending_;
    std::atomic<uint32_t> validation_errors_{0};
    VulkanCommandList command_list_{*this};
};

// =====================================================================================================================
// Creation
// =====================================================================================================================

VkBool32 VulkanDevice::debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                      VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                      const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    auto* self = static_cast<VulkanDevice*>(user);
    const char* label = "info";
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
        label = "error";
        self->validation_errors_.fetch_add(1);
#if defined(SAIGE_RHI_VULKAN_TESTING)
        g_process_validation_errors.fetch_add(1);  // survives the device: leaks are only reported while it is destroyed
#endif
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0) {
        label = "warning";
    }
    std::cerr << "[saige.vulkan] validation " << label << ": " << (data->pMessage != nullptr ? data->pMessage : "")
              << '\n';
    return VK_FALSE;
}

VulkanDevice::VulkanDevice(const DeviceDesc& desc) : desc_(desc)
{
    if (desc_.frames_in_flight == 0) {
        desc_.frames_in_flight = 1;
    }
    std::string error;
    if (!vk_.load_library(&error)) {
        throw std::runtime_error("Vulkan: " + error);
    }
    try {
        create_instance();
        pick_physical_device();
        create_logical_device();
        create_frame_resources();
    } catch (...) {
        // Partial construction: tear down whatever exists, then rethrow.
        if (device_ != VK_NULL_HANDLE) {
            vk_.vkDeviceWaitIdle(device_);
            for (FrameSlot& s : slots_) {
                for (VkSemaphore sem : s.acquire_semaphores) {
                    vk_.vkDestroySemaphore(device_, sem, nullptr);
                }
                if (s.pool != VK_NULL_HANDLE) {
                    vk_.vkDestroyCommandPool(device_, s.pool, nullptr);
                }
            }
            if (upload_pool_ != VK_NULL_HANDLE) {
                vk_.vkDestroyCommandPool(device_, upload_pool_, nullptr);
            }
            if (timeline_ != VK_NULL_HANDLE) {
                vk_.vkDestroySemaphore(device_, timeline_, nullptr);
            }
            vk_.vkDestroyDevice(device_, nullptr);
        }
        if (messenger_ != VK_NULL_HANDLE) {
            vk_.vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
        }
        if (instance_ != VK_NULL_HANDLE) {
            vk_.vkDestroyInstance(instance_, nullptr);
        }
        vk_.unload();
        throw;
    }
}

void VulkanDevice::create_instance()
{
    uint32_t api = VK_API_VERSION_1_0;
    if (vk_.vkEnumerateInstanceVersion != nullptr) {
        vk_.vkEnumerateInstanceVersion(&api);
    }
    const ApiVersion loader{VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api)};
    // The instance is asked for min(loader, desc cap, the highest version the backend has a path for). The default cap is
    // the baseline, so the request stays 1.3 unless a caller opts in.
    const std::optional<ApiVersion> requested = negotiate_instance_version(loader, desc_.max_api_version);
    if (!requested) {
        if (loader < api_baseline) {
            throw std::runtime_error("Vulkan: the loader supports only Vulkan " + to_string(loader) + "; SAIGE requires " +
                                     to_string(api_baseline));
        }
        throw std::runtime_error("Vulkan: DeviceDesc::max_api_version " + to_string(desc_.max_api_version) +
                                 " is below the supported baseline " + to_string(api_baseline));
    }
    info_.loader_api_version = loader;
    info_.instance_api_version = *requested;

    uint32_t count = 0;
    vk_.vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vk_.vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    const auto has_ext = [&](const char* name) {
        return std::any_of(available.begin(), available.end(),
                           [&](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
    };

    std::vector<const char*> extensions;
    if (has_ext(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        debug_utils_ = true;
    }
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    const char* platform_surface = VK_KHR_WIN32_SURFACE_EXTENSION_NAME;
#elif defined(VK_USE_PLATFORM_XLIB_KHR)
    const char* platform_surface = VK_KHR_XLIB_SURFACE_EXTENSION_NAME;
#else
    const char* platform_surface = nullptr;
#endif
    if (platform_surface != nullptr && has_ext(VK_KHR_SURFACE_EXTENSION_NAME) && has_ext(platform_surface)) {
        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.push_back(platform_surface);
        surface_extensions_ = true;
    } else if (desc_.require_presentation) {
        throw std::runtime_error("Vulkan: window-system surface extensions are unavailable");
    }

    std::vector<const char*> layers;
    if (desc_.enable_validation || desc_.require_validation) {
        vk_.vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> layer_props(count);
        vk_.vkEnumerateInstanceLayerProperties(&count, layer_props.data());
        const bool found = std::any_of(layer_props.begin(), layer_props.end(), [](const VkLayerProperties& l) {
            return std::strcmp(l.layerName, validation_layer) == 0;
        });
        if (found && debug_utils_) {
            layers.push_back(validation_layer);
            info_.validation_enabled = true;
        } else if (desc_.require_validation) {
            throw std::runtime_error(std::string("Vulkan: validation required but ") + validation_layer +
                                     " (or VK_EXT_debug_utils) is not installed");
        } else {
            std::cerr << "[saige.vulkan] warning: validation requested but " << validation_layer
                      << " is not installed; continuing without validation\n";
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = desc_.application_name.c_str();
    app.pEngineName = "SAIGE";
    app.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    app.apiVersion = VK_MAKE_API_VERSION(0, requested->major, requested->minor, 0);

    // Optional synchronization validation, enabled in code (environment variables are ignored by elevated Windows
    // processes). The extension is provided by the validation layer itself.
    VkValidationFeatureEnableEXT sync_feature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation_features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    validation_features.enabledValidationFeatureCount = 1;
    validation_features.pEnabledValidationFeatures = &sync_feature;
    if (info_.validation_enabled && desc_.enable_synchronization_validation) {
        uint32_t layer_ext_count = 0;
        vk_.vkEnumerateInstanceExtensionProperties(validation_layer, &layer_ext_count, nullptr);
        std::vector<VkExtensionProperties> layer_exts(layer_ext_count);
        vk_.vkEnumerateInstanceExtensionProperties(validation_layer, &layer_ext_count, layer_exts.data());
        const bool has_features_ext = std::any_of(layer_exts.begin(), layer_exts.end(), [](const VkExtensionProperties& e) {
            return std::strcmp(e.extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME) == 0;
        });
        if (has_features_ext) {
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
            info_.synchronization_validation_enabled = true;
        } else {
            std::cerr << "[saige.vulkan] warning: synchronization validation requested but "
                         "VK_EXT_validation_features is unavailable; continuing without it\n";
        }
    }

    VkDebugUtilsMessengerCreateInfoEXT messenger_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    messenger_info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    messenger_info.pfnUserCallback = &VulkanDevice::debug_callback;
    messenger_info.pUserData = this;

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    if (info_.validation_enabled) {
        // The messenger struct on the instance chain also validates instance creation/destruction. The features
        // struct goes first on that chain; the messenger's own pNext stays null because the same struct is reused for
        // vkCreateDebugUtilsMessengerEXT, which forbids a pNext chain.
        ci.pNext = &messenger_info;
        if (info_.synchronization_validation_enabled) {
            validation_features.pNext = &messenger_info;
            ci.pNext = &validation_features;
        }
    }
    check(vk_.vkCreateInstance(&ci, nullptr, &instance_), "vkCreateInstance");

    std::string error;
    if (!vk_.load_instance(instance_, &error)) {
        throw std::runtime_error("Vulkan: " + error);
    }
    if (info_.validation_enabled && vk_.vkCreateDebugUtilsMessengerEXT != nullptr) {
        check(vk_.vkCreateDebugUtilsMessengerEXT(instance_, &messenger_info, nullptr, &messenger_),
              "vkCreateDebugUtilsMessengerEXT");
    }
}

void VulkanDevice::pick_physical_device()
{
    uint32_t count = 0;
    vk_.vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vk_.vkEnumeratePhysicalDevices(instance_, &count, devices.data());

    std::string rejected;
    int best_score = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props{};
        vk_.vkGetPhysicalDeviceProperties(pd, &props);
        const std::string name = props.deviceName;
        if (props.apiVersion < VK_API_VERSION_1_3) {
            rejected += " " + name + " (Vulkan < 1.3);";
            continue;
        }
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        f12.pNext = &f13;
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        f2.pNext = &f12;
        vk_.vkGetPhysicalDeviceFeatures2(pd, &f2);
        // shaderDemoteToHelperInvocation is mandatory in Vulkan 1.3; HLSL `discard` (alpha-masked materials) compiles to it.
        if (f13.dynamicRendering == VK_FALSE || f13.synchronization2 == VK_FALSE || f12.timelineSemaphore == VK_FALSE ||
            f13.shaderDemoteToHelperInvocation == VK_FALSE) {
            rejected += " " + name + " (missing dynamicRendering/synchronization2/timelineSemaphore/shaderDemoteToHelperInvocation);";
            continue;
        }
        uint32_t qcount = 0;
        vk_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, nullptr);
        std::vector<VkQueueFamilyProperties> families(qcount);
        vk_.vkGetPhysicalDeviceQueueFamilyProperties(pd, &qcount, families.data());
        int family = -1;
        for (uint32_t i = 0; i < qcount; ++i) {
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
                family = static_cast<int>(i);
                break;
            }
        }
        if (family < 0) {
            rejected += " " + name + " (no graphics queue);";
            continue;
        }
        uint32_t ext_count = 0;
        vk_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, nullptr);
        std::vector<VkExtensionProperties> exts(ext_count);
        vk_.vkEnumerateDeviceExtensionProperties(pd, nullptr, &ext_count, exts.data());
        const bool has_swapchain = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
            return std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
        });
        if (desc_.require_presentation && !has_swapchain) {
            rejected += " " + name + " (no VK_KHR_swapchain);";
            continue;
        }
        int score = 1;
        switch (props.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: score = 1000; break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: score = 500; break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: score = 100; break;
        default: score = 1; break;
        }
        if (score > best_score) {
            best_score = score;
            physical_ = pd;
            queue_family_ = static_cast<uint32_t>(family);
            timestamp_valid_bits_ = families[static_cast<std::size_t>(family)].timestampValidBits;
            timestamp_period_ns_ = props.limits.timestampPeriod;
            memory_budget_ = std::any_of(exts.begin(), exts.end(), [](const VkExtensionProperties& e) {
                return std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME) == 0;
            });
            swapchain_extension_ = has_swapchain && surface_extensions_;
            info_.device_name = name;
            info_.api_version_major = VK_API_VERSION_MAJOR(props.apiVersion);
            info_.api_version_minor = VK_API_VERSION_MINOR(props.apiVersion);
            info_.software_device = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
            selected_api_version_ = props.apiVersion;
            selected_extensions_.clear();
            for (const VkExtensionProperties& e : exts) {
                selected_extensions_.emplace_back(e.extensionName);
            }
        }
    }
    if (physical_ == VK_NULL_HANDLE) {
        throw std::runtime_error("Vulkan: no suitable Vulkan 1.3 device." +
                                 (rejected.empty() ? std::string(" No devices found.") : " Rejected:" + rejected));
    }
    vk_.vkGetPhysicalDeviceMemoryProperties(physical_, &memory_props_);
    info_.backend = Backend::Vulkan;
    info_.presentation_supported = swapchain_extension_;
    probe_capabilities();
}

/// Reads which optional capabilities above the baseline the selected device offers, and from where (core version or promoted
/// extension). Read-only: nothing is enabled on the logical device, and a probe that finds nothing changes nothing.
void VulkanDevice::probe_capabilities()
{
    const ApiVersion device{VK_API_VERSION_MAJOR(selected_api_version_), VK_API_VERSION_MINOR(selected_api_version_)};
    const ApiVersion effective = effective_device_version(info_.instance_api_version, device);
    info_.effective_api_version = effective;

    const auto has_extension = [&](const char* name) {
        return std::find(selected_extensions_.begin(), selected_extensions_.end(), name) != selected_extensions_.end();
    };
    std::array<FeatureQuery, optional_feature_count> q{};
    const auto at = [&](OptionalFeature f) -> FeatureQuery& { return q[static_cast<std::size_t>(f)]; };

    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;

    // Promoted extensions are probed through their own feature structs and only when the device advertises them: chaining a
    // struct for an extension the device lacks is invalid.
    VkPhysicalDeviceVulkan14Features f14{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES};
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR local_read{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    VkPhysicalDeviceHostImageCopyFeaturesEXT host_copy{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES_EXT};
    VkPhysicalDeviceMaintenance5FeaturesKHR m5{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
    VkPhysicalDeviceMaintenance6FeaturesKHR m6{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
    VkPhysicalDeviceIndexTypeUint8FeaturesKHR uint8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR};
    VkPhysicalDeviceLineRasterizationFeaturesKHR lines{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LINE_RASTERIZATION_FEATURES_KHR};
    const bool core14 = effective >= ApiVersion{1, 4};
    void** tail = &f12.pNext;
    const auto chain = [&](auto& structure) {
        *tail = &structure;
        tail = &structure.pNext;
    };
    const bool ext_local_read = has_extension(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    const bool ext_host_copy = has_extension(VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);
    const bool ext_m5 = has_extension(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
    const bool ext_m6 = has_extension(VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
    const bool ext_uint8 = has_extension(VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME);
    const bool ext_lines = has_extension(VK_KHR_LINE_RASTERIZATION_EXTENSION_NAME);
    const bool ext_push = has_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if (core14) {
        chain(f14);
    } else {
        if (ext_local_read) chain(local_read);
        if (ext_host_copy) chain(host_copy);
        if (ext_m5) chain(m5);
        if (ext_m6) chain(m6);
        if (ext_uint8) chain(uint8);
        if (ext_lines) chain(lines);
    }
    vk_.vkGetPhysicalDeviceFeatures2(physical_, &f2);

    at(OptionalFeature::ScalarBlockLayout).feature_bit = f12.scalarBlockLayout == VK_TRUE;
    if (core14) {
        at(OptionalFeature::PushDescriptor).feature_bit = f14.pushDescriptor == VK_TRUE;
        at(OptionalFeature::DynamicRenderingLocalRead).feature_bit = f14.dynamicRenderingLocalRead == VK_TRUE;
        at(OptionalFeature::HostImageCopy).feature_bit = f14.hostImageCopy == VK_TRUE;
        at(OptionalFeature::Maintenance5).feature_bit = f14.maintenance5 == VK_TRUE;
        at(OptionalFeature::Maintenance6).feature_bit = f14.maintenance6 == VK_TRUE;
        at(OptionalFeature::IndexTypeUint8).feature_bit = f14.indexTypeUint8 == VK_TRUE;
        at(OptionalFeature::LineRasterization).feature_bit = f14.bresenhamLines == VK_TRUE;
    } else {
        at(OptionalFeature::PushDescriptor).feature_bit = ext_push;  // the extension has no feature bit: advertising is support
        at(OptionalFeature::DynamicRenderingLocalRead).feature_bit = ext_local_read && local_read.dynamicRenderingLocalRead == VK_TRUE;
        at(OptionalFeature::HostImageCopy).feature_bit = ext_host_copy && host_copy.hostImageCopy == VK_TRUE;
        at(OptionalFeature::Maintenance5).feature_bit = ext_m5 && m5.maintenance5 == VK_TRUE;
        at(OptionalFeature::Maintenance6).feature_bit = ext_m6 && m6.maintenance6 == VK_TRUE;
        at(OptionalFeature::IndexTypeUint8).feature_bit = ext_uint8 && uint8.indexTypeUint8 == VK_TRUE;
        at(OptionalFeature::LineRasterization).feature_bit = ext_lines && lines.bresenhamLines == VK_TRUE;
    }
    at(OptionalFeature::PushDescriptor).extension_present = ext_push;
    at(OptionalFeature::DynamicRenderingLocalRead).extension_present = ext_local_read;
    at(OptionalFeature::HostImageCopy).extension_present = ext_host_copy;
    at(OptionalFeature::Maintenance5).extension_present = ext_m5;
    at(OptionalFeature::Maintenance6).extension_present = ext_m6;
    at(OptionalFeature::IndexTypeUint8).extension_present = ext_uint8;
    at(OptionalFeature::LineRasterization).extension_present = ext_lines;
    info_.capabilities = negotiate_capabilities(effective, q);
}

void VulkanDevice::create_logical_device()
{
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queue_family_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.timelineSemaphore = VK_TRUE;
    f12.pNext = &f13;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    {
        VkPhysicalDeviceFeatures2 supported{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        vk_.vkGetPhysicalDeviceFeatures2(physical_, &supported);
        anisotropy_supported_ = supported.features.samplerAnisotropy == VK_TRUE;
        f2.features.samplerAnisotropy = anisotropy_supported_ ? VK_TRUE : VK_FALSE;
    }

    std::vector<const char*> extensions;
    if (swapchain_extension_) {
        extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    if (memory_budget_) {
        extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    }
    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.pNext = &f2;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qci;
    ci.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ci.ppEnabledExtensionNames = extensions.data();
    check(vk_.vkCreateDevice(physical_, &ci, nullptr, &device_), "vkCreateDevice");

    std::string error;
    if (!vk_.load_device(device_, swapchain_extension_, &error)) {
        throw std::runtime_error("Vulkan: " + error);
    }
    vk_.vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
    info_.driver_name = "vulkan";
}

void VulkanDevice::create_frame_resources()
{
    VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    type.initialValue = 0;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    sci.pNext = &type;
    check(vk_.vkCreateSemaphore(device_, &sci, nullptr, &timeline_), "vkCreateSemaphore(timeline)");
    set_name(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(timeline_), "FrameTimeline");

    slots_.resize(desc_.frames_in_flight);
    // GPU timestamps need a graphics queue that writes timestamps (timestampValidBits > 0) and a usable tick period.
    timestamps_ = timestamp_valid_bits_ > 0 && timestamp_period_ns_ > 0.0f;
    if (timestamps_) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qi.queryCount = static_cast<uint32_t>(slots_.size()) * max_gpu_timestamps;
        check(vk_.vkCreateQueryPool(device_, &qi, nullptr, &timestamp_pool_), "vkCreateQueryPool(timestamps)");
        info_.gpu_timestamps = true;
        info_.timestamp_period_ns = static_cast<double>(timestamp_period_ns_);
    }
    for (FrameSlot& slot : slots_) {
        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pci.queueFamilyIndex = queue_family_;
        check(vk_.vkCreateCommandPool(device_, &pci, nullptr, &slot.pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = slot.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        check(vk_.vkAllocateCommandBuffers(device_, &ai, &slot.cmd), "vkAllocateCommandBuffers");
    }
    VkCommandPoolCreateInfo upci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    upci.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    upci.queueFamilyIndex = queue_family_;
    check(vk_.vkCreateCommandPool(device_, &upci, nullptr, &upload_pool_), "vkCreateCommandPool(upload)");
}

VulkanDevice::~VulkanDevice()
{
    if (device_ == VK_NULL_HANDLE) {
        return;
    }
    vk_.vkDeviceWaitIdle(device_);
    pending_.drain_all([](std::function<void()>&& fn) { fn(); });

    std::vector<SwapchainHandle> sc_handles;
    swapchains_.for_each([&](SwapchainHandle h, VkSwapchainRes&) { sc_handles.push_back(h); });
    for (SwapchainHandle h : sc_handles) {
        destroy(h);
    }
    pending_.drain_all([](std::function<void()>&& fn) { fn(); });

    const uint32_t leaked = buffers_.size() + textures_.size() + samplers_.size() + bind_group_layouts_.size() +
                            bind_groups_.size() + shaders_.size() + pipelines_.size();
    if (leaked != 0) {
        std::cerr << "[saige.vulkan] warning: " << leaked << " resource(s) still alive at device destruction; releasing\n";
    }
    buffers_.for_each([&](BufferHandle, VkBufferRes& b) {
        vk_.vkDestroyBuffer(device_, b.buffer, nullptr);
        free_memory(b.memory);
    });
    textures_.for_each([&](TextureHandle, VkTextureRes& t) {
        vk_.vkDestroyImageView(device_, t.view, nullptr);
        if (!t.swapchain_image) {
            vk_.vkDestroyImage(device_, t.image, nullptr);
            free_memory(t.memory);
        }
    });
    samplers_.for_each([&](SamplerHandle, VkSamplerRes& s) { vk_.vkDestroySampler(device_, s.sampler, nullptr); });
    // Destroying a pool frees every set allocated from it, so leaked groups need no individual release.
    for (const VkDescriptorPool pool : descriptor_pools_) {
        vk_.vkDestroyDescriptorPool(device_, pool, nullptr);
    }
    bind_group_layouts_.for_each(
        [&](BindGroupLayoutHandle, VkBindGroupLayoutRes& l) { vk_.vkDestroyDescriptorSetLayout(device_, l.layout, nullptr); });
    shaders_.for_each([&](ShaderHandle, VkShaderRes& s) { vk_.vkDestroyShaderModule(device_, s.module, nullptr); });
    pipelines_.for_each([&](PipelineHandle, VkPipelineRes& p) {
        vk_.vkDestroyPipeline(device_, p.pipeline, nullptr);
        vk_.vkDestroyPipelineLayout(device_, p.layout, nullptr);
    });

    for (FrameSlot& s : slots_) {
        for (VkSemaphore sem : s.acquire_semaphores) {
            vk_.vkDestroySemaphore(device_, sem, nullptr);
        }
        vk_.vkDestroyCommandPool(device_, s.pool, nullptr);
    }
    vk_.vkDestroyCommandPool(device_, upload_pool_, nullptr);
    if (timestamp_pool_ != VK_NULL_HANDLE) {
        vk_.vkDestroyQueryPool(device_, timestamp_pool_, nullptr);
    }
    vk_.vkDestroySemaphore(device_, timeline_, nullptr);
    vk_.vkDestroyDevice(device_, nullptr);
    if (messenger_ != VK_NULL_HANDLE) {
        vk_.vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    }
    vk_.vkDestroyInstance(instance_, nullptr);
    vk_.unload();
}

// =====================================================================================================================
// Helpers
// =====================================================================================================================

uint32_t VulkanDevice::find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags required,
                                        VkMemoryPropertyFlags preferred) const
{
    uint32_t fallback = std::numeric_limits<uint32_t>::max();
    for (uint32_t i = 0; i < memory_props_.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) == 0) {
            continue;
        }
        const VkMemoryPropertyFlags flags = memory_props_.memoryTypes[i].propertyFlags;
        if ((flags & required) != required) {
            continue;
        }
        if ((flags & preferred) == preferred) {
            return i;
        }
        if (fallback == std::numeric_limits<uint32_t>::max()) {
            fallback = i;
        }
    }
    if (fallback == std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Vulkan: no compatible memory type");
    }
    return fallback;
}

VkDeviceMemory VulkanDevice::allocate(const VkMemoryRequirements& req, MemoryLocation location)
{
    VkMemoryPropertyFlags required = 0;
    VkMemoryPropertyFlags preferred = 0;
    switch (location) {
    case MemoryLocation::GpuOnly: preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT; break;
    case MemoryLocation::CpuToGpu:
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        preferred = required | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case MemoryLocation::GpuToCpu:
        required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        preferred = required | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    }
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory_type(req.memoryTypeBits, required, preferred);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    check(vk_.vkAllocateMemory(device_, &ai, nullptr, &memory), "vkAllocateMemory");
    // The driver now owns memory nobody else knows about: if recording it throws (the ledger allocates a map node), free
    // it before propagating, and only count the bytes once the registration has succeeded.
    try {
        if (consume_memory_bookkeeping_fault()) {
            throw std::bad_alloc();
        }
        allocation_sizes_.emplace(memory, req.size);
    } catch (...) {
        vk_.vkFreeMemory(device_, memory, nullptr);
        throw;
    }
    allocated_bytes_ += req.size;
    return memory;
}

void VulkanDevice::free_memory(VkDeviceMemory memory)
{
    if (memory == VK_NULL_HANDLE) {
        return;
    }
    if (const auto it = allocation_sizes_.find(memory); it != allocation_sizes_.end()) {
        allocated_bytes_ -= it->second;
        allocation_sizes_.erase(it);
    }
    vk_.vkFreeMemory(device_, memory, nullptr);
}

void VulkanDevice::set_name(VkObjectType type, uint64_t handle, const char* name)
{
    if (name == nullptr || vk_.vkSetDebugUtilsObjectNameEXT == nullptr || !debug_utils_) {
        return;
    }
    VkDebugUtilsObjectNameInfoEXT ni{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    ni.objectType = type;
    ni.objectHandle = handle;
    ni.pObjectName = name;
    vk_.vkSetDebugUtilsObjectNameEXT(device_, &ni);
}

void VulkanDevice::immediate_submit(const std::function<void(VkCommandBuffer)>& record)
{
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = upload_pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vk_.vkAllocateCommandBuffers(device_, &ai, &cmd), "vkAllocateCommandBuffers(upload)");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vk_.vkBeginCommandBuffer(cmd, &bi), "vkBeginCommandBuffer(upload)");
    record(cmd);
    check(vk_.vkEndCommandBuffer(cmd), "vkEndCommandBuffer(upload)");

    const uint64_t value = ++timeline_counter_;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = timeline_;
    signal.value = value;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &signal;
    check(vk_.vkQueueSubmit2(queue_, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2(upload)");

    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &timeline_;
    wi.pValues = &value;
    check(vk_.vkWaitSemaphores(device_, &wi, std::numeric_limits<uint64_t>::max()), "vkWaitSemaphores(upload)");
    check(vk_.vkResetCommandPool(device_, upload_pool_, 0), "vkResetCommandPool(upload)");
}

uint64_t VulkanDevice::completed_value() const
{
    uint64_t value = 0;
    check(vk_.vkGetSemaphoreCounterValue(device_, timeline_, &value), "vkGetSemaphoreCounterValue");
    return value;
}

void VulkanDevice::collect_garbage()
{
    pending_.collect(completed_value(), [](std::function<void()>&& fn) { fn(); });
}

// =====================================================================================================================
// Resources
// =====================================================================================================================

BufferHandle VulkanDevice::create_buffer(const BufferDesc& desc, std::span<const std::byte> initial_data)
{
    if (const ValidationResult v = validate(desc); !v) {
        throw std::invalid_argument(std::string("create_buffer: ") + v.error);
    }
    if (initial_data.size() > desc.size) {
        throw std::invalid_argument("create_buffer: initial data larger than the buffer");
    }
    VkBufferUsageFlags usage = 0;
    const auto map = [&](BufferUsage bit, VkBufferUsageFlags vk_bit) {
        if (has_any(desc.usage, bit)) {
            usage |= vk_bit;
        }
    };
    map(BufferUsage::Vertex, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    map(BufferUsage::Index, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    map(BufferUsage::Uniform, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    map(BufferUsage::Storage, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    map(BufferUsage::Indirect, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    map(BufferUsage::TransferSrc, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    map(BufferUsage::TransferDst, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    const bool staged = !initial_data.empty() && desc.memory == MemoryLocation::GpuOnly;
    if (staged) {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }

    VkBufferRes res;
    res.desc = desc;
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = desc.size;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    check(vk_.vkCreateBuffer(device_, &ci, nullptr, &res.buffer), "vkCreateBuffer");
    VkMemoryRequirements req{};
    vk_.vkGetBufferMemoryRequirements(device_, res.buffer, &req);
    try {
        res.memory = allocate(req, desc.memory);
        check(vk_.vkBindBufferMemory(device_, res.buffer, res.memory, 0), "vkBindBufferMemory");
        if (desc.memory != MemoryLocation::GpuOnly) {
            void* ptr = nullptr;
            check(vk_.vkMapMemory(device_, res.memory, 0, VK_WHOLE_SIZE, 0, &ptr), "vkMapMemory");
            res.mapped = static_cast<std::byte*>(ptr);
            if (!initial_data.empty()) {
                std::memcpy(res.mapped, initial_data.data(), initial_data.size());
            }
        }
    } catch (...) {
        vk_.vkDestroyBuffer(device_, res.buffer, nullptr);
        if (res.memory != VK_NULL_HANDLE) {
            free_memory(res.memory);
        }
        throw;
    }
    set_name(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(res.buffer), desc.debug_name);

    if (staged) {
        std::optional<BufferHandle> staging;
        const auto release_staging = [&] {
            if (staging) {
                if (std::optional<VkBufferRes> s = buffers_.release(*staging)) {
                    vk_.vkDestroyBuffer(device_, s->buffer, nullptr);
                    free_memory(s->memory);
                }
                staging.reset();
            }
        };
        try {
            BufferDesc staging_desc;
            staging_desc.size = initial_data.size();
            staging_desc.usage = BufferUsage::TransferSrc;
            staging_desc.memory = MemoryLocation::CpuToGpu;
            staging_desc.debug_name = "UploadStaging";
            staging = create_buffer(staging_desc, initial_data);
            const VkBuffer src = buffers_.get(*staging)->buffer;
            const VkBuffer dst = res.buffer;
            const VkDeviceSize size = initial_data.size();
            immediate_submit([&](VkCommandBuffer cmd) {
                VkBufferCopy region{0, 0, size};
                vk_.vkCmdCopyBuffer(cmd, src, dst, 1, &region);
                // Make the copy's writes available to everything that follows (vertex/index fetch, uniform and storage reads,
                // later copies). Queue order alone does not make memory visible; synchronization validation reports this
                // as READ_AFTER_WRITE without the barrier.
                VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
                bb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
                bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                bb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                bb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                bb.buffer = dst;
                bb.offset = 0;
                bb.size = VK_WHOLE_SIZE;
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                di.bufferMemoryBarrierCount = 1;
                di.pBufferMemoryBarriers = &bb;
                vk_.vkCmdPipelineBarrier2(cmd, &di);
            });
            // The copy has completed (immediate_submit waits); later frames are ordered after it on the queue.
        } catch (...) {
            // The destination has no handle yet, so nothing else owns it: release both buffers before propagating.
            release_staging();
            vk_.vkDestroyBuffer(device_, res.buffer, nullptr);
            free_memory(res.memory);
            throw;
        }
        release_staging();
    }
    return buffers_.emplace(std::move(res));
}

TextureHandle VulkanDevice::create_texture(const TextureDesc& desc)
{
    if (const ValidationResult v = validate(desc); !v) {
        throw std::invalid_argument(std::string("create_texture: ") + v.error);
    }
    if (desc.dimension != TextureDimension::Texture2D) {
        throw std::invalid_argument("create_texture: the 0.1 Vulkan backend supports Texture2D only");
    }
    VkImageUsageFlags usage = 0;
    const auto map = [&](TextureUsage bit, VkImageUsageFlags vk_bit) {
        if (has_any(desc.usage, bit)) {
            usage |= vk_bit;
        }
    };
    map(TextureUsage::Sampled, VK_IMAGE_USAGE_SAMPLED_BIT);
    map(TextureUsage::Storage, VK_IMAGE_USAGE_STORAGE_BIT);
    map(TextureUsage::ColorAttachment, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    map(TextureUsage::DepthStencilAttachment, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    map(TextureUsage::TransferSrc, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    map(TextureUsage::TransferDst, VK_IMAGE_USAGE_TRANSFER_DST_BIT);

    VkTextureRes res;
    res.desc = desc;
    res.desc.mip_levels = resolve_mip_levels(desc);
    res.aspect = aspect_of(desc.format);
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = to_vk(desc.format);
    ci.extent = {desc.width, desc.height, 1};
    ci.mipLevels = res.desc.mip_levels;
    ci.arrayLayers = desc.array_layers;
    ci.samples = static_cast<VkSampleCountFlagBits>(desc.sample_count);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vk_.vkCreateImage(device_, &ci, nullptr, &res.image), "vkCreateImage");
    VkMemoryRequirements req{};
    vk_.vkGetImageMemoryRequirements(device_, res.image, &req);
    try {
        res.memory = allocate(req, MemoryLocation::GpuOnly);
        check(vk_.vkBindImageMemory(device_, res.image, res.memory, 0), "vkBindImageMemory");
        // Views are only valid for images usable by shaders or as attachments; a transfer-only texture has none.
        constexpr VkImageUsageFlags view_usages = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if ((usage & view_usages) != 0) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = res.image;
            vi.viewType = desc.array_layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            vi.format = ci.format;
            vi.subresourceRange = {res.aspect, 0, res.desc.mip_levels, 0, desc.array_layers};
            check(vk_.vkCreateImageView(device_, &vi, nullptr, &res.view), "vkCreateImageView");
        }
    } catch (...) {
        vk_.vkDestroyImage(device_, res.image, nullptr);
        if (res.memory != VK_NULL_HANDLE) {
            free_memory(res.memory);
        }
        throw;
    }
    set_name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(res.image), desc.debug_name);
    return textures_.emplace(std::move(res));
}

void VulkanDevice::upload_texture(TextureHandle texture, std::span<const std::byte> data)
{
    const VkTextureRes* t = textures_.get(texture);
    if (t == nullptr || t->swapchain_image) {
        throw std::invalid_argument("upload_texture: invalid texture handle");
    }
    if (in_frame_) {
        throw std::logic_error("upload_texture: not allowed between begin_frame() and end_frame()");
    }
    const TextureDesc& d = t->desc;
    if (const ValidationResult v = validate_texture_upload(d, data.size()); !v) {
        throw std::invalid_argument(v.error);
    }

    // Copy regions: mip level outermost, then array layer, rows tightly packed (matches compute_texture_data_size).
    std::vector<VkBufferImageCopy> regions;
    const Extent3D base{d.width, d.height, 1};
    uint64_t offset = 0;
    for (uint32_t level = 0; level < d.mip_levels; ++level) {
        const Extent3D e = mip_extent(base, level);
        const SurfaceLayout layout = compute_surface_layout(d.format, e.width, e.height);
        for (uint32_t layer = 0; layer < d.array_layers; ++layer) {
            VkBufferImageCopy r{};
            r.bufferOffset = offset;
            r.imageSubresource = {t->aspect, level, layer, 1};
            r.imageExtent = {e.width, e.height, 1};
            regions.push_back(r);
            offset += layout.slice_pitch;
        }
    }

    BufferDesc staging_desc;
    staging_desc.size = data.size();
    staging_desc.usage = BufferUsage::TransferSrc;
    staging_desc.memory = MemoryLocation::CpuToGpu;
    staging_desc.debug_name = "TextureUploadStaging";
    const BufferHandle staging = create_buffer(staging_desc, data);
    const VkBuffer src = buffers_.get(staging)->buffer;
    const VkImage image = t->image;
    const VkImageAspectFlags aspect = t->aspect;
    const uint32_t mips = d.mip_levels;
    const uint32_t layers = d.array_layers;
    try {
        immediate_submit([&](VkCommandBuffer cmd) {
            auto transition = [&](VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst_stage,
                                  VkAccessFlags2 dst_access, VkImageLayout from, VkImageLayout to) {
                VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                b.srcStageMask = src_stage;
                b.srcAccessMask = src_access;
                b.dstStageMask = dst_stage;
                b.dstAccessMask = dst_access;
                b.oldLayout = from;
                b.newLayout = to;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = image;
                b.subresourceRange = {aspect, 0, mips, 0, layers};
                VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                di.imageMemoryBarrierCount = 1;
                di.pImageMemoryBarriers = &b;
                vk_.vkCmdPipelineBarrier2(cmd, &di);
            };
            // A re-upload overwrites a texture that earlier frames may still be reading or writing. Those frames are
            // already submitted and may still be executing (the host only waits for *this* submission), so the first
            // transition is a full dependency on all prior work. Write-after-read needs only the execution dependency
            // (ALL_COMMANDS); write-after-write also needs the earlier writes (a previous upload's copy, or rendering
            // into a texture that also has attachment usage) made available, hence MEMORY_WRITE as the source access.
            // A fresh texture has no prior work, so this costs it nothing.
            transition(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            vk_.vkCmdCopyBufferToImage(cmd, src, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       static_cast<uint32_t>(regions.size()), regions.data());
            transition(VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                       VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                       VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });
    } catch (...) {
        destroy(staging);
        throw;
    }
    // immediate_submit waited for completion, so the staging buffer is no longer in use.
    if (std::optional<VkBufferRes> s = buffers_.release(staging)) {
        vk_.vkDestroyBuffer(device_, s->buffer, nullptr);
        free_memory(s->memory);
    }
}

SamplerHandle VulkanDevice::create_sampler(const SamplerDesc& desc)
{
    if (const ValidationResult v = validate(desc); !v) {
        throw std::invalid_argument(std::string("create_sampler: ") + v.error);
    }
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = to_vk(desc.mag_filter);
    ci.minFilter = to_vk(desc.min_filter);
    ci.mipmapMode = to_vk(desc.mipmap_mode);
    ci.addressModeU = to_vk(desc.address_u);
    ci.addressModeV = to_vk(desc.address_v);
    ci.addressModeW = to_vk(desc.address_w);
    ci.mipLodBias = desc.mip_lod_bias;
    // Anisotropy degrades to trilinear on devices without the feature instead of failing the load.
    ci.anisotropyEnable = (desc.max_anisotropy > 1.0f && anisotropy_supported_) ? VK_TRUE : VK_FALSE;
    ci.maxAnisotropy = ci.anisotropyEnable == VK_TRUE ? desc.max_anisotropy : 1.0f;
    ci.compareEnable = desc.compare_enable ? VK_TRUE : VK_FALSE;
    ci.compareOp = to_vk(desc.compare_op);
    ci.minLod = desc.min_lod;
    ci.maxLod = desc.max_lod;
    ci.borderColor = to_vk(desc.border_color);
    VkSamplerRes res;
    res.desc = desc;
    check(vk_.vkCreateSampler(device_, &ci, nullptr, &res.sampler), "vkCreateSampler");
    return samplers_.emplace(std::move(res));
}

VkDescriptorSet VulkanDevice::allocate_descriptor_set(VkDescriptorSetLayout layout, VkDescriptorPool& pool_out)
{
    auto try_allocate = [&](VkDescriptorPool pool, VkDescriptorSet& set) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        return vk_.vkAllocateDescriptorSets(device_, &ai, &set);
    };
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (!descriptor_pools_.empty()) {
        const VkResult r = try_allocate(descriptor_pools_.back(), set);
        if (r == VK_SUCCESS) {
            pool_out = descriptor_pools_.back();
            return set;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            check(r, "vkAllocateDescriptorSets");
        }
    }
    // The current pool is full (or none exists yet): add one. Sizes comfortably fit any single group.
    const VkDescriptorPoolSize sizes[] = {
        {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1024},
        {VK_DESCRIPTOR_TYPE_SAMPLER, 512},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 512},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 512},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 256;
    pi.poolSizeCount = 4;
    pi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    check(vk_.vkCreateDescriptorPool(device_, &pi, nullptr, &pool), "vkCreateDescriptorPool");
    descriptor_pools_.push_back(pool);
    check(try_allocate(pool, set), "vkAllocateDescriptorSets (new pool)");
    pool_out = pool;
    return set;
}

bool VulkanDevice::group_resources_alive(const VkBindGroupRes& g) const
{
    return std::all_of(g.textures.begin(), g.textures.end(), [&](TextureHandle h) { return textures_.contains(h); }) &&
           std::all_of(g.samplers.begin(), g.samplers.end(), [&](SamplerHandle h) { return samplers_.contains(h); }) &&
           std::all_of(g.buffers.begin(), g.buffers.end(), [&](BufferHandle h) { return buffers_.contains(h); });
}

BindGroupLayoutHandle VulkanDevice::create_bind_group_layout(const BindGroupLayoutDesc& desc)
{
    if (const ValidationResult v = validate(desc); !v) {
        throw std::invalid_argument(std::string("create_bind_group_layout: ") + v.error);
    }
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (const BindGroupLayoutEntry& e : desc.entries) {
        VkDescriptorSetLayoutBinding b{};
        b.binding = e.binding;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        switch (e.type) {
        case BindingType::SampledTexture: b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; break;
        case BindingType::Sampler: b.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; break;
        case BindingType::UniformBuffer: b.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; break;
        case BindingType::StorageBuffer: b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; break;
        }
        bindings.push_back(b);
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = static_cast<uint32_t>(bindings.size());
    ci.pBindings = bindings.data();
    VkBindGroupLayoutRes res;
    res.desc = desc;
    res.desc.debug_name = nullptr;
    check(vk_.vkCreateDescriptorSetLayout(device_, &ci, nullptr, &res.layout), "vkCreateDescriptorSetLayout");
    set_name(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<uint64_t>(res.layout), desc.debug_name);
    return bind_group_layouts_.emplace(std::move(res));
}

BindGroupHandle VulkanDevice::create_bind_group(BindGroupLayoutHandle layout, std::span<const BindGroupEntry> entries)
{
    const VkBindGroupLayoutRes* l = bind_group_layouts_.get(layout);
    if (l == nullptr) {
        throw std::invalid_argument("create_bind_group: invalid layout");
    }
    BindGroupResolver resolver;
    resolver.texture = [this](TextureHandle h) -> std::optional<TextureDesc> {
        const VkTextureRes* t = textures_.get(h);
        return t != nullptr ? std::optional<TextureDesc>(t->desc) : std::nullopt;
    };
    resolver.sampler_exists = [this](SamplerHandle h) { return samplers_.contains(h); };
    resolver.buffer = [this](BufferHandle h) -> std::optional<BufferDesc> {
        const VkBufferRes* b = buffers_.get(h);
        return b != nullptr ? std::optional<BufferDesc>(b->desc) : std::nullopt;
    };
    if (const ValidationResult v = validate(l->desc, entries, resolver); !v) {
        throw std::invalid_argument(std::string("create_bind_group: ") + v.error);
    }

    VkBindGroupRes res;
    res.layout_handle = layout;
    res.set = allocate_descriptor_set(l->layout, res.pool);

    std::vector<VkDescriptorImageInfo> images(entries.size());
    std::vector<VkDescriptorBufferInfo> buffers(entries.size());
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(entries.size());
    for (const BindGroupLayoutEntry& expected : l->desc.entries) {
        for (std::size_t i = 0; i < entries.size(); ++i) {
            const BindGroupEntry& e = entries[i];
            if (e.binding != expected.binding) {
                continue;
            }
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = res.set;
            w.dstBinding = e.binding;
            w.descriptorCount = 1;
            if (expected.type == BindingType::SampledTexture) {
                res.textures.push_back(e.texture);
                images[i].imageView = textures_.get(e.texture)->view;
                images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                w.pImageInfo = &images[i];
            } else if (expected.type == BindingType::Sampler) {
                res.samplers.push_back(e.sampler);
                images[i].sampler = samplers_.get(e.sampler)->sampler;
                w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                w.pImageInfo = &images[i];
            } else {
                res.buffers.push_back(e.buffer);
                buffers[i].buffer = buffers_.get(e.buffer)->buffer;
                buffers[i].offset = e.offset;
                buffers[i].range = e.size == ~uint64_t{0} ? VK_WHOLE_SIZE : e.size;
                w.descriptorType = expected.type == BindingType::UniformBuffer ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                              : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w.pBufferInfo = &buffers[i];
            }
            writes.push_back(w);
        }
    }
    vk_.vkUpdateDescriptorSets(device_, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    return bind_groups_.emplace(std::move(res));
}

void VulkanDevice::destroy(BindGroupLayoutHandle handle)
{
    std::optional<VkBindGroupLayoutRes> res = bind_group_layouts_.release(handle);
    if (!res) {
        std::cerr << "[saige.vulkan] warning: destroy() on an invalid bind group layout handle\n";
        return;
    }
    defer([this, l = res->layout] { vk_.vkDestroyDescriptorSetLayout(device_, l, nullptr); });
}

void VulkanDevice::destroy(BindGroupHandle handle)
{
    std::optional<VkBindGroupRes> res = bind_groups_.release(handle);
    if (!res) {
        std::cerr << "[saige.vulkan] warning: destroy() on an invalid bind group handle\n";
        return;
    }
    defer([this, pool = res->pool, set = res->set] { vk_.vkFreeDescriptorSets(device_, pool, 1, &set); });
}

void VulkanDevice::destroy(SamplerHandle handle)
{
    std::optional<VkSamplerRes> res = samplers_.release(handle);
    if (!res) {
        std::cerr << "[saige.vulkan] warning: destroy() on an invalid sampler handle\n";
        return;
    }
    defer([this, s = res->sampler] { vk_.vkDestroySampler(device_, s, nullptr); });
}

ShaderHandle VulkanDevice::create_shader(const ShaderDesc& desc)
{
    if (desc.spirv.size() < 5 || desc.spirv[0] != 0x07230203u) {
        throw std::invalid_argument("create_shader: not a SPIR-V module");
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = desc.spirv.size() * sizeof(uint32_t);
    ci.pCode = desc.spirv.data();
    VkShaderRes res;
    res.stage = desc.stage;
    res.entry = desc.entry_point;
    check(vk_.vkCreateShaderModule(device_, &ci, nullptr, &res.module), "vkCreateShaderModule");
    set_name(VK_OBJECT_TYPE_SHADER_MODULE, reinterpret_cast<uint64_t>(res.module), desc.debug_name);
    return shaders_.emplace(std::move(res));
}

PipelineHandle VulkanDevice::create_graphics_pipeline(const GraphicsPipelineDesc& desc)
{
    const VkShaderRes* vs = shaders_.get(desc.vertex_shader);
    const VkShaderRes* ps = shaders_.get(desc.pixel_shader);
    if (vs == nullptr || vs->stage != ShaderStage::Vertex || ps == nullptr || ps->stage != ShaderStage::Pixel) {
        throw std::invalid_argument("create_graphics_pipeline: invalid vertex or pixel shader");
    }
    if (desc.push_constant_size > max_push_constants || desc.push_constant_size % 4 != 0) {
        throw std::invalid_argument("create_graphics_pipeline: push constant size must be a multiple of 4 and <= 128");
    }
    if ((desc.depth.test || desc.depth.write) && !has_depth(desc.depth_format)) {
        throw std::invalid_argument("create_graphics_pipeline: depth state without a depth format");
    }

    VkPipelineRes res;
    res.push_constant_size = desc.push_constant_size;
    res.bind_group_layouts = desc.bind_group_layouts;
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, desc.push_constant_size};
    if (desc.bind_group_layouts.size() > max_bind_groups_per_pipeline) {
        throw std::invalid_argument("create_graphics_pipeline: too many bind group layouts");
    }
    std::vector<VkDescriptorSetLayout> set_layouts;
    std::vector<const BindGroupLayoutDesc*> layout_descs;
    for (const BindGroupLayoutHandle h : desc.bind_group_layouts) {
        const VkBindGroupLayoutRes* l = bind_group_layouts_.get(h);
        if (l == nullptr) {
            throw std::invalid_argument("create_graphics_pipeline: invalid bind group layout");
        }
        set_layouts.push_back(l->layout);
        layout_descs.push_back(&l->desc);
    }
    // Every binding is visible to both stages, so per-stage device limits are checked against the pipeline's totals.
    if (const ValidationResult v = validate_pipeline_bindings(layout_descs); !v) {
        throw std::invalid_argument(std::string("create_graphics_pipeline: ") + v.error);
    }
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = static_cast<uint32_t>(set_layouts.size());
    lci.pSetLayouts = set_layouts.data();
    lci.pushConstantRangeCount = desc.push_constant_size > 0 ? 1u : 0u;
    lci.pPushConstantRanges = &range;
    check(vk_.vkCreatePipelineLayout(device_, &lci, nullptr, &res.layout), "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs->module;
    stages[0].pName = vs->entry.c_str();
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps->module;
    stages[1].pName = ps->entry.c_str();

    VkVertexInputBindingDescription binding{0, desc.vertex_layout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (const VertexAttribute& a : desc.vertex_layout.attributes) {
        attributes.push_back({a.location, 0, to_vk(a.format), a.offset});
    }
    VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (!attributes.empty()) {
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
        vertex_input.pVertexAttributeDescriptions = attributes.data();
    }

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    switch (desc.topology) {
    case PrimitiveTopology::TriangleList: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case PrimitiveTopology::TriangleStrip: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case PrimitiveTopology::LineList: ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    }
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.lineWidth = 1.0f;
    switch (desc.cull_mode) {
    case CullMode::None: raster.cullMode = VK_CULL_MODE_NONE; break;
    case CullMode::Front: raster.cullMode = VK_CULL_MODE_FRONT_BIT; break;
    case CullMode::Back: raster.cullMode = VK_CULL_MODE_BACK_BIT; break;
    }
    raster.frontFace = desc.front_face == FrontFace::CounterClockwise ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                                      : VK_FRONT_FACE_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = desc.depth.test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = desc.depth.write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = to_vk(desc.depth.compare);
    std::vector<VkPipelineColorBlendAttachmentState> blend(desc.color_formats.size());
    for (auto& b : blend) {
        b.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = static_cast<uint32_t>(blend.size());
    cb.pAttachments = blend.data();
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynamic_states;

    std::vector<VkFormat> color_formats;
    for (Format f : desc.color_formats) {
        color_formats.push_back(to_vk(f));
    }
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = static_cast<uint32_t>(color_formats.size());
    rendering.pColorAttachmentFormats = color_formats.data();
    rendering.depthAttachmentFormat = has_depth(desc.depth_format) ? to_vk(desc.depth_format) : VK_FORMAT_UNDEFINED;
    rendering.stencilAttachmentFormat =
        has_stencil(desc.depth_format) ? to_vk(desc.depth_format) : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertex_input;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dyn;
    ci.layout = res.layout;
    const VkResult r = vk_.vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &res.pipeline);
    if (r != VK_SUCCESS) {
        vk_.vkDestroyPipelineLayout(device_, res.layout, nullptr);
        fail("vkCreateGraphicsPipelines", r);
    }
    set_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(res.pipeline), desc.debug_name);
    return pipelines_.emplace(std::move(res));
}

void VulkanDevice::destroy(BufferHandle handle)
{
    std::optional<VkBufferRes> res = buffers_.release(handle);
    if (!res) {
        std::cerr << "[saige.vulkan] warning: destroy() on an invalid buffer handle\n";
        return;
    }
    defer([this, b = res->buffer, m = res->memory] {
        vk_.vkDestroyBuffer(device_, b, nullptr);
        free_memory(m);
    });
}

void VulkanDevice::destroy(TextureHandle handle)
{
    const VkTextureRes* t = textures_.get(handle);
    if (t == nullptr || t->swapchain_image) {
        std::cerr << "[saige.vulkan] warning: destroy() on an invalid texture handle (or a swapchain image)\n";
        return;
    }
    std::optional<VkTextureRes> res = textures_.release(handle);
    defer([this, img = res->image, view = res->view, mem = res->memory] {
        vk_.vkDestroyImageView(device_, view, nullptr);
        vk_.vkDestroyImage(device_, img, nullptr);
        free_memory(mem);
    });
}

void VulkanDevice::destroy(ShaderHandle handle)
{
    std::optional<VkShaderRes> res = shaders_.release(handle);
    if (!res) {
        return;
    }
    // Shader modules are not referenced by in-flight work once pipelines exist; deferring keeps it uniform.
    defer([this, m = res->module] { vk_.vkDestroyShaderModule(device_, m, nullptr); });
}

void VulkanDevice::destroy(PipelineHandle handle)
{
    std::optional<VkPipelineRes> res = pipelines_.release(handle);
    if (!res) {
        return;
    }
    defer([this, p = res->pipeline, l = res->layout] {
        vk_.vkDestroyPipeline(device_, p, nullptr);
        vk_.vkDestroyPipelineLayout(device_, l, nullptr);
    });
}

std::span<std::byte> VulkanDevice::map(BufferHandle handle)
{
    VkBufferRes* b = buffers_.get(handle);
    if (b == nullptr || b->mapped == nullptr) {
        return {};
    }
    return {b->mapped, static_cast<std::size_t>(b->desc.size)};
}

std::optional<TextureDesc> VulkanDevice::texture_desc(TextureHandle handle) const
{
    const VkTextureRes* t = textures_.get(handle);
    return t != nullptr ? std::optional<TextureDesc>(t->desc) : std::nullopt;
}

// =====================================================================================================================
// Swapchains
// =====================================================================================================================

SwapchainHandle VulkanDevice::create_swapchain(const SwapchainDesc& desc)
{
    if (!swapchain_extension_) {
        throw std::runtime_error("Vulkan: presentation is not supported by this device/instance");
    }
    VkSwapchainRes sc;
    sc.desc = desc;
    switch (desc.surface.system) {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    case NativeSurface::System::Win32: {
        VkWin32SurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        ci.hinstance = static_cast<HINSTANCE>(desc.surface.display);
        ci.hwnd = reinterpret_cast<HWND>(desc.surface.window);
        check(vk_.vkCreateWin32SurfaceKHR(instance_, &ci, nullptr, &sc.surface), "vkCreateWin32SurfaceKHR");
        break;
    }
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
    case NativeSurface::System::Xlib: {
        VkXlibSurfaceCreateInfoKHR ci{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
        ci.dpy = static_cast<Display*>(desc.surface.display);
        ci.window = static_cast<Window>(desc.surface.window);
        check(vk_.vkCreateXlibSurfaceKHR(instance_, &ci, nullptr, &sc.surface), "vkCreateXlibSurfaceKHR");
        break;
    }
#endif
    default:
        throw std::invalid_argument("create_swapchain: unsupported window system for this build");
    }
    VkBool32 supported = VK_FALSE;
    vk_.vkGetPhysicalDeviceSurfaceSupportKHR(physical_, queue_family_, sc.surface, &supported);
    if (supported == VK_FALSE) {
        vk_.vkDestroySurfaceKHR(instance_, sc.surface, nullptr);
        throw std::runtime_error("Vulkan: the graphics queue cannot present to this surface");
    }

    uint32_t count = 0;
    vk_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, sc.surface, &count, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(count);
    vk_.vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, sc.surface, &count, formats.data());
    const VkFormat preferred = to_vk(desc.preferred_format);
    const VkSurfaceFormatKHR* chosen = nullptr;
    for (const auto& f : formats) {
        if (f.format == preferred && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = &f;
        }
    }
    for (const auto& f : formats) {
        if (chosen == nullptr && is_srgb(from_vk_surface_format(f.format)) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            chosen = &f;
        }
    }
    for (const auto& f : formats) {
        if (chosen == nullptr && from_vk_surface_format(f.format) != Format::Unknown) {
            chosen = &f;
        }
    }
    if (chosen == nullptr) {
        vk_.vkDestroySurfaceKHR(instance_, sc.surface, nullptr);
        throw std::runtime_error("Vulkan: no supported 8-bit surface format");
    }
    sc.vk_format = chosen->format;
    sc.color_space = chosen->colorSpace;
    sc.format = from_vk_surface_format(chosen->format);
    try {
        build_swapchain(sc);
    } catch (...) {
        release_swapchain_images(sc);
        if (sc.swapchain != VK_NULL_HANDLE) {
            vk_.vkDestroySwapchainKHR(device_, sc.swapchain, nullptr);
        }
        vk_.vkDestroySurfaceKHR(instance_, sc.surface, nullptr);
        throw;
    }
    return swapchains_.emplace(std::move(sc));
}

void VulkanDevice::release_swapchain_images(VkSwapchainRes& sc)
{
    for (TextureHandle h : sc.images) {
        if (std::optional<VkTextureRes> t = textures_.release(h)) {
            vk_.vkDestroyImageView(device_, t->view, nullptr);
        }
    }
    sc.images.clear();
    for (VkSemaphore s : sc.render_finished) {
        vk_.vkDestroySemaphore(device_, s, nullptr);
    }
    sc.render_finished.clear();
}

void VulkanDevice::build_swapchain(VkSwapchainRes& sc)
{
    VkSurfaceCapabilitiesKHR caps{};
    check(vk_.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, sc.surface, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(sc.desc.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(sc.desc.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (sc.desc.width == 0 || sc.desc.height == 0 || extent.width == 0 || extent.height == 0) {
        sc.extent = {0, 0};  // minimized: no images until a nonzero resize
        return;
    }

    uint32_t mode_count = 0;
    vk_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, sc.surface, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vk_.vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, sc.surface, &mode_count, modes.data());
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;  // always available, vsync
    if (!sc.desc.vsync) {
        for (VkPresentModeKHR m : {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR}) {
            if (std::find(modes.begin(), modes.end(), m) != modes.end()) {
                mode = m;
                break;
            }
        }
    }

    uint32_t image_count = std::max(sc.desc.min_image_count, caps.minImageCount);
    if (caps.maxImageCount != 0) {
        image_count = std::min(image_count, caps.maxImageCount);
    }
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if ((caps.supportedCompositeAlpha & static_cast<VkCompositeAlphaFlagsKHR>(alpha)) == 0) {
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
            if ((caps.supportedCompositeAlpha & static_cast<VkCompositeAlphaFlagsKHR>(a)) != 0) {
                alpha = a;
                break;
            }
        }
    }
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const bool transfer_dst = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) != 0;
    if (transfer_dst) {
        usage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    VkSwapchainKHR old = sc.swapchain;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = sc.surface;
    ci.minImageCount = image_count;
    ci.imageFormat = sc.vk_format;
    ci.imageColorSpace = sc.color_space;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = usage;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    check(vk_.vkCreateSwapchainKHR(device_, &ci, nullptr, &sc.swapchain), "vkCreateSwapchainKHR");
    if (old != VK_NULL_HANDLE) {
        vk_.vkDestroySwapchainKHR(device_, old, nullptr);
    }
    sc.extent = extent;
    set_name(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<uint64_t>(sc.swapchain), sc.desc.debug_name);

    uint32_t count = 0;
    vk_.vkGetSwapchainImagesKHR(device_, sc.swapchain, &count, nullptr);
    std::vector<VkImage> images(count);
    vk_.vkGetSwapchainImagesKHR(device_, sc.swapchain, &count, images.data());
    for (VkImage image : images) {
        VkTextureRes t;
        t.desc.format = sc.format;
        t.desc.width = extent.width;
        t.desc.height = extent.height;
        t.desc.usage = TextureUsage::ColorAttachment | (transfer_dst ? TextureUsage::TransferDst : TextureUsage::None);
        t.desc.debug_name = "SwapchainImage";
        t.image = image;
        t.swapchain_image = true;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = sc.vk_format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        check(vk_.vkCreateImageView(device_, &vi, nullptr, &t.view), "vkCreateImageView(swapchain)");
        sc.images.push_back(textures_.emplace(std::move(t)));

        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem = VK_NULL_HANDLE;
        check(vk_.vkCreateSemaphore(device_, &si, nullptr, &sem), "vkCreateSemaphore(present)");
        sc.render_finished.push_back(sem);
    }
}

void VulkanDevice::resize_swapchain(SwapchainHandle handle, uint32_t width, uint32_t height)
{
    VkSwapchainRes* sc = swapchains_.get(handle);
    if (sc == nullptr) {
        throw std::invalid_argument("resize_swapchain: invalid swapchain");
    }
    if (in_frame_) {
        throw std::logic_error("resize_swapchain: cannot resize inside a frame");
    }
    // Simple and safe for 0.1: let in-flight frames finish before the images go away.
    vk_.vkDeviceWaitIdle(device_);
    collect_garbage();
    release_swapchain_images(*sc);
    sc->desc.width = width;
    sc->desc.height = height;
    if (width == 0 || height == 0) {
        if (sc->swapchain != VK_NULL_HANDLE) {
            vk_.vkDestroySwapchainKHR(device_, sc->swapchain, nullptr);
            sc->swapchain = VK_NULL_HANDLE;
        }
        sc->extent = {0, 0};
        return;
    }
    build_swapchain(*sc);
}

void VulkanDevice::destroy(SwapchainHandle handle)
{
    VkSwapchainRes* sc = swapchains_.get(handle);
    if (sc == nullptr) {
        return;
    }
    vk_.vkDeviceWaitIdle(device_);
    collect_garbage();
    release_swapchain_images(*sc);
    if (sc->swapchain != VK_NULL_HANDLE) {
        vk_.vkDestroySwapchainKHR(device_, sc->swapchain, nullptr);
    }
    vk_.vkDestroySurfaceKHR(instance_, sc->surface, nullptr);
    swapchains_.release(handle);
}

Format VulkanDevice::swapchain_format(SwapchainHandle handle) const
{
    const VkSwapchainRes* sc = swapchains_.get(handle);
    return sc != nullptr ? sc->format : Format::Unknown;
}

// =====================================================================================================================
// Frames
// =====================================================================================================================

void VulkanDevice::collect_timestamps(FrameSlot& slot)
{
    // Each query reports (value, availability). Queries the frame did not write stay unavailable after the reset.
    uint64_t raw[max_gpu_timestamps * 2] = {};
    const uint32_t first = static_cast<uint32_t>(&slot - slots_.data()) * max_gpu_timestamps;
    const VkResult r = vk_.vkGetQueryPoolResults(device_, timestamp_pool_, first, max_gpu_timestamps, sizeof(raw), raw,
                                                 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r != VK_SUCCESS && r != VK_NOT_READY) {
        check(r, "vkGetQueryPoolResults(timestamps)");
    }
    GpuTimestamps t;
    t.frame_number = slot.frame_number;
    t.period_ns = static_cast<double>(timestamp_period_ns_);
    t.valid_bits = std::min(timestamp_valid_bits_, 64u);
    const uint64_t mask = timestamp_valid_bits_ >= 64 ? ~uint64_t{0} : ((uint64_t{1} << timestamp_valid_bits_) - 1u);
    for (uint32_t i = 0; i < max_gpu_timestamps; ++i) {
        if (raw[i * 2 + 1] != 0) {
            t.available_mask |= 1u << i;
            t.ticks[i] = raw[i * 2] & mask;
        }
    }
    if (t.available_mask != 0) {
        completed_timestamps_ = t;
    }
}

std::optional<GpuTimestamps> VulkanDevice::completed_timestamps() const
{
    return completed_timestamps_;
}

DeviceMemoryStats VulkanDevice::memory_stats() const
{
    DeviceMemoryStats s;
    s.allocated_bytes = allocated_bytes_;
    s.allocation_count = static_cast<uint32_t>(allocation_sizes_.size());
    if (memory_budget_ && vk_.vkGetPhysicalDeviceMemoryProperties2 != nullptr) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        props.pNext = &budget;
        vk_.vkGetPhysicalDeviceMemoryProperties2(physical_, &props);
        for (uint32_t i = 0; i < props.memoryProperties.memoryHeapCount; ++i) {
            if ((props.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
                s.budget_available = true;
                s.budget_bytes += budget.heapBudget[i];
                s.usage_bytes += budget.heapUsage[i];
            }
        }
    }
    return s;
}

CommandList& VulkanDevice::begin_frame()
{
    if (in_frame_) {
        throw std::logic_error("begin_frame: previous frame not ended");
    }
    slot_index_ = static_cast<uint32_t>(frame_number_ % slots_.size());
    FrameSlot& slot = slots_[slot_index_];
    if (slot.timeline_value != 0) {
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline_;
        wi.pValues = &slot.timeline_value;
        check(vk_.vkWaitSemaphores(device_, &wi, std::numeric_limits<uint64_t>::max()), "vkWaitSemaphores(frame)");
    }
    collect_garbage();
    if (timestamps_ && slot.recorded) {
        collect_timestamps(slot);  // this slot's previous frame has finished: its timestamps are final
    }
    check(vk_.vkResetCommandPool(device_, slot.pool, 0), "vkResetCommandPool");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vk_.vkBeginCommandBuffer(slot.cmd, &bi), "vkBeginCommandBuffer");
    if (timestamps_) {
        vk_.vkCmdResetQueryPool(slot.cmd, timestamp_pool_, slot_index_ * max_gpu_timestamps, max_gpu_timestamps);
    }
    slot.frame_number = frame_number_;
    slot.recorded = true;
    presents_.clear();
    in_frame_ = true;
    command_list_.begin(slot.cmd);
    return command_list_;
}

AcquireResult VulkanDevice::acquire_next_image(SwapchainHandle handle)
{
    if (!in_frame_) {
        throw std::logic_error("acquire_next_image: call between begin_frame() and end_frame()");
    }
    VkSwapchainRes* sc = swapchains_.get(handle);
    if (sc == nullptr) {
        throw std::invalid_argument("acquire_next_image: invalid swapchain");
    }
    if (sc->swapchain == VK_NULL_HANDLE || sc->images.empty()) {
        return {AcquireStatus::NotReady, {}};
    }
    FrameSlot& slot = slots_[slot_index_];
    const std::size_t index_in_frame = presents_.size();
    while (slot.acquire_semaphores.size() <= index_in_frame) {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VkSemaphore sem = VK_NULL_HANDLE;
        check(vk_.vkCreateSemaphore(device_, &si, nullptr, &sem), "vkCreateSemaphore(acquire)");
        slot.acquire_semaphores.push_back(sem);
    }
    const VkSemaphore acquire = slot.acquire_semaphores[index_in_frame];
    uint32_t image_index = 0;
    const VkResult r = vk_.vkAcquireNextImageKHR(device_, sc->swapchain, std::numeric_limits<uint64_t>::max(), acquire,
                                                 VK_NULL_HANDLE, &image_index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        return {AcquireStatus::OutOfDate, {}};
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        fail("vkAcquireNextImageKHR", r);
    }
    presents_.push_back({handle, image_index, acquire});
    return {AcquireStatus::Success, sc->images[image_index]};
}

FrameResult VulkanDevice::end_frame()
{
    if (!in_frame_) {
        throw std::logic_error("end_frame: no frame in progress");
    }
    FrameSlot& slot = slots_[slot_index_];
    check(vk_.vkEndCommandBuffer(slot.cmd), "vkEndCommandBuffer");

    std::vector<VkSemaphoreSubmitInfo> waits;
    std::vector<VkSemaphoreSubmitInfo> signals;
    for (const PendingPresent& p : presents_) {
        VkSemaphoreSubmitInfo w{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        w.semaphore = p.acquire;
        w.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        waits.push_back(w);
        VkSemaphoreSubmitInfo s{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        s.semaphore = swapchains_.get(p.swapchain)->render_finished[p.image_index];
        s.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        signals.push_back(s);
    }
    const uint64_t value = ++timeline_counter_;
    VkSemaphoreSubmitInfo timeline_signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    timeline_signal.semaphore = timeline_;
    timeline_signal.value = value;
    timeline_signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signals.push_back(timeline_signal);

    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = slot.cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = static_cast<uint32_t>(waits.size());
    si.pWaitSemaphoreInfos = waits.data();
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = static_cast<uint32_t>(signals.size());
    si.pSignalSemaphoreInfos = signals.data();
    in_frame_ = false;
    slot.timeline_value = value;
    ++frame_number_;
    check(vk_.vkQueueSubmit2(queue_, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit2");

    FrameResult result;
    for (const PendingPresent& p : presents_) {
        VkSwapchainRes* sc = swapchains_.get(p.swapchain);
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sc->render_finished[p.image_index];
        pi.swapchainCount = 1;
        pi.pSwapchains = &sc->swapchain;
        pi.pImageIndices = &p.image_index;
        const VkResult r = vk_.vkQueuePresentKHR(queue_, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            result.swapchain_out_of_date = true;
        } else if (r != VK_SUCCESS) {
            fail("vkQueuePresentKHR", r);
        }
        result.presented = true;
    }
    presents_.clear();
    return result;
}

void VulkanDevice::wait_idle()
{
    if (in_frame_) {
        throw std::logic_error("wait_idle: cannot wait inside a frame");
    }
    check(vk_.vkDeviceWaitIdle(device_), "vkDeviceWaitIdle");
    pending_.drain_all([](std::function<void()>&& fn) { fn(); });
}

// =====================================================================================================================
// Command list
// =====================================================================================================================

void VulkanCommandList::barrier(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers)
{
    std::vector<VkImageMemoryBarrier2> images;
    images.reserve(textures.size());
    for (const TextureBarrier& b : textures) {
        const VkTextureRes* t = device_.textures_.get(b.texture);
        if (t == nullptr) {
            throw std::invalid_argument("barrier: invalid texture");
        }
        VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        ib.srcStageMask = to_vk(b.before.stages);
        ib.srcAccessMask = to_vk(b.before.access);
        ib.dstStageMask = to_vk(b.after.stages);
        ib.dstAccessMask = to_vk(b.after.access);
        ib.oldLayout = to_vk(b.before.layout);
        ib.newLayout = to_vk(b.after.layout);
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = t->image;
        ib.subresourceRange.aspectMask = t->aspect;
        ib.subresourceRange.baseMipLevel = b.range.base_mip;
        ib.subresourceRange.levelCount = b.range.mip_count == all_mips ? VK_REMAINING_MIP_LEVELS : b.range.mip_count;
        ib.subresourceRange.baseArrayLayer = b.range.base_layer;
        ib.subresourceRange.layerCount =
            b.range.layer_count == all_layers ? VK_REMAINING_ARRAY_LAYERS : b.range.layer_count;
        images.push_back(ib);
    }
    std::vector<VkBufferMemoryBarrier2> bufs;
    for (const BufferBarrier& b : buffers) {
        const VkBufferRes* buf = device_.buffers_.get(b.buffer);
        if (buf == nullptr) {
            throw std::invalid_argument("barrier: invalid buffer");
        }
        VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        bb.srcStageMask = to_vk(b.before.stages);
        bb.srcAccessMask = to_vk(b.before.access);
        bb.dstStageMask = to_vk(b.after.stages);
        bb.dstAccessMask = to_vk(b.after.access);
        bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = buf->buffer;
        bb.offset = b.offset;
        bb.size = b.size == ~uint64_t{0} ? VK_WHOLE_SIZE : b.size;
        bufs.push_back(bb);
    }
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = static_cast<uint32_t>(images.size());
    di.pImageMemoryBarriers = images.data();
    di.bufferMemoryBarrierCount = static_cast<uint32_t>(bufs.size());
    di.pBufferMemoryBarriers = bufs.data();
    device_.vk_.vkCmdPipelineBarrier2(cmd_, &di);
}

void VulkanCommandList::begin_rendering(const RenderingDesc& desc)
{
    std::vector<VkRenderingAttachmentInfo> colors;
    for (const ColorAttachment& a : desc.colors) {
        const VkTextureRes* t = device_.textures_.get(a.texture);
        if (t == nullptr) {
            throw std::invalid_argument("begin_rendering: invalid color attachment");
        }
        VkRenderingAttachmentInfo ai{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ai.imageView = t->view;
        ai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ai.loadOp = a.load == AttachmentLoadOp::Load    ? VK_ATTACHMENT_LOAD_OP_LOAD
                    : a.load == AttachmentLoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                        : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        ai.storeOp = a.store == AttachmentStoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        ai.clearValue.color = {{a.clear.r, a.clear.g, a.clear.b, a.clear.a}};
        colors.push_back(ai);
    }
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    bool has_stencil_aspect = false;
    if (desc.depth) {
        const VkTextureRes* t = device_.textures_.get(desc.depth->texture);
        if (t == nullptr) {
            throw std::invalid_argument("begin_rendering: invalid depth attachment");
        }
        depth.imageView = t->view;
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = desc.depth->load == AttachmentLoadOp::Load    ? VK_ATTACHMENT_LOAD_OP_LOAD
                       : desc.depth->load == AttachmentLoadOp::Clear ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                     : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depth.storeOp =
            desc.depth->store == AttachmentStoreOp::Store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depth.clearValue.depthStencil = {desc.depth->clear_depth, 0};
        has_stencil_aspect = (t->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) != 0;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{desc.area.x, desc.area.y}, {desc.area.width, desc.area.height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = static_cast<uint32_t>(colors.size());
    ri.pColorAttachments = colors.data();
    ri.pDepthAttachment = desc.depth ? &depth : nullptr;
    ri.pStencilAttachment = has_stencil_aspect ? &depth : nullptr;
    device_.vk_.vkCmdBeginRendering(cmd_, &ri);
}

void VulkanCommandList::end_rendering()
{
    device_.vk_.vkCmdEndRendering(cmd_);
}

void VulkanCommandList::set_viewport(const Viewport& v)
{
    // Negative height (core since Vulkan 1.1) flips Y so NDC +Y is up, matching every other backend.
    VkViewport vp{v.x, v.y + v.height, v.width, -v.height, v.min_depth, v.max_depth};
    device_.vk_.vkCmdSetViewport(cmd_, 0, 1, &vp);
}

void VulkanCommandList::set_scissor(const Rect2D& r)
{
    VkRect2D rect{{r.x, r.y}, {r.width, r.height}};
    device_.vk_.vkCmdSetScissor(cmd_, 0, 1, &rect);
}

void VulkanCommandList::bind_pipeline(PipelineHandle pipeline)
{
    const VkPipelineRes* p = device_.pipelines_.get(pipeline);
    if (p == nullptr) {
        throw std::invalid_argument("bind_pipeline: invalid pipeline");
    }
    device_.vk_.vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    if (pipeline != bound_pipeline_) {
        // Descriptor compatibility also depends on push constant ranges and every lower set's layout, so a different
        // pipeline starts with nothing bound: the caller rebinds its groups (same-handle rebinds keep them).
        bound_groups_.fill({});
    }
    bound_pipeline_ = pipeline;
    bound_layout_ = p->layout;
    bound_push_size_ = p->push_constant_size;
}

void VulkanCommandList::write_timestamp(uint32_t index)
{
    if (index >= max_gpu_timestamps) {
        throw std::invalid_argument("write_timestamp: index out of range");
    }
    if ((written_timestamps_ & (1u << index)) != 0) {
        throw std::invalid_argument("write_timestamp: index " + std::to_string(index) + " was already written in this frame");
    }
    written_timestamps_ |= 1u << index;
    if (!device_.timestamps_) {
        return;  // unavailable on this device: documented no-op (DeviceInfo::gpu_timestamps is false)
    }
    device_.vk_.vkCmdWriteTimestamp2(cmd_, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, device_.timestamp_pool_,
                                     device_.slot_index_ * max_gpu_timestamps + index);
}

void VulkanCommandList::bind_group(uint32_t set, BindGroupHandle group)
{
    // Every misuse is rejected here, before a Vulkan call is made (the validation layer is a safety net, not the contract).
    const VkPipelineRes* p = device_.pipelines_.get(bound_pipeline_);
    if (p == nullptr || bound_layout_ == VK_NULL_HANDLE) {
        throw std::invalid_argument("bind_group: no pipeline bound");
    }
    if (set >= p->bind_group_layouts.size()) {
        throw std::invalid_argument("bind_group: the bound pipeline has no descriptor set " + std::to_string(set));
    }
    const VkBindGroupRes* g = device_.bind_groups_.get(group);
    if (g == nullptr) {
        throw std::invalid_argument("bind_group: invalid bind group");
    }
    if (g->layout_handle != p->bind_group_layouts[set]) {
        throw std::invalid_argument("bind_group: the group's layout differs from the pipeline's layout for set " +
                                    std::to_string(set));
    }
    // A group does not own what it references. A later frame that reuses a group after one of its resources was destroyed
    // would read freed descriptors, so that is rejected here (rebuild the group). Destroying a resource after the group
    // is bound in the same frame is still safe: destruction is deferred until this frame's submission retires.
    if (!device_.group_resources_alive(*g)) {
        throw std::invalid_argument("bind_group: the group refers to a resource that has been destroyed; rebuild the group");
    }
    bound_groups_[set] = group;
    device_.vk_.vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, bound_layout_, set, 1, &g->set, 0, nullptr);
}

/// A pipeline that declares descriptor sets can only draw once a compatible group is bound at every one of them.
void VulkanCommandList::check_draw_state(const char* what) const
{
    const VkPipelineRes* p = device_.pipelines_.get(bound_pipeline_);
    if (p == nullptr) {
        throw std::invalid_argument(std::string(what) + ": no pipeline bound");
    }
    for (std::size_t set = 0; set < p->bind_group_layouts.size(); ++set) {
        const VkBindGroupRes* g = device_.bind_groups_.get(bound_groups_[set]);
        if (g == nullptr || g->layout_handle != p->bind_group_layouts[set]) {
            throw std::invalid_argument(std::string(what) + ": no compatible bind group at set " + std::to_string(set));
        }
    }
}

void VulkanCommandList::push_constants(std::span<const std::byte> data, uint32_t offset)
{
    if (bound_layout_ == VK_NULL_HANDLE || offset + data.size() > bound_push_size_) {
        throw std::invalid_argument("push_constants: no pipeline bound or range exceeded");
    }
    device_.vk_.vkCmdPushConstants(cmd_, bound_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, offset,
                                   static_cast<uint32_t>(data.size()), data.data());
}

void VulkanCommandList::bind_vertex_buffer(BufferHandle buffer, uint64_t offset)
{
    const VkBufferRes* b = device_.buffers_.get(buffer);
    if (b == nullptr) {
        throw std::invalid_argument("bind_vertex_buffer: invalid buffer");
    }
    const VkDeviceSize off = offset;
    device_.vk_.vkCmdBindVertexBuffers(cmd_, 0, 1, &b->buffer, &off);
}

void VulkanCommandList::bind_index_buffer(BufferHandle buffer, IndexType type, uint64_t offset)
{
    const VkBufferRes* b = device_.buffers_.get(buffer);
    if (b == nullptr) {
        throw std::invalid_argument("bind_index_buffer: invalid buffer");
    }
    device_.vk_.vkCmdBindIndexBuffer(cmd_, b->buffer, offset,
                                     type == IndexType::Uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
}

void VulkanCommandList::draw(uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex,
                             uint32_t first_instance)
{
    check_draw_state("draw");
    device_.vk_.vkCmdDraw(cmd_, vertex_count, instance_count, first_vertex, first_instance);
}

void VulkanCommandList::draw_indexed(uint32_t index_count, uint32_t instance_count, uint32_t first_index,
                                     int32_t vertex_offset, uint32_t first_instance)
{
    check_draw_state("draw_indexed");
    device_.vk_.vkCmdDrawIndexed(cmd_, index_count, instance_count, first_index, vertex_offset, first_instance);
}

void VulkanCommandList::copy_texture_to_buffer(TextureHandle texture, BufferHandle buffer)
{
    const VkTextureRes* t = device_.textures_.get(texture);
    const VkBufferRes* b = device_.buffers_.get(buffer);
    if (t == nullptr || b == nullptr) {
        throw std::invalid_argument("copy_texture_to_buffer: invalid texture or buffer");
    }
    const SurfaceLayout layout = compute_surface_layout(t->desc.format, t->desc.width, t->desc.height);
    if (b->desc.size < layout.slice_pitch) {
        throw std::invalid_argument("copy_texture_to_buffer: buffer too small");
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {t->aspect, 0, 0, 1};
    region.imageExtent = {t->desc.width, t->desc.height, 1};
    device_.vk_.vkCmdCopyImageToBuffer(cmd_, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b->buffer, 1, &region);
}

void VulkanCommandList::begin_debug_label(const char* name)
{
    if (device_.vk_.vkCmdBeginDebugUtilsLabelEXT != nullptr && device_.debug_utils_) {
        VkDebugUtilsLabelEXT label{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
        label.pLabelName = name;
        device_.vk_.vkCmdBeginDebugUtilsLabelEXT(cmd_, &label);
    }
}

void VulkanCommandList::end_debug_label()
{
    if (device_.vk_.vkCmdEndDebugUtilsLabelEXT != nullptr && device_.debug_utils_) {
        device_.vk_.vkCmdEndDebugUtilsLabelEXT(cmd_);
    }
}

// =====================================================================================================================

std::unique_ptr<Device> create_vulkan_device(const DeviceDesc& desc)
{
    return std::make_unique<VulkanDevice>(desc);
}

bool vulkan_device_available(std::string* reason)
{
    try {
        DeviceDesc desc;
        desc.enable_validation = false;
        const auto device = create_vulkan_device(desc);
        return device != nullptr;
    } catch (const std::exception& e) {
        if (reason != nullptr) {
            *reason = e.what();
        }
        return false;
    }
}

#if defined(SAIGE_RHI_VULKAN_TESTING)
namespace testing {
void fail_memory_bookkeeping_after(uint32_t n) noexcept
{
    g_memory_bookkeeping_fault.store(n, std::memory_order_relaxed);
}

uint32_t process_validation_error_count() noexcept
{
    return g_process_validation_errors.load(std::memory_order_relaxed);
}
} // namespace testing
#endif

} // namespace saige::rhi
