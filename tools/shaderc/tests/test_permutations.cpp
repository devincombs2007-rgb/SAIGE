#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaderc/permutations.hpp"

#include <set>
#include <string>

using namespace saige::shaderc;

namespace {

ShaderDecl parse(const std::string& src)
{
    ParseResult r = parse_annotations(src);
    REQUIRE(r.ok());
    return std::move(*r.decl);
}

} // namespace

TEST_CASE("Permutations: full cartesian product without exclusions")
{
    const ShaderDecl d = parse("// @shader S\n// @entry M ps\n// @option A {0,1}\n// @option Q {0,1,2}\n");
    const ExpandResult r = expand_permutations(d);
    REQUIRE(r.ok());
    REQUIRE(r.permutations.size() == 6);
    CHECK(r.excluded_count == 0);
    // Deterministic order: last option varies fastest.
    CHECK(r.permutations[0].values == std::vector<int>{0, 0});
    CHECK(r.permutations[1].values == std::vector<int>{0, 1});
    CHECK(r.permutations[2].values == std::vector<int>{0, 2});
    CHECK(r.permutations[3].values == std::vector<int>{1, 0});

    std::set<uint64_t> keys;
    for (const Permutation& p : r.permutations) {
        keys.insert(p.key);
    }
    CHECK(keys.size() == 6);  // keys are unique
}

TEST_CASE("Permutations: no options yields exactly one permutation with key 0")
{
    const ShaderDecl d = parse("// @shader S\n// @entry M cs\n");
    const ExpandResult r = expand_permutations(d);
    REQUIRE(r.ok());
    REQUIRE(r.permutations.size() == 1);
    CHECK(r.permutations[0].key == 0);
    CHECK(r.permutations[0].values.empty());
}

TEST_CASE("Permutations: @exclude removes combinations")
{
    const ShaderDecl d = parse("// @shader S\n// @entry M ps\n// @option ALPHA_TEST {0,1}\n"
                               "// @option VERTEX_COLORS {0,1}\n// @option Q {0,1,2}\n"
                               "// @exclude ALPHA_TEST && VERTEX_COLORS\n// @exclude Q == 2 && !ALPHA_TEST\n");
    const ExpandResult r = expand_permutations(d);
    REQUIRE(r.ok());
    // 12 raw. Excluded: AT=1,VC=1 (3); Q=2 with AT=0 (2). Total 5 -> 7 remain.
    CHECK(r.excluded_count == 5);
    CHECK(r.permutations.size() == 7);
    for (const Permutation& p : r.permutations) {
        CHECK_FALSE(p.values[0] == 1 && p.values[1] == 1);
        CHECK_FALSE(p.values[2] == 2 && p.values[0] == 0);
    }
}

TEST_CASE("Permutations: budget is enforced after exclusions")
{
    const ShaderDecl over = parse("// @shader S\n// @entry M ps\n// @option A {0,1}\n// @option B {0,1}\n"
                                  "// @budget 3\n");
    CHECK_FALSE(expand_permutations(over).ok());

    const ShaderDecl fits = parse("// @shader S\n// @entry M ps\n// @option A {0,1}\n// @option B {0,1}\n"
                                  "// @exclude A && B\n// @budget 3\n");
    CHECK(expand_permutations(fits).ok());
}

TEST_CASE("Permutations: raw permutation explosions are rejected early")
{
    std::string src = "// @shader Huge\n// @entry M ps\n// @budget 1000000\n";
    for (int i = 0; i < 21; ++i) {  // 2^21 raw > max_raw_permutations
        src += "// @option O" + std::to_string(i) + " {0, 1}\n";
    }
    const ExpandResult r = expand_permutations(parse(src));
    CHECK_FALSE(r.ok());
    CHECK(r.permutations.empty());
}

TEST_CASE("Permutation keys: encode/decode round trip uses value indices")
{
    const ShaderDecl d = parse("// @shader S\n// @entry M ps\n// @option LAYERS {1,2,4,8}\n// @option A {0,1}\n");
    const std::vector<uint32_t> idx{3, 1};  // LAYERS=8, A=1
    const std::optional<uint64_t> key = encode_key(d, idx);
    REQUIRE(key.has_value());
    CHECK(*key == (3u | (1u << 2)));
    const std::optional<std::vector<int>> values = decode_key(d, *key);
    REQUIRE(values.has_value());
    CHECK(*values == std::vector<int>{8, 1});

    CHECK_FALSE(encode_key(d, std::vector<uint32_t>{4, 0}).has_value());  // index out of range
    CHECK_FALSE(encode_key(d, std::vector<uint32_t>{0}).has_value());     // wrong arity
    CHECK_FALSE(decode_key(d, uint64_t{1} << 10).has_value());           // bits beyond layout
}

TEST_CASE("Permutation keys: decoding rejects unused index codes")
{
    const ShaderDecl d = parse("// @shader S\n// @entry M ps\n// @option Q {0,1,2}\n");  // 2 bits, code 3 unused
    CHECK(decode_key(d, 2).has_value());
    CHECK_FALSE(decode_key(d, 3).has_value());
}

TEST_CASE("Permutations: defines match option names and values")
{
    const ShaderDecl d = parse("// @shader S\n// @entry M ps\n// @option NORMAL_MAP {0,1}\n// @option LAYERS {2,4}\n");
    const ExpandResult r = expand_permutations(d);
    REQUIRE(r.ok());
    const auto defines = defines_for(d, r.permutations.back());
    REQUIRE(defines.size() == 2);
    CHECK(defines[0].first == "NORMAL_MAP");
    CHECK(defines[0].second == "1");
    CHECK(defines[1].first == "LAYERS");
    CHECK(defines[1].second == "4");
}
