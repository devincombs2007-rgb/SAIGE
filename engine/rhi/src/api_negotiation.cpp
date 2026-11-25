#include "saige/rhi/api_negotiation.hpp"

namespace saige::rhi {

std::string to_string(ApiVersion version)
{
    return std::to_string(version.major) + "." + std::to_string(version.minor);
}

std::optional<ApiVersion> negotiate_instance_version(ApiVersion loader, ApiVersion cap) noexcept
{
    const ApiVersion requested = min_version(min_version(loader, cap), api_implemented);
    if (requested < api_baseline) {
        return std::nullopt;
    }
    return requested;
}

ApiVersion effective_device_version(ApiVersion instance, ApiVersion device) noexcept
{
    return min_version(instance, device);
}

const char* to_string(OptionalFeature feature) noexcept
{
    switch (feature) {
    case OptionalFeature::PushDescriptor: return "push_descriptor";
    case OptionalFeature::DynamicRenderingLocalRead: return "dynamic_rendering_local_read";
    case OptionalFeature::HostImageCopy: return "host_image_copy";
    case OptionalFeature::Maintenance5: return "maintenance5";
    case OptionalFeature::Maintenance6: return "maintenance6";
    case OptionalFeature::ScalarBlockLayout: return "scalar_block_layout";
    case OptionalFeature::IndexTypeUint8: return "index_type_uint8";
    case OptionalFeature::LineRasterization: return "line_rasterization";
    }
    return "unknown";
}

ApiVersion core_version(OptionalFeature feature) noexcept
{
    return feature == OptionalFeature::ScalarBlockLayout ? ApiVersion{1, 2} : ApiVersion{1, 4};
}

const char* to_string(FeatureSource source) noexcept
{
    switch (source) {
    case FeatureSource::None: return "none";
    case FeatureSource::CoreVersion: return "core";
    case FeatureSource::Extension: return "extension";
    }
    return "unknown";
}

CapabilityReport negotiate_capabilities(ApiVersion effective,
                                        const std::array<FeatureQuery, optional_feature_count>& queries) noexcept
{
    CapabilityReport report;
    for (std::size_t i = 0; i < optional_feature_count; ++i) {
        const OptionalFeature feature = static_cast<OptionalFeature>(i);
        const FeatureQuery& q = queries[i];
        if (!q.feature_bit) {
            report.sources[i] = FeatureSource::None;
        } else if (effective >= core_version(feature)) {
            report.sources[i] = FeatureSource::CoreVersion;
        } else if (q.extension_present) {
            report.sources[i] = FeatureSource::Extension;
        } else {
            report.sources[i] = FeatureSource::None;
        }
    }
    return report;
}

} // namespace saige::rhi
