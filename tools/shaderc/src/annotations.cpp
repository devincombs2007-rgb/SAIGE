#include "saige/shaderc/annotations.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <charconv>

namespace saige::shaderc {

namespace {

bool isIdentStart(char c) noexcept
{
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isIdentChar(char c) noexcept
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

bool isValidIdentifier(std::string_view s) noexcept
{
    if (s.empty() || !isIdentStart(s.front())) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), isIdentChar);
}

std::optional<int> parseInt(std::string_view s) noexcept
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
std::pair<std::string_view, std::string_view> splitWord(std::string_view s) noexcept
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
    int intValue = 0;
    Expression::Op cmp = Expression::Op::Eq;
};

class Lexer {
public:
    explicit Lexer(std::string_view s) : m_s(s) {}

    Token next()
    {
        while (m_i < m_s.size() && std::isspace(static_cast<unsigned char>(m_s[m_i])) != 0) {
            ++m_i;
        }
        if (m_i >= m_s.size()) {
            return {Token::End, {}};
        }
        const std::size_t start = m_i;
        const char c = m_s[m_i];
        auto two = [&](char second) { return m_i + 1 < m_s.size() && m_s[m_i + 1] == second; };

        if (isIdentStart(c)) {
            while (m_i < m_s.size() && isIdentChar(m_s[m_i])) {
                ++m_i;
            }
            return {Token::Ident, m_s.substr(start, m_i - start)};
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0 ||
            (c == '-' && m_i + 1 < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_i + 1])) != 0)) {
            ++m_i;
            while (m_i < m_s.size() && std::isdigit(static_cast<unsigned char>(m_s[m_i])) != 0) {
                ++m_i;
            }
            Token t{Token::Int, m_s.substr(start, m_i - start)};
            const std::optional<int> v = parseInt(t.text);
            if (!v) {
                t.kind = Token::Bad;
            } else {
                t.intValue = *v;
            }
            return t;
        }
        if (c == '&' && two('&')) {
            m_i += 2;
            return {Token::AndAnd, m_s.substr(start, 2)};
        }
        if (c == '|' && two('|')) {
            m_i += 2;
            return {Token::OrOr, m_s.substr(start, 2)};
        }
        if (c == '(') {
            ++m_i;
            return {Token::LParen, m_s.substr(start, 1)};
        }
        if (c == ')') {
            ++m_i;
            return {Token::RParen, m_s.substr(start, 1)};
        }
        if (c == '!' && two('=')) {
            m_i += 2;
            return {Token::Cmp, m_s.substr(start, 2), 0, Expression::Op::Ne};
        }
        if (c == '!') {
            ++m_i;
            return {Token::Not, m_s.substr(start, 1)};
        }
        if (c == '=') {
            m_i += two('=') ? 2u : 1u;
            return {Token::Cmp, m_s.substr(start, m_i - start), 0, Expression::Op::Eq};
        }
        if (c == '<' || c == '>') {
            const bool orEqual = two('=');
            m_i += orEqual ? 2u : 1u;
            const Expression::Op op = c == '<' ? (orEqual ? Expression::Op::Le : Expression::Op::Lt)
                                               : (orEqual ? Expression::Op::Ge : Expression::Op::Gt);
            return {Token::Cmp, m_s.substr(start, m_i - start), 0, op};
        }
        ++m_i;
        return {Token::Bad, m_s.substr(start, 1)};
    }

private:
    std::string_view m_s;
    std::size_t m_i = 0;
};

} // namespace

// =====================================================================================================================
// Stages
// =====================================================================================================================

std::optional<ShaderStage> parseStage(std::string_view name)
{
    if (name == "vs") return ShaderStage::Vertex;
    if (name == "ps") return ShaderStage::Pixel;
    if (name == "cs") return ShaderStage::Compute;
    if (name == "ms") return ShaderStage::Mesh;
    if (name == "as") return ShaderStage::Amplification;
    if (name == "lib") return ShaderStage::Library;
    return std::nullopt;
}

std::string_view stageName(ShaderStage stage)
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

std::string dxcProfile(ShaderStage stage, int smMajor, int smMinor)
{
    return std::string(stageName(stage)) + "_" + std::to_string(smMajor) + "_" + std::to_string(smMinor);
}

// =====================================================================================================================
// Expressions
// =====================================================================================================================

bool Expression::evaluate(std::span<const int> optionValues) const
{
    return m_root >= 0 && eval(m_root, optionValues);
}

bool Expression::eval(int node, std::span<const int> values) const
{
    const Node& n = m_nodes[static_cast<std::size_t>(node)];
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

std::optional<Expression> parseExpression(std::string_view text, std::span<const PermutationOption> options,
                                          std::string* error)
{
    Expression expr;
    expr.m_source = std::string(trim(text));
    Lexer lexer(text);
    Token tok = lexer.next();
    std::string err;

    auto addNode = [&](Expression::Node n) {
        expr.m_nodes.push_back(n);
        return static_cast<int>(expr.m_nodes.size() - 1);
    };

    // Recursive descent with explicit lambdas (std::function avoided; recursion via a small struct).
    struct Parser {
        Lexer& lexer;
        Token& tok;
        std::string& err;
        std::span<const PermutationOption> options;
        decltype(addNode)& add;
        int depth = 0;

        int parseOr()
        {
            int lhs = parseAnd();
            while (lhs >= 0 && tok.kind == Token::OrOr) {
                tok = lexer.next();
                const int rhs = parseAnd();
                if (rhs < 0) {
                    return -1;
                }
                lhs = add({Expression::Op::Or, lhs, rhs, 0, 0});
            }
            return lhs;
        }

        int parseAnd()
        {
            int lhs = parseUnary();
            while (lhs >= 0 && tok.kind == Token::AndAnd) {
                tok = lexer.next();
                const int rhs = parseUnary();
                if (rhs < 0) {
                    return -1;
                }
                lhs = add({Expression::Op::And, lhs, rhs, 0, 0});
            }
            return lhs;
        }

        int parseUnary()
        {
            if (++depth > 64) {
                err = "expression nested too deeply";
                return -1;
            }
            int result = -1;
            if (tok.kind == Token::Not) {
                tok = lexer.next();
                const int operand = parseUnary();
                result = operand < 0 ? -1 : add({Expression::Op::Not, operand, -1, 0, 0});
            } else if (tok.kind == Token::LParen) {
                tok = lexer.next();
                const int inner = parseOr();
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
                    const auto optionIndex = static_cast<uint32_t>(it - options.begin());
                    tok = lexer.next();
                    if (tok.kind == Token::Cmp) {
                        const Expression::Op op = tok.cmp;
                        tok = lexer.next();
                        if (tok.kind != Token::Int) {
                            err = "expected an integer after comparison";
                        } else {
                            result = add({op, -1, -1, optionIndex, tok.intValue});
                            tok = lexer.next();
                        }
                    } else {
                        result = add({Expression::Op::Ne, -1, -1, optionIndex, 0});  // bare NAME => NAME != 0
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

    Parser parser{lexer, tok, err, options, addNode};
    expr.m_root = parser.parseOr();
    if (expr.m_root >= 0 && tok.kind != Token::End) {
        err = "unexpected token '" + std::string(tok.text) + "'";
        expr.m_root = -1;
    }
    if (expr.m_root < 0) {
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

ParseResult parseAnnotations(std::string_view source)
{
    ParseResult result;
    ShaderDecl decl;
    bool haveShader = false;
    std::vector<std::pair<int, std::string>> pendingExcludes;  // parsed after all options are known

    auto error = [&](int line, std::string msg) { result.errors.push_back({line, std::move(msg)}); };

    int lineNo = 0;
    std::size_t pos = 0;
    while (pos <= source.size()) {
        const std::size_t nl = source.find('\n', pos);
        std::string_view line = source.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        pos = nl == std::string_view::npos ? source.size() + 1 : nl + 1;
        ++lineNo;

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

        const auto [directive, rest] = splitWord(line.substr(1));
        if (directive == "shader") {
            if (haveShader) {
                error(lineNo, "duplicate @shader");
            } else if (!isValidIdentifier(rest)) {
                error(lineNo, "@shader requires an identifier name");
            } else {
                decl.name = std::string(rest);
                haveShader = true;
            }
        } else if (directive == "entry") {
            const auto [name, stageText] = splitWord(rest);
            const std::optional<ShaderStage> stage = parseStage(stageText);
            if (!isValidIdentifier(name)) {
                error(lineNo, "@entry requires a function name");
            } else if (!stage) {
                error(lineNo, "@entry '" + std::string(name) + "': unknown stage '" + std::string(stageText) +
                                  "' (expected vs, ps, cs, ms, as or lib)");
            } else if (std::any_of(decl.entries.begin(), decl.entries.end(),
                                   [&](const EntryPoint& e) { return e.name == name; })) {
                error(lineNo, "duplicate @entry '" + std::string(name) + "'");
            } else {
                decl.entries.push_back({std::string(name), *stage});
            }
        } else if (directive == "option") {
            const auto [name, valuesText] = splitWord(rest);
            if (!isValidIdentifier(name)) {
                error(lineNo, "@option requires an identifier name");
                continue;
            }
            if (std::any_of(decl.options.begin(), decl.options.end(),
                            [&](const PermutationOption& o) { return o.name == name; })) {
                error(lineNo, "duplicate @option '" + std::string(name) + "'");
                continue;
            }
            if (!valuesText.starts_with("{") || !valuesText.ends_with("}")) {
                error(lineNo, "@option '" + std::string(name) + "': values must be a list like {0, 1}");
                continue;
            }
            PermutationOption opt;
            opt.name = std::string(name);
            std::string_view list = valuesText.substr(1, valuesText.size() - 2);
            bool bad = false;
            while (!bad) {
                const std::size_t comma = list.find(',');
                const std::string_view item = list.substr(0, comma);
                const std::optional<int> v = parseInt(item);
                if (!v) {
                    error(lineNo, "@option '" + opt.name + "': invalid value '" + std::string(trim(item)) + "'");
                    bad = true;
                } else if (std::find(opt.values.begin(), opt.values.end(), *v) != opt.values.end()) {
                    error(lineNo, "@option '" + opt.name + "': duplicate value " + std::to_string(*v));
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
            pendingExcludes.emplace_back(lineNo, std::string(rest));
        } else if (directive == "budget") {
            const std::optional<int> b = parseInt(rest);
            if (!b || *b < 1) {
                error(lineNo, "@budget requires a positive integer");
            } else {
                decl.budget = static_cast<uint32_t>(*b);
            }
        } else {
            error(lineNo, "unknown directive '@" + std::string(directive) + "'");
        }
    }

    // Permutation key layout: each option stores its value index in ceil(log2(count)) bits.
    uint32_t offset = 0;
    for (PermutationOption& opt : decl.options) {
        const auto count = static_cast<uint32_t>(opt.values.size());
        opt.bitOffset = offset;
        opt.bitCount = count <= 1 ? 0u : static_cast<uint32_t>(std::bit_width(count - 1u));
        offset += opt.bitCount;
    }
    decl.keyBits = offset;
    if (offset > 64) {
        error(0, "permutation options need " + std::to_string(offset) + " key bits (maximum 64)");
    }

    for (const auto& [line, text] : pendingExcludes) {
        std::string err;
        std::optional<Expression> e = parseExpression(text, decl.options, &err);
        if (!e) {
            error(line, "@exclude: " + err);
        } else {
            decl.excludes.push_back(std::move(*e));
        }
    }

    if (!haveShader) {
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
