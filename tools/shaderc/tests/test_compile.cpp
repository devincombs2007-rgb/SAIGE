#include "saige/gfx_test/saige_test.hpp"

#include "saige/shaderc/compile.hpp"

#include <map>
#include <string>

using namespace saige::shaderc;
namespace fs = std::filesystem;

namespace {

ShaderDecl parse(const std::string& src)
{
    ParseResult r = parse_annotations(src);
    REQUIRE(r.ok());
    return std::move(*r.decl);
}

FileReader memory_reader(const std::map<std::string, std::string>& files)
{
    return [&files](const fs::path& p) -> std::optional<std::string> {
        const auto it = files.find(p.lexically_normal().generic_string());
        if (it == files.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

} // namespace

TEST_CASE("Compile: snake_case conversion for symbols")
{
    CHECK(to_snake_case("SceneCube") == "scene_cube");
    CHECK(to_snake_case("VSMain") == "vs_main");
    CHECK(to_snake_case("PSMain") == "ps_main");
    CHECK(to_snake_case("GBufferPass") == "g_buffer_pass");
    CHECK(to_snake_case("already_snake") == "already_snake");
}

TEST_CASE("Compile: entry symbols carry a permutation suffix only when options exist")
{
    const ShaderDecl plain = parse("// @shader SceneCube\n// @entry VSMain vs\n");
    CHECK(entry_symbol(plain, plain.entries[0], 0) == "scene_cube_vs_main");
    const ShaderDecl opts = parse("// @shader Lit\n// @entry PSMain ps\n// @option A {0,1}\n");
    CHECK(entry_symbol(opts, opts.entries[0], 1) == "lit_ps_main_p1");
}

TEST_CASE("Compile: glslang and dxc command lines")
{
    const EntryPoint vs{"VSMain", ShaderStage::Vertex};
    const std::vector<std::pair<std::string, std::string>> defines{{"ALPHA_TEST", "1"}};
    const std::vector<fs::path> includes{"shaders/common"};

    const CompilerConfig glslang{Compiler::Glslang, "glslangValidator", "vulkan1.3"};
    const std::string g = build_compile_command(glslang, "a.hlsl", vs, defines, includes, "out.spv");
    CHECK(g.find("-V -D --target-env vulkan1.3") != std::string::npos);
    CHECK(g.find("-S vert") != std::string::npos);
    CHECK(g.find("-e VSMain") != std::string::npos);
    CHECK(g.find("-DALPHA_TEST=1") != std::string::npos);
    CHECK(g.find("\"-Ishaders/common\"") != std::string::npos);
    CHECK(g.find("-o \"out.spv\"") != std::string::npos);

    const CompilerConfig dxc{Compiler::Dxc, "dxc", "vulkan1.3"};
    const EntryPoint ps{"PSMain", ShaderStage::Pixel};
    const std::string d = build_compile_command(dxc, "a.hlsl", ps, defines, includes, "out.spv");
    CHECK(d.find("-spirv -fspv-target-env=vulkan1.3") != std::string::npos);
    CHECK(d.find("-T ps_6_6") != std::string::npos);
    CHECK(d.find("-E PSMain") != std::string::npos);
    CHECK(d.find("-D ALPHA_TEST=1") != std::string::npos);
    CHECK(d.find("-Fo \"out.spv\"") != std::string::npos);
}

TEST_CASE("Compile: source hash covers includes and ignores CR line endings")
{
    std::map<std::string, std::string> files{
        {"s/a.hlsl", "#include \"b.hlsli\"\nfloat4 main() : SV_Target { return 0; }\n"},
        {"s/b.hlsli", "#define X 1\n"},
    };
    std::string error;
    const auto h1 = compute_source_hash("s/a.hlsl", {}, memory_reader(files), &error);
    REQUIRE(h1.has_value());

    files["s/a.hlsl"] = "#include \"b.hlsli\"\r\nfloat4 main() : SV_Target { return 0; }\r\n";
    CHECK(compute_source_hash("s/a.hlsl", {}, memory_reader(files), &error) == h1);  // CRLF checkout

    files["s/b.hlsli"] = "#define X 2\n";
    CHECK(compute_source_hash("s/a.hlsl", {}, memory_reader(files), &error) != h1);  // include changed

    files["s/a.hlsl"] = "#include \"missing.hlsli\"\n";
    CHECK_FALSE(compute_source_hash("s/a.hlsl", {}, memory_reader(files), &error).has_value());
    CHECK(error.find("missing.hlsli") != std::string::npos);
}

TEST_CASE("Compile: generated header embeds words and a readable source hash")
{
    CompiledEntry e;
    e.symbol = "scene_cube_vs_main";
    e.entry = {"VSMain", ShaderStage::Vertex};
    e.spirv = {0x07230203u, 0x00010600u, 0u, 7u, 0u};
    EmbeddedHeaderInfo info{"SceneCube", "shaders/scene/cube.hlsl", "0123456789abcdef0123456789abcdef", "glslang"};
    const std::string text = generate_embedded_header(info, std::span<const CompiledEntry>(&e, 1));
    CHECK(text.find("inline constexpr uint32_t scene_cube_vs_main[] = {") != std::string::npos);
    CHECK(text.find("0x07230203,") != std::string::npos);
    CHECK(text.find("scene_cube_source_hash[] = \"0123456789abcdef0123456789abcdef\"") != std::string::npos);
    CHECK(text.find("namespace saige::shaders") != std::string::npos);
    CHECK(read_recorded_hash(text) == std::string("0123456789abcdef0123456789abcdef"));
    CHECK_FALSE(read_recorded_hash("// nothing here\n").has_value());
}
