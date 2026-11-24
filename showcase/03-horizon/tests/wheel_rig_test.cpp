// The wheel measurement and the wheel's roll. The synthetic part always runs: a wheel posed with a baked steer, a baked spin and an offset, which must be measured and re-posed exactly. The model part runs
// against CarConcept when the assets are fetched (pass the asset directory as argv[1] or set SAIGE_HORIZON_ASSETS): every wheel of the real model, measured and re-posed, must turn about +Y.
#include "wheel_rig.hpp"

#include "saige/gfx_assets/gltf.hpp"
#include "saige/render/transform.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>

using namespace saige;
using namespace saige::horizon;
using render::Float3;

namespace {

int failures = 0;

void expect(bool ok, const char* what, double value = 0.0)
{
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s (%.6f)\n", what, value);
        ++failures;
    }
}

double angle_deg(Float3 a, Float3 b)
{
    const double d = static_cast<double>(a.x * b.x + a.y * b.y + a.z * b.z) / (std::sqrt(static_cast<double>(a.x * a.x + a.y * a.y + a.z * a.z)) * std::sqrt(static_cast<double>(b.x * b.x + b.y * b.y + b.z * b.z)));
    return std::acos(std::max(-1.0, std::min(1.0, d))) * 180.0 / 3.14159265358979323846;
}

// A tire and a rim and a brake disc, built about +Y at the origin, then posed by `pose`.
std::vector<Float3> make_wheel(const render::Mat4& pose)
{
    std::vector<Float3> pts;
    const float pi = 3.14159265f;
    for (int i = 0; i < 360; ++i) {
        const float a = static_cast<float>(i) * pi / 180.0f * 1.0f + 0.003f * static_cast<float>(i * i % 7);  // not evenly spaced: the fit must not depend on the density
        const float c = std::cos(a), s = std::sin(a);
        for (const float r : {0.384f, 0.30f, 0.20f}) {
            for (const float y : {-0.14f, -0.05f, 0.05f, 0.14f}) pts.push_back(render::transform_point(pose, {r * c, y, r * s}));
        }
    }
    return pts;
}

} // namespace

int main(int argc, char** argv)
{
    // Roll direction: a car moving forward (+X) rolls the top of the wheel (+Z) forward.
    {
        const Float3 top = render::transform_point(wheel_spin(0.1f), {0, 0, 1});
        expect(top.x > 0.09f, "positive spin moves the top of the wheel forward", static_cast<double>(top.x));
        const Float3 bottom = render::transform_point(wheel_spin(0.1f), {0, 0, -1});
        expect(bottom.x < -0.09f, "positive spin moves the bottom of the wheel backward", static_cast<double>(bottom.x));
        const Float3 axle = render::transform_point(wheel_spin(1.234f), {0, 1, 0});
        expect(std::fabs(axle.y - 1.0f) < 1e-6f, "the roll leaves the axle where it is", static_cast<double>(axle.y));
    }
    // A wheel posed the way CarConcept's front wheels are: about 30 degrees of baked steer, an arbitrary spin, a wheel center away from the origin.
    for (const float yaw_deg : {0.0f, 30.0f, -30.0f, 7.5f}) {
        for (const float spin_deg : {0.0f, 110.0f, -55.0f}) {
            const float rad = 3.14159265f / 180.0f;
            render::Mat4 pose = render::Mat4::identity();
            pose.m[12] = 1.4857f;
            pose.m[13] = 0.9755f;
            pose.m[14] = 0.3838f;
            pose = pose * render::to_mat4(render::from_axis_angle({0, 0, 1}, yaw_deg * rad)) * render::to_mat4(render::from_axis_angle({0, 1, 0}, spin_deg * rad));
            const std::vector<Float3> pts = make_wheel(pose);
            const WheelFit fit = fit_wheel(pts);
            expect(fit.valid, "fit is valid");
            const Float3 want_axle = render::transform_point(pose, {0, 1, 0}) - render::transform_point(pose, {0, 0, 0});
            expect(angle_deg(fit.axle, want_axle) < 0.05, "axle recovered", angle_deg(fit.axle, want_axle));
            const double center_error = std::sqrt(static_cast<double>((fit.center.x - 1.4857f) * (fit.center.x - 1.4857f) + (fit.center.y - 0.9755f) * (fit.center.y - 0.9755f) + (fit.center.z - 0.3838f) * (fit.center.z - 0.3838f)));
            expect(center_error < 1e-3, "center recovered", center_error);
            expect(std::fabs(fit.radius - 0.384f) < 1e-3f, "radius recovered", static_cast<double>(fit.radius));
            expect(std::fabs(fit.half_width - 0.14f) < 1e-3f, "width recovered", static_cast<double>(fit.half_width));
            // Re-posed: center at the origin, axle on +Y, whatever the pose was.
            const render::Mat4 rig = wheel_to_rig_frame(fit);
            std::vector<Float3> posed;
            for (const Float3& p : pts) posed.push_back(render::transform_point(rig, p));
            const WheelFit after = fit_wheel(posed);
            expect(angle_deg(after.axle, {0, 1, 0}) < 0.05, "re-posed axle is +Y", angle_deg(after.axle, {0, 1, 0}));
            expect(std::fabs(after.center.x) < 1e-3f && std::fabs(after.center.y) < 1e-3f && std::fabs(after.center.z) < 1e-3f, "re-posed center is the origin");
            // The visible wobble: a wheel rolled about +Y through its center sweeps the circles of radius hypot(x, z) about that line, but it is built from circles about its own axle. They are the same circles exactly when
            // the axle is +Y; otherwise the tire swings in and out of its own rolling circle as it turns.
            const auto mismatch = [&](const std::vector<Float3>& points, Float3 center, Float3 axle) {
                double worst = 0.0;
                for (const Float3& p : points) {
                    const Float3 d = p - center;
                    const double along = static_cast<double>(d.x * axle.x + d.y * axle.y + d.z * axle.z);
                    const double own = std::sqrt(std::max(0.0, static_cast<double>(d.x * d.x + d.y * d.y + d.z * d.z) - along * along));
                    worst = std::max(worst, std::fabs(own - std::hypot(static_cast<double>(d.x), static_cast<double>(d.z))));
                }
                return worst;
            };
            const double posed_mismatch = mismatch(posed, after.center, after.axle);
            expect(posed_mismatch < 1e-3, "no wobble when the re-posed wheel rolls about +Y", posed_mismatch);
            if (std::fabs(yaw_deg) >= 30.0f) {
                // The control: the wheel as authored, rolled about +Y through its center, does wobble (so this check can fail).
                const double raw_mismatch = mismatch(pts, fit.center, fit.axle);
                expect(raw_mismatch > 0.05, "control: an un-posed wheel with a baked steer is off its rolling circle", raw_mismatch);
            }
        }
    }
    // The real model.
    const char* env = std::getenv("SAIGE_HORIZON_ASSETS");
    const std::filesystem::path root = argc > 1 ? argv[1] : env != nullptr ? env : "";
    const auto car = root / "khronos" / "CarConcept.glb";
    if (root.empty() || !std::filesystem::exists(car)) {
        std::printf("synthetic wheels checked; the CarConcept model is not available (pass the asset directory or set SAIGE_HORIZON_ASSETS): that part skipped\n");
        return failures == 0 ? 0 : 1;
    }
    const auto loaded = gfx::load_gltf_file(car);
    if (!loaded.ok()) {
        std::fprintf(stderr, "FAIL: CarConcept does not load: %s\n", loaded.error.c_str());
        return 1;
    }
    const auto masks = wheel_masks(*loaded.asset);
    for (std::size_t w = 0; w < 4; ++w) {
        const std::vector<Float3> pts = wheel_points(*loaded.asset, masks[w]);
        const WheelFit fit = fit_wheel(pts);
        expect(fit.valid && pts.size() > 5000, "wheel has geometry", static_cast<double>(pts.size()));
        // The tire is as big as the simulated wheel (0.384 m) and 0.28 m or so wide.
        expect(std::fabs(fit.radius - 0.384f) < 0.01f, "model tire radius matches the vehicle's wheel radius", static_cast<double>(fit.radius));
        expect(fit.half_width > 0.10f && fit.half_width < 0.20f, "model tire width plausible", static_cast<double>(fit.half_width));
        const render::Mat4 rig = wheel_to_rig_frame(fit);
        std::vector<Float3> posed;
        for (const Float3& p : pts) posed.push_back(render::transform_point(rig, p));
        const WheelFit after = fit_wheel(posed);
        const double tilt = angle_deg(after.axle, {0, 1, 0});
        std::printf("wheel %zu: authored axle (%.3f, %.3f, %.3f) = %.2f deg from +Y; radius %.3f, half width %.3f; after re-posing %.3f deg from +Y\n", w, static_cast<double>(fit.axle.x), static_cast<double>(fit.axle.y),
                    static_cast<double>(fit.axle.z), angle_deg(fit.axle, {0, 1, 0}), static_cast<double>(fit.radius), static_cast<double>(fit.half_width), tilt);
        expect(tilt < 0.05, "re-posed model wheel turns about +Y", tilt);
        expect(std::fabs(after.center.x) < 2e-3f && std::fabs(after.center.y) < 2e-3f && std::fabs(after.center.z) < 2e-3f, "re-posed model wheel is centered");
    }
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
