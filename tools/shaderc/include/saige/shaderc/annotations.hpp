// saige-shaderc — shader annotation parsing (`@shader`, `@entry`, `@option`, `@exclude`, `@budget`).
//
// Design: docs/rendering/shader-pipeline.md §3. Annotations live in the leading `//` comment block of an
// HLSL file:
//
//   // @shader   GBufferPass
//   // @entry    VSMain vs
//   // @entry    PSMain ps
//   // @option   ALPHA_TEST       {0, 1}
//   // @option   QUALITY          {0, 1, 2}
//   // @exclude  ALPHA_TEST && QUALITY >= 2
//   // @budget   16
//
// Parsing stops at the first line that is neither blank nor a `//` comment.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace saige::shaderc {

enum class ShaderStage : uint8_t { Vertex, Pixel, Compute, Mesh, Amplification, Library };

/// Parses the short stage names used in `@entry`: vs, ps, cs, ms, as, lib.
[[nodiscard]] std::optional<ShaderStage> parse_stage(std::string_view name);
[[nodiscard]] std::string_view stage_name(ShaderStage stage);
/// DXC target profile, e.g. "ps_6_6", "lib_6_6".
[[nodiscard]] std::string dxc_profile(ShaderStage stage, int sm_major = 6, int sm_minor = 6);

struct EntryPoint {
    std::string name;
    ShaderStage stage = ShaderStage::Vertex;
};

/// One permutation dimension. The permutation key stores the *index* of the chosen value in
/// `bit_count` bits at `bit_offset`.
struct PermutationOption {
    std::string name;
    std::vector<int> values;
    uint32_t bit_offset = 0;
    uint32_t bit_count = 0;
};

/// Boolean expression over option values, used by `@exclude`.
/// Grammar: or := and ('||' and)* ; and := unary ('&&' unary)* ;
///          unary := '!' unary | '(' or ')' | NAME [cmp INT] ; cmp := = | == | != | < | <= | > | >=
/// A bare NAME means NAME != 0.
class Expression {
public:
    enum class Op : uint8_t { Or, And, Not, Eq, Ne, Lt, Le, Gt, Ge };

    struct Node {
        Op op = Op::Eq;
        int lhs = -1;  ///< Child node (Or/And/Not), unused for comparisons.
        int rhs = -1;
        uint32_t option = 0;  ///< Comparisons: option index.
        int value = 0;        ///< Comparisons: right-hand constant.
    };

    /// `option_values[i]` is the *value* (not index) of option i.
    [[nodiscard]] bool evaluate(std::span<const int> option_values) const;
    [[nodiscard]] const std::string& source() const noexcept { return source_; }

private:
    friend std::optional<Expression> parse_expression(std::string_view text, std::span<const PermutationOption> options,
                                                     std::string* error);
    [[nodiscard]] bool eval(int node, std::span<const int> values) const;

    std::vector<Node> nodes_;
    int root_ = -1;
    std::string source_;
};

/// Parses an `@exclude` expression. On failure returns nullopt and sets `*error`.
[[nodiscard]] std::optional<Expression> parse_expression(std::string_view text,
                                                        std::span<const PermutationOption> options,
                                                        std::string* error);

struct ShaderDecl {
    std::string name;
    std::vector<EntryPoint> entries;
    std::vector<PermutationOption> options;
    std::vector<Expression> excludes;
    uint32_t budget = 64;  ///< Maximum number of surviving permutations.
    uint32_t key_bits = 0;  ///< Total bits used by the permutation key.
};

struct Diagnostic {
    int line = 0;  ///< 1-based source line (0 = whole file).
    std::string message;
};

struct ParseResult {
    std::optional<ShaderDecl> decl;
    std::vector<Diagnostic> errors;
    [[nodiscard]] bool ok() const noexcept { return decl.has_value() && errors.empty(); }
};

[[nodiscard]] ParseResult parse_annotations(std::string_view source);

} // namespace saige::shaderc
