"""Merge safety for port/ (and optionally the shared source dirs) between a worker and main.

    python port/tools/merge_check.py w7                 # report what w7 changed in port/
    python port/tools/merge_check.py w7 --all           # also src/ include/ libs/ config/
    python port/tools/merge_check.py w7 --apply         # copy safe changes + clean 3-way merges into main
    python port/tools/merge_check.py w7 --merge-out D   # write 3-way results (and conflicts) to D for review
    python port/tools/merge_check.py w7 --record        # record main's current port/ as w7's base

Why: copying a worker's port/ file wholesale over main drops whatever other agents
merged into main since that worker synced (hooks, renames, knobs). This tool compares
three versions of every file:
    base   = main's file when the worker last synced (recorded by --record, which
             `agent_sync.ps1 -Port` runs after copying main's port/ into the worker),
    worker = the worker's current file,
    main   = main's current file,
and classifies it:
    worker-only   worker changed it, main did not since the sync -> safe to copy
    main-only     main changed it, the worker did not -> worker copy is STALE, do not copy
    both-same     both changed it identically -> nothing to do
    BOTH          both changed it differently -> 3-way merge (clean or CONFLICT)
    new-in-worker / deleted-in-worker / new-in-main (informational)

Base data lives in the worker folder: .sync_port_manifest (path<TAB>md5, like
.sync_manifest) and .sync_port_base/ (copies of the text files, for the 3-way merge).
Without a recorded base the tool falls back to "worker differs from main" and marks
every such file UNKNOWN (no base): merge those by hand.
--all adds src/include/libs/config using the existing .sync_manifest hashes (no base
copies there, so BOTH files are reported but not auto-merged; agent_merge.ps1 handles
the worker-only ones).

The 3-way merge is line based (diff3 style): hunks of the base->worker and base->main
diffs that do not overlap are both applied; overlapping hunks become a conflict block
(<<<<<<< worker / ======= / >>>>>>> main) in the --merge-out copy and are never applied
to main by --apply.
"""
import argparse
import difflib
import hashlib
import os
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def find_main():
    """The main tree: this script's tree when run from main, else the one next to workers/."""
    if ROOT.parent.name != "workers":
        return ROOT
    for p in ROOT.parents[1].iterdir():
        if p.is_dir() and p.name.startswith("sms-main"):
            return p
    return ROOT


MAIN = find_main()
WORKERS = MAIN.parent / "workers"

# port/ content that is generated or too big to matter for merges
SKIP_DIRS = {"build", "build_hl", "sjis", "smoke", "smoke_ext", "__pycache__", "cache", "user", "gen"}
SKIP_PREFIX = ("build_aurora",)
TEXT_EXT = {".py", ".cpp", ".c", ".h", ".hpp", ".inl", ".md", ".ps1", ".txt", ".json", ".ini",
            ".patch", ".cmake", ".bat", ".wgsl", ".toml", ".yml", ".yaml", ".in"}
BASE_MAX = 4 << 20


def skipped(rel_parts):
    for part in rel_parts[:-1]:
        if part in SKIP_DIRS or part.startswith(SKIP_PREFIX):
            return True
    return False


def files_under(root, sub="port"):
    base = root / sub
    out = {}
    if not base.exists():
        return out
    for p in base.rglob("*"):
        if not p.is_file():
            continue
        rel = p.relative_to(root)
        if skipped(rel.parts) or p.suffix.lower() in (".exe", ".pdb", ".obj", ".lib", ".bmp", ".png", ".dll"):
            continue
        out[rel.as_posix()] = p
    return out


def md5(p):
    return hashlib.md5(p.read_bytes()).hexdigest().upper()


def read_lines(p):
    return p.read_bytes().decode("utf-8", errors="surrogateescape").splitlines(keepends=True)


def write_lines(p, lines):
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes("".join(lines).encode("utf-8", errors="surrogateescape"))


# ----------------------------------------------------------------------------- record

def record(worker):
    w = WORKERS / worker
    man, base = w / ".sync_port_manifest", w / ".sync_port_base"
    if base.exists():
        shutil.rmtree(base)
    rows = []
    for rel, p in sorted(files_under(MAIN).items()):
        rows.append(f"{rel}\t{md5(p)}")
        if p.suffix.lower() in TEXT_EXT and p.stat().st_size <= BASE_MAX:
            dst = base / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dst)
    man.write_text("\n".join(rows) + "\n", encoding="utf-8")
    print(f"recorded {len(rows)} port/ files of main as {worker}'s base")


def load_manifest(path):
    m = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8-sig", errors="replace").splitlines():
            parts = line.split("\t")
            if len(parts) == 2:
                m[parts[0].replace("\\", "/")] = parts[1].upper()
    return m


# ----------------------------------------------------------------------------- 3-way

def merge3(base, mine, theirs):
    """Line-based diff3. Returns (merged_lines, conflict_count)."""
    def hunks(a, b):
        sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
        return [(i1, i2, b[j1:j2]) for tag, i1, i2, j1, j2 in sm.get_opcodes() if tag != "equal"]

    hm, ht = hunks(base, mine), hunks(base, theirs)
    out, conflicts, pos = [], 0, 0
    i = j = 0
    while i < len(hm) or j < len(ht):
        # start a region at the earliest hunk, then absorb every hunk of either side
        # that overlaps it (or starts at the same base line)
        if j >= len(ht) or (i < len(hm) and hm[i][0] <= ht[j][0]):
            start, end = hm[i][0], hm[i][1]
        else:
            start, end = ht[j][0], ht[j][1]
        gm, gt = [], []
        grew = True
        while grew:
            grew = False
            while i < len(hm) and (hm[i][0] < end or hm[i][0] == start):
                gm.append(hm[i])
                end = max(end, hm[i][1])
                i += 1
                grew = True
            while j < len(ht) and (ht[j][0] < end or ht[j][0] == start):
                gt.append(ht[j])
                end = max(end, ht[j][1])
                j += 1
                grew = True
        out += base[pos:start]

        def apply(group):
            res, p = [], start
            for a1, a2, repl in group:
                res += base[p:a1] + repl
                p = a2
            return res + base[p:end]

        if gm and gt:
            vm, vt = apply(gm), apply(gt)
            if vm == vt:
                out += vm
            else:
                conflicts += 1
                out += ["<<<<<<< worker\n"] + vm + ["=======\n"] + vt + [">>>>>>> main\n"]
        else:
            out += apply(gm or gt)
        pos = end
    out += base[pos:]
    return out, conflicts


# ----------------------------------------------------------------------------- check

def check(worker, include_all, apply, merge_out, apply_new=False):
    w = WORKERS / worker
    if not w.exists():
        sys.exit(f"no worker folder {w}")
    base_man = load_manifest(w / ".sync_port_manifest")
    base_dir = w / ".sync_port_base"
    wf, mf = files_under(w), files_under(MAIN)
    if include_all:
        shared_man = load_manifest(w / ".sync_manifest")
        base_man = {**{k.replace("\\", "/"): v for k, v in shared_man.items()}, **base_man}
        for d in ("src", "include", "libs", "config"):
            wf.update(files_under(w, d))
            mf.update(files_under(MAIN, d))
    rows = {k: [] for k in ("worker-only", "BOTH-clean", "BOTH-CONFLICT", "BOTH-nobase", "UNKNOWN",
                            "main-only", "both-same", "new-in-worker", "deleted-in-worker", "new-in-main")}
    for rel in sorted(set(wf) | set(mf)):
        wp, mp = wf.get(rel), mf.get(rel)
        wh = md5(wp) if wp else None
        mh = md5(mp) if mp else None
        bh = base_man.get(rel)
        if wh == mh:
            continue
        if bh is None and not base_man:
            if wp and mp:
                hint = "worker newer" if wp.stat().st_mtime > mp.stat().st_mtime else "MAIN NEWER"
            else:
                hint = "only in worker" if wp else "only in main"
            rows["UNKNOWN"].append(f"{rel}  ({hint})")
            continue
        if wp and not mp:
            if bh is None:
                rows["new-in-worker"].append(rel)
            else:
                rows["main-only"].append(rel + "  (deleted in main)")
            continue
        if mp and not wp:
            rows["new-in-main" if bh is None else "deleted-in-worker"].append(rel)
            continue
        w_changed, m_changed = wh != bh, mh != bh
        if w_changed and not m_changed:
            rows["worker-only"].append(rel)
        elif m_changed and not w_changed:
            rows["main-only"].append(rel)
        elif bh is None:
            rows["UNKNOWN"].append(rel + "  (not in the recorded base)")
        else:
            bp = base_dir / rel
            if not bp.exists():
                rows["BOTH-nobase"].append(rel)
                continue
            merged, n = merge3(read_lines(bp), read_lines(wp), read_lines(mp))
            rows["BOTH-CONFLICT" if n else "BOTH-clean"].append(rel + (f"  ({n} conflict blocks)" if n else ""))
            if merge_out:
                write_lines(Path(merge_out) / rel, merged)
            if apply and not n:
                write_lines(MAIN / rel, merged)
    if apply:
        for rel in rows["worker-only"] + (rows["new-in-worker"] if apply_new else []):
            dst = MAIN / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(wf[rel], dst)
            # copy2 keeps the worker's mtime, which can be older than main's objects:
            # touch it so the incremental builds (ninja, build_aurora) recompile it.
            os.utime(dst, None)
    labels = {
        "worker-only": "worker changed, main did not: safe to copy" + (" (copied)" if apply else ""),
        "new-in-worker": "new in the worker" + (" (copied)" if apply and apply_new else " (copy with --apply-new)"),
        "BOTH-clean": "both changed, 3-way merge is clean" + (" (merged into main)" if apply else ""),
        "BOTH-CONFLICT": "both changed, CONFLICT: merge by hand (see --merge-out)",
        "BOTH-nobase": "both changed, no base copy (binary/large): merge by hand",
        "UNKNOWN": "differs, no recorded base: check by hand",
        "main-only": "main changed since the sync, worker did not: worker copy is STALE, do not copy",
        "deleted-in-worker": "deleted in the worker (or main)",
        "new-in-main": "new in main since the sync (worker lacks it)",
        "both-same": "",
    }
    print(f"== {worker} vs main ({MAIN.name}); base: "
          + (f"{len(base_man)} recorded files" if base_man else "NONE (run --record after syncing)"))
    for key in ("BOTH-CONFLICT", "BOTH-nobase", "UNKNOWN", "BOTH-clean", "worker-only", "new-in-worker",
                "deleted-in-worker", "main-only", "new-in-main"):
        if rows[key]:
            print(f"-- {labels[key]} ({len(rows[key])})")
            for r in rows[key]:
                print("   ", r)
    return 1 if rows["BOTH-CONFLICT"] or rows["BOTH-nobase"] else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("worker")
    ap.add_argument("--record", action="store_true", help="record main's current port/ as the worker's base")
    ap.add_argument("--all", action="store_true", help="also check src/ include/ libs/ config/")
    ap.add_argument("--apply", action="store_true",
                    help="copy worker-only/new files and write clean 3-way merges into main")
    ap.add_argument("--apply-new", action="store_true", help="with --apply: also copy files new in the worker")
    ap.add_argument("--merge-out", help="write every 3-way result (with conflict markers) here")
    a = ap.parse_args()
    if a.record:
        record(a.worker)
        return 0
    return check(a.worker, a.all, a.apply, a.merge_out, a.apply_new)


if __name__ == "__main__":
    sys.exit(main())
