// saige-shaderc — content-addressed cache keys for compiled shader blobs.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "saige/shaderc/annotations.h"

namespace saige::shaderc {

/// Builds a 128-bit key from named, length-prefixed fields, so ("ab", "c") and ("a", "bc") never collide.
///
/// The hash is two decorrelated 64-bit FNV-1a lanes with a final avalanche mix. It needs no external
/// dependencies. It will move to XXH3-128 or BLAKE3 once the third-party policy (D-03) is decided; the
/// key format is versioned via `kCacheKeyVersion`.
class CacheKeyBuilder {
public:
    CacheKeyBuilder();

    CacheKeyBuilder& add(std::string_view field, std::string_view value);
    CacheKeyBuilder& add(std::string_view field, uint64_t value);

    /// 32 lowercase hex characters.
    [[nodiscard]] std::string hex() const;
    [[nodiscard]] std::pair<uint64_t, uint64_t> value() const;

private:
    void mixBytes(std::string_view bytes);
    void mixU64(uint64_t v);

    uint64_t m_a;
    uint64_t m_b;
};

inline constexpr uint32_t kCacheKeyVersion = 1;

/// Everything that affects the compiled output of one permutation of one entry point.
struct CompileRequest {
    std::string_view source;
    std::span<const std::string> includeContents;  ///< Contents of resolved includes, in scan order.
    std::string_view entryPoint;
    ShaderStage stage = ShaderStage::Vertex;
    std::span<const std::pair<std::string, std::string>> defines;
    std::string_view target;            ///< e.g. "spirv-vk13"
    std::string_view compilerVersion;   ///< e.g. dxc --version output
    std::string_view toolVersion;       ///< saige-shaderc version
    std::span<const std::string> extraArgs;  ///< Optimization level, debug info and similar flags.
};

[[nodiscard]] std::string computeCacheKey(const CompileRequest& request);

} // namespace saige::shaderc
