#include "saige/asset_id.hpp"

#include <iostream>
#include <string_view>

int main() {
    constexpr saige::AssetId id{0x0123456789abcdefULL, 0xfedcba9876543210ULL};
    constexpr auto encoded = "01234567-89ab-cdef-fedc-ba9876543210";
    const auto decoded = saige::parse_asset_id(encoded);
    if (decoded != id || saige::format_asset_id(id) != encoded || id.is_builtin()) {
        std::cerr << "Asset ID did not preserve both words in canonical order\n";
        return 1;
    }
    const auto builtin = saige::parse_asset_id("00000000-0000-0000-0000-000000000000");
    if (!builtin || !builtin->is_builtin()) {
        std::cerr << "The reserved built-in asset ID did not round-trip\n";
        return 1;
    }
    for (const std::string_view malformed : {
             "01234567-89ab-cdef-fedc-ba987654321",
             "01234567_89ab-cdef-fedc-ba9876543210",
             "01234567-89Ab-cdef-fedc-ba9876543210",
             "01234567-89ab-cdef-fedc-ba987654321g"}) {
        if (saige::parse_asset_id(malformed)) {
            std::cerr << "Accepted a noncanonical asset ID\n";
            return 1;
        }
    }
}
