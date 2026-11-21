#include "saige/runtime.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct ScriptedPlatform final : saige::IPlatform {
    std::vector<std::chrono::milliseconds> deltas;
    std::chrono::steady_clock::time_point time{};
    std::size_t polls = 0;

    std::chrono::steady_clock::time_point now() override { return time; }
    bool poll_events() override {
        const auto index = polls++;
        if (index == deltas.size()) {
            return false;
        }
        time += deltas.at(index);
        return true;
    }
};

struct Game final : saige::IGame {
    void fixed_update(saige::World& world, std::chrono::nanoseconds step) override {
        require(step == 10ms, "game received a variable timestep");
        (void)world.create();
    }
};

struct Recorder final : saige::IFrameSink {
    std::vector<std::uint64_t> ticks;
    std::vector<double> phases;

    void submit(const saige::FrameView& frame) override {
        require(frame.instances.size() == frame.tick, "snapshot missed a game update");
        ticks.push_back(frame.tick);
        phases.push_back(frame.alpha);
    }
};

void test_fixed_updates_and_quit() {
    saige::World world;
    Game game;
    Recorder sink;
    ScriptedPlatform platform;
    platform.deltas = {5ms, 9ms, 111ms, 5ms};
    const auto stats = saige::run(world, platform, game, sink, std::nullopt, {10ms, 2});
    require(stats.frames == 4 && stats.ticks == 4 && platform.polls == 5, "incorrect quit or update count");
    require(sink.ticks == std::vector<std::uint64_t>{0, 1, 3, 4}, "catch-up limit or carry is wrong");
    const std::vector<double> expected{0.5, 0.4, 0.5, 0.0};
    for (std::size_t i = 0; i < expected.size(); ++i) {
        require(std::abs(expected[i] - sink.phases.at(i)) < 1e-12, "incorrect fraction toward next tick");
    }
}

void test_frame_budget() {
    saige::World world;
    Game game;
    Recorder sink;
    ScriptedPlatform platform;
    platform.deltas = {10ms, 10ms};
    const auto skipped = saige::run(world, platform, game, sink, 0, {10ms, 2});
    require(skipped.frames == 0 && platform.polls == 0 && sink.ticks.empty(), "zero budget polled or submitted");
    const auto stats = saige::run(world, platform, game, sink, 1, {10ms, 2});
    require(stats.frames == 1 && stats.ticks == 1 && platform.polls == 1, "frame budget failed to stop loop");
}

void test_invalid_config() {
    saige::World world;
    Game game;
    Recorder sink;
    ScriptedPlatform platform;
    for (const auto config : {saige::RuntimeConfig{0ns, 1}, saige::RuntimeConfig{1ns, 0}}) {
        bool rejected = false;
        try {
            (void)saige::run(world, platform, game, sink, std::nullopt, config);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected && platform.polls == 0, "invalid config reached platform");
    }
}
} // namespace

int main() {
    try {
        test_fixed_updates_and_quit();
        test_frame_budget();
        test_invalid_config();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
