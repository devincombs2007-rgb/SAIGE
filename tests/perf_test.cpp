#include "saige/perf.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <type_traits>
#include <new>
#include <functional>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

// Allocation fault injection for the exception-safety tests: the n-th allocation after fail_allocation_after(n) throws
// std::bad_alloc, and every allocation is counted. Process-wide, which is fine for a single-threaded test executable. Not built on
// MSVC, where replacing the global operator new in a test executable interacts with the static runtime; those tests are skipped.
#if !defined(_MSC_VER)
namespace {
std::atomic<unsigned> g_fail_countdown{0};
std::atomic<unsigned long long> g_allocations{0};
} // namespace

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    unsigned c = g_fail_countdown.load(std::memory_order_relaxed);
    if (c != 0 && g_fail_countdown.fetch_sub(1, std::memory_order_relaxed) == 1) throw std::bad_alloc();
    if (void* p = std::malloc(size != 0 ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
constexpr bool fault_injection_available = true;
void fail_allocation_after(unsigned n) { g_fail_countdown.store(n); }
unsigned long long allocations_seen() { return g_allocations.load(); }
#else
constexpr bool fault_injection_available = false;
void fail_allocation_after(unsigned) {}
unsigned long long allocations_seen() { return 0; }
#endif

namespace {
using namespace std::chrono_literals;
using saige::perf::FrameRecorder;
using saige::perf::ReportContext;
using saige::perf::Zones;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance = 1e-9) {
    return std::abs(a - b) <= tolerance;
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

std::vector<std::int64_t> milliseconds(int first, int last) {
    std::vector<std::int64_t> v;
    for (int i = first; i <= last; ++i) v.push_back(static_cast<std::int64_t>(i) * 1'000'000);
    return v;
}

void test_summary_percentiles() {
    auto samples = milliseconds(1, 100);
    std::shuffle(samples.begin(), samples.end(), std::mt19937(7));  // order must not matter
    const auto s = saige::perf::summarize(samples);
    require(s.count == 100 && near(s.min_ms, 1) && near(s.max_ms, 100), "count, min or max");
    require(near(s.mean_ms, 50.5), "mean");
    require(near(s.p50_ms, 50) && near(s.p95_ms, 95) && near(s.p99_ms, 99), "nearest-rank percentiles of 1..100");

    // Rank rounds up: 20 samples put p95 at the 19th and p99 at the 20th, 7 samples put p50 at the 4th.
    const auto twenty = saige::perf::summarize(milliseconds(1, 20));
    require(near(twenty.p95_ms, 19) && near(twenty.p99_ms, 20) && near(twenty.p50_ms, 10), "ranks for 20 samples");
    require(near(saige::perf::summarize(milliseconds(1, 7)).p50_ms, 4), "rank rounds up for 7 samples");

    const auto one = saige::perf::summarize(milliseconds(5, 5));
    require(one.count == 1 && near(one.p50_ms, 5) && near(one.p99_ms, 5) && near(one.mean_ms, 5), "single sample");
    const auto none = saige::perf::summarize({});
    require(none.count == 0 && near(none.mean_ms, 0) && near(none.p99_ms, 0), "empty input is zeroed, not an error");

    // A single outlier dominates p99 and max but not p50: the reason percentiles are reported at all.
    auto spike = milliseconds(16, 16);
    spike.assign(99, 16'000'000);
    spike.push_back(120'000'000);
    const auto spiked = saige::perf::summarize(spike);
    require(near(spiked.p50_ms, 16) && near(spiked.p95_ms, 16) && near(spiked.p99_ms, 16) && near(spiked.max_ms, 120),
            "one spike in 100 frames is the maximum and not the 99th percentile");
    spike.push_back(120'000'000);
    require(near(saige::perf::summarize(spike).p99_ms, 120), "two spikes in 101 frames reach p99");
}

void test_count_over_is_strict() {
    const std::vector<std::int64_t> v{1, 2, 3, 3, 4};
    require(saige::perf::count_over(v, 3) == 1 && saige::perf::count_over(v, 0) == 5 && saige::perf::count_over(v, 4) == 0,
            "only samples strictly above the threshold count");
}

void test_zones() {
    Zones zones;
    zones.record("physics.step", 2ms);
    zones.record("chunk.stage_next", 40ms);
    zones.record("physics.step", 4ms);
    require(zones.find("missing") == nullptr, "unknown zone");
    const auto* physics = zones.find("physics.step");
    require(physics && physics->summary().count == 2 && near(physics->summary().mean_ms, 3), "zone samples accumulate");
    require(zones.all().begin()->first == "chunk.stage_next", "zones iterate in name order");
    {
        const auto scope = zones.scope("scoped");
    }
    const auto* scoped = zones.find("scoped");
    require(scoped && scoped->summary().count == 1 && scoped->samples()[0] >= 0, "a scope records exactly one non-negative sample");
}

saige::CpuFrameMetrics frame(std::uint64_t index, std::uint64_t ticks, std::chrono::nanoseconds simulation,
                             std::chrono::nanoseconds total, std::size_t instances = 1) {
    return {index, ticks, instances, 100us, simulation, 200us, 300us, total};
}

FrameRecorder synthetic_recorder() {
    return FrameRecorder({.measure_wall_interval = false, .memory_every_frames = 0});
}

void test_frame_recorder_derived_series() {
    auto recorder = synthetic_recorder();
    recorder.record(frame(1, 0, 10us, 4ms, 3));
    recorder.record(frame(2, 2, 6ms, 8ms, 7));
    recorder.record(frame(3, 3, 9ms, 12ms, 5));
    require(recorder.frames() == 3 && recorder.ticks() == 5, "frame and tick counts");
    require(recorder.frames_without_tick() == 1 && recorder.max_ticks_in_frame() == 3, "tickless frames and the catch-up peak");
    require(recorder.peak_instances() == 7, "peak instance count");
    const auto per_tick = recorder.per_tick_simulation().samples();
    require(per_tick.size() == 2 && per_tick[0] == 3'000'000 && per_tick[1] == 3'000'000, "simulation time is divided by ticks run");
    require(recorder.wall_interval().samples().empty(), "wall interval off for synthetic metrics");
    require(!recorder.memory().resident_bytes && !recorder.memory().peak_resident_bytes, "memory sampling off");
    require(near(recorder.total().summary().mean_ms, 8), "total series");
}

struct ScriptedPlatform final : saige::IPlatform {
    std::size_t frames_left;
    std::chrono::nanoseconds delta;
    std::chrono::steady_clock::time_point time{};
    explicit ScriptedPlatform(std::size_t frames, std::chrono::nanoseconds d) : frames_left(frames), delta(d) {}
    std::chrono::steady_clock::time_point now() override { return time; }
    bool poll_events() override {
        if (frames_left == 0) return false;
        --frames_left;
        time += delta;
        return true;
    }
};

struct CountingGame final : saige::IGame {
    std::uint64_t updates = 0;
    saige::Entity marker{};
    void fixed_update(saige::World& world, std::chrono::nanoseconds) override {
        if (!world.alive(marker)) marker = world.create();
        ++updates;
        world.set_position(marker, {static_cast<float>(updates), 0.0f, 0.0f});
    }
};

struct NullSink final : saige::IFrameSink {
    saige::Vec3 last{};
    void submit(const saige::FrameView& frame) override {
        if (!frame.instances.empty()) last = frame.instances.front().position;
    }
};

void test_recording_a_real_run_does_not_change_it() {
    constexpr saige::RuntimeConfig config{10ms, 8};
    // Plain run.
    saige::World plain_world;
    CountingGame plain_game;
    NullSink plain_sink;
    ScriptedPlatform plain_platform(60, 10ms);
    const auto plain = saige::run(plain_world, plain_platform, plain_game, plain_sink, std::nullopt, config);

    // The same run with the recorder and the timing decorator attached.
    saige::World timed_world;
    CountingGame timed_inner;
    Zones zones;
    saige::perf::TimedGame timed_game(timed_inner, zones);
    NullSink timed_sink;
    FrameRecorder recorder;
    ScriptedPlatform timed_platform(60, 10ms);
    const auto timed = saige::run(timed_world, timed_platform, timed_game, timed_sink, std::nullopt, config, &recorder);

    require(plain.frames == timed.frames && plain.ticks == timed.ticks, "instrumentation changed the tick or frame count");
    require(plain_game.updates == timed_inner.updates && plain_sink.last == timed_sink.last, "instrumentation changed the simulation");
    require(recorder.frames() == timed.frames && recorder.ticks() == timed.ticks, "the recorder saw every frame and tick");
    const auto* step = zones.find("fixed_step");
    require(step && step->summary().count == timed.ticks, "one fixed_step sample per fixed tick");
    require(recorder.wall_interval().samples().size() == timed.frames - 1, "a wall interval between each pair of frames");
    require(recorder.total().summary().count == timed.frames, "a total sample per frame");
}

ReportContext make_context(std::string workload, std::string build, std::string adapter_class) {
    ReportContext context;
    context.workload = std::move(workload);
    context.build = std::move(build);
    context.adapter_class = std::move(adapter_class);
    return context;
}

FrameRecorder recorder_with_intervals(std::chrono::nanoseconds interval, int count, int slow = 0,
                                      std::chrono::nanoseconds slow_interval = 40ms) {
    auto recorder = synthetic_recorder();
    for (int i = 1; i <= count; ++i) {
        recorder.record(frame(static_cast<std::uint64_t>(i), 1, 1ms, 3ms));
        recorder.add_presentation_interval(i <= slow ? slow_interval : interval);
    }
    return recorder;
}

ReportContext qualified_context() {
    ReportContext context = make_context("course", "Release", "physical");
    context.paced = true;
    context.qualified_target_hardware = true;
    context.hardware = "Windows 11, Ryzen 5 5600, RTX 3060 12GB, 16GB, 1080p High";
    context.preset_version = "high-1";
    context.course_version = "course-1";
    return context;
}

bool verdict_is(const std::string& json, const char* verdict) {
    return contains(json, std::string("\"verdict\":\"") + verdict + "\"");
}

void test_report_never_claims_the_target_without_a_qualified_paced_run() {
    const auto smooth = recorder_with_intervals(16ms, 200);  // 200 intervals of 16 ms: comfortably within the goal
    Zones zones;
    // The qualified baseline is the only configuration that can produce a verdict.
    require(verdict_is(saige::perf::to_json(smooth, zones, qualified_context()), "within_budget"), "qualified smooth run is within budget");

    // Removing any single qualification turns the verdict back into not_evaluated, with the reason named.
    const auto not_evaluated = [&](const char* what, const std::function<void(ReportContext&)>& change, const char* reason) {
        ReportContext context = qualified_context();
        change(context);
        const auto json = saige::perf::to_json(smooth, zones, context);
        require(verdict_is(json, "not_evaluated"), std::string("a verdict was given with ") + what);
        require(contains(json, reason), std::string("the reason for not_evaluated should mention: ") + reason);
    };
    not_evaluated("a software adapter", [](ReportContext& c) { c.adapter_class = "software"; }, "does not measure real GPU hardware");
    not_evaluated("no renderer", [](ReportContext& c) { c.adapter_class = "none"; }, "does not measure real GPU hardware");
    not_evaluated("an unknown adapter", [](ReportContext& c) { c.adapter_class = "unknown"; }, "does not measure real GPU hardware");
    not_evaluated("an unpaced run", [](ReportContext& c) { c.paced = false; }, "not paced by presentation");
    not_evaluated("an unqualified machine", [](ReportContext& c) { c.qualified_target_hardware = false; }, "not marked as the documented target");
    not_evaluated("no versioned preset", [](ReportContext& c) { c.preset_version.clear(); }, "not both versioned");
    not_evaluated("no versioned course", [](ReportContext& c) { c.course_version.clear(); }, "not both versioned");

    // No intervals at all (a single frame) cannot be judged.
    auto single = synthetic_recorder();
    single.record(frame(1, 1, 1ms, 3ms));
    require(verdict_is(saige::perf::to_json(single, zones, qualified_context()), "not_evaluated"), "no intervals, no verdict");

    // The statistic is the delivered interval, not CPU time: a run whose CPU frames are tiny but whose intervals are long is over.
    const auto slow_delivery = recorder_with_intervals(33ms, 200);
    require(verdict_is(saige::perf::to_json(slow_delivery, zones, qualified_context()), "over_budget"), "slow delivered intervals are over budget");
    // And CPU frames over budget do not matter if the delivered intervals are on time.
    auto cpu_heavy = synthetic_recorder();
    for (std::uint64_t i = 1; i <= 100; ++i) {
        cpu_heavy.record(frame(i, 1, 1ms, 30ms));
        cpu_heavy.add_presentation_interval(16ms);
    }
    require(verdict_is(saige::perf::to_json(cpu_heavy, zones, qualified_context()), "within_budget"), "the verdict follows the interval, not the CPU time");

    // p99: one slow interval in 100 is the maximum, two reach p99.
    require(verdict_is(saige::perf::to_json(recorder_with_intervals(16ms, 100, 1), zones, qualified_context()), "within_budget"),
            "one slow interval in 100 does not move p99");
    require(verdict_is(saige::perf::to_json(recorder_with_intervals(16ms, 101, 2), zones, qualified_context()), "over_budget"),
            "two slow intervals in 101 reach p99");
    // The budget is inclusive: an interval exactly at the budget is within it.
    ReportContext exact = qualified_context();
    exact.budget_ms = 16.0;
    require(verdict_is(saige::perf::to_json(recorder_with_intervals(16ms, 50), zones, exact), "within_budget"), "an interval equal to the budget is within it");
}

void test_a_paced_run_needs_measured_presentation_intervals() {
    // Core review of #68: paced = true with the default observer timing used to be enough for a qualified verdict, although no
    // presentation interval was ever measured. The observer's own wall-clock intervals are kept in a different series and never count.
    FrameRecorder observed({.measure_wall_interval = true, .memory_every_frames = 0});
    for (std::uint64_t i = 1; i <= 50; ++i) observed.record(frame(i, 1, 1ms, 3ms));
    require(observed.wall_interval().size() == 49 && observed.presentation_interval().size() == 0, "observer intervals and presentation intervals are separate series");
    Zones zones;
    const auto json = saige::perf::to_json(observed, zones, qualified_context());
    require(verdict_is(json, "not_evaluated"), "a paced, qualified run with no presentation interval has no verdict");
    require(contains(json, "no presentation intervals were recorded"), "and the reason says so");
    require(contains(json, "\"observer_interval_ms\":{\"count\":49") && contains(json, "\"presentation_interval_ms\":{\"count\":0"),
            "the report lists both series with their own counts");
    // Adding the host's presentation intervals is the only thing that changes it.
    for (int i = 0; i < 50; ++i) observed.add_presentation_interval(16ms);
    require(verdict_is(saige::perf::to_json(observed, zones, qualified_context()), "within_budget"), "with presentation intervals the verdict is evaluated");
    // The verdict follows the presentation series: slow presentation over fast observer intervals is over budget.
    FrameRecorder slow_present({.measure_wall_interval = true, .memory_every_frames = 0});
    for (std::uint64_t i = 1; i <= 50; ++i) {
        slow_present.record(frame(i, 1, 1ms, 3ms));
        slow_present.add_presentation_interval(40ms);
    }
    require(verdict_is(saige::perf::to_json(slow_present, zones, qualified_context()), "over_budget"), "the verdict uses presentation intervals, not the observer's");
}

void test_non_positive_presentation_intervals_are_rejected() {
    // Core re-review of #68: one add_presentation_interval(-1ns) in an otherwise qualified run produced verdict "within_budget".
    Zones zones;
    const auto rejects = [](FrameRecorder& recorder, std::chrono::nanoseconds interval) {
        try {
            recorder.add_presentation_interval(interval);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    // An empty series stays empty after rejected samples, so the qualified run is still not evaluated.
    FrameRecorder empty({.measure_wall_interval = false, .memory_every_frames = 0});
    for (std::uint64_t i = 1; i <= 20; ++i) empty.record(frame(i, 1, 1ms, 3ms));
    for (const std::chrono::nanoseconds bad : {-1ns, 0ns, std::chrono::nanoseconds(-16ms), std::chrono::nanoseconds::min()}) {
        require(rejects(empty, bad), "an interval of " + std::to_string(bad.count()) + " ns is rejected");
    }
    require(empty.presentation_interval().size() == 0, "rejected samples record nothing");
    const auto json = saige::perf::to_json(empty, zones, qualified_context());
    require(verdict_is(json, "not_evaluated") && contains(json, "no presentation intervals were recorded"), "a qualified run with only rejected samples is not evaluated");

    // A rejected sample does not disturb a series that already has samples, and the smallest positive interval is accepted.
    auto recorder = recorder_with_intervals(16ms, 50);
    const auto before = recorder.presentation_interval().size();
    require(rejects(recorder, -1ns) && rejects(recorder, 0ns), "rejected after valid samples");
    require(recorder.presentation_interval().size() == before, "the series is unchanged by a rejected sample");
    require(verdict_is(saige::perf::to_json(recorder, zones, qualified_context()), "within_budget"), "valid samples still qualify");
    recorder.add_presentation_interval(1ns);
    require(recorder.presentation_interval().size() == before + 1, "one nanosecond is a valid interval");
}

void test_budget_is_validated_before_it_is_converted() {
    // Core review of #68: a NaN budget was cast to int64 (undefined behavior) and doubled.
    const auto recorder = recorder_with_intervals(16ms, 20);
    Zones zones;
    const auto rejected = [&](double budget) {
        ReportContext context = qualified_context();
        context.budget_ms = budget;
        try {
            (void)saige::perf::to_json(recorder, zones, context);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    for (const double bad : {nan, inf, -inf, 0.0, -0.0, -1.0, -16.67, 1e300, saige::perf::max_budget_ms * 1.0001, 1e19}) {
        require(rejected(bad), "budget_ms = " + std::to_string(bad) + " is rejected");
    }
    for (const double ok : {0.001, 1.0, 16.6667, 33.3333, 1000.0, saige::perf::max_budget_ms}) {
        require(!rejected(ok), "budget_ms = " + std::to_string(ok) + " is accepted");
    }
}

void test_zones_cannot_move_under_a_scope() {
    // A Scope keeps a pointer into its Zones, so copying, moving or assigning a Zones would leave live scopes dangling.
    static_assert(!std::is_copy_constructible_v<Zones> && !std::is_copy_assignable_v<Zones>, "Zones must not be copyable");
    static_assert(!std::is_move_constructible_v<Zones> && !std::is_move_assignable_v<Zones>, "Zones must not be movable");
    Zones zones;
    zones.reserve("a", 4);
    {
        const auto scope = zones.scope("a");
        (void)scope;
    }
    require(zones.find("a") != nullptr && zones.find("a")->size() == 1, "a scope records into its zone");
}

void test_report_contents() {
    auto recorder = synthetic_recorder();
    for (std::uint64_t i = 1; i <= 10; ++i) {
        recorder.record(frame(i, 1, 2ms, i <= 8 ? 10ms : 20ms));
        recorder.add_presentation_interval(i <= 8 ? 10ms : 20ms);
    }
    Zones zones;
    zones.record("chunk.stage_next", 40ms);
    zones.record("physics.step", 1ms);
    ReportContext context = make_context("quote\"and\\slash", "Debug", "none");
    context.fixed_step_ns = 16'666'667;
    const auto json = saige::perf::to_json(recorder, zones, context);
    require(contains(json, "\"schema_version\":1") && contains(json, "\"frames\":10") && contains(json, "\"ticks\":10"), "counts");
    require(contains(json, "\"workload\":\"quote\\\"and\\\\slash\""), "strings are escaped");
    require(contains(json, "\"fixed_step_ns\":16666667"), "fixed step");
    // 8 frames of 10 ms and 2 of 20 ms average 12 ms, which is 83.3333 FPS; only the two 20 ms intervals exceed 16.67 ms.
    require(contains(json, "\"cpu_average_fps\":83.3333") && contains(json, "\"presentation_average_fps\":83.3333"), "average fps follows the mean");
    require(contains(json, "\"intervals_over_budget\":2") && contains(json, "\"intervals_over_2x_budget\":0") &&
                contains(json, "\"cpu_frames_over_budget\":2"), "budget counters");
    require(contains(json, "\"statistic\":\"p99_presentation_interval\"") && contains(json, "\"paced\":false") && contains(json, "\"dropped_timing_samples\":0"),
            "the statistic, the pacing and the dropped-sample count are reported");
    require(contains(json, "\"chunk.stage_next\":{\"count\":1,\"mean_ms\":40.0000"), "zones are listed with their statistics");
    require(json.find("chunk.stage_next") < json.find("physics.step"), "zones appear in name order");
}

// ---------------------------------------------------------------------------------------------------------------------------
// Exception safety of timing scopes and recording (owner request on #43: non-throwing scope teardown and allocations).

// A plain exception: std::runtime_error allocates its message, which would put the injected failure on the wrong allocation.
struct GameError {};
struct ThrowingGame final : saige::IGame {
    void fixed_update(saige::World&, std::chrono::nanoseconds) override { throw GameError{}; }
};

void test_scope_teardown_never_throws_and_is_allocation_free_when_reserved() {
    if (!fault_injection_available) return;
    Zones zones;
    zones.reserve("hot", 10);
    // With the zone reserved, a scope neither allocates nor can fail.
    const auto before = allocations_seen();
    {
        for (int i = 0; i < 10; ++i) {
            const auto scope = zones.scope("hot");
        }
    }
    const auto after = allocations_seen();  // read before require() builds its message string, which allocates
    require(after == before, "scopes on a reserved zone must not allocate");
    require(zones.find("hot")->summary().count == 10 && zones.dropped_samples() == 0, "all ten samples were recorded");

    // Growth fails: the destructor swallows it, counts the drop and nothing terminates.
    Zones tight;
    tight.record("grow", 1ms);  // capacity 1, size 1: the next record must allocate
    {
        const auto scope = tight.scope("grow");
        fail_allocation_after(1);
    }  // ~Scope tries to grow the series, the allocation throws, the sample is dropped
    fail_allocation_after(0);
    require(tight.dropped_samples() == 1 && tight.find("grow")->summary().count == 1, "a failed growth drops one sample and keeps the rest");
    // The zone still works afterwards.
    { const auto scope = tight.scope("grow"); }
    require(tight.find("grow")->summary().count == 2 && tight.dropped_samples() == 1, "recording resumes after a dropped sample");

    // Creating a scope on a new zone allocates in the constructor, where throwing is safe: nothing is timed or recorded.
    Zones fresh;
    fail_allocation_after(1);
    bool threw = false;
    try {
        const auto scope = fresh.scope("new zone");
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    fail_allocation_after(0);
    require(threw && fresh.all().empty() && fresh.dropped_samples() == 0, "a failed scope construction leaves no trace");

    // A scope unwinding because the timed code threw still records, and a failure to record while unwinding does not terminate.
    Zones unwinding;
    unwinding.reserve("fixed_step", 4);
    ThrowingGame game;
    saige::perf::TimedGame timed(game, unwinding);
    saige::World world;
    bool propagated = false;
    try {
        timed.fixed_update(world, 10ms);
    } catch (const GameError&) {
        propagated = true;
    }
    require(propagated && unwinding.find("fixed_step")->summary().count == 1, "the exception propagates and the tick was still timed");
    Zones unwinding_tight;
    unwinding_tight.record("fixed_step", 1ms);
    saige::perf::TimedGame timed_tight(game, unwinding_tight);
    propagated = false;
    try {
        fail_allocation_after(1);  // the first allocation is the scope's own series growth during unwinding
        timed_tight.fixed_update(world, 10ms);
    } catch (const GameError&) {
        propagated = true;
    }
    fail_allocation_after(0);
    require(propagated && unwinding_tight.dropped_samples() == 1, "the game's exception survives a failed sample during unwinding");
}

void test_recording_is_all_or_nothing() {
    if (!fault_injection_available) return;
    // Fail each allocation in turn that record() could make; whichever fails, the recorder must be unchanged and still usable.
    for (unsigned nth = 1; nth <= 8; ++nth) {
        FrameRecorder recorder({.measure_wall_interval = false, .memory_every_frames = 0});
        recorder.record(frame(1, 1, 2ms, 4ms, 3));  // sizes every series to a full capacity of 64
        for (int i = 2; i <= 64; ++i) recorder.record(frame(static_cast<std::uint64_t>(i), 1, 2ms, 4ms, 3));  // exactly at capacity
        const auto frames = recorder.frames();
        const auto ticks = recorder.ticks();
        const auto totals = recorder.total().size();
        fail_allocation_after(nth);
        bool threw = false;
        try {
            recorder.record(frame(65, 1, 2ms, 4ms, 9));  // must grow every series
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        fail_allocation_after(0);
        if (threw) {
            require(recorder.frames() == frames && recorder.ticks() == ticks && recorder.total().size() == totals &&
                        recorder.simulation().size() == totals && recorder.per_tick_simulation().size() == totals &&
                        recorder.peak_instances() == 3,
                    "a failed record() changed the recorder");
        }
        recorder.record(frame(66, 1, 2ms, 4ms, 5));  // and it keeps working
        require(recorder.frames() == frames + 1 + (threw ? 0u : 1u), "recording continues after a failure");
        require(recorder.total().size() == recorder.frames(), "every series stays in step with the frame count");
    }

    // Delivered intervals added by the host have the same guarantee.
    FrameRecorder recorder({.measure_wall_interval = false, .memory_every_frames = 0});
    for (int i = 0; i < 64; ++i) recorder.add_presentation_interval(16ms);
    const auto count = recorder.presentation_interval().size();
    fail_allocation_after(1);
    bool threw = false;
    try {
        recorder.add_presentation_interval(16ms);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    fail_allocation_after(0);
    require(threw && recorder.presentation_interval().size() == count, "a failed add_presentation_interval changed nothing");

    // Sampling process memory is best effort: failing it must not fail or alter a recorded frame.
    FrameRecorder sampling({.measure_wall_interval = false, .memory_every_frames = 1});
    sampling.record(frame(1, 1, 1ms, 2ms));  // sizes series; its own memory sample succeeds
    for (unsigned nth = 1; nth <= 6; ++nth) {
        fail_allocation_after(nth);
        bool failed = false;
        try {
            sampling.record(frame(2 + nth, 1, 1ms, 2ms));
        } catch (const std::bad_alloc&) {
            failed = true;
        }
        fail_allocation_after(0);
        (void)failed;
        require(sampling.total().size() == sampling.frames(), "series and counters agree after any single allocation failure");
    }
}

void test_process_memory() {
    const auto memory = saige::perf::sample_process_memory();
#if defined(__linux__) || defined(_WIN32)
    require(memory.resident_bytes && *memory.resident_bytes > 0, "resident memory is reported");
    require(memory.peak_resident_bytes && *memory.peak_resident_bytes >= *memory.resident_bytes, "the peak is at least the current size");
    std::vector<char> block(32u << 20, 1);  // touch 32 MiB so the working set visibly grows
    volatile char sink = 0;
    for (std::size_t i = 0; i < block.size(); i += 4096) sink = static_cast<char>(sink + block[i]);
    const auto after = saige::perf::sample_process_memory();
    require(*after.peak_resident_bytes >= *memory.peak_resident_bytes, "the high-water mark never decreases");
    (void)sink;
#else
    (void)memory;
#endif
}

} // namespace

int main() {
    try {
        test_summary_percentiles();
        test_count_over_is_strict();
        test_zones();
        test_frame_recorder_derived_series();
        test_recording_a_real_run_does_not_change_it();
        test_report_never_claims_the_target_without_a_qualified_paced_run();
        test_report_contents();
        test_a_paced_run_needs_measured_presentation_intervals();
        test_budget_is_validated_before_it_is_converted();
        test_non_positive_presentation_intervals_are_rejected();
        test_zones_cannot_move_under_a_scope();
        test_scope_teardown_never_throws_and_is_allocation_free_when_reserved();
        test_recording_is_all_or_nothing();
        test_process_memory();
    } catch (const std::exception& error) {
        std::cerr << "perf test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "perf tests passed\n";
}
