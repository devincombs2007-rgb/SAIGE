// Objects meeting surfaces: how a falling object's velocity changes at a contact depends on both materials. Bounce heights follow the combined restitution, friction stops a slide without reversing it, no
// contact ever adds energy, a settling object comes to rest instead of bouncing forever, and the result is the same whichever side the normal points to.
#include "saige/physics_material.hpp"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace saige;

void require(bool c, const std::string& m) {
    if (!c) throw std::runtime_error(m);
}

float speed2(Vec3 v) { return v.x * v.x + v.y * v.y + v.z * v.z; }

struct Floor {
    std::vector<Aabb> boxes;
    StaticCollision world;
    SurfaceMap map;
};
Floor make_floor() {
    Floor f;
    f.boxes = {{{-50, -50, -1}, {0, 50, 0}}, {{0, -50, -1}, {50, 50, 0}}};  // two slabs, side by side, tops at z = 0
    f.world = StaticCollision(f.boxes, {});
    f.map = SurfaceMap(f.world, Surface::Concrete);
    return f;
}

// Drops a point from `h` meters onto what is below it and returns the height of the first bounce's apex (0 if it does not leave the ground), through the collision and the map.
float bounce_height(const Floor& f, float x, const PhysicsMaterial& object, float h) {
    const std::optional<RayHit> hit = f.world.raycast(Vec3{x, 0, h}, Vec3{0, 0, -1}, h + 1.0f, RayMode::Surface);
    require(hit.has_value(), "the drop hits the floor");
    const float v = std::sqrt(2.0f * 9.81f * h);
    const Vec3 after = contact_velocity({0, 0, -v}, *hit, object, f.map);
    return after.z > 0.0f ? after.z * after.z / (2.0f * 9.81f) : 0.0f;
}

void test_bounce_heights_follow_the_materials() {
    Floor f = make_floor();
    // The right slab is rubber, the left stays concrete.
    f.map.set({StaticPrimitive::Kind::Box, 1}, Surface::Rubber);
    f.map.set({StaticPrimitive::Kind::Box, 0}, Surface::Ice);
    const PhysicsMaterial& ball = physics_material(Surface::Rubber);  // a rubber ball (restitution 0.65)
    const PhysicsMaterial& chunk = physics_material(Surface::Concrete);
    const float h = 2.0f;
    const float rubber_rubber = bounce_height(f, 10.0f, ball, h), rubber_ice = bounce_height(f, -10.0f, ball, h);
    const float chunk_rubber = bounce_height(f, 10.0f, chunk, h), chunk_ice = bounce_height(f, -10.0f, chunk, h);
    std::cout << "  drop from 2 m, apex of the first bounce: rubber ball on rubber " << rubber_rubber << ", on ice " << rubber_ice << "; concrete chunk on rubber " << chunk_rubber << ", on ice " << chunk_ice << "\n";
    // Apex = e^2 h with e the larger restitution of the pair.
    require(std::fabs(rubber_rubber - 0.65f * 0.65f * h) < 0.02f, "rubber on rubber: e = 0.65");
    require(std::fabs(rubber_ice - 0.65f * 0.65f * h) < 0.02f, "a lively ball stays lively on a dead floor (the larger restitution of the pair)");
    require(std::fabs(chunk_rubber - 0.65f * 0.65f * h) < 0.02f, "and a dead chunk on a lively floor bounces with the floor's restitution");
    require(std::fabs(chunk_ice - 0.20f * 0.20f * h) < 0.02f, "a concrete chunk on ice: e = 0.2 (the chunk's)");
    require(chunk_rubber > 5.0f * chunk_ice, "the surface the object lands on decides: several times the bounce");
}

void test_friction_stops_a_slide_and_never_reverses_it() {
    const PhysicsMaterial& chunk = physics_material(Surface::Concrete);
    const Vec3 up{0, 0, 1};
    // A grazing contact: tangential 4 m/s, 1 m/s into the surface.
    const Vec3 on_concrete = contact_velocity({4, 0, -1}, up, chunk, physics_material(Surface::Concrete));
    const Vec3 on_ice = contact_velocity({4, 0, -1}, up, chunk, physics_material(Surface::Ice));
    const Vec3 on_rubber = contact_velocity({4, 0, -1}, up, chunk, physics_material(Surface::Rubber));
    std::cout << "  tangential speed after a graze (4 m/s along, 1 m/s into): concrete " << on_concrete.x << ", ice " << on_ice.x << ", rubber " << on_rubber.x << " m/s\n";
    require(on_ice.x > on_concrete.x && on_concrete.x > on_rubber.x, "less friction keeps more of the slide");
    require(on_concrete.x > 0.0f && on_ice.x > 0.0f && on_rubber.x >= 0.0f, "friction never reverses the slide");
    // A hard hit with little sliding: friction takes all of it, never more.
    const Vec3 stopped = contact_velocity({0.3f, 0, -8}, up, chunk, physics_material(Surface::Concrete));
    require(std::fabs(stopped.x) < 1e-6f && stopped.z > 0.0f, "a small slide in a hard impact is stopped, not reversed");
    // Along the surface, or leaving it, the velocity is untouched.
    require(contact_velocity({3, 1, 0}, up, chunk, physics_material(Surface::Concrete)) == Vec3({3, 1, 0}), "moving along the surface: unchanged");
    require(contact_velocity({3, 1, 2}, up, chunk, physics_material(Surface::Concrete)) == Vec3({3, 1, 2}), "leaving the surface: unchanged");
}

void test_no_contact_adds_energy_and_the_pair_is_symmetric() {
    const Surface all[] = {Surface::Default, Surface::Asphalt, Surface::AsphaltWet, Surface::Concrete, Surface::Gravel, Surface::Dirt, Surface::Grass, Surface::Sand, Surface::Snow, Surface::Ice, Surface::Rubber};
    std::uint32_t rng = 12345;
    const auto next = [&rng] {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) / 16777216.0f;
    };
    int checked = 0;
    for (const Surface a : all) {
        for (const Surface b : all) {
            for (int i = 0; i < 200; ++i) {
                const Vec3 v{(next() - 0.5f) * 20.0f, (next() - 0.5f) * 20.0f, -next() * 15.0f};
                Vec3 n{(next() - 0.5f) * 0.6f, (next() - 0.5f) * 0.6f, 1.0f};
                const float l = std::sqrt(speed2(n));
                n = {n.x / l, n.y / l, n.z / l};
                const Vec3 out = contact_velocity(v, n, physics_material(a), physics_material(b));
                require(speed2(out) <= speed2(v) * (1.0f + 1e-5f), "a contact never adds energy");
                const Vec3 leaving = contact_velocity(v, {-n.x, -n.y, -n.z}, physics_material(a), physics_material(b));
                require(v.x * -n.x + v.y * -n.y + v.z * -n.z < 0.0f || leaving == v, "a normal facing the way the object is going is a contact it is leaving: unchanged");
                const Vec3 swapped = contact_velocity(v, n, physics_material(b), physics_material(a));
                require(std::fabs(out.x - swapped.x) < 1e-4f && std::fabs(out.y - swapped.y) < 1e-4f && std::fabs(out.z - swapped.z) < 1e-4f, "the pair is symmetric");
                ++checked;
            }
        }
    }
    std::cout << "  " << checked << " random contacts: no energy gain, pair order irrelevant\n";
}

void test_settling_and_bad_input() {
    const PhysicsMaterial& m = physics_material(Surface::Rubber);
    // Repeated bounces of a lively ball come to rest: the sequence of apex heights falls to nothing in a finite number of bounces.
    float v = 6.0f;
    int bounces = 0;
    while (bounces < 100) {
        const Vec3 out = contact_velocity({0, 0, -v}, {0, 0, 1}, m, m);
        if (!(out.z > 0.0f)) break;
        v = out.z;
        ++bounces;
    }
    std::cout << "  a rubber ball dropped at 6 m/s comes to rest after " << bounces << " bounces\n";
    require(bounces > 3 && bounces < 30, "it bounces a few times, then rests");
    // A normal that is not a unit vector or not finite is a caller error: the velocity is handed back.
    const Vec3 in{1, 2, -3};
    require(contact_velocity(in, {0, 0, 0}, m, m) == in, "a zero normal: unchanged");
    require(contact_velocity(in, {0, 0, 10}, m, m) == in, "a long normal: unchanged");
    require(contact_velocity(in, {std::nanf(""), 0, 1}, m, m) == in, "a NaN normal: unchanged");
    const Vec3 nan_v = contact_velocity({std::nanf(""), 0, -1}, {0, 0, 1}, m, m);
    require(std::isnan(nan_v.x), "a NaN velocity is handed back, not turned into numbers");
}

} // namespace

int main() {
    try {
        test_bounce_heights_follow_the_materials();
        test_friction_stops_a_slide_and_never_reverses_it();
        test_no_contact_adds_energy_and_the_pair_is_symmetric();
        test_settling_and_bad_input();
    } catch (const std::exception& e) {
        std::cerr << "contact response test failed: " << e.what() << '\n';
        return 1;
    }
    std::cout << "contact response tests passed\n";
}
