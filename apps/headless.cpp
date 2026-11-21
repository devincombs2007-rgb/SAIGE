#include <saige/runtime.hpp>
#include <saige/world.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;

class HeadlessPlatform final : public saige::IPlatform {
public:
    explicit HeadlessPlatform(std::chrono::nanoseconds period)
        : period_(period), deadline_(Clock::now()) {}

    Clock::time_point now() override { return Clock::now(); }

    bool poll_events() override {
        deadline_ += period_;
        std::this_thread::sleep_until(deadline_);
        // Resume normal pacing after a debugger pause or a suspended process.
        if (Clock::now() - deadline_ >= period_) {
            deadline_ = Clock::now();
        }
        return true;
    }

private:
    std::chrono::nanoseconds period_;
    Clock::time_point deadline_;
};

class MovingMarker final : public saige::IGame {
public:
    explicit MovingMarker(saige::World& world) : marker_(world.create()) {}

    void fixed_update(saige::World& world, std::chrono::nanoseconds step) override {
        position_.x += std::chrono::duration<float>(step).count();
        world.set_position(marker_, position_);
    }

private:
    saige::Entity marker_;
    saige::Vec3 position_{};
};

class FrameProbe final : public saige::IFrameSink {
public:
    void submit(const saige::FrameView& frame) override {
        instances = frame.instances.size();
        if (instances > 0) marker_x = frame.instances.front().position.x;
    }

    std::size_t instances{};
    float marker_x{};
};

constexpr std::string_view usage =
    "Usage: saige_headless [--frames COUNT] [--json]\n"
    "Run the moving-marker sample for COUNT frames (default 60).\n"
    "--json emits a versioned summary on standard output.\n";

struct Options {
    std::uint64_t frames = 60;
    bool json = false;
};

std::optional<Options> parse_options(int argc, char** argv) {
    Options options;
    bool has_frames = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--json" && !options.json) {
            options.json = true;
        } else if (arg == "--frames" && !has_frames && i + 1 < argc) {
            has_frames = true;
            const std::string_view value(argv[++i]);
            const auto [end, error] = std::from_chars(
                value.data(), value.data() + value.size(), options.frames);
            if (error != std::errc{} || end != value.data() + value.size()) return {};
        } else {
            return {};
        }
    }
    return options;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << usage;
        return 0;
    }
    const auto options = parse_options(argc, argv);
    if (!options) {
        std::cerr << usage;
        return 2;
    }
    try {
        const saige::RuntimeConfig config{};
        HeadlessPlatform platform(config.fixed_step);
        saige::World world;
        MovingMarker game(world);
        FrameProbe probe;
        const auto stats = saige::run(world, platform, game, probe, options->frames, config);
        if (options->json) {
            std::cout << "{\"schema_version\":1,\"frames\":" << stats.frames
                      << ",\"ticks\":" << stats.ticks << ",\"instances\":" << probe.instances
                      << ",\"marker_x\":" << probe.marker_x << "}\n";
        } else {
            std::cout << "SAIGE frames=" << stats.frames << " ticks=" << stats.ticks
                      << " instances=" << probe.instances << " marker_x=" << probe.marker_x << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "SAIGE startup or runtime error: " << error.what() << '\n';
        return 1;
    }
}
