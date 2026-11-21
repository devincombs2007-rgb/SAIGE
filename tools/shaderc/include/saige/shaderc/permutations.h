// saige-shaderc — permutation expansion and permutation keys.
#pragma once

#include "saige/shaderc/annotations.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace saige::shaderc {

struct Permutation {
    uint64_t key = 0;         ///< Stable bitfield of value indices (see PermutationOption::bitOffset).
    std::vector<int> values;  ///< Chosen value per option, in declaration order.
};

struct ExpandResult {
    std::vector<Permutation> permutations;  ///< Deterministic order: last option varies fastest.
    uint64_t excludedCount = 0;
    std::vector<std::string> errors;
    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

/// Hard cap on the raw (pre-exclusion) permutation space, to reject accidental explosions early.
inline constexpr uint64_t kMaxRawPermutations = 1u << 20;

/// Expands the cartesian product of all options, removes `@exclude`d combinations, and enforces `@budget`.
[[nodiscard]] ExpandResult expandPermutations(const ShaderDecl& decl);

/// Encodes per-option *value indices* into a key. Returns nullopt if an index is out of range.
[[nodiscard]] std::optional<uint64_t> encodeKey(const ShaderDecl& decl, std::span<const uint32_t> valueIndices);

/// Decodes a key into per-option values. Returns nullopt if the key is malformed for this declaration.
[[nodiscard]] std::optional<std::vector<int>> decodeKey(const ShaderDecl& decl, uint64_t key);

/// Preprocessor defines for a permutation: {name, value} in option order.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> definesFor(const ShaderDecl& decl,
                                                                           const Permutation& permutation);

} // namespace saige::shaderc
