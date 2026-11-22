// SAIGE Render — the GPU resource registry: shared ownership, live counters and leak reporting for buffers, textures
// and samplers.
//
// Ownership: every resource is created through the registry and held through a reference (`GpuRef`). References are
// copyable and share the resource; when the last one goes away the resource is handed to the device for destruction.
// The device defers the real destruction until the GPU has finished every submission that could still use it, so
// releasing a reference while frames are in flight is always safe (rhi::Device::destroy contract). The registry adds
// what the device does not know: who still holds what, how much is live, and what leaked.
//
// Threading: single-threaded, like rhi::Device (0.1 contract D-08). References are not safe to copy or release from
// two threads.
//
// Lifetime safety: a reference keeps the registry's bookkeeping alive but not the device. Destroying the registry while
// references remain reports each live resource (the leak report), destroys them on the device, and turns the late
// references into inert handles whose release does nothing, so teardown order mistakes are reported, not undefined.
#pragma once

#include "saige/rhi/device.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace saige::render {

enum class ResourceKind : uint8_t { Buffer, Texture, Sampler, BindGroup };
inline constexpr std::size_t resource_kind_count = 4;
[[nodiscard]] const char* to_string(ResourceKind kind) noexcept;

namespace detail {
struct RegistryState;
} // namespace detail

/// What a reference points at. Opaque; compare by `handle()` on the typed reference.
struct RefCore {
    std::shared_ptr<detail::RegistryState> state;
    uint32_t slot = 0;
    uint32_t generation = 0;

    RefCore() = default;
    RefCore(std::shared_ptr<detail::RegistryState> s, uint32_t slot_index, uint32_t gen) noexcept
        : state(std::move(s)), slot(slot_index), generation(gen)
    {
    }
    RefCore(const RefCore& other);
    RefCore(RefCore&& other) noexcept;
    RefCore& operator=(const RefCore& other);
    RefCore& operator=(RefCore&& other) noexcept;
    ~RefCore();

    [[nodiscard]] bool valid() const noexcept { return state != nullptr; }
    [[nodiscard]] uint32_t use_count() const noexcept;
    [[nodiscard]] uint64_t bytes() const noexcept;
    [[nodiscard]] std::string name() const;
    void reset() noexcept;
};

/// A shared reference to one registry-owned resource. Default-constructed references are empty.
template <typename Handle>
class GpuRef {
public:
    GpuRef() = default;
    [[nodiscard]] Handle handle() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return core_.valid(); }
    explicit operator bool() const noexcept { return valid(); }
    /// How many references share the resource (including this one); 0 for an empty reference.
    [[nodiscard]] uint32_t use_count() const noexcept { return core_.use_count(); }
    /// Estimated GPU bytes (buffers: size; textures: tightly packed size of every mip and layer).
    [[nodiscard]] uint64_t bytes() const noexcept { return core_.bytes(); }
    [[nodiscard]] std::string name() const { return core_.name(); }
    void reset() noexcept
    {
        core_.reset();
        handle_ = {};
    }
    friend bool operator==(const GpuRef& a, const GpuRef& b) noexcept { return a.handle_ == b.handle_; }

private:
    friend class GpuResourceRegistry;
    GpuRef(RefCore core, Handle handle) noexcept : core_(std::move(core)), handle_(handle) {}
    RefCore core_;
    Handle handle_{};
};

using BufferRef = GpuRef<rhi::BufferHandle>;
using TextureRef = GpuRef<rhi::TextureHandle>;
using SamplerRef = GpuRef<rhi::SamplerHandle>;
using BindGroupRef = GpuRef<rhi::BindGroupHandle>;

/// One resource of a bind group, held by reference: the group keeps it alive for as long as the group exists, so a group
/// can never outlive what it binds. Set `texture` for a SampledTexture binding, `sampler` for a Sampler binding, `buffer`
/// (and optionally a byte range) for buffer bindings.
struct BindGroupBinding {
    uint32_t binding = 0;
    TextureRef texture;
    SamplerRef sampler;
    BufferRef buffer;
    uint64_t offset = 0;
    uint64_t size = ~uint64_t{0};
};

struct ResourceStats {
    uint32_t live = 0;          ///< resources with at least one reference
    uint64_t live_bytes = 0;    ///< estimated GPU bytes of those
    uint64_t peak_bytes = 0;    ///< highest live_bytes seen
    uint64_t created = 0;
    uint64_t destroyed = 0;     ///< released by their last reference (the device retires them when the GPU is done)
};

struct ResourceInfo {
    ResourceKind kind = ResourceKind::Buffer;
    std::string name;
    uint64_t bytes = 0;
    uint32_t references = 0;
};

class GpuResourceRegistry {
public:
    /// The device must outlive the registry. Resources are created on it and destroyed on it.
    explicit GpuResourceRegistry(rhi::Device& device);
    ~GpuResourceRegistry();
    GpuResourceRegistry(const GpuResourceRegistry&) = delete;
    GpuResourceRegistry& operator=(const GpuResourceRegistry&) = delete;

    /// Creation throws what the device throws; nothing is registered on failure.
    [[nodiscard]] BufferRef create_buffer(const rhi::BufferDesc& desc, std::span<const std::byte> initial_data = {});
    /// With `data`, uploads every mip and layer (`Device::upload_texture`; the texture needs `Sampled | TransferDst`).
    /// If the upload throws, the texture is destroyed and nothing is registered.
    [[nodiscard]] TextureRef create_texture(const rhi::TextureDesc& desc, std::span<const std::byte> data = {});
    /// Samplers are immutable and shared: the same description returns a reference to the same sampler.
    [[nodiscard]] SamplerRef get_sampler(const rhi::SamplerDesc& desc);
    /// Creates an immutable bind group (`Device::create_bind_group`) that retains every resource it binds. The references
    /// must come from this registry. Throws what the device throws; nothing is registered on failure. When the last
    /// reference to the group goes, the group is released first and then its resources, so the device sees the group
    /// destroyed before the textures it bound.
    [[nodiscard]] BindGroupRef create_bind_group(rhi::BindGroupLayoutHandle layout, std::span<const BindGroupBinding> bindings,
                                                 const char* name = "");

    [[nodiscard]] ResourceStats stats(ResourceKind kind) const noexcept;
    [[nodiscard]] ResourceStats total_stats() const noexcept;
    [[nodiscard]] uint32_t live_count() const noexcept { return total_stats().live; }
    [[nodiscard]] std::vector<ResourceInfo> live_resources() const;
    /// Human-readable list of everything still referenced (empty when nothing is). Used for the teardown report.
    [[nodiscard]] std::string leak_report() const;
    /// Where the teardown leak report goes (default: stderr). Called only if resources are still referenced.
    void set_leak_sink(std::function<void(std::string_view)> sink);

    [[nodiscard]] rhi::Device& device() const noexcept { return device_; }

private:
    rhi::Device& device_;
    std::shared_ptr<detail::RegistryState> state_;
};

} // namespace saige::render
