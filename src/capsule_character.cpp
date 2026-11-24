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
    // Every sweep the controller issues must stay inside the supported envelope (capsule_sweep.hpp): the farthest a tick moves is the
    // fastest speed over the longest step; the step-up and ground probes add their own reach.
    const float fastest = std::max({c.sprint_speed, c.terminal_speed, c.jump_speed});
    if (!(fastest * std::chrono::duration<float>(max_step).count() <= 0.5f * max_sweep_length) || !(c.step_height + c.ground_snap <= 0.25f * max_sweep_length) ||
        !(c.height <= 0.25f * max_sweep_length)) {
        throw std::invalid_argument("Capsule character configuration exceeds the supported sweep envelope");
    }
}

// A position the controller can simulate from: the capsule at rest there is inside the supported envelope. Anything else must be
// rebased to chunk-local coordinates by the caller (the controller never answers from a lower-precision sweep).
void require_in_envelope(const CapsuleCharacterConfig& config, Vec3 feet) {
    check_sweep_envelope({feet, config.radius, config.height}, Vec3{0.0f, 0.0f, 0.0f});
}

void validate(std::span<const Aabb> boxes) {
    for (const Aabb& box : boxes) {
        if (!finite(box.min) || !finite(box.max) || box.min.x >= box.max.x || box.min.y >= box.max.y || box.min.z >= box.max.z) {
            throw std::invalid_argument("Capsule character geometry contains an invalid box");
        }
    }
}

void validate(std::span<const Triangle> triangles) {
    for (const Triangle& t : triangles) {
        if (!is_valid_triangle(t)) throw std::invalid_argument("Capsule character geometry contains an invalid triangle");
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
    : CapsuleCharacter(spawn, geometry, std::span<const Triangle>{}, config) {}

CapsuleCharacter::CapsuleCharacter(Vec3 spawn, std::span<const Aabb> boxes, std::span<const Triangle> triangles, CapsuleCharacterConfig config)
    : config_(config), spawn_(spawn), geometry_(boxes.begin(), boxes.end()), triangles_(triangles.begin(), triangles.end()) {
    validate(config_);
    if (!finite(spawn)) throw std::invalid_argument("Capsule character spawn must be finite");
    require_in_envelope(config_, spawn);
    validate(geometry_);
    validate(triangles_);
    if (overlaps(spawn_, geometry_, triangles_)) throw std::invalid_argument("Capsule character spawn overlaps geometry");
    reset();
}

std::optional<CapsuleHit> CapsuleCharacter::sweep(Vec3 feet, Vec3 displacement) const {
    const Capsule capsule = capsule_at(feet);
    auto hit = sweep_capsule_boxes(capsule, displacement, geometry_);
    if (triangles_.empty()) return hit;
    // The earliest contact over both kinds of surface, by the exact contact fraction (contact_t, double) rather than the rounded t; a box
    // wins an exact tie (it is checked first, and is the exact analytic result).
    bool from_triangle = false;
    if (const auto tri = sweep_capsule_triangles(capsule, displacement, triangles_)) {
        if (!hit || tri->contact_t < hit->contact_t) {
            hit = tri;
            from_triangle = true;
        }
    }
    if (!hit || geometry_.empty() || hit->started_overlapping || hit->t <= 0.0f) return hit;
    // Each primitive made its stop clear of its own kind of surface only. The stop of the surface that was hit must also be clear of the
    // other kind (a ramp meeting a wall), so judge it against both and back off if needed, the same rule the primitives use: the surface
    // that was hit exactly, the others within half the contact tolerance, none deeper than where the capsule started.
    constexpr double tolerance = 1e-5;
    const std::size_t hit_index = hit->index;
    const auto clear = [&](Vec3 pose) {
        Capsule posed = capsule;
        posed.feet = pose;
        for (std::size_t i = 0; i < geometry_.size(); ++i) {
            const double allowed = std::min(static_cast<double>(capsule_box_gap(capsule, geometry_[i])), 0.0) -
                                   ((!from_triangle && i == hit_index) ? 0.0 : 0.5 * tolerance);
            if (capsule_box_gap(posed, geometry_[i]) < allowed) return false;
        }
        for (std::size_t i = 0; i < triangles_.size(); ++i) {
            const double allowed = std::min(static_cast<double>(capsule_triangle_gap(capsule, triangles_[i])), 0.0) -
                                   ((from_triangle && i == hit_index) ? 0.0 : 0.5 * tolerance);
            if (capsule_triangle_gap(posed, triangles_[i]) < allowed) return false;
        }
        return true;
    };
    if (!clear(hit->stop)) {
        hit->stop = detail::conservative_stop(feet, displacement, hit->t, clear);
        const float length = std::sqrt(length2(displacement));
        const Vec3 moved = hit->stop - feet;
        hit->t = length > 0.0f ? std::clamp(dot(moved, displacement) / (length * length), 0.0f, hit->t) : 0.0f;
    }
    return hit;
}

bool CapsuleCharacter::overlaps(Vec3 feet, std::span<const Aabb> boxes, std::span<const Triangle> triangles) const {
    const Capsule capsule = capsule_at(feet);
    return std::any_of(boxes.begin(), boxes.end(), [&](const Aabb& box) { return capsule_box_push_out(capsule, box).has_value(); }) ||
           std::any_of(triangles.begin(), triangles.end(), [&](const Triangle& t) { return capsule_triangle_push_out(capsule, t).has_value(); });
}

bool CapsuleCharacter::recover_from_overlap() {
    // Normally a no-op. It exists for float rounding at large coordinates and for geometry that changed under the character. A bounded
    // number of passes resolves surfaces one after another; a capsule squeezed between surfaces that are closer than its width cannot be
    // separated from both, so the result says whether it ended up clear.
    //
    // A triangle has no thickness, so pushing out of one surface can carry the capsule straight through a thin one beside it and
    // report success on the wrong side of a wall. A push that would move the capsule to the other side of a triangle than the one it
    // started the recovery on is therefore a failure, not a solution (boxes have thickness, so they cannot be crossed this way).
    const Vec3 origin = state_.feet;
    // Transactional: a recovery that fails (it would cross a triangle, or cannot clear the capsule) leaves the feet where they were,
    // never at a half-resolved pose.
    const auto fail = [&] {
        state_.feet = origin;
        return false;
    };
    const auto apply_push = [&](const std::optional<Vec3>& push) {
        if (!push) return false;
        const float length = std::sqrt(length2(*push));
        state_.feet = state_.feet + *push + (length > 0.0f ? *push * (skin / length) : Vec3{});
        return true;
    };
    for (int pass = 0; pass < 4; ++pass) {
        bool moved = false;
        for (const Aabb& box : geometry_) moved = apply_push(capsule_box_push_out(capsule(), box)) || moved;
        for (const Triangle& triangle : triangles_) {
            const auto push = capsule_triangle_push_out(capsule(), triangle);
            if (!push) continue;
            const Vec3 normal = triangle_normal(triangle);
            const float half = config_.height * 0.5f;
            const float started_on = dot(normal, Vec3{origin.x, origin.y, origin.z + half} - triangle.a);
            if (started_on != 0.0f && dot(*push, normal) * started_on < 0.0f) return fail();  // would cross the triangle
            moved = apply_push(push) || moved;
        }
        if (!moved) return true;
    }
    return !overlaps(state_.feet, geometry_, triangles_) || fail();
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
        feet = hit->stop;  // the contact position as the sweep reconstructed it (clear in float), not feet + remaining * t
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
    const Vec3 landed = down->stop + Vec3{0.0f, 0.0f, skin};
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
        state_.feet = hit->stop + normal * skin;
        const Vec3 leftover = remaining * (1.0f - hit->t);

        const bool walkable = is_walkable(normal, config_.max_slope_radians);
        if (!walkable && normal.z > 0.0f) steep_contact_ = true;
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
    // Strong guarantee: a tick that throws (a non-finite input, or a capsule that has been carried outside the supported envelope by
    // earlier motion) leaves the character exactly as it was before the call.
    const CapsuleCharacterState saved = state_;
    const bool saved_jump = previous_jump_;
    const bool saved_steep = steep_contact_;
    try {
        step_unchecked(input, dt);
    } catch (...) {
        state_ = saved;
        previous_jump_ = saved_jump;
        steep_contact_ = saved_steep;
        throw;
    }
}

void CapsuleCharacter::step_unchecked(CapsuleInput input, std::chrono::nanoseconds dt) {
    if (dt <= std::chrono::nanoseconds::zero() || dt > max_step || !std::isfinite(input.forward) || !std::isfinite(input.left) || !std::isfinite(input.traction)) {
        throw std::invalid_argument("Capsule character step requires finite input and a fixed step in (0, 100 ms]");
    }
    const float seconds = std::chrono::duration<float>(dt).count();

    (void)recover_from_overlap();  // a trapped capsule cannot be freed here; it simply cannot move (every sweep starts overlapping)
    const bool was_sliding = steep_contact_;
    steep_contact_ = false;

    // Horizontal velocity moves toward the input's target, faster on the ground than in the air. Air control with no input is a brake
    // on the horizontal speed, which is right for a fall but wrong for a slide: on a surface too steep to stand on, an idle character
    // must keep the speed gravity gives it, or it creeps down the slope at a fraction of the speed it should have.
    const bool no_input = input.forward == 0.0f && input.left == 0.0f;
    const bool keep_horizontal_speed = !state_.grounded && was_sliding && no_input;
    const float speed = input.sprint ? config_.sprint_speed : config_.walk_speed;
    const float forward = std::clamp(input.forward, -1.0f, 1.0f);
    const float left = std::clamp(input.left, -1.0f, 1.0f);
    const float scale = speed / std::max(1.0f, std::hypot(forward, left));
    const float dx = forward * scale - state_.velocity.x;
    const float dy = left * scale - state_.velocity.y;
    const float change = std::hypot(dx, dy);
    const float traction = std::clamp(input.traction, 0.02f, 1.0f);
    const float limit = (state_.grounded ? config_.ground_acceleration * traction : config_.air_acceleration) * seconds;
    const float fraction = change > limit ? limit / change : 1.0f;
    if (!keep_horizontal_speed) {
        state_.velocity.x += dx * fraction;
        state_.velocity.y += dy * fraction;
    }

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
    return replace_static_geometry(geometry, std::span<const Triangle>{});
}

bool CapsuleCharacter::replace_static_geometry(std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
    validate(boxes);
    validate(triangles);
    if (overlaps(state_.feet, boxes, triangles) || overlaps(spawn_, boxes, triangles)) return false;
    std::vector<Aabb> candidate(boxes.begin(), boxes.end());
    std::vector<Triangle> candidate_triangles(triangles.begin(), triangles.end());
    const CapsuleCharacterState saved = state_;
    const bool saved_steep = steep_contact_;
    geometry_.swap(candidate);
    triangles_.swap(candidate_triangles);
    try {
        // The same ground probe as a step uses (it looks past a steep rim the capsule is touching), so a collision swap keeps the support
        // a character already had.
        const auto ground = probe_ground(state_.feet);
        state_.grounded = ground.has_value() && state_.velocity.z <= 0.0f;
        if (state_.grounded) {
            state_.ground_normal = ground->normal;
            state_.velocity.z = 0.0f;
        }
    } catch (...) {  // transactional: the old set and the old state stay in force
        geometry_.swap(candidate);
        triangles_.swap(candidate_triangles);
        state_ = saved;
        steep_contact_ = saved_steep;
        throw;
    }
    return true;
}

bool CapsuleCharacter::migrate_origin(Vec3 shift, std::span<const Aabb> boxes, std::span<const Triangle> triangles) {
    CapsuleCharacter candidate(*this);
    candidate.spawn_ = spawn_ + shift;
    candidate.state_.feet = state_.feet + shift;
    require_in_envelope(config_, candidate.spawn_);
    require_in_envelope(config_, candidate.state_.feet);
    if (!candidate.replace_static_geometry(boxes, triangles)) return false;
    *this = std::move(candidate);
    return true;
}

bool CapsuleCharacter::teleport(Vec3 feet) {
    if (!finite(feet)) throw std::invalid_argument("Capsule character teleport position must be finite");
    require_in_envelope(config_, feet);  // outside the supported range: rebase first; nothing has changed
    // Transactional: a blocked teleport (false) and a throwing one (the recovery or the ground probe leaving the supported range, as a capsule standing
    // at the edge of it does when the probe looks down) both leave the character exactly as it was.
    const CapsuleCharacterState saved = state_;
    const bool saved_steep = steep_contact_;
    try {
        state_.feet = feet;
        state_.velocity = {};
        state_.grounded = false;
        steep_contact_ = false;
        if (!recover_from_overlap()) {
            state_ = saved;
            steep_contact_ = saved_steep;
            return false;
        }
        snap_to_ground();
    } catch (...) {
        state_ = saved;
        steep_contact_ = saved_steep;
        throw;
    }
    return true;
}

bool CapsuleCharacter::remember_grounded_checkpoint() noexcept {
    if (!state_.grounded) return false;
    spawn_ = state_.feet;
    return true;
}

void CapsuleCharacter::reset() {
    // Strong guarantee: the new state is built aside and adopted only when the ground probe has succeeded.
    CapsuleCharacterState fresh;
    fresh.feet = spawn_;
    if (const auto ground = probe_ground(spawn_)) {  // the same probe as a step: a spawn touching a ledge's rim still counts as supported
        fresh.grounded = true;
        fresh.ground_normal = ground->normal;
    }
    state_ = fresh;
    steep_contact_ = false;
    previous_jump_ = false;
}

} // namespace saige
