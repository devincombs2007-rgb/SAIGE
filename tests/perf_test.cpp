#include "saige/perf.hpp"

#include <algorithm>
#include <iostream>
#include <numeric>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace std::chrono_literals;
using saige::perf::FrameRecorder;
using saige::perf::ReportContext;
using saige::perf::Zones;

void require(bool condition, const char* message) {
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

void test_report_never_claims_the_target_without_qualified_hardware() {
    auto fast = synthetic_recorder();
    for (std::uint64_t i = 1; i <= 100; ++i) fast.record(frame(i, 1, 1ms, 10ms));
    Zones zones;
    zones.record("physics.step", 1ms);

    ReportContext software{"course", "Release", "software"};
    software.qualified_target_hardware = true;  // must not matter: software is not hardware
    const auto soft = saige::perf::to_json(fast, zones, software);
    require(contains(soft, "\"verdict\":\"not_evaluated\"") && !contains(soft, "within_budget"), "software must not get a verdict");
    require(contains(soft, "\"adapter_class\":\"software\""), "adapter class is reported");

    const auto none = saige::perf::to_json(fast, zones, {"course", "Release", "none"});
    require(contains(none, "\"verdict\":\"not_evaluated\"") && contains(none, "does not measure real GPU hardware"), "no renderer");

    ReportContext physical_unqualified{"course", "Release", "physical"};
    const auto unqualified = saige::perf::to_json(fast, zones, physical_unqualified);
    require(contains(unqualified, "\"verdict\":\"not_evaluated\"") && contains(unqualified, "not documented"),
            "a physical GPU without documented target hardware is still not a verdict");

    ReportContext qualified{"course", "Release", "physical"};
    qualified.qualified_target_hardware = true;
    require(contains(saige::perf::to_json(fast, zones, qualified), "\"verdict\":\"within_budget\""), "qualified fast run is within budget");
    auto slow = synthetic_recorder();
    for (std::uint64_t i = 1; i <= 100; ++i) slow.record(frame(i, 1, 1ms, i == 100 ? 30ms : 10ms));
    require(contains(saige::perf::to_json(slow, zones, qualified), "\"verdict\":\"within_budget\""),
            "one 30 ms frame in 100 is the maximum, so p99 is 10 ms and stays within budget");
    slow.record(frame(101, 1, 1ms, 30ms));
    require(contains(saige::perf::to_json(slow, zones, qualified), "\"verdict\":\"over_budget\""), "two slow frames put p99 over budget");
}

void test_report_contents() {
    auto recorder = synthetic_recorder();
    for (std::uint64_t i = 1; i <= 10; ++i) recorder.record(frame(i, 1, 2ms, i <= 8 ? 10ms : 20ms));
    Zones zones;
    zones.record("chunk.stage_next", 40ms);
    zones.record("physics.step", 1ms);
    ReportContext context{"quote\"and\\slash", "Debug", "none"};
    context.fixed_step_ns = 16'666'667;
    const auto json = saige::perf::to_json(recorder, zones, context);
    require(contains(json, "\"schema_version\":1") && contains(json, "\"frames\":10") && contains(json, "\"ticks\":10"), "counts");
    require(contains(json, "\"workload\":\"quote\\\"and\\\\slash\""), "strings are escaped");
    require(contains(json, "\"fixed_step_ns\":16666667"), "fixed step");
    // 8 frames of 10 ms and 2 of 20 ms average 12 ms, which is 83.3333 FPS; only the two 20 ms frames exceed 16.67 ms.
    require(contains(json, "\"cpu_average_fps\":83.3333"), "average fps follows the mean frame time");
    require(contains(json, "\"frames_over_budget\":2") && contains(json, "\"frames_over_2x_budget\":0"), "budget counters");
    require(contains(json, "\"chunk.stage_next\":{\"count\":1,\"mean_ms\":40.0000"), "zones are listed with their statistics");
    require(json.find("chunk.stage_next") < json.find("physics.step"), "zones appear in name order");
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
        test_report_never_claims_the_target_without_qualified_hardware();
        test_report_contents();
        test_process_memory();
    } catch (const std::exception& error) {
        std::cerr << "perf test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "perf tests passed\n";
}
