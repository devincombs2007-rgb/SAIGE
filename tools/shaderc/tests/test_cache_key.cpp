#include "saige/gfx_test/saige_test.h"

#include "saige/shaderc/cache_key.h"

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
        r.includeContents = includes;
        r.entryPoint = "main";
        r.stage = ShaderStage::Pixel;
        r.defines = defines;
        r.target = "spirv-vk13";
        r.compilerVersion = "dxc 1.8";
        r.toolVersion = "0.1.0";
        r.extraArgs = args;
        return r;
    }
};

} // namespace

TEST_CASE("CacheKey: deterministic and 32 hex characters")
{
    const Fixture f;
    const std::string a = computeCacheKey(f.request());
    const std::string b = computeCacheKey(f.request());
    CHECK(a == b);
    CHECK(a.size() == 32);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("CacheKey: every input field changes the key")
{
    const Fixture base;
    std::set<std::string> keys{computeCacheKey(base.request())};
    auto variant = [&](auto mutate) {
        Fixture f = base;
        CompileRequest r = f.request();
        mutate(f, r);
        keys.insert(computeCacheKey(r));
    };
    variant([](Fixture& f, CompileRequest& r) { f.source += " "; r.source = f.source; });
    variant([](Fixture& f, CompileRequest& r) { f.includes[0] = "#define X 2"; r.includeContents = f.includes; });
    variant([](Fixture&, CompileRequest& r) { r.entryPoint = "main2"; });
    variant([](Fixture&, CompileRequest& r) { r.stage = ShaderStage::Compute; });
    variant([](Fixture& f, CompileRequest& r) { f.defines[0].second = "0"; r.defines = f.defines; });
    variant([](Fixture&, CompileRequest& r) { r.target = "dxil-sm66"; });
    variant([](Fixture&, CompileRequest& r) { r.compilerVersion = "dxc 1.9"; });
    variant([](Fixture&, CompileRequest& r) { r.toolVersion = "0.2.0"; });
    variant([](Fixture& f, CompileRequest& r) { f.args[0] = "-O0"; r.extraArgs = f.args; });
    variant([](Fixture&, CompileRequest& r) { r.includeContents = {}; });
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
    d.add("defin", "eA=1");
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
