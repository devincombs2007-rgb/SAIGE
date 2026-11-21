#include "saige/shaderc/annotations.hpp"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>

namespace saige::shaderc {

namespace {

bool is_ident_start(char c) noexcept
{
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_ident_char(char c) noexcept
{
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

std::string_view trim(std::string_view s) noexcept
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())) != 0) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())) != 0) {
        s.remove_suffix(1);
    }
    return s;
}

bool is_valid_identifier(std::string_view s) noexcept
{
    if (s.empty() || !is_ident_start(s.front())) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), is_ident_char);
}

std::optional<int> parse_int(std::string_view s) noexcept
{
    s = trim(s);
    int value = 0;
    const char* end = s.data() + s.size();
    const auto [ptr, ec] = std::from_chars(s.data(), end, value);
    if (ec != std::errc{} || ptr != end || s.empty()) {
        return std::nullopt;
    }
    return value;
}

/// Splits off the first whitespace-delimited word.
std::pair<std::string_view, std::string_view> split_word(std::string_view s) noexcept
{
    s = trim(s);
    std::size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i])) == 0) {
        ++i;
    }
    return {s.substr(0, i), trim(s.substr(i))};
}

// ----------------------------------------------------------------------------------------------- expression parser

struct Token {
    enum Kind : uint8_t { Ident, Int, AndAnd, OrOr, Not, LParen, RParen, Cmp, End, Bad } kind = End;
    std::string_view text;
    int int_value = 0;
    Expression::Op cmp = Expression::Op::Eq;
};

class Lexer {
public:
    explicit Lexer(std::string_view s) : s_(s) {}

    Token next()
    {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_])) != 0) {
            ++i_;
        }
        if (i_ >= s_.size()) {
            return {Token::End, {}};
        }
        const std::size_t start = i_;
        const char c = s_[i_];
        auto two = [&](char second) { return i_ + 1 < s_.size() && s_[i_ + 1] == second; };

        if (is_ident_start(c)) {
            while (i_ < s_.size() && is_ident_char(s_[i_])) {
                ++i_;
            }
            return {Token::Ident, s_.substr(start, i_ - start)};
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '-' && i_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_ + 1])) != 0)) {
            ++i_;
            while (i_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_])) != 0) {
                ++i_;
            }
            Token t{Token::Int, s_.substr(start, i_ - start)};
            const std::optional<int> v = parse_int(t.text);
            if (!v) {
                t.kind = Token::Bad;
            } else {
                t.int_value = *v;
            }
            return t;
        }
        if (c == '&' && two('&')) {
            i_ += 2;
            return {Token::AndAnd, s_.substr(start, 2)};
        }
        if (c == '|' && two('|')) {
            i_ += 2;
            return {Token::OrOr, s_.substr(start, 2)};
        }
        if (c == '(') {
            ++i_;
            return {Token::LParen, s_.substr(start, 1)};
        }
        if (c == ')') {
            ++i_;
            return {Token::RParen, s_.substr(start, 1)};
        }
        if (c == '!' && two('=')) {
            i_ += 2;
            return {Token::Cmp, s_.substr(start, 2), 0, Expression::Op::Ne};
        }
        if (c == '!') {
            ++i_;
            return {Token::Not, s_.substr(start, 1)};
        }
        if (c == '=') {
            i_ += two('=') ? 2u : 1u;
            return {Token::Cmp, s_.substr(start, i_ - start), 0, Expression::Op::Eq};
        }
        if (c == '<' || c == '>') {
            const bool or_equal = two('=');
            i_ += or_equal ? 2u : 1u;
            const Expression::Op op = c == '<' ? (or_equal ? Expression::Op::Le : Expression::Op::Lt)
                                               : (or_equal ? Expression::Op::Ge : Expression::Op::Gt);
            return {Token::Cmp, s_.substr(start, i_ - start), 0, op};
        }
        ++i_;
        return {Token::Bad, s_.substr(start, 1)};
    }

private:
    std::string_view s_;
    std::size_t i_ = 0;
};

} // namespace

// =====================================================================================================================
// Stages
// =====================================================================================================================

std::optional<ShaderStage> parse_stage(std::string_view name)
{
    if (name == "vs") return ShaderStage::Vertex;
    if (name == "ps") return ShaderStage::Pixel;
    if (name == "cs") return ShaderStage::Compute;
    if (name == "ms") return ShaderStage::Mesh;
    if (name == "as") return ShaderStage::Amplification;
    if (name == "lib") return ShaderStage::Library;
    return std::nullopt;
}

std::string_view stage_name(ShaderStage stage)
{
    switch (stage) {
    case ShaderStage::Vertex: return "vs";
    case ShaderStage::Pixel: return "ps";
    case ShaderStage::Compute: return "cs";
    case ShaderStage::Mesh: return "ms";
    case ShaderStage::Amplification: return "as";
    case ShaderStage::Library: return "lib";
    }
    return "?";
}

std::string dxc_profile(ShaderStage stage, int sm_major, int sm_minor)
{
    return std::string(stage_name(stage)) + "_" + std::to_string(sm_major) + "_" + std::to_string(sm_minor);
}

// =====================================================================================================================
// Expressions
// =====================================================================================================================

bool Expression::evaluate(std::span<const int> option_values) const
{
    return root_ >= 0 && eval(root_, option_values);
}

bool Expression::eval(int node, std::span<const int> values) const
{
    const Node& n = nodes_[static_cast<std::size_t>(node)];
    switch (n.op) {
    case Op::Or: return eval(n.lhs, values) || eval(n.rhs, values);
    case Op::And: return eval(n.lhs, values) && eval(n.rhs, values);
    case Op::Not: return !eval(n.lhs, values);
    default: break;
    }
    const int v = n.option < values.size() ? values[n.option] : 0;
    switch (n.op) {
    case Op::Eq: return v == n.value;
    case Op::Ne: return v != n.value;
    case Op::Lt: return v < n.value;
    case Op::Le: return v <= n.value;
    case Op::Gt: return v > n.value;
    case Op::Ge: return v >= n.value;
    default: return false;
    }
}

std::optional<Expression> parse_expression(std::string_view text, std::span<const PermutationOption> options,
                                          std::string* error)
{
    Expression expr;
    expr.source_ = std::string(trim(text));
    Lexer lexer(text);
    Token tok = lexer.next();
    std::string err;

    auto add_node = [&](Expression::Node n) {
        expr.nodes_.push_back(n);
        return static_cast<int>(expr.nodes_.size() - 1);
    };

    // Recursive descent with explicit lambdas (std::function avoided; recursion via a small struct).
    struct Parser {
        Lexer& lexer;
        Token& tok;
        std::string& err;
        std::span<const PermutationOption> options;
        decltype(add_node)& add;
        int depth = 0;

        int parse_or()
        {
            int lhs = parse_and();
            while (lhs >= 0 && tok.kind == Token::OrOr) {
                tok = lexer.next();
                const int rhs = parse_and();
                if (rhs < 0) {
                    return -1;
                }
                lhs = add({Expression::Op::Or, lhs, rhs, 0, 0});
            }
            return lhs;
        }

        int parse_and()
        {
            int lhs = parse_unary();
            while (lhs >= 0 && tok.kind == Token::AndAnd) {
                tok = lexer.next();
                const int rhs = parse_unary();
                if (rhs < 0) {
                    return -1;
                }
                lhs = add({Expression::Op::And, lhs, rhs, 0, 0});
            }
            return lhs;
        }

        int parse_unary()
        {
            if (++depth > 64) {
                err = "expression nested too deeply";
                return -1;
            }
            int result = -1;
            if (tok.kind == Token::Not) {
                tok = lexer.next();
                const int operand = parse_unary();
                result = operand < 0 ? -1 : add({Expression::Op::Not, operand, -1, 0, 0});
            } else if (tok.kind == Token::LParen) {
                tok = lexer.next();
                const int inner = parse_or();
                if (inner >= 0 && tok.kind != Token::RParen) {
                    err = "expected ')'";
                } else if (inner >= 0) {
                    tok = lexer.next();
                    result = inner;
                }
            } else if (tok.kind == Token::Ident) {
                const std::string_view name = tok.text;
                const auto it = std::find_if(options.begin(), options.end(),
                                             [&](const PermutationOption& o) { return o.name == name; });
                if (it == options.end()) {
                    err = "unknown option '" + std::string(name) + "'";
                } else {
                    const auto option_index = static_cast<uint32_t>(it - options.begin());
                    tok = lexer.next();
                    if (tok.kind == Token::Cmp) {
                        const Expression::Op op = tok.cmp;
                        tok = lexer.next();
                        if (tok.kind != Token::Int) {
                            err = "expected an integer after comparison";
                        } else {
                            result = add({op, -1, -1, option_index, tok.int_value});
                            tok = lexer.next();
                        }
                    } else {
                        result = add({Expression::Op::Ne, -1, -1, option_index, 0});  // bare NAME => NAME != 0
                    }
                }
            } else {
                err = tok.kind == Token::End ? "unexpected end of expression"
                                             : "unexpected token '" + std::string(tok.text) + "'";
            }
            --depth;
            return result;
        }
    };

    Parser parser{lexer, tok, err, options, add_node};
    expr.root_ = parser.parse_or();
    if (expr.root_ >= 0 && tok.kind != Token::End) {
        err = "unexpected token '" + std::string(tok.text) + "'";
        expr.root_ = -1;
    }
    if (expr.root_ < 0) {
        if (error != nullptr) {
            *error = err.empty() ? "invalid expression" : err;
        }
        return std::nullopt;
    }
    return expr;
}

// =====================================================================================================================
// Annotation block
// =====================================================================================================================

ParseResult parse_annotations(std::string_view source)
{
    ParseResult result;
    ShaderDecl decl;
    bool have_shader = false;
    std::vector<std::pair<int, std::string>> pending_excludes;  // parsed after all options are known

    auto error = [&](int line, std::string msg) { result.errors.push_back({line, std::move(msg)}); };

    int line_no = 0;
    std::size_t pos = 0;
    while (pos <= source.size()) {
        const std::size_t nl = source.find('\n', pos);
        std::string_view line = source.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? source.size() + 1 : nl + 1;
        ++line_no;

        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (!line.starts_with("//")) {
            break;  // end of the leading comment block
        }
        line = trim(line.substr(2));
        if (!line.starts_with("@")) {
            continue;  // ordinary comment
        }

        const auto [directive, rest] = split_word(line.substr(1));
        if (directive == "shader") {
            if (have_shader) {
                error(line_no, "duplicate @shader");
            } else if (!is_valid_identifier(rest)) {
                error(line_no, "@shader requires an identifier name");
            } else {
                decl.name = std::string(rest);
                have_shader = true;
            }
        } else if (directive == "entry") {
            const auto [name, stage_text] = split_word(rest);
            const std::optional<ShaderStage> stage = parse_stage(stage_text);
            if (!is_valid_identifier(name)) {
                error(line_no, "@entry requires a function name");
            } else if (!stage) {
                error(line_no, "@entry '" + std::string(name) + "': unknown stage '" + std::string(stage_text) +
                                  "' (expected vs, ps, cs, ms, as or lib)");
            } else if (std::any_of(decl.entries.begin(), decl.entries.end(),
                                   [&](const EntryPoint& e) { return e.name == name; })) {
                error(line_no, "duplicate @entry '" + std::string(name) + "'");
            } else {
                decl.entries.push_back({std::string(name), *stage});
            }
        } else if (directive == "option") {
            const auto [name, values_text] = split_word(rest);
            if (!is_valid_identifier(name)) {
                error(line_no, "@option requires an identifier name");
                continue;
            }
            if (std::any_of(decl.options.begin(), decl.options.end(),
                            [&](const PermutationOption& o) { return o.name == name; })) {
                error(line_no, "duplicate @option '" + std::string(name) + "'");
                continue;
            }
            if (!values_text.starts_with("{") || !values_text.ends_with("}")) {
                error(line_no, "@option '" + std::string(name) + "': values must be a list like {0, 1}");
                continue;
            }
            PermutationOption opt;
            opt.name = std::string(name);
            std::string_view list = values_text.substr(1, values_text.size() - 2);
            bool bad = false;
            while (!bad) {
                const std::size_t comma = list.find(',');
                const std::string_view item = list.substr(0, comma);
                const std::optional<int> v = parse_int(item);
                if (!v) {
                    error(line_no, "@option '" + opt.name + "': invalid value '" + std::string(trim(item)) + "'");
                    bad = true;
                } else if (std::find(opt.values.begin(), opt.values.end(), *v) != opt.values.end()) {
                    error(line_no, "@option '" + opt.name + "': duplicate value " + std::to_string(*v));
                    bad = true;
                } else {
                    opt.values.push_back(*v);
                }
                if (comma == std::string_view::npos) {
                    break;
                }
                list.remove_prefix(comma + 1);
            }
            if (!bad) {
                decl.options.push_back(std::move(opt));
            }
        } else if (directive == "exclude") {
            pending_excludes.emplace_back(line_no, std::string(rest));
        } else if (directive == "budget") {
            const std::optional<int> b = parse_int(rest);
            if (!b || *b < 1) {
                error(line_no, "@budget requires a positive integer");
            } else {
                decl.budget = static_cast<uint32_t>(*b);
            }
        } else {
            error(line_no, "unknown directive '@" + std::string(directive) + "'");
        }
    }

    // Permutation key layout: each option stores its value index in ceil(log2(count)) bits.
    uint32_t offset = 0;
    for (PermutationOption& opt : decl.options) {
        const auto count = static_cast<uint32_t>(opt.values.size());
        opt.bit_offset = offset;
        opt.bit_count = count <= 1 ? 0u : static_cast<uint32_t>(std::bit_width(count - 1u));
        offset += opt.bit_count;
    }
    decl.key_bits = offset;
    if (offset > 64) {
        error(0, "permutation options need " + std::to_string(offset) + " key bits (maximum 64)");
    }

    for (const auto& [line, text] : pending_excludes) {
        std::string err;
        std::optional<Expression> e = parse_expression(text, decl.options, &err);
        if (!e) {
            error(line, "@exclude: " + err);
        } else {
            decl.excludes.push_back(std::move(*e));
        }
    }

    if (!have_shader) {
        error(0, "missing @shader declaration");
    }
    if (decl.entries.empty()) {
        error(0, "at least one @entry is required");
    }

    if (result.errors.empty()) {
        result.decl = std::move(decl);
    }
    return result;
}

} // namespace saige::shaderc
