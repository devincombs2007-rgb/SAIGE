#include "saige/character_motor.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace saige {
namespace {

constexpr float half_width = 0.30f;
constexpr float character_height = 1.70f;
constexpr float contact_tolerance = 0.00001f;
constexpr std::array axes{&Vec3::x, &Vec3::y, &Vec3::z};

bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

StaticBox bounds(Vec3 feet) {
    return {{feet.x - half_width, feet.y - half_width, feet.z},
            {feet.x + half_width, feet.y + half_width, feet.z + character_height}};
}

bool overlaps(StaticBox a, StaticBox b, std::size_t axis) {
    const auto field = axes[axis];
    return a.min.*field < b.max.*field - contact_tolerance &&
           a.max.*field > b.min.*field + contact_tolerance;
}

// Clamp motion at the first face crossed along one axis. Recomputing bounds between axes preserves tangential movement.
bool sweep_axis(Vec3& feet, std::size_t axis, float displacement, std::span<const StaticBox> course) {
    if (displacement == 0.0f) return false;
    const auto field = axes[axis];
    const auto character = bounds(feet);
    const bool positive = displacement > 0.0f;
    float allowed = std::abs(displacement);
    bool blocked = false;
    for (const auto& box : course) {
        if (!overlaps(character, box, (axis + 1) % 3) || !overlaps(character, box, (axis + 2) % 3)) continue;
        const float gap = positive ? box.min.*field - character.max.*field : character.min.*field - box.max.*field;
        if (gap >= -contact_tolerance && gap <= allowed) {
            allowed = std::max(gap, 0.0f);
            blocked = true;
        }
    }
    feet.*field += positive ? allowed : -allowed;
    return blocked;
}

bool is_supported(Vec3 feet, std::span<const StaticBox> course) {
    return sweep_axis(feet, 2, -contact_tolerance, course);
}

void validate_boxes(std::span<const StaticBox> course) {
    for (const auto& box : course) {
        if (!finite(box.min) || !finite(box.max) || box.min.x >= box.max.x ||
            box.min.y >= box.max.y || box.min.z >= box.max.z) {
            throw std::invalid_argument("Character course contains an invalid box");
        }
    }
}

bool overlaps_course(Vec3 feet, std::span<const StaticBox> course) {
    const auto character = bounds(feet);
    return std::ranges::any_of(course, [&](const StaticBox& box) {
        return overlaps(character, box, 0) && overlaps(character, box, 1) && overlaps(character, box, 2);
    });
}

} // namespace

CharacterMotor::CharacterMotor(Vec3 spawn, std::span<const StaticBox> course)
    : spawn_(spawn), course_(course.begin(), course.end()), state_{spawn, {}, false} {
    if (!finite(spawn)) throw std::invalid_argument("Character spawn must be finite");
    validate_boxes(course_);
    if (overlaps_course(spawn, course_)) {
        throw std::invalid_argument("Character spawn overlaps course geometry");
    }
    reset();
}

void CharacterMotor::step(CharacterInput input, std::chrono::nanoseconds dt) {
    if (dt <= std::chrono::nanoseconds::zero() || dt > std::chrono::milliseconds(100) ||
        !std::isfinite(input.forward) || !std::isfinite(input.left)) {
        throw std::invalid_argument("Character step requires finite input and a positive fixed step of at most 100 ms");
    }

    const float seconds = std::chrono::duration<float>(dt).count();
    const float speed = input.sprint ? 6.0f : 3.5f;
    const float forward = std::clamp(input.forward, -1.0f, 1.0f);
    const float left = std::clamp(input.left, -1.0f, 1.0f);
    const float direction_scale = speed / std::max(1.0f, std::hypot(forward, left));
    Vec3& velocity = state_.velocity;
    const float dx = forward * direction_scale - velocity.x;
    const float dy = left * direction_scale - velocity.y;
    const float change = std::hypot(dx, dy);
    const float limit = (state_.grounded ? 22.0f : 7.0f) * seconds;
    const float fraction = change > limit ? limit / change : 1.0f;
    velocity.x += dx * fraction;
    velocity.y += dy * fraction;

    if (input.jump && !previous_jump_ && state_.grounded) velocity.z = 4.4f;
    previous_jump_ = input.jump;
    velocity.z = std::max(velocity.z - 9.81f * seconds, -30.0f);

    state_.grounded = false;
    for (std::size_t axis = 0; axis < axes.size(); ++axis) {
        const auto field = axes[axis];
        if (sweep_axis(state_.feet, axis, velocity.*field * seconds, course_)) {
            if (axis == 2 && velocity.z < 0.0f) state_.grounded = true;
            velocity.*field = 0.0f;
        }
    }
}

bool CharacterMotor::replace_static_geometry(std::span<const StaticBox> geometry) {
    validate_boxes(geometry);
    if (overlaps_course(state_.feet, geometry) || overlaps_course(spawn_, geometry)) {
        return false;
    }
    std::vector<StaticBox> candidate(geometry.begin(), geometry.end());
    course_.swap(candidate);
    state_.grounded = state_.velocity.z <= 0.0f && is_supported(state_.feet, course_);
    if (state_.grounded) state_.velocity.z = 0.0f;
    return true;
}

void CharacterMotor::reset() noexcept {
    state_ = {spawn_, {}, is_supported(spawn_, course_)};
    previous_jump_ = false;
}

} // namespace saige
