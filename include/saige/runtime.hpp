#pragma once

#include "saige/scene_hierarchy.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace saige {

class IPlatform {
public:
    virtual ~IPlatform() = default;
    virtual std::chrono::steady_clock::time_point now() = 0;
    // Pump pending events. Return false to stop before updating or submitting.
    virtual bool poll_events() = 0;
};

struct CameraView {
    Vec3 position;
    Quaternion rotation; // Local +X forward and +Z up.
    float fov_y_radians{1.04719755F};
    float near_plane{0.1F};
};

class IGame {
public:
    virtual ~IGame() = default;
    virtual void fixed_update(World& world, std::chrono::nanoseconds step) = 0;
    [[nodiscard]] virtual std::optional<CameraView> camera_view() const { return {}; }
};

// The most recently completed simulation tick, and the fraction toward the next.
// All instances belong to one snapshot and are valid only during submit().
struct FrameView {
    std::uint64_t tick;
    double alpha;
    std::span<const RenderInstance> instances;
    std::optional<CameraView> camera{};
};

class IFrameSink {
public:
    virtual ~IFrameSink() = default;
    virtual void submit(const FrameView& frame) = 0;
};

struct RuntimeConfig {
    std::chrono::nanoseconds fixed_step{16'666'667};
    std::uint32_t max_updates_per_frame{8};
};

struct RunStats {
    std::uint64_t frames{};
    std::uint64_t ticks{};
};

struct CpuFrameMetrics {
    std::uint64_t frame;
    std::uint64_t ticks;
    std::size_t instance_count;
    std::chrono::nanoseconds input;
    std::chrono::nanoseconds simulation;
    std::chrono::nanoseconds extraction;
    std::chrono::nanoseconds submission;
    std::chrono::nanoseconds total;
};

class ICpuFrameObserver {
public:
    virtual ~ICpuFrameObserver() = default;
    virtual void record(const CpuFrameMetrics& frame) = 0;
};

// The caller owns the world and adapters. Tick and frame counts start at zero
// for each run. A zero frame budget skips polling, simulation, and submission.
RunStats run(World& world, IPlatform& platform, IGame& game, IFrameSink& sink,
             std::optional<std::uint64_t> frame_budget = std::nullopt,
             RuntimeConfig config = {}, ICpuFrameObserver* observer = nullptr);

} // namespace saige
