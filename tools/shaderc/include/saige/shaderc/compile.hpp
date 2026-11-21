// saige-shaderc — invoking an external HLSL -> SPIR-V compiler and embedding the results in C++ headers.
//
// Compilers: DXC (preferred, Vulkan SDK) or glslangValidator (HLSL front end; the 0.1 default on Linux CI).
// Generated headers are checked in under shaders/generated so engine builds need no shader compiler; each
// header records a hash of its HLSL sources, and `saige-shaderc verify` detects stale headers.
#pragma once

#include "saige/shaderc/annotations.hpp"
#include "saige/shaderc/includes.hpp"
#include "saige/shaderc/permutations.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace saige::shaderc {

enum class Compiler : uint8_t { Dxc, Glslang };

struct CompilerConfig {
    Compiler compiler = Compiler::Glslang;
    std::filesystem::path executable;  ///< Full path or a name resolved through PATH.
    std::string target_env = "vulkan1.3";
};

/// Command line for one entry point / permutation. Arguments are quoted for the host shell.
[[nodiscard]] std::string build_compile_command(const CompilerConfig& config, const std::filesystem::path& source,
                                                const EntryPoint& entry,
                                                std::span<const std::pair<std::string, std::string>> defines,
                                                std::span<const std::filesystem::path> include_dirs,
                                                const std::filesystem::path& output);

/// Hash of a shader's source and all transitive includes (contents only, CR characters ignored, so the value
/// is identical on every platform and checkout path).
[[nodiscard]] std::optional<std::string> compute_source_hash(const std::filesystem::path& source,
                                                             std::span<const std::filesystem::path> include_dirs,
                                                             const FileReader& read_file, std::string* error);

struct CompiledEntry {
    std::string symbol;  ///< C++ identifier, e.g. scene_cube_vs_main
    EntryPoint entry;
    uint64_t permutation_key = 0;
    std::vector<uint32_t> spirv;
};

struct EmbeddedHeaderInfo {
    std::string shader_name;    ///< @shader name
    std::string source_label;   ///< repository-relative path for the comment
    std::string source_hash;
    std::string compiler_label; ///< e.g. "glslangValidator 15.1.0"
};

/// C++ header text with one `inline constexpr uint32_t <symbol>[]` per compiled entry, plus
/// `<shader>_source_hash` and `<shader>_compiler` string constants.
[[nodiscard]] std::string generate_embedded_header(const EmbeddedHeaderInfo& info,
                                                   std::span<const CompiledEntry> entries);

/// Extracts the source hash recorded in a generated header (nullopt if absent).
[[nodiscard]] std::optional<std::string> read_recorded_hash(std::string_view header_text);

/// Converts identifiers like "SceneCube" / "VSMain" to "scene_cube" / "vs_main".
[[nodiscard]] std::string to_snake_case(std::string_view identifier);

/// Symbol for an entry: <shader>_<entry>[_p<key in hex>] (the suffix only when the shader has options).
[[nodiscard]] std::string entry_symbol(const ShaderDecl& decl, const EntryPoint& entry, uint64_t permutation_key);

/// Reads a SPIR-V binary; nullopt if missing, misaligned or without the SPIR-V magic number.
[[nodiscard]] std::optional<std::vector<uint32_t>> read_spirv(const std::filesystem::path& path);

} // namespace saige::shaderc
