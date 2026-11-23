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
#include <vector>

namespace {

using saige::Aabb;
using saige::CapsuleCharacter;
using saige::CapsuleCharacterConfig;
using saige::CapsuleCharacterState;
using saige::CapsuleInput;
using saige::Vec3;

constexpr std::chrono::nanoseconds dt{16'666'667};

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

bool near(double a, double b, double tolerance) {
    return std::abs(a - b) <= tolerance;
}

const Aabb floor_box{{-200, -200, -1}, {200, 200, 0}};

std::vector<Aabb> with_floor(std::vector<Aabb> boxes) {
    boxes.insert(boxes.begin(), floor_box);
    return boxes;
}

void run(CapsuleCharacter& c, CapsuleInput input, int ticks) {
    for (int i = 0; i < ticks; ++i) c.step(input, dt);
}

// The smallest gap between the capsule and any box. Negative means penetration (exact while the capsule axis is outside the box).
float min_gap(const CapsuleCharacter& c, const std::vector<Aabb>& boxes) {
    float gap = std::numeric_limits<float>::infinity();
    for (const Aabb& box : boxes) gap = std::min(gap, saige::capsule_box_gap(c.capsule(), box));
    return gap;
}

constexpr CapsuleInput forward{1.0f, 0.0f, false, false};
constexpr CapsuleInput idle{};

void test_rests_on_the_floor_without_drift() {
    const auto boxes = with_floor({});
    CapsuleCharacter c({1.0f, 2.0f, 0.0f}, boxes);
    require(c.state().grounded, "spawned on the floor and grounded");
    run(c, idle, 2);
    const CapsuleCharacterState settled = c.state();
    run(c, idle, 600);
    require(c.state() == settled, "ten seconds of standing still change nothing at all (no jitter, no drift)");
    require(c.state().grounded && c.state().velocity == Vec3{0, 0, 0}, "grounded at rest");
    require(near(c.state().feet.z, 0.0, 5e-4), "feet rest on the floor within the contact skin");
    require(near(c.state().feet.x, 1.0, 1e-6) && near(c.state().feet.y, 2.0, 1e-6), "no horizontal drift");
}

void test_falls_and_lands() {
    const auto boxes = with_floor({});
    CapsuleCharacter c({0, 0, 2.0f}, boxes);
    require(!c.state().grounded, "spawned in the air");
    int ticks = 0;
    while (!c.state().grounded && ticks < 200) {
        c.step(idle, dt);
        ++ticks;
        require(min_gap(c, boxes) > -2e-4f, "never penetrates the floor while falling");
    }
    // t = sqrt(2h/g) = 0.638 s = 38.3 ticks; the discrete integration lands within a tick or two of that.
    require(ticks >= 37 && ticks <= 41, "lands when free fall says: " + std::to_string(ticks) + " ticks");
    require(near(c.state().feet.z, 0.0, 1e-3) && c.state().velocity.z == 0.0f, "stands on the floor at rest");
}

void test_wall_stops_and_slides() {
    const auto boxes = with_floor({{{3, -50, 0}, {4, 50, 3}}});
    CapsuleCharacter c({0, 0, 0}, boxes);
    run(c, forward, 300);
    require(c.state().feet.x <= 3.0f - 0.3f + 1e-4f && c.state().feet.x >= 3.0f - 0.3f - 0.01f, "stops at the wall, one radius away");
    require(c.state().velocity.x == 0.0f && c.state().grounded, "no velocity into the wall, still grounded");
    const Vec3 stuck = c.state().feet;
    run(c, forward, 120);
    require(near(c.state().feet.x, stuck.x, 1e-5) && near(c.state().feet.y, stuck.y, 1e-6), "pressing into a wall is stable");

    // Walking diagonally into it slides along it at the full tangential speed's share.
    CapsuleCharacter d({0, 0, 0}, boxes);
    run(d, {1.0f, 1.0f, false, false}, 300);
    require(d.state().feet.x <= 3.0f - 0.3f + 1e-4f, "sliding does not enter the wall");
    require(d.state().feet.y > 10.0f, "keeps moving along the wall: y = " + std::to_string(d.state().feet.y));
    require(min_gap(d, boxes) > -2e-4f, "no penetration while sliding");
}

void test_corner_holds() {
    const auto boxes = with_floor({{{3, -50, 0}, {4, 50, 3}}, {{-50, 3, 0}, {50, 4, 3}}});
    CapsuleCharacter c({0, 0, 0}, boxes);
    run(c, {1.0f, 1.0f, false, true}, 400);
    require(near(c.state().feet.x, 2.7, 0.02) && near(c.state().feet.y, 2.7, 0.02), "settles in the corner");
    const Vec3 held = c.state().feet;
    run(c, {1.0f, 1.0f, false, true}, 200);
    require(near(c.state().feet.x, held.x, 1e-5) && near(c.state().feet.y, held.y, 1e-5), "stable in the corner, no jitter");
    require(min_gap(c, boxes) > -2e-4f, "no penetration in the corner");
}

void test_steps() {
    const auto stepped = [](float height) { return with_floor({{{2, -50, 0}, {8, 50, height}}}); };
    {
        const auto boxes = stepped(0.30f);
        CapsuleCharacter c({0, 0, 0}, boxes);
        run(c, forward, 80);  // 80 ticks at 3.5 m/s is 4.7 m: past the edge at x = 2, still over the 6 m long top
        require(near(c.state().feet.z, 0.30, 2e-3) && c.state().grounded, "climbs a 0.30 m step and stands on it");
        require(c.state().feet.x > 4.0f, "and keeps walking across the top: x = " + std::to_string(c.state().feet.x));
        require(min_gap(c, boxes) > -2e-4f, "no penetration while stepping");
    }
    {
        const auto boxes = stepped(0.34f);
        CapsuleCharacter c({0, 0, 0}, boxes);
        run(c, forward, 80);
        require(near(c.state().feet.z, 0.34, 2e-3), "climbs a 0.34 m step (just under the 0.35 m limit)");
    }
    {
        const auto boxes = stepped(0.40f);
        CapsuleCharacter c({0, 0, 0}, boxes);
        run(c, forward, 200);
        require(near(c.state().feet.z, 0.0, 1e-3) && c.state().feet.x <= 2.0f - 0.3f + 1e-4f, "a 0.40 m ledge is a wall");
    }
    {
        // In the air there is no step-up: a jump too low to clear the ledge hits it.
        const auto boxes = stepped(0.40f);
        CapsuleCharacter c({0, 0, 0}, boxes);
        run(c, forward, 40);
        require(c.state().feet.x < 2.0f, "still before the ledge");
    }
    {
        // Step down is a ground snap: a small drop never leaves the ground.
        const std::vector<Aabb> boxes{{{-200, -200, -1}, {2, 200, 0}}, {{2, -200, -1}, {200, 200, -0.08f}}};
        CapsuleCharacter c({0, 0, 0}, boxes);
        bool airborne = false;
        for (int i = 0; i < 200; ++i) {
            c.step(forward, dt);
            airborne = airborne || !c.state().grounded;
        }
        require(!airborne && near(c.state().feet.z, -0.08, 2e-3), "walks down a 0.08 m drop without leaving the ground");
    }
}

void test_ledge_drop_is_a_fall() {
    const std::vector<Aabb> boxes{{{-200, -200, -1}, {5, 200, 0}}, {{5, -200, -3}, {200, 200, -2}}};
    CapsuleCharacter c({0, 0, 0}, boxes);
    bool left_the_ground = false;
    float lowest_velocity = 0.0f;
    for (int i = 0; i < 400; ++i) {
        c.step(forward, dt);
        left_the_ground = left_the_ground || !c.state().grounded;
        lowest_velocity = std::min(lowest_velocity, c.state().velocity.z);
        require(min_gap(c, boxes) > -2e-4f, "no penetration over the ledge");
    }
    require(left_the_ground && lowest_velocity < -3.0f, "a 2 m drop is a fall");
    require(c.state().grounded && near(c.state().feet.z, -2.0, 2e-3), "and lands on the lower floor");
}

void test_jump_rules() {
    const auto boxes = with_floor({});
    CapsuleCharacter c({0, 0, 0}, boxes);
    run(c, idle, 2);
    float apex = 0.0f;
    int takeoffs = 0;
    bool was_grounded = true;
    // Hold jump for 300 ticks: one jump, then no automatic repeat on landing.
    for (int i = 0; i < 300; ++i) {
        c.step({0, 0, true, false}, dt);
        apex = std::max(apex, c.state().feet.z);
        if (was_grounded && !c.state().grounded) ++takeoffs;
        was_grounded = c.state().grounded;
    }
    require(takeoffs == 1, "holding the button jumps once: " + std::to_string(takeoffs));
    // v^2 / 2g = 0.987 m; the semi-implicit step puts the discrete apex a little below.
    require(apex > 0.93f && apex < 1.0f, "apex height: " + std::to_string(apex));
    require(c.state().grounded, "landed again");

    // Releasing and pressing again jumps again.
    run(c, idle, 2);
    c.step({0, 0, true, false}, dt);
    require(!c.state().grounded && c.state().velocity.z > 3.0f, "a fresh press jumps");

    // No double jump: a new press in the air does nothing.
    run(c, idle, 5);
    const float vz = c.state().velocity.z;
    c.step({0, 0, true, false}, dt);
    require(near(c.state().velocity.z, vz - 9.81 / 60.0, 1e-4), "pressing in the air only falls");
}

void test_ceiling() {
    const auto boxes = with_floor({{{-50, -50, 1.9f}, {50, 50, 3.0f}}});  // 1.9 m above the floor: 0.2 m of headroom
    CapsuleCharacter c({0, 0, 0}, boxes);
    run(c, idle, 2);
    float apex = 0.0f;
    for (int i = 0; i < 120; ++i) {
        c.step({0, 0, i == 0, false}, dt);
        apex = std::max(apex, c.state().feet.z);
        require(min_gap(c, boxes) > -2e-4f, "never penetrates the ceiling");
    }
    require(apex <= 0.2f + 1e-3f && apex > 0.15f, "the jump stops at the ceiling: " + std::to_string(apex));
    require(c.state().grounded, "and the character comes back down");
}

void test_rim_slope_limit() {
    // Standing on the rim of a pillar: the contact normal tilts as more of the capsule overhangs. 50 degrees is the limit, so
    // a sphere center 0.1 m past the edge (tilt 19 degrees) holds and 0.28 m past it (69 degrees) slides off.
    const std::vector<Aabb> boxes{{{-5, -5, -10}, {1, 5, 0}}, {{-200, -200, -12}, {200, 200, -10}}};
    {
        CapsuleCharacter c({1.1f, 0, 0.1f}, boxes);
        run(c, idle, 300);
        require(c.state().grounded && near(c.state().feet.x, 1.1, 1e-3), "a 19 degree rim contact is ground and holds position");
        require(c.state().ground_normal.z > std::cos(0.87266463) && near(c.state().ground_normal.x, 0.1 / 0.3, 0.02), "the ground normal is the tilted rim normal");
    }
    {
        CapsuleCharacter c({1.28f, 0, 0.1f}, boxes);
        bool grounded_ever = false;
        for (int i = 0; i < 300; ++i) {
            c.step(idle, dt);
            grounded_ever = grounded_ever || (c.state().grounded && c.state().feet.z > -1.0f);
            require(min_gap(c, boxes) > -2e-4f, "no penetration sliding off the rim");
        }
        require(!grounded_ever && c.state().feet.z < -9.0f && c.state().feet.x > 1.3f, "a 69 degree rim contact is not ground: the character slides off and falls");
    }
}

void test_no_tunneling_at_terminal_speed() {
    const std::vector<Aabb> boxes{{{-200, -200, -0.05f}, {200, 200, 0}}};  // a 5 cm slab
    CapsuleCharacter c({0, 0, 120.0f}, boxes);
    float fastest = 0.0f;
    for (int i = 0; i < 400 && !c.state().grounded; ++i) {
        c.step(idle, dt);
        fastest = std::min(fastest, c.state().velocity.z);
    }
    require(fastest <= -29.9f, "reached terminal speed");
    require(c.state().grounded && c.state().feet.z > -0.01f && c.state().feet.z < 0.001f, "lands on a 5 cm slab at 30 m/s without tunneling");
}

void test_geometry_replacement() {
    const auto boxes = with_floor({});
    CapsuleCharacter c({0, 0, 0}, boxes);
    run(c, idle, 5);
    const CapsuleCharacterState before = c.state();
    // A box through the character is refused and nothing changes.
    const auto blocking = with_floor({{{-1, -1, 0.2f}, {1, 1, 1.0f}}});
    require(!c.replace_static_geometry(blocking) && c.state() == before, "overlapping replacement is rejected and changes nothing");
    // A box through the spawn point is refused too (a reset must always be possible).
    CapsuleCharacter away({5, 0, 0}, boxes);
    run(away, forward, 120);
    const CapsuleCharacterState moved = away.state();
    require(moved.feet.x > 5.5f, "the second character walked off its spawn");
    const auto at_spawn = with_floor({{{4.5f, -1, 0}, {5.5f, 1, 2}}});
    require(!away.replace_static_geometry(at_spawn) && away.state() == moved, "a replacement over the reset position is rejected");
    // A clear replacement is accepted, and removing the floor makes the character fall.
    require(c.replace_static_geometry(boxes) && c.state().grounded, "an equal replacement is accepted and stays grounded");
    const std::vector<Aabb> no_floor{{{50, 50, 0}, {51, 51, 1}}};
    require(c.replace_static_geometry(no_floor) && !c.state().grounded, "losing the floor is noticed at once");
    run(c, idle, 30);
    require(c.state().feet.z < -1.0f, "and the character falls");
    // Invalid boxes throw.
    bool threw = false;
    try {
        (void)c.replace_static_geometry(std::vector<Aabb>{{{1, 1, 1}, {0, 2, 2}}});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "an inverted box throws");
}

void test_invalid_input() {
    const auto boxes = with_floor({});
    const auto throws = [](auto&& call) {
        try {
            call();
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CapsuleCharacter c({0, 0, 0}, boxes);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    require(throws([&] { c.step({nan, 0, false, false}, dt); }), "NaN input");
    require(throws([&] { c.step(idle, std::chrono::nanoseconds(0)); }), "zero step");
    require(throws([&] { c.step(idle, std::chrono::milliseconds(101)); }), "a step over 100 ms");
    require(throws([&] { CapsuleCharacter bad({nan, 0, 0}, boxes); }), "NaN spawn");
    require(throws([&] { CapsuleCharacter bad({0, 0, -0.5f}, boxes); }), "a spawn inside the floor");
    CapsuleCharacterConfig config;
    config.height = 0.4f;  // below two radii
    require(throws([&] { CapsuleCharacter bad({0, 0, 0}, boxes, config); }), "a capsule shorter than two radii");
    config = {};
    config.max_slope_radians = 2.0f;
    require(throws([&] { CapsuleCharacter bad({0, 0, 0}, boxes, config); }), "a slope limit past vertical");
    require(throws([&] { CapsuleCharacter bad({0, 0, 0}, std::vector<Aabb>{{{0, 0, 0}, {0, 1, 1}}}); }), "a flat box");
}

// A busy course and a long scripted input that walks, sprints, strafes, jumps, climbs, drops and bumps walls.
std::vector<Aabb> busy_course() {
    std::vector<Aabb> boxes = with_floor({});
    std::uint32_t seed = 99;
    const auto next = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return static_cast<float>((seed >> 8) & 0xFFFF) / 65535.0f;
    };
    for (int i = 0; i < 80; ++i) {
        const float x = -30.0f + next() * 60.0f;
        const float y = -30.0f + next() * 60.0f;
        const float w = 0.5f + next() * 2.0f, d = 0.5f + next() * 2.0f, h = 0.1f + next() * 1.5f;
        if (std::abs(x) < 2.5f && std::abs(y) < 2.5f) continue;  // keep the spawn clear
        boxes.push_back({{x, y, 0}, {x + w, y + d, h}});
    }
    boxes.push_back({{12, -40, 0}, {13, 40, 4}});  // a long wall
    return boxes;
}

CapsuleInput scripted(std::uint32_t tick) {
    CapsuleInput in;
    const std::uint32_t phase = tick / 90;
    in.forward = (phase % 5 == 4) ? -1.0f : 1.0f;
    in.left = ((phase % 3) == 0) ? 0.6f : (phase % 3 == 1 ? -0.6f : 0.0f);
    in.sprint = (tick / 200) % 2 == 0;
    in.jump = tick % 70 < 2;
    return in;
}

void test_replay_is_deterministic_and_never_penetrates() {
    const auto boxes = busy_course();
    constexpr std::uint32_t ticks = 5000;
    CapsuleCharacter a({0, 0, 0}, boxes), b({0, 0, 0}, boxes);
    std::vector<CapsuleCharacterState> history;
    history.reserve(ticks);
    float worst = std::numeric_limits<float>::infinity();
    std::uint32_t airborne = 0, grounded = 0;
    for (std::uint32_t t = 0; t < ticks; ++t) {
        a.step(scripted(t), dt);
        b.step(scripted(t), dt);
        require(a.state() == b.state(), "two instances diverged at tick " + std::to_string(t));
        history.push_back(a.state());
        worst = std::min(worst, min_gap(a, boxes));
        (a.state().grounded ? grounded : airborne) += 1;
    }
    require(worst > -3e-4f, "penetrated geometry during the replay by " + std::to_string(-worst));
    require(grounded > 1000 && airborne > 500, "the script exercises both ground and air: " + std::to_string(grounded) + "/" + std::to_string(airborne));

    // The same script after reset() reproduces the same ticks exactly.
    a.reset();
    for (std::uint32_t t = 0; t < ticks; ++t) {
        a.step(scripted(t), dt);
        require(a.state() == history[t], "reset replay differs at tick " + std::to_string(t));
    }
    // And the whole run is sensitive to its input: a different script gives a different final state.
    CapsuleCharacter c({0, 0, 0}, boxes);
    for (std::uint32_t t = 0; t < ticks; ++t) c.step(scripted(t + 1), dt);
    require(!(c.state() == history.back()), "a different input script must give a different result");
    // Fold the run into a checksum, printed so two builds can be compared by eye.
    std::uint64_t hash = 14695981039346656037ull;
    for (const auto& s : history) {
        for (const float f : {s.feet.x, s.feet.y, s.feet.z}) {
            std::uint32_t bits;
            std::memcpy(&bits, &f, sizeof bits);
            hash = (hash ^ bits) * 1099511628211ull;
        }
    }
    std::cout << "  replay checksum over " << ticks << " ticks: " << std::hex << hash << std::dec << '\n';
}

void test_float_rounding_never_leaves_the_capsule_in_a_wall() {
    // Positions are floats, so far from the origin a step lands on a coarse grid: 2.4e-4 m at 3 km, 8 mm at 65 km. Moving exactly to
    // contact can round to the inside of a wall, and the walls here sit at irrational offsets so that really happens. The skin kept
    // after each contact keeps the capsule on the outside on every tick, at every heading, at every distance.
    constexpr float bases[] = {300.0f, 3000.0f, 65000.0f};
    int runs = 0;
    float worst = std::numeric_limits<float>::infinity();
    for (const float base : bases) {
        for (int heading = 0; heading < 8; ++heading) {
            const float a = static_cast<float>(heading) * 3.14159265f / 4.0f;
            const std::vector<Aabb> room{{{base - 1000, base - 1000, -1}, {base + 1000, base + 1000, 0}},
                                         {{base + 3.1234567f, base - 1000, 0}, {base + 4.5f, base + 1000, 3}},
                                         {{base - 4.5f, base - 1000, 0}, {base - 3.0987654f, base + 1000, 3}},
                                         {{base - 1000, base + 3.2468013f, 0}, {base + 1000, base + 4.5f, 3}},
                                         {{base - 1000, base - 4.5f, 0}, {base + 1000, base - 3.1357913f, 3}}};
            CapsuleCharacter c({base, base, 0.0f}, room);
            for (int i = 0; i < 900; ++i) {
                const float turn = static_cast<float>(i) / 120.0f;
                c.step({std::cos(a + turn), std::sin(a + turn), false, (i / 50) % 2 == 0}, dt);
                for (std::size_t k = 1; k < room.size(); ++k) worst = std::min(worst, saige::capsule_box_gap(c.capsule(), room[k]));
            }
            ++runs;
        }
    }
    require(runs == 24, "all runs happened");
    require(worst >= 0.0f, "the capsule was inside a wall by " + std::to_string(-worst) + " m on some tick");
}

void test_teleport_and_overlap_recovery() {
    const std::vector<Aabb> boxes = with_floor({{{3, -50, 0}, {4, 50, 3}}});
    CapsuleCharacter c({0, 0, 0}, boxes);
    run(c, {1, 0, true, false}, 10);
    // Open air: placed at rest and falls.
    c.teleport({0, 0, 3});
    require(c.state().feet == Vec3{0, 0, 3} && c.state().velocity == Vec3{0, 0, 0} && !c.state().grounded, "teleport to open air");
    // Partly inside a wall: pushed out to touching, along the shortest way (back along x).
    c.teleport({2.8f, 0, 0});  // the capsule would reach x = 3.1
    require(min_gap(c, boxes) >= 0.0f && min_gap(c, boxes) < 3e-4f && c.state().feet.x < 2.71f && c.state().feet.x > 2.69f,
            "teleport into a wall ends up touching it: x = " + std::to_string(c.state().feet.x));
    require(c.state().grounded, "and standing on the floor");
    // Sunk into the floor: lifted onto it.
    c.teleport({0, 0, -0.5f});
    require(near(c.state().feet.z, 0.0, 5e-4) && c.state().grounded, "teleport into the floor lifts the character onto it");
    // Deep inside the wall, with the axis inside the box: leaves through the nearest face.
    c.teleport({3.4f, 0, 0.5f});
    require(min_gap(c, boxes) >= 0.0f, "teleport deep into a wall leaves it");
    require(c.state().feet.x > 4.29f || c.state().feet.x < 2.71f || c.state().feet.z > 2.99f, "and ends on the outside of the box");
    // Valid afterwards: the character walks on normally.
    c.teleport({0, 0, 0});
    run(c, forward, 30);
    require(c.state().feet.x > 0.4f && c.state().grounded, "stepping works after a teleport");
    bool threw = false;
    try {
        c.teleport({std::numeric_limits<float>::quiet_NaN(), 0, 0});
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    require(threw, "a non-finite teleport throws");
}

void test_wedge_between_two_rims_settles() {
    // Two pillars 0.4 m apart: closer than the capsule is wide (0.6 m). A character dropped onto the gap is wedged on both rims,
    // whose contact normals tilt toward each other. It must settle without penetrating or jittering.
    const std::vector<Aabb> boxes{{{-5, -5, -10}, {-0.2f, 5, 0}}, {{0.2f, -5, -10}, {5, 5, 0}}, {{-200, -200, -12}, {200, 200, -10}}};
    CapsuleCharacter c({0, 0, 1.0f}, boxes);
    run(c, idle, 120);
    const CapsuleCharacterState settled = c.state();
    run(c, idle, 300);
    require(c.state() == settled, "a character wedged between two rims settles and stays put");
    require(min_gap(c, boxes) > -1e-4f, "no penetration in the wedge");
    require(c.state().feet.z > -1.0f && c.state().grounded, "it is held by the rims, not fallen through");
    // Pushing sideways while wedged does not tunnel into either pillar.
    run(c, {0, 1, false, true}, 120);
    require(min_gap(c, boxes) > -1e-4f, "no penetration when pushing along the wedge");
}

void test_sliding_along_a_blocked_ledge_stays_grounded() {
    // Found by the Showcase 01 course: a low plinth (0.25 m, under the step height) with a column rising from it. Walking diagonally
    // past the plinth's face, the step onto it is blocked by the column, so the character slides along the face with the bottom
    // sphere touching the plinth's top edge. That rim contact has a steep (non-walkable) normal that the downward ground probe already
    // touches at t = 0; it must not hide the walkable floor right beneath. Before the fix `grounded` flickered every other tick and the
    // feet bounced by several millimeters.
    const std::vector<Aabb> boxes = with_floor({{{-12.6f, -7.6f, 0.0f}, {-11.4f, -6.4f, 0.25f}}, {{-12.42f, -7.42f, 0.0f}, {-11.58f, -6.58f, 3.5f}}});
    CapsuleCharacter c({-10.4f, -7.7f, 0.0f}, boxes);
    int ungrounded = 0;
    float highest = 0.0f;
    bool touched = false;
    for (int tick = 0; tick < 240; ++tick) {
        c.step({-0.7071f, 0.7071f, false, false}, dt);
        if (!c.state().grounded) ++ungrounded;
        highest = std::max(highest, c.state().feet.z);
        touched = touched || min_gap(c, boxes) < 0.01f;
    }
    require(touched, "the scenario really presses the character against the plinth");
    require(ungrounded == 0, "sliding along a ledge face keeps the character grounded on every tick");
    require(highest < 0.01f, "and does not make the feet bounce");
    require(min_gap(c, boxes) > -1e-4f, "without penetration");
}

void test_reset() {
    const auto boxes = with_floor({});
    CapsuleCharacter c({1, 2, 0}, boxes);
    run(c, {1, 1, true, true}, 77);
    c.reset();
    require(c.state().feet == Vec3{1, 2, 0} && c.state().velocity == Vec3{0, 0, 0} && c.state().grounded, "reset returns to the spawn at rest");
    c.step({0, 0, true, false}, dt);
    require(!c.state().grounded, "a jump pressed on the first tick after a reset works");
}

} // namespace

int main() {
    try {
        test_rests_on_the_floor_without_drift();
        test_falls_and_lands();
        test_wall_stops_and_slides();
        test_corner_holds();
        test_steps();
        test_ledge_drop_is_a_fall();
        test_jump_rules();
        test_ceiling();
        test_rim_slope_limit();
        test_no_tunneling_at_terminal_speed();
        test_geometry_replacement();
        test_invalid_input();
        test_replay_is_deterministic_and_never_penetrates();
        test_float_rounding_never_leaves_the_capsule_in_a_wall();
        test_teleport_and_overlap_recovery();
        test_wedge_between_two_rims_settles();
        test_sliding_along_a_blocked_ledge_stays_grounded();
        test_reset();
    } catch (const std::exception& error) {
        std::cerr << "capsule character test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "capsule character tests passed\n";
}
