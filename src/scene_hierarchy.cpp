#include "saige/scene_hierarchy.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace saige {
namespace {

bool finite(Vec3 vector) {
    return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

WorldMatrix local_matrix(const LocalTransform& transform) {
    const auto [px, py, pz] = transform.position;
    const auto [sx, sy, sz] = transform.scale;
    const auto [qx, qy, qz, qw] = transform.rotation;
    const auto length = std::hypot(std::hypot(static_cast<double>(qx), static_cast<double>(qy)),
                                   std::hypot(static_cast<double>(qz), static_cast<double>(qw)));
    if (!finite(transform.position) || !finite(transform.scale) ||
        sx <= 0 || sy <= 0 || sz <= 0 || !std::isfinite(length) || length == 0) {
        throw std::invalid_argument("Invalid local scene transform");
    }

    const auto x = static_cast<float>(qx / length);
    const auto y = static_cast<float>(qy / length);
    const auto z = static_cast<float>(qz / length);
    const auto w = static_cast<float>(qw / length);
    return {
        sx * (1 - 2 * (y*y + z*z)), sx * (2 * (x*y + z*w)), sx * (2 * (x*z - y*w)), 0,
        sy * (2 * (x*y - z*w)), sy * (1 - 2 * (x*x + z*z)), sy * (2 * (y*z + x*w)), 0,
        sz * (2 * (x*z + y*w)), sz * (2 * (y*z - x*w)), sz * (1 - 2 * (x*x + y*y)), 0,
        px, py, pz, 1
    };
}

WorldMatrix multiply(const WorldMatrix& parent, const WorldMatrix& local) {
    WorldMatrix result{};
    for (std::size_t column = 0; column < 4; ++column) {
        for (std::size_t row = 0; row < 4; ++row) {
            for (std::size_t k = 0; k < 4; ++k) {
                result[column * 4 + row] += parent[k * 4 + row] * local[column * 4 + k];
            }
        }
    }
    return result;
}
} // namespace

std::vector<WorldMatrix> resolve_world_matrices(std::span<const SceneNode> nodes) {
    enum class State : unsigned char { unseen, visiting, resolved };
    std::vector<WorldMatrix> matrices(nodes.size());
    std::vector<State> states(nodes.size(), State::unseen);
    std::vector<std::size_t> path;

    for (std::size_t start = 0; start < nodes.size(); ++start) {
        auto current = std::optional{start};
        while (current) {
            const auto node = *current;
            if (node >= nodes.size()) {
                throw std::invalid_argument("Scene parent index is out of range");
            }
            if (states[node] == State::resolved) {
                break;
            }
            if (states[node] == State::visiting) {
                throw std::invalid_argument("Scene hierarchy contains a cycle");
            }
            states[node] = State::visiting;
            path.push_back(node);
            current = nodes[node].parent;
        }

        while (!path.empty()) {
            const auto node = path.back();
            path.pop_back();
            const auto local = local_matrix(nodes[node].local);
            const auto parent = nodes[node].parent;
            const auto world = parent ? multiply(matrices[*parent], local) : local;
            if (!std::ranges::all_of(world, [](float value) { return std::isfinite(value); })) {
                throw std::invalid_argument("Scene world transform exceeds numeric range");
            }
            matrices[node] = world;
            states[node] = State::resolved;
        }
    }
    return matrices;
}

} // namespace saige
