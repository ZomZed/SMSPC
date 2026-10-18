# AGENTS.md - Super Mario Sunshine matching decompilation (US, GMSE01)

Entry point for any AI coding agent (Codex, Gemini CLI, Cursor, Aider, Claude) or human.
Full method, worker setup and report template: `docs/AI_CONTRIBUTING.md`.
The older long-form repository guide is `docs/DECOMP_GUIDE.md` (layout, conventions, UNUSED functions).

## What this is
A matching decompilation of Super Mario Sunshine (GameCube, PowerPC Gekko, Metrowerks mwcceppc GC/1.2.5).
Goal: C++ in `src/` + `include/` that compiles to the same bytes as the original `mario.dol`.
Task: raise the match % of functions that are not yet 100% (measured by objdiff fuzzy match).
Also in the tree: a PC port (`port/`, `#ifdef SMS_PORT` blocks). It must never influence the GameCube build.

## Hard rules
1. Match the original exactly. Never "fix" a bug of the original game; reproduce it. (Mistakes in earlier decomp code may be fixed.)
2. The GameCube DOL must stay byte-identical: `build/GMSE01/mario.dol: OK` after a FULL `python -m ninja`.
3. No other function may lose more than 0.5 fuzzy points (list any loss you accept; justify it).
4. Keep every `#ifdef SMS_PORT` / `SMS_PORT_AURORA` block verbatim. Port-only code goes only inside those blocks. Write C++98 that clang also accepts.
5. Fakematch padding only when the instructions already match and padding makes the function byte-identical
   (frame-size only), e.g. `volatile u32 pad[N]; (void)pad;` with the comment `// FAKEMATCH(frame): <N> bytes; real construct unknown`
   on the line above. Never use padding, dummy code or data to hide instruction differences.
6. Editable: `src/`, `include/`. Headers only minimally. `libs/JSystem/include/JSystem/JGeometry/` and `.../JMath/` headers may be edited
   (then a full build + DOL check is mandatory). Other `libs/` and `configure.py` stay untouched unless a task says otherwise.
7. Shift-JIS sources stay Shift-JIS and never get a BOM: edit with a real editor/patch tool, not PowerShell `Set-Content`,
   not `sed -i` on Shift-JIS files, not a Python rewrite with the wrong encoding.
8. Stuck after about 8 tool calls on one function: leave a one-line TODO, record it with `ai_task.py tried`, move on.
   Headbutting a function to 100% creates fakematches; leaving it is fine.

## Do NOT touch
- `SMS_PC/` (the user's PC release folder), user memory cards/saves, port settings (`port_settings.ini`), `port/` (unless your task is port work).
- `orig/` (original game files), `build/` (generated), `config/GMSE01/build.sha1`, `objdiff.json`, `build.ninja`.
- Do not commit, push, or download anything unless told to. Do not overwrite another agent's worker copy.

## Environment
- Python 3.10+ with `ninja` (`python -m pip install ninja` if `python -m ninja --version` fails). Everything below is `python -m ninja` / `python tools/...`
  run from the repo root. On Windows `python` must be a real interpreter (the Microsoft Store stub in `WindowsApps` is not one);
  use the full path to python.exe if needed. No compiler install is needed: the build downloads/uses `build/compilers` and `build/tools`
  (set up by `python configure.py --version GMSE01`; needs `orig/GMSE01/` with the user's own game files - ask the user, never fetch them).
- Not a git repo on the original machine; do not assume `git` exists.
- First build of a fresh tree takes minutes; later rebuilds only recompile touched files.

## One-command workflow (`tools/ai_task.py`, stdlib only)
```
python -m ninja && python -m ninja baseline        # once, on an unedited tree: the reference for the loss check
python tools/ai_task.py next --n 10                # untried targets, biggest unmatched bytes first
python tools/ai_task.py next --n 10 --include-tried  # nearly everything was tried once: lists all with the earlier note
python tools/ai_task.py show Enemy/Kukku perform   # unit + function substring: signature resolved, noise filtered
python tools/ai_task.py search Enemy/Kukku "<full signature>"  # FIRST for register/stack-only diffs: CPU-only search over
                                                   # decl placement, split/joined inits, operand swaps; keeps the best shape
# ... edit src/Enemy/Kukku.cpp (structural changes the search cannot make) ...
python tools/ai_task.py build Enemy/Kukku          # rebuild only that object (fast loop), then `show` again
python tools/ai_task.py check                      # full ninja + DOL sha + losses; exit 1 on DOL mismatch or loss > 0.5
python tools/ai_task.py tried Enemy/Kukku perform__ "regalloc only; tried X, Y"   # record a dead end
```
`show` tells you when only register names, frame size or literal-pool labels differ ("no real differences").
Those are poor targets; read `docs/MATCHING_CHEATSHEET.md` "Frame-only functions" first.
Low-level tools: `tools/decomp-diff.py -u mario/<Unit> -d "<function>" [--range a-b] [--no-collapse]`,
`python -m ninja changes_all`, `tools/near_triage.py`, `tools/regalloc_search.py`, `tools/perm_run.py` (permuter, C units only).
m2c (not in the repo) helps when a function is far below 95%: `python m2c.py -t ppc -f <mangled> --globals=used build/GMSE01/asm/<path>.s`.

## Where the knowledge lives
- `docs/MATCHING_CHEATSHEET.md` - one page of solved patterns (inline depth, frame-only, loads/stores, branch shapes). Read this first.
- `tools/match_tried.txt` - dead ends, one per line (`<unit>\t<mangled substring>\t<reason>`); `grep` it before starting a function.
- `docs/AGENT_MATCHING_TIPS.md` - long tips archive. Grep for a heading only when the cheat sheet points there.
- `docs/AI_CONTRIBUTING.md` - the method, parallel workers, gotchas, report template.
- `docs/DECOMP_GUIDE.md` - repository layout, code conventions. `docs/PORT_AGENT_NOTES.md` / `port/` - PC port only.

## How to report (short, under 40 lines)
1. `ai_task.py check` result: DOL line, units improved, functions improved, every loss.
2. Files changed, one line each (headers listed separately).
3. Up to 5 new patterns, one line each (add worthwhile ones to `docs/MATCHING_CHEATSHEET.md`).
4. For every function given up: the exact `match_tried.txt` line you added, and what the remaining difference is.
Template: `docs/AI_CONTRIBUTING.md` "Report template".
