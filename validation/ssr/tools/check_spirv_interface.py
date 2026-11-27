#!/usr/bin/env python3
"""Check the compiled SPIR-V interface against what the Vulkan harness creates (descriptor set layout and push-constant ranges).

The harness builds: reconstruct = set 0, bindings 0-7 COMBINED_IMAGE_SAMPLER, 8-10 STORAGE_IMAGE, push constants 36 bytes
(uvec2, uvec2, vec2, float, float, uint at offsets 0, 8, 16, 24, 28, 32); sharpen = set 0, binding 0 sampler, 1 storage image, push constants
12 bytes (uvec2 at 0, float at 8). Any drift between the shaders and the host structures is a failure here, before a device is involved."""
import re, subprocess, sys

def dis(path, spirv_dis):
    return subprocess.run([spirv_dis, path], capture_output=True, text=True, check=True).stdout

def interface(text):
    names = dict(re.findall(r"OpName (%\S+) \"(\w+)\"", text))
    binds, sets = {}, {}
    for m in re.finditer(r"OpDecorate (%\S+) Binding (\d+)", text): binds[m.group(1)] = int(m.group(2))
    for m in re.finditer(r"OpDecorate (%\S+) DescriptorSet (\d+)", text): sets[m.group(1)] = int(m.group(2))
    kinds = {}
    # a combined image sampler is a variable of a pointer to a sampled image type; a storage image is an Image type with Sampled=2
    types = {}
    for m in re.finditer(r"(%\S+) = OpTypeImage %\S+ 2D \d \d \d (\d) (\S+)", text): types[m.group(1)] = ("storage" if m.group(2) == "2" else "sampled_image", m.group(3))
    sampled = {m.group(1) for m in re.finditer(r"(%\S+) = OpTypeSampledImage", text)}
    ptrs = {m.group(1): m.group(2) for m in re.finditer(r"(%\S+) = OpTypePointer UniformConstant (%\S+)", text)}
    for m in re.finditer(r"(%\S+) = OpVariable (%\S+) UniformConstant", text):
        v, p = m.group(1), m.group(2)
        t = ptrs.get(p)
        if t in sampled: kinds[v] = "combined_image_sampler"
        elif t in types and types[t][0] == "storage": kinds[v] = "storage_image:" + types[t][1]
        else: kinds[v] = "other"
    out = {}
    for v, b in binds.items(): out[(sets.get(v, -1), b)] = (names.get(v, v), kinds.get(v, "?"))
    offsets = {}
    for m in re.finditer(r"OpMemberDecorate (%\S+) (\d+) Offset (\d+)", text): offsets.setdefault(m.group(1), {})[int(m.group(2))] = int(m.group(3))
    pc_block = None
    for m in re.finditer(r"(%\S+) = OpVariable (%\S+) PushConstant", text):
        ptr = m.group(2); mm = re.search(re.escape(ptr) + r" = OpTypePointer PushConstant (%\S+)", text); pc_block = mm.group(1)
    return out, offsets.get(pc_block, {}), pc_block

def check(path, spirv_dis, want_binds, want_offsets, label):
    binds, offs, _ = interface(dis(path, spirv_dis))
    ok = True
    if {k: v[1] for k, v in binds.items()} != want_binds:
        print("%s: descriptor interface differs:\n  shader %s\n  host   %s" % (label, {k: v[1] for k, v in sorted(binds.items())}, want_binds)); ok = False
    if [offs[i] for i in sorted(offs)] != want_offsets:
        print("%s: push-constant offsets differ: shader %s host %s" % (label, [offs[i] for i in sorted(offs)], want_offsets)); ok = False
    print("%s: %s (%d bindings, push offsets %s)" % (label, "OK" if ok else "MISMATCH", len(binds), [offs[i] for i in sorted(offs)]))
    return ok

def main():
    spirv_dis, d = sys.argv[1], sys.argv[2]
    recon = {(0, i): "combined_image_sampler" for i in range(8)}
    recon.update({(0, 8): "storage_image:Rgba16f", (0, 9): "storage_image:R32f", (0, 10): "storage_image:R16f"})
    sharp = {(0, 0): "combined_image_sampler", (0, 1): "storage_image:Rgba16f"}
    ok = check(d + "/ssr_reconstruct.spv", spirv_dis, recon, [0, 8, 16, 24, 28, 32], "reconstruct fp32")
    ok &= check(d + "/ssr_reconstruct_fp16.spv", spirv_dis, recon, [0, 8, 16, 24, 28, 32], "reconstruct fp16")
    ok &= check(d + "/ssr_sharpen.spv", spirv_dis, sharp, [0, 8], "sharpen")
    sys.exit(0 if ok else 1)

main()
