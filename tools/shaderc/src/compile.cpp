#include "saige/shaderc/compile.hpp"

#include "saige/shaderc/cache_key.hpp"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace saige::shaderc {

namespace fs = std::filesystem;

namespace {

std::string quote(const std::string& arg)
{
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out.push_back(c);
        }
    }
    out += '"';
    return out;
}

std::string glslang_stage(ShaderStage stage)
{
    switch (stage) {
    case ShaderStage::Vertex: return "vert";
    case ShaderStage::Pixel: return "frag";
    case ShaderStage::Compute: return "comp";
    case ShaderStage::Mesh: return "mesh";
    case ShaderStage::Amplification: return "task";
    case ShaderStage::Library: return "rgen";
    }
    return "vert";
}

std::string strip_cr(std::string text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c != '\r') {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

std::string build_compile_command(const CompilerConfig& config, const fs::path& source, const EntryPoint& entry,
                                  std::span<const std::pair<std::string, std::string>> defines,
                                  std::span<const fs::path> include_dirs, const fs::path& output)
{
    std::string cmd = quote(config.executable.string());
    if (config.compiler == Compiler::Dxc) {
        cmd += " -spirv -fspv-target-env=" + config.target_env;
        cmd += " -T " + dxc_profile(entry.stage);
        cmd += " -E " + entry.name;
        cmd += " -HV 2021 -WX -O3";
        for (const fs::path& dir : include_dirs) {
            cmd += " -I " + quote(dir.string());
        }
        for (const auto& [name, value] : defines) {
            cmd += " -D " + name + "=" + value;
        }
        cmd += " -Fo " + quote(output.string()) + " " + quote(source.string());
    } else {
        cmd += " -V -D --target-env " + config.target_env;
        cmd += " -S " + glslang_stage(entry.stage);
        cmd += " -e " + entry.name;
        for (const fs::path& dir : include_dirs) {
            cmd += " " + quote("-I" + dir.string());
        }
        for (const auto& [name, value] : defines) {
            cmd += " -D" + name + "=" + value;
        }
        cmd += " -o " + quote(output.string()) + " " + quote(source.string());
    }
    return cmd;
}

std::optional<std::string> compute_source_hash(const fs::path& source, std::span<const fs::path> include_dirs,
                                               const FileReader& read_file, std::string* error)
{
    const std::optional<std::string> text = read_file(source);
    if (!text) {
        if (error != nullptr) {
            *error = "cannot read '" + source.generic_string() + "'";
        }
        return std::nullopt;
    }
    const IncludeScanResult scan = scan_includes(source, include_dirs, read_file);
    if (!scan.ok()) {
        if (error != nullptr) {
            *error = scan.errors.front();
        }
        return std::nullopt;
    }
    CacheKeyBuilder key;
    key.add("source", strip_cr(*text));
    key.add("include.count", scan.dependencies.size());
    for (const fs::path& dep : scan.dependencies) {
        key.add("include", strip_cr(read_file(dep).value_or("")));
    }
    return key.hex();
}

std::string to_snake_case(std::string_view id)
{
    std::string out;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (std::isupper(static_cast<unsigned char>(c)) != 0) {
            const char prev = i > 0 ? id[i - 1] : '\0';
            const char next = i + 1 < id.size() ? id[i + 1] : '\0';
            const bool boundary = i > 0 && prev != '_' &&
                                  (std::islower(static_cast<unsigned char>(prev)) != 0 ||
                                   std::isdigit(static_cast<unsigned char>(prev)) != 0 ||
                                   (std::isupper(static_cast<unsigned char>(prev)) != 0 &&
                                    std::islower(static_cast<unsigned char>(next)) != 0));
            if (boundary) {
                out.push_back('_');
            }
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string entry_symbol(const ShaderDecl& decl, const EntryPoint& entry, uint64_t permutation_key)
{
    std::string symbol = to_snake_case(decl.name) + "_" + to_snake_case(entry.name);
    if (!decl.options.empty()) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "_p%llx", static_cast<unsigned long long>(permutation_key));
        symbol += buf;
    }
    return symbol;
}

std::optional<std::vector<uint32_t>> read_spirv(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string bytes = ss.str();
    if (bytes.size() < 20 || bytes.size() % 4 != 0) {
        return std::nullopt;
    }
    std::vector<uint32_t> words(bytes.size() / 4);
    for (std::size_t i = 0; i < words.size(); ++i) {
        const auto b = [&](std::size_t k) { return static_cast<uint32_t>(static_cast<unsigned char>(bytes[i * 4 + k])); };
        words[i] = b(0) | (b(1) << 8) | (b(2) << 16) | (b(3) << 24);  // SPIR-V files are little-endian here
    }
    if (words[0] != 0x07230203u) {
        return std::nullopt;
    }
    return words;
}

std::string generate_embedded_header(const EmbeddedHeaderInfo& info, std::span<const CompiledEntry> entries)
{
    const std::string base = to_snake_case(info.shader_name);
    std::string out;
    out += "// Generated by saige-shaderc from " + info.source_label + ". Do not edit.\n";
    out += "// Regenerate: cmake --build <build-dir> --target saige_regenerate_shaders\n";
    out += "// saige-source-hash: " + info.source_hash + "\n";
    out += "#pragma once\n\n#include <cstdint>\n\nnamespace saige::shaders {\n\n";
    out += "inline constexpr char " + base + "_source_hash[] = \"" + info.source_hash + "\";\n";
    out += "inline constexpr char " + base + "_compiler[] = \"" + info.compiler_label + "\";\n";
    for (const CompiledEntry& e : entries) {
        out += "\n// " + e.entry.name + " (" + std::string(stage_name(e.entry.stage)) + ")";
        if (e.permutation_key != 0) {
            out += ", permutation key " + std::to_string(e.permutation_key);
        }
        out += ", " + std::to_string(e.spirv.size()) + " words\n";
        out += "inline constexpr uint32_t " + e.symbol + "[] = {";
        for (std::size_t i = 0; i < e.spirv.size(); ++i) {
            if (i % 8 == 0) {
                out += "\n   ";
            }
            char buf[16];
            std::snprintf(buf, sizeof(buf), " 0x%08x,", e.spirv[i]);
            out += buf;
        }
        out += "\n};\n";
    }
    out += "\n} // namespace saige::shaders\n";
    return out;
}

std::optional<std::string> read_recorded_hash(std::string_view header_text)
{
    constexpr std::string_view marker = "// saige-source-hash: ";
    const std::size_t pos = header_text.find(marker);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t start = pos + marker.size();
    const std::size_t end = header_text.find_first_of("\r\n", start);
    return std::string(header_text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
}

} // namespace saige::shaderc
