// glTF skins, joints and weights, animation clips, and the CPU posing and skinning reference (gltf.hpp, animation.hpp).
//
// The asset is an "arm" of three joints stacked along glTF +Y (root at the origin, then 1 m up, then 1 m up again) with a six-vertex strip
// skinned to them. Expected positions are computed here from first principles (rotations about glTF +Z of a known pivot), not by calling the
// code under test, then converted to engine space with the documented (x, y, z) -> (z, x, y).

#include "saige/gfx_test/saige_test.hpp"

#include "saige/gfx_assets/animation.hpp"
#include "saige/gfx_assets/gltf.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace saige::gfx;
using saige::render::Float3;
using saige::render::Float4;
using saige::render::Mat4;

namespace {

struct Blob {
    std::vector<uint8_t> bytes;
    void f32(float v)
    {
        uint32_t u;
        std::memcpy(&u, &v, 4);
        for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>((u >> (8 * i)) & 0xFF));
    }
    void u8(uint8_t v) { bytes.push_back(v); }
    void u16(uint16_t v) { bytes.push_back(static_cast<uint8_t>(v & 0xFF)); bytes.push_back(static_cast<uint8_t>(v >> 8)); }
    void pad4() { while (bytes.size() % 4 != 0) bytes.push_back(0); }
    std::size_t size() const { return bytes.size(); }
};

std::string base64(const std::vector<uint8_t>& raw)
{
    static const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < raw.size(); i += 3) {
        const uint32_t b0 = raw[i], b1 = i + 1 < raw.size() ? raw[i + 1] : 0, b2 = i + 2 < raw.size() ? raw[i + 2] : 0;
        const uint32_t t = (b0 << 16) | (b1 << 8) | b2;
        out += a[(t >> 18) & 63];
        out += a[(t >> 12) & 63];
        out += i + 1 < raw.size() ? a[(t >> 6) & 63] : '=';
        out += i + 2 < raw.size() ? a[t & 63] : '=';
    }
    return out;
}

struct Options {
    bool reversed = false;          // list the joints bottom-up (tip first), so a child precedes... follows its parent in a different order
    bool joints_without_weights = false;
    bool duplicate_joint = false;
    float weight_scale = 1.0f;      // scales every weight (unnormalized weights)
    float negative_weight = 0.0f;   // a negative weight on vertex 0
    int bad_joint_index = -1;       // vertex 0 names this joint
    bool decreasing_times = false;
    bool short_output = false;
    bool skin_on_mesh_without_joints = false;
    int bad_skin_index = -1;
    int extra_node_between = 0;     // a non-joint node between joint 1 and joint 2
    bool no_normals = false;        // flat normals are generated and the primitive is de-indexed
    bool tip_rest_rotation = false; // the tip joint's rest transform is a 90 degree rotation about glTF z (its inverse bind matrix accounts for it)
};

/// Builds the arm. Joint j of the glTF file (in `joints` order) is node `order[j]`, so a reversed file lists tip, mid, root.
std::string arm_gltf(const Options& o = {})
{
    // Logical joints: 0 root, 1 mid, 2 tip. File order of joints: identity, or reversed.
    const int order[3] = {o.reversed ? 2 : 0, 1, o.reversed ? 0 : 2};  // file joint slot -> logical joint
    int slot_of[3];
    for (int s = 0; s < 3; ++s) slot_of[order[s]] = s;

    Blob b;
    // 0: positions (6 x vec3): A(0,0,0) B(0,1,0) C(0,2,0) D(.5,1,0) E(.5,2,0) F(.5,0,0), glTF space.
    const float pos[6][3] = {{0, 0, 0}, {0, 1, 0}, {0, 2, 0}, {0.5f, 1, 0}, {0.5f, 2, 0}, {0.5f, 0, 0}};
    for (const auto& p : pos) { b.f32(p[0]); b.f32(p[1]); b.f32(p[2]); }
    const std::size_t joints_at = b.size();
    // Logical bindings: A,F -> root; B -> mid; C -> tip; D -> root/mid half and half; E -> mid/tip half and half.
    const int jl[6][4] = {{0, 0, 0, 0}, {1, 0, 0, 0}, {2, 0, 0, 0}, {0, 1, 0, 0}, {1, 2, 0, 0}, {0, 0, 0, 0}};
    for (int v = 0; v < 6; ++v) {
        for (int k = 0; k < 4; ++k) {
            int logical = jl[v][k];
            int index = slot_of[logical];
            if (v == 0 && k == 0 && o.bad_joint_index >= 0) index = o.bad_joint_index;
            b.u8(static_cast<uint8_t>(index));
        }
    }
    b.pad4();
    const std::size_t weights_at = b.size();
    const float w[6][4] = {{1, 0, 0, 0}, {1, 0, 0, 0}, {1, 0, 0, 0}, {0.5f, 0.5f, 0, 0}, {0.5f, 0.5f, 0, 0}, {1, 0, 0, 0}};
    for (int v = 0; v < 6; ++v) {
        for (int k = 0; k < 4; ++k) {
            float x = w[v][k] * o.weight_scale;
            if (v == 0 && k == 1 && o.negative_weight != 0.0f) x = o.negative_weight;
            b.f32(x);
        }
    }
    const std::size_t indices_at = b.size();
    for (const int i : {0, 1, 3, 0, 3, 5, 1, 2, 4, 1, 4, 3}) b.u16(static_cast<uint16_t>(i));
    b.pad4();
    const std::size_t ibm_at = b.size();
    for (int s = 0; s < 3; ++s) {
        const float ty = -static_cast<float>(order[s]);  // inverse bind of logical joint j: translate by (0, -j, 0)
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, ty, 0, 1};
        if (o.tip_rest_rotation && order[s] == 2) {  // inverse of T(0,2,0) R_z(90): R_z(-90) T(0,-2,0), column-major
            const float r[16] = {0, -1, 0, 0, 1, 0, 0, 0, 0, 0, 1, 0, -2, 0, 0, 1};
            for (int k = 0; k < 16; ++k) m[k] = r[k];
        }
        for (const float f : m) b.f32(f);
    }
    // Animation 0 on logical joints: mid rotates about glTF +Z (0 -> 90 degrees over 1 s, LINEAR); tip translates (STEP: (0,1,0) then (0,2,0) at 0.5 s).
    const std::size_t t01_at = b.size();
    if (o.decreasing_times) { b.f32(1.0f); b.f32(0.0f); } else { b.f32(0.0f); b.f32(1.0f); }
    const std::size_t rot_at = b.size();
    const float half = std::sqrt(0.5f);
    for (const float f : {0.0f, 0.0f, 0.0f, 1.0f}) b.f32(f);
    for (const float f : {0.0f, 0.0f, half, half}) b.f32(f);  // +90 degrees about glTF z
    const std::size_t t_step_at = b.size();
    b.f32(0.0f);
    b.f32(0.5f);
    const std::size_t step_at = b.size();
    for (const float f : {0.0f, 1.0f, 0.0f, 0.0f, 2.0f, 0.0f}) b.f32(f);
    // Animation 1: root translation with CUBICSPLINE: keys at 0 and 1 s, (0,0,0) -> (2,0,0), out-tangent 3 m/s at the first key, in-tangent -1 m/s at the second.
    const std::size_t cubic_at = b.size();
    for (const float f : {/*in0*/ 0.0f, 0.0f, 0.0f, /*v0*/ 0.0f, 0.0f, 0.0f, /*out0*/ 3.0f, 0.0f, 0.0f, /*in1*/ -1.0f, 0.0f, 0.0f, /*v1*/ 2.0f, 0.0f, 0.0f, /*out1*/ 0.0f, 0.0f, 0.0f}) b.f32(f);
    const std::size_t normals_at = b.size();
    for (int v = 0; v < 6; ++v) { b.f32(0.0f); b.f32(0.0f); b.f32(1.0f); }
    std::string json = "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0,3]}],";
    // nodes: 0 root (children 1), 1 mid (child 2 or an intermediate), 2 tip, 3 skinned mesh at x = 5.
    const std::string mid_children = o.extra_node_between ? "[4]" : "[2]";
    json += "\"nodes\":[{\"name\":\"root\",\"children\":[1]},{\"name\":\"mid\",\"translation\":[0,1,0],\"children\":" + mid_children + "},{\"name\":\"tip\",\"translation\":[0,1,0]" + std::string(o.tip_rest_rotation ? ",\"rotation\":[0,0,0.70710678,0.70710678]" : "") + "},"
            "{\"name\":\"arm\",\"mesh\":0,\"skin\":" + std::to_string(o.bad_skin_index >= 0 ? o.bad_skin_index : 0) + ",\"translation\":[5,0,0]}" +
            (o.extra_node_between ? ",{\"name\":\"gap\",\"children\":[2]}" : "") + "],";
    json += "\"skins\":[{\"name\":\"armature\",\"joints\":[";
    for (int s = 0; s < 3; ++s) json += (s ? "," : "") + std::to_string(o.duplicate_joint && s == 2 ? order[0] : order[s]);
    json += "],\"inverseBindMatrices\":5}],";
    json += "\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"JOINTS_0\":1" + std::string(o.joints_without_weights ? "" : ",\"WEIGHTS_0\":2") + std::string(o.no_normals ? "" : ",\"NORMAL\":11") + "},\"indices\":3}]}],";
    // animations: logical joint j is node j.
    json += "\"animations\":[{\"name\":\"bend\",\"samplers\":[{\"input\":6,\"output\":7,\"interpolation\":\"LINEAR\"},{\"input\":8,\"output\":9,\"interpolation\":\"STEP\"}],"
            "\"channels\":[{\"sampler\":0,\"target\":{\"node\":1,\"path\":\"rotation\"}},{\"sampler\":1,\"target\":{\"node\":2,\"path\":\"translation\"}}]},"
            "{\"name\":\"slide\",\"samplers\":[{\"input\":6,\"output\":10,\"interpolation\":\"CUBICSPLINE\"}],\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,\"path\":\"translation\"}}]}],";
    const auto view = [&](std::size_t offset, std::size_t length) { return "{\"buffer\":0,\"byteOffset\":" + std::to_string(offset) + ",\"byteLength\":" + std::to_string(length) + "}"; };
    json += "\"bufferViews\":[" + view(0, joints_at) + "," + view(joints_at, weights_at - joints_at) + "," + view(weights_at, indices_at - weights_at) + "," +
            view(indices_at, ibm_at - indices_at) + "," + view(ibm_at, t01_at - ibm_at) + "," + view(t01_at, rot_at - t01_at) + "," + view(rot_at, t_step_at - rot_at) + "," +
            view(t_step_at, step_at - t_step_at) + "," + view(step_at, cubic_at - step_at) + "," + view(cubic_at, normals_at - cubic_at) + "," + view(normals_at, b.size() - normals_at) + "],";
    const std::size_t out_values = o.short_output ? 1 : 2;
    json += "\"accessors\":["
            "{\"bufferView\":0,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\"},"
            "{\"bufferView\":1,\"componentType\":5121,\"count\":6,\"type\":\"VEC4\"},"
            "{\"bufferView\":2,\"componentType\":5126,\"count\":6,\"type\":\"VEC4\"},"
            "{\"bufferView\":3,\"componentType\":5123,\"count\":12,\"type\":\"SCALAR\"},"
            "{\"bufferView\":3,\"componentType\":5123,\"count\":12,\"type\":\"SCALAR\"},"  // 4 unused (keeps the numbering below simple)
            "{\"bufferView\":4,\"componentType\":5126,\"count\":3,\"type\":\"MAT4\"},"
            "{\"bufferView\":5,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
            "{\"bufferView\":6,\"componentType\":5126,\"count\":" + std::to_string(out_values) + ",\"type\":\"VEC4\"},"
            "{\"bufferView\":7,\"componentType\":5126,\"count\":2,\"type\":\"SCALAR\"},"
            "{\"bufferView\":8,\"componentType\":5126,\"count\":2,\"type\":\"VEC3\"},"
            "{\"bufferView\":9,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\"},"
            "{\"bufferView\":10,\"componentType\":5126,\"count\":6,\"type\":\"VEC3\"}],";
    json += "\"buffers\":[{\"byteLength\":" + std::to_string(b.size()) + ",\"uri\":\"data:application/octet-stream;base64," + base64(b.bytes) + "\"}]}";
    return json;
}

GltfAsset load(const std::string& json, const GltfLimits& limits = {})
{
    const auto r = load_gltf({reinterpret_cast<const uint8_t*>(json.data()), json.size()}, {}, limits);
    if (!r.ok()) std::printf("  load error: %s\n", r.error.c_str());
    REQUIRE(r.ok());
    return *r.asset;
}

std::string load_error(const std::string& json, const GltfLimits& limits = {})
{
    const auto r = load_gltf({reinterpret_cast<const uint8_t*>(json.data()), json.size()}, {}, limits);
    return r.ok() ? std::string() : r.error;
}

/// glTF -> engine, for expected values written in glTF space.
Float3 engine(float x, float y, float z) { return {z, x, y}; }

bool near(Float3 a, Float3 b, float tol = 1e-5f) { return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol && std::fabs(a.z - b.z) <= tol; }

Float3 vertex(const std::vector<float>& p, std::size_t v) { return {p[v * 3], p[v * 3 + 1], p[v * 3 + 2]}; }

/// The skinned positions at time `t` of animation `clip`, for the first skin and first primitive.
std::vector<float> skinned_at(const GltfAsset& a, std::size_t clip, float t, bool loop = false)
{
    const Skin& skin = a.skins[0];
    Pose pose = rest_pose(skin);
    evaluate_clip(a.animations[clip], bind_clip(a.animations[clip], skin), t, loop, pose);
    return skin_positions(a.meshes[0].primitives[0], skin_matrices(skin, pose));
}

} // namespace

TEST_CASE("glTF skin: joints, hierarchy, rest transforms and inverse bind matrices arrive in engine space")
{
    const GltfAsset a = load(arm_gltf());
    REQUIRE(a.skins.size() == 1);
    const Skin& s = a.skins[0];
    CHECK(s.name == "armature");
    REQUIRE(s.joint_count() == 3);
    CHECK(s.joint_nodes == std::vector<int32_t>{0, 1, 2});
    CHECK(s.joint_names == std::vector<std::string>{"root", "mid", "tip"});  // the node names, in joint order, for mapping a skeleton by bone name
    CHECK(s.parent == std::vector<int32_t>{-1, 0, 1});
    CHECK(near(s.rest[1].position, engine(0, 1, 0)));  // glTF +Y 1 m up is engine +Z
    CHECK(near(s.rest[0].scale, {1, 1, 1}) && std::fabs(s.rest[0].rotation.w - 1.0f) < 1e-6f);
    // The inverse bind of the tip is a translation by -2 m in glTF y = engine z.
    CHECK(near(saige::render::transform_point(s.inverse_bind[2], {0, 0, 2}), {0, 0, 0}));
    CHECK(near(saige::render::transform_point(s.inverse_bind[1], {0, 0, 1}), {0, 0, 0}));
    // The instance is skinned and placed by identity: the mesh node's own translation (5 m) is irrelevant to a skinned mesh.
    REQUIRE(a.instances.size() == 1);
    CHECK(a.instances[0].skin == 0);
    CHECK(near(saige::render::transform_point(a.instances[0].world, {1, 2, 3}), {1, 2, 3}));
    // Per-vertex data: four influences, weights sum to one.
    const Primitive& p = a.meshes[0].primitives[0];
    REQUIRE(p.joints.size() == 24);
    REQUIRE(p.weights.size() == 24);
    for (std::size_t v = 0; v < 6; ++v) {
        float sum = 0.0f;
        for (std::size_t k = 0; k < 4; ++k) sum += p.weights[v * 4 + k];
        CHECK(std::fabs(sum - 1.0f) < 1e-6f);
    }
    CHECK(p.joints[3 * 4 + 1] == 1);  // vertex D: root and mid
    CHECK(std::fabs(p.weights[3 * 4] - 0.5f) < 1e-6f);
    // Bounds are the rest pose of the skinned mesh (not shifted by the node's translation).
    CHECK(near({a.bounds.min.x, a.bounds.min.y, a.bounds.min.z}, engine(0, 0, 0)));
    CHECK(near({a.bounds.max.x, a.bounds.max.y, a.bounds.max.z}, engine(0.5f, 2, 0)));
}

TEST_CASE("glTF skin: the rest pose skins every vertex to where it started")
{
    for (const bool reversed : {false, true}) {
        Options o;
        o.reversed = reversed;
        const GltfAsset a = load(arm_gltf(o));
        const Skin& s = a.skins[0];
        const std::vector<float> rest = skin_positions(a.meshes[0].primitives[0], skin_matrices(s, rest_pose(s)));
        const Primitive& p = a.meshes[0].primitives[0];
        for (std::size_t v = 0; v < 6; ++v) CHECK(near(vertex(rest, v), vertex(p.positions, v), 1e-6f));
    }
}

TEST_CASE("glTF animation: a clip's pose skins the vertices to analytically expected positions (LINEAR rotation, STEP translation)")
{
    const GltfAsset a = load(arm_gltf());
    REQUIRE(a.animations.size() == 2);
    CHECK(a.animations[0].name == "bend");
    CHECK(std::fabs(a.animations[0].duration - 1.0f) < 1e-6f);
    // At t = 0.25 the mid joint is rotated 22.5 degrees about glTF +Z (pivot (0, 1, 0)); the tip's STEP translation still holds (0, 1, 0).
    const float th = 22.5f * 3.14159265358979f / 180.0f;
    const auto rot = [&](float x, float y) { return engine(0.0f + (x * std::cos(th) - (y - 1.0f) * std::sin(th)), 1.0f + (x * std::sin(th) + (y - 1.0f) * std::cos(th)), 0.0f); };
    const std::vector<float> p = skinned_at(a, 0, 0.25f);
    CHECK(near(vertex(p, 0), engine(0, 0, 0)));         // A: root only: unmoved
    CHECK(near(vertex(p, 1), rot(0, 1)));               // B: mid only
    CHECK(near(vertex(p, 2), rot(0, 2)));               // C: tip: mid's rotation carries it; its own step translation (0, 1, 0) is its rest value
    // D: half root (stays at (0.5, 1)), half mid (rotated about the pivot): the average of the two skinned positions.
    CHECK(near(vertex(p, 3), (engine(0.5f, 1, 0) + rot(0.5f, 1)) * 0.5f));
    // At t = 0.75 the STEP track has switched: the tip sits 2 m up from the mid joint, so vertex C is carried 1 m farther along the bent arm.
    const float th2 = 67.5f * 3.14159265358979f / 180.0f;
    const std::vector<float> q = skinned_at(a, 0, 0.75f);
    // C: joint2 world = mid world * T(0, 2, 0) (rest was 1): the extra 1 m is carried along the rotated +Y of the mid joint.
    const Float3 expected_c = engine(-std::sin(th2) * 2.0f, 1.0f + std::cos(th2) * 2.0f, 0.0f);
    CHECK(near(vertex(q, 2), expected_c));
    // Looping wraps time over the clip's duration; clamping holds the end pose.
    CHECK(near(vertex(skinned_at(a, 0, 1.25f, true), 2), vertex(skinned_at(a, 0, 0.25f), 2)));
    CHECK(near(vertex(skinned_at(a, 0, 5.0f, false), 1), vertex(skinned_at(a, 0, 1.0f), 1)));
    CHECK(near(vertex(skinned_at(a, 0, -3.0f, false), 1), vertex(skinned_at(a, 0, 0.0f), 1)));
    // Non-finite time samples at 0.
    CHECK(near(vertex(skinned_at(a, 0, std::nanf(""), true), 2), vertex(skinned_at(a, 0, 0.0f), 2)));
}

TEST_CASE("glTF animation: the cubic spline of a translation follows the Hermite basis, tangents included")
{
    const GltfAsset a = load(arm_gltf());
    const Skin& s = a.skins[0];
    Pose pose = rest_pose(s);
    const ClipBinding binding = bind_clip(a.animations[1], s);
    // p(t) = h00 v0 + h10 dt b0 + h01 v1 + h11 dt a1, with v0 = 0, b0 = 3 (out-tangent of key 0), v1 = 2 and a1 = -1 (in-tangent of key 1), dt = 1.
    const auto hermite = [](float t) {
        const float t2 = t * t, t3 = t2 * t;
        return (t3 - 2 * t2 + t) * 3.0f + (-2 * t3 + 3 * t2) * 2.0f + (t3 - t2) * -1.0f;
    };
    for (const float t : {0.0f, 0.25f, 0.5f, 0.8f, 1.0f}) {
        evaluate_clip(a.animations[1], binding, t, false, pose);
        CHECK(near(pose.local[0].position, engine(hermite(t), 0, 0)));
    }
    evaluate_clip(a.animations[1], binding, 0.5f, false, pose);
    CHECK(std::fabs(pose.local[0].position.y - 1.5f) < 1e-6f);  // glTF x = engine y; the tangents matter: zero tangents would give 1.0
}

TEST_CASE("glTF skin: a joint with a non-trivial rest rotation converts it to engine space and still skins to the rest pose")
{
    Options o;
    o.tip_rest_rotation = true;
    const GltfAsset a = load(arm_gltf(o));
    const Skin& s = a.skins[0];
    // 90 degrees about glTF +Z is 90 degrees about engine +X (the axes permute with the vector).
    const float h = std::sqrt(0.5f);
    CHECK(std::fabs(s.rest[2].rotation.x - h) < 1e-6f && std::fabs(s.rest[2].rotation.y) < 1e-6f && std::fabs(s.rest[2].rotation.z) < 1e-6f && std::fabs(s.rest[2].rotation.w - h) < 1e-6f);
    const std::vector<float> rest = skin_positions(a.meshes[0].primitives[0], skin_matrices(s, rest_pose(s)));
    for (std::size_t v = 0; v < 6; ++v) CHECK(near(vertex(rest, v), vertex(a.meshes[0].primitives[0].positions, v), 1e-5f));
}

TEST_CASE("glTF animation: joint order in the file does not change the result, and parents need not precede children")
{
    Options reversed;
    reversed.reversed = true;
    const GltfAsset forward = load(arm_gltf());
    const GltfAsset backward = load(arm_gltf(reversed));
    CHECK(backward.skins[0].joint_nodes == std::vector<int32_t>{2, 1, 0});
    CHECK(backward.skins[0].joint_names == std::vector<std::string>{"tip", "mid", "root"});
    CHECK(backward.skins[0].parent == std::vector<int32_t>{1, 2, -1});  // tip -> mid (slot 1) -> root (slot 2), the root has none
    for (const std::size_t clip : {std::size_t{0}, std::size_t{1}}) {
        for (const float t : {0.0f, 0.2f, 0.5f, 0.75f, 1.0f}) {
            const std::vector<float> a = skinned_at(forward, clip, t), b = skinned_at(backward, clip, t);
            for (std::size_t v = 0; v < 6; ++v) CHECK(near(vertex(a, v), vertex(b, v), 1e-5f));
        }
    }
}

TEST_CASE("glTF skin: unnormalized weights are renormalized, zero weights bind the first joint, and extra input is rejected precisely")
{
    Options scaled;
    scaled.weight_scale = 3.0f;
    const GltfAsset a = load(arm_gltf(scaled));
    for (std::size_t v = 0; v < 6; ++v) {
        float sum = 0.0f;
        for (std::size_t k = 0; k < 4; ++k) sum += a.meshes[0].primitives[0].weights[v * 4 + k];
        CHECK(std::fabs(sum - 1.0f) < 1e-6f);
    }
    Options zero;
    zero.weight_scale = 0.0f;
    const GltfAsset z = load(arm_gltf(zero));
    CHECK(z.meshes[0].primitives[0].weights[0] == 1.0f && z.meshes[0].primitives[0].weights[1] == 0.0f);

    const auto rejects = [&](const Options& o, const char* what, const GltfLimits& limits = {}) {
        const std::string e = load_error(arm_gltf(o), limits);
        const bool ok = e.find(what) != std::string::npos;
        if (!ok) std::printf("  expected an error mentioning '%s', got '%s'\n", what, e.c_str());
        return ok;
    };
    Options no_weights;
    no_weights.joints_without_weights = true;
    CHECK(rejects(no_weights, "must come together"));
    Options dup;
    dup.duplicate_joint = true;
    CHECK(rejects(dup, "twice"));
    Options negative;
    negative.negative_weight = -0.5f;
    CHECK(rejects(negative, "negative weight"));
    Options bad_joint;
    bad_joint.bad_joint_index = 3;
    CHECK(rejects(bad_joint, "names joint 3"));
    Options times;
    times.decreasing_times = true;
    CHECK(rejects(times, "strictly increasing"));
    Options shortened;
    shortened.short_output = true;
    CHECK(rejects(shortened, "values for 2 keys"));
    Options bad_skin;
    bad_skin.bad_skin_index = 7;
    CHECK(rejects(bad_skin, "invalid skin index"));
    Options gap;
    gap.extra_node_between = 1;
    CHECK(rejects(gap, "non-joint node between"));
    GltfLimits tight;
    tight.max_joints_per_skin = 2;
    CHECK(rejects({}, "joints is over the limit", tight));
}

TEST_CASE("glTF animation: the evaluation API rejects mismatched inputs and handles degenerate clips")
{
    const GltfAsset a = load(arm_gltf());
    const Skin& s = a.skins[0];
    Pose pose = rest_pose(s);
    // A binding from another clip is refused; a pose with the wrong joint count is refused.
    const ClipBinding binding = bind_clip(a.animations[0], s);
    bool threw = false;
    try {
        AnimationClip other = a.animations[0];
        other.channels.pop_back();
        evaluate_clip(other, binding, 0.0f, false, pose);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    threw = false;
    try {
        Pose small;
        small.local.resize(2);
        (void)skin_matrices(s, small);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);
    // A channel that targets a node that is not a joint of the skin is skipped, not an error.
    AnimationClip foreign = a.animations[0];
    foreign.channels[0].node = 3;
    const ClipBinding fb = bind_clip(foreign, s);
    CHECK(fb.channel_joint[0] == -1);
    Pose p2 = rest_pose(s);
    evaluate_clip(foreign, fb, 0.5f, false, p2);
    CHECK(near({p2.local[1].rotation.x, p2.local[1].rotation.y, p2.local[1].rotation.z}, {0, 0, 0}));
    // sample_track on an empty or single-key track.
    AnimationSampler one;
    one.times = {0.5f};
    one.values = {1, 2, 3};
    one.components = 3;
    const Float4 v = sample_track(one, 10.0f);
    CHECK(v.x == 1 && v.y == 2 && v.z == 3);
    CHECK(sample_track(AnimationSampler{}, 1.0f).w == 0.0f);
    // A file without skins or animations still loads as before.
    GltfAsset plain;
    CHECK(plain.skins.empty() && plain.animations.empty());
}

TEST_CASE("glTF skin: a skinned primitive without normals is de-indexed with its joints and weights kept in step")
{
    Options o;
    o.no_normals = true;
    const GltfAsset a = load(arm_gltf(o));
    const Primitive& p = a.meshes[0].primitives[0];
    REQUIRE(p.normals_generated);
    REQUIRE(p.vertex_count() == 12);  // four triangles, three corners each
    REQUIRE(p.joints.size() == 48);
    REQUIRE(p.weights.size() == 48);
    const Skin& s = a.skins[0];
    const std::vector<float> rest = skin_positions(p, skin_matrices(s, rest_pose(s)));
    for (std::size_t v = 0; v < 12; ++v) CHECK(near(vertex(rest, v), vertex(p.positions, v), 1e-6f));
    // A bent pose moves the de-indexed vertices exactly as it moves the shared ones: compare with the indexed asset corner by corner.
    const GltfAsset indexed = load(arm_gltf());
    const std::vector<float> bent_flat = skinned_at(a, 0, 0.4f), bent = skinned_at(indexed, 0, 0.4f);
    const std::vector<uint32_t>& corners = indexed.meshes[0].primitives[0].indices;
    for (std::size_t c = 0; c < corners.size(); ++c) CHECK(near(vertex(bent_flat, c), vertex(bent, corners[c]), 1e-5f));
}
