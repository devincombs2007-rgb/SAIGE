// saige-shaderc — offline shader tool (annotation validation, permutation listing, dependency scanning).
// Compilation through DXC is added once the third-party policy (issue #2, D-03) is settled.

#include "saige/shaderc/annotations.h"
#include "saige/shaderc/includes.h"
#include "saige/shaderc/permutations.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr const char* kVersion = "0.1.0";

std::optional<std::string> readFile(const std::filesystem::path& path)
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
                 "usage: saige-shaderc <command> <file.hlsl> [-I <dir>]...\n"
                 "commands:\n"
                 "  check         validate @shader/@entry/@option/@exclude annotations and the permutation budget\n"
                 "  permutations  list surviving permutations (key + defines)\n"
                 "  deps          list transitive #include dependencies\n"
                 "  --version     print the tool version\n");
    return 2;
}

bool loadDecl(const std::filesystem::path& file, saige::shaderc::ShaderDecl& out)
{
    const std::optional<std::string> source = readFile(file);
    if (!source) {
        std::fprintf(stderr, "error: cannot read '%s'\n", file.generic_string().c_str());
        return false;
    }
    saige::shaderc::ParseResult parsed = saige::shaderc::parseAnnotations(*source);
    for (const auto& d : parsed.errors) {
        std::fprintf(stderr, "%s:%d: error: %s\n", file.generic_string().c_str(), d.line, d.message.c_str());
    }
    if (!parsed.ok()) {
        return false;
    }
    out = std::move(*parsed.decl);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && std::string(argv[1]) == "--version") {
        std::printf("saige-shaderc %s\n", kVersion);
        return 0;
    }
    if (argc < 3) {
        return usage();
    }
    const std::string command = argv[1];
    const std::filesystem::path file = argv[2];
    std::vector<std::filesystem::path> includeDirs;
    for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-I" && i + 1 < argc) {
            includeDirs.emplace_back(argv[++i]);
        } else if (arg.rfind("-I", 0) == 0 && arg.size() > 2) {
            includeDirs.emplace_back(arg.substr(2));
        } else {
            return usage();
        }
    }

    if (command == "deps") {
        const auto scan = saige::shaderc::scanIncludes(file, includeDirs, readFile);
        for (const auto& dep : scan.dependencies) {
            std::printf("%s\n", dep.generic_string().c_str());
        }
        for (const auto& e : scan.errors) {
            std::fprintf(stderr, "error: %s\n", e.c_str());
        }
        return scan.ok() ? 0 : 1;
    }

    saige::shaderc::ShaderDecl decl;
    if (!loadDecl(file, decl)) {
        return 1;
    }
    const saige::shaderc::ExpandResult expanded = saige::shaderc::expandPermutations(decl);
    for (const auto& e : expanded.errors) {
        std::fprintf(stderr, "%s: error: %s\n", file.generic_string().c_str(), e.c_str());
    }

    if (command == "check") {
        if (expanded.ok()) {
            std::printf("%s: OK (%zu entries, %zu options, %zu permutations, %llu excluded, budget %u)\n",
                        decl.name.c_str(), decl.entries.size(), decl.options.size(), expanded.permutations.size(),
                        static_cast<unsigned long long>(expanded.excludedCount), decl.budget);
        }
        return expanded.ok() ? 0 : 1;
    }
    if (command == "permutations") {
        for (const auto& p : expanded.permutations) {
            std::printf("0x%016llx", static_cast<unsigned long long>(p.key));
            for (const auto& [name, value] : saige::shaderc::definesFor(decl, p)) {
                std::printf(" -D%s=%s", name.c_str(), value.c_str());
            }
            std::printf("\n");
        }
        return expanded.ok() ? 0 : 1;
    }
    return usage();
}
