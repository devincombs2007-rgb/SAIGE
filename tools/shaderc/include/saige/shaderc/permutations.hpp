// saige-shaderc — permutation expansion and permutation keys.
#pragma once

#include "saige/shaderc/annotations.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace saige::shaderc {

struct Permutation {
    uint64_t key = 0;         ///< Stable bitfield of value indices (see PermutationOption::bit_offset).
    std::vector<int> values;  ///< Chosen value per option, in declaration order.
};

struct ExpandResult {
    std::vector<Permutation> permutations;  ///< Deterministic order: last option varies fastest.
    uint64_t excluded_count = 0;
    std::vector<std::string> errors;
    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

/// Hard cap on the raw (pre-exclusion) permutation space, to reject accidental explosions early.
inline constexpr uint64_t max_raw_permutations = 1u << 20;

/// Expands the cartesian product of all options, removes `@exclude`d combinations, and enforces `@budget`.
[[nodiscard]] ExpandResult expand_permutations(const ShaderDecl& decl);

/// Encodes per-option *value indices* into a key. Returns nullopt if an index is out of range.
[[nodiscard]] std::optional<uint64_t> encode_key(const ShaderDecl& decl, std::span<const uint32_t> value_indices);

/// Decodes a key into per-option values. Returns nullopt if the key is malformed for this declaration.
[[nodiscard]] std::optional<std::vector<int>> decode_key(const ShaderDecl& decl, uint64_t key);

/// Preprocessor defines for a permutation: {name, value} in option order.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> defines_for(const ShaderDecl& decl,
                                                                           const Permutation& permutation);

} // namespace saige::shaderc
