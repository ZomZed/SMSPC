# Port agent notes (read this first)

Goal of this fork: a **PC port**. Every function must EXIST in source and BEHAVE like the
original. Byte-exact matching is a bonus, not the goal.

## Definition of done ("port-ready")
- Every symbol of the unit (from the JP map) is defined, in map order (reversed for
  `-inline deferred` units; nearly all game code is).
- Each function is >= ~85% fuzzy, or lower only when blocked by a known issue below.
- Build green, `build\tools\dtk.exe shasum -c config\GMSE01\build.sha1` OK,
  `python -m ninja changes_all` shows no regressions elsewhere.
- Then STOP polishing and report. Do not spend time pushing 95% -> 100%.

## Never
- Fix original game bugs (keep them, add `// TODO: original behaviour`).
- Edit MSL / dolphin SDK. Edit JSystem only for header-only additions, and say so.
- PowerShell/.NET text replacement on sources (breaks Shift-JIS / adds BOM). Use Edit/Write.
- `ninja -t clean`, or write outside your own worker folder.

## Runtime-bug lessons (from testing the all-source build in Dolphin)
- A `~` diff line is NOT always harmless: if the differing operand is an IMMEDIATE or a
  symbol/label (e.g. `li r4, 0x4` vs `li r4, 0x6`, a different GX enum or float constant),
  it is a real behaviour bug. Only register-name and stack-offset differences are harmless.
- Check data/vtable matches too: a header declaring virtuals in the wrong ORDER compiles
  call sites identically but runs the wrong bodies (seen in PollutionCount).
- Mis-decoded float constants: a double's high word read as a float (120.0 -> 3.46875f).
- `#ifdef VERSION_GMSP01` is true for US too; check which branch the US asm really has.
- Wrong symbol names in config/GMSE01/symbols.txt make all-source builds read the wrong data.
- Local arrays handed to SDK getters must be full size (GXGetViewportv writes 6 floats,
  GXGetProjectionv 7). An undersized array silently overwrites the next local; this broke
  Mario's occlusion test, the behind-wall silhouette, and with it fruit/eye rendering.
  A stack-frame size diff around such a call is a hint, not always "frame-only".
- Diagnostic switches exist for rendering bugs (all-source only, via SMS_EXTRA_DEFINES):
  SMS_SHOW_RASA, SMS_FORCE_CULLNONE, SMS_FORCE_NOBLEND (J3DMaterialFactory),
  SMS_A0_PROBE + SMS_DIRTY_TRACE (GX dirty state), SMS_ENTRY_TRACE (draw-buffer entries),
  SMS_OCC_TRACE (Mario occlusion / draw sync), SMS_ALPHA_DEBUG.

- decomp-diff's fuzzy % can read 100% while float constants differ (seen in THino2Params).
  For behaviour sweeps run `python tools/semdiff.py <Folder>/<unit>` (from the project root,
  after building): register/stack-insensitive diff of opcodes, immediates, offsets, call
  targets and resolved float values.
- Scan tools for decomp-correction sweeps (from the project root, after building; each has
  usage in its docstring):
  - `python tools/aligned_scan.py build/aligned.txt [Enemy/ MoveBG/ ...]`: walks objdiff's
    aligned instruction pairs of every nonmatching function and keeps only likely logic
    differences (same-base field offset, cror condition, cmpwi/li/rlwinm immediate, float
    literal, call target or one-sided call), scored per function. Skips ctors/factories.
  - `python tools/aligned_isolated.py build/aligned.txt`: drops the pairs that are just two
    values swapped by scheduling; what is left is usually one wrong offset/constant/call.
  - `python tools/lowmatch.py [--below 60] [prefix ...]`: functions under 60% or with a big
    size mismatch (fabricated bodies, missing branches).
  - `python tools/mwcc_probe.py "Player/Mario.hpp" "OFF(TMario, mJumpParams)" "sizeof(X)"`:
    evaluates offsets/sizes with the real MWCC (GameCube layout) to name the field behind an
    asm offset. A TParamRT<T> is 0x14 bytes with its value at +0x10.
  - False positives that remain: constant-pool labels for equal values (`{@1234}` vs
    `{@567}`, compare the values in the .s), loop unrolling, inlined vs out-of-line TVec3 /
    checkStatusType helpers, fmadds vs fmuls+fadds. Always confirm with decomp-diff -d.
- Many message IDs were transcribed from JP: balloons used 0xE00xx, US uses plain (often
  shifted) IDs; CardLoad/CardSave, scenario-name and Talk tables also differ. Check the US asm.

## Known blockers (don't burn time on these; leave a one-line TODO)
- **Frame-only diffs** (only stack offsets/frame size differ): very common, often 8 bytes
  short everywhere. Skip.
- **`JGeometry::TQuat4::rotate` / `mul` / `slerp`** (JGQuat4.hpp): the original inlines a
  different shape; tried twice, no safe fix. The 1-arg `mul` has a wrong y term - write the
  product out by hand if you need it.
- **TVec3 copy ctor / operator=** (JGVec3.hpp): written as a Vec* copy on purpose; making
  them implicit breaks matched units. Don't touch.
- **`std::fmodf`**: original weak copy differs from our MSL; use it and leave a TODO.
- **Inline depth**: original often calls a helper out of line one level deeper than we do,
  or inlines something we call. MWCC decides by *statement count* of the callee
  (early returns / one assignment per line can flip it). Check the JP map's UNUSED list
  for the real helper name before inventing one. `#pragma dont_inline` only as a last
  resort, with a TODO.
- **`TVec3::scaleAdd`** (JGVec3.hpp): our header computes `b + c*scale`, the original weak copy
  `b*scale + c`. Fixing the header alone regresses many units whose call sites were written
  for the swapped form; needs a coordinated header+callers change. Don't try it piecemeal.
- **`#pragma inline_depth(N)`** is applied file-wide (value at end of file wins), so it can't
  target one function; tried in cameragc/cannon/MapMirror/BathtubKiller, always net loss.
- **Missing weak template copies** (`TVec3::set<f>`, `TEnemyManager::getSaveParam`, ...):
  our compiler inlines all calls so no copy is emitted. Ignore.

## Useful facts
- JP linker map: `orig/GMSJ01/files/mario.MAP` (grep only). Symbol checker:
  `python tools/validate-symbol-order.py -u <unit> --map orig/GMSJ01/files/mario.MAP`
- Draft generator: `python tools/port_scaffold.py <unit>` writes
  `build/drafts/<unit>.txt` (symbol list in source order + m2c drafts of every function).
- m2c drafts contain `M2C_ERROR(/* unknown instruction: cror eq, gt, eq */)`: that is
  just a `>=` compare (`cror eq, lt, eq` = `<=`). Not a real problem.
- m2c: `..\m2c\m2c-master\m2c.py -t ppc -f <mangled> --globals=used build/GMSE01/asm/<path>.s`
- US config was swept: symbols/splits are trustworthy. If a unit you work on still has
  `fn_`/`lbl_` names or sections that look wrong, report it.
- Particle IDs go in `include/System/Particles.hpp`; actor types in
  `include/Strategic/ActorTypes.hpp` (shared - add, never rename).
- Spawning: enemies are created from `src/System/MarNameRefGen_*.cpp`. If yours is
  commented out there, mention it in your report (don't edit unless asked).
- Good style references written this session: src/Enemy/killer.cpp, bosswanwan.cpp,
  tinkoopa.cpp, Koopa.cpp, koopajr.cpp, src/Animal/Bird.cpp, BeeHive.cpp.
