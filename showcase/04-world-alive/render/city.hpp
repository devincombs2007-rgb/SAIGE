// Showcase 04, the static city as GPU models: the ground (roads, markings, crosswalks, sidewalks, kerbs, park, promenade, piers), every building (facade bays with window reveals, storefronts, cornices, roofs,
// balconies, awnings), the accessible interiors with their furniture, the street furniture, and the signal heads that the traffic simulation switches.
#pragma once

#include "district.hpp"
#include "gfx.hpp"

#include <functional>
#include <memory>

namespace saige::alive::rend {

// Where a static is drawn: `District` only the real district (its own ground, waterfront, piers), `Scenery` only the copies that continue the grid beyond the district (their ground is
// clipped so that it never overlaps the real district), `Both` everything else (buildings and props, whose copies are filtered by position).
enum class Role : std::uint8_t { Both = 0, District = 1, Scenery = 2 };

struct Placed {
    render::GpuModel model;
    Mat4 world = Mat4::identity();
    float cx = 0, cy = 0, radius = 0;   // bounding circle in the plane, for distance culling
    Role role = Role::Both;
};

class CityScene {
public:
    CityScene(render::SceneRenderer& renderer, const Materials& materials, const District& district, const fs::path& polyhaven_cache, std::vector<std::string>& warnings);
    ~CityScene();

    // Draws of everything static within `far_distance` of the eye (the renderer's frustum culling does the rest).
    void collect(std::vector<render::ModelDraw>& out, Float3 eye, float far_distance) const;
    // The signal heads: each approach's vehicle signal shows the colour `vehicle_state(intersection, leg)` returns (0 off, 1 red, 2 amber, 3 green), each crosswalk's pedestrian signal `walk(crosswalk)`.
    void collect_signals(std::vector<render::ModelDraw>& out, const std::function<int(std::uint32_t, int)>& vehicle_state, const std::function<bool(std::uint32_t)>& walk) const;
    // A prop model by the district's type name (street furniture, interior furniture), already scaled to the size its collision box implies; null if the type is drawn procedurally elsewhere.
    [[nodiscard]] std::size_t draw_count() const;
    [[nodiscard]] std::size_t triangle_count() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saige::alive::rend
