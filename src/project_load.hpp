#pragma once

#include "saige/project.hpp"

namespace saige::detail {

struct ValidatedProject {
    Project project;
    std::filesystem::path root;
    std::vector<WorldMatrix> matrices; // Same order as project.nodes.
};

[[nodiscard]] ValidatedProject load_validated_project(const std::filesystem::path& path);

} // namespace saige::detail
