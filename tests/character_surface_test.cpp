// A character on different ground: traction changes how quickly it gets up to speed and how far it slides when it lets go. With traction left at its default the controller is bit-identical to before.
#include "saige/capsule_character.hpp"
#include "saige/physics_material.hpp"

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

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

struct Floor {
    std::vector<Aabb> boxes;
    StaticCollision world;
    SurfaceMap map;
};
Floor make_floor(Surface s) {
    Floor f;
    f.boxes = {{{-500, -50, -1}, {500, 50, 0}}};
    f.world = StaticCollision(f.boxes, {});
    f.map = SurfaceMap(f.world, s);
    return f;
}

struct Run {
    float speed_after_half_second;
    float slide_distance;     // from letting go of the input at full walking speed (or the top speed it reached) to a standstill
    float top_speed;
};
Run run_on(Surface surface, bool use_traction) {
    const Floor f = make_floor(surface);
    CapsuleCharacter c({0, 0, 0}, f.boxes, {});
    Run r{};
    int i = 0;
    for (; i < 60 * 8; ++i) {  // push for up to 8 s, or until walking speed
        const float traction = use_traction ? ground_traction(f.world, f.map, c.state().feet) : 1.0f;
        c.step({1.0f, 0.0f, false, false, traction}, tick);
        if (i == 29) r.speed_after_half_second = c.state().velocity.x;
        if (c.state().velocity.x >= 3.45f) {
            if (i < 29) r.speed_after_half_second = c.state().velocity.x;  // already at walking speed before half a second
            break;
        }
    }
    r.top_speed = c.state().velocity.x;
    const float x0 = c.state().feet.x;
    for (int k = 0; k < 60 * 20 && c.state().velocity.x > 0.05f; ++k) {
        const float traction = use_traction ? ground_traction(f.world, f.map, c.state().feet) : 1.0f;
        c.step({0.0f, 0.0f, false, false, traction}, tick);
    }
    r.slide_distance = c.state().feet.x - x0;
    return r;
}

void test_table() {
    require(foot_traction(physics_material(Surface::Asphalt)) == 1.0f, "asphalt is full traction");
    require(foot_traction(physics_material(Surface::Ice)) < 0.15f, "ice is slippery");
    require(foot_traction(physics_material(Surface::Ice)) >= 0.02f, "traction never reaches zero");
    require(foot_traction(physics_material(Surface::Rubber)) == 1.0f, "a grippy surface is clamped to 1");
    require(foot_traction(physics_material(Surface::Snow)) < foot_traction(physics_material(Surface::Grass)), "snow is slipperier than grass");
    const Floor f = make_floor(Surface::Ice);
    require(std::fabs(ground_traction(f.world, f.map, {0, 0, 0}) - foot_traction(physics_material(Surface::Ice))) < 1e-6f, "the traction under the feet is the surface's");
    require(ground_traction(f.world, f.map, {0, 0, 50}) == 1.0f, "nothing underfoot reads as full traction");
}

void test_default_is_the_old_behavior() {
    const Floor f = make_floor(Surface::Ice);
    CapsuleCharacter a({0, 0, 0}, f.boxes, {}), b({0, 0, 0}, f.boxes, {});
    for (int i = 0; i < 400; ++i) {
        const float fw = std::sin(static_cast<float>(i) * 0.05f);
        a.step({fw, 0.3f, i == 100, false}, tick);                  // traction left out
        b.step({fw, 0.3f, i == 100, false, 1.0f}, tick);            // explicitly 1
    }
    require(a.state() == b.state(), "the default traction is bit-identical to 1");
    bool threw = false;
    try { a.step({1.0f, 0.0f, false, false, std::nanf("")}, tick); } catch (const std::invalid_argument&) { threw = true; }
    require(threw, "a non-finite traction is rejected");
    // Out of range values are clamped, not rejected: 0 behaves like the minimum, 5 like 1.
    CapsuleCharacter c({0, 0, 0}, f.boxes, {}), d({0, 0, 0}, f.boxes, {});
    for (int i = 0; i < 60; ++i) {
        c.step({1.0f, 0.0f, false, false, 5.0f}, tick);
        d.step({1.0f, 0.0f, false, false, 1.0f}, tick);
    }
    require(c.state() == d.state(), "a traction above 1 is clamped to 1");
}

void test_traction_changes_the_motion() {
    const Run concrete = run_on(Surface::Concrete, true), ice = run_on(Surface::Ice, true), none = run_on(Surface::Ice, false);
    std::cout << "  speed after half a second: concrete " << concrete.speed_after_half_second << ", ice " << ice.speed_after_half_second << " m/s; sliding to a stop from " << concrete.top_speed << " / " << ice.top_speed
              << " m/s: concrete " << concrete.slide_distance << " m, ice " << ice.slide_distance << " m (ice with the traction ignored: " << none.slide_distance << " m)\n";
    require(concrete.speed_after_half_second > 3.0f, "concrete: walking speed within half a second");
    require(ice.speed_after_half_second < 0.3f * concrete.speed_after_half_second, "ice: a fraction of the speed after half a second");
    require(ice.top_speed >= 3.4f, "ice: it does get up to speed eventually");
    require(ice.slide_distance > 8.0f * concrete.slide_distance, "ice: it slides much farther before stopping");
    require(std::fabs(none.slide_distance - concrete.slide_distance) < 0.05f * concrete.slide_distance + 0.02f, "ignoring the traction on ice behaves like concrete (the old behavior)");
}

} // namespace

int main() {
    try {
        test_table();
        test_default_is_the_old_behavior();
        test_traction_changes_the_motion();
    } catch (const std::exception& e) {
        std::cerr << "character surface test failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "character surface tests passed\n";
}
