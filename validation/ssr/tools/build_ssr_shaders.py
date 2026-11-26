#!/usr/bin/env python3
"""Compile Gemini's SSR shaders (FP32, FP16, sharpen) and the mutant shaders with glslangValidator, validate every binary with spirv-val.

  build_ssr_shaders.py --glslang G --spirv-val V --src DIR --out DIR [--expect-mutants a,b,c]

A mutant is the unmodified shader with exactly one textual replacement; each replacement must match exactly once or the tool fails (a
mutant that silently equals the original would be a test that cannot fail)."""
import argparse, os, subprocess, sys

# name: (shader file, old text, new text)
MUTANTS = {
    "exposure_ignored":   ("ssr_reconstruct.comp", ") * pc.exposure_ratio;", ");"),
    "jitter_sign":        ("ssr_reconstruct.comp", "- 0.5 - pc.jitter_px;", "- 0.5 + pc.jitter_px;"),
    "cut_ignored":        ("ssr_reconstruct.comp", "if (camera_cut || oob) {", "if (oob) {"),
    "mv_sign":            ("ssr_reconstruct.comp", "vec2 hist_px = vec2(ox) + mv_out;", "vec2 hist_px = vec2(ox) - mv_out;"),
    "depth_test_flipped": ("ssr_reconstruct.comp", "if (reversed_z ? (d > best_depth) : (d < best_depth)) {", "if (reversed_z ? (d < best_depth) : (d > best_depth)) {"),
    "no_disocclusion":    ("ssr_reconstruct.comp", "> pc.disocclusion_threshold;", "> 1.0e9;"),
    "no_history_clamp":   ("ssr_reconstruct.comp", "ssr_vec3 clipped_yc = clamp(hist_yc, ssr_vec3(c_min), ssr_vec3(c_max));", "ssr_vec3 clipped_yc = hist_yc;"),
    "alpha_raised":       ("ssr_reconstruct.comp", "float base_alpha = 0.10 +", "float base_alpha = 0.40 +"),
    "transparency_ignored": ("ssr_reconstruct.comp", "reactive = max(reactive, transp * 0.7);", "reactive = reactive;"),
    "confidence_constant": ("ssr_reconstruct.comp", "imageStore(u_history_conf_out, ox, vec4(new_conf, 0.0, 0.0, 0.0));", "imageStore(u_history_conf_out, ox, vec4(1.0, 0.0, 0.0, 0.0));"),
    "depth_history_zero": ("ssr_reconstruct.comp", "imageStore(u_history_depth_out, ox, vec4(center_depth, 0.0, 0.0, 0.0));", "imageStore(u_history_depth_out, ox, vec4(0.0, 0.0, 0.0, 0.0));"),
    "decompress_floor":   ("ssr_reconstruct.comp", "max(1e-3, 1.0 - luminance(cf))", "max(1e-1, 1.0 - luminance(cf))"),
    "sharpen_off":        ("ssr_sharpen.comp", "clamp(pc.sharpness, 0.0, 1.0) * 0.25;", "clamp(pc.sharpness, 0.0, 1.0) * 0.0;"),
    "sharpen_north_south": ("ssr_sharpen.comp", "ox + ivec2(0, -1)", "ox + ivec2(0, 1)"),
}

def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("FAILED: %s\n%s%s" % (" ".join(cmd), r.stdout, r.stderr))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glslang", required=True); ap.add_argument("--spirv-val", required=True)
    ap.add_argument("--src", required=True); ap.add_argument("--out", required=True); ap.add_argument("--expect-mutants")
    a = ap.parse_args()
    if a.expect_mutants is not None and sorted(a.expect_mutants.split(",")) != sorted(MUTANTS):
        sys.exit("mutant list mismatch between CMake and build_ssr_shaders.py: %s vs %s" % (sorted(a.expect_mutants.split(",")), sorted(MUTANTS)))
    os.makedirs(a.out, exist_ok=True)
    def compile_(src, dst, defines=()):
        run([a.glslang, "-V", "--target-env", "vulkan1.4", src, "-o", dst] + ["-D" + d for d in defines])
        run([a.spirv_val, "--target-env", "vulkan1.4", dst])
    rc, sh = os.path.join(a.src, "ssr_reconstruct.comp"), os.path.join(a.src, "ssr_sharpen.comp")
    compile_(rc, os.path.join(a.out, "ssr_reconstruct.spv"))
    compile_(rc, os.path.join(a.out, "ssr_reconstruct_fp16.spv"), ["SSR_USE_FP16=1"])
    compile_(sh, os.path.join(a.out, "ssr_sharpen.spv"))
    for name, (fn, old, new) in MUTANTS.items():
        text = open(os.path.join(a.src, fn)).read()
        if text.count(old) != 1:
            sys.exit("mutant %s: pattern matched %d times (want 1): %r" % (name, text.count(old), old))
        mdir = os.path.join(a.out, "mutants"); os.makedirs(mdir, exist_ok=True)
        # the glslang front end infers the stage from the extension: keep .comp
        msrc = os.path.join(mdir, name + ".comp")
        open(msrc, "w").write(text.replace(old, new))
        compile_(msrc, os.path.join(mdir, name + ".spv"))
    open(os.path.join(a.out, "shaders.stamp"), "w").write("ok\n")
    print("compiled and validated: reconstruct fp32/fp16, sharpen, %d mutants" % len(MUTANTS))

main()
