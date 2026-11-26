// Showcase 04: district validation. Pure functions over the district data that find the defects an author (or a generator) can make before anything is rendered or simulated:
// overlapping buildings, buildings on roads, walls that are hidden or exposed by mistake, coplanar faces that would fight for the depth buffer, furniture in parking bays or on the
// roadway, props inside buildings, interior staircases that are blocked. Every finding carries a stable code, a severity, a position and the ids involved, so an editor can list,
// highlight and gate on them. The contract (codes, severities, the Finding layout) is versioned by kValidationContractVersion; see docs/showcase/04-defect-register.md.
//
// The checks use only `District` (data), never meshes, so an authoring tool that produces a `District` (or anything that can be converted to one) gets the same checks.
#pragma once

#include "district.hpp"
#include "vegetation_shape.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace saige::alive {

inline constexpr int kValidationContractVersion = 1;

enum class Severity : std::uint8_t { Info = 0, Warning = 1, Error = 2, Critical = 3 };
[[nodiscard]] const char* severity_name(Severity s);

struct Finding {
    std::string code;            // stable identifier, e.g. "BLD_OVERLAP"
    Severity severity = Severity::Error;
    std::string message;
    double x = 0, y = 0;         // where, in district meters
    std::vector<std::uint32_t> ids;  // building ids, prop indices, spot indices ... (meaning depends on the code; see the register)
};

struct ValidationReport {
    std::vector<Finding> findings;
    [[nodiscard]] std::size_t count(const std::string& code) const;
    [[nodiscard]] std::size_t count_at_least(Severity s) const;
    [[nodiscard]] bool has_blocking() const { return count_at_least(Severity::Error) > 0; }
};

// The parts [u0, u1] (meters along the face, from the face's left corner as seen from outside) of `b`'s face `leg` that are NOT pressed against another building: the part a
// viewer in the street could see. Empty when the whole face is covered. Sampled every `step` meters; spans shorter than `min_span` are dropped.
[[nodiscard]] std::vector<std::pair<double, double>> exposed_face_spans(const District& d, const Building& b, Leg leg, double step = 0.05, double min_span = 0.15);

// The face's frame as the renderer builds it: origin (the left corner seen from outside), unit direction along the face, outward normal and length.
struct FaceFrame {
    std::array<double, 2> origin{}, u{}, out{};
    double length = 0;
};
[[nodiscard]] FaceFrame face_frame(const Rect& footprint, Leg leg);

// The rule the Showcase 04 film was rendered with: a face is dropped when ONE point, 1 m outside its midpoint, lies inside another building. Kept so a regression test can
// quantify what that rule got wrong; nothing renders with it any more.
[[nodiscard]] bool legacy_face_hidden(const District& d, const Building& b, Leg leg);

// The faces of an interior wall box (bit mask, 1 -x, 2 +x, 4 -y, 8 +y, 16 -z, 32 +z, as MeshBuilder::box takes it) that may be drawn: a face that lies on the building's footprint
// boundary is the outside of the building and belongs to the facade, so drawing it too puts two different surfaces in one plane (the depth-buffer flicker of Showcase 04).
[[nodiscard]] unsigned interior_wall_face_mask(const Rect& footprint, const Rect& wall, double eps = 1e-4);

// The awning of a shopfront as the renderer builds it: 1.55 m deep, from 2.22 m to 3.0 m above the sidewalk, across the face with 0.3 m to spare at each end. Awnings are not in the collision set
// (nothing walks into them) but a camera and a tree's leaves must stay out of them. One box per commercial front.
[[nodiscard]] std::vector<Box3> awning_boxes(const District& d);
// A building's volume (footprint, ground to roof).
[[nodiscard]] Box3 building_box(const Building& b);

// A tree at (x, y) with `scale`: would its canopy clear every building and awning by `margin` meters? (The generator uses this to place trees; the validator reports the ones that do not.)
[[nodiscard]] bool tree_clear_of_buildings(const District& d, const StreetProp& tree, float margin = 0.3f);

// Is the camera at `eye`, looking at `look` (vertical field of view `fov_deg`, 16:9), obstructed? Nine rays are cast across the frustum (centre, corners and edge midpoints at 90% of the half angle).
// The camera is obstructed when the eye is inside tree leaves or an awning, or when at least two rays meet an awning within 3 m or tree leaves within 4 m, or the centre ray meets leaves within `view_len`.
// That is what a viewer perceives as "something in front of the lens": a slab or a leaf mass covering part of the frame. `trees` are the district's tree props, `awnings` its awning boxes.
[[nodiscard]] bool view_obstructed(const std::vector<StreetProp>& trees, const std::vector<Box3>& awnings, const std::array<float, 3>& eye, const std::array<float, 3>& look, float fov_deg = 55.0f, float view_len = 6.0f);

// Framing: is the line from the camera to its subject (the look point) blocked by a building the camera is not inside? A shot inside a building (the cafe interior) is not judged by this.
[[nodiscard]] bool sightline_blocked_by_building(const District& d, const std::array<float, 3>& eye, const std::array<float, 3>& look);
// Is the eye inside any solid collision box tall enough to matter (a wall, a prop, a parked car's body is below an eye at 1.7 m and does not count)?
[[nodiscard]] bool eye_in_solid(const District& d, const std::array<float, 3>& eye);

[[nodiscard]] ValidationReport validate_district(const District& d);

} // namespace saige::alive
