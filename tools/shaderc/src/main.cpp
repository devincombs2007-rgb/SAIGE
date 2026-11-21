// saige-shaderc — offline shader tool: annotation validation, permutation listing, dependency scanning,
// compilation to SPIR-V through DXC or glslang, embedding into C++ headers, and stale-header verification.

#include "saige/shaderc/annotations.hpp"
#include "saige/shaderc/compile.hpp"
#include "saige/shaderc/includes.hpp"
#include "saige/shaderc/permutations.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#    define SAIGE_POPEN _popen
#    define SAIGE_PCLOSE _pclose
#else
#    define SAIGE_POPEN popen
#    define SAIGE_PCLOSE pclose
#endif

namespace {

namespace fs = std::filesystem;
using namespace saige::shaderc;

constexpr const char* tool_version = "0.2.0";

std::optional<std::string> read_file(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int usage()
{
    std::fprintf(stderr,
                 "usage: saige-shaderc <command> <file.hlsl> [options] [-I <dir>]...\n"
                 "commands:\n"
                 "  check         validate annotations and the permutation budget\n"
                 "  permutations  list surviving permutations (key + defines)\n"
                 "  deps          list transitive #include dependencies\n"
                 "  hash          print the source hash (source + includes)\n"
                 "  compile       compile every entry/permutation to SPIR-V\n"
                 "                  --compiler glslang|dxc  --compiler-path <exe>  --work-dir <dir>\n"
                 "                  [--embed <header.hpp> --label <source label>]\n"
                 "  verify        --embed <header.hpp>: fail if the generated header is stale\n"
                 "  --version     print the tool version\n");
    return 2;
}

std::string first_line_of_command(const std::string& command)
{
    std::string out;
    if (FILE* pipe = SAIGE_POPEN(command.c_str(), "r")) {
        char buf[256];
        if (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
            out = buf;
        }
        SAIGE_PCLOSE(pipe);
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}

struct Options {
    std::string command;
    fs::path file;
    std::vector<fs::path> include_dirs;
    std::map<std::string, std::string> named;
};

std::optional<Options> parse(int argc, char** argv)
{
    if (argc < 3) {
        return std::nullopt;
    }
    Options o;
    o.command = argv[1];
    o.file = argv[2];
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-I" && i + 1 < argc) {
            o.include_dirs.emplace_back(argv[++i]);
        } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            o.include_dirs.emplace_back(arg.substr(2));
        } else if (arg.rfind("--", 0) == 0 && i + 1 < argc) {
            o.named[arg.substr(2)] = argv[++i];
        } else {
            return std::nullopt;
        }
    }
    return o;
}

bool load_decl(const fs::path& file, ShaderDecl& out)
{
    const std::optional<std::string> source = read_file(file);
    if (!source) {
        std::fprintf(stderr, "error: cannot read '%s'\n", file.generic_string().c_str());
        return false;
    }
    ParseResult parsed = parse_annotations(*source);
    for (const auto& d : parsed.errors) {
        std::fprintf(stderr, "%s:%d: error: %s\n", file.generic_string().c_str(), d.line, d.message.c_str());
    }
    if (!parsed.ok()) {
        return false;
    }
    out = std::move(*parsed.decl);
    return true;
}

std::string named_or(const Options& o, const std::string& key, const std::string& fallback)
{
    const auto it = o.named.find(key);
    return it != o.named.end() ? it->second : fallback;
}

int run_compile(const Options& o, const ShaderDecl& decl, const ExpandResult& expanded)
{
    CompilerConfig config;
    const std::string compiler = named_or(o, "compiler", "glslang");
    if (compiler == "dxc") {
        config.compiler = Compiler::Dxc;
    } else if (compiler != "glslang") {
        std::fprintf(stderr, "error: unknown compiler '%s' (glslang or dxc)\n", compiler.c_str());
        return 2;
    }
    config.executable = named_or(o, "compiler-path", compiler == "dxc" ? "dxc" : "glslangValidator");
    const fs::path work_dir = named_or(o, "work-dir", ".");
    std::error_code ec;
    fs::create_directories(work_dir, ec);

    std::vector<CompiledEntry> compiled;
    for (const Permutation& perm : expanded.permutations) {
        const auto defines = defines_for(decl, perm);
        for (const EntryPoint& entry : decl.entries) {
            CompiledEntry ce;
            ce.entry = entry;
            ce.permutation_key = perm.key;
            ce.symbol = entry_symbol(decl, entry, perm.key);
            const fs::path out = work_dir / (ce.symbol + ".spv");
            const std::string cmd = build_compile_command(config, o.file, entry, defines, o.include_dirs, out);
            const int rc = std::system(cmd.c_str());
            if (rc != 0) {
                std::fprintf(stderr, "error: compiler failed (%d): %s\n", rc, cmd.c_str());
                return 1;
            }
            std::optional<std::vector<uint32_t>> words = read_spirv(out);
            if (!words) {
                std::fprintf(stderr, "error: '%s' is not a valid SPIR-V binary\n", out.generic_string().c_str());
                return 1;
            }
            ce.spirv = std::move(*words);
            std::printf("compiled %s (%zu words)\n", ce.symbol.c_str(), ce.spirv.size());
            compiled.push_back(std::move(ce));
        }
    }

    if (o.named.count("embed") == 0) {
        return 0;
    }
    std::string error;
    const std::optional<std::string> hash = compute_source_hash(o.file, o.include_dirs, read_file, &error);
    if (!hash) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    EmbeddedHeaderInfo info;
    info.shader_name = decl.name;
    info.source_label = named_or(o, "label", o.file.filename().string());
    info.source_hash = *hash;
    info.compiler_label = first_line_of_command("\"" + config.executable.string() + "\" --version");
    if (info.compiler_label.empty()) {
        info.compiler_label = compiler;
    }
    for (char& c : info.compiler_label) {
        if (c == '"' || c == '\\') {
            c = '\'';
        }
    }
    const fs::path header = o.named.at("embed");
    const std::string text = generate_embedded_header(info, compiled);
    if (read_file(header).value_or("") == text) {
        std::printf("%s is up to date\n", header.generic_string().c_str());
        return 0;
    }
    std::ofstream out(header, std::ios::binary);
    out << text;
    if (!out) {
        std::fprintf(stderr, "error: cannot write '%s'\n", header.generic_string().c_str());
        return 1;
    }
    std::printf("wrote %s\n", header.generic_string().c_str());
    return 0;
}

int run_hash_or_verify(const Options& o)
{
    std::string error;
    const std::optional<std::string> hash = compute_source_hash(o.file, o.include_dirs, read_file, &error);
    if (!hash) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    if (o.command == "hash") {
        std::printf("%s\n", hash->c_str());
        return 0;
    }
    if (o.named.count("embed") == 0) {
        return usage();
    }
    const std::string header_path = o.named.at("embed");
    const std::optional<std::string> header = read_file(header_path);
    const std::optional<std::string> recorded = header ? read_recorded_hash(*header) : std::nullopt;
    if (!recorded || *recorded != *hash) {
        std::fprintf(stderr,
                     "error: %s is stale for %s (recorded %s, current %s). Regenerate with "
                     "`cmake --build <build-dir> --target saige_regenerate_shaders`.\n",
                     header_path.c_str(), o.file.generic_string().c_str(), recorded ? recorded->c_str() : "<none>",
                     hash->c_str());
        return 1;
    }
    std::printf("%s: up to date\n", header_path.c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string(argv[1]) == "--version") {
        std::printf("saige-shaderc %s\n", tool_version);
        return 0;
    }
    const std::optional<Options> parsed = parse(argc, argv);
    if (!parsed) {
        return usage();
    }
    const Options& o = *parsed;

    if (o.command == "deps") {
        const auto scan = scan_includes(o.file, o.include_dirs, read_file);
        for (const auto& dep : scan.dependencies) {
            std::printf("%s\n", dep.generic_string().c_str());
        }
        for (const auto& e : scan.errors) {
            std::fprintf(stderr, "error: %s\n", e.c_str());
        }
        return scan.ok() ? 0 : 1;
    }
    if (o.command == "hash" || o.command == "verify") {
        return run_hash_or_verify(o);
    }

    ShaderDecl decl;
    if (!load_decl(o.file, decl)) {
        return 1;
    }
    const ExpandResult expanded = expand_permutations(decl);
    for (const auto& e : expanded.errors) {
        std::fprintf(stderr, "%s: error: %s\n", o.file.generic_string().c_str(), e.c_str());
    }
    if (!expanded.ok()) {
        return 1;
    }

    if (o.command == "check") {
        std::printf("%s: OK (%zu entries, %zu options, %zu permutations, %llu excluded, budget %u)\n", decl.name.c_str(),
                    decl.entries.size(), decl.options.size(), expanded.permutations.size(),
                    static_cast<unsigned long long>(expanded.excluded_count), decl.budget);
        return 0;
    }
    if (o.command == "permutations") {
        for (const auto& p : expanded.permutations) {
            std::printf("0x%016llx", static_cast<unsigned long long>(p.key));
            for (const auto& [name, value] : defines_for(decl, p)) {
                std::printf(" -D%s=%s", name.c_str(), value.c_str());
            }
            std::printf("\n");
        }
        return 0;
    }
    if (o.command == "compile") {
        return run_compile(o, decl, expanded);
    }
    return usage();
}
