// saige_alive: Showcase 04 "A World Alive". The real SAIGE renderer draws the living district (alive_world): the static city, the signals, the residents, the traffic and whatever the simulation is doing.
// Every pixel is read back from the renderer's output target; report.json records the device, the resolution, the cadence and the build.
#include "actors.hpp"
#include "city.hpp"
#include "director.hpp"
#include "vegetation_shape.hpp"
#include "district_validate.hpp"
#include "gfx.hpp"
#include "scout.hpp"
#include "world.hpp"

#include "saige/render/atmosphere.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <algorithm>
#include <chrono>
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
// MSVC spells the pipe calls with a leading underscore; the binary mode keeps the frame bytes untouched by newline translation.
#define SAIGE_POPEN(cmd) _popen(cmd, "wb")
#define SAIGE_PCLOSE(f) _pclose(f)
#else
#define SAIGE_POPEN(cmd) popen(cmd, "w")
#define SAIGE_PCLOSE(f) pclose(f)
#endif

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
    bool film = false;
    std::uint32_t seed = 8;
    double film_seconds = 180.0, preroll = 25.0, fps = 30.0, time_scale = 47.0;
    int start_frame = 0, end_frame = -1;
    fs::path video;
    fs::path scout_file;
    bool scout_legacy = false;   // read a headerless (pre-format-1) scout file, such as the one the delivered film was shot from
    fs::path audio_log;
    bool no_render = false;
    std::vector<int> only_frames;
    int crf = 14;
    fs::path out = "alive-out";
    std::uint32_t width = 1280, height = 720, ssaa = 1;
    std::uint32_t shadow_map = 2048;
    float shadow_distance = 220.0f;
    float far_distance = 260.0f;
    double hour = 17.5;
    double hour_step = 0.0;   // stills: the hour advances by this much per still (a sun sequence in one run)
    float exposure_bias = 0.45f;
    bool validate = false;
    // Diagnostics: switch one renderer pass off to attribute an artefact to it (shadows, ao, bloom, fxaa).
    bool disable_shadows = false, disable_ao = false, disable_bloom = false, disable_fxaa = false;
    bool disable_stabilize = false;   // diagnostics: cascades not snapped to texels (what the shadow stability test measures)
    bool check_camera = false;   // film mode: count the frames whose eye is inside tree foliage or whose first 6 m of view pass through leaves (exit status 3 if any)
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
            else if (a == "--film") o.film = true;
            else if (a == "--seed") o.seed = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--film-seconds") o.film_seconds = std::stod(value());
            else if (a == "--preroll") o.preroll = std::stod(value());
            else if (a == "--fps") o.fps = std::stod(value());
            else if (a == "--time-scale") o.time_scale = std::stod(value());
            else if (a == "--start-frame") o.start_frame = std::stoi(value());
            else if (a == "--end-frame") o.end_frame = std::stoi(value());
            else if (a == "--video") o.video = value();
            else if (a == "--scout-file") o.scout_file = value();
            else if (a == "--scout-legacy") o.scout_legacy = true;
            else if (a == "--audio-log") o.audio_log = value();
            else if (a == "--no-render") o.no_render = true;
            else if (a == "--frames") {
                std::stringstream ss(value());
                std::string tok;
                while (std::getline(ss, tok, ',')) o.only_frames.push_back(std::stoi(tok));
            }
            else if (a == "--crf") o.crf = std::stoi(value());
            else if (a == "--sim-seconds") o.sim_seconds = std::stod(value());
            else if (a == "--no-accidents") o.accidents = false;
            else if (a == "--residents") o.residents = std::stoi(value());
            else if (a == "--vehicles") o.vehicles = std::stoi(value());
            else if (a == "--out") o.out = value();
            else if (a == "--width") o.width = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--height") o.height = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--ssaa") o.ssaa = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--hour") o.hour = std::stod(value());
            else if (a == "--hour-step") o.hour_step = std::stod(value());
            else if (a == "--shadow-map") o.shadow_map = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--shadow-distance") o.shadow_distance = std::stof(value());
            else if (a == "--exposure") o.exposure_bias = std::stof(value());
            else if (a == "--validate") o.validate = true;
            else if (a == "--check-camera") o.check_camera = true;
            else if (a == "--disable") {
                std::stringstream ss(value());
                std::string tok;
                while (std::getline(ss, tok, ',')) {
                    if (tok == "shadows") o.disable_shadows = true;
                    else if (tok == "ao") o.disable_ao = true;
                    else if (tok == "bloom") o.disable_bloom = true;
                    else if (tok == "fxaa") o.disable_fxaa = true;
                    else if (tok == "stabilize") o.disable_stabilize = true;
                    else usage("--disable takes shadows,ao,bloom,fxaa,stabilize");
                }
            }
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
        rs.fog_density = 0.00006f;
        rs.lighting.sun_direction = {0.5f, 0.1f, 0.6f};
        rs.lighting.sun_color = {2.4f, 2.0f, 1.65f};
        rs.lighting.ambient_ground = {0.17f, 0.15f, 0.125f};
        rs.tone_map.exposure_ev = -0.5f;
        rs.atmosphere.enabled = true;
        rs.atmosphere.exposure = 1.0f;
        rs.atmosphere.sun_disc_intensity = 3500.0f;
        rs.atmosphere.moon_intensity = 14.0f;
        rs.atmosphere.star_intensity = 60.0f;
        rs.shadows.enabled = !opt.disable_shadows;
        rs.shadows.cascades = 3;
        rs.shadows.map_size = opt.shadow_map;
        rs.shadows.distance = opt.shadow_distance;
        rs.shadows.filter_radius = 1;
        rs.shadows.stabilize = !opt.disable_stabilize;
        rs.bloom.enabled = !opt.disable_bloom;
        rs.ambient_occlusion.enabled = !opt.disable_ao;
        rs.fxaa.enabled = !opt.disable_fxaa;

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
        wc.seed = opt.seed;
        wc.time_scale = opt.film ? opt.time_scale : 20.0;
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

        if (opt.film) {
            ScoutReport rep;
            if (!opt.scout_file.empty() && fs::exists(opt.scout_file)) {
                // An existing file is read strictly and never silently replaced: a file that cannot be read is an error, not a reason to scout again.
                std::ifstream in(opt.scout_file, std::ios::binary);
                std::string text(static_cast<std::size_t>(fs::file_size(opt.scout_file)), '\0');
                in.read(text.data(), static_cast<std::streamsize>(text.size()));
                if (!in || static_cast<std::size_t>(in.gcount()) != text.size()) throw std::runtime_error("cannot read scout file " + opt.scout_file.string());
                const ScoutParse pr = parse_scout_report(text, rep, opt.scout_legacy);
                if (!pr.ok) throw std::runtime_error("scout file " + opt.scout_file.string() + ": " + pr.error);
                if (pr.has_seed && pr.seed != opt.seed) throw std::runtime_error("scout file " + opt.scout_file.string() + " was made for seed " + std::to_string(pr.seed) + ", this run is seed " + std::to_string(opt.seed));
                std::printf("scout file %s: %s, %zu events\n", opt.scout_file.string().c_str(), pr.legacy ? "legacy layout (no seed, no end line: truncation cannot be detected)" : "format 1", rep.events.size());
            } else {
                rep = scout_world(wc, opt.preroll + opt.film_seconds + 1.0);
                if (!opt.scout_file.empty()) {
                    const fs::path tmp = opt.scout_file.string() + ".tmp";
                    {
                        std::ofstream out(tmp, std::ios::binary);
                        write_scout_report(out, rep, opt.seed);
                        out.flush();
                        if (!out) throw std::runtime_error("cannot write scout file " + tmp.string());
                    }
                    fs::rename(tmp, opt.scout_file);
                }
            }
            std::printf("scout (seed %u): accident at %.1f s sim (film %.1f), response on scene at %.1f, cleared %.1f\n", opt.seed, rep.accident_t, rep.accident_t - opt.preroll, rep.onscene_t, rep.cleared_t);
            Director director(district, rep, opt.film_seconds, opt.preroll);
            for (const auto& sh : director.list()) std::printf("  shot at %6.1f s: %s\n", sh.first, sh.second.c_str());
            const double dt = 1.0 / opt.fps;
            const int total = static_cast<int>(opt.film_seconds * opt.fps);
            const int last = opt.end_frame < 0 ? total : std::min(opt.end_frame, total);
            FILE* pipe = nullptr;
            if (!opt.video.empty()) {
                fs::create_directories(opt.video.has_parent_path() ? opt.video.parent_path() : fs::path("."));
                char cmd[1024];
                std::snprintf(cmd, sizeof cmd, "ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgba -s %ux%u -r %g -i - -c:v libx264 -preset veryfast -crf %d -pix_fmt yuv420p \"%s\"", opt.width, opt.height, opt.fps, opt.crf, opt.video.string().c_str());
                pipe = SAIGE_POPEN(cmd);
                if (pipe == nullptr) throw std::runtime_error("cannot start ffmpeg");
            }
            // The world runs from its start: the pre-roll first (the streets fill up), then one step per frame; frames before the requested range are simulated but not drawn, so that any range is exactly what
            // a continuous render would show.
            for (double t = 0.0; t < opt.preroll; t += dt) {
                world.step(dt);
                actors.animate(world.pedestrians(), dt);
            }
            FILE* audio = opt.audio_log.empty() ? nullptr : std::fopen(opt.audio_log.string().c_str(), "w");
            double frame_ms = 0.0;
            int drawn = 0;
            int camera_violations = 0, cam_obstructed = 0, cam_solid = 0, cam_hidden = 0;
            std::map<std::string, int> violation_prints;
            std::map<std::string, std::array<int, 3>> by_shot;   // per shot: frames, obstructed + solid, subject hidden
            for (int f = 0; f < last; ++f) {
                const double tf = static_cast<double>(f) * dt;
                world.step(dt);
                actors.animate(world.pedestrians(), dt);
                const std::vector<TrafficEvent> frame_events = world.take_traffic_events();
                for (const TrafficEvent& e : frame_events)
                    if (e.type == TrafficEvent::Type::Impact && e.energy > 2500.0f) actors.impact(world.time_seconds(), e.x, e.y, 0.3, e.energy);
                const Cam cam = director.camera(tf, dt, world);
                if (opt.check_camera) {
                    static const std::vector<Box3> awnings = awning_boxes(district);
                    static const std::vector<StreetProp> trees = [&] {
                        std::vector<StreetProp> t;
                        for (const StreetProp& p : district.props)
                            if (p.type == "tree") t.push_back(p);
                        return t;
                    }();
                    const std::array<float, 3> eye3{cam.eye.x, cam.eye.y, cam.eye.z}, look3{cam.look.x, cam.look.y, cam.look.z};
                    const bool obstructed = view_obstructed(trees, awnings, eye3, look3, cam.fov_deg);
                    const bool solid = eye_in_solid(district, eye3);
                    const bool hidden = director.shot_tracks_subject(tf) && sightline_blocked_by_building(district, eye3, look3);   // only shots that follow a resident or a vehicle have a subject for a building to hide
                    if (obstructed) ++cam_obstructed;
                    if (solid) ++cam_solid;
                    if (hidden) ++cam_hidden;
                    const bool bad = obstructed || solid || hidden;
                    auto& bs = by_shot[director.shot_name(tf)];
                    ++bs[0];
                    if (obstructed || solid) ++bs[1];
                    if (hidden) {
                        ++bs[2];
                        if (++violation_prints["hidden:" + director.shot_name(tf)] <= 3) std::printf("subject hidden at frame %d (t=%.2f s, shot '%s')\n", f, tf, director.shot_name(tf).c_str());
                    }
                    if (bad) {
                        ++camera_violations;
                        if (++violation_prints[director.shot_name(tf)] <= 3) std::printf("camera obstructed at frame %d (t=%.2f s, shot '%s')\n", f, tf, director.shot_name(tf).c_str());
                    }
                }
                if (f < opt.start_frame) continue;
                if (audio != nullptr) {
                    // What the soundtrack needs about this frame: where the listener is, every vehicle, the residents within earshot, and the world's events (each is a real engine fact: a vehicle's rpm, a resident's gait, an impact's energy).
                    bool indoors = false;
                    for (const Interior& in : district.interiors)
                        if (in.room.contains(static_cast<double>(cam.eye.x), static_cast<double>(cam.eye.y)) && cam.eye.z < 3.8f) indoors = true;
                    std::fprintf(audio, "{\"f\":%d,\"t\":%.4f,\"in\":%d,\"cam\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f],\"veh\":[", f, tf, indoors ? 1 : 0, static_cast<double>(cam.eye.x), static_cast<double>(cam.eye.y), static_cast<double>(cam.eye.z), static_cast<double>(cam.look.x), static_cast<double>(cam.look.y), static_cast<double>(cam.look.z));
                    bool first = true;
                    for (const VehicleView& v : world.vehicles()) {
                        std::fprintf(audio, "%s[%u,%d,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d]", first ? "" : ",", v.id, static_cast<int>(v.kind), v.x, v.y, v.z, v.speed, v.engine_rpm, v.siren ? 1 : 0, v.disabled ? 1 : 0, v.braking ? 1 : 0);
                        first = false;
                    }
                    std::fprintf(audio, "],\"ped\":[");
                    first = true;
                    for (const PedView& p : world.pedestrians()) {
                        if (!p.visible || std::hypot(p.x - static_cast<double>(cam.eye.x), p.y - static_cast<double>(cam.eye.y)) > 90.0) continue;
                        std::fprintf(audio, "%s[%llu,%.2f,%.2f,%.2f,%.2f,%d,%d]", first ? "" : ",", static_cast<unsigned long long>(p.id), p.x, p.y, p.z, p.speed, static_cast<int>(p.state), p.inside_building ? 1 : 0);
                        first = false;
                    }
                    std::fprintf(audio, "],\"ev\":[");
                    first = true;
                    for (const TrafficEvent& e : frame_events) {
                        std::fprintf(audio, "%s[%d,%.2f,%.2f,%.0f,%u,%d]", first ? "" : ",", static_cast<int>(e.type), e.x, e.y, static_cast<double>(e.energy), e.vehicle, static_cast<int>(e.surface_kind));
                        first = false;
                    }
                    std::fprintf(audio, "]}\n");
                }
                if (opt.no_render) continue;
                if (!opt.only_frames.empty() && std::find(opt.only_frames.begin(), opt.only_frames.end(), f) == opt.only_frames.end()) continue;
                const render::Camera camera = render::look_at_camera(cam.eye, cam.look, {0, 0, 1}, cam.fov_deg * static_cast<float>(kPi / 180.0), 0.15f);
                std::vector<render::ModelDraw> draws;
                city.collect(draws, cam.eye, opt.far_distance);
                actors.collect_parked(draws, cam.eye, opt.far_distance);
                actors.collect(draws, world, world.time_seconds(), cam.eye, opt.far_distance);
                city.collect_signals(draws, [&](std::uint32_t inter, int leg) {
                    const traffic::IntersectionController* ic = district.traffic.find_intersection(inter);
                    const Intersection& I = district.intersections[inter - 1];
                    if (ic == nullptr) return 0;
                    const traffic::SignalColor col = ic->signal_for_lane(I.inbound[static_cast<std::size_t>(leg)]);
                    return col == traffic::SignalColor::Green ? 3 : col == traffic::SignalColor::Yellow ? 2 : 1;
                }, [&](std::uint32_t cw) { return world.walk_signal(cw); });
                const double hour = std::fmod(world.minute_of_day() / 60.0, 24.0);
                const render::SolarPosition sun = render::solar_position(190, hour, 37.0, kPi * 0.5);
                const double elev_deg = sun.elevation * 180.0 / kPi;
                render::SceneFrame frame;
                frame.tick = static_cast<std::uint64_t>(f);
                frame.camera = &camera;
                frame.models = draws;
                render::SceneLighting light = render::time_of_day_lighting(rs.atmosphere, sun.to_sun, rs.lighting, 0.2f);
                {   // Moonlight: below the horizon the sun's ambient gives way to a faint blue fill.
                    const float n = static_cast<float>(smooth((-elev_deg + 1.0) / 9.0));
                    const render::Float3 moon{0.030f, 0.042f, 0.085f};
                    light.ambient_ground = {light.ambient_ground.x * (1 - n) + moon.x * n, light.ambient_ground.y * (1 - n) + moon.y * n, light.ambient_ground.z * (1 - n) + moon.z * n};
                    light.ambient_sky_floor = 1.7f * n;
                }
                light.emissive_gain = static_cast<float>(emissive_gain_at(elev_deg));
                float shot_ev = 0.0f;
                if (director.shot_name(tf) == "inside the cafe") {   // the room's own lights: the emissive fixtures and the warm wall fill are lit for this shot (the renderer has no point lights)
                    light.emissive_gain = std::max(light.emissive_gain, 0.8f);
                    shot_ev = -0.5f;
                }
                frame.lighting = &light;
                frame.exposure_ev = static_cast<float>(exposure_for(elev_deg)) + opt.exposure_bias + shot_ev;
                const auto r0 = std::chrono::steady_clock::now();
                Rgba img = downsample(target.render(renderer, frame, nullptr), opt.ssaa);
                frame_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count();
                ++drawn;
                if (pipe != nullptr) {
                    if (std::fwrite(img.px.data(), 1, img.px.size(), pipe) != img.px.size()) throw std::runtime_error("ffmpeg pipe closed");
                } else {
                    char name[64];
                    std::snprintf(name, sizeof name, "frame_%05d.ppm", f);
                    write_ppm(opt.out / name, img);
                }
                if (!opt.only_frames.empty()) std::printf("camera frame %d eye %.4f %.4f %.4f look %.4f %.4f %.4f fov %.2f\n", f, static_cast<double>(cam.eye.x), static_cast<double>(cam.eye.y), static_cast<double>(cam.eye.z), static_cast<double>(cam.look.x), static_cast<double>(cam.look.y), static_cast<double>(cam.look.z), static_cast<double>(cam.fov_deg));
                if (f % 30 == 0) std::printf("frame %d/%d  t=%.1f s  shot '%s'  sun %.1f deg  %.2f s/frame avg\n", f, total, tf, director.shot_name(tf).c_str(), elev_deg, drawn > 0 ? frame_ms / drawn / 1000.0 : 0.0);
                std::fflush(stdout);
            }
            if (pipe != nullptr) SAIGE_PCLOSE(pipe);
            if (audio != nullptr) std::fclose(audio);
            std::printf("film: %d frames drawn, %.2f s/frame average\n", drawn, drawn > 0 ? frame_ms / drawn / 1000.0 : 0.0);
            if (opt.check_camera)
                for (const auto& [name, c] : by_shot)
                    if (c[1] > 0 || c[2] > 0) std::printf("  shot '%s': %d frames, %d obstructed or in solid, %d with the subject hidden\n", name.c_str(), c[0], c[1], c[2]);
            if (opt.check_camera) std::printf("camera check: %d of %d frames have the camera in or looking through foliage or an awning, inside solid geometry, or with a building between it and its subject (obstructed %d, in solid %d, subject hidden %d)\n", camera_violations, last, cam_obstructed, cam_solid, cam_hidden);
            return opt.check_camera && camera_violations > 0 ? 3 : 0;
        }

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
            const render::SolarPosition sun = render::solar_position(190, opt.hour + opt.hour_step * static_cast<double>(index), 37.0, kPi * 0.5);
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
