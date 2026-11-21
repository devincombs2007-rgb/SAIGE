// saige-shaderc — content-addressed cache keys for compiled shader blobs.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "saige/shaderc/annotations.hpp"

namespace saige::shaderc {

/// Builds a 128-bit key from named, length-prefixed fields, so ("ab", "c") and ("a", "bc") never collide.
///
/// The hash is two decorrelated 64-bit FNV-1a lanes with a final avalanche mix. It needs no external
/// dependencies. It will move to XXH3-128 or BLAKE3 once the third-party policy (D-03) is decided; the
/// key format is versioned via `cache_key_version`.
class CacheKeyBuilder {
public:
    CacheKeyBuilder();

    CacheKeyBuilder& add(std::string_view field, std::string_view value);
    CacheKeyBuilder& add(std::string_view field, uint64_t value);

    /// 32 lowercase hex characters.
    [[nodiscard]] std::string hex() const;
    [[nodiscard]] std::pair<uint64_t, uint64_t> value() const;

private:
    void mix_bytes(std::string_view bytes);
    void mix_u64(uint64_t v);

    uint64_t a_;
    uint64_t b_;
};

inline constexpr uint32_t cache_key_version = 1;

/// Everything that affects the compiled output of one permutation of one entry point.
struct CompileRequest {
    std::string_view source;
    std::span<const std::string> include_contents;  ///< Contents of resolved includes, in scan order.
    std::string_view entry_point;
    ShaderStage stage = ShaderStage::Vertex;
    std::span<const std::pair<std::string, std::string>> defines;
    std::string_view target;            ///< e.g. "spirv-vk13"
    std::string_view compiler_version;   ///< e.g. dxc --version output
    std::string_view tool_version;       ///< saige-shaderc version
    std::span<const std::string> extra_args;  ///< Optimization level, debug info and similar flags.
};

[[nodiscard]] std::string compute_cache_key(const CompileRequest& request);

} // namespace saige::shaderc
