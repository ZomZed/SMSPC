# Aurora build of the PC port

Aurora (MIT, https://github.com/encounter/aurora) provides GX, GD, VI, PAD, CARD, AR/ARQ, mtx,
THP decode and the OS arena/alloc/time on SDL3 + Dawn (WebGPU).
The port links against the copy built at `port_deps/aurora/build`
(CMake + Ninja, MSVC, x64, RelWithDebInfo, `-DAURORA_DAWN_PROVIDER=package -DAURORA_ENABLE_DVD=OFF`).
No Aurora sources are patched.

## Building

```
python port/tools/build_aurora.py --stubs -j 20   # first time / after the symbol set changes
python port/tools/build_aurora.py -j 20           # incremental
python port/tools/build_aurora.py --run --timeout 60   # build, then start the exe (killed after 60 s)
```

`--run` starts the exe in `port/build_aurora` with `SMS_PORT_DISC` defaulting to `../../dolphin_test/files`
(the extracted disc next to the workers), so nothing Aurora writes (pipeline cache, ...) lands in the disc folder.

Output: `port/build_aurora/sms_port.exe`, with `webgpu_dawn.dll`, `dxcompiler.dll`, `dxil.dll`,
`SDL3.dll`, `libpng16.dll` and `zlib1.dll` copied next to it.
Logs: `build.log` (compile), `link.log`, `compile_failures.txt`, `undefined.txt`.

How the script works:
- Units come from `census.units()`; flags are its own (x86_64-pc-windows-msvc, C++17,
  `-fms-runtime-lib=dll` to match Aurora's `/MD`), with `AURORA`, `TARGET_PC` and `SMS_PORT_AURORA` defined.
- Include order: `port/include`, `include`, **`port/include_aurora`**, **Aurora `include/`**,
  `libs/dolphin/include`, the other middleware includes.
  So `<dolphin/*.h>` resolves to Aurora's headers; ours are only reached for headers Aurora does not have
  (for example `dolphin/gd/GDVert.h`).
- Game/JSystem units get `port/include/port_compat.h` and then `port/include_aurora/port_aurora.h` force-included.
- `port/src/*.c*` is compiled too, except the files in `PORT_SRC_SKIP` (see "SDK split" below).
  `port_main.cpp` and `port_aurora_sdk.cpp` are wrapped in `#ifdef SMS_PORT_AURORA`, so they compile to nothing in `build_port.py`.
- `sdk_os_thread.cpp`, `sdk_dvd.cpp` and `sdk_osalloc.cpp` (`PORT_SRC_SDK_HEADERS`) are compiled with our `libs/dolphin/include`
  instead of Aurora's headers: they were written against our declarations (`long` vs `s32`, non-const `char*`, ...).
  Only signatures differ (extern "C", same x64 ABI); the struct layouts they touch were checked to be identical in both header sets
  (Aurora's `OSThread` only adds `error`/`specific[2]` at the end, past every field used).
- Aurora components never built in `port_deps` (`aurora_os`, `pad`, `si`, `mtx`, `gd`, `card`, `thp`, `ms`)
  are compiled by the script from the exact `cl.exe` command lines in Aurora's `build.ninja`
  into `port/build_aurora/aurora/*.lib` (nothing is written to `port_deps`).
- Link: `lld-link` in a cached vcvars64 environment, with the Aurora component libs plus everything the `simple` example links
  (`aurora_core/gx/vi/main`, abseil, fmt, xxhash, sqlite3, Tracy, zstd, imgui, freetype, zlib, libpng,
  `SDL3.lib`, `webgpu_dawn.lib`, and the Windows system libs).
- Units that fail to compile are left out of the link.
  `--stubs` turns every undefined symbol into a stub in `port/build_aurora/gen/stubs_aurora.cpp`:
  functions log `[port] missing: NAME` once and return 0; MSVC-mangled data symbols get 256 zero bytes.

## SDK split (which implementation provides what)

| Area | Implementation in the Aurora build | Not used |
|------|------------------------------------|----------|
| OS threads, scheduler, interrupts, messages, mutex, cond, alarms | `sdk_os_thread.cpp` (w5) | Aurora has none |
| OSInit, main RAM, boot info/low-memory globals, arena, time, stopwatch, calendar, reset, sound mode, `OSReport`/`OSVReport`/`OSPanic` | `sdk_os_misc.cpp` (w5; `SMS_PORT_AURORA` block defines Aurora's `OSBaseAddress`/`MEM1Start`/`MEM1End`) | Aurora `OSInit.cpp`, `OSMemory.cpp`, `OSArena.cpp`, `OSBootInfo.cpp`, `OSTime.cpp` (never pulled from `aurora_os.lib`) |
| `OSFatal` | `port_aurora_sdk.cpp` | |
| OS address conversion (`OSPhysicalToCached` ...) | Aurora `OSAddress.cpp` (works on our `MEM1Start`) | |
| Caches (`DC*`/`IC*`) | `sdk_os_misc.cpp` (`DCZeroRange` rounds to 32-byte lines like hardware) | Aurora `OSCache.cpp` (only pulled if `LC*` were used; the game doesn't) |
| OSAlloc heap + IPL ROM font queries | `sdk_osalloc.cpp` (Aurora has no ROM font) | Aurora `OSAlloc.cpp` (never pulled) |
| DVD | `sdk_dvd.cpp`, folder based | Aurora DVD (built with `AURORA_ENABLE_DVD=OFF`) |
| AR/ARQ/AI/DSP (fake JAudio DSP) | `sdk_audio.cpp` (w5) with **our** `arq.h`/`ai.h` (shims in `include_aurora/dolphin/`: the 64-bit `ARQRequest` is 56 bytes, Aurora's 40) | Aurora `AR.cpp` (never pulled) |
| VI | Aurora `vi.cpp` (`VIInit`, `VIConfigure`, `VIFlush`, window) + `port_main.cpp` (retrace, wait, callbacks, frame buffers) | `sdk_vi_pad_card.cpp` (skipped) |
| PAD, SI, CARD | Aurora | `sdk_vi_pad_card.cpp` (skipped) |
| GX, GD, mtx, THP decode | Aurora (+ `port_aurora_sdk.cpp` for what Aurora leaves out) | |
| `sinf`/`cosf`, `__GDCurrentDL` | x64 UCRT, Aurora gd | `sdk_misc.c` (skipped) |
| Crash reporter | `port_crash.cpp` | |
| THPPlayer (state machine, decode thread, YUV draw) | `sdk_thp.cpp` + `port_thp_decode.cpp` (own baseline-JPEG decoder; see BOOTLOG "THP decode") | Aurora `THPDec.cpp`/`THPAudio.cpp` (`aurora_thp.lib` is built but nothing references it) |

"Never pulled" means the port defines every symbol of that Aurora object that is referenced, so lld-link never loads it from the static library;
if one of them is pulled again later it shows up as a duplicate-symbol link error, not as silent double behaviour.

## RAM model

One model, w5's: `sdk_os_misc.cpp` maps 32 MiB of main RAM (`PORT_MRAM_SIZE`) at **0x80000000** (mirrored at 0xC0000000) from a static initialiser
that runs before any game constructor, and backs the 0xCC000000 register window with scratch memory (direct GX FIFO writes at 0xCC008000 land there).
- Aurora cannot be handed a MEM1 pointer (`AuroraOSInitMemory` allocates it itself, with `calloc` in release builds), but its `OSMemory.cpp` object is
  replaceable at link time: `sdk_os_misc.cpp` defines `OSBaseAddress`, `MEM1Start` and `MEM1End` (= our mapping) and `OSGetPhysicalMemSize`,
  so Aurora's `OSAddress.cpp` and its headers' `OS_BASE_CACHED` / `__OSBusClock` (read through `OSBaseAddress`) all use the 0x80000000 RAM.
- `OSInit` (called by `port_main.cpp` like `__start` would) is w5's; the arena is 0x80004020..0x82000000, the boot info says 24 MiB (0x01800000) like a retail console.
- `config.mem1Size` is set to `PORT_MRAM_SIZE` only for Aurora's `OSAddress` asserts.
- Everything the game allocates from JKR heaps is below 4 GB, so 32-bit audio/ARQ addresses and ResTIMG/J3D deltas stay valid, and absolute
  low-memory reads (`*(u32*)0x80000028`) work.

## Entry point and frame pacing (`port/src/port_main.cpp`)

- Aurora's `aurora_main.lib` owns the process `main()` and calls `aurora_main()`, defined in `port_main.cpp`.
- It initialises Aurora (window "Super Mario Sunshine", 1280x960, 16 MiB ARAM, vsync off), calls `OSInit()`,
  registers the 59.94 Hz VI retrace interrupt on w5's interrupt thread, begins the first frame and calls the game's `main()`.
  `src/main.cpp` is compiled with `-Dmain=sms_game_main`.
- The retrace interrupt (interrupt context, like the GameCube's) bumps the retrace count, runs the pre/post retrace callbacks and wakes waiters.
- `VIWaitForRetrace()` sleeps the calling game thread on the retrace queue (so lower-priority game threads such as the setup thread run meanwhile).
  On the main thread it then ends the Aurora frame in progress (submit + present), pumps window events (closing the window exits) and begins the next frame,
  so GX calls are always recorded inside a frame. Other threads (`JKRDvdRipper` waits for retraces while loading) only wait.
  The port also implements `VIGetRetraceCount`, `VIGetNextField`, `VIGetDTVStatus`,
  `VISetNextFrameBuffer`/`VIGet*FrameBuffer`, `VISetBlack` and the retrace callbacks (Aurora has none of them).
- The crash reporter is `port_crash.cpp` (shared with `build_port.py`).

## Header-conflict categories and fixes

| # | Category | Example | Fix |
|---|----------|---------|-----|
| 1 | Aurora changed a signature for PC (extra arguments) | `GXSetArray(attr, data, stride)` needs `size` and `le` | Call sites pass real sizes through `port/include/port_gx.h` (see "GX path" below). The SDK 3-argument form stays as a C++ overload in `port_aurora.h` for unconverted call sites: `le=true`, readable span capped at 64 KiB, logged once per attribute. |
| 2 | Aurora changed a signature for PC (other arguments) | `CARDInit()` becomes `CARDInit(game, maker)` | C++ overload `CARDInit()` calls `CARDInit("GMSE", "01")` |
| 3 | const-correctness | `GXSetCopyFilter` takes non-const tables | template overload taking const pointers |
| 4 | SDK address macros became functions taking `u32` | `OSPhysicalToCached(ptr)` | template overloads for pointers (identity); `OSPhysicalToCached(0)` still uses Aurora's |
| 5 | Return type differs | `GDGetCurrPointer()` returns `void*` in Aurora, `u8*` in ours | explicit `(u8*)` cast in `J3DMaterial.cpp` (a no-op on GC) |
| 6 | Callback ABI is pointer-sized | `ARQCallback(uintptr_t)`; the game used `u32` | `JKRAramPiece::doneDMA` and `JASDvdThread` `aramDmaFinish*` take `uintptr_t` (same type as `u32` under MWCC); our `arq.h` uses `uintptr_t` under `SMS_PORT` |
| 7 | Functions/macros the game uses that Aurora does not declare | `GXSetDrawSync`, `GXWaitDrawDone`, `GXReadPixMetric`, `GXClearPixMetric`, `GXEnable/DisableBreakPt`, `GXLoadNrmMtxIndx3x3`, `GXGetTexObjAll`, `GXSetDispCopyFrame2Field` + `GXCopyMode`, `GXInitLightShininess`, `GXTex*Ids` tables, `GDBegin/GDEnd/GDGetCurrent`, `GDWrite_s8/s16/s32`, `__DSP_*_task` | declarations in `port_aurora.h`, copied from our headers; implementations in `port_aurora_sdk.cpp` (stub or minimal) |
| 8 | Aurora leaves a function to the application | `OSReport`, `OSVReport`, `OSPanic`, `OSFatal` (`#if 0` in Aurora) | `OSReport`/`OSVReport`/`OSPanic` in `sdk_os_misc.cpp` (one definition for both port builds), `OSFatal` in `port_aurora_sdk.cpp` |
| 9 | Opaque struct on TARGET_PC | `OSContext` is `char storage[0x2C8]`; JUTException reads `gpr`/`fpr`/`srr0` | shim `include_aurora/dolphin/os/OSContext.h` re-includes Aurora's header with `TARGET_PC` undefined (same size, PPC fields) |
| 10 | Different typedef where Aurora has no implementation | `OSErrorHandler` is variadic in Aurora; the game passes 4-argument handlers | shim `include_aurora/dolphin/os/OSError.h` uses our header |
| 11 | Enum spelling | `OS_EXCEPTION_DSI` vs Aurora's `__OS_EXCEPTION_DSI`; `OS_ERROR_MEMORY_PROTECTION` | `#define`s in `port_aurora.h` |
| 12 | Our split headers duplicate Aurora's contents | `dolphin/os/OSStopwatch.h` redefines `OSStopwatch`; `dolphin.h` includes siblings with quotes (bypassing Aurora: `u32` redefinition) | shims in `include_aurora/` |
| 13 | Compiler-attribute macro | Aurora's `ATTRIBUTE_ALIGN` is `__declspec(align)` for clang-msvc, which is invalid after the declarator | `port_aurora.h` redefines it as `__attribute__((aligned))` |
| 14 | Global `operator new/delete` replacement | JSystem's `JKRHeap.cpp` replaces them process-wide. Aurora/SDL/STL static initialisers then crashed (no JKRHeap yet). | `build_aurora.py` renames the global operators in game/JSystem objects only (`llvm-objcopy --redefine-syms`, for example `??2@YAPEAX_K@Z` to `sms_jkr_operator_new`; the four plain ones plus the two sized deletes JKRHeap.cpp defines under `SMS_PORT`). Game units are built with `-fno-sized-deallocation -fno-aligned-allocation`. Aurora keeps the CRT allocator. |
| 15 | C `extern inline` emitted externally by clang-msvc | `fake_tgmath.h` `sqrtf` in `hx_wiper.c` clashes with the UCRT | per-unit `-Dsqrtf=hx_wiper_sqrtf` (and `sqrt`, `fabs`, `floor`) |
| 16 | Aurora's header ABI differs from the 64-bit one the game/port uses | `ARQRequest` (u32 vs pointer-sized owner/source/dest), `AIGetDMAStartAddr` return type | shims `include_aurora/dolphin/arq.h` and `ai.h` use our headers (Aurora's AR/ARQ is not linked) |
| 17 | MSVC mangles return types | `MapDraw.cpp` declares `int SMSGetGameRenderWidth()` (MWCC ODR quirk), the definition returns `u16` | under `SMS_PORT` it includes `System/Resolution.hpp` |

## GX path (milestone 5)

How game GX traffic reaches Aurora (all port code is `#ifdef SMS_PORT`; Aurora-only parts also need `SMS_PORT_AURORA`):

- **Direct FIFO writes.** Game units compile against Aurora's `dolphin/gx/GXVert.h`, where `GXCmd1u8/u16/u32`, `GXParam*` and the `GXPosition*`/`GXColor*`/... vertex
  calls are functions that append to Aurora's command stream (or to the open display list).
  No game or JSystem source dereferences `GXWGFifo` (0xCC008000) itself (only `libs/dolphin/src`, which is not built), so nothing lands in the 0xCC000000 scratch page.
  `PacketUtil.cpp` `FifoSet*`, `JRenderer`, `J3DPacket` and `ShadowUtil` already go through these.
- **Vertex, matrix and light arrays** are native order on PC (J3D VTX1 after the swap pass; CPU-built arrays), so they go to Aurora with `le = true` and a real byte size:
  - `port/include/port_gx.h`: `PORTGXSetArray` (native), `PORTGXSetArrayBE` (raw GameCube bytes), `PORTGXSetArrayBase` (base only, Aurora's `GX_AURORA_LOAD_ARRAYBASE` command,
    the replacement for a CP array-base register write), `PORTGDSetArray`/`PORTGDSetArrayNone` (display-list forms); implemented in `port_aurora_sdk.cpp`.
  - `J3DVertexData::mPortArraySize[]`: extent of each VTX1 array (to the next table or the block end), computed in `J3DModelLoader::readVertex`.
    `getPortArraySize(idx, array, stride)` uses the vertex count times the stride for positions, so skinning copies and game-supplied position buffers are covered.
  - `J3DShape::makeVtxArrayCmd` (shape VCD/VAT display list) uses `PORTGDSetArray`; `kVcdVatDLSize` is 0x180 on Aurora (22 bytes per array instead of 12).
    `makeVcdVatCmd` pads the unused tail with `GX_NOP` because `draw()` calls the full size (BOOTLOG R1).
- **EFB readback** (`port_aurora_sdk.cpp`, public `aurora/gfx.hpp` API: `resolve_pass` + an encoder task + `MapAsync`): `GXPeekARGB` and the `SMS_PORT_SHOT` GPU screenshots (BOOTLOG R0).
- **EFB copies, `GXPeekZ`, pixel metrics** (`port_efb.cpp`, w8; BOOTLOG "EFB features"): `GXCopyTex` stays Aurora's GPU copy, keyed by destination; copies the CPU reads (the `GX_CTF_R8` pollution maps) are also read back into the destination memory.
  `GXPeekZ` replaces Aurora's end-of-frame depth snapshot with a snapshot at the peek's point of the frame. `GXClearPixMetric`/`GXReadPixMetric` count non-black pixels of a snapshot.
  Game calls of `GXSetTexCopySrc/Dst`, `GXSetCopyClear` and `GXCopyTex` are renamed to `port_*` by `build_aurora.py`; `port_main.cpp` calls `port_efb_end_frame()` before each `aurora_end_frame()`.
  - `J3DLoadArrayBasePtr(attr, ptr, size)` (new overload) sends `PORTGXSetArrayBase`; the CP 0xA0 register write is GameCube-only (Aurora logs an error for it).
    `J3DShape::loadVtxArray` and `BathWaterManager` pass sizes; the 2-argument form falls back to the bounded readable span.
  - `J3DSys::setModelDrawMtx/setModelNrmMtx(mtx, num)` give the position/normal matrix arrays `drawMtxNum * sizeof(Mtx/Mtx33)` (from `J3DShape::draw`).
  - Game call sites: `BathWaterManager` (height/normal/uv maps), `DLUtil` (quad buffers), `MapObjGrass` (colour table), `JDRLighting` (light array),
    `ModelWaterManager` (`tmp_data` positions are big-endian bytes: `PORTGXSetArrayBE`).
  - `GXLoadNrmMtxIndx3x3` (Aurora has none) writes `GX_LOAD_INDX_B` like the SDK; Aurora's command processor executes it.
- **Display lists** are copied into the FIFO as they are by Aurora's `GXCallDisplayList` and keep GameCube byte order. Nothing in them needs the 0x80000000 mapping any more:
  array bases and textures go through Aurora's own commands with 64-bit pointers. No decompiled loader uses the MDL3 prebuilt material display lists, and nested `CALL_DL` is never emitted.
  CPU readers of shape display lists (`J3DCluster` mtx index scan, `SMS_CountPolygonNumInShape`) read the counts/indices big-endian.
- **Textures in J3D material display lists.** `loadTexNo` writes BP image/TLUT address registers (physical address >> 5) that Aurora ignores.
  On Aurora it also appends `GX_AURORA_LOAD_TLUT` (slot = texture map) and `GX_AURORA_LOAD_TEXOBJ` (host pointer, size, format, mipmap; object id 0 = cached by content)
  through GD (`PORTGDLoadTlut`, `PORTGDLoadTexImage`). `J3DMaterial::countDLSize` adds `PORT_GD_TEX_EXTRA` (8 x 0x50 bytes) for them.
  Mode registers (wrap, filter, LOD) still come from the BP writes. Texels and palettes stay big-endian; Aurora decodes GameCube formats from them (CMPR, I4, I8, IA, RGB5A3, RGBA8 seen working).
  `GXInitTexObj`/`GXLoadTexObj` users (JUTTexture, J2D) work unchanged.
- **Endian fixes found on the way**: `J3DTevStage::load` read its four command bytes as a native u32 (register id ended up in the low byte), and `JUtility::TColor`'s
  u32 form (`toUInt32`, `set(u32)`) was byte-reversed; both now build/parse 0xRRGGBBAA explicitly.

Smoke test: `SMS_PORT_GXTEST=1` makes `port_main.cpp` run `port_gx_test()` (`port/src/port_gxtest.cpp`) instead of the game: it creates the JKR heaps, DVD and GX like
`TApplication::initialize`, loads `/mario/bmd/ma_mdl1.bmd` from `/data/mario.szs` through `JKRDvdToMainRam` + `JKRMemArchive` + `J3DModelLoaderDataBase::load`,
draws it with `J3DModel::calc/entry/viewCalc` into two `J3DDrawBuffer`s (one light, fixed camera, turning) and saves a screenshot halfway
(`SMS_PORT_GXTEST_SHOT`, default `gxtest.bmp` in the working directory; window capture with PrintWindow, else a screen copy, so keep the window visible).
Options: `SMS_PORT_GXTEST_MODEL=<archive>:<resource>`, `SMS_PORT_GXTEST_FRAMES`, `SMS_PORT_GXTEST_CAM=<eye y>,<eye z>,<target y>`, `SMS_PORT_GXTEST_DUMP=1` (material DL hex dump).

```
cd port/build_aurora
set SMS_PORT_DISC=..\..\..\..\dolphin_test\files
set SMS_PORT_GXTEST=1
sms_port.exe
```

## Aurora patches

The shared Aurora tree (`port_deps/aurora`, used by every worker) carries these source patches. Each is recorded as a unified diff in `port/patches/`
(paths `a/<file>` / `b/<file>`, apply with `git apply` or `patch -p1` on a clean checkout); the pristine files are kept in `port/patches/orig/`,
and `python port/tools/aurora_patch_diff.py <name> <file>...` regenerates a patch after an edit.
Rebuild the changed library in place (it stays compiling after every step):
`python -m ninja aurora_gx aurora_core aurora_vi` in `port_deps/aurora/build`, inside the vcvars environment (`port/build_aurora/msvc_env.json` holds it), then relink with `build_aurora.py`.
Several agents patch this tree: re-read a file right before editing it, and keep the pristine copy of every file a patch touches in `port/patches/orig/` (one patch per file set, so they can be regenerated independently).
Where two agents' patches touch the same file, the `orig/` copy is the file with the other patch already applied (e.g. `orig/lib/gx/gx.cpp` and `orig/lib/dolphin/vi/vi.cpp` include w8's `aurora_efb_size.patch`).
Apply order on a clean checkout: `aurora_efb_size`, `aurora_copy_filter`, `aurora_shader`, `aurora_display_aspect`, `aurora_pipeline_wait`, `aurora_depth_tex_formats`, `aurora_copy_scale`, `aurora_msaa_depth`, `aurora_visual_options`, `aurora_present_capture`, `aurora_efb_floor`, `aurora_getenv_once` (w12; its context is the unrecorded `AURORA_BIGDRAWS` diagnostics the tree carries).
`aurora_copy_scale`, `aurora_msaa_depth`, `aurora_visual_options` (w6, round 5) and `aurora_efb_floor` (w6, R23) are written by `python port/tools/aurora_w6_patches.py` as "live file minus w6's edits" -> live file, so they contain only w6 hunks and apply on top of the others (w8's half-texel clamp inside `fxaa_sample` is part of `aurora_visual_options`, since the function is new there).

**Pipeline cache.** Hand-started runs use Aurora's per-user cache (`%APPDATA%\Super Mario Sunshine`, or `SMS_PORT_CACHE_DIR`); it persists across runs and every known pipeline is compiled in the background at startup (`rebuild_pipeline_cache`).
`build_aurora.py --run` deliberately starts from an empty per-user cache. With `aurora_pipeline_wait.patch` a draw whose pipeline is still compiling waits for it, so a cold cache costs load-time hitches but never missing geometry, and a bad cached entry only logs an error.

**Bundled seed (w6):** `port/data/initial_pipeline_cache.db` (899 GX pipelines, 3.6 MB) is copied next to `sms_port.exe` by every `build_aurora.py` build and into `--package` folders.
Aurora opens `<exe folder>/initial_pipeline_cache.db` read-only at startup (`resourcesPath` defaults to the exe folder), merges its rows into the per-user cache (`Seeded pipeline cache from ... (899 rows merged)`) and prewarms them in the background.
Rows are machine independent (GX pipeline config + first frame used); rows of another `GXPipelineConfigVersion` are skipped one by one, never fatal.
Regenerate after a pipeline-config change (version bump) or new content: delete `port/build_aurora/cache_seed`, run `powershell -File port\tools\qa_sweep.ps1 -CacheDir cache_seed` (all stages, about 40 minutes; also writes the QA screenshots), then `python port/tools/make_pipeline_seed.py port/build_aurora/cache_seed`.
Measured (empty per-user cache, plaza warp, retraces for frames 1200-1800, 600 = no hitch): 1397 without the seed, 801 with it, 607 with a warm per-user cache. The remaining hitch is the single pipeline thread still prewarming 900 pipelines when the stage needs its own (the Dawn shader blob cache is driver-specific and cannot be shipped).
Shader-text changes (like `aurora_shader.patch` 4/4) do not change pipeline keys, so the seed stays valid; only `GXPipelineConfigVersion` bumps invalidate it.

| Patch | File | What | Why |
|-------|------|------|-----|
| `aurora_shader.patch` (1/4) | `lib/gx/shader.cpp` | Alpha-combiner compare modes `GX_TEV_COMP_R8/GR16/BGR24_*` compare the colour combiner's A/B inputs (captured as `tevN_ca`/`tevN_cb` before the stage's colour op writes) | Generated WGSL swizzled `.r` on an `f32` (Dawn parse error, Delfino Plaza and airstrip); hardware semantics as in Dolphin. BOOTLOG R2. |
| `aurora_shader.patch` (2/4) | `lib/gx/shader.cpp` | A TEV/indirect stage reading a texcoord at or beyond `numTexGens` gets (0, 0) and a warning listing the stages, instead of `FATAL("unhandled tcg src 21")` | JPA indirect particles with the texgen count lowered later (Pianta, Noki, Gelato); hardware behaviour as modelled by Dolphin. BOOTLOG R3. |
| `aurora_shader.patch` (3/4) | `lib/gx/shader.cpp` | A texgen whose source `GX_TG_TEXn` is missing from the vertex format reads (0, 0) (warning) instead of `FATAL("unmapped vtx attr 13")` | Sirena Beach (warp 6,0); Dolphin's vertex shader does the same. BOOTLOG R5. |
| `aurora_shader.patch` (4/4) | `lib/gx/shader.cpp`, `lib/gx/shader_info.cpp` | `GX_TG_SRTG` texgens (source `GX_TG_COLOR0/1`) take S/T from the colour channel's **lit** output, as hardware/Dolphin do, instead of the raw vertex colour (zero when the vertex has none); the channel is computed even when only a texgen uses it. Lighting is written to a local `ccN_lit` first (copied to `out.ccN`). | SMS's toon shading: Mario's materials, the diving helmet and the maps' pollution-ground material look up `H_toon_*` ramp textures with lit colour as coordinates; they all sampled one corner texel (flat shading). Reading back `out.ccN` crashed the AMD D3D12 driver in `CreateGraphicsPipelineState`, hence the local. BOOTLOG R14. |
| `aurora_pipeline_wait.patch` | `lib/gx/pipeline.cpp`, `lib/gx/gx.cpp`, `lib/gfx/pipeline_cache.cpp/.hpp` | (a) `gx::render` waits for a pipeline that is still compiling (`wait_pipeline`: moved to the front of the queue) instead of skipping the draw. (b) Shader/pipeline **validation** errors fail only that pipeline (error log, draws skipped) instead of the fatal uncaptured-error handler. (c) `build_pipeline` captures D3D12 `E_OUTOFMEMORY` (Dawn reports it as an *Internal* error) and retries 5 times with back-off, then gives up on that pipeline. (d) Debug: `AURORA_SHADER_DUMP=<dir>` writes every GX pipeline's WGSL. | (a) No missing geometry after a load, like the GameCube; supersedes w8's consumed-pass-only wait (`aurora_copy_pass_pipelines.patch`, same `wait_pipeline` API). (b) A bad entry in the persistent per-user pipeline cache can no longer stop startup. (c) Transient E_OUTOFMEMORY in Sirena/Hotel/Pianta. BOOTLOG R6, R7. |
| `aurora_pipeline_wait.patch` (round 3 additions) | `lib/gx/gx.cpp`, `lib/gx/pipeline.hpp` | (e) All 16 `GXLogicOp`s approximated with blending (`to_blend_state`; exact for a white source). (f) The pipeline key stores only whether the constant destination alpha is off / zero / non-zero; the value is the blend constant. `GXPipelineConfigVersion` 13 -> 14 so old caches full of per-value duplicates are pruned. | (e) `GX_LO_INV` (Mario's logic-op draw, JPA particles) was a fatal error that, once recorded in the per-user cache, killed every later start during prewarm (Pinna Park). (f) 1310 of 2044 GX pipelines in a full sweep differed only by that value (fades), causing startup E_OUTOFMEMORY; 899 after. BOOTLOG R10, R11. |
| `aurora_depth_tex_formats.patch` | `lib/gfx/texture_convert.cpp` | `GX_TF_Z8/Z16/Z24X8` read from RAM decode like I8/IA8/RGBA8 | Corona boss: a Z24X8 depth-copy destination bound before the first copy was a fatal `unknown texture format 22`. BOOTLOG R12. |
| `aurora_display_aspect.patch` | `lib/window.hpp`, `lib/window.cpp`, `lib/dolphin/vi/vi.cpp` | `VILockAspectRatio`/`VIUnlockAspectRatio` (declared in `dolphin/vi.h`, never defined): the FIT framebuffer uses the locked aspect instead of the EFB's | w9's proposal applied by hand (exact diff now in the file): the 640x448 EFB is shown at exactly 4:3 in windows and fullscreen; `port_window.cpp` already calls it. BOOTLOG R8. |
| `aurora_efb_size.patch` (w8) | `lib/dolphin/vi/vi.cpp`, `vi_internal.hpp`, `lib/gx/gx.cpp`, `lib/webgpu/gpu.cpp/.hpp`, `lib/aurora.cpp`, `lib/window.cpp` | The EFB render target covers the whole GameCube EFB: `vi::efb_size()` = at least 640x528 logical; `gx::logical_fb_size()` returns it (viewports, scissors, copies map against it); `resize_swapchain` allocates the target at the presented frame buffer's scale extended to the EFB, rounded up (so the presented part is fully rasterised at non-integer scales; BOOTLOG E20); presenting (viewport and resample UVs) uses only the configured top-left part (`present_content_width/height`); `window.cpp resize_swapchain()` also forwards to `webgpu::resize_swapchain` when the window size is unchanged, so the target follows a render-mode change (a no-op otherwise); the present sample is clamped half a texel inside the presented part, so bilinear upscaling never blends in EFB row 448 (BOOTLOG E16). | Games draw and copy anywhere in the 640x528 EFB before the display copy. SMS's airstrip pollution map is 512x512 on a 640x448 render mode: clamped to 448 rows and rescaled, the map slid off and emptied in a few frames. Port code maps peeks/screenshots against the same EFB size (`port_efb.cpp`, `port_aurora_sdk.cpp`). Without the window.cpp hunk the target kept the 640x480 boot mode's EFB size after the game switched to 640x448, leaving rows 448-528 as a black band in screenshots (BOOTLOG E9). BOOTLOG E6. |
| `aurora_present_capture.patch` (w8) | `include/aurora/gfx.hpp`, `lib/aurora.cpp`, `lib/webgpu/gpu.cpp` | `gfx::request_present_capture(callback, userdata)`: one-shot copy of the swapchain texture after the present blit and ImGui, i.e. exactly what the window shows, read back after submit. The surface is configured with `CopySrc` when its capabilities allow it. | Agents could not see the presented picture: the screen copy shows other windows on top and PrintWindow is black for D3D12. Port side: `SMS_PORT_SHOT_PRESENT=1` writes `present_<frame>.bmp` next to `shot_<frame>.bmp` (`port_aurora_sdk.cpp`); works with the window covered or off-screen (`SMS_PORT_OFFSCREEN`). BOOTLOG E20. |
| `aurora_copy_scale.patch` (w6) | `lib/dolphin/gx/GXFrameBuffer.cpp` | `copy_tex` maps the copy source rectangle with rounded edges (`map_copy_rect`, instead of the scissor's outward floor/ceil) and gives a 1:1 copy (destination size = source size in logical pixels) exactly the source rectangle's size. | At internal resolutions where 512 logical rows map to N + f target rows with f < 0.5 (4x: 2194.4; window scale 2.41) the source was one row taller than the destination, so every copy resampled. The airstrip pollution map is drawn back (nearest) and copied every frame, so it drifted and blurred until empty, and the goop counter read "clean". BOOTLOG R17. |
| `aurora_msaa_depth.patch` (w6) | `lib/gfx/tex_copy_conv.cpp/.hpp`, `lib/gfx/encoding.cpp`, `lib/gfx/depth_peek.cpp`, `include/aurora/gfx.hpp`, `lib/gfx/frame_packet.hpp`, `lib/gfx/recording.cpp` | With `AuroraConfig::msaa` 4: depth texture copies from the multisampled EFB use multisampled variants of the Z8/Z16 conversion pipelines (`texture_depth_multisampled_2d`, sample 0; `ConvRequest::srcSamples`), instead of `FATAL("Depth tex copies from multisampled EFB targets are not supported")`; Aurora's own depth peek builds its compute pipeline for the configured sample count instead of a fatal. `ResolveDesc::colorSample0`: the colour snapshot is sample 0 of each pixel (compute pass + buffer copy, `snapshot_color_sample0`) instead of the resolve. The copy-conversion and depth-snapshot pipelines retry E_OUTOFMEMORY (`create_render_pipeline_retry`) instead of the fatal handler. | The port's MSAA option (Corona's bath-water depth copies); pixel counters and CPU-read copies stay one sample per pixel like the GameCube (`port_efb.cpp` asks for sample 0). w8's snapshots/peeks already resolved depth with `snapshot_depth`. BOOTLOG R18. |
| `aurora_visual_options.patch` (w6) | `include/aurora/aurora.h`, `lib/gfx/texture.cpp`, `lib/webgpu/gpu.cpp`, `lib/dolphin/gx/GXFrameBuffer.cpp` | Run-time options, all off by default: `aurora_set_texture_filtering(aniso, trilinear)` (forced anisotropy for linear-filtered mipmapped textures; linear mip blending); `aurora_set_post_process(gamma, brightness, fxaa)` in the present resample pass (FXAA "PC fast" on the source picture; brightness/gamma skipped at 1.0); `aurora_set_copy_filter_enabled(false)` skips `GXSetCopyFilter`'s vertical filter on texture copies. | The port's graphics options (F1 Display tab, `[graphics]` in port_settings.ini). Defaults render byte-identically (EFB screenshots compared). BOOTLOG R19. |
| `aurora_copy_filter.patch` (w8) | `include/dolphin/gx/GXAurora.h`, `lib/dolphin/gx/GXFrameBuffer.cpp`, `lib/gx/command_processor.cpp`, `lib/gx/gx.hpp`, `lib/gfx/recording.cpp/.hpp`, `lib/gfx/frame_packet.hpp`, `lib/gfx/tex_copy_conv.cpp/.hpp`, `lib/gfx/encoding.cpp` | `GXSetCopyFilter`'s vertical filter (was a no-op): the seven coefficients become weights for the EFB lines above/at/below (out of 64), sent through a new `GX_AURORA_LOAD_COPY_FILTER` command and applied in the texture-copy shader (`sample_efb`); a filtered copy always takes the shader blit. The AA sample pattern is ignored (single-sampled EFB). | SMS copies with the deflicker filter (`SMSVFilter_flicker`: screen texture, mirrors) and `BathWaterManager`'s ripple copy (`{21,0,0,22,0,0,21}`), which is part of its water simulation. BOOTLOG E8. |
| `aurora_efb_floor.patch` (w6) | `lib/window.cpp`, `lib/webgpu/gpu.cpp` | (a) `get_window_size`: the EFB frame buffer is never below the configured render mode (640x448) in either axis; a smaller one (internal scale < 1, widescreen in a small window) is scaled up uniformly. (b) `calculate_present_viewport` fills the surface when the content's shape differs from it only by the target's whole-pixel rounding (gap <= half a target pixel + 1). | (a) Below native, several GameCube pixels share a target pixel and the CPU-read copies stop matching: airstrip pollution map 4532 texels at 0.5x and 4551 at 0.75x instead of 4548. (b) 16:9 at 1x is a 796x448 target (796.4 rounded), which left a black column at the right of a 1600x900 window. Defaults unaffected (window-sized target). BOOTLOG R23. |
| `aurora_getenv_once.patch` (w12) | `lib/gx/command_processor.cpp`, `lib/gfx/recording.cpp` | The `AURORA_BIGDRAWS` diagnostics read the variable once (`static const bool`) instead of calling `getenv` twice per draw and once per frame. Also (not in the patch, `lib/webgpu/gpu.cpp`): `AURORA_LOW_POWER=1` requests the low-power adapter (integrated GPU) for weak-GPU tests; default unchanged. | UCRT `getenv` takes the CRT lock and scans the whole environment block: ~70% of the FIFO processor's time in Delfino Plaza, 40 ms per frame, 15 fps. BOOTLOG "Performance (w12)" P1. |

## Status (2026-10-08)

- `build_aurora.py` is the port build. All 579 game/JSystem units and all 13 port files compile (593 objects with the stub file).
  17 symbols are stubbed: the 16 THPPlayer functions and `ActivePlayer`.
- `sms_port.exe` opens the Aurora window (D3D12), maps RAM at 0x80000000, runs the game's `main()`; the setup thread runs on w5's scheduler,
  the boot `gameLoop()` presents frames, then `initialize_bootAfter()` mounts the `nlogo` archive, builds the `JUTResFont` and constructs `MSound`.
- It stops in `MSound::MSound` -> `JALSystem::init()` -> `TFlagManager::TFlagManager()`: `new u16[mSeTable.mSoundMax[i]]` returns null
  and the zero-fill writes to address 0 (JAudio, w5's area; with 64 MiB RAM it stops at the same place, so it is not the RAM size).
- Next blockers: JAudio (w5), big-endian/64-bit J3D data (w6), THPPlayer, EFB peeks (`GXPeekARGB`) and pixel metrics.
- GX path groundwork (w8): array sizes/endianness, J3D array-base and texture commands, `GXLoadNrmMtxIndx3x3` (see "GX path").
  The `SMS_PORT_GXTEST=1` smoke test draws Mario (`ma_mdl1.bmd`) skinned and textured, and FLUDD (`wg_mdl1.bmd`), through the real J3D path.
- Remaining GX gaps: EFB peeks and pixel metrics; indexed light loads (`GX_LIGHT_ARRAY`: Aurora's `GXLightObj` is not in XF layout, no caller found yet);
  `GXSetDispCopyFrame2Field`/display copies; J3D texture-in-DL objects use object id 0 (content-hashed on every rebind);
  Mario's dirt/body-colour TEV registers come from game code the smoke test does not run.

