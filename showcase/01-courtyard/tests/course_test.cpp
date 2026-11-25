// The scripted course is a contract: it must complete, replay bit-identically, never penetrate geometry and never flicker.
#include "course.hpp"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace saige;
using namespace saige::showcase;

namespace {
void require(bool ok, const std::string& what)
{
    if (!ok) throw std::runtime_error(what);
}
int count(const CourseResult& r, EventKind k)
{
    return static_cast<int>(std::count_if(r.events.begin(), r.events.end(), [&](const Event& e) { return e.kind == k; }));
}
} // namespace

int main()
{
    try {
        const SceneData scene = load_scene(std::string(SAIGE_SHOWCASE_ASSETS) + "/scene.json");
        require(scene.colliders.size() > 20 && scene.decals.size() > 10 && scene.route.size() > 10, "the scene has colliders, decals and a route");
        require(scene.colliders.size() == scene.collider_names.size(), "every collider is named");
        require(scene.triangles.size() >= 8 && scene.triangles.size() == scene.triangle_names.size(), "the scene has named ramp triangles (non-box collision)");

        const CourseResult a = run_course(scene);
        const CourseResult b = run_course(scene);
        require(a.finished && a.failed_waypoints == 0, "the course completes without abandoning a waypoint");
        require(a.hash == b.hash && a.trace.size() == b.trace.size(), "two runs of the course are bit-identical");
        require(a.trace.size() > 1000 && a.trace.size() < 60 * 120, "the course is a reasonable length");

        // It exercises what it claims to: jumps onto crates, steps up the dais, a drop, and a slide along a wall.
        require(count(a, EventKind::Jump) >= 2, "it jumps at least twice");
        require(a.max_height >= 0.99f, "it stands on the 1 m crate");
        require(count(a, EventKind::StepUp) >= 3, "it climbs the marble steps");
        require(count(a, EventKind::WaypointTimedOut) >= 1, "it runs into the north wall");
        require(count(a, EventKind::Land) >= 3, "it lands after jumps and the drop");

        // The ramp plaza exercises triangle collision: a walkable 25 degree ramp climbed grounded onto a 1.4 m platform, then a 54 degree
        // chute that is too steep to stand on, down which the character slides to the floor.
        int on_ramp = 0, on_platform = 0;
        for (const TickRecord& r : a.trace) {
            if (r.grounded && r.ground_normal_z > 0.89f && r.ground_normal_z < 0.92f && r.feet.z > 0.1f) ++on_ramp;
            if (r.grounded && r.feet.z > 1.39f && r.feet.z < 1.41f && r.feet.x > 4.0f && r.feet.x < 6.7f) ++on_platform;
        }
        require(on_ramp >= 50, "it walks up the 25 degree ramp, grounded, for at least 50 ticks (got " + std::to_string(on_ramp) + ")");
        require(on_platform >= 40, "and across the platform at 1.4 m (got " + std::to_string(on_platform) + " ticks)");
        bool slid = false;
        for (std::size_t t = 1; t < a.trace.size(); ++t) {
            const TickRecord& r = a.trace[t];
            // Descending beside the chute with horizontal speed along it: sliding, not a vertical drop.
            if (!r.grounded && r.feet.x > 7.3f && r.feet.x < 7.9f && r.feet.y < -8.0f && r.velocity.z < -2.0f && r.velocity.x > 1.4f && r.feet.z > 0.1f) slid = true;
        }
        require(slid, "it slides down the 54 degree chute rather than standing on it");
        std::size_t floor_after = 0;
        for (std::size_t t = 0; t < a.trace.size(); ++t) {
            if (a.trace[t].feet.x > 8.0f && a.trace[t].feet.y < -8.5f && a.trace[t].feet.z < 0.01f && a.trace[t].grounded) { floor_after = t; break; }
        }
        require(floor_after > 0, "and ends on the floor beyond the chute");

        // Never inside geometry, and no ground flicker: any airborne stretch is a real jump or fall, not a probe artifact.
        CapsuleCharacterConfig config;
        config.radius = scene.radius;
        config.height = scene.height;
        int airborne_run = 0;
        int shortest = 1 << 30;
        for (const TickRecord& r : a.trace) {
            const Capsule cap{r.feet, config.radius, config.height};
            for (const Aabb& box : scene.colliders) require(capsule_box_gap(cap, box) > -1e-3f, "the character never penetrates a collider");
            for (const Triangle& tri : scene.triangles) require(capsule_triangle_gap(cap, tri) > -1e-3f, "nor a ramp triangle");
            if (!r.grounded) {
                ++airborne_run;
            } else if (airborne_run > 0) {
                shortest = std::min(shortest, airborne_run);
                airborne_run = 0;
            }
        }
        require(shortest >= 6, "no airborne stretch is shorter than 6 ticks (a flicker, not a jump)");

        // The route reaches the end: the character finishes in front of the material row on the dais.
        const Vec3 end = a.trace.back().feet;
        require(end.x > 12.5f && end.z > 0.55f && end.z < 0.65f, "the course ends on the dais in front of the material row");

        std::printf("showcase course test passed: %zu ticks, hash %016llx\n", a.trace.size(), static_cast<unsigned long long>(a.hash));
    } catch (const std::exception& e) {
        std::cerr << "showcase course test failed: " << e.what() << "\n";
        return 1;
    }
}
