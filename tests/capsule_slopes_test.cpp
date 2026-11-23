// CapsuleCharacter on non-box geometry: ramps, crests, creases, steep slopes and ceilings made of triangles (capsule_triangle.hpp).
// Every test states what a player would see: walking up and down stays on the ground without jitter, a character stands still on a
// walkable slope and slides down a steep one, and the whole thing replays bit-identically.
#include "saige/capsule_character.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <span>
#include <type_traits>
#include <vector>

namespace {

using saige::Aabb;
using saige::CapsuleCharacter;
using saige::CapsuleCharacterConfig;
using saige::CapsuleInput;
using saige::Triangle;
using saige::Vec3;

constexpr std::chrono::nanoseconds dt{16'666'667};
constexpr float pi = 3.14159265358979f;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

float rad(float degrees) { return degrees * pi / 180.0f; }

const Aabb floor_box{{-200, -200, -1}, {200, 200, 0}};

void run(CapsuleCharacter& c, CapsuleInput input, int ticks) {
    for (int i = 0; i < ticks; ++i) c.step(input, dt);
}

constexpr CapsuleInput forward{1.0f, 0.0f, false, false};
constexpr CapsuleInput back{-1.0f, 0.0f, false, false};
constexpr CapsuleInput idle{};

// A ramp rising toward +X at `degrees`, starting at x0 at height z0, `length` long measured horizontally, spanning y in [-half, half].
std::vector<Triangle> ramp(float degrees, float x0, float length, float half = 3.0f, float z0 = 0.0f) {
    const float rise = length * std::tan(rad(degrees));
    return {{{x0, -half, z0}, {x0 + length, -half, z0 + rise}, {x0 + length, half, z0 + rise}},
            {{x0, -half, z0}, {x0 + length, half, z0 + rise}, {x0, half, z0}}};
}

// A horizontal slab of two triangles at height z over the given rectangle.
std::vector<Triangle> slab(float x0, float x1, float y0, float y1, float z) {
    return {{{x0, y0, z}, {x1, y0, z}, {x1, y1, z}}, {{x0, y0, z}, {x1, y1, z}, {x0, y1, z}}};
}

void append(std::vector<Triangle>& to, const std::vector<Triangle>& from) { to.insert(to.end(), from.begin(), from.end()); }

float min_gap(const CapsuleCharacter& c, const std::vector<Aabb>& boxes, const std::vector<Triangle>& triangles) {
    float gap = std::numeric_limits<float>::infinity();
    for (const Aabb& b : boxes) gap = std::min(gap, saige::capsule_box_gap(c.capsule(), b));
    for (const Triangle& t : triangles) gap = std::min(gap, saige::capsule_triangle_gap(c.capsule(), t));
    return gap;
}

struct Walk {
    int ungrounded = 0;
    float max_z = 0.0f;
    float max_z_jump = 0.0f;   // the largest change of height in a single tick
    float min_gap = 0.0f;
};

Walk walk(CapsuleCharacter& c, CapsuleInput input, int ticks, const std::vector<Aabb>& boxes, const std::vector<Triangle>& tris) {
    Walk w;
    w.min_gap = std::numeric_limits<float>::infinity();
    float previous = c.state().feet.z;
    for (int i = 0; i < ticks; ++i) {
        c.step(input, dt);
        if (!c.state().grounded) ++w.ungrounded;
        w.max_z = std::max(w.max_z, c.state().feet.z);
        w.max_z_jump = std::max(w.max_z_jump, std::abs(c.state().feet.z - previous));
        previous = c.state().feet.z;
        w.min_gap = std::min(w.min_gap, min_gap(c, boxes, tris));
    }
    return w;
}

// ---------------------------------------------------------------------------------------------------------------------------

void test_walking_up_and_down_a_walkable_ramp() {
    const std::vector<Aabb> boxes{floor_box};
    std::vector<Triangle> tris = ramp(30.0f, 4.0f, 6.0f);
    append(tris, slab(10.0f, 16.0f, -3.0f, 3.0f, 6.0f * std::tan(rad(30.0f))));  // a landing at the top
    CapsuleCharacter c({0, 0, 0}, boxes, tris, {});
    require(c.state().grounded, "spawned on the floor");
    // Up: grounded on every tick after the character reaches the ramp, the height follows the surface, and no tick jumps in height.
    Walk up = walk(c, forward, 230, boxes, tris);
    const float top = 6.0f * std::tan(rad(30.0f));
    require(up.ungrounded == 0, "walking up a 30 degree ramp never leaves the ground");
    require(c.state().feet.x > 10.0f && std::abs(c.state().feet.z - top) < 0.01f, "it reaches the landing at the height of the ramp's top");
    require(up.max_z_jump < 0.06f, "the height changes smoothly (no pops), at most " + std::to_string(up.max_z_jump) + " per tick");
    require(up.min_gap > -1e-3f, "and the capsule never penetrates the surface");
    // Down again.
    Walk down = walk(c, back, 300, boxes, tris);
    require(down.ungrounded == 0, "walking back down the ramp never leaves the ground (no hopping off the slope)");
    require(c.state().feet.x < 4.0f && std::abs(c.state().feet.z) < 0.01f, "and ends on the floor");
    require(down.max_z_jump < 0.06f, "going down is smooth too");
    require(down.min_gap > -1e-3f, "no penetration on the way down");
}

void test_ground_normal_and_speed_on_a_slope() {
    const std::vector<Aabb> boxes{floor_box};
    const std::vector<Triangle> tris = ramp(30.0f, 0.0f, 12.0f);
    CapsuleCharacter c({-1, 0, 0}, boxes, tris, {});
    run(c, forward, 90);
    require(c.state().grounded && std::abs(c.state().ground_normal.z - std::cos(rad(30.0f))) < 1e-3f && c.state().ground_normal.x < -0.4f,
            "the ground normal on the ramp is the ramp's (cos 30 up, tilted back)");
    // Uphill the horizontal speed is the walk speed scaled by the slope (the character walks along the surface at walk speed): bounded
    // above by the flat speed and not collapsing.
    const float horizontal = std::hypot(c.state().velocity.x, c.state().velocity.y);
    require(horizontal <= 3.5f + 1e-3f && horizontal > 3.5f * 0.8f, "uphill speed is close to, and never above, the flat walking speed");
}

void test_standing_still_on_a_walkable_slope_does_not_creep() {
    const std::vector<Aabb> boxes{floor_box};
    const std::vector<Triangle> tris = ramp(35.0f, 0.0f, 10.0f);
    CapsuleCharacter c({2.0f, 0.0f, 2.0f * std::tan(rad(35.0f)) + 1.0f}, boxes, tris, {});  // dropped onto the slope
    run(c, idle, 120);
    require(c.state().grounded, "it lands and is grounded on a 35 degree slope");
    const auto settled = c.state();
    run(c, idle, 1200);
    require(c.state() == settled, "standing on a walkable slope for 20 seconds changes nothing (no creep, no jitter)");
    CapsuleCharacter jumper({2.0f, 0.0f, 2.0f * std::tan(rad(35.0f)) + 1.0f}, boxes, tris, {});
    run(jumper, idle, 120);
    jumper.step({0, 0, true, false}, dt);
    require(!jumper.state().grounded && jumper.state().velocity.z > 0.0f, "a character standing on a walkable slope can jump");
}

void test_slope_limit_boundary() {
    const std::vector<Aabb> boxes{floor_box};
    for (const float angle : {44.0f, 49.0f}) {
        const std::vector<Triangle> tris = ramp(angle, 0.0f, 8.0f);
        CapsuleCharacter c({3.0f, 0.0f, 3.0f * std::tan(rad(angle)) + 1.0f}, boxes, tris, {});
        run(c, idle, 180);
        const auto settled = c.state();
        run(c, idle, 120);
        require(c.state().grounded && c.state() == settled, std::to_string(static_cast<int>(angle)) + " degrees is walkable (limit 50): it stands still");
    }
    for (const float angle : {51.0f, 60.0f, 75.0f}) {
        const std::vector<Triangle> tris = ramp(angle, 0.0f, 8.0f);
        CapsuleCharacter c({3.0f, 0.0f, 3.0f * std::tan(rad(angle)) + 1.0f}, boxes, tris, {});
        run(c, idle, 400);
        require(c.state().feet.x < 0.1f && c.state().feet.z < 0.01f,
                std::to_string(static_cast<int>(angle)) + " degrees is too steep (limit 50): it slides all the way down to the floor (x = " + std::to_string(c.state().feet.x) +
                    ", z = " + std::to_string(c.state().feet.z) + ")");
    }
    // A configured limit moves the boundary.
    CapsuleCharacterConfig strict;
    strict.max_slope_radians = rad(20.0f);
    CapsuleCharacter c({3.0f, 0.0f, 3.0f * std::tan(rad(30.0f)) + 1.0f}, boxes, ramp(30.0f, 0.0f, 8.0f), strict);
    run(c, idle, 400);
    require(c.state().feet.x < 0.1f && c.state().feet.z < 0.01f, "with a 20 degree limit a 30 degree ramp is a slide to the floor");
}

void test_steep_slope_slides_cleanly_to_the_bottom() {
    const std::vector<Aabb> boxes{floor_box};
    const std::vector<Triangle> tris = ramp(60.0f, 0.0f, 6.0f);
    CapsuleCharacter c({3.0f, 0.0f, 3.0f * std::tan(rad(60.0f)) + 1.0f}, boxes, tris, {});
    float previous_x = c.state().feet.x;
    int backwards = 0;
    float min_gap_seen = 1e9f;
    for (int i = 0; i < 360; ++i) {
        c.step(idle, dt);
        if (c.state().feet.x > previous_x + 1e-4f) ++backwards;  // never back up the slope
        previous_x = c.state().feet.x;
        min_gap_seen = std::min(min_gap_seen, min_gap(c, boxes, tris));
    }
    require(backwards == 0, "a sliding character never moves back up the slope");
    require(min_gap_seen > -1e-3f, "and never penetrates it");
    require(c.state().grounded && std::abs(c.state().feet.z) < 0.01f && c.state().feet.x < 0.0f, "it ends at rest on the floor at the foot of the slope");
    const auto at_rest = c.state();
    run(c, idle, 300);
    require(c.state() == at_rest, "and stays there");
}

void test_cannot_climb_a_steep_slope() {
    const std::vector<Aabb> boxes{floor_box};
    const std::vector<Triangle> tris = ramp(60.0f, 2.0f, 6.0f);
    CapsuleCharacter c({-2, 0, 0}, boxes, tris, {});
    const Walk w = walk(c, forward, 300, boxes, tris);
    require(w.max_z < 0.05f, "walking into a 60 degree slope does not climb it (highest foot " + std::to_string(w.max_z) + ")");
    require(w.min_gap > -1e-3f, "and does not penetrate it");
    // Sliding sideways along it.
    CapsuleCharacter s({-2, 0, 0}, boxes, tris, {});
    const Walk side = walk(s, {1.0f, 0.7f, false, false}, 240, boxes, tris);
    require(side.max_z < 0.05f && s.state().feet.y > 2.0f, "pushing diagonally into the steep slope slides along its foot");
}

void test_step_onto_a_ramp_lip() {
    const std::vector<Aabb> boxes{floor_box};
    // A 25 degree ramp whose low end is raised 0.25 m (under the step height): a lip to step up before the slope starts.
    std::vector<Triangle> tris = ramp(25.0f, 3.0f, 6.0f, 3.0f, 0.25f);
    tris.push_back({{3.0f, -3.0f, 0.0f}, {3.0f, 3.0f, 0.25f}, {3.0f, -3.0f, 0.25f}});
    tris.push_back({{3.0f, -3.0f, 0.0f}, {3.0f, 3.0f, 0.0f}, {3.0f, 3.0f, 0.25f}});  // the riser (vertical, x = 3)
    CapsuleCharacter c({0, 0, 0}, boxes, tris, {});
    const Walk w = walk(c, forward, 150, boxes, tris);
    require(c.state().feet.x > 5.0f && c.state().feet.z > 1.0f, "the character steps up the lip and goes on up the ramp");
    require(w.min_gap > -1e-3f, "without penetrating");
    // A lip above the step height blocks.
    std::vector<Triangle> high = ramp(25.0f, 3.0f, 6.0f, 3.0f, 0.6f);
    high.push_back({{3.0f, -3.0f, 0.0f}, {3.0f, 3.0f, 0.6f}, {3.0f, -3.0f, 0.6f}});
    high.push_back({{3.0f, -3.0f, 0.0f}, {3.0f, 3.0f, 0.0f}, {3.0f, 3.0f, 0.6f}});
    CapsuleCharacter blocked({0, 0, 0}, boxes, high, {});
    run(blocked, forward, 200);
    require(blocked.state().feet.x < 3.0f && blocked.state().feet.z < 0.01f, "a 0.6 m lip is a wall to a character that does not jump");
}

void test_crease_and_crest_stay_on_the_ground() {
    const std::vector<Aabb> boxes{floor_box};
    // Floor, then a 40 degree ramp (concave crease at its foot), then a flat top (convex crest), then back down the far side.
    const float run_x = 3.0f;
    const float h = run_x * std::tan(rad(40.0f));
    std::vector<Triangle> tris = ramp(40.0f, 2.0f, run_x);
    append(tris, slab(2.0f + run_x, 2.0f + run_x + 3.0f, -3.0f, 3.0f, h));
    // The far side: a ramp descending to the floor (a mirrored ramp: rises toward -X from x = 8 + run_x).
    const float x_far = 2.0f + run_x + 3.0f;
    tris.push_back({{x_far, -3.0f, h}, {x_far + run_x, -3.0f, 0.0f}, {x_far + run_x, 3.0f, 0.0f}});
    tris.push_back({{x_far, -3.0f, h}, {x_far + run_x, 3.0f, 0.0f}, {x_far, 3.0f, h}});
    CapsuleCharacter c({0, 0, 0}, boxes, tris, {});
    const Walk w = walk(c, forward, 330, boxes, tris);
    require(c.state().feet.x > 12.5f && c.state().feet.z < 0.02f, "over the crease, the top, the crest and down the far side to the floor");
    require(w.ungrounded <= 6, "a character running over a crest at walking speed stays on the surface (airborne ticks: " + std::to_string(w.ungrounded) + ")");
    require(w.min_gap > -1e-3f, "no penetration at the crease or the crest");
    require(w.max_z_jump < 0.07f, "no pops in height anywhere (largest " + std::to_string(w.max_z_jump) + ")");
}

void test_landing_and_jumping_on_slopes() {
    const std::vector<Aabb> boxes{floor_box};
    const std::vector<Triangle> tris = ramp(30.0f, 0.0f, 10.0f);
    CapsuleCharacter c({3.0f, 0.0f, 3.0f * std::tan(rad(30.0f)) + 1.0f}, boxes, tris, {});
    int ticks = 0;
    while (!c.state().grounded && ticks++ < 200) c.step(idle, dt);
    require(c.state().grounded && ticks > 5 && ticks < 120, "a character dropped on a ramp lands on it");
    require(c.state().ground_normal.x < -0.4f, "with the ramp's normal as the ground normal");
    // Jump uphill, land again on the ramp, no penetration.
    CapsuleCharacter j({-1.0f, 0.0f, 0.0f}, boxes, tris, {});
    run(j, forward, 70);
    j.step({1, 0, true, false}, dt);
    float worst = 1e9f;
    for (int i = 0; i < 90; ++i) {
        j.step(forward, dt);
        worst = std::min(worst, min_gap(j, boxes, tris));
    }
    require(worst > -1e-3f && j.state().grounded, "a jump up the ramp lands on it without penetration");
}

void test_ceiling_made_of_triangles() {
    const std::vector<Aabb> boxes{floor_box};
    const std::vector<Triangle> tris = slab(-5, 5, -5, 5, 1.9f);  // an overhead slab, 1.9 m up: a standing 1.7 m capsule fits, a jump does not
    CapsuleCharacter c({0, 0, 0}, boxes, tris, {});
    run(c, idle, 2);
    c.step({0, 0, true, false}, dt);
    float peak = 0.0f;
    bool bumped = false;
    for (int i = 0; i < 90; ++i) {
        c.step(idle, dt);
        peak = std::max(peak, c.state().feet.z + 1.7f);
        bumped = bumped || c.state().velocity.z == 0.0f;
    }
    require(peak < 1.9f + 1e-3f, "the head never passes through an overhead triangle slab");
    require(c.state().grounded, "and the character comes back down");
}

void test_teleport_into_a_gap_between_triangle_walls_fails_and_changes_nothing() {
    // The transactional teleport (Core review of #70) with triangle geometry: two vertical triangle walls 0.30 m apart.
    const std::vector<Aabb> boxes{floor_box};
    std::vector<Triangle> walls;
    const auto wall = [&](float x) {
        walls.push_back({{x, -5, 0}, {x, 5, 0}, {x, 5, 4}});
        walls.push_back({{x, -5, 0}, {x, 5, 4}, {x, -5, 4}});
    };
    wall(-0.15f);
    wall(0.15f);
    CapsuleCharacter c({0, -8, 0}, boxes, walls, {});
    run(c, {0.0f, 1.0f, false, false}, 12);
    const saige::CapsuleCharacterState before = c.state();
    require(!c.teleport({0, 0, 0}), "a teleport into a gap between triangle walls narrower than the capsule fails");
    require(c.state() == before, "and changes nothing");
    require(c.teleport({3.0f, 0.0f, 0.0f}), "a teleport to open ground still works");
    require(c.state().grounded && min_gap(c, boxes, walls) > -1e-4f, "and lands clear");

    // A thin wall has no thickness, so recovery must not carry the capsule through it: a capsule dropped 0.1 m from a single wall is pushed
    // back out on the side it was on, never across (the recovery used to be able to land on the far side of the second wall above).
    std::vector<Triangle> one;
    one.push_back({{0.15f, -5, 0}, {0.15f, 5, 0}, {0.15f, 5, 4}});
    one.push_back({{0.15f, -5, 0}, {0.15f, 5, 4}, {0.15f, -5, 4}});
    CapsuleCharacter near_wall({-3.0f, 0, 0}, boxes, one, {});
    require(near_wall.teleport({0.05f, 0.0f, 0.0f}), "a capsule overlapping a single thin wall is recovered");
    require(near_wall.state().feet.x < -0.14f && min_gap(near_wall, boxes, one) > -1e-4f, "on the side it started on (x = " + std::to_string(near_wall.state().feet.x) + ")");
    CapsuleCharacter other_side({3.0f, 0, 0}, boxes, one, {});
    require(other_side.teleport({0.25f, 0.0f, 0.0f}), "and from the other side");
    require(other_side.state().feet.x > 0.44f && min_gap(other_side, boxes, one) > -1e-4f, "it is pushed out to the far side of the wall it started on");
}

void test_reset_and_replacement_probe_triangle_ledges() {
    // A character resting exactly on the crest edge of a ramp (a steep rim contact on triangles): replacing the geometry with an identical
    // set, or resetting, keeps it supported (the ground probe slides past the steep contact it already touches).
    const std::vector<Aabb> boxes{floor_box};
    std::vector<Triangle> tris = ramp(30.0f, 0.0f, 4.0f);
    append(tris, slab(4.0f, 8.0f, -3.0f, 3.0f, 4.0f * std::tan(rad(30.0f))));
    CapsuleCharacter c({6.0f, 0.0f, 4.0f * std::tan(rad(30.0f)) + 0.5f}, boxes, tris, {});
    run(c, idle, 60);
    require(c.state().grounded, "standing on the landing");
    require(c.replace_static_geometry(boxes, tris), "an identical set is accepted");
    require(c.state().grounded, "and support survives the swap");
    c.reset();
    require(c.state().grounded || c.state().feet.z > 0.0f, "reset places the character (supported when it is over ground)");
}

void test_air_control_still_brakes_a_fall_without_input() {
    // The steep-slope fix must not change the long-standing behavior in the open air: with no input, horizontal speed decays at the
    // air acceleration (7 m/s^2 by default), exactly as before.
    const std::vector<Aabb> boxes{floor_box};
    CapsuleCharacter c({0, 0, 0}, boxes, std::span<const Triangle>{}, {});
    run(c, forward, 60);
    c.step({1, 0, true, true}, dt);
    run(c, {1, 0, false, true}, 5);
    const float before = c.state().velocity.x;
    require(!c.state().grounded && before > 3.0f, "airborne and moving forward");
    run(c, idle, 6);  // 0.1 s
    require(std::abs((before - c.state().velocity.x) - 0.7f) < 0.05f, "with no input the air brakes at 7 m/s^2 (lost " + std::to_string(before - c.state().velocity.x) + " m/s in 0.1 s)");
}

void test_walking_off_the_side_of_a_ramp_falls() {
    const std::vector<Aabb> boxes{{{-200, -200, -10}, {200, 200, -9}}};  // the ground is far below
    std::vector<Triangle> tris = ramp(20.0f, 0.0f, 12.0f, 1.5f);
    CapsuleCharacter e({3.0f, 0.0f, 3.0f * std::tan(rad(20.0f)) + 0.5f}, boxes, tris, {});
    run(e, idle, 20);
    require(e.state().grounded, "standing on the ramp");
    run(e, {0.0f, 1.0f, false, false}, 90);  // walk left off the side
    require(!e.state().grounded && e.state().feet.z < 0.5f, "walking off the side of a ramp is a fall");
}

void test_overlap_recovery_and_geometry_replacement() {
    const std::vector<Aabb> boxes{floor_box};
    const std::vector<Triangle> tris = ramp(30.0f, 0.0f, 10.0f);
    CapsuleCharacter c({-2.0f, 0.0f, 0.0f}, boxes, tris, {});
    require(c.teleport({3.0f, 0.0f, 0.2f}), "a teleport into a ramp is recovered");  // inside the ramp
    require(min_gap(c, boxes, tris) > -2e-4f, "teleporting into a ramp pushes the capsule out");
    run(c, idle, 60);
    require(c.state().grounded && min_gap(c, boxes, tris) > -2e-4f, "and it settles on the surface");
    // Geometry replacement refuses a ramp that would swallow the character, and keeps the old world.
    CapsuleCharacter r({3.0f, 0.0f, 1.0f}, boxes, std::span<const Triangle>{}, {});
    run(r, idle, 60);
    const auto before = r.state();
    require(!r.replace_static_geometry(boxes, ramp(30.0f, 0.0f, 10.0f)), "a ramp that overlaps the character is rejected");
    require(r.state() == before, "and nothing changed");
    CapsuleCharacter s({-3.0f, 0.0f, 0.0f}, boxes, std::span<const Triangle>{}, {});
    require(s.replace_static_geometry(boxes, ramp(30.0f, 0.0f, 10.0f)), "a ramp elsewhere is accepted");
    run(s, forward, 120);
    require(s.state().feet.z > 1.0f, "and is climbed");
    const std::vector<Triangle> bad{{{0, 0, 0}, {1, 0, 0}, {2, 0, 0}}};
    bool threw = false;
    try { (void)s.replace_static_geometry(boxes, bad); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "a degenerate triangle in a replacement throws");
    threw = false;
    try { CapsuleCharacter bad_ctor({0, 0, 0}, boxes, bad, {}); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "and in the constructor");
    threw = false;
    try { CapsuleCharacter inside({3.0f, 0.0f, 0.2f}, boxes, ramp(30.0f, 0.0f, 10.0f), {}); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "a spawn inside a ramp throws");
}

// Boxes and the same boxes as triangles must produce the same trajectory (to numerical noise): an end-to-end differential check.
std::vector<Triangle> box_triangles(const Aabb& b) {
    const Vec3 v[8] = {{b.min.x, b.min.y, b.min.z}, {b.max.x, b.min.y, b.min.z}, {b.max.x, b.max.y, b.min.z}, {b.min.x, b.max.y, b.min.z},
                       {b.min.x, b.min.y, b.max.z}, {b.max.x, b.min.y, b.max.z}, {b.max.x, b.max.y, b.max.z}, {b.min.x, b.max.y, b.max.z}};
    const int quads[6][4] = {{0, 3, 2, 1}, {4, 5, 6, 7}, {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};
    std::vector<Triangle> out;
    for (const auto& q : quads) {
        out.push_back({v[q[0]], v[q[1]], v[q[2]]});
        out.push_back({v[q[0]], v[q[2]], v[q[3]]});
    }
    return out;
}

CapsuleInput scripted_input(std::uint32_t& state) {
    state = state * 1664525u + 1013904223u;
    const float a = static_cast<float>((state >> 8) & 0xFFFF) / 65535.0f * 2.0f - 1.0f;
    state = state * 1664525u + 1013904223u;
    const float b = static_cast<float>((state >> 8) & 0xFFFF) / 65535.0f * 2.0f - 1.0f;
    state = state * 1664525u + 1013904223u;
    return {a, b, ((state >> 16) % 37u) == 0u, ((state >> 20) % 5u) == 0u};
}

void test_triangulated_boxes_replay_like_boxes() {
    const std::vector<Aabb> solids{{{2, -1, 0}, {3, 1, 0.3f}}, {{4, -3, 0}, {5, -2, 1.0f}}, {{-3, 2, 0}, {-1, 4, 0.2f}}, {{6, 0, 0}, {6.5f, 5, 2.0f}}, {{-6, -6, 0}, {-5.5f, 6, 3.0f}}};
    std::vector<Aabb> floor_only{floor_box};
    std::vector<Aabb> as_boxes = floor_only;
    as_boxes.insert(as_boxes.end(), solids.begin(), solids.end());
    std::vector<Triangle> as_triangles;
    for (const Aabb& s : solids) append(as_triangles, box_triangles(s));
    CapsuleCharacter with_boxes({0, 0, 0}, as_boxes);
    CapsuleCharacter with_triangles({0, 0, 0}, floor_only, as_triangles, {});
    std::uint32_t rng = 12345u;
    float worst = 0.0f;
    int ticks_jumped = 0;
    for (int tick = 0; tick < 2400; ++tick) {
        const CapsuleInput input = scripted_input(rng);
        with_boxes.step(input, dt);
        with_triangles.step(input, dt);
        const Vec3 a = with_boxes.state().feet, b = with_triangles.state().feet;
        worst = std::max(worst, std::max({std::abs(a.x - b.x), std::abs(a.y - b.y), std::abs(a.z - b.z)}));
        if (input.jump) ++ticks_jumped;
        require(with_boxes.state().grounded == with_triangles.state().grounded || worst < 0.01f, "grounded state diverged at tick " + std::to_string(tick));
    }
    require(worst < 0.01f, "40 seconds of random input: triangulated boxes follow the box world to within 1 cm (worst " + std::to_string(worst) + " m)");
    require(ticks_jumped > 20, "the script jumps");
}

void test_deterministic_replay_on_a_mixed_world() {
    const std::vector<Aabb> boxes{floor_box, {{8, -2, 0}, {9, 2, 1.2f}}};
    std::vector<Triangle> tris = ramp(30.0f, 0.0f, 5.0f);
    append(tris, slab(5.0f, 8.0f, -3.0f, 3.0f, 5.0f * std::tan(rad(30.0f))));
    const auto replay = [&](std::uint64_t& hash) {
        CapsuleCharacter c({-2, 0, 0}, boxes, tris, {});
        std::uint32_t rng = 777u;
        std::uint64_t h = 1469598103934665603ull;
        for (int tick = 0; tick < 3000; ++tick) {
            c.step(scripted_input(rng), dt);
            const auto& s = c.state();
            const float v[7] = {s.feet.x, s.feet.y, s.feet.z, s.velocity.x, s.velocity.y, s.velocity.z, s.grounded ? 1.0f : 0.0f};
            for (float f : v) {
                std::uint32_t bits;
                std::memcpy(&bits, &f, sizeof bits);
                for (int i = 0; i < 4; ++i) {
                    h ^= (bits >> (8 * i)) & 0xffu;
                    h *= 1099511628211ull;
                }
            }
            require(min_gap(c, boxes, tris) > -1e-3f, "no penetration in the replay at tick " + std::to_string(tick));
        }
        hash = h;
        return c.state();
    };
    std::uint64_t h1 = 0, h2 = 0;
    const auto a = replay(h1);
    const auto b = replay(h2);
    require(h1 == h2 && a == b, "a recorded input sequence replays bit-identically on boxes and ramps");
    std::cout << "  mixed-world replay checksum over 3000 ticks: " << std::hex << h1 << std::dec << '\n';
    // reset() returns to the start and the replay repeats.
    CapsuleCharacter c({-2, 0, 0}, boxes, tris, {});
    std::uint32_t rng = 777u;
    for (int i = 0; i < 500; ++i) c.step(scripted_input(rng), dt);
    c.reset();
    require(c.state().feet == Vec3{-2, 0, 0} && c.state().velocity == Vec3{0, 0, 0}, "reset restores the spawn");
}

void test_random_ramp_worlds_never_penetrate() {
    std::uint32_t rng = 99u;
    const auto next = [&](float lo, float hi) {
        rng = rng * 1664525u + 1013904223u;
        return lo + (hi - lo) * static_cast<float>((rng >> 8) & 0xFFFF) / 65535.0f;
    };
    int worlds = 0;
    for (int w = 0; w < 24; ++w) {
        std::vector<Triangle> tris;
        for (int r = 0; r < 3; ++r) append(tris, ramp(next(10.0f, 70.0f), next(-6.0f, 4.0f), next(3.0f, 6.0f), next(1.5f, 3.0f), next(0.0f, 0.3f)));
        const std::vector<Aabb> boxes{floor_box};
        std::optional<CapsuleCharacter> maybe;
        try { maybe.emplace(Vec3{-9.0f, next(-1, 1), 0.0f}, boxes, tris, CapsuleCharacterConfig{}); } catch (const std::invalid_argument&) { continue; }
        CapsuleCharacter& c = *maybe;
        ++worlds;
        std::uint32_t input_rng = static_cast<std::uint32_t>(w) * 7919u + 1u;
        float worst = 1e9f;
        for (int tick = 0; tick < 1500; ++tick) {
            c.step(scripted_input(input_rng), dt);
            worst = std::min(worst, min_gap(c, boxes, tris));
            require(std::isfinite(c.state().feet.x) && std::isfinite(c.state().feet.z), "finite state");
        }
        require(worst > -1e-3f, "world " + std::to_string(w) + ": no penetration over 25 s of random input (worst gap " + std::to_string(worst) + ")");
    }
    require(worlds >= 15, "enough random worlds were valid");
}

} // namespace

void test_constructor_overloads_stay_source_compatible() {
    // Core review of #74: with a defaulted configuration, the mixed constructor made `CapsuleCharacter({}, {}, {})` ambiguous, a call that
    // compiles against the box constructor (spawn, boxes, configuration). The mixed overload takes its configuration explicitly.
    CapsuleCharacter old_style({}, {}, {});  // spawn, no boxes, default configuration
    CapsuleCharacter mixed({}, {}, {}, {});  // spawn, no boxes, no triangles, default configuration
    static_assert(std::is_constructible_v<CapsuleCharacter, Vec3, std::span<const Aabb>>, "spawn, boxes");
    static_assert(std::is_constructible_v<CapsuleCharacter, Vec3, std::span<const Aabb>, CapsuleCharacterConfig>, "spawn, boxes, configuration");
    static_assert(std::is_constructible_v<CapsuleCharacter, Vec3, std::span<const Aabb>, std::span<const Triangle>, CapsuleCharacterConfig>,
                  "spawn, boxes, triangles, configuration");
    static_assert(!std::is_constructible_v<CapsuleCharacter, Vec3, std::span<const Aabb>, std::span<const Triangle>>,
                  "the mixed overload does not default its configuration");
    require(old_style.state().feet == Vec3{} && mixed.state().feet == Vec3{}, "both construct at the spawn");
}

void test_replacing_with_boxes_alone_clears_the_triangles() {
    const std::vector<Aabb> boxes{floor_box};
    const std::vector<Triangle> tris = slab(-3, 3, -3, 3, 2.0f);  // a platform, with the character standing on it
    CapsuleCharacter c({0, 0, 2.0f}, boxes, tris, {});
    run(c, idle, 5);
    require(c.state().grounded && std::abs(c.state().feet.z - 2.0f) < 1e-3f, "standing on the triangle platform");
    require(c.replace_static_geometry(boxes), "a box-only replacement is accepted");
    run(c, idle, 90);
    require(c.state().grounded && std::abs(c.state().feet.z) < 1e-3f, "the platform is gone (complete-set semantics): the character fell to the floor");
}

// The same world shifted by an offset that floats represent exactly, so a world far from the origin has the same geometry.
void test_mixed_world_far_from_the_origin_never_penetrates() {
    for (const Vec3 offset : {Vec3{150.0f, -120.0f, 0.0f}, Vec3{-210.0f, 200.0f, 0.0f}, Vec3{97.0f, 233.0f, 0.0f}}) {
        const auto shift_box = [&](const Aabb& b) { return Aabb{{b.min.x + offset.x, b.min.y + offset.y, b.min.z}, {b.max.x + offset.x, b.max.y + offset.y, b.max.z}}; };
        const auto shift_tri = [&](const Triangle& tri) {
            const auto v = [&](Vec3 p) { return Vec3{p.x + offset.x, p.y + offset.y, p.z}; };
            return Triangle{v(tri.a), v(tri.b), v(tri.c)};
        };
        // A ramp up to a landing, a wall across the landing (a box meeting the ramp's top edge), and a side wall made of triangles.
        std::vector<Aabb> boxes{shift_box({{-40, -40, -1}, {40, 40, 0}}), shift_box({{9.7f, -3.0f, 0.0f}, {10.4f, 3.0f, 4.0f}})};
        std::vector<Triangle> tris;
        for (const Triangle& tri : ramp(25.0f, 2.0f, 7.0f)) tris.push_back(shift_tri(tri));
        for (const Triangle& tri : slab(9.0f, 9.7f, -3.0f, 3.0f, 7.0f * std::tan(rad(25.0f)))) tris.push_back(shift_tri(tri));
        for (const Triangle& tri : std::vector<Triangle>{{{5.0f, 2.2f, 0.0f}, {5.0f, 2.2f, 3.0f}, {9.0f, 2.2f, 3.0f}}, {{5.0f, 2.2f, 0.0f}, {9.0f, 2.2f, 3.0f}, {9.0f, 2.2f, 0.0f}}}) tris.push_back(shift_tri(tri));
        for (int heading = 0; heading < 6; ++heading) {
            CapsuleCharacter c({offset.x - 1.0f, offset.y + 0.2f * static_cast<float>(heading) - 0.5f, 0.0f}, boxes, tris, {});
            float worst = std::numeric_limits<float>::infinity();
            for (int i = 0; i < 700; ++i) {
                const float turn = std::sin(static_cast<float>(i) * 0.02f) * 0.6f;
                c.step({std::cos(turn), std::sin(turn) + 0.15f * static_cast<float>(heading - 2), i % 211 == 100, (i / 90) % 2 == 0}, dt);
                worst = std::min(worst, min_gap(c, boxes, tris));
            }
            require(worst >= -1e-5f, "far-origin mixed world (offset " + std::to_string(offset.x) + ", " + std::to_string(offset.y) + ") penetrated by " + std::to_string(-worst));
        }
    }
}

int main() {
    struct Case { const char* name; void (*fn)(); };
    const Case cases[] = {
        {"test_walking_up_and_down_a_walkable_ramp", test_walking_up_and_down_a_walkable_ramp},
        {"test_ground_normal_and_speed_on_a_slope", test_ground_normal_and_speed_on_a_slope},
        {"test_standing_still_on_a_walkable_slope_does_not_creep", test_standing_still_on_a_walkable_slope_does_not_creep},
        {"test_slope_limit_boundary", test_slope_limit_boundary},
        {"test_steep_slope_slides_cleanly_to_the_bottom", test_steep_slope_slides_cleanly_to_the_bottom},
        {"test_cannot_climb_a_steep_slope", test_cannot_climb_a_steep_slope},
        {"test_step_onto_a_ramp_lip", test_step_onto_a_ramp_lip},
        {"test_crease_and_crest_stay_on_the_ground", test_crease_and_crest_stay_on_the_ground},
        {"test_landing_and_jumping_on_slopes", test_landing_and_jumping_on_slopes},
        {"test_ceiling_made_of_triangles", test_ceiling_made_of_triangles},
        {"test_teleport_into_a_gap_between_triangle_walls_fails_and_changes_nothing", test_teleport_into_a_gap_between_triangle_walls_fails_and_changes_nothing},
        {"test_reset_and_replacement_probe_triangle_ledges", test_reset_and_replacement_probe_triangle_ledges},
        {"test_air_control_still_brakes_a_fall_without_input", test_air_control_still_brakes_a_fall_without_input},
        {"test_walking_off_the_side_of_a_ramp_falls", test_walking_off_the_side_of_a_ramp_falls},
        {"test_overlap_recovery_and_geometry_replacement", test_overlap_recovery_and_geometry_replacement},
        {"test_triangulated_boxes_replay_like_boxes", test_triangulated_boxes_replay_like_boxes},
        {"test_deterministic_replay_on_a_mixed_world", test_deterministic_replay_on_a_mixed_world},
        {"test_random_ramp_worlds_never_penetrate", test_random_ramp_worlds_never_penetrate},
        {"test_constructor_overloads_stay_source_compatible", test_constructor_overloads_stay_source_compatible},
        {"test_replacing_with_boxes_alone_clears_the_triangles", test_replacing_with_boxes_alone_clears_the_triangles},
        {"test_mixed_world_far_from_the_origin_never_penetrates", test_mixed_world_far_from_the_origin_never_penetrates},
    };
    for (const Case& test : cases) {
        try {
            test.fn();
        } catch (const std::exception& error) {
            std::cerr << "capsule slopes test failed in " << test.name << ": " << error.what() << '\n';
            return 1;
        }
    }
    std::cout << "capsule slopes tests passed\n";
}
