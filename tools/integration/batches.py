#!/usr/bin/env python3
"""Records and replays the review batches of docs/engineering/integration-batches.md without pushing a single branch.

A batch branch is a chain of steps over `main`: fast-forward to a pull request's head, merge a pull request's head (with the hand resolution of its conflicts), or apply a patch (a commit that
had to be ported by hand, or a fix that only the build revealed). Every step is stored as data: the pull request and its head SHA, and for a merge the *resolution patch*: the difference
between what `git merge-tree` produces on its own (with conflict markers, `merge.conflictStyle=diff3`) and the tree that was validated. Replaying needs only the repository, the pull request
heads (`refs/pull/N/head`) and the patches here; the replay is verified against the tree SHA recorded for every step, so a different result is an error, not a surprise.

    python3 tools/integration/batches.py rebuild [--only BATCH] [--fetch]   # recreates local branches batch/<name> in the current repository (no remote is touched)
    python3 tools/integration/batches.py verify                             # rebuilds in a temporary object set and compares every recorded tree
    python3 tools/integration/batches.py record SRC_REF MAIN_SHA            # (maintainers) records the steps of an existing first-parent chain; used to produce the manifest

Nothing here merges into `main` or pushes: the branches are local until the owner decides.
"""
import json, os, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
MANIFEST = HERE / "batches" / "manifest.json"
PATCHES = HERE / "batches" / "patches"
ENV = {**os.environ, "GIT_AUTHOR_NAME": "SAIGE integration", "GIT_AUTHOR_EMAIL": "integration@saige.invalid", "GIT_COMMITTER_NAME": "SAIGE integration",
       "GIT_COMMITTER_EMAIL": "integration@saige.invalid", "GIT_AUTHOR_DATE": "2026-11-25T00:00:00Z", "GIT_COMMITTER_DATE": "2026-11-25T00:00:00Z"}


def git(*args, input=None, env=None, check=True):
    r = subprocess.run(["git", "-c", "merge.conflictStyle=diff3", *args], input=input, capture_output=True, text=False, env=env or ENV)
    if check and r.returncode != 0:
        raise SystemExit(f"git {' '.join(args)} failed:\n{r.stderr.decode(errors='replace')}")
    return r


def out(*args, **k):
    return git(*args, **k).stdout.decode().strip()


def merge_tree(a, b):
    """The tree `git merge-tree` writes for merging b into a, with conflicted files holding diff3 markers whose labels are normalised (the labels are commit names, which differ between
    the original merge and a replay; the marker *text* is what a resolution patch depends on). Returns (tree, conflicted)."""
    r = git("merge-tree", "--write-tree", "--name-only", "--no-messages", a, b, check=False)
    if r.returncode not in (0, 1):
        raise SystemExit(f"merge-tree {a} {b}: {r.stderr.decode()}")
    lines = r.stdout.decode().split("\n")
    tree, conflicted = lines[0].strip(), []
    for l in lines[1:]:
        if not l.strip():
            break
        conflicted.append(l.strip())
    if not conflicted:
        return tree, False
    with tempfile.TemporaryDirectory() as d:
        env = {**ENV, "GIT_INDEX_FILE": str(Path(d) / "index")}
        git("read-tree", tree, env=env)
        for path in sorted(set(conflicted)):
            blob = git("cat-file", "-p", f"{tree}:{path}", check=False)
            if blob.returncode != 0:
                continue
            fixed = []
            for line in blob.stdout.split(b"\n"):
                if line.startswith(b"<<<<<<< "):
                    line = b"<<<<<<< ours"
                elif line.startswith(b">>>>>>> "):
                    line = b">>>>>>> theirs"
                elif line.startswith(b"||||||| "):
                    line = b"||||||| base"
                fixed.append(line)
            h = git("hash-object", "-w", "--stdin", input=b"\n".join(fixed), env=env).stdout.decode().strip()
            mode = out("ls-tree", tree, path).split()[0]
            git("update-index", "--add", "--cacheinfo", f"{mode},{h},{path}", env=env)
        return out("write-tree", env=env), True


def tree_with_patch(base_tree, patch_bytes):
    with tempfile.TemporaryDirectory() as d:
        env = {**ENV, "GIT_INDEX_FILE": str(Path(d) / "index")}
        git("read-tree", base_tree, env=env)
        if patch_bytes:
            r = subprocess.run(["git", "apply", "--cached", "--whitespace=nowarn", "--allow-empty"], input=patch_bytes, capture_output=True, env=env)
            if r.returncode != 0:
                raise SystemExit("patch does not apply: " + r.stderr.decode())
        return out("write-tree", env=env)


def commit(tree, parents, message):
    args = ["commit-tree", tree]
    for p in parents:
        args += ["-p", p]
    return out(*args, "-m", message)


def record(src, main):
    steps = []
    commits = out("rev-list", "--first-parent", "--reverse", f"{main}..{src}").split()
    PATCHES.mkdir(parents=True, exist_ok=True)
    for old in PATCHES.glob("*.patch"):
        old.unlink()
    # Leading single-parent commits that belong to a fast-forwarded pull request are replaced by one fast-forward to the first merge's first parent.
    first_merge = next(i for i, c in enumerate(commits) if len(out("rev-list", "--parents", "-n1", c).split()) == 3)
    ff_target = commits[first_merge]
    steps.append({"kind": "ff", "ref": ff_target, "tree": out("rev-parse", ff_target + "^{tree}"), "note": "descriptor rollback (#67) fast-forwarded, with main merged in"})
    for c in commits[first_merge + 1:]:
        parts = out("rev-list", "--parents", "-n1", c).split()
        parent, subject = parts[1], out("log", "-1", "--format=%s", c)
        tree = out("rev-parse", c + "^{tree}")
        n = len(steps)
        if len(parts) == 3:
            other = parts[2]
            auto, conflicted = merge_tree(parent, other)
            patch = git("diff", "--binary", auto, tree).stdout
            step = {"kind": "merge", "ref": other, "tree": tree, "conflicted": conflicted}
        else:
            patch = git("diff", "--binary", parent, c).stdout
            step = {"kind": "patch", "tree": tree, "subject": subject}
        if patch:
            name = f"{n:02d}-{step['kind']}-{(step.get('ref') or c)[:9]}.patch"
            (PATCHES / name).write_bytes(patch)
            step["patch"] = name
        step["pr"] = None
        steps.append(step)
    manifest = {"main": out("rev-parse", main), "steps": steps, "batches": []}
    MANIFEST.write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"recorded {len(steps)} steps")


def overlay_tree(base_tree, source_tree, paths):
    """`base_tree` with every path (a directory or a file) replaced by its content in `source_tree`: how the production-infrastructure batch is cut out of the final tree."""
    with tempfile.TemporaryDirectory() as d:
        env = {**ENV, "GIT_INDEX_FILE": str(Path(d) / "index")}
        git("read-tree", base_tree, env=env)
        for path in paths:
            listing = git("ls-files", "-s", "--", path, env=env).stdout.decode().splitlines()
            for line in listing:
                git("update-index", "--force-remove", line.split("\t", 1)[1], env=env)
            for line in git("ls-tree", "-r", source_tree, "--", path).stdout.decode().splitlines():
                meta, name = line.split("\t", 1)
                mode, _, sha = meta.split()
                git("update-index", "--add", "--cacheinfo", f"{mode},{sha},{name}", env=env)
        return out("write-tree", env=env)


def replay(manifest, only=None, verify_only=False):
    main = manifest["main"]
    head = None
    tips = {}
    batch_ends = {b["after_step"]: b for b in manifest["batches"]}
    for i, s in enumerate(manifest["steps"]):
        if s["kind"] == "ff":
            head = s["ref"]
            tree = out("rev-parse", head + "^{tree}")
        elif s["kind"] == "merge":
            auto, _ = merge_tree(head, s["ref"])
            patch = (PATCHES / s["patch"]).read_bytes() if s.get("patch") else b""
            tree = tree_with_patch(auto, patch)
            head = commit(tree, [head, s["ref"]], f"integration step {i}: merge {s['ref'][:9]}")
        else:
            base = out("rev-parse", head + "^{tree}")
            patch = (PATCHES / s["patch"]).read_bytes() if s.get("patch") else b""
            tree = tree_with_patch(base, patch)
            head = commit(tree, [head], s.get("subject", f"integration step {i}"))
        if tree != s["tree"]:
            raise SystemExit(f"step {i} ({s['kind']} {s.get('ref', '')}): tree {tree} differs from the recorded {s['tree']}")
        if i in batch_ends:
            tips[batch_ends[i]["branch"]] = head
        for o in manifest.get("overlays", []):
            if o["after_step"] == i:
                base = out("rev-parse", tips[o["base_branch"]] + "^{tree}")
                src = s["tree"] if o["source_step"] == i else None
                if src is None:
                    raise SystemExit("an overlay's source must be the step it follows")
                tree = overlay_tree(base, src, o["paths"])
                if tree != o["tree"]:
                    raise SystemExit(f"overlay {o['branch']}: tree {tree} differs from the recorded {o['tree']}")
                tips[o["branch"]] = commit(tree, [tips[o["base_branch"]]], o["title"])
    return tips


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "record":
        record(sys.argv[2], sys.argv[3])
        return
    manifest = json.loads(MANIFEST.read_text())
    if "--fetch" in sys.argv:
        for s in manifest["steps"]:
            if s.get("pr"):
                git("fetch", "-q", "origin", f"refs/pull/{s['pr']}/head")
    tips = replay(manifest)
    if cmd == "rebuild":
        for name, sha in tips.items():
            if "--only" in sys.argv and sys.argv[sys.argv.index("--only") + 1] not in name:
                continue
            git("branch", "-f", name, sha)
            print(f"{name} -> {sha[:9]}")
    else:
        print(f"verified {len(manifest['steps'])} steps; batches: {', '.join(tips)}")


if __name__ == "__main__":
    main()
