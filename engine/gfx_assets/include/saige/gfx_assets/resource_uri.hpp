// SAIGE gfx_assets — strict base64 and the resource-URI policy for imported assets (docs/rendering/gltf-subset.md).
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace saige::gfx {

/// Strict RFC 4648 base64: the standard alphabet, length a multiple of 4, '=' padding only at the end (at most two),
/// no whitespace. Returns nullopt for anything else.
[[nodiscard]] std::optional<std::vector<uint8_t>> decode_base64(std::string_view text);

struct DataUri {
    std::string mime_type;  ///< empty if the URI gave none
    std::vector<uint8_t> bytes;
};

/// True if the URI starts with "data:" (case-insensitive).
[[nodiscard]] bool is_data_uri(std::string_view uri);

/// Decodes `data:[<mime>];base64,<payload>`. Only base64 data URIs are accepted. Nullopt on any malformation.
[[nodiscard]] std::optional<DataUri> decode_data_uri(std::string_view uri);

struct ResolvedUri {
    bool ok = false;
    std::string path;   ///< normalized, '/'-separated relative path when ok
    std::string error;  ///< why it was rejected when !ok
};

/// Applies the resource-URI policy: percent-decode once, then reject empty, over-long (>1024), absolute, drive-letter or
/// scheme ('...:'), backslash, control-character, NUL, empty-segment and '..' paths. '.' segments are dropped.
[[nodiscard]] ResolvedUri resolve_relative_uri(std::string_view uri);

} // namespace saige::gfx
