#include "actors.hpp"

#include "../../03-horizon/src/wheel_rig.hpp"
#include "saige/animation/humanoid.hpp"
#include "saige/animation/locomotion.hpp"
#include "saige/gfx_assets/animation.hpp"
#include "saige/gfx_scene/stage_model.hpp"
#include "sea.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>

namespace saige::alive::rend {

namespace {

float hashf(std::uint32_t a, std::uint32_t b = 0) {
    std::uint32_t h = a * 0x9e3779b1u ^ (b + 0x7f4a7c15u) * 0x85ebca6bu;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    h *= 0x297a2d39u;
    h ^= h >> 15;
    return static_cast<float>(h & 0xffffff) / 16777216.0f;
}

render::GpuModel filter_placements(const render::GpuModel& model, const std::vector<bool>& keep, const Mat4& pre) {
    render::GpuModel out = model;
    out.placements.clear();
    for (std::size_t i = 0; i < model.placements.size() && i < keep.size(); ++i) {
        if (!keep[i]) continue;
        render::GpuPlacement p = model.placements[i];
        p.local = pre * p.local;
        out.placements.push_back(p);
    }
    return out;
}

struct CarRig {
    render::GpuModel body;
    std::array<render::GpuModel, 4> wheel, hub;
    std::array<Float3, 4> wc{};
    Float3 lo{}, hi{};
    bool ok = false;
};

constexpr Float3 kPaints[] = {{0.666f, 0.0f, 0.0f}, {0.60f, 0.60f, 0.58f}, {0.035f, 0.035f, 0.04f}, {0.03f, 0.10f, 0.45f}, {0.85f, 0.60f, 0.03f}, {0.03f, 0.28f, 0.12f}, {0.38f, 0.40f, 0.44f}};
constexpr int kPaintCount = 7;

} // namespace

struct Actors::Impl {
    render::SceneRenderer& r;
    const Materials& mats;
    const District& d;
    fs::path cache, anim_root;
    std::vector<std::string>& warnings;
    bool fixtures = false;   // the fixture path: a procedural car and no residents (no third-party model is read)

    // cars
    std::vector<CarRig> cars;   // one per paint
    // procedural vehicles
    struct Truck {
        render::GpuModel body, wheel;
        render::GpuModel bar[3];   // lights: off, A (blue/red), B (red/blue)
        std::array<Float3, 4> centre_rest{};
        float radius = 0.4f;
        bool bar_present = false;
        Float3 bar_at{};
    };
    Truck vans[4], ambulance, engine;
    render::GpuModel boat_models[3];
    // sea
    std::unique_ptr<WaterSurface> sea;
    std::unique_ptr<render::GpuModel> sea_model;
    // smoke
    render::GpuModel puffs[4];
    render::GpuModel spark;
    render::GpuModel hazard[2];   // off, on (one small lamp at each corner of a unit box, scaled to the vehicle)
    struct Fx {
        double t0;
        double x, y, z;
        float energy;
        std::uint32_t seed;
    };
    std::vector<Fx> fx;
    // people
    std::unique_ptr<gfx::GltfAsset> ual;
    render::GpuModel person;
    std::vector<render::SkinPalette> palettes;
    std::unique_ptr<anim::LocomotionSet> set_idle, set_talk;
    struct Anim {
        std::unique_ptr<anim::LocomotionController> ctl;
        bool talk = false;
    };
    std::map<std::uint64_t, Anim> anims;
    std::map<std::uint64_t, std::unique_ptr<render::GpuModel>> outfits;
    std::vector<std::pair<std::uint64_t, const PedView*>> drawn_;
    std::vector<PedView> last_peds;

    Impl(render::SceneRenderer& renderer, const Materials& m, const District& dist, const fs::path& c, const fs::path& a, std::vector<std::string>& w) : r(renderer), mats(m), d(dist), cache(c), anim_root(a), warnings(w) {}

    // ---- cars ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    CarRig stage_car(Float3 paint) {
        CarRig rig;
        gfx::GltfLoadResult loaded = gfx::load_gltf_file(cache / "khronos" / "CarConcept" / "CarConcept.glb");
        if (!loaded.ok()) throw std::runtime_error("CarConcept: " + loaded.error);
        gfx::GltfAsset& asset = *loaded.asset;
        for (gfx::Material& m : asset.materials)
            if (m.name == "Paint 1 Carmine") {
                m.base_color[0] = paint.x;
                m.base_color[1] = paint.y;
                m.base_color[2] = paint.z;
            }
        gfx::StageSettings ss;
        ss.max_anisotropy = 8.0f;
        gfx::StageReport rep;
        const render::GpuModel full = gfx::stage_model(r, asset, "CarConcept", ss, &rep);
        for (const std::string& w : rep.warnings) warnings.push_back("CarConcept: " + w);
        const std::size_t n = asset.instances.size();
        const std::array<std::vector<bool>, 4> wheel = saige::horizon::wheel_masks(asset);
        std::vector<bool> body(n, false), everything(n, true);
        for (std::size_t i = 0; i < n; ++i) body[i] = !(wheel[0][i] || wheel[1][i] || wheel[2][i] || wheel[3][i]);
        if (full.placements.size() != n) throw std::runtime_error("CarConcept: placements and instances differ");
        rig.body = filter_placements(full, body, Mat4::identity());
        const auto bounds_of = [&](const std::vector<bool>& keep, Float3& lo, Float3& hi) {
            lo = {1e30f, 1e30f, 1e30f};
            hi = {-1e30f, -1e30f, -1e30f};
            for (std::size_t i = 0; i < n; ++i) {
                if (!keep[i] || asset.instances[i].mesh < 0) continue;
                for (const gfx::Primitive& p : asset.meshes[static_cast<std::size_t>(asset.instances[i].mesh)].primitives) {
                    if (!p.bounds.valid()) continue;
                    for (int c = 0; c < 8; ++c) {
                        const Float3 q{(c & 1) ? p.bounds.max.x : p.bounds.min.x, (c & 2) ? p.bounds.max.y : p.bounds.min.y, (c & 4) ? p.bounds.max.z : p.bounds.min.z};
                        const Float3 w = render::transform_point(asset.instances[i].world, q);
                        lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
                        hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
                    }
                }
            }
        };
        bounds_of(body, rig.lo, rig.hi);
        const saige::horizon::WheelPartMasks parts = saige::horizon::split_wheel_masks(asset, wheel);
        for (std::size_t w = 0; w < 4; ++w) {
            const saige::horizon::WheelFit fit = saige::horizon::fit_wheel(saige::horizon::wheel_points(asset, wheel[w]));
            rig.wc[w] = fit.center;
            rig.wheel[w] = filter_placements(full, parts.spinning[w], saige::horizon::wheel_to_rig_frame(fit));
            rig.hub[w] = filter_placements(full, parts.fixed[w], saige::horizon::wheel_to_rig_frame(fit));
        }
        rig.ok = true;
        return rig;
    }

    // The fixture car: a procedural sedan in the vehicle frame (boxes and the shared wheel), used by the fixture path (no third-party car model). Same rig contract as stage_car.
    CarRig stage_fixture_car(int paint_index) {
        CarRig rig;
        const VehicleConfig cfg = vehicle_config_for(VehicleKind::Sedan);
        const float g = cfg.wheels[0].mount.z - cfg.suspension_travel + 0.17f - cfg.wheel_radius;
        const Mat paint = static_cast<Mat>(static_cast<int>(MCar0) + paint_index % 8);
        MeshBuilder mb;
        const float hx = cfg.half_extents.x, hy = cfg.half_extents.y;
        mb.box(paint, {-hx, -hy, g + 0.30f}, {hx, hy, g + 0.85f}, 63);                               // body
        mb.box(paint, {-hx * 0.45f, -hy * 0.9f, g + 0.85f}, {hx * 0.40f, hy * 0.9f, g + 1.38f}, 63);   // cabin
        mb.quad(MGlassDark, {hx * 0.40f, -hy * 0.86f, g + 1.36f}, {hx * 0.40f, hy * 0.86f, g + 1.36f}, {hx * 0.62f, hy * 0.86f, g + 0.87f}, {hx * 0.62f, -hy * 0.86f, g + 0.87f});
        mb.box(MPaintDarkGrey, {-hx - 0.04f, -hy * 0.98f, g + 0.22f}, {hx + 0.04f, hy * 0.98f, g + 0.34f}, 63);   // bumpers
        mb.quad(MHeadlight, {hx + 0.041f, -hy * 0.85f, g + 0.55f}, {hx + 0.041f, -hy * 0.55f, g + 0.55f}, {hx + 0.041f, -hy * 0.55f, g + 0.7f}, {hx + 0.041f, -hy * 0.85f, g + 0.7f});
        mb.quad(MHeadlight, {hx + 0.041f, hy * 0.55f, g + 0.55f}, {hx + 0.041f, hy * 0.85f, g + 0.55f}, {hx + 0.041f, hy * 0.85f, g + 0.7f}, {hx + 0.041f, hy * 0.55f, g + 0.7f});
        rig.body = upload_model(r, "fixture_car", mb.finish(), mats.list());
        Truck tmp;
        build_wheel(tmp, cfg.wheel_radius, 0.24f);
        for (std::size_t w = 0; w < 4; ++w) {
            rig.wheel[w] = tmp.wheel;
            rig.wc[w] = {cfg.wheels[w].mount.x, cfg.wheels[w].mount.y, cfg.wheels[w].mount.z - cfg.suspension_travel};
        }
        rig.lo = {-hx, -hy, g};
        rig.hi = {hx, hy, g + 1.38f};
        rig.ok = true;
        return rig;
    }

    // ---- procedural trucks ----------------------------------------------------------------------------------------------------------------------------------------------------------
    void build_wheel(Truck& t, float radius, float width) {
        MeshBuilder mb;
        // A wheel about +Y: the cylinder is built about +Z and turned.
        MeshBuilder tire;
        tire.cylinder(MTire, {0, 0, -width * 0.5f}, radius, width, 18, true);
        tire.cylinder(MChrome, {0, 0, -width * 0.5f - 0.005f}, radius * 0.58f, width + 0.01f, 12, true);
        for (int k = 0; k < 5; ++k) {
            const float a = static_cast<float>(2.0 * kPi * k / 5);
            tire.box(MPaintDarkGrey, {std::cos(a) * radius * 0.35f - 0.04f, std::sin(a) * radius * 0.35f - 0.04f, -width * 0.5f - 0.012f}, {std::cos(a) * radius * 0.35f + 0.04f, std::sin(a) * radius * 0.35f + 0.04f, width * 0.5f + 0.012f}, 63);
        }
        t.wheel = upload_model(r, "wheel", tire.finish(), mats.list());
        // rotate Z-axis cylinder to Y axis: bake into the model's placement
        t.wheel.placements.front().local = rot_x(static_cast<float>(kPi * 0.5));
        t.radius = radius;
    }

    void build_trucks() {
        // The wheel model is shared in shape by every truck; each truck's body is built in the chassis frame with the ground at `g` below the centre.
        const auto cfg_van = vehicle_config_for(VehicleKind::Van);
        const auto cfg_amb = vehicle_config_for(VehicleKind::Ambulance);
        const auto cfg_eng = vehicle_config_for(VehicleKind::FireEngine);
        const auto ground = [](const VehicleConfig& c) { return c.wheels[0].mount.z - c.suspension_travel + 0.17f - c.wheel_radius; };
        const Mat paints[4] = {MCar2, MCar1, MCar6, MCar5};
        for (int v = 0; v < 4; ++v) {
            Truck& t = vans[v];
            const float g = ground(cfg_van);
            MeshBuilder mb;
            const Mat paint = paints[v];
            mb.box(paint, {-2.45f, -0.97f, g + 0.30f}, {0.95f, 0.97f, g + 2.15f}, 63);
            mb.box(paint, {0.95f, -0.97f, g + 0.30f}, {2.45f, 0.97f, g + 1.0f}, 63);
            mb.box(paint, {0.95f, -0.95f, g + 1.0f}, {1.9f, 0.95f, g + 1.55f}, 63);
            // windscreen and side windows
            mb.quad(MGlassDark, {1.9f, -0.93f, g + 1.55f}, {1.9f, 0.93f, g + 1.55f}, {2.3f, 0.92f, g + 1.0f}, {2.3f, -0.92f, g + 1.0f});
            mb.quad(MGlassDark, {1.0f, 0.955f, g + 1.1f}, {1.85f, 0.955f, g + 1.1f}, {1.85f, 0.955f, g + 1.5f}, {1.0f, 0.955f, g + 1.5f});
            mb.quad(MGlassDark, {1.85f, -0.955f, g + 1.1f}, {1.0f, -0.955f, g + 1.1f}, {1.0f, -0.955f, g + 1.5f}, {1.85f, -0.955f, g + 1.5f});
            mb.box(MPaintDarkGrey, {-2.5f, -0.98f, g + 0.22f}, {2.55f, 0.98f, g + 0.34f}, 63);   // bumpers and sills
            mb.quad(MHeadlight, {2.451f, -0.85f, g + 0.55f}, {2.451f, -0.55f, g + 0.55f}, {2.451f, -0.55f, g + 0.72f}, {2.451f, -0.85f, g + 0.72f});
            mb.quad(MHeadlight, {2.451f, 0.55f, g + 0.55f}, {2.451f, 0.85f, g + 0.55f}, {2.451f, 0.85f, g + 0.72f}, {2.451f, 0.55f, g + 0.72f});
            mb.quad(MTaillight, {-2.451f, 0.55f, g + 0.7f}, {-2.451f, 0.9f, g + 0.7f}, {-2.451f, 0.9f, g + 0.95f}, {-2.451f, 0.55f, g + 0.95f});
            mb.quad(MTaillight, {-2.451f, -0.9f, g + 0.7f}, {-2.451f, -0.55f, g + 0.7f}, {-2.451f, -0.55f, g + 0.95f}, {-2.451f, -0.9f, g + 0.95f});
            t.body = upload_model(r, "van", mb.finish(), mats.list());
        }
        build_wheel(vans[0], cfg_van.wheel_radius, 0.28f);
        for (int v = 1; v < 4; ++v) vans[v].wheel = vans[0].wheel;
        // ambulance
        {
            Truck& t = ambulance;
            const float g = ground(cfg_amb);
            MeshBuilder mb;
            mb.box(MAmbulanceWhite, {-2.8f, -1.0f, g + 0.35f}, {1.15f, 1.0f, g + 2.35f}, 63);
            mb.box(MAmbulanceWhite, {1.15f, -1.0f, g + 0.35f}, {2.8f, 1.0f, g + 1.1f}, 63);
            mb.box(MAmbulanceWhite, {1.15f, -0.98f, g + 1.1f}, {2.1f, 0.98f, g + 1.75f}, 63);
            mb.quad(MGlassDark, {2.1f, -0.94f, g + 1.75f}, {2.1f, 0.94f, g + 1.75f}, {2.6f, 0.94f, g + 1.1f}, {2.6f, -0.94f, g + 1.1f});
            mb.quad(MGlassDark, {1.2f, 1.002f, g + 1.2f}, {2.0f, 1.002f, g + 1.2f}, {2.0f, 1.002f, g + 1.7f}, {1.2f, 1.002f, g + 1.7f});
            mb.quad(MGlassDark, {2.0f, -1.002f, g + 1.2f}, {1.2f, -1.002f, g + 1.2f}, {1.2f, -1.002f, g + 1.7f}, {2.0f, -1.002f, g + 1.7f});
            // stripes
            mb.box(MEmergencyRed, {-2.8f, -1.005f, g + 0.95f}, {1.15f, 1.005f, g + 1.2f}, 63);
            mb.box(MEmergencyYellow, {-2.8f, -1.005f, g + 0.8f}, {1.15f, 1.005f, g + 0.88f}, 63);
            // a white cross on a red field on each side
            for (const float s : {1.0f, -1.0f}) {
                const float y = s * 1.012f;
                mb.quad(MEmergencyRed, {-1.5f, y, g + 1.45f}, {-0.5f, y, g + 1.45f}, {-0.5f, y, g + 2.15f}, {-1.5f, y, g + 2.15f});
            }
            mb.box(MPaintDarkGrey, {-2.85f, -1.0f, g + 0.25f}, {2.85f, 1.0f, g + 0.38f}, 63);
            mb.quad(MHeadlight, {2.801f, -0.88f, g + 0.6f}, {2.801f, -0.55f, g + 0.6f}, {2.801f, -0.55f, g + 0.8f}, {2.801f, -0.88f, g + 0.8f});
            mb.quad(MHeadlight, {2.801f, 0.55f, g + 0.6f}, {2.801f, 0.88f, g + 0.6f}, {2.801f, 0.88f, g + 0.8f}, {2.801f, 0.55f, g + 0.8f});
            mb.quad(MTaillight, {-2.801f, 0.5f, g + 0.7f}, {-2.801f, 0.95f, g + 0.7f}, {-2.801f, 0.95f, g + 1.0f}, {-2.801f, 0.5f, g + 1.0f});
            mb.quad(MTaillight, {-2.801f, -0.95f, g + 0.7f}, {-2.801f, -0.5f, g + 0.7f}, {-2.801f, -0.5f, g + 1.0f}, {-2.801f, -0.95f, g + 1.0f});
            mb.box(MPaintDarkGrey, {1.3f, -0.9f, g + 2.35f}, {1.5f, 0.9f, g + 2.45f}, 63);
            t.body = upload_model(r, "ambulance", mb.finish(), mats.list());
            build_wheel(t, cfg_amb.wheel_radius, 0.30f);
            t.bar_present = true;
            t.bar_at = {1.4f, 0.0f, g + 2.45f};
        }
        // fire engine
        {
            Truck& t = engine;
            const float g = ground(cfg_eng);
            MeshBuilder mb;
            mb.box(MEmergencyRed, {-4.0f, -1.1f, g + 0.45f}, {1.6f, 1.1f, g + 2.35f}, 63);
            mb.box(MEmergencyRed, {1.6f, -1.1f, g + 0.45f}, {4.0f, 1.1f, g + 2.9f}, 63);
            mb.quad(MGlassDark, {3.2f, -1.0f, g + 2.8f}, {3.2f, 1.0f, g + 2.8f}, {4.01f, 1.0f, g + 1.7f}, {4.01f, -1.0f, g + 1.7f});
            mb.quad(MGlassDark, {1.7f, 1.102f, g + 1.8f}, {3.1f, 1.102f, g + 1.8f}, {3.1f, 1.102f, g + 2.75f}, {1.7f, 1.102f, g + 2.75f});
            mb.quad(MGlassDark, {3.1f, -1.102f, g + 1.8f}, {1.7f, -1.102f, g + 1.8f}, {1.7f, -1.102f, g + 2.75f}, {3.1f, -1.102f, g + 2.75f});
            mb.box(MEmergencyYellow, {-4.0f, -1.105f, g + 1.1f}, {1.6f, 1.105f, g + 1.25f}, 63);
            for (const float s : {1.0f, -1.0f})
                for (int k = 0; k < 3; ++k) {
                    const float x0 = -3.8f + 1.8f * static_cast<float>(k);
                    mb.quad(MPaintSteel, {x0, s * 1.112f, g + 1.35f}, {x0 + 1.6f, s * 1.112f, g + 1.35f}, {x0 + 1.6f, s * 1.112f, g + 2.2f}, {x0, s * 1.112f, g + 2.2f});
                }
            // ladder on the roof
            mb.box(MPaintSteel, {-3.8f, -0.55f, g + 2.35f}, {1.2f, -0.45f, g + 2.5f}, 63);
            mb.box(MPaintSteel, {-3.8f, 0.45f, g + 2.35f}, {1.2f, 0.55f, g + 2.5f}, 63);
            for (float x = -3.7f; x < 1.2f; x += 0.35f) mb.box(MPaintSteel, {x, -0.5f, g + 2.4f}, {x + 0.05f, 0.5f, g + 2.47f}, 63);
            mb.box(MChrome, {4.0f, -1.0f, g + 0.45f}, {4.12f, 1.0f, g + 0.7f}, 63);
            mb.quad(MHeadlight, {4.001f, -0.9f, g + 0.8f}, {4.001f, -0.55f, g + 0.8f}, {4.001f, -0.55f, g + 1.05f}, {4.001f, -0.9f, g + 1.05f});
            mb.quad(MHeadlight, {4.001f, 0.55f, g + 0.8f}, {4.001f, 0.9f, g + 0.8f}, {4.001f, 0.9f, g + 1.05f}, {4.001f, 0.55f, g + 1.05f});
            mb.quad(MTaillight, {-4.001f, 0.6f, g + 0.8f}, {-4.001f, 1.0f, g + 0.8f}, {-4.001f, 1.0f, g + 1.1f}, {-4.001f, 0.6f, g + 1.1f});
            mb.quad(MTaillight, {-4.001f, -1.0f, g + 0.8f}, {-4.001f, -0.6f, g + 0.8f}, {-4.001f, -0.6f, g + 1.1f}, {-4.001f, -1.0f, g + 1.1f});
            t.body = upload_model(r, "fire engine", mb.finish(), mats.list());
            build_wheel(t, cfg_eng.wheel_radius, 0.38f);
            t.bar_present = true;
            t.bar_at = {2.9f, 0.0f, g + 2.9f};
        }
        // light bars: off, A, B
        for (Truck* t : {&ambulance, &engine}) {
            for (int state = 0; state < 3; ++state) {
                MeshBuilder mb;
                const Mat left = state == 0 ? MPaintDarkGrey : state == 1 ? MLightBlue : MLightRed;
                const Mat right = state == 0 ? MPaintDarkGrey : state == 1 ? MLightRed : MLightBlue;
                mb.box(MPaintBlack, {-0.12f, -0.8f, 0.0f}, {0.12f, 0.8f, 0.07f}, 63);
                mb.box(left, {-0.1f, 0.05f, 0.07f}, {0.1f, 0.75f, 0.2f}, 63);
                mb.box(right, {-0.1f, -0.75f, 0.07f}, {0.1f, -0.05f, 0.2f}, 63);
                t->bar[state] = upload_model(r, "lightbar", mb.finish(), mats.list());
            }
        }
    }

    // ---- boats ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    render::GpuModel build_boat(float length, float beam, Mat hull, bool sail) {
        MeshBuilder mb;
        const int stations = 9;
        const float hl = length * 0.5f, hb = beam * 0.5f;
        const auto half_beam = [&](float u) {   // u in [0,1] from stern to bow
            return hb * (u < 0.7f ? 1.0f - 0.08f * std::fabs(u - 0.4f) : std::max(0.0f, 1.0f - (u - 0.7f) / 0.3f * 0.95f));
        };
        const auto rise = [&](float u) { return 0.65f + (u > 0.6f ? (u - 0.6f) * 1.2f : 0.0f); };
        for (int i = 0; i + 1 < stations; ++i) {
            const float u0 = static_cast<float>(i) / (stations - 1), u1 = static_cast<float>(i + 1) / (stations - 1);
            const float x0 = -hl + u0 * length, x1 = -hl + u1 * length;
            const float b0 = half_beam(u0), b1 = half_beam(u1), z0 = rise(u0) - 0.55f, z1 = rise(u1) - 0.55f;
            // side panels and bottom
            mb.quad(hull, {x0, b0, 0.0f}, {x0, b0, z0}, {x1, b1, z1}, {x1, b1, 0.0f});
            mb.quad(hull, {x0, -b0, z0}, {x0, -b0, 0.0f}, {x1, -b1, 0.0f}, {x1, -b1, z1});
            mb.quad(hull, {x0, -b0 * 0.5f, -0.18f}, {x1, -b1 * 0.5f, -0.18f}, {x1, b1 * 0.5f, -0.18f}, {x0, b0 * 0.5f, -0.18f});
            mb.quad(hull, {x0, b0 * 0.5f, -0.18f}, {x1, b1 * 0.5f, -0.18f}, {x1, b1, 0.0f}, {x0, b0, 0.0f});
            mb.quad(hull, {x0, -b0, 0.0f}, {x1, -b1, 0.0f}, {x1, -b1 * 0.5f, -0.18f}, {x0, -b0 * 0.5f, -0.18f});
            // deck
            mb.quad(MBoatDeck, {x0, -b0, z0}, {x1, -b1, z1}, {x1, b1, z1}, {x0, b0, z0});
        }
        mb.quad(hull, {-hl, -half_beam(0.0f), 0.0f}, {-hl, half_beam(0.0f), 0.0f}, {-hl, half_beam(0.0f), rise(0.0f) - 0.55f}, {-hl, -half_beam(0.0f), rise(0.0f) - 0.55f});
        if (sail) {
            mb.cylinder(MPaintSteel, {-0.1f * length, 0, 0.1f}, 0.04f, length * 0.9f, 8, false);
            mb.quad(MSail, {-0.1f * length + 0.01f, 0, 0.5f}, {-0.1f * length + 0.01f, 0, length * 0.85f}, {-0.1f * length + length * 0.38f, 0, 0.7f}, {-0.1f * length + length * 0.38f, 0, 0.5f});
            mb.quad(MSail, {-0.1f * length - 0.01f, 0, length * 0.85f}, {-0.1f * length - 0.01f, 0, 0.5f}, {-0.1f * length + length * 0.38f - 0.02f, 0, 0.5f}, {-0.1f * length + length * 0.38f - 0.02f, 0, 0.7f});
        } else {
            // cabin and windscreen
            mb.box(MBoatHullWhite, {-0.15f * length, -hb * 0.62f, 0.12f}, {0.2f * length, hb * 0.62f, 0.95f}, 63);
            mb.quad(MGlassDark, {0.2f * length, -hb * 0.58f, 0.95f}, {0.2f * length, hb * 0.58f, 0.95f}, {0.2f * length + 0.35f, hb * 0.58f, 0.45f}, {0.2f * length + 0.35f, -hb * 0.58f, 0.45f});
            mb.box(MPaintDarkGrey, {-hl - 0.2f, -0.12f, -0.05f}, {-hl + 0.1f, 0.12f, 0.5f}, 63);   // outboard
        }
        return upload_model(r, "boat", mb.finish(), mats.list());
    }

    // ---- sea --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    render::GpuModel build_sea(double t, Float3 eye) {
        (void)eye;
        std::vector<float> xs, ys;
        for (float x = -330.0f; x <= 330.0f;) {
            xs.push_back(x);
            x += std::fabs(x) < 170.0f ? 3.0f : 10.0f;
        }
        const float q = static_cast<float>(d.quay_y);
        ys.push_back(q);
        for (float y = q + 2.0f; y <= q + 400.0f;) {
            ys.push_back(y);
            y += y < q + 70.0f ? 3.0f : (y < q + 140.0f ? 6.0f : 18.0f);
        }
        MeshIn mi;
        mi.material = MWater;
        mi.vertices.reserve(xs.size() * ys.size());
        for (std::size_t j = 0; j < ys.size(); ++j)
            for (std::size_t i = 0; i < xs.size(); ++i) {
                const double x = static_cast<double>(xs[i]), y = static_cast<double>(ys[j]);
                const double h = sea->height(x, y, t);
                const WaterVec n = sea->normal(x, y, t);
                render::ModelVertex v;
                v.position = {xs[i], ys[j], static_cast<float>(h)};
                v.normal = {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)};
                v.uv[0] = xs[i] * 0.05f;
                v.uv[1] = ys[j] * 0.05f;
                v.tangent[0] = 1;
                v.tangent[3] = 1;
                mi.vertices.push_back(v);
            }
        const std::uint32_t w = static_cast<std::uint32_t>(xs.size());
        for (std::uint32_t j = 0; j + 1 < ys.size(); ++j)
            for (std::uint32_t i = 0; i + 1 < w; ++i) {
                const std::uint32_t a = j * w + i, b = a + 1, c = a + w + 1, e = a + w;
                mi.indices.insert(mi.indices.end(), {a, b, c, a, c, e});
            }
        // Flat water beyond the animated mesh, out to the horizon.
        MeshIn far;
        far.material = MWater;
        const float lo = -300000.0f, hi = 300000.0f, x0 = xs.front(), x1 = xs.back(), y1 = ys.back();
        const float lvl = static_cast<float>(sea->config().level);
        const auto add = [&](float ax, float ay, float bx, float by) {
            const std::uint32_t base = static_cast<std::uint32_t>(far.vertices.size());
            const float px[4] = {ax, bx, bx, ax}, py[4] = {ay, ay, by, by};
            for (int k = 0; k < 4; ++k) {
                render::ModelVertex v;
                v.position = {px[k], py[k], lvl};
                v.normal = {0, 0, 1};
                v.tangent[0] = 1;
                v.tangent[3] = 1;
                far.vertices.push_back(v);
            }
            far.indices.insert(far.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
        };
        add(lo, q, x0, hi);
        add(x1, q, hi, hi);
        add(x0, y1, x1, hi);
        return upload_model(r, "sea", {mi, far}, mats.list());
    }

    // ---- people -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    // The mannequin's mesh is one colour; clothes need regions. Each triangle goes to the region of the body part its first vertex is bound to: 0 skin, 1 shirt, 2 trousers, 3 shoes, 4 hair, 5 forearms (sleeves or
    // skin), 6 shins (trousers or skin). A person is then the same geometry with a material per region.
    void split_regions() {
        const gfx::Skin& skin = ual->skins.at(0);
        const auto map = anim::map_humanoid(skin);
        if (!map) throw std::runtime_error("UAL rig is not a humanoid");
        std::vector<int> joint_region(skin.joint_count(), 2);   // anything above no mapped bone (root, hips children): trousers
        std::vector<std::int32_t> head_joint{(*map)[anim::Bone::Head]};
        const auto region_of_bone = [&](std::int32_t j) -> int {
            using anim::Bone;
            for (std::size_t b = 0; b < anim::bone_count; ++b) {
                if ((*map).joint[b] != j) continue;
                switch (static_cast<Bone>(b)) {
                case Bone::Head: return 0;
                case Bone::Neck: return 0;
                case Bone::Spine: case Bone::Chest: case Bone::LeftShoulder: case Bone::RightShoulder: case Bone::LeftUpperArm: case Bone::RightUpperArm: return 1;
                case Bone::LeftLowerArm: case Bone::RightLowerArm: return 5;
                case Bone::LeftHand: case Bone::RightHand: return 0;
                case Bone::Hips: case Bone::LeftUpperLeg: case Bone::RightUpperLeg: return 2;
                case Bone::LeftLowerLeg: case Bone::RightLowerLeg: return 6;
                case Bone::LeftFoot: case Bone::RightFoot: case Bone::LeftToes: case Bone::RightToes: return 3;
                default: return 2;
                }
            }
            return -1;
        };
        for (std::size_t j = 0; j < skin.joint_count(); ++j) {
            std::int32_t k = static_cast<std::int32_t>(j);
            int reg = -1;
            while (k >= 0 && reg < 0) {
                reg = region_of_bone(k);
                k = skin.parent[static_cast<std::size_t>(k)];
            }
            joint_region[j] = reg >= 0 ? reg : 2;
        }
        std::vector<gfx::Material> placeholder(7);
        for (std::size_t i = 0; i < placeholder.size(); ++i) placeholder[i].name = "region" + std::to_string(i);
        ual->materials = placeholder;
        for (gfx::Mesh& mesh : ual->meshes) {
            std::vector<gfx::Primitive> out;
            for (gfx::Primitive& p : mesh.primitives) {
                if (p.joints.empty()) { p.material = 0; out.push_back(std::move(p)); continue; }
                const std::uint32_t n = p.vertex_count();
                std::vector<int> vreg(n);
                float head_top = -1e30f;
                for (std::uint32_t v = 0; v < n; ++v) {
                    int best = 0;
                    for (int i = 1; i < 4; ++i)
                        if (p.weights[v * 4 + static_cast<std::uint32_t>(i)] > p.weights[v * 4 + static_cast<std::uint32_t>(best)]) best = i;
                    vreg[v] = joint_region[p.joints[v * 4 + static_cast<std::uint32_t>(best)]];
                    if (p.joints[v * 4 + static_cast<std::uint32_t>(best)] == static_cast<std::uint16_t>(head_joint[0])) head_top = std::max(head_top, p.positions[v * 3 + 2]);
                }
                for (std::uint32_t v = 0; v < n; ++v)
                    if (vreg[v] == 0 && p.joints[v * 4] == static_cast<std::uint16_t>(head_joint[0]) && p.positions[v * 3 + 2] > head_top - 0.11f) vreg[v] = 4;
                std::array<std::vector<std::uint32_t>, 7> idx;
                for (std::size_t t = 0; t + 2 < p.indices.size(); t += 3) {
                    int count[7] = {};
                    for (int k = 0; k < 3; ++k) ++count[vreg[p.indices[t + static_cast<std::size_t>(k)]]];
                    int reg = vreg[p.indices[t]];
                    for (int rg = 0; rg < 7; ++rg)
                        if (count[rg] >= 2) reg = rg;
                    idx[static_cast<std::size_t>(reg)].insert(idx[static_cast<std::size_t>(reg)].end(), {p.indices[t], p.indices[t + 1], p.indices[t + 2]});
                }
                for (int rg = 0; rg < 7; ++rg) {
                    if (idx[static_cast<std::size_t>(rg)].empty()) continue;
                    gfx::Primitive q = p;
                    q.indices = std::move(idx[static_cast<std::size_t>(rg)]);
                    q.material = rg;
                    out.push_back(std::move(q));
                }
            }
            mesh.primitives = std::move(out);
        }
    }

    // A person's own copy of the model with their clothes: a material per region.
    std::unique_ptr<render::GpuModel> dress(std::uint32_t id) const {
        auto m = std::make_unique<render::GpuModel>(person);
        const auto pick = [&](std::uint32_t salt, std::uint32_t n) { return static_cast<std::uint32_t>(hashf(id, salt) * static_cast<float>(n)) % n; };
        const Mat skin = static_cast<Mat>(static_cast<std::uint32_t>(MSkin) + pick(1, 3));
        const Mat shirt = static_cast<Mat>(static_cast<std::uint32_t>(MShirt0) + pick(2, 8));
        const Mat trousers = static_cast<Mat>(static_cast<std::uint32_t>(MTrouser0) + pick(3, 4));
        const Mat hair = static_cast<Mat>(static_cast<std::uint32_t>(MHair0) + pick(4, 3));
        const bool long_sleeves = pick(5, 4) != 0, shorts = pick(6, 5) == 0;
        const Mat list[7] = {skin, shirt, trousers, MShoe, hair, long_sleeves ? shirt : skin, shorts ? skin : trousers};
        m->materials.assign(7, mats.list()[MSkin]);
        for (std::size_t k = 0; k < 7; ++k) m->materials[k] = mats.list()[list[k]];
        return m;
    }

    void stage_people() {
        gfx::GltfLoadResult loaded = gfx::load_gltf_file(anim_root / "ual" / "AnimationLibrary_Godot_Standard.glb");
        if (!loaded.ok()) throw std::runtime_error("UAL: " + loaded.error);
        ual = std::make_unique<gfx::GltfAsset>(std::move(*loaded.asset));
        split_regions();
        gfx::StageSettings ss;
        gfx::StageReport rep;
        person = gfx::stage_model(r, *ual, "UAL", ss, &rep);
        for (const std::string& w : rep.warnings) warnings.push_back("UAL: " + w);
        if (person.skin_joint_count == 0) throw std::runtime_error("UAL has no skin");
        const gfx::Skin& skin = ual->skins.at(0);
        const auto map = anim::map_humanoid(skin);
        if (!map) throw std::runtime_error("UAL rig is not a humanoid");
        const auto find = [&](const char* name) -> const gfx::AnimationClip* {
            for (const gfx::AnimationClip& c : ual->animations)
                if (c.name == name) return &c;
            return nullptr;
        };
        set_idle = std::make_unique<anim::LocomotionSet>(skin, *map);
        set_talk = std::make_unique<anim::LocomotionSet>(skin, *map);
        for (anim::LocomotionSet* s : {set_idle.get(), set_talk.get()}) {
            std::string why;
            for (const char* cyc : {"Walk_Loop", "Jog_Fwd_Loop", "Sprint_Loop"}) {
                if (const gfx::AnimationClip* c = find(cyc)) {
                    if (!s->add_cycle(*c, &why)) warnings.push_back(std::string("locomotion cycle ") + cyc + ": " + why);
                }
            }
        }
        if (const gfx::AnimationClip* c = find("Idle_Loop")) set_idle->set_idle(*c);
        if (const gfx::AnimationClip* c = find("Idle_Talking_Loop")) set_talk->set_idle(*c);
        if (!set_idle->usable()) throw std::runtime_error("no locomotion cycle could be built from the UAL");
        for (int i = 0; i < 48; ++i) palettes.push_back(r.model_renderer().create_skin_palette(person.skin_joint_count, "person"));
        std::printf("people: UAL skin %u joints, %zu cycles (ground speeds:", person.skin_joint_count, set_idle->cycles().size());
        for (const auto& cy : set_idle->cycles()) std::printf(" %.2f", static_cast<double>(cy.info.ground_speed));
        std::printf(")\n");
    }

    void init() {
        sea = std::make_unique<WaterSurface>(harbor_sea_config(d.sea_level));
        for (int i = 0; i < kPaintCount; ++i) cars.push_back(fixtures ? stage_fixture_car(i) : stage_car(kPaints[i]));
        build_trucks();
        boat_models[0] = build_boat(4.6f, 1.8f, MBoatHullWhite, false);
        boat_models[1] = build_boat(5.4f, 2.0f, MBoatHullBlue, false);
        boat_models[2] = build_boat(6.6f, 2.3f, MBoatHullWhite, true);
        for (int i = 0; i < 4; ++i) {
            MeshBuilder mb;
            mb.sphere(static_cast<Mat>(static_cast<int>(MSmoke0) + i), {0, 0, 0}, 1.0f, 5, 8);
            puffs[i] = upload_model(r, "puff", mb.finish(), mats.list());
        }
        for (int on = 0; on < 2; ++on) {
            MeshBuilder mb;
            static_assert(kHazardLampsPerModel == 1, "the hazard model holds one lamp; it is drawn once per corner");
            mb.box(on ? MSignalAmber : MSignalOff, {-0.03f, -0.03f, -0.03f}, {0.03f, 0.03f, 0.03f}, 63);   // one lamp at the origin; hazard_lamp_offsets() places it
            hazard[on] = upload_model(r, "hazard", mb.finish(), mats.list());
        }
        {
            MeshBuilder mb;
            mb.sphere(MSpark, {0, 0, 0}, 0.045f, 3, 5);
            spark = upload_model(r, "spark", mb.finish(), mats.list());
        }
        if (!fixtures) stage_people();
    }

    void collect_fx(std::vector<render::ModelDraw>& out, double time) {
        std::erase_if(fx, [&](const Fx& f) { return time - f.t0 > 3.0; });
        for (const Fx& f : fx) {
            const float age = static_cast<float>(time - f.t0);
            const int puffs_n = std::clamp(static_cast<int>(f.energy / 5000.0f), 3, 11);
            for (int k = 0; k < puffs_n; ++k) {
                const float a = hashf(f.seed, static_cast<std::uint32_t>(k)) * 6.2832f, sp = 0.8f + 2.4f * hashf(f.seed, static_cast<std::uint32_t>(k + 40));
                const float life = 2.4f;
                const float u = std::clamp(age / life, 0.0f, 0.999f);
                const float px = static_cast<float>(f.x) + std::cos(a) * sp * age * (1.0f - 0.35f * u), py = static_cast<float>(f.y) + std::sin(a) * sp * age * (1.0f - 0.35f * u);
                const float pz = static_cast<float>(f.z) + 0.25f + age * (0.9f + hashf(f.seed, static_cast<std::uint32_t>(k + 80)) * 1.0f);
                const int lv = std::clamp(3 - static_cast<int>(u * 4.0f), 0, 3);
                out.push_back({&puffs[static_cast<std::size_t>(lv)], translation({px, py, pz}) * scaling(0.35f + u * 1.5f)});
            }
            if (age < 0.8f) {
                const int n = std::clamp(static_cast<int>(f.energy / 3000.0f), 6, 24);
                for (int k = 0; k < n; ++k) {
                    const float a = hashf(f.seed, static_cast<std::uint32_t>(k + 200)) * 6.2832f, sp = 2.0f + 6.0f * hashf(f.seed, static_cast<std::uint32_t>(k + 240)), up = 1.5f + 4.0f * hashf(f.seed, static_cast<std::uint32_t>(k + 280));
                    const float px = static_cast<float>(f.x) + std::cos(a) * sp * age, py = static_cast<float>(f.y) + std::sin(a) * sp * age;
                    const float pz = std::max(static_cast<float>(f.z) + up * age - 4.9f * age * age, 0.16f);
                    out.push_back({&spark, translation({px, py, pz})});
                }
            }
        }
    }

    // ---- drawing ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------
    // A body shortened about its far end by front/rear damage and pitched towards the harder hit end: what a visible crumple zone reads as without a deformable mesh.
    static Mat4 crumple(const Mat4& m, const VehicleView& v, float x_lo, float x_hi) {
        const float f = std::clamp(v.zones[0], 0.0f, 1.0f), r = std::clamp(v.zones[1], 0.0f, 1.0f);
        if (f < 0.02f && r < 0.02f) return m;
        const float shorten = 1.0f - 0.16f * f - 0.12f * r;
        const float pivot = f >= r ? x_lo : x_hi;
        const float pitch = 0.07f * f - 0.05f * r;
        return m * translation({pivot, 0, 0}) * rot_y(pitch) * scaling(shorten, 1.0f, 1.0f - 0.05f * std::max(f, r)) * translation({-pivot, 0, 0});
    }

    void draw_car(std::vector<render::ModelDraw>& out, const CarRig& rig, const VehicleView& v) {
        const VehicleConfig cfg = vehicle_config_for(v.kind == VehicleKind::Hatchback || v.kind == VehicleKind::Taxi ? VehicleKind::Sedan : v.kind);
        const Mat4 body_world = translation({static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}) * from_quat(v.q[0], v.q[1], v.q[2], v.q[3]);
        float wz_model = 0, wz_phys = 0;
        for (std::size_t w = 0; w < 4; ++w) {
            wz_model += rig.wc[w].z * 0.25f;
            wz_phys += (cfg.wheels[w].mount.z - cfg.suspension_travel + v.wheel_compression[w]) * 0.25f;
        }
        const float cx = (rig.lo.x + rig.hi.x) * 0.5f, cy = (rig.lo.y + rig.hi.y) * 0.5f;
        const Mat4 car_m = body_world * translation({-cx, -cy, -(wz_model - wz_phys)});
        out.push_back({&rig.body, crumple(car_m, v, rig.lo.x, rig.hi.x)});
        for (std::size_t w = 0; w < 4; ++w) {
            const VehicleWheelConfig& wc = cfg.wheels[w];
            const Float3 centre{wc.mount.x, wc.mount.y, wc.mount.z - cfg.suspension_travel + v.wheel_compression[w]};
            const float steer = w < 2 ? v.wheel_steer[w] : 0.0f;
            const Mat4 hub = body_world * translation(centre) * rot_z(steer);
            out.push_back({&rig.wheel[w], hub * rot_y(v.wheel_spin[w])});
            if (!rig.hub[w].placements.empty()) out.push_back({&rig.hub[w], hub});
        }
    }

    void draw_truck(std::vector<render::ModelDraw>& out, const Truck& t, const VehicleView& v, double time) {
        const VehicleConfig cfg = vehicle_config_for(v.kind);
        const Mat4 body_world = translation({static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}) * from_quat(v.q[0], v.q[1], v.q[2], v.q[3]);
        out.push_back({&t.body, crumple(body_world, v, -2.4f, 2.4f)});
        for (std::size_t w = 0; w < 4; ++w) {
            const VehicleWheelConfig& wc = cfg.wheels[w];
            const Float3 centre{wc.mount.x, wc.mount.y, wc.mount.z - cfg.suspension_travel + v.wheel_compression[w]};
            const float steer = w < 2 ? v.wheel_steer[w] : 0.0f;
            out.push_back({&t.wheel, body_world * translation(centre) * rot_z(steer) * rot_y(v.wheel_spin[w])});
        }
        if (t.bar_present) {
            const int state = v.siren ? (static_cast<int>(std::floor(time * 5.0)) % 2 == 0 ? 1 : 2) : 0;
            out.push_back({&t.bar[state], body_world * translation(t.bar_at)});
        }
    }

    void collect_vehicles(std::vector<render::ModelDraw>& out, const World& world, double time, Float3 eye, float far) {
        for (const VehicleView& v : world.vehicles()) {
            if (std::hypot(static_cast<float>(v.x) - eye.x, static_cast<float>(v.y) - eye.y) > far) continue;
            switch (v.kind) {
            case VehicleKind::Sedan:
            case VehicleKind::Taxi:
            case VehicleKind::Hatchback: {
                const int paint = v.kind == VehicleKind::Taxi ? 4 : v.palette % kPaintCount;
                draw_car(out, cars[static_cast<std::size_t>(paint)], v);
                break;
            }
            case VehicleKind::Van: draw_truck(out, vans[v.palette % 4], v, time); break;
            case VehicleKind::Ambulance: draw_truck(out, ambulance, v, time); break;
            case VehicleKind::FireEngine: draw_truck(out, engine, v, time); break;
            }
            if (v.disabled) {
                const VehicleConfig cfgv = vehicle_config_for(v.kind);
                const bool on = static_cast<int>(std::floor(time * 1.6)) % 2 == 0;
                const Mat4 body_world = translation({static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)}) * from_quat(v.q[0], v.q[1], v.q[2], v.q[3]);
                // One hazard lamp at each corner of the chassis box (hazard_lamp_offsets, traffic_driver.hpp).
                for (const auto& o : hazard_lamp_offsets(cfgv)) out.push_back({&hazard[on ? 1 : 0], body_world * translation({o[0], o[1], o[2]})});
                // Smoke from under the bonnet: puffs rising and spreading over a few seconds.
                for (int k = 0; k < 14; ++k) {
                    const float age = std::fmod(static_cast<float>(time) * 0.30f + static_cast<float>(k) / 14.0f, 1.0f);
                    const float lv = std::clamp(3.0f - age * 3.2f, 0.0f, 3.0f);
                    const float c = std::cos(static_cast<float>(v.yaw)), s = std::sin(static_cast<float>(v.yaw));
                    const float drift = age * 1.8f;
                    const Float3 at{static_cast<float>(v.x) + c * 1.6f + drift * 0.6f, static_cast<float>(v.y) + s * 1.6f + drift * 0.3f, static_cast<float>(v.z) + 0.3f + age * 3.6f};
                    out.push_back({&puffs[static_cast<std::size_t>(lv)], translation(at) * scaling(0.35f + age * 1.6f) * rot_z(static_cast<float>(k))});
                }
            }
        }
    }

    void collect_people(std::vector<render::ModelDraw>& out, Float3 eye, float far) {
        if (fixtures) return;
        // Nearest first, as many as there are palettes.
        std::vector<const PedView*> near;
        for (const PedView& p : last_peds) {
            if (!p.visible) continue;
            if (std::hypot(static_cast<float>(p.x) - eye.x, static_cast<float>(p.y) - eye.y) > far) continue;
            near.push_back(&p);
        }
        std::sort(near.begin(), near.end(), [&](const PedView* a, const PedView* b) { return std::hypot(a->x - static_cast<double>(eye.x), a->y - static_cast<double>(eye.y)) < std::hypot(b->x - static_cast<double>(eye.x), b->y - static_cast<double>(eye.y)); });
        const gfx::Skin& skin = ual->skins.at(0);
        for (std::size_t i = 0; i < near.size() && i < palettes.size(); ++i) {
            const PedView& p = *near[i];
            const auto it = anims.find(p.id);
            if (it == anims.end()) continue;
            palettes[i].update(gfx::skin_matrices(skin, it->second.ctl->pose()));
            auto& outfit = outfits[p.id];
            if (!outfit) outfit = dress(static_cast<std::uint32_t>(p.id));
            out.push_back({outfit.get(), translation({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)}) * rot_z(static_cast<float>(p.yaw)), &palettes[i]});
        }
    }
};

Actors::Actors(render::SceneRenderer& renderer, const Materials& materials, const District& district, const fs::path& cache, const fs::path& anim_root, std::vector<std::string>& warnings, bool fixtures) : impl_(std::make_unique<Impl>(renderer, materials, district, cache, anim_root, warnings)) {
    impl_->fixtures = fixtures;
    impl_->init();
}
Actors::~Actors() = default;

void Actors::animate(const std::vector<PedView>& peds, double dt) {
    Impl& i = *impl_;
    if (i.fixtures) return;
    i.last_peds = peds;
    for (const PedView& p : peds) {
        auto it = i.anims.find(p.id);
        if (it == i.anims.end()) {
            Impl::Anim a;
            a.talk = hashf(static_cast<std::uint32_t>(p.id)) < 0.5f;
            a.ctl = std::make_unique<anim::LocomotionController>(a.talk ? *i.set_talk : *i.set_idle, anim::LocomotionConfig{});
            it = i.anims.emplace(p.id, std::move(a)).first;
        }
        if (!p.visible) continue;
        anim::LocomotionInput in;
        in.speed = static_cast<float>(p.speed);
        it->second.ctl->update(static_cast<float>(dt), in);
    }
}

void Actors::collect(std::vector<render::ModelDraw>& out, const World& world, double t, Float3 eye, float far_distance) {
    Impl& i = *impl_;
    i.collect_vehicles(out, world, t, eye, far_distance);
    i.collect_people(out, eye, far_distance);
    i.collect_fx(out, t);
    // Boats at their berths, riding the same surface the sea mesh shows.
    int k = 0;
    for (const BoatSpawn& b : i.d.boats) {
        const double h = i.sea->height(b.x, b.y, t);
        const WaterVec n = i.sea->normal(b.x, b.y, t);
        const float roll = std::asin(std::clamp(static_cast<float>(-n.y), -0.5f, 0.5f)) * 0.6f, pitch = std::asin(std::clamp(static_cast<float>(n.x), -0.5f, 0.5f)) * 0.6f;
        const render::GpuModel& m = i.boat_models[static_cast<std::size_t>(k++ % 3)];
        out.push_back({&m, translation({static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(h) - 0.2f}) * rot_z(static_cast<float>(b.yaw)) * rot_x(roll) * rot_y(pitch)});
    }
    // The sea itself: a fresh mesh of this instant (the previous frame's buffers are released once the GPU is done with them).
    i.sea_model = std::make_unique<render::GpuModel>(i.build_sea(t, eye));
    out.push_back({i.sea_model.get(), Mat4::identity()});
}

void Actors::collect_parked(std::vector<render::ModelDraw>& out, Float3 eye, float far_distance) const {
    const Impl& i = *impl_;
    for (const StreetProp& p : i.d.props) {
        if (p.type != "parked_car") continue;
        if (std::hypot(static_cast<float>(p.x) - eye.x, static_cast<float>(p.y) - eye.y) > far_distance) continue;
        const CarRig& rig = i.cars[static_cast<std::size_t>(p.scale) % static_cast<std::size_t>(kPaintCount)];
        const VehicleConfig cfg = vehicle_config_for(VehicleKind::Sedan);
        float wz_model = 0;
        for (std::size_t w = 0; w < 4; ++w) wz_model += rig.wc[w].z * 0.25f;
        const float cx = (rig.lo.x + rig.hi.x) * 0.5f, cy = (rig.lo.y + rig.hi.y) * 0.5f;
        // Wheels at ride height rest on the road: the wheel centre is one wheel radius above it.
        const Mat4 car_m = translation({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) + cfg.wheel_radius}) * rot_z(static_cast<float>(p.yaw)) * translation({-cx, -cy, -wz_model});
        out.push_back({&rig.body, car_m});
        for (std::size_t w = 0; w < 4; ++w) {
            const VehicleWheelConfig& wc = cfg.wheels[w];
            const Mat4 hub = translation({static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z) + cfg.wheel_radius}) * rot_z(static_cast<float>(p.yaw)) * translation({wc.mount.x - (cx - cx), wc.mount.y, 0.0f});
            out.push_back({&rig.wheel[w], hub});
            if (!rig.hub[w].placements.empty()) out.push_back({&rig.hub[w], hub});
        }
    }
}

void Actors::impact(double t, double x, double y, double z, float energy) { impl_->fx.push_back({t, x, y, z, energy, static_cast<std::uint32_t>(t * 977.0) + static_cast<std::uint32_t>(x * 13.0)}); }

double Actors::sea_height(double x, double y, double t) const { return impl_->sea->height(x, y, t); }

} // namespace saige::alive::rend
