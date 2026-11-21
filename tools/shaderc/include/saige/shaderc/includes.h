// saige-shaderc — `#include` dependency scanning (for incremental rebuilds and hot reload).
#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace saige::shaderc {

/// Reads a file's contents, or returns nullopt if it doesn't exist. Injected so tests can use an in-memory
/// file system (and the engine can route reads through Core's VFS, D-13).
using FileReader = std::function<std::optional<std::string>(const std::filesystem::path&)>;

struct IncludeScanResult {
    /// Every transitively included file (resolved, normalized), unique, in discovery order. Excludes the root.
    std::vector<std::filesystem::path> dependencies;
    std::vector<std::string> errors;
    [[nodiscard]] bool ok() const noexcept { return errors.empty(); }
};

/// Scans `root` and everything it includes. `#include "x"` resolves relative to the including file first,
/// then `includeDirs`. `#include <x>` searches `includeDirs` only. Includes inside `#if` blocks are always
/// followed, an over-approximation that is correct for dependency tracking. Include cycles are tolerated
/// (each file is visited once).
[[nodiscard]] IncludeScanResult scanIncludes(const std::filesystem::path& root,
                                             std::span<const std::filesystem::path> includeDirs,
                                             const FileReader& readFile);

/// Extracts include directives from one source text: {target, isAngleBracket}. Ignores `//` and `/* */` comments.
[[nodiscard]] std::vector<std::pair<std::string, bool>> extractIncludes(std::string_view source);

} // namespace saige::shaderc
