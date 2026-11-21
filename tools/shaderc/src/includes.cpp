#include "saige/shaderc/includes.h"

#include <algorithm>
#include <cctype>
#include <deque>

namespace saige::shaderc {

namespace fs = std::filesystem;

namespace {

/// Removes comments while preserving line structure and string literals.
std::string stripComments(std::string_view src)
{
    std::string out;
    out.reserve(src.size());
    enum class State { Code, LineComment, BlockComment, String } state = State::Code;
    for (std::size_t i = 0; i < src.size(); ++i) {
        const char c = src[i];
        const char next = i + 1 < src.size() ? src[i + 1] : '\0';
        switch (state) {
        case State::Code:
            if (c == '/' && next == '/') {
                state = State::LineComment;
                ++i;
            } else if (c == '/' && next == '*') {
                state = State::BlockComment;
                ++i;
                out.push_back(' ');
            } else {
                if (c == '"') {
                    state = State::String;
                }
                out.push_back(c);
            }
            break;
        case State::LineComment:
            if (c == '\n') {
                state = State::Code;
                out.push_back('\n');
            }
            break;
        case State::BlockComment:
            if (c == '*' && next == '/') {
                state = State::Code;
                ++i;
            } else if (c == '\n') {
                out.push_back('\n');
            }
            break;
        case State::String:
            out.push_back(c);
            if (c == '\\' && next != '\0') {
                out.push_back(next);
                ++i;
            } else if (c == '"' || c == '\n') {
                state = State::Code;
            }
            break;
        }
    }
    return out;
}

} // namespace

std::vector<std::pair<std::string, bool>> extractIncludes(std::string_view source)
{
    std::vector<std::pair<std::string, bool>> result;
    const std::string clean = stripComments(source);
    std::size_t pos = 0;
    while (pos < clean.size()) {
        std::size_t end = clean.find('\n', pos);
        if (end == std::string::npos) {
            end = clean.size();
        }
        std::string_view line(clean.data() + pos, end - pos);
        pos = end + 1;

        auto skipSpace = [&line] {
            while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())) != 0) {
                line.remove_prefix(1);
            }
        };
        skipSpace();
        if (line.empty() || line.front() != '#') {
            continue;
        }
        line.remove_prefix(1);
        skipSpace();
        if (!line.starts_with("include")) {
            continue;
        }
        line.remove_prefix(7);
        skipSpace();
        if (line.empty()) {
            continue;
        }
        const char open = line.front();
        const char close = open == '<' ? '>' : (open == '"' ? '"' : '\0');
        if (close == '\0') {
            continue;  // macro-expanded include: not supported in shaders
        }
        const std::size_t closePos = line.find(close, 1);
        if (closePos == std::string_view::npos) {
            continue;
        }
        result.emplace_back(std::string(line.substr(1, closePos - 1)), open == '<');
    }
    return result;
}

IncludeScanResult scanIncludes(const fs::path& root, std::span<const fs::path> includeDirs, const FileReader& readFile)
{
    IncludeScanResult result;
    const fs::path rootNorm = root.lexically_normal();
    std::vector<fs::path> visited{rootNorm};
    std::deque<fs::path> queue{rootNorm};

    while (!queue.empty()) {
        const fs::path current = queue.front();
        queue.pop_front();
        const std::optional<std::string> text = readFile(current);
        if (!text) {
            if (current == rootNorm) {
                result.errors.push_back("cannot read '" + current.generic_string() + "'");
            }
            continue;  // missing includes are reported at the include site below
        }

        for (const auto& [target, angle] : extractIncludes(*text)) {
            std::vector<fs::path> candidates;
            if (!angle) {
                candidates.push_back((current.parent_path() / target).lexically_normal());
            }
            for (const fs::path& dir : includeDirs) {
                candidates.push_back((dir / target).lexically_normal());
            }

            std::optional<fs::path> resolved;
            for (const fs::path& c : candidates) {
                if (std::find(visited.begin(), visited.end(), c) != visited.end() || readFile(c).has_value()) {
                    resolved = c;
                    break;
                }
            }
            if (!resolved) {
                result.errors.push_back("'" + current.generic_string() + "': cannot resolve #include " +
                                        (angle ? "<" + target + ">" : "\"" + target + "\""));
                continue;
            }
            if (std::find(visited.begin(), visited.end(), *resolved) == visited.end()) {
                visited.push_back(*resolved);
                result.dependencies.push_back(*resolved);
                queue.push_back(*resolved);
            }
        }
    }
    return result;
}

} // namespace saige::shaderc
