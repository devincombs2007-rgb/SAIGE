// saige_anim_viewer: the humanoid animation module on the real renderer. A scripted run of the UAL character on a small course (a floor, a hill and a 1 m ledge): it idles, walks, sprints, rolls (root-motion
// action), crouch-walks (capsule crouch, UAL crouch set, stance blend), finds the ledge with find_ledge and climbs it (UAL 2's vertical root-motion action), all driven by Core's capsule character with
// the animation following the physics (locomotion from the capsule's real speed, FootPlacement on the ground, actions asking the capsule for their travel and reading back how far it got).
//
// Writes one PPM per captured frame under --out and a report.json (device, whether it was a software rasterizer, the stage of the script each frame is from). Nothing here is a mock-up: the images are
// read back from the renderer's output target. A software-Vulkan run is never evidence of GPU performance; this tool makes no performance claim.
//
// Usage: saige_anim_viewer [--assets DIR] [--out DIR] [--width N] [--height N] [--every TICKS] [--seconds S] [--self-check]
//   DIR is a directory filled by tools/animation/fetch_humanoid_assets.py (or SAIGE_ANIM_ASSETS). Exit 77: assets or Vulkan device not available.
#include "saige/animation/action.hpp"
#include "saige/animation/bvh.hpp"
#include "saige/animation/capsule_glue.hpp"
#include "saige/animation/layers.hpp"
#include "saige/animation/retarget.hpp"
#include "saige/gfx_assets/gltf.hpp"
#include "saige/gfx_scene/stage_model.hpp"
#include "saige/render/camera.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/render/transform.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace saige;
using namespace saige::anim;
using render::Float3;

namespace {

struct Options {
    fs::path assets;
    fs::path out = "anim-viewer-out";
    uint32_t width = 640, height = 360;
    int every = 6;
    float seconds = 17.0f;
    bool self_check = false;
};

[[noreturn]] void usage_error(const std::string& what) {
    std::fprintf(stderr, "saige_anim_viewer: %s\nusage: saige_anim_viewer [--assets DIR] [--out DIR] [--width N] [--height N] [--every TICKS] [--seconds S] [--self-check]\n", what.c_str());
    std::exit(2);
}

Options parse(int argc, char** argv) {
    Options o;
    if (const char* env = std::getenv("SAIGE_ANIM_ASSETS")) o.assets = env;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= argc) usage_error(a + " needs a value");
            return argv[++i];
        };
        if (a == "--assets") o.assets = value();
        else if (a == "--out") o.out = value();
        else if (a == "--width") o.width = static_cast<uint32_t>(std::stoul(value()));
        else if (a == "--height") o.height = static_cast<uint32_t>(std::stoul(value()));
        else if (a == "--every") o.every = std::stoi(value());
        else if (a == "--seconds") o.seconds = std::stof(value());
        else if (a == "--self-check") o.self_check = true;
        else usage_error("unknown argument " + a);
    }
    if (o.width < 64 || o.height < 64 || o.width > 4096 || o.height > 4096 || o.every < 1 || !(o.seconds > 0.5f)) usage_error("width/height must be 64..4096, every at least 1 and seconds above 0.5");
    return o;
}

const gfx::AnimationClip* find_clip(const gfx::GltfAsset& a, const char* name) {
    for (const gfx::AnimationClip& c : a.animations) {
        if (c.name == name) return &c;
    }
    return nullptr;
}

// ---- A box as an in-memory glTF (unit cube, flat colour), so the course needs no asset files.
std::string base64(const std::vector<uint8_t>& d) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < d.size(); i += 3) {
        const uint32_t v = (uint32_t{d[i]} << 16) | (i + 1 < d.size() ? uint32_t{d[i + 1]} << 8 : 0u) | (i + 2 < d.size() ? uint32_t{d[i + 2]} : 0u);
        s += t[(v >> 18) & 63];
        s += t[(v >> 12) & 63];
        s += i + 1 < d.size() ? t[(v >> 6) & 63] : '=';
        s += i + 2 < d.size() ? t[v & 63] : '=';
    }
    return s;
}

// A mesh given in engine axes (+X forward, +Y left, +Z up) as an in-memory glTF (flat colour). glTF is +Y up with its front along +Z: engine (x, y, z) is glTF (y, z, x).
std::string mesh_gltf(const std::vector<float>& pos, const std::vector<float>& nor, const std::vector<uint16_t>& idx, float r, float g, float b) {
    const size_t n = pos.size() / 3;
    std::vector<float> gp(pos.size()), gn(nor.size());
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i < n; ++i) {
        const float p[3] = {pos[i * 3 + 1], pos[i * 3 + 2], pos[i * 3]};
        const float q[3] = {nor[i * 3 + 1], nor[i * 3 + 2], nor[i * 3]};
        for (size_t c = 0; c < 3; ++c) {
            gp[i * 3 + c] = p[c];
            gn[i * 3 + c] = q[c];
            lo[c] = std::min(lo[c], p[c]);
            hi[c] = std::max(hi[c], p[c]);
        }
    }
    const size_t vb = n * 12, ib = idx.size() * 2;
    std::vector<uint8_t> bytes(vb * 2 + ib + (ib % 4 != 0 ? 4 - ib % 4 : 0));
    std::memcpy(bytes.data(), gp.data(), vb);
    std::memcpy(bytes.data() + vb, gn.data(), vb);
    std::memcpy(bytes.data() + vb * 2, idx.data(), ib);
    std::ostringstream j;
    j << R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
      << R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1},"indices":2,"material":0}]}],)"
      << R"("materials":[{"pbrMetallicRoughness":{"baseColorFactor":[)" << r << "," << g << "," << b << R"(,1],"metallicFactor":0.0,"roughnessFactor":0.85}}],)"
      << R"("accessors":[{"bufferView":0,"componentType":5126,"count":)" << n << R"(,"type":"VEC3","min":[)" << lo[0] << "," << lo[1] << "," << lo[2] << R"(],"max":[)" << hi[0] << "," << hi[1] << "," << hi[2] << R"(]},)"
      << R"({"bufferView":1,"componentType":5126,"count":)" << n << R"(,"type":"VEC3"},{"bufferView":2,"componentType":5123,"count":)" << idx.size() << R"(,"type":"SCALAR"}],)"
      << R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" << vb << R"(},{"buffer":0,"byteOffset":)" << vb << R"(,"byteLength":)" << vb << R"(},{"buffer":0,"byteOffset":)" << vb * 2 << R"(,"byteLength":)" << ib << R"(}],)"
      << R"("buffers":[{"byteLength":)" << bytes.size() << R"(,"uri":"data:application/octet-stream;base64,)" << base64(bytes) << R"("}]})";
    return j.str();
}

// A unit cube centered at the origin.
std::string box_gltf(float r, float g, float b) {
    struct Face {
        float n[3], u[3], v[3];
    };
    const Face faces[6] = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 0, 1}, {0, 1, 0}}, {{0, 1, 0}, {0, 0, 1}, {1, 0, 0}},
                           {{0, -1, 0}, {1, 0, 0}, {0, 0, 1}}, {{0, 0, 1}, {1, 0, 0}, {0, 1, 0}},  {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}};
    std::vector<float> pos, nor;
    std::vector<uint16_t> idx;
    for (const Face& f : faces) {
        const uint16_t base = static_cast<uint16_t>(pos.size() / 3);
        const float su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k) {
            for (int c = 0; c < 3; ++c) {
                pos.push_back(0.5f * f.n[c] + 0.5f * su[k] * f.u[c] + 0.5f * sv[k] * f.v[c]);
                nor.push_back(f.n[c]);
            }
        }
        for (const int i : {0, 1, 2, 0, 2, 3}) idx.push_back(static_cast<uint16_t>(base + i));
    }
    return mesh_gltf(pos, nor, idx, r, g, b);
}

// A wedge: x in [0, 1], y in [-0.5, 0.5], the top rising from z = 0 at x = 0 to z = 1 at x = 1 (scaled to a ramp by the draw's transform).
std::string wedge_gltf(float r, float g, float b) {
    std::vector<float> pos, nor;
    std::vector<uint16_t> idx;
    const auto quad = [&](const float p[4][3], const float n[3]) {
        const uint16_t base = static_cast<uint16_t>(pos.size() / 3);
        for (int k = 0; k < 4; ++k) {
            for (int c = 0; c < 3; ++c) {
                pos.push_back(p[k][c]);
                nor.push_back(n[c]);
            }
        }
        for (const int i : {0, 1, 2, 0, 2, 3}) idx.push_back(static_cast<uint16_t>(base + i));
    };
    const float inv = 0.70710678f;
    const float slope[4][3] = {{0, -0.5f, 0}, {1, -0.5f, 1}, {1, 0.5f, 1}, {0, 0.5f, 0}};  // counter-clockwise seen from above
    const float slope_n[3] = {-inv, 0, inv};
    quad(slope, slope_n);
    const float back[4][3] = {{1, -0.5f, 0}, {1, 0.5f, 0}, {1, 0.5f, 1}, {1, -0.5f, 1}};
    const float back_n[3] = {1, 0, 0};
    quad(back, back_n);
    for (const float side : {-1.0f, 1.0f}) {  // the two triangular ends, as degenerate quads (the last two vertices coincide)
        const float tri[4][3] = {{0, 0.5f * side, 0}, {1, 0.5f * side, 0}, {1, 0.5f * side, 1}, {1, 0.5f * side, 1}};
        const float n[3] = {0, side, 0};
        const uint16_t base = static_cast<uint16_t>(pos.size() / 3);
        for (int k = 0; k < 3; ++k) {
            for (int c = 0; c < 3; ++c) {
                pos.push_back(tri[k][c]);
                nor.push_back(n[c]);
            }
        }
        if (side > 0) for (const int i : {0, 1, 2}) idx.push_back(static_cast<uint16_t>(base + i));
        else for (const int i : {0, 2, 1}) idx.push_back(static_cast<uint16_t>(base + i));
    }
    return mesh_gltf(pos, nor, idx, r, g, b);
}

render::GpuModel stage_text(render::SceneRenderer& renderer, const char* name, const std::string& text) {
    const gfx::GltfLoadResult loaded = gfx::load_gltf(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()), {});
    if (!loaded.ok()) throw std::runtime_error(std::string("box ") + name + ": " + loaded.error);
    return gfx::stage_model(renderer, *loaded.asset, name);
}

render::GpuModel stage_box(render::SceneRenderer& renderer, const char* name, float r, float g, float b) { return stage_text(renderer, name, box_gltf(r, g, b)); }
render::GpuModel stage_wedge(render::SceneRenderer& renderer, const char* name, float r, float g, float b) { return stage_text(renderer, name, wedge_gltf(r, g, b)); }

struct Rgba {
    uint32_t width = 0, height = 0;
    std::vector<uint8_t> px;
};

bool write_ppm(const fs::path& path, const Rgba& img) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out << "P6\n" << img.width << " " << img.height << "\n255\n";
    std::vector<char> rgb(size_t{img.width} * img.height * 3);
    for (size_t i = 0; i < size_t{img.width} * img.height; ++i) std::memcpy(rgb.data() + i * 3, img.px.data() + i * 4, 3);
    out.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(out);
}

class Offscreen {
public:
    Offscreen(rhi::Device& device, uint32_t width, uint32_t height) : device_(device), width_(width), height_(height) {
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8_SRGB;
        td.width = width;
        td.height = height;
        td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
        td.debug_name = "ViewerTarget";
        color_ = device.create_texture(td);
        rhi::BufferDesc bd;
        bd.size = uint64_t{width} * height * 4;
        bd.usage = rhi::BufferUsage::TransferDst;
        bd.memory = rhi::MemoryLocation::GpuToCpu;
        bd.debug_name = "ViewerReadback";
        readback_ = device.create_buffer(bd);
    }
    ~Offscreen() {
        device_.destroy(readback_);
        device_.destroy(color_);
    }
    Offscreen(const Offscreen&) = delete;
    Offscreen& operator=(const Offscreen&) = delete;
    Rgba render(render::SceneRenderer& renderer, const render::SceneFrame& frame) {
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

double luma_stddev(const Rgba& img) {
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

double mean_abs_difference(const Rgba& a, const Rgba& b) {
    double d = 0;
    for (size_t i = 0; i < a.px.size(); ++i) d += std::fabs(static_cast<double>(a.px[i]) - static_cast<double>(b.px[i]));
    return d / static_cast<double>(a.px.size());
}

// ---- The animation rig: the UAL character with the locomotion set (UAL walk and sprint, CMU captures between them), the crouch set, and the actions.
struct Character {
    std::unique_ptr<gfx::GltfAsset> ual, ual2;
    const gfx::Skin* skin = nullptr;
    std::optional<HumanoidMap> map;
    RigInfo rig;
    std::unique_ptr<LocomotionSet> stand, crouch;
    std::unique_ptr<ActionClip> roll, climb;
    float climb_stand_distance = 0.65f;  // how far from the ledge's face the character must stand for the climb clip's first foot to meet it
    ~Character() {
        // The sets and actions point into the assets: release them first.
        climb.reset();
        roll.reset();
        crouch.reset();
        stand.reset();
    }
};

std::unique_ptr<gfx::GltfAsset> load_asset(const fs::path& file) {
    gfx::GltfLoadResult loaded = gfx::load_gltf_file(file);
    if (!loaded.ok()) throw std::runtime_error(file.string() + ": " + loaded.error);
    return std::make_unique<gfx::GltfAsset>(std::move(*loaded.asset));
}

void build_character(Character& c, const fs::path& root) {
    c.ual = load_asset(root / "ual" / "AnimationLibrary_Godot_Standard.glb");
    c.skin = &c.ual->skins.at(0);
    c.map = map_humanoid(*c.skin);
    if (!c.map) throw std::runtime_error("the UAL rig is not a humanoid");
    c.rig = analyze_rig(*c.skin, *c.map);
    c.stand = std::make_unique<LocomotionSet>(*c.skin, *c.map);
    for (const char* id : {"07_01", "07_12", "16_35", "16_45"}) {
        std::ifstream in(root / "cmu" / (std::string(id) + ".bvh"));
        if (!in) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        BvhOptions opt;
        opt.unit_scale = cmu_unit_scale;
        const BvhResult bvh = parse_bvh(ss.str(), opt);
        if (!bvh.ok()) continue;
        const auto smap = map_humanoid(*bvh.skin);
        if (!smap) continue;
        const RigInfo srig = analyze_rig(*bvh.skin, *smap);
        const LoopResult loop = extract_loop(*bvh.skin, *smap, srig, *bvh.clip);
        if (!loop.clip) continue;
        const gfx::Pose srest = gfx::rest_pose(*bvh.skin);
        c.stand->add_cycle(retarget_clip({&*bvh.skin, &*smap, &srig, &srest}, *loop.clip, *c.skin, *c.map, c.rig));
    }
    if (const gfx::AnimationClip* clip = find_clip(*c.ual, "Sprint_Loop")) c.stand->add_cycle(*clip);
    if (const gfx::AnimationClip* clip = find_clip(*c.ual, "Idle_Loop")) c.stand->set_idle(*clip);
    if (const gfx::AnimationClip* clip = find_clip(*c.ual, "Jump_Start")) c.stand->set_jump_start(*clip);
    if (const gfx::AnimationClip* clip = find_clip(*c.ual, "Jump_Loop")) c.stand->set_air(*clip);
    if (const gfx::AnimationClip* clip = find_clip(*c.ual, "Jump_Land")) c.stand->set_land(*clip);
    if (!c.stand->usable()) throw std::runtime_error("no locomotion cycle could be built");
    c.crouch = std::make_unique<LocomotionSet>(*c.skin, *c.map);
    const gfx::AnimationClip* cf = find_clip(*c.ual, "Crouch_Fwd_Loop");
    const gfx::AnimationClip* ci = find_clip(*c.ual, "Crouch_Idle_Loop");
    if (cf != nullptr && ci != nullptr && c.crouch->add_cycle(*cf)) c.crouch->set_idle(*ci);
    if (const gfx::AnimationClip* clip = find_clip(*c.ual, "Roll_RM")) c.roll = std::make_unique<ActionClip>(*c.skin, *c.map, c.rig, *clip);
    const fs::path ual2 = root / "ual2" / "UAL2_Standard.glb";
    if (fs::exists(ual2)) {
        c.ual2 = load_asset(ual2);
        const gfx::Skin& skin2 = c.ual2->skins.at(0);
        const auto map2 = map_humanoid(skin2);
        const gfx::AnimationClip* clip = find_clip(*c.ual2, "ClimbUp_1m_RM");
        if (map2 && clip != nullptr) {
            // UAL 2's rig has more joints than the UAL's: the climb is retargeted onto the UAL's rig (root motion scaled by the leg ratio, which is 1.0 here).
            const RigInfo rig2 = analyze_rig(skin2, *map2);
            const gfx::Pose rest2 = gfx::rest_pose(skin2);
            const gfx::AnimationClip moved = retarget_clip({&skin2, &*map2, &rig2, &rest2}, *clip, *c.skin, *c.map, c.rig);
            c.climb = std::make_unique<ActionClip>(*c.skin, *c.map, c.rig, moved, 60.0f, true);
            // Where the clip's first foot reaches the ledge's height, ahead of the hips' start: the character stands that far (less the length of the foot) from the face.
            const gfx::ClipBinding bind = gfx::bind_clip(c.climb->clip(), *c.skin);
            gfx::Pose pose = gfx::rest_pose(*c.skin);
            gfx::evaluate_clip(c.climb->clip(), bind, 0.0f, false, pose);
            const Float3 start = joint_world(*c.skin, pose)[static_cast<std::size_t>((*c.map)[Bone::Hips])].position;
            const float floor_z = std::min(joint_world(*c.skin, pose)[static_cast<std::size_t>((*c.map)[Bone::LeftToes])].position.z, joint_world(*c.skin, pose)[static_cast<std::size_t>((*c.map)[Bone::RightToes])].position.z);
            for (int k = 0; k <= 200; ++k) {
                gfx::evaluate_clip(c.climb->clip(), bind, c.climb->duration() * static_cast<float>(k) / 200.0f, false, pose);
                const std::vector<JointWorld> w = joint_world(*c.skin, pose);
                bool found = false;
                for (const Bone toes : {Bone::LeftToes, Bone::RightToes}) {
                    const Float3 t = w[static_cast<std::size_t>((*c.map)[toes])].position;
                    if (t.z - floor_z > 0.9f) {
                        c.climb_stand_distance = t.x - start.x - 0.12f;
                        found = true;
                    }
                }
                if (found) break;
            }
        }
    }
}

const char* stage_name(int s) {
    static const char* n[] = {"idle", "walk", "sprint", "roll", "crouch walk", "stand up", "approach ledge", "climb", "on the ledge", "jump"};
    return n[s];
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options opt = parse(argc, argv);
        if (opt.assets.empty() || !fs::exists(opt.assets / "ual" / "AnimationLibrary_Godot_Standard.glb") || !fs::exists(opt.assets / "cmu")) {
            std::printf("humanoid assets not available (tools/animation/fetch_humanoid_assets.py DIR; pass --assets DIR or set SAIGE_ANIM_ASSETS): skipped\n");
            return 77;
        }
        std::string why;
        if (!rhi::vulkan_device_available(&why)) {
            std::printf("no Vulkan 1.3 device (%s); skipping rendering\n", why.c_str());
            return 77;
        }
        Character ch;
        build_character(ch, opt.assets);
        std::printf("locomotion cycles:");
        for (const LocomotionCycle& cy : ch.stand->cycles()) std::printf(" %.2f", static_cast<double>(cy.info.ground_speed));
        std::printf(" m/s; crouch %s; roll %s; climb %s (stand %.2f m from the face)\n", ch.crouch->usable() ? "yes" : "no", ch.roll ? "yes" : "no", ch.climb ? "yes" : "no (needs UAL 2: SAIGE_ANIM_ASSETS/ual2)", static_cast<double>(ch.climb_stand_distance));

        rhi::DeviceDesc dd;
        dd.application_name = "SAIGE animation viewer";
        std::unique_ptr<rhi::Device> device = rhi::create_vulkan_device(dd);
        const rhi::DeviceInfo info = device->info();
        std::printf("device: %s (%s), software=%d\n", info.device_name.c_str(), info.driver_name.c_str(), info.software_device ? 1 : 0);

        render::RendererSettings rs;
        rs.camera_mode = render::CameraMode::Fixed;
        rs.hdr = true;
        rs.model_shading = render::ModelShading::MetallicRoughness;
        rs.skinning = true;
        rs.dynamic_lighting = true;
        rs.ground_half_size = 0.001f;
        rs.sky = {0.46f, 0.62f, 0.85f, 1.0f};
        rs.lighting.sun_direction = {-0.45f, -0.55f, 0.7f};
        rs.lighting.sun_color = {2.2f, 1.95f, 1.7f};
        rs.lighting.ambient_ground = {0.22f, 0.20f, 0.18f};
        rs.tone_map.exposure_ev = -1.0f;
        rs.shadows.enabled = true;
        rs.shadows.cascades = 1;
        rs.shadows.map_size = 1024;
        rs.shadows.distance = 40.0f;
        rs.shadows.filter_radius = 1;

        int exit_code = 0;
        {
            Offscreen target(*device, opt.width, opt.height);
            render::SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, rs);
            gfx::StageSettings ss;
            gfx::StageReport rep;
            render::GpuModel man = gfx::stage_model(renderer, *ch.ual, "UAL", ss, &rep);
            if (man.skin_joint_count == 0) throw std::runtime_error("the UAL mesh has no skin");
            render::SkinPalette palette = renderer.model_renderer().create_skin_palette(man.skin_joint_count, "man");
            const render::GpuModel floor_model = stage_box(renderer, "floor", 0.20f, 0.27f, 0.18f);
            const render::GpuModel ledge_model = stage_box(renderer, "ledge", 0.42f, 0.30f, 0.22f);
            const render::GpuModel marker_model = stage_box(renderer, "marker", 0.85f, 0.80f, 0.70f);
            const render::GpuModel hill_model = stage_wedge(renderer, "hill", 0.34f, 0.40f, 0.26f);

            // The course: a floor, a ledge 1 m high whose face is at x = 46, and a row of posts every 5 m to read the speed from.
            const float ledge_x = 46.0f;
            std::vector<Aabb> boxes{{{-60, -20, -1}, {120, 20, 0}}, {{ledge_x, -20, 0}, {120, 20, 1.0f}}};
            // A hill of two 12 degree slopes between x = 31 and 43 (walked over before the ledge): the ground IK has something to do.
            const float hill_x0 = 31.0f, hill_len = 6.0f, hill_rise = hill_len * std::tan(12.0f * 3.14159265f / 180.0f);
            std::vector<Triangle> tris{{{hill_x0, -20, 0}, {hill_x0, 20, 0}, {hill_x0 + hill_len, 20, hill_rise}},
                                       {{hill_x0, -20, 0}, {hill_x0 + hill_len, 20, hill_rise}, {hill_x0 + hill_len, -20, hill_rise}},
                                       {{hill_x0 + hill_len, -20, hill_rise}, {hill_x0 + hill_len, 20, hill_rise}, {hill_x0 + 2 * hill_len, 20, 0}},
                                       {{hill_x0 + hill_len, -20, hill_rise}, {hill_x0 + 2 * hill_len, 20, 0}, {hill_x0 + 2 * hill_len, -20, 0}}};
            const StaticCollision world(boxes, tris);
            CapsuleCharacterConfig cfg;
            cfg.crouch_height = crouch_capsule_height(1.0f, 1.5f, cfg.height, 2.0f * cfg.radius);  // from the crouch pose's head (about 0.94 m) and the standing head (1.52 m)
            cfg.crouch_speed = 1.4f;
            CapsuleCharacter capsule({0.0f, 0.0f, 0.0f}, boxes, tris, cfg);
            LocomotionController stand_ctl(*ch.stand, {});
            std::optional<LocomotionController> crouch_ctl;
            if (ch.crouch->usable()) crouch_ctl.emplace(*ch.crouch, LocomotionConfig{});
            StanceBlend stance(0.2f);
            FootPlacement feet;
            const GroundQuery ground = ground_query(world);
            const ObstacleQuery obstacle = obstacle_query(world);
            ActionPlayer action;
            bool climbing = false;
            int stage = 0;
            float stage_time = 0.0f;
            bool roll_done = false, climb_started = false, on_ledge = false;
            float roll_end = 1e9f;
            float final_height = 0.0f;

            const auto draw_course = [&](std::vector<render::ModelDraw>& draws) {
                const auto box = [&](const render::GpuModel& m, Float3 center, Float3 size) {
                    render::Transform t;
                    t.position = center;
                    t.scale = size;
                    draws.push_back({&m, render::to_matrix(t)});
                };
                box(floor_model, {30, 0, -0.5f}, {180, 40, 1.0f});
                box(ledge_model, {ledge_x + 45.0f, 0, 0.5f}, {90, 40, 1.0f});
                {
                    render::Transform up;  // the rising half: x from hill_x0 to hill_x0 + hill_len (the wedge's origin is its base start), 40 m wide
                    up.position = {hill_x0, 0, 0};
                    up.scale = {hill_len, 40.0f, hill_rise};
                    draws.push_back({&hill_model, render::to_matrix(up)});
                    render::Transform down = up;  // the falling half: the same wedge turned half a turn about the vertical
                    down.position = {hill_x0 + 2.0f * hill_len, 0, 0};
                    down.rotation = render::from_axis_angle({0, 0, 1}, 3.14159265f);
                    draws.push_back({&hill_model, render::to_matrix(down)});
                }
                for (int p = -10; p <= 24; ++p) box(marker_model, {static_cast<float>(p) * 5.0f, 3.2f, 0.35f}, {0.25f, 0.25f, 0.7f});
            };

            fs::create_directories(opt.out);
            std::vector<Rgba> frames;
            std::vector<std::string> stage_of;
            std::vector<double> times;
            const float dt = 1.0f / 60.0f;
            const std::chrono::nanoseconds tick{16666667};
            const int total = static_cast<int>(opt.seconds * 60.0f);
            float last_yaw = 0.0f;
            for (int i = 0; i < total; ++i) {
                const float t = static_cast<float>(i) * dt;
                stage_time += dt;
                CapsuleInput in;
                const CapsuleCharacterState& s = capsule.state();
                // The script: stand, walk, sprint, roll, crouch-walk, stand, approach the ledge, climb it, walk on.
                if (on_ledge) {
                    stage = 8;
                } else if (!climbing) {
                    if (t < 0.8f) stage = 0;
                    else if (t < 2.0f) stage = 1;
                    else if (t < 3.2f) stage = 2;
                    else if (t < 4.4f) stage = 9;  // a jump from the sprint, then the landing
                    else if (!roll_done && ch.roll) stage = 3;
                    else if (t < roll_end + 1.5f) stage = 4;
                    else if (t < roll_end + 2.0f) stage = 5;
                    else stage = 6;
                }
                if (stage == 9) {
                    in.forward = 1.0f;
                    in.sprint = true;
                    in.jump = i == static_cast<int>(3.2f * 60.0f);
                } else if (stage == 1 || stage == 2) {
                    in.forward = 1.0f;
                    in.sprint = stage == 2;
                } else if (stage == 4) {
                    in.forward = 1.0f;
                    in.crouch = true;
                } else if (stage == 5) {
                    in.forward = 1.0f;
                }
                if (stage == 3 && !action.active() && !roll_done) action.start(*ch.roll);
                if (stage == 8) in.forward = 1.0f;
                if (stage == 6 && !climbing) {
                    const LedgeInfo ledge = find_ledge(world, s.feet, 0.0f);
                    const float stand_distance = ch.climb_stand_distance;  // where the clip's first foot meets the ledge's face
                    if (ledge.found && ledge.distance > stand_distance + 0.05f) {
                        in.forward = 0.55f;
                    } else if (ledge.found && ch.climb && !climb_started) {
                        action.start(*ch.climb);
                        climbing = true;
                        climb_started = true;
                        stage = 7;
                    } else if (!ledge.found) {
                        in.forward = 1.0f;
                    }
                }
                const bool acting = action.active();
                if (acting) {
                    in.drive = capsule_drive(action, dt, last_yaw);
                    in.jump = false;
                    in.sprint = false;
                }
                const Vec3 before = s.feet;
                capsule.step(in, tick);
                const CapsuleCharacterState& after = capsule.state();
                final_height = after.feet.z;
                if (acting) {
                    if (!after.grounded && !action.vertical()) action.cancel();
                    action.advance(dt, drive_progress(before, after.feet, in.drive, dt));
                    if (!action.active()) {
                        if (stage == 3) {
                            roll_done = true;
                            roll_end = t;
                            stage_time = 0.0f;
                        }
                        if (climbing) {
                            climbing = false;
                            on_ledge = true;
                            stage = 8;
                        }
                    }
                }
                // The pose: locomotion from the capsule's real speed, the stance blend, the action over it, then the feet.
                stand_ctl.update(dt, locomotion_input(after));
                gfx::Pose pose = stand_ctl.pose();
                stance.update(dt, after.crouched);
                if (crouch_ctl) {
                    LocomotionInput low = locomotion_input(after);
                    crouch_ctl->update(dt, low);
                    if (stance.weight() > 0.0f) blend_into(pose, crouch_ctl->pose(), stance.weight());
                }
                const render::Transform root{{after.feet.x, after.feet.y, after.feet.z}, render::from_axis_angle({0, 0, 1}, last_yaw), {1, 1, 1}};
                const bool locomoting = !action.active();
                if (action.active()) blend_into(pose, action.pose(), action.weight());
                (void)feet.apply(*ch.skin, *ch.map, ch.rig, pose, root, locomoting ? &ground : nullptr, &obstacle, after.grounded && locomoting && stance.weight() < 0.5f, dt);
                palette.update(gfx::skin_matrices(*ch.skin, pose));

                if (i % opt.every == 0) {
                    std::vector<render::ModelDraw> draws;
                    draw_course(draws);
                    draws.push_back({&man, render::to_matrix(root), &palette});
                    const Float3 focus{after.feet.x + 1.4f, 0.0f, after.feet.z + 0.95f};
                    const render::Camera cam = render::look_at_camera({focus.x - 0.3f, -4.6f, focus.z + 0.7f}, focus);
                    render::SceneFrame frame;
                    frame.tick = static_cast<uint64_t>(i);
                    frame.camera = &cam;
                    frame.models = draws;
                    Rgba img = target.render(renderer, frame);
                    char name[64];
                    std::snprintf(name, sizeof name, "frame_%04zu.ppm", frames.size());
                    if (!write_ppm(opt.out / name, img)) throw std::runtime_error("cannot write a frame");
                    frames.push_back(std::move(img));
                    stage_of.push_back(stage_name(stage));
                    times.push_back(static_cast<double>(t));
                }
            }
            std::ofstream rep_out(opt.out / "report.json");
            rep_out << "{\n  \"device\": \"" << info.device_name << "\", \"software_rasterizer\": " << (info.software_device ? "true" : "false") << ",\n  \"note\": \"images are read back from the renderer; a software run is never evidence of GPU performance\",\n  \"frames\": [\n";
            for (size_t k = 0; k < frames.size(); ++k) rep_out << "    {\"file\": \"frame_" << std::string(4 - std::min<size_t>(4, std::to_string(k).size()), '0') << k << ".ppm\", \"time_s\": " << times[k] << ", \"stage\": \"" << stage_of[k] << "\"}" << (k + 1 < frames.size() ? "," : "") << "\n";
            rep_out << "  ]\n}\n";
            std::printf("rendered %zu frames to %s\n", frames.size(), opt.out.string().c_str());
            if (opt.self_check) {
                double biggest_change = 0.0, lowest_detail = 1e9;
                for (size_t k = 0; k < frames.size(); ++k) {
                    lowest_detail = std::min(lowest_detail, luma_stddev(frames[k]));
                    if (k > 0) biggest_change = std::max(biggest_change, mean_abs_difference(frames[k - 1], frames[k]));
                }
                std::printf("self-check: %zu frames, lowest luma deviation %.1f, largest change between frames %.2f\n", frames.size(), lowest_detail, biggest_change);
                if (frames.size() < 3 || lowest_detail < 3.0 || biggest_change < 0.5) {
                    std::fprintf(stderr, "self-check failed: a frame is empty or the picture never changes\n");
                    exit_code = 1;
                }
                // The script must have done what it says: with the climb available and time for it, the character ends on the 1 m ledge.
                if (ch.climb && opt.seconds >= 16.0f) {
                    std::printf("self-check: the character ends at height %.2f m (the ledge is 1.00 m)\n", static_cast<double>(final_height));
                    if (std::fabs(final_height - 1.0f) > 0.05f) {
                        std::fprintf(stderr, "self-check failed: the character did not end up on the ledge\n");
                        exit_code = 1;
                    }
                }
            }
        }
        return exit_code;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "saige_anim_viewer: %s\n", e.what());
        return 1;
    }
}
