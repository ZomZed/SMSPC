# Matching cheat sheet (read this, not the whole tips doc)

One page of the levers that actually moved functions in this project. For details grep
`docs/AGENT_MATCHING_TIPS.md` for the heading named in brackets; do not read it top to bottom.

## Check first (cheap, big wins)
- `decomp-diff.py -u <unit> -s missing`: a missing `__sinit_<unit>_cpp` = missing rogue includes
  `<MSound/MSSetSound.hpp>` + `<MSound/MSoundBGM.hpp>` (riccohook +20 points). [missing __sinit]
- Fabricated bodies: a function twice the ROM size (`tools/lowmatch.py`) is guessed code; rewrite from the asm/m2c.
- Skip near-matches whose only diffs are stack offsets / frame size ("frame-only"): say so and move on.

## Inline depth (most remaining diffs are this)
- Inline pass limits: inlined at pass 1 if <= 9 statements, pass 2 if <= 6, pass 3 only if 1, never pass 4; a loop
  inside blocks pass >= 1; non-`inline` templates never inline. [Measured inline pass limits]
- ROM calls a tiny helper out of line where ours inlines it: add one inline level, e.g. a fabricated one-statement
  wrapper (`pushNerve(n) { mSpine->pushNerve(n); }`) or a static inline helper around the call site.
- `theNerve()` compare with an out-of-line nerve ctor + `cntlzw` bool: chain const bool inline helpers
  (`isExploding()`, `isDyingNerve() { return a() || b(); }`). [theNerve ... bool helpers]
- Two-return bool inline (`if (x) return true; return false;`) stays `bl` inside an inlined `&&`/`||`; the ternary
  form inlines. [two-return bool inline]
- A weak helper the ROM calls out of line but ours inlines (`getMActorAnmData`, `isIllegalData`, `TVec3::set<f>`,
  `getSaveParam`, `TVec3::add`; check `decomp-diff.py -s missing`): wrap the call in a one-statement static inline
  helper at the call site (`BGGetAnmData(k) { return k->getMActorAnmData(); }`); if one level is not enough, chain two.
  Fixed ~20 boss nerves at once (bossgesso, bosswanwan). `BOOL` two-return helpers give `cmpwi r0,0`, `bool` gives `clrlwi.`.
- SOLVED "per-caller inline budget": two chained one-statement wrappers at the call site (W1 -> W2 -> real callee) push it
  to inline pass 3, where multi-statement callees are not inlined; a looping helper needs one level. NpcBase::perform
  64% -> 93%, cameragc::perform 83% -> 98%.
- "Per-caller inline budget" in a huge perform is often just helpers whose callees the ROM keeps out of line: wrap the
  calls (2-3 levels for `set`/`setLength`) INSIDE the inlined helper, but only in helpers that have no standalone copy in
  the ROM (wrapping `appearGuidePane`, which also exists standalone, broke it 99.96% -> 77%). Guide::perform 78% -> 89%.
  For a header inline whose inner call you cannot wrap, copy its body into a local static helper with the call wrapped.
- Default-arg ctor + member initialised in the mem-initializer list keeps `TVec3::set<f>` out of line (creators).
- A constant-count loop of a non-trivial inline over `a[i]`/`b[i]` fully unrolls; walking `++src, ++dst` pointers gives
  the ROM's mtctr loop unrolled x3 (calcInHouseNo_ 76% -> 95.5%).
- Two inline temporaries in one expression are built right-to-left; the ROM's copy, copy, sub, sub order needs a helper
  with two named locals (MapCheck crossXZ). A small local array (`TVec2<f32> p[4]`) stays in memory; separate locals
  are scalarised. `.set(x, y)` evaluates y first, a ctor `TVec2(x, y)` x first.
- A uniform string-pool offset shift (`addi rX, r30, off` all off by the same 0x18) is literal-pool layout: skip it.

## Loads, stores, order
- A value reloaded after its compare (`if (t > 0) --t;`): read it through a `const` getter. [const getter]
- Member passed into an inline is re-read after every call; a local stays in a register.
- Two accessor calls in one expression evaluate right-to-left; local const refs left-to-right.
- `Mtx`/`TPosition3f` local read element-wise is scalar-replaced: read through `MtxPtr m = mtx;` to keep it in memory.
- Copy-init (`T a = b;` -> `bl copy`) vs assignment (inline word copy) of struct locals.
- A named product local blocks fmadds fusion; `i / 4.0f` vs `i * 0.25f` changes operand order; `f32 eps =
  epsilon();` keeps a runtime `fneg`.
- `JGeometry::TVec3<f32>(a - b).length()` is the ROM's distance test; `x - (x/10)*10` stops CSE with `x % 10`.
- Switch shape: `case 3: default:` moves the pivot; a one-case `switch` gives signed `cmpw` + `beq/b`.
- Nerve compares: `&TNerveX::theNerve() == mSpine->getCurrentNerve()`.
- Triviality: an empty `~T() {}` pins a type to memory (fields reloaded across calls). [Triviality]

## Do not spend time on (known unsolved; `tools/match_targets.py` already skips them)
Huge `perform`s are worth one try with the helper-wrapping rule above, nothing more;
nested vector operators one pass deeper (Tongue movement, MarioSpecial wire*); `TQuat4::rotate` depth (fireWanwan,
TabePuku Drag); `normalize()` after `length()` CSE; `updateTimers` address-taken decrement; frame-only functions.

## Library header shapes found (user-approved JSystem edits, 2026-10-15)
- `TQuat4<T>::rotate(const TVec3&, TVec3&)` must be <= 6 statements (`TQuat4<T> q;` + 4 assignments +
  `rDest.set(expr, expr, expr)`) to inline at pass 2 like the ROM. An auto-inlined .cpp member costs no inline pass in
  ours but one in the ROM: wrap the inner call (not the member) in a one-statement static inline.

## Frame-only functions (measured; details: [Frame-size accounting])
- List the ROM's `(r1)` slots and classify them (`stw` x3 = copy ctor / `operator=` / by-value param; `stfs` x3 = ctor
  temp or `set`; `lfs`+`stfs` pairs = `set(const Vec&)`), then add the construct that produces the missing object.
- Costs: named / ctor-temp / by-value `TVec3` 12 each; `a * k` = 24 (+8 dead assigned, +24 dead into a `bl`); `a + b`
  assigned = 12 + 24 dead; `TVec3(a-b).length()` = 24 + 20 dead; trivial 8-byte temp as inline `this` = +12 dead (an
  empty dtor removes it).
- Dead 4-byte chain temps appear only once the function has a stack object: 1/2/3 inline getter levels = +8/+20/+32,
  +8 if consumed inline, +8 via the global accessor.
- Layout high->low: named locals (declaration order) > caller temps (source order) > inlined callee objects (return >
  param copy > locals) > dead slots; an object low in the ROM but high in ours is callee-side there.

## TVec3 `v = v * k` copy count (2026-10-15)
- ROM `v = v * k` = 2 copies (param copy, scale, copy back): use a file-local helper
  `static inline const TVec3<f32>& scaledRef(TVec3<f32> v, f32 k) { v *= k; return v; }` (return by value under `#ifdef SMS_PORT`), `v = scaledRef(v, k);` (hamukuri forceRoll 90->99.7).
- Result fed into a `const&` callee (`+=`, setGlobalScale, operator+) = 3 copies: keep the plain value-returning `operator*`.
- `k * v` (scalar first, ref-returning friend in JGVec3.hpp) loads k before the param copy; `v * k` after. Pick by the ROM's load order.

## Dead `lwz ->unk38` + branchless `cmplwi` (2026-10-15)
- That is a discarded call to the inline `TEnemyManager::getActiveObjNum()`: write `mgr->getActiveObjNum();` as a
  statement (koopajr TKoopaJr::init 95.8 -> 100). An empty `if` is dropped; a `volatile bool` adds extra code.
  SOLVED for manager load/loadAfter: `getActiveObjNum();` before the base load() and again after `unk38 = new ...`
  (BathtubKiller, koopajr, coasterkiller managers all 100%).

## Small branch/compare shapes (2026-10-15)
- ROM `beq next; b exit` after two early returns: merge them, `if (a || b) return;` (3 functions fixed in one round).
- ROM `cmplwi` where ours has `cmpwi` on an s32/enum member: cast `(u32)x` at the compare; for a u8 state byte cast `(u8)`.
- ROM compares a bool with `cmpwi` instead of `clrlwi.`: use `BOOL`/`TRUE` for the local.
- A float passed through a named local can add an `fmr`; pass the expression directly.
- Extra `extsh` in ours: an `s16` local holding an int call result adds it (pass the call straight into the expression);
  an s16 member set from `rand()` wants `(u16)rand()`. ROM `neg; extsh` on a store wants `(s16)(-(s32)(expr))`.
- `tools/near_triage.py` ranks near-matches by real instruction differences (register/offset/pool noise removed).
- Two TColor member stores with "temp+copy, temp, one index calc" in the ROM: a 2-param static inline taking both colours
  BY VALUE (args evaluated right-to-left) (Talk2D2 setTagParam 94->99.6). `li r0,-1; stw` into a local = `TColor c;`.
- A member template like `TVec3::set<f>` kept out of line in the ROM may need THREE chained one-statement wrappers.
- ROM `extsh.` after `lha` of an s16 member compared to 0: `(s16)(int)member == 0`.
- Float codegen (BossHanachan perform 90.8->96.4): a scalarised local `TVec3 off(200,0,0)` keeps `off.z*s`/`-off.x` as real
  fmuls/fneg (literals fold away); `x*x + z*z` fuses to fmuls+fmadds only when both factors are named f32 locals (the
  second-declared product fuses); ROM fnmsubs = `X - (a*(b*c))`; a clamp on an argument wants a by-value inline helper
  (no extra fmr); `f32& ref = arr[i]->m` gives lfsu/stfs 0(r4) pairs; a pointer local declared AFTER its first value use
  reproduces the ROM's addi/mr CSE.
- A .cpp member the ROM calls out of line but ours auto-inlines: two chained one-statement static inline wrappers at the
  call site (TAnimalBird::init 8.9 -> 100). A length with dot inlined and sqrt out of line: by-value wrapper whose body is
  `sqrtWrapper(v.dot(v))`. Rotate on a temporary: `SMS_Eular2Quat(x).rotate(v, v)` avoids an extra quat copy.
- An uninitialised declaration assigned inside a call argument keeps a named local without counting as a statement for
  the inline limit.
- TMatrix34::concat (GC body fixed 2026-10-15): `a(i,2)*b(2,j) + (a(i,0)*b(0,j) + a(i,1)*b(1,j))`, col 3 = `a(i,3) + (...)`.
  It schedules like the ROM only when both operands are read through SMatrix34C::at() on references (TPosition3f& or
  casts), not through Mtx pointer indexing (TKoopa::calcRootMatrix, TBathtubGrip::calcRootMatrix -> 100).
- `var * literal` puts the literal first in fmuls; the ROM's `rate * 2pi` order needs the literal through an inline
  parameter. `-rad + a*b` gives fneg+fmadds; `a*b - rad` gives fmsubs. Implicit operator= with 7 member copies won't
  inline in deep template code; 6 statements will (join two copies with a comma).
- Data/linking (2026-10-15): string-pool and .rodata order follow INCLUDE order. 53 ROM units (mostly Enemy) carry the
  (0,0,0)/(1,1,1) Vec pair from MapCollisionEntry.hpp right after the InfectiousStrings strings: include
  <Map/MapCollisionEntry.hpp> after the rogue includes. sinit order of MSound statics: MSSetSound.hpp before
  MSoundBGM.hpp. Before flipping a 100% unit to Matching, check include order; scratchpad w8_doldiff.py maps a DOL byte
  difference to its symbol.
- Fused `fmuls y*y; fmuls z*z; fmadds x*x; fadds` on a STACK TVec3 = the local is scalarisable. Our TVec3 copy ctor's
  `*(Vec*)this` cast pins copies to memory (no fusion, y/z reloaded). Per-site: `TVec3 v; XCopyVec(v, expr);` with a
  file-local `static inline void XCopyVec(Vec& d, const Vec& s) { d = s; }`, or plain `Vec` locals. Member/pointer
  vectors never fuse in the ROM either. (A header-wide ctor fix is being evaluated.)
- "normalize() after length()" CSE (bossManta +7.6, fruitsboat +4): `f32 sq = v.squared();` then `TUtil<f32>::sqrt(sq)` for
  each length use and an explicit `if (sq <= epsilon()) v.zero(); else v.scale(one() * W(sq));` where W is two chained
  one-statement wrappers around TUtil::inv_sqrt. A switch default that skips code but still reaches the tail:
  `case N+1: default: goto end;`. In-place quaternion product + `mtx.setQuat(q)` (no product local) matches the ROM slots.

## Out-of-line math inside inlined code (2026-10-16 agents)
- Check the ROM asm for `bl` to `TVec3::add/sub/scale/dot` or `TUtil<f32>::sqrt` where ours inlines: wrap ONLY that call
  site in 3 chained one-statement static inlines (hanasambo checkGenCoin +6.6, elecNokonoko shoot +10.5, Kazekun +5).
  `length()` inlined but its dot/sqrt out of line: wrap each (`Len(v){return Sqrt1(Dot1(v,v));}`), pass the temporary directly.
  Never put the wrappers in a shared accessor (getParam): callers whose ROM inlines it lose (limitkoopa: 10 functions).
- ROM reloads `mOwner`/members after every call where ours caches: the callee .cpp member was inlined; hand-flatten it at
  the call site (Koopa TKoopaFlame::attack_ 94.9 -> 98).
- Extra copy after an out-of-line `sub`: by-value helper that takes the param, mutates, returns it (cannon killerShoot).
- ROM copies one vector twice and reads .x from one, .z from the other: helper `splitXZ(const TVec3&, f32& x, f32& z)` with
  two word-copies through address-exposed temps, kept in a helper so the caller still inlines (chuuhana rolling 91.9 -> 99.8).
- `sqrt(x*x + z*z)` fuses like the ROM only with named f32 locals, second-used declared first.
- int constant converted to float after a call (`li r0,60` after the bl): pointer local first, then `int f = 60;`, then use.
- Before any FAKEMATCH pad: confirm `ai_task.py show` says 100% after a full `python -m ninja` (a smaller diff is not enough).
- Header-inline call (TVec3 copy ctor, `operator*=`) that the ROM calls out of line inside a small helper: make the helper take
  `const TVec3&` and do `TVec3 v(v0); v *= s; return v;` in its body (MarioCollision damageExec 97.7 -> 99.8). A static inline
  wrapper around a single call only helps while the wrapper itself still inlines; otherwise you get `bl Wrapper`.
- `clrlwi` of an s32 index placed AFTER `bl getModel`: the u16 cast lives inside an inline taking the object + s32
  (`NpcAnmMtx(npc, idx) { return npc->getModel()->getAnmMtx((u16)idx); }`); a cast at the call site comes too early
  (NpcEffect setPollutionEffectMtxPtr_ 90.5 -> 99.9). String-pointer locals take r28, r27... in declaration order.
- WARNING (2026-10-17): "register-only" diff lines can hide real logic bugs. cameragc calcPosAndAt_ had
  `fmr {f0},{f1}` vs ours `fmr {f1},{f0}` after a compare: the ROM took min(minCushion, dist*cushion), ours the max,
  which capped the first-person look-up at 33 deg and made the Noki Bay sun warp impossible. When a `~` line swaps
  the operands of fmr/mr/select right after a compare+branch, check which value each path ends up with.
