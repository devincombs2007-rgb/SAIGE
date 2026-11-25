// SAIGE RHI null backend: a CPU-only Device that validates usage and records commands.
//
// Uses: unit and integration tests without a GPU, headless CI, and engine runs where no GPU is wanted.
// API misuse (drawing outside rendering, missing pipeline, push constants out of range, protocol errors) is
// counted as a validation error, mirroring what the Vulkan validation layer reports for real backends.
#pragma once

#include "saige/rhi/device.hpp"

#include <memory>
#include <string>
#include <vector>

namespace saige::rhi {

class NullCommandList;

enum class CommandType : uint8_t {
    Barrier,
    BeginRendering,
    EndRendering,
    SetViewport,
    SetScissor,
    BindPipeline,
    Timestamp,  ///< index in `count`
    BindGroup,
    PushConstants,
    BindVertexBuffer,
    BindIndexBuffer,
    Draw,
    DrawIndexed,
    CopyTextureToBuffer,
    BeginDebugLabel,
    EndDebugLabel,
};

/// One recorded command. Only the fields relevant to `type` are set.
struct RecordedCommand {
    CommandType type = CommandType::Draw;
    PipelineHandle pipeline;
    BufferHandle buffer;
    TextureHandle texture;
    BindGroupHandle group;        ///< BindGroup (set index in `count`)
    RenderingDesc rendering;
    Viewport viewport;
    Rect2D scissor;
    std::vector<TextureBarrier> texture_barriers;
    std::vector<BufferBarrier> buffer_barriers;
    std::vector<std::byte> push_data;
    uint32_t push_offset = 0;
    uint32_t count = 0;           ///< vertex/index count
    uint32_t instance_count = 0;
    std::string label;
};

class NullDevice final : public Device {
public:
    explicit NullDevice(const DeviceDesc& desc = {});
    ~NullDevice() override;

    [[nodiscard]] const DeviceInfo& info() const noexcept override;

    BufferHandle create_buffer(const BufferDesc& desc, std::span<const std::byte> initial_data = {}) override;
    TextureHandle create_texture(const TextureDesc& desc) override;
    void upload_texture(TextureHandle texture, std::span<const std::byte> data) override;
    SamplerHandle create_sampler(const SamplerDesc& desc) override;
    BindGroupLayoutHandle create_bind_group_layout(const BindGroupLayoutDesc& desc) override;
    BindGroupHandle create_bind_group(BindGroupLayoutHandle layout, std::span<const BindGroupEntry> entries) override;
    void destroy(BindGroupLayoutHandle handle) override;
    void destroy(BindGroupHandle handle) override;
    ShaderHandle create_shader(const ShaderDesc& desc) override;
    PipelineHandle create_graphics_pipeline(const GraphicsPipelineDesc& desc) override;
    void destroy(BufferHandle handle) override;
    void destroy(TextureHandle handle) override;
    void destroy(SamplerHandle handle) override;
    void destroy(ShaderHandle handle) override;
    void destroy(PipelineHandle handle) override;
    [[nodiscard]] std::span<std::byte> map(BufferHandle handle) override;
    [[nodiscard]] std::optional<TextureDesc> texture_desc(TextureHandle handle) const override;

    SwapchainHandle create_swapchain(const SwapchainDesc& desc) override;
    void resize_swapchain(SwapchainHandle handle, uint32_t width, uint32_t height) override;
    void destroy(SwapchainHandle handle) override;
    [[nodiscard]] Format swapchain_format(SwapchainHandle handle) const override;

    CommandList& begin_frame() override;
    AcquireResult acquire_next_image(SwapchainHandle handle) override;
    FrameResult end_frame() override;
    void wait_idle() override;
    [[nodiscard]] uint64_t frame_number() const noexcept override;
    [[nodiscard]] bool supports_filtered_sampling(Format format) const override;
    [[nodiscard]] uint32_t validation_error_count() const noexcept override;
    /// The null device has no GPU: timestamps are unavailable. Memory is estimated from the resource descriptions
    /// (tightly packed texture sizes), so it is a stand-in for tests, not a measurement.
    [[nodiscard]] std::optional<GpuTimestamps> completed_timestamps() const override;
    [[nodiscard]] DeviceMemoryStats memory_stats() const override;

    // ----- Test inspection ------------------------------------------------------------------------------
    /// Fault injection: the n-th (1-based) creation after this call (buffer, texture, sampler, bind group layout, bind group, shader or
    /// pipeline, counted together in call order) throws std::runtime_error, as a failed device allocation would, leaving nothing
    /// registered for that call. 0 cancels. Consumed by the creation it fails. Lets tests prove that code acquiring several resources
    /// releases the earlier ones when a later acquisition fails.
    void fail_creation_after(uint32_t n) noexcept;
    /// Makes supports_filtered_sampling(format) answer false (Format::Unknown cancels), to test the renderer's refusal.
    void deny_filtered_sampling(Format format) noexcept;
    /// Commands recorded in the most recently ended frame.
    [[nodiscard]] const std::vector<RecordedCommand>& last_frame_commands() const noexcept;
    [[nodiscard]] const std::vector<std::string>& validation_messages() const noexcept;
    /// Live (not yet destroyed) buffers + textures + shaders + pipelines, excluding swapchain images.
    [[nodiscard]] uint32_t live_resource_count() const noexcept;
    /// Objects destroyed by the caller but still waiting for their frame to retire.
    [[nodiscard]] uint32_t pending_destruction_count() const noexcept;
    /// Bytes handed to upload_texture for `texture` (0 if never uploaded or invalid).
    [[nodiscard]] uint64_t uploaded_bytes(TextureHandle texture) const noexcept;
    [[nodiscard]] Extent3D swapchain_extent(SwapchainHandle handle) const;
    [[nodiscard]] uint32_t swapchain_recreate_count(SwapchainHandle handle) const;
    [[nodiscard]] uint32_t present_count() const noexcept;
    /// The next acquire on `handle` reports OutOfDate (simulates a window-system resize race).
    void force_out_of_date(SwapchainHandle handle);

private:
    friend class NullCommandList;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saige::rhi
