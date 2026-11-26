#!/usr/bin/env python3
"""Diagram of chair orientation from `alive_validate --seats SEED` dumps (before = the delivered generator, after = the fixed one).

  plot_seating.py BEFORE.txt AFTER.txt OUT.png [--building ID]

Each chair is an arrow in its front direction (cos yaw, sin yaw), the front of the chair model (tools/check_prop_orientation.py); tables are circles. A chair is red when it faces more than
30 degrees away from the nearest table within 1.6 m (the SEAT_FACING rule), green otherwise. Nothing is retouched: positions and yaws are the generator's."""
import math, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

def load(path):
    rows = []
    for ln in open(path):
        t = ln.split()
        if len(t) == 7 and t[0] == "S": rows.append((int(t[1]), t[2], t[3], float(t[4]), float(t[5]), float(t[6])))
    return rows

def judge(rows):
    out = []
    for (b, where, typ, x, y, yaw) in rows:
        if typ not in ("chair", "cafe_chair"): continue
        best, bd = None, 1e9
        for (b2, w2, t2, x2, y2, _) in rows:
            if b2 == b and w2 == where and t2 in ("table_round", "table_square", "cafe_table"):
                d = math.hypot(x2 - x, y2 - y)
                if d < bd: best, bd = (x2, y2), d
        if best is None or bd > 1.6: out.append((x, y, yaw, None)); continue
        c = max(-1.0, min(1.0, ((best[0] - x) * math.cos(yaw) + (best[1] - y) * math.sin(yaw)) / bd))
        out.append((x, y, yaw, math.degrees(math.acos(c))))
    return out

def draw(ax, rows, building, title):
    sel = [r for r in rows if r[0] == building and r[1] == "interior"]
    for (_, _, typ, x, y, _) in sel:
        if typ != "chair" and typ != "cafe_chair": ax.add_patch(plt.Circle((x, y), 0.45, color="#b08d57", alpha=0.8))
    bad = 0
    for (x, y, yaw, deg) in judge(sel):
        ok = deg is None or deg <= 30.0
        bad += 0 if ok else 1
        ax.arrow(x, y, 0.55 * math.cos(yaw), 0.55 * math.sin(yaw), head_width=0.18, length_includes_head=True, color="#2a9d3f" if ok else "#d62828")
        ax.plot([x], [y], "s", color="#333", ms=5)
    ax.set_aspect("equal"); ax.set_title("%s: %d of %d chairs more than 30 deg off" % (title, bad, len([1 for r in sel if r[2] == "chair"])), fontsize=9)
    ax.grid(alpha=0.25)

def main():
    before, after, out = load(sys.argv[1]), load(sys.argv[2]), sys.argv[3]
    ids = sorted({r[0] for r in before if r[1] == "interior" and r[2] == "chair"})
    if "--building" in sys.argv: ids = [int(sys.argv[sys.argv.index("--building") + 1])]
    ids = ids[:3]
    fig, axes = plt.subplots(len(ids), 2, figsize=(10, 3.6 * len(ids)), squeeze=False)
    for i, bid in enumerate(ids):
        draw(axes[i][0], before, bid, "delivered generator, building %d" % bid)
        draw(axes[i][1], after, bid, "fixed generator, building %d" % bid)
    fig.suptitle("Interior chairs: arrow = the chair's front; red = faces away from its table (world plan view, metres)", fontsize=10)
    fig.tight_layout(); fig.savefig(out, dpi=110)

main()
