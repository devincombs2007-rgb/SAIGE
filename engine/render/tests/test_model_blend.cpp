// Blend support decisions of the model renderer that need no GPU: which scene formats blend, what a blended material becomes where
// they do not, and that blend pipelines are created only when a blended material exists. Pixel behavior is tested on Vulkan
// (engine/rhi_vulkan/tests/test_blend_render.cpp).
#include "saige/gfx_test/saige_test.hpp"

#include "saige/render/model_renderer.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/rhi_null/null_device.hpp"

using namespace saige;
using namespace saige::render;

namespace {

struct Fixture {
    rhi::NullDevice device;
    GpuResourceRegistry registry{device};
};

ModelRenderSettings blending_on()
{
    ModelRenderSettings s;
    s.alpha_blending = true;
    return s;
}

} // namespace

TEST_CASE("ModelRenderer: scene formats that blend are blended, others fall back to opaque")
{
    for (const rhi::Format format : {rhi::Format::RGBA8_SRGB, rhi::Format::RGBA8_UNORM, rhi::Format::BGRA8_SRGB, rhi::Format::RGBA16_FLOAT}) {
        Fixture f;
        {
            ModelRenderer models(f.device, f.registry, format, rhi::Format::D32_FLOAT, blending_on());
            CHECK(models.blending_supported());
            MaterialData glass;
            glass.alpha_blend = true;
            CHECK(models.create_material(glass, MaterialTextures{}, "glass").blend);
            CHECK_FALSE(models.create_material(MaterialData{}, MaterialTextures{}, "solid").blend);
        }
    }
    // 32-bit float scene targets are outside the portable blend subset: a blended material draws opaque (staging warns about it).
    Fixture f;
    {
        ModelRenderer models(f.device, f.registry, rhi::Format::RGBA32_FLOAT, rhi::Format::D32_FLOAT, blending_on());
        CHECK_FALSE(models.blending_supported());
        MaterialData glass;
        glass.alpha_blend = true;
        const GpuMaterial m = models.create_material(glass, MaterialTextures{}, "glass");
        CHECK_FALSE(m.blend);
    }
    CHECK(f.device.validation_error_count() == 0);
}

TEST_CASE("ModelRenderer: blend pipelines exist only once a blended material does, and are released with the renderer")
{
    Fixture f;
    const auto live = [&] { return f.device.live_resource_count(); };
    const uint32_t before = live();
    {
        ModelRenderer models(f.device, f.registry, rhi::Format::RGBA8_SRGB, rhi::Format::D32_FLOAT, blending_on());
        // A material costs one uniform buffer; the first blended one also creates the four blend pipelines
        // (mirrored x front-face pass and back-face pass), once.
        const uint32_t base = live();
        const GpuMaterial solid = models.create_material(MaterialData{}, MaterialTextures{}, "solid");
        const uint32_t per_material = live() - base;
        CHECK(per_material >= 1);
        MaterialData glass;
        glass.alpha_blend = true;
        const uint32_t before_glass = live();
        const GpuMaterial a = models.create_material(glass, MaterialTextures{}, "glass 1");
        CHECK(live() - before_glass == per_material + 4);
        const uint32_t before_second = live();
        const GpuMaterial b = models.create_material(glass, MaterialTextures{}, "glass 2");
        CHECK(live() - before_second == per_material);  // the pipelines are shared
        CHECK(a.blend);
        CHECK(b.blend);
        CHECK_FALSE(solid.blend);
    }
    f.device.wait_idle();
    CHECK(live() == before);  // nothing leaks
    CHECK(f.device.validation_error_count() == 0);
}

TEST_CASE("ModelRenderer: alpha blending is off by default, so a BLEND material draws opaque as in 0.2 and no blend pipeline is created")
{
    CHECK_FALSE(ModelRenderSettings{}.alpha_blending);  // the defaults themselves are the compatibility promise
    CHECK_FALSE(RendererSettings{}.alpha_blending);
    Fixture f;
    const uint32_t before = f.device.live_resource_count();
    {
        ModelRenderer models(f.device, f.registry, rhi::Format::RGBA8_SRGB, rhi::Format::D32_FLOAT, {});  // default settings
        CHECK(models.blending_supported());  // the format could blend...
        CHECK_FALSE(models.alpha_blending()); // ...but nobody asked
        CHECK_FALSE(models.blending_enabled());
        const uint32_t base = f.device.live_resource_count();
        const GpuMaterial solid = models.create_material(MaterialData{}, MaterialTextures{}, "solid");
        const uint32_t per_material = f.device.live_resource_count() - base;
        MaterialData glass;
        glass.alpha_blend = true;
        const uint32_t before_glass = f.device.live_resource_count();
        const GpuMaterial m = models.create_material(glass, MaterialTextures{}, "glass");
        CHECK_FALSE(m.blend);
        CHECK(f.device.live_resource_count() - before_glass == per_material);  // only what a material costs: no blend pipelines
        CHECK_FALSE(solid.blend);
    }
    f.device.wait_idle();
    CHECK(f.device.live_resource_count() == before);
}
