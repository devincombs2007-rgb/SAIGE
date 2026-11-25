// API version negotiation and the read-only capability probe on a real device. The pure rules are tested without a device in
// engine/rhi/tests/test_api_negotiation.cpp; this checks that the Vulkan backend feeds them correctly, that the default path
// is unchanged (Vulkan 1.3), that opting in only probes, and that the baseline path stays usable and renders identically.

#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_test/image.hpp"
#include "saige/render/camera.hpp"
#include "saige/render/scene_renderer.hpp"
#include "saige/rhi/api_negotiation.hpp"
#include "saige/rhi_vulkan/vulkan_device.hpp"
#include "vulkan_test_util.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace saige;
using namespace saige::render;

namespace {

std::unique_ptr<rhi::Device> make_device(rhi::ApiVersion cap, const char* name)
{
    rhi::DeviceDesc desc;
    desc.enable_validation = true;
    const char* require = std::getenv("SAIGE_REQUIRE_VALIDATION");
    desc.require_validation = require != nullptr && require[0] != '\0' && require[0] != '0';
    desc.application_name = name;
    desc.max_api_version = cap;
    return rhi::create_vulkan_device(desc);
}

void print_report(const char* label, const rhi::DeviceInfo& info)
{
    std::printf("  %s: loader %s, instance %s, device %u.%u, effective %s on '%s'\n", label,
                rhi::to_string(info.loader_api_version).c_str(), rhi::to_string(info.instance_api_version).c_str(),
                info.api_version_major, info.api_version_minor, rhi::to_string(info.effective_api_version).c_str(),
                info.device_name.c_str());
    for (std::size_t i = 0; i < rhi::optional_feature_count; ++i) {
        const auto f = static_cast<rhi::OptionalFeature>(i);
        std::printf("    %-28s %s\n", rhi::to_string(f), rhi::to_string(info.capabilities.source(f)));
    }
}

/// The invariants any report must satisfy, whatever the device: a feature is only core at or above its core version.
void check_report_invariants(const rhi::DeviceInfo& info)
{
    CHECK(info.effective_api_version == rhi::effective_device_version(
                                            info.instance_api_version, rhi::ApiVersion{info.api_version_major, info.api_version_minor}));
    CHECK(info.instance_api_version >= rhi::api_baseline);
    CHECK(info.instance_api_version <= rhi::api_implemented);
    CHECK(info.instance_api_version <= info.loader_api_version);
    for (std::size_t i = 0; i < rhi::optional_feature_count; ++i) {
        const auto f = static_cast<rhi::OptionalFeature>(i);
        if (info.capabilities.source(f) == rhi::FeatureSource::CoreVersion) {
            CHECK(info.effective_api_version >= rhi::core_version(f));
        }
        if (info.capabilities.source(f) == rhi::FeatureSource::Extension) {
            CHECK(info.effective_api_version < rhi::core_version(f));
        }
    }
}

} // namespace

TEST_CASE("Vulkan capabilities: the default request stays on the 1.3 baseline and promotes nothing from 1.4")
{
    const auto device = make_device(rhi::api_baseline, "caps_default");
    const rhi::DeviceInfo& info = device->info();
    print_report("default", info);
    CHECK(info.instance_api_version == rhi::api_baseline);  // a newer loader is not asked for more than the cap
    CHECK(info.effective_api_version == rhi::api_baseline);
    check_report_invariants(info);
    // At an effective 1.3, only scalar block layout (core since 1.2) can be a core feature.
    for (std::size_t i = 0; i < rhi::optional_feature_count; ++i) {
        const auto f = static_cast<rhi::OptionalFeature>(i);
        if (f != rhi::OptionalFeature::ScalarBlockLayout) {
            CHECK(info.capabilities.source(f) != rhi::FeatureSource::CoreVersion);
        }
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan capabilities: opting in to 1.4 negotiates per loader and per device, and only probes")
{
    const auto device = make_device(rhi::ApiVersion{1, 4}, "caps_opt_in");
    const rhi::DeviceInfo& info = device->info();
    print_report("opt-in 1.4", info);
    check_report_invariants(info);
    // The instance version is exactly min(loader, 1.4): a loader below 1.4 yields the fallback, not an error.
    CHECK(info.instance_api_version == rhi::min_version(info.loader_api_version, rhi::ApiVersion{1, 4}));
    if (info.loader_api_version >= rhi::ApiVersion{1, 4} && rhi::ApiVersion{info.api_version_major, info.api_version_minor} >= rhi::ApiVersion{1, 4}) {
        CHECK(info.effective_api_version == rhi::ApiVersion{1, 4});
    } else {
        std::printf("  note: loader or device below 1.4; this run exercised the fallback negotiation\n");
        CHECK(info.effective_api_version < rhi::ApiVersion{1, 4});
    }
    device->wait_idle();
    CHECK(device->validation_error_count() == 0);
}

TEST_CASE("Vulkan capabilities: a cap below the baseline is refused with an actionable message")
{
    std::string message;
    try {
        (void)make_device(rhi::ApiVersion{1, 2}, "caps_below_baseline");
    } catch (const std::runtime_error& e) {
        message = e.what();
    }
    CHECK(message.find("below the supported baseline") != std::string::npos);
    CHECK(message.find("1.2") != std::string::npos);
}

TEST_CASE("Vulkan capabilities: the same scene renders byte-identically on the 1.3 path and with 1.4 opted in")
{
    const auto render = [](rhi::ApiVersion cap, const char* name) {
        const auto device = make_device(cap, name);
        test::Image image;
        {
            test::OffscreenTarget target(*device, 160, 90);
            SceneRenderer renderer(*device, rhi::Format::RGBA8_SRGB);
            SceneInstance a;
            a.position = {0, 0, 0};
            a.color = {0.9f, 0.08f, 0.05f};
            SceneInstance b;
            b.position = {3, 1, 0};
            b.color = {0.05f, 0.2f, 0.95f};
            b.scale = 1.5f;
            const Camera camera = look_at_camera({-7.0f, -6.0f, 7.0f}, {1.0f, 0.5f, 0.0f}, {0, 0, 1}, 55.0f * pi / 180.0f, 0.1f);
            image = target.render(renderer, {a, b}, camera, 11);
        }
        device->wait_idle();
        CHECK(device->validation_error_count() == 0);
        return image;
    };
    const test::Image baseline = render(rhi::api_baseline, "caps_render_13");
    const test::Image opted_in = render(rhi::ApiVersion{1, 4}, "caps_render_14");
    CHECK(baseline.width == opted_in.width);
    CHECK(baseline.rgba == opted_in.rgba);  // probing changes nothing about what is drawn
}
