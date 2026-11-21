#pragma once

#include "saige/world.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace saige {

struct Quaternion {
    float x{}, y{}, z{}, w{1.0F};
};

struct LocalTransform {
    Vec3 position{};
    Quaternion rotation{};
    Vec3 scale{1.0F, 1.0F, 1.0F};
};

struct SceneNode {
    LocalTransform local{};
    std::optional<std::size_t> parent{};
};

// Column-major matrix applied to column vectors. Translation is at indices 12–14.
using WorldMatrix = std::array<float, 16>;

// Resolve a flat scene into world matrices in input order. Invalid indices,
// cycles, nonfinite poses and nonpositive scales throw std::invalid_argument.
[[nodiscard]] std::vector<WorldMatrix> resolve_world_matrices(std::span<const SceneNode> nodes);

} // namespace saige
