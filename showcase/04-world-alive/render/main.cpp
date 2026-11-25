// saige_alive: Showcase 04 "A World Alive". The real SAIGE renderer draws the living district (alive_world): the static city, the signals, the residents, the traffic and whatever the simulation is doing.
// Every pixel is read back from the renderer's output target; report.json records the device, the resolution, the cadence and the build.
#include "actors.hpp"
#include "city.hpp"
#include "gfx.hpp"
#include "world.hpp"

#include "saige/render/atmosphere.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace saige;
using namespace saige::alive;
using namespace saige::alive::rend;
namespace fs = std::filesystem;

namespace {

struct Options {
    fs::path assets = "/tmp/saige-cache";
    fs::path anim = "/tmp/anim_assets";
    double sim_seconds = 0.0;
    bool accidents = true;
    int residents = 60, vehicles = 14;
    fs::path out = "alive-out";
    std::uint32_t width = 1280, height = 720, ssaa = 1;
    std::uint32_t shadow_map = 2048;
    float shadow_distance = 220.0f;
    float far_distance = 260.0f;
    double hour = 17.5;
    float exposure_bias = 0.0f;
    bool validate = false;
    std::vector<std::array<double, 7>> stills;
};

[[noreturn]] void usage(const std::string& what) {
    std::cerr << "saige_alive: " << what << "\nusage: saige_alive [--assets DIR] [--out DIR] [--width N] [--height N] [--ssaa N] [--hour H] [--still x,y,z,lx,ly,lz,fov]... [--shadow-map N] [--exposure EV] [--validate]\n";
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage("missing value for " + a);
            return argv[++i];
        };
        try {
            if (a == "--assets") o.assets = value();
            else if (a == "--anim") o.anim = value();
            else if (a == "--sim-seconds") o.sim_seconds = std::stod(value());
            else if (a == "--no-accidents") o.accidents = false;
            else if (a == "--residents") o.residents = std::stoi(value());
            else if (a == "--vehicles") o.vehicles = std::stoi(value());
            else if (a == "--out") o.out = value();
            else if (a == "--width") o.width = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--height") o.height = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--ssaa") o.ssaa = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--hour") o.hour = std::stod(value());
            else if (a == "--shadow-map") o.shadow_map = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--shadow-distance") o.shadow_distance = std::stof(value());
            else if (a == "--exposure") o.exposure_bias = std::stof(value());
            else if (a == "--validate") o.validate = true;
            else if (a == "--still") {
                std::array<double, 7> c{};
                std::stringstream ss(value());
                std::string tok;
                std::size_t k = 0;
                while (std::getline(ss, tok, ',') && k < 7) c[k++] = std::stod(tok);
                if (k != 7) usage("--still takes x,y,z,lx,ly,lz,fov");
                o.stills.push_back(c);
            } else usage("unknown option " + a);
        } catch (const std::logic_error&) {
            usage("bad value for " + a);
        }
    }
    return o;
}

double smooth(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
double emissive_gain_at(double elevation_deg) {
    const double s = smooth((7.0 - elevation_deg) / 13.0);
    return s * std::sqrt(s);
}
// Exposure in stops as the light falls: the eye adapts.
double exposure_for(double elevation_deg) {
    struct K { double e, ev; };
    static const K keys[] = {{-18, 2.1}, {-8, 1.5}, {-3, 0.6}, {3, -0.1}, {10, -0.45}, {25, -0.55}, {90, -0.55}};
    if (elevation_deg <= keys[0].e) return keys[0].ev;
    for (std::size_t i = 0; i + 1 < sizeof(keys) / sizeof(keys[0]); ++i)
        if (elevation_deg <= keys[i + 1].e) return keys[i].ev + (keys[i + 1].ev - keys[i].ev) * smooth((elevation_deg - keys[i].e) / (keys[i + 1].e - keys[i].e));
    return keys[6].ev;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options opt = parse(argc, argv);
        std::string why;
        if (!rhi::vulkan_device_available(&why)) {
            std::printf("no Vulkan 1.3 device (%s); skipping\n", why.c_str());
            return 77;
        }
        rhi::DeviceDesc dd;
        dd.application_name = "SAIGE A World Alive";
        dd.enable_validation = opt.validate;
        std::unique_ptr<rhi::Device> device = rhi::create_vulkan_device(dd);
        const rhi::DeviceInfo info = device->info();
        std::printf("device: %s (%s), software=%d\n", info.device_name.c_str(), info.driver_name.c_str(), info.software_device ? 1 : 0);

        render::RendererSettings rs;
        rs.camera_mode = render::CameraMode::Fixed;
        rs.hdr = true;
        rs.model_shading = render::ModelShading::MetallicRoughness;
        rs.alpha_blending = true;
        rs.skinning = true;
        rs.dynamic_lighting = true;
        rs.ground_half_size = 0.001f;
        rs.sky = {0.30f, 0.46f, 0.78f, 1.0f};
        rs.fog_density = 0.00035f;
        rs.lighting.sun_direction = {0.5f, 0.1f, 0.6f};
        rs.lighting.sun_color = {2.4f, 2.0f, 1.65f};
        rs.lighting.ambient_ground = {0.17f, 0.15f, 0.125f};
        rs.tone_map.exposure_ev = -0.5f;
        rs.atmosphere.enabled = true;
        rs.atmosphere.exposure = 1.0f;
        rs.atmosphere.sun_disc_intensity = 3500.0f;
        rs.atmosphere.moon_intensity = 14.0f;
        rs.atmosphere.star_intensity = 60.0f;
        rs.shadows.enabled = true;
        rs.shadows.cascades = 3;
        rs.shadows.map_size = opt.shadow_map;
        rs.shadows.distance = opt.shadow_distance;
        rs.shadows.filter_radius = 1;
        rs.bloom.enabled = true;
        rs.ambient_occlusion.enabled = true;
        rs.fxaa.enabled = true;

        Offscreen target(*device, opt.width * opt.ssaa, opt.height * opt.ssaa);
        render::SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, rs);
        std::vector<std::string> warnings;
        const auto t0 = std::chrono::steady_clock::now();
        Materials materials(renderer, opt.assets / "polyhaven", warnings);
        WorldConfig wc;
        wc.residents = opt.residents;
        wc.vehicles = opt.vehicles;
        wc.accidents = opt.accidents;
        wc.start_minute = opt.hour * 60.0;
        alive::World world(wc);
        const District& district = world.district();
        CityScene city(renderer, materials, district, opt.assets / "polyhaven", warnings);
        Actors actors(renderer, materials, district, opt.assets, opt.anim, warnings);
        std::printf("city: %zu draws, %zu triangles, built in %.1f s\n", city.draw_count(), city.triangle_count(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        for (const std::string& w : warnings) std::printf("warning: %s\n", w.c_str());
        const auto s0 = std::chrono::steady_clock::now();
        for (double t = 0.0; t < opt.sim_seconds; t += 1.0 / 30.0) {
            world.step(1.0 / 30.0);
            if (std::fmod(t, 1.0) < 1.0 / 30.0) actors.animate(world.pedestrians(), 1.0);   // a coarse pre-roll of the poses
        }
        actors.animate(world.pedestrians(), 1.0 / 30.0);
        if (opt.sim_seconds > 0.0) std::printf("simulated %.0f s in %.1f s\n", opt.sim_seconds, std::chrono::duration<double>(std::chrono::steady_clock::now() - s0).count());

        int index = 0;
        for (const auto& c : opt.stills) {
            const render::Float3 eye{static_cast<float>(c[0]), static_cast<float>(c[1]), static_cast<float>(c[2])};
            const render::Float3 look{static_cast<float>(c[3]), static_cast<float>(c[4]), static_cast<float>(c[5])};
            const render::Camera camera = render::look_at_camera(eye, look, {0, 0, 1}, static_cast<float>(c[6] * kPi / 180.0), 0.15f);
            std::vector<render::ModelDraw> draws;
            city.collect(draws, eye, opt.far_distance);
            actors.collect_parked(draws, eye, opt.far_distance);
            actors.collect(draws, world, world.time_seconds(), eye, opt.far_distance);
            city.collect_signals(draws, [&](std::uint32_t inter, int leg) {
                const traffic::IntersectionController* ic = district.traffic.find_intersection(inter);
                const Intersection& I = district.intersections[inter - 1];
                if (ic == nullptr) return 0;
                const traffic::SignalColor col = ic->signal_for_lane(I.inbound[static_cast<std::size_t>(leg)]);
                return col == traffic::SignalColor::Green ? 3 : col == traffic::SignalColor::Yellow ? 2 : 1;
            }, [&](std::uint32_t cw) { return world.walk_signal(cw); });
            const render::SolarPosition sun = render::solar_position(190, opt.hour, 37.0, kPi * 0.5);
            const double elev_deg = sun.elevation * 180.0 / kPi;
            render::SceneFrame frame;
            frame.tick = static_cast<std::uint64_t>(index);
            frame.camera = &camera;
            frame.models = draws;
            render::SceneLighting light = render::time_of_day_lighting(rs.atmosphere, sun.to_sun, rs.lighting, 0.2f);
            light.emissive_gain = static_cast<float>(emissive_gain_at(elev_deg));
            frame.lighting = &light;
            frame.exposure_ev = static_cast<float>(exposure_for(elev_deg)) + opt.exposure_bias;
            render::FrameStats stats;
            const auto r0 = std::chrono::steady_clock::now();
            Rgba img = downsample(target.render(renderer, frame, &stats), opt.ssaa);
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - r0).count();
            char name[64];
            std::snprintf(name, sizeof name, "still_%02d.ppm", index);
            write_ppm(opt.out / name, img);
            std::printf("still %d: sun elevation %.1f deg, %.2f s, %zu draws\n", index, elev_deg, secs, draws.size());
            ++index;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "saige_alive: " << e.what() << "\n";
        return 1;
    }
}
