// Repeatable CPU benchmark of the first playable character course (issue #62).
//
// A scripted, fully deterministic input replay drives the fixed-step CharacterMotor over a field of static boxes while a
// ChunkStream stages the chunks around the player, crossing chunk boundaries in both directions. The platform clock is virtual:
// every frame advances exactly one fixed step, so the tick sequence, poses and chunk requests are identical on every run and
// machine. Only the *measurement* uses the real steady clock, and nothing measured feeds back into the simulation. The loop runs
// unpaced, so the report is CPU cost per frame, with no renderer ("adapter_class":"none"); it is not a delivered-FPS figure and
// the 16.67 ms budget verdict is always "not_evaluated" here (see docs/performance-measurement.md).
#include "saige/chunk_manifest.hpp"
#include "saige/character_motor.hpp"
#include "saige/perf.hpp"
#include "saige/runtime.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifndef SAIGE_BENCH_BUILD
#define SAIGE_BENCH_BUILD "unknown"
#endif

namespace {

using namespace std::chrono_literals;

constexpr std::chrono::nanoseconds fixed_step{16'666'667};
constexpr double chunk_cell_size = 32.0;  // meters per chunk cell
constexpr std::uint32_t chunk_radius = 1; // a 3x3 neighborhood stays resident

// Virtual clock: one fixed step per frame, so frames == ticks and replay does not depend on how fast this machine runs.
class VirtualPlatform final : public saige::IPlatform {
public:
    explicit VirtualPlatform(std::uint64_t frames) : frames_left_(frames) {}
    std::chrono::steady_clock::time_point now() override { return time_; }
    bool poll_events() override {
        if (frames_left_ == 0) return false;
        --frames_left_;
        time_ += fixed_step;
        return true;
    }

private:
    std::uint64_t frames_left_;
    std::chrono::steady_clock::time_point time_{};
};

// 20 seconds forward at sprint/walk, 20 seconds back, with periodic jumps and a gentle strafe, repeating. Pure function of the tick.
saige::CharacterInput scripted_input(std::uint64_t tick) {
    const std::uint64_t phase = tick % 2400;
    saige::CharacterInput input;
    input.forward = phase < 1200 ? 1.0f : -1.0f;
    input.left = ((tick / 300) % 2 == 0) ? 0.25f : -0.25f;
    input.sprint = (tick / 600) % 2 == 0;
    input.jump = tick % 90 < 2;  // an edge every 90 ticks: the motor jumps on the rising edge when grounded
    return input;
}

std::vector<saige::StaticBox> make_course(std::size_t obstacle_count) {
    std::vector<saige::StaticBox> course;
    course.push_back({{-4096, -4096, -1}, {4096, 4096, 0}});  // floor
    // Obstacles beside the route (y in [2, 40]) so the scripted run is not blocked yet every sweep tests them; a fixed linear
    // congruence keeps the layout identical everywhere.
    std::uint32_t seed = 12345;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f;
    };
    for (std::size_t i = 0; i < obstacle_count; ++i) {
        const float x = -200.0f + next() * 400.0f;
        const float y = 2.0f + next() * 38.0f;
        const float height = 0.4f + next() * 1.2f;
        course.push_back({{x, y, 0.0f}, {x + 0.6f + next(), y + 0.6f + next(), height}});
    }
    return course;
}

struct ChunkCounters {
    std::uint64_t requests{};
    std::uint64_t stages{};
    std::uint64_t commits{};
    std::uint64_t ticks_pending{};
    std::uint64_t longest_pending_ticks{};
    std::size_t peak_retained_bytes{};
    std::size_t peak_active{};
};

std::uint64_t fnv1a(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

class BenchGame final : public saige::IGame {
public:
    BenchGame(saige::World& world, std::vector<saige::StaticBox> course, std::vector<saige::ChunkSource> sources,
              saige::perf::Zones& zones)
        : marker_(world.create()), motor_(saige::Vec3{}, course), sources_(std::move(sources)), zones_(zones) {}

    void fixed_update(saige::World& world, std::chrono::nanoseconds step) override {
        {
            const auto scope = zones_.scope("physics.step");
            motor_.step(scripted_input(tick_), step);
        }
        const saige::CharacterState& state = motor_.state();
        world.set_position(marker_, state.feet);
        checksum_ = fnv1a(checksum_, &state.feet, sizeof state.feet);
        checksum_ = fnv1a(checksum_, &state.grounded, sizeof state.grounded);
        update_chunks(state.feet);
        ++tick_;
    }

    [[nodiscard]] std::uint64_t checksum() const noexcept { return checksum_; }
    [[nodiscard]] const ChunkCounters& counters() const noexcept { return counters_; }
    [[nodiscard]] const saige::CharacterState& state() const noexcept { return motor_.state(); }

private:
    void update_chunks(saige::Vec3 position) {
        const saige::ChunkCoord center = saige::chunk_containing(position, chunk_cell_size);
        if (!requested_ || center != requested_center_) {
            const auto scope = zones_.scope("chunk.request");
            stream_.request(saige::select_chunks(sources_, center, chunk_radius));
            requested_center_ = center;
            requested_ = true;
            pending_ = true;
            pending_ticks_ = 0;
            ++counters_.requests;
        }
        if (!pending_) return;
        ++counters_.ticks_pending;
        ++pending_ticks_;
        bool staged = false;
        {
            const auto scope = zones_.scope("chunk.stage_next");
            staged = stream_.stage_next();
        }
        if (staged) ++counters_.stages;
        bool committed = false;
        {
            const auto scope = zones_.scope("chunk.commit_frame");
            committed = stream_.commit_frame();
        }
        counters_.peak_retained_bytes = std::max(counters_.peak_retained_bytes, stream_.retained_asset_bytes());
        counters_.peak_active = std::max(counters_.peak_active, stream_.active_count());
        if (committed) {
            ++counters_.commits;
            counters_.longest_pending_ticks = std::max(counters_.longest_pending_ticks, pending_ticks_);
            pending_ = false;
        }
    }

    saige::Entity marker_;
    saige::CharacterMotor motor_;
    std::vector<saige::ChunkSource> sources_;
    saige::perf::Zones& zones_;
    saige::ChunkStream stream_;
    saige::ChunkCoord requested_center_{};
    bool requested_ = false;
    bool pending_ = false;
    std::uint64_t pending_ticks_ = 0;
    std::uint64_t tick_ = 0;
    std::uint64_t checksum_ = 14695981039346656037ull;
    ChunkCounters counters_;
};

class NullSink final : public saige::IFrameSink {
public:
    void submit(const saige::FrameView& frame) override { instances = frame.instances.size(); }
    std::size_t instances{};
};

bool parse_count(std::string_view text, std::uint64_t& out) {
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), out);
    return error == std::errc{} && end == text.data() + text.size();
}

} // namespace

int main(int argc, char** argv) {
    std::uint64_t frames = 3600;
    std::uint64_t obstacles = 128;
    std::optional<std::filesystem::path> fixture;
    std::optional<std::string> json_path;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        const auto value = [&](std::string_view& out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string_view text;
        if (argument == "--frames" && value(text) && parse_count(text, frames) && frames > 0 && frames <= 10'000'000) continue;
        if (argument == "--obstacles" && value(text) && parse_count(text, obstacles) && obstacles <= 100'000) continue;
        if (argument == "--fixture" && value(text)) { fixture = std::filesystem::path(std::string(text)); continue; }
        if (argument == "--json" && value(text)) { json_path = std::string(text); continue; }
        std::cerr << "Usage: saige_course_bench --fixture SCENE_PROJECT [--frames N] [--obstacles N] [--json FILE|-]\n";
        return 2;
    }
    if (!fixture) {
        std::cerr << "saige_course_bench needs --fixture, a scene project used for every chunk\n";
        return 2;
    }

    try {
        std::vector<saige::ChunkSource> sources;
        for (std::int32_t y = -2; y <= 2; ++y) {
            for (std::int32_t x = -4; x <= 8; ++x) sources.push_back({{x, y}, *fixture});
        }
        saige::World world;
        saige::perf::Zones zones;
        BenchGame game(world, make_course(static_cast<std::size_t>(obstacles)), std::move(sources), zones);
        saige::perf::TimedGame timed(game, zones);
        saige::perf::FrameRecorder recorder({.measure_wall_interval = true, .memory_every_frames = 60,
                                             .reserve_frames = static_cast<std::size_t>(frames)});
        VirtualPlatform platform(frames);
        NullSink sink;
        const saige::RuntimeConfig config{fixed_step, 8};
        const auto stats = saige::run(world, platform, timed, sink, std::nullopt, config, &recorder);

        saige::perf::ReportContext context;
        context.workload = "character_course_replay";
        context.build = SAIGE_BENCH_BUILD;
        context.adapter_class = "none";
        context.fixed_step_ns = fixed_step.count();
        const std::string perf = saige::perf::to_json(recorder, zones, context);

        const ChunkCounters& chunks = game.counters();
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "{\"bench\":{\"paced\":false,\"frames_requested\":" << frames << ",\"obstacles\":" << obstacles
            << ",\"replay_checksum\":\"" << std::hex << game.checksum() << std::dec << "\""
            << ",\"final_feet\":[" << game.state().feet.x << ',' << game.state().feet.y << ',' << game.state().feet.z << "]"
            << ",\"chunk_cell_size_m\":" << chunk_cell_size << ",\"chunk_requests\":" << chunks.requests
            << ",\"chunk_stage_calls_that_staged\":" << chunks.stages << ",\"chunk_commits\":" << chunks.commits
            << ",\"ticks_with_chunk_work\":" << chunks.ticks_pending << ",\"longest_pending_ticks\":" << chunks.longest_pending_ticks
            << ",\"peak_active_chunks\":" << chunks.peak_active << ",\"peak_retained_asset_bytes\":" << chunks.peak_retained_bytes
            << "},\"perf\":" << perf << "}\n";
        if (!json_path || *json_path == "-") {
            std::cout << out.str();
        } else {
            std::ofstream file(*json_path, std::ios::binary);
            file << out.str();
            if (!file) throw std::runtime_error("could not write " + *json_path);
        }
        std::clog << "course bench: frames=" << stats.frames << " ticks=" << stats.ticks << " chunk_commits=" << chunks.commits << '\n';
    } catch (const std::exception& error) {
        std::cerr << "saige_course_bench error: " << error.what() << '\n';
        return 1;
    }
}
