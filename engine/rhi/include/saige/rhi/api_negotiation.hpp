// Graphics API version and optional-capability negotiation, independent of any backend.
//
// Three versions exist and must not be conflated: what the *loader* offers, what the *instance* was asked for, and what each
// *physical device* reports. The rules below are pure functions of those numbers plus queried feature bits, so every
// fallback case can be tested without a device. Policy and rationale: docs/rendering/vulkan-adapter-matrix.md (section 2).
//
// The baseline never changes here: a device below `api_baseline` is unusable, and nothing above it is ever required.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace saige::rhi {

struct ApiVersion {
    uint32_t major = 0;
    uint32_t minor = 0;

    friend constexpr bool operator==(const ApiVersion& a, const ApiVersion& b) noexcept
    {
        return a.major == b.major && a.minor == b.minor;
    }
    friend constexpr bool operator<(const ApiVersion& a, const ApiVersion& b) noexcept
    {
        return a.major != b.major ? a.major < b.major : a.minor < b.minor;
    }
    friend constexpr bool operator>(const ApiVersion& a, const ApiVersion& b) noexcept { return b < a; }
    friend constexpr bool operator<=(const ApiVersion& a, const ApiVersion& b) noexcept { return !(b < a); }
    friend constexpr bool operator>=(const ApiVersion& a, const ApiVersion& b) noexcept { return !(a < b); }
};

/// The lowest API version the backend runs on. Raising it is a review decision, never a side effect of a build.
inline constexpr ApiVersion api_baseline{1, 3};
/// The highest API version the backend has a code path for. The instance is never asked for more, whatever the loader offers.
inline constexpr ApiVersion api_implemented{1, 4};

[[nodiscard]] constexpr ApiVersion min_version(ApiVersion a, ApiVersion b) noexcept { return a < b ? a : b; }

[[nodiscard]] std::string to_string(ApiVersion version);

/// The instance version to request: `min(loader, cap, api_implemented)`. nullopt if that is below `api_baseline` (the loader
/// is too old, or the cap asks for less than the backend can run on).
[[nodiscard]] std::optional<ApiVersion> negotiate_instance_version(ApiVersion loader, ApiVersion cap) noexcept;

/// What a device can be treated as: `min(instance, device)`. A 1.4 loader with a 1.3 device is a 1.3 device, and a 1.4 device
/// behind a 1.3 instance is also 1.3: core functionality newer than the instance version must not be used.
[[nodiscard]] ApiVersion effective_device_version(ApiVersion instance, ApiVersion device) noexcept;

/// Capabilities above the baseline that the backend can probe. Probing enables nothing: a feature is only ever *enabled* by
/// a change that gives it a user and a test.
enum class OptionalFeature : uint8_t {
    PushDescriptor,
    DynamicRenderingLocalRead,
    HostImageCopy,
    Maintenance5,
    Maintenance6,
    ScalarBlockLayout,
    IndexTypeUint8,
    LineRasterization,
};
inline constexpr std::size_t optional_feature_count = 8;

[[nodiscard]] const char* to_string(OptionalFeature feature) noexcept;
/// The first core version that includes the feature (1.2 for scalar block layout, 1.4 for the rest).
[[nodiscard]] ApiVersion core_version(OptionalFeature feature) noexcept;

/// Where a feature comes from on a particular device.
enum class FeatureSource : uint8_t {
    None,         ///< unavailable: use the baseline path
    CoreVersion,  ///< part of the core API at the effective version and its feature bit is set
    Extension,    ///< below the core version, but the promoted extension is present and its feature bit is set
};
[[nodiscard]] const char* to_string(FeatureSource source) noexcept;

/// What the backend queried for one feature.
struct FeatureQuery {
    bool feature_bit = false;       ///< the device reports the feature (from the core struct or the extension's struct)
    bool extension_present = false; ///< the promoted extension is advertised by the device
};

struct CapabilityReport {
    std::array<FeatureSource, optional_feature_count> sources{};  ///< indexed by OptionalFeature; default None

    [[nodiscard]] FeatureSource source(OptionalFeature feature) const noexcept
    {
        return sources[static_cast<std::size_t>(feature)];
    }
    [[nodiscard]] bool has(OptionalFeature feature) const noexcept { return source(feature) != FeatureSource::None; }
};

/// CoreVersion if `effective >= core_version(feature)` and the feature bit is set; otherwise Extension if the extension is
/// present and the feature bit is set; otherwise None. Never decided by the API version alone, and never by the extension's
/// presence alone: both a version or extension *and* the feature bit are required.
[[nodiscard]] CapabilityReport negotiate_capabilities(ApiVersion effective,
                                                      const std::array<FeatureQuery, optional_feature_count>& queries) noexcept;

} // namespace saige::rhi
