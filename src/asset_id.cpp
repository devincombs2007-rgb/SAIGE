#include "saige/asset_id.hpp"

#include <array>

namespace saige {
namespace {
constexpr std::string_view hex = "0123456789abcdef";
constexpr std::string_view shape = "00000000-0000-0000-0000-000000000000";
} // namespace

std::string format_asset_id(AssetId id) {
    std::string result(shape);
    const std::array words{id.high, id.low};
    std::size_t digit = 0;
    for (char& character : result) {
        if (character == '-') {
            continue;
        }
        const auto word = words[digit / 16];
        const auto shift = 4 * (15 - digit % 16);
        character = hex[(word >> shift) & 0xF];
        ++digit;
    }
    return result;
}

std::optional<AssetId> parse_asset_id(std::string_view value) noexcept {
    if (value.size() != shape.size()) {
        return std::nullopt;
    }
    AssetId result;
    std::size_t digit = 0;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (shape[i] == '-') {
            if (value[i] != '-') {
                return std::nullopt;
            }
            continue;
        }
        const auto nibble = hex.find(value[i]);
        if (nibble == std::string_view::npos) {
            return std::nullopt;
        }
        auto& word = digit < 16 ? result.high : result.low;
        word = (word << 4) | nibble;
        ++digit;
    }
    return result;
}

} // namespace saige
