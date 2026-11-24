#pragma once

// Wheel and terrain contact queries against an immutable StaticCollision snapshot (issue #43; the next vehicle slice, agreed with Core).
//
// One probe per wheel: a support segment from `origin` (the wheel's top-of-travel center in the published chunk-local frame) along `direction` for `length`
// meters, swept as a point (radius 0) or as a sphere of the wheel's `radius`. The query answers what the wheel would rest on: where along the segment it
// first touches the world, the contact point, the unit support normal and the snapshot-scoped shape id. Core owns suspension, traction, wheel state and
// everything gameplay; this is geometry only and holds no state.
//
// Semantics (specified, and tested):
//  * Nearest contact. The first contact along the segment wins, whatever it is: a surface the wheel cannot stand on is not skipped to find ground behind it.
//    A blocker between the mount and the ground (a beam, a steep face, an overhang's underside) is therefore reported as `Blocked`, not ignored.
//  * Ties. Exactly equal contact times go to the lower shape id, boxes before triangles (a box wins an exact tie against a triangle): the rule of every other
//    StaticCollision sweep. A ray is exact in double; a sphere sweep inherits the capsule sweeps' exact ordering.
//  * Sidedness. Boxes are solid. Triangles are two-sided: a surface is hit from whichever side the probe comes from, and the reported normal always faces
//    the probe's origin side. "Underside" is therefore a hit whose facing normal points away from `up`, and a ramp mesh with flipped winding still supports.
//  * Slope. A contact is `Support` when dot(normal, up) >= cos(max_slope); otherwise it is `Blocked`. `up` is the chassis' (or the world's) up; max_slope
//    is in [0, pi/2) radians. A vertical wall, an overhang's underside or a steeper-than-permitted ramp face is Blocked.
//  * Miss versus initial overlap. `Miss`: the segment never touches anything. `InitialOverlap`: the probe starts touching or inside the world (a ray whose origin
//    is inside a solid box, a sphere that already penetrates a primitive): distance 0, the normal is the push-out direction (a ray: the reverse of the direction),
//    and the caller recovers (raises the wheel or the chassis) before trusting a support. A probe that merely touches a surface from outside and moves away or
//    along it is a Miss for that surface, the capsule sweeps' rule, and it never masks what lies beyond: a ray uses RayMode::Surface, so a radius-0 probe leaving or running
//    along a box face skips that box and finds the next. A triangle has no inside to leave: a ray starting on one is in contact with it (distance 0).
//
// Envelope. A sphere probe is a capsule sweep, so it shares capsule_sweep.hpp's: the whole path inside +-max_local_extent chunk-local meters and `length` at
// most max_sweep_length, else std::invalid_argument. A ray accepts any finite coordinates (and the same length bound). Invalid input throws.

#include "saige/static_collision.hpp"

#include <cstddef>
#include <span>

namespace saige {

inline constexpr float max_wheel_radius = 4.0f;

struct WheelProbe {
    Vec3 origin;                       // the wheel center at the top of its travel
    Vec3 direction{0.0f, 0.0f, -1.0f}; // the way the wheel travels toward the ground (normalized here; any finite non-zero vector)
    float length = 1.0f;               // meters, in (0, max_sweep_length]
    float radius = 0.0f;               // 0: a ray; otherwise the wheel's radius, in (0, max_wheel_radius]
    Vec3 up{0.0f, 0.0f, 1.0f};         // for the slope test (normalized here)
    float max_slope_radians = 0.9f;    // about 51.6 degrees: [0, pi/2)
};

enum class WheelContactStatus : unsigned char { Miss, Support, Blocked, InitialOverlap };

struct WheelContact {
    WheelContactStatus status{WheelContactStatus::Miss};
    double distance{};          // exact travel of `origin` along `direction` to the contact (0 for InitialOverlap)
    float travel{};             // distance / length, in [0, 1]
    Vec3 center;                // origin + direction * distance: the wheel's center at contact
    Vec3 point;                 // the contact point on the surface
    Vec3 normal;                // unit, from the surface toward the probe origin's side: the support normal
    float slope_cos{};          // dot(normal, up)
    StaticPrimitive primitive;  // snapshot-scoped shape id (boxes first, then triangles)
};

// Throws std::invalid_argument for a non-finite or out-of-range probe (see above) or a sphere path outside the envelope.
[[nodiscard]] WheelContact probe_wheel(const StaticCollision& world, const WheelProbe& probe);
[[nodiscard]] WheelContact probe_wheel(const StaticCollision& world, const WheelProbe& probe, QueryScratch& scratch);

// One probe per wheel, in order; `out` must have exactly as many elements. Equal to calling probe_wheel for each (nothing is shared between wheels). All probes are
// validated before any is answered, to the exact condition the answer would throw on (including a sphere's sweep envelope), so a bad one anywhere leaves `out` untouched
// (std::bad_alloc from the scratch growing aside). A ray allocates nothing; a sphere allocates nothing once `scratch` has grown to what it gathers.
void probe_wheels(const StaticCollision& world, std::span<const WheelProbe> probes, std::span<WheelContact> out, QueryScratch& scratch);

} // namespace saige
