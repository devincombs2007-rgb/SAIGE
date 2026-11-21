// SAIGE RHI — backend-agnostic synchronization model (maps 1:1 onto Vulkan synchronization2 and D3D12
// enhanced barriers).
#pragma once

#include "saige/rhi/flags.h"
#include "saige/rhi/handle.h"

#include <cstdint>

namespace saige::rhi {

/// Pipeline stages that a resource access happens in.
enum class PipelineStage : uint32_t {
    None = 0,
    DrawIndirect = 1u << 0,
    VertexInput = 1u << 1,
    VertexShader = 1u << 2,
    TaskShader = 1u << 3,
    MeshShader = 1u << 4,
    PixelShader = 1u << 5,
    EarlyFragmentTests = 1u << 6,
    LateFragmentTests = 1u << 7,
    ColorAttachmentOutput = 1u << 8,
    ComputeShader = 1u << 9,
    RayTracingShader = 1u << 10,
    Transfer = 1u << 11,
    AccelerationStructureBuild = 1u << 12,
    Host = 1u << 13,
    AllCommands = 1u << 31,
};
SAIGE_RHI_ENUM_FLAGS(PipelineStage)

/// Kinds of memory access. A barrier makes writes from its source scope available and visible to the
/// accesses in its destination scope.
enum class Access : uint32_t {
    None = 0,
    IndirectCommandRead = 1u << 0,
    IndexRead = 1u << 1,
    VertexAttributeRead = 1u << 2,
    UniformRead = 1u << 3,
    ShaderSampledRead = 1u << 4,
    ShaderStorageRead = 1u << 5,
    ShaderStorageWrite = 1u << 6,
    ColorAttachmentRead = 1u << 7,
    ColorAttachmentWrite = 1u << 8,
    DepthStencilRead = 1u << 9,
    DepthStencilWrite = 1u << 10,
    TransferRead = 1u << 11,
    TransferWrite = 1u << 12,
    AccelerationStructureRead = 1u << 13,
    AccelerationStructureWrite = 1u << 14,
    HostRead = 1u << 15,
    HostWrite = 1u << 16,
};
SAIGE_RHI_ENUM_FLAGS(Access)

inline constexpr Access kWriteAccessMask = Access::ShaderStorageWrite | Access::ColorAttachmentWrite |
                                           Access::DepthStencilWrite | Access::TransferWrite |
                                           Access::AccelerationStructureWrite | Access::HostWrite;

[[nodiscard]] constexpr bool isWriteAccess(Access a) noexcept { return hasAny(a, kWriteAccessMask); }
[[nodiscard]] constexpr Access writeAccessOnly(Access a) noexcept { return a & kWriteAccessMask; }

/// Image layouts (ignored for buffers).
enum class TextureLayout : uint8_t {
    Undefined,  ///< Contents may be discarded.
    General,    ///< Storage access, or mixed access within a pass.
    ColorAttachment,
    DepthStencilAttachment,
    DepthStencilReadOnly,  ///< Depth test without writes; also allows sampling.
    ShaderReadOnly,
    TransferSrc,
    TransferDst,
    Present,
};

/// A resource's synchronization state: which stages access it, how, and (for textures) in which layout.
struct ResourceState {
    PipelineStage stages = PipelineStage::None;
    Access access = Access::None;
    TextureLayout layout = TextureLayout::Undefined;

    [[nodiscard]] constexpr bool writes() const noexcept { return isWriteAccess(access); }
    friend constexpr bool operator==(const ResourceState&, const ResourceState&) noexcept = default;
};

inline constexpr uint32_t kAllMips = 0xFFFFFFFFu;
inline constexpr uint32_t kAllLayers = 0xFFFFFFFFu;

struct TextureSubresourceRange {
    uint32_t baseMip = 0;
    uint32_t mipCount = kAllMips;
    uint32_t baseLayer = 0;
    uint32_t layerCount = kAllLayers;
};

/// A texture memory dependency plus optional layout transition.
/// `before.layout == Undefined` means the previous contents are discarded.
struct TextureBarrier {
    TextureHandle texture;
    ResourceState before;
    ResourceState after;
    TextureSubresourceRange range;
};

/// A buffer memory dependency. Layouts are ignored.
struct BufferBarrier {
    BufferHandle buffer;
    ResourceState before;
    ResourceState after;
    uint64_t offset = 0;
    uint64_t size = ~uint64_t{0};  ///< Whole buffer by default.
};

} // namespace saige::rhi
