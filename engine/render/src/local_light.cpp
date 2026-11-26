#include "saige/render/local_light.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>

namespace saige::render {

static_assert(offsetof(LocalLightGpu, position_radius) == 0);
static_assert(offsetof(LocalLightGpu, color_intensity) == 16);
static_assert(offsetof(LocalLightGpu, direction_outer) == 32);
static_assert(offsetof(LocalLightGpu, cone) == 48);

namespace {

constexpr float kMaxIntensity = 1.0e9f;
constexpr float kMaxRadius = 1.0e5f;
constexpr float kMaxCoordinate = 1.0e7f;
constexpr double kPi = 3.14159265358979323846;

bool finite3(const std::array<float, 3>& v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

void validate_common(const std::array<float, 3>& position, const std::array<float, 3>& color, float intensity, float radius)
{
    if (!finite3(position) || std::fabs(position[0]) > kMaxCoordinate || std::fabs(position[1]) > kMaxCoordinate || std::fabs(position[2]) > kMaxCoordinate) {
        throw std::invalid_argument("LocalLightSet: the position must be finite and within 1e7 m");
    }
    if (!finite3(color) || color[0] < 0.0f || color[1] < 0.0f || color[2] < 0.0f) throw std::invalid_argument("LocalLightSet: the colour must be finite and non-negative");
    if (!std::isfinite(intensity) || intensity < 0.0f || intensity > kMaxIntensity) throw std::invalid_argument("LocalLightSet: the intensity must be finite, non-negative and at most 1e9");
    if (!std::isfinite(radius) || !(radius > 0.0f) || radius > kMaxRadius) throw std::invalid_argument("LocalLightSet: the radius must be finite, positive and at most 1e5 m");
}

} // namespace

LocalLightSet::LocalLightSet(uint32_t capacity) : capacity_(capacity)
{
    if (capacity < 1 || capacity > 65536) throw std::invalid_argument("LocalLightSet: the capacity must be 1 to 65536");
    lights_.reserve(capacity);
}

int LocalLightSet::add_point(const PointLightDesc& l)
{
    validate_common(l.position, l.color, l.intensity, l.radius);
    if (lights_.size() >= capacity_) {
        ++rejected_;
        return -1;
    }
    LocalLightGpu g{};
    g.position_radius[0] = l.position[0]; g.position_radius[1] = l.position[1]; g.position_radius[2] = l.position[2]; g.position_radius[3] = l.radius;
    g.color_intensity[0] = l.color[0]; g.color_intensity[1] = l.color[1]; g.color_intensity[2] = l.color[2]; g.color_intensity[3] = l.intensity;
    g.direction_outer[3] = -2.0f;
    g.cone[0] = 1.0f;
    g.cone[1] = 1.0f;
    lights_.push_back(g);
    return static_cast<int>(lights_.size()) - 1;
}

int LocalLightSet::add_spot(const SpotLightDesc& l)
{
    validate_common(l.position, l.color, l.intensity, l.radius);
    if (!finite3(l.direction)) throw std::invalid_argument("LocalLightSet: the spot axis must be finite");
    const double dx = static_cast<double>(l.direction[0]), dy = static_cast<double>(l.direction[1]), dz = static_cast<double>(l.direction[2]);
    const double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (std::fabs(len - 1.0) > 1e-3) throw std::invalid_argument("LocalLightSet: the spot axis must be a unit vector");
    if (!std::isfinite(l.inner_angle) || !std::isfinite(l.outer_angle) || !(l.outer_angle > 0.0f) || l.outer_angle > static_cast<float>(kPi) || l.inner_angle < 0.0f || l.inner_angle > l.outer_angle) {
        throw std::invalid_argument("LocalLightSet: a spot needs 0 <= inner <= outer <= pi and outer > 0");
    }
    if (lights_.size() >= capacity_) {
        ++rejected_;
        return -1;
    }
    const double cos_outer = std::cos(static_cast<double>(l.outer_angle)), cos_inner = std::cos(static_cast<double>(l.inner_angle));
    LocalLightGpu g{};
    g.position_radius[0] = l.position[0]; g.position_radius[1] = l.position[1]; g.position_radius[2] = l.position[2]; g.position_radius[3] = l.radius;
    g.color_intensity[0] = l.color[0]; g.color_intensity[1] = l.color[1]; g.color_intensity[2] = l.color[2]; g.color_intensity[3] = l.intensity;
    g.direction_outer[0] = l.direction[0]; g.direction_outer[1] = l.direction[1]; g.direction_outer[2] = l.direction[2]; g.direction_outer[3] = static_cast<float>(cos_outer);
    g.cone[0] = static_cast<float>(cos_inner);
    g.cone[1] = static_cast<float>(1.0 / std::max(cos_inner - cos_outer, 1e-4));
    lights_.push_back(g);
    return static_cast<int>(lights_.size()) - 1;
}

void LocalLightSet::clear() noexcept
{
    lights_.clear();
    rejected_ = 0;
}

std::span<const std::byte> LocalLightSet::bytes() const noexcept
{
    return std::as_bytes(std::span<const LocalLightGpu>(lights_));
}

} // namespace saige::render
