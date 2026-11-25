// Showcase 04: the harbor's sea state, shared by the simulation (what the boats float on) and the renderer (what the water surface shows), so that a boat rides exactly the surface that is drawn.
#pragma once

#include "saige/water.hpp"

namespace saige::alive {

// A moderate breeze from the north-west pushing small waves into the harbor: about 0.3 m significant wave height, with the quay's sea level at -1.1 m (the promenade is 1.25 m above it).
[[nodiscard]] inline WaterConfig harbor_sea_config(double sea_level = -1.1) {
    WaterConfig c;
    c.level = sea_level;
    c.density = 1025.0;
    c.choppiness = 0.35;
    c.waves = waves_from_wind(4.2, -1.9, 9);
    return c;
}

} // namespace saige::alive
