# AI / contributor guide: matching work, step by step

Tool-agnostic. Short rules are in `/AGENTS.md`; this file is the full method. Commands run from the repo root.
`python` must be a real interpreter with `ninja` installed (`python -m pip install ninja`). On Windows the
`WindowsApps\python.exe` stub does not work; use the full path to a real python.exe.
The PowerShell helper scripts find a working python themselves.

## 1. Setup (once per tree)
1. `python configure.py --version GMSE01` (creates `build.ninja`; needs the user's game files in `orig/GMSE01/`).
2. `python -m ninja` must end with `build/GMSE01/mario.dol: OK`.
3. `python -m ninja baseline` - snapshot of every function's match %, used later by `changes_all` and `ai_task.py check`.
   Take it on an unedited tree. Re-take it after you accept a verified improvement and want a new reference.
   (A stale baseline makes `check` report old losses that are not yours.)

## 2. The loop
1. Pick: `python tools/ai_task.py next --n 10`. Lists unmatched-bytes ranked functions of non-complete game units, already
   excluding `tools/match_tried.txt` entries and mechanisms known to be unsolved. Prefer units at 90-98% with several
   non-matching functions; single functions at 99.9% are almost always frame-only or register-only.
2. Read `docs/MATCHING_CHEATSHEET.md` (one page) and `grep -n <name> tools/match_tried.txt`.
3. Look: `python tools/ai_task.py show <unit> <function substring>`. It resolves the full overload signature
   (`decomp-diff.py -d` needs it) and drops lines that differ only in register names, stack offsets or literal-pool labels.
   `--full` also prints the whole diff. For a function far below 95% read the ROM asm (`build/GMSE01/asm/<path>.s`) and
   consider rewriting from m2c output; it is often structurally wrong (missing branches, wrong inline helpers, wrong types).
4. Edit the source in `src/` (or minimally `include/`).
5. Rebuild just that object: `python tools/ai_task.py build <unit>`, then `show` again. Do not print a whole big function more than once;
   use `python tools/decomp-diff.py -u mario/<unit> -d "<full signature>" --range 100-200`.
6. Repeat; give up on a function after about 8 attempts (TODO line + `ai_task.py tried`).
7. End: `python tools/ai_task.py check`. It runs a full `ninja`, verifies the DOL sha1 (`config/GMSE01/build.sha1`), and lists every
   function that changed with losses marked. Exit code 1 when the DOL differs or any function lost more than 0.5 points.
   Fix or undo the losers (a header change can break a linked unit that you never opened - that is why the full build is mandatory).
8. Report (template below).

## 3. Method notes
- Doubt every assumption; take the evidence (asm, debug strings, neighbouring functions, other TUs). When strong evidence is
  missing, leave the function non-matching and move on.
- Inline depth is the cause of most remaining diffs (the compiler inlines or does not inline a tiny getter/wrapper). Techniques
  that worked: static-inline wrappers pushing a call out of line, two chained one-statement wrappers at a call site (pushes the callee to inline
  pass 3), local copies of member vectors, term/operand order, adding dropped calls. Details: cheat sheet.
- Frame-only remainder (instructions identical, stack size differs): first try the real constructs from the cheat sheet
  ("Frame-only functions": by-value param, ctor temp, operator expression, named local, inline getter level, empty dtor).
  Only then padding with the FAKEMATCH comment (AGENTS.md rule 5).
- Header levers (JGeometry/JMath shared inlines) can move hundreds of functions; there the loss rule is "clearly net positive, then
  re-tune the losers". Always full ninja + DOL check afterwards. Other `libs/` stay untouched.
- Register-only diffs: `tools/regalloc_search.py` (decl-order/split search) rarely helps; about 800 near-matches differ only in register
  allocation or frame size and are the floor. `tools/perm_run.py` runs decomp-permuter on plain-C units only (the permuter cannot parse C++);
  it is not in the repo (expected at `../decomp-permuter`). It works on preprocessed output, so it DROPS `#ifdef SMS_PORT` blocks and
  expands macros: never transplant a function containing `#if`, and review partial results for semantic changes.
- Game units use compiler GC/1.2.5 (1.2.5n and 1.3.2 measured worse). Do not switch versions.
- A fully matched unit may be flipped to `MatchingFor("GMSE01")` in `configure.py`; only the maintainer does that
  (flipping can break the DOL through duplicate symbols or differing bytes).

## 4. Parallel workers without any vendor tooling
Use several copies of the tree so agents never share build state.
1. Create: copy the whole tree (build/ and orig/ included; `robocopy src dst /MIR` on Windows, `rsync -a` elsewhere) to
   `../workers/<name>`. Hard-link `orig/` to save disk (a tree is several GB; agents have died on full disks).
2. In the copy run `python configure.py --version GMSE01` again: `build.ninja` holds an absolute PCH path (`SMS.mch`) into the source tree.
3. Windows shortcut: `powershell -File tools\agent_sync.ps1 -Worker w1` does 1-2 from main (mirrors src, include, config, libs, docs, tools,
   configure.py, AGENTS.md/CLAUDE.md; touches changed files; rebuilds; verifies the DOL; takes the baseline; writes `.sync_stamp` and
   `.sync_manifest`). `-Port` also copies `port/`.
4. The agent works ONLY inside its worker and runs the loop above there (its baseline is the one from sync time).
5. Merge: `powershell -File tools\agent_merge.ps1 -Worker w1 -DryRun` first. It lists files the worker changed since sync and marks CONFLICT where
   main changed the same file meanwhile (merge those by hand, 3-way). Without `-DryRun` it copies the files into main, runs a full ninja, checks the
   DOL, prints unit changes and decreases, and resets the baseline only when clean. Without PowerShell do the same by hand: diff worker vs main
   (`diff -rq src include libs config`), copy the changed files, full ninja, `ai_task.py check`.
   `tools\merge_round.ps1 -Worker w1` = merge + verify + re-sync.
6. After merging several workers run ONE final full `python -m ninja`: a per-merge DOL check misses a header edit from worker B breaking a
   unit that was still fine after worker A's merge.
7. Never merge and re-sync a worker while its agent is still running (sync overwrites its edits and baseline); confirm it has reported.
   Never chain merge+sync after a CONFLICT skip. Keep at most 4 agents in parallel (CPU and disk).

## 5. Gotchas
- PCH: `build.ninja` has an absolute PCH path; re-run `configure.py` in every copied tree. `--build-dir` for another game version in the
  same tree is broken for the same reason - use a separate copy.
- Stale objects: robocopy / `cp -p` preserve old mtimes, so ninja reuses stale objects and the DOL check fails. Touch every copied file (agent_sync does).
- BOM / Shift-JIS: many sources are Shift-JIS. PowerShell `Set-Content`/`Out-File` and editors that "convert to UTF-8" add a BOM or corrupt them.
  `agent_merge` warns `BOM in <file>`. Use byte-exact edit tools; check with `head -c3 file | xxd` (EF BB BF = bad).
- No US linker map exists: `tools/validate-symbol-order.py` cannot run for GMSE01. The JP map (`orig/GMSJ01/files/mario.MAP`) works with `--map`
  for units present in both versions.
- Shell heredocs can mangle backslashes in inline Python (`\0`, `\n`); write scripts to a file or build backslashes with `chr(92)`, and check for NUL bytes.
- Relative paths in .NET/PowerShell resolve against the process directory, not `cd`; use absolute paths.
- Ninja target names use the OS separator (`build\GMSE01\src\Enemy\Kukku.o` on Windows); `ai_task.py build` handles it.
- A failed compile can silently keep the old `.o`; read the ninja output (`ai_task.py build` prints FAILED).
- Disk space: a full disk once emptied a source file. Check file sizes before merging.
- Do not launch the game or the PC port for matching tasks.

## 6. Report template (under 40 lines)
```
check: DOL OK | units improved: <n> | functions improved: <n> | losses > 0.5: none (or: name old -> new, why accepted)
Files changed:
  src/Enemy/Kukku.cpp - <one line>
  headers: include/Enemy/Kukku.hpp - <one line>
New patterns (<= 5):
  - <one line each; add general ones to docs/MATCHING_CHEATSHEET.md>
Given up (lines added to tools/match_tried.txt):
  Enemy/Kukku<TAB>perform__<TAB><reason>
Remaining difference per function worked on:
  <function>: <reg alloc / frame N bytes / inline depth / ...>
```
