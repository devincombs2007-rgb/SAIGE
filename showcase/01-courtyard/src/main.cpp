// saige_showcase — Showcase 01 "Courtyard": a scripted run of SAIGE's capsule character through a PBR-lit courtyard, rendered by the
// real renderer (HDR, metallic-roughness, normal mapping, sorted alpha blending) on whatever Vulkan 1.3 device is present.
//
// What it produces (all under --out): stills/*.ppm, seq/frame_*.ppm, trace.json (every tick of the character), report.json (device,
// evidence class, event counts, hashes) and perf.json (saige::perf). Nothing here is a mock-up: the images are read back from the
// renderer's output target. Which device made them is recorded in report.json, so a software-rasterizer run is never mistaken for a
// physical-GPU one. This tool makes no performance claim; see docs/showcase/01-courtyard.md.
#include "course.hpp"

#include "saige/gfx_assets/gltf.hpp"
#include "saige/gfx_scene/stage_model.hpp"
#include "saige/perf.hpp"
#include "saige/render/camera.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/render/transform.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace saige;
using namespace saige::showcase;
using render::Float3;

namespace {

constexpr float pi_f = 3.14159265358979f;

struct Options {
    fs::path assets =
#ifdef SAIGE_SHOWCASE_DEFAULT_ASSETS
        SAIGE_SHOWCASE_DEFAULT_ASSETS;
#else
        "showcase/01-courtyard/assets";
#endif
    fs::path out = "showcase-out";
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t ssaa = 1;            // render at ssaa x the size and box-filter down
    int stills = -1;              // how many of the named stills to render (-1: all)
    double sequence_seconds = -1; // total length of the camera sequence (-1: the whole run; 0: none)
    uint32_t capture_every = 2;   // sequence frame every N fixed ticks (2 = 30 fps from the 60 Hz step)
    double intro_seconds = 4.0;   // establishing fly-over before the run
    bool validate = false;
    bool self_check = false;
    bool simulate_only = false;
    float exposure_ev = -0.3f;
    float sun_scale = 1.0f;       // multiplies the sun color
    float ambient_scale = 1.0f;   // multiplies the ground ambient
};

struct Rgba {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> px;  // RGBA8, sRGB encoded
};

[[noreturn]] void usage_error(const std::string& what)
{
    std::cerr << "saige_showcase: " << what << "\n"
              << "usage: saige_showcase [--assets DIR] [--out DIR] [--width N] [--height N] [--ssaa N] [--stills N]\n"
              << "                      [--sequence-seconds S] [--capture-every N] [--intro-seconds S] [--exposure EV] [--sun K] [--ambient K]\n"
              << "                      [--validate] [--self-check] [--simulate-only]\n";
    std::exit(2);
}

Options parse(int argc, char** argv)
{
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage_error("missing value for " + a);
            return argv[++i];
        };
        try {
            if (a == "--assets") o.assets = value();
            else if (a == "--out") o.out = value();
            else if (a == "--width") o.width = static_cast<uint32_t>(std::stoul(value()));
            else if (a == "--height") o.height = static_cast<uint32_t>(std::stoul(value()));
            else if (a == "--ssaa") o.ssaa = static_cast<uint32_t>(std::stoul(value()));
            else if (a == "--stills") o.stills = std::stoi(value());
            else if (a == "--sequence-seconds") o.sequence_seconds = std::stod(value());
            else if (a == "--capture-every") o.capture_every = static_cast<uint32_t>(std::stoul(value()));
            else if (a == "--intro-seconds") o.intro_seconds = std::stod(value());
            else if (a == "--exposure") o.exposure_ev = std::stof(value());
            else if (a == "--sun") o.sun_scale = std::stof(value());
            else if (a == "--ambient") o.ambient_scale = std::stof(value());
            else if (a == "--validate") o.validate = true;
            else if (a == "--self-check") o.self_check = true;
            else if (a == "--simulate-only") o.simulate_only = true;
            else usage_error("unknown option " + a);
        } catch (const std::logic_error&) {
            usage_error("bad value for " + a);
        }
    }
    if (o.width < 16 || o.height < 16 || o.width > 8192 || o.height > 8192 || o.ssaa < 1 || o.ssaa > 4 || o.capture_every < 1) {
        usage_error("width/height must be 16..8192, ssaa 1..4 and capture-every at least 1");
    }
    return o;
}

// ---- Presentation: the heading and camera paths are a pure function of the trace, so any tick can be rendered on its own.

struct Pose {
    Float3 feet;
    float yaw = 0.0f;
    float lean = 0.0f;  // radians of forward lean, from the horizontal speed
    Float3 camera_eye;
    Float3 camera_target;
};

float wrap_pi(float a)
{
    while (a > pi_f) a -= 2.0f * pi_f;
    while (a < -pi_f) a += 2.0f * pi_f;
    return a;
}

std::vector<Pose> make_poses(const CourseResult& run)
{
    std::vector<Pose> poses;
    poses.reserve(run.trace.size());
    float yaw = 0.0f;
    float cam_yaw = 0.0f;
    Float3 eye{};
    for (std::size_t t = 0; t < run.trace.size(); ++t) {
        const TickRecord& r = run.trace[t];
        const float speed = std::hypot(r.velocity.x, r.velocity.y);
        if (speed > 0.4f) {
            yaw += wrap_pi(std::atan2(r.velocity.y, r.velocity.x) - yaw) * 0.22f;
        }
        cam_yaw += wrap_pi(yaw - cam_yaw) * 0.025f;
        const Float3 feet{r.feet.x, r.feet.y, r.feet.z};
        Float3 want{feet.x - std::cos(cam_yaw) * 5.4f, feet.y - std::sin(cam_yaw) * 5.4f, 2.3f + feet.z * 0.7f};
        // Inside the walls, except that the camera may follow the character out through the west gate (|y| < 3).
        want.x = std::clamp(want.x, std::abs(feet.y) < 3.0f && feet.x < -14.0f ? -24.0f : -17.6f, 17.4f);
        want.y = std::clamp(want.y, -11.5f, 11.5f);
        if (t == 0) eye = want;
        eye = {eye.x + (want.x - eye.x) * 0.07f, eye.y + (want.y - eye.y) * 0.07f, eye.z + (want.z - eye.z) * 0.07f};
        const Float3 target{feet.x + std::cos(cam_yaw) * 1.6f, feet.y + std::sin(cam_yaw) * 1.6f, feet.z + 1.15f};
        poses.push_back({feet, yaw, std::min(speed / 6.0f, 1.0f) * 0.14f, eye, target});
    }
    return poses;
}

// Stills may name a moment of the run by what the character is doing instead of by a tick number.
constexpr int tick_none = -1;
constexpr int tick_on_ramp = -2;    // standing on the 25 degree ramp, halfway up
constexpr int tick_on_chute = -3;   // sliding down the 54 degree chute

struct Still {
    const char* name;
    Float3 eye, target;
    int tick;  // character pose from the run: a tick number, or one of tick_none / tick_on_ramp / tick_on_chute
    float fov_deg;
};

int resolve_tick(int code, const CourseResult& run)
{
    if (code >= 0) return std::min<int>(code, static_cast<int>(run.trace.size()) - 1);
    if (code == tick_none) return -1;
    for (std::size_t t = 0; t < run.trace.size(); ++t) {
        const TickRecord& r = run.trace[t];
        if (code == tick_on_ramp && r.grounded && r.ground_normal_z < 0.92f && r.ground_normal_z > 0.89f && r.feet.z > 0.75f && r.feet.y < -8.0f) return static_cast<int>(t);
        if (code == tick_on_chute && !r.grounded && r.feet.x > 7.3f && r.feet.x < 7.9f && r.feet.z < 0.8f && r.feet.y < -8.0f) return static_cast<int>(t);
    }
    return -1;
}

const std::vector<Still>& stills()
{
    static const std::vector<Still> list{
        {"01_establishing", {-17.0f, -10.5f, 7.0f}, {4.0f, 0.5f, 1.0f}, 538, 58.0f},
        {"02_colonnade", {-15.0f, -9.6f, 1.7f}, {-3.5f, 7.0f, 1.8f}, tick_none, 62.0f},
        {"03_crate_jump", {-9.8f, 0.2f, 2.4f}, {-5.4f, 3.6f, 0.9f}, 280, 55.0f},
        {"04_marble_steps", {0.8f, -5.8f, 1.5f}, {6.0f, 0.0f, 0.5f}, 548, 55.0f},
        {"05_material_row", {9.6f, -3.6f, 2.0f}, {15.6f, 0.0f, 1.5f}, 2440, 55.0f},
        {"06_glass_and_lanterns", {11.0f, 4.5f, 1.7f}, {16.3f, -0.5f, 1.7f}, tick_none, 62.0f},
        {"07_barrels", {-7.6f, -1.6f, 1.5f}, {-10.4f, -4.2f, 0.7f}, tick_none, 55.0f},
        {"08_overview", {1.0f, -9.0f, 30.0f}, {1.0f, 0.0f, 0.0f}, 1200, 50.0f},
        {"09_ramp_climb", {1.4f, -5.0f, 2.8f}, {2.6f, -9.0f, 0.9f}, tick_on_ramp, 62.0f},
        {"10_chute_slide", {11.4f, -6.6f, 1.7f}, {7.3f, -9.0f, 0.7f}, tick_on_chute, 58.0f},
    };
    return list;
}

// ---- Rendering plumbing (the same offscreen pattern the engine's own tests use).

class Offscreen {
public:
    Offscreen(rhi::Device& device, uint32_t width, uint32_t height) : device_(device), width_(width), height_(height)
    {
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_SRGB;
        td.width = width;
        td.height = height;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        td.debug_name = "ShowcaseTarget";
        color_ = device.create_texture(td);
        rhi::BufferDesc bd;
        bd.size = uint64_t{width} * height * 4;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = "ShowcaseReadback";
        readback_ = device.create_buffer(bd);
    }
    ~Offscreen()
    {
        device_.destroy(readback_);
        device_.destroy(color_);
    }
    Offscreen(const Offscreen&) = delete;
    Offscreen& operator=(const Offscreen&) = delete;

    Rgba render(render::SceneRenderer& renderer, const render::SceneFrame& frame)
    {
        render::RenderTarget target;
        target.texture = color_;
        target.width = width_;
        target.height = height_;
        target.readback_buffer = readback_;
        rhi::CommandList& cmd = device_.begin_frame();
        (void)renderer.render(cmd, frame, target);
        (void)device_.end_frame();
        device_.wait_idle();
        const auto bytes = device_.map(readback_);
        Rgba img;
        img.width = width_;
        img.height = height_;
        img.px.assign(reinterpret_cast<const uint8_t*>(bytes.data()), reinterpret_cast<const uint8_t*>(bytes.data()) + size_t{width_} * height_ * 4);
        return img;
    }

private:
    rhi::Device& device_;
    uint32_t width_, height_;
    rhi::TextureHandle color_;
    rhi::BufferHandle readback_;
};

Rgba downsample(const Rgba& src, uint32_t factor)
{
    if (factor == 1) return src;
    Rgba out;
    out.width = src.width / factor;
    out.height = src.height / factor;
    out.px.resize(size_t{out.width} * out.height * 4);
    for (uint32_t y = 0; y < out.height; ++y) {
        for (uint32_t x = 0; x < out.width; ++x) {
            uint32_t sum[4] = {0, 0, 0, 0};
            for (uint32_t dy = 0; dy < factor; ++dy) {
                for (uint32_t dx = 0; dx < factor; ++dx) {
                    const uint8_t* p = &src.px[(size_t{y * factor + dy} * src.width + x * factor + dx) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
            }
            for (int c = 0; c < 4; ++c) out.px[(size_t{y} * out.width + x) * 4 + static_cast<size_t>(c)] = static_cast<uint8_t>((sum[c] + factor * factor / 2) / (factor * factor));
        }
    }
    return out;
}

bool write_ppm(const fs::path& path, const Rgba& img)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "P6\n" << img.width << " " << img.height << "\n255\n";
    for (size_t i = 0; i < size_t{img.width} * img.height; ++i) out.write(reinterpret_cast<const char*>(&img.px[i * 4]), 3);
    return static_cast<bool>(out);
}

// Standard deviation of luma, to catch an empty or uniform frame.
double luma_stddev(const Rgba& img)
{
    double sum = 0, sum2 = 0;
    const size_t n = size_t{img.width} * img.height;
    for (size_t i = 0; i < n; ++i) {
        const double l = 0.2126 * img.px[i * 4] + 0.7152 * img.px[i * 4 + 1] + 0.0722 * img.px[i * 4 + 2];
        sum += l;
        sum2 += l * l;
    }
    const double mean = sum / static_cast<double>(n);
    return std::sqrt(std::max(0.0, sum2 / static_cast<double>(n) - mean * mean));
}

double mean_abs_diff(const Rgba& a, const Rgba& b)
{
    double d = 0;
    const size_t n = size_t{a.width} * a.height * 4;
    for (size_t i = 0; i < n; ++i) d += std::abs(int{a.px[i]} - int{b.px[i]});
    return d / static_cast<double>(n);
}

std::string json_escape(const std::string& s)
{
    std::string r;
    for (const char c : s) {
        if (c == '"' || c == '\\') r += '\\';
        r += (static_cast<unsigned char>(c) < 0x20) ? ' ' : c;
    }
    return r;
}

void write_trace(const fs::path& path, const CourseResult& run, const SceneData& scene)
{
    std::ofstream out(path);
    out.imbue(std::locale::classic());
    char buf[256];
    out << "{\"schema_version\":1,\"step_ns\":" << fixed_step.count() << ",\"character\":{\"radius\":" << scene.radius << ",\"height\":" << scene.height
        << "},\n\"events\":[";
    for (std::size_t i = 0; i < run.events.size(); ++i) {
        const Event& e = run.events[i];
        std::snprintf(buf, sizeof buf, "%s{\"tick\":%u,\"kind\":\"%s\",\"waypoint\":%u}", i ? "," : "", e.tick, to_string(e.kind), e.waypoint);
        out << buf;
    }
    out << "],\n\"ticks\":[";
    for (std::size_t i = 0; i < run.trace.size(); ++i) {
        const TickRecord& r = run.trace[i];
        std::snprintf(buf, sizeof buf, "%s[%.4f,%.4f,%.4f,%.3f,%.3f,%.3f,%d,%u]", i ? ",\n" : "\n", static_cast<double>(r.feet.x), static_cast<double>(r.feet.y),
                      static_cast<double>(r.feet.z), static_cast<double>(r.velocity.x), static_cast<double>(r.velocity.y), static_cast<double>(r.velocity.z), r.grounded ? 1 : 0, r.waypoint);
        out << buf;
    }
    out << "\n],\"columns\":[\"x\",\"y\",\"z\",\"vx\",\"vy\",\"vz\",\"grounded\",\"waypoint\"]}\n";
}

render::GpuModel stage(render::SceneRenderer& renderer, const fs::path& file, const char* name, std::vector<std::string>& warnings)
{
    gfx::GltfLoadResult loaded = gfx::load_gltf_file(file);
    if (!loaded.ok()) throw std::runtime_error(file.string() + ": " + loaded.error);
    gfx::StageSettings ss;
    ss.max_anisotropy = 8.0f;
    gfx::StageReport rep;
    render::GpuModel model = gfx::stage_model(renderer, *loaded.asset, name, ss, &rep);
    for (const std::string& w : loaded.asset->warnings) warnings.push_back(std::string(name) + ": " + w);
    for (const std::string& w : rep.warnings) warnings.push_back(std::string(name) + ": " + w);
    std::printf("staged %-10s %u textures (%.1f MiB), %u primitives, %.1f KiB vertices\n", name, rep.textures, static_cast<double>(rep.texture_bytes) / 1048576.0, rep.primitives,
                static_cast<double>(rep.vertex_bytes) / 1024.0);
    return model;
}

} // namespace

int main(int argc, char** argv)
{
    const Options opt = parse(argc, argv);
    try {
        perf::Zones zones;
        zones.reserve("sim.course", 1);
        const SceneData scene = load_scene(opt.assets / "scene.json");
        CourseResult run;
        {
            const auto scope = zones.scope("sim.course");
            run = run_course(scene);
        }
        std::map<std::string, int> counts;
        for (const Event& e : run.events) ++counts[to_string(e.kind)];
        std::printf("course: %zu ticks (%.1f s), %.1f m walked, max height %.2f m, %u failed waypoints, hash %016llx\n", run.trace.size(),
                    static_cast<double>(run.trace.size()) / 60.0, static_cast<double>(run.distance), static_cast<double>(run.max_height), run.failed_waypoints,
                    static_cast<unsigned long long>(run.hash));
        std::error_code ec;
        fs::create_directories(opt.out, ec);
        write_trace(opt.out / "trace.json", run, scene);
        if (!run.finished || run.failed_waypoints != 0) {
            std::cerr << "the scripted course did not complete cleanly\n";
            return 1;
        }
        if (opt.simulate_only) return 0;

        std::string why;
        if (!rhi::vulkan_device_available(&why)) {
            std::printf("no Vulkan 1.3 device (%s); skipping rendering\n", why.c_str());
            return 77;
        }
        rhi::DeviceDesc dd;
        dd.enable_validation = opt.validate;
        const char* require = std::getenv("SAIGE_REQUIRE_VALIDATION");
        dd.require_validation = opt.validate && require != nullptr && require[0] != '\0' && require[0] != '0';
        dd.application_name = "SAIGE Showcase 01";
        std::unique_ptr<rhi::Device> device = rhi::create_vulkan_device(dd);
        const rhi::DeviceInfo info = device->info();
        std::printf("device: %s (%s), software=%d, validation=%d\n", info.device_name.c_str(), info.driver_name.c_str(), info.software_device ? 1 : 0, info.validation_enabled ? 1 : 0);

        const uint32_t rw = opt.width * opt.ssaa, rh = opt.height * opt.ssaa;
        render::RendererSettings rs;
        rs.camera_mode = render::CameraMode::Fixed;
        rs.hdr = true;
        rs.model_shading = render::ModelShading::MetallicRoughness;
        rs.alpha_blending = true;
        rs.ground_half_size = 0.001f;  // the courtyard has its own cobbled ground; hide the built-in checkerboard
        rs.sky = {0.30f, 0.46f, 0.78f, 1.0f};
        rs.fog_density = 0.0035f;
        rs.lighting.sun_direction = {-0.50f, -0.42f, 0.76f};
        rs.lighting.sun_color = {1.9f * opt.sun_scale, 1.65f * opt.sun_scale, 1.4f * opt.sun_scale};
        rs.lighting.ambient_ground = {0.14f * opt.ambient_scale, 0.12f * opt.ambient_scale, 0.10f * opt.ambient_scale};
        rs.tone_map.exposure_ev = opt.exposure_ev;

        std::vector<std::string> warnings;
        double render_total_ms = 0;
        uint32_t still_count = 0, seq_count = 0;
        Rgba first_frame, last_frame, quiet_still;
        perf::FrameRecorder::Options fo;
        fo.measure_wall_interval = false;
        fo.memory_every_frames = 0;
        perf::FrameRecorder recorder(fo);
        std::string failure;
        uint64_t device_alloc_bytes = 0;
        uint32_t staged_textures_note = 0;
        (void)staged_textures_note;
        {
            Offscreen target(*device, rw, rh);
            render::SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, rs);
            render::GpuModel courtyard = stage(renderer, opt.assets / "courtyard.gltf", "courtyard", warnings);
            render::GpuModel character = stage(renderer, opt.assets / "character.gltf", "character", warnings);
            render::GpuModel blob = stage(renderer, opt.assets / "blob.gltf", "blob", warnings);
            const std::vector<Pose> poses = make_poses(run);

            const auto build_models = [&](int tick, std::vector<render::ModelDraw>& models) {
                models.clear();
                models.push_back({&courtyard, render::Mat4::identity()});
                for (const Decal& d : scene.decals) {
                    render::Transform t;
                    t.position = {d.x, d.y, d.z};
                    t.scale = {d.width, d.depth, 1.0f};
                    models.push_back({&blob, render::to_matrix(t)});
                }
                if (tick >= 0) {
                    const Pose& p = poses[static_cast<std::size_t>(tick)];
                    render::Transform t;
                    t.position = p.feet;
                    t.rotation = render::from_axis_angle({0, 0, 1}, p.yaw) * render::from_axis_angle({0, 1, 0}, p.lean);
                    models.push_back({&character, render::to_matrix(t)});
                    const float ground = surface_height(scene, p.feet.x, p.feet.y, p.feet.z + 0.25f);
                    const float height = std::max(0.0f, p.feet.z - ground);
                    render::Transform s;
                    s.position = {p.feet.x, p.feet.y, ground + 0.012f};
                    const float size = 1.15f * std::max(0.45f, 1.0f - 0.35f * height);
                    s.scale = {size, size, 1.0f};
                    models.push_back({&blob, render::to_matrix(s)});
                }
            };

            std::vector<render::ModelDraw> models;
            const auto shoot = [&](const render::Camera& cam, int tick) {
                build_models(tick, models);
                render::SceneFrame frame;
                frame.tick = tick >= 0 ? static_cast<uint64_t>(tick) : 0;
                frame.camera = &cam;
                frame.models = models;
                const auto t0 = std::chrono::steady_clock::now();
                Rgba img = downsample(target.render(renderer, frame), opt.ssaa);
                const auto elapsed = std::chrono::steady_clock::now() - t0;
                recorder.add_frame_interval(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
                render_total_ms += std::chrono::duration<double, std::milli>(elapsed).count();
                return img;
            };

            // Stills.
            const std::size_t still_limit = opt.stills < 0 ? stills().size() : std::min<std::size_t>(static_cast<std::size_t>(opt.stills), stills().size());
            for (std::size_t i = 0; i < still_limit; ++i) {
                const Still& s = stills()[i];
                const render::Camera cam = render::look_at_camera(s.eye, s.target, {0, 0, 1}, s.fov_deg * pi_f / 180.0f, 0.1f);
                const int tick = resolve_tick(s.tick, run);
                Rgba img = shoot(cam, tick);
                if (!write_ppm(opt.out / "stills" / (std::string(s.name) + ".ppm"), img)) throw std::runtime_error("cannot write a still");
                if (i == 0) quiet_still = img;
                ++still_count;
            }

            // Sequence: an establishing fly-over, then the chase camera for the whole scripted run.
            if (opt.sequence_seconds != 0) {
                const double fps = 60.0 / static_cast<double>(opt.capture_every);
                const double cap = opt.sequence_seconds < 0 ? 1e9 : opt.sequence_seconds;
                const uint32_t max_frames = static_cast<uint32_t>(std::min(cap * fps, 1e7));
                const uint32_t intro_frames = std::min(static_cast<uint32_t>(opt.intro_seconds * fps), max_frames);
                uint32_t frame_no = 0;
                const auto save = [&](const Rgba& img) {
                    char name[64];
                    std::snprintf(name, sizeof name, "frame_%05u.ppm", frame_no);
                    if (!write_ppm(opt.out / "seq" / name, img)) throw std::runtime_error("cannot write a sequence frame");
                    if (frame_no == 0) first_frame = img;
                    last_frame = img;
                    ++frame_no;
                    ++seq_count;
                };
                for (uint32_t k = 0; k < intro_frames; ++k) {
                    const float u = intro_frames > 1 ? static_cast<float>(k) / static_cast<float>(intro_frames - 1) : 0.0f;
                    const float ease = u * u * (3.0f - 2.0f * u);
                    const float angle = (-2.55f + 1.55f * ease);
                    const float radius = 27.0f - 6.0f * ease;
                    Float3 eye{4.0f + radius * std::cos(angle), radius * 0.62f * std::sin(angle), 15.0f - 8.5f * ease};
                    Float3 look{2.0f + 4.0f * ease, 0.0f, 1.0f};
                    // The last part of the fly-over hands over to the chase camera's first pose, so there is no cut.
                    const float w = std::clamp((u - 0.55f) / 0.45f, 0.0f, 1.0f);
                    const float blend = w * w * (3.0f - 2.0f * w);
                    const Pose& first = poses[0];
                    eye = {eye.x + (first.camera_eye.x - eye.x) * blend, eye.y + (first.camera_eye.y - eye.y) * blend, eye.z + (first.camera_eye.z - eye.z) * blend};
                    look = {look.x + (first.camera_target.x - look.x) * blend, look.y + (first.camera_target.y - look.y) * blend, look.z + (first.camera_target.z - look.z) * blend};
                    const render::Camera cam = render::look_at_camera(eye, look, {0, 0, 1}, (58.0f + 4.0f * blend) * pi_f / 180.0f, 0.1f);
                    save(shoot(cam, 0));
                }
                for (std::size_t tick = 0; tick < run.trace.size() && frame_no < max_frames; tick += opt.capture_every) {
                    const Pose& p = poses[tick];
                    const render::Camera cam = render::look_at_camera(p.camera_eye, p.camera_target, {0, 0, 1}, 62.0f * pi_f / 180.0f, 0.1f);
                    save(shoot(cam, static_cast<int>(tick)));
                }
            }
            device_alloc_bytes = device->memory_stats().allocated_bytes;
            std::printf("rendered %u stills and %u sequence frames at %ux%u (ssaa %u) in %.1f s; GPU allocations %.1f MiB\n", still_count, seq_count, opt.width, opt.height,
                        opt.ssaa, render_total_ms / 1000.0, static_cast<double>(device_alloc_bytes) / 1048576.0);
            for (const std::string& w : warnings) std::printf("note: %s\n", w.c_str());
            device->wait_idle();
        }  // renderer, models and target are destroyed before the device

        int exit_code = 0;
        if (opt.self_check) {
            // Cheap structural checks that a broken frame (empty, uniform, frozen) cannot pass.
            if (still_count > 0 && luma_stddev(quiet_still) < 8.0) failure += "the first still is nearly uniform; ";
            if (seq_count > 4 && mean_abs_diff(first_frame, last_frame) < 1.0) failure += "the first and last sequence frames are almost identical; ";
            if (seq_count > 4 && luma_stddev(last_frame) < 8.0) failure += "the last sequence frame is nearly uniform; ";
        }
#ifdef SAIGE_RHI_VULKAN_TESTING
        const uint32_t validation_errors = rhi::testing::process_validation_error_count();
        if (validation_errors != 0) failure += "Vulkan validation reported " + std::to_string(validation_errors) + " error(s); ";
#endif
        device.reset();

        perf::ReportContext ctx;
        ctx.workload = "showcase_01_courtyard_offscreen_render";
        ctx.build =
#ifdef NDEBUG
            "release";
#else
            "debug";
#endif
        ctx.adapter_class = info.software_device ? "software" : "physical";
        ctx.paced = false;                     // offscreen, unpaced: these intervals say nothing about delivered frame pacing
        ctx.qualified_target_hardware = false;
        ctx.hardware = info.device_name + " (" + info.driver_name + ")";
        ctx.fixed_step_ns = fixed_step.count();
        {
            std::ofstream perf_out(opt.out / "perf.json");
            perf_out << perf::to_json(recorder, zones, ctx);
        }
        {
            std::ofstream rep(opt.out / "report.json");
            rep.imbue(std::locale::classic());
            rep << "{\n \"schema_version\": 1,\n \"showcase\": \"01-courtyard\",\n";
            rep << " \"evidence_class\": \"" << (info.software_device ? "software_vulkan_cpu_rasterizer" : "physical_gpu_unqualified") << "\",\n";
            rep << " \"performance_claim\": \"none: offscreen, unpaced, not target hardware; the 60 FPS verdict stays not_evaluated\",\n";
            rep << " \"device\": {\"name\": \"" << json_escape(info.device_name) << "\", \"driver\": \"" << json_escape(info.driver_name) << "\", \"api\": \"" << info.api_version_major << "."
                << info.api_version_minor << "\", \"software\": " << (info.software_device ? "true" : "false") << ", \"validation\": " << (info.validation_enabled ? "true" : "false")
                << ", \"gpu_timestamps\": " << (info.gpu_timestamps ? "true" : "false") << "},\n";
            rep << " \"render\": {\"width\": " << opt.width << ", \"height\": " << opt.height << ", \"ssaa\": " << opt.ssaa << ", \"stills\": " << still_count
                << ", \"sequence_frames\": " << seq_count << ", \"capture_fps\": " << 60.0 / static_cast<double>(opt.capture_every) << ", \"mean_frame_ms\": "
                << (still_count + seq_count ? render_total_ms / (still_count + seq_count) : 0.0) << ", \"gpu_allocated_bytes\": " << device_alloc_bytes << ",\n  \"shading\": \"metallic_roughness\", \"hdr\": true, \"alpha_blending\": true, \"shadows\": \"none (blob decals are an artist-placed approximation)\"},\n";
            rep << " \"course\": {\"ticks\": " << run.trace.size() << ", \"seconds\": " << static_cast<double>(run.trace.size()) / 60.0 << ", \"distance_m\": " << static_cast<double>(run.distance)
                << ", \"max_height_m\": " << static_cast<double>(run.max_height) << ", \"failed_waypoints\": " << run.failed_waypoints << ", \"state_hash\": \"" << std::hex << run.hash << std::dec << "\", \"events\": {";
            bool first = true;
            for (const auto& [kind, n] : counts) {
                rep << (first ? "" : ", ") << "\"" << kind << "\": " << n;
                first = false;
            }
            rep << "}},\n \"self_check\": " << (opt.self_check ? (failure.empty() ? "\"passed\"" : "\"failed\"") : "\"not_run\"") << "\n}\n";
        }
        if (!failure.empty()) {
            std::cerr << "saige_showcase: " << failure << "\n";
            exit_code = 1;
        }
        return exit_code;
    } catch (const std::exception& e) {
        std::cerr << "saige_showcase: " << e.what() << "\n";
        return 1;
    }
}
