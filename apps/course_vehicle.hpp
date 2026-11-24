#pragma once

#include "course_input.hpp"
#include "saige/capsule_character.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <optional>
#include <span>

namespace saige::sample {

class CourseVehicle {
public:
    explicit CourseVehicle(Vec3 spawn) : spawn_(spawn), position_(spawn) {}

    void step(MovementActions input, bool brake, std::chrono::nanoseconds step, std::span<const Aabb> boxes) {
        const float dt = std::chrono::duration<float>(step).count();
        const float target = brake ? 0.0f : input.forward * (input.forward >= 0 ? 10.0f : 4.0f);
        const float acceleration = brake ? 14.0f : input.forward == 0 ? 3.0f : 6.0f;
        speed_ += std::clamp(target - speed_, -dt * acceleration, dt * acceleration);
        const float steering = input.left * std::clamp(speed_ * 0.4f, -1.0f, 1.0f);
        heading_ = std::remainder(heading_ + steering * 1.3f * dt, 2.0f * std::numbers::pi_v<float>);
        const Vec3 move{std::cos(heading_) * speed_ * dt, std::sin(heading_) * speed_ * dt, 0};
        const Capsule body{position_, 0.6f, 1.2f};
        Vec3 candidate{position_.x + move.x, position_.y + move.y, position_.z};
        if (const auto hit = sweep_capsule_boxes(body, move, boxes)) {
            candidate = hit->stop;
            park();
        }
        // This prototype stays on its flat driving surface.
        if (!floor_below({candidate, body.radius, body.height}, boxes)) {
            park();
            return;
        }
        position_ = candidate;
    }

    [[nodiscard]] bool can_enter(const CapsuleCharacterState& character, const CapsuleCharacterConfig& config,
                                 std::span<const Aabb> boxes) const {
        const Vec3 approach{position_.x - character.feet.x, position_.y - character.feet.y, 0};
        return character.grounded && std::abs(character.feet.z - position_.z) < 0.25f &&
               std::hypot(approach.x, approach.y) <= 1.4f &&
               !sweep_capsule_boxes({character.feet, config.radius, config.height}, approach, boxes);
    }

    [[nodiscard]] std::optional<Vec3> exit_point(const CapsuleCharacterConfig& config,
                                                 std::span<const Aabb> boxes) const {
        if (std::abs(speed_) > 0.25f) return {};
        const Capsule rider{position_, config.radius, config.height};
        const Vec3 left{-std::sin(heading_), std::cos(heading_), 0};
        for (const float distance : {1.15f, -1.15f}) {
            const Vec3 move{left.x * distance, left.y * distance, 0};
            if (sweep_capsule_boxes(rider, move, boxes)) continue;
            const Capsule side{{position_.x + move.x, position_.y + move.y, position_.z},
                               rider.radius, rider.height};
            if (const auto floor = floor_below(side, boxes)) return floor->stop;
        }
        return {};
    }

    void park() noexcept { speed_ = 0; }
    void reset() noexcept {
        position_ = spawn_;
        heading_ = 0;
        park();
    }
    [[nodiscard]] Vec3 position() const noexcept { return position_; }
    [[nodiscard]] float heading() const noexcept { return heading_; }
    [[nodiscard]] float speed() const noexcept { return speed_; }

private:
    [[nodiscard]] static std::optional<CapsuleHit> floor_below(Capsule capsule, std::span<const Aabb> boxes) {
        const auto hit = sweep_capsule_boxes(capsule, {0, 0, -0.08f}, boxes);
        return hit && !hit->started_overlapping && hit->normal.z > 0.99f ? hit : std::nullopt;
    }

    Vec3 spawn_;
    Vec3 position_;
    float heading_{};
    float speed_{};
};

} // namespace saige::sample
