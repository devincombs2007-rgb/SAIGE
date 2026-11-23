#pragma once

#include "saige/capsule_character.hpp"
#include "saige/desktop.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>

namespace saige::sample {

class DesktopGame final : public IGame {
public:
    DesktopGame(World& world, const IDesktopPlatform& platform, bool course)
        : marker_(world.create()), platform_(platform) {
        if (!course) return;
        constexpr std::array geometry{
            Aabb{{-32, -32, -1}, {32, 32, 0}},
            Aabb{{2.5f, -0.5f, 0}, {3.5f, 0.5f, 1}},
            Aabb{{4.5f, 0.5f, 0}, {5.5f, 1.5f, 1}},
            Aabb{{5.5f, -1.5f, 0}, {6.5f, -0.5f, 1}},
        };
        motor_.emplace(Vec3{}, geometry);
        for (std::size_t i = 1; i < geometry.size(); ++i) {
            const auto& box = geometry[i];
            (void)world.create({(box.min.x + box.max.x) * 0.5f, (box.min.y + box.max.y) * 0.5f, box.min.z});
        }
    }

    void fixed_update(World& world, std::chrono::nanoseconds step) override {
        const int right = platform_.key_down(Key::d) || platform_.key_down(Key::right);
        const int left = platform_.key_down(Key::a) || platform_.key_down(Key::left);
        const int forward = platform_.key_down(Key::w) || platform_.key_down(Key::up);
        const int back = platform_.key_down(Key::s) || platform_.key_down(Key::down);
        if (motor_) {
            if (platform_.key_down(Key::r)) {
                motor_->reset();
            } else {
                motor_->step({static_cast<float>(forward - back), static_cast<float>(left - right),
                              platform_.key_down(Key::space), platform_.key_down(Key::shift)}, step);
                if (motor_->state().feet.z < -8.0f) motor_->reset();
            }
            world.set_position(marker_, motor_->state().feet);
            return;
        }
        const float seconds = std::chrono::duration<float>(step).count();
        // The marker drifts one world unit per second so unattended runs exercise simulation.
        position_.x += seconds * (1.0f + 4.0f * static_cast<float>(forward - back));
        position_.y += seconds * 4.0f * static_cast<float>(left - right);
        world.set_position(marker_, position_);
    }

private:
    Entity marker_;
    const IDesktopPlatform& platform_;
    Vec3 position_{};
    std::optional<CapsuleCharacter> motor_;
};

} // namespace saige::sample
