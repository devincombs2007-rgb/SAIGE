#include "saige/win32_platform.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>

namespace {

class DesktopGame final : public saige::IGame {
public:
    DesktopGame(saige::World& world, const saige::IDesktopPlatform& platform)
        : marker_(world.create()), platform_(platform) {}

    void fixed_update(saige::World& world, std::chrono::nanoseconds step) override {
        using saige::Key;
        const int right = platform_.key_down(Key::d) || platform_.key_down(Key::right);
        const int left = platform_.key_down(Key::a) || platform_.key_down(Key::left);
        const int forward = platform_.key_down(Key::w) || platform_.key_down(Key::up);
        const int back = platform_.key_down(Key::s) || platform_.key_down(Key::down);
        const float seconds = std::chrono::duration<float>(step).count();
        // The marker drifts one world unit per second so unattended runs exercise simulation.
        position_.x += seconds * (1.0f + 4.0f * static_cast<float>(forward - back));
        position_.y += seconds * 4.0f * static_cast<float>(left - right);
        world.set_position(marker_, position_);
    }

private:
    saige::Entity marker_;
    const saige::IDesktopPlatform& platform_;
    saige::Vec3 position_{};
};

// The sample reports the submitted pose until a graphics adapter is selected.
class DesktopFrameProbe final : public saige::IFrameSink {
public:
    void submit(const saige::FrameView& frame) override {
        instances = frame.instances.size();
        if (!frame.instances.empty()) {
            position = frame.instances.front().position;
        }
    }

    std::size_t instances{};
    saige::Vec3 position{};
};

} // namespace

int main(int argc, char** argv) {
    std::optional<std::uint64_t> frames;
    if (argc != 1) {
        if (argc != 3 || std::string_view(argv[1]) != "--frames") {
            std::cerr << "Usage: saige_desktop [--frames COUNT]\n";
            return 2;
        }
        const std::string_view value(argv[2]);
        std::uint64_t count{};
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
        if (error != std::errc{} || end != value.data() + value.size()) {
            std::cerr << "--frames expects a nonnegative integer\n";
            return 2;
        }
        frames = count;
    }

    try {
        saige::Win32Platform platform(L"SAIGE | WASD to move | Esc to quit");
        saige::World world;
        DesktopGame game(world, platform);
        DesktopFrameProbe probe;
        const auto stats = saige::run(world, platform, game, probe, frames);
        std::cout << "SAIGE desktop frames=" << stats.frames << " ticks=" << stats.ticks
                  << " instances=" << probe.instances << " marker_x=" << probe.position.x
                  << " marker_y=" << probe.position.y << '\n';
    } catch (const std::exception& error) {
        std::cerr << "SAIGE desktop error: " << error.what() << '\n';
        return 1;
    }
}
