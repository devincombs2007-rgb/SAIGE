// Showcase 03: measuring a wheel from its geometry. A vehicle model's wheel nodes are posed however the artist left them (CarConcept's front wheels are authored with about 30 degrees of steer baked into
// the node matrices, and every wheel with an arbitrary spin), so neither the node's translation nor its axes can be trusted as "the axle". The wheel is instead measured from its vertices: the axle is the
// direction in which the wheel is thin, the center is the middle of the circle the wheel sweeps. `wheel_to_rig_frame` then re-poses the wheel so that its center is the origin and its axle is +Y, which is
// the frame the vehicle simulation spins (about +Y) and steers (about +Z) in. Spinning a wheel about an axle that is not the wheel's own wobbles it; this is the guard against that.
#pragma once

#include "saige/gfx_assets/gltf.hpp"
#include "saige/render/math.hpp"
#include "saige/render/transform.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

namespace saige::horizon {

struct WheelFit {
    render::Float3 center{};        // the center of the wheel's circle, in the frame the points were given in
    render::Float3 axle{0, 1, 0};   // unit direction of the axle, with a non-negative +Y component
    float radius = 0.0f;            // the largest distance of a point from the axle line
    float half_width = 0.0f;        // half the extent of the points along the axle
    bool valid = false;
};

namespace detail {

// Eigenvector of the smallest eigenvalue of a symmetric 3x3 matrix (cyclic Jacobi rotations).
inline void smallest_eigenvector(double a[3][3], double out[3])
{
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int sweep = 0; sweep < 64; ++sweep) {
        const double off = std::fabs(a[0][1]) + std::fabs(a[0][2]) + std::fabs(a[1][2]);
        if (off < 1e-18) break;
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(a[p][q]) < 1e-30) continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
                for (int k = 0; k < 3; ++k) {  // A <- A * J
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {  // A <- J^T * A
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {  // V <- V * J
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    int best = 0;
    for (int i = 1; i < 3; ++i) {
        if (a[i][i] < a[best][best]) best = i;
    }
    for (int k = 0; k < 3; ++k) out[k] = v[k][best];
}

} // namespace detail

// Fits the axle and center of a wheel from points on its tire, rim and brake disc (anything that is symmetric about the axle; not a brake caliper). Needs at least a few non-degenerate points.
[[nodiscard]] inline WheelFit fit_wheel(const std::vector<render::Float3>& points)
{
    WheelFit fit;
    if (points.size() < 8) return fit;
    double mean[3] = {0, 0, 0};
    for (const render::Float3& p : points) {
        mean[0] += static_cast<double>(p.x);
        mean[1] += static_cast<double>(p.y);
        mean[2] += static_cast<double>(p.z);
    }
    const double n = static_cast<double>(points.size());
    for (double& m : mean) m /= n;
    double cov[3][3] = {};
    for (const render::Float3& p : points) {
        const double d[3] = {static_cast<double>(p.x) - mean[0], static_cast<double>(p.y) - mean[1], static_cast<double>(p.z) - mean[2]};
        for (int i = 0; i < 3; ++i) {
            for (int j = 0; j < 3; ++j) cov[i][j] += d[i] * d[j];
        }
    }
    double axle[3];
    detail::smallest_eigenvector(cov, axle);
    if (axle[1] < 0.0) {
        for (double& a : axle) a = -a;
    }
    // A basis of the plane the wheel turns in.
    double ref[3] = {0, 0, 1};
    if (std::fabs(axle[2]) > 0.9) {
        ref[0] = 1;
        ref[2] = 0;
    }
    double e1[3] = {axle[1] * ref[2] - axle[2] * ref[1], axle[2] * ref[0] - axle[0] * ref[2], axle[0] * ref[1] - axle[1] * ref[0]};
    const double l1 = std::sqrt(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]);
    for (double& e : e1) e /= l1;
    const double e2[3] = {axle[1] * e1[2] - axle[2] * e1[1], axle[2] * e1[0] - axle[0] * e1[2], axle[0] * e1[1] - axle[1] * e1[0]};
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    for (const render::Float3& p : points) {
        const double d[3] = {static_cast<double>(p.x) - mean[0], static_cast<double>(p.y) - mean[1], static_cast<double>(p.z) - mean[2]};
        const double c[3] = {d[0] * axle[0] + d[1] * axle[1] + d[2] * axle[2], d[0] * e1[0] + d[1] * e1[1] + d[2] * e1[2], d[0] * e2[0] + d[1] * e2[1] + d[2] * e2[2]};
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], c[k]);
            hi[k] = std::max(hi[k], c[k]);
        }
    }
    // The middle of the extent along each of the three directions: for a full circle, the middle of its extent along any diameter is its center, whatever the density of the vertices.
    const double mid[3] = {(lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5};
    double center[3];
    for (int k = 0; k < 3; ++k) center[k] = mean[k] + mid[0] * axle[k] + mid[1] * e1[k] + mid[2] * e2[k];
    double radius = 0.0;
    for (const render::Float3& p : points) {
        const double d[3] = {static_cast<double>(p.x) - center[0], static_cast<double>(p.y) - center[1], static_cast<double>(p.z) - center[2]};
        const double along = d[0] * axle[0] + d[1] * axle[1] + d[2] * axle[2];
        const double r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - along * along;
        radius = std::max(radius, std::sqrt(std::max(r2, 0.0)));
    }
    fit.center = {static_cast<float>(center[0]), static_cast<float>(center[1]), static_cast<float>(center[2])};
    fit.axle = {static_cast<float>(axle[0]), static_cast<float>(axle[1]), static_cast<float>(axle[2])};
    fit.radius = static_cast<float>(radius);
    fit.half_width = static_cast<float>((hi[0] - lo[0]) * 0.5);
    fit.valid = true;
    return fit;
}

// The matrix that puts the fitted wheel at the origin with its axle along +Y (a rotation about the center, after moving it there).
[[nodiscard]] inline render::Mat4 wheel_to_rig_frame(const WheelFit& fit)
{
    render::Mat4 move = render::Mat4::identity();
    move.m[12] = -fit.center.x;
    move.m[13] = -fit.center.y;
    move.m[14] = -fit.center.z;
    const render::Float3 a = render::normalize(fit.axle);
    const render::Float3 cross{a.y * 0.0f - a.z * 1.0f, a.z * 0.0f - a.x * 0.0f, a.x * 1.0f - a.y * 0.0f};  // a x (0,1,0)
    const float s = std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z);
    if (s < 1e-6f) return move;
    return render::to_mat4(render::from_axis_angle(cross, std::atan2(s, a.y))) * move;
}

// The roll of a wheel about its axle (+Y, the car's left) for the vehicle's spin angle, which is positive when the car moves forward (+X). A right-handed rotation about +Y carries +Z toward +X, so the top of
// the wheel moves forward for positive angles, which is what a rolling wheel does.
[[nodiscard]] inline render::Mat4 wheel_spin(float spin) { return render::to_mat4(render::from_axis_angle({0.0f, 1.0f, 0.0f}, spin)); }

// Which instances of CarConcept belong to which wheel (front left, front right, rear left, rear right). The model lists a wheel node (named WheelFrontL...), then its rim, brake disc and brake pad, and
// the tire, whose node has no name, so an unnamed node belongs to the wheel before it.
[[nodiscard]] inline std::array<std::vector<bool>, 4> wheel_masks(const gfx::GltfAsset& asset)
{
    std::array<std::vector<bool>, 4> masks;
    for (auto& m : masks) m.assign(asset.instances.size(), false);
    int current = -1;
    const auto starts = [](const std::string& s, const char* p) { return s.rfind(p, 0) == 0; };
    for (std::size_t i = 0; i < asset.instances.size(); ++i) {
        const std::string& nm = asset.instances[i].name;
        if (starts(nm, "WheelFrontL")) current = 0;
        else if (starts(nm, "WheelFrontR")) current = 1;
        else if (starts(nm, "WheelRearL")) current = 2;
        else if (starts(nm, "WheelRearR")) current = 3;
        else if (nm == "None" || nm.empty()) {
            // the tire: it follows the rim and the brake of its wheel
        } else current = -1;
        if (current >= 0) masks[static_cast<std::size_t>(current)][i] = true;
    }
    return masks;
}

// The wheel's points in the frame of its car, from the instances of a staged model that make up the wheel. `keep` marks the instances; brake calipers (pads) are left out because they are not symmetric
// about the axle.
[[nodiscard]] inline std::vector<render::Float3> wheel_points(const gfx::GltfAsset& asset, const std::vector<bool>& keep)
{
    std::vector<render::Float3> pts;
    for (std::size_t i = 0; i < asset.instances.size() && i < keep.size(); ++i) {
        const gfx::MeshInstance& inst = asset.instances[i];
        if (!keep[i] || inst.mesh < 0 || inst.name.find("BrakePad") != std::string::npos) continue;
        for (const gfx::Primitive& prim : asset.meshes[static_cast<std::size_t>(inst.mesh)].primitives) {
            for (std::size_t v = 0; v + 2 < prim.positions.size(); v += 3) {
                pts.push_back(render::transform_point(inst.world, {prim.positions[v], prim.positions[v + 1], prim.positions[v + 2]}));
            }
        }
    }
    return pts;
}

} // namespace saige::horizon
