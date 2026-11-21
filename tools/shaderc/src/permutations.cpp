#include "saige/shaderc/permutations.h"

namespace saige::shaderc {

std::optional<uint64_t> encodeKey(const ShaderDecl& decl, std::span<const uint32_t> valueIndices)
{
    if (valueIndices.size() != decl.options.size() || decl.keyBits > 64) {
        return std::nullopt;
    }
    uint64_t key = 0;
    for (std::size_t i = 0; i < decl.options.size(); ++i) {
        const PermutationOption& opt = decl.options[i];
        if (valueIndices[i] >= opt.values.size()) {
            return std::nullopt;
        }
        key |= uint64_t{valueIndices[i]} << opt.bitOffset;
    }
    return key;
}

std::optional<std::vector<int>> decodeKey(const ShaderDecl& decl, uint64_t key)
{
    if (decl.keyBits > 64) {
        return std::nullopt;
    }
    if (decl.keyBits < 64 && (key >> decl.keyBits) != 0) {
        return std::nullopt;  // bits set beyond the declared layout
    }
    std::vector<int> values;
    values.reserve(decl.options.size());
    for (const PermutationOption& opt : decl.options) {
        const uint64_t mask = opt.bitCount == 0 ? 0 : ((uint64_t{1} << opt.bitCount) - 1u);
        const uint64_t index = (key >> opt.bitOffset) & mask;
        if (index >= opt.values.size()) {
            return std::nullopt;
        }
        values.push_back(opt.values[static_cast<std::size_t>(index)]);
    }
    return values;
}

ExpandResult expandPermutations(const ShaderDecl& decl)
{
    ExpandResult result;
    if (decl.keyBits > 64) {
        result.errors.push_back("permutation key exceeds 64 bits");
        return result;
    }

    uint64_t raw = 1;
    for (const PermutationOption& opt : decl.options) {
        if (opt.values.empty()) {
            result.errors.push_back("option '" + opt.name + "' has no values");
            return result;
        }
        raw *= opt.values.size();
        if (raw > kMaxRawPermutations) {
            result.errors.push_back("shader '" + decl.name + "' has more than " + std::to_string(kMaxRawPermutations) +
                                    " raw permutations; split it or convert options to dynamic branches");
            return result;
        }
    }

    const std::size_t n = decl.options.size();
    std::vector<uint32_t> indices(n, 0);
    std::vector<int> values(n);
    for (uint64_t i = 0; i < raw; ++i) {
        for (std::size_t o = 0; o < n; ++o) {
            values[o] = decl.options[o].values[indices[o]];
        }

        bool excluded = false;
        for (const Expression& e : decl.excludes) {
            if (e.evaluate(values)) {
                excluded = true;
                break;
            }
        }
        if (excluded) {
            ++result.excludedCount;
        } else {
            Permutation p;
            p.key = *encodeKey(decl, indices);
            p.values = values;
            result.permutations.push_back(std::move(p));
        }

        // Odometer increment: last option varies fastest.
        for (std::size_t o = n; o-- > 0;) {
            if (++indices[o] < decl.options[o].values.size()) {
                break;
            }
            indices[o] = 0;
        }
    }

    if (result.permutations.size() > decl.budget) {
        result.errors.push_back("shader '" + decl.name + "' produces " + std::to_string(result.permutations.size()) +
                                " permutations, over its @budget of " + std::to_string(decl.budget));
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> definesFor(const ShaderDecl& decl, const Permutation& permutation)
{
    std::vector<std::pair<std::string, std::string>> defines;
    defines.reserve(decl.options.size());
    for (std::size_t i = 0; i < decl.options.size() && i < permutation.values.size(); ++i) {
        defines.emplace_back(decl.options[i].name, std::to_string(permutation.values[i]));
    }
    return defines;
}

} // namespace saige::shaderc
