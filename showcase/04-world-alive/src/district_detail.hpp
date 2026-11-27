// Internal pieces of the district builder (district.cpp, district_interiors.cpp, district_network.cpp).
#pragma once

#include "district.hpp"

#include <array>

namespace saige::alive {

struct Rng {
    std::uint64_t s;
    explicit Rng(std::uint64_t seed) : s(seed * 0x9e3779b97f4a7c15ull + 0x1234567ull) {}
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9e3779b97f4a7c15ull);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        return z ^ (z >> 31);
    }
    double unit() { return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0); }
    double range(double a, double b) { return a + (b - a) * unit(); }
    int pick(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
};

inline Aabb box(double x0, double y0, double z0, double x1, double y1, double z1) {
    return Aabb{{static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(z0)}, {static_cast<float>(x1), static_cast<float>(y1), static_cast<float>(z1)}};
}

// The collision box of a prop or a piece of furniture, in world coordinates, or false when it does not block (flat things, awnings). Dimensions are by type; `scale` stretches counters and shelves along their length.
bool prop_box(const StreetProp& p, Aabb& out);

// The pedestrian path through a park, from the sidewalk along its north side, in, and back out: one definition for the navigation graph and for where trees may stand.
inline std::array<std::array<double, 2>, 5> park_path_points(const Rect& p) {
    const double sy = p.y1 + 1.75, cx = p.cx();
    return {{{cx - 24.0, sy}, {cx - 24.0, p.y1 - 8.0}, {cx, p.cy() - 6.0}, {cx + 24.0, p.y1 - 8.0}, {cx + 24.0, sy}}};
}

void build_interiors(District& d, Rng& rng);   // chooses the accessible buildings and models their rooms, furniture and stairs
void build_props(District& d, Rng& rng);       // street furniture
void build_ped_network(District& d);            // sidewalks, crossings, doors, promenade, piers, interiors
void build_venues(District& d, Rng& rng);       // businesses and homes for the population
void build_collision(District& d);              // the static collision set

} // namespace saige::alive
