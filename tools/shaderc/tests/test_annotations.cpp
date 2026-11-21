#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaderc/annotations.hpp"

#include <string>
#include <vector>

using namespace saige::shaderc;

namespace {

const char* valid = R"(// Deferred GBuffer pass
//
// @shader   GBufferPass
// @entry    VSMain vs
// @entry    PSMain ps
// @option   ALPHA_TEST     {0, 1}
// @option   QUALITY        {0, 1, 2}
// @option   LAYERS         {1, 2, 4, 8, 16}
// @exclude  ALPHA_TEST && QUALITY >= 2
// @budget   32

#include "common.hlsli"
// @option NOT_PARSED {0, 1}   <- after the header block, ignored
)";

bool has_error_containing(const ParseResult& r, const std::string& needle)
{
    for (const Diagnostic& d : r.errors) {
        if (d.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("Annotations: parses a complete header block")
{
    const ParseResult r = parse_annotations(valid);
    REQUIRE(r.ok());
    const ShaderDecl& d = *r.decl;
    CHECK(d.name == "GBufferPass");
    REQUIRE(d.entries.size() == 2);
    CHECK(d.entries[0].name == "VSMain");
    CHECK(d.entries[0].stage == ShaderStage::Vertex);
    CHECK(d.entries[1].stage == ShaderStage::Pixel);
    REQUIRE(d.options.size() == 3);  // NOT_PARSED is outside the header block
    CHECK(d.options[1].values == std::vector<int>{0, 1, 2});
    CHECK(d.excludes.size() == 1);
    CHECK(d.budget == 32);
}

TEST_CASE("Annotations: permutation key layout packs value indices tightly")
{
    const ParseResult r = parse_annotations(valid);
    REQUIRE(r.ok());
    const ShaderDecl& d = *r.decl;
    CHECK(d.options[0].bit_offset == 0);
    CHECK(d.options[0].bit_count == 1);  // 2 values
    CHECK(d.options[1].bit_offset == 1);
    CHECK(d.options[1].bit_count == 2);  // 3 values
    CHECK(d.options[2].bit_offset == 3);
    CHECK(d.options[2].bit_count == 3);  // 5 values
    CHECK(d.key_bits == 6);
}

TEST_CASE("Annotations: shaders without options are valid")
{
    const ParseResult r = parse_annotations("// @shader Blit\n// @entry CSMain cs\n");
    REQUIRE(r.ok());
    CHECK(r.decl->options.empty());
    CHECK(r.decl->key_bits == 0);
    CHECK(r.decl->budget == 64);
}

TEST_CASE("Annotations: reports missing @shader and @entry")
{
    const ParseResult r = parse_annotations("// just a comment\nfloat4 main() : SV_Target { return 0; }\n");
    CHECK_FALSE(r.ok());
    CHECK(has_error_containing(r, "missing @shader"));
    CHECK(has_error_containing(r, "@entry is required"));
}

TEST_CASE("Annotations: reports malformed directives with line numbers")
{
    const ParseResult r = parse_annotations(
        "// @shader Bad\n"
        "// @entry Main gs\n"            // line 2: unknown stage
        "// @option A 0, 1\n"            // line 3: missing braces
        "// @option B {0, x}\n"          // line 4: bad value
        "// @option C {0, 0}\n"          // line 5: duplicate value
        "// @frobnicate yes\n"           // line 6: unknown directive
        "// @budget 0\n"                 // line 7: invalid budget
        "// @entry PSMain ps\n");
    CHECK_FALSE(r.ok());
    CHECK(r.errors.size() == 6);
    CHECK(r.errors[0].line == 2);
    CHECK(has_error_containing(r, "unknown stage 'gs'"));
    CHECK(has_error_containing(r, "values must be a list"));
    CHECK(has_error_containing(r, "invalid value 'x'"));
    CHECK(has_error_containing(r, "duplicate value 0"));
    CHECK(has_error_containing(r, "unknown directive '@frobnicate'"));
    CHECK(has_error_containing(r, "@budget"));
}

TEST_CASE("Annotations: duplicate names are rejected")
{
    const ParseResult r = parse_annotations(
        "// @shader S\n// @shader T\n// @entry M vs\n// @entry M ps\n// @option O {0,1}\n// @option O {0,1}\n");
    CHECK(has_error_containing(r, "duplicate @shader"));
    CHECK(has_error_containing(r, "duplicate @entry 'M'"));
    CHECK(has_error_containing(r, "duplicate @option 'O'"));
}

TEST_CASE("Annotations: @exclude may reference options declared later, but not unknown ones")
{
    const ParseResult ok = parse_annotations("// @shader S\n// @entry M cs\n// @exclude A && B\n// @option A {0,1}\n"
                                            "// @option B {0,1}\n");
    CHECK(ok.ok());

    const ParseResult bad = parse_annotations("// @shader S\n// @entry M cs\n// @option A {0,1}\n// @exclude A && Z\n");
    CHECK(has_error_containing(bad, "unknown option 'Z'"));
    CHECK(bad.errors[0].line == 4);
}

TEST_CASE("Annotations: key layout over 64 bits is an error")
{
    std::string src = "// @shader Huge\n// @entry M cs\n";
    for (int i = 0; i < 33; ++i) {
        src += "// @option O" + std::to_string(i) + " {0, 1, 2}\n";  // 2 bits each = 66 bits
    }
    const ParseResult r = parse_annotations(src);
    CHECK(has_error_containing(r, "key bits"));
}

// ----------------------------------------------------------------------------------------------------- expressions

namespace {

std::vector<PermutationOption> abc()
{
    return {{"A", {0, 1}, 0, 1}, {"B", {0, 1}, 1, 1}, {"Q", {0, 1, 2, 3}, 2, 2}};
}

bool eval(const std::string& text, std::vector<int> values)
{
    const auto opts = abc();
    std::string err;
    const std::optional<Expression> e = parse_expression(text, opts, &err);
    REQUIRE(e.has_value());
    return e->evaluate(values);
}

} // namespace

TEST_CASE("Expression: comparison operators")
{
    CHECK(eval("Q == 2", {0, 0, 2}));
    CHECK(eval("Q = 2", {0, 0, 2}));
    CHECK(eval("Q != 2", {0, 0, 1}));
    CHECK(eval("Q < 2", {0, 0, 1}));
    CHECK_FALSE(eval("Q < 2", {0, 0, 2}));
    CHECK(eval("Q <= 2", {0, 0, 2}));
    CHECK(eval("Q > 2", {0, 0, 3}));
    CHECK(eval("Q >= 3", {0, 0, 3}));
}

TEST_CASE("Expression: bare names, negation, precedence and parentheses")
{
    CHECK(eval("A", {1, 0, 0}));
    CHECK_FALSE(eval("A", {0, 0, 0}));
    CHECK(eval("!A", {0, 0, 0}));
    // && binds tighter than ||
    CHECK(eval("A || B && Q == 3", {1, 0, 0}));
    CHECK_FALSE(eval("(A || B) && Q == 3", {1, 0, 0}));
    CHECK(eval("!(A && B)", {1, 0, 0}));
    CHECK(eval("!!A", {1, 0, 0}));
}

TEST_CASE("Expression: syntax errors are reported")
{
    const auto opts = abc();
    std::string err;
    CHECK_FALSE(parse_expression("A &&", opts, &err).has_value());
    CHECK(err.find("unexpected end") != std::string::npos);
    CHECK_FALSE(parse_expression("(A || B", opts, &err).has_value());
    CHECK(err.find("')'") != std::string::npos);
    CHECK_FALSE(parse_expression("Q == ", opts, &err).has_value());
    CHECK_FALSE(parse_expression("A B", opts, &err).has_value());
    CHECK_FALSE(parse_expression("A & B", opts, &err).has_value());
    CHECK_FALSE(parse_expression("", opts, &err).has_value());
}

TEST_CASE("Stages: names and DXC profiles")
{
    CHECK(parse_stage("ms") == ShaderStage::Mesh);
    CHECK(parse_stage("lib") == ShaderStage::Library);
    CHECK_FALSE(parse_stage("gs").has_value());
    CHECK(dxc_profile(ShaderStage::Pixel) == "ps_6_6");
    CHECK(dxc_profile(ShaderStage::Library, 6, 8) == "lib_6_8");
    for (ShaderStage s : {ShaderStage::Vertex, ShaderStage::Pixel, ShaderStage::Compute, ShaderStage::Mesh,
                          ShaderStage::Amplification, ShaderStage::Library}) {
        CHECK(parse_stage(stage_name(s)) == s);
    }
}
