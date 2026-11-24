#!/usr/bin/env python3
"""Validates a skinned glTF/GLB for SAIGE's GPU skinning and animation retargeting: joint count against the 256-joint palette, hierarchy sanity, rest-pose scale, the bone naming scheme
(Mixamo, CesiumMan-style, Rigify/Blender, generic), a mapping onto a canonical humanoid skeleton, and what each animation clip drives.

  skeleton_validate.py MODEL.glb [--json]
"""
import json, re, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gltf_nodes import read_gltf_json  # noqa: E402

CANONICAL = {  # canonical bone -> regexes over lower-cased names without namespace prefixes
    "hips": r"hips|pelvis|torso_joint_1|root", "spine": r"^spine$|spine1|torso_joint_2", "chest": r"spine2|chest|torso_joint_3", "neck": r"neck", "head": r"^head$|head$|neck_joint_2",
    "l_upper_arm": r"leftarm$|upperarm_?l|arm_joint_l$|arm\.l|l_upperarm|arm_joint_l__4_", "l_forearm": r"leftforearm|forearm_?l|lowerarm_?l|arm_joint_l__3_", "l_hand": r"lefthand$|hand_?l|arm_joint_l__2_",
    "r_upper_arm": r"rightarm$|upperarm_?r|arm_joint_r$|arm\.r|r_upperarm", "r_forearm": r"rightforearm|forearm_?r|lowerarm_?r|arm_joint_r__2_", "r_hand": r"righthand$|hand_?r|arm_joint_r__3_",
    "l_thigh": r"leftupleg|thigh_?l|upleg_?l|leg_joint_l_1", "l_shin": r"leftleg$|calf_?l|shin_?l|leg_joint_l_2", "l_foot": r"leftfoot|foot_?l|leg_joint_l_3", "l_toe": r"lefttoebase|toe_?l|leg_joint_l_5",
    "r_thigh": r"rightupleg|thigh_?r|upleg_?r|leg_joint_r_1", "r_shin": r"rightleg$|calf_?r|shin_?r|leg_joint_r_2", "r_foot": r"rightfoot|foot_?r|leg_joint_r_3", "r_toe": r"righttoebase|toe_?r|leg_joint_r_5",
}
REQUIRED = ["hips", "head", "l_thigh", "r_thigh", "l_shin", "r_shin", "l_foot", "r_foot"]


def scheme(names):
    low = " ".join(names).lower()
    if "mixamorig" in low:
        return "mixamo"
    if "def-" in low or "org-" in low or "mch-" in low:
        return "rigify"
    if "skeleton_" in low or "leg_joint" in low:
        return "cesium"
    if "bip" in low:
        return "biped"
    return "generic"


def main():
    path = sys.argv[1]
    js, _ = read_gltf_json(path)
    nodes = js.get("nodes", [])
    res = {"file": Path(path).name, "skins": [], "problems": [], "animations": []}
    for si, skin in enumerate(js.get("skins", [])):
        joints = skin["joints"]
        names = [re.sub(r"^.*:", "", nodes[j].get("name", f"node{j}")) for j in joints]
        info = {"index": si, "joints": len(joints), "scheme": scheme(names), "root": nodes[skin.get("skeleton", joints[0])].get("name")}
        if len(joints) > 256:
            res["problems"].append(f"skin {si} has {len(joints)} joints; the palette holds 256")
        scales = []
        for j in joints:
            s = nodes[j].get("scale")
            if s:
                scales.append(max(abs(1 - v) for v in s))
        if scales and max(scales) > 0.05:
            res["problems"].append(f"skin {si}: joint rest scales differ from 1 by up to {max(scales):.2f} (bake the scale before retargeting)")
        mapping = {}
        for canon, rx in CANONICAL.items():
            for n in names:
                if re.search(rx, n.lower()):
                    mapping[canon] = n
                    break
        info["canonical_mapping"] = mapping
        missing = [r for r in REQUIRED if r not in mapping]
        info["humanoid"] = not missing
        info["missing_required"] = missing
        res["skins"].append(info)
        if missing:
            res["problems"].append(f"skin {si} does not map to a humanoid (missing {', '.join(missing)})")
    if len(js.get("skins", [])) > 1:
        res["problems"].append("more than one skin: SAIGE stages one skin per model; split the model")
    skin_nodes = {j for s in js.get("skins", []) for j in s["joints"]}
    for a in js.get("animations", []):
        driven = {c["target"]["node"] for c in a["channels"] if "node" in c["target"]}
        paths = {}
        for c in a["channels"]:
            paths[c["target"]["path"]] = paths.get(c["target"]["path"], 0) + 1
        res["animations"].append({"name": a.get("name"), "channels": len(a["channels"]), "joints_driven": len(driven & skin_nodes), "non_joint_nodes_driven": len(driven - skin_nodes), "paths": paths})
        if paths.get("weights"):
            res["problems"].append(f"animation {a.get('name')} uses morph target weights (the engine does not support them)")
    res["ok"] = not res["problems"] and bool(res["skins"])
    print(json.dumps(res, indent=1))


main()
