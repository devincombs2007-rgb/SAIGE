// Showcase 04, the moving things as GPU models: the sea (its mesh follows the same wave model the boats float on), boats, cars (the licensed CarConcept model with paint variants, with its wheels spinning and steering
// where the vehicle physics puts them), vans, ambulances and fire engines (procedural), the residents (the UAL humanoid, skinned on the GPU, posed by the engine's locomotion controller from their real speed), and
// the smoke over a wreck.
#pragma once

#include "world.hpp"
#include "gfx.hpp"

#include <memory>

namespace saige::alive::rend {

class Actors {
public:
    // `cache` is the asset cache root (polyhaven/, khronos/), `anim_root` holds ual/AnimationLibrary_Godot_Standard.glb.
    Actors(render::SceneRenderer& renderer, const Materials& materials, const District& district, const fs::path& cache, const fs::path& anim_root, std::vector<std::string>& warnings);
    ~Actors();

    // Advances the residents' animation by `dt` seconds of video time (call once per rendered frame, with the frame's pedestrian views).
    void animate(const std::vector<PedView>& peds, double dt);
    // The draws of everything that moves, near the eye, at world time `t`.
    void collect(std::vector<render::ModelDraw>& out, const World& world, double t, Float3 eye, float far_distance);
    // The parked cars of the district (scenery with a collision box), drawn as the same car model.
    void collect_parked(std::vector<render::ModelDraw>& out, Float3 eye, float far_distance) const;
    // An impact the traffic reported (the engine's impact event): dust and sparks at the point, scaled by the energy lost.
    void impact(double t, double x, double y, double z, float energy);
    // The sea's height at a point and time (the surface drawn).
    [[nodiscard]] double sea_height(double x, double y, double t) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace saige::alive::rend
