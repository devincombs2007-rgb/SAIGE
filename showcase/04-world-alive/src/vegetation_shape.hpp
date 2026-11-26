// Showcase 04: the shape of a street tree as data. The renderer draws the canopy from these spheres, the director keeps the camera out of them and the validator checks the camera's
// view against them, so there is one definition of where the leaves are (before, only the renderer knew: the physics and the camera saw a 0.44 m trunk).
#pragma once

#include "district.hpp"

#include <array>
#include <cmath>
#include <cstdint>

namespace saige::alive {

[[nodiscard]] inline float shape_hash(std::uint32_t a, std::uint32_t b = 0, std::uint32_t c = 0) {
    std::uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu ^ (c + 0x165667b1u) * 0xc2b2ae35u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xffffffu) / 16777216.0f;
}

struct CanopyBlob {
    float x, y, z, r;
};

inline constexpr int kTreeBlobs = 5;

// The seed a tree prop is built with (position-derived, so the same tree always has the same shape).
[[nodiscard]] inline std::uint32_t tree_seed(const StreetProp& p) { return static_cast<std::uint32_t>(p.x * 13 + p.y * 7); }

[[nodiscard]] inline float tree_height(std::uint32_t seed, float scale) { return (4.2f + shape_hash(seed, 3) * 1.8f) * scale; }

[[nodiscard]] inline std::array<CanopyBlob, kTreeBlobs> tree_blobs(float x, float y, float z, float yaw, float scale, std::uint32_t seed) {
    std::array<CanopyBlob, kTreeBlobs> out{};
    const float h = tree_height(seed, scale);
    for (int i = 0; i < kTreeBlobs; ++i) {
        const float a = yaw + static_cast<float>(i) * 1.256f + shape_hash(seed, static_cast<std::uint32_t>(i)) * 0.6f;
        const float rr = (i == 0 ? 0.0f : 0.9f + shape_hash(seed, 10 + static_cast<std::uint32_t>(i)) * 0.6f) * scale;
        const float cz = z + h * 0.6f + (i == 0 ? 0.9f : shape_hash(seed, 20 + static_cast<std::uint32_t>(i)) * 1.2f) * scale;
        const float rad = (1.5f + shape_hash(seed, 30 + static_cast<std::uint32_t>(i)) * 0.5f) * scale;
        out[static_cast<std::size_t>(i)] = {x + std::cos(a) * rr, y + std::sin(a) * rr, cz, rad};
    }
    return out;
}

// Distance from a point to the nearest canopy surface of `p` (negative inside a blob).
[[nodiscard]] inline float canopy_clearance(const StreetProp& p, float x, float y, float z) {
    float best = 1e9f;
    for (const CanopyBlob& b : tree_blobs(static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z), static_cast<float>(p.yaw), static_cast<float>(p.scale), tree_seed(p))) {
        const float d = std::sqrt((x - b.x) * (x - b.x) + (y - b.y) * (y - b.y) + (z - b.z) * (z - b.z)) - b.r;
        best = std::min(best, d);
    }
    return best;
}

} // namespace saige::alive
