#pragma once

// Repeatable frame and subsystem timing for the 60 FPS goal (issue #62).
//
// What this measures: CPU time on the thread that runs saige::run(), in nanoseconds from std::chrono::steady_clock, plus process
// memory where the platform reports it. What it does not measure: GPU execution (use RendererFrameSink::diagnostics() for pass
// timings where the adapter supports timestamps) or anything about real hardware when the renderer is software Vulkan or absent.
// The report therefore carries an adapter class and never states that the 16.67 ms target is met unless the run is explicitly
// marked as qualified physical-GPU target hardware.
//
// Nothing here changes simulation: observers and decorators only read clocks, so fixed-step determinism is unaffected.

#include "saige/runtime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace saige::perf {

// 60 frames per second.
inline constexpr double target_frame_ms = 1000.0 / 60.0;

struct Summary {
    std::size_t count{};
    double mean_ms{};
    double min_ms{};
    double p50_ms{};
    double p95_ms{};
    double p99_ms{};
    double max_ms{};
};

// Nearest-rank percentiles (rank = ceil(count * percent / 100)), the definition saige_scene_trace already uses. An empty input
// yields a zeroed summary with count 0.
[[nodiscard]] Summary summarize(std::span<const std::int64_t> nanoseconds);
// Samples strictly above the threshold.
[[nodiscard]] std::size_t count_over(std::span<const std::int64_t> nanoseconds, std::int64_t threshold_ns) noexcept;

class Series {
public:
    void add(std::chrono::nanoseconds duration) { samples_.push_back(duration.count()); }
    void reserve(std::size_t n) { samples_.reserve(n); }
    [[nodiscard]] std::span<const std::int64_t> samples() const noexcept { return samples_; }
    [[nodiscard]] Summary summary() const { return summarize(samples_); }

private:
    std::vector<std::int64_t> samples_;
};

// Named duration series ("fixed_step", "physics.step", "chunk.stage_next", ...), listed in name order for stable reports.
class Zones {
public:
    class Scope {
    public:
        Scope(Zones& zones, std::string_view name) : zones_(&zones), name_(name), start_(std::chrono::steady_clock::now()) {}
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() { zones_->record(name_, std::chrono::steady_clock::now() - start_); }

    private:
        Zones* zones_;
        std::string name_;
        std::chrono::steady_clock::time_point start_;
    };

    void record(std::string_view name, std::chrono::nanoseconds duration);
    [[nodiscard]] Scope scope(std::string_view name) { return Scope(*this, name); }
    [[nodiscard]] const Series* find(std::string_view name) const;
    [[nodiscard]] const std::map<std::string, Series, std::less<>>& all() const noexcept { return zones_; }

private:
    std::map<std::string, Series, std::less<>> zones_;
};

struct ProcessMemory {
    std::optional<std::uint64_t> resident_bytes;       // current working set
    std::optional<std::uint64_t> peak_resident_bytes;  // high-water mark of the working set
};
// Linux: /proc/self/status (VmRSS, VmHWM). Windows: GetProcessMemoryInfo. Elsewhere both fields are empty.
[[nodiscard]] ProcessMemory sample_process_memory();

// An ICpuFrameObserver that keeps every frame's phase times, the wall-clock interval between consecutive frames (which includes
// any presentation pacing wait, unlike the CPU total) and the process memory high-water mark.
class FrameRecorder final : public ICpuFrameObserver {
public:
    struct Options {
        bool measure_wall_interval = true;      // read steady_clock in record(); turn off for synthetic-metric tests
        std::uint32_t memory_every_frames = 60; // 0 disables sampling
        std::size_t reserve_frames = 0;
    };

    FrameRecorder() : FrameRecorder(Options{}) {}
    explicit FrameRecorder(Options options);

    void record(const CpuFrameMetrics& frame) override;

    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t ticks() const noexcept { return ticks_; }
    [[nodiscard]] std::size_t peak_instances() const noexcept { return peak_instances_; }
    [[nodiscard]] const Series& total() const noexcept { return total_; }
    [[nodiscard]] const Series& input() const noexcept { return input_; }
    [[nodiscard]] const Series& simulation() const noexcept { return simulation_; }
    [[nodiscard]] const Series& extraction() const noexcept { return extraction_; }
    [[nodiscard]] const Series& submission() const noexcept { return submission_; }
    // Per frame: simulation time divided by the number of fixed ticks that frame ran (frames with no ticks are skipped).
    [[nodiscard]] const Series& per_tick_simulation() const noexcept { return per_tick_simulation_; }
    // Wall-clock time between the ends of consecutive frames (the first frame has none).
    [[nodiscard]] const Series& wall_interval() const noexcept { return wall_interval_; }
    [[nodiscard]] const ProcessMemory& memory() const noexcept { return memory_; }
    // Frames that ran no fixed tick (render faster than the simulation rate), and the most ticks any frame ran.
    [[nodiscard]] std::uint64_t frames_without_tick() const noexcept { return frames_without_tick_; }
    [[nodiscard]] std::uint64_t max_ticks_in_frame() const noexcept { return max_ticks_in_frame_; }

private:
    Options options_;
    std::uint64_t frames_{};
    std::uint64_t ticks_{};
    std::uint64_t frames_without_tick_{};
    std::uint64_t max_ticks_in_frame_{};
    std::size_t peak_instances_{};
    Series total_, input_, simulation_, extraction_, submission_, per_tick_simulation_, wall_interval_;
    std::optional<std::chrono::steady_clock::time_point> last_frame_end_;
    ProcessMemory memory_;
};

// Times each fixed_update of the wrapped game into zones under `zone` (default "fixed_step"). Forwards the camera.
class TimedGame final : public IGame {
public:
    TimedGame(IGame& inner, Zones& zones, std::string zone = "fixed_step")
        : inner_(inner), zones_(zones), zone_(std::move(zone)) {}

    void fixed_update(World& world, std::chrono::nanoseconds step) override {
        const auto scope = zones_.scope(zone_);
        inner_.fixed_update(world, step);
    }
    [[nodiscard]] std::optional<CameraView> camera_view() const override { return inner_.camera_view(); }

private:
    IGame& inner_;
    Zones& zones_;
    std::string zone_;
};

// What a report needs to know about the run so its numbers are not misread.
struct ReportContext {
    std::string workload;                // e.g. "character_course_replay"
    std::string build;                   // e.g. the CMake build type
    std::string adapter_class = "none";  // "none" (no renderer), "software", "physical" or "unknown"
    // Set only for a run on the documented target hardware at the documented resolution and preset (issue #62 leaves that
    // choice open). Without it, and for any adapter class other than "physical", the verdict is "not_evaluated".
    bool qualified_target_hardware = false;
    double budget_ms = target_frame_ms;
    std::int64_t fixed_step_ns = 0;
};

// Machine-readable report, schema_version 1, numbers in milliseconds with a classic locale. Zones are included in name order.
[[nodiscard]] std::string to_json(const FrameRecorder& frames, const Zones& zones, const ReportContext& context);

} // namespace saige::perf
