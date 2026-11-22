#include "saige/gfx_test/saige_test.hpp"

#include "saige/rhi/api_negotiation.hpp"

#include <array>
#include <string>

using namespace saige::rhi;

namespace {

constexpr ApiVersion v12{1, 2};
constexpr ApiVersion v13{1, 3};
constexpr ApiVersion v14{1, 4};
constexpr ApiVersion v15{1, 5};

std::array<FeatureQuery, optional_feature_count> all_queries(bool bit, bool extension)
{
    std::array<FeatureQuery, optional_feature_count> q{};
    for (FeatureQuery& f : q) {
        f.feature_bit = bit;
        f.extension_present = extension;
    }
    return q;
}

} // namespace

TEST_CASE("API negotiation: the instance version is min(loader, cap, implemented) and never below the baseline")
{
    CHECK(*negotiate_instance_version(v13, v13) == v13);
    CHECK(*negotiate_instance_version(v14, v13) == v13);   // the default cap keeps a 1.4 loader on 1.3
    CHECK(*negotiate_instance_version(v14, v14) == v14);
    CHECK(*negotiate_instance_version(v13, v14) == v13);   // a 1.3 loader cannot be asked for 1.4
    CHECK(*negotiate_instance_version(v15, v15) == v14);   // never more than the backend has a path for
    CHECK(*negotiate_instance_version(ApiVersion{2, 0}, v14) == v14);
    CHECK_FALSE(negotiate_instance_version(v12, v14).has_value());  // loader too old
    CHECK_FALSE(negotiate_instance_version(v14, v12).has_value());  // a cap below the baseline cannot be honored
    CHECK_FALSE(negotiate_instance_version(ApiVersion{}, v14).has_value());
}

TEST_CASE("API negotiation: a device is only as new as its instance and itself")
{
    CHECK(effective_device_version(v14, v14) == v14);
    CHECK(effective_device_version(v14, v13) == v13);  // 1.4 loader, 1.3 device
    CHECK(effective_device_version(v13, v14) == v13);  // 1.4 device behind a 1.3 instance: no 1.4 core use
    CHECK(effective_device_version(v13, v13) == v13);
}

TEST_CASE("API negotiation: a feature needs a version or an extension and its feature bit")
{
    // 1.4 device, bits set: everything promoted is core; scalar block layout is core since 1.2.
    CapabilityReport r = negotiate_capabilities(v14, all_queries(true, false));
    for (std::size_t i = 0; i < optional_feature_count; ++i) {
        CHECK(r.sources[i] == FeatureSource::CoreVersion);
    }

    // 1.3 device with the extensions advertised and bits set: promoted features arrive as extensions, scalar layout as core.
    r = negotiate_capabilities(v13, all_queries(true, true));
    CHECK(r.source(OptionalFeature::ScalarBlockLayout) == FeatureSource::CoreVersion);
    CHECK(r.source(OptionalFeature::PushDescriptor) == FeatureSource::Extension);
    CHECK(r.source(OptionalFeature::HostImageCopy) == FeatureSource::Extension);
    CHECK(r.source(OptionalFeature::Maintenance5) == FeatureSource::Extension);

    // 1.3 device without the extensions: fall back to the baseline path for everything promoted.
    r = negotiate_capabilities(v13, all_queries(true, false));
    CHECK_FALSE(r.has(OptionalFeature::PushDescriptor));
    CHECK_FALSE(r.has(OptionalFeature::DynamicRenderingLocalRead));
    CHECK_FALSE(r.has(OptionalFeature::HostImageCopy));
    CHECK(r.has(OptionalFeature::ScalarBlockLayout));  // core in 1.2, so a 1.3 device with the bit has it

    // The feature bit decides: the version alone and the extension alone are never enough.
    r = negotiate_capabilities(v14, all_queries(false, true));
    for (std::size_t i = 0; i < optional_feature_count; ++i) {
        CHECK(r.sources[i] == FeatureSource::None);
    }
    r = negotiate_capabilities(v13, all_queries(false, true));
    for (std::size_t i = 0; i < optional_feature_count; ++i) {
        CHECK(r.sources[i] == FeatureSource::None);
    }

    // Features are independent: one clear bit on a 1.4 device removes only that feature.
    std::array<FeatureQuery, optional_feature_count> q = all_queries(true, true);
    q[static_cast<std::size_t>(OptionalFeature::HostImageCopy)].feature_bit = false;
    r = negotiate_capabilities(v14, q);
    CHECK_FALSE(r.has(OptionalFeature::HostImageCopy));
    CHECK(r.has(OptionalFeature::PushDescriptor));
    CHECK(r.has(OptionalFeature::Maintenance6));

    // A 1.4 device behind a 1.3 instance is evaluated at its effective version: extension path, not core.
    r = negotiate_capabilities(effective_device_version(v13, v14), all_queries(true, true));
    CHECK(r.source(OptionalFeature::PushDescriptor) == FeatureSource::Extension);
}

TEST_CASE("API negotiation: names and core versions are stable")
{
    CHECK(to_string(v13) == "1.3");
    CHECK(to_string(v14) == "1.4");
    CHECK(core_version(OptionalFeature::ScalarBlockLayout) == v12);
    CHECK(core_version(OptionalFeature::PushDescriptor) == v14);
    CHECK(std::string(to_string(OptionalFeature::HostImageCopy)) == "host_image_copy");
    CHECK(std::string(to_string(FeatureSource::Extension)) == "extension");
    // Every feature has a distinct, non-"unknown" name.
    for (std::size_t i = 0; i < optional_feature_count; ++i) {
        CHECK(std::string(to_string(static_cast<OptionalFeature>(i))) != "unknown");
    }
}
