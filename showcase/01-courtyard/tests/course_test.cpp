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

        // Never inside geometry, and no ground flicker: any airborne stretch is a real jump or fall, not a probe artifact.
        CapsuleCharacterConfig config;
        config.radius = scene.radius;
        config.height = scene.height;
        int airborne_run = 0;
        int shortest = 1 << 30;
        for (const TickRecord& r : a.trace) {
            const Capsule cap{r.feet, config.radius, config.height};
            for (const Aabb& box : scene.colliders) require(capsule_box_gap(cap, box) > -1e-3f, "the character never penetrates a collider");
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
