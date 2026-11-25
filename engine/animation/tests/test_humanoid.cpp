// The humanoid bone map and the BVH importer.
#include "saige/gfx_test/saige_test.hpp"

#include "fixtures.hpp"
#include "saige/animation/bvh.hpp"

using namespace saige;
using namespace saige::anim;

namespace {

std::vector<std::string> names(std::initializer_list<const char*> n) { return {n.begin(), n.end()}; }

} // namespace

TEST_CASE("Humanoid: the rigs of Quaternius (Rigify), Unreal, Mixamo and CMU all map, and a missing core bone is rejected") {
    const std::vector<std::string> rigify = names({"root", "DEF-hips", "DEF-spine.001", "DEF-spine.002", "DEF-spine.003", "DEF-neck", "DEF-head", "DEF-shoulder.L", "DEF-upper_arm.L", "DEF-forearm.L",
                                                   "DEF-hand.L", "DEF-shoulder.R", "DEF-upper_arm.R", "DEF-forearm.R", "DEF-hand.R", "DEF-thigh.L", "DEF-shin.L", "DEF-foot.L", "DEF-toe.L",
                                                   "DEF-thigh.R", "DEF-shin.R", "DEF-foot.R", "DEF-toe.R", "DEF-f_index.01.L"});
    const auto a = map_humanoid(rigify);
    REQUIRE(a.has_value());
    CHECK((*a)[Bone::Hips] == 1);
    CHECK((*a)[Bone::Spine] == 2);
    CHECK((*a)[Bone::Chest] == 4);
    CHECK((*a)[Bone::LeftUpperLeg] == 15 && (*a)[Bone::RightUpperLeg] == 19);
    CHECK((*a)[Bone::LeftToes] == 18 && (*a)[Bone::RightToes] == 22);
    CHECK((*a)[Bone::LeftLowerArm] == 9 && (*a)[Bone::RightLowerArm] == 13);

    const fixtures::Biped unreal = fixtures::make_biped(1.0f, fixtures::Naming::Unreal);
    const fixtures::Biped mixamo = fixtures::make_biped(1.0f, fixtures::Naming::Mixamo);
    for (std::size_t b = 0; b < bone_count; ++b) CHECK(unreal.map.joint[b] == mixamo.map.joint[b]);  // the same roles on the same joints
    CHECK(unreal.map.has(Bone::LeftToes));

    const std::vector<std::string> cmu = names({"Hips", "LHipJoint", "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "RHipJoint", "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase", "LowerBack",
                                                "Spine", "Spine1", "Neck", "Neck1", "Head", "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand", "RightShoulder", "RightArm", "RightForeArm", "RightHand"});
    const auto c = map_humanoid(cmu);
    REQUIRE(c.has_value());
    CHECK((*c)[Bone::Hips] == 0 && (*c)[Bone::LeftUpperLeg] == 2 && (*c)[Bone::LeftLowerLeg] == 3 && (*c)[Bone::LeftFoot] == 4 && (*c)[Bone::LeftToes] == 5);
    CHECK((*c)[Bone::Spine] == 11);  // LowerBack is the lowest segment
    CHECK((*c)[Bone::Chest] == 13);  // Spine1 the highest
    CHECK((*c)[Bone::LeftUpperArm] == 18 && (*c)[Bone::RightLowerArm] == 23);

    std::string why;
    CHECK(!map_humanoid(names({"Hips", "Spine", "LeftUpLeg", "LeftLeg", "LeftFoot"}), &why).has_value());  // no right leg
    CHECK(why.find("missing core bone") != std::string::npos);
    CHECK(!map_humanoid(names({"a", "b", "c"})).has_value());
    CHECK(!map_humanoid(std::vector<std::string>{}).has_value());
}

TEST_CASE("Humanoid: rig analysis finds the facing direction from the legs, whichever way the rig was built") {
    for (const float yaw : {0.0f, 1.0f, -2.0f, 3.0f}) {
        const fixtures::Biped b = fixtures::make_biped(1.0f, fixtures::Naming::Unreal, yaw);
        CHECK(std::fabs(b.rig.forward.x - std::cos(yaw)) < 1e-3f);
        CHECK(std::fabs(b.rig.forward.y - std::sin(yaw)) < 1e-3f);
        CHECK(std::fabs(b.rig.hips_height - 0.95f) < 1e-3f);
        CHECK(std::fabs(b.rig.leg_length - (0.43f + 0.40f + 0.07f)) < 1e-3f);
    }
}

TEST_CASE("BVH: channels, order, units and the engine's axes") {
    // A root with a position and ZYX rotation channels, and one child 10 units up (+Y in the file).
    const char* text =
        "HIERARCHY\nROOT Hips\n{\n OFFSET 0 0 0\n CHANNELS 6 Xposition Yposition Zposition Zrotation Yrotation Xrotation\n"
        " JOINT Child\n {\n  OFFSET 0 10 0\n  CHANNELS 3 Zrotation Yrotation Xrotation\n  End Site\n  {\n   OFFSET 0 5 0\n  }\n }\n}\n"
        "MOTION\nFrames: 3\nFrame Time: 0.1\n"
        "0 0 0 0 0 0 0 0 0\n"
        "1 2 3 90 0 0 0 0 0\n"
        "4 5 6 0 90 0 0 0 0\n";
    BvhOptions opt;
    opt.unit_scale = 0.1f;
    opt.first_frame_is_rest = false;
    const BvhResult r = parse_bvh(text, opt);
    REQUIRE(r.ok());
    CHECK(r.skin->joint_count() == 2);  // the end site is not a joint
    CHECK(r.skin->joint_names[0] == "Hips" && r.skin->joint_names[1] == "Child");
    CHECK(r.skin->parent[1] == 0);
    // Rest: the child is 1 m up (file +Y is the engine's +Z).
    CHECK(std::fabs(r.skin->rest[1].position.z - 1.0f) < 1e-5f && std::fabs(r.skin->rest[1].position.x) < 1e-5f && std::fabs(r.skin->rest[1].position.y) < 1e-5f);
    CHECK(std::fabs(r.frame_time - 0.1f) < 1e-6f);
    CHECK(std::fabs(r.clip->duration - 0.2f) < 1e-5f);
    const gfx::ClipBinding bind = gfx::bind_clip(*r.clip, *r.skin);
    // Frame 1: the root moved to the file's (1, 2, 3) x 0.1 = engine (0.3, 0.1, 0.2) and turned 90 degrees about the file's Z, which is the engine's +X: the child's offset (0, 0, 1) goes to (0, -1, 0).
    gfx::Pose pose = gfx::rest_pose(*r.skin);
    gfx::evaluate_clip(*r.clip, bind, 0.1f, false, pose);
    const std::vector<JointWorld> w = joint_world(*r.skin, pose);
    CHECK(std::fabs(w[0].position.x - 0.3f) < 1e-5f && std::fabs(w[0].position.y - 0.1f) < 1e-5f && std::fabs(w[0].position.z - 0.2f) < 1e-5f);
    CHECK(std::fabs(w[1].position.x - 0.3f) < 1e-4f && std::fabs(w[1].position.y - (0.1f - 1.0f)) < 1e-4f && std::fabs(w[1].position.z - 0.2f) < 1e-4f);
    // Frame 2: a 90 degree turn about the file's Y (the engine's +Z) leaves a vertical offset where it is.
    gfx::evaluate_clip(*r.clip, bind, 0.2f, false, pose);
    const std::vector<JointWorld> w2 = joint_world(*r.skin, pose);
    CHECK(std::fabs(w2[1].position.x - w2[0].position.x) < 1e-4f && std::fabs(w2[1].position.y - w2[0].position.y) < 1e-4f && std::fabs(w2[1].position.z - w2[0].position.z - 1.0f) < 1e-4f);

    // With the CMU convention the first frame is the rest pose and not part of the clip.
    BvhOptions cmu;
    cmu.unit_scale = 0.1f;
    const BvhResult r2 = parse_bvh(text, cmu);
    REQUIRE(r2.ok());
    CHECK(std::fabs(r2.clip->duration - 0.1f) < 1e-5f);
    // Rest is frame 0 (root at the origin, no rotation): unchanged here, but the root has a translation channel so it is included in the rest position.
    CHECK(std::fabs(r2.skin->rest[0].position.x) < 1e-6f);
}

TEST_CASE("BVH: malformed files are rejected with a message and no crash") {
    CHECK(!parse_bvh("").ok());
    CHECK(!parse_bvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n").ok());                                          // truncated hierarchy
    CHECK(!parse_bvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n CHANNELS 3 Xposition Yposition Wposition\n}\nMOTION\nFrames: 2\nFrame Time: .1\n0 0 0\n0 0 0\n").ok());  // unknown channel
    CHECK(!parse_bvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n CHANNELS 3 Xposition Yposition Zposition\n}\nMOTION\nFrames: 5\nFrame Time: .1\n0 0 0\n").ok());               // short data
    CHECK(!parse_bvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n CHANNELS 3 Xposition Yposition Zposition\n}\nMOTION\nFrames: 2\nFrame Time: 0\n0 0 0\n0 0 0\n").ok());           // zero frame time
    CHECK(!parse_bvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n CHANNELS 3 Xposition Yposition Zposition\n}\nMOTION\nFrames: 2\nFrame Time: .1\n0 0 nan\n0 0 0\n").ok());      // non-finite
    BvhOptions tiny;
    tiny.max_joints = 1;
    CHECK(!parse_bvh("HIERARCHY\nROOT A\n{\n OFFSET 0 0 0\n CHANNELS 3 Xposition Yposition Zposition\n JOINT B\n {\n  OFFSET 0 0 0\n  CHANNELS 3 Xrotation Yrotation Zrotation\n }\n}\nMOTION\nFrames: 2\nFrame Time: .1\n0 0 0 0 0 0\n0 0 0 0 0 0\n", tiny).ok());
    std::string deep;
    for (int i = 0; i < 3; ++i) deep += "JOINT J\n{\nOFFSET 0 0 0\n";
    const BvhResult bad = parse_bvh("HIERARCHY\nROOT A\n{\nOFFSET 0 0 0\nCHANNELS 3 Xposition Yposition Zposition\n" + deep);
    CHECK(!bad.ok());
    CHECK(!bad.error.empty());
}
