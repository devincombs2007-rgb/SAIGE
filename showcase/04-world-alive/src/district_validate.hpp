// Showcase 04: district validation. Pure functions over the district data that find the defects an author (or a generator) can make before anything is rendered or simulated:
// overlapping buildings, buildings on roads, walls that are hidden or exposed by mistake, coplanar faces that would fight for the depth buffer, furniture in parking bays or on the
// roadway, props inside buildings, interior staircases that are blocked. Every finding carries a stable code, a severity, a position and the ids involved, so an editor can list,
// highlight and gate on them. The contract (codes, severities, the Finding layout) is versioned by kValidationContractVersion; see docs/showcase/04-defect-register.md.
//
// The checks use only `District` (data), never meshes, so an authoring tool that produces a `District` (or anything that can be converted to one) gets the same checks.
#pragma once

#include "district.hpp"

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

[[nodiscard]] ValidationReport validate_district(const District& d);

} // namespace saige::alive
