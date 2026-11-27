#pragma once

#include "saige/editor_scene.hpp"

#include <cstdint>
#include <vector>

namespace saige::editor {

inline constexpr std::uint32_t kEditorProjectMagic = 0x53454431u;  // "SED1"
inline constexpr std::uint16_t kEditorProjectVersion = 1;
inline constexpr std::uint64_t kMaxEditorNodes = 1000000ULL;
inline constexpr std::uint32_t kMaxEditorStringLen = 65536u;

class EditorProjectSerializer {
 public:
  [[nodiscard]] static std::vector<std::uint8_t> serialize(
      const EditorScene& scene);
  [[nodiscard]] static bool deserialize(const std::vector<std::uint8_t>& blob,
                                        EditorScene& out_scene);
};

}  // namespace saige::editor
