// The prop table: one source of truth for how a placed prop looks and how it collides. The world generator builds collision from it and the renderer draws from it, so the
// rendered model and the physical hull cannot drift apart (Showcase 03 shipped with the concrete barriers' collision turned 90 degrees from their rendered models, because
// the hull was typed in by hand while the model was authored with its length along another axis).
//
// Frame: a prop's local frame is engine space after the importer's glTF conversion (+X forward, +Y left, +Z up) with the base on z = 0. `yaw_offset` turns the authored model so
// that its long axis lies along local +X, the direction `PropInstance::yaw` means. `scale` multiplies the instance's own scale to give the visual size in meters.
#pragma once

#include "saige/capsule_triangle.hpp"
#include "world.hpp"

#include <vector>

namespace saige::horizon {

struct PropBox {
    float min_x, min_y, min_z, max_x, max_y, max_z;
};

struct PropSpec {
    PropType type;
    const char* model;       // directory under models/ (Poly Haven), or nullptr for a procedural model
    float yaw_offset;        // radians about +Z applied to the authored model before the instance's yaw
    float scale;             // visual scale multiplier on top of the instance's scale
    PropBox visual;          // the authored model's engine-space bounds, as the engine importer reports them (before yaw_offset and scale)
    bool solid;              // has collision
    PropBox hull;            // collision box in the same frame as `visual` (before yaw_offset and scale); ignored when not solid
    bool hull_follows_scale = true;  // trees: the trunk scales with the instance
};

[[nodiscard]] const PropSpec& prop_spec(PropType type);

// The oriented box a placed prop occupies, in world meters: its center on the ground plane, half extents, the bottom and top heights and its heading.
struct PlacedBox {
    double cx, cy;
    double half_length, half_width;  // along the heading and across it
    double z_min, z_max;
    double heading;
};

[[nodiscard]] PlacedBox placed_visual_box(const PropInstance& p);
[[nodiscard]] PlacedBox placed_hull_box(const PropInstance& p);

// Appends the twelve triangles of an oriented box (outward faces) in coordinates relative to the cell corner (ox, oy). The bottom is sunk `sink` meters below z_min so that a prop on a slope
// has no gap under it.
void append_box_hull(std::vector<Triangle>& out, const PlacedBox& box, double ox, double oy, double sink);

} // namespace saige::horizon
