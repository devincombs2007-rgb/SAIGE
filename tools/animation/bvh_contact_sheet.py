#!/usr/bin/env python3
"""A contact sheet of a BVH clip: eight poses across its length as a top view (blue = left foot, red = right foot, green = head) and a front view, for reviewing a clip by eye before it is used. Needs numpy and matplotlib.

    bvh_contact_sheet.py CLIP.bvh OUT.png

Found by using it: CMU files open with a T-pose frame, so a root-yaw change measured from the first frame is a 90 degree artifact (a "turn in place" that is the T-pose turning into the first real pose). Measure from the third frame on.
"""
import sys, re, numpy as np
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt

def parse(path):
    toks = open(path).read().split("\n")
    joints = []; stack = []; i = 0
    chan = []
    while i < len(toks):
        l = toks[i].strip(); i += 1
        if l.startswith(("ROOT", "JOINT")):
            name = l.split()[1]; joints.append({"name": name, "parent": stack[-1] if stack else -1, "off": None, "chan": []}); stack.append(len(joints) - 1)
        elif l.startswith("End Site"):
            joints.append({"name": "end", "parent": stack[-1], "off": None, "chan": []}); stack.append(len(joints) - 1)
        elif l.startswith("OFFSET"):
            joints[stack[-1]]["off"] = np.array([float(x) for x in l.split()[1:4]])
        elif l.startswith("CHANNELS"):
            p = l.split(); joints[stack[-1]]["chan"] = p[2:]
        elif l == "}":
            stack.pop()
        elif l.startswith("MOTION"):
            break
    n = int(toks[i].split()[1]); ft = float(toks[i + 1].split()[2]); i += 2
    data = np.array([[float(x) for x in toks[i + k].split()] for k in range(n)])
    return joints, data, ft

def rot(axis, a):
    c, s = np.cos(a), np.sin(a)
    if axis == "X": return np.array([[1,0,0],[0,c,-s],[0,s,c]])
    if axis == "Y": return np.array([[c,0,s],[0,1,0],[-s,0,c]])
    return np.array([[c,-s,0],[s,c,0],[0,0,1]])

def fk(joints, frame):
    pos = [None]*len(joints); R = [None]*len(joints); k = 0
    for j, jt in enumerate(joints):
        t = np.zeros(3); r = np.eye(3)
        for ch in jt["chan"]:
            v = frame[k]; k += 1
            if ch.endswith("position"): t["XYZ".index(ch[0])] = v
            else: r = r @ rot(ch[0], np.radians(v))
        if jt["parent"] < 0:
            pos[j] = jt["off"] + t; R[j] = r
        else:
            p = jt["parent"]; pos[j] = pos[p] + R[p] @ (jt["off"] + t); R[j] = R[p] @ r
    return np.array(pos)

path, out = sys.argv[1], sys.argv[2]
joints, data, ft = parse(path)
n = len(data); picks = np.linspace(0, n - 1, 8).astype(int)
fig, axes = plt.subplots(2, 8, figsize=(24, 7))
allp = [fk(joints, data[i]) for i in picks]
# BVH up is Y: plot top view (x, z) and front view (x, y)
xs = np.concatenate([p[:, 0] for p in allp]); zs = np.concatenate([p[:, 2] for p in allp])
cx, cz = (xs.min()+xs.max())/2, (zs.min()+zs.max())/2; half = max(xs.max()-xs.min(), zs.max()-zs.min())/2 + 5
for c, (i, p) in enumerate(zip(picks, allp)):
    for r, (a, b) in enumerate(((0, 2), (0, 1))):
        ax = axes[r][c]
        for j, jt in enumerate(joints):
            if jt["parent"] >= 0:
                q = p[jt["parent"]]; ax.plot([q[a], p[j][a]], [q[b], p[j][b]], "k-", lw=1.2)
        names = {jt["name"]: j for j, jt in enumerate(joints)}
        for nm, col in (("LeftFoot", "tab:blue"), ("RightFoot", "tab:red"), ("LeftToeBase", "tab:blue"), ("RightToeBase", "tab:red"), ("Head", "tab:green")):
            if nm in names: ax.plot(p[names[nm]][a], p[names[nm]][b], "o", color=col, ms=4)
        if r == 0: ax.set_xlim(cx-half, cx+half); ax.set_ylim(cz+half, cz-half); ax.set_title(f"t={i*ft:.2f}s", fontsize=9)
        else: ax.set_xlim(cx-half, cx+half); ax.set_ylim(0, 2*half)
        ax.set_aspect("equal"); ax.set_xticks([]); ax.set_yticks([])
plt.suptitle(f"{path.split('/')[-1]}: top view (top row, blue = left foot, red = right, green = head) and front view (bottom)")
plt.tight_layout(); plt.savefig(out, dpi=60)
