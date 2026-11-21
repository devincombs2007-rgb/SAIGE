#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace saige {

struct AssetId {
    std::uint64_t high{};
    std::uint64_t low{};

    [[nodiscard]] constexpr bool is_builtin() const noexcept { return high == 0 && low == 0; }
    constexpr bool operator==(const AssetId&) const = default;
};

// Stable persisted form: lowercase hex in 8-4-4-4-12 groups, most significant
// digits first. IDs are opaque; this encoding imposes no UUID version bits.
[[nodiscard]] std::string format_asset_id(AssetId id);
[[nodiscard]] std::optional<AssetId> parse_asset_id(std::string_view value) noexcept;

} // namespace saige
