// CapsuleInput::drive: the horizontal velocity an action (a roll, from a clip's root motion) imposes while the character stands on the ground. It moves the character at exactly that velocity on open
// ground, never through geometry, not in the air, not for a non-finite value, and with the drive inactive the controller is bit-identical to one that has no such field.
#include "saige/capsule_character.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;
constexpr std::chrono::nanoseconds tick{16666667};
constexpr float seconds = 0.016666667f;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

std::vector<Aabb> level(bool with_wall) {
    std::vector<Aabb> b{{{-100, -50, -1}, {200, 50, 0}}};
    if (with_wall) b.push_back({{6, -50, 0}, {7, 50, 10}});  // a wall at x = 6
    return b;
}

CapsuleCharacter spawn(const std::vector<Aabb>& boxes) { return CapsuleCharacter({0, 0, 0}, boxes, CapsuleCharacterConfig{}); }

bool same(const CapsuleCharacterState& a, const CapsuleCharacterState& b) { return a == b; }

CapsuleInput driven(float x, float y) {
    CapsuleInput in;
    in.drive = {true, x, y};
    return in;
}

void test_open_ground_moves_at_the_drive() {
    const auto boxes = level(false);
    CapsuleCharacter c = spawn(boxes);
    for (int i = 0; i < 60; ++i) c.step(driven(5.0f, 2.0f), tick);  // 1 s (60 ticks of 1/60 s)
    const CapsuleCharacterState s = c.state();
    std::cout << "  after 60 ticks at (5, 2) m/s: x " << s.feet.x << ", y " << s.feet.y << ", z " << s.feet.z << "\n";
    require(std::fabs(s.feet.x - 5.0f) < 0.02f && std::fabs(s.feet.y - 2.0f) < 0.01f, "the character travels at the drive velocity (no ramp up: the action owns it)");
    require(std::fabs(s.feet.z) < 1e-3f && s.grounded, "and stays on the ground");
    // The first tick already has the full velocity (a walk's input would only have accelerated).
    CapsuleCharacter d = spawn(boxes);
    d.step(driven(5.0f, 0.0f), tick);
    require(std::fabs(d.state().velocity.x - 5.0f) < 1e-5f, "the velocity is the drive from the first tick");
    // Letting go: ordinary input takes over (the velocity is then decelerated by the ground's own rules, not snapped to zero by the drive).
    d.step({}, tick);
    require(d.state().velocity.x > 3.0f, "after the drive ends the ground acceleration decelerates it (the controller's own rule)");
}

void test_it_never_goes_through_geometry() {
    const auto boxes = level(true);
    CapsuleCharacter c = spawn(boxes);
    for (int i = 0; i < 120; ++i) c.step(driven(8.0f, 0.0f), tick);  // would carry it to x = 16 in two seconds
    std::cout << "  driven at 8 m/s into a wall at x = 6: stopped at x " << c.state().feet.x << "\n";
    require(c.state().feet.x < 6.0f - 0.29f && c.state().feet.x > 5.0f, "stopped against the wall (capsule radius 0.3 m)");
    // Along a wall at a slant: it slides.
    CapsuleCharacter s = spawn(boxes);
    for (int i = 0; i < 60; ++i) s.step(driven(4.0f, 4.0f), tick);  // 4 m of y if it slides freely
    require(s.state().feet.y > 3.8f && s.state().feet.x < 5.75f, "driven diagonally into the wall it slides along it");
}

void test_air_and_invalid_input() {
    const auto boxes = level(false);
    CapsuleCharacter c({0, 0, 3}, boxes, CapsuleCharacterConfig{});  // dropped from 3 m
    require(!c.state().grounded, "starts in the air");
    c.step(driven(9.0f, 0.0f), tick);
    require(std::fabs(c.state().velocity.x) < 1.0f, "in the air the drive is ignored (the air's own acceleration applies)");
    bool threw = false;
    try { c.step(driven(std::nanf(""), 0.0f), tick); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "a non-finite drive is an invalid input");
    const CapsuleCharacterState before = c.state();
    threw = false;
    try { c.step(driven(0.0f, INFINITY), tick); } catch (const std::invalid_argument&) { threw = true; }
    require(threw && same(before, c.state()), "and the state is untouched by it");
    // A huge drive is limited to the terminal speed.
    CapsuleCharacter g = spawn(boxes);
    g.step(driven(1e9f, 0.0f), tick);
    require(g.state().velocity.x <= CapsuleCharacterConfig{}.terminal_speed * 1.0001f && g.state().velocity.x > 0.0f, "a huge drive is capped at the terminal speed");
}

void test_inactive_is_bit_identical() {
    const auto boxes = level(true);
    CapsuleCharacter a = spawn(boxes), b = spawn(boxes);
    bool all_same = true;
    for (int i = 0; i < 400; ++i) {
        CapsuleInput in;
        in.forward = (i % 90 < 60) ? 1.0f : -0.3f;
        in.left = (i % 70 < 20) ? 0.5f : 0.0f;
        in.sprint = i % 150 > 100;
        in.jump = (i % 100) == 40;
        CapsuleInput inactive = in;
        inactive.drive = {false, 7.0f, -3.0f};  // values present, but not active
        a.step(in, tick);
        b.step(inactive, tick);
        all_same = all_same && same(a.state(), b.state());
    }
    require(all_same, "an inactive drive (whatever its numbers) leaves every state bit-identical");
    (void)seconds;
}

CapsuleInput scripted(float x, float y, float z) {
    CapsuleInput in;
    in.drive = {true, x, y, z, true};
    return in;
}

void test_vertical_drive_climbs_a_ledge() {
    // A ledge 1 m high from x = 1.2 on.
    std::vector<Aabb> boxes{{{-50, -50, -1}, {200, 50, 0}}, {{1.2f, -50, 0}, {200, 50, 1.0f}}};
    CapsuleCharacter c = spawn(boxes);
    for (int i = 0; i < 6; ++i) c.step({}, tick);
    require(c.state().grounded, "starts on the ground");
    // Up 1.05 m in 0.6 s, then forward 1.5 m in 0.6 s settling 5 cm down onto the ledge (as the end of a climb clip does).
    for (int i = 0; i < 36; ++i) c.step(scripted(0.0f, 0.0f, 1.05f / 0.6f), tick);
    require(!c.state().grounded && c.state().feet.z > 1.0f, "a rising drive leaves the ground and goes up");
    for (int i = 0; i < 36; ++i) c.step(scripted(1.5f / 0.6f, 0.0f, -0.05f / 0.6f), tick);
    std::cout << "  climb: feet at x " << c.state().feet.x << ", z " << c.state().feet.z << ", grounded " << c.state().grounded << "\n";
    require(c.state().grounded && std::fabs(c.state().feet.z - 1.0f) < 0.02f && c.state().feet.x > 1.3f, "it ends standing on the ledge");
    // Ignoring `vertical`, the z is ignored: a grounded drive stays on the floor.
    CapsuleCharacter f = spawn(boxes);
    CapsuleInput flat;
    flat.drive = {true, 1.0f, 0.0f, 3.0f, false};
    for (int i = 0; i < 20; ++i) f.step(flat, tick);
    require(f.state().grounded && f.state().feet.z < 0.01f, "z without vertical is ignored");
}

void test_vertical_drive_collides() {
    // A ceiling at 1.0 m over x in [3, 6]: rising under it stops at the ceiling; there is no gravity while a drive holds the character in the air.
    std::vector<Aabb> boxes{{{-50, -50, -1}, {200, 50, 0}}, {{3, -50, 2.0f}, {6, 50, 3.0f}}};
    CapsuleCharacter c({4.5f, 0, 0}, boxes, CapsuleCharacterConfig{});
    for (int i = 0; i < 60; ++i) c.step(scripted(0.0f, 0.0f, 2.0f), tick);
    std::cout << "  rising under a ceiling at 2.0 m: the top of the capsule at " << (c.state().feet.z + 1.7f) << " m\n";
    require(c.state().feet.z + 1.7f < 2.0f + 1e-3f && c.state().feet.z > 0.25f, "a ceiling stops the rise");
    CapsuleCharacter h({0, 0, 3.0f}, boxes, CapsuleCharacterConfig{});
    for (int i = 0; i < 60; ++i) h.step(scripted(0.0f, 0.0f, 0.0f), tick);
    require(std::fabs(h.state().feet.z - 3.0f) < 1e-4f && !h.state().grounded, "a zero scripted drive holds the character in the air: no gravity");
    h.step({}, tick);
    require(h.state().feet.z < 3.0f, "and gravity takes over the moment the drive ends");
    // A wall: the forward part is stopped, the rise carries on.
    std::vector<Aabb> wall{{{-50, -50, -1}, {200, 50, 0}}, {{2, -50, 0}, {3, 50, 10}}};
    CapsuleCharacter w = spawn(wall);
    for (int i = 0; i < 60; ++i) w.step(scripted(3.0f, 0.0f, 1.0f), tick);
    require(w.state().feet.x < 2.0f - 0.29f && w.state().feet.z > 0.9f, "against a wall the forward part is stopped and the rise carries on (slides)");
    bool threw = false;
    try { w.step(scripted(0.0f, 0.0f, std::nanf("")), tick); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "a non-finite z is an invalid input");
}

} // namespace

int main() {
    try {
        test_open_ground_moves_at_the_drive();
        test_it_never_goes_through_geometry();
        test_air_and_invalid_input();
        test_inactive_is_bit_identical();
        test_vertical_drive_climbs_a_ledge();
        test_vertical_drive_collides();
    } catch (const std::exception& e) {
        std::cerr << "character drive test failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "character drive tests passed\n";
}
