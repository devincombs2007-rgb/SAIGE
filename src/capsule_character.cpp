#include "saige/capsule_character.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace saige {
namespace {

// Separation kept from a surface after a contact, so float rounding cannot leave the capsule penetrating on the next sweep. The
// sweep primitive treats anything within 1e-5 as touching.
constexpr float skin = 1e-4f;
constexpr int max_slide_iterations = 4;
constexpr std::chrono::nanoseconds max_step = std::chrono::milliseconds(100);

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float length2(Vec3 a) { return dot(a, a); }

bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void validate(const CapsuleCharacterConfig& c) {
    const auto positive = [](float v) { return std::isfinite(v) && v > 0.0f; };
    const auto non_negative = [](float v) { return std::isfinite(v) && v >= 0.0f; };
    if (!positive(c.radius) || !std::isfinite(c.height) || c.height < 2.0f * c.radius || !positive(c.walk_speed) ||
        !positive(c.sprint_speed) || !positive(c.ground_acceleration) || !positive(c.air_acceleration) || !non_negative(c.gravity) ||
        !non_negative(c.jump_speed) || !positive(c.terminal_speed) || !std::isfinite(c.max_slope_radians) || c.max_slope_radians < 0.0f ||
        c.max_slope_radians >= 1.5707963f || !non_negative(c.step_height) || !non_negative(c.ground_snap)) {
        throw std::invalid_argument("Capsule character configuration is invalid");
    }
}

void validate(std::span<const Aabb> boxes) {
    for (const Aabb& box : boxes) {
        if (!finite(box.min) || !finite(box.max) || box.min.x >= box.max.x || box.min.y >= box.max.y || box.min.z >= box.max.z) {
            throw std::invalid_argument("Capsule character geometry contains an invalid box");
        }
    }
}

// Remove from `v` the part that points into each plane met so far. Clipping twice converges for the planes a box world produces;
// whatever still points into a plane is met again by the next sweep, and the iteration cap ends a wedge. (A dedicated crease rule
// was tried and had no effect on any scenario, including a character wedged between two rims, so it is not carried.)
Vec3 clip_to_planes(Vec3 v, const std::array<Vec3, 3>& planes, int count) {
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 0; i < count; ++i) {
            const float into = dot(v, planes[static_cast<std::size_t>(i)]);
            if (into < 0.0f) v = v - planes[static_cast<std::size_t>(i)] * into;
        }
    }
    return v;
}

} // namespace

CapsuleCharacter::CapsuleCharacter(Vec3 spawn, std::span<const Aabb> geometry, CapsuleCharacterConfig config)
    : config_(config), spawn_(spawn), geometry_(geometry.begin(), geometry.end()) {
    validate(config_);
    if (!finite(spawn)) throw std::invalid_argument("Capsule character spawn must be finite");
    validate(geometry_);
    if (overlaps(spawn_, geometry_)) throw std::invalid_argument("Capsule character spawn overlaps geometry");
    reset();
}

std::optional<CapsuleHit> CapsuleCharacter::sweep(Vec3 feet, Vec3 displacement) const {
    return sweep_capsule_boxes(capsule_at(feet), displacement, geometry_);
}

bool CapsuleCharacter::overlaps(Vec3 feet, std::span<const Aabb> geometry) const {
    const Capsule capsule = capsule_at(feet);
    return std::any_of(geometry.begin(), geometry.end(), [&](const Aabb& box) { return capsule_box_push_out(capsule, box).has_value(); });
}

void CapsuleCharacter::recover_from_overlap() {
    // Normally a no-op. It exists for float rounding at large coordinates and for geometry that changed under the character.
    for (int pass = 0; pass < 4; ++pass) {
        bool moved = false;
        for (const Aabb& box : geometry_) {
            if (const auto push = capsule_box_push_out(capsule(), box)) {
                const float length = std::sqrt(length2(*push));
                state_.feet = state_.feet + *push + (length > 0.0f ? *push * (skin / length) : Vec3{});
                moved = true;
            }
        }
        if (!moved) return;
    }
}

std::optional<CapsuleCharacter::GroundContact> CapsuleCharacter::probe_ground(Vec3 feet) const {
    // Look for walkable ground within the snap distance below `feet`. A steep contact met on the way is not an answer by itself: the
    // capsule may merely be touching the top edge of a low ledge it is sliding along, with the real floor right beneath. So the
    // probe slides down along such a contact (a few times) before concluding there is no ground. Standing on a rim that overhangs too
    // much finds no walkable contact within the distance and still reports "not grounded", so the character slides off as before.
    Vec3 remaining{0.0f, 0.0f, -config_.ground_snap - skin};
    for (int iteration = 0; iteration < max_slide_iterations; ++iteration) {
        const auto hit = sweep(feet, remaining);
        if (!hit) return std::nullopt;
        feet = feet + remaining * hit->t;
        if (is_walkable(hit->normal, config_.max_slope_radians)) return GroundContact{feet, hit->normal};
        feet = feet + hit->normal * skin;
        const Vec3 leftover = remaining * (1.0f - hit->t);
        remaining = leftover - hit->normal * dot(leftover, hit->normal);
        if (remaining.z > -1e-6f) return std::nullopt;  // the contact stops the descent: nothing underfoot
    }
    return std::nullopt;
}

bool CapsuleCharacter::ground_underfoot(Vec3 feet) const {
    return probe_ground(feet).has_value();
}

void CapsuleCharacter::snap_to_ground() {
    if (const auto ground = probe_ground(state_.feet)) {
        // Back off vertically, not along the normal: on a tilted contact (a ledge's rim) the normal has a horizontal part, and
        // re-applying it every tick would creep a standing character sideways.
        state_.feet = ground->feet + Vec3{0.0f, 0.0f, skin};
        state_.grounded = true;
        state_.ground_normal = ground->normal;
        state_.velocity.z = std::min(state_.velocity.z, 0.0f);
    } else {
        state_.grounded = false;
    }
}

bool CapsuleCharacter::try_step_up(Vec3 leftover) {
    leftover.z = 0.0f;
    if (config_.step_height <= 0.0f || length2(leftover) < 1e-10f) return false;
    const Vec3 origin = state_.feet;

    // Raise by up to the step height, stopping short of a ceiling.
    float rise = config_.step_height;
    if (const auto up = sweep(origin, {0.0f, 0.0f, rise})) rise = std::max(0.0f, up->t * rise - skin);
    if (rise < 0.01f) return false;
    const Vec3 raised = origin + Vec3{0.0f, 0.0f, rise};

    // Move forward at that height. A capsule stopped against a vertical face has its bottom sphere a full radius from the ledge's
    // edge, which would land on the rim with a contact normal steeper than the slope limit. So the move reaches at least as far as
    // it takes for the sphere to overhang the edge by no more than the slope limit allows (plus a little margin), usually less than
    // two ticks of walking. A tall obstacle at the raised height stops it at once and the attempt fails.
    const float wanted = std::sqrt(length2(leftover));
    const float reach = std::max(wanted, config_.radius * (1.0f - std::sin(config_.max_slope_radians)) + 0.02f);
    const Vec3 ahead = leftover * (reach / wanted);
    float travelled = 1.0f;
    if (const auto across = sweep(raised, ahead)) travelled = std::max(0.0f, across->t - skin / reach);
    if (travelled * reach < std::min(wanted, 0.01f)) return false;
    const Vec3 over = raised + ahead * travelled;

    // Come back down: it is a step only if that finds walkable ground within the distance climbed plus the snap distance.
    const float drop = rise + config_.ground_snap + skin;
    const auto down = sweep(over, {0.0f, 0.0f, -drop});
    if (!down || down->started_overlapping || !is_walkable(down->normal, config_.max_slope_radians)) return false;
    // The ledge, not the capsule, is what is limited. Landing on a ledge's rim leaves the feet below its top (the bottom is a
    // hemisphere), and a walkable rim contact could then be rolled over, so measure the supporting surface itself.
    if (down->point.z - origin.z > config_.step_height + 1e-3f) return false;
    const Vec3 landed = over + Vec3{0.0f, 0.0f, -drop * down->t + skin};
    if (landed.z - origin.z < 0.005f) return false;  // not higher than where it started: it was not a step

    state_.feet = landed;
    state_.grounded = true;
    state_.ground_normal = down->normal;
    return true;
}

void CapsuleCharacter::slide(Vec3 remaining, bool can_step) {
    std::array<Vec3, 3> planes{};
    int plane_count = 0;
    for (int iteration = 0; iteration < max_slide_iterations; ++iteration) {
        if (length2(remaining) < 1e-12f) return;
        const auto hit = sweep(state_.feet, remaining);
        if (!hit) {
            state_.feet = state_.feet + remaining;
            return;
        }
        const Vec3 normal = hit->normal;
        state_.feet = state_.feet + remaining * hit->t + normal * skin;
        const Vec3 leftover = remaining * (1.0f - hit->t);

        const bool walkable = is_walkable(normal, config_.max_slope_radians);
        if (can_step && !walkable && normal.z > -0.2f && try_step_up(leftover)) return;

        // The character no longer moves into this surface.
        const float into = dot(state_.velocity, normal);
        if (into < 0.0f) state_.velocity = state_.velocity - normal * into;
        if (!state_.grounded && walkable) {  // landing: a walkable surface met from above while falling
            state_.grounded = true;
            state_.ground_normal = normal;
        }

        if (plane_count < 3) planes[static_cast<std::size_t>(plane_count++)] = normal;
        remaining = clip_to_planes(leftover, planes, plane_count);
    }
}

void CapsuleCharacter::step(CapsuleInput input, std::chrono::nanoseconds dt) {
    if (dt <= std::chrono::nanoseconds::zero() || dt > max_step || !std::isfinite(input.forward) || !std::isfinite(input.left)) {
        throw std::invalid_argument("Capsule character step requires finite input and a fixed step in (0, 100 ms]");
    }
    const float seconds = std::chrono::duration<float>(dt).count();

    recover_from_overlap();

    // Horizontal velocity moves toward the input's target, faster on the ground than in the air.
    const float speed = input.sprint ? config_.sprint_speed : config_.walk_speed;
    const float forward = std::clamp(input.forward, -1.0f, 1.0f);
    const float left = std::clamp(input.left, -1.0f, 1.0f);
    const float scale = speed / std::max(1.0f, std::hypot(forward, left));
    const float dx = forward * scale - state_.velocity.x;
    const float dy = left * scale - state_.velocity.y;
    const float change = std::hypot(dx, dy);
    const float limit = (state_.grounded ? config_.ground_acceleration : config_.air_acceleration) * seconds;
    const float fraction = change > limit ? limit / change : 1.0f;
    state_.velocity.x += dx * fraction;
    state_.velocity.y += dy * fraction;

    // A jump needs the button to go down while standing; holding it does not repeat.
    const bool jumped = input.jump && !previous_jump_ && state_.grounded;
    previous_jump_ = input.jump;
    if (jumped) {
        state_.velocity.z = config_.jump_speed;
        state_.grounded = false;
    }

    if (state_.grounded) {
        // Walk along the ground, then keep contact over small drops and step edges.
        Vec3 move = Vec3{state_.velocity.x, state_.velocity.y, 0.0f} * seconds;
        move = move - state_.ground_normal * dot(move, state_.ground_normal);
        state_.velocity.z = 0.0f;
        slide(move, /*can_step=*/true);
        if (state_.grounded) snap_to_ground();
    } else {
        state_.velocity.z = std::max(state_.velocity.z - config_.gravity * seconds, -config_.terminal_speed);
        slide(state_.velocity * seconds, /*can_step=*/false);
        if (state_.grounded) state_.velocity.z = std::min(state_.velocity.z, 0.0f);
    }
}

bool CapsuleCharacter::replace_static_geometry(std::span<const Aabb> geometry) {
    validate(geometry);
    if (overlaps(state_.feet, geometry) || overlaps(spawn_, geometry)) return false;
    std::vector<Aabb> candidate(geometry.begin(), geometry.end());
    geometry_.swap(candidate);
    const auto hit = sweep(state_.feet, {0.0f, 0.0f, -config_.ground_snap - skin});
    state_.grounded = hit && is_walkable(hit->normal, config_.max_slope_radians) && state_.velocity.z <= 0.0f;
    if (state_.grounded) {
        state_.ground_normal = hit->normal;
        state_.velocity.z = 0.0f;
    }
    return true;
}

void CapsuleCharacter::teleport(Vec3 feet) {
    if (!finite(feet)) throw std::invalid_argument("Capsule character teleport position must be finite");
    state_.feet = feet;
    state_.velocity = {};
    state_.grounded = false;
    recover_from_overlap();
    snap_to_ground();
}

void CapsuleCharacter::reset() {
    state_ = {};
    state_.feet = spawn_;
    previous_jump_ = false;
    const auto hit = sweep(spawn_, {0.0f, 0.0f, -config_.ground_snap - skin});
    state_.grounded = hit && is_walkable(hit->normal, config_.max_slope_radians);
    if (state_.grounded) state_.ground_normal = hit->normal;
}

} // namespace saige
