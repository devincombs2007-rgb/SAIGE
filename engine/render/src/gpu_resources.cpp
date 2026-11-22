#include "saige/render/gpu_resources.hpp"

#include <algorithm>
#include <iostream>
#include <sstream>
#include <utility>

namespace saige::render {

const char* to_string(ResourceKind kind) noexcept
{
    switch (kind) {
    case ResourceKind::Buffer: return "Buffer";
    case ResourceKind::Texture: return "Texture";
    case ResourceKind::Sampler: return "Sampler";
    }
    return "Unknown";
}

namespace detail {

struct Entry {
    bool live = false;
    ResourceKind kind = ResourceKind::Buffer;
    uint32_t generation = 1;
    uint32_t refs = 0;
    uint64_t bytes = 0;
    uint32_t raw = 0;  ///< the device handle's raw value; the typed handle is rebuilt from it on destruction
    std::string name;
};

struct SamplerEntry {
    rhi::SamplerDesc desc;
    uint32_t slot = 0;
    uint32_t generation = 0;
};

struct RegistryState {
    rhi::Device* device = nullptr;
    bool closed = false;  ///< the registry is gone: remaining references are inert
    std::vector<Entry> entries;
    std::vector<uint32_t> free_slots;
    ResourceStats stats[resource_kind_count];
    uint64_t peak_total_bytes = 0;  ///< highest sum of live bytes across all kinds
    std::vector<SamplerEntry> samplers;  ///< live samplers, for sharing
    std::function<void(std::string_view)> sink;

    [[nodiscard]] Entry* find(uint32_t slot, uint32_t generation) noexcept
    {
        if (slot >= entries.size()) {
            return nullptr;
        }
        Entry& e = entries[slot];
        return (e.live && e.generation == generation) ? &e : nullptr;
    }

    /// Makes the next `add` (and the later release of what it registers) infallible: every allocation the bookkeeping
    /// needs happens here, before any device resource exists. Throws std::bad_alloc with nothing changed but capacity.
    /// Invariant afterwards: entries and free_slots have room for one more slot, and free_slots can hold every slot that
    /// will ever exist, so `release` never allocates.
    void reserve_slot()
    {
        if (free_slots.empty() && entries.size() == entries.capacity()) {
            entries.reserve(std::max<std::size_t>(16, entries.size() * 2));
        }
        if (free_slots.capacity() < entries.capacity()) {
            free_slots.reserve(entries.capacity());
        }
    }

    /// Registers a device resource. Cannot throw after `reserve_slot` (moves and arithmetic only).
    uint32_t add(ResourceKind kind, uint32_t raw, uint64_t bytes, std::string name, uint32_t& generation_out) noexcept
    {
        uint32_t slot;
        if (!free_slots.empty()) {
            slot = free_slots.back();
            free_slots.pop_back();
        } else {
            slot = static_cast<uint32_t>(entries.size());
            entries.emplace_back();
        }
        Entry& e = entries[slot];
        e.live = true;
        e.kind = kind;
        e.refs = 1;
        e.bytes = bytes;
        e.raw = raw;
        e.name = std::move(name);
        generation_out = e.generation;
        ResourceStats& s = stats[static_cast<std::size_t>(kind)];
        ++s.live;
        ++s.created;
        s.live_bytes += bytes;
        s.peak_bytes = std::max(s.peak_bytes, s.live_bytes);
        uint64_t total_live = 0;
        for (const ResourceStats& k : stats) {
            total_live += k.live_bytes;
        }
        peak_total_bytes = std::max(peak_total_bytes, total_live);
        return slot;
    }

    void destroy_on_device(const Entry& e) noexcept
    {
        try {
            switch (e.kind) {
            case ResourceKind::Buffer: device->destroy(rhi::BufferHandle::from_raw(e.raw)); break;
            case ResourceKind::Texture: device->destroy(rhi::TextureHandle::from_raw(e.raw)); break;
            case ResourceKind::Sampler: device->destroy(rhi::SamplerHandle::from_raw(e.raw)); break;
            }
        } catch (...) {
            // Releasing a reference must never throw; the device reports its own misuse.
        }
    }

    void retain(uint32_t slot, uint32_t generation) noexcept
    {
        if (Entry* e = find(slot, generation)) {
            ++e->refs;
        }
    }

    void release(uint32_t slot, uint32_t generation) noexcept
    {
        Entry* e = find(slot, generation);
        if (e == nullptr || e->refs == 0) {
            return;
        }
        if (--e->refs != 0) {
            return;
        }
        ResourceStats& s = stats[static_cast<std::size_t>(e->kind)];
        --s.live;
        ++s.destroyed;
        s.live_bytes -= e->bytes;
        if (e->kind == ResourceKind::Sampler) {
            samplers.erase(std::remove_if(samplers.begin(), samplers.end(),
                                          [&](const SamplerEntry& se) { return se.slot == slot && se.generation == generation; }),
                           samplers.end());
        }
        if (!closed) {
            destroy_on_device(*e);
        }
        e->live = false;
        e->name.clear();
        e->generation = e->generation == 0xFFFFFFFFu ? 1u : e->generation + 1u;  // stale references no longer match
        // Allocation-free by construction (reserve_slot sized free_slots for every slot). If that invariant were ever
        // broken the slot is simply not reused: releasing a reference must never allocate or throw.
        if (free_slots.size() < free_slots.capacity()) {
            free_slots.push_back(slot);
        }
    }
};

} // namespace detail

// ----------------------------------------------------------------------------------------------------- RefCore
RefCore::RefCore(const RefCore& other) : state(other.state), slot(other.slot), generation(other.generation)
{
    if (state) {
        state->retain(slot, generation);
    }
}

RefCore::RefCore(RefCore&& other) noexcept
    : state(std::move(other.state)), slot(other.slot), generation(other.generation)
{
    other.state = nullptr;
}

RefCore& RefCore::operator=(const RefCore& other)
{
    if (this != &other) {
        RefCore copy(other);  // retain first, so self-assignment through an alias stays safe
        *this = std::move(copy);
    }
    return *this;
}

RefCore& RefCore::operator=(RefCore&& other) noexcept
{
    if (this != &other) {
        reset();
        state = std::move(other.state);
        slot = other.slot;
        generation = other.generation;
        other.state = nullptr;
    }
    return *this;
}

RefCore::~RefCore()
{
    reset();
}

void RefCore::reset() noexcept
{
    if (state) {
        state->release(slot, generation);
        state.reset();
    }
}

uint32_t RefCore::use_count() const noexcept
{
    if (!state) {
        return 0;
    }
    const detail::Entry* e = state->find(slot, generation);
    return e != nullptr ? e->refs : 0u;
}

uint64_t RefCore::bytes() const noexcept
{
    if (!state) {
        return 0;
    }
    const detail::Entry* e = state->find(slot, generation);
    return e != nullptr ? e->bytes : 0u;
}

std::string RefCore::name() const
{
    if (!state) {
        return {};
    }
    const detail::Entry* e = state->find(slot, generation);
    return e != nullptr ? e->name : std::string();
}

// ----------------------------------------------------------------------------------------------------- Registry
GpuResourceRegistry::GpuResourceRegistry(rhi::Device& device) : device_(device), state_(std::make_shared<detail::RegistryState>())
{
    state_->device = &device;
    state_->sink = [](std::string_view message) { std::cerr << message; };
}

GpuResourceRegistry::~GpuResourceRegistry()
{
    const std::string report = leak_report();
    if (!report.empty() && state_->sink) {
        try {
            state_->sink(report);
        } catch (...) {
        }
    }
    // Release what is still referenced so the device does not also report it, then make late references inert.
    for (detail::Entry& e : state_->entries) {
        if (e.live) {
            state_->destroy_on_device(e);
            e.live = false;
            e.refs = 0;
        }
    }
    state_->closed = true;
}

BufferRef GpuResourceRegistry::create_buffer(const rhi::BufferDesc& desc, std::span<const std::byte> initial_data)
{
    // Transactional: everything that can fail for lack of memory happens before the device resource exists, and the
    // registration after it cannot fail, so there is no window in which a device handle exists without an owner.
    std::string name = desc.debug_name != nullptr ? desc.debug_name : "";
    state_->reserve_slot();
    const rhi::BufferHandle handle = device_.create_buffer(desc, initial_data);
    uint32_t generation = 0;
    const uint32_t slot = state_->add(ResourceKind::Buffer, handle.raw(), desc.size, std::move(name), generation);
    return BufferRef(RefCore(state_, slot, generation), handle);
}

TextureRef GpuResourceRegistry::create_texture(const rhi::TextureDesc& desc, std::span<const std::byte> data)
{
    std::string name = desc.debug_name != nullptr ? desc.debug_name : "";
    const uint64_t bytes = rhi::compute_texture_data_size(desc);  // may throw for an invalid description: nothing exists yet
    state_->reserve_slot();
    const rhi::TextureHandle handle = device_.create_texture(desc);
    if (!data.empty()) {
        try {
            device_.upload_texture(handle, data);
        } catch (...) {
            device_.destroy(handle);
            throw;
        }
    }
    uint32_t generation = 0;
    const uint32_t slot = state_->add(ResourceKind::Texture, handle.raw(), bytes, std::move(name), generation);
    return TextureRef(RefCore(state_, slot, generation), handle);
}

SamplerRef GpuResourceRegistry::get_sampler(const rhi::SamplerDesc& desc)
{
    for (const detail::SamplerEntry& se : state_->samplers) {
        if (se.desc == desc && state_->find(se.slot, se.generation) != nullptr) {
            state_->retain(se.slot, se.generation);
            const detail::Entry& e = state_->entries[se.slot];
            return SamplerRef(RefCore(state_, se.slot, se.generation), rhi::SamplerHandle::from_raw(e.raw));
        }
    }
    std::string name = "sampler";
    state_->reserve_slot();
    state_->samplers.reserve(state_->samplers.size() + 1);  // the push_back below cannot allocate
    const rhi::SamplerHandle handle = device_.create_sampler(desc);
    uint32_t generation = 0;
    const uint32_t slot = state_->add(ResourceKind::Sampler, handle.raw(), 0, std::move(name), generation);
    state_->samplers.push_back({desc, slot, generation});
    return SamplerRef(RefCore(state_, slot, generation), handle);
}

ResourceStats GpuResourceRegistry::stats(ResourceKind kind) const noexcept
{
    return state_->stats[static_cast<std::size_t>(kind)];
}

ResourceStats GpuResourceRegistry::total_stats() const noexcept
{
    ResourceStats total;
    for (const ResourceStats& s : state_->stats) {
        total.live += s.live;
        total.live_bytes += s.live_bytes;
        total.created += s.created;
        total.destroyed += s.destroyed;
    }
    total.peak_bytes = state_->peak_total_bytes;  // the peak of the sum, not the sum of the per-kind peaks
    return total;
}

std::vector<ResourceInfo> GpuResourceRegistry::live_resources() const
{
    std::vector<ResourceInfo> out;
    for (const detail::Entry& e : state_->entries) {
        if (e.live) {
            out.push_back({e.kind, e.name, e.bytes, e.refs});
        }
    }
    return out;
}

std::string GpuResourceRegistry::leak_report() const
{
    const std::vector<ResourceInfo> live = live_resources();
    if (live.empty()) {
        return {};
    }
    std::ostringstream out;
    out << "[saige.render] GPU resource leak: " << live.size() << " resource(s) still referenced when the registry was destroyed\n";
    for (const ResourceInfo& r : live) {
        out << "  " << to_string(r.kind) << " '" << (r.name.empty() ? "(unnamed)" : r.name) << "', " << r.bytes << " bytes, "
            << r.references << " reference(s)\n";
    }
    return out.str();
}

void GpuResourceRegistry::set_leak_sink(std::function<void(std::string_view)> sink)
{
    state_->sink = std::move(sink);
}

} // namespace saige::render
