// Every optional post effect together on a validated Vulkan device with synchronization validation: ambient occlusion, bloom, tone map and FXAA, over the atmosphere, in
// the order scene -> AO -> bloom -> tone map -> FXAA. Each effect is switched off in turn to show it contributes; the full chain must be deterministic across frames in flight.
// Software Vulkan (lavapipe in CI): correctness evidence only.

#include "saige/gfx_test/saige_test.hpp"

#include "model_test_util.hpp"
#include "vulkan_test_util.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace saige;
using namespace saige::test;
using namespace saige::render;

namespace {

constexpr uint32_t kW = 160;
constexpr uint32_t kH = 120;

struct Chain {
    bool ao = true, bloom = true, fxaa = true;
};

struct Run {
    Image image, again;
    FrameStats stats;
};

Run render_chain(const Chain& c)
{
    const auto device = make_validated_device("post_chain", /*synchronization=*/true);
    RendererSettings s = deterministic_settings();
    s.hdr = true;
    s.tone_map.op = ToneMapOperator::AcesFitted;
    s.atmosphere.enabled = true;
    s.fog_density = 0.0f;
    s.ambient_occlusion.enabled = c.ao;
    s.ambient_occlusion.radius = 0.8f;
    s.ambient_occlusion.intensity = 1.5f;
    s.bloom.enabled = c.bloom;
    s.bloom.threshold = 0.6f;  // the sky and the sunlit faces are over it
    s.bloom.intensity = 0.4f;
    s.fxaa.enabled = c.fxaa;
    SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB, s);
    OffscreenTarget target(*device, kW, kH);
    std::vector<SceneInstance> cubes(3);
    cubes[0].position = {0.0f, 0.0f, 0.0f};
    cubes[1].position = {2.0f, 1.5f, 0.0f};
    cubes[1].color = {0.2f, 0.7f, 0.3f};
    cubes[1].yaw_offset = 0.6f;
    cubes[2].position = {-1.5f, -2.0f, 0.0f};
    cubes[2].color = {0.9f, 0.3f, 0.2f};
    const Camera camera = look_at_camera({-6.0f, -5.0f, 3.0f}, {0.0f, 0.0f, 0.6f});
    Run r;
    r.image = target.render(renderer, cubes, camera, 0, {}, &r.stats);
    (void)target.render(renderer, cubes, camera, 0, {});  // a second frame in flight
    r.again = target.render(renderer, cubes, camera, 0, {});
    CHECK(device->validation_error_count() == 0);
    return r;
}

uint32_t differing(const Image& a, const Image& b)
{
    uint32_t n = 0;
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) n += max_channel_delta(a.at(x, y), b.at(x, y)) > 2 ? 1u : 0u;
    return n;
}

}  // namespace

TEST_CASE("Post chain: ambient occlusion, bloom, tone map and FXAA together run in order, each contributes, and the result is deterministic across frames in flight")
{
    const Run all = render_chain({});
    CHECK(all.image.rgba == all.again.rgba);
    CHECK(all.stats.ao_draw_calls == 2);
    CHECK(all.stats.bloom_draw_calls >= 3);
    CHECK(all.stats.fxaa_draw_calls == 1);
    const Run no_ao = render_chain({false, true, true});
    const Run no_bloom = render_chain({true, false, true});
    const Run no_fxaa = render_chain({true, true, false});
    const Run none = render_chain({false, false, false});
    CHECK(no_ao.stats.ao_draw_calls == 0 && no_bloom.stats.bloom_draw_calls == 0 && no_fxaa.stats.fxaa_draw_calls == 0);
    const uint32_t d_ao = differing(all.image, no_ao.image), d_bloom = differing(all.image, no_bloom.image), d_fxaa = differing(all.image, no_fxaa.image), d_none = differing(all.image, none.image);
    std::printf("  pixels changed by removing AO %u, bloom %u, FXAA %u, all three %u (of %u)\n", d_ao, d_bloom, d_fxaa, d_none, kW * kH);
    CHECK(d_ao >= 60);
    CHECK(d_bloom >= 60);
    CHECK(d_fxaa >= 60);
    CHECK(d_none >= d_ao && d_none >= d_bloom && d_none >= d_fxaa);
}
