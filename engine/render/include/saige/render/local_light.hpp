// The host side of local lights (point and spot; docs/rendering/local-lights.md): the 64-byte record the shaders read (shaders/scene/local_lights.hlsli, struct LocalLight) and a set that validates what goes into it.
//
// A light that cannot be lit correctly is refused here, not on the GPU: every number must be finite, colour and intensity non-negative, the radius positive, a spot's axis a unit vector and its angles ordered 0 <= inner <= outer <= pi
// with outer > 0. The set has a capacity; a light added beyond it is NOT stored and is counted (`rejected()`), so a scene that has more lights than the budget degrades visibly in the statistics instead of by memory corruption.
// Units are the shader's: `intensity` is irradiance in lux on a surface facing the light squarely at 1 m (candela is lux at 1 m); `radius` is the distance beyond which the light contributes exactly nothing (it is also the culling sphere).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace saige::render {

/// One light as the shaders read it: four float4. Offsets are part of the contract (static_asserts in local_light.cpp and a GPU test compare them with the shader).
struct LocalLightGpu {
    float position_radius[4];  ///< xyz: world position; w: radius (m)
    float color_intensity[4];  ///< rgb: linear hue; w: intensity (lux at 1 m)
    float direction_outer[4];  ///< xyz: unit axis (spot); w: cos(outer half angle), or -2 for a point light
    float cone[4];             ///< x: cos(inner half angle); y: 1 / max(cos(inner) - cos(outer), 1e-4); zw: 0
};
static_assert(sizeof(LocalLightGpu) == 64, "the light record is four float4");
static_assert(alignof(LocalLightGpu) == 4);

struct PointLightDesc {
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 3> color{1, 1, 1};
    float intensity = 1.0f;
    float radius = 10.0f;
};

struct SpotLightDesc {
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 3> direction{0, 0, -1};
    std::array<float, 3> color{1, 1, 1};
    float intensity = 1.0f;
    float radius = 10.0f;
    float inner_angle = 0.3f;  ///< half angle, radians
    float outer_angle = 0.5f;  ///< half angle, radians, in (0, pi]
};

class LocalLightSet {
public:
    /// `capacity` lights at most (1 to 65536).
    explicit LocalLightSet(uint32_t capacity);

    /// Stores the light and returns its index, or returns -1 when the set is full (the light is counted in rejected()). Throws std::invalid_argument for a light that is not valid (see the header comment).
    int add_point(const PointLightDesc& light);
    int add_spot(const SpotLightDesc& light);

    void clear() noexcept;
    [[nodiscard]] uint32_t size() const noexcept { return static_cast<uint32_t>(lights_.size()); }
    [[nodiscard]] uint32_t capacity() const noexcept { return capacity_; }
    /// Lights refused because the set was full since the last clear().
    [[nodiscard]] uint32_t rejected() const noexcept { return rejected_; }
    [[nodiscard]] std::span<const LocalLightGpu> lights() const noexcept { return lights_; }
    /// The bytes to upload (size() * 64 of them; empty for no lights).
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;

private:
    uint32_t capacity_;
    uint32_t rejected_ = 0;
    std::vector<LocalLightGpu> lights_;
};

} // namespace saige::render
