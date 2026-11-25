// Internal helpers shared by stage_model.cpp and prepared_model.cpp.
#pragma once

#include "saige/gfx_scene/stage_model.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace saige::gfx::detail {

inline void warn_once(StageReport& report, std::string message)
{
    if (std::find(report.warnings.begin(), report.warnings.end(), message) == report.warnings.end()) {
        report.warnings.push_back(std::move(message));
    }
}

inline std::string label(std::string_view name, const std::string& kind, std::size_t index, const std::string& item_name)
{
    std::string out(name);
    out += '/';
    out += kind;
    out += '[' + std::to_string(index) + ']';
    if (!item_name.empty()) {
        out += " '" + item_name + "'";
    }
    return out;
}

[[noreturn]] inline void fail(const std::string& what, const std::string& why)
{
    throw std::runtime_error(what + ": " + why);
}

/// A problem with what the asset says about one texture slot (a bad index, a missing or undecodable image). It is fatal for the base
/// color slot, as in 0.2; for the optional slots, which 0.2 never read, it leaves the slot neutral with a warning. A *device* failure
/// (GPU upload) is never a SlotProblem: it always fails the staging.
struct SlotProblem {
    std::string where;
    std::string why;
};

} // namespace saige::gfx::detail
