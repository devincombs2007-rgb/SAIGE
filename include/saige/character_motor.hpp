#pragma once

#include "saige/world.hpp"

#include <chrono>
#include <span>
#include <vector>

namespace saige {

struct StaticBox {
    Vec3 min;
    Vec3 max;
};

struct CharacterInput {
    float forward{};
    float left{};
    bool jump{};
    bool sprint{};
};

struct CharacterState {
    Vec3 feet;
    Vec3 velocity;
    bool grounded{};
    bool operator==(const CharacterState&) const = default;
};

// Fixed-step, Z-up prototype for static box collision in local world coordinates.
class CharacterMotor {
public:
    CharacterMotor(Vec3 spawn, std::span<const StaticBox> course);
    void step(CharacterInput input, std::chrono::nanoseconds dt);
    // Replace the static collision set between ticks. Returns false if the player or reset pose
    // would overlap the candidate. Invalid boxes throw. Either rejection keeps the old state.
    bool replace_static_geometry(std::span<const StaticBox> geometry);
    void reset() noexcept;
    [[nodiscard]] const CharacterState& state() const noexcept { return state_; }

private:
    Vec3 spawn_;
    std::vector<StaticBox> course_;
    CharacterState state_;
    bool previous_jump_{};
};

} // namespace saige
