// saige_horizon: Showcase 03 "Beyond the Horizon". The real SAIGE renderer draws a scripted run of the streamed world simulation (sim.hpp): a character walks, runs,
// sprints and jumps to a parked car, drives it across the countryside and into a roadside barrier, climbs out and walks away as the sun sets. Nothing is composited from
// outside footage: every pixel is read back from the renderer's output target, and report.json records the device, the resolution, the cadence and the build.
#include "gpu_assets.hpp"
#include "props.hpp"
#include "sim.hpp"
#include "wheel_rig.hpp"

#include "saige/gfx_assets/animation.hpp"
#include "saige/gfx_assets/gltf.hpp"
#include "saige/gfx_scene/stage_model.hpp"
#include "saige/render/camera.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/render/transform.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <algorithm>
#include <array>
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
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace saige;
using namespace saige::horizon;
using render::Float3;
using render::Mat4;

namespace {

constexpr double pi = 3.14159265358979323846;
constexpr float pi_f = 3.14159265358979f;

// ---- Options -----------------------------------------------------------------------------------------------------------------------------------------------------

struct Options {
    fs::path assets = "/tmp/assets03/data";
    fs::path out = "horizon-out";
    fs::path frames_pipe;
    std::uint32_t width = 1280, height = 720;
    std::uint32_t ssaa = 1;
    double fps = 30.0;            // distinct frames per second of video time
    double start = 0.0, end = -1.0;  // simulation seconds
    std::vector<double> stills;   // render these times as still PPMs instead of a sequence
    std::uint32_t shadow_map = 1024;
    float shadow_distance = 160.0f;
    int cell_radius = 3;          // cells around the camera that are drawn in detail
    bool validate = false;
    bool no_bloom = false, no_ao = false, no_fxaa = false;
    float exposure_bias = 0.0f;
    float sun_disc = 3500.0f, moon = 14.0f, stars = 60.0f;
    double slow = 1.0;
    std::optional<std::array<double, 7>> cam;  // a fixed debug camera: eye, look, fov
};

[[noreturn]] void usage_error(const std::string& what)
{
    std::cerr << "saige_horizon: " << what << "\n"
              << "usage: saige_horizon [--assets DIR] [--out DIR] [--width N] [--height N] [--ssaa N] [--fps F] [--start S] [--end S] [--still T]... [--frames-pipe FIFO]\n"
              << "                     [--shadow-map N] [--shadow-distance M] [--cell-radius N] [--exposure EV] [--validate] [--no-bloom] [--no-ao] [--no-fxaa]\n";
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
            else if (a == "--frames-pipe") o.frames_pipe = value();
            else if (a == "--width") o.width = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--height") o.height = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--ssaa") o.ssaa = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--fps") o.fps = std::stod(value());
            else if (a == "--start") o.start = std::stod(value());
            else if (a == "--end") o.end = std::stod(value());
            else if (a == "--still") o.stills.push_back(std::stod(value()));
            else if (a == "--shadow-map") o.shadow_map = static_cast<std::uint32_t>(std::stoul(value()));
            else if (a == "--shadow-distance") o.shadow_distance = std::stof(value());
            else if (a == "--cell-radius") o.cell_radius = std::stoi(value());
            else if (a == "--exposure") o.exposure_bias = std::stof(value());
            else if (a == "--sun-disc") o.sun_disc = std::stof(value());
            else if (a == "--moon") o.moon = std::stof(value());
            else if (a == "--stars") o.stars = std::stof(value());
            else if (a == "--cam") {
                std::array<double, 7> c{};
                std::stringstream ss(value());
                std::string tok;
                std::size_t k = 0;
                while (std::getline(ss, tok, ',') && k < 7) c[k++] = std::stod(tok);
                if (k != 7) usage_error("--cam takes x,y,z,lx,ly,lz,fov");
                o.cam = c;
            }
            else if (a == "--validate") o.validate = true;
            else if (a == "--no-bloom") o.no_bloom = true;
            else if (a == "--no-ao") o.no_ao = true;
            else if (a == "--no-fxaa") o.no_fxaa = true;
            else usage_error("unknown option " + a);
        } catch (const std::logic_error&) {
            usage_error("bad value for " + a);
        }
    }
    if (o.width < 16 || o.height < 16 || o.width > 8192 || o.height > 8192 || o.ssaa < 1 || o.ssaa > 4 || o.fps <= 0.0 || o.fps > 120.0) usage_error("bad width/height/ssaa/fps");
    return o;
}

// ---- Small math ---------------------------------------------------------------------------------------------------------------------------------------------------

struct V3 {
    double x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V3 unit3(V3 a)
{
    const double l = len(a);
    return l > 1e-12 ? a * (1.0 / l) : V3{1, 0, 0};
}
V3 lerp(V3 a, V3 b, double t) { return a + (b - a) * t; }
double lerp(double a, double b, double t) { return a + (b - a) * t; }
double smooth(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
V3 flat_dir(double yaw) { return {std::cos(yaw), std::sin(yaw), 0}; }
V3 left_of(V3 dir) { return {-dir.y, dir.x, 0}; }
double wrap_angle(double a)
{
    while (a > pi) a -= 2 * pi;
    while (a < -pi) a += 2 * pi;
    return a;
}

Mat4 translation(Float3 t)
{
    Mat4 m = Mat4::identity();
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    return m;
}
Mat4 rot_x(float a) { return render::to_mat4(render::from_axis_angle({1, 0, 0}, a)); }
Mat4 rot_y(float a) { return render::to_mat4(render::from_axis_angle({0, 1, 0}, a)); }
Mat4 rot_z(float a) { return render::to_mat4(render::from_axis_angle({0, 0, 1}, a)); }
Mat4 scaling(float s)
{
    Mat4 m = Mat4::identity();
    m.m[0] = m.m[5] = m.m[10] = s;
    return m;
}
Mat4 scaling(float x, float y, float z)
{
    Mat4 m = Mat4::identity();
    m.m[0] = x;
    m.m[5] = y;
    m.m[10] = z;
    return m;
}
Mat4 from_quat(const VehicleQuat& q) { return render::to_mat4(render::normalize(render::Quat{q.x, q.y, q.z, q.w})); }

// ---- Frame readback -----------------------------------------------------------------------------------------------------------------------------------------------

struct Rgba {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> px;
};

class Offscreen {
public:
    Offscreen(rhi::Device& device, std::uint32_t width, std::uint32_t height) : device_(device), width_(width), height_(height)
    {
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_SRGB;
        td.width = width;
        td.height = height;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        td.debug_name = "HorizonTarget";
        color_ = device.create_texture(td);
        rhi::BufferDesc bd;
        bd.size = std::uint64_t{width} * height * 4;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = "HorizonReadback";
        readback_ = device.create_buffer(bd);
    }
    ~Offscreen()
    {
        device_.destroy(readback_);
        device_.destroy(color_);
    }
    Offscreen(const Offscreen&) = delete;
    Offscreen& operator=(const Offscreen&) = delete;

    Rgba render(render::SceneRenderer& renderer, const render::SceneFrame& frame, render::FrameStats* stats = nullptr)
    {
        render::RenderTarget target;
        target.texture = color_;
        target.width = width_;
        target.height = height_;
        target.readback_buffer = readback_;
        rhi::CommandList& cmd = device_.begin_frame();
        const render::FrameStats s = renderer.render(cmd, frame, target);
        if (stats != nullptr) *stats = s;
        (void)device_.end_frame();
        device_.wait_idle();
        const auto bytes = device_.map(readback_);
        Rgba img;
        img.width = width_;
        img.height = height_;
        img.px.assign(reinterpret_cast<const std::uint8_t*>(bytes.data()), reinterpret_cast<const std::uint8_t*>(bytes.data()) + std::size_t{width_} * height_ * 4);
        return img;
    }

private:
    rhi::Device& device_;
    std::uint32_t width_, height_;
    rhi::TextureHandle color_;
    rhi::BufferHandle readback_;
};

Rgba downsample(const Rgba& src, std::uint32_t factor)
{
    if (factor == 1) return src;
    Rgba out;
    out.width = src.width / factor;
    out.height = src.height / factor;
    out.px.resize(std::size_t{out.width} * out.height * 4);
    for (std::uint32_t y = 0; y < out.height; ++y) {
        for (std::uint32_t x = 0; x < out.width; ++x) {
            std::uint32_t sum[4] = {0, 0, 0, 0};
            for (std::uint32_t dy = 0; dy < factor; ++dy) {
                for (std::uint32_t dx = 0; dx < factor; ++dx) {
                    const std::uint8_t* p = &src.px[(std::size_t{y * factor + dy} * src.width + x * factor + dx) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
            }
            for (int c = 0; c < 4; ++c) out.px[(std::size_t{y} * out.width + x) * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>((sum[c] + factor * factor / 2) / (factor * factor));
        }
    }
    return out;
}

bool write_ppm(std::ostream& out, const Rgba& img)
{
    out << "P6\n" << img.width << " " << img.height << "\n255\n";
    std::vector<char> rgb(std::size_t{img.width} * img.height * 3);
    for (std::size_t i = 0; i < std::size_t{img.width} * img.height; ++i) std::memcpy(rgb.data() + i * 3, img.px.data() + i * 4, 3);
    out.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(out);
}

bool write_ppm(const fs::path& path, const Rgba& img)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    return out && write_ppm(out, img);
}

// ---- The recorded run (so the director can look ahead) --------------------------------------------------------------------------------------------------------------

struct TrackPoint {
    V3 car, chr;
    double car_yaw = 0, chr_yaw = 0, car_speed = 0, chr_speed = 0;
    bool driving = false;
    Phase phase = Phase::Establish;
};

class Track {
public:
    void push(const TrackPoint& p) { pts_.push_back(p); }
    [[nodiscard]] double duration() const { return static_cast<double>(pts_.size() - 1) / 60.0; }
    [[nodiscard]] TrackPoint at(double t) const
    {
        const double f = std::clamp(t, 0.0, duration()) * 60.0;
        const std::size_t i = static_cast<std::size_t>(f);
        const std::size_t j = std::min(i + 1, pts_.size() - 1);
        const double u = f - static_cast<double>(i);
        TrackPoint r = pts_[i];
        r.car = lerp(pts_[i].car, pts_[j].car, u);
        r.chr = lerp(pts_[i].chr, pts_[j].chr, u);
        r.car_yaw = pts_[i].car_yaw + wrap_angle(pts_[j].car_yaw - pts_[i].car_yaw) * u;
        r.chr_yaw = pts_[i].chr_yaw + wrap_angle(pts_[j].chr_yaw - pts_[i].chr_yaw) * u;
        r.car_speed = lerp(pts_[i].car_speed, pts_[j].car_speed, u);
        r.chr_speed = lerp(pts_[i].chr_speed, pts_[j].chr_speed, u);
        return r;
    }

private:
    std::vector<TrackPoint> pts_;
};

TrackPoint capture_point(const Sim& sim)
{
    TrackPoint p;
    const D2 o = sim.origin();
    if (const Vehicle* v = sim.vehicle()) {
        const VehicleState& s = v->state();
        p.car = {static_cast<double>(s.position.x) + o.x, static_cast<double>(s.position.y) + o.y, static_cast<double>(s.position.z)};
        p.car_yaw = s.yaw;
        p.car_speed = s.forward_speed;
    }
    const Vec3 f = sim.character_feet();
    p.chr = {static_cast<double>(f.x) + o.x, static_cast<double>(f.y) + o.y, static_cast<double>(f.z)};
    p.chr_yaw = sim.character_yaw();
    p.chr_speed = sim.character_speed();
    p.driving = sim.driving();
    p.phase = sim.phase();
    return p;
}

// ---- Time of day --------------------------------------------------------------------------------------------------------------------------------------------------

struct SunKey {
    double t, elevation_deg, azimuth_deg;
};
// Sunrise ahead of the town, noon over the drive, a low sun behind the walker, dusk and night at the overlook.
constexpr SunKey sun_keys[] = {{0, 4.0, 12}, {6, 5.0, 12}, {14, 7.5, 10}, {24, 11.5, 2}, {47, 24, -15}, {75, 44, -45}, {91, 43, -75}, {100, 38, -95}, {108, 22, -125}, {116, 9, -150},
                               {121, 3.5, -158}, {126, -0.5, -163}, {131, -6.5, -170}, {136, -12.5, -176}, {142, -17.0, -180}};

Float3 sun_at(double t, double* elevation_out = nullptr)
{
    const std::size_t n = sizeof(sun_keys) / sizeof(sun_keys[0]);
    std::size_t i = 0;
    while (i + 2 < n && t > sun_keys[i + 1].t) ++i;
    const double u = smooth((t - sun_keys[i].t) / (sun_keys[i + 1].t - sun_keys[i].t));
    const double el = lerp(sun_keys[i].elevation_deg, sun_keys[i + 1].elevation_deg, u) * pi / 180.0;
    const double az = lerp(sun_keys[i].azimuth_deg, sun_keys[i + 1].azimuth_deg, u) * pi / 180.0;
    if (elevation_out != nullptr) *elevation_out = el * 180.0 / pi;
    return {static_cast<float>(std::cos(el) * std::cos(az)), static_cast<float>(std::cos(el) * std::sin(az)), static_cast<float>(std::sin(el))};
}

// Exposure in stops: the eye adapts as the light falls.
double exposure_at(double t)
{
    struct K {
        double t, ev;
    };
    static const K keys[] = {{0, 0.2}, {8, -0.1}, {20, -0.4}, {47, -0.55}, {91, -0.55}, {104, -0.45}, {116, -0.1}, {124, 0.6}, {132, 1.5}, {142, 2.1}};
    const std::size_t n = sizeof(keys) / sizeof(keys[0]);
    if (t <= keys[0].t) return keys[0].ev;
    for (std::size_t i = 0; i + 1 < n; ++i) {
        if (t <= keys[i + 1].t) return lerp(keys[i].ev, keys[i + 1].ev, smooth((t - keys[i].t) / (keys[i + 1].t - keys[i].t)));
    }
    return keys[n - 1].ev;
}

std::uint32_t emissive_level_at(double elevation_deg)
{
    if (elevation_deg > 6.0) return 0;
    if (elevation_deg > 1.0) return 1;
    if (elevation_deg > -4.0) return 2;
    return 3;
}

// ---- The camera director ------------------------------------------------------------------------------------------------------------------------------------------

struct Shot {
    V3 eye, look;
    double fov_deg = 55.0;
    int cell_radius = 3;
    bool boom = true;  // keep solid geometry out of the sight line
    const char* name = "";
};

struct Director {
    const horizon::World& world;
    const Track& track;
    const Sim::Timeline& tl;
    double crash_time;
    D2 crash_point;

    [[nodiscard]] V3 clamp_ground(V3 eye, double clearance = 0.7) const
    {
        eye.z = std::max(eye.z, world.height(eye.x, eye.y) + clearance);
        return eye;
    }

    [[nodiscard]] Shot shot(double t) const
    {
        const TrackPoint now = track.at(t);
        Shot s;
        const auto chr_eye = [&](double lift) { return V3{now.chr.x, now.chr.y, now.chr.z + lift}; };
        const auto car_mid = [&](const TrackPoint& p) { return V3{p.car.x, p.car.y, p.car.z + 0.2}; };
        const V3 car_dir = [&] {
            const TrackPoint a = track.at(t - 1.0);
            const V3 d = car_mid(now) - car_mid(a);
            return std::hypot(d.x, d.y) > 0.5 ? unit3({d.x, d.y, 0}) : flat_dir(now.car_yaw);
        }();
        const V3 chr_fwd = flat_dir(now.chr_yaw);
        const double t_enter = tl.enter, t_drive = tl.drive, t_crash = crash_time;
        if (t < 8.0) {
            // An aerial crane over the sleeping town as the sun clears the horizon.
            const double u = smooth(t / 8.0);
            s.eye = lerp({44, -46, 52}, {70, -24, 24}, u);
            s.look = lerp({122, -4, 6}, {104, -5, 3}, u);
            s.fov_deg = 54;
            s.name = "sunrise aerial";
            s.boom = false;
            s.cell_radius = 4;
        } else if (t < 15.0) {
            const double u = smooth((t - 8.0) / 7.0);
            s.eye = lerp({76, 8.5, 1.5}, {84, 7.0, 1.5}, u);
            s.look = lerp({100, -5, 2.4}, {97, -5, 1.6}, u);
            s.fov_deg = lerp(46, 38, u);
            s.name = "dawn approach";
        } else if (t < tl.walk + 2.5) {
            const double u = smooth((t - 15.0) / (tl.walk + 2.5 - 15.0));
            s.eye = lerp({99.5, -8.8, 1.55}, {100.4, -8.2, 1.5}, u);
            s.look = chr_eye(1.25);
            s.fov_deg = lerp(40, 34, u);
            s.name = "idle push-in";
        } else if (t < tl.sprint + 0.2) {
            // A side track across the walk and the run.
            s.eye = {now.chr.x + 2.5 + (t - tl.walk) * 0.30, now.chr.y + 8.2, now.chr.z + 1.25};
            s.look = chr_eye(1.1);
            s.fov_deg = 46;
            s.name = "side track";
        } else if (t < tl.jump + 2.0) {
            // A fixed low camera by the route that the sprint and the jump run past.
            s.eye = {138.0, 6.0, 0.85};
            s.eye.z = world.height(138.0, 6.0) + 0.9;
            s.look = chr_eye(1.2);
            s.fov_deg = lerp(52, 38, smooth((t - tl.sprint) / 4.0));
            s.name = "sprint and jump";
        } else if (t < t_enter) {
            // The walk up to the car from the gas station side.
            s.eye = {148.0, 20.0, world.height(148.0, 20.0) + 2.4};
            s.look = lerp(chr_eye(1.2), car_mid(now), 0.35);
            s.fov_deg = 42;
            s.name = "approach the car";
        } else if (t < t_drive) {
            // The driver's door.
            const double u = smooth((t - t_enter) / (t_drive - t_enter));
            const V3 door = car_mid(now) + left_of(flat_dir(now.car_yaw)) * 1.0 + flat_dir(now.car_yaw) * 0.3 + V3{0, 0, 0.7};
            s.eye = door + left_of(flat_dir(now.car_yaw)) * lerp(3.4, 2.7, u) - flat_dir(now.car_yaw) * 2.2 + V3{0, 0, 0.5};
            s.look = door;
            s.fov_deg = 38;
            s.name = "door";
        } else if (t < t_drive + 7.0) {
            // Chase from behind and above, as the car pulls out of the station.
            const TrackPoint lag = track.at(t - 0.4);
            s.eye = car_mid(lag) - car_dir * 9.0 + V3{0, 0, 3.3};
            s.look = car_mid(now) + car_dir * 3.0 + V3{0, 0, 0.6};
            s.fov_deg = 54;
            s.name = "chase";
        } else if (t < t_drive + 13.5) {
            // A roadside pass-by at a fixed point on the main street.
            const double tp = t_drive + 10.2;
            const TrackPoint at_pass = track.at(tp);
            const V3 dir = unit3({track.at(tp + 0.5).car.x - track.at(tp - 0.5).car.x, track.at(tp + 0.5).car.y - track.at(tp - 0.5).car.y, 0});
            s.eye = at_pass.car + left_of(dir) * -7.5 + V3{0, 0, 1.15};
            s.eye = clamp_ground(s.eye, 0.9);
            s.look = car_mid(now) + V3{0, 0, 0.3};
            s.fov_deg = lerp(40, 28, smooth(1.0 - std::min(1.0, len(car_mid(now) - s.eye) / 60.0)));
            s.name = "pass-by";
        } else if (t < t_drive + 22.0) {
            // A drone above and beside the car, drifting round it.
            const double u = (t - (t_drive + 13.5)) / 8.5;
            const double ang = lerp(2.3, 3.6, u);
            s.eye = car_mid(now) + V3{std::cos(ang + std::atan2(car_dir.y, car_dir.x)) * 22.0, std::sin(ang + std::atan2(car_dir.y, car_dir.x)) * 22.0, 11.0 + 5.0 * u};
            s.eye = clamp_ground(s.eye, 3.0);
            s.look = car_mid(now) + car_dir * 4.0;
            s.fov_deg = 48;
            s.name = "drone";
            s.boom = false;
            s.cell_radius = 4;
        } else if (t < t_drive + 30.0) {
            // Out ahead of the car and low, looking back at it.
            s.eye = car_mid(now) + car_dir * 8.5 + left_of(car_dir) * 2.4 + V3{0, 0, 0.95};
            s.eye = clamp_ground(s.eye, 0.7);
            s.look = car_mid(now) + V3{0, 0, 0.5};
            s.fov_deg = 50;
            s.name = "lead";
        } else if (t < t_crash - 8.5) {
            const TrackPoint lag = track.at(t - 0.5);
            s.eye = car_mid(lag) - car_dir * 6.5 + left_of(car_dir) * -4.3 + V3{0, 0, 1.7};
            s.eye = clamp_ground(s.eye, 1.0);
            s.look = car_mid(now) + V3{0, 0, 0.5};
            s.fov_deg = 52;
            s.name = "rear quarter";
        } else if (t < t_crash + 2.5) {
            // The bend: a fixed camera at the outside of the corner sees the car come in and the barrier take it.
            const V3 cp = {crash_point.x, crash_point.y, world.height(crash_point.x, crash_point.y)};
            const TrackPoint at_hit = track.at(t_crash - 0.3);
            const V3 dir = unit3({at_hit.car.x - track.at(t_crash - 1.3).car.x, at_hit.car.y - track.at(t_crash - 1.3).car.y, 0});
            s.eye = cp - dir * 22.0 + left_of(dir) * 9.0;
            s.eye.z = world.height(s.eye.x, s.eye.y) + 1.5;
            s.look = car_mid(now) + V3{0, 0, 0.4};
            s.fov_deg = lerp(42, 36, smooth((t - (t_crash - 8.5)) / 8.5));
            s.name = "approach bend";
        } else if (t < tl.exit + 0.3) {
            // The wreck close up, dust still hanging.
            const V3 dir = flat_dir(now.car_yaw);
            const double u = smooth((t - (t_crash + 2.5)) / (tl.exit + 0.3 - (t_crash + 2.5)));
            s.eye = car_mid(now) + dir * lerp(5.8, 4.4, u) + left_of(dir) * lerp(-4.0, -3.2, u) + V3{0, 0, lerp(1.6, 1.3, u)};
            s.eye = clamp_ground(s.eye, 0.8);
            s.look = car_mid(now) + dir * 1.8 + V3{0, 0, 0.55};
            s.fov_deg = 46;
            s.name = "wreck";
        } else if (t < tl.walk_away + 1.5) {
            // The driver climbs out and walks into the dust: from in front of the wreck, over the bonnet.
            const V3 dir = flat_dir(now.car_yaw);
            s.eye = car_mid(now) + dir * 7.2 + left_of(dir) * 2.6 + V3{0, 0, 1.55};
            s.eye = clamp_ground(s.eye, 0.8);
            s.look = lerp(car_mid(now) + V3{0, 0, 0.5}, chr_eye(1.2), 0.55 * smooth((t - tl.exit - 0.8) / 1.5));  // the capsule only appears at the door once the rider steps out
            s.fov_deg = 44;
            s.name = "exit";
            s.boom = false;
        } else if (t < tl.overlook - 2.0) {
            // Following the walk away, low and behind, with the sun in the lens.
            s.eye = chr_eye(0.0) - chr_fwd * 5.6 + left_of(chr_fwd) * 1.3 + V3{0, 0, 1.25};
            s.eye = clamp_ground(s.eye, 0.9);
            s.look = chr_eye(1.2) + chr_fwd * 1.5;
            s.fov_deg = 46;
            s.name = "walk away";
        } else if (t < tl.end - 16.0) {
            // The overlook: the walker stands against the low sun, seen from the dark side of the hill.
            const double u = smooth((t - (tl.overlook - 2.0)) / (tl.end - 16.0 - (tl.overlook - 2.0)));
            const V3 sun_h = unit3({-0.91, -0.42, 0});
            s.eye = chr_eye(0.0) - sun_h * lerp(7.0, 11.0, u) + left_of(sun_h) * lerp(2.4, 4.0, u) + V3{0, 0, lerp(1.2, 2.4, u)};
            s.eye = clamp_ground(s.eye, 0.9);
            s.look = chr_eye(1.6) + sun_h * 12.0 + V3{0, 0, lerp(0.0, 3.0, u)};
            s.fov_deg = lerp(46, 52, u);
            s.name = "overlook";
            s.boom = false;
            s.cell_radius = 4;
        } else {
            // The pull-back at night: up and away from the lone walker, tilting from the first stars down to the wreck and the long road.
            const double u = smooth((t - (tl.end - 16.0)) / 16.0);
            const V3 sun_h = unit3({-0.91, -0.42, 0});
            const V3 pair = (chr_eye(0.0) + V3{now.car.x, now.car.y, now.car.z}) * 0.5;
            s.eye = chr_eye(0.0) - sun_h * lerp(11.0, 42.0, u) + left_of(sun_h) * lerp(4.0, 16.0, u) + V3{0, 0, lerp(2.4, 1.0, u) + 34.0 * u * u};
            const V3 sky = chr_eye(1.6) + sun_h * -8.0 + V3{0, 0, 16.0};
            s.look = lerp(sky, pair, smooth(u * 1.15));
            s.fov_deg = lerp(52, 60, u);
            s.name = "pull-back";
            s.boom = false;
            s.cell_radius = 4;
        }
        s.eye = clamp_ground(s.eye, 0.5);
        return s;
    }
};

// ---- The show -----------------------------------------------------------------------------------------------------------------------------------------------------

struct CellGpu {
    std::array<render::GpuModel, emissive_levels> models;
    std::vector<PropInstance> props;
    double ox = 0, oy = 0;
};

struct CarParts {
    render::GpuModel body, door_l, door_r, hood;
    std::array<render::GpuModel, 4> wheels;
    std::array<Float3, 4> wheel_center{};  // in the corrected car frame (model space)
    Float3 body_lo{}, body_hi{};
    Float3 door_l_hinge{}, door_r_hinge{}, hood_hinge{};
    float wheel_radius = 0.35f;
    Mat4 fix = Mat4::identity();
    bool ok = false;
};

class Show {
public:
    Show(rhi::Device& device, const Options& opt, const horizon::World& world) : device_(device), opt_(opt), world_(world)
    {
        const fs::path data = opt.assets;
        render::RendererSettings rs;
        rs.camera_mode = render::CameraMode::Fixed;
        rs.hdr = true;
        rs.model_shading = render::ModelShading::MetallicRoughness;
        rs.alpha_blending = true;
        rs.skinning = true;
        rs.dynamic_lighting = true;
        rs.ground_half_size = 0.001f;
        rs.sky = {0.30f, 0.46f, 0.78f, 1.0f};
        rs.fog_density = 0.00055f;
        rs.lighting.sun_direction = {0.5f, 0.1f, 0.6f};
        rs.lighting.sun_color = {2.4f, 2.0f, 1.65f};
        rs.lighting.ambient_ground = {0.17f, 0.15f, 0.125f};
        rs.tone_map.exposure_ev = -0.5f;
        rs.atmosphere.enabled = true;
        rs.atmosphere.exposure = 1.0f;
        rs.atmosphere.sun_disc_intensity = opt.sun_disc;
        rs.atmosphere.moon_intensity = opt.moon;
        rs.atmosphere.star_intensity = opt.stars;
        rs.shadows.enabled = true;
        rs.shadows.cascades = 3;
        rs.shadows.map_size = opt.shadow_map;
        rs.shadows.distance = opt.shadow_distance;
        rs.shadows.filter_radius = 1;
        rs.bloom.enabled = !opt.no_bloom;
        rs.ambient_occlusion.enabled = !opt.no_ao;
        rs.fxaa.enabled = !opt.no_fxaa;
        settings_ = rs;
        renderer_ = std::make_unique<render::SceneRenderer>(device, rhi::Format::RGBA8_SRGB, rs);
        materials_ = std::make_unique<MaterialLibrary>(*renderer_, data, warnings_);
        stage_props(data);
        stage_car(data);
        stage_character(data);
        stage_fx();
        // The coarse terrain beyond the streamed cells: the whole run, lowered a little so that it never fights the detailed cells above it.
        const double fx0 = -400, fy0 = -520, fx1 = 1500, fy1 = 380;
        far_origin_ = {fx0, fy0};
        MeshPart far = world.far_terrain(fx0, fy0, fx1, fy1, 22.0, 0.9);
        far_model_ = upload_model(*renderer_, "far terrain", cell_meshes(&far, 1), materials_->level(0));
        std::printf("far terrain: %zu vertices\n", far.vertices.size());
        // A very coarse skirt out to the horizon, so that the sky pass never shows through below the skyline.
        MeshPart skirt = world.far_terrain(-4000, -3500, 5500, 3500, 100.0, 5.0);
        skirt_origin_ = {-4000, -3500};
        skirt_model_ = upload_model(*renderer_, "terrain skirt", cell_meshes(&skirt, 1), materials_->level(0));
    }

    [[nodiscard]] render::SceneRenderer& renderer() { return *renderer_; }
    [[nodiscard]] const std::vector<std::string>& warnings() const { return warnings_; }
    [[nodiscard]] const CarParts& car() const { return car_; }

    // Renders simulation state `sim` at video time `t` through the camera `shot`.
    Rgba render(Offscreen& target, const Sim& sim, double t, const Shot& shot, std::uint64_t frame_index, render::FrameStats* stats)
    {
        const D2 origin = sim.origin();
        draws_.clear();
        double elevation = 0;
        const Float3 to_sun = sun_at(t, &elevation);
        const std::uint32_t level = emissive_level_at(elevation);

        const auto local = [&](double wx, double wy, double wz) { return Float3{static_cast<float>(wx - origin.x), static_cast<float>(wy - origin.y), static_cast<float>(wz)}; };
        const Float3 eye = local(shot.eye.x, shot.eye.y, shot.eye.z), look = local(shot.look.x, shot.look.y, shot.look.z);
        const render::Camera camera = render::look_at_camera(eye, look, {0, 0, 1}, static_cast<float>(shot.fov_deg * pi / 180.0), 0.12f);

        // Terrain: far, then the detailed cells around the camera.
        draws_.push_back({&skirt_model_, translation(local(skirt_origin_.x, skirt_origin_.y, 0))});
        draws_.push_back({&far_model_, translation(local(far_origin_.x, far_origin_.y, 0))});
        const int radius = std::max(opt_.cell_radius, shot.cell_radius);
        const int ccx = static_cast<int>(std::floor(shot.eye.x / cell_size)), ccy = static_cast<int>(std::floor(shot.eye.y / cell_size));
        const int fcx = static_cast<int>(std::floor(shot.look.x / cell_size)), fcy = static_cast<int>(std::floor(shot.look.y / cell_size));
        std::vector<const CellGpu*> visible;
        for (int cy = std::min(ccy, fcy) - radius; cy <= std::max(ccy, fcy) + radius; ++cy) {
            for (int cx = std::min(ccx, fcx) - radius; cx <= std::max(ccx, fcx) + radius; ++cx) {
                // Only cells within the radius of the eye or of the point looked at.
                const double mx = (cx + 0.5) * cell_size, my = (cy + 0.5) * cell_size;
                const double de = std::hypot(mx - shot.eye.x, my - shot.eye.y), dl = std::hypot(mx - shot.look.x, my - shot.look.y);
                if (std::min(de, dl) > (radius + 0.5) * cell_size) continue;
                const CellGpu& c = cell(cx, cy);
                visible.push_back(&c);
                draws_.push_back({&c.models[level], translation(local(c.ox, c.oy, 0))});
            }
        }
        trim_cells(ccx, ccy, fcx, fcy, radius + 2);
        // Props of those cells.
        for (const CellGpu* c : visible) {
            for (const PropInstance& p : c->props) add_prop(p, local(p.x, p.y, p.z));
        }
        // Physical props and debris.
        for (const PropBody& b : sim.bodies()) add_body(b, origin);
        // Dust.
        for (const DustPuff& d : sim.dust()) {
            const float f = std::clamp(d.age / d.life, 0.0f, 0.999f);
            const int lv = std::clamp(5 - static_cast<int>(f * 6.0f), 0, 5);
            Mat4 m = translation(local(d.x, d.y, d.z)) * scaling(d.size * (0.45f + 1.1f * f));
            draws_.push_back({&puffs_[static_cast<std::size_t>(lv)], m});
        }
        add_car(sim, origin, level, t);
        add_character(sim, origin, t);

        render::SceneFrame frame;
        frame.tick = frame_index;
        frame.camera = &camera;
        frame.models = draws_;
        render::SceneLighting light{};
        light = render::time_of_day_lighting(settings_.atmosphere, to_sun, settings_.lighting, 0.2f);
        {
            // Moonlight: below the horizon the sun's ambient gives way to a faint blue fill (the sky's own horizon glow fades to nothing).
            const float n = static_cast<float>(smooth((-elevation + 1.0) / 9.0));
            const Float3 moon{0.030f, 0.042f, 0.085f};
            light.ambient_ground = {light.ambient_ground.x * (1 - n) + moon.x * n, light.ambient_ground.y * (1 - n) + moon.y * n, light.ambient_ground.z * (1 - n) + moon.z * n};
            light.ambient_sky_floor = 1.7f * n;
        }
        frame.lighting = &light;
        frame.exposure_ev = static_cast<float>(exposure_at(t)) + opt_.exposure_bias;
        Rgba img = downsample(target.render(*renderer_, frame, stats), opt_.ssaa);
        return img;
    }

    // Trees, rocks and lamps have no collision, so the camera boom cannot see them: push an eye that sits inside one out to its edge.
    void nudge_from_props(Shot& shot)
    {
        const int cx0 = static_cast<int>(std::floor(shot.eye.x / cell_size)), cy0 = static_cast<int>(std::floor(shot.eye.y / cell_size));
        for (int pass = 0; pass < 3; ++pass) {
            for (int cy = cy0 - 1; cy <= cy0 + 1; ++cy) {
                for (int cx = cx0 - 1; cx <= cx0 + 1; ++cx) {
                    for (const PropInstance& p : cell(cx, cy).props) {
                        double r = 0;
                        double top = 0;
                        switch (p.type) {
                            case PropType::TreePine: r = 3.3 * static_cast<double>(p.scale); top = 9.5 * static_cast<double>(p.scale); break;
                            case PropType::TreeRound: r = 3.6 * static_cast<double>(p.scale); top = 6.5 * static_cast<double>(p.scale); break;
                            case PropType::RockLarge: r = 3.0 * static_cast<double>(p.scale) * 0.3 + 0.8; top = 3.0; break;
                            case PropType::RockSmall: r = 1.2; top = 2.0; break;
                            case PropType::Lamp: r = 0.7; top = 6.0; break;
                            default: continue;
                        }
                        const double dx = shot.eye.x - p.x, dy = shot.eye.y - p.y;
                        const double d = std::hypot(dx, dy);
                        if (d < r && shot.eye.z < p.z + top) {
                            const double ux = d > 1e-6 ? dx / d : 1.0, uy = d > 1e-6 ? dy / d : 0.0;
                            shot.eye.x = p.x + ux * (r + 0.4);
                            shot.eye.y = p.y + uy * (r + 0.4);
                        }
                    }
                }
            }
        }
    }

    [[nodiscard]] std::size_t cells_loaded() const { return cells_.size(); }
    [[nodiscard]] double cell_build_seconds() const { return cell_seconds_; }

private:
    // ---- Staging.
    void stage_props(const fs::path& data)
    {
        const auto prop = [&](const char* name) { return load_prop(*renderer_, data / "models" / name / (std::string(name) + ".gltf"), warnings_); };
        prop_models_[static_cast<std::size_t>(PropType::Barrier)] = prop("concrete_road_barrier_02");
        prop_models_[static_cast<std::size_t>(PropType::Lamp)] = prop("street_lamp_01");
        prop_models_[static_cast<std::size_t>(PropType::UtilityBox)] = prop("utility_box_01");
        prop_models_[static_cast<std::size_t>(PropType::RockLarge)] = prop("rock_07");
        prop_models_[static_cast<std::size_t>(PropType::RockSmall)] = prop("rock_09");
        prop_models_[static_cast<std::size_t>(PropType::ShrubA)] = prop("shrub_03");
        prop_models_[static_cast<std::size_t>(PropType::ShrubB)] = prop("shrub_sorrel_01");
        prop_models_[static_cast<std::size_t>(PropType::Barrel)] = prop("barrel_03");
        prop_models_[static_cast<std::size_t>(PropType::BarrelStack)] = prop("wooden_barrels_01");
        prop_models_[static_cast<std::size_t>(PropType::Bench)] = prop("painted_wooden_bench");
        const std::vector<render::GpuMaterial>& mats = materials_->level(0);
        for (unsigned i = 0; i < 3; ++i) {
            pines_.push_back(upload_model(*renderer_, "pine", tree_pine_meshes(i), mats));
            rounds_.push_back(upload_model(*renderer_, "round tree", tree_round_meshes(i), mats));
        }
        for (unsigned i = 0; i < 4; ++i) chunks_.push_back(upload_model(*renderer_, "chunk", chunk_meshes(i), mats));
        for (std::uint32_t i = 0; i < 6; ++i) puffs_[i] = upload_model(*renderer_, "puff", puff_meshes(ExtraDust0 + i), mats);
    }

    void stage_fx() {}

    void stage_car(const fs::path& data)
    {
        gfx::GltfLoadResult loaded = gfx::load_gltf_file(data / "khronos" / "CarConcept.glb");
        if (!loaded.ok()) throw std::runtime_error("CarConcept: " + loaded.error);
        gfx::GltfAsset& asset = *loaded.asset;
        gfx::StageSettings ss;
        ss.max_anisotropy = 4.0f;
        gfx::StageReport rep;
        const render::GpuModel full = gfx::stage_model(*renderer_, asset, "CarConcept", ss, &rep);
        std::printf("CarConcept: %zu instances, %zu placements, %u textures (%.1f MiB), %llu triangles\n", asset.instances.size(), full.placements.size(), rep.textures,
                    static_cast<double>(rep.texture_bytes) / 1048576.0, static_cast<unsigned long long>(full.triangle_count()));
        for (const std::string& w : rep.warnings) warnings_.push_back("CarConcept: " + w);
        // CarConcept is authored Y-up with the front along +Z, so the importer's (x, y, z) -> (z, x, y) already gives +X forward, +Y left, +Z up: no correction is needed.
        car_.fix = Mat4::identity();
        const std::size_t n = asset.instances.size();
        std::vector<bool> body(n, false), door_l(n, false), door_r(n, false), hood(n, false);
        const std::array<std::vector<bool>, 4> wheel = wheel_masks(asset);
        for (std::size_t i = 0; i < n; ++i) {
            const std::string& nm = asset.instances[i].name;
            const bool in_wheel = wheel[0][i] || wheel[1][i] || wheel[2][i] || wheel[3][i];
            if (in_wheel) continue;
            if (nm.rfind("BodyDoorL", 0) == 0 || nm.rfind("InteriorDoorL", 0) == 0) door_l[i] = true;
            else if (nm.rfind("BodyDoorR", 0) == 0 || nm.rfind("InteriorDoorR", 0) == 0) door_r[i] = true;
            else if (nm.rfind("BodyHood", 0) == 0) hood[i] = true;
            else body[i] = true;
        }
        if (full.placements.size() != n) {
            warnings_.push_back("CarConcept: placements and instances differ in count; the car is drawn rigid");
            car_.body = full;
            car_.ok = false;
            return;
        }
        // Bounds in the corrected frame.
        const auto bounds_of = [&](const std::vector<bool>& keep, Float3& lo, Float3& hi) {
            lo = {1e30f, 1e30f, 1e30f};
            hi = {-1e30f, -1e30f, -1e30f};
            for (std::size_t i = 0; i < n; ++i) {
                if (!keep[i] || asset.instances[i].mesh < 0) continue;
                for (const gfx::Primitive& p : asset.meshes[static_cast<std::size_t>(asset.instances[i].mesh)].primitives) {
                    if (!p.bounds.valid()) continue;
                    for (int c = 0; c < 8; ++c) {
                        const Float3 q{(c & 1) ? p.bounds.max.x : p.bounds.min.x, (c & 2) ? p.bounds.max.y : p.bounds.min.y, (c & 4) ? p.bounds.max.z : p.bounds.min.z};
                        const Float3 w = render::transform_point(car_.fix, render::transform_point(asset.instances[i].world, q));
                        lo = {std::min(lo.x, w.x), std::min(lo.y, w.y), std::min(lo.z, w.z)};
                        hi = {std::max(hi.x, w.x), std::max(hi.y, w.y), std::max(hi.z, w.z)};
                    }
                }
            }
        };
        std::vector<bool> everything(n, true);
        Float3 lo, hi;
        bounds_of(body, car_.body_lo, car_.body_hi);
        bounds_of(everything, lo, hi);
        std::printf("car body bounds (engine frame, model units): x %.2f..%.2f  y %.2f..%.2f  z %.2f..%.2f   whole: x %.2f..%.2f z %.2f..%.2f\n", static_cast<double>(car_.body_lo.x),
                    static_cast<double>(car_.body_hi.x), static_cast<double>(car_.body_lo.y), static_cast<double>(car_.body_hi.y), static_cast<double>(car_.body_lo.z),
                    static_cast<double>(car_.body_hi.z), static_cast<double>(lo.x), static_cast<double>(hi.x), static_cast<double>(lo.z), static_cast<double>(hi.z));
        // Each wheel is measured from its vertices, not from its node: the model's front wheels are authored with about 30 degrees of steer baked in (and every wheel with an arbitrary spin), so the node's
        // axes and bounds say nothing about where the axle is. Rolled about the car's +Y as it was, a front wheel wobbled. See wheel_rig.hpp and the horizon_wheel_rig test.
        for (std::size_t w = 0; w < 4; ++w) {
            wheel_fit_[w] = fit_wheel(wheel_points(asset, wheel[w]));
            car_.wheel_center[w] = wheel_fit_[w].center;
            car_.wheel_radius = wheel_fit_[w].radius;
            std::printf("wheel %zu: center (%.3f, %.3f, %.3f) radius %.3f half width %.3f; the model's axle is %.1f degrees off the car's +Y\n", w, static_cast<double>(car_.wheel_center[w].x),
                        static_cast<double>(car_.wheel_center[w].y), static_cast<double>(car_.wheel_center[w].z), static_cast<double>(car_.wheel_radius), static_cast<double>(wheel_fit_[w].half_width),
                        std::acos(static_cast<double>(wheel_fit_[w].axle.y)) * 180.0 / 3.14159265358979323846);
        }
        bounds_of(door_l, lo, hi);
        car_.door_l_hinge = {hi.x, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};  // the door's front edge: +X is forward, the hinge is at its front
        std::printf("left door x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f\n", static_cast<double>(lo.x), static_cast<double>(hi.x), static_cast<double>(lo.y), static_cast<double>(hi.y), static_cast<double>(lo.z), static_cast<double>(hi.z));
        car_.door_l_hinge = {hi.x, hi.y - 0.08f, lo.z};
        bounds_of(door_r, lo, hi);
        car_.door_r_hinge = {hi.x, lo.y, lo.z};
        bounds_of(hood, lo, hi);
        car_.hood_hinge = {lo.x, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
        // The staged parts, each pre-multiplied by the frame correction (and the wheels re-centered on their axle).
        car_.body = filter_placements(full, body, car_.fix);
        car_.door_l = filter_placements(full, door_l, car_.fix);
        car_.door_r = filter_placements(full, door_r, car_.fix);
        car_.hood = filter_placements(full, hood, car_.fix);
        for (std::size_t w = 0; w < 4; ++w) car_.wheels[w] = filter_placements(full, wheel[w], wheel_to_rig_frame(wheel_fit_[w]) * car_.fix);
        car_.ok = true;
    }

    void stage_character(const fs::path& data)
    {
        gfx::GltfLoadResult loaded = gfx::load_gltf_file(data / "khronos" / "CesiumMan.glb");
        if (!loaded.ok()) throw std::runtime_error("CesiumMan: " + loaded.error);
        man_asset_ = std::make_unique<gfx::GltfAsset>(std::move(*loaded.asset));
        gfx::StageSettings ss;
        gfx::StageReport rep;
        man_model_ = gfx::stage_model(*renderer_, *man_asset_, "CesiumMan", ss, &rep);
        for (const std::string& w : rep.warnings) warnings_.push_back("CesiumMan: " + w);
        std::printf("CesiumMan: %zu skins, %zu clips, %u skinned primitives, %u joints\n", man_asset_->skins.size(), man_asset_->animations.size(), rep.skinned_primitives, man_model_.skin_joint_count);
        if (man_asset_->skins.empty() || man_asset_->animations.empty()) throw std::runtime_error("CesiumMan has no skin or clip");
        palette_ = renderer_->model_renderer().create_skin_palette(man_model_.skin_joint_count, "man");
        binding_ = gfx::bind_clip(man_asset_->animations[0], man_asset_->skins[0]);
        // The standing pose: the moment of the walk cycle at which the feet are closest together (the clip has no idle of its own).
        {
            const gfx::Skin& skin = man_asset_->skins[0];
            const gfx::AnimationClip& clip = man_asset_->animations[0];
            const std::size_t jn = skin.joint_count();
            float best = 1e30f;
            for (int k = 0; k < 120; ++k) {
                const float tt = clip.duration * static_cast<float>(k) / 120.0f;
                gfx::Pose pose = gfx::rest_pose(skin);
                gfx::evaluate_clip(clip, binding_, tt, true, pose);
                const std::vector<Mat4> m = gfx::skin_matrices(skin, pose);
                const auto foot = [&](std::size_t j) {
                    const std::optional<Mat4> inv = render::inverse(skin.inverse_bind[j]);
                    const Float3 bind = inv ? render::transform_point(*inv, {0, 0, 0}) : Float3{};
                    return render::transform_point(m[j], bind);
                };
                const Float3 a = foot(jn - 1), b = foot(jn - 2);
                const Float3 d = a - b;
                const float sep = d.x * d.x + d.y * d.y + d.z * d.z;
                if (sep < best) {
                    best = sep;
                    idle_time_ = tt;
                }
            }
            std::printf("standing pose at clip time %.3f of %.3f\n", static_cast<double>(idle_time_), static_cast<double>(clip.duration));
        }
    }

    // ---- Cells.
    const CellGpu& cell(int cx, int cy)
    {
        const CellKey key{cx, cy};
        const auto it = cells_.find(key);
        if (it != cells_.end()) return *it->second;
        const auto t0 = std::chrono::steady_clock::now();
        const CellData data = world_.build_cell(cx, cy);
        auto gpu = std::make_unique<CellGpu>();
        gpu->ox = cx * cell_size;
        gpu->oy = cy * cell_size;
        gpu->props = data.props;
        const std::vector<MeshIn> meshes = cell_meshes(data.parts);
        for (std::uint32_t l = 0; l < emissive_levels; ++l) gpu->models[l] = upload_model(*renderer_, "cell", meshes, materials_->level(l));
        cell_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return *cells_.emplace(key, std::move(gpu)).first->second;
    }

    void trim_cells(int ax, int ay, int bx, int by, int keep)
    {
        for (auto it = cells_.begin(); it != cells_.end();) {
            const int cx = it->first.first, cy = it->first.second;
            const bool near_a = std::abs(cx - ax) <= keep && std::abs(cy - ay) <= keep, near_b = std::abs(cx - bx) <= keep && std::abs(cy - by) <= keep;
            if (!near_a && !near_b) it = cells_.erase(it);
            else ++it;
        }
    }

    // ---- Props.
    void add_prop(const PropInstance& p, Float3 pos)
    {
        if (p.scale <= 0.0f) return;
        const float yaw = p.yaw;
        const std::uint64_t h = hash64(static_cast<std::uint64_t>(std::llround(p.x * 4.0)), static_cast<std::uint64_t>(std::llround(p.y * 4.0)), 91);
        switch (p.type) {
            case PropType::TreePine:
                draws_.push_back({&pines_[h % pines_.size()], translation(pos) * rot_z(yaw) * scaling(p.scale)});
                return;
            case PropType::TreeRound:
                draws_.push_back({&rounds_[h % rounds_.size()], translation(pos) * rot_z(yaw) * scaling(p.scale)});
                return;
            default: break;
        }
        // How the model is turned and sized comes from the prop table, the same one that builds the collision hull.
        const PropSpec& spec = prop_spec(p.type);
        draws_.push_back({&prop_models_[static_cast<std::size_t>(p.type)], translation(pos) * rot_z(yaw + spec.yaw_offset) * scaling(p.scale * spec.scale)});
    }

    void add_body(const PropBody& b, D2 origin)
    {
        const double lx = b.x - origin.x, ly = b.y - origin.y;
        const float h = b.height;
        if (b.debris) {
            const std::size_t k = static_cast<std::size_t>(std::llround(std::fabs(b.x * 7.0 + b.y * 13.0))) % chunks_.size();
            Mat4 m = translation({static_cast<float>(lx), static_cast<float>(ly), static_cast<float>(b.z) + h * 0.5f}) * rot_z(b.yaw) * rot_y(b.pitch) * rot_x(b.roll) * scaling(b.scale * 1.6f);
            draws_.push_back({&chunks_[k], m});
            return;
        }
        float extra = 1.0f;
        if (b.type == PropType::Bench) extra = 0.7f;
        Mat4 m = translation({static_cast<float>(lx), static_cast<float>(ly), static_cast<float>(b.z) + h * 0.5f}) * rot_z(b.yaw) * rot_y(b.pitch) * rot_x(b.roll) * translation({0, 0, -h * 0.5f}) *
                 scaling(b.scale * extra);
        draws_.push_back({&prop_models_[static_cast<std::size_t>(b.type)], m});
    }

    // ---- The car.
    void add_car(const Sim& sim, D2 origin, std::uint32_t level, double t)
    {
        (void)origin;
        (void)level;
        (void)t;
        const Vehicle* v = sim.vehicle();
        if (v == nullptr) return;
        const VehicleState& st = v->state();
        const VehicleConfig& cfg = v->config();
        const Mat4 body_world = translation({st.position.x, st.position.y, st.position.z}) * from_quat(st.orientation);
        // The model's body origin sits at the chassis box center: the box is centered on the body's bounds in x and y, and its z puts the wheel centers at the rest height.
        const Float3 c = box_center_in_model();
        const Mat4 to_model_origin = translation({-c.x, -c.y, -c.z});
        const Mat4 car_m = body_world * to_model_origin;
        const float dmg = sim.damage();
        if (!car_.ok) {
            draws_.push_back({&car_.body, car_m});
            return;
        }
        Mat4 body_m = car_m;
        if (dmg > 0.0f) {
            // A crumpled nose: the body is shortened a little about the rear axle.
            const float squash = 1.0f - 0.06f * std::min(dmg, 1.0f);
            const Float3 pivot{car_.body_lo.x, 0, 0};
            body_m = car_m * translation(pivot) * scaling(squash, 1.0f, 1.0f - 0.02f * dmg) * translation({-pivot.x, 0, 0});
        }
        draws_.push_back({&car_.body, body_m});
        // The hood lifts at the front edge once the nose is hit hard.
        Mat4 hood_m = body_m;
        if (dmg > 0.15f) {
            const float a = 0.30f * std::min(dmg * 1.6f, 1.0f);
            hood_m = body_m * translation(car_.hood_hinge) * rot_y(-a) * translation({-car_.hood_hinge.x, -car_.hood_hinge.y, -car_.hood_hinge.z});
        }
        draws_.push_back({&car_.hood, hood_m});
        // Doors.
        const float door_angle = sim.door().angle;
        Mat4 dl = car_m * translation(car_.door_l_hinge) * rot_z(-door_angle) * translation({-car_.door_l_hinge.x, -car_.door_l_hinge.y, -car_.door_l_hinge.z});
        draws_.push_back({&car_.door_l, dl});
        draws_.push_back({&car_.door_r, car_m});
        // Wheels: the suspension and steering from the vehicle, the spin from the drive.
        for (std::size_t w = 0; w < 4; ++w) {
            const VehicleWheelConfig& wc = cfg.wheels[w];
            const VehicleWheelState& ws = st.wheels[w];
            const Float3 center_body{wc.mount.x, wc.mount.y, wc.mount.z - cfg.suspension_travel + ws.compression};
            Mat4 m = body_world * translation(center_body) * rot_z(ws.steer_angle) * wheel_spin(ws.spin);
            draws_.push_back({&car_.wheels[w], m});
        }
    }

    Float3 box_center_in_model() const
    {
        // x and y: the middle of the body; z: chosen so that the model's wheel centers sit where the suspension puts them at rest.
        const float cx = (car_.body_lo.x + car_.body_hi.x) * 0.5f, cy = (car_.body_lo.y + car_.body_hi.y) * 0.5f;
        float wz = 0;
        for (const Float3& w : car_.wheel_center) wz += w.z * 0.25f;
        const VehicleConfig cfg;  // geometry is read from the live vehicle in add_car; this is only the default rest estimate
        (void)cfg;
        return {cx, cy, wz - rest_wheel_z_};
    }

public:
    void set_rest_wheel_z(float z) { rest_wheel_z_ = z; }

private:
    // ---- The character.
    void add_character(const Sim& sim, D2 origin, double t)
    {
        (void)origin;
        (void)t;
        if (!sim.character_visible()) return;
        const gfx::AnimationClip& clip = man_asset_->animations[0];
        const gfx::Skin& skin = man_asset_->skins[0];
        gfx::Pose pose = gfx::rest_pose(skin);
        const float moving = sim.character_speed();
        const float cycle = sim.gait_phase();
        const float time = moving > 0.15f ? cycle * clip.duration : idle_time_;
        gfx::evaluate_clip(clip, binding_, time, true, pose);
        palette_.update(gfx::skin_matrices(skin, pose));
        const Vec3 feet = sim.character_feet();
        const float lean = std::min(moving / 8.0f, 1.0f) * 0.22f;
        Mat4 m = translation({feet.x, feet.y, feet.z}) * rot_z(sim.character_yaw()) * rot_y(lean) * rot_z(char_fix_yaw_);
        draws_.push_back({&man_model_, m, &palette_});
    }

    rhi::Device& device_;
    Options opt_;
    const horizon::World& world_;
    render::RendererSettings settings_;
    std::unique_ptr<render::SceneRenderer> renderer_;
    std::unique_ptr<MaterialLibrary> materials_;
    std::vector<std::string> warnings_;
    std::array<render::GpuModel, static_cast<std::size_t>(PropType::Count)> prop_models_;
    std::vector<render::GpuModel> pines_, rounds_, chunks_;
    std::array<render::GpuModel, 6> puffs_;
    CarParts car_;
    float rest_wheel_z_ = 0.0f;
    std::array<WheelFit, 4> wheel_fit_{};
    std::unique_ptr<gfx::GltfAsset> man_asset_;
    render::GpuModel man_model_;
    render::SkinPalette palette_;
    gfx::ClipBinding binding_;
    float char_fix_yaw_ = 0.0f;
    float idle_time_ = 0.0f;
    D2 far_origin_{};
    render::GpuModel far_model_;
    D2 skirt_origin_{};
    render::GpuModel skirt_model_;
    std::map<CellKey, std::unique_ptr<CellGpu>> cells_;
    double cell_seconds_ = 0.0;
    std::vector<render::ModelDraw> draws_;

public:
    void set_character_fix(float yaw) { char_fix_yaw_ = yaw; }
};

// A game-style camera boom: nothing solid (a wall, a canopy) may stand between the eye and what it looks at; the eye moves in along the sight line to just before it.
void avoid_occluders(Shot& shot, const Sim& sim)
{
    const D2 o = sim.origin();
    const V3 d = shot.eye - shot.look;
    const double dist = len(d);
    if (dist < 1.0) return;
    const double ex = shot.eye.x - o.x, ey = shot.eye.y - o.y, lx = shot.look.x - o.x, ly = shot.look.y - o.y;
    if (std::fabs(ex) > 240 || std::fabs(ey) > 240 || std::fabs(lx) > 240 || std::fabs(ly) > 240) return;
    const V3 u = d * (1.0 / dist);
    const auto hit = sim.collision().raycast({static_cast<float>(lx), static_cast<float>(ly), static_cast<float>(shot.look.z)}, {static_cast<float>(u.x), static_cast<float>(u.y), static_cast<float>(u.z)},
                                             static_cast<float>(dist));
    if (hit && !hit->started_inside) {
        const double nd = std::max(1.5, hit->distance - 0.4);
        shot.eye = shot.look + u * nd;
    }
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

} // namespace

int main(int argc, char** argv)
{
    const Options opt = parse(argc, argv);
    try {
        const horizon::World world;
        // Pass one: the whole run, recorded, so that the camera can look ahead.
        Track track;
        Sim::Timeline timeline;
        double crash_time = 0;
        D2 crash_point{};
        std::uint64_t run_hash = 0;
        {
            Sim sim(world);
            timeline = sim.timeline();
            track.push(capture_point(sim));
            while (sim.phase() != Phase::Done && sim.time() < 143.0) {
                sim.tick();
                track.push(capture_point(sim));
            }
            run_hash = sim.state_hash();
            timeline = sim.timeline();
            if (!sim.impacts().empty()) {
                const ImpactEvent& e = sim.impacts().front();
                crash_time = e.time;
                crash_point = {e.x, e.y};
            }
            std::printf("recorded %.1f s (hash %016llx); crash at %.2f s\n", track.duration(), static_cast<unsigned long long>(run_hash), crash_time);
        }
        if (timeline.end <= 0) timeline.end = track.duration();
        const Director director{world, track, timeline, crash_time, crash_point};

        std::string why;
        if (!rhi::vulkan_device_available(&why)) {
            std::printf("no Vulkan 1.3 device (%s); skipping rendering\n", why.c_str());
            return 77;
        }
        rhi::DeviceDesc dd;
        dd.enable_validation = opt.validate;
        dd.application_name = "SAIGE Showcase 03";
        std::unique_ptr<rhi::Device> device = rhi::create_vulkan_device(dd);
        const rhi::DeviceInfo info = device->info();
        std::printf("device: %s (%s), software=%d\n", info.device_name.c_str(), info.driver_name.c_str(), info.software_device ? 1 : 0);

        std::uint32_t frames = 0;
        double render_ms_total = 0, render_ms_max = 0;
        std::uint64_t draw_calls_max = 0;
        std::string first_shot_name;
        {
            const std::uint32_t rw = opt.width * opt.ssaa, rh = opt.height * opt.ssaa;
            Offscreen target(*device, rw, rh);
            Show show(*device, opt, world);
            // The car's rest geometry from the vehicle configuration: the wheels' rest height (mount - travel + the static compression).
            {
                Sim probe(world);
                if (const Vehicle* v = probe.vehicle()) {
                    const VehicleConfig& c = v->config();
                    const float rest = c.mass * 9.81f / (4.0f * c.spring);
                    show.set_rest_wheel_z(c.wheels[0].mount.z - c.suspension_travel + rest);
                    std::printf("vehicle: wheel radius %.3f, mount (%.2f, %.2f, %.2f), travel %.2f, static compression %.3f\n", static_cast<double>(c.wheel_radius), static_cast<double>(c.wheels[0].mount.x),
                                static_cast<double>(c.wheels[0].mount.y), static_cast<double>(c.wheels[0].mount.z), static_cast<double>(c.suspension_travel), static_cast<double>(rest));
                }
            }
            std::ofstream pipe;
            if (!opt.frames_pipe.empty()) {
                pipe.open(opt.frames_pipe, std::ios::binary);
                if (!pipe) throw std::runtime_error("cannot open the frame pipe");
            }
            Sim sim(world);
            const double end = opt.end > 0 ? opt.end : track.duration();
            std::vector<double> times;
            if (!opt.stills.empty()) {
                times = opt.stills;
                std::sort(times.begin(), times.end());
            } else {
                for (double t = opt.start; t < end - 1e-9; t += 1.0 / opt.fps) times.push_back(t);
            }
            for (const double t : times) {
                while (sim.time() + 1e-9 < t && sim.phase() != Phase::Done) sim.tick();
                Shot shot = director.shot(t);
                if (opt.cam) {
                    shot.eye = {(*opt.cam)[0], (*opt.cam)[1], (*opt.cam)[2]};
                    shot.look = {(*opt.cam)[3], (*opt.cam)[4], (*opt.cam)[5]};
                    shot.fov_deg = (*opt.cam)[6];
                    shot.name = "debug";
                }
                else {
                    show.nudge_from_props(shot);
                    if (shot.boom) avoid_occluders(shot, sim);
                }
                if (frames == 0) first_shot_name = shot.name;
                render::FrameStats stats;
                const auto t0 = std::chrono::steady_clock::now();
                Rgba img = show.render(target, sim, t, shot, frames, &stats);
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                render_ms_total += ms;
                render_ms_max = std::max(render_ms_max, ms);
                draw_calls_max = std::max<std::uint64_t>(draw_calls_max, stats.draw_calls);
                if (!opt.stills.empty()) {
                    char name[96];
                    std::snprintf(name, sizeof name, "still_%07.2f_%s.ppm", t, shot.name);
                    for (char* p = name; *p; ++p) if (*p == ' ') *p = '_';
                    if (!write_ppm(opt.out / name, img)) throw std::runtime_error("cannot write a still");
                    std::printf("t=%6.2f  %-18s %7.0f ms  draws %u  cells %zu\n", t, shot.name, ms, stats.draw_calls, show.cells_loaded());
                } else if (pipe.is_open()) {
                    if (!write_ppm(pipe, img)) throw std::runtime_error("the frame pipe closed");
                } else {
                    char name[64];
                    std::snprintf(name, sizeof name, "frame_%06u.ppm", frames);
                    if (!write_ppm(opt.out / "seq" / name, img)) throw std::runtime_error("cannot write a frame");
                }
                ++frames;
                if (opt.stills.empty() && frames % 30 == 0) std::printf("t=%6.2f s  frame %u  %-16s %6.0f ms\n", t, frames, shot.name, ms);
            }
            std::printf("rendered %u frames at %ux%u (ssaa %u) in %.1f s; cells built %zu in %.1f s\n", frames, opt.width, opt.height, opt.ssaa, render_ms_total / 1000.0, show.cells_loaded(),
                        show.cell_build_seconds());
            for (const std::string& w : show.warnings()) std::printf("note: %s\n", w.c_str());
            device->wait_idle();
        }
        device.reset();
        fs::create_directories(opt.out);
        std::ofstream rep(opt.out / "report.json");
        rep.imbue(std::locale::classic());
        rep << "{\n \"schema_version\": 1,\n \"showcase\": \"03-beyond-the-horizon\",\n";
        rep << " \"evidence_class\": \"" << (info.software_device ? "software_vulkan_cpu_rasterizer" : "physical_gpu_unqualified") << "\",\n";
        rep << " \"performance_claim\": \"none: offline capture, one frame at a time; the 60 FPS verdict stays not_evaluated\",\n";
        rep << " \"device\": {\"name\": \"" << json_escape(info.device_name) << "\", \"driver\": \"" << json_escape(info.driver_name) << "\", \"software\": " << (info.software_device ? "true" : "false") << "},\n";
        rep << " \"render\": {\"width\": " << opt.width << ", \"height\": " << opt.height << ", \"ssaa\": " << opt.ssaa << ", \"frames\": " << frames << ", \"distinct_fps\": " << opt.fps
            << ", \"mean_frame_ms\": " << (frames ? render_ms_total / frames : 0.0) << ", \"max_frame_ms\": " << render_ms_max << ", \"max_draw_calls\": " << draw_calls_max << "},\n";
        rep << " \"simulation\": {\"tick_hz\": 60, \"seconds\": " << track.duration() << ", \"state_hash\": \"" << std::hex << run_hash << std::dec << "\", \"crash_time\": " << crash_time << "}\n}\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "saige_horizon: " << e.what() << "\n";
        return 1;
    }
}
