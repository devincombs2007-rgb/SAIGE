// Imported models on a real (validated) Vulkan device: texture orientation and shading checked against a CPU oracle,
// occlusion independent of draw order, mirrored and non-uniformly scaled instances, double-sided and alpha-mask materials,
// and degenerate transforms. CI: lavapipe. Validation errors fail each test.
//
// Oracle: with fog off, a lit pixel's linear color is `albedo * K(normal)` where K = ambient(normal) + sun_color * max(0,
// n.l), the same terms the shader uses. The tests compute K on the CPU (normals from the CPU's inverse transpose) and
// compare, so a wrong texture orientation, winding, normal transform or lighting term fails.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/transform.hpp"
#include "model_test_util.hpp"
#include "vulkan_test_util.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 192;
constexpr uint32_t kH = 108;

RendererSettings model_settings()
{
    RendererSettings s = deterministic_settings();
    s.fog_density = 0.0f;  // lit color is then exactly albedo * K, independent of distance
    s.sky = {0.30f, 0.50f, 0.82f, 1.0f};
    return s;
}

/// K(normal) for the three channels: the shader's ambient + sun terms.
Float3 lighting_k(Float3 n, const RendererSettings& s)
{
    const Float3 sun = normalize(Float3{-0.40f, -0.55f, 0.73f});
    const float ndl = std::max(0.0f, dot(n, sun));
    const float t = n.z * 0.5f + 0.5f;
    const Float3 ground{0.20f, 0.18f, 0.16f};
    const Float3 sky{s.sky.r * 0.6f, s.sky.g * 0.6f, s.sky.b * 0.6f};
    const Float3 ambient = ground * (1.0f - t) + sky * t;
    return {ambient.x + 1.00f * ndl, ambient.y + 0.95f * ndl, ambient.z + 0.85f * ndl};
}

struct Pixel {
    int x, y;
};

Pixel to_pixel(const Mat4& vp, Float3 world)
{
    const Projected p = project_point(vp, world);
    const PixelPos px = ndc_to_pixel(p.ndc_x, p.ndc_y, static_cast<float>(kW), static_cast<float>(kH));
    return {static_cast<int>(px.x), static_cast<int>(px.y)};
}

/// Linear color of a readback pixel.
Float3 linear_at(const Image& img, Pixel p)
{
    const uint8_t* c = img.at(static_cast<uint32_t>(p.x), static_cast<uint32_t>(p.y));
    return {srgb_to_linear(c[0]), srgb_to_linear(c[1]), srgb_to_linear(c[2])};
}

struct Rig {
    std::unique_ptr<rhi::Device> device;
    std::unique_ptr<SceneRenderer> renderer;
    std::unique_ptr<OffscreenTarget> target;

    /// `shading`: Legacy (the default, the 0.2 look) or Extended (also shades the emissive and occlusion textures).
    explicit Rig(const char* name, ModelShading shading = ModelShading::Legacy)
    {
        device = make_validated_device(name, /*synchronization=*/true);
        RendererSettings settings = model_settings();
        settings.model_shading = shading;
        renderer = std::make_unique<SceneRenderer>(*device, rhi::Format::RGBA8_SRGB, settings);
        target = std::make_unique<OffscreenTarget>(*device, kW, kH);
    }
    ~Rig()
    {
        target.reset();
        renderer.reset();
    }

    GpuMaterial material(const MaterialParams& params, const TextureRef& texture = {}, const SamplerRef& sampler = {},
                         const char* name = "test.material")
    {
        return renderer->model_renderer().create_material(params, texture, sampler, name);
    }

    Image render(const Camera& camera, std::span<const ModelDraw> draws, FrameStats* stats = nullptr)
    {
        return target->render(*renderer, {}, camera, 0, draws, stats);
    }
};

/// The checker from Core's fixture: 8x8, 2x2-pixel cells, orange e86c25 / blue 2282ba, orange top-left. sRGB.
std::vector<std::byte> checker_texels(uint8_t alpha_left = 255)
{
    std::vector<std::byte> out;
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x) {
            const bool orange = (((x / 2) + (y / 2)) % 2) == 0;
            const uint8_t r = orange ? 0xE8 : 0x22, g = orange ? 0x6C : 0x82, b = orange ? 0x25 : 0xBA;
            out.push_back(static_cast<std::byte>(r));
            out.push_back(static_cast<std::byte>(g));
            out.push_back(static_cast<std::byte>(b));
            out.push_back(static_cast<std::byte>(x < 4 ? alpha_left : 255));
        }
    }
    return out;
}

TextureRef checker_texture(Rig& rig, uint8_t alpha_left = 255)
{
    rhi::TextureDesc td;
    td.format = rhi::Format::RGBA8_SRGB;
    td.width = td.height = 8;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = "test.checker";
    return rig.renderer->registry().create_texture(td, checker_texels(alpha_left));
}

SamplerRef nearest_sampler(Rig& rig)
{
    rhi::SamplerDesc sd;
    sd.min_filter = sd.mag_filter = rhi::Filter::Nearest;
    sd.mipmap_mode = rhi::MipmapMode::Nearest;
    sd.address_u = sd.address_v = rhi::AddressMode::ClampToEdge;
    return rig.renderer->registry().get_sampler(sd);
}

Float3 texel_linear(bool orange)
{
    return orange ? Float3{srgb_to_linear(0xE8), srgb_to_linear(0x6C), srgb_to_linear(0x25)}
                  : Float3{srgb_to_linear(0x22), srgb_to_linear(0x82), srgb_to_linear(0xBA)};
}

} // namespace

TEST_CASE("Models: a textured quad shows its texture in the right orientation and lit by the oracle")
{
    Rig rig("model_texture");
    {
        const Camera camera = look_at_camera({-4, 0, 1.5f}, {0, 0, 1.5f});
        const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
        const GpuModel quad = make_quad(rig.renderer->registry(), rig.material({}, checker_texture(rig), nearest_sampler(rig)), 0.0f, 1.5f);
        const ModelDraw draw{&quad, Mat4::identity()};
        FrameStats stats;
        const Image img = rig.render(camera, std::span(&draw, 1), &stats);
        CHECK(stats.model_instances == 1);
        CHECK(stats.model_instances_skipped == 0);

        const Float3 k = lighting_k({-1, 0, 0}, model_settings());  // the quad's normal
        const Float3 top_left{0, +1, 2.5f};
        const Float3 top_right{0, -1, 2.5f};
        const Float3 bottom_left{0, +1, 0.5f};
        size_t bad = 0;
        size_t checked = 0;
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 4; ++i) {
                // Center of cell (i, j): i counts from the screen's left, j from the top.
                const Float3 world = top_left + (top_right - top_left) * ((static_cast<float>(i) + 0.5f) / 4.0f) +
                                     (bottom_left - top_left) * ((static_cast<float>(j) + 0.5f) / 4.0f);
                const Float3 got = linear_at(img, to_pixel(vp, world));
                const Float3 want = texel_linear(((i + j) % 2) == 0) ;
                const Float3 expected{want.x * k.x, want.y * k.y, want.z * k.z};
                ++checked;
                bad += near3(got, expected, 0.03f) ? 0u : 1u;
            }
        }
        CHECK(checked == 16);
        CHECK(bad == 0);  // every cell: right color, in the right place, with the right lighting
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Models: occlusion does not depend on draw order, for parallel and intersecting quads")
{
    Rig rig("model_occlusion");
    {
        MaterialParams red;
        red.base_color[0] = 0.8f; red.base_color[1] = 0.05f; red.base_color[2] = 0.05f;
        MaterialParams blue;
        blue.base_color[0] = 0.05f; blue.base_color[1] = 0.05f; blue.base_color[2] = 0.8f;
        const Camera camera = look_at_camera({-5, 0, 1.5f}, {0, 0, 1.5f});
        const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
        GpuResourceRegistry& registry = rig.renderer->registry();

        // Parallel: blue is nearer (x = 0), red farther (x = 1), shifted so they overlap partly.
        const GpuModel near_quad = make_quad(registry, rig.material(blue), 0.0f, 1.5f);
        const GpuModel far_quad = make_quad(registry, rig.material(red), 1.0f, 1.5f);
        const ModelDraw far_first[2] = {{&far_quad, translation({0, -0.8f, 0})}, {&near_quad, Mat4::identity()}};
        const ModelDraw near_first[2] = {{&near_quad, Mat4::identity()}, {&far_quad, translation({0, -0.8f, 0})}};
        const Image a = rig.render(camera, far_first);
        const Image b = rig.render(camera, near_first);
        CHECK(a.rgba == b.rgba);  // identical in either order
        const Float3 overlap = linear_at(a, to_pixel(vp, {0, -0.4f, 1.5f}));  // inside both quads
        CHECK(overlap.z > overlap.x * 4.0f);                                  // blue (near) wins
        const Float3 red_only = linear_at(a, to_pixel(vp, {1, -1.6f, 1.5f}));  // only the shifted far quad covers this
        CHECK(red_only.x > red_only.z * 4.0f);

        // Intersecting: one quad rotated about Z by 50 degrees through the other, so each is in front on one side.
        const ModelDraw crossed_a[2] = {{&near_quad, Mat4::identity()}, {&far_quad, rotation_z(0.87f) * translation({-1, 0, 0})}};
        const ModelDraw crossed_b[2] = {crossed_a[1], crossed_a[0]};
        const Image c = rig.render(camera, crossed_a);
        const Image d = rig.render(camera, crossed_b);
        CHECK(c.rgba == d.rgba);
        // Both colors appear (each wins somewhere along the intersection): the test is not vacuous.
        size_t reddish = 0, bluish = 0;
        for (uint32_t y = 0; y < kH; ++y) {
            for (uint32_t x = 0; x < kW; ++x) {
                const Float3 p = linear_at(c, {static_cast<int>(x), static_cast<int>(y)});
                reddish += (p.x > p.z * 4.0f && p.x > 0.08f) ? 1u : 0u;
                bluish += (p.z > p.x * 4.0f && p.z > 0.08f) ? 1u : 0u;
            }
        }
        CHECK(reddish > 50);
        CHECK(bluish > 50);
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Models: a mirrored instance renders identically to its unmirrored twin (winding and normals)")
{
    Rig rig("model_mirror");
    {
        MaterialParams grey;
        grey.base_color[0] = grey.base_color[1] = grey.base_color[2] = 0.6f;
        const GpuModel cube = make_cube(rig.renderer->registry(), rig.material(grey));
        const Camera camera = look_at_camera({-3, -2, 2}, {0, 0, 0.5f});
        const Mat4 lift = translation({0, 0, 0.5f});
        const ModelDraw normal{&cube, lift};
        const ModelDraw mirrored{&cube, lift * scale_matrix({-1, 1, 1})};  // determinant -1; a cube is mirror symmetric
        const Image a = rig.render(camera, std::span(&normal, 1));
        FrameStats stats;
        const Image b = rig.render(camera, std::span(&mirrored, 1), &stats);
        CHECK(stats.model_instances == 1);
        CHECK(a.rgba == b.rgba);  // inside-out winding, culling the wrong faces or flipped normals would all differ
        const Mask drawn = changed_pixels(a, rig.target->render(*rig.renderer, {}, camera));
        CHECK(drawn.count > 500);  // and the cube is really there
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Models: normals follow the inverse transpose under rotation, non-uniform scale and shear")
{
    Rig rig("model_normals");
    {
        MaterialParams grey;
        grey.base_color[0] = grey.base_color[1] = grey.base_color[2] = 0.5f;
        const GpuModel cube = make_cube(rig.renderer->registry(), rig.material(grey));
        const Camera camera = look_at_camera({-5, -4, 3}, {0, 0, 1});
        const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));

        // A general affine: rotate about Z, scale unevenly, then shear x by z. Not representable as T*R*S.
        Mat4 shear = Mat4::identity();
        shear.at(0, 2) = 0.6f;
        const Mat4 m = translation({0, 0, 1}) * rotation_z(0.7f) * scale_matrix({1.0f, 2.0f, 0.8f}) * shear;
        const ModelDraw draw{&cube, m};
        const Image img = rig.render(camera, std::span(&draw, 1));

        const std::optional<Mat4> inv = inverse(m);
        REQUIRE(inv.has_value());
        const Mat4 inv_t = transpose(*inv);  // CPU normal matrix, the oracle for the shader's cofactor
        struct Face { Float3 normal; };
        const Face faces[6] = {{{1, 0, 0}}, {{-1, 0, 0}}, {{0, 1, 0}}, {{0, -1, 0}}, {{0, 0, 1}}, {{0, 0, -1}}};
        const Float3 eye = camera.position;
        int visible = 0, bad = 0;
        for (const Face& f : faces) {
            const Float3 center_local = f.normal * 0.5f;
            const Float4 cw = m * Float4{center_local.x, center_local.y, center_local.z, 1.0f};
            const Float3 center{cw.x, cw.y, cw.z};
            const Float4 nw4 = inv_t * Float4{f.normal.x, f.normal.y, f.normal.z, 0.0f};
            const Float3 n = normalize(Float3{nw4.x, nw4.y, nw4.z});
            if (dot(n, eye - center) <= 0.2f) {
                continue;  // faces turned away from the camera are not visible
            }
            ++visible;
            const Float3 k = lighting_k(n, model_settings());
            const Float3 got = linear_at(img, to_pixel(vp, center));
            const Float3 expected = k * 0.5f;
            bad += near3(got, expected, 0.03f) ? 0 : 1;
        }
        CHECK(visible >= 2);
        CHECK(bad == 0);
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Models: double-sided materials light their back faces, single-sided ones are culled")
{
    Rig rig("model_double_sided");
    {
        MaterialParams one;
        one.base_color[0] = one.base_color[1] = one.base_color[2] = 0.6f;
        MaterialParams two = one;
        two.double_sided = true;
        GpuResourceRegistry& registry = rig.renderer->registry();
        const GpuModel single = make_quad(registry, rig.material(one), 0.0f, 1.5f);
        const GpuModel doubled = make_quad(registry, rig.material(two), 0.0f, 1.5f);
        const Camera front = look_at_camera({-4, 0, 1.5f}, {0, 0, 1.5f});
        const Camera back = look_at_camera({4, 0, 1.5f}, {0, 0, 1.5f});
        const Mat4 vp_front = view_projection_matrix(front, static_cast<float>(kW) / static_cast<float>(kH));
        const Mat4 vp_back = view_projection_matrix(back, static_cast<float>(kW) / static_cast<float>(kH));
        const ModelDraw s{&single, Mat4::identity()};
        const ModelDraw d{&doubled, Mat4::identity()};

        const Float3 sky_linear{model_settings().sky.r, model_settings().sky.g, model_settings().sky.b};
        const Float3 center{0, 0, 1.5f};
        // Front: both materials draw the quad lit with its outward normal (-X).
        const Float3 k_front = lighting_k({-1, 0, 0}, model_settings()) * 0.6f;
        CHECK(near3(linear_at(rig.render(front, std::span(&s, 1)), to_pixel(vp_front, center)), k_front, 0.03f));
        CHECK(near3(linear_at(rig.render(front, std::span(&d, 1)), to_pixel(vp_front, center)), k_front, 0.03f));
        // Back: the single-sided quad is culled (we see the ground behind it, not the quad) ...
        const Float3 behind_single = linear_at(rig.render(back, std::span(&s, 1)), to_pixel(vp_back, center));
        CHECK_FALSE(near3(behind_single, lighting_k({+1, 0, 0}, model_settings()) * 0.6f, 0.03f));
        // ... and the double-sided quad is lit with the reversed normal (+X).
        const Float3 k_back = lighting_k({+1, 0, 0}, model_settings()) * 0.6f;
        CHECK(near3(linear_at(rig.render(back, std::span(&d, 1)), to_pixel(vp_back, center)), k_back, 0.03f));
        (void)sky_linear;
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Models: an alpha-mask material discards fragments below its cutoff")
{
    Rig rig("model_alpha_mask");
    {
        MaterialParams masked;
        masked.alpha_cutoff = 0.5f;
        const GpuModel quad = make_quad(rig.renderer->registry(), rig.material(masked, checker_texture(rig, /*alpha_left=*/0), nearest_sampler(rig)), 0.0f, 1.5f);
        const Camera camera = look_at_camera({-4, 0, 1.5f}, {0, 0, 1.5f});
        const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
        const ModelDraw draw{&quad, Mat4::identity()};
        const Image masked_img = rig.render(camera, std::span(&draw, 1));
        const Image empty = rig.render(camera, {});
        // Texture x < 4 is transparent. Screen left is +Y, uv.x = 0 at the left, so the quad's left half is gone.
        const Pixel left = to_pixel(vp, {0, +0.5f, 1.5f});
        const Pixel right = to_pixel(vp, {0, -0.5f, 1.5f});
        CHECK(near3(linear_at(masked_img, left), linear_at(empty, left), 0.01f));     // shows what is behind: the ground/sky
        CHECK_FALSE(near3(linear_at(masked_img, right), linear_at(empty, right), 0.05f));  // the opaque half is drawn
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Models: non-finite and degenerate transforms are skipped and counted, not drawn")
{
    Rig rig("model_degenerate");
    {
        MaterialParams grey;
        const GpuModel cube = make_cube(rig.renderer->registry(), rig.material(grey));
        const Camera camera = look_at_camera({-4, 0, 1}, {0, 0, 0.5f});
        Mat4 nan_matrix = Mat4::identity();
        nan_matrix.at(0, 3) = std::nanf("");
        Mat4 inf_matrix = Mat4::identity();
        inf_matrix.at(1, 1) = INFINITY;
        const Mat4 flat = scale_matrix({1, 1, 0});  // determinant 0
        const ModelDraw draws[4] = {{&cube, nan_matrix}, {&cube, inf_matrix}, {&cube, flat}, {&cube, translation({0, 0, 0.5f})}};
        FrameStats stats;
        const Image img = rig.render(camera, draws, &stats);
        CHECK(stats.model_instances == 1);
        CHECK(stats.model_instances_skipped == 3);
        CHECK(stats.draw_calls >= 2);  // ground + the one valid cube
        CHECK(img.width == kW);
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Models: several instances of one model share its GPU resources")
{
    Rig rig("model_instances");
    {
        MaterialParams grey;
        const GpuModel cube = make_cube(rig.renderer->registry(), rig.material(grey));
        const Camera camera = look_at_camera({-8, 0, 2}, {0, 0, 0.5f});
        const ResourceStats before = rig.renderer->registry().total_stats();
        std::vector<ModelDraw> draws;
        for (int i = -3; i <= 3; ++i) {
            draws.push_back({&cube, translation({0, static_cast<float>(i) * 1.5f, 0.5f})});
        }
        FrameStats stats;
        (void)rig.render(camera, draws, &stats);
        CHECK(stats.model_instances == 7);
        const ResourceStats after = rig.renderer->registry().total_stats();
        CHECK(after.created == before.created);  // drawing creates nothing: seven instances, one set of buffers
        CHECK(after.live == before.live);
    }
    CHECK(rig.device->validation_error_count() == 0);
}

// ------------------------------------------------------------------------------------------ material slots (M1)
namespace {

/// A single-texel texture of the given format.
TextureRef texel_texture(Rig& rig, rhi::Format format, uint8_t r, uint8_t g, uint8_t b, const char* name)
{
    rhi::TextureDesc td;
    td.format = format;
    td.width = td.height = 1;
    td.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDst;
    td.debug_name = name;
    const std::byte texel[4] = {std::byte{r}, std::byte{g}, std::byte{b}, std::byte{255}};
    return rig.renderer->registry().create_texture(td, texel);
}

/// The shader's ambient term alone for a normal (no sun), to separate what occlusion scales.
Float3 ambient_k(Float3 n, const RendererSettings& s)
{
    const float t = n.z * 0.5f + 0.5f;
    const Float3 ground{0.20f, 0.18f, 0.16f};
    const Float3 sky{s.sky.r * 0.6f, s.sky.g * 0.6f, s.sky.b * 0.6f};
    return ground * (1.0f - t) + sky * t;
}

/// Pixels of the quad's interior (inset from its edges) that are neither texel color of the checker: blended by filtering.
struct CheckerPurity {
    size_t pure = 0;
    size_t blended = 0;
};
CheckerPurity checker_purity(const Image& img, const Mat4& vp)
{
    CheckerPurity out;
    const Pixel a = to_pixel(vp, {0, +0.9f, 2.4f});
    const Pixel b = to_pixel(vp, {0, -0.9f, 0.6f});
    for (int y = a.y; y <= b.y; ++y) {
        for (int x = a.x; x <= b.x; ++x) {
            const Float3 c = linear_at(img, {x, y});
            if (near3(c, texel_linear(true), 0.03f) || near3(c, texel_linear(false), 0.03f)) {
                ++out.pure;
            } else {
                ++out.blended;
            }
        }
    }
    return out;
}

} // namespace

TEST_CASE("Models: the emissive slot reads its own texture as sRGB through its own sampler")
{
    Rig rig("model_slot_emissive", ModelShading::Extended);
    {
        const Camera camera = look_at_camera({-4, 0, 1.5f}, {0, 0, 1.5f});
        const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
        GpuResourceRegistry& registry = rig.renderer->registry();
        rhi::SamplerDesc linear_desc;  // magnified linearly, clamped: blends texels at cell edges
        linear_desc.address_u = linear_desc.address_v = rhi::AddressMode::ClampToEdge;
        const SamplerRef linear = registry.get_sampler(linear_desc);
        const SamplerRef nearest = nearest_sampler(rig);

        // Black base color, so the pixel is the emissive texture times the factor and nothing else (no lighting term).
        MaterialData data;
        data.base_color[0] = data.base_color[1] = data.base_color[2] = 0.0f;
        data.emissive[0] = data.emissive[1] = data.emissive[2] = 1.0f;

        // Emissive: nearest. Base color: linear. Every pixel must be one of the two texel colors, decoded from sRGB.
        MaterialTextures a;
        a[MaterialSlot::Emissive] = {checker_texture(rig), nearest};
        a[MaterialSlot::BaseColor] = {checker_texture(rig), linear};
        const GpuModel quad_a = make_quad(registry, rig.renderer->model_renderer().create_material(data, a, "emissive.nearest"), 0.0f, 1.5f);
        const ModelDraw draw_a{&quad_a, Mat4::identity()};
        const CheckerPurity pa = checker_purity(rig.render(camera, std::span(&draw_a, 1)), vp);
        CHECK(pa.pure > 1000);
        CHECK(pa.blended == 0);

        // Swapped: the emissive slot's sampler is now linear while the base color slot is nearest. The emissive texture
        // must follow its own sampler, so cell edges blend. (If the shader used one sampler for both, this would be 0.)
        MaterialTextures b;
        b[MaterialSlot::Emissive] = {checker_texture(rig), linear};
        b[MaterialSlot::BaseColor] = {checker_texture(rig), nearest};
        const GpuModel quad_b = make_quad(registry, rig.renderer->model_renderer().create_material(data, b, "emissive.linear"), 0.0f, 1.5f);
        const ModelDraw draw_b{&quad_b, Mat4::identity()};
        const CheckerPurity pb = checker_purity(rig.render(camera, std::span(&draw_b, 1)), vp);
        CHECK(pb.blended > 20);
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Models: the occlusion slot scales only the ambient term, read as linear data")
{
    Rig rig("model_slot_occlusion", ModelShading::Extended);
    {
        const Camera camera = look_at_camera({-4, 0, 1.5f}, {0, 0, 1.5f});
        const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
        GpuResourceRegistry& registry = rig.renderer->registry();
        const RendererSettings s = model_settings();
        const Float3 n{-1, 0, 0};
        const Float3 full = lighting_k(n, s);
        const Float3 ambient = ambient_k(n, s);
        const Float3 sun{full.x - ambient.x, full.y - ambient.y, full.z - ambient.z};
        const Pixel center = to_pixel(vp, {0, 0, 1.5f});

        // R = 128 in a *linear* texture is 0.502. If it were read as sRGB it would be 0.216: far enough apart to tell.
        const TextureRef occlusion = texel_texture(rig, rhi::Format::RGBA8_UNORM, 128, 128, 128, "test.occlusion");
        const float occ = 128.0f / 255.0f;
        for (const float strength : {1.0f, 0.5f, 0.0f}) {
            MaterialData data;  // white albedo, so the lit color is exactly the lighting term
            data.occlusion_strength = strength;
            MaterialTextures textures;
            textures[MaterialSlot::Occlusion] = {occlusion, {}};
            const GpuModel quad = make_quad(registry, rig.renderer->model_renderer().create_material(data, textures, "occ"), 0.0f, 1.5f);
            const ModelDraw draw{&quad, Mat4::identity()};
            const Float3 got = linear_at(rig.render(camera, std::span(&draw, 1)), center);
            const float scale = 1.0f + (occ - 1.0f) * strength;
            const Float3 want{ambient.x * scale + sun.x, ambient.y * scale + sun.y, ambient.z * scale + sun.z};
            CHECK(near3(got, want, 0.015f));
            if (strength == 1.0f) {
                CHECK(near3(got, full, 0.05f) == false);  // not vacuous: occlusion visibly darkens
            }
            if (strength == 0.0f) {
                CHECK(near3(got, full, 0.015f));  // strength 0 ignores the texture
            }
        }
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Models: a material with empty slots renders exactly like one with explicit neutral textures")
{
    Rig rig("model_slot_neutral");
    {
        const Camera camera = look_at_camera({-4, 0, 1.5f}, {0, 0, 1.5f});
        GpuResourceRegistry& registry = rig.renderer->registry();
        MaterialData data;
        data.base_color[0] = 0.7f; data.base_color[1] = 0.4f; data.base_color[2] = 0.2f;
        data.emissive[0] = 0.05f;
        const GpuModel plain = make_quad(registry, rig.material(data), 0.0f, 1.5f);
        MaterialTextures explicit_neutral;
        explicit_neutral[MaterialSlot::BaseColor].texture = texel_texture(rig, rhi::Format::RGBA8_SRGB, 255, 255, 255, "n.base");
        explicit_neutral[MaterialSlot::MetallicRoughness].texture = texel_texture(rig, rhi::Format::RGBA8_UNORM, 255, 255, 255, "n.mr");
        explicit_neutral[MaterialSlot::Normal].texture = texel_texture(rig, rhi::Format::RGBA8_UNORM, 128, 128, 255, "n.normal");
        explicit_neutral[MaterialSlot::Occlusion].texture = texel_texture(rig, rhi::Format::RGBA8_UNORM, 255, 255, 255, "n.occ");
        explicit_neutral[MaterialSlot::Emissive].texture = texel_texture(rig, rhi::Format::RGBA8_SRGB, 255, 255, 255, "n.emissive");
        const GpuModel spelled = make_quad(registry, rig.renderer->model_renderer().create_material(data, explicit_neutral, "n"), 0.0f, 1.5f);
        const ModelDraw a{&plain, Mat4::identity()};
        const ModelDraw b{&spelled, Mat4::identity()};
        CHECK(rig.render(camera, std::span(&a, 1)).rgba == rig.render(camera, std::span(&b, 1)).rgba);
    }
    CHECK(rig.device->validation_error_count() == 0);
}

TEST_CASE("Models (Legacy, the default): populated emissive and occlusion textures leave a 0.2 scene exactly as it was")
{
    // A 0.2 material with an emissive factor, plus an emissive texture and an occlusion texture that 0.2 never read. In the default
    // Legacy mode the image must equal the same material without those textures, and equal the 0.2 lighting oracle: albedo * K plus
    // the emissive factor. In Extended mode the extra textures do change it (so the comparison is not vacuous).
    for (const ModelShading shading : {ModelShading::Legacy, ModelShading::Extended}) {
        Rig rig("model_legacy_compat", shading);
        {
            const Camera camera = look_at_camera({-4, 0, 1.5f}, {0, 0, 1.5f});
            const Mat4 vp = view_projection_matrix(camera, static_cast<float>(kW) / static_cast<float>(kH));
            GpuResourceRegistry& registry = rig.renderer->registry();
            MaterialData data;
            data.base_color[0] = 0.6f; data.base_color[1] = 0.5f; data.base_color[2] = 0.4f;
            data.emissive[0] = 0.05f; data.emissive[1] = 0.02f; data.emissive[2] = 0.01f;
            MaterialTextures extras;
            extras[MaterialSlot::Emissive].texture = checker_texture(rig);   // an sRGB texture with values the factor would not hide
            extras[MaterialSlot::Occlusion].texture = texel_texture(rig, rhi::Format::RGBA8_UNORM, 64, 64, 64, "test.occ");
            const GpuModel plain = make_quad(registry, rig.renderer->model_renderer().create_material(data, MaterialTextures{}, "plain"), 0.0f, 1.5f);
            const GpuModel populated = make_quad(registry, rig.renderer->model_renderer().create_material(data, extras, "populated"), 0.0f, 1.5f);
            const ModelDraw a{&plain, Mat4::identity()};
            const ModelDraw b{&populated, Mat4::identity()};
            const Image image_plain = rig.render(camera, std::span(&a, 1));
            const Image image_populated = rig.render(camera, std::span(&b, 1));
            if (shading == ModelShading::Legacy) {
                CHECK(image_plain.rgba == image_populated.rgba);
                const Float3 k = lighting_k({-1, 0, 0}, model_settings());
                const Float3 want{0.6f * k.x + 0.05f, 0.5f * k.y + 0.02f, 0.4f * k.z + 0.01f};  // the 0.2 formula
                CHECK(near3(linear_at(image_populated, to_pixel(vp, {0, 0, 1.5f})), want, 0.012f));
            } else {
                CHECK(image_plain.rgba != image_populated.rgba);
            }
        }
        CHECK(rig.device->validation_error_count() == 0);
    }
}
