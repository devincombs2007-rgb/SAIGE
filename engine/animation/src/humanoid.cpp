#include "saige/animation/humanoid.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <utility>

namespace saige::anim {

namespace {

enum class Side { None, Left, Right };

struct Parsed {
    std::string base;  // lower case, no prefix, no separators, no side
    Side side = Side::None;
};

bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

Parsed parse_name(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (const char c : raw) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const std::size_t colon = s.find(':');
    if (colon != std::string::npos) s = s.substr(colon + 1);        // "mixamorig:LeftArm"
    if (starts_with(s, "def-") || starts_with(s, "def_")) s = s.substr(4);  // Rigify deformation bones
    if (starts_with(s, "bip01_")) s = s.substr(6);
    Parsed p;
    // Side: a word, or a trailing separator + l/r.
    for (const char* word : {"left", "right"}) {
        const std::size_t at = s.find(word);
        if (at != std::string::npos) {
            p.side = word[0] == 'l' ? Side::Left : Side::Right;
            s.erase(at, std::char_traits<char>::length(word));
            break;
        }
    }
    if (p.side == Side::None && s.size() >= 2) {
        const char last = s.back(), sep = s[s.size() - 2];
        if ((last == 'l' || last == 'r') && (sep == '_' || sep == '.' || sep == '-' || sep == ' ')) {
            p.side = last == 'l' ? Side::Left : Side::Right;
            s.erase(s.size() - 2);
        }
    }
    for (const char c : s) {
        if (c != '_' && c != '.' && c != '-' && c != ' ') p.base.push_back(c);
    }
    return p;
}

bool one_of(const std::string& base, std::initializer_list<const char*> names) {
    for (const char* n : names) {
        if (base == n) return true;
    }
    return false;
}

constexpr int not_a_spine = -1000;

// "spine", "spine01", "spine1", "spine001": the index of a spine segment (a "lowerback" counts as the one below "spine"), not_a_spine if the base is not a spine name.
int spine_index(const std::string& base) {
    if (base == "lowerback") return -1;
    if (base.rfind("spine", 0) != 0) return not_a_spine;
    const std::string digits = base.substr(5);
    if (digits.empty()) return 0;
    for (const char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return not_a_spine;
    }
    return std::atoi(digits.c_str());
}

} // namespace

const char* bone_name(Bone b) noexcept {
    static const char* const names[] = {"hips", "spine", "chest", "neck", "head", "left_shoulder", "left_upper_arm", "left_lower_arm", "left_hand", "right_shoulder", "right_upper_arm",
                                        "right_lower_arm", "right_hand", "left_upper_leg", "left_lower_leg", "left_foot", "left_toes", "right_upper_leg", "right_lower_leg", "right_foot", "right_toes"};
    return static_cast<std::size_t>(b) < bone_count ? names[static_cast<std::size_t>(b)] : "?";
}

std::optional<HumanoidMap> map_humanoid(const std::vector<std::string>& joint_names, std::string* why) {
    HumanoidMap map;
    std::vector<std::pair<int, std::int32_t>> spines;  // (segment index, joint)
    const auto fail = [&](const std::string& reason) -> std::optional<HumanoidMap> {
        if (why != nullptr) *why = reason;
        return std::nullopt;
    };
    const auto assign = [&](Bone b, std::int32_t joint) {
        if (!map.has(b)) map.joint[static_cast<std::size_t>(b)] = joint;  // the first joint that matches keeps the role
    };
    for (std::size_t j = 0; j < joint_names.size(); ++j) {
        const Parsed p = parse_name(joint_names[j]);
        const std::int32_t joint = static_cast<std::int32_t>(j);
        const bool L = p.side == Side::Left, R = p.side == Side::Right, none = p.side == Side::None;
        if (none && one_of(p.base, {"hips", "pelvis", "hip"})) assign(Bone::Hips, joint);
        else if (none && spine_index(p.base) != not_a_spine) spines.push_back({spine_index(p.base), joint});
        else if (none && one_of(p.base, {"chest", "upperchest"})) assign(Bone::Chest, joint);
        else if (none && one_of(p.base, {"neck", "neck01", "neck1"})) assign(Bone::Neck, joint);
        else if (none && one_of(p.base, {"head"})) assign(Bone::Head, joint);
        else if ((L || R) && one_of(p.base, {"shoulder", "clavicle"})) assign(L ? Bone::LeftShoulder : Bone::RightShoulder, joint);
        else if ((L || R) && one_of(p.base, {"upperarm", "arm"})) assign(L ? Bone::LeftUpperArm : Bone::RightUpperArm, joint);
        else if ((L || R) && one_of(p.base, {"forearm", "lowerarm"})) assign(L ? Bone::LeftLowerArm : Bone::RightLowerArm, joint);
        else if ((L || R) && one_of(p.base, {"hand"})) assign(L ? Bone::LeftHand : Bone::RightHand, joint);
        else if ((L || R) && one_of(p.base, {"thigh", "upleg", "upperleg"})) assign(L ? Bone::LeftUpperLeg : Bone::RightUpperLeg, joint);
        else if ((L || R) && one_of(p.base, {"calf", "shin", "leg", "lowerleg"})) assign(L ? Bone::LeftLowerLeg : Bone::RightLowerLeg, joint);
        else if ((L || R) && one_of(p.base, {"foot"})) assign(L ? Bone::LeftFoot : Bone::RightFoot, joint);
        else if ((L || R) && one_of(p.base, {"toe", "toes", "toebase", "ball"})) assign(L ? Bone::LeftToes : Bone::RightToes, joint);
    }
    if (!spines.empty()) {
        std::sort(spines.begin(), spines.end());
        assign(Bone::Spine, spines.front().second);
        if (!map.has(Bone::Chest)) assign(Bone::Chest, spines.back().second);
    }
    for (const Bone core : {Bone::Hips, Bone::Spine, Bone::LeftUpperLeg, Bone::LeftLowerLeg, Bone::LeftFoot, Bone::RightUpperLeg, Bone::RightLowerLeg, Bone::RightFoot}) {
        if (!map.has(core)) return fail(std::string("missing core bone: ") + bone_name(core));
    }
    for (std::size_t a = 0; a < bone_count; ++a) {
        for (std::size_t b = a + 1; b < bone_count; ++b) {
            if (map.joint[a] >= 0 && map.joint[a] == map.joint[b] && !((a == static_cast<std::size_t>(Bone::Spine) && b == static_cast<std::size_t>(Bone::Chest)))) {
                return fail(std::string("one joint claimed by two roles: ") + bone_name(static_cast<Bone>(a)) + " and " + bone_name(static_cast<Bone>(b)));
            }
        }
    }
    return map;
}

std::optional<HumanoidMap> map_humanoid(const gfx::Skin& skin, std::string* why) {
    if (skin.joint_names.size() != skin.joint_count()) {
        if (why != nullptr) *why = "the skin carries no joint names";
        return std::nullopt;
    }
    return map_humanoid(skin.joint_names, why);
}

} // namespace saige::anim
