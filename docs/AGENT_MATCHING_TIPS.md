# MWCC Matching Tips for Agents

This document collects practical knowledge about how the Metrowerks CodeWarrior C++ compiler (MWCC GC/1.2.5) generates PowerPC code. These tips help when iterating on source code to achieve a byte-identical match against the original binary.

Read this document before attempting to match functions. The patterns described here are recurring and will save significant trial-and-error time. Expand the document as needed. Ask for human review whenever the contents of the document disagree with observed reality.

## What NOT to focus on

There are some things that might seem like low hanging fruit at first sight but really are not. They are:

- function stack frame size
- prologue and epilogue shape
- wrong registers being used

These things are usually the hardest ones to get right and so should be left for the very end of matching an entire file, when nothing else remains to be done.
Instead of these, first and foremost wrong instructions being used and ordering of these instructions should be resolved.

## Function definition order matters

Functions with non-weak symbols must be defined in code in the exact order they appear in the target binary.
MWCC never reorders symbols, so this is a rule that must never be broken.

## Do not use reinterpret casts to access fields

Never use raw byte offsets and reinterpret casts to access fields -- the codegen from that almost always differs from using proper fields.
Instead, introduce the relevant fields and use them, or rather even getters/setters if already available.

## MWCC dislikes reordering

The compiler never reorders memory stores, loads and function calls relative to one another.
They always should be performed in source code in the exact order they appear in target assembly.
This a good first place to focus on when matching, as it helps shape the rest of the function.

## MWCC can eliminate redundant reads, but not writes

All writes to memory that happen in code will be reproduced in assembly, and vice-versa, when reconstructing code from assembly, you MUST do repeated redundant stores into memory -- that's just the way the original code was written.

Redundant reads, on the other hand, can be eliminated by MWCC, but not always, e.g. function inlining might inhibit this.

## MWCC is very reluctant to optimize anything that uses stores ints to memory

If a Vec struct that contains 3 floats is copied via it's compiler-generated copy ctor or assignment operator -- it will be compiled to integer loads/stores. This makes the compiler unable to keep the values used in registers and forces it to spill them to stack, even if the surrounding code clearly allows for them to stay in floating point registers.

Same logic applies to a Color struct that contains four 8-bit ints: if the ints are initialized one by one, then the compiler will not be able to optimize it to simple bit manipulations in integral registers and will keep the struct on the stack.

## MWCC 1.2.5 stack padding bugs

Our version of MWCC has a bug where the backend allocates more stack than necessary.

Most commonly this happens when functions were inlined: inlined function calls often inflate the stack frame size. To correctly match a function with stack frame size issues, UNUSED inlines from the MAP need to be reconstructed based on their size, and sometimes new inlines need to be fabricated based on own's best judgement.

Another instance of it is using a ternary operator sometimes taking up more stack than using ifs.

Next, local variables can expand the stack even if they are always stored in a register and never actually spilled to the stack.

When no obviously correct way to make stack frame size match exists, a trick should be used to correctly match the function's context: a temporary char array of required size to inflate the stack. Such hacks however should be removed or commented out after the function is matching to allow for a possible proper solution in the future.

## Reading frame-size differences

The frame is `8 + locals rounded up to 8 + saved GPRs × 4 rounded up to 8 + saved FPRs × 8`. A 4-byte change in locals can therefore move the frame by 8 or by 0. Each inlined call site keeps its own temporaries, sibling branches don't share them, and a loop body counts once. A frame that is too small usually means the original had more inline expansions or deeper inline chains at the same instructions. It rarely means a missing local.

## Ifs

Ifs are always compiled to very simple code:
- compare (`cmpwi`/`cmplwi`/etc, or arithmetic instruction with a dot)
- conditional branch (`beq`/`bne`/`ble`/etc)
- the true block
- unconditional branch to end of false block (`b`)
- the false block

The compiler **NEVER** swaps the order of the true block and false block.
It is also very reluctant in changing the control flow, so C++ control flow usually corresponds to assembly one to one.

Ternary operator is compiled similarly, but it is the one exception to control flow  being the same. In the following case MWCC might initialize the variable's register with the "otherwise" value (zero) instead of doing so in the false branch, which eliminates the false branch entirely.
```
int b = thing == nullptr ? thing->field : 0;
```

## Sequential integer comparisons in a disjunction

Whe MWCC sees code like `if (a == 8 || a == 9 || a == 10)` it can optimize it to be `if (a - 8 <= 2)` sometimes. When the latter pattern is encountered with enums -- it should be reversed into multiple disjuncted equality comparisons.

## Assigning a `&&`/`||` chain to a bool synthesizes intermediate bools

When a multi-term short-circuit expression is **assigned to a `bool` variable**, MWCC materializes its own intermediate boolean temporaries for the chain — it initializes a flag register to `0`, sets it to `1` when a sub-condition passes, and merges flags for each grouping level.
This produces a distinctive shape: several `li rN, 0` up front, then
`li rN, 1` at the deep success points, then `clrlwi.`/`beq` merges.

```cpp
// Target has flag-register merges (r0/r4/r3 = 0, then =1 at success, then merged).
// This form reproduces them:
bool same = (-e <= dx && dx <= e) && (-e <= dy && dy <= e) && (-e <= dz && dz <= e);
if (!same) { ... }

// Nested ifs do NOT — they compile to early-exit branches straight to the body,
// with no intermediate flag bools:
bool same = false;
if (-e <= dx && dx <= e)
    if (-e <= dy && dy <= e)
        if (-e <= dz && dz <= e)
            same = true;
```

When the target shows those `li 0`/`li 1`/merge flags for a compound predicate, spell it as a single `bool x = cond && cond && ...;` assignment (grouped to match the flag nesting), not as nested `if`s.

Concrete case (`MtxUtil.cpp` `TRope::constraintTail`): nested-if near-zero test 89.3%;
`bool same = (-e<=dx&&dx<=e) && ...;` with `<=` and inline `-JGeometry::TUtil<f32>::epsilon()`
→ 99.8%.

## Switches

MWCC can compile switches in one of two ways: jump table or branching.
Jump tables are easily identifiable via `mtctr` and `bctr` instructions being used.
Switches that became branches usually have control flow that doesn't look like an if: multiple conditional branch instructions follow a single comparison instruction. E.g.
```
cmpwi r0, 0x1
beq ...
bge ...
cmpwi r0, 0x0
bge ...
b   ...
... the code block inside the switch ...
```

### Case ordering is strict

MWCC **never reorders cases in a switch statement** during compilation. The order of `case` labels in the source code must match exactly the order they appear in the assembled jump table or branch chain. If a function exhibits a non-matching switch and you've verified the case logic is correct, inspect the target assembly to determine the actual case sequence and reorder the source cases accordingly. Even if semantically equivalent, the compiled output will differ until case order matches.

## Nonsensical control flow

As MWCC inlines functions, sometimes nonsensical control flow will be encountered in the assembly, one that doesn't correspond to any structured control flow constructions like switches, ifs or loops. Such cases are usually explained by **function inlining** rather than gotos. The place where a goto was supposedly used would actually correspond to a return statement, and the place where it points to would be the boundary of the inlined call.

## Keep track of known relevant inlines

Reconstructing correct inline calls is crucial in matching code correctly. When a similar block of code reoccurs -- always consider the possibility that it's an inline, but never disregard the possibility that the original authors simply copy-pasted it. When starting on a new function, explore the inlines already available in the different classes that it uses, as well as in the current translation unit.

UNUSED functions from the MAP must often be reconstructed even when they do not exist as standalone code in the final binary: their inlined bodies still determine caller codegen (register allocation, load offsets, branch layout). Treat their MAP signature and size as constraints, recover a plausible body from repeated callsite patterns, and validate by diffing the caller(s) after each inline-shape change rather than expecting a direct symbol-level match for the UNUSED function itself.

### A getter that repeats before every operation is a wrapper inline

m2c writes an inlined wrapper as one pointer local that is assigned again and again:

```cpp
pBuf = DSPInterface::getDSPHandle(channel->unk0);
pBuf->setPauseFlag(1);
pBuf = DSPInterface::getDSPHandle(channel->unk0);
pBuf->flushChannel();
```

When the same lookup comes back before every single operation, the original source almost always called a wrapper that hides the lookup:

```cpp
DSPInterface::setPauseFlag(channel->unk0, 1);
DSPInterface::flushChannel(channel->unk0);
```

Both forms give the same instructions, but the wrapper form reads correctly and it colours the registers correctly in long functions.
`JAInter::StreamLib::callBack` went from 98.7% to 99.8% on this rewrite alone.
Keep the pointer local only where the code **reads a field** through it.

## Check out-of-line copies before changing an inline

When MWCC declines to inline something, it emits a weak or local copy into the caller's unit, and `mario.MAP` lists it. That copy is the original body. Diff it before changing the inline. If the ROM calls an inline out of line at some sites and inlines it at others, the difference is at those call sites, not in its body.

## Narrowing casts are a hint

A `clrlwi`/`extsh` before a use rarely means the original had a cast. Look first for a parameter, return, local or member type that narrows the value.

## Signedness and width show in the compare

`cmpwi` vs `cmplwi` and `extsb`/`extsh` vs `clrlwi` give the sign and width of a value. Fix the declared type rather than adding casts.

## Reference Locals Affect Register Allocation

Introducing a reference local before accessing struct members can change how the compiler allocates registers:

```cpp
// Direct access — compiler may reload base pointer each time:
unk0[i].mPos.set(...);
unk0[i].mVel.set(0.0f, 0.0f, 0.0f);

// Reference local — compiler keeps base in a register:
Node& node = unk0[i];
node.mPos.set(...);
node.mVel.set(0.0f, 0.0f, 0.0f);
```

Check whether the target reloads the base address or reuses a register. If it reuses, a reference local is likely in the original code.

## Inlined Functions Only Load `this` Once

Whenever a simple implementation loads a pointer into a register multiple times but the original did it once — the original might have used a function that got inlined.

```cpp
// `field` pointer could get loaded twice because
// the compiler assumes function calls can modify any memory
field->nonInlinedFuncCall1();
field->nonInlinedFuncCall2();

// `field` pointer loaded as `this` once
field->inlineFuncThatCallsBoth();

...

class FieldClass {
...
  void inlineFuncThatCallsBoth() {
    nonInlinedFuncCall1();
    nonInlinedFuncCall2();
  }
...
};
```

This is sometimes indistinguishable from a local reference being used.

## Intermediate Variables Heavily Influence Register Allocation

Intermediate variables are not inherently bad. They are a strong codegen lever in MWCC and can change register pressure, load/store timing, and expression scheduling.

Start with the simplest hypothesis, then iterate:
1. First try a fully inlined arithmetic form (no intermediate variables).
2. If that does not match, try decomposed forms with temporaries.
3. If still nonmatching, vary grouping/order (including compound assignments).

Examples (all semantically equivalent, all can compile differently):

```cpp
// Inline form
f32 c0 = (z1 - z0) * (x2 - x1) - (x1 - x0) * (z2 - z1);

// Decomposed form A
f32 dx = x1 - x0;
f32 dz = z1 - z0;
f32 c0 = dz * (x2 - x1) - dx * (z2 - z1);

// Decomposed form B
f32 c0 = (z1 - z0) * (x2 - x1);
c0 -= (x1 - x0) * (z2 - z1);
```

Practical rule: do not treat temporaries as "wrong" or inlining as "always right". Treat decomposition style as an iterative search dimension and test multiple shapes until one matches. When trying different approaches, always try things that look reasonable to have been written by a human first, and only if nothing sensible works start trying crazy and weird approaches.

## Avoid Reference Aliases as a Workaround

When trying to match complex struct access patterns, DO NOT use reference aliases as a general strategy to "improve" codegen. While they can sometimes help with register reuse (see "Reference Locals Affect Register Allocation" above), they frequently harm matching by changing pointer offset calculations and register assignments in ways that deviate from the original.

Only introduce reference aliases when:
1. The original code clearly had them (evidenced by a single load of a base pointer that's reused)
2. Direct member access results in repeated redundant loads that don't appear in the target
3. You are confident from diff analysis that a reference was intended

Otherwise, use direct member access or direct getter calls inline in expressions and let the common subexpression elimination & redundant load optimizations do the rest.

## Reconstructing initializer-heavy functions (init/ctors) from raw asm

When a function body is mostly field assignments (typical for `init()` / constructors), the fastest path is to decode the raw `.s` file directly rather than relying on `m2c` or guessing.

Workflow:

1. Find the function in `build/GMSJ01/asm/<path>.s` by mangled name (`grep -n` for `.fn <mangled>`).
2. Walk the prologue forward and map every `stb`/`sth`/`stw`/`stfs` to a class field using the `/* 0xNN */` offsets in the header. Each store = one source statement.
3. Float and integer constants live at the bottom of the same `.s` file as `.obj "@NNNN"` blocks with `.float`/`.4byte` directives. The assembly references them as `lfs f0, "@NNNN"@sda21(r0)`.
4. String literals (joint names, file paths) are in `.rodata` / `.sdata2` and referenced as `addi r4, r30, 0xNNN` where `r30` is the loaded address of `@1490` (the TU's local rodata base).
5. Cross-reference against the existing partial source — anything in the asm but not in the source is missing.

Compare offsets one-for-one. If the asm writes to offsets `0xFC, 0x100, 0x104` and the source only zeroes `0x108..0x110`, you're missing a `unkFC.zero();` before `unk108.zero();`.

## Recognising common MWCC store idioms

**`u32 → f32` cast** (typical for `(f32)someByte` from a packed struct like `GXColor`):
```
lbz   r0, OFF(rBase)              ; load unsigned byte
lis   r4, 0x4330                  ; high half of magic constant
lfd   f1, "@magic"@sda21(r0)      ; magic = 4503599627370496.0 (0x4330000000000000)
stw   r0, X(r1)                   ; store low half on stack
stw   r4, X-4(r1)                 ; store high half on stack
lfd   f0, X-4(r1)                 ; load as double
fsubs f0, f0, f1                  ; subtract magic → real float value
stfs  f0, FIELD(rThis)
```
Source: `unk84.x = (f32)bodyColor[0].r;` — straight cast of a `u8` field to `f32`.

**`(u8)` cast on an `int`-returning call before storing to a `u16` field**:
```
bl    JUTNameTab::getIndex(...)
clrlwi r0, r3, 24                 ; mask to low 8 bits
sth    r0, FIELD(rThis)           ; store as halfword
```
Source: `mField = (u8)getIndex(...);` even though `mField` is declared `u16`. Without the `(u8)` cast, MWCC emits `sth r3, FIELD(rThis)` directly with no `clrlwi`. This pattern appears repeatedly for joint-index assignments.

## Constant Hoisting and Loop Codegen

The compiler hoists constant loads (`lfs`, `lfd` from SDA/SDA2) before loops. The exact set of constants hoisted depends on:
- Which expressions appear in the loop body
- Which inline functions are called (they may reference additional constants)
- The order of operations within the loop

If the target hoists a constant (e.g., `lfd f28, @5181@sda21`) before a loop but our build does not, it means the compiler sees a different code structure. This is usually a symptom of a deeper structural mismatch in the loop body or inlined functions, not fixable by just moving the load.

## Function-local statics with non-trivial initializers: `init$NNN` is the guard

When a function-local `static` has a non-trivial initializer that has to run at first call — typically a constructor call like `static JGeometry::TVec3<f32> pos(1815.0f, 1500.0f, 1550.0f);` — MWCC emits **two symbols in `.sbss`**:

- The variable itself (e.g. `pos$NNN`), sized for the type (a Vec is 12 bytes).
- A 1-byte construction guard named `init$NNN` (note the literal name `init`, regardless of what your source variable is called).

The function-entry codegen is:

```asm
lbz   r0, init$NNN@sda21       ; load the guard byte
extsb. r0, r0                  ; sign-extend with dot (sets CR)
bne   .L_skip_init             ; already constructed → skip
... initializer code, writes to pos$NNN ...
li    r0, 1
stb   r0, init$NNN@sda21       ; mark constructed
.L_skip_init:
```

So when the target shows an `init$NNN` byte that is loaded with `lbz` + `extsb.` and tested, **don't** model it as a plain `static bool foo;` in your source — model it as the *guard* for some other static local with a real constructor. Pick the source statement that produces the matching initializer body (commonly a `JGeometry::TVec3<f32>(x, y, z)` if the init code writes three floats).

Aggregate-initialized PODs (`static Vec pos = { 1.0f, 2.0f, 3.0f };`) and zero-initialized statics (`static Vec pos;`) do NOT produce a guard — they sit in `.data` / `.bss` with no first-call check. The `init$NNN` pattern only appears when MWCC needs to run constructor code at first entry.

## A zero-argument call of a constructor with default arguments costs one inline pass

MWCC expands inline calls in passes, and each pass permits a smaller callee
(pass 0: no limit, pass 1: 10 statements, pass 2: 7, pass 3: 3, pass 4: none).
A constructor written with default arguments adds one pass to that count when
it is called with **no** arguments:

```cpp
class TGameSequence {
	TGameSequence(u8 stage = 0, u8 scenario = 0, JDrama::TFlagT<u16> flag = 0)
	{
		set(stage, scenario, flag);
	}
};

TGameSequence local;          // synthesized __ct__13TGameSequenceFv, 1 statement,
                              // then the real ctor one pass deeper
TGameSequence local(a, b);    // the real ctor directly, no wrapper
```

The same rule applies to a member of such a type that the enclosing
constructor default-initialises, for example `TFlagT<u16> unkC;` in
`JDrama::TViewObj`. The wrapper has no symbol of its own; it is always
expanded.

How to recognise it: the calls inside a constructor body stay `bl` although
the ladder says they fit, while the same callees are expanded at that depth
elsewhere. `TApplication::TApplication()` keeps `bl TFlagT::TFlagT(const
TFlagT&)` and `bl TFlagT::set(u16)` for each `TGameSequence` member; with a
plain `TGameSequence() { set(0, 0, 0); }` both are expanded and the function
is at 36%. The default-argument form gives 100%. `inline_trace.py` shows the
wrapper as `__ct__XFv [1 stmt]` one pass above the real constructor.

## A member function called on a bare global substitutes `this`; called through an accessor it binds it

When an inline member function is called on a global object written by name,
`gpApplication.setNextArea(x)`, MWCC substitutes `this` with the constant
address, folds the member offsets, and then CSEs `gpApplication + 0x12` into a
callee-saved register:

```
addi r30, r5, 0x12       ; &gpApplication.mNextArea
stb  r0, 0x12(r5)
...
addi r3, r30, 2          ; &mNextArea.unk2
```

When the receiver is an expression that MWCC will not repeat (an inline
accessor call, a local pointer, or a local reference), `this` is bound to a
temporary that holds `&gpApplication` itself, and every offset folds from it:

```
addi r31, r4, gpApplication@l
stb  r0, 0x12(r31)
stb  r0, 0x13(r31)
addi r3, r31, 0x14
```

The second shape, with the global's own address kept across the call, is the
sign that the original went through an accessor such as `SMSGetMSound()` or
`SMSGetMarDirector()`. Nine functions of `System/` show it for
`gpApplication`, and every other `gpApplication` site in the tree compiles to
the same bytes through `SMSGetApplication()`, so the accessor is used
everywhere. Expect the same of the other `SMSGet*` accessors: the bare global
and the accessor differ only where the result feeds another inline call.

The same test tells a getter from a raw field read. A getter such as
`u8 getStage() const { return unk0; }` reads through `this`, so its return
value is force-loaded into a compiler temporary. Two visible effects: the
temporary keeps a 4-byte slot (a frame 4 bytes short per call is the
symptom), and when two such calls are the arguments of one `bl`, MWCC
evaluates them in the reverse order. `TMarDirector::currentStateFinalize`
loads `mCurrArea.unk1` before `unk0` for `endStageEntranceDemo`, which only
the getters reproduce. An accessor that returns a reference leaves no trace
at all, so it can neither be proved nor disproved this way.

## Default arguments vs. spelling them out changes inlining

When a base/member constructor is invoked with a value that happens to be that
constructor's own default argument, prefer the default-arg call form over
repeating the literal. It is not just cosmetic: it can change MWCC's inline
depth decisions.

Concrete case (`BathWaterManager.cpp`): `JDrama::TViewObj`'s ctor defaults its
name to `"<TViewObj>"`. Writing a member's base as `JDrama::TViewObj("<TViewObj>")`
inlined the nested `TNameRef`/`TFlagT` ctors one level too deep; writing it as
`JDrama::TViewObj()` (letting the default supply the identical string) kept those
nested ctors as real `bl` calls, exactly like the target. This took the
enclosing constructor from 90.6% to 100%. If a nested ctor inlines when the
target keeps it as a call (or vice-versa), check whether the original used a
default argument.

## Unsized arrays go to `.data`, sized ones may go to `.sdata`

An array declared with an explicit size that is small enough (≤ the `-sdata`
threshold, default 8 bytes) can be placed in `.sdata` and addressed GP-relative
(`@sda21`, giving an indexed `lwzx` load). An array declared **unsized** — e.g.
`static const char* fileNames[]` with the size deduced from the initializer —
is always placed in `.data` instead, and addressed absolutely (`lis/addi @ha/@l`
+ `add`/`lwz`).

So if a small global array should live in `.data` (target uses `lis/addi`) but
yours lands in `.sdata` (`@sda21`), drop the explicit array bound and let the
initializer size it. Concrete case (`BathWaterManager.cpp`): `fileNames[2]` →
`.sdata` (nonmatching); `fileNames[]` → `.data` (match).

## Local Symbol Mangling: `@unnamed@` vs `static`

MWCC mangles symbols inside anonymous namespaces with an `@unnamed@` prefix.

- If a local symbol's mangled name includes `@unnamed@`, model it as being in an anonymous namespace.
- If the symbol is local but does not include `@unnamed@`, prefer a plain `static` function/variable instead.

## Symbol Order with `-inline deferred`

When a TU is compiled with `-inline deferred` (see TU-specific flags in `configure.py`), define symbols in reverse order relative to the map/symbol listing for that TU.

- In practice, function-definition order in the `.cpp` should be reversed for those TUs.
- If order-sensitive matching drifts for an `-inline deferred` TU, verify definition order before attempting smaller codegen tweaks.

## TVec3 / Vector Codegen Patterns

`JGeometry::TVec3<f32>` is a 12-byte struct with `x`, `y`, `z` float members. How you read/write it drastically affects code generation.

### Use the inlines

`TVec3` and related `JGeometry` types have lots of inlines, and most of the time the original code would have used those.
When naive decompilation of the math doesn't work out, try using the inlines.

### Multiplication by zero/one

MWCC can optimize out multiplication by floating point one or zero written out explicitly in code, but fails to do so when the multiplication comes from an inlined function.
This routinely occurs when an "up" vector is created and then cross-multiplied with another vector -- codegen contains trivial multiplications by zero or one.
When decompiled naively as `1.0f * unk170.z - 0.0f * unk170.y`, MWCC will optimize the multiplications out, but if the "up" vector is properly created and the `cross` inline is used, it will fail to optimize them out, achieving the desired codegen.

### Construction: component-by-component vs constructor

```cpp
// Constructor form — compiler batches all loads, then all stores:
//   lfs f0, ...; lfs f1, ...; lfs f2, ...
//   stfs f0, 0(rN); stfs f1, 4(rN); stfs f2, 8(rN)
JGeometry::TVec3<f32> pos(x, y, z);

// Component-by-component — compiler interleaves load/store pairs:
//   lfs f0, ...; stfs f0, 0(rN)
//   lfs f0, ...; stfs f0, 4(rN)
//   lfs f0, ...; stfs f0, 8(rN)
JGeometry::TVec3<f32> pos;
pos.x = x;
pos.y = y;
pos.z = z;
```

Check the target assembly to see which pattern (batched vs interleaved) is used, and write the source accordingly.

### `Vec3::zero()` vs `Vec3::set(0,0,0)`

`.zero()` emits stores in **reverse component order** (`z`, `y`, `x` → offsets `+8, +4, +0`). `.set(0,0,0)` typically emits forward order. Match the asm order to pick the right call.

### Assignment: `operator=` vs `.set()`

```cpp
// operator= (struct copy) — generates lwz/stw (word load/store):
//   lwz r0, 0(rSrc); stw r0, 0(rDst)
//   lwz r0, 4(rSrc); stw r0, 4(rDst)
//   lwz r0, 8(rSrc); stw r0, 8(rDst)
node.mPos = param_1;

// .set(vec) (float copy) — generates lfs/stfs (float load/store):
//   lfs f0, 0(rSrc); stfs f0, 0(rDst)
//   lfs f0, 4(rSrc); stfs f0, 4(rDst)
//   lfs f0, 8(rSrc); stfs f0, 8(rDst)
node.mPos.set(param_1);

// .set(x, y, z) (3-arg form) — generates lfs/stfs like component assignment
node.mPos.set(expr_x, expr_y, expr_z);
```

The target assembly will clearly show `lwz`/`stw` (integer move) vs `lfs`/`stfs` (float move). Choose the source pattern that matches.

### Constructor with a zero component + `+=` vs direct component init

When building a vector where some components are `0.0f + <expr>` and one is a
real value, the target may show *literal* `0.0` adds on the zero components:
```
lfs f4, @zero ; fadds f4, f4, f0   ; component = 0.0 + src.x
```
This is the signature of the original writing a base vector with literal-zero
components and then adding another vector, e.g.
```cpp
// point2 = (0, -unkC.y, 0) then add unk58 -> emits the "0.0 + unk58.x" adds
JGeometry::TVec3<f32> point2(0.0f, -1.0f * unkC.y, 0.0f);
point2 += data.unk58;
```
Writing the fully-fused form `TVec3(unk58.x, -unkC.y + unk58.y, unk58.z)`
instead reads the components directly with no zero-adds and does not match.
Concrete case (`BathWaterManager.cpp` `calcBathtub` inner-band branch): the
`+=` form recovered the `0.0 + x`/`0.0 + z` adds; see next tip for the `-1.0f *`.

### Negation as `-1.0f * x` (fmuls) vs `-x` (fneg)

`-x` compiles to `fneg`. But the target sometimes shows a standalone
`fmuls f_, f(-1.0), f(x)` (multiply by the `-1.0` SDA constant) where you'd
expect a negation — and crucially kept as a *separate* fmuls, not fused into a
following `fmadds`/`fnmsubs`. This happens when the original literally wrote
`-1.0f * x` (MWCC in does not algebraically fold
`-1.0f * x` -> `fneg x`). Prefer `-1.0f * x` over `-x` when the target shows the
extra `-1.0` load + `fmuls`. Concrete case (`BathWaterManager.cpp`
`calcBathtub`): `-1.0f * unkC.y` matched; `-unkC.y` gave `fneg` and did not.

### Trouble in fnmsubs town

Computation-heavy code commonly has code like `B - A * C` or `A * C - B`  for floating-point A, B, C. E.g. any cross product will have analogous code.
MWCC likes to compile such code into PowerPC `fmsubs` and `fnmsubs` instructions, but they have a quirk in how they are implemented related to double to single precision conversions: technically, fnmsubs is `-float(A * C - B)`, but MWCC uses it for `B - A * C`.
Decompilers like ghidra and m2c like to preserve this technicality and commonly emit code like `-(a * b - c * d)` in code like vector cross products.
Rewriting it as `c * d - a * b`, opening the braces, usually helps matching it better.

## Reading a source array through `void*` defeats CSE (controls loop unroll)

When a loop copies the same source element to several destinations, e.g.
```cpp
TVec3<s16>* src = (TVec3<s16>*)getVtxPosArray();
for (...) { a[i] = src[i]; b[i] = src[i]; c[i] = src[i]; }
```
MWCC's alias analysis can prove `src` (a clean typed pointer) doesn't alias the
`new`-allocated destinations and **CSEs the `src[i]` load** — reading it once and
reusing it for all three stores. That shrinks the loop body, and MWCC's `-O4`
unroller then unrolls it *more* (e.g. ×8). The target often reloads `src[i]` for
every store (no CSE) and unrolls only ×2.

To reproduce the reloads (and the smaller unroll factor), read the source
through a `void*` and cast at each use, which blocks the alias analysis:
```cpp
void* src = getVtxPosArray();
for (...) {
    a[i] = ((TVec3<s16>*)src)[i];
    b[i] = ((TVec3<s16>*)src)[i];
    c[i] = ((TVec3<s16>*)src)[i];
}
```
Concrete case (`DrawUtil.cpp` `TTrembleModelEffect::init`): the typed-`src` form
CSE'd + unrolled ×8 (54%); the `void*`-cast form reloaded + unrolled ×2 (100%).

## `const` on an inline's pointer parameter also defeats CSE

The same effect appears at an inline boundary, and there the fix is one word.
When an inline reads a field through a pointer parameter and the caller reads
the *same* field of the *same* object, MWCC normally merges the two into one
load. Making the parameter point to `const` stops the merge, and both loads
appear.

```cpp
// one load of symbol->mNameOffset, shared with the caller
const char* getSymbolName(TSpcSymbol* symbol);
// two loads: the inline's own, and the caller's
const char* getSymbolName(const TSpcSymbol* symbol);
```

Concrete case (`Strategic/spcinterp.cpp` `TSpcInterp::dump`): the original loads
`symbol->mNameOffset` twice, once inside the inlined `getSymbolName` and once as
a `SpcTrace` argument. Adding `const` took `dump` from 98.8% to 100%, and left
every other caller of `getSymbolName` matching.

This is worth reaching for before restructuring anything: it is a smaller and
far more plausible change than the usual alternatives (hoisting the field into a
local, or dropping the intermediate local altogether), both of which were tried
here and were worse.

## An inline's locals are numbered in reverse when it is inlined

This one explains a whole family of "the out-of-line copy and the inlined copy
want opposite stack layouts" puzzles, so it is worth knowing the mechanism.

`CParser_NewLocalDataObject` **prepends** each new local to the function's
`locals` list, so the list is in reverse declaration order. Two consumers then
walk that list forward:

- `assign_locals_to_memory(locals)` gives out stack offsets, **increasing** —
  so when a function is compiled normally, the **first-declared** local ends up
  at the **highest** offset.
- `CInline_SetupArgsExpression` recreates the callee's locals in the caller,
  walking the packed array forward and prepending each one — which **reverses
  the order a second time**. So in an inlined copy the **last-declared** local
  ends up at the highest offset.

The consequence: any two named locals of an inline function **swap places**
between its out-of-line copy and every site that inlines it. No declaration
order satisfies both. Measured on `TSpcInterp::fetchU32`: `src` first gives the
four inline sites 100% and the out-of-line copy 99.8%; `result` first gives
exactly the reverse.

The escape is not to reorder but to **reduce the function to a single local
that needs a stack home** — with nothing to swap, both layouts agree. A local
whose initialiser carries a type-conversion node gets propagated into a compiler
temporary and stops needing a home, so writing the accessor to return `void*`
and casting at the call site is enough:

```cpp
void* getText(u32 offset);                        // was u8*
u8* src = (u8*)mBinary->getText(mProgramCounter);  // now a temp, not a local
u32 result;                                       // the only named local left
```

That took `fetchU32`, `fetchS32`, `execvar` and `execfunc` to 100% at once
(`spcinterp` 62.2% -> 67.7% matched code). Note it also fixed the two call sites
that fetch *twice*: a second named local was what pushed their temporaries out
of step, not anything about the second fetch itself.

Symptom to recognise: the out-of-line copy of a weak inline and its inline sites
each want the same pair of slots in opposite orders, and both frames are already
the right size.

## `T x = f();` costs one more stack object than `T x; x = f();`

When `f` is inlined, initialising a local from the call builds the return value
in its own temporary and then copies it into `x` — **two** stack objects.
Declaring first and assigning lands the inlined return straight in `x`'s slot —
**one**. Neither spelling changes a single instruction; only the frame moves, so
this is the cheapest knob there is when a frame is off by exactly the size of
one object.

```cpp
ExecFunction f = chooseExecFunction(cmd);   // return temp + copy
ExecFunction f; f = chooseExecFunction(cmd); // return lands in f
```

Both directions have paid off in `Strategic/spcinterp.cpp`:
- `TSpcInterp::update` was 8 bytes too big holding an inlined 12-byte
  pointer-to-member. Declare-then-assign took it to 100%. (Binding a
  `const T&` instead is in between — it drops the copy but adds an address
  register: 96.2%.)
- `TSpcBinary::init` was 8 bytes too *small*. Spelling the inlined `calcKey`
  result as a local — `u32 hash = calcKey(...); symbol->mNameHash = hash;`
  instead of assigning the call directly — added exactly the two missing
  objects and took it to 100%, with `calcAndStoreKeys` still size-exact
  against the map.

So: frame one object too big, look for an initialisation to split; one object
too small, look for a call result that should have been named.

## Reading a value into a local removes a bound temporary

`CInline_SetupArgsExpression` binds an argument to a compiler temporary whenever
the expression is unsafe to repeat — and a call is always unsafe. A read of an
unmodified local is safe, so it gets substituted instead and **no temporary is
created**. When a frame holds one 4-byte temporary too many, moving the argument
into a local is often the whole fix:

```cpp
interp->push((int)interp->pop().typeof());   // binds a temp for the argument
u32 type = interp->pop().typeof();           // no temp; `type` stays in a register
interp->push((int)type);
```

Concrete case (`Strategic/spcinterp.cpp` `spcTypeof`): 96.3% to 100%.

Beware the mirror image: this only helps when the local itself does not need a
slot. If the enclosing function already has a settled set of named locals, the
new local lands in the local region and shifts every offset above it, which
costs more than the temporary saved. The same change in `execadd` dropped it
from 100.0% to 96.6% for exactly that reason.

## Each inline level in a call chain leaves one dead 4-byte temporary

An expression such as `gpFoo->getBar()->baz()` builds one compiler temporary per
inline level that it goes through, and each temporary keeps a stack slot even
when the optimiser removes every instruction that touched it.
The instruction stream is then identical to the ROM's and only the stack offsets
disagree — the classic "our frame is 8 bytes short" symptom.

Two levers add a level, and both are ordinary source that a person would write:

- **Go through the global's inline accessor.**
  `SMSGetMarDirector()->getConsole()` instead of `gpMarDirector->getConsole()`
  adds exactly one temporary. This only works when the *next* member call is
  itself inline; if the accessor result feeds an out-of-line `bl` at once, MWCC
  reads the global in place and no temporary appears.
- **Hold the chain result in a named local.**
  `TGCConsole2* console = SMSGetMarDirector()->getConsole(); console->foo();`
  adds one more temporary than the same expression written in one line, because
  the local is used as the receiver of the following call.

Concrete case (`System/EventWatcher.cpp` `evManiCoinDown`): the bare
`gpMarDirector->getConsole()->startAppearStar()` gave 2 temporaries and a 0x28
frame; the accessor gave 3 and 0x30 with the wrong layout; the accessor plus the
`console` local gave 4 and a 100% match.

Because the effect depends on what follows the accessor, do not convert a whole
file blindly — measure each site. In the same file `evSetNextStage` is
measurably *worse* with the accessor, which is real evidence that the original
used the bare global there.

## A by-value class parameter forces the copy through memory

When a class is passed by value, MWCC materialises the copy and reads the copy
back, even for a two-word POD:

```
lwz  r4, 0(r3)        ; source.mType stays in a register
lwz  r0, 4(r3)
stw  r0, 0x38(r1)     ; source.mData
stw  r4, 0x3c(r1)     ; copy.mType
lwz  r0, 0x38(r1)
stw  r0, 0x40(r1)     ; copy.mData
lwz  r0, 0x3c(r1)     ; the copy is read back from memory
```

Two stack objects, and the second field never gets forwarded through the
register. Seeing this shape in the target is a reliable sign that the value goes
into a by-value parameter — in `EventWatcher.cpp` that parameter is
`getNameRefPtr(TSpcSlice)`. A named local initialised from the same call gives
two objects as well, but forwards the first field through a register, so the two
shapes are easy to tell apart.

## Working with JSUMemoryInputStream

Practice shows that most of the time in game code `operator>>` overloads were used for reading from them, and sometimes they were chained together.
This fact was derived from stack frame padding issues in various `JDRama::TNameRef::load` overloads -- only by using `operator>>` and sometimes chaining them (e.g. `stream >> vec.x >> vec.y >> vec.z;`) does stack the stack frame size matches the original.
However, methods like `stream.readU16()` were also occasionally used, but codegen differs a bit when it is used and so judgement must be made on a case-by-case basis.
Doing raw `stream.read(&someInt, 4);` calls is the least likely option, because humans are lazy and wouldn't want to specify the size manually -- always prefer avoiding it.

## Forcing local arrays onto the stack with unrolled loops

Small local arrays (e.g. `f32 dist[3]`, `f32 m[3][2]`) as well as structures are prime candidates for MWCC's *scalar replacement of aggregates* — it promotes the elements into FP or GP registers, drops the stack storage entirely, and is then free to reorder/fuse the element computations.
If the target keeps such an array on the stack (you see `stfs`/`lfs` of consecutive slots, computed in a strict order), your straight-line, element-by-element source will usually mismatch because yours got scalarized.

Two ways to force the array back onto the stack:

1. `(void)&arr;` — taking the address marks it address-taken so it must live in
   memory. Works, but it's an obviously-fake artifact.
   Still useful as an intermediate step in figuring out the other details of the function to then replace with a proper match.
2. **Write the init/compute as a `for` loop that MWCC unrolls.** Iterating over
   the array with an index the compiler can fully unroll (small constant trip
   count) makes MWCC materialize the array on the stack and emit the element
   stores/loads in loop order — no address-of hack, and it reads like plausible
   original code. This also tends to fix the *ordering* of the stores, not just
   their presence.

```cpp
// scalarized into registers + reordered (mismatch), or needs (void)&:
f32 dist[3];
dist[0] = unk24[0]; dist[1] = unk24[1]; dist[2] = unk24[2];

// unrolled loop: array is spilled to the stack in order (match):
f32 dist[3];
for (int i = 0; i < 3; ++i)
    dist[i] = unk24[i];
```

Concrete case (`DrawUtil.cpp` `TSilhouette::loadAfter`, a Cramer's-rule solve
over `f32 atten[3]`/`dist[3]`/`m[3][2]`): element-by-element assignment needed
three `(void)&` hacks and still only reached ~92% (residual store scheduling);
rewriting the `dist` copy and the `m` fill as small unrolled `for` loops removed
the hacks and took it to 99.7%. Prefer the loop form — it's both cleaner and a
better structural match than address-of forcing.

## Triviality of a type influences codegen

A user-declared destructor (even an empty `~T() {}`) makes a class non-trivial, and MWCC pins non-trivial types to memory instead of promoting them into registers.
Triviality propagates: a non-trivial member/base makes the enclosing class non-trivial too.

Two symptoms of a *missing* destructor, both meaning "the target keeps this in memory but our build cached it in a register":

- **Struct fields reload across calls in the target, but ours caches them.**
  A trivial struct is scalar-replaced (fields live in registers across calls); a non-trivial one is reloaded from memory after any opaque call.
- **The target spills a freshly-`new`'d object pointer to the stack and reloads it as `this`, but ours keeps it in a register.**
  For `new T()` with a non-trivial `T`, the constructed object is a separate address-taken temporary with a stack home, so `this` is reloaded from the stack during construction.
  Only visible when the constructor is inlined (out-of-line ctors call a real `bl` and never show it).

Fix: give the smallest offending value/helper type an empty `~T() {}` and re-check.
This is a global change, so re-run the baseline — one destructor can fix (or shift) many callsites at once.
Concrete case: adding `~TMsRange<f32>()` (a field of `TSmallEnemyParams`) took several `TFooManager::load` functions from ~95% to 100%.

## A `theNerve()` compare that calls the nerve ctor out of line: chain inline bool helpers

Target shape (BathtubKiller's dying check): the first `==` term has `theNerve()` inlined (the guard) but
`bl TNerveBase<T>::TNerveBase()` out of line; the second term is `bl X::theNerve()` followed by
`subf / cntlzw / extrwi. / bne` (a bool materialised as a value).
Writing the `||` chain directly in the function inlines the first `theNerve()` together with its ctor and compares with `cmplw`.
What reproduces it exactly is one more inline level, as header inlines (weak, fully inlined, so absent from the map):

```cpp
bool isExploding() const { return mSpine->getCurrentNerve() == &TNerveBathtubKillerExplode::theNerve(); }
bool isBreaking() const { return mSpine->getCurrentNerve() == &TNerveBathtubKillerBreak::theNerve(); }
bool isDyingNerve() const { return isExploding() || isBreaking(); }
```

The extra level pushes the ctor one pass deeper (out of line) and the bool-returning helper gives the `cntlzw`.
Took four BathtubKiller functions from 62-82% to 100%.
Related: `bool dying = isDyingNerve(); if (!dying)` costs exactly one 8-byte frame slot compared with `if (!isDyingNerve())` when the helper is inlined; some functions need the named form, most the direct one.

## A missing `__sinit_<unit>_cpp` means missing rogue includes

When `decomp-diff.py -s missing` lists the unit's static initializer (riccohook: 764 bytes, 20 points of the unit),
the fix is the usual rogue includes `<MSound/MSSetSound.hpp>` and `<MSound/MSoundBGM.hpp>` (their static JSUList ctors), not any code.
Check `-s missing` first on every unit.

## A constant array index through a float local keeps `fctiwz`

`f32 idx = t * 4.0f; return &unk0[idx];` reproduces the target's `lfs 4.0; fctiwz` for a constant index;
`(int)(t * 4.0f)`, `idx * 0.25f * 4.0f` and const references all fold the conversion away.
Operand order of the multiply follows the source: `i / 4.0f` emits `fmuls var, 0.25` (variable first), `i * 0.25f` puts the constant first.

## Not solved yet (recorded so nobody repeats the experiments)

- `JGeometry::TQuat4::rotate(const TVec3&, TVec3&)`: the target inlines it one level deep with `bl TVec4::TVec4()` and `bl TVec3::set<f>(f,f,f)` out of line and `q` on the stack (fireWanwan doAdjustTarget, TabePuku's Drag nerve, ~86% functions); the JGQuat4.hpp body is too large to inline at that depth and scalarises `q` at depth 0. Needs a shorter body in libs (a human decision).
- Class-template / member-template helpers stop inlining one pass earlier than ordinary inlines: the target keeps `ArrayWrapper<T>::size()/operator[] const`, `TVec3::set<f>(...)`, `TVec4::TVec4()` out of line at depth >= 1 while ours inlines them (limitkoopa perform, wireTrap initWire, fireWanwan TailHit::perform).
- `updateTimers`-style address-taken decrement (`addi r4, this, off` ... `stw r0, 0(r4)`; BathtubKiller, TTinKoopa, CameraDemo): `int&`, `int*`, local-pointer, `(*p)--` and `v - 1` forms in a static inline helper all fail to produce it.
- Manager `load`/`loadAfter` with a dead `lwz unk38; cmplwi r0, 0` and a much bigger frame (BathtubKillerManager, KoopaJrSubmarineManager): an unidentified inline reading the params pointer.

## A two-return bool inline stays out of line inside an inlined `&&`

`bool TMario::checkStatusType(u32 flag) const { if (mStatus & flag) return true; return false; }` compiles to the same
instructions as the ternary form when inlined, but when it sits in the right-hand side of an `&&` inside a function
that is itself inlined, MWCC emits a weak copy and calls it (the ROM has `checkStatusType__6TMarioCFl` in cameragc);
the single-statement ternary form is inlined there. The two-return spelling took `isMarioCrabWalk_` /
`isMarioAimWithGun_` from 53% to 99.8% without changing any other user.

## A member passed as an inline argument is re-read after every call

`helper(unk154, unkE0, unk170)` with `helper` inlined: the ROM reloads `unk170` after each call inside the helper
(the argument is substituted, not bound to a temporary), while a local passed the same way stays in a register.
Use this to tell "member passed" from "local passed" (GCConsole2 countShine).

## Copy-initialisation vs assignment of a struct local

`JUTRect bounds = a;` calls `bl JUTRect::copy`; a later `bounds = b;` is an inlined 4-word copy. The ROM reusing
the same stack slots across two sites means one shared local assigned twice, not two inline expansions
(countBlueCoin 76% -> 96%). Likewise a dead `JUTRect rect;` at a block top shows as `bl JUTRect::set(0,0,0,0)`
with no use: a default-constructed local that was never used.

## Spell a recomputed expression differently

The ROM recomputes `x - (x / 10) * 10` instead of reusing an earlier `x % 10`; the same instructions, but spelling
the second use differently stops MWCC from sharing the first.

## Creator (MarNameRefGen_*) units

- Rogue-include order reproduces the rodata layout: `DummyStrings.hpp`, then `cDirtyFileName` / `cDirtyTexName`,
  then `InfectiousStrings.hpp` (BossEnemy); MarNameRefGen.cpp itself has the dirty names after the mtx-calc names.
- A `new T` whose ctor is a header inline with a default name argument, plus a `TVec3` member initialised in the
  mem-initializer list (`m(0.0f, 0.0f, 0.0f)`), leaves `TVec3::set<f>(f,f,f)` as the local out-of-line copy the
  map lists; `m.set(0,0,0)` in the body inlines it. The same depth rule keeps `dot` / `scale` out of line when
  `normalize()` is called from one extra inline level (MapObjPlane calcNrm: a static inline
  `calcPlaneNormal(out, c, a, b)` helper).
- Constructors that appear nowhere in mario.MAP are header inlines; the US compiler also does not emit the weak
  dtors the JP map lists in these units (ours do, as harmless "extra" symbols).
- Two accessor calls in one expression (`a->getB().z - a->getC().z`) evaluate the right operand first; two local
  const references evaluate left to right. Match the ROM's load order with references.
- `#pragma inline_max_total_size` is ignored by GC/1.2.5.

## Not solved yet, part 2 (per-caller inline budget)

In the same TU the ROM inlines a small helper into one small caller but calls it out of line from a much larger
function that still inlines other tiny helpers later on (NpcBase: `calcRootMatrix` inlines `execMotionBlend_`, the
14 KB `perform` calls it; the TTelesaSlot ctor; `TAnimalBird::initParams`; cameragc/MapMirror/Guide/BathWater
`perform`s keep `TVec3::set<f>`, `dot`, `scaleAdd`, `TUtil::one`, `MsSqrtf`, `MsClamp<f>`, `setPaneAlpha` as `bl`
inside an inlined body). `#pragma dont_inline` on the helper fixes the big caller and breaks the small ones, so it is
not the answer; compiling without the PCH changes nothing. This one mechanism is most of the remaining gap in
cameragc::perform (83%), MapMirror::perform (54%), Guide::perform (78%), BathWaterManager::throwMario (77%) and
NpcBase::perform (64%). Not a fakematch candidate; leave such functions alone until the trigger is understood.

## Measured inline pass limits (MWCC 1.2.5, `-O4,p -inline auto,deferred`)

Measured with scratch TUs compiled with the project flags (Koopa / bosstelesa work, 2026-10-14):

- A callee is inlined at pass 1 (directly in the function) when it has at most 9 statements, at pass 2 (inside an
  inlined body) when it has at most 6, at pass 3 only when it has 1, never at pass 4.
- Nested inline bodies do not count toward the caller's statement count. `return;` and `else {}` do not count;
  `x = x;` and declarations with initialisers do.
- Auto-inlined (non-`inline`) functions start at pass 0 like explicit inlines. Virtual callers, default arguments on
  ordinary functions, the PCH and source order (callee after caller) make no difference.
- A function containing a loop is inlined at pass 0 but refused at pass 1 (TTelesaSlot::getSlotResult).
- Non-`inline` template functions are never inlined (`template<class T> T CLBSquared(T v)` stays `bl` everywhere, as
  in the ROM); `inline` templates and class-template members inline normally.
- `#pragma inline_max_total_size` / `inline_max_size` are silently ignored.

Use this to steer depth: padding a helper to ten statements makes it inline at pass 0 but not pass 1 (TKoopa::changeAnm
matches that way, at the cost of one nerve's frame: see the TODO there). A fabricated one-statement wrapper adds one
pass: `TKoopa::pushNerve(n)` around `mSpine->pushNerve(n)` gives `bl pushNerve` where `stagger` is inlined into the
parts' `receiveMessage`, while the standalone functions still inline it; two static inline levels put `TVec3::set` at
the pass where MWCC refuses it (`KoopaEmitFlameEffects`). Several ROM sites still behave as if an auto-inlined member
were one pass deeper than ours (checkMarioWhichSide, isCanWalk, getSlotResult): the mechanism is not known; the
wrappers reproduce it.

## Other findings from the boss units

- Nerve comparisons: `&TNerveX::theNerve() == mSpine->getCurrentNerve()` is the operand order the ROM compiles
  (`cmplw r4(nerve), r0(cur)`).
- An unsigned clamp with a dead lower test (`cmplwi r0,0; bge; li r0,0`): `if (a < 0)` on a u8/u32 is folded at parse
  time, but variable bounds through an inline (`MsClamp<u32>(a, 0, 254)`) keep it. Only at pass <= 1 (deeper, MsClamp
  is called out of line).
- Building without the PCH changes only `.rodata` offsets (string addresses shift by 0x18), never code.
- A member of type `TMtx34f` whose ROM ctor is `bl TMatrix34::TMatrix34()` out of line inside a creator is really a
  `TPosition3f` (one more ctor nesting level), as TKiller has.
- A velocity built component by component into a local, then assigned (`mVelocity = velocity;`), and
  `TVec3 v(0, 2, 10); e->mVelocity = v;` instead of `.set(...)` are what the ROM's TBubble code compiles to.

## Timer reloaded after its compare: read it through a const getter

ROM shape `lwz r0,X; cmpwi r0,0; ble; lwz r3,X; subi; stw` for `if (t > 0) --t;` (wireTrap, Kukku; fireWanwan
moveObject and Amenbo control have it too). A load through a `const T*` is not CSE'd with a load through `T*`:
`if (getTimer() > 0) --mTimer;` with `int getTimer() const` reproduces it; non-const or `int&` helpers and a plain
`if` share the load.

## More small codegen levers (2026-10-15)

- `case 3: default:` sharing a block moves the switch compare tree's pivot (fruitsboat createModelData 100%).
- A named local holding a product blocks fmadds fusion: `f32 d = 0.1f * (a - b); x += d;` gives fmuls + fadds,
  `x += 0.1f * (a - b)` gives fmadds.
- fmuls operand order follows the operand kind: a register local is emitted first in either spelling; reading the
  value through its getter (`getParam()->flameRadius.get()`) puts it second.
- A `switch` on `getActorType()` with one case has the signed `cmpw` + `beq/b` shape an `if` does not.
- Not solved: the ROM CSEs `squared()` between `length()` and `normalize()` (libs JGVec3 `normalize()` goes through
  `setLength(const TVec3&, f32)`, a human libs decision); by-value TVec3 return copied straight to the destination
  (`mPositionDelta = calcMomentum()`); `mr` vs the ROM's `addi rD, r3, 0` for parameter copies.

## Vector and matrix idioms the ROM compiles (2026-10-15, Yoshi / BeeHive / MapObjLib / Tongue)

- A "distance < x" test on a difference is `JGeometry::TVec3<f32>(a - b).length()` (copy, inline fsubs, second
  copy, `bl sqrt`); a named `rem = a - b; rem.length()` gives `bl sub` and an inlined sqrt instead.
- `TUtil<f32>::epsilon()` passed straight into an inline parameter is constant-folded; a named
  `f32 eps = TUtil<f32>::epsilon();` keeps the ROM's runtime `fneg`.
- A `TPosition3f` / `Mtx` local read back element-wise (`at()`) is scalar-replaced and its identity stores dropped;
  reading it through its `MtxPtr` conversion (`MtxPtr m = rotation;`) keeps it on the stack like the ROM.
- Several `MsMtxSetRot*` calls on one local `Mtx` share the jma table globals in ours; writing through a `MtxPtr`
  variable makes MWCC reload them per call, as the ROM does.
- `TQuat4::getZDir` is the inline behind "2*(x*z)+2*(w*y), ..." triples followed by `scale(1.0f)`.
- A two-return bool (`isMarioThrough()`, `isWaterSurface()`) inside a `||` in a static inline helper stays `bl`, as
  in the ROM (same rule as checkStatusType).
- Not solved: in `mPosition = base + d * f` the ROM expands `operator*` one inline pass deeper than `operator+`
  (its by-value copy ctor and `operator*=` are `bl`), and `operator-` inside `(a - b) * f` one deeper still; our
  JGVec3.hpp friend operators expand every operator of an expression at the same pass. Probably needs a libs change
  (human decision); it is the whole remaining gap in Tongue::movement and MarioSpecial's wire* functions.

## Frame-size accounting: what each construct costs on the stack (measured)

Measured with scratch TUs compiled with the project flags (frame work, 2026-10-14; the TUs are
small functions over the real `JGeometry::TVec3<f32>`, `TMsRange`, `TPathNode`, `JUTRect`). A frame
is 8 (back chain + lr) + locals + saved registers, rounded up to 8. "dead" means slots that no
instruction ever touches; the ROM keeps them, so our frame has to produce them with real source,
never with padding. Everything below was reproducible on the first try, so treat the numbers as rules.

### A. Objects that get a real (used) slot

| construct | bytes used | dead | note |
|---|---|---|---|
| `TVec3 v = src;` named local | 12 | 0 | word copy `stw x3` |
| `dst = TVec3(a, b, c);` (ctor temp + `operator=`) | 12 | 0 | `stfs x3` then `lwz/stw x3`; `dst.set(a,b,c)` costs 0 |
| by-value `TVec3` parameter of an inlined helper | 12 | 0 | word copy, read back from memory (see "by-value class parameter") |
| by-value non-trivial 8-byte struct (has `~T(){}`) to an inlined helper | 8 | 0 | |
| `dst = o->getPos();` (inline getter returning `TVec3` by value) | 12 | 0 | |
| `sink(o->getPos());` (same getter, result bound to a `const&` of a `bl`) | 12 | 8 | frame 0x20 |
| `const TVec3& p = o->getPos(); sink(p);` | 12 | 12 | frame 0x20; consumed inline instead of by a `bl`: 12 + 16 (0x28) |
| `static inline TVec3 make() { TVec3 v; v.set(..); return v; }` then `dst = make();` | 24 | 4 | callee local + return object, no NRVO (0x28) |
| `sink(make());` | 24 | 16 | 0x30 |
| `return TVec3(a, b, c);` inside the inline helper, assigned | 12 | 4 | constructed straight into the return object (0x20 with r31) |
| `dst = a * k;` (`friend TVec3 operator*(TVec3 fst, f32)`) | 24 | 8 | param copy + return object; `scale` stays a `bl` (pass 3) -> r31 saved; 0x30 |
| `sink(a * k);` | 24 | 24 | 0x38 |
| `TVec3 r = a * k; sink(r);` | 36 | 0 | named local added, dead gone (0x30) |
| `dst = a + b;` (`operator+/-` return a reference, `JG_VEC3_BINOP_RESULT`) | 12 | 24 | 0x30 with r31 |
| `sink(a - b);` | 12 | 12 | 0x20 |
| `TVec3(a - b).length()` | 24 | 20 | 0x38; `TVec3 d; d.sub(a, b); d.length()` is fully scalarised: frame 0x8 |
| `TMsRange<f32>(lo, hi).rand()` temp (non-trivial type) | 8 + 8 (int->float conv) | 8 | 0x28 incl. f31; a named `TMsRange r(lo, hi)` gives the identical layout |
| trivial 2-float temp used as `this` of an inline method (`TrivRange(0,20).width()`) | 8 | 12 | 0x20 (!) |
| same struct with an empty `~T() {}` | 8 | 0 | 0x10 — the destructor removes the dead copy (see "Triviality") |
| named trivial 2-float local, inline method | 0 | 0 | scalarised, frame 0x8 |
| `TPathNode(actor)` temp passed to an inline `const TPathNode&` | 16 | 12 | 0x28; by value: 16 + 8 (0x20); a named `TPathNode n(actor)` is identical to the temp |
| `*r = JUTRect(1, 2, 3, 4);` | 16 | 16 | 0x28 |
| `f32 e = o->pos.x;` kept in an FPR | 0 | 0 | but see B |

### B. Dead 4-byte temporaries of inline call chains (the "frame is 8/12 bytes short" case)

They only exist when the function already has at least one stack-resident object: a function whose
locals all live in registers stays at frame 0x8 whatever it inlines (`sinkf(1); sinki(gObj->getX())`
is 0x8). Once there is one 12-byte local (`TVec3 v = o->pos; sink(v);`, frame 0x18), measured extra:

- 1 inline getter level (`gObj->getX()`) feeding a `bl`: +8 (0x20). 2 levels (`gHolder->getObj()->getX()`):
  +20 (0x30). 3 levels: +32 (0x38). So 8 for the first level and 12 for each further level.
- The last link being a plain field (`...->getObj()->pad[0]`) does not count as a level (3 links = +20).
- Result stored to a field instead of passed to a `bl`: +8 more (1 level 16, 2 levels 28).
- Chain written before the local in source order: +4 (1 level 12, 2 levels 28).
- Chain held in a named pointer local (`Obj* ob = ...; ob->pad[0]`): 3 links = +24.
- Going through the global's inline accessor (`getWorld()->...` instead of `gWorld->...`): +8.
- An `f32` local in an FPR plus one inline getter: 16 dead (0x20 with f31) — register locals count as
  "has a frame" for this rule as soon as an FPR save is needed.

### C. Where MWCC puts things (high address -> low)

saved GPR/FPR block; 8-byte int->float conversion temps; the function's *named* locals in declaration
order (first declared = highest), regardless of block nesting; then caller-side temporaries in source
order; then the objects of inlined callees: return object above by-value parameter copy above the
callee's own locals; dead temporaries lowest (just above the 8-byte header). Nesting the code in
`if/else` blocks does not change the order (flat and nested versions gave identical layouts).

### D. Reading the ROM to find the missing construct

1. Collect every `(r1)` offset the ROM touches between the prologue and the saved registers and do the
   same for ours (`decomp-diff.py -d <fn> | grep '(r1)'`). Classify each object by its access shape:
   `stw x3` from another address = `TVec3` copy ctor / `operator=` / by-value param; `stfs x3` of
   constants = `set(a,b,c)` or a `TVec3(a,b,c)` ctor temp; `lfs`+`stfs` pairs = `set(const Vec&)` /
   `TVec3(const Vec&)` (element copy); `lis 0x4330` + `stw` pair + `lfd` = int->float conversion;
   `stw`+`lwz` back on the same 4 bytes = a by-value class parameter.
2. An object the ROM has and we lack: add the construct from table A that produces that shape
   (e.g. ROM word-copies the source into X, element-copies X into Y, scales Y in place and
   word-copies Y into the member: `TVec3 a = src; TVec3 b; b.set(a); b.scale(k); dst = b;` —
   bosstelesa `TNerveBubbleLive` 92.5% -> 98.1%).
3. Same objects, different order: use C. An object that is low in the ROM but high in ours is
   callee-side there (by-value parameter / inlined helper local) and a named local here, or vice versa.
4. Remaining untouched gap: 8/12/20/32 = inline getter chain (B); 24 next to a `TVec3` copy = a friend
   operator (by-value `fst`) whose result feeds a `bl`; 12 after a trivial 8-byte temp = the type
   needs (or must lose) its empty destructor; 16 after a `TPathNode`/`JUTRect` temp = that temp was
   passed by value / assigned via a temp.
5. If the gap cannot be assigned to a construct, record the function as frame-only in
   `tools/match_tried.txt` with the byte count; never pad.
