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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
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
    // May throw std::bad_alloc when the series must grow; never throws while size() < capacity().
    void add(std::chrono::nanoseconds duration) { samples_.push_back(duration.count()); }
    void reserve(std::size_t n) { samples_.reserve(n); }
    // Makes room for one more sample with geometric growth (so repeated calls stay cheap); throws std::bad_alloc if it cannot.
    void ensure_room_for_one() {
        if (samples_.size() == samples_.capacity()) samples_.reserve(std::max<std::size_t>(64, samples_.capacity() * 2));
    }
    [[nodiscard]] std::size_t size() const noexcept { return samples_.size(); }
    [[nodiscard]] std::size_t capacity() const noexcept { return samples_.capacity(); }
    [[nodiscard]] std::span<const std::int64_t> samples() const noexcept { return samples_; }
    [[nodiscard]] Summary summary() const { return summarize(samples_); }

private:
    std::vector<std::int64_t> samples_;
};

// Named duration series ("fixed_step", "physics.step", "chunk.stage_next", ...), listed in name order for stable reports.
//
// Exception safety: a Scope resolves its series when it is created, which may allocate and may throw, before anything is timed.
// Its destructor never throws and never allocates when the series has spare capacity (see reserve()): if growing the series
// fails, the sample is dropped and counted in dropped_samples() rather than terminating the process, which matters most when the
// scope is unwinding because the timed code threw. Reserve every zone used in a fixed-step loop up front and recording is
// allocation-free there.
class Zones {
public:
    Zones() = default;
    // Scopes hold pointers into the zone table, so a Zones object must stay where it is while any Scope exists. Copying, moving and
    // assigning are therefore not allowed at all (a move or an assignment would leave live scopes pointing at freed or replaced series).
    Zones(const Zones&) = delete;
    Zones& operator=(const Zones&) = delete;
    Zones(Zones&&) = delete;
    Zones& operator=(Zones&&) = delete;
    ~Zones() = default;

    class Scope {
    public:
        // Throws (std::bad_alloc) only here, before the timed region starts.
        Scope(Zones& zones, std::string_view name) : zones_(&zones), series_(&zones.series(name)) {
            start_ = std::chrono::steady_clock::now();
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        ~Scope() noexcept {
            const auto elapsed = std::chrono::steady_clock::now() - start_;
            try {
                series_->add(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
            } catch (...) {
                zones_->note_dropped();
            }
        }

    private:
        Zones* zones_;
        Series* series_;
        std::chrono::steady_clock::time_point start_;
    };

    // Strong guarantee: if it throws, the zone's samples are unchanged.
    void record(std::string_view name, std::chrono::nanoseconds duration) { series(name).add(duration); }
    // Creates the zone if needed and makes room for `samples` more records so that scopes on it cannot allocate.
    void reserve(std::string_view name, std::size_t samples);
    [[nodiscard]] Scope scope(std::string_view name) { return Scope(*this, name); }
    [[nodiscard]] const Series* find(std::string_view name) const;
    [[nodiscard]] const std::map<std::string, Series, std::less<>>& all() const noexcept { return zones_; }
    // Samples a Scope had to discard because its series could not grow. Zero in a healthy run; reports include it.
    [[nodiscard]] std::uint64_t dropped_samples() const noexcept { return dropped_; }

private:
    Series& series(std::string_view name);
    void note_dropped() noexcept { ++dropped_; }

    std::map<std::string, Series, std::less<>> zones_;
    std::uint64_t dropped_{};
};

struct ProcessMemory {
    std::optional<std::uint64_t> resident_bytes;       // current working set
    std::optional<std::uint64_t> peak_resident_bytes;  // high-water mark of the working set
};
// Linux: /proc/self/status (VmRSS, VmHWM). Windows: GetProcessMemoryInfo. Elsewhere both fields are empty.
[[nodiscard]] ProcessMemory sample_process_memory();

// An ICpuFrameObserver that keeps every frame's phase times, the observer's wall-clock interval between consecutive frames, the
// presentation intervals a host reports, and the process memory high-water mark.
//
// The two interval series are deliberately separate. The observer interval is what this process can see by itself (the time between
// the ends of two record() calls): it includes any pacing wait the loop performs, but it is not evidence of what the display showed.
// A presentation interval is supplied by the host from real presentation timestamps (add_presentation_interval). Only the latter
// can support a verdict; a run that claims to be paced but never recorded one cannot be qualified.
class FrameRecorder final : public ICpuFrameObserver {
public:
    struct Options {
        bool measure_wall_interval = true;      // read steady_clock in record(); turn off for synthetic-metric tests
        std::uint32_t memory_every_frames = 60; // 0 disables sampling
        std::size_t reserve_frames = 0;
    };

    FrameRecorder() : FrameRecorder(Options{}) {}
    explicit FrameRecorder(Options options);

    // All-or-nothing: room is made in every series first, so if that throws (std::bad_alloc) no counter or series has changed;
    // after it, recording cannot fail.
    void record(const CpuFrameMetrics& frame) override;

    // Records one delivered frame interval measured by the host from presentation timestamps. These, and only these, feed the
    // verdict. A delivered interval is positive by definition: zero or negative (a clock that did not advance, or timestamps out
    // of order) is rejected with std::invalid_argument before anything is recorded, so it can never contribute to a verdict.
    // Strong guarantee. May throw std::bad_alloc; reserve with Options::reserve_frames to avoid it in a frame loop.
    void add_presentation_interval(std::chrono::nanoseconds interval)
    {
        if (interval <= std::chrono::nanoseconds::zero()) throw std::invalid_argument("a presentation interval must be positive");
        presentation_interval_.add(interval);
    }

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
    // The observer's wall-clock time between the ends of consecutive frames (the first frame has none). Never used for a verdict.
    [[nodiscard]] const Series& wall_interval() const noexcept { return wall_interval_; }
    // Presentation intervals supplied by the host through add_presentation_interval().
    [[nodiscard]] const Series& presentation_interval() const noexcept { return presentation_interval_; }
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
    Series total_, input_, simulation_, extraction_, submission_, per_tick_simulation_, wall_interval_, presentation_interval_;
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
//
// The goal recorded on issue #62 is a p99 *delivered frame interval* of at most 16.67 ms on the provisional baseline: Windows 11,
// Ryzen 5 5600, RTX 3060 12 GB, 16 GB RAM, SSD, native 1920x1080, High preset, ray tracing, upscaling and frame generation off,
// on a documented repeatable course. A verdict therefore needs ALL of: a physical adapter, a paced (presented) run, the
// qualified_target_hardware flag, and a named preset and course version. Anything less is "not_evaluated".
struct ReportContext {
    std::string workload;                // e.g. "character_course_replay"
    std::string build;                   // e.g. the CMake build type
    std::string adapter_class = "none";  // "none" (no renderer), "software", "physical" or "unknown"
    // True when frames were presented at the display's pace, so the interval between frames is what a player sees. An unpaced loop
    // (as the headless course benchmark is) produces intervals that say nothing about delivered frame pacing.
    bool paced = false;
    // Set only for a run on the documented target hardware at the documented resolution and preset.
    bool qualified_target_hardware = false;
    std::string hardware;       // free text describing the machine and display settings the run actually used
    std::string preset_version; // versioned graphics preset name; empty while no preset is versioned
    std::string course_version; // versioned scripted course fixture; empty while none is versioned
    // Must be finite, positive and at most max_budget_ms; to_json throws std::invalid_argument otherwise (a NaN or a huge value cannot be
    // converted to the integer nanoseconds the counters compare against).
    double budget_ms = target_frame_ms;
    std::int64_t fixed_step_ns = 0;
};

inline constexpr double max_budget_ms = 1.0e6;  // 1000 s: far beyond any frame budget, small enough that 2x in nanoseconds fits in int64

// Machine-readable report, schema_version 1, numbers in milliseconds with a classic locale. Zones are included in name order.
[[nodiscard]] std::string to_json(const FrameRecorder& frames, const Zones& zones, const ReportContext& context);

} // namespace saige::perf
