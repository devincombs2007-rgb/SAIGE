#include "saige/gfx_test/saige_test.h"

#include "saige/shaderc/annotations.h"

#include <string>
#include <vector>

using namespace saige::shaderc;

namespace {

const char* kValid = R"(// Deferred GBuffer pass
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

bool hasErrorContaining(const ParseResult& r, const std::string& needle)
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
    const ParseResult r = parseAnnotations(kValid);
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
    const ParseResult r = parseAnnotations(kValid);
    REQUIRE(r.ok());
    const ShaderDecl& d = *r.decl;
    CHECK(d.options[0].bitOffset == 0);
    CHECK(d.options[0].bitCount == 1);  // 2 values
    CHECK(d.options[1].bitOffset == 1);
    CHECK(d.options[1].bitCount == 2);  // 3 values
    CHECK(d.options[2].bitOffset == 3);
    CHECK(d.options[2].bitCount == 3);  // 5 values
    CHECK(d.keyBits == 6);
}

TEST_CASE("Annotations: shaders without options are valid")
{
    const ParseResult r = parseAnnotations("// @shader Blit\n// @entry CSMain cs\n");
    REQUIRE(r.ok());
    CHECK(r.decl->options.empty());
    CHECK(r.decl->keyBits == 0);
    CHECK(r.decl->budget == 64);
}

TEST_CASE("Annotations: reports missing @shader and @entry")
{
    const ParseResult r = parseAnnotations("// just a comment\nfloat4 main() : SV_Target { return 0; }\n");
    CHECK_FALSE(r.ok());
    CHECK(hasErrorContaining(r, "missing @shader"));
    CHECK(hasErrorContaining(r, "@entry is required"));
}

TEST_CASE("Annotations: reports malformed directives with line numbers")
{
    const ParseResult r = parseAnnotations(
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
    CHECK(hasErrorContaining(r, "unknown stage 'gs'"));
    CHECK(hasErrorContaining(r, "values must be a list"));
    CHECK(hasErrorContaining(r, "invalid value 'x'"));
    CHECK(hasErrorContaining(r, "duplicate value 0"));
    CHECK(hasErrorContaining(r, "unknown directive '@frobnicate'"));
    CHECK(hasErrorContaining(r, "@budget"));
}

TEST_CASE("Annotations: duplicate names are rejected")
{
    const ParseResult r = parseAnnotations(
        "// @shader S\n// @shader T\n// @entry M vs\n// @entry M ps\n// @option O {0,1}\n// @option O {0,1}\n");
    CHECK(hasErrorContaining(r, "duplicate @shader"));
    CHECK(hasErrorContaining(r, "duplicate @entry 'M'"));
    CHECK(hasErrorContaining(r, "duplicate @option 'O'"));
}

TEST_CASE("Annotations: @exclude may reference options declared later, but not unknown ones")
{
    const ParseResult ok = parseAnnotations("// @shader S\n// @entry M cs\n// @exclude A && B\n// @option A {0,1}\n"
                                            "// @option B {0,1}\n");
    CHECK(ok.ok());

    const ParseResult bad = parseAnnotations("// @shader S\n// @entry M cs\n// @option A {0,1}\n// @exclude A && Z\n");
    CHECK(hasErrorContaining(bad, "unknown option 'Z'"));
    CHECK(bad.errors[0].line == 4);
}

TEST_CASE("Annotations: key layout over 64 bits is an error")
{
    std::string src = "// @shader Huge\n// @entry M cs\n";
    for (int i = 0; i < 33; ++i) {
        src += "// @option O" + std::to_string(i) + " {0, 1, 2}\n";  // 2 bits each = 66 bits
    }
    const ParseResult r = parseAnnotations(src);
    CHECK(hasErrorContaining(r, "key bits"));
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
    const std::optional<Expression> e = parseExpression(text, opts, &err);
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
    CHECK_FALSE(parseExpression("A &&", opts, &err).has_value());
    CHECK(err.find("unexpected end") != std::string::npos);
    CHECK_FALSE(parseExpression("(A || B", opts, &err).has_value());
    CHECK(err.find("')'") != std::string::npos);
    CHECK_FALSE(parseExpression("Q == ", opts, &err).has_value());
    CHECK_FALSE(parseExpression("A B", opts, &err).has_value());
    CHECK_FALSE(parseExpression("A & B", opts, &err).has_value());
    CHECK_FALSE(parseExpression("", opts, &err).has_value());
}

TEST_CASE("Stages: names and DXC profiles")
{
    CHECK(parseStage("ms") == ShaderStage::Mesh);
    CHECK(parseStage("lib") == ShaderStage::Library);
    CHECK_FALSE(parseStage("gs").has_value());
    CHECK(dxcProfile(ShaderStage::Pixel) == "ps_6_6");
    CHECK(dxcProfile(ShaderStage::Library, 6, 8) == "lib_6_8");
    for (ShaderStage s : {ShaderStage::Vertex, ShaderStage::Pixel, ShaderStage::Compute, ShaderStage::Mesh,
                          ShaderStage::Amplification, ShaderStage::Library}) {
        CHECK(parseStage(stageName(s)) == s);
    }
}
