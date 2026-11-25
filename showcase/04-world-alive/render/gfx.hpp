// Showcase 04, the drawing side: the material library (Poly Haven CC0 PBR textures and plain painted/emissive materials), a small mesh builder for the district's procedural geometry, the upload of CPU meshes
// into renderer models, licensed prop loading, and the offscreen target the frames are read back from. Nothing here knows about the simulation.
#pragma once

#include "saige/gfx_assets/gltf.hpp"
#include "saige/render/camera.hpp"
#include "saige/render/model.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/render/transform.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <map>
#include <string>
#include <vector>

namespace saige::alive::rend {

namespace fs = std::filesystem;
using render::Float3;
using render::Mat4;

inline constexpr double kPi = 3.14159265358979323846;

// ---- matrices --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
[[nodiscard]] Mat4 translation(Float3 t);
[[nodiscard]] Mat4 rot_z(float a);
[[nodiscard]] Mat4 rot_x(float a);
[[nodiscard]] Mat4 rot_y(float a);
[[nodiscard]] Mat4 scaling(float s);
[[nodiscard]] Mat4 scaling(float x, float y, float z);
[[nodiscard]] Mat4 from_quat(float x, float y, float z, float w);

// ---- readback --------------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct Rgba {
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t> px;
};

class Offscreen {
public:
    Offscreen(rhi::Device& device, std::uint32_t width, std::uint32_t height);
    ~Offscreen();
    Offscreen(const Offscreen&) = delete;
    Offscreen& operator=(const Offscreen&) = delete;
    Rgba render(render::SceneRenderer& renderer, const render::SceneFrame& frame, render::FrameStats* stats = nullptr);

private:
    rhi::Device& device_;
    std::uint32_t width_, height_;
    rhi::TextureHandle color_;
    rhi::BufferHandle readback_;
};

[[nodiscard]] Rgba downsample(const Rgba& src, std::uint32_t factor);
bool write_ppm(std::ostream& out, const Rgba& img);
bool write_ppm(const fs::path& path, const Rgba& img);

// ---- materials -------------------------------------------------------------------------------------------------------------------------------------------------------------------------
enum Mat : std::uint32_t {
    MAsphalt, MSidewalk, MCurb, MPromenade, MPaving, MPier, MGrass,
    MBrickA, MBrickB, MBrickC, MBrickPattern, MPlasterA, MPlasterB, MPlasterC, MStucco, MCladdingA, MCladdingB, MConcreteTile, MFactory,
    MRoofSlate, MRoofGrey, MRoofClay, MRoofMetal,
    MFloorWood, MFloorWoodDark, MFloorTile, MMetalPlate,
    MBark, MFoliage, MFoliageDark,
    MGlassDark, MGlassLit, MGlassLitCool, MDoorRed, MDoorGreen, MDoorBlue, MDoorWood, MFrameWhite, MFrameDark,
    MAwningRed, MAwningGreen, MAwningBlue, MAwningYellow,
    MMarkWhite, MMarkYellow, MPaintSteel, MPaintDarkGrey, MPaintBlack, MPaintWhite,
    MSignalRed, MSignalAmber, MSignalGreen, MSignalWalk, MSignalOff, MLampGlow, MBulbWarm,
    MCar0, MCar1, MCar2, MCar3, MCar4, MCar5, MCar6, MCar7,
    MTire, MChrome, MAmbulanceWhite, MEmergencyRed, MEmergencyYellow, MLightBlue, MLightRed, MHeadlight, MTaillight,
    MWater, MFoam, MWake, MSmoke0, MSmoke1, MSmoke2, MSmoke3, MSpark, MDebris, MGlassShard, MBoatHullWhite, MBoatHullBlue, MBoatHullRed, MBoatDeck, MSail,
    MShirt0, MShirt1, MShirt2, MShirt3, MShirt4, MShirt5, MShirt6, MShirt7, MSkin, MSkin1, MSkin2,
    MHair0, MHair1, MHair2, MTrouser0, MTrouser1, MTrouser2, MTrouser3, MShoe, MInterior, MCeiling,
    MCount
};

class Materials {
public:
    // `cache` is the asset cache's polyhaven folder (textures under <name>/1k/<name>_{diff,arm,nor_gl}_1k.jpg).
    Materials(render::SceneRenderer& renderer, const fs::path& cache, std::vector<std::string>& warnings);
    [[nodiscard]] const std::vector<render::GpuMaterial>& list() const { return gpu_; }
    [[nodiscard]] static float tile_meters(Mat m);   // metres of surface one texture repeat covers (1 for untextured)

private:
    std::vector<render::GpuMaterial> gpu_;
};

// ---- mesh building ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
struct MeshIn {
    std::vector<render::ModelVertex> vertices;
    std::vector<std::uint32_t> indices;
    std::uint32_t material = 0;
};

// Accumulates triangles per material. Texture coordinates are in metres divided by the material's tile size, so the texel density is the same on every surface.
class MeshBuilder {
public:
    // A quad a-b-c-d (counter-clockwise seen from the normal's side). `uvs` gives the texture coordinates of a, b, c, d in metres; by default planar by the quad's own edges.
    void quad(Mat m, Float3 a, Float3 b, Float3 c, Float3 d);
    void quad_uv(Mat m, Float3 a, Float3 b, Float3 c, Float3 d, std::array<std::array<float, 2>, 4> uv_metres);
    void tri(Mat m, Float3 a, Float3 b, Float3 c);
    // An axis-aligned box; `faces` bit mask: 1 -x, 2 +x, 4 -y, 8 +y, 16 -z, 32 +z.
    void box(Mat m, Float3 lo, Float3 hi, unsigned faces = 63);
    // A box rotated by `yaw` about z around its centre on the ground (lo/hi in the box's own frame, centred at `at`).
    void oriented_box(Mat m, Float3 at, float yaw, Float3 half_extents, float z0);
    void cylinder(Mat m, Float3 base, float radius, float height, int segments = 12, bool caps = true);
    void cone(Mat m, Float3 base, float radius_bottom, float radius_top, float height, int segments = 12);
    void sphere(Mat m, Float3 centre, float radius, int rings = 6, int segments = 10);
    [[nodiscard]] std::vector<MeshIn> finish();
    [[nodiscard]] std::size_t triangle_count() const;

private:
    struct Soup {
        std::vector<Float3> pos, nrm;
        std::vector<std::array<float, 2>> uv;
        std::vector<std::uint32_t> idx;
    };
    std::map<std::uint32_t, Soup> parts_;
};

[[nodiscard]] render::GpuModel upload_model(render::SceneRenderer& renderer, const std::string& name, const std::vector<MeshIn>& meshes, const std::vector<render::GpuMaterial>& materials);
[[nodiscard]] render::GpuModel load_prop(render::SceneRenderer& renderer, const fs::path& gltf, std::vector<std::string>& warnings, Float3* lo = nullptr, Float3* hi = nullptr);

} // namespace saige::alive::rend
