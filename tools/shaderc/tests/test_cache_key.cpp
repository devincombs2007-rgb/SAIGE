#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaderc/cache_key.hpp"

#include <set>
#include <string>
#include <vector>

using namespace saige::shaderc;

namespace {

struct Fixture {
    std::string source = "float4 main() : SV_Target { return 1; }";
    std::vector<std::string> includes{"#define X 1"};
    std::vector<std::pair<std::string, std::string>> defines{{"ALPHA_TEST", "1"}};
    std::vector<std::string> args{"-O3"};

    CompileRequest request() const
    {
        CompileRequest r;
        r.source = source;
        r.include_contents = includes;
        r.entry_point = "main";
        r.stage = ShaderStage::Pixel;
        r.defines = defines;
        r.target = "spirv-vk13";
        r.compiler_version = "dxc 1.8";
        r.tool_version = "0.1.0";
        r.extra_args = args;
        return r;
    }
};

} // namespace

TEST_CASE("CacheKey: deterministic and 32 hex characters")
{
    const Fixture f;
    const std::string a = compute_cache_key(f.request());
    const std::string b = compute_cache_key(f.request());
    CHECK(a == b);
    CHECK(a.size() == 32);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("CacheKey: every input field changes the key")
{
    const Fixture base;
    std::set<std::string> keys{compute_cache_key(base.request())};
    auto variant = [&](auto mutate) {
        Fixture f = base;
        CompileRequest r = f.request();
        mutate(f, r);
        keys.insert(compute_cache_key(r));
    };
    variant([](Fixture& f, CompileRequest& r) { f.source += " "; r.source = f.source; });
    variant([](Fixture& f, CompileRequest& r) { f.includes[0] = "#define X 2"; r.include_contents = f.includes; });
    variant([](Fixture&, CompileRequest& r) { r.entry_point = "main2"; });
    variant([](Fixture&, CompileRequest& r) { r.stage = ShaderStage::Compute; });
    variant([](Fixture& f, CompileRequest& r) { f.defines[0].second = "0"; r.defines = f.defines; });
    variant([](Fixture&, CompileRequest& r) { r.target = "dxil-sm66"; });
    variant([](Fixture&, CompileRequest& r) { r.compiler_version = "dxc 1.9"; });
    variant([](Fixture&, CompileRequest& r) { r.tool_version = "0.2.0"; });
    variant([](Fixture& f, CompileRequest& r) { f.args[0] = "-O0"; r.extra_args = f.args; });
    variant([](Fixture&, CompileRequest& r) { r.include_contents = {}; });
    CHECK(keys.size() == 11);
}

TEST_CASE("CacheKey: field boundaries are unambiguous")
{
    CacheKeyBuilder a;
    a.add("f", "ab").add("g", "c");
    CacheKeyBuilder b;
    b.add("f", "a").add("g", "bc");
    CHECK(a.hex() != b.hex());

    CacheKeyBuilder c;
    c.add("define", "A=1");
    CacheKeyBuilder d;
    d.add("defin", "e_a=1");
    CHECK(c.hex() != d.hex());
}

TEST_CASE("CacheKey: low collision rate on many similar inputs")
{
    std::set<std::string> keys;
    for (int i = 0; i < 20000; ++i) {
        CacheKeyBuilder k;
        k.add("permutation", static_cast<uint64_t>(i));
        keys.insert(k.hex());
    }
    CHECK(keys.size() == 20000);
}
