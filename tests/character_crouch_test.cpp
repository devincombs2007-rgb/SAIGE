// Crouching: the capsule comes down to the crouch height with the feet in place, the character moves at the crouch speed, can pass under what the standing capsule cannot, and stays crouched under a low
// ceiling until there is room to stand. Sprinting and jumping are not possible while crouched.
#include "saige/capsule_character.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;
constexpr std::chrono::nanoseconds tick{16666667};

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

// A floor and a low beam across the way from x = 5 to x = 8, its underside at 1.3 m (a standing capsule is 1.7 m, a crouched one 1.02 m).
std::vector<Aabb> tunnel() { return {{{-50, -20, -1}, {100, 20, 0}}, {{5, -20, 1.3f}, {8, 20, 3.0f}}}; }

CapsuleCharacter spawn(const std::vector<Aabb>& boxes, CapsuleCharacterConfig cfg = {}) { return CapsuleCharacter({0, 0, 0}, boxes, cfg); }

CapsuleInput walk(bool crouch, bool sprint = false) {
    CapsuleInput in;
    in.forward = 1.0f;
    in.crouch = crouch;
    in.sprint = sprint;
    return in;
}

void test_config() {
    const auto boxes = tunnel();
    const auto bad = [&](CapsuleCharacterConfig c) {
        try {
            (void)spawn(boxes, c);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    CapsuleCharacterConfig c;
    c.crouch_height = 0.4f;  // under the capsule's diameter (0.6)
    require(bad(c), "a crouch height under the diameter is rejected");
    c.crouch_height = 2.0f;  // taller than standing
    require(bad(c), "a crouch height above the standing height is rejected");
    c.crouch_height = std::nanf("");
    require(bad(c), "a NaN crouch height is rejected");
    c = {};
    c.crouch_speed = -1.0f;
    require(bad(c), "a negative crouch speed is rejected");
    c = {};
    c.crouch_height = 1.2f;
    c.crouch_speed = 2.0f;
    require(!bad(c), "a valid crouch configuration is accepted");
    // The default height adapts to a short character: height 0.9 with the default radius gives a crouch height of the diameter.
    c = {};
    c.height = 0.9f;
    CapsuleCharacter s = spawn(boxes, c);
    s.step(walk(true), tick);
    require(std::fabs(s.capsule().height - 0.6f) < 1e-6f, "the default crouch height never goes under the diameter");
}

void test_lowering_and_speed() {
    const auto boxes = tunnel();
    CapsuleCharacter c = spawn(boxes);
    for (int i = 0; i < 6; ++i) c.step({}, tick);
    const float z0 = c.state().feet.z;
    c.step(walk(true), tick);
    require(c.state().crouched && std::fabs(c.capsule().height - 1.02f) < 1e-5f, "crouching lowers the capsule to 60% of the standing height");
    require(std::fabs(c.state().feet.z - z0) < 1e-4f && c.state().grounded, "the feet stay where they were");
    for (int i = 0; i < 60; ++i) c.step(walk(true, true), tick);  // sprint is ignored while crouched
    const float v = std::hypot(c.state().velocity.x, c.state().velocity.y);
    std::cout << "  crouched speed with the sprint key held: " << v << " m/s (walking is " << CapsuleCharacterConfig{}.walk_speed << ")\n";
    require(std::fabs(v - 0.5f * CapsuleCharacterConfig{}.walk_speed) < 0.02f, "the crouch speed is half the walking speed and sprint does not change it");
    // Jumping is not possible while crouched.
    CapsuleInput jump = walk(true);
    jump.jump = true;
    c.step(jump, tick);
    require(c.state().grounded && c.state().velocity.z <= 0.0f, "no jump while crouched");
    // Standing up in the open is immediate.
    c.step(walk(false), tick);
    require(!c.state().crouched && std::fabs(c.capsule().height - 1.7f) < 1e-6f, "standing up in the open");
}

void test_passing_under_a_beam() {
    const auto boxes = tunnel();
    CapsuleCharacter standing = spawn(boxes);
    for (int i = 0; i < 240; ++i) standing.step(walk(false), tick);
    std::cout << "  standing against a beam at 1.3 m: stopped at x = " << standing.state().feet.x << "\n";
    require(standing.state().feet.x < 5.0f && standing.state().feet.x > 4.0f, "a standing character cannot walk under a 1.3 m beam");

    CapsuleCharacter crouching = spawn(boxes);
    float entered = -1.0f;
    bool stood_up_inside = false;
    for (int i = 0; i < 400; ++i) {
        // Crouch to get under; let go of the key in the middle of the tunnel: it must stay crouched until clear of the beam.
        const bool key = crouching.state().feet.x < 6.5f || crouching.state().feet.x > 8.5f ? true : false;
        crouching.step(walk(key), tick);
        const float x = crouching.state().feet.x;
        if (entered < 0.0f && x > 5.5f) entered = x;
        if (x > 5.5f && x < 7.7f && !crouching.state().crouched) stood_up_inside = true;
    }
    std::cout << "  crouching: reached x = " << crouching.state().feet.x << ", stood up inside the tunnel: " << (stood_up_inside ? "yes" : "no") << ", crouched at the end: " << crouching.state().crouched << "\n";
    require(entered > 0.0f && crouching.state().feet.x > 9.0f, "a crouched character passes under the beam");
    require(!stood_up_inside, "releasing the key under the beam does not stand the character up into it");

    // And once clear of the beam with the key released, it stands.
    CapsuleCharacter late = spawn(boxes);
    for (int i = 0; i < 400; ++i) late.step(walk(late.state().feet.x < 8.6f), tick);
    require(!late.state().crouched && late.state().feet.x > 9.0f, "clear of the beam, it stands up");
}

void test_reset_and_replacement() {
    const auto boxes = tunnel();
    CapsuleCharacter c = spawn(boxes);
    c.step(walk(true), tick);
    require(c.state().crouched, "crouched");
    c.reset();
    require(!c.state().crouched && std::fabs(c.capsule().height - 1.7f) < 1e-6f, "a reset character stands");
    // The default input never crouches: a long scripted run keeps the standing height.
    CapsuleCharacter d = spawn(boxes);
    bool ever = false;
    for (int i = 0; i < 300; ++i) {
        CapsuleInput in;
        in.forward = (i % 80 < 50) ? 1.0f : 0.0f;
        in.jump = i % 120 == 30;
        in.sprint = i % 200 > 100;
        d.step(in, tick);
        ever = ever || d.state().crouched;
    }
    require(!ever, "without the crouch input the character never crouches");
}

} // namespace

int main() {
    try {
        test_config();
        test_lowering_and_speed();
        test_passing_under_a_beam();
        test_reset_and_replacement();
    } catch (const std::exception& e) {
        std::cerr << "character crouch test failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "character crouch tests passed\n";
}
