#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaderc/includes.hpp"

#include <map>
#include <string>

using namespace saige::shaderc;
namespace fs = std::filesystem;

namespace {

struct MemoryFs {
    std::map<std::string, std::string> files;

    FileReader reader() const
    {
        return [this](const fs::path& p) -> std::optional<std::string> {
            const auto it = files.find(p.lexically_normal().generic_string());
            if (it == files.end()) {
                return std::nullopt;
            }
            return it->second;
        };
    }
};

bool has_dep(const IncludeScanResult& r, const std::string& path)
{
    for (const fs::path& d : r.dependencies) {
        if (d.generic_string() == path) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST_CASE("Includes: extracts quoted and angle includes, ignoring comments")
{
    const auto inc = extract_includes(R"(
#include "a.hlsli"
  #  include <shared/b.hlsli>
// #include "commented.hlsli"
/* #include "block.hlsli"
   #include "block2.hlsli" */
#include MACRO_INCLUDE
const char* s = "#include \"in_string.hlsli\"";
#include "c.hlsli" // trailing comment
#pragma once
)");
    REQUIRE(inc.size() == 3);
    CHECK(inc[0].first == "a.hlsli");
    CHECK_FALSE(inc[0].second);
    CHECK(inc[1].first == "shared/b.hlsli");
    CHECK(inc[1].second);
    CHECK(inc[2].first == "c.hlsli");
}

TEST_CASE("Includes: transitive resolution through relative paths and include dirs")
{
    MemoryFs mfs;
    mfs.files["shaders/passes/gbuffer.hlsl"] = "#include \"../common/material.hlsli\"\n#include <shared/scene.hlsli>\n";
    mfs.files["shaders/common/material.hlsli"] = "#include \"brdf.hlsli\"\n";
    mfs.files["shaders/common/brdf.hlsli"] = "// leaf\n";
    mfs.files["shaders/shared/scene.hlsli"] = "#include \"../common/brdf.hlsli\"\n";  // diamond

    const std::vector<fs::path> dirs{"shaders"};
    const IncludeScanResult r = scan_includes("shaders/passes/gbuffer.hlsl", dirs, mfs.reader());
    REQUIRE(r.ok());
    CHECK(r.dependencies.size() == 3);  // brdf counted once
    CHECK(has_dep(r, "shaders/common/material.hlsli"));
    CHECK(has_dep(r, "shaders/common/brdf.hlsli"));
    CHECK(has_dep(r, "shaders/shared/scene.hlsli"));
}

TEST_CASE("Includes: quoted includes prefer the including file's directory")
{
    MemoryFs mfs;
    mfs.files["a/main.hlsl"] = "#include \"util.hlsli\"\n";
    mfs.files["a/util.hlsli"] = "";
    mfs.files["lib/util.hlsli"] = "";
    const std::vector<fs::path> dirs{"lib"};
    const IncludeScanResult r = scan_includes("a/main.hlsl", dirs, mfs.reader());
    REQUIRE(r.ok());
    REQUIRE(r.dependencies.size() == 1);
    CHECK(r.dependencies[0].generic_string() == "a/util.hlsli");
}

TEST_CASE("Includes: angle includes search include dirs only")
{
    MemoryFs mfs;
    mfs.files["a/main.hlsl"] = "#include <util.hlsli>\n";
    mfs.files["a/util.hlsli"] = "";
    const IncludeScanResult r = scan_includes("a/main.hlsl", {}, mfs.reader());
    CHECK_FALSE(r.ok());  // not found: the local directory is not searched for <...>
}

TEST_CASE("Includes: cycles terminate and missing files are reported")
{
    MemoryFs mfs;
    mfs.files["x.hlsl"] = "#include \"y.hlsli\"\n#include \"missing.hlsli\"\n";
    mfs.files["y.hlsli"] = "#include \"x.hlsl\"\n";
    const IncludeScanResult r = scan_includes("x.hlsl", {}, mfs.reader());
    CHECK(r.dependencies.size() == 1);
    REQUIRE(r.errors.size() == 1);
    CHECK(r.errors[0].find("missing.hlsli") != std::string::npos);

    const IncludeScanResult none = scan_includes("nope.hlsl", {}, mfs.reader());
    CHECK_FALSE(none.ok());
}
