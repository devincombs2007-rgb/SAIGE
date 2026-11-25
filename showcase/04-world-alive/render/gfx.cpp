#include "gfx.hpp"

#include "saige/gfx_assets/tangents.hpp"
#include "saige/gfx_scene/stage_model.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace saige::alive::rend {

// ---- matrices --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
Mat4 translation(Float3 t) {
    Mat4 m = Mat4::identity();
    m.m[12] = t.x;
    m.m[13] = t.y;
    m.m[14] = t.z;
    return m;
}
Mat4 rot_x(float a) { return render::to_mat4(render::from_axis_angle({1, 0, 0}, a)); }
Mat4 rot_y(float a) { return render::to_mat4(render::from_axis_angle({0, 1, 0}, a)); }
Mat4 rot_z(float a) { return render::to_mat4(render::from_axis_angle({0, 0, 1}, a)); }
Mat4 scaling(float s) { return scaling(s, s, s); }
Mat4 scaling(float x, float y, float z) {
    Mat4 m = Mat4::identity();
    m.m[0] = x;
    m.m[5] = y;
    m.m[10] = z;
    return m;
}
Mat4 from_quat(float x, float y, float z, float w) { return render::to_mat4(render::normalize(render::Quat{x, y, z, w})); }

// ---- readback --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
Offscreen::Offscreen(rhi::Device& device, std::uint32_t width, std::uint32_t height) : device_(device), width_(width), height_(height) {
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_SRGB;
    td.width = width;
    td.height = height;
    td.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSrc;
    td.debug_name = "AliveTarget";
    color_ = device.create_texture(td);
    rhi::BufferDesc bd;
    bd.size = std::uint64_t{width} * height * 4;
    bd.usage = rhi::BufferUsage::TransferDst;
    bd.memory = rhi::MemoryLocation::GpuToCpu;
    bd.debug_name = "AliveReadback";
    readback_ = device.create_buffer(bd);
}
Offscreen::~Offscreen() {
    device_.destroy(readback_);
    device_.destroy(color_);
}
Rgba Offscreen::render(render::SceneRenderer& renderer, const render::SceneFrame& frame, render::FrameStats* stats) {
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

Rgba downsample(const Rgba& src, std::uint32_t factor) {
    if (factor <= 1) return src;
    Rgba out;
    out.width = src.width / factor;
    out.height = src.height / factor;
    out.px.resize(std::size_t{out.width} * out.height * 4);
    for (std::uint32_t y = 0; y < out.height; ++y)
        for (std::uint32_t x = 0; x < out.width; ++x) {
            std::uint32_t sum[4] = {0, 0, 0, 0};
            for (std::uint32_t dy = 0; dy < factor; ++dy)
                for (std::uint32_t dx = 0; dx < factor; ++dx) {
                    const std::uint8_t* p = &src.px[(std::size_t{y * factor + dy} * src.width + x * factor + dx) * 4];
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                }
            for (int c = 0; c < 4; ++c) out.px[(std::size_t{y} * out.width + x) * 4 + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>((sum[c] + factor * factor / 2) / (factor * factor));
        }
    return out;
}

bool write_ppm(std::ostream& out, const Rgba& img) {
    out << "P6\n" << img.width << " " << img.height << "\n255\n";
    std::vector<char> rgb(std::size_t{img.width} * img.height * 3);
    for (std::size_t i = 0; i < std::size_t{img.width} * img.height; ++i) std::memcpy(rgb.data() + i * 3, img.px.data() + i * 4, 3);
    out.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(out);
}
bool write_ppm(const fs::path& path, const Rgba& img) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    return out && write_ppm(out, img);
}

// ---- materials -------------------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace {

struct MatDef {
    const char* texture;     // Poly Haven folder, or null for a plain material
    float color[4];          // base colour factor (linear), alpha for blended materials
    float metallic, roughness;
    float emissive[3];
    float tile_m;            // metres per texture repeat
    bool double_sided;
    bool blend;
    float normal_scale;
};

constexpr MatDef defs[MCount] = {
    /* MAsphalt      */ {"asphalt_04", {0.80f, 0.80f, 0.82f, 1}, 1, 1, {0, 0, 0}, 4.0f, false, false, 1.0f},
    /* MSidewalk     */ {"concrete_pavement", {0.92f, 0.92f, 0.92f, 1}, 1, 1, {0, 0, 0}, 2.5f, false, false, 1.0f},
    /* MCurb         */ {"concrete_floor", {0.78f, 0.78f, 0.78f, 1}, 1, 1, {0, 0, 0}, 2.0f, false, false, 1.0f},
    /* MPromenade    */ {"stone_tiles_02", {0.92f, 0.90f, 0.86f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MPaving       */ {"large_floor_tiles_02", {0.90f, 0.88f, 0.84f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MPier         */ {"weathered_brown_planks", {0.85f, 0.80f, 0.75f, 1}, 1, 1, {0, 0, 0}, 2.0f, false, false, 1.0f},
    /* MGrass        */ {"leafy_grass", {0.62f, 0.68f, 0.50f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MBrickA       */ {"red_brick_03", {0.95f, 0.90f, 0.88f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MBrickB       */ {"red_bricks_04", {0.95f, 0.92f, 0.90f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MBrickC       */ {"brick_wall_001", {0.95f, 0.90f, 0.88f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MBrickPattern */ {"patterned_brick_wall_03", {0.95f, 0.93f, 0.90f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MPlasterA     */ {"beige_wall_001", {0.98f, 0.95f, 0.88f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MPlasterB     */ {"white_plaster_02", {0.96f, 0.96f, 0.94f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MPlasterC     */ {"painted_plaster_wall", {0.90f, 0.82f, 0.70f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MStucco       */ {"white_stucco", {0.95f, 0.92f, 0.86f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MCladdingA    */ {"exterior_wall_cladding_02", {0.90f, 0.90f, 0.92f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MCladdingB    */ {"exterior_wall_cladding_03", {0.92f, 0.90f, 0.86f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MConcreteTile */ {"concrete_tile_facade", {0.88f, 0.88f, 0.88f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MFactory      */ {"factory_wall", {0.85f, 0.88f, 0.90f, 1}, 1, 1, {0, 0, 0}, 3.0f, false, false, 1.0f},
    /* MRoofSlate    */ {"roof_slates_03", {0.85f, 0.85f, 0.88f, 1}, 1, 1, {0, 0, 0}, 3.0f, true, false, 1.0f},
    /* MRoofGrey     */ {"grey_roof_01", {0.85f, 0.85f, 0.85f, 1}, 1, 1, {0, 0, 0}, 3.0f, true, false, 1.0f},
    /* MRoofClay     */ {"clay_roof_tiles_03", {0.95f, 0.85f, 0.80f, 1}, 1, 1, {0, 0, 0}, 3.0f, true, false, 1.0f},
    /* MRoofMetal    */ {"corrugated_iron", {0.80f, 0.82f, 0.85f, 1}, 1, 1, {0, 0, 0}, 3.0f, true, false, 1.0f},
    /* MFloorWood    */ {"oak_wood_planks", {0.95f, 0.90f, 0.85f, 1}, 1, 1, {0, 0, 0}, 2.0f, false, false, 1.0f},
    /* MFloorWoodDark*/ {"wood_planks", {0.78f, 0.70f, 0.62f, 1}, 1, 1, {0, 0, 0}, 2.0f, false, false, 1.0f},
    /* MFloorTile    */ {"floor_tiles_02", {0.95f, 0.95f, 0.95f, 1}, 1, 1, {0, 0, 0}, 2.0f, false, false, 1.0f},
    /* MMetalPlate   */ {"metal_plate", {0.85f, 0.85f, 0.88f, 1}, 1, 1, {0, 0, 0}, 2.0f, false, false, 1.0f},
    /* MBark         */ {"bark_brown_01", {0.85f, 0.80f, 0.75f, 1}, 1, 1, {0, 0, 0}, 2.0f, false, false, 1.0f},
    /* MFoliage      */ {"leafy_grass", {0.55f, 0.80f, 0.32f, 1}, 1, 1, {0, 0, 0}, 1.2f, true, false, 0.6f},
    /* MFoliageDark  */ {"leafy_grass", {0.34f, 0.55f, 0.24f, 1}, 1, 1, {0, 0, 0}, 1.2f, true, false, 0.6f},
    /* MGlassDark    */ {nullptr, {0.02f, 0.03f, 0.045f, 1}, 0.7f, 0.06f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MGlassLit     */ {nullptr, {0.10f, 0.10f, 0.12f, 1}, 0.0f, 0.15f, {1.45f, 0.95f, 0.48f}, 1, true, false, 1.0f},
    /* MGlassLitCool */ {nullptr, {0.10f, 0.10f, 0.12f, 1}, 0.0f, 0.15f, {0.85f, 1.05f, 1.35f}, 1, true, false, 1.0f},
    /* MDoorRed      */ {nullptr, {0.30f, 0.045f, 0.035f, 1}, 0.0f, 0.45f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MDoorGreen    */ {nullptr, {0.05f, 0.18f, 0.10f, 1}, 0.0f, 0.45f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MDoorBlue     */ {nullptr, {0.05f, 0.10f, 0.28f, 1}, 0.0f, 0.45f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MDoorWood     */ {nullptr, {0.30f, 0.18f, 0.09f, 1}, 0.0f, 0.55f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MFrameWhite   */ {nullptr, {0.80f, 0.80f, 0.78f, 1}, 0.0f, 0.5f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MFrameDark    */ {nullptr, {0.07f, 0.07f, 0.08f, 1}, 0.2f, 0.45f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MAwningRed    */ {nullptr, {0.55f, 0.07f, 0.06f, 1}, 0.0f, 0.85f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MAwningGreen  */ {nullptr, {0.06f, 0.30f, 0.15f, 1}, 0.0f, 0.85f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MAwningBlue   */ {nullptr, {0.06f, 0.14f, 0.45f, 1}, 0.0f, 0.85f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MAwningYellow */ {nullptr, {0.65f, 0.48f, 0.08f, 1}, 0.0f, 0.85f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MMarkWhite    */ {nullptr, {0.62f, 0.62f, 0.60f, 1}, 0.0f, 0.55f, {0, 0, 0}, 1, false, false, 1.0f},
    /* MMarkYellow   */ {nullptr, {0.70f, 0.52f, 0.06f, 1}, 0.0f, 0.55f, {0, 0, 0}, 1, false, false, 1.0f},
    /* MPaintSteel   */ {nullptr, {0.30f, 0.32f, 0.34f, 1}, 0.8f, 0.4f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MPaintDarkGrey*/ {nullptr, {0.10f, 0.11f, 0.12f, 1}, 0.4f, 0.5f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MPaintBlack   */ {nullptr, {0.02f, 0.02f, 0.02f, 1}, 0.0f, 0.6f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MPaintWhite   */ {nullptr, {0.75f, 0.75f, 0.73f, 1}, 0.0f, 0.5f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MSignalRed    */ {nullptr, {0.05f, 0.01f, 0.01f, 1}, 0.0f, 0.3f, {7.0f, 0.4f, 0.3f}, 1, true, false, 1.0f},
    /* MSignalAmber  */ {nullptr, {0.05f, 0.03f, 0.0f, 1}, 0.0f, 0.3f, {7.0f, 3.5f, 0.2f}, 1, true, false, 1.0f},
    /* MSignalGreen  */ {nullptr, {0.01f, 0.05f, 0.02f, 1}, 0.0f, 0.3f, {0.3f, 6.0f, 1.2f}, 1, true, false, 1.0f},
    /* MSignalWalk   */ {nullptr, {0.04f, 0.05f, 0.05f, 1}, 0.0f, 0.3f, {1.2f, 5.0f, 5.5f}, 1, true, false, 1.0f},
    /* MSignalOff    */ {nullptr, {0.02f, 0.02f, 0.02f, 1}, 0.0f, 0.3f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MLampGlow     */ {nullptr, {0.10f, 0.10f, 0.10f, 1}, 0.0f, 0.3f, {9.0f, 7.0f, 4.0f}, 1, true, false, 1.0f},
    /* MBulbWarm     */ {nullptr, {0.10f, 0.10f, 0.10f, 1}, 0.0f, 0.3f, {5.0f, 3.2f, 1.4f}, 1, true, false, 1.0f},
    /* MCar0         */ {nullptr, {0.55f, 0.04f, 0.04f, 1}, 0.45f, 0.28f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MCar1         */ {nullptr, {0.05f, 0.10f, 0.40f, 1}, 0.45f, 0.28f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MCar2         */ {nullptr, {0.70f, 0.70f, 0.68f, 1}, 0.4f, 0.3f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MCar3         */ {nullptr, {0.04f, 0.04f, 0.05f, 1}, 0.5f, 0.25f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MCar4         */ {nullptr, {0.40f, 0.42f, 0.45f, 1}, 0.6f, 0.3f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MCar5         */ {nullptr, {0.04f, 0.28f, 0.16f, 1}, 0.45f, 0.28f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MCar6         */ {nullptr, {0.80f, 0.58f, 0.06f, 1}, 0.4f, 0.3f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MCar7         */ {nullptr, {0.35f, 0.18f, 0.08f, 1}, 0.45f, 0.3f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MTire         */ {nullptr, {0.02f, 0.02f, 0.02f, 1}, 0.0f, 0.85f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MChrome       */ {nullptr, {0.80f, 0.80f, 0.82f, 1}, 1.0f, 0.12f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MAmbulanceWhite*/ {nullptr, {0.82f, 0.82f, 0.80f, 1}, 0.2f, 0.3f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MEmergencyRed */ {nullptr, {0.60f, 0.03f, 0.03f, 1}, 0.3f, 0.3f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MEmergencyYellow*/ {nullptr, {0.80f, 0.65f, 0.02f, 1}, 0.2f, 0.4f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MLightBlue    */ {nullptr, {0.01f, 0.02f, 0.08f, 1}, 0.0f, 0.3f, {0.4f, 1.2f, 9.0f}, 1, true, false, 1.0f},
    /* MLightRed     */ {nullptr, {0.08f, 0.01f, 0.01f, 1}, 0.0f, 0.3f, {9.0f, 0.5f, 0.3f}, 1, true, false, 1.0f},
    /* MHeadlight    */ {nullptr, {0.15f, 0.15f, 0.15f, 1}, 0.0f, 0.2f, {7.0f, 6.5f, 5.5f}, 1, true, false, 1.0f},
    /* MTaillight    */ {nullptr, {0.10f, 0.01f, 0.01f, 1}, 0.0f, 0.3f, {3.5f, 0.1f, 0.1f}, 1, true, false, 1.0f},
    /* MWater        */ {nullptr, {0.015f, 0.07f, 0.10f, 1}, 0.0f, 0.045f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MFoam         */ {nullptr, {0.85f, 0.90f, 0.92f, 0.55f}, 0.0f, 0.5f, {0, 0, 0}, 1, true, true, 1.0f},
    /* MWake         */ {nullptr, {0.85f, 0.92f, 0.95f, 0.22f}, 0.0f, 0.6f, {0, 0, 0}, 1, true, true, 1.0f},
    /* MSmoke0       */ {nullptr, {0.66f, 0.62f, 0.56f, 0.03f}, 0.0f, 1.0f, {0, 0, 0}, 1, false, true, 1.0f},
    /* MSmoke1       */ {nullptr, {0.62f, 0.58f, 0.52f, 0.07f}, 0.0f, 1.0f, {0, 0, 0}, 1, false, true, 1.0f},
    /* MSmoke2       */ {nullptr, {0.55f, 0.52f, 0.48f, 0.12f}, 0.0f, 1.0f, {0, 0, 0}, 1, false, true, 1.0f},
    /* MSmoke3       */ {nullptr, {0.40f, 0.38f, 0.36f, 0.20f}, 0.0f, 1.0f, {0, 0, 0}, 1, false, true, 1.0f},
    /* MSpark        */ {nullptr, {0.05f, 0.05f, 0.05f, 1}, 0.0f, 0.5f, {18.0f, 8.0f, 2.0f}, 1, true, false, 1.0f},
    /* MDebris       */ {"concrete_floor", {0.85f, 0.85f, 0.85f, 1}, 1, 1, {0, 0, 0}, 0.6f, true, false, 1.0f},
    /* MGlassShard   */ {nullptr, {0.55f, 0.70f, 0.72f, 0.45f}, 0.0f, 0.05f, {0, 0, 0}, 1, true, true, 1.0f},
    /* MBoatHullWhite*/ {nullptr, {0.85f, 0.85f, 0.83f, 1}, 0.0f, 0.2f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MBoatHullBlue */ {nullptr, {0.05f, 0.14f, 0.35f, 1}, 0.0f, 0.2f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MBoatHullRed  */ {nullptr, {0.45f, 0.05f, 0.04f, 1}, 0.0f, 0.2f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MBoatDeck     */ {"oak_wood_planks", {0.85f, 0.75f, 0.60f, 1}, 1, 1, {0, 0, 0}, 1.5f, true, false, 1.0f},
    /* MSail         */ {nullptr, {0.88f, 0.86f, 0.80f, 1}, 0.0f, 0.9f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MShirt0       */ {nullptr, {0.55f, 0.08f, 0.08f, 1}, 0.0f, 0.8f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MShirt1       */ {nullptr, {0.08f, 0.18f, 0.50f, 1}, 0.0f, 0.8f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MShirt2       */ {nullptr, {0.10f, 0.38f, 0.18f, 1}, 0.0f, 0.8f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MShirt3       */ {nullptr, {0.70f, 0.62f, 0.12f, 1}, 0.0f, 0.8f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MShirt4       */ {nullptr, {0.60f, 0.60f, 0.58f, 1}, 0.0f, 0.8f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MShirt5       */ {nullptr, {0.12f, 0.12f, 0.14f, 1}, 0.0f, 0.8f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MShirt6       */ {nullptr, {0.45f, 0.20f, 0.45f, 1}, 0.0f, 0.8f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MShirt7       */ {nullptr, {0.70f, 0.35f, 0.12f, 1}, 0.0f, 0.8f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MSkin         */ {nullptr, {0.62f, 0.44f, 0.34f, 1}, 0.0f, 0.65f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MSkin1        */ {nullptr, {0.42f, 0.27f, 0.20f, 1}, 0.0f, 0.65f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MSkin2        */ {nullptr, {0.30f, 0.17f, 0.12f, 1}, 0.0f, 0.65f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MHair0        */ {nullptr, {0.015f, 0.012f, 0.010f, 1}, 0.0f, 0.55f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MHair1        */ {nullptr, {0.10f, 0.05f, 0.025f, 1}, 0.0f, 0.55f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MHair2        */ {nullptr, {0.45f, 0.33f, 0.15f, 1}, 0.0f, 0.55f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MTrouser0     */ {nullptr, {0.04f, 0.07f, 0.18f, 1}, 0.0f, 0.85f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MTrouser1     */ {nullptr, {0.025f, 0.025f, 0.03f, 1}, 0.0f, 0.85f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MTrouser2     */ {nullptr, {0.30f, 0.24f, 0.14f, 1}, 0.0f, 0.85f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MTrouser3     */ {nullptr, {0.12f, 0.12f, 0.13f, 1}, 0.0f, 0.85f, {0, 0, 0}, 1, true, false, 1.0f},
    /* MShoe         */ {nullptr, {0.05f, 0.04f, 0.035f, 1}, 0.0f, 0.6f, {0, 0, 0}, 1, true, false, 1.0f},
};

std::vector<std::uint8_t> read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

float Materials::tile_meters(Mat m) { return defs[m].tile_m; }

Materials::Materials(render::SceneRenderer& renderer, const fs::path& cache, std::vector<std::string>& warnings) {
    gfx::GltfAsset lib;
    lib.samplers.push_back({});
    std::map<std::string, int> texture_index;
    const auto texture = [&](const fs::path& file) -> gfx::TextureRef {
        const std::string key = file.string();
        const auto it = texture_index.find(key);
        if (it != texture_index.end()) return {it->second, 0};
        gfx::Image image;
        image.name = file.filename().string();
        image.mime_type = "image/jpeg";
        image.bytes = read_file(file);
        lib.images.push_back(std::move(image));
        lib.textures.push_back({static_cast<std::int32_t>(lib.images.size() - 1), 0});
        const int t = static_cast<int>(lib.textures.size() - 1);
        texture_index[key] = t;
        return {t, 0};
    };
    for (std::uint32_t m = 0; m < MCount; ++m) {
        const MatDef& d = defs[m];
        gfx::Material mat;
        mat.name = "mat" + std::to_string(m);
        std::copy(d.color, d.color + 4, mat.base_color);
        mat.metallic = d.metallic;
        mat.roughness = d.roughness;
        mat.double_sided = d.double_sided;
        mat.normal_scale = d.normal_scale;
        for (int c = 0; c < 3; ++c) mat.emissive[c] = d.emissive[c];
        if (d.blend) mat.alpha_mode = gfx::AlphaMode::Blend;
        if (d.texture != nullptr) {
            const std::string name = d.texture;
            fs::path dir = cache / name / "1k";
            if (!fs::exists(dir / (name + "_diff_1k.jpg")) && !fs::exists(dir / (name + "_diffuse_1k.jpg"))) dir = cache / name;   // an older layout
            const std::string diff = fs::exists(dir / (name + "_diff_1k.jpg")) ? name + "_diff_1k.jpg" : name + "_diffuse_1k.jpg";
            try {
                mat.base_color_texture = texture(dir / diff);
                mat.metallic_roughness_texture = texture(dir / (name + "_arm_1k.jpg"));
                mat.occlusion_texture = mat.metallic_roughness_texture;
                mat.normal_texture = texture(dir / (name + "_nor_gl_1k.jpg"));
                mat.metallic = 1.0f;
                mat.roughness = 1.0f;
            } catch (const std::exception& e) {
                warnings.push_back(std::string("material ") + name + ": " + e.what() + " (drawn untextured)");
                mat.base_color_texture = {};
                mat.metallic_roughness_texture = {};
                mat.occlusion_texture = {};
                mat.normal_texture = {};
                mat.metallic = 0.0f;
                mat.roughness = 0.8f;
            }
        }
        lib.materials.push_back(std::move(mat));
    }
    gfx::Mesh mesh;
    gfx::Primitive prim;
    prim.positions = {0, 0, 0, 0.001f, 0, 0, 0, 0.001f, 0};
    prim.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
    prim.uv0 = {0, 0, 1, 0, 0, 1};
    prim.indices = {0, 1, 2};
    prim.bounds.expand(Float3{0, 0, 0});
    prim.bounds.expand(Float3{0.001f, 0.001f, 0});
    for (std::uint32_t m = 0; m < lib.materials.size(); ++m) {
        gfx::Primitive p = prim;
        p.material = static_cast<std::int32_t>(m);
        mesh.primitives.push_back(std::move(p));
    }
    mesh.bounds = prim.bounds;
    lib.meshes.push_back(std::move(mesh));
    lib.instances.push_back({0, 0, Mat4::identity(), "library"});
    lib.bounds = prim.bounds;
    gfx::StageReport report;
    gfx::StageSettings ss;
    ss.max_anisotropy = 8.0f;
    const render::GpuModel staged = gfx::stage_model(renderer, lib, "materials", ss, &report);
    for (const std::string& w : report.warnings) warnings.push_back("materials: " + w);
    std::printf("material library: %u textures (%.1f MiB), %zu materials\n", report.textures, static_cast<double>(report.texture_bytes) / 1048576.0, staged.materials.size());
    gpu_.assign(staged.materials.begin(), staged.materials.begin() + MCount);
}

// ---- mesh builder ----------------------------------------------------------------------------------------------------------------------------------------------------------------------
namespace {
Float3 cross3(Float3 a, Float3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot3(Float3 a, Float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 norm3(Float3 a) {
    const float l = std::sqrt(dot3(a, a));
    return l > 1e-12f ? Float3{a.x / l, a.y / l, a.z / l} : Float3{0, 0, 1};
}
} // namespace

void MeshBuilder::quad_uv(Mat m, Float3 a, Float3 b, Float3 c, Float3 d, std::array<std::array<float, 2>, 4> uv_metres) {
    Soup& s = parts_[m];
    const Float3 n = norm3(cross3(b - a, c - a));
    const float inv = 1.0f / Materials::tile_meters(m);
    const std::uint32_t base = static_cast<std::uint32_t>(s.pos.size());
    const Float3 p[4] = {a, b, c, d};
    for (int i = 0; i < 4; ++i) {
        s.pos.push_back(p[i]);
        s.nrm.push_back(n);
        s.uv.push_back({uv_metres[static_cast<std::size_t>(i)][0] * inv, uv_metres[static_cast<std::size_t>(i)][1] * inv});
    }
    s.idx.insert(s.idx.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void MeshBuilder::quad(Mat m, Float3 a, Float3 b, Float3 c, Float3 d) {
    const Float3 e1 = b - a, e2 = d - a;
    const float w = std::sqrt(dot3(e1, e1)), h = std::sqrt(dot3(e2, e2));
    quad_uv(m, a, b, c, d, {{{0, 0}, {w, 0}, {w, h}, {0, h}}});
}

void MeshBuilder::tri(Mat m, Float3 a, Float3 b, Float3 c) {
    Soup& s = parts_[m];
    const Float3 n = norm3(cross3(b - a, c - a));
    const float inv = 1.0f / Materials::tile_meters(m);
    const std::uint32_t base = static_cast<std::uint32_t>(s.pos.size());
    const Float3 e1 = b - a, e2 = c - a;
    const float w = std::sqrt(dot3(e1, e1));
    const float u2 = dot3(e2, norm3(e1)), v2 = std::sqrt(std::max(0.0f, dot3(e2, e2) - u2 * u2));
    s.pos.push_back(a); s.pos.push_back(b); s.pos.push_back(c);
    for (int i = 0; i < 3; ++i) s.nrm.push_back(n);
    s.uv.push_back({0, 0}); s.uv.push_back({w * inv, 0}); s.uv.push_back({u2 * inv, v2 * inv});
    s.idx.insert(s.idx.end(), {base, base + 1, base + 2});
}

void MeshBuilder::box(Mat m, Float3 lo, Float3 hi, unsigned faces) {
    const Float3 c[8] = {{lo.x, lo.y, lo.z}, {hi.x, lo.y, lo.z}, {hi.x, hi.y, lo.z}, {lo.x, hi.y, lo.z}, {lo.x, lo.y, hi.z}, {hi.x, lo.y, hi.z}, {hi.x, hi.y, hi.z}, {lo.x, hi.y, hi.z}};
    if (faces & 1) quad(m, c[3], c[0], c[4], c[7]);    // -x
    if (faces & 2) quad(m, c[1], c[2], c[6], c[5]);    // +x
    if (faces & 4) quad(m, c[0], c[1], c[5], c[4]);    // -y
    if (faces & 8) quad(m, c[2], c[3], c[7], c[6]);    // +y
    if (faces & 16) quad(m, c[3], c[2], c[1], c[0]);   // -z
    if (faces & 32) quad(m, c[4], c[5], c[6], c[7]);   // +z
}

void MeshBuilder::oriented_box(Mat m, Float3 at, float yaw, Float3 h, float z0) {
    const float c = std::cos(yaw), s = std::sin(yaw);
    const auto rot = [&](float x, float y, float z) { return Float3{at.x + x * c - y * s, at.y + x * s + y * c, z0 + z}; };
    const Float3 p[8] = {rot(-h.x, -h.y, 0), rot(h.x, -h.y, 0), rot(h.x, h.y, 0), rot(-h.x, h.y, 0), rot(-h.x, -h.y, 2 * h.z), rot(h.x, -h.y, 2 * h.z), rot(h.x, h.y, 2 * h.z), rot(-h.x, h.y, 2 * h.z)};
    quad(m, p[3], p[0], p[4], p[7]);
    quad(m, p[1], p[2], p[6], p[5]);
    quad(m, p[0], p[1], p[5], p[4]);
    quad(m, p[2], p[3], p[7], p[6]);
    quad(m, p[3], p[2], p[1], p[0]);
    quad(m, p[4], p[5], p[6], p[7]);
}

void MeshBuilder::cone(Mat m, Float3 base, float rb, float rt, float height, int seg) {
    for (int i = 0; i < seg; ++i) {
        const float a0 = static_cast<float>(2.0 * kPi * i / seg), a1 = static_cast<float>(2.0 * kPi * (i + 1) / seg);
        const Float3 b0{base.x + std::cos(a0) * rb, base.y + std::sin(a0) * rb, base.z}, b1{base.x + std::cos(a1) * rb, base.y + std::sin(a1) * rb, base.z};
        const Float3 t0{base.x + std::cos(a0) * rt, base.y + std::sin(a0) * rt, base.z + height}, t1{base.x + std::cos(a1) * rt, base.y + std::sin(a1) * rt, base.z + height};
        const float w = static_cast<float>(2.0 * kPi * static_cast<double>(rb) / seg), h = std::sqrt(height * height + (rb - rt) * (rb - rt));
        quad_uv(m, b0, b1, t1, t0, {{{0, 0}, {w, 0}, {w, h}, {0, h}}});
    }
}

void MeshBuilder::cylinder(Mat m, Float3 base, float r, float h, int seg, bool caps) {
    cone(m, base, r, r, h, seg);
    if (!caps) return;
    for (int i = 0; i < seg; ++i) {
        const float a0 = static_cast<float>(2.0 * kPi * i / seg), a1 = static_cast<float>(2.0 * kPi * (i + 1) / seg);
        tri(m, {base.x, base.y, base.z + h}, {base.x + std::cos(a0) * r, base.y + std::sin(a0) * r, base.z + h}, {base.x + std::cos(a1) * r, base.y + std::sin(a1) * r, base.z + h});
        tri(m, {base.x, base.y, base.z}, {base.x + std::cos(a1) * r, base.y + std::sin(a1) * r, base.z}, {base.x + std::cos(a0) * r, base.y + std::sin(a0) * r, base.z});
    }
}

void MeshBuilder::sphere(Mat m, Float3 c, float r, int rings, int seg) {
    const auto point = [&](int ri, int si) {
        const float phi = static_cast<float>(kPi * ri / rings), th = static_cast<float>(2.0 * kPi * si / seg);
        return Float3{c.x + r * std::sin(phi) * std::cos(th), c.y + r * std::sin(phi) * std::sin(th), c.z + r * std::cos(phi)};
    };
    for (int ri = 0; ri < rings; ++ri)
        for (int si = 0; si < seg; ++si) {
            const Float3 a = point(ri, si), b = point(ri, si + 1), d = point(ri + 1, si), e = point(ri + 1, si + 1);
            if (ri != 0) tri(m, a, d, b);
            if (ri != rings - 1) tri(m, b, d, e);
        }
}

std::size_t MeshBuilder::triangle_count() const {
    std::size_t n = 0;
    for (const auto& [k, s] : parts_) n += s.idx.size() / 3;
    return n;
}

std::vector<MeshIn> MeshBuilder::finish() {
    std::vector<MeshIn> out;
    for (auto& [mat, s] : parts_) {
        if (s.idx.empty()) continue;
        gfx::Primitive p;
        p.positions.reserve(s.pos.size() * 3);
        for (std::size_t i = 0; i < s.pos.size(); ++i) {
            p.positions.insert(p.positions.end(), {s.pos[i].x, s.pos[i].y, s.pos[i].z});
            p.normals.insert(p.normals.end(), {s.nrm[i].x, s.nrm[i].y, s.nrm[i].z});
            p.uv0.insert(p.uv0.end(), {s.uv[i][0], s.uv[i][1]});
        }
        p.indices = s.idx;
        (void)gfx::generate_tangents(p);
        MeshIn mi;
        mi.material = mat;
        const std::size_t n = s.pos.size();
        mi.vertices.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            render::ModelVertex& v = mi.vertices[i];
            v.position = s.pos[i];
            v.normal = s.nrm[i];
            v.uv[0] = s.uv[i][0];
            v.uv[1] = s.uv[i][1];
            if (p.tangents.size() == n * 4)
                for (int c = 0; c < 4; ++c) v.tangent[c] = p.tangents[i * 4 + static_cast<std::size_t>(c)];
        }
        mi.indices = std::move(s.idx);
        out.push_back(std::move(mi));
    }
    parts_.clear();
    return out;
}

// ---- upload ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------
render::GpuModel upload_model(render::SceneRenderer& renderer, const std::string& name, const std::vector<MeshIn>& meshes, const std::vector<render::GpuMaterial>& materials) {
    render::GpuModel model;
    model.name = name;
    model.materials = materials;
    std::vector<render::GpuPrimitive> prims;
    render::GpuResourceRegistry& registry = renderer.registry();
    for (const MeshIn& m : meshes) {
        if (m.vertices.empty() || m.indices.empty()) continue;
        render::GpuPrimitive p;
        rhi::BufferDesc vb;
        vb.size = m.vertices.size() * sizeof(render::ModelVertex);
        vb.usage = rhi::BufferUsage::Vertex | rhi::BufferUsage::TransferDst;
        vb.debug_name = "alive vertices";
        rhi::BufferDesc ib;
        ib.size = m.indices.size() * sizeof(std::uint32_t);
        ib.usage = rhi::BufferUsage::Index | rhi::BufferUsage::TransferDst;
        ib.debug_name = "alive indices";
        p.vertices = registry.create_buffer(vb, std::as_bytes(std::span(m.vertices)));
        p.indices = registry.create_buffer(ib, std::as_bytes(std::span(m.indices)));
        p.index_type = rhi::IndexType::Uint32;
        p.index_count = static_cast<std::uint32_t>(m.indices.size());
        p.material = m.material;
        Float3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
        for (const render::ModelVertex& v : m.vertices) {
            lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
            hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
        }
        p.bounds_center = (lo + hi) * 0.5f;
        float r = 0;
        for (const render::ModelVertex& v : m.vertices) {
            const Float3 d = v.position - p.bounds_center;
            r = std::max(r, std::sqrt(render::dot(d, d)));
        }
        p.bounds_radius = r;
        prims.push_back(std::move(p));
    }
    model.meshes.push_back(std::move(prims));
    model.placements.push_back({0, Mat4::identity()});
    return model;
}

render::GpuModel load_prop(render::SceneRenderer& renderer, const fs::path& path, std::vector<std::string>& warnings, Float3* lo, Float3* hi) {
    gfx::GltfLoadResult loaded = gfx::load_gltf_file(path);
    if (!loaded.ok()) throw std::runtime_error("cannot load " + path.string() + ": " + loaded.error);
    gfx::StageReport report;
    gfx::StageSettings ss;
    ss.max_anisotropy = 8.0f;
    render::GpuModel model = gfx::stage_model(renderer, *loaded.asset, path.stem().string(), ss, &report);
    for (const std::string& w : report.warnings) warnings.push_back(path.stem().string() + ": " + w);
    if (lo != nullptr && hi != nullptr) {
        *lo = {1e30f, 1e30f, 1e30f};
        *hi = {-1e30f, -1e30f, -1e30f};
        for (const gfx::MeshInstance& inst : loaded.asset->instances) {
            if (inst.mesh < 0) continue;
            for (const gfx::Primitive& p : loaded.asset->meshes[static_cast<std::size_t>(inst.mesh)].primitives) {
                if (!p.bounds.valid()) continue;
                for (int c = 0; c < 8; ++c) {
                    const Float3 q{(c & 1) ? p.bounds.max.x : p.bounds.min.x, (c & 2) ? p.bounds.max.y : p.bounds.min.y, (c & 4) ? p.bounds.max.z : p.bounds.min.z};
                    const Float3 w = render::transform_point(inst.world, q);
                    *lo = {std::min(lo->x, w.x), std::min(lo->y, w.y), std::min(lo->z, w.z)};
                    *hi = {std::max(hi->x, w.x), std::max(hi->y, w.y), std::max(hi->z, w.z)};
                }
            }
        }
    }
    return model;
}

} // namespace saige::alive::rend
