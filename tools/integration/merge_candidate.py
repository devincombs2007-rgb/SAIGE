#!/usr/bin/env python3
"""Builds an integration candidate: merges the open pull requests' heads, in the dependency order of docs/engineering/integration-plan.md, into the current checkout (a throwaway worktree of `main`), and prints one
line per PR: merged cleanly, merged with an additive resolution (a conflict in which both sides only added lines at the same place, or both appended names to one list line: kept both), or needs a human (a
conflict where the two sides changed the same lines). Never touches a remote and never resolves anything but those two additive shapes. Heads must be fetched first:

    git worktree add --detach /tmp/ic origin/main && cd /tmp/ic && git config merge.conflictStyle diff3
    for n in <PR numbers>; do git fetch origin refs/pull/$n/head:refs/remotes/pr/$n; done
    python3 /path/to/tools/integration/merge_candidate.py

The result is evidence for the owner's landing decision (`docs/engineering/integration-candidate.md`), not a branch to push or merge.
"""
import os, subprocess, re, sys
def sh(*a, **k): return subprocess.run(a, capture_output=True, text=True, **k)
# STOP_AT_HUMAN=1 leaves the first merge that needs a person in progress (conflict markers in the working tree) and stops, so it can be resolved by hand and the run continued with SKIP_TO=<next PR>.
# ORDER="76 79" runs only those PRs, in that order; SKIP_TO=N starts at PR N of the default order.
stop_at_human = os.environ.get("STOP_AT_HUMAN") == "1"
order = [67,44,45,64,42,60,63,86,69,70,65,73,74,80,88,71,75,77,78,81,82,83,92,95,98,76,79,89,85,87,90,91,96,84,94,99,93,97]
log = []
if os.environ.get("ORDER"): order = [int(x) for x in os.environ["ORDER"].split()]
elif os.environ.get("SKIP_TO"): order = order[order.index(int(os.environ["SKIP_TO"])):]
for n in order:
    ref = f"refs/remotes/pr/{n}"
    head = sh("git","rev-parse","--short",ref).stdout.strip()
    if not head: continue
    r = sh("git","merge","--no-edit","-q",ref)
    if r.returncode == 0:
        log.append((n, head, "clean", [])); continue
    files = sh("git","diff","--name-only","--diff-filter=U").stdout.split()
    auto, hard = [], []
    for f in files:
        txt = open(f, errors="surrogateescape").read()
        pat = re.compile(r"<<<<<<< [^\n]*\n(.*?)\|\|\|\|\|\|\| [^\n]*\n(.*?)=======\n(.*?)>>>>>>> [^\n]*\n", re.S)
        ok = True
        def rep(m):
            global ok
            ours, base, theirs = m.group(1), m.group(2), m.group(3)
            if base.strip() == "":
                return ours + theirs  # both sides only added lines at the same place
            bl, ol, tl = base.rstrip("\n").split("\n"), ours.rstrip("\n").split("\n"), theirs.rstrip("\n").split("\n")
            if len(bl) == 1 and len(ol) >= 1 and len(tl) >= 1:
                # one list line (sources of a target, suites of a test loop) that both sides appended names to: keep ours and add theirs' new names; extra lines of theirs (comments) go above
                strip = lambda line: line.rstrip()[:-1] if line.rstrip().endswith(")") else line.rstrip()
                closing = ")" if ol[-1].rstrip().endswith(")") else ""
                btok, otok, ttok = strip(bl[0]).split(), strip(ol[-1]).split(), strip(tl[-1]).split()
                if all(t in otok for t in btok) and all(t in ttok for t in btok):
                    add = [t for t in ttok if t not in otok]
                    indent = ol[-1][: len(ol[-1]) - len(ol[-1].lstrip())]
                    merged = indent + " ".join(otok + add) + closing
                    extras = [l for l in ol[:-1]] + [l for l in tl[:-1] if l not in ol[:-1]]
                    extra = "\n".join(extras) + ("\n" if extras else "")
                    return extra + merged + "\n"
            ok = False
            return m.group(0)
        out = pat.sub(rep, txt)
        if ok and "<<<<<<<" not in out:
            open(f, "w", errors="surrogateescape").write(out); sh("git","add",f); auto.append(f)
        else:
            hard.append(f)
    if hard:
        log.append((n, head, "needs-human", hard))
        if stop_at_human:
            for n2, h2, s2, f2 in log: print(f"#{n2} {h2} {s2} {' '.join(f2)}")
            print(f"stopped: #{n} is left merging in the working tree; conflicted files: {' '.join(hard)}"); sys.exit(0)
        sh("git","merge","--abort")
    else:
        c = sh("git","commit","--no-edit","-q")
        log.append((n, head, "additive-resolution", auto))
for n, h, s, f in log: print(f"#{n} {h} {s} {' '.join(f)}")
