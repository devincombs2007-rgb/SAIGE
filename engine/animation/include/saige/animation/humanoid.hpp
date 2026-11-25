// SAIGE animation: a humanoid skeleton described by bone roles, so clips and rigs from different sources (Quaternius, Unreal, Mixamo, CMU) can be used interchangeably.
//
// `map_humanoid` finds the joints that play each role by name (case, prefixes such as "DEF-" or "mixamorig:", and the separators `_`, `.`, `-` and space are ignored; sides are told from a
// "left"/"right" word or an L/R suffix after a separator). A rig that lacks any bone of the core set (hips, one spine, both legs and feet) is rejected, because locomotion and IK need them; optional
// bones (neck, head, arms, toes) may be absent and read as -1.
#pragma once

#include "saige/gfx_assets/gltf.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace saige::anim {

enum class Bone : std::uint8_t {
    Hips, Spine, Chest, Neck, Head,
    LeftShoulder, LeftUpperArm, LeftLowerArm, LeftHand,
    RightShoulder, RightUpperArm, RightLowerArm, RightHand,
    LeftUpperLeg, LeftLowerLeg, LeftFoot, LeftToes,
    RightUpperLeg, RightLowerLeg, RightFoot, RightToes,
    Count
};
inline constexpr std::size_t bone_count = static_cast<std::size_t>(Bone::Count);

struct HumanoidMap {
    std::array<std::int32_t, bone_count> joint;  // joint index in the skin per role, -1 when the rig has none
    HumanoidMap() { joint.fill(-1); }
    [[nodiscard]] std::int32_t operator[](Bone b) const noexcept { return joint[static_cast<std::size_t>(b)]; }
    [[nodiscard]] bool has(Bone b) const noexcept { return (*this)[b] >= 0; }
};

// nullopt (with the reason in `why` if given) when a core bone is missing or two roles claim the same joint.
[[nodiscard]] std::optional<HumanoidMap> map_humanoid(const std::vector<std::string>& joint_names, std::string* why = nullptr);
[[nodiscard]] std::optional<HumanoidMap> map_humanoid(const gfx::Skin& skin, std::string* why = nullptr);

[[nodiscard]] const char* bone_name(Bone b) noexcept;

} // namespace saige::anim
