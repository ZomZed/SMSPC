# Headless boot log (x64, `build_port.py`)

Running list of boot blockers, one per entry: symptom, then cause, then fix.

Run: `port\build\sms_port.exe` with the working directory `dolphin_test`.
Debug knobs (all off by default):
- `SMS_PORT_STACKS=<s>` dumps every thread's stack after `<s>` seconds, then exits (`port_crash.cpp`).
- `SMS_PORT_THP_SPEED=<n>` plays movies `<n>` times faster, so tests skip the 94 s intro (`sdk_thp.cpp`).

`OSPanic` now prints a backtrace.
`TApplication::proc` logs every state change (`[port] TApplication state N`).

## 1. `JALSystem::init` writes to address 0
- **Symptom:** crash in the inlined `TFlagManager` constructor, called from `MSound::MSound`.
- **Cause:** 64-bit growth.
  `new` returned null because the 0x151800-byte sound `JKRSolidHeap` was full.
  JAudio objects carry 8-byte pointers, and every JKR heap block is rounded up to 16 bytes on x64, so boot needs about 0x16E000.
  `mSoundMax` was correct.
  MWCC null-checks the result of `new`; clang does not.
- **Fix:** `src/MSound/MSound.cpp` creates the heap with 0x200000 bytes under `SMS_PORT`.
  The port arena is 32 MB instead of 24 MB, so this fits.
  `portNew` (`JKRHeap.cpp`) now logs a failed `new` with the heap and its free sizes, so future cases show up straight away.

## 2. Audio thread crash in `TTrack::mainProc` (pointer `0x7800000000`)
- **Symptom:** the audio thread crashes in `updateSeq`; `mChildren[2]` holds `0x0000007800000000`.
- **Cause:** a clang optimisation of undefined behaviour in `TTrack::writeRegParam`.
  For sequence registers 0x20, 0x21, 0x2E and 0x2F, `uVar6` is never set before `setFlag(uVar6)`.
  clang treats that read as unreachable and deletes those four switch cases.
  The register value 0x78 was then stored through `unk0[0x2E]`, which lands in `mChildren[2]`.
  On the GameCube, `uVar6` is r31, which still holds the caller's `TSeqCtrl*`, i.e. the track address (from the asm of `TSeqParser::mainProc`).
- **Fix:** in `JASTrack.cpp`, under `SMS_PORT`:
  - `uVar6` is initialised to the low half of `&mSeqCtrl`, the GameCube value.
  - Register slots past the 6-entry `unk0` array are addressed from the struct base (`SMS_REG_SLOT`), so out-of-bounds indexing cannot be exploited the same way.

## 3. Endless intro movie
- **Symptom:** boot reaches `APP_STATE_DONE`, i.e. `TMovieDirector` with movie 9, and never leaves it.
- **Cause:** missing SDK piece.
  `THPPlayer*` were logging stubs, so `THPPlayerGetState()` stayed at 0.
- **Fix:** new `port/src/sdk_thp.cpp`, a host THPPlayer.
  - It has the same API, the same `ActivePlayer` struct and the same state machine as the decompiled `THPPlayer.c`.
  - It reads the big-endian header.
  - Frames advance at the movie's frame rate from the VI post-retrace callback, and it reaches state 3 (played) at the end.
  - Nothing is decoded or drawn yet.

  Also, the GameCube `TMovieDirector::setupThreadFunc` returns `rsetup()`'s result: it falls off the end with r3 still set, and `direct()` reads it through `OSJoinThread`.
  The `SMS_PORT` version had been changed to `return nullptr`; it now returns `rsetup()`'s value.

## 4. `JPAParticle` constructor writes to null
- **Symptom:** crash in `JPAEmitterManager::JPAEmitterManager` during `TMarDirector::loadResource`.
- **Cause:** 64-bit growth.
  The particle, emitter and field pools are solid heaps sized `count * sizeof(T)`.
  `sizeof(JPAParticle)` is 0xF8 on x64, and each block is rounded up to 0x100.
- **Fix:** `JPAEmitterManager.cpp` sizes each pool element as `ALIGN_NEXT(sizeof(T), 16)` under `SMS_PORT` (`POOL_ELEM_SIZE`).

## 5. `JPAResourceManager::load` writes to `0xA`
- **Symptom:** crash right after the previous fix; the emitter data pointer is null.
- **Cause:** endianness.
  The `.jpa` reads were all native-order: the header magic `JEFF`/`jpa1`, the block walk, the shape blocks and the `TEX1`/`KFA1` sizes.
- **Fix:**
  - `JPA_FILE_S16/U32/F32` macros in `JPADataBlock.hpp` read file fields big-endian under `SMS_PORT`.
    They are used in `JPAEmitterLoader`, `JPADataBlock`, `JPABaseShape`, `JPAExtraShape`, `JPASweepShape` and `JPAExTexShape`, and for the block size in `JPAEmitter`/`JPAField`.
  - The `JPAColorRegAnmKey` frame indices are converted before `makeColorTable`, including the one key past the end that the loop compares against.
  - The `KFA1` key table in the private copy is swapped in the `JPAKeyFrameAnime` constructor.
  - The file data is never modified, so `BEM1`/`FLD1` keep working through the typed (swapping) stream reads.

## 6. `TApplication::mountStageArchive` reads from null
- **Symptom:** `unk30` is null.
  `TNameRefGen::search("ステージ毎シナリオアーカイブ名群")` found nothing.
- **Cause:** a port-layer bug.
  The sources are UTF-8. The GameCube build passes them through sjiswrap, so string literals end up Shift-JIS, like the game data.
  clang only supports UTF-8 (`-fexec-charset=shift_jis` is rejected), so all 1965 Japanese literals in 267 files were UTF-8, and every lookup by Japanese name failed.
- **Fix:** new `port/tools/sjis_mirror.py`, used by `build_port.py` and `build_aurora.py`.
  - It writes copies of the affected sources and headers to `port/build*/sjis/` with narrow literals re-encoded as cp932 octal escapes.
  - The build compiles those copies, and the mirror include roots come first.
  - Comments and wide literals are left alone, and quoted includes that resolve next to the original file are made absolute.

## 7. `JKRHeap` aborts with an alloc of 0xB0000000 bytes
- **Symptom:** the abort happens in `TGraphWeb::TGraphWeb`, called from `TConductor::makeGraphGroup` in `setupObjects`.
- **Cause:** endianness.
  The rail file (`scene.ral`: `GraphDesc` table plus `TRailNode[]`) is used in place, and `translateNodes` rewrites it in place.
- **Fix:** `graph.cpp`, `portSwapRail()` under `SMS_PORT` in the `TGraphGroup` constructor.
  It swaps the whole file to native order once, and is idempotent: a first node count of 0x10000 or more means the file is still big-endian.
  `JKRDefaultMemoryErrorRoutine` now logs the heap, size and alignment before it aborts.

# Aurora boot log (`build_aurora.py`)

Run: `python port/tools/build_aurora.py --run --timeout 120`, or `port\build_aurora\sms_port.exe` with `SMS_PORT_DISC` set.
New knobs:
- `SMS_PORT_SHOT=<frame>[,<frame>...]` saves the window as `shot_<frame>.bmp` after presenting each listed frame (`port_main.cpp`; PrintWindow, else a screen copy, so keep the window visible and uncovered).
- `SMS_PORT_GS_DEBUG=1` while running `build_aurora.py` builds game/JSystem units with `-fstack-protector-all` and reports the function whose stack cookie was smashed (`sms_gs_check` in `port_crash.cpp`).
  Debug only; ninja rebuilds everything when it is toggled.
- stdout is unbuffered in the Aurora build, so `sms_port.exe > log.txt 2>&1` keeps OSReport and port/Aurora lines in order.

Build note: `robocopy` keeps the source timestamps, so after a sync the ninja objects and the `build_aurora/sjis` mirror can look newer than the synced sources.
Delete `port/build_aurora/obj`, `port/build_aurora/sjis` and `.ninja_log`/`.ninja_deps` after a sync (otherwise stale objects link, e.g. duplicate `OSReport`).

## 8. Crash in `vfprintf` from `OSReport` in `TApplication::proc` (format pointer `0x3F80000000000000`)
- **Symptom:** the state-change `OSReport` on the second loop iteration crashes in the CRT.
  The format argument itself is `0x3F80000000000000`; the literal's address was kept in `rbx` across the loop and came back changed from `gameLoop()`.
  The CARD "Failed to open file" message just before is harmless (no save file yet) and unrelated.
- **Cause:** a decomp transcription bug.
  `TApplication::gameLoop` and `TApplication::drawDVDErr` declared the projection matrix for `C_MTXOrtho` as `Mtx` (3x4, 48 bytes), but `C_MTXOrtho` writes a `Mtx44`.
  The last row `{0, 0, 0, 1.0f}` overran the local by 16 bytes into the saved registers (on the GameCube it lands in other stack slots).
  The GameCube frames confirm `Mtx44`: in both functions the matrix and the next local are 0x40 bytes apart (`gameLoop` 0x1B0 and 0x1F0, `drawDVDErr` 0xB8 and 0xF8).
- **Fix:** both locals are `Mtx44` in `src/System/Application.cpp`.
  The unit is NonMatching; the match percentages are unchanged, and the DOL is unchanged.
- **How it was found:** a stack-protector build hid the bug (layout change), so a naked-asm `rbx` probe around the calls in `proc` showed that `gameLoop()` returned with `rbx` changed.

## 9. `TSpcBinary::init` reads from `0xC75B7A04`
- **Symptom:** the crash happens in `calcAndStoreKeys`, called from `TEventWatcher::launchScript` in `TMarDirector::setupObjects` on the title stage.
- **Cause:** endianness.
  The SPC event script (`.sb`) is used in place: header, data-offset table, symbol table and bytecode immediates were all read native-order.
- **Fix:** `spcinterp.cpp`, `spcPortSwapBinary()` under `SMS_PORT` in the `TSpcBinary` constructor.
  It swaps the header, the data-offset table and the symbol table to native order once.
  It is idempotent, because several binaries can wrap one resource (`/default.sb`): a text offset of 0x10000 or more means the file is still big-endian, since the text always starts right after the 0x1C-byte header.
  `fetchS32/U32/F32` in `spcinterp.hpp` read the bytecode big-endian through `SPC_FETCH_BYTE`, which is `src[i]` on the GameCube, the same tokens as before.

## 10. Audio thread crash in `JAISystemInterface::setSePortParameter` (track `0x5_8020DD10`)
- **Symptom:** `args->mTrack` has `5` in its upper 32 bits.
- **Cause:** 64-bit layout.
  `JAIPlayerParameter` overlays `TPortArgs` with `f32/u32 mArgsAs*[10]`, and `setSeqPortargsF32/U32` index it as 4-byte words in the GameCube layout (word 1 = `mFlags`).
  On x64 `mTrack` is 8 bytes, so word 1 is the upper half of `mTrack` and every parameter landed 4 bytes early.
- **Fix:** `JAISystemInterface.cpp` maps the word index to the field (`portArgWord`) under `SMS_PORT`: word 0 is `mTrack`, and word n is `mFlags` + 4(n-1).
  `setSeqPortargsPS16` has no caller.

## 11. New game: `TMovieRumble::init` → `Koga::ToolData::GetValue` reads `0x82000004`
- **Symptom:** the crash comes after choosing a New file, in the opening movie's setup thread (`TMovieDirector::rsetup`).
- **Cause:** endianness.
  The movie rumble table (`/subtitle/rnbl/*.bcr`, a JMap/BCSV file) is read in place: header, field table and cells were native-order.
  The intro movie has no `.bcr`, so only the opening movie hit this.
- **Fix:** `src/MarioUtil/ToolData.cpp`, `toolDataSwapToNative()` under `SMS_PORT` in `ToolData::Attach`.
  It swaps the header, the field table (hash, mask, offset), and every entry's numeric cells (LONG/FLOAT/LONG_2/STRING_PTR as 32-bit, SHORT as 16-bit; strings are left alone).
  Each cell offset is swapped once, even when several fields share it.
  It is idempotent: a field count of 0x10000 or more means the file is still big-endian.
- **Verified:** with scripted input, title → file select → New → opening movie (`TMovieDirector` inside `APP_STATE_GAMEPLAY`, area 0) → Delfino airstrip (area 0) gets past this point.
  The user also reported rumble during the movie.

## SMS_PORT_WARP (debug warp)
`SMS_PORT_WARP=<area>,<scenario>`, or `build_aurora.py --run --warp <area>,<scenario>`, is off by default and exists only under `SMS_PORT`.
After the Nintendo logo (`APP_STATE_NLOGO`, once `initialize_nlogoAfter()` has loaded the common archives and created the game heap), `TApplication::proc` skips the intro movie, title and file select.
It does what choosing a New file does: `TFlagManager::firstStart()`, save slot 0, `mNextArea.set(area, scenario, 0)`, then `APP_STATE_GAMEPLAY`.
The stage's own arrival movie still plays through `checkAdditionalMovie()` (use `SMS_PORT_THP_SPEED`).
- Code: `src/System/Application.cpp` (the block after the post-gameLoop switch) and `port_warp_target()` in `port/src/sdk_os_misc.cpp`, so it works in both port builds.
- Examples: `--warp 0,0` (Delfino airstrip), `--warp 1,0` (Delfino Plaza), `--warp 2,0` (Bianco Hills episode 1).
- Checked: `--warp 1,0` plays the plaza arrival movie (state 6 twice) and loads Delfino Plaza (state 5, area 1/0) in about 10 s.
  `--warp 0,0` plays the opening movie and loads the airstrip (state 5, area 0/0, twice). Both then stop at the w6 and w5 blockers below.

## Run and debug knobs added (Aurora build)
- `build_aurora.py --run` uses real D3D12 and a visible window by default; `--nullgfx` selects Aurora's Null backend (no GPU device, software SDL renderer, window off-screen and never activated).
  `SMS_PORT_OFFSCREEN=1` moves a real window off-screen. Aurora ignores negative positions, so +30000 is used.
- `--run` only kills the `sms_port.exe` it started itself (PID in `build_aurora/run_pid.txt`), because the user may be running the same exe.
- `--run` gives the game its own pipeline cache, `SMS_PORT_CACHE_DIR=port/build_aurora/cache`, emptied on every run.
  Aurora recompiles every cached pipeline at startup, so one bad GX state recorded by an earlier run fails the next run before the game starts.
  **The per-user cache** (`%APPDATA%\Super Mario Sunshine`, used by double-click runs) **can be in that state now**: it holds the bad WGSL pipeline recorded by a run that reached the airstrip.
  Hand-started runs then die at boot with the WGSL fatal until that cache is cleared. That folder is outside w7, so I left it alone.
- Double-click launch: with no `SMS_PORT_DISC` and no `./files`, `sdk_dvd.cpp` walks up from the exe's directory looking for `dolphin_test\files`, then `files`.
- Crash, OSPanic and Aurora FATAL text and backtraces are also appended to `sms_port_crash.log` next to the exe.
  On an Aurora FATAL, the game main thread's stack is dumped as well (`port_dump_thread`), because Aurora's FIFO and pipeline workers run on their own threads.
- `SMS_PORT_PADSCRIPT=<frame>:<buttons>,...` presses buttons on pad 0 for 10 frames each (START, A, B, X, Y, Z, L, R, UP/DOWN/LEFT/RIGHT or a hex mask, joined with `+`).
  Game `PADRead` calls are renamed to `port_PADRead` in game objects.
- `SMS_PORT_SHOT=<frame>,...` saves screenshots (`shot_<frame>.bmp`). The shot is a screen copy, so the log line says when another window covers the game window.
- `SMS_PORT_GXSYNC=1` drains Aurora's FIFO before and after every `GXCallDisplayList` (`port_GXCallDisplayList`), so a FIFO error stops inside the game call that queued it, and the fatal handler hex-dumps that display list.
- `SMS_PORT_WATCH=<hex addr>` sets a hardware write watchpoint in all threads. It can wait until a frame (`SMS_PORT_WATCH_FRAME`) or until a GD display list is built over the address (`SMS_PORT_WATCH_GD=1`).
- `SMS_PORT_GS_DEBUG=1` (while building) adds a stack-protector build with a reporting cookie check (entry 8).

## Blockers on the new-game path, outside my area (for w6 and w5)
- **Airstrip, w6: Aurora FATAL `indexed XF load outside array 22: offset=2004408, size=16, array size=36`.**
  Root cause, found with `SMS_PORT_GXSYNC` plus the DL dump: Mario's `J3DShape::mVcdVatCmd` display list.
  `makeVcdVatCmd` writes its commands (0xB4 bytes here) and `GDPadCurr32`, but `J3DShape::draw` calls `GXCallDisplayList(mVcdVatCmd, kVcdVatDLSize)` with 0x180 on Aurora.
  The tail of the buffer (from `J3DShapeFactory::allocVcdVatCmdBuffer`, `new u8[]`, not cleared) still holds stale heap data, matrices left by the previous stage.
  From offset 0x140 those bytes parse as `2B D9 7E 3D` = `LOAD_INDX_B`, index 0xD97E.
  The fix belongs in w6's J3D SMS_PORT code: clear the buffer, or pass the real list length.
  A local test that zeroed the buffer did not, on its own, remove the WGSL fatal below; I reverted it.
- **Plaza (warp 1,0) and airstrip, w6: Aurora FATAL `WebGPU error 2: Error while parsing WGSL: :399:67 error: cannot index into expression of type 'f32'`.**
  It comes from `aurora::gx::build_shader` on the pipeline worker.
  The shader source cannot be dumped (`EnableDebugPrints` is constexpr false in Aurora).
- **w5: two JAudio corruption crashes, intermittent, during the title, file select and airstrip.**
  1. `JASystem::Driver::updatecallLogicalChannel` gets channel `0x8030E487`, an unaligned pointer, via `TTrack::noteOn` → `noteOff`. `TNoteMgr::unk0[]` holds a bad pointer.
  2. `TChannelMgr::getListHead` reads a non-canonical pointer in `TTrack::mainProc` (`mChannelUpdater` list head).
  Both are TTrack memory corruption; I did not find the writer.

## Where boot is now (real D3D12 renders, `port/build_aurora/aurora_*.png`)
- Nintendo logo → intro movie (no picture yet: w8 owns THP decode) → title logo (`aurora_title_logo.png`) → title menu and options (`aurora_title_menu.png`) → file select with Mario on the beach (`aurora_file_select.png`).
- New file → opening movie → Delfino airstrip loads, then stops at the w6 XF fatal above.
- `--warp 1,0` loads Delfino Plaza, which stops at the w6 WGSL fatal.
- The "jumps back to the save select" report: the opening movie plays (the rumble table works after fix 11), but THP frames are not drawn yet, so the last file-select frame stays on screen.

## THP decode (movie video)
- **Symptom:** movies "played" (state machine, timing and rumble cues ran) but the screen kept the previous frame: `sdk_thp.cpp` decoded nothing and `THPPlayerDrawCurrentFrame` drew nothing.
- **Fix:**
  - New `port/src/port_thp_decode.cpp` (`port_thp_video_decode`): a self-written baseline JPEG decoder (Huffman with a 9-bit fast table, float separable IDCT) for THP video frames.
    It follows `THPDec.c`'s bitstream rules: markers SOI/DQT/SOF0/DHT/DRI/SOS (APPn/COM skipped), 3 components with Y 2x2 and U/V 1x1 sampling,
    no 0xFF00 byte stuffing in the scan, and a restart interval only byte-aligns and resets the DC predictors (no RSTn bytes).
    Output is `THPVideoDecode`'s: Y (w x h) and U/V (w/2 x h/2) as GX_TF_I8 8x4-texel tiles of 32 bytes, plain JPEG samples (U/V centred on 128).
  - `sdk_thp.cpp`: Prepare starts a host decoder thread (`std::thread`, created inside `port_host_alloc_push`) standing in for the SDK's read + video decode threads, and waits for the first frame (PrepareReady).
    It walks the frame chain like `THPRead.c` (first word of each frame = next frame size; looping goes back to `movieDataOffsets`), reads into `readBuffer[0]` (or uses `movieData` for on-memory movies) and decodes into a free texture set.
    PlayControl pops decoded sets at the frame-rate clock as before; replaced sets go to a used queue that `THPPlayerDrawDone` (after `GXDrawDone`, which drains Aurora's FIFO) / `THPPlayerPostDrawDone` returns to the decoder, as in the SDK.
    `SMS_PORT_THP_SPEED` skipping moved into the decoder (it still always lands on the last frame of a non-looping movie).
    A decode error is logged instead of setting `videoError` (which would end the movie with state 5).
  - `THPPlayerDrawCurrentFrame` (Aurora build only) issues `THPDraw.c`'s `THPGXYuv2RgbSetup` / `THPGXYuv2RgbDraw` / `THPGXRestore` through GX: three I8 textures and the four TEV stages with the SDK's constants (C0 = {-90, 0, -114, 135}, K0/K1/K2), so Aurora does the YUV to RGB conversion.
    `TModelGate` binds the texture sets directly as J3D textures; they now hold real I8 data in the layout it expects.
  - Logs: `[port] THP open <file>: WxH, N frames ...` and `[port] THP stop: state S, frame F of N`.
  - Audio: not decoded or mixed (the port has no host audio output yet; TODO in `sdk_thp.cpp`). Movies with audio run on the frame-rate clock with `curAudioNumber` following the video.
- **Test:** `python port/tools/test_thp.py --movie openingA --frames 0,150,600 --all` builds `port/tools/test_thp.cpp` with the decoder, decodes every frame (fails on any error) and writes PNGs of the listed frames
  (untiled, converted with the TEV stage constants) to `port/build/test_thp/`.
  openingA (2049 frames), Entrance (2816) and EX128x144_q0 (300) decode without errors at about 0.6, 0.9 and 0.06 ms per frame.
- **In game (Aurora):** the boot intro (`Entrance.thp`) and the new-game opening (`openingA.thp`) show real video (`port/build_aurora/thp_intro_entrance_850.png`, `thp_intro_entrance_1001.png`, `thp_opening_openingA.png`).
  Headless with `SMS_PORT_THP_SPEED=8`, Entrance reaches its last frame (2815) and state 3.
- **Seen on the way (not THP):** every other presented frame is black. The game renders at 30 Hz and waits two retraces per frame; `port_main.cpp`'s `VIWaitForRetrace` ends and presents an Aurora frame on every retrace, so the second present of each game frame is empty
  (consecutive shots 600/601/602/603: black, picture, black, picture; the Nintendo logo too). `SMS_PORT_SHOT` therefore needs pairs of adjacent frames. Likely fix: only end/present the Aurora frame when the game copied a new frame (VISetNextFrameBuffer/VIFlush) since the last present.

## Where boot is now
- The Nintendo logo, the Dolby screen and the intro movie (THP) render: see `port/build_aurora/shot_30.png`, `shot_120.png` and `shot_500.png`.
- With `SMS_PORT_THP_SPEED=8`, boot reaches the title stage (`APP_STATE_GAMEPLAY`, area 15/0): `setupObjects` completes, and particles, rails, J3D models and the event script all load.
  It runs for more than 4000 frames without crashing, and cycles title 竊・attract movie (state 6) 竊・title.
- One run before fix 10 captured the title-stage sky with seagulls (`shot_1300.png` from that run, now overwritten).
  Since fix 10 every title-stage capture is black, so the next thing to look at is rendering: the fader, EFB copies (`GXCopyTex`), and `GXPeekARGB`.


# Input, window, frame pacing (w9)

## W9-1. Every other presented frame is black (flicker)
- **Symptom:** consecutive `SMS_PORT_SHOT` captures alternate picture/black (Nintendo logo, Dolby screen, title).
- **Cause:** frame pacing in the port layer.
  The game draws at 30 Hz and waits two VI retraces per frame (`JDrama::TVideo::waitForRetrace(2)`), but `VIWaitForRetrace` ended and presented an Aurora frame on every retrace.
  Aurora presents its EFB (its `GXCopyDisp` does nothing), and the second Aurora frame of each game frame had no drawing in it, so it presented a cleared, black EFB.
  On the GameCube the VI keeps scanning out the last XFB until the game copies a new one.
- **Fix:** `port_main.cpp` presents only when the game has called `GXCopyDisp` since the last present.
  `build_aurora.py` renames game/JSystem calls of `GXCopyDisp` to `port_GXCopyDisp`, which sets the flag and forwards.
  On other retraces the previous image stays on screen, and only `SDL_PumpEvents` runs, so the window stays responsive and the open frame is not touched.
  A close request still exits if no frame has been presented for 60 retraces.
  Retrace timing and `gPortFrameCount` (the `SMS_PORT_SHOT`/`SMS_PORT_PADSCRIPT` frame numbers) are unchanged: one count per main-thread retrace.
  Until the first `GXCopyDisp`, and in the `SMS_PORT_GXTEST` smoke test, every retrace presents as before.
  `SMS_PORT_PRESENT_ALL=1` restores the old behaviour for comparison.
- **Screenshot fix:** `SMS_PORT_SHOT` captured `FindWindowA("Super Mario Sunshine")`, which can be another running copy (other agents' runs, off-screen `--nullgfx` windows).
  It now captures this process's own SDL window (its `HWND` from the SDL window properties).
- **Evidence:** shots 180-240 (Dolby fade out/in) step smoothly, two identical captures per game frame, with no black frame in between.
  Shots 400-407 (Nintendo logo) and 1600-1605 and 2400-2403 (title stage, `SMS_PORT_THP_SPEED=8`) are all picture frames.
  With `SMS_PORT_PRESENT_ALL=1` the same run alternates (e.g. 400 black, 401 logo, 402 black, 403 logo).


# Static audit (w10)

Proactive sweep for the three crash classes (unswapped big-endian data, 64-bit layout, clang UB), plus bitfield order.
Tools:
- `python port/tools/build_aurora.py --warnings [path-prefix]`: no build; syntax-checks every unit with the exact Aurora flags plus `-Wreturn-type -Wuninitialized -Wsometimes-uninitialized -Wpointer-to-int-cast -Wint-to-pointer-cast -Warray-bounds -Wshorten-64-to-32 -Wreturn-stack-address -Wunsequenced -Winteger-overflow ...` (`WARN_FLAGS`) and writes `port/build_aurora/warnings.txt` (about 40 s for all 583 units).
- `python port/tools/disc_inventory.py`: every file format on the disc; the format 竊・loader 竊・status table is in ENDIAN.md "Disc format inventory".
- `python port/tools/test_assets.py`: `.pad` and `.bcr` through the real loaders.

## A1. CodeWarrior semantics as compiler flags (build_aurora.py, game/JSystem units)
`-fcheck-new` (MWCC null-checks `new` before the constructor; JKRHeap's `new` returns null when full, see fix 1), `-fwrapv` (signed overflow wraps on PowerPC), `-fno-strict-return` (a non-void function that falls off its end returns garbage instead of being "unreachable", the JASDvd/`setupThreadFunc` class), `-fno-delete-null-pointer-checks`.
Strict aliasing is already off (clang's MSVC target defaults to relaxed aliasing; checked: no TBAA metadata).
Checked: title loop, `--warp 1,0`, `2,0`, `2,5` run 3000+ frames without a crash.

## A2. `TSpineEnemy::walkToCurPathNode` / `zigzagToCurPathNode` read an uninitialised local (every enemy that walks a graph)
- **Cause:** decomp transcription errors in `src/Enemy/enemy.cpp`: `if (fVar3 == 0.0f)` tested the variable being computed instead of `MsSin(turn_speed)` (asm: `fcmpu f0, f1` on the sin result), and in `zigzag` the turn amount `fVar2` was never set when the node is near (the GC code copies the angle difference first, `fmr f5, f4`).
  clang may fold either read to anything.
- **Fix:** both corrected (plain source fixes in a NonMatching unit; match goes up: walk 95.9 竊・97.8 %, zigzag 93.9 竊・94.7 %).

## A3. `TMario::slideProcess` reads `turn` uninitialised when the slide has (almost) stopped
- On GC `turn` lives in r31, which its only caller (`doSliding`) holds at 0 (`stopped`); `turn = 0` under `SMS_PORT`.

## A4. JGeometry `TVec3` `operator+`/`operator-` (friend, by-value first argument) and `TVec2::operator+/-` return a reference to a dead object
- `TVec3`'s return a reference to their by-value parameter (clang 22 marks such arguments `dead_on_return`); `TVec2`'s return a reference to a local.
  Used for nearly every `a - b` in game code. Return by value under `SMS_PORT` (`JGVec3.hpp`, `JGVec2.hpp`).

## A5. `.pad` input replays read in place (Shadow Mario chases)
- `port_swap_pad` in `TMarioInputReplay::init` (`src/Player/MarioRecord.cpp`), tested on all 381 files of 20 scenes.

## A6. TEX1 ResTIMG headers whose pad byte is 0x5A on disc were never swapped (Corona boss)
- See ENDIAN.md "TEX1 pad-byte marker": `port_swap_tex1_timgs_force` inside `port_swap_j3d_model`.

## A7. Bitfield order / byte-order overlays (sweep of all src/ and libs/)
MWCC allocates bitfields from the most significant bit, clang/x86 from the least; byte overlays of multi-byte integers flip on little-endian.
- `JASChannel.hpp` `TChannel::MixConfig`: `u16 mWhole` written as numbers (0x150, 0x210, ..., bank data `unk4E[]`) and read through `{u8 u; u8 l0:4; u8 l1:4}`. **Fixed** (fields reversed under `SMS_PORT`: `l1:4, l0:4, u`).
- Already fixed by w7: `JASSeqParser.cpp` note-on command bits, `JASChannel.cpp` `asP` hi/lo union.
- Safe (only accessed by name, never overlaid): `JAIBasic::unk1C` flags, `MuteBit`.
- SDK/MSL bitfields (`PPCArch.h`, `OSCache.c`, `SIBios.c`, `ansi_files.h`): not compiled for PC / not used by game code. No GX register bitfields exist in the headers the port uses (Aurora's own).
- Other byte views checked and safe: `JUtility::TColor` and `J3DTevStage::load` (already `SMS_PORT`), `J3DCluster` DL reads (`sms_be16`), `spcinterp` fetches, `JAIBasic` init data, `PacketUtil`/`JRenderer` float bits.

## Open items found by the audit (not fixed; owners)
- `JASTrack.cpp` `TTrack::mainProc` (w5): the `goto bail` path ORs an uninitialised `r31` into `unk3B4`. On GC r31 is the caller's: for child tracks that is `mainProc`'s loop offset `i*4`, for the root track whatever the player held. Needs a GC-faithful value.
- `spider.cpp` `TSpider::bind`: `unaff_f29` (y offset) is the caller's f29 on the "touching a wall but moving away" path; the GC value comes from far up the call chain. Float only, no crash.
- `SelectShine2.cpp` `TSelectShine::move`: `height` uninitialised for the one frame where `mBobPhase == 4.0f` (visual).
- `JAIGFrameStream.cpp` (w8): `needDecode` read when `movieframe == 0` (result is the same either way through `||`).
- `--warp 4,0` (Ricco, w11): Aurora fatal "unhandled tcg src 21" at about frame 1200 (texgen source 21 is not a valid `GXTexGenSrc`; check whether a material/texgen value is misread).


# Audio (w5)

Sound now comes out: JAudio's DSP voices are rendered on the host and the AI DMA buffers go to SDL3.
Files: `port/src/sdk_audio.cpp` (AR/ARQ/AI/DSP entry points and the DSP mail protocol), `port/src/port_audio_dsp.cpp` (host "microcode"),
`port/src/port_audio_out.cpp` (SDL3 output, WAV capture, extra PCM stream), `port/src/port_audio.h`.

How it works:
- JAudio drives the DSP over the mailbox exactly as on the GameCube (`dsptask.c`, `dspproc.c`, `JASDSPBuf.cpp`): `0x81` DsetupTable (voice blocks, tables, FX lines),
  `0x82` DsyncFrame (sub-frame count, left/right frame buffers, output level), then one `DSPReleaseHalt` per sub-frame (7 x 80 samples = one 560-sample frame).
  `sdk_audio.cpp` decodes the mails; each `DSPReleaseHalt` renders one sub-frame synchronously and then answers `0xFF00` through JAudio's `__DSPHandler`, so the
  audio thread's frame loop is unchanged.
- The voice blocks (`JASystem::DSPInterface::DSPBuffer`) and FX lines (`FXBuffer`) are written by natively compiled game code, so the renderer reads them through the
  game's own structs (native order, x64 layout). The resampling/ADPCM tables are static arrays above 4 GB in the exe, so their 32-bit mail addresses are truncated:
  the renderer takes them from the symbols (`DSPRES_FILTER`, `DSPADPCM_FILTER`).
- Per voice: AFC ADPCM from ARAM (9-byte frames of 16 4-bit samples; 5-byte 2-bit frames) with JAudio's coefficient table, PCM8/PCM16 (big-endian) from ARAM,
  PCM16 from main RAM in a circular buffer (source `0x21`, JAI streams; the renderer keeps `currentPosition`/`samplesBeforeLoop`/`remainingLength` up to date,
  JAIGFrameStream reads them), 64-sample oscillators; loops with the stored ADPCM history; pitch = 4.12 step resampled with JAudio's 4-tap polyphase table;
  FIR (outer/"muffle" filter), IIR (pass-through unless a sequence animates it), distance low-pass; per-bus volume ramps over the sub-frame, or the auto-mixer
  (volume/pan/FX send) form; pause; end requests (ramp to 0, then `done`). Four FX lines (delay buffer in main RAM, 8-tap FIR feedback, two sends).
  Envelopes, vibrato and sequencing are CPU-side JAudio and already ran.
- Output level: DsyncFrame's level is applied as 2.14 fixed point (0x4000 = unity; SMS sends 0x5000 = x1.25). Read as 4.12 (x5) the boot jingle and the title
  ambience saturate constantly; as 2.14 the title stage peaks just under full scale. This is the one tuned constant (see the comment in `port_audio_dsp.cpp`).
- AI: the DMA interrupt (560 frames at 32 kHz = 17.5 ms) stays the game's audio clock. At each tick the buffer that starts playing goes to
  `port_audio_out_ai` (AI order is right/left; swapped for SDL). The next period is trimmed by up to +-2 % in 0.5 % steps to hold the SDL queue near 3 buffers.
- Knobs: `SMS_PORT_AUDIO=0` (no device), `SMS_PORT_WAV=<path>` (record the AI output, 16-bit stereo 32 kHz), `SMS_PORT_AUDIO_LOG=1` (DSP setup, per-voice start
  lines, periodic voice/queue/underrun stats, JAI sound requests), `SMS_PORT_AUDIO_TRACE=<voice>` (per-sub-frame pitch/volume of one voice).
- For w8 (THP movie sound): `port_audio_submit_stream(samples, frames, rate)` queues interleaved stereo s16 (left, right) at any rate on a second SDL stream
  mixed into the same device; `port_audio_clear_stream()` drops it. (The original THPPlayer mixes through `JASystem::Kernel::registerMixCallback(.., 3)`
  instead; that path also works now, since `vframeWork` runs the mix callback into the real DAC buffer.)

Test evidence (2026-10-08, `SMS_PORT_THP_SPEED=8`, 75 s from boot, `SMS_PORT_WAV`):
- 0-1.5 s boot jingle (peak 29138), 20-65 s title stage: title BGM (sequence `0x80010010`, chords of one looped instrument at pitches 0x0d75/0x11f6/0x155c =
  4:3 and ~8:5 ratios) plus the looping wave/seagull ambience (`0x5003-0x5005`, auto-mixer voices). RMS -25..-35 dBFS, 373 clipped samples in 75 s
  (all in the boot jingle), onset autocorrelation peaks at 88/118/171 BPM. PCM16 SE data checked big-endian (sample-to-sample smoothness 0.67-0.83 vs 1.32 swapped);
  the boot jingle decoded offline from `w1stLoad_0.aw` with the same AFC rules is a clean waveform (rms -13 dBFS, full-scale peaks), which is why the
  output level was taken as 2.14.
- Normal-speed 150 s `build_aurora.py --run`: 8400 AI buffers, 0 underruns, 0 queue drops, queue steady at ~1450-1770 frames (45-55 ms), no crash or hang.
- Not heard in gameplay yet: with `SMS_PORT_WARP=1,0` Delfino Plaza renders but JAI gets almost no sound requests (one SE, no BGM), so nothing plays there;
  that is game-side (the warp skips the title/file-select flow), not the mixer. Menu sounds were not reached (title screen START did not open the file select in the test run).

## A-1. JAudio bitfields and byte overlays (shared with w7/w10)
- `JASSeqParser.cpp` `cmdNoteOn`: the command-byte bitfields were read in clang's LSB-first order, so the voice index came from an arbitrary register and
  `TNoteMgr::unk0[voice]` wrote past `TNoteMgr` into `mChannelUpdater` (the intermittent `updatecallLogicalChannel` / `getListHead` audio-thread crashes).
- `JASChannel.hpp` `MixConfig` and `JASChannel.cpp` `playLogicalChannel` `asP` union: bus numbers / mix curves read from the wrong byte (voices routed to bus `0x50`).
## A-2. `.afc` stream header and PCM stream data are big-endian
- `JAIGFrameStream.cpp` `__start`: header fields read with `sms_be32/16`; `__DecodePCM` swaps the PCM16 samples. (Only `title.afc` exists, ADPCM.)
## A-3. `TTrack::mainProc` `goto bail` ORs the caller's r31 into `unk3B4`
- GameCube asm: at `bail` r31 has not been set by `mainProc`, so it is the caller's: `i*4` from the parent's child loop, or `i*0xC` (the `callList` entry
  offset) from `Kernel::subframeCallback` -> `rootCallback` (which never touches r31). Reproduced under `SMS_PORT` with `sPortEntryR31` (JASTrack.cpp) and
  `Kernel::gPortCallbackR31` (JASCallback.cpp). DOL unchanged.

Left to do (audio): THP movie audio (w8, API above); the exact DSP output scaling and the IIR/FIR coefficient semantics are reconstructions;
Dolby surround (back buses are folded into L/R); `JAIBasic` debug request log is `SMS_PORT_AUDIO_LOG` only.

# Memory card (w5)

Saves work through Aurora's CARD (GCI-folder mode): `<user dir>/USA/Card A/01-GMSE-super_mario_sunshine.gci`, a 0x40-byte GCI directory entry plus the
0xE000-byte save file exactly as the game writes it. The user dir is Aurora's `userPath`: by default SDL's pref path `%APPDATA%\Super Mario Sunshine\`
(same folder as Aurora's caches/imgui.ini); `SMS_PORT_USER_DIR=<dir>` moves it (test runs use it so they never touch the player's save).
- The boot log line `[aurora ERROR: aurora::card] Failed to open file: super_mario_sunshine` only means there is no save yet: the game's
  `readOptionBlock` gets `CARD_RESULT_NOFILE` and offers to create the file, as on a fresh GameCube card.
- `CARDUnmount` in Aurora is a stub returning `NOCARD`; the SDK returns `READY`, and `TCardLoad` reads that status back after `unmount()`.
  `CardManager.cpp` (under `SMS_PORT_AURORA`) calls `port_CARDUnmount` (`port_card.cpp`), which returns `READY` while the card folder exists.
- The save blob is native-order on PC (TFlagManager writes raw, see ENDIAN.md), so a PC save is not interchangeable with a real GameCube save (checksums and
  flag words would read wrong); the GCI container itself is standard.
- Self-test: `SMS_PORT_CARDTEST=1` runs `port_card_test()` instead of the game: the game's own `TCardManager` (worker thread, probe/mount, create,
  option block, save slot 2 written twice through the alternating copies with `TFlagManager::save` data + a timestamped marker, unmount, read back,
  bookmark scan). Two runs in a row: first run creates the file and PASSes; the second reports the first run's marker from disk, then PASSes
  (`read-back matches`, `unmount -> 0`). Real in-game save-after-a-Shine was not driven (gameplay input path not reachable unattended yet).



# Rendering (Aurora build, w6)

Test runs: `SMS_PORT_SHOT` (now a GPU readback, R0) plus `SMS_PORT_WARP`, `SMS_PORT_PADSCRIPT` A presses to get past intro dialogs, `SMS_PORT_THP_SPEED=8` for the title.
Screenshots: `port/build_aurora/final/` (title 1800/2700, airstrip 1000/1400, plaza 1950/2550) and `port/build_aurora/stages/` (one run per area).

## R0. EFB readback: GPU screenshots and `GXPeekARGB` (handed over to w8)
Status: written and running in w6; handed over to w8 (EFB features owner) as is. w6 makes no further edits to these functions.
- **Files:** `port/src/port_aurora_sdk.cpp` ("EFB readback" block: `efbRequestRead`, `efbReadEncode`, `efbReadSubmitted`, `efbReadComplete`, `efbPeekARGB`, `port_efb_screenshot`, `port_VIConfigure`, `GXPeekARGB`, `GXPokeAlphaRead`),
  `port/src/port_main.cpp` (`shotListed`, `maybeScreenshotEfb`, called in `VIWaitForRetrace` right before `aurora_end_frame` of a presented frame),
  `port/tools/build_aurora.py` (`VIConfigure` -> `port_VIConfigure` rename; Dawn include dir and `-std=gnu++20` for the files in `PORT_SRC_CXX20`).
- **How:** only Aurora's public API (`aurora/gfx.hpp`), no Aurora patch.
  `resolve_pass({color})` drains the FIFO and snapshots the EFB colour target at the current point of the command stream; an encoder task (`push_encoder_task`) copies a rectangle into a MapRead buffer; its after-submit callback maps it.
  Results arrive 1-3 frames later.
- **Screenshots:** `SMS_PORT_SHOT=<frame>,...` reads the EFB back from the GPU (render-target size, 1280x960 BMP), so covered, minimised or off-screen windows and other running copies no longer spoil shots.
  With w9's present-on-`GXCopyDisp` pacing, a listed frame that presents nothing is captured at the next present (same image as on screen).
  `SMS_PORT_SHOT_WINDOW=1` restores the old window capture.
- **`GXPeekARGB(x, y)`:** each call queues a one-pixel readback at that point of the frame (logical EFB coordinates mapped like `AURORA_VIEWPORT_FIT`, logical size from the game's `VIConfigure`) and returns the newest completed value for that pixel, or for the last peeked pixel if it moved.
  `GXPokeAlphaRead` is honoured (the game sets `GX_READ_NONE` in `Application.cpp`).
  The caller is `TMario::drawSyncCallback` (occlusion probe: alpha 0x10 under Mario means visible); our `GXSetDrawSync` calls the callback synchronously, so the peek happens right after the probe cube, as on hardware, but its answer lags 1-3 frames.
  Not verified in gameplay yet (whether the probe's dst-alpha write survives Aurora's alpha-update handling is untested).
- `GXPeekZ` is Aurora's own (end-of-frame depth snapshot at 30 Hz); `GXCopyTex` is Aurora's own `copy_tex` (keyed by destination pointer); `GXCopyDisp`/`GXSetCopyFilter` are Aurora no-ops. w6 changed none of these.

## R1. Airstrip: Aurora FATAL `indexed XF load outside array 22`
- **Symptom:** see w7's entry above (Mario's `J3DShape::mVcdVatCmd`, stale bytes after the commands parse as `LOAD_INDX_B` index 0xD97E).
- **Cause:** port layer. On the GameCube `makeVcdVatCmd` fills the whole 0xC0 buffer: VCD 21 bytes + 12 arrays x (base + stride CP writes, 12 bytes, also for unused arrays via `GDSetArrayRaw`) + VAT 18 bytes = 183, padded to 0xC0 by `GDPadCurr32`, and `draw()` calls exactly 0xC0.
  On Aurora the buffer is 0x180 (`kVcdVatDLSize`) and an unused array writes only its stride, so the list ends early and `draw()` executed the uninitialised tail of the `new u8[]` buffer.
  Real hardware never sees such a tail, so this is not a GC tolerance question.
- **Fix:** `J3DShape::makeVcdVatCmd` (`libs/JSystem/src/J3D/J3DGraphBase/J3DShape.cpp`, `SMS_PORT`) pads the rest of the buffer with `GX_NOP` (0) after `GDPadCurr32`.
  GameCube DOL unchanged (`shasum` OK).
- **Result:** airstrip (`--warp 0,0`) runs through the Peach dialog into gameplay without a fatal (`final/airstrip_shot_*.png`).

## R2. Plaza/airstrip: WGSL `cannot index into expression of type 'f32'`
- **Symptom:** the WGSL line was `prev.a = clamp(select(0.0, ..., round(tev_overflow_f32(tevreg1.a).r * 255.0) > round(sampled0.a.r * 255.0)) + 0.0, 0.0, 1.0);` (Dawn prints the source line, so no `EnableDebugPrints` build was needed).
- **Cause:** Aurora shader generator. For an alpha combiner in compare mode `GX_TEV_COMP_R8_*`, `GR16_*` or `BGR24_*`, `tev_op` swizzles `.r`/`.rg`/`.rgb` on its A/B inputs, but the alpha path passed the scalar alpha inputs.
  On hardware these alpha compare modes compare the **colour** combiner's A and B inputs (only `A8` = `RGB8` compares alpha); Dolphin's `tevin_a`/`tevin_b` carry the colour inputs in rgb and the alpha input in a.
- **Fix:** Aurora patch `port/patches/aurora_shader.patch` (`lib/gx/shader.cpp`): for those ops the colour stage's A/B inputs are captured into `tevN_ca`/`tevN_cb` before the stage's colour op writes its output register, and the alpha compare uses them.
- **Result:** Delfino Plaza (`--warp 1,0`) loads and plays (`final/plaza_shot_2550.png`).

## R3. Warps 4,0 (reported by w10), 8,0 (Pianta Village) and 9,0 (Noki Bay): Aurora FATAL `unhandled tcg src 21`
(In w6's runs warp 3,0 is Ricco Harbor and 4,0 Gelato Beach.)
- **Cause:** not a misread value. 21 is `GX_MAX_TEXGENSRC`, Aurora's "never configured" default for texcoords at or beyond `numTexGens` (only the first `numTexGens` texgens are copied into the shader config).
  The failing state (logged now): one TEV stage, `tc0 map0` with indirect matrix `GX_ITM_0`, indirect stage 0 on texcoord 1, but `numTexGens` = 1.
  That is the JPA indirect-texture particle setup (`JPADrawSetupTev::setupTev` case 1, a matching unit) with the texgen count lowered later in the same state; the GameCube draws it the same way.
- **Fix:** Aurora patch (same `aurora_shader.patch`): a sampled texcoord that was never generated reads (0, 0), the hardware behaviour as modelled by Dolphin, with a warning that lists the stages.
- **Result:** warps 8,0 and 9,0 run (`stages/a8_*`, `stages/a9_*`); 4,0 ran 3000 frames without the warning in this build's run.

## R4. Visual survey (D3D12, 1280x960)
- Title stage (3D Delfino behind the logo: sky, sun with rays, clouds, seagulls, sea): correct; the `BOOTLOG` "title stage went black" note above is obsolete (it was the black alternate frames fixed by w9, plus window-capture shots).
- Airstrip: Peach, Mario (blue overalls, red shirt, white gloves), the plane, its projected shadow, sea, sand, goop puddle with the swirl texture, coins HUD: correct.
  The "overalls black" from the early gxtest does not happen in game.
- Plaza, Bianco (2,0), Ricco (3,0), Gelato (4,0), Pianta (8,0), Noki (9,0): geometry, textures, transparency, water and HUD look right.
  The first frames after a stage loads miss geometry while Aurora compiles pipelines asynchronously (test runs start with an empty pipeline cache); later frames are complete.
- Not yet compared closely with real-hardware captures: lighting intensity of night/dusk stages (Pianta looks bright), Mario's dirt layer (`H_ma_rak` swap; the striped overalls seen once were the spin-jump effect), FLUDD's water tank (likely uses EFB copies, w8).
- Intermittent, not rendering: `D3D12 create graphics pipeline state failed with E_OUTOFMEMORY` (seen in Sirena 6,0, 7,0 and once in 8,0, while other agents were running several game copies on the same GPU; not reproduced in a rerun of 8,0).
  JAudio crashes (w5) stopped after taking w7's JASChannel/JASSeqParser fixes (now in main).



## W9-2. Settings file, controller semantics, keyboard, window, overlay
New files: `port/src/port_settings.h/.cpp`, `port_input.cpp` and `port_window.cpp`, all inside `#ifdef SMS_PORT_AURORA`.
`port_main.cpp` gets four one-line hooks, each marked `w9:`:
- `port_settings_configure(&config)` before `aurora_initialize`;
- `port_window_frame()` in both branches of `VIWaitForRetrace`;
- `port_input_read()` in `port_PADRead`.

- **Settings:** `port_settings.ini` next to the exe (`SMS_PORT_SETTINGS=<path>` overrides it).
  It is created with commented defaults.
  Window size, position, maximized and fullscreen are saved 1 s after they change, and at exit.
  A saved position on no connected display is ignored.
  Before this, the window always opened at (0,0): `AuroraConfig` was zeroed, and Aurora treats 0 as a real position.
- **Sticks:** Aurora adds a 24% per-axis dead zone before `PADClamp`, which snaps the stick to the axes within about 14ﾂｰ.
  It is now `[input] stick_deadzone`/`cstick_deadzone`, default 0, so only the GameCube's own `PADClamp` dead zone (15 counts) applies.
  The DualSense rests at ﾂｱ4, which `PADClamp` turns into 0 (`SMS_PORT_PADLOG=1` shows raw and clamped values).
  Aurora's `PADClamp` already scales a full ﾂｱ127 to the GameCube octagon (72 on the axes, 40/40 on the diagonals).
- **Analog L/R:** Aurora maps the trigger axis onto 0..255, so the clamped analog value (0..150) saturated at 70% of the pull.
  The rest, up to Aurora's emulated click at 95%, did nothing.
  With `linear_triggers` (default), the travel up to the click maps linearly onto raw 30..180, i.e. 0..150 after `PADClamp`.
  The click itself is the digital L/R at full analog, as on a GameCube pad.
- **Buttons:** with `layout = gamecube` (default), `PADSetDefaultMapping` is called before `PADInit` for PS3/PS4/PS5/standard/Xbox pads.
  Buttons go where a GameCube pad has them: PlayStation Cross A, Square B, Circle X, Triangle Y, R1 Z, L2/R2 analog L/R, Options Start.
  Xbox: A A, X B, B X, Y Y, which was already Aurora's Xbox layout.
- **Rumble:** goes through Aurora's `PADControlMotor` to SDL rumble; `[input] rumble` sets the strength (default 50%, Aurora's own default).
- **Keyboard pad** (on top of a controller; controller 1 when none is connected):
  - WASD = main stick; Left Shift held = soft (45% tilt, and light analog L/R without the click);
  - arrow keys = C-stick;
  - Space = A, E = B, Q = X, F = Y, Z = Z, C = L, R = R, Return = Start (not while Alt is held);
  - I/J/K/L = D-pad.
  Every key can be remapped in `[keyboard]` with SDL key names.
  `SMS_PORT_KEYSCRIPT=<frame>:<key>[+key],...` holds keys for 10 frames, for unattended tests.
- **Window:**
  - Alt+Enter / F11 toggles borderless fullscreen; F3 toggles the overlay.
  - `aspect = 4:3` (default): the windowed client area is locked to 4:3 and the picture fills it.
  - When the client area is not 4:3 (fullscreen, maximized), Aurora's FIT policy letterboxes, at the EFB's 640:448 shape.
  - `port/patches/aurora_display_aspect.patch` (for w6) implements Aurora's declared-but-missing `VILockAspectRatio`.
    The port already calls it through a link-time fallback (`/alternatename`), so once the patch is applied, fullscreen is exactly 4:3 too.
  - `internal_scale` goes to `VISetFrameBufferScale` (0 = window resolution, 1 = native 640x448, ...).
- **Overlay (F3):** text through Aurora's ImGui foreground draw list, showing game fps, present interval (avg/max) and VI rate.
  ImGui is declared locally, because imgui.h is not on the port include path.
- **Frame statistics:** `SMS_PORT_FRAMESTATS=<seconds>` or `[debug] frame_stats` prints the VI rate, main-thread retrace gaps and present intervals.


## W9-3. Frame pacing measurements
- **Windowed, title/attract loop, 10 s windows:** VI 59.940-59.942 Hz (no drift), 29.97-30.07 presents/s.
  Present interval 33.3 ms, sd about 1.0 ms, max about 36 ms; the only spikes (50-67 ms) happen during stage loads.
- **Precise sleep:** `sdk_os_thread.cpp` `irqThread` now sleeps 1 ms less than needed and spins (`SwitchToThread`) for the rest (marked `w9`).
  Before, a 1 ms late wake-up delayed the retrace.
  Main-thread retrace gap max: 20.1-21.6 ms before, 18.9-20.0 ms after (ideal 16.68).
  Cost: about 1 ms of spinning per retrace on the interrupt thread.
- **Minimized** (`ShowWindow(SW_MINIMIZE)` for 8 s): the game keeps running at full speed, about 30 presents/s, and the frame counter keeps pace with the retrace count.
  Jitter is higher while minimized (present sd about 5 ms, a few 30 ms retrace gaps).
  It never speeds up or slows down, and nothing is logged.
- **Unfocused:** keeps running; Aurora's `pauseOnFocusLost` is off.
  The controller is ignored while unfocused unless `[input] background_input = 1` (SDL default).
- **High-refresh monitors:** the game is paced by the VI interrupt (59.94 Hz), not by vsync.
  Presents use Dawn's Mailbox mode (vsync off, no tearing), so the game cannot run faster on a 144 Hz display.
  On a 60 Hz display, 59.94 vs 60 Hz repeats one frame about every 17 s.


## W9-4. Test runs no longer open the player's controller
`SMS_PORT_NO_PADS=1` turns off every SDL joystick backend (HIDAPI, RawInput, WGI, DirectInput, XInput, GameInput) and ignores all gamepads (`SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT`).
It is set in `port_settings.cpp` before SDL starts.
Test instances then neither read nor rumble the player's pad; the keyboard, `SMS_PORT_KEYSCRIPT` and `SMS_PORT_PADSCRIPT` still work.
`build_aurora.py --run` sets it unless `--pads` is given.
Checked: with it set, the DualSense is no longer added; without it, it is.


## W9-5. Disc images without extraction (`port_disc.cpp`, hook in `sdk_dvd.cpp`)
- **Formats:** reads the US disc in place from `.iso`/`.gcm` and from CISO (`.ciso`).
  It parses the disc header (game ID, magic 0xC2339F3D, FST offset/size at 0x424) and the FST itself, so entry numbers are in disc order.
  Reads past a file's end return the following disc bytes, as on hardware.
  RVZ/WIA/GCZ are recognised, and the message says how to convert them to ISO in Dolphin; RVZ needs zstd/LZMA plus Dolphin's junk-data generator, so it is left for later.
- **Region check:** the game ID must be GMSE01.
  Other discs get a message box that names the region (Japan GMSJ01, Europe GMSP01, other GameCube game).
  Extracted folders are checked through `sys/boot.bin` next to `files/` when it exists.
- **Search order:**
  1. a path on the command line;
  2. `%SMS_PORT_DISC%`;
  3. `[game] disc =` in port_settings.ini;
  4. auto-detection: next to the exe (folder, or `*.iso/*.gcm/*.ciso`), `./files`, then a walk up from the exe for `dolphin_test\files` and `files`.
  A disc named explicitly (1-3) that is unusable stops the search with its reason, instead of silently picking something else.
  If nothing is found: a message box explaining what is needed, then SDL's file-open dialog; the choice goes to `disc =`.
- **`sdk_dvd.cpp` (w7) hook:** under `SMS_PORT_AURORA`, `build()` asks `port_disc_locate()`, and `doRead()` reads through `port_disc_read()` for images.
  `Entry` gains `discOffset`. `build_port.py` builds are unchanged.
- **Checked:**
  - booted to the title from `orig/GMSE01/game.iso` and from a CISO made from it (`make_ciso.py` in my scratchpad);
  - the folder path still works;
  - the JP ISO on the command line shows the region message box (`port/build_aurora/w9_wrong_region_box.png`);
  - with no data at all, the "not found" box appears.
  The file dialog itself (after clicking OK) was not clicked through.


## W9-6. In-game settings menu (`port_menu.cpp`)
- **Opening:** F1, or the controller's Back/Create/View button, toggles a Dear ImGui window with tabs:
  - Display: fullscreen, aspect, internal resolution, overlay;
  - Controller: layout, dead zones, linear triggers, rumble, background input;
  - Keyboard: enable, soft tilt, and every binding with Rebind (Esc cancels, Backspace clears);
  - Game: disc path.
- **Applying:** changes apply at once, except the button layout, which applies at the next start.
  They are saved to port_settings.ini.
- **Input:** while the menu is open the game sees idle controllers, and ImGui keyboard/gamepad navigation is on.
- **Code:** ImGui comes from Aurora's build (`imgui-src` on the port include path, `IMGUI_USER_CONFIG` as Aurora builds it).
  The F3 overlay now uses the real ImGui API from here too (`port_window.cpp` no longer declares ImGui itself).
- **Checked:** screen capture of the open menu at the intro movie (`port/build_aurora/w9_settings_menu.png`).
  `SMS_PORT_SHOT` now reads back the EFB, so ImGui is not in those captures.


## W9-8. RVZ, WIA and GCZ disc images (`port_disc_wia.cpp`)
- **What it reads:** Dolphin's compressed formats, read as the plain disc.
  - RVZ: zstd or uncompressed; any chunk size; per-group stored/compressed flag; RVZ packing with regenerated padding.
  - WIA: none, purge, bzip2, LZMA or LZMA2 (RVZ also LZMA/LZMA2/bzip2).
  - GCZ: zlib, including stored blocks.
  `port_disc.cpp` tries it before the ISO/CISO path, so the region check, FST and search order are unchanged.
  `*.rvz/*.wia/*.gcz` next to the exe are auto-detected, and the file dialog lists them.
- **Written from the format description**, not from Dolphin's code:
  - file head, disc struct, raw data and group tables (stored compressed);
  - groups covering the disc from each raw entry's offset rounded down to 0x8000;
  - RVZ group flags and packed size; literal/padding records.
- **Padding generator:** Nintendo's lagged Fibonacci generator (521/32 lags, 17-word seed, byte 1 taken 18 bits down, 4 steps before output).
  It is reseeded at every 0x8000-byte disc block, so a record at disc offset o starts (o % 0x8000) bytes into its stream.
  Before writing the C++ reader I checked my own Python version against the real padding of game.iso:
  - the seed recovered at 40 whole blocks regenerates each block byte for byte, and does not continue into the next block (reseeding confirmed);
  - a padding run starting mid-block regenerates with the in-block offset as skip, and not with a wrong skip (control).
- **Libraries, all static, no new DLLs:**
  - zstd 1.5.7 and zlib from Aurora's build (already linked);
  - bzip2 1.0.8 sources, unmodified, in `port_deps/bzip2-1.0.8`, copied from the local Cargo registry; `build_aurora.py` compiles them (`BZ_EXPORT`).
- **LZMA/LZMA2:** liblzma from xz 5.8.4 (`port_deps/xz-5.8.4`, official release tarball, SHA-256 checked, see its SOURCE.txt).
  Only the raw LZMA1/LZMA2 decoder is compiled (`common.c`, `filter_common.c`, `filter_decoder.c`, `lz_decoder.c`, `lzma_decoder.c`, `lzma2_decoder.c`).
  The configuration comes from `build_aurora.py` defines (`HAVE_DECODER_LZMA1/2`, `LZMA_API_STATIC`, ...); nothing from the tree is run.
  `PORT_DISC_HAVE_LZMA` turns on `lzma_raw_decoder` with the properties from the disc struct.
  DolphinTool-made WIA LZMA, WIA LZMA2 and RVZ LZMA2 (128 KiB) images of game.iso are IDENTICAL over the full disc (`test_disc.py --full`).
  The game boots to the title from the RVZ LZMA2 image; LZMA decodes at about 30-45 MB/s, so boot loads take a few seconds longer than with zstd.
- **Test:** `port/tools/test_disc.py` builds `port/tools/test_disc.cpp` with the reader.
  It compares every FST file and, with `--full`, the whole disc in 4 MiB steps plus 3000 random reads against the ISO.
  Test images were made from `orig/GMSE01/game.iso` with the PC's own DolphinTool (`<project>/Documents\Dolphin-x64`).
  All of these are IDENTICAL over the full 1,459,978,240 bytes:

  | Image | Notes |
  |---|---|
  | RVZ zstd 128 KiB | 1.07 GB |
  | RVZ uncompressed 32 KiB | packing only |
  | WIA bzip2 2 MiB | |
  | WIA none | |
  | GCZ 32 KiB | |
  | The user's own `Godzilla - Destroy All Monsters Melee (USA).rvz` (zstd) | compared with DolphinTool's ISO conversion of it |

  The game boots to the title from the zstd RVZ.
  `SMS_PORT_DISCSTATS=1` showed 0.086 s spent in the reader over about 3,800 boot reads.
  The roughly 3-4 s hitch when the title stage loads happens with the extracted folder too (4.3 s there), so it is not the reader.
  The test images were deleted afterwards.

## W9-9. Input recording and replay (`port_replay.cpp`), determinism fixes
- **Recording** (default on; `[debug] record`, `SMS_PORT_RECORD=0/1`, F1 menu Game tab) runs from boot.
  - The pads as the game reads them (stored only when they change); the clamped values follow from the game's own `PADClamp`.
  - The `srand` seed.
  - Every main-thread `OSIsThreadTerminated` answer (loader threads) and `THPPlayerGetState` answer (movie end).
  - A card snapshot (all files in `<user dir>/USA/Card A`) and the disc identity (game ID, header/FST hash and file name, no paths).
  - The debug settings that change the game (`SMS_PORT_WARP`, `SMS_PORT_THP_SPEED`, `SMS_PORT_NODRAW`), re-applied on replay.
  - Mario's position, action and the rand state every 120 game frames (checkpoints).
  Every event carries its game frame (the PADRead count).
  About 2 KB per minute of play; capped at 64 MB.
- **Whole session, not a ring buffer:** the brief asked for the last N minutes, but a replay can only start from boot (no state snapshots), so the whole session is kept.
- **Saving:** F9, the menu button, a crash (a chained unhandled-exception filter) or an abort/Aurora FATAL (SIGABRT) writes a zstd-compressed `sms_replay_<date>_<time>.bin` next to the exe, i.e. next to `sms_port_crash.log`.
- **Replay:** `SMS_PORT_REPLAY=<file>` does the following:
  - restores the card copy into `%TEMP%\sms_replay_<pid>` (removed at exit) and points `SMS_PORT_USER_DIR` there;
  - ignores physical controllers and the keyboard, and never saves port_settings.ini;
  - feeds the recorded answers in order. A recorded "loader finished" that comes early is safe: every caller joins the thread next;
  - compares the checkpoints and prints the first differences; at the end it prints "N of M checkpoints differ";
  - `SMS_PORT_REPLAY_EXIT=1` exits at the end with status 0 when nothing differs.
- **Nondeterminism found and fixed:**
  1. **rand/srand:** the game's calls went to the host UCRT `rand`, which is a different LCG from MSL's and keeps per-thread state.
     The game objects now call `port_rand`/`port_srand` (renamed in `build_aurora.py`): MSL's single global generator (x * 0x41C64E6D + 12345, bits 16..30).
     This is also a GameCube-fidelity fix: random sequences now match the original's.
  2. **Load-screen length:** the game polls its setup threads with `OSIsThreadTerminated` once per frame, so how many frames a load lasts depends on the PC. Journaled.
  3. **Movie length:** THP movies advance with the VI retrace, and the movie director polls `THPPlayerGetState` per frame. Journaled.
  4. **EFB readbacks (port_efb.cpp, w8's file, marked w9):** results arrived "1-3 frames later" depending on GPU timing.
     Bianco Hills' goop collision reads them, so the first replay matched for 1700 game frames and then drifted by 2-15 units (same rand state).
     While recording or replaying, results are now applied exactly 3 game frames after the request, in request order, waiting for the GPU (pumping Dawn's `ProcessEvents`) if it is later.
  5. **Bug found on the way:** a replay used to switch `keyboard` off in the settings, and the exit-time save stored it. Replays now never touch the settings.
  6. **GXPeekARGB (port_aurora_sdk.cpp, w6's file, marked w9):** Mario's occlusion probe (draw-sync callback) feeds the camera; a recording diverged in water at game frame 1800. While recording/replaying, peek N now returns peek N-3's result, waiting for the GPU if needed.
  The EFB delivery counts w8's game-frame clock (`sGameFrame`, E18), so interpolated display frames (W9-10) do not shift it.
- **Test:** `SMS_PORT_WARP=2,0` (Bianco Hills), `SMS_PORT_THP_SPEED=8`, scripted input (Space through the intro, then about 2 minutes of walking in four directions with jumps and FLUDD spraying, F9 at the end).
  The recording is 1.7 KB for 4560 game frames.
  - Replay: 0 of 37 checkpoints differ; Mario's exact float position, action and rand state are equal at every checkpoint.
  - Replay with 24 CPU-burning processes running: again 0 of 37. Only the retrace counts differ (9109 vs 9112).
  - The final build replays the same file with 0 of 37.
  - w7's `SMS_PORT_TRACE` lines sample on retrace buckets, which shift between runs, so they cannot be compared line by line; the checkpoints are keyed on game frames.
  - Card snapshot: a file in the card folder is captured and restored into the replay's folder.
  - Not exercised: the crash path, and replays across different builds or PCs (a GPU-dependent result other than the EFB readbacks would show up as a checkpoint difference).
- **Overhead:** recording costs nothing measurable. The Bianco load gap (frame 600 at retrace about 1000) is the same with recording off.

## W9-10. Frame-rate option: interpolated display frames (`port_interp.cpp`)
- **Setting:** `[window] frame_rate = 30 | 60 | 120 | 144 | display | unlimited` (default 30), F1 menu Display tab "Frame rate"; `SMS_PORT_FPS=<rate>|display|unlimited` overrides it for one run (tests). The F3 overlay now shows display fps and the game's own rate ("logic 29.97 Hz"); the frame-stats log line has "logic frames N/s".
- **The game logic is not touched:** `direct()` still runs its two simulation ticks per call, once per two 59.94 Hz retraces, and a logic frame's picture is presented at its retrace as before. 30 = exactly the old path (no snapshots, no extra frames).
- **How a logic frame is drawn (found on the way):** the last tick of `TMarDirector::direct()` ends with the perform list `unk34` (CALC_VIEW | ENTRY: camera matrices, J3D `viewCalc` of every model, draw-buffer entries) for the new state; the *next* `direct()` call starts with the draw lists, which only render those buffers. A first attempt that re-ran just the draw lists showed the next logic frame's picture (everything is baked in at entry time).
- **Extra display frame** (`port_mardirector_redraw`, `src/System/MarDirectorDirect.cpp`, SMS_PORT):
  1. the view half of `unk34` (`CUE_CALC_VIEW | CUE_SET_PROJECTION`, no entries) with the camera matrices and every J3D model's joint/envelope world matrices replaced by `lerp(S_s, S_s+1, alpha)`; the logic frame's draw-buffer entries stay, and J3D packets read their model's current draw matrices when drawn (`J3DShape` via `mCurrentViewNo`);
  2. the GX draw lists (`unk40`, `mPerformListGX`, silhouette, `mPerformListGXPost`), not `unk38`/`unk3C` (pollution layers + graffiti check, bath-water preprocessor: passes that accumulate or are read back);
  3. GXDrawDone (Aurora reads indexed matrices and arrays on its FIFO thread), then the view half once more with the live values: every model and object is back to what the logic `unk34` left, each model's double-buffered draw matrices on the same buffer;
  4. the screen fader as `gameLoop` draws it, without `TSMSFader::update` (`port_app_redraw`, `src/System/Application.cpp`).
  A second attempt re-ran the whole `unk34` (with entries): packets entered twice before a draw-buffer reset made cyclic J3D packet lists, and the next logic frame overflowed Aurora's 2 MB index buffer (abort a few seconds into Bianco Hills). The view-only passes fixed it.
- **Snapshots:** each logic `unk34` pass stores the camera's view/projection matrices and, through the `J3DModel::viewCalc` hook (`libs/JSystem/.../J3DModel.cpp`, SMS_PORT), every model's matrices, three states deep (frames due between a logic draw and its retrace still need the older pair). A model viewCalc'd twice in one pass with different matrices is instanced and never interpolated. Entries not seen for 6 passes are dropped.
- **Timing:** state S_s's picture is up from P_s, the first retrace after its logic draw; a frame at time t shows `alpha = (t - P_s) / tick` (tick = measured logic period, ~33.37 ms). Original latency. Frames are drawn ahead by their recent maximum cost so they are presented at their due time; a frame that would not finish before the next retrace waits (the retrace starts the next logic frame); the slot of a logic frame's own retrace is left to it. 60 and 120 are locked to the VI period (60 = one frame on every retrace: the logic pictures plus one interpolated frame in between). Unlimited still sleeps 1 ms per frame so lower-priority game threads (memory card, loaders) run.
- **Cuts and teleports:** the camera snaps when it moves > 1000 units or turns > ~45 degrees in one logic frame, or the projection changes kind or fov by > 30 %; a joint snaps when it moves > 500 units or a basis axis turns > 60 degrees.
- **No game state changes from a display frame:** camera and model matrices restored; shadow requests (consumed by the next logic draw) saved/restored; sound effects gated (`mSeGateMask = None`); `rand()` on the main thread draws from a scratch copy (`port_rand_scratch`, port_replay.cpp); w8's `port_efb_set_presentation_only(1)` (E18: GPU-only copies, game-frame peeks/counters, no readbacks); draw-sync callbacks (Mario's occlusion probe, sun peeks, pollution counters) are not called (`port_aurora_sdk.cpp` `GXSetDrawSync`, marked w9); `port_GXCopyDisp` ignores the redraw's display copy; `port_efb_end_frame` runs only for a frame holding a logic draw. If a display frame falls between a logic draw and its retrace, the EFB is cleared (`port_efb_clear_all`) and the frame replaces the logic picture.
- **Not interpolated (30 Hz steps, drawn at the newer state):** particles, FLUDD water, shadows, non-J3D effects, 2D/HUD, mirror reflections. Instanced models (one J3DModel at several places) step too.
- **Files:** new `port/src/port_interp.cpp`; game side (all `#ifdef SMS_PORT`, GameCube `shasum -c` OK): `src/System/MarDirectorDirect.cpp`, `src/System/Application.cpp`, `libs/JSystem/src/J3D/J3DGraphAnimator/J3DModel.cpp`. Others' files, marked w9: `port_main.cpp` (w7: `VIWaitForRetrace` waits with an `OSAlarm` and draws due frames, `displayFrame`, `SMS_PORT_SHOT_DISPLAY`, retrace time stamp, `port_GXCopyDisp` guard), `port_efb.cpp` (w8: `port_efb_clear_all`), `port_aurora_sdk.cpp` (w6: draw-sync callbacks). Both were re-synced from main (w8's E18 presentation-only hook and game-frame clock) and W9-9's deterministic delivery re-applied on top: requests carry `sGameFrame` and are applied 3 game frames later; the deterministic GXPeekARGB returns its last value in presentation-only frames. w9: `port_settings.*`, `port_menu.cpp`, `port_window.cpp` (overlay/logic rate, `port_window_display_frame`), `port_replay.cpp` (`port_rand_scratch`, stronger checkpoints).
- **Evidence** (Bianco Hills, scripted input, other agents' builds running on the same PC):
  - **Same game state at every setting:** replay checkpoints now also hash every live actor's position and live flags and the camera's position, view and projection matrices (`E_CHK2`, 28 bytes; old files still compare Mario + rand). One session recorded at 30 (`sms_replay_20261008_185648.bin`), replayed at 30, 60 and unlimited (after the merge onto main's port_efb/port_aurora_sdk): 0 of 37 checkpoints differ each time. Before the merge the same check at 30, 60, 144 and unlimited: 0 of 37 each.
  - **Frame times** (`SMS_PORT_FRAMESTATS`, steady gameplay windows): 30: 29.97 presents/s (sd 3.3 ms); 60: 59.94/s, interval 16.68 ms, sd 1.3 ms, logic 29.97 Hz; Match display (60 Hz monitor): 59.94/s; 144: 125-138/s (sd 3-4 ms); unlimited: 170-200/s; logic 29.9-30.0 Hz in all. A display frame costs ~4 ms of main-thread time: 1.2 ms view pass + draw lists, 2.5 ms waiting for Aurora's FIFO thread to process them (needed before the restoring pass), 0.1 ms restore + fader, the rest present. So 120/144 reach ~100-138 fps on this (shared) machine; at 144 many logic pictures are replaced by interpolated frames.
  - **Interpolation visible:** 60 Hz captures (`SMS_PORT_SHOT_DISPLAY`) alternate logic/interpolated frames with even differences (mean abs diff ~22 between neighbours instead of pairs of identical images); the interpolated frame shows Mario's pose and the camera between its neighbours.
  - **Cut:** Delfino Plaza intro skipped with A (iris wipe to Mario's camera, camera jumps 7474 units): the frame at the cut is flagged CUT and shows the new camera (diff to the following logic picture 0.10, no in-between smear); the wipe and HUD are drawn in display frames.
  - **Overlay:** F3 shows e.g. "99.9 fps 10.0 ms (max 24.9) logic 30.0 Hz VI 59.95 Hz" (120 setting, window capture).
  - GameCube build: `shasum -c` OK after the game-side changes.
- **Coordinator rule (E18) on accumulating passes:** pollution stamp/decay/copy (`unk38`/`unk3C`) and the bath-water preprocessor (in `unk3C`) are skipped. `TModelWaterManager` has no EFB copy; its view work (`calcDrawVtx`/`calcVMAll`, splat matrices) is recomputed from the particles each pass and its double-buffered quad list ends on the same buffer after the two view passes, so nothing accumulates; Aurora clears the EFB per frame, so no EFB-feedback build-up exists here either.
- **Debug knobs:** `SMS_PORT_INTERP_LOG=1` (a line per frame: alpha, states, models interpolated, camera movement, "over logic", "CUT"), `SMS_PORT_SHOT_DISPLAY=<frame>:<n>` (the next n presented frames as `disp_NNN_L/I.bmp`), `SMS_PORT_INTERP_NORESTORE=1` (skips the restoring pass; diagnosis only).

## W9-11. Frame-rate option rebased onto main (2026-10-08 evening)
- **How:** `agent_sync.ps1 -Worker w9 -Port` (records main's port/ as the base), then every file w9 touches rebuilt as main's current version plus only w9's hunks (hunk-by-hunk, main's side kept for the other agents' changes):
  - `port_main.cpp`: kept w13's `port_disc_early_handoff` and w8's `SMS_PORT_SHOT_PRESENT` / `port_present_screenshot`; re-applied the `VIWaitForRetrace` pacing loop (OSAlarm), `displayFrame`, retrace time stamp, `SMS_PORT_SHOT_DISPLAY`, `port_GXCopyDisp` guard.
  - `port_efb.cpp`: on top of w8's readback pooling and per-key coalescing (`keyBusy`, `sOutstanding`, `JobBuffers`): deterministic delivery (`detFrame`, `sDetQueue`, `detEndFrame`) and `port_efb_clear_all`. New: `keyBusy` never coalesces while recording/replaying (whether a request is made must not depend on the GPU's pace); a det job still balances w8's outstanding counters before `detEndFrame` takes it.
  - `port_aurora_sdk.cpp`: kept w8's present capture and the rounded-down screenshot size; re-applied the deterministic `GXPeekARGB` and the draw-sync suppression in presentation-only frames.
  - `port_disc.cpp/.h`: on top of w13's regions: replay identity without paths (file name only).
  - `build_aurora.py`: on top of `--region`: the rand/srand/OSIsThreadTerminated/THPPlayerGetState rename block and the README frame-rate/F9 text.
  - `MarDirectorDirect.cpp`: kept main's `VERSION_GMSP01` balloon fix (w13); `Application.cpp`, `J3DModel.cpp`, `port_settings/menu/window/input`: w9 hunks only on top of w6's graphics settings.
- **w6's VSync and the frame rate are one setting now:** the VSync checkbox sits under "Frame rate" in the Display tab; with VSync on, the display-frame period is capped at the monitor's refresh (`periodMs`, `port_interp.cpp`), so 120/144/Unlimited never draw frames that would only block in the present; the menu says "(VSync caps the frame rate at the monitor's N Hz)" and the ini comment says so too.
- **Found on the way:** `SMS_PORT_REPLAY_EXIT=1` ended in `abort()` (exit 0xC0000409): `std::exit` ran the atexit destructor of Aurora's joinable `g_pipelineCacheWriterThread`. It now calls `aurora_shutdown()` first, like the window-close path; exit status 0 again.
- **Checks (all on this rebased tree):**
  - Hooks present: `port_efb_end_frame`, `port_sweep_pad_apply`, `port_window_frame`, `port_input_read`, the `RA<0-255>` padscript token, `port_disc_early_handoff`, `SHOT_PRESENT`, w6's msaa/anisotropy/fxaa/vsync, `copy_pipeline_seed`, `SANITIZE`, `--region` (same counts as main, plus w9's).
  - `build_aurora.py`: 622/622, 0 undefined. GameCube: `shasum -c` OK.
  - Replay determinism: a new recording at 30 (Bianco Hills, scripted keys, 4602 game frames) replayed at 30, 60, unlimited and 144 + VSync: 0 of 37 checkpoints differ each time. 60: 59.9 presents/s, logic 29.95-29.99 Hz; unlimited: ~345 presents/s, logic 30; 144 + VSync on a 60 Hz monitor: 59.94 presents/s (capped), logic 30.
  - `smoke_all.py --quick`: PASS 20, FAIL 0, STUCK 0.
  - `merge_check.py w9 --all`: 15 worker-only files + 3 new, no BOTH/STALE/CONFLICT.

## W9-12. DualSense: analog triggers checked, adaptive triggers, lightbar (`port_padfx.cpp`)
- **Test controller without touching the player's pad:** `SMS_PORT_VPAD=ps5|ps4|xbox` attaches a virtual controller through SDL's virtual joystick driver (DualSense 054C:0CE6, DualShock 4 054C:09CC, Xbox Series 045E:0B12).
  Every physical backend stays off as with `SMS_PORT_NO_PADS` (HIDAPI, RawInput, WGI, DirectInput, XInput, GameInput); only the virtual VID/PID passes `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT`.
  `SMS_PORT_VPADSCRIPT=<from>[-<to>]:<ctl>[=<v>][+...]` drives it (LT/RT 0..1, LX/LY/RX/RY, CROSS/SQUARE/CIRCLE/TRIANGLE/OPTIONS/CREATE/LB/RB/D-pad).
  Its callbacks log what SDL hands the controller: `[vpad] SetLED`, `SendEffect` (the 47-byte DS5 effect state), `Rumble`, `RumbleTriggers`.
  So everything up to SDL's HID report is exercised; the report itself (SDL's PS5/PS4 driver) and the feel are not.
- **(a) Analog L/R, end to end** (virtual DualSense, Bianco, `SMS_PORT_PADLOG`, `SMS_PORT_PADFX_LOG=2`): SDL trigger axis -> Aurora binding (value x255) -> `linearTrigger` -> game `PADClamp`.
  RT 0.05/0.10/0.15/0.20/0.25 -> raw 38/45/54/62/70 -> clamped 8/15/24/32/40 (linear, ~158 per unit of travel); RT 0.95 -> clamped 150 with digital R off; RT 1.0 -> digital R on (Aurora's click at 31151/32767 = 95.07%), analog 150.
  FLUDD: half press (RT 0.5: analog 79, R button 0) while walking: forward speed 27-30 with water coming out; full press (RT 1.0 + stick held): R button 1, forward speed 0 (planted aiming). Both as on a GameCube pad.
- **(b) Adaptive triggers** (DualSense only; `SDL_SendGamepadEffect` with SDL's 47-byte DS5 effect state, enable bits 0x04|0x08 only, so rumble and lightbar are untouched):
  - GameCube click: trigger mode 0x21 (feedback, 3-bit force per zone), zones 0-8 light, zone 9 (where the 95% click is) firm; strength 60% = `21 ff 03 49 92 24 21 ...`.
  - Full FLUDD feedback (R): slack (0x05) when the tank / Yoshi's juice is empty and while a trigger nozzle waits for the release (TNozzleTrigger DEAD); a pulse while spraying (0x26 vibration, 10 Hz, amplitude rising from zone 2); hover thrum (15 Hz); turbo buzz (45 Hz); rocket kick (30 Hz) at launch; resistance growing with the rocket/turbo charge. L keeps the click.
  - Updated once per game frame, sent only on change; logged in Bianco: click -> spray pulse at a half press -> click -> hover thrum while hovering -> slack (release) -> click.
  - Settings: `[input] adaptive_triggers = off|gamecube|fludd` (default gamecube), `trigger_strength` (60); F1 Controller tab. Triggers are reset to no effect when the window closes.
- **(c) Lightbar** (DualSense, DualShock 4; Aurora's `PADSetColor`, applied by its next `PADRead`; only on change):
  - Colour: the game's own water colour table `gModelWaterManagerWaterColor[TModelWaterManager::unk5D5F]` (0 = FLUDD water, 1-3 = Yoshi's juices; `TYoshi::mType` is copied there while riding), scaled to full brightness with the saturation raised to >= 80%: FLUDD (51,85,255), orange (255,169,2), purple (156,1,255), pink (255,51,142).
  - Brightness: remaining fluid (TWaterGun current / `getMaxWater()`; riding: TYoshi `unkC / unk8`, as the HUD gauge) through 0.03 + 0.97 * fill^2.2, and 0.012 at empty: FLUDD at 100/75/50/25/10/0% = (51,85,255) (28,46,139) (12,20,61) (4,6,19) (2,3,9) (1,1,3). Times the brightness setting.
  - Dim warm white (15,14,13) at 100% brightness without FLUDD (airstrip before the pickup: checked), in the pause/map/save menus and outside a stage (title, file select, movies).
  - Checked: Bianco spray drains the light 1.000 -> 0.915 in 1-step changes; Ricco 3,7 with `SMS_PORT_DEBUG_CALL=hatch@1500` and `SMS_PORT_PADSCRIPT=...1505-1600:G11000;1100,1540:A,...` (Mario lands on Yoshi at f1576): pink juice (251,50,140) at 0.993, dimming to (210,42,117) at 0.914 while Yoshi spits.
  - Settings: `[input] lightbar = off|fluid|static` (default fluid), `lightbar_brightness`, `lightbar_color`; F1 Controller tab.

## W9-13. HD rumble (`port_haptics.cpp`), Xbox impulse triggers
- `[input] rumble_mode = off|original|hd` (default original), strength = the existing `rumble`, categories `hd_fludd/hd_movement/hd_world/hd_damage`; F1 Controller tab.
- HD: `build_aurora.py` renames the game's `PADControlMotor` to `port_PADControlMotor`, which drops the game's on/off pulses on controller 1 (Original passes them on, Off stops the motor); nothing in the game changes.
  The engine reads, once per game frame: the game's rumble channels (`SMSRumbleMgr`: each active channel's type and this frame's curve value -> one voice per type: hip drop = heavy thud, damage = sharp hit, electric = buzz, MAPOBJ_APPEAR and the unnamed boss/event types = rolling rumble ...), sound-effect starts (a marked `SMS_PORT` hook in `JAIBasic::startSoundBasic` queues SE ids: footsteps by ground from the id ranges of `MSD_SE_MA_WALK_*` - stone, soil, sand/gravel, wood, metal, grass, carpet, water, mud/goop - left/right foot from bit 2, Yoshi steps, wall bumps, water entry/exit, slides, coins, red/blue coins, 1-up, Shine get, Yoshi gulp/juice), Mario's status (landing scaled by the fall speed, wall kick, dive, swim in/out) and FLUDD (spray buzz scaled by the analog R and thinning with the tank, rocket charge and launch, hover hum, turbo rattle).
- Backends: two motors (`SDL_RumbleGamepad`, a low/high mix per voice); Xbox impulse triggers (`SDL_RumbleGamepadTriggers`, FLUDD on the right trigger); DualSense HD haptics (W9-14). A GameCube pad on the adapter keeps the game's pulses (on/off motor).
- Checked (virtual pads, `SMS_PORT_HAPTICS_LOG=1`): footsteps (`step stone`, pairs per stride), landings (amp 0.4-0.6 for jumps), spray, hover; Xbox: `RumbleTriggers 0,12216` while spraying, falling slowly with the water.

## W9-14. DualSense HD haptics over USB and Bluetooth (`port_haptics.cpp`)
- USB: the 4-channel audio device of the controller (name contains "DualSense"/"Wireless Controller"), SDL audio stream at 48 kHz, channels 3-4 = actuators; with HD on, SDL rumble is set to 0 (its rumble emulation would switch the audio haptics off).
- Bluetooth: report 0x32 written with `SDL_hid_write` on the path SDL's driver uses (`SDL_GetGamepadPath`): seq<<4, 0x91/7 `FE 00 00 00 00 FF counter`, 0x92/64 haptic samples, padding, CRC-32 over 0xA2 + 138 bytes; every 32/3000 s on an absolute schedule (high-resolution waitable timer). No state sub-packet, so SDL's 0x31 reports (lightbar, triggers) interleave. Layout from SAxense (MPL-2.0) and LinuxAudio4Dualsense5 (AGPL-3.0), facts only; their two readings of the 64 bytes (stereo 3 kHz / mono 6 kHz) are both implemented (`SMS_PORT_HAPTICS_BT_LAYOUT=mono6k`).
- Tests never open a device; `SMS_PORT_HAPTICS_DUMP=<prefix>` (+ `SMS_PORT_HAPTICS_DUMP_END=<frame>`) records `<prefix>_bt.bin`, `_hd.wav`, `_speaker.wav`.
  Checked with an independent script (zlib CRC): 6188 reports, CRC 0 bad, sequence and counter unbroken, sub-packet headers as specified; the haptic payload follows the events (mean |s8| ~60 of 127 during spray/landings after the gain fix, 0 when idle).

## W9-15. DualSense speaker (`port_speaker.cpp`, libopus, tap in `port_audio_dsp.cpp`)
- `[audio] controller_speaker = off|fludd|fludd_pickups`, `main_mix = keep|duck|remove`, `speaker_volume`; F1 Audio tab.
- Which voices: once per game frame the SE track of every playing FLUDD/pickup sound (JAI `mSeTrack[cat][slot]`, `trackToSeqp`, child tracks) is keyed by its `TChannelMgr`; the mixer tap (w5's `port_audio_dsp.cpp`, marked w9) traces DSP voice i -> `TDSPChannel::DSPCH[i].mSign` (the TChannel) -> its channel manager. Tagged voices are captured from their front-bus contribution (game volume, pan, fades) and kept / ducked (x0.35) / removed in the main mix; the speaker stream advances one DSP sub-frame at a time (silence when nothing plays).
- USB: channels 1-2 of the 4-channel device. Bluetooth: report 0x36 (398 bytes: 0x91 config `FE 40 40 40 40 40 counter`, 0x90/63 state, 0x92/64 haptics, 0x93/200 Opus, CRC over 0xA2 + 394 bytes), 512 samples of the 48 kHz stream resampled to 480 per packet, Opus 48 kHz stereo 10 ms CBR 160 kbit/s (200 bytes). The state sub-packet restates our trigger effects, lightbar and (Original mode) motor so SDL's 0x31 and ours agree; speaker switched on first with an effect state (0xA0/0x80, volume 0x64, audio control 0x30, preamp 0x02) through `SDL_SendGamepadEffect`.
- libopus 1.6.1 float encoder compiled from `build_aurora.py` (own list, no scripts; `OPUS_BUILD USE_ALLOCA HAVE_LRINTF`); `opus_packet_get_nb_frames` (decoder-side helper the encoder calls) is a 10-line RFC 6716 TOC parser in `port_speaker.cpp`.
- Checked: dumped 0x36 stream decoded by a separate full-libopus tool: 6081 packets, 0 bad; decoded speaker audio -31 dBFS RMS while spraying, -53..-57 with FLUDD idle, silence otherwise. Main mix with `SMS_PORT_AUDIO_SE_ONLY=1`: keep ~-27 dBFS, remove ~-42 dBFS (the FLUDD sounds gone).

## W9-16. GameCube adapter, replay exit
- SDL 3.4.10 (Aurora's prebuilt) has the HIDAPI GameCube driver with its libusb/WinUSB backend built in; Aurora marks 057E:0337 as a GameCube pad (no trigger emulation, digital L/R from MISC3/MISC4, on/off rumble with the brake); the port leaves it alone (no linear-trigger remap, no rumble-intensity change, no lightbar/trigger effects, HD keeps the game's pulses). Windows needs the WinUSB driver once (Zadig), documented in the package README. Not tested on hardware (no adapter, and test runs keep every physical backend off).
- `SMS_PORT_REPLAY_EXIT=1` now ends with `TerminateProcess` after removing the card copy: after `aurora_shutdown` the AI interrupt thread used the destroyed audio stream (access violation at exit, seen once). 0 of 37 at unlimited and 30 with every new feature on, exit status 0.

## W9-17. Crash opening the guide map: .bti headers reverted to big-endian by a 2D-archive refill (`JKRMemArchive.cpp`)
- **Symptom:** Bianco Hills, jump + Z (Z opens the guide map; the ground pound is L in the air) -> access violation on Aurora's FIFO thread in `hash_texture_source`, reading 0xA0C80F00.
- **Found with** a temporary `GXInitTexObj` wrapper (renamed for game objects, logging images outside emulated RAM with a backtrace; removed afterwards): `TGuide::resetScore` -> `J2DPicture::changeTexture(unkC8[n]->getTexInfo())` -> `JUTTexture::storeTIMG` -> `GXInitTexObj(image 0xA0C80F00, 9216x9216, RGB5A3)`. The header at 0x80C80F00 was `05 02 00 24 00 24 ... 01 01 00 00 01 00 00 00 00 00 00 20`: `/guide/timg/coin_number_N.bti` (36x36, image offset 0x20) in big-endian order, marker byte 0x19 = 0 (never swapped, or swapped and then overwritten).
- **Cause:** the pause/HUD archive (`game_6`) and the guide archive share one fixed 2D buffer (`TMarDirector::unkD4`); `SMSSwitch2DArchive` / `SMSMountAramArchive` copy the raw archive image from ARAM into it again and `mountFixed` it. `TGuide::load` made its JUTTextures from `JKRGetResource` (swapped on that first fetch); opening the map refills the buffer with the raw image and calls `storeTIMG` on the same headers again, with no `getResource` that would swap them. On the GameCube the data is native, so this sharing is harmless there.
- **Fix (root, swap layer):** `JKRMemArchive::open(void* buffer, ...)` (the fixed-buffer mount) swaps every resource header in the image at mount time, as `getResource` would on first fetch (`port_swap_resource`: `.bti`, `.bfn`; idempotent by their markers). Covers the game_6/guide switch, the PAL language reload of `cmn2d`, and every other `mountFixed`.
- **Sweep:** the archives that are refilled in place are `game_6` (9 `.blo`, 108 `.bti`) and `guide` (1 `.blo`, 97 `.bti`, 1 `.bmg`): only `.bti` headers are swapped in place (`.blo` is parsed through big-endian streams, `.bmg` is never swapped). Other `mountFixed` users mount freshly loaded buffers before their objects are built. A quick smoke (20 scenes) with the debug wrapper logged no texture image outside emulated RAM.
- **Checked:** the Bianco repro runs on (guide drawn with its coin digits, HUD intact after closing it); 4 hip drops each in Bianco 2,0, Ricco 3,0, Gelato 4,0, Pinna 5,0, Sirena 6,0 and the plaza 1,1, with the guide opened and closed in each; GameCube `shasum -c` OK.

## W9-18. Quality-of-life tab (`port_qol.cpp`, F1 > Quality of life, `[qol]`)
- Every option is off by default (= the original). Options: skip boot logos + intro movie; Start skips any cutscene/movie; dialogue text speed normal/fast/instant; fast-forward while a key is held (Tab, rebindable; 2x/4x); auto-save after a Shine; continue in the level after a Shine; pause on focus loss; camera invert X / Y and speed 50-200 %; music / SFX / voice volume; blue-coin tracker and Shine checklist (pause screen, F4 toggles both any time); infinite lives / FLUDD water.
- **Replays:** the options that change the game (skip flags, text speed, auto-save, continue, camera, cheats, the fast-forward speed active this frame) are packed once per game frame (`port_qol_frame`, before `port_replay_pads`) into a 16-byte state; a recording stores it as event `E_QOL` (7) when it changes, a replay applies the recorded state instead of the player's settings. Older replays have no `E_QOL` and play as all-off. Volume, trackers and focus pause do not touch game state and are not recorded; focus pause is off during a replay.
- **Multiplayer (w10 `port_mp_active`)**: fast-forward, focus pause, cheats, auto-save and continue-after-Shine are forced off.
- **`PORT_SKIP_CUTSCENES` is now opt-in in the Aurora build**: the Start-skips-in-engine-cutscene code (`MarDirectorDirect.cpp` updateGameMode case 3/4, `CameraDemo.cpp skipDemoCamera`) runs only with "Start skips any cutscene" on; before it was always on in the PC build, which is not original behaviour.
- **Edits in other agents' files (small, marked w9):** `sdk_audio.cpp` (w5) `aiTick` returns early while paused for focus; `port_audio_dsp.cpp` (w5) scales a voice by `port_qol_voice_gain(tag)` when a volume is below 100 % (tags from `port_speaker.cpp`: SE id of the voice, 0 = music/stream), and the speaker capture now asks `port_speaker_wants(tag)` (the tag exists for every SE while volumes are active); `port_main.cpp` VIWaitForRetrace (pause loop, fast-forward); `port_interp.cpp` (no interpolation while fast-forwarding); `port_menu.cpp` (tab + `port_qol_draw`). **w5:** please keep the two hooks if you rework the mixer loop.

## W9-19. QoL game-side hooks (all `SMS_PORT_AURORA`, GameCube `shasum -c` OK)
- Skip intro: `TGCLogoDir::direct` returns `APP_STATE_DONE` at once; `TApplication` after the logo goes to area 15 (the title stage, `APP_STATE_GAMEPLAY`) instead of `APP_STATE_DONE` (Entrance.thp). (`APP_STATE_MENU` is the debug stage select, not the title.)
- Skip cutscenes: `MovieDirector.cpp` both `MSF_FMV_WATCHED_BASE` checks go through `PORT_QOL_MOVIE_SKIPPABLE(watched)`; the stage-intro flyover also lets Start skip the plaza 1,1 / 1,9 intros; in-engine demos as above.
- Text speed (`Talk2D2.cpp` openNormalWindow / moveTalkWindow): fast = the game's own A-held path (`unk26C = 1`, alpha step 0x80, no per-character pause); instant = alpha 0xFF, a whole line shown at once, and each line's slide-in snapped when the line before it is complete (the original resets a line to 1.0 while the previous one is unfinished, so snapping all lines at once left lines 2-3 empty; fixed).
- Auto-save (`MarDirectorDirect.cpp portAutoSave`): on `CARD_SAVE_PENDING` after a Shine (`unk261 == 6`): `getBookmarkInfos`; if the loaded file (`mSaveFile`) is valid and the card's copy is ours (not broken, same last-save time), `saveBookmark(file)` -> `saveSuccess` -> option block -> unmount, toast "Game saved"; any other case falls back to the normal prompt (`STATE_CARD_SAVE`).
- Continue after a Shine: at `SHINE_GET_PENDING`, if allowed (option on, Mario in `WIN_DEMO`, not an ex-stage, not airstrip 0 / plaza 1 / Corona 60) the white fade-out, `STAGE_TRANSITION_PENDING` and `decideNextStage` are skipped; Shine flag, count and save happen as usual. When the demo and the save are over: `SHINE_TAKEN` off, Mario `changePlayerStatus(WAIT, 0, true)` (forced: `WIN_DEMO` carries flag 0x1000, which blocks an unforced change, as `readBillboard` does) and `MSMainProc::startStageBGM`. The Shine object took `HIT_MESSAGE_TAKE`, so it cannot be collected again.
- Camera (`MarioGamePad.cpp updateMeaning`): `port_qol_camera_stick(&mCompSPos[6], &mCompSPos[7])` after the C-stick copy (invert, scale).
- Fast-forward (`port_main.cpp`): every 2nd/4th main-thread VIWaitForRetrace returns without waiting and counts as a retrace (`TVideo::waitForRetrace` loops until the count advances, so without the virtual retrace 4x stayed at 2x).
- Cheats (`port_qol_frame`, in a stage): `MSF_LIFE_COUNT` kept >= 3; `mCurrentWater` kept at `getMaxWater()` while Mario has FLUDD.

## W9-20. QoL checks (scripted runs, isolated settings/user/cache dirs, `SMS_PORT_NO_PADS=1`, audio off)
- Skip intro: title screen at frame 701 (Dolby logo still up at 150 without it). Skip cutscenes: first-time openingA.thp stopped by Start at movie frame 280 (not skippable with the option off); Shine-get demo ended at frame 2077 instead of 2237; plaza 1,1 intro to control at tick 366 instead of 1170.
- Text: screenshot grids normal / fast / instant of Peach's airstrip lines (fast ~2x; instant all three lines at the first readable frame).
- Fast-forward (Tab via `SMS_PORT_KEYSCRIPT`, `SMS_PORT_FRAMESTATS`): logic 29.97/s -> 59.94/s (2x), ~116/s (4x); ">> 4x" badge in `SMS_PORT_SHOT_PRESENT`.
- Focus pause (new test knob `SMS_PORT_FOCUSSCRIPT=<frame>:<ms>`): "[qol] paused" ... "resumed" after 6 s, game thread stopped in between.
- Camera: C-stick right 30 frames: turns one way, the other with invert X, about twice as far at 200 %.
- Volumes (WAV): all three at 0 -> RMS 0 (every DSP voice goes through the gain); title screen music 0 -> peaks 803 -> 263 (the ambience SFX remain).
- Trackers: pause screen with blue coins (stage names, x/30, total 240) and the Bianco episode list; F4 shows them without pausing.
- Cheats: water 10000 after 24 s of spraying (9381 without); lives 2 -> 3 after a fall in a secret course.
- Auto-save / continue (Gelato 4,2 `tpshine`, card seeded from the smoke new-game card): continue + auto-save -> "Game saved", "continuing in stage 4", Mario walks and jumps afterwards, Shine count 1, no stage change; auto-save alone -> saved, back to the plaza.
- Replays: QoL-off recording replayed with QoL off and with everything on: 0 of 21 differ each; recording with FF 4x / camera / cheats / instant text replayed with QoL off: 0 of 22; a recording made by main's pre-QoL exe (copied to the scratchpad) replayed in this build with every option on: 0 of 18 (same as main replaying it).
- smoke_all --quick 20/20 PASS; GameCube `shasum -c` OK.
- Not covered: multiplayer gating only by code (no session run); the voice/SFX split is by SE id range (0x78xx/0x88xx = voices), not verified per sound; continue-after-Shine checked in one stage (Gelato 4,2) only, boss stages rely on the exclusion list and the WIN_DEMO condition.

## W9-7. Release package (`build_aurora.py --package <dir>`)
- **Contents:**
  - `sms_port.exe`, with the PE subsystem switched to Windows GUI, so no console window opens;
  - the Dawn/DXC/SDL/libpng/zlib DLLs;
  - the Visual C++ runtime DLLs that the exe and those DLLs import (msvcp140, msvcp140_atomic_wait, vcruntime140, vcruntime140_1), from the VS redist folder;
  - a README (game requirement, controls, hotkeys, where settings/saves live, licences).
  The UCRT is part of Windows 10/11. No game data is ever copied. The target folder must be empty.
- **Checked:** packaged into my scratchpad (46 MB, 12 files) and started from there with the ISO on the command line, outside the repo.
  It reached the title, with no console window (`port/build_aurora/w9_package_run_iso.png`).



## A-4. Output scale (calibration)
- `DSPRES_FILTER`'s layout and JAudio's volume paths give these anchors: every DSP volume is Q15 (normal mix buses: `TChannel` volume x
  `inputGainDown` x 16384; auto mixer: volume x 32767.5; JAI streams: 0x7FFF / 0x5A7E straight from `JAIGFrameStream`), and the DsyncFrame level
  defaults to 0x4000 (`dspproc.c`). Kept as 2.14 (0x4000 = unity; SMS's `DsetMixerLevel(5.0)` = 0x5000 = x1.25) because the stream path rules out
  4.12: `title.afc` is mastered to -0.7 dBFS peaks / -21 dBFS RMS (decoded offline), and at volume 0x7FFF a x5 output would clip it constantly, as it
  did the boot jingle (a full-volume auto-mixer voice). With 2.14 a full-volume SE or stream plays at x1.25 and a full-volume music channel at x0.5.
- Measured after the A-5 fix (WAV, `SMS_PORT_THP_SPEED=8`): title BGM -30..-35 dBFS RMS; Bianco Hills stage BGM -28 dBFS; jump/FLUDD/footstep
  windows -15..-17 dBFS (peaks touch full scale, 5 clipped samples in 50 s); THP movie sound (CPU-mixed at unity by the SDK's MixAudio) -14..-20 dBFS.
  Open: sequence music sits 10-15 dB under the movie and SFX. That follows from JAudio's own numbers (music capped at 0.802 x 0.5 of full scale)
  and was not tuned away; if a real-hardware recording shows otherwise, the music bus (not the output level) is where a factor is missing.
- Auto-mixer pan law now uses the quarter-sine table JAudio passes the DSP (`DSPRES_FILTER` s16 512-639) instead of `cos`/`sin`.
- Filters: no FIR (outer "muffle") or animated IIR coefficients were seen in the title, Delfino Plaza or Bianco Hills runs (`SMS_PORT_AUDIO_LOG` reports the first ones).

## A-5. Resampling table was read as 128 phases; it has 64
- `DSPRES_FILTER` is 640 s16: 0-255 = 64 phases x 4 taps (phase 0 `{0x0C39, 0x66AD, 0x0D46, -0x21}`, phase 63 its mirror), 256-511 = four 64-sample
  oscillator tables (sine, triangle, two single-pulse), 512-639 = a quarter sine. The renderer used 128 phases, so half of all output samples were filtered
  with the oscillator tables as "taps": broadband noise on every pitched voice (spectral flatness 0.2-0.5, now 0.00-0.03 on the title music; harmonic lines and
  seagull chirps are now clean in the spectrogram). Fixed: 64 phases (`frac >> 10`); oscillator voices use the DSP's own tables.

## A-6. Gameplay sound with `SMS_PORT_WARP` (no warp change needed)
- `SMS_PORT_WARP=1,0` is a fresh save's first plaza visit: `checkAdditionalMovie` plays the court movies (openingBA/BB), then `TMovieDirector` shows the
  "save?" prompt over the plaza scene. The screenshot that looked like gameplay was that prompt, so no stage sound flow had run (no
  `MSMainProc::setMSoundEnterStage`). Same as the natural path. Answering it (`SMS_PORT_PADSCRIPT=1300:A,1500:A,1700:A`) plays openingBC, then the
  plaza starts: demo BGM `0x80010017`, stage BGM `0x80010001`, ambience, jump/voice (`0x78xx`), FLUDD (`0x0024`/`0x6800`), footsteps (`0x18xx`).
- `SMS_PORT_WARP=2,0` (Bianco Hills) goes straight to the stage: demo BGM `0x80010027` then stage BGM `0x80010002`; with
  `SMS_PORT_PADSCRIPT=1000-1400:S0,1100:A,1250:A,1450-1600:R,1650:A,1700-1900:S90+R,1950:A,2000-2200:S180,2100:B` 76 distinct sounds were requested
  (jumps, spray, footsteps, landing, voices), all rendered (levels in A-4). Coins were not collected in the scripted runs.
- Debug logs added (`SMS_PORT_AUDIO_LOG`): `MSMainProc` stage sound setup and `startStageBGM`, `MSBgm::startBGM`, SE-gate drops (`MSound::gateCheck`),
  and each distinct JAI sound id once (`JAIBasic::startSoundBasic`).

## A-7. THP movie audio
- `sdk_thp.cpp` now decodes and plays movie sound like the SDK: the decoder thread runs `THPAudioDecode` (Aurora's, SDK API, right/left order) on each
  frame's audio component (selected track) into the SDK's three audio buffers; `THPPlayerInit` registers `audioCallbackWithMSound` as JAudio's mode-3 mix
  callback (removed again in `THPPlayerStop`/`Quit`, as in `THPPlayer.c`); `MixAudio` (VolumeTable, volume ramp) runs from `JASystem::Kernel::vframeWork`
  on the AI clock; `PlayControl` only advances the picture while `curVideoNumber - curAudioNumber <= 1`, and a non-looping movie ends after its last audio frame.
- `SMS_PORT_THP_SPEED > 1` turns movie audio off (frames are skipped) and keeps the old video-clock behaviour.
- Evidence: normal-speed boot, `Entrance.thp` (2 ch, 32 kHz, 2 tracks): tonal music -14..-20 dBFS RMS (spectral flatness ~0.002), no silent 50 ms window
  from 8 s to 96 s, the movie ends (state 3, frame 2815 of 2816) about 95 s after it opens (2816 frames / 29.97 fps = 94 s), 0 AI underruns.

Left to do (audio): music-vs-SFX balance against a real recording (A-4); IIR/FIR semantics are reconstructions (not exercised yet);
Dolby surround (back buses are folded into L/R).


## R5. Sirena (warp 6,0): Aurora FATAL `unmapped vtx attr 13`
- **Cause:** a texgen with source `GX_TG_TEX0` while the vertex format has no `GX_VA_TEX0`; Aurora's shader generator had no fallback.
  Dolphin's vertex shader generator uses (0, 0) for a texgen whose texture coordinate is not in the vertex.
- **Fix:** `aurora_shader.patch` (3/3): same fallback, with a warning naming the attribute. Sirena now plays (`stages2/a6_*`: sunset beach).


## R6. D3D12 `CreateGraphicsPipelineState` E_OUTOFMEMORY (warps 6,0 / 7,0 / 8,0)
- **Pipeline count:** not unbounded. A temporary probe in `find_pipeline` counted distinct GX pipeline configs, configs with the alpha-compare refs zeroed, and configs with `dstAlpha` normalised: about 100 in Sirena after the load and under 200 after 75 s, and the normalised counts were identical (no per-draw value in the key).
  Process memory stayed flat (private bytes 713 MB, GPU dedicated about 250 MB over 100 s), so nothing leaks either.
- **Nature:** intermittent and per pipeline, not per shader. The failing pipeline (`GX Pipeline 8857a2bda610474b main`, a one-stage texture shader) compiled fine on the next runs; 3 more runs of 6,0 and 2 of 7,0 did not fail.
  It came while 5-7 other `sms_port` copies (other agents) shared the GPU. Dawn reports it as error type 4, **Internal**, not OutOfMemory.
- **Fix:** `aurora_pipeline_wait.patch` (c): `gx::build_pipeline` captures Internal and OutOfMemory errors with error scopes and retries up to 5 times with back-off (20-320 ms); if it still fails, that pipeline is dropped (error logged, its draws skipped) instead of aborting the game.
  Debug aid added on the way: `AURORA_SHADER_DUMP=<dir>` writes every GX pipeline's WGSL, named like the pipeline in Dawn's errors.


## R7. Missing geometry for the first frames after a load
- **Cause:** Aurora compiles pipelines on a worker thread and `gx::render` skipped any draw whose pipeline was not ready; with an empty cache a new stage drew without most of its map for a few frames (`final/plaza_shot_1950.png` of the previous round).
- **Fix:** `aurora_pipeline_wait.patch` (a): `gx::render` calls `wait_pipeline`, which moves a queued or prewarming pipeline to the front of the queue and waits for it. Nothing is ever dropped, as on the GameCube; the cost is a load-time hitch with a cold cache.
  This also covers w8's EFB-copy case (their `aurora_copy_pass_pipelines.patch` waited only in passes whose result is consumed; same `wait_pipeline` API, so that patch is no longer needed).
- **Persistent cache:** hand-started runs use the per-user cache, which persists (checked: two runs on one cache directory; the second run loads it and prunes stale Dawn entries), and every known pipeline is compiled in the background at startup.
  The old failure mode (one bad recorded pipeline aborting the next start) is gone: `aurora_pipeline_wait.patch` (b) turns shader/pipeline validation errors into a logged, skipped pipeline.
- **Result:** plaza warp with an empty cache: the "Delfino Plaza" intro camera (frame 1950, the same frame that lacked the map before) shows the full street, buildings and NPCs.


## R8. Exact 4:3 display (w9's `VILockAspectRatio`)
- Applied w9's proposed `aurora_display_aspect.patch` by hand to Aurora (`lib/window.hpp/.cpp`, `lib/dolphin/vi/vi.cpp`); the file in `port/patches/` is now the exact diff.
  `port_window.cpp` picks it up automatically: the log now says `window: aspect 4:3` without the "no Aurora aspect lock" note.
- Note for w8 (EFB readback owner): with the aspect locked, the render target is 4:3 while the logical EFB is 640x448, and Aurora maps logical coordinates with separate x/y scales and no offset (`map_logical_viewport`).
  `efbPeekARGB`'s `mapLogical` (R0) assumes a uniform scale with centring, which was already slightly off before (1280x960 target, 640x448 EFB); it should be `x * targetW / logicalW`, `y * targetH / logicalH`.


## R9. Warp 5,0 and visual checks
- **Warp 5,0 (Pinna Park beach):** renders and plays now (`shot_1200`: beach, park entrance, Mario, HUD); the all-black frames of the previous round do not reproduce with today's main plus these patches.
- **Night/dusk lighting:** the Hotel Delfino interior (7,0) renders with its dark-blue night lighting and Sirena (6,0) with its sunset tint, so channel/ambient/light colours reach the shaders.
  Plaza and Ricco walls show their baked vertex-colour shading. Pianta's brighter look is therefore left as it is; it needs a real-hardware capture to judge, and nothing in the pipeline points to a port bug.
- **Mario's dirt layer:** the swap `H_ma_rak_dummy` -> scene `h_ma_rak.bti` (airport: CMPR 128x128 pink/orange swirl) goes through `SMS_ChangeTextureAll` + `J3DTexture::setResTIMG` with the archive-swapped header and s32 offsets, and the DL is rebuilt every frame, so the right image is bound. A dirty Mario was not captured yet.
- **Fog:** Aurora's fog path is used by the stages surveyed (Noki and Pianta distances) with no visible errors; not compared with hardware.
- Stage survey after these patches (`port/build_aurora/stages2/`): 2,0 3,0 4,0 6,0 7,0 8,0 9,0 run without a fatal.




## 12. Airstrip: first goop clean crashes in `TPollutionLayer::cleaned` (wild read at line 189)
- **Symptom:** in airstrip scenario 1 (after FLUDD), spraying goop crashes within seconds: `TPollutionLayer::cleaned` 竊・`stamp` 竊・`TPollutionManager::clean` 竊・`TModelWaterManager::move`.
- **Cause:** an original-game bug that is harmless on the GameCube.
  The effect ring buffer is `static TVec3 pos[10]`, but the wrap test is `if (now_pos_no > 10)`, so `pos[10]` is written.
  On the GameCube, `pos$2426` (0x803F8A40, 0x78 bytes) is followed by `@3094` (0x803F8AB8), the next `.bss` object of this unit. Nothing references it (`build/GMSE01/asm/Map/PollutionLayer.s`), so the stray write is lost.
  On PC the 12 bytes landed on live statics (the `counter_x`/`counter_z` table indices), which sent `x_offset_table[counter_x]` out of bounds.
- **Fix:** `src/Map/PollutionLayer.cpp`, under `SMS_PORT`: `pos[11]`, whose 11th slot absorbs the write the way `@3094` does.
  The `> 10` behaviour is kept, so the effect positions cycle exactly as on the GameCube.


## Open note for w6 from w8 (E25): `snapshot_color_sample0` fallback (`aurora_msaa_depth.patch`)
- `tex_copy_conv.cpp snapshot_color_sample0` returns without writing the snapshot when its compute pipeline could not be created (the `failed` path) or the scene format is not RGBA8/BGRA8. The pooled snapshot texture then keeps whatever an earlier use left in it, so pixel counters and the CPU pollution maps would read stale data silently with MSAA on.
- Suggested fix: make it return `bool`, and in `encoding.cpp` fall back to the existing resolved-copy branch (`CopyTextureToTexture` from `copySourceTexture`) when it returns false. A resolved count is slightly off at polygon edges; a stale one is wrong.
- Smaller points:
  - For an sRGB scene format the packed bytes are linear values. That is harmless now because the EFB is `to_linear`; encode or reject such formats.
  - A uniform buffer and a bind group are created per snapshot. They could be pooled as in w8's E23 (performance only).
- Whoever owns the Aurora patches after w6 moves to multiplayer can pick this up; port_efb.cpp needs no change for it.


# Sanitizers (w10)

## Builds
- `python port/tools/build_aurora.py --asan [--run ...]` 竊・`port/build_aurora_asan/`, `--ubsan` 竊・`port/build_aurora_ubsan/` (both: `_asan_ubsan`). Separate directories, so the normal build is never invalidated.
- Only game/JSystem units are instrumented; port/ host code and Aurora are not.
- ASan: `-fsanitize=address -fsanitize-recover=address`, linked against VS18 LLVM's `clang_rt.asan_dynamic-x86_64` (+ `asan_dynamic_runtime_thunk`, `/wholearchive`; the static thunk clashes with ucrt's malloc); the DLL is copied next to the exe.
- UBSan: `-fsanitize=undefined -fno-sanitize=vptr,function,alignment,shift-base` with `clang_rt.ubsan_standalone` + onecore/shell32/dbghelp. Microsoft's ubsan runtime needs two ASan "continue on error" symbols; the build generates a tiny shim (`gen/ubsan_coe_shim.cpp`). `ubsan_standalone_cxx` is excluded (/MT, only for vptr).
- `--run` sets `halt_on_error=0`, so one run collects every report, written to `<out>/san.<pid>` (both runtimes share one `log_path`; it must be relative, a drive letter's ':' ends the option).
- **Game heaps (`SMS_PORT_ASAN`, `port/include/port_asan.h`):** `JKRExpHeap` poisons every free block (header included) and the slack after each allocation (beyond `ALIGN_NEXT(size, 4)`, the GameCube rounding); `JKRSolidHeap` poisons its free range and alignment slack, and re-poisons on `freeAll`/`freeTail`/`restoreState`. The two heap units are compiled uninstrumented (they walk their own poisoned headers) and call `__asan_(un)poison_memory_region` directly. So overruns between game allocations, use-after-free inside JKR heaps, and reads of free heap memory are reported like malloc bugs.
- `SMS_PORT_ASAN_SELFTEST=1` (asan builds, `src/main.cpp`) reads one byte past a 20-byte game allocation and one from a freed block: two `use-after-poison` reports prove the poisoning works.

## Runs
`--nullgfx`, 75 s each (about 4200 frames): title loop (with `SMS_PORT_THP_SPEED=8`: intro, title, attract movie, title) and `--warp N,0` for N = 1..9. All runs reach gameplay and keep going; none crashed.

## Fixed (each found by a sanitizer, each checked against the GC asm)
- S1 **`JASystem::DSPInterface::DSPBuffer::setBusConnect` reads past `connect_table[12]`** (ASan, audio thread at boot): sequences select bus 12..15 through `cmdBusConnect`. On the GameCube the read continues into the next `.data` object, `JASystem::Driver::FORCE_STOP_TABLE {0xF, 0xF, 0}`; under `SMS_PORT` the table carries those bytes as entries 12..19 (0 beyond, where the GameCube would read `Dvd::audioRootPath`). (JAudio: w5's area.)
- S2 **`SMS_InitChangeNpcColor` reads past `sMonteW_GlassBColorBuf[3]`** (ASan, Bianco Hills): Monte W NPCs with an individual-difference index 3..5. Under `SMS_PORT` the table is extended with the bytes the GameCube reads there (the following `.rodata`: `sMonteW_GlassBColorChange` and the `"_megane_mat"` string), so the glasses keep their GameCube colours (`src/NPC/NpcInitData.cpp`).
- S3 **`TAmenbo` (pond skater) used material indices as joint indices** (ASan heap overrun in `updateRipple`): `init` looked `cJointNames` up in the material name table; the GC asm reads `J3DModelData+0xB0` (`getJointName()`). Same transcription error in `TFireWanwan::init` for `"center"`/`"jnt_head"` (found by cross-checking every `getIndex` against the asm offsets). Plain fixes in NonMatching units.
- S4 **`TFireWanwanTailHit::performNodes` read node 5 of 5** (ASan): the second loop is `i < 4` in the GC asm (`cmpwi r30, 4`), the decomp had 5.
- S5 **`TGCConsole2` indexes `unk334[23]` of 22** (UBSan, every level): `unk330` starts at 0x17. On the GameCube `unk334[23]` is the next member `unk390` (a `TBoundPane*`) and `isVisible()/hide()` touch the high byte of its `unk4.x2`; on x64 the J2DPane layout made it the low byte, so the plain code saw "visible" and corrupted the rectangle. `SMS_PORT` helpers reproduce the GameCube access (`src/GC2D/GCConsole2.cpp`).
- S6 **float 竊・u32 conversions** (UBSan): MWCC converts through `__cvt_fp2unsigned` (negative 竊・0, 竕･ 2^32 竊・0xFFFFFFFF); a plain cast of a negative float wraps on x86. New `sms_cvt_fp2unsigned()` (`port/include/port_mwcc.h`, C and C++). Applied where it bites:
  - `TObjHitCheck::getTableIndex`: actors near the origin went to hit-check bucket 0xBE instead of 0 (missed actor-actor hits in every level).
  - `hx_wiper.c` (all 14 float sites): `Hxs2_Circle` skipped the whole iris loop when the radius exceeded half the screen; `Hx_Test4`'s `rstep` wrapped to ~4 billion once it went below 0.
  The other `bl __cvt_fp2unsigned` sites in game/JSystem code (listed below) only see non-negative values in the runs so far.

## Reported, not fixed
- `TMActorKeeper::createMActor` / `TModelDataKeeper` (ASan stack-use-after-scope, most levels): `registerDataAndJoinNewNode` stores the caller's `name` pointer, and `TMapObjBase::makeMActors` passes a stack buffer (`"%s.bmd"`). Later lookups `strcmp` against that dead stack slot. This is the original game's behaviour (same dangling pointer on the GameCube); with the same call depth the slot holds the same string, otherwise a model is loaded twice. Left as is.
- `J3DTevBlock4::load` (UBSan bounds, w6): loops 16 over `mTevKColorSel[4]`/`mTevKAlphaSel[4]` into the following members. Matching unit, identical byte-only layout on PC, so it reads the same bytes as the GameCube.
- `JASBNKParser.cpp:169` (`header->mInstOffsets - 1`, then `[i + 1]`): pointer arithmetic idiom, no out-of-range access.
- `RumbleChannelDataMgr::getChannelDataTbl` called on a null manager (`TMovieRumble`): the method never uses `this`. Harmless.
- Enum loads (`VITVMode` -1 in JDrama video, `J3DMdlDataFlag` combinations), `J3DModel.cpp:690` null + offset: harmless.
- float 竊・s16/u8/u16 out of range (angles 32768..65535, colours): MWCC does `fctiwz` and keeps the low bits; clang's x86 code does the same for any value that fits in an int, so behaviour matches. Not changed.
- (The remaining `__cvt_fp2unsigned` sites are now all converted, see S7.)

## S7. Every float → u32 site now converts like MWCC
All 60 `bl __cvt_fp2unsigned` call sites in the GameCube game/JSystem asm were mapped back to source; each one now goes through `sms_cvt_fp2unsigned` under `SMS_PORT` (`#ifdef` or a file-local macro that is the plain cast on GC):
- `CLBLinearInbetween<u32/unsigned long>` (`include/Camera/cameralib.hpp`, via `CLBPortFromFloat<T>`),
- `TGCConsole2::setTimer` (timer from stopwatch ticks),
- `TSelectDir::direct` ×2, `TMarDirector::nextStateInitialize`, `TMovieDirector::direct` (fade-out frame counts), `TMarioGamePad::reset` ×2 (button repeat),
- `hx_wiper.c` `Hx_GameOver` ×2 (bounce timers; the other 14 were S6),
- `J3DDrawBuffer::entryZSort` (z bucket),
- `JAIBasic::checkNextFrameSe` ×3 and `sendPlayingSeCommand` ×2 (`JAIGFrameSe.cpp`, priority and distance wait),
- `JAISound` `rand_u32` (inlined into all five `setSeInter*`; a product ≥ 2^32 now saturates) and `setSeInterPitch`'s random range,
- `TOscillator::calc` release-table index (a negative index was clamped to 15 instead of 0),
- `JPADrawCalcColorAnmFrameRepeat::calc`, `J3DGDSetFog` (`JRenderer.cpp`) and `FifoSetFog` (`PacketUtil.cpp`): the fog mantissa for B < 0,
- `MSSetSoundTL<T>::startSoundSetDyna` (random delay).
- Not changed: JGadget `TVector::GetSize_extend_` (`std-vector.hpp`, 6 template instances): `size_t` is 64-bit on PC and the value is a positive growth size, so the result is the same.

## Gameplay sweep (real D3D12 rendering, scripted input)
`--asan` and `--ubsan` (in parallel), `--warp A,E` for areas 1..9 and episodes 0..2 (27 runs each, 90 s, `SMS_PORT_THP_SPEED=8`), with
`SMS_PORT_PADSCRIPT="100-1500/60:A,+0-600:S0,+0-2400/45:A,+600-1200:S90+R,+1200-1800:S180,+1800-2400:S270,+1800-2400/50:B,+2400-3000:S45+R"`
(A through the intro dialogs, then run, jump, spray, dive and turn once Mario has control). Found and fixed:
- S8 **`TSleepBossHanachanManager` crashes in `clipEnemies` (null `mSaveParams`)** (ASan + UBSan, `--warp 4,1`): the sleeping Wiggler's manager and actor are built by constructors that are inlined into the MarNameRefGen creator on the GameCube; the fabricated `LINK_ALL_SOURCE` constructors in `SleepBossHanachan.cpp` left out `mSaveParams = new TDemoBossHanachanSaveParams("/enemy/sleepBossHanachan.prm")` and the zeroing of `mShinePosition` / `unk15C` (all visible in the `MarNameRefGen_BossEnemy` asm). Added.
- S9 **`JKRArchive::findFsResource` crashes on `"/../map/pad3/Setting.prm"`** (ASan + UBSan, `--warp 8,1`, `TEnemyMario::TSettingParams`): the path follows the root's `..` entry, whose directory index is 0xFFFFFFFF. On the GameCube `mDirectories + 0xFFFFFFFF` wraps to `mDirectories[-1]` (the tail of the RARC info block); on x64 it pointed 64 GB away. New `JKRArchive::portDirEntry()` (`SMS_PORT`, `JKRArchive.hpp` / `JKRArchivePri.cpp`) reproduces the 32-bit wrap and reads that tail the way the GameCube does (big-endian `mNum`/`mFirstIdx` from the unswapped flag/padding bytes).
- S10 **`TSeqParser::cmdIIRCutOff` reads past `CUTOFF_TO_IIR_TABLE[128]`** (ASan, Delfino Plaza): sequences pass index 128+. The GameCube read continues into `Player::s_key_table`, so under `SMS_PORT` indices 128..159 return the big-endian halves of those floats (same literals), 160+ return 0 (`JASSeqParser.cpp`, JAudio: w5's area).
Re-runs of `1,0`, `1,1`, `4,1`, `7,1`, `8,1` with the fixes: no new reports, no crash.
Reported, not fixed:
- **w6:** `--warp 7,1` under ASan once crashed inside the AMD D3D12 driver (`amdxc64.dll`, thread T21) from `aurora::gx::build_pipeline` (`port_deps/aurora/lib/gx/gx.cpp:463`, `create_pipeline` → pipeline worker). Not reproduced on the re-run; possibly ASan's interceptors in the driver's allocations, possibly a real bad pipeline state.
- The `createMActor` dangling-name report shows up in most levels (original behaviour, see above).

float → s32 (`fctiwz`): PowerPC saturates (≥ 2^31 → 0x7FFFFFFF), x86 `cvttss2si` returns 0x80000000 for any out-of-range value; the two differ only for values ≥ 2^31 (and for narrow casts of such values). The UBSan sweeps never saw a float→int value beyond ±1.5e5, so no site was changed; `sms_cvt_fp2int()` (`port_mwcc.h`) is there for any that needs it.



## A-8. Stage BGM never started after the first sequence (loading flag race); output scale revisited
- **Cause of the quiet "music":** after a warp, the stage BGM (`0x80010002` Bianco, `0x80010001` plaza) stayed in JAI state 0 forever; only the
  first sequence of each slot (entrance demo, title BGM) ever played. `JAIBasic::checkEntriedSeq` sets `mLoadingFlag = true` *after*
  `Vload::loadFileAsync`, and `checkDvdLoadArc` clears it when the load completes. On the GameCube the DVD read always takes a while; in the port
  `sdk_dvd.cpp` completes reads at once, so the (higher-priority) JAudio DVD thread finished and cleared the flag before the game thread set it, the
  slot looked "loading" forever and `checkEntriedSeq` returned early for every later sequence. Same class: `JAIGFrameStream` `LoadADPCM` sets
  `DvdLoadFlag` after `DVDReadAsyncPrio`, whose callback clears it. Fixed under `SMS_PORT` by setting both flags before issuing the read
  (`JAIGFrameSequence.cpp`, `JAIGFrameStream.cpp`). With it the stage BGM starts after the demo (slot 0: `80010002` state 4, loading 0).
  The root cause (synchronous completion of async disc reads in `sdk_dvd.cpp`) can bite other game code that sets a "busy" flag after an async read;
  not changed here (not w5's file).
- **Volume chain traced** (debug: `SMS_PORT_AUDIO_VOLDUMP=1` prints every root track tree and note-on factors): stage BGM root timed volume 0.866
  (squared, mode 0) x JAI outer 0.443 (= `MSBgm::smMainVolume` 0.75 x the seq table volume) = 0.332; child tracks 0.4-1.0 (squared); note volume =
  (velocity/127)^2 x instrument volume (~1.0); then x `MAX_MIXERLEVEL` (0.802 x 16384) and the pan curve. Every factor matches the JAudio code
  (`writeTimeParam` matches the GameCube object; `updateTrack`, `updateEffectorParam`, `checkPlayingSeqTrack` differ only in register allocation),
  nothing is applied twice and no u8/u16 or byte-order misread was found in this chain.
- **Output scale:** with the BGM actually playing, the A-4 reading left sequence music ~13 dB under the THP movie music (mixed at unity by the SDK)
  and the SFX. The scale is now taken literally from JAudio: DsyncFrame level in 4.12 (`DsetMixerLevel(x)` = x * 4096; the default 0x4000 = x4
  makes JAudio's default chain unity, inputGainDown 0.5 -> 0.25 x 4), and the auto mixer (which gets volume x 32767.5 with no channel mixer level)
  gets base gain 0x1FFF (0.25), the constant stored right after the oscillator tables in `DSPRES_FILTER` and exactly the normal path's cap at
  JAudio's default. (`title.afc`, the A-4 counter-argument, is only reachable as `MSD_STR_SPACEWORLD`, a Spaceworld-demo leftover.)
- **Measured** (`SMS_PORT_THP_SPEED=8`, WAV): Bianco stage BGM alone -17..-19 dBFS RMS; with jump/FLUDD/footsteps -13.7..-16; Entrance.thp music
  -14..-20; title stage -25 (music + ambience); clipping 0-177 samples per minute (system SEs, boot coin jingle). L/R balanced (-16.6/-16.5 dBFS).

## A-9. Filters and the downmix, checked against the microcode
- **IIR:** the routine at jdsp word 0x0C11 (entered when `filterMode & 0x20` and the four coefficients differ from {0x7FFF,0,0,0}, words 0x032A-0x034A)
  was run in a small emulator (`iiremu.py`, w5 scratch): its impulse response equals `y = c0 x + c1 x[n-1] + c2 y[n-1] + c3 y[n-2]` (Q15), the form
  `port_audio_dsp.cpp` uses (the microcode adds one sample of latency). Underwater coefficients (forced with `SMS_PORT_FORCE_UNDERWATER=1`, which makes
  `MSSeCallBack::setWaterCameraFir` act as if the camera were in water): {5521, 2476, 17028, 13932}, a low-pass with +12.9 dB at DC and -21.5 dB at
  Nyquist. In Noki Bay and Bianco the sequences apply it to SE/ambient voices only (24 % of voice-sub-frames under water, none dry); the stage BGM keeps
  {0x7FFF,0,0,0}, so the overall spectrum barely changes (energy above 2 kHz 4.3 % -> 2.7 % in Bianco).
- **FIR / distance low-pass:** not used by anything in the title, plaza, Bianco or Noki runs (statistics in `SMS_PORT_AUDIO_LOG`).
- **Downmix:** the microcode mixes the other buses into the front pair itself (jdsp 0x00EF-0x0114 at the start of a sub-frame, 0x03F4-0x0434 after
  the voices): 0x0F40 (previous sub-frame) -> L x1.0, R x-0.5615; 0x0CA0 (40 samples late) -> L x-0.5615, R x1.0; 0x09A0 -> both x0.7071; 0x0FA0 -> R x-1.0.
  Implemented as decoded (before: back buses were simply added to L/R, which put all FX-line returns sent to 0x0F40 on the left).
- **Reverb (FX lines):** SMS sets them once from FX scene 0 (no per-stage change exists in the code): all four on, 48 blocks (120 ms), returns to
  0x0D00/0x0D60 and 0x0F40 at 4096 (lines 0-1) or 2047 (lines 2-3), FIR {0, 0, 846, 846, 4715, 8343, 13421, -4096}. Caves/indoors differ only by the
  voices' own FX sends (sequence/SE parameters).

## A-10. Pause, coins, 1-up, Shine
- Pause (`SMS_PORT_PADSCRIPT=...:START`): `MSD_SE_SY_PAUSE_ON`, then `MSound::pauseOn` fades every BGM track to 0 over 60 frames and mutes the SE
  categories except system sounds (category 4): measured BGM -15 -> -41 -> -62 dBFS within 2 s, system SEs still audible; START again plays
  `PAUSE_OFF` and the BGM returns within ~10 frames. That is the game's behaviour (full mute, not a duck).
- Coin `0x4811`, red coin `0x4846`, 1-up `0x4841`, blue coin `0x4845`, Shine-get SE `0x4814` and the Shine-get fanfare BGM `0x80010009`, started with
  the new debug hook `SMS_PORT_SE_TEST=<call>:<hex id>,...` (`MSound::mainLoop`), all render: -11..-15 dBFS RMS over the BGM's -18 (blue coin clips 185
  samples). The boot jingle is the coin sound `MSD_SE_MV_COIN` (0x7915). Mario's voice clips (`0x78xx`) play in gameplay; movie voices come with
  the THP audio (A-7). Not done: collecting real coins/Shines through gameplay, Peach/Toadsworth in-engine cutscene voices.

Left to do (audio): a real-hardware reference for the absolute balance (A-8 is derived from JAudio's constants, not a recording); FIR and FX-line
tap order are not verified against the microcode (only the IIR and the downmix are); `sdk_dvd.cpp`'s synchronous async reads (A-8).



## E6. Airstrip goop vanished: Aurora's EFB was smaller than the GameCube's
- **Symptom:** the airstrip pollution map (512x512 `GX_CTF_R8`, drawn and copied at EFB (0,0)-(512,512)) emptied within a few frames (readback 5703 polluted texels, then 0) and its counter read "clean" after the 30-zero debounce. Plaza and Bianco maps (256 rows or less) were fine.
- **Cause:** Aurora's EFB render target was the configured frame buffer, 640x448 logical for SMS's render mode; the GameCube EFB is 640x528 and the game draws and copies anywhere in it before the display copy. Rows past 448 were clamped and the copy rescaled, so the map slid every frame.
- **Fix:** `port/patches/aurora_efb_size.patch` (shared Aurora tree, libs rebuilt): `vi::efb_size()` (at least 640x528), used by `gx::logical_fb_size()`; the render target is allocated at the presented frame buffer's scale extended to the whole EFB (1280x896 presented -> 1280x1056 target); presenting uses only the configured top-left part (present viewport and resample UVs).
  Port side: `port_efb.cpp` maps against `vi::efb_size()`; `port_aurora_sdk.cpp` peeks map against the EFB size and `SMS_PORT_SHOT` captures only the presented part (`port_efb_screenshot_ex(path, true)` captures the whole EFB, used by `SMS_PORT_EFB_DEBUG`).
- **Result (warp 0,0):** the map keeps its goop (memory 6306 polluted texels, readback 5601 then a stable 4548; pixel metric 4548). Mario walking into it at (-1882, 1136) is hurt (life meter, CPU collision map) and the goop renders (`port/build_aurora/efb_airstrip_goop.png`). Display is unchanged (`shot_2600`: airstrip intro at the right aspect, no extra rows).
- **Cleaning (warp 1,0, plaza goop around (576, 4992), `PADSCRIPT +60-800:G6200;2500,+800-1800:G700;4400,+1700-3200:R`):** spraying cleans a patch (`port/build_aurora/efb_plaza_cleaning.png`), the layer's CPU map goes 3333 -> 3252 -> 3113 polluted texels (sampled every 2nd texel) and the counter 13346 -> 13011 -> 12453 pixels, i.e. it follows the real map and never reads "clean" while goop is left. Cleaning a whole layer to "clean" was not done; the airstrip needs FLUDD picked up first.
- The `TPollutionLayer` world-position probe used to find the goop was a temporary local edit, removed.


## E7. Mario's silhouette behind walls
- With the scripted plaza walk above, Mario ended up behind a building wall: the game draws its silhouette effect (dark Mario shape inside a darkened circle) over the wall: `port/build_aurora/efb_silhouette_plaza.png`. The occlusion test (E4) and the silhouette pass work; no fix needed.


## E8. `GXSetCopyFilter` (vertical copy filter)
- Was a no-op in Aurora. `port/patches/aurora_copy_filter.patch`: the seven coefficients become weights for the EFB lines above (0+1), at (2+3+4) and below (5+6), out of 64; `vf == GX_FALSE` is the hardware's identity filter. They travel through the FIFO (`GX_AURORA_LOAD_COPY_FILTER`) into the copy's uniforms and `sample_efb` in the texture-copy shader applies them one logical EFB line apart; a filtered copy always takes the shader blit.
- Users: the screen texture and mirror copies (`SMSVFilter_flicker`, via `JDrama::IssueGXSetCopyFilter` with flag 0x20) and `BathWaterManager`'s ripple copy (`{21,0,0,22,0,0,21}`). The pollution copies pass no filter. Bianco and its pause menu render as before (slightly softer pause background).





## A-11. Voice mixer gain law, read from the microcode (replaces the A-8 auto-mixer base and the in-sub-frame ramps)
- **Q16, not Q15.** The microcode starts in M0 (`8b00` at word 0x0059; every `M2` site restores M0 before returning), and neither voice mixer sets
  M2, so a channel volume multiplies as `(gain * sample) >> 16`. The bus store is saturating (40-bit mode). The output stage (0x0D22-0x0D31) is M0
  too, followed by `ASRNRX` by 4, which gives `(bus * level) >> 12`: the 4.12 output level of A-8 stands. All voice paths are therefore 6 dB quieter
  than in round 3. A full-volume music voice is now x0.625 at SMS's level 5.0. Bianco, the hotel and the title now peak at 13 000-22 000 with no
  clipping (the hotel peaked at 32 768 before).
- **Volume changes are smoothed, not ramped per sub-frame** (normal channels, 0x0386-0x03B6, mixer 0x0D0A):
  - The step is `(target - current) << 16 >> 5`. The first 64 samples use `current` and the last 16 use `current + step`.
  - The stored current becomes `current + floor((target - current) / 32)`, so a channel approaches its target with a 32-sub-frame (80 ms) time
    constant.
  - Note starts are not affected: `setMixerInitVolume` sets current = target.
  - Previously the port ramped linearly to the target within each sub-frame.
- **Channel delay:**
  - The level word's low byte delays a channel by that many samples (the source pointer moves back).
  - The low byte moves one step per sub-frame toward the high byte.
  - It is implemented, with 255 samples of history per voice, but unused in every run so far (`channel delays 0` in the `SMS_PORT_AUDIO_LOG` stats).
- **End request, normal channels** (0x0370-0x0385):
  - Each sub-frame, every channel's target becomes `current >> 1`.
  - The voice is done only when all six current volumes are 0.
  - Before, the port set done at once.
- **End request, auto mixer:** target = `current >> 1` (the auto mixer copies target to current every sub-frame, so this halves per sub-frame). The
  voice is done at current 0, about 15 sub-frames. Voice counts did not change (hotel: 24.5 average, 43 maximum, before and after).
- **Auto mixer** (0x0D32-0x0DC0, bus table at 0x0DD6), as decoded:
  - Position word: high byte left/right, low 7 bits front/back.
  - `P = {L*F, R*F, L*B, R*B} >> 16` from the quarter-sine table, where `X` = sin[i] and its mirror sin[i ^ 0x7F].
  - Direct gains `G = P * vol >> 16` go to 0x0D00, 0x0D60, 0x0F40 and 0x0CA0.
  - FX gains `G * (fx * vol >> 15) >> 16` go to **0x0E80, 0x0EE0, 0x0C00 and 0x0C50**, i.e. FX lines 2-3 plus two back FX buses. The port used to send
    to lines 0-1.
  - Ramp step `(P * (target - vol) >> 16) << 11`, the FX one times `fx >> 16`. Then vol = target.
  - At the start of each sub-frame, 0x0C00 (previous sub-frame) and 0x0C50 (40 samples late) are folded into lines 2 and 3 with the same x1.0 / x-0.5615
    spread as the main back buses (0x01A6-0x01C9, 0x01F0-0x0204).
  - The old 0x1FFF "auto base" is gone. The exact products give 0x1FFC at full volume, centre, which is still 0.125, the same as the normal path at
    JAudio's cap.


## A-12. FIR and FX-line tap order, checked against the microcode
- **Voice FIR** (0x0BF6-0x0C10, called from 0x0300-0x0328): the N history samples (VPB 0x0440, oldest first) are copied in front of the sub-frame,
  and `y[j] = sum c[k] * x[j - N + k]`. `c[0]` takes the oldest sample and `c[N-1]` takes `x[j-1]`, so there is one sample of latency. The rounded
  product (MOVPZ) is used, and the last N inputs go back to the history. The port had the taps reversed (newest first, including the current sample).
- **FX line** (per sub-frame, before the voices: 0x0172-0x01A5 with 0x012D; write-back after the voices: 0x0205-0x0230 with 0x01D6). Each line has
  an 88-word DSP buffer (bus 0x0DC0 + 0x60 * line):
  1. Words 80-87 move to 0-7.
  2. The block at the current delay position (written `blocks` sub-frames ago) is DMAd to words 8-87.
  3. If mode bit 0 is clear, the 8-tap FIR (0x0BD7, same tap order as the voice FIR) runs in place over words 0-79.
  4. Words 0-79 are mixed into the two return buses (0x00D8, Q15).
  5. If mode bit 1 is clear, the FIR runs again.
  6. The voices then add their sends to words 0-79, and that sum is written back to the delay line. The position then advances.

  SMS uses mode 2: the FIR runs before the returns, and the delay line holds `send + FIR(delayed)`. That matches the old port model except for the tap
  order. The port now emulates the buffer exactly, including the case where an earlier line's return into a later line's bus is overwritten.
- `port_audio_dsp.cpp` is changed for all of the above (`fxLinesRead` / `fxLinesWrite` / `fxFir`; the voice FIR block).


## A-13. Reverb sends: Hotel Delfino (warp 7,0), Corona Mountain (warp 60,0)
- Stage ids are from `stageArc.bin`: 7 = delfino (the hotel), 14 = casino, 16 = mareUndersea, 60 = coronaBoss. The FX config is the same everywhere
  (A-9). The per-place difference is the SE send: `MSHandle::setSeDistanceParameters` sets fxmix from `getMapInfoFxParameter(ground)`.
- **Hotel:**
  - Auto-mixed SEs carry fx `0x0E00`-`0x1C00`, and lines 2-3 get them: mean |in| 15-43, |out| 1-4.
  - No sequence voice sends to lines 0-1 there.
  - WAV -22.3 dBFS RMS, peak 19 935, 0 underruns.
- **Corona Mountain:**
  - fx `0x3100`-`0x7200`, mostly `0x4800` (about 4x the hotel).
  - Lines 2-3: mean |in| up to 118, |out| up to 11. Lines 0-1 carry the stage BGM's sends.
  - The WAV's autocorrelation has a small positive peak at 3 800-3 880 samples (the 120 ms delay plus the return path) in both 8 s windows tested.
- **Bianco:** lines 0-1 carry the stage BGM sends (|in| 57-170, |out| 10-31); lines 2-3 carry the SEs.
- New `SMS_PORT_AUDIO_LOG` statistics every 2000 sub-frames: `fx mean |in| ... |out| ...` per line, plus channel delay and end-fade counts.


## A-14. NPC voices in in-engine scenes
- **Debug aids:**
  - `SMS_PORT_AUDIO_SE_ONLY=1` silences the normal-mixer voices (the sequence music) in the capture. Volumes and fades still run.
  - `SMS_PORT_AUDIO_LOG` now logs every NPC sound start (0x80xx, 0x88xx-0x89xx) with its time in the `SMS_PORT_WAV` capture
    (`JAIBasic::startSoundBasic`, under `SMS_PORT`; `port_audio_out_wav_seconds`).
- **Airstrip intro (warp 0,0, after openingA):**
  - Peach `MSD_SE_NPC_VM_PEACH_WORRY` (0x8852) is requested at 72.2 s, together with Pianta voices `VA_MONTM_ANGRY1/2` and `DANCE1/2`, footsteps and
    ukulele.
  - The SE-only capture shows a pitch-gliding harmonic burst at 72.2-72.8 s (`port/build_aurora/audio_airstrip_peach.png`).
  - Toadsworth does not speak before Mario moves. His `VM_KINOJ_NORMAL` (0x8856), started in the airstrip with `SMS_PORT_SE_TEST`, renders as
    0.7 s bursts at -17 dBFS at 75.1 s and 81.7 s, with silence around them (`audio_airstrip_toadsworth.png`). So his voice data is in the airstrip
    scene bank and plays.
- **Plaza arrival (warp 1,0, `SMS_PORT_THP_SPEED=8`, A every 80 frames):**
  - After openingBA/BB/BC the plaza scene requests Pianta voices `VA_MONTM_ANGRY1/2` about every 10.7 s, plus `MONTM_SINK` and `MONTW_SINK`.
  - A burst at -20 dBFS lines up with the 46.2 s request (`audio_plaza_pianta.png`).
  - No Peach or Toadsworth line in this scene.
- **Bianco episode 1 intro (warp 2,0):** Pianta `VA_MONTM_DANCE1/2` from 3.6 s on, plus 0x8913, 0x896A and 0x8979.


## A-15. ARQ completion is asynchronous; ASan hunt
- `ARQPostRequest` and `ARStartDMA` used to complete inside the call. The copy still happens at once, but the callback now comes from the interrupt
  thread (`port_irq_raise`) after a FIFO "bus" delay of 20 us + 12 ns per byte, the same style as w7's DVD fix (`sdk_audio.cpp`). Bianco, plaza and
  hotel runs: BGM fine, 0 underruns, no "pool exhausted".
- **ASan builds** (`build_aurora.py --asan`, `ASAN_OPTIONS=halt_on_error=0`):
  - Runs: plaza 200 s with input, Bianco 160 s, title/attract with SE tests.
  - No finding in audio code, and no `TChannelMgr::getListHead` crash; those were most likely the round-1 bitfield bugs.
  - One non-audio report in both stages: stack-buffer-overflow / stack-use-after-scope in `strcmp` from `TMActorKeeper::createMActor`
    (`src/Strategic/ObjModel.cpp:164`), via `TCoin::initMapObj` <- `TNpcCoin` <- `TBaseNPC::setIndividualDifference_`. Reported, not touched.

Left to do (audio): a real-hardware reference for the absolute level (A-8 and A-11 come from the microcode and JAudio's constants, not a recording).
Bus overflow with the FX-return mixer (0x00D8 runs in 16-bit mode and its stores may wrap, not saturate) is clamped instead. Unconfirmed: the junk
low accumulator bits that can delay `done` by a few sub-frames in the normal-channel end fade are not modelled. `sdk_dvd.cpp`'s synchronous async
reads (A-8) belong to w7.
- w14 note for w5: the AR/ARQ completion delivery in `sdk_audio.cpp` was changed by w14 (marked `w14`): one ordered queue that also waits until the posting thread has moved past the code after `ARQPostRequest` (a lost-update hang in `JASystem::Dvd::loadToAramDvdTMain`), and the default cost is now the fixed 20 us (`SMS_PORT_ARQ_BUS_TIME=1` restores 20 us + 12 ns/byte). Details: "Performance (w14)", "ARQ race".

## A-16. 5.1 / 7.1 surround output (user request)
- **Setting:** F1 > Audio, "Speakers / headphones" (above w9's "Controller speaker" group in the same tab), and `[audio] output =
  stereo | original | 5.1 | 7.1` in port_settings.ini (next to w9's `controller_speaker` keys); `SMS_PORT_AUDIO_OUTPUT=` overrides it for one run
  (not saved). Default `original`: the game's Sound option decides and the Pro Logic II matrix-encoded stereo goes out exactly as before (none of
  the extra work below runs). `Stereo` forces stereo mixing (mono stays mono).
- **Game routing:** `JAIGlobalParameter::setParamSoundOutputMode` (SMS_PORT) remembers the player's value and mixes as
  `port_audio_game_output_mode(value)`: surround (2) for 5.1 / 7.1. It is the only path to the output mode (Application boot, Option menu). The saved
  option (`MSF_SOUND_MODE`) is never touched; a menu change re-applies it at once (`port_jai_reapply_output_mode`). Log: `game sound mode 1, mixing as 2`.
- **Channels** (`port_audio_dsp.cpp`, from the same buses as the microcode downmix, taken before the matrix):
  - FL = 0x0D00, FR = 0x0D60 - 0x0FA0 (0x0FA0 stays on the right as in the downmix).
  - RL / RR = the back buses 0x0F40 / 0x0CA0 without the matrix delays, plus the reverb of the auto mixer's back FX sends 0x0C00 / 0x0C50.
    Those two buses are FX sends, not dry channels: the microcode folds them matrix-encoded into FX lines 2-3 (x1.0 / x-0.5615), and lines 2-3 return
    to 0x0D00 / 0x0D60 and 0x0F40 (log: `fx2 ... out 0d00 vol 2047, out 0f40 vol 2047`). In 5.1 / 7.1 two extra "rear" lines with line 2's and
    line 3's settings carry 0x0C00 -> RL and 0x0C50 -> RR unencoded, and two "fold" lines fed with the encoded fold alone subtract its share from the
    discrete buses (the lines are linear up to the 16-bit stores). So a source behind the camera keeps its reverb behind; nothing else changes.
  - FC = the centre bus 0x09A0 plus a conservative phantom centre: only content the fronts share strongly (smoothed correlation > 0.3, 50 ms),
    at most half the mid signal (-6 dB), half of it taken out of each front (a centred mono sound keeps its summed level), 10 ms gain glide.
  - LFE = 2nd-order 120 Hz low-pass of 0.25 x (FL + FR + FC + RL + RR) (before the phantom split). Output level and clamping as for stereo.
  - 7.1: the rear pair goes equally (-3 dB) to back and side.
- **Plumbing:** the renderer keeps a 6-channel shadow per DSP output buffer; `JASAiCtrl::vframeWork` (SMS_PORT) pairs it with the AI buffer it builds
  (`port_audio_mc_bind`), adding whatever the extra mix put on top (THP movie sound) to FL/FR; `port_audio_out_ai` sends 6 / 8 channels to SDL3 in
  SDL's order (FL FR FC LFE BL BR [SL SR]); the THP stream is upmixed to FL/FR. If the default device reports fewer channels than chosen, the stream
  stays stereo (the matrix downmix of the surround mix), with a log line and the menu's "Now:" note; `SMS_PORT_AUDIO_FORCE_MC=1` opens the 6 / 8
  channel stream anyway (SDL downmixes) for testing. `SMS_PORT_WAV` records 6 / 8 channels in 5.1 / 7.1. w9's DualSense speaker tap is unchanged.
- **Positional check** (new: `SMS_PORT_SE_TEST=<call>:<id>@<dist>;<deg>` starts a positional SE `<dist>` units from the audio camera, `<deg>` from
  its view direction, and logs the WAV time). Bianco (warp 2,0), coin SE 0x4811 at 600 units, 33 events cycling 0 / 180 / 90 / 270 degrees,
  `SMS_PORT_AUDIO_SE_ONLY=1`; energy in the 400 ms after each event minus the 400 ms before, averaged per angle (dBFS):

  | angle | FL | FR | FC | BL (RL) | BR (RR) | rear share |
  |---|---|---|---|---|---|---|
  | 0 (in front) | -24.1 | -24.3 | -27.7 | -56.8 | silent | 0.00 |
  | 90 (right) | -34.5 | -23.8 | -45.9 | -55.3 | -23.9 | 0.47 |
  | 180 (behind) | silent | silent | silent | -22.6 | -22.6 | 1.00 |
  | 270 (left) | -22.7 | -36.4 | silent | -22.7 | -60.3 | 0.49 |

  7.1 (same test, 18 events): identical pattern, back = side = rear - 3 dB. Stereo device without forcing: "5.1 chosen, but the output device reports
  2 channels: stereo fallback". THP (openingBA in 5.1): FL/FR -21 dBFS, the other channels carry only game sounds.
- **Underruns:** Bianco 5.1 100 s, 7.1 80 s, plaza 5.1 100 s (forced 6 / 8-channel SDL streams on this machine's 2-channel WASAPI device):
  0 underruns, 0 drops.
- GameCube build unchanged (`build/GMSE01/mario.dol: OK`). README (package text in `build_aurora.py`) has a Sound section.
- Not merged from my previous round: the `JAIGFrameSequence` / `JAIGFrameStream` flag-order patches (w7 reverted them, W7-H).

## Boss fights (w5)
Scripted fights, played on real game logic with `SMS_PORT_PADSCRIPT` bots. Everything below is debug-only, off by default, under `SMS_PORT`.

**Tools**
- `SMS_PORT_BOSSTRACE=1` (or `=<class substring>`): one `[boss]` line per nerve / HP change of every boss-like spine (`TSpineBase::update` ->
  `port/src/port_bosstrace.cpp`; class and nerve names from the PDB, `port_symname.cpp`), Shine state changes (`TShine::control`), and per-boss
  details (Petey's head hits, Gooper's tentacles and beak releases, King Boo's slot results, Wiggler's sprout choice, the coaster turret, the tub grips).
  The boss-specific SMS_PORT blocks also tell the bots what to do (`gPortBossWant` + targets in `gPortBossAux` / `gPortItems` / `gPortThrowAt`).
- PADSCRIPT tokens (port_main.cpp, w7's file; mine are marked w5): `AIM<d>;<deg>;<gun>` (stand `<d>` in front, squat-spray with nozzle pitch `<gun>`,
  pump the trigger; stomp mode for weak spots; spray-a-point mode; walks to a collectible Shine when no boss is left), `PULL` (grab-and-pull),
  `THROW` (fetch an item, throw it at a point), `TURRET` (coaster: turn and fire), `ROCKET` (rocket nozzle + ground pound on a target),
  `B<d>;<deg>`, `FB`, bare `RA` (= w7's `RA192`), `NB` (entry applies only while no boss is tracked: A presses for the Shine demo / save prompt).
  Several bots in one entry: the boss code picks (`gPortBossWant` 0 pull, 1 spray, 2 stomp, 3 wait, 4 throw, 5 / 6 spray / wait at a point).
  A beaten boss (hit points 0, or dead) is no longer tracked, so AIM goes for the Shine.
- `SMS_PORT_BOSS_TARGET=<class>`, `SMS_PORT_PULL_DIR=<deg>` (pull the beak toward open floor), `SMS_PORT_TURRET_SIGN=-`.
- Test aids (change the game state; test runs only): `SMS_PORT_GODMODE=1` (Mario's health never drops; falling into the void still kills),
  `SMS_PORT_INFWATER=1` (FLUDD stays full, for arenas where the bot cannot refill), `SMS_PORT_DEBUG_CALL=flag<hex>@<frame>` (sets a flag the way a
  stage script does; added to w13's knob).
- `SMS_PORT_NODRAW=1` for the long runs: with real D3D12 an access violation inside the AMD driver's pipeline creation
  (`aurora::gx::build_pipeline` -> `D3D12GetInterface`, a pipeline-worker thread) ended one King Boo run at f9417; it is the same driver crash w7
  reported at the airstrip Piranha (for w6). Game logic does not depend on drawing.

**Decomp correction**
- `TMario::initModel` (Pinna / Koopa coaster): the cart was placed with the rail's base matrix and the rail with the cart's joint, reversed. The asm
  (`MarioDraw.s`, 0x802472D0 and 0x802473D0) copies the rail model's node matrix 0 (`lwz r3,0x58(rail model)`) into the cart model's base matrix
  (`addi r4, cart model, 0x20`): `mTorocco->getModel()->setBaseTRMtx(mPinaRail->getModel()->getAnmMtx(0))`. Fixed in plain source (the unit is
  NonMatching, not linked into the DOL). Before, the rail's base matrix was taken from the cart's not-yet-computed joint and never reset, so the
  cart matrix copied from the rail every frame (`calcBaseMtx`) was garbage (the predecessor's NaN camera at pinnaBoss); now the coaster rides
  (screenshot at warp 58,1) and the fight completes.

**Results** (each run: `--warp` to the boss stage on a fresh save, `SMS_PORT_GODMODE=1`; "plaza" = `TApplication state 5 (area 1/0)` after the save prompt)
- **Petey Piranha (warp 55,0, biancoBoss): complete.** `AIM800;0;-0x1800` + `NB` A presses, `SMS_PORT_BOSS_TARGET=TBossPakkun` (else the bot turns to
  a small `TStayPakkun` after the fight). Water into the open mouth (Vomit) -> Swallow -> TumbleIn -> Tumble -> jump + ground pound on the navel:
  StompReact f4595 (hp 2), f6146 (hp 1), PreDie f8467, Die f8584; Shine at (-600, 250, -200) collectible f9423, collected (shines 1, flags 8242),
  save prompt (state 11) f10083, plaza f10644. Standing 800 out matters: inside the 600 swing length he only swings (Wait <-> Swing), the original rule.
- **Gooper Blooper (warp 59,0, ricco8): complete.** `PULL+AIM700;0;-0x0C00`, `SMS_PORT_PULL_DIR=-44`, `SMS_PORT_INFWATER=1`. Front tentacles pulled off
  (TentacleDamage f1076, f1614), face sprayed (EyeDamage), beak pulled and let go past `mSLBeakLengthDamage` (800 in this stage's prm, read by the
  bot) -> BeakDamage f2128 (hp 1); second round f3078-f4172 -> Die f4242; Shine collectible f5128, collected (shines 1), save prompt f5703,
  plaza f6262. Findings: letting go short of 800 scores nothing (as on the GameCube); pulled straight away from him, Mario reaches the pier edge
  first and the round-2 releases happened at ~585 (status change -> THROWN), hence the pull direction knob.
- **Mecha-Bowser (warp 58,1, pinnaBoss1): complete.** `TURRET`, then `+1@0:AIM700;0;0` in the next stage. The bottle rockets on the coaster are picked
  up by spraying them (`TRocket::behaveToWater` -> on the nozzle) and fired with R; the bot turns the cart toward a waiting rocket, sprays it, turns
  to Mecha-Bowser and fires: Break f2810, f3518, f7210, f10236 -> stage change to 13/7, Shine collectible, collected (shines 1), save prompt f19094,
  plaza f19650. (`TTinKoopa::receiveMessage` only reacts to `ACTOR_TYPE_ROCKET`; plain water does nothing, as in the original.)
- **King Boo (warp 56,0, delfinoBoss): complete.** `THROW+AIM600;0;0x0400`, `SMS_PORT_INFWATER=1`. Spray the tongue (Appear) -> slot; when the
  slot gives chilis and fruit (PrepareSlot), fetch a chili and throw it into the mouth (`setSpicy`), then throw a fruit at him while he is spicy
  (`checkHitObject` -> kill -> Die, `decHitPoints` because spicy): Die f2625 (hp 2), f17213 (hp 1), f23949 (hp 0); Shine collectible f24627,
  collected (shines 1), save prompt f25199, plaza f25761. Findings, all original rules: B while running is a dive (stop before picking up);
  items rest against the slot / walls, so the walk stops ~120 short and the pick-up has to come from there; King Boo only comes back from
  HideWait while nothing stands in his kill area (Mario parked in the middle kept him hidden); a fruit that hits him while he is not spicy only
  makes him hide.
- **Eely-Mouth (warp 57,0, mareBoss): complete.** `EEL` + periodic A. Mario is in the dive status down there (`TMario::diving`: the stick moves
  him level, gravity sinks him, the hover nozzle lifts him and its jets wash what is below; air drains on its own, `SMS_PORT_GODMODE` now keeps
  it full too). Hovering at -17000 over the hole brings the eel up (mouth cube, Appear f2562); the bot then hovers 400 above the nearest tooth
  with hit points left, R held while below that depth. All eight teeth must come down to 1 (`deadCheck`); each time it eats Mario (16 Eats)
  `quickBack` refills the unfinished teeth, as in the original. Die f35219; Shine collectible f36449, collected (shines 1), save prompt f37845,
  plaza f37897.
- **Phantamanta (warp 6,0, sirena0 = Sirena Beach episode 1 "The Manta Storm", not Ricco): complete.** The battle starts on flag 0x50007
  (`MSF_MANTA_SPAWNED`), which sirena0's `appearmanta.sb` sets; its trigger was not found, so `SMS_PORT_DEBUG_CALL=flag50007@1000` sets it (a
  0x5xxxx stage flag set through `SMS_PORT_WARP_FLAGS` does not survive the stage setup). `AIM0;0;0x0400` (no stand point: close to 1200 and spray
  from there) and the tracker follows the nearest live manta. AppearDemo f1002, 33 splits, last MantaDeath f16614; Shine f18196, collected
  (shines 1), save prompt f19138, plaza f19714.
- **Wiggler (warp 4,2, mamma2 = Gelato Beach episode 3 "Wiggler Ahoy!"): 2 of 3 hits, not finished.** He tips over when he walks over a raised
  sand mound: the bot sprays the sand sprout / sand-bomb base nearest to him until it is full height (sand bomb 2 expanded, state 8, then
  Tumble at its spot), then stomps the weak segment (unk174) while he is Down: Damage f4231 (hp 2), f8325 (hp 1). Left: his later falls landed in
  the sea (segment at y -200..-460, Mario cannot ground pound in water), and the stomp needs the segment's joint matrix (its `mPosition` lags
  ~600 units; fixed in the bot) and a jump from up to 350 away (the lying body blocks walking closer).
- **Final Bowser (warp 60,0, coronaBoss): 1 of 5 grips, not finished.** The rim is a five-pointed shape (radius ~3300 between the points, ~5300 at
  the grips; tp tests); `ROCKET` walks round at radius 3300, out to a grip, rockets up (y 5400 -> ~7950) and ground pounds over it: the grip
  takes the super hip drop (`TBathtubGrip::receiveMessage`, 5 -> 4 grips) every time. Left: the hit grip crumbles under Mario and the tub rocks
  (grip heights 4900-6170), and every retreat so far slid into the bath / off the tub (falls kill even with GODMODE); `SMS_PORT_WARP_FLAGS=20001=50`
  for the retries. Each new life restarts the fight with 5 grips (original).
- Stopped here on the coordinator's refocus (high-frame-rate effects); the bots and traces stay in for whoever picks the last two up.

## Handoff (w5, stopped on the usage limit)
- Merge-ready: A-16 5.1/7.1 surround; boss tools + `port/PLAYTEST_BOSSES.md` (Wiggler 4,2 and Bowser 60,0 left for the user); MarioDraw.cpp decomp correction.
- Half-done, built but not fully checked: high-fps particles/FLUDD water (`port/src/port_interp_fx.cpp` new; hooks in `JPAEmitterManager::drawBase`,
  `ModelWaterManager.cpp` makeEmit/garbageCollect/perform CUE_CALC_VIEW; accessors `port_interp_interpolating/alpha` (same text as w9's) and
  `port_interp_states` added to w9's `port_interp.cpp`). Logic frames only record, display frames lerp and restore; 30 fps does nothing.
- Checked: Bianco 120 fps, a traced FLUDD drop moves monotonically L -> I -> I -> L, including the "newer state already entered" case; no crash.
- Not done: JPA trace check (`SMS_PORT_INTERP_FXLOG=1` logging just added, not run), plaza/Ricco/Corona shots, 30 fps replay identity, sun/lens flare review, GC shasum, smoke, merge_check.
- Next: run `SMS_PORT_INTERP_FXLOG=1` at 120 fps, check particle traces; then the checks above; drop the FXLOG trace code if noisy. `SMS_PORT_INTERP_FX=0|jpa|water` for A/B runs.




## R10. Pinna Park (13,2): Aurora FATAL `unsupported logic op 10`, then every later start dies
- **Cause:** `GX_BM_LOGIC` with `GX_LO_INV`: `TMario::drawLogic` draws Mario in white with `mBlendLogicOp`, and JPA particles pick ops from `JPABaseShape::stLogicOp`. Aurora's `to_blend_state` handled only CLEAR, COPY and NOOP.
  Worse, the pipeline config was recorded in the persistent cache before it failed, so every later start died while prewarming it (the first QA sweep lost every stage after Pinna Park).
- **Fix:** `aurora_pipeline_wait.patch` (e): all 16 logic ops approximated with blend factors (WebGPU has no logic ops); exact for 0/1 colours and a white source, which is how the game uses them.


## R11. 2044 GX pipelines per full sweep, startup E_OUTOFMEMORY retries
- **Cause:** the pipeline key contained the constant destination-alpha value (`GXSetDstAlpha(GX_TRUE, a)`), although only "off / zero / non-zero" changes the pipeline; the value is the blend constant. Masking fields of the 2044 recorded configs showed 1310 differing only there (fading silhouettes and effects).
  With a cache that large, startup prewarm made the driver return E_OUTOFMEMORY for many pipelines (the R6 retries kicked in dozens of times per stage).
- **Fix:** `aurora_pipeline_wait.patch` (f): normalised in `populate_pipeline_config`; `GXPipelineConfigVersion` 13 -> 14 so per-value duplicates in existing caches are pruned. 899 pipelines in the re-run sweep, no OOM retries.
  Other key fields that still multiply pipelines a little: the POS array stride (315 extra; different vertex formats) and the alpha-compare references (134; would need uniforms).


## R12. Corona: Aurora FATAL `convert_texture: unknown texture format 22`
- **Cause:** `GX_TF_Z24X8` (the Corona bath water's depth copy, `BathWaterManager`) bound as a texture before any `GXCopyTex` reached it, so Aurora tried to decode it from RAM.
- **Fix:** `aurora_depth_tex_formats.patch`: Z8 / Z16 / Z24X8 decode with the I8 / IA8 / RGBA8 tile layouts they share.


## R13. Bundled pipeline seed and QA sweep
- **Seed:** `port/data/initial_pipeline_cache.db` (899 pipelines), generated by `port/tools/qa_sweep.ps1` + `port/tools/make_pipeline_seed.py`; `build_aurora.py` copies it next to the exe and into `--package` (marked w6 additions). Aurora merges it read-only into the per-user cache (`Seeded pipeline cache ... 899 rows merged`).
  Cold-start hitch, plaza warp, retraces for frames 1200-1800 (600 = none): 1397 without the seed, 801 with it, 607 with a warm cache. Bianco's first 600 frames still cost about +330 retraces: the single pipeline thread is busy prewarming when the stage asks for its own pipelines.
- **QA sweep:** `port/build_aurora/qa_sweep/` (31 stage/scenario runs, 5 shots each) with `index.md` listing what looks right and wrong.
  Open visual items: the black band under every shot (w8's 640x528 EFB, screenshot/presentation crop), the faint FLUDD water stream (refraction pass built from EFB copies, w8), Mario as a pink glowing blob undersea, Pianta's brightness, the offset silhouette shadow on water beds.
  Gameplay: Bianco boss (55,0) crashes in `TPollutionLayer::cleaned` from `TMario::gunExec` (not rendering).
- **Dirty Mario:** not captured. The airstrip goop floats on the sea and the scripted walks ended in water; the dirt texture path was checked statically in R9.





# Level sweep C (w13): Delfino Plaza story states, secret courses, Corona Mountain, Bowser

Stage numbers (from `data/stageArc.bin`, the `ScenarioArchiveNamesInStage` table `TApplication::mountStageArchive` reads):
1 = Delfino Plaza (`dolpic0, dolpic1, dolpic10, -, -, dolpic5..9`: scenarios 0, 1, 2, 5, 6, 7, 8, 9; 3 and 4 are `none.arc`),
20-27 = `dolpic_ex0..7` (only 0-4 on the disc; `SMS_isExMap()` is 21-52, so 20 is not an EX map), 28/29 = `bia_ex0/1`, 30/31 = `rico_ex0/1`,
32/33 = `mam_ex0/1`, 34-39 = `pinna_ex0..5` (none on the disc), 40/41 = `sirena_ex0/1`, 42/43 = `monte_ex0/1`, 44/45 = `mare_ex0/1`,
46-52 = `coro_ex0..6` (52 = Corona Mountain, in `TMarDirector::changeState`'s banner list), 60 = `coronaBoss` (Bowser's bath).
On the disc: 20-24, 29-33, 40-42, 44, 46-48, 50-52, 60.

How the plaza layout is picked: `TMarDirector::moveStage()` turns a next area of (1, 0xFF) into `decideNextScenario(1)`
(`MarDirectorDirect.cpp`): `MSF_CORONA_VISITED` -> 2 (post-game); all seven Shadow Mario shines (6, 0x10, 0x1A, 0x24, 0x2E, 0x38, 0x42) -> 9;
`MSF_BOWSER_BOAT_CUTSCENE_TRIGGERED` -> 8; M on boathouse and lighthouse -> 7 with 10+ shines, else 6; M on statue -> 5;
`MSF_BIANCO_POLLUTED_PIRANHA_DEFEATED` -> 1; else 0. `TMarDirector::setupObjects()` then reads more flags: scenario 5 starts the Ricco
(3+ shines, no boathouse M) and Gelato (5+, no lighthouse M) unlock demos, scenario 8 sets `MSF_SHADOW_MARIO_EVENT`
(1: shine 0x21 and no Yoshi; 2: Yoshi, no plaza turbo nozzle, 25+ shines; 3: turbo but no rocket, 30+ shines) and `MSF_NOKI_AVAILABLE` (20+).
So `--warp 1,<scenario>` picks the layout directly, and the flags below make the objects match that story state.

## Knobs added (`Application.cpp`, `SMS_PORT`)
- `SMS_PORT_WARP_FLAGS=<items>` (with `SMS_PORT_WARP`): sets save flags on the warp's fresh save, after `firstStart()`.
  Items: `<hex flag>` (bool true / int 1), `<hex flag>=<dec>`, `S<hex>[-<hex>]` (`setShineFlag`, keeps the count right), and cumulative presets
  `story1`, `story5`, `story6`, `story7`, `story8` (no Shadow Mario event), `story8y` / `story8t` / `story8r` (events 1 / 2 / 3), `story9`, `story2`
  (each sets the flags `decideNextScenario`/`setupObjects` read for that plaza scenario plus plausible shines, visits and watched FMVs).
  Examples: `SMS_PORT_WARP=1,8 SMS_PORT_WARP_FLAGS=story8r`; `SMS_PORT_WARP_FLAGS=story9,20001=0` (no spare lives, to test the game over).
- `SMS_PORT_DEBUG_CALL=<what>@<frame>` (once per stage): `bathdemo` calls `TBathtub::startDemo()` in the Bowser fight (what the fifth super hip
  drop does), so the ending (streaming movie 14, epilogue, staff roll, omake, save prompt, title) can be driven; `tpshine` moves Mario onto the object
  named `繧ｷ繝｣繧､繝ｳ` (the shine at the end of a secret course), so the touch -> shine-get demo -> plaza path runs.

## C1. Corona Bowser fight: Mario falls through the bathtub (`JGeometry::TMatrix34::concat`)
- **Symptom:** `--warp 60,0`: Mario drops from y 5401 with no floor below (floor -11000) and dies (`MARIO_STATUS_FALL_DEAD`) about 150 frames after
  the intro; on later lives he only sometimes lands.
- **Cause:** decomp transcription bug in a JSystem header. `TMatrix34::concat(a, b)` (`JGMatrix34.hpp`) read `a.at(3, n)` / `b.at(3, n)`, past the 3x4
  matrix, and transposed the product; the 1-argument `concat(b)` transposed the rotation. The GameCube code (traced with a symbolic evaluator over the
  target asm of `TBathtubGrip::calcRootMatrix` and `TKoopaJrSubmarine::calcRootMatrix`) is a plain row-by-column 3x4 product (`m[i][j] = sum_k a[i][k] b[k][j]`,
  plus `a[i][3]` in the translation column). `TBathtubGrip::calcRootMatrix` and `TBathtub::setupCollisions_` build the tub's grip and inside-wall
  collision matrices with it, so the collisions were placed at garbage positions.
- **Fix:** both `concat` forms compute the product under `SMS_PORT`. Other callers fixed by this: `BathWaterManager` (drop matrices),
  `JDrama::TPolarCamera::perform`, `JDrama::TSmJ3DAct::perform`, `TKoopaJrSubmarine::calcRootMatrix` (the submarine was rotated the wrong way).
- **Verified:** Mario stands on the tub (floor 5400), takes Bowser's fire (`FIRE_DOWN`, hp 8 -> 1) and plays 6600+ frames over four lives.

## C2. Corona Bowser fight: Aurora FATAL `convert_texture: unknown texture format 22` at load
- **Cause:** port gap. `TBathWaterMeshRenderer`'s constructor makes `clear_z_TX`, a 4x4 `GX_TF_Z24X8` texture; Aurora cannot decode Z formats.
  It is sampled in `prerender` with colour and alpha updates off. `--nodraw` / `--nullgfx` do not get past it (not a J3D display list).
- **Fix:** `BathWaterManager.cpp` (`SMS_PORT_AURORA`) initialises it as `GX_TF_RGBA8`: read as colour, a Z24X8 texel is the RGBA8 texel with the
  same bytes (same 4x4 AR/GB tile layout), so the sampled values are the GameCube's.
- Not checked (w8): the depth copy `GXCopyTex(unk800A4, GX_TF_Z24X8)` that `makeHeightMap` reads back on the CPU (no crash).

## Found here, already fixed in main by w10 (taken from main, my versions dropped)
- Plaza goop cleaning crash in `TPollutionLayer::cleaned` (`pos[10]` written by the original `> 10` wrap; clang put `counter_x` there): main's entry 12.
- Plaza scenario 8, third Shadow Mario event: `JKRArchive::findFsResource` on `"/../map/pad3/Setting.prm"` (root `..` index 0xFFFFFFFF): main's S9
  (`portDirEntry()`, reproduces the 32-bit wrap).
- Game over never finished: `Hx_GameOver`'s `boundtimer` loads `boundtable`'s -8.0 as u32 (`__cvt_fp2unsigned` gives 0 on the GameCube, x64 gave
  0xFFFFFFF8, so the game-over wipe never ended and the director stayed in state 7): main's `HX_F2U` on those two loads.

## Status (Aurora build, real D3D12, `SMS_PORT_THP_SPEED=16`, test saves in a scratch `SMS_PORT_USER_DIR`)
Input for the long runs: A presses, stick sweeps and a G-autopilot tour of the plaza (ten waypoints between +-7000/+-9000, R spray every 90 frames,
A every 200). "Frames" are `[port] frame` counts (2 per game frame). Last full pass on main as of 16:33 plus C1/C2.

| Stage / state | Flags | Result |
|---|---|---|
| Plaza 0 (goop, first visit; arrival movies BA/BB/BC + save prompt) | fresh | 7200-8400 frames with the tour |
| Plaza 1 (kagemario movie, reload) | `story1` | 8400 frames, tour |
| Plaza 2 (post-game) | `story2` | 8400 frames, tour |
| Plaza 5 (statue M; Ricco + Gelato unlock demos) | `story5` | 7800 frames, tour |
| Plaza 6 (boathouse + lighthouse M, < 10 shines) | `story6` | 7800 frames, tour |
| Plaza 7 (10+ shines) | `story7` | 8400 frames, tour |
| Plaza 8, no event / event 1 (Yoshi pineapple) / event 2 (turbo) / event 3 (rocket) | `story8` / `8y` / `8t` / `8r` | 7200-8400 frames each, tour |
| Plaza 9 (flooded, all Shadow Marios beaten) | `story9` | 7800-8400 frames, tour |
| Plaza 3 / 4 | - | `none.arc`, not reachable in the game (crash in `TGuide::load`, not pursued) |
| EX 20-24, 29-33, 40-42, 44, 46-48, 50-51 | `story7`/`story9` | 3600-4200 frames each with deaths and restarts; no crash |
| EX shine-get -> plaza (`tpshine`) | `story9` | 21, 29, 31, 32, 40, 41, 42, 46, 47, 48, 50, 51: get demo, state 11, back to plaza 1/9 (20, 22-24, 30, 33, 44: no object named `繧ｷ繝｣繧､繝ｳ`) |
| Corona Mountain (52) | `story9` | 9600 frames, deaths in lava |
| Bowser fight (60) | `story9` | 7800 frames, four lives (C1, C2) |
| Game over (60, `20001=0`) | `story9` | death -> game-over wipe -> save prompt -> `TFlagManager::restore()` -> airstrip, as for an unsaved GameCube file |
| Ending (`bathdemo`) | `story9` | movie 14 -> epilogue -> staff roll -> omakeA -> save prompt -> title stage, 9600 frames (needs w6's `mVcdVatCmd` NOP padding, now in main) |

Seen, not mine:
- `GXPeekARGB` / `GXReadPixMetric` still log "unimplemented on Aurora" in the plaza (w8, if not already covered by `port_efb.cpp`).
- Environment: with two or more D3D12 windows open, one run failed with `D3D12 create graphics pipeline state failed with E_OUTOFMEMORY` and one crashed
  inside the AMD driver; both passed when rerun. Screen-copy screenshots pick up other agents' windows when they overlap.
- Not a crash, for whoever polishes the Corona boss units: `tools/semdiff.py` still shows logic differences in `BathWaterManager` (`perform`,
  `throwMario`, `initializeIfYet_`), `BathtubKiller` and `MapObjCorona` (`calcBathtubData`, `updatePosture_`).





## 13. DVD reads completed before the request call returned (stage BGM refused, flags stuck)
- **Symptom (found by w5):** after a stage's entrance demo, the stage BGM never started.
  `JAIBasic::checkEntriedSeq` refused every sequence because a slot's `mLoadingFlag` stayed set.
- **Cause:** `sdk_dvd.cpp` did every read inline.
  - `DVDReadAsync` ran its callback before returning, so code that sets a "loading" flag after issuing the read (as `JAIGFrameStream` does) never saw the flag cleared.
  - `DVDReadPrio` also returned instantly, so a higher-priority loader thread finished its whole request, callback included, before the lower-priority caller ran its next statement.
    That is the `JAIGFrameSequence` case: `loadFileAsync` hands the read to `JASDvdThread`.
  - On hardware, a request only starts the drive. A sync read blocks only its own thread, other threads run, and completion comes later as an interrupt.
- **Fix (port side only, `sdk_dvd.cpp` and `sdk_os_thread.cpp`):**
  - Async reads and `DVDPrepareStreamAsync` go into one queue, in issue order.
  - They complete in interrupt context through `port_retrace_defer`, at the first of:
    - no game thread runnable: `switchOut` raises an immediate one-shot interrupt that drains the queue;
    - the next VI retrace: `port_retrace_run()` from `viRetrace` in `port_main.cpp`;
    - a transfer-time bound of 0.2 ms + length at 20 MB/s (`port_defer_kick`).
  - Work deferred from inside a handler (chained reads) waits for the next drain.
  - The callback never runs before the caller's next statement.
  - Block state is WAITING while queued behind others, BUSY while first in line, then END or FATAL_ERROR.
  - `DVDGetDriveStatus` reports BUSY while anything is queued. `DVDCancel` and `DVDCancelAsync` cancel a queued request, whose callback gets `DVD_RESULT_CANCELED`.
  - Code that cannot sleep reads inline: interrupt context, or a host thread that is not a game thread (THP reader, card worker; `port_cannot_block`).
- **Sync reads:**
  - **Default:** a sync read first waits for the async requests issued before it (one drive), then reads inline.
  - **`SMS_PORT_DVD_SYNC_BLOCK=1`:** the hardware behaviour. The request joins the same queue and the caller sleeps until it completes, so other threads run meanwhile.
    - This fixes w5's `JAIGFrameSequence` case at the root, with the original flag order: plaza (warp 1,0) stage BGM averages 23 voices against 12 without it.
    - It is off by default because it exposes an intermittent JAudio crash at the file select (2 of 6 runs with START presses through the title; 0 of 6 by default).
      - The crash: `JAIBasic::processFrameWork` dereferences `mSeSequence == nullptr`.
      - A hardware watchpoint shows `checkStoppedSeq` cleared the handle because `checkSeqActiveFlag` returned 0 for the SE sequence 0x80000800.
      - No child of that track ended through `TTrack::mainProc`.
      - The rate moved with unrelated code-layout changes (debug prints that never fired took it to 0 of 4), which points to undefined behaviour in JAudio that the new timing exposes.
      - Next: w10's UBSan/ASan builds with `SMS_PORT_DVD_SYNC_BLOCK=1`, then make it the default.
- **w5's two flag-order patches:**
  - The `JAIGFrameStream.cpp` one (`DvdLoadFlag = 1` before the read) is no longer needed: async callbacks are deferred now. Revert it.
  - The `JAIGFrameSequence.cpp` one (`mLoadingFlag` before `loadFileAsync`) is still needed while sync reads are inline by default. It becomes unnecessary once `SMS_PORT_DVD_SYNC_BLOCK` is the default.
- **Tested in the default mode, real D3D12:**
  - Boot 竊・intro 竊・title 竊・file select: 6 of 6 runs, no crash.
  - Warps 1,0, 2,0 and 6,0 load and start their stage BGM request, with no crash.
  - The airstrip new-game path is unchanged.
- **Determinism:** the idle and retrace points are deterministic in game terms. The 20 MB/s bound uses host time, so w9's input replay may want it off (it only matters while another thread runs a long frame).
- **ARQ/AR DMA (w5's `sdk_audio.cpp`) still completes inline**, with the same hazard. Suggested change, using the same helper:
  - `ARQPostRequest` and `ARStartDMA` should do the copy and the callback in a handler passed to `port_retrace_defer`.
  - That handler should copy at completion time, not at post time.



# Performance (w14)

Goal: hold the original 30 fps with steady pacing and load fast, without changing game behaviour.
Measured on the user's i9-12900K + RX 9070 XT (D3D12) while 8 other agents' `sms_port.exe` and builds ran, so absolute numbers are noisy; A/B runs alternate both variants of the same binary through environment switches.

## Profiler: `SMS_PORT_PROFILE=1`
`port/include/port_profile.h`, `port/src/port_profile.cpp`. Off by default (each hook is one branch on a global).
Every `SMS_PORT_PROFILE_N` presented frames (default 300) stderr gets the present-to-present interval (avg, sd, min, p1, p99, max, frames more than 2 ms off), the whole-process CPU use, and avg/p99/max ms per frame plus calls per frame for:

| Row | What |
|---|---|
| `direct` | `TApplication::gameLoop` `mDirector->direct()`; in a stage split into `move` (simulation ticks, perform lists) and `gxgen` (draw perform lists: J3D/GX command generation) |
| `setupObj` | `TMarDirector::setupObjects` (main thread, at a stage load) |
| `viwait` / `wakelat` | main thread asleep in `VIWaitForRetrace` / retrace interrupt to main thread running again |
| `fifo` | waiting for Aurora's FIFO worker: the game's per-frame `GXDrawDone` (`THPPlayerDrawDone`) and the drain before the present |
| `endframe` / `beginframe` | `aurora_end_frame` / event pump + `aurora_begin_frame` (frame-slot wait = GPU back-pressure) |
| `audio` | `port_dsp_render_subframe` |
| `handoff` / `preempt` | host thread switch latency / time a readied thread waited for a lower-priority one to reach a scheduling point (the GameCube preempts at once) |
| `intrwait` / `irqlate` | contended `gIntr` (interrupts disabled elsewhere) / VI retrace interrupt lateness |
| `dvd`, `yaz0`, `swap`, `heap` | DVD reads (bytes too), Yaz0/Yay0 decompression (incl. its reads), J3D model/anim swap passes, `JKRHeap::alloc/free` |

At each `TApplication` state change a load measurement starts; when the stage's first frame is presented it prints `[prof] load: ... first stage frame presented N ms after the state change`, the load's DVD/Yaz0/swap/heap/setupObjects/audio totals and the read-ahead cache counters.
`SMS_PORT_PROFILE=2` also prints a backtrace where a thread ends a > 4 ms stretch without a scheduling point.
`SMS_PORT_PROFILE=3` instead dumps the main thread's stack (and the thread holding the CPU; every game thread with `SMS_PORT_PROFILE_ALL=1`) when no frame was presented for 150 ms (`SMS_PORT_PROFILE_HITCH_MS`), then resumes the threads. Debugging only (it suspends threads to walk them).

## What the profile showed
- The game is far from CPU-bound: main thread busy 2-8 ms of the 33.4 ms frame, game logic + GX generation 1-6 ms, audio 0.2-0.3 ms. 30.0 fps everywhere once loaded.
- Swap passes run once per file (magic marker): 2-3 ms per stage load in total. DVD: ~1450 reads / 15 MB per stage load. Heap: ~44000 calls, 2-4 ms. No debug logging in any per-frame path.
- `-O1` vs `-O2`: per-frame game time within noise; load CPU work about 5-10% lower. Clean build time unchanged (~50 s at `-j 16`).
- Plaza's 0.5 s extra load is the original's: `TMarDirector::thpInit` waits 0.5 s (`OSGetTick` loop) after preparing the plaza TV movie; it is in the US DOL (`802B3654`), so it stays.
- Port-caused problems found:
  1. VI retrace up to 1.5 ms late (millisecond condition-variable timeout; w9's later "sleep 1 ms short, spin" fixed the lateness by spinning up to 2 ms before every interrupt).
  2. A higher-priority thread readied by an interrupt (the audio thread on every AI/DSP interrupt) waited until the running thread reached a scheduling point: 4-20 ms while the main thread drew, 30-80 ms during `setupObjects`, 50-110 ms behind big synchronous DVD reads at boot, 220 ms behind `THPPlayerPrepare` on a cold file cache. The GameCube switches at the interrupt.
  3. The main thread held the CPU while Aurora waited for the GPU in `aurora_begin_frame` (100-270 ms seen under GPU contention), starving the audio thread the same way.
  4. The present moved one retrace later whenever a frame's CPU work exceeded one retrace (16.7 ms), so intervals alternated 50/17 ms. With the EFB readbacks (`port_efb.cpp`) draining Aurora's FIFO mid-frame, Bianco's `gxgen` is 4-6 ms (p99 13-16 ms) and this hit 1.5-3% of frames.
  5. ARQ completion bus-time model (w5, A-15: 20 us + 12 ns/byte): JASystem's DVD thread posts 32 KB chunks and spins until each completes while `setupObjects` (`MSound::loadGroupWave`) blocks on its queue. Bianco `setupObjects` 44 -> 200 ms, plaza +170 ms. Fixed after the restart (see "ARQ race" below).

## Changes
- **Interrupt timer** (`sdk_os_thread.cpp`): high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) + wake event; only the last 0.5 ms before a due time is spun. About the same lateness as w9's spin version, 6-15% of a core less process CPU.
- **Scheduling points** (`port_sched_point()`): `port_GXCallDisplayList`, `JKRHeap::alloc`, the inline read of `DVDReadPrio`. A pending switch to a higher-priority thread is taken there (only with interrupts enabled), as the GameCube would have taken it at the interrupt.
- **CPU release around host blocking work** (`port_blocking_begin/end`): `DVDReadPrio`'s inline read when 64 KB or more (the file handle is opened first; since w7 made `SMS_PORT_DVD_SYNC_BLOCK` the default, that path only runs with `SMS_PORT_DVD_SYNC_BLOCK=0`: sync readers now sleep on w7's queue, which releases the CPU by itself), and `THPPlayerPrepare`'s wait for the first decoded frame. Like a GameCube thread asleep during a DMA, other game threads run meanwhile. When no thread is runnable it calls w7's `onIdle()` like `switchOut`.
- **In-place preemption** (`port_preemptible_begin/end`): around the Aurora present and `THPPlayerDrawDone`'s `GXDrawDone`, threads of priority <= 9 (JUTException 0, audio 2, JASystem DVD 3, JKRAram/AramStream/Decomp 8) take the CPU at once. Lower-priority threads still wait: Aurora is not reentrant, and the draw-sync thread (15) peeks the EFB for the lens flare.
- **Present rule** (`port_main.cpp`, w9's area): present on the retrace after `TVideo::waitForRetrace`'s `VIFlush` (renamed to `port_VIFlush` by `build_aurora.py`), where the GameCube VI flips. Steady cadence for any frame CPU time below 33 ms; one retrace more latency than before, the same as the GameCube. `SMS_PORT_PRESENT_EARLY=1` restores the old rule.
- **DVD read-ahead cache** (new `port/src/port_dvd_cache.cpp` + `.h`; one marked hook per read path in `sdk_dvd.cpp`'s `doRead`): reads below 256 KB are served from 256 KB chunks (aligned in the host file for extracted folders, on the disc for images, so a compressed RVZ/WIA/GCZ block is decoded once and then copied from memory) in a 16-chunk LRU (4 MB); a read that continues the previous read of the same source prefetches the next chunk on a background host thread (at most 2 in flight; a read of a chunk in flight waits for it). Reads of 256 KB or more go straight to the destination. It changes only how fast the host copy is made: when a read completes for the game is still decided by `sdk_dvd.cpp` (w7's queue, `port_retrace_defer`, `SMS_PORT_DVD_SYNC_BLOCK`), and the bytes are identical (`SMS_PORT_DVD_READAHEAD=verify` compares every read with a direct one: 0 mismatches on folder, .iso and .rvz). Thread safety: own lock, never held during I/O; `port_disc_read` is thread-safe (w9). `SMS_PORT_DVD_READAHEAD=0` turns it off.
- **Build** (`build_aurora.py`): `-O2` (`SMS_PORT_OPT` overrides). No `-march` (x86-64-v3 would let clang fuse multiply-adds and change float results). ThinLTO is not usable: the `llvm-objcopy --redefine-syms` pass cannot rewrite bitcode objects.
- `SMS_PORT_W14_LEGACY=1` turns the scheduler changes off (w9's timer, no extra scheduling points, no CPU release) for A/B runs.

## Results
Pacing, same binary, warm pipeline cache, 2 rounds x 2 runs (before = `SMS_PORT_W14_LEGACY=1 SMS_PORT_PRESENT_EARLY=1`, i.e. main as of 17:00; steady-state windows after the load):

| Scene | Present interval sd (ms) | p1 / p99 (ms) | Frames > 2 ms off | Process CPU | `preempt` ms/frame |
|---|---|---|---|---|---|
| Title (attract) | 1.3-6.3 -> 0.40-0.46 (one run 5.7) | 30.7-31.8 / 34.8-58 -> 31.5-32.1 / 34.7-51 | 4-16 -> 1-12 per 600 | 26-38% -> 22-23% | 0.25-0.95 -> 0.06-0.20 |
| Delfino Plaza (1,0) | 0.18-7.0 -> 0.46-1.27 | 23.8-32.5 / 34.1-43 -> 31.0-32.2 / 34.6-36.0 | 5-91 -> 9-33 per 1800 | 44-58% -> 35-46% | 0.60-3.45 -> 0.40-0.79 |
| Bianco Hills (2,0) | 1.47-3.62 -> 0.55-0.81 | 30.1-31.3 / 34.9-43.8 -> 30.8-31.7 / 34.8-35.8 | 14-46 -> 12-31 per 1800 | 54-116% -> 42-52% | 0.93-2.16 -> 0.63-0.98 |

Plaza's own frames rarely pass 16.7 ms of CPU, so its gain is mostly the preemption and CPU columns.

Stage loads (state change to first stage frame), read-ahead off -> on (same binary; the rest of each load is game work, `setupObjects` waiting for the ARQ wave uploads, and plaza's 0.5 s THP wait):

| Source | Bianco load | Bianco DVD time | Bianco Yaz0 | Plaza load | Plaza DVD time |
|---|---|---|---|---|---|
| extracted folder | 271-272 -> 271-275 ms | 12.3-13.5 -> 3.8-4.0 ms | 47-48 -> 42-44 ms | 775 -> 779 ms | 12.4 -> 6.7 ms |
| .iso | 226-229 -> 226-271 ms | 6.7-7.3 -> 2.6-6.0 ms | 37-43 -> 35-46 ms | 773 -> 791 ms | 12.9 -> 6.1 ms |
| .rvz (zstd 5, 128 KB blocks) | 292-305 -> 258-297 ms | 27-30 -> 6.3-6.4 ms | 66-72 -> 50 ms | 807 -> 788 ms | 26.2 -> 6.9 ms |

Cache counters per Bianco load: ~1450 hits, 6-9 misses, ~60 chunks prefetched. The DVD layer was never the bottleneck (the folder/iso host reads were already 4-13 ms per load); RVZ gains most.
Earlier, against the pre-w9 timer (plaza, older tree): VI `irqlate` 0.24 -> 0.06 ms/frame, `preempt` 0.50 -> 0.15 ms/frame, present sd 0.48 -> 0.35 ms.

First visits with the bundled pipeline seed (w6, `initial_pipeline_cache.db`, empty per-run cache): Bianco 0 and 56 retraces lost after the load (no freeze); plaza 18 retraces in one run, but 672 retraces (a 6.7 s `beginframe` stall) in the other, probably while the 899 seeded pipelines were still being prewarmed on a busy machine. Without the seed the plaza lost ~575 retraces (9.6 s).

## Left as is (other owners)
- ~~ARQ bus time (w5)~~: fixed by the restart below ("ARQ race").
- **EFB readbacks drain the FIFO mid-frame (w8, `port_efb.cpp`):** the largest per-frame cost now (Bianco `gxgen` 1.2 -> 4-6 ms, p99 13-16 ms). Far below 33 ms, and the present rule keeps it from showing as jitter.
- **Async DVD reads in interrupt context (w7):** `completeReq` reads on the interrupt thread with the interrupt lock held; a big async read there delays the VI retrace for its duration (the read-ahead cache helps for small reads only).
- Boot: `initialize_bootAfter` waits ~150 ms on `loadWave` (main thread, game design).
- The emulated 59.94 Hz VI runs on the host clock, not the display's refresh (w9).
- Several runs here died inside the AMD D3D12 driver (driver thread) while 8 other instances shared the GPU, with and without these changes.
- `SMS_PORT_THP_SPEED` > 1 stalls the intro movie for ~3 s (the decoder skipping through the file); normal speed plays smoothly.

## Restart (w14, 2026-10-08 evening): rebased onto main, w9's frame-rate option
The predecessor's work was rescued to `workers\w14_rescue\` (its scratch copies and rebase scripts in `_predecessor\`) and re-applied by hand onto main as of 22:29 (w9's interpolation, w13 regions, w8 present capture, w15 multiplayer hooks; every hook in RESTART_BRIEF kept).
- **Present rule vs. w9's interpolation** (`port_main.cpp`): the flip-on-`VIFlush` rule applies while no display frames are drawn (frame rate 30, the default, and outside stages at any rate). While `port_interp_active()`, w9's pacing places the logic frame's present as before: its segments start at the first retrace after the logic draw, and `interpWait` defers the logic slot to that retrace, so presenting a retrace later would put display frames out of step.
- Display frames (`displayFrame`) are counted by the profiler and their `aurora_end_frame`/`beginFrame` are a preemptible block like the logic present; the main thread's retrace sleeps (`interpWait` too) are timed as `viwait`, so display-frame rendering counts as busy time.
- Profiler: "off by >2 ms" is measured against the window's median interval when it is not ~33.4 ms (frame rate 60 and up); load lines also print the ARQ counters.

## ARQ race (w14 edit in w5's `sdk_audio.cpp`, marked; w5 please keep)
`JASystem::Dvd::loadToAramDvdTMain` posts each 32 KB chunk and then does `++bufferFull` (a plain read-modify-write of a volatile); the completion `aramDmaFinish` does `--bufferFull` on the host interrupt thread, which runs truly in parallel. If the poster's host thread is descheduled inside its `++` for longer than the completion delay, one update is lost and the DVD thread (priority 3) spins forever in `while (bufferFull != 0)`, starving every lower-priority thread (the predecessor saw 3 hangs in ~20 runs with a 20 us delay; with w5's 12 ns/byte model the window is ~0.4 ms, so rarer, not gone).
Fix: AR/ARQ completions go through one FIFO queue (`arqComplete`) and are delivered after their transfer time **and** once the posting game thread has certainly gone past the code after `ARQPostRequest`: it passed a scheduling point since posting (new `port_thread_sched_epoch`, bumped in `checkPreempt`, i.e. every interrupt-enabled `OSRestoreInterrupts`/`OSEnableInterrupts`, `port_os_poll`, `port_sched_point`; for the DVD thread that is the next chunk's `DVDReadPrio` or `DVDClose`), or it is no longer the running game thread, or its host thread used 200k CPU cycles (~50 us) since posting (the busy-waits with no SDK call in between; time spent descheduled does not count). Otherwise the head is re-checked every 50 us. Posts from interrupt context or host threads only wait for the transfer time. `SMS_PORT_ARQ_UNORDERED=1` (debug) delivers on time alone.
With the ordering guaranteed, the per-request cost is the fixed 20 us by default (`SMS_PORT_ARQ_BUS_TIME=1` restores w5's 20 us + 12 ns/byte): the game only ever waits on these completions, nothing reads the DMA duration.
Verification (same exe, extracted folder, `SMS_PORT_PROFILE`, scripted walking after the load, 4-6 instances in parallel on a machine shared with other agents): 30 alternating Bianco (2,0) / plaza (1,2) loads, 0 hangs; with the A/B and stress runs below, about 85 loads with the fast timing and no ARQ hang. The load lines show 2-21 "held" re-checks per load (completions that were due before their poster had moved on). 24 runs with `SMS_PORT_ARQ_UNORDERED=1` did not hang either, so the race stays rare even unguarded.
Two early runs (fresh pipeline caches, 3 instances right after the smoke suite) stopped presenting ~0.5 s after Mario got control and never recovered within the 35 s run. Not reproduced in ~60 later runs, cold caches included. The cold-cache runs with `SMS_PORT_PROFILE=3` showed 4-12 s stalls, each one with the main thread inside `aurora_begin_frame` → `wait_for_gpu_progress` (GPU/pipeline back-pressure, not a game-thread spin; an ARQ spin would leave main in `waitForCpu`), so these were most likely GPU stalls longer than the rest of the run.

## Re-measured after the restart (w14 exe as merged; before = `SMS_PORT_W14_LEGACY=1 SMS_PORT_PRESENT_EARLY=1 SMS_PORT_ARQ_BUS_TIME=1 SMS_PORT_DVD_READAHEAD=0`)
Same binary, variants interleaved, 2 runs per cell, 60 s each, 4 instances at a time, warm per-slot pipeline caches, `SMS_PORT_FPS` = w9's frame rate; pacing = medians over the 300-presented-frame windows after the load (scripted walking/camera):

| Scene, rate | Load (ms) | setupObjects (ms) | Present interval sd (ms) | p1 / p99 / max (ms) | Process CPU (% of a core) | `preempt` ms/frame |
|---|---|---|---|---|---|---|
| Bianco 2,0 @30 | 266 -> 99 | 193 -> 56 | 1.72 -> 0.76 | 30.6/34.9/50.2 -> 31.7/35.4/38.6 | 57 -> 47 | 0.86 -> 0.67 |
| Plaza 1,2 @30 | 792 -> 650 | 210 -> 66 | 0.60 -> 0.60 | 32.1/34.2/37.5 -> 31.7/34.6/37.9 | 47 -> 37 | 0.81 -> 0.63 |
| Bianco 2,0 @60 | 275 -> 99 | 197 -> 55 | 3.58 -> 3.88 | 8.7/24.3/28.6 -> 8.7/24.8/28.2 | 79 -> 68 | 0.77 -> 0.59 |
| Plaza 1,2 @60 | 783 -> 650 | 207 -> 70 | 3.01 -> 3.50 | 10.2/22.7/23.8 -> 9.9/23.5/24.9 | 70 -> 57 | 0.87 -> 0.72 |

59.93-59.94 fps at 60 and 29.97 at 30 in every cell. Plaza's load includes the original 0.5 s `thpInit` wait; 1,2 is used because 1,0/1,1 on a fresh save start with a movie director (the load line then measures the movie) and 1,3 crashes in `TGuide::load` (`J2DTextBox::setFont` on a null font, warp-only, not looked into).
Further load runs (30 s each, 5 per cell): Bianco 283 (before) / 266 (bus time only) / 99-115 (now) ms; the ARQ timing is nearly the whole gain, the read-ahead and scheduler changes the rest.
**For w9 (frame pacing at 60):** the interval sd of ~3.5 ms at 60 is w9's pacing, not w14's (the same with the w14 changes off): a display frame is started `port_interp_lead()` (= recent maximum cost + 1.2 ms, decaying 3%/frame) before its due time and presented as soon as it is drawn, so it lands early by the lead minus its cost (logged pairs: logic -> display ~15 ms, display -> logic ~18.5 ms; p1 8.7 ms after a cost spike). Holding the finished display frame until its due time (`aurora_end_frame` at `target`, waiting with the CPU released to priority <= 9 threads) should bring it close to the 30 fps sd.

## Audio starved by EFB reads (2026-10-12)
A friend's laptop crackled (stutter, static, drop-outs) in the Delfino Plaza, which every multiplayer lobby uses, but not at the airstrip.
Cause: the draw-sync callbacks (Mario's occlusion probe, the sun, the pollution counters) read the EFB every frame, and on Aurora each read resolves the frame drawn so far and waits for GPU results: tens of ms per frame in a heavy stage on a slow PC.
The main thread held the game CPU meanwhile, and JAudio's audio thread needs the CPU once per DSP sub-frame (`syncDSP` -> `audioproc` message 1 -> `DSPBuf::updateDSP`), so a DMA buffer's sub-frames were not done in time and `updateDac` left the old buffer playing.
Fix: `GXPeekARGB`, `GXPeekZ`, `GXReadPixMetric` and `detEndFrame`'s wait run under `port_preemptible_begin` (`port_efb_host_begin`, port_efb.cpp), like the present: the audio thread may run during them. `SMS_PORT_EFB_HOLD=1` restores the old behaviour.
Measured (plaza, hidden instance): 230-250 of every 600 DMA buffers replayed before, 0 after; airstrip 0 both ways; a 2-player session 0.
Tools: `SMS_PORT_AUDIO_LOG=1` now also prints how many of the last 600 DMA buffers were replayed (sdk_audio.cpp `aiTick`); `SMS_PORT_AUDIO_MUTE=1` opens the device at zero gain (queue statistics without sound); `SMS_PORT_PROFILE=2` also tallies the audio thread's waits for the CPU by the call chain that ended them.

## Audio starved by EFB reads (2026-10-12)
A friend's laptop crackled (stutter, static, drop-outs) in the Delfino Plaza, which every multiplayer lobby uses, but not at the airstrip.
Cause: the draw-sync callbacks (Mario's occlusion probe, the sun, the pollution counters) read the EFB every frame, and on Aurora each read resolves the frame drawn so far and waits for GPU results: tens of ms per frame in a heavy stage on a slow PC.
The main thread held the game CPU meanwhile, and JAudio's audio thread needs the CPU once per DSP sub-frame (`syncDSP` -> `audioproc` message 1 -> `DSPBuf::updateDSP`), so a DMA buffer's sub-frames were not done in time and `updateDac` left the old buffer playing.
Fix: `GXPeekARGB`, `GXPeekZ`, `GXReadPixMetric` and `detEndFrame`'s wait run under `port_preemptible_begin` (`port_efb_host_begin`, port_efb.cpp), like the present: the audio thread may run during them. `SMS_PORT_EFB_HOLD=1` restores the old behaviour.
Measured (plaza, hidden instance): 230-250 of every 600 DMA buffers replayed before, 0 after; airstrip 0 both ways; a 2-player session 0.
Tools: `SMS_PORT_AUDIO_LOG=1` now also prints how many of the last 600 DMA buffers were replayed (sdk_audio.cpp `aiTick`); `SMS_PORT_AUDIO_MUTE=1` opens the device at zero gain (queue statistics without sound); `SMS_PORT_PROFILE=2` also tallies the audio thread's waits for the CPU by the call chain that ended them.

# Performance (w12, 2026-10-14)

Goal: faster and smoother on weak PCs (2-4 cores, integrated GPUs) without changing what the game does or shows.
Measured on the i9-12900K + RX 9070 XT with hidden (offscreen) instances; "2 cores" = process affinity mask 3 set right after
start; "iGPU" = the Intel UHD 770 of the same machine (`AURORA_LOW_POWER=1`, below). Scripted movement in every run
(`SMS_PORT_MOVE_SCRIPT`, 900 frames from control start, `SMS_PORT_SKIP_STAGE_INTRO=1`), `SMS_PORT_PROFILE=1`, audio device open but
muted (`SMS_PORT_AUDIO=1 SMS_PORT_AUDIO_MUTE=1 SMS_PORT_AUDIO_LOG=1`) so the DMA-replay counter is live.

## Sampling profiler: `SMS_PORT_PROFILE_SAMPLE=<hz>` (new `port/src/port_sampler.cpp`)
With `SMS_PORT_PROFILE` on, a host thread suspends the game's main thread and Aurora's "FIFO processor" and "render worker"
threads <hz> times a second (default 1000), unwinds them with `RtlVirtualUnwind` (no dbghelp while a thread is stopped), resumes
them, and every `SMS_PORT_PROFILE_SAMPLE_S` seconds (default 10) prints per thread the functions with the most samples where they
were running ("self") and the functions most often anywhere on the stack ("incl"), symbolized through the PDB. Off unless set.
It is a debugging tool: suspending threads distorts timing, badly so on a 2-core affinity run (the sampler thread itself gets
descheduled while it holds a game thread suspended; stalls of seconds appeared only with it on). Never use it for pacing numbers.

## P1. `getenv` per draw in Aurora's FIFO processor (the plaza at 15 fps)
Delfino Plaza (1,2), the multiplayer lobby stage, ran at 15 fps even on this machine: `gxgen` 40 ms per frame, process CPU 95-187%
of a core. Nothing in the known knobs changed it (copy readbacks off, `SMS_PORT_EFB_HOLD=1`, no interpolation).
The sampler showed the main thread 60% of its time waiting in `aurora::gx::fifo::drain` (from `GXPeekARGB` -> `resolve_pass`, the
occlusion probe's draw-sync callback, which has to wait for the FIFO processor to reach that point) and the FIFO processor 78% busy
in `draw_prim`, of which ~70% inside the UCRT (`getenv`: critical section plus a linear scan of the whole environment block).
Cause: the "SMS port: diagnostics" `AURORA_BIGDRAWS` checks in `port_deps/aurora/lib/gx/command_processor.cpp` (`draw_prim`) called
`getenv` twice per draw, and `lib/gfx/recording.cpp` once per frame. They are in the shared Aurora tree but in no patch file
(not in `port/patches/orig/` either). Fixed by reading the variable once (`static const bool`); recorded as
`port/patches/aurora_getenv_once.patch` (apply after the others). `aurora_gx.lib` rebuilt in `port_deps/aurora/build`.

Same binary before/after is not possible for this one (the fix is in Aurora's static library); before = the previous library, same
port sources, same run script, warm pipeline cache, steady-state windows (frames 301-900 after control start):

| Scene (full speed) | fps | `gxgen` ms/frame avg / p99 | `fifo` wait ms/frame | main busy ms/frame | present interval p99 / max | process CPU |
|---|---|---|---|---|---|---|
| Plaza 1,2 before | 15.1-15.7 | 39.6-40.7 / 42.4-47.1 | 10.1-11.6 | 50.5-51.8 | 66.8-67.0 / 67.7-117.5 ms | 92-99% of a core |
| Plaza 1,2 after | 29.97 | 2.5-2.6 / 2.9-3.4 | 0.17-0.19 | 3.6-3.7 | 33.7 / 33.8-34.1 ms | 40-41% |
| Bianco 2,0 after | 29.97 | 3.2 / 3.8-4.0 | 0.17-0.24 | 4.5-4.6 | 33.7-33.9 / 34.3-34.5 ms | 46-48% |
| Ricco 3,0 after | 29.97 | 2.5-2.7 / 3.3 | 0.1-0.5 | 3.7-4.3 | 33.7-34.0 / 33.9-34.4 ms | 39-42% |
| Pinna 5,1 after | 29.97 (one 200 ms hitch) | 2.6 / 3.2 | 0.17-0.25 | 3.4-4.5 | 34.0-34.4 / 34.4 (200) ms | 40-44% |
| Sirena 6,0 after | 29.97 | 1.8 / 2.4-2.6 | 0.6-0.7 | 3.3-3.5 | 33.7-34.0 / 34.1-34.5 ms | 37-39% |

The other stages were not measured before the fix in this round; the `getenv` cost is proportional to the draw count, so they were
hit the same way (w14's 2026-10-08 numbers, 30 fps everywhere, predate the diagnostics). Audio: 0 DMA buffers replayed per 600 in
every steady-state window, before and after (the EFB reads were already preemptible, "Audio starved by EFB reads" above).
Loads unchanged: Bianco/Ricco/Pinna/Sirena 116 ms, plaza 583-667 ms (its 0.5 s `thpInit` wait).

What is left on the main thread at full speed (sampler, plaza/Bianco/Pinna): ~90% asleep in `VIWaitForRetrace`; of the ~10% busy,
5-6% is the FIFO drain inside `GXPeekARGB`'s `resolve_pass` (the main thread waits for the FIFO processor to catch up at the
draw-sync point, ~2 ms per frame), the rest is the game's own `TPerformList::perform`. The FIFO processor is ~10% busy, the render
worker ~5%.

## P2. 2 cores and the integrated GPU
Steady state holds 30 fps in every measured stage (plaza, Bianco, Sirena) with 2 cores: main busy 5-9 ms, process CPU 27-38% of a
core. What changes is the scheduler's host-thread hand-off latency: `handoff` (dispatch -> the woken game thread runs) 0.2-0.4 ms per
frame with 24 cores -> 4-8 ms per frame with 2 (30-37 hand-offs per frame), `preempt` 0.08 -> 1-1.5 ms, and the hitches: single
present intervals of 100-2500 ms, each with a `handoff`/`preempt`/`irqlate` maximum of the same size (a game host thread readied but
not scheduled while Aurora's workers and the driver's threads hold both cores), and 50-170 of 600 DMA buffers replayed in the windows
that contain them. `SMS_PORT_PROFILE=3` dumps during those hitches: the main thread asleep in `waitForCpu` (OSSleepThread from
`VIWaitForRetrace`, or `checkPreempt` from `OSRestoreInterrupts`), once inside Aurora's `wait_for_gpu_progress` (GPU back-pressure).

**Fix: game host threads above normal priority** (`sdk_os_thread.cpp` `gameHostPriority()`, `threadEntry` and the main thread at
init): every host thread that carries an OSThread runs at `THREAD_PRIORITY_ABOVE_NORMAL`. Only one of them executes game code at a
time ("one CPU"), so this takes at most one core from Aurora's FIFO/render workers, Dawn's pipeline thread and the driver's threads
(all normal priority); a hand-off is then taken by Windows at once instead of after the quantum of whichever worker holds the other
core. The interrupt thread stays TIME_CRITICAL. `SMS_PORT_GAME_PRIO=0` = normal (old behaviour), `=2` = HIGHEST.
Same binary, 2-core affinity, no sampler, variants alternated, steady-state windows (frames 301-900):

| Scene, 2 cores | `handoff` ms/frame avg (p99) | `preempt` ms/frame avg (p99) | main busy ms/frame | present p99 / max (ms) | DMA replays per 600 |
|---|---|---|---|---|---|
| Plaza 1,2 normal | 3.3-3.6 (5.8-6.0) | 0.51 (4.1-4.7) | 5.9-6.0 | 33.9-35.2 / 35.4-38.1 | 0, 0 |
| Plaza 1,2 above normal | 1.0 (4.1-4.4) | 0.14-0.18 (1.6-2.1) | 5.4-5.9 | 34.1-35.5 / 35.2-41.8 | 0, 0 |
| Bianco 2,0 normal | 4.5-5.2 (9.0-73) | 0.7-1.1 (4.5-18.7) | 8.0-11.9 | 34.4-108.6 / 35.6-163 | 0, 0 |
| Bianco 2,0 above normal | 1.0 (4.0) | 0.19-0.20 (2.5-2.6) | 6.8-7.4 | 34.7-35.3 / 36.3 | 0, 0 |
| Sirena 6,0 normal | 3.4-3.9 (6.9-7.4) | 0.6-0.7 (4.4-4.5) | 6.0-6.1 | 34.5-35.0 / 36.1-36.7 | 0, 0 |
| Sirena 6,0 above normal | 1.1-1.3 (3.4) | 0.14-0.16 (1.8) | 4.9-5.3 | 34.3-34.7 / 34.8-35.5 | 0, 0 |

`THREAD_PRIORITY_HIGHEST` (`=2`) is worse: plaza 617 and 884 ms stalls with `irqlate` 386-529 ms and 24-37 DMA replays per 600, Sirena
a 435 ms stall; it starves the host threads the game depends on (the audio device callback, Dawn) and is only kept as a knob.
The load windows (frames 1-300, 5 plaza runs each): maximum interval 67-183 ms above normal (one run 1133 ms: a 1.1 s `fifo` drain
wait during the stage's first frame, `handoff`/`irqlate` 1116/677 ms, not reproduced in 4 more runs) vs 117-285 ms normal; load
600-767 ms in both. Full-speed (24-core) numbers are unchanged by the priority (handoff 0.2-0.4 ms/frame either way).


iGPU (UHD 770, plaza, full cores): 30 fps steady with main busy 3.7-3.9 ms, but process CPU 137-143% of a core (the Intel driver's
threads) and a 6.9 s `beginframe` stall on the first frames (the pipeline cache's 899 pipelines compiled for a new driver: the Dawn
blob cache is per driver and cannot be shipped) plus one 76 ms `beginframe` later. The second run on the same cache dir would not pay
it again; a player's first start on a laptop does.

## Tools added (all off by default)
- `SMS_PORT_PROFILE_SAMPLE=<hz>`, `SMS_PORT_PROFILE_SAMPLE_S=<s>`: sampling profiler (above).
- `SMS_PORT_MOVE_SAVE_REPLAY=1` (`port_movetest.cpp`): with `SMS_PORT_MOVE_EXIT=1`, saves the input recording next to the exe before
  quitting, so a scripted run can be replayed under another build (`SMS_PORT_REPLAY=<file>`) and compared frame by frame.
- `AURORA_LOW_POWER=1` (`port_deps/aurora/lib/webgpu/gpu.cpp`, `RequestAdapterOptions.powerPreference`): asks for the low-power
  adapter, i.e. the integrated GPU of a machine that has both. Default unchanged (HighPerformance).
- `SMS_PORT_GAME_PRIO=0/1/2` (`sdk_os_thread.cpp`): host priority of the game's threads (see P2).

## Correctness
- Every run ends the movement script at the same position per stage (plaza 1446.7 275.0 7314.5 / -37.5, Bianco -5655.3 3520.5
  15969.9 / 26.5, Ricco -9506.2 -79.7 4392.9 / -156.0, Pinna -2414.4 140.4 7888.0 / -109.1, Sirena -3565.1 375.0 2803.3 / 53.2),
  before and after each change, at full speed, with 2 cores and on the iGPU. One 2-core plaza run (with the sampler on, a 1.9 s
  stall during the load) ended elsewhere: Mario was bumped by an NPC at frame 100-110 (NPC paths depend on the boot-time `srand`
  seed, which differs per run); not a port difference.
- Screenshots at the same frame differ between any two live runs of the same binary (the random seed, 1-3 frame GPU latency of the
  occlusion peek), so pixel comparisons need `SMS_PORT_REPLAY`; `SMS_PORT_MOVE_SAVE_REPLAY=1` was added for that. Done for the plaza (full speed) and Bianco (2 cores): a run recorded with the final build, replayed with the final build and with the build before the priority change (`SMS_PORT_REPLAY`, 0 of 8 checkpoints differ, same end position): EFB screenshots at frames 300, 600 and 890 from control start byte-identical across all five replays of each stage except one replay of the *old* build in Bianco (11/17 pixels off by 1 at frames 300/600 and a 4146-pixel patch at 890, rows 558-687: a one-off GPU-side nondeterminism; two further replays of the old build matched the new one exactly).
- GameCube build: `python -m ninja` -> `build/GMSE01/mario.dol: OK` (no game source touched; `port_movetest.cpp`,
  `port_profile.cpp`, `sdk_os_thread.cpp` are port files).

## Tried, no gain
- `SMS_PORT_COPY_READBACK=0`, `SMS_PORT_EFB_HOLD=1` in the plaza: `gxgen` unchanged (the cost was P1).
- `THREAD_PRIORITY_HIGHEST` for the game threads (above).
- The sampling profiler itself at 2 cores: a confounder (stalls of 1-2.6 s only with it on); every pacing number here is without it.

## Left
- The main thread's wait for the FIFO processor at every draw-sync peek (`resolve_pass` -> `fifo::drain`, ~2 ms per frame, 5-6% of
  the main thread's time): could be removed by issuing the snapshot request as a FIFO sub-command processed on the FIFO thread (the
  `GX_AURORA_REQUEST_DEPTH_SNAPSHOT` pattern), with `fifo::drain` made a no-op on the FIFO thread itself. Touches Aurora, port_efb.cpp
  and port_aurora_sdk.cpp thread assumptions and the deterministic replay path; not worth it until a CPU-bound case shows up (the
  main thread is 90% idle on 2 cores).
- First start on a new GPU/driver compiles ~900 pipelines (6.9 s stall on the UHD 770 here); the seed cannot carry driver blobs.
- The 2-core hitches (P2) are a scheduler/host-thread matter in `sdk_os_thread.cpp` (w14's area): with the priority change the steady state is clean, but single 100-300 ms intervals remain in the load windows (and once 1.1 s), each with a matching `handoff`/`irqlate` maximum: a readied game host thread or the interrupt thread not scheduled while Aurora's FIFO processor, the pipeline thread and the driver fill both cores. `SMS_PORT_PROFILE=3` dumps show the main thread asleep in `waitForCpu` while the thread recorded as running is itself in `waitForCpu` (a hand-off in flight), i.e. host scheduling, not game work.
# Level sweep A (w11): Bianco Hills, Ricco Harbor, Gelato Beach, Pinna Park

How the levels were run: `SMS_PORT_WARP=<area>,<scenario>` (w7's warp), scripted input (below), and an 85 s run per scenario, with a crash log and Mario's position every 10 s.
Area numbers come from `/data/stageArc.bin` (the "ScenarioArchiveNamesInStage" table `TApplication::mountStageArchive` indexes):
- 2 = Bianco Hills (scenario 0-7 = bianco0, 1, 2, 3, 4, 7, 6, 5), 55 = biancoBoss
- 3 = Ricco Harbor (ricco0-7), 59 = ricco8
- 4 = Gelato Beach (mamma0-7)
- 5 = Pinna Park beach (pinnaBeach0, 1, 3, 2, 4, 4, 4, 4), 13 = Pinna Park (pinnaParco0-7; 8-10 = pinnaDemo0-2), 58 = pinnaBoss0/1 (jet coaster)
- secret courses are 28-39 (bia_ex*, rico_ex*, mam_ex*, pinna_ex*): w12's area.

New debug knobs (all off by default):
- `SMS_PORT_STICKSCRIPT` (`port/src/port_sweep_input.cpp`, hooked with one line in `port_main.cpp`'s `port_PADRead`):
  `<from>-<to>:<x>,<y>[,<buttons>]` holds the main stick and buttons on pad 0 for those frames;
  `auto:<from>[-<to>]` walks Mario in a slowly turning circle, sprays (R) every other 2 s and jumps every 4 s.
  (`SMS_PORT_PADSCRIPT` has since gained `S<degrees>` stick tokens; the `auto` walk could be folded into it.)
- `SMS_PORT_LOGMARIO=1` (`MarDirectorDirect.cpp`): Mario's position, status and HP every 10 s of gameplay.
- `TMarDirector::changeState` logs every director state change (`[port] TMarDirector state 1 -> 3`), like `TApplication::proc` does.

## Found in the sweep and fixed by someone else in parallel (nothing of w11's left to merge)
- Fatal WGSL "cannot index into expression of type 'f32'" on every polluted stage (alpha compare `GX_TEV_COMP_R8_GT` from `PollutionCount.cpp`): `aurora_shader.patch` 1/3 (w6). w11's patch was the same.
- Fatal "unhandled tcg src 21" on every Gelato Beach episode (an indirect stage reading a texcoord past `numTexGens`): `aurora_shader.patch` 2/3 (w6). w11 used texcoord 0 (Dolphin's quirk); w6 uses (0, 0).
- Fatal "unsupported logic op 10" in Pinna Park episodes 3/4: `aurora_logic_ops.patch` in `port_deps`.
- Audio thread crashes in `TChannelMgr::receiveAllChannels` / `TNoteMgr::getChannel(28)` (note-on bitfield order in `JASSeqParser.cpp`): w7.
- biancoBoss / ricco8 crash in `TPollutionLayer::cleaned` (`pos[10]` written past the array; on the GameCube it lands in an unused destructor-chain record): w7 (`pos[11]`).
- Gelato Beach episode 2 crash in `TDemoBossHanachanManager::clipEnemies` (empty `LINK_ALL_SOURCE` constructors): w10 S8.
- Gelato Beach episode 5 (Il Piantissimo) crash in `JKRArchive::findFsResource` for `"/../map/pad/Setting.prm"` (root `..` = index 0xFFFFFFFF): w10 S9. w11 had checked on `params.szs` that the GameCube's wrapped lookup finds nothing.
- Gelato Beach episode 5: a 32768x32768 texture at `0xC0928E60` (TEX1 header with 0x5A in the pad byte on disc): w10 (`port_swap_tex1_timgs_force`).
- Pinna Park episodes 1 and 5 crash in `TMarioInputReplay::play` (Shadow Mario `.pad` replays): w10 (`port_swap_pad`).
- Pinna Park episode 7 -> coaster: Aurora "draw vertex data overrun" from the stale tail of the `J3DShape` VCD/VAT list: w6 (NOP padding).

## A1. Jet coaster camera (pinnaBoss 0/1, also entered from Pinna Park episode 7) becomes NaN on its first frame: crash in `matan`, then in `TMapCollisionData::checkGround`
- **Symptom:** `CPolarSubCamera::calcExternalData_` -> `CLBCrossToPolar` -> `matan` reads `atntable[0x80000000]`.
  With `matan` fixed alone, the next NaN consumer (`TMapObjWave::getHeight` -> `checkGround` grid index) crashed.
- **Cause:** an uninitialised read that differs between the platforms.
  `ctrlJetCoasterCamera_` (L-button mode, the default) builds the camera from the Torocco's joint-2 matrix (`getToroccoMtx_`) before the Torocco model has had its first `calc()`.
  `J3DModel::entryModelData` allocates `mNodeMatrices` uninitialised. On the GameCube the heap still holds the previous stage's data, but the PC arena starts out zeroed.
  The zero up axis is normalised to NaN (as `frsqrte(0)` would on hardware), and the camera chases NaN for the rest of the stage.
- **Fix:**
  - `J3DModel.cpp`, under `SMS_PORT`: `mNodeMatrices` start as identity matrices; the first `calc()` overwrites them as before.
  - `MathUtil.cpp` `GetAtanTable`, under `SMS_PORT`: NaN or out-of-range ratios follow the GameCube's `fctiwz` saturation and 32-bit address arithmetic (index 0x80000000 * 2 wraps to `atntable[0]`), instead of reading 4 GB away.
- Verified: `58,0`, `58,1` and `13,6` (into the coaster) run to the end of the test with Mario moving along the track.

## Notes from the sweep
- `13,8`-`13,10` (pinnaDemo0-2) have no archive on the disc (`/data/scene` has no `pinnaDemo*.szs`), so `mountStageArchive` returns null; they are not reachable in the game. Not a port bug.
- With several `sms_port` instances on one GPU, two runs died inside the AMD D3D12 driver while compiling a pipeline (`E_OUTOFMEMORY`, and a crash in the driver's shader compiler); a rerun passed both times. (`aurora_pipeline_wait.patch` now retries `E_OUTOFMEMORY`.)
- With main's rendering fixes (synced 16:3x), gameplay screenshots of these levels show the stage, Mario and the HUD (e.g. Bianco 1 and the pinnaBoss coaster at frame 2400). Before that sync they were black.

Status after the main sync (one 85 s run each, scripted walk/spray/jump, no crash): Bianco 2,0-2,7 and 55,0; Ricco 3,0-3,7 and 59,0; Gelato 4,0-4,7; Pinna beach 5,0-5,7; Pinna Park 13,0-13,7; pinnaBoss 58,0/58,1.
(Full sweep on the build before the main sync; a 10-scenario smoke set on the final one.)


# Level sweep B (w12): Sirena Beach, Hotel Delfino, casino, Noki Bay, Pianta Village
Areas (from `data/stageArc.bin`, the table order is the area number): 6 Sirena (`sirena0-7`), 7 Hotel Delfino (`delfino0-4`), 14 casino (`casino0-1`),
56 `delfinoBoss`, 9 Noki Bay (`mare0-7`), 16 `mareUndersea`, 57 `mareBoss`, 8 Pianta Village (`monte*`, scenario n loads the archive the table lists, not `monte<n>`).
The bottle level `mare_ex0` (area 44, the diving secret) is entered from `--warp 9,2` by the scripted input below; the EX courses belong to w13.

How it was run: `SMS_PORT_WARP=<area>,<scenario>`, Null backend, 75-130 s per scenario (4200-7200 presented frames),
`SMS_PORT_PADSCRIPT` pressing A every 40 frames from frame 240 to 1200 (skips the intro dialogue), B twice, and w11's `SMS_PORT_STICKSCRIPT=auto:<frame>`
(`port_sweep_input.cpp`, walks in a turning circle, sprays, jumps; hooked into `port_PADRead` the same way as in w11).
Mario often loses a life and the game returns to Delfino Plaza (area 1) before the run ends; that is the game's own behaviour.
Real D3D12 shots of Sirena 6,1, Hotel 7,0, casino 14,0, Noki 9,0 and Pianta 8,0 at frame 1800 all render the level, HUD, Mario and NPCs.

## B1. `JKRArchive::findFsResource` reads at `mDirectories + 0xFFFFFFFF` (Noki Bay episode 5, Shadow Mario)
- **Symptom:** `--warp 9,4` crashes in `setupObjects`: `TEMario::load` 竊・`TEnemyMario::initEnemyValues` 竊・`TSettingParams` 竊・`TParams::load` 竊・`findFsResource`, reading `0x10_81F8003A`.
- **Cause:** 64-bit pointer arithmetic on an index the GameCube wraps.
  `TEnemyMario` loads `"/../map/pad/Setting.prm"` (also `pad2`, `pad3`). The scene archive misses (from `/map/params`, `..` is `/map`, which has no `map`), so `TParams::load` falls back to `params.szs` from its root, and the root's `..` entry has directory index 0xFFFFFFFF.
  On the GameCube `mDirectories + 0xFFFFFFFF` wraps to the 16 bytes before the node table (the info block tail, read as 256 entries from file entry 0); `params.szs` has no `map` among them, so the lookup returns null and Shadow Mario keeps the default settings. On x64 the index is not wrapped.
- **Fix:** `libs/JSystem/src/JKernel/JKRArchivePri.cpp`, under `SMS_PORT`: `findFsResource` returns null for a directory index past `num_nodes`, which is the GameCube result for the only path that gets there. GameCube DOL unchanged (shasum OK).

## Crashes in these levels fixed by other workers (picked up from main / their trees while sweeping)
- Every stage start: audio-thread crash in `TChannelMgr::getListHead` (list head `0x0003000000000000`). Found with a watchpoint: `TTrack::noteOn` stored `TNoteMgr::unk20[index]` with a voice index read from the wrong bits of the note-on byte (bitfield order). Fixed the same way by w5/w7 (`JASSeqParser.cpp`, in main).
- Hotel Delfino episode 4 (`--warp 7,3`): `TMarioInputReplay::play` reads `0xE0A56AC0`, the `.pad` Shadow Mario replay is big-endian. w10's `port_swap_pad` (A5).
- Sirena episode 6 (`--warp 6,5`) with spraying: `TPollutionLayer::cleaned` reads through a corrupted `counter_x`, the original's `pos[10]` overrun. w7/w11's `pos[11]` (in main).
- Noki Bay episode 5: Aurora reads a texture past the end of RAM (`0xC09E5160` = an unswapped ResTIMG image offset). Gone with w10's TEX1 header fix (A6).
- Bottle level (area 44, from `--warp 9,2`): Aurora fatal `draw vertex data overrun` in `J3DShape::draw`, the stale tail of `mVcdVatCmd` (w7's note). Fixed by the NOP padding in main's `J3DShape.cpp`.
- Aurora fatals that stopped Sirena 6,0 (`unmapped vtx attr 13`), Noki/Pianta (`unhandled tcg src 21`), the WGSL alpha-compare error, and `unsupported logic op 10` (Mario/Shadow Mario silhouette, `GXSetBlendMode(GX_BM_LOGIC, ..., GX_LO_INV)` with colour and alpha updates off, `MarioDraw.cpp`) are all handled by the shared Aurora as of 16:00.

## Status (final pass, main as of 16:00 plus B1)
No crash in any scenario. Each row: loads, frames presented when the run was stopped (all runs alive), and where the run ended.
| Warp | Level | Result |
|---|---|---|
| 6,0-6,7 | Sirena Beach episodes 1-8 | all load, 4800 frames, alive; 6,5 ends in the plaza (life lost) |
| 7,0-7,4 | Hotel Delfino (5 scenarios, incl. 7,3 Shadow Mario replay) | all load, 4800 frames, alive |
| 14,0 / 14,1 | casino | load, 4800 frames, alive |
| 56,0 | `delfinoBoss` | loads, 4800 frames, alive |
| 9,0-9,7 | Noki Bay episodes 1-8 | all load, 4800 frames, alive; 9,0/9,1/9,3 end in the plaza; 9,2 enters the bottle level (area 44) and keeps running |
| 16,0 | `mareUndersea` | loads, 4800 frames, alive |
| 57,0 | `mareBoss` | loads, 4800 frames, alive (ends in the plaza) |
| 8,0-8,7 | Pianta Village episodes 1-8 | all load, 4800 frames, alive; with the circling stick input Mario loses a life and the run ends in the plaza (with buttons only, all 8 stay in the village for 3000 frames) |

## Open items
- `--warp 60,0` (`coronaBoss`, now w13): Aurora fatal `convert_texture: unknown texture format 22` right after the stage loads (seen before the scope change).
- Most Noki/Pianta runs end in the plaza after Mario loses a life, so later parts of those levels (bosses that need progress, e.g. the Pianta Village chain chomp / Noki episode objectives) were only reached through `mareBoss`/`delfinoBoss` warps, not by playing.


(Note: B1 was not merged; main's JKRArchive::portDirEntry from w10 S9 covers the same case.)

# Constructor audit (w11)

Scope (shared with w13: w11 owns constructors and factories): every class built in the name-ref factories (`TMarNameRefGen::getNameRef*`), the enemy managers' `createEnemyInstance`, every `__ct__` function, and every function that constructs a class whose constructor the GameCube inlines (no `__ct__` symbol).

Method:
- `tools/ctordiff.py <unit> [function]` (new): compares target and our object function by function as a multiset of initialisation events, ignoring registers and scheduling.
  Events are calls, and stores through non-stack bases with offset and constant/relocated value (vtables show up as `@__vt__X`).
  `semdiff.py` drops offset stores as noise, so it cannot see missing field initialisation.
- Factories: per-`strcmp`-block structural diff of `decomp-diff --no-collapse` output (a block whose class has an inlined constructor shows its base-ctor call, vtable and field stores).
- All units were swept: 449 classes in the factories, about 180 inlined-constructor sites in non-100% functions, and all `__ct__` functions present in both objects. Also checked: no game `__ct__` exists in a target object without a copy in ours, apart from `TTelesaSlot` (ours is inlined; same stores, checked by hand).
- Noise left out: nerve singletons (`@instance$N` naming), relocations against anonymous data (`@L` vs a named array), indexed stores (`stwx`, not modelled), and the same field reached from a different base register.

Fixes (all plain decomp corrections; the GameCube DOL check still passes):
1. **Mario's and Yoshi's animation sounds were plain `MAnmSound`.** `TMario::initValues` (MarioInit.s) and `TYoshi::init` (2x, Yoshi.s) construct `MAnmSound` and then store `__vt__14MAnmSoundMario` at +0x94.
   So they are `MAnmSoundMario`, whose `startAnimSound` override plays Mario's surface-dependent animation sounds. Ours built the base class, so the override never ran.
   `MarioInit.cpp`, `Yoshi.cpp`: `new MAnmSoundMario(...)`. `TMario::initValues` is now 100% fuzzy (was 98.8), `TYoshi::init` 99.6.
2. **"HideObj" built a `THideObjBase`.** The factory (MarNameRefGen_MapObj.s 0x31A0) calls `THideObjBase(name)` and then stores the `THideObj` vtable.
   `THideObj` had a declared but undefined constructor, so the source used the base class, which lacks `THideObj::touchPlayer` (touch-to-reveal objects never revealed).
   `MapObjHide.hpp`: inline `THideObj` constructor; factory uses `new THideObj`.
3. **"WatermelonStatic" missing from `getNameRef_MapObj`.** The target has an entry between "WaterMelon" and "ShiningStone" (0x1C28): `new(0x138)`, `TMapObjBase("蝗ｺ螳壹せ繧､繧ｫ")`, `TWatermelonStatic` vtable.
   Ours returned null for it. `MapObjMamma.hpp`: inline constructor; `MarNameRefGen_MapObj.cpp`: the entry.
4. `TMapObjStartDemo`: the inlined constructor (0x624) also zeroes `unk138` (`MapObjTown.hpp`).
5. `TPictureTelesa`: the inlined constructor (0x2828) clears `unk174` (`stb 0`) (`MapObjSirena.hpp`).
6. `TBossEel` from the factory gets the name `"?"` (`@3235`, like the other boss entries), not the header default (`MarNameRefGen_BossEnemy.cpp`).
- Effect on matching: `getNameRef_MapObj` 96.7 -> 97.5%. What remains in the factories is out-of-line versus inlined constructors (our constructors are separate functions where the GameCube inlines them; checked block by block to make the same calls and stores), plus register allocation.

Checked, no change needed:
- `TTelesaSlot`, `TFruitBasket` (`unk150 = 0`), `TSimpleEffect` (`unk44 = 1`), `TGateShadow`, `TBasketReverse`, `TWaterHit/FruitHit/HipDropHideObj`, `TMewManager`, `TDiffusionNameKuriManager`, `TNameKuriLauncher`, `TNpcParts` (24 pointers zeroed), the TBubble/TBossEel/TBossPakkun/TBossGesso/TBossWanwan/TTinKoopa/TKoopaJr part constructors, and the Sleep/DemoBossHanachan stand-ins (w10 S8).
- `TBathWaterManager::loadAfter`: same final vtables. `symbols.txt` names the flat renderer's vtable `__vt__18TBathWaterRenderer` (its entries are the `TBathWaterFlatRenderer` functions), so the names differ but not the behaviour.
- `getNameRef_NPC` and `getNameRef_Map` match; `getNameRef` (MarNameRefGen.cpp) has no structural differences.

Port check: `4,7` (watermelon festival), `2,0` and `1,0` warps run.
`1,0` (Delfino Plaza) never leaves director state 0 within 40 s. Main's own exe (17:05) does the same, so it is not caused by these changes (w7's area).


## R14. SRTG texgens used the vertex colour instead of the lit colour (Mario's toon shading; also an AMD driver crash)
- **Found via:** "Mario as a pink blob undersea". Of the 99 shaders of the Noki undersea stage, 22 had an SRTG texgen whose coordinate came from `in_clr0`, or from `vec4f(0.0)` when the vertex has no colour.
- **GX semantics:** `GX_TG_SRTG` with source `GX_TG_COLOR0/1` takes S/T from the colour channel's output after lighting (Dolphin: `o.colors_N.xy`). SMS uses it for toon shading: the lit colour indexes `H_toon_*` ramp textures (seen in Mario's materials, the diving helmet top and the pollution-ground material of the maps; 22 of the 99 undersea pipelines).
  Aurora used the raw vertex colour, so those ramps were sampled at one fixed texel and the toon shading was flat.
- **Fix:** `aurora_shader.patch` (4/4): the texgen reads the lit colour, and `shader_info` marks the channel as used so its lighting and uniforms exist even if no TEV stage rasterizes it.
  The first version read `out.ccN` back from the vertex output struct. The AMD D3D12 driver then crashed (access violation inside `CreateGraphicsPipelineState`, from Aurora's pipeline thread) in 3 of 4 Noki undersea runs. The lighting now goes to a local `ccN_lit` and is copied to `out.ccN`; 3 clean runs followed.
- **Result:** Mario's cap and clothes now show the toon ramp: a lit top, a darker band and the highlight (`port/build_aurora/shots/mario_srtg_after.png` vs `final/airstrip_shot_1400.png`).


## R15. "Mario as a pink glowing blob" in Noki undersea / Noki boss: original look
- The blob is the diving helmet `/mario/watergun2/body/diver_helm.bmd`, seen from the top-down underwater camera. Material `_mat_diverhelm_helm` has no texture.
  Stage 0: `prev = rasC0 - 0.5`; colour channel 0 is lit with diffuse SIGN and spot attenuation.
  Stage 1: `prev = 2 * (lerp(C2 = (105, 93, 178), C1 = (44, 52, 97), prev) + rasC1)`; channel 1 is the specular light (light 2, `GX_AF_SPEC`).
  The result is a translucent lavender/blue glass with a white specular spot. Mario's red cap seen through it reads as pink.
  Aurora's shader for this material implements exactly that (TEV colours from the material, SIGN diffuse, SPEC attenuation, channel outputs clamped like the hardware).
- The helmet's top cap (`_mat_diverhelm_top`, toon ramp `H_toon_2_4i` via SRTG) was affected by R14 and is fixed.


## R16. Mario's shadow as a sharp silhouette offset from him on river/sea beds, and Pianta's brightness: original look
- **Shadow:** `TMBindShadowManager::perform` sets `mLightDir = normalize(gpLightManager->getLightPos())`, and the shadow volume extrudes Mario's silhouette along that direction (`drawShadowVolume`, `calcVtx`).
  The sun is not overhead, so on a floor below the water surface the shadow lands away from Mario; it is a sharp silhouette by design. The volume passes (alpha add/subtract, then a colour pass) render as the game sets them.
- **Pianta:** the ground material `_green1` is `texture x vertex colour` with lighting off (shader: `mix(0, tex, rast0)`; `rast0 = in_clr0`).
  Its texture `S_green_mon1_mip` (CMPR 256x256) decodes to an average of RGB (0, 133, 0): pure green with darker patches.
  The map's vertex colours average (118, 132, 209), a blue night tint, and are fetched byte-exact. So the saturated green with zero red/blue is in the data.
  The night stages that are dark (Hotel Delfino, Sirena dusk) are dark through the same path.
- **Indirect texturing (Aurora vs GX):** the paths SMS uses were read in the generated shaders. They are `ITF_8` with STU bias -128, static matrices `ITM_0..2` (s.10 elements, scale exponent), wrap off, texcoord scale applied before the indirect scale, and the indirect texture read as `.abg` (as Dolphin does).
  The texcoord-beyond-`numTexGens` case is R3. No discrepancy was found. The undersea caustics were not isolated in a capture.






## W7-G. Gameplay driving: scripted input reference and airstrip boss status
- **PADSCRIPT entry forms** (`port_main.cpp`):
  - `<f>:` holds the input for 10 frames.
  - `<a>-<b>:` holds it over a range.
  - `<a>-<b>/<p>:` taps it for 5 frames every `p` frames.
  - `+<f>`: counted from when the current stage gave Mario control.
  - `+<n>@<f>`: the n-th stage of the run (0 = first). This clock keeps counting through the movies after it.
  - `+d<f>`: counted from the end of the latest demo or talk (director mode back to 0), e.g. a boss's appear demo.
- **Tokens:** buttons or a hex mask; `S<deg>` (main stick, 0 up, 90 right); `G<x>;<z>` (autopilot to world x,z; `Application.cpp`).
- **`SMS_PORT_TRACE=1`:** state changes, Mario's position, action, water, stick and speed every 120 frames, and a heartbeat every 600 frames.
- **`--warp 0,1`** loads the post-FLUDD airstrip directly (control at about f207), the fastest way into the Piranha fight.
- **FLUDD aiming (from the source):**
  - While R is fully held and Mario stands still, he is in `MARIO_STATUS_SQUAT` (0x0C008220).
  - There the pitch follows `unk36E + stickY * mRButtonMult`, clamped and chased, so stick Y tilts the stream.
  - The `G` autopilot holds stick Y near +1, which pins the aim low.
  - The nozzle only emits while R's ramp (`unk372`) catches up, so a held stream dies after about 450 frames. Pump R: 60 frames on, 2 off.
- **Airstrip Piranha, real rendering:**
  - Walk to the west ledge with `+0@100-300:G-5000;1250,+0@300-900:G-2750;1480`.
  - Pump R; it wakes at about +1130 and plays its appear demo.
  - One real-input hit (HP 3 to 2, AwakeDamage, animation 0xC) came from pumping R from the ledge with 16 four-frame `S0` pulses spread over about 3500 frames.
    That combination did not hit again in 3 more runs: the mouth timing has to match the stream.
  - Wait2 lasts about 600-1000 frames before Dive, and only the first appearance plays a demo.
  - The 10000-unit tank lasts about 6500 frames of spraying.
  - Still open: the remaining 2 hits (needs a refill route or spraying only while the head is vulnerable), the shine get, the arrest movie and the transition to the plaza.
- **Seen once and left alone (w6):** an access violation inside the AMD D3D12 driver during pipeline creation (no game frames), while many GPU instances from other agents were running.
- **Headless `build_port.py` link (`gPortCallbackR31`):** not a type mismatch.
  - Within each build the definition and the reference mangle the same way (`unsigned long` in headless, `unsigned int` in Aurora).
  - The failure came from a stale object or census list, and a fresh census plus rebuild links.


# Decomp corrections (w13)

Plain source fixes (no `SMS_PORT`) in NonMatching units where the source computed something other than the GameCube asm. They fix the PC port and the all-source GameCube DOL alike; `dtk shasum` stays OK.
Found with `tools/semdiff.py`, an aligned objdiff scan (instruction pairs whose difference is an immediate, a field offset on the same base register, a data value, a `cror` condition or a call only on one side), and w11's `port/ctordiff_candidates.md`. Field offsets were resolved with MWCC itself (`offsetof` probes compiled with the project flags). Each fix was rebuilt and re-diffed: the differing instruction pair is gone (match % in the entries). Original-game behaviour was left alone.
Checked and found to be codegen noise (no change): `setStatusToJumping` trample switch, `doRunning`, `setCheckData`, `calcBathtubData`, `throwMario`, `makeKillerVelocity`, `TKoopa` nerve statics, `makeMtxRotByAxis`, `calcTowerCenterPos_`, `TMareEventDepressWall::initCommon`, `execCameraModeChangeProc_` (inlining differences only).
Open: `TEnemyMario::setStickToAngle` multiplies by an sdata2 constant 0.0f in every inlined copy (we have 64.0f, and a literal 0.0f is folded by MWCC, so the real expression is unknown; TODO in source). `TMario::checkController` stores to nozzle+0x714 (left as is, see the existing TODO). `BathWaterManager::perform` still differs in its constants (1.0 vs 0.9/10); not resolved.

- Player/MarioRun `TMario::catching`: jump during a catch compared mForwardVel with mDeParams.mClashSpeed (+0x8E8); asm reads +0xCD0 = mJumpParams.mRotBroadEnableV (offset probed with MWCC). Now 99.9 (frame only).
- Player/MarioRun `TMario::surfing`: wall clash on the shell missed the system SE MSD_SE_SY_DAMAGE (0x4823, gateCheck + startSoundSystemSE in asm). Now 100%.
- Player/MarioRun `TMario::running` (2 sites): brake condition was normal.y <= cos 80 deg; asm `cror eq, gt, eq` = >=.
- Player/MarioSpecial `TMario::wireSWait`: used mWireBounceVelPrev (+0x448) where asm uses mWireBounceVel (+0x444) (read and the 5.0 store), and input mask 0x10000 where asm tests 0x8000.
- Player/MarioSpecial `TMario::barWait`: input mask 0x10000 -> 0x8000 (asm rlwinm bit 16).
- Player/MarioWait `TMario::waitingCommonEvents`: checkFlag(0x200000 IS_PERFORMING) -> 0x4000 (asm bit 17), TAKE_POSE input 0x10000 -> 0x8000.
- Player/MarioWait `TMario::sleeping`: wake-up voices used startSoundActor; asm calls startVoice. Now 100%.
- Player/MarioSpecial `TMario::hanging`: wall check before a ledge shuffle used newPos.y; asm uses 10.0f + newPos.y (same as record2).
- Player/MarioSpecial `TMario::fenceMove`: sideways fence movement used the wall normal (x, z) itself; asm uses it turned 90 deg (-z, x) / (z, -x), i.e. along the fence.
- Player/Yoshi `TYoshi::movement` (2 sites): the ground under Yoshi/the egg was one checkGround; asm then re-checks up to 5 times from groundY - 1 while the floor isMarioThrough() and not water (see-through floors). Also the B-button tongue in `movement` (an emitTongue() copy) must not set unkDC = 3: the asm of movement has no store there, unlike doSearch's copy, so movement now has the body without it (match 93.5 -> 90.4% from inlining isMarioThrough/isWaterSurface; logic now equal).
- System/MarDirectorDirect: `THPPlayerPlay()` for the plaza (stage 1) was in `currentStateFinalize` STATE_UNK0; the asm has it at the end of `nextStateInitialize` STATE_UNK2. `nextStateInitialize` STATE_CARD_SAVE also missed `if (3 <= unk261 < 5)` MSD_SE_SY_COLLECT_DELIGHT. currentStateFinalize now 100%, nextStateInitialize 95.2 -> 99.4%.
- Map/MapEventDolpic `TDolpicEventRiccoMammaGate::watch`: after SMS_MarioWarpRequest the asm plays MSD_SE_SY_CLEAR_SIGN_BIG (0x484D) through gateCheck; missing. 92.3 -> 99.8%.
- Enemy/walkerEnemy `TWalkerEnemy::moveObject`: the jump-start added 5.0f to mRotation.y (+0x30); asm adds it to mPosition.y (+0x14).
- MoveBG/MapObjMamma `TShiningStone::endDemo` (inlined into `TLeanMirror::control`): after each of the first two lights the asm shows balloon 0x32 (unk74 == 1) or 0x33; missing. LeanMirror::control 90.3 -> 99.9%.
- MoveBG/MapObjMamma `startCameraShakeSE` (demo camera callback of the Hanachan fall): asm is `li r3, 0; blr`; the source played MSD_SE_OBJ_QUAKE. Now 100%.
- MSound/MSound `MSound::talkModeIn`: BGM duck volume 0.6 -> 0.48 (US asm; under VERSION_GMSE01, others unchanged).
- GC2D/Talk2D2 `TTalk2D2::makeBoxLine`: line-full threshold 1.1 -> 1.05 (US asm; VERSION_GMSE01, like the neighbouring US offsets).
- Enemy/bossgesso `TNerveBGDie::execute`: death jump calcVelocityToJumpToY height 0.0 -> 50.0 (asm @4708).
- MoveBG/MapObjBall `TMapObjBall::touchWall`: wall bounce used -(-1 + restitution); asm adds +1.0 (reflection v += -(1 + e) (v.n) n). Affects every rolling map object (balls, fruit, watermelons).


## E9. Black band under every shot (rows 448-528)
- **Cause:** the EFB render target is sized in `webgpu::resize_swapchain`, which Aurora's `window.cpp resize_swapchain()` only called when the window size changed. The target was allocated while the boot render mode was 640x480 and was never re-sized when SMS switched to 640x448, so the presented part (448/528 of a target sized for 480) left a black band at the bottom.
- **Fix:** `aurora_efb_size.patch` gains a `lib/window.cpp` hunk: with an unchanged window size it still forwards to `webgpu::resize_swapchain` (a no-op when nothing changed), so the target follows render-mode changes. Every aspect and fullscreen go through the same path (present viewport and resample UVs use `present_content_width/height`). `SMS_PORT_SHOT` crops round to the nearest pixel (`port_aurora_sdk.cpp`): Bianco shots are now 1280x960 at 4:3, with no band (were 1280x896 with the band, then 1280x959).



## E10. Pollution counters: a fully cleaned layer read "never clean"; stage changes
- **Residual:** `TPollutionCounterLayer` subtracts `polygons * 4` from `GXReadPixMetric` (asm: an unconditional `subf`) because the GameCube counter counts 4 pixels per rasterized polygon even when every pixel fails the alpha test. The port counts only passing pixels, so a clean layer (2 polygons) read -8 = 0xFFFFFFF8, which the unsigned "cleaned" checks (`cmplw` against `mCleanedDegree`) treat as dirty forever. `src/Map/PollutionCount.cpp`: the subtraction is skipped under `#ifdef SMS_PORT` (GC build unchanged). A forced full wipe now reads degree 0.
- **Metric state per stage (`port_efb.cpp`):** draw-sync tokens are reused by every stage; a token not read for more than 10 frames starts over (result, zero streak and "seen non-zero" cleared; results read before the restart are dropped). The zero debounce is 2 reads once the counter has read non-zero this stage (goop being cleaned), still 30 for a counter that never did (stage start).
- **Copy readback after a CPU write:** readbacks of copies issued before the game rewrote a destination on the CPU (or before a destination was first seen) are dropped, so an old GPU copy cannot overwrite a fresh CPU wipe (`DestInfo::discardUpTo`).



## E11. Cleaned events and the airstrip
- The airstrip's progression is boss-based, not counter-based: `TAirportEventSink::watch` fires when the gatekeeper (Proto Piranha) is dead (demo camera, `layer(0)->startDecay()`, shine). With the gatekeeper marked dead by a temporary test aid, the demo camera ran, the layer decayed to 0 through the GPU decay pass, the counter read degree 0 and the Piantas cheered (`port/build_aurora/efb_airstrip_cleaned_event.png`). Test aids removed.
- Real spraying lowers the counts as on GC (airstrip row 4548 -> 3708, standing sweeps 4548 -> 2588; plaza 13346 -> 12453). A full scripted clean of the whole airstrip was not done: the goop count also rises (enemies, regrowth) and the scripted Mario dies. No script queries the pollution level in airstrip 0/1 or plaza 1/0.
- The 6306 (memory) vs 4548 (counter) difference on the airstrip is the game's own TEV rule: texels below the threshold decay by a step every frame, so the faint edges of the initial map fade. Not a port bug.



## E12. EFB-copy readback latency
- The CPU readers of pollution maps (`isPolluted`, goop collision, the cleaning effect at >100, `PollutionAction`, `updateDepthMap`) see a copy 1-2 frames later than on GC; none of them depends on a copy made in the same frame (each frame reads what the previous frames drew, and the map only changes through the game's own stamp/decay passes). The Manta and bath-water copies are GPU-only; `initTexImage` writes the CPU maps only at load. No copy needs to be synchronous.
- **Note:** main's `port_main.cpp` (as of 17:36) had lost the `port_efb_end_frame()` call before `aurora_end_frame()`. Without it finished copy readbacks are never stored, so the CPU pollution maps stay at their load-time contents (goop collision, cleaning effects and `isPolluted` ignore spraying). Restored in w8's `port_main.cpp`.



## E13. FLUDD water stream (reported as faint)
- Drawn by `TModelWaterManager::drawRefracAndSpec` over one camera-facing quad per flying particle (`TDLTexQuad`, stretched 4x along the view-space velocity): pass 1 = the screen texture (640x448 -> 320x224 RGB565 copy, made just before) through an indirect warp from `waterref.bti` (IA8, +-9 texels at 0.07), alpha = `waterMask.bti` (I8); pass 2 = water colour 0 at alpha 0x14 x mask; pass 3 = additive `C1 + C0*spec` with alpha = `waterSpec.bti` (I8, a small highlight at the top left).
- Checked one pass at a time on Bianco with temporary overrides: the quads rasterize where expected (solid red), the mask samples correctly (opaque grey shows a white body with dark edges), the warp shifts the background inside the stream (opaque pass 1 vs unwarped), and the highlight lands at the quad's front end. Copy order (pollution, shadow, screen texture, then the refraction) and the indirect matrix/bias/scale maths match the hardware (Dolphin's `>> 3`, s0.10 matrix, 1/128-texel offsets = Aurora's texel offsets). The water uses 1 particle per frame (pow 40, size 50); 3-11 quads are in flight in a standing spray. Turning the depth test off adds little.
- Result: no port-side difference found; the stream renders as the TEV setup describes (subtle refraction + highlights). Not changed. A side-by-side with real hardware or Dolphin is needed to say what "faint" differs in. Screenshots: `port/build_aurora/fludd_stream.png` (crop, standing spray), `fludd_stream_mask.png` (mask pass forced opaque).



## E14. `GXPeekARGB` / `GXReadPixMetric` "unimplemented" messages (w13)
- Main's Aurora build logs no "unimplemented"/"missing" line for them in the plaza or Bianco (`port_efb.cpp` / `port_aurora_sdk.cpp` implement both). The stubs in `stubs_auto.cpp` belong to the headless build. Most likely a stale `port/build_aurora/gen/stubs_aurora.cpp` in that worker: delete it and relink.
- PADSCRIPT `RA<0-255>` (`port_main.cpp`): analog R alone, without the digital R click (FLUDD: a light press sprays while walking; a full R press makes Mario stand still).






# Store/call audit (w11)

Method: `tools/ctordiff.py --rank` (now covering every NonMatching function in `report.json`, not only constructors) compares the target and our object per function as a multiset of calls, field stores (offset + constant), float literals and, with `-c`, field loads, `li` immediates and bit masks. Registers and scheduling are ignored.
Ranked lists: `--rank Player Camera MarioUtil Strategic -c` for w11's directories; everything else went to w13 in `port/ctordiff_candidates.md` (180 functions plus a false-positive guide).
Plain decomp corrections (no `SMS_PORT`); `dtk shasum` OK after every batch; Aurora build 619/619; smoke runs 2,0 / 3,0 / 4,0 / 5,1 / 13,1 without a crash.
w13 found several of the same bugs independently (catching, surfing SE, wireSWait, barWait, waitingCommonEvents, hanging, Yoshi see-through floors and the movement tongue): main's versions (w13 section above) were kept for those and are not repeated here.

Fixes (each re-diffed: the differing event is gone):
- Camera/CameraNotice `getNoticeActor_`: MsIsInSight got the Mario angle through DEG2SHORTANGLE; asm multiplies by 0.00549316 = 180/32768, i.e. SHORTANGLE2DEG.
- Camera/CameraNotice `calcNoticeTargetYrot_`: the angle ratio used DEG2SHORTANGLE(1.0f) (182.04); asm constant is 3.05176e-05 = 1/32768.
- Strategic/MirrorActor `TMirrorActor::checkIsInMirror` (2 sites): both "mirror model absent and not unk1A & 4" conditions also require !(unk1A & 0x20) (asm rlwinm bit 26). 95.3 -> 98.9%.
- Player/MarioAutodemo `TMario::electricDamage`: the end status is 0x88D (`li r4, 0x88d`), not MARIO_STATUS_WAIT. Now 100%.
- Player/Yoshi `TYoshi::changeHand`, `TYoshi::thinkAnimation`: hip-attack-end check compared 0x8023C; asm `addis -128; cmplwi 572` = 0x80023C (MARIO_STATUS_HIP_ATTACK_END). Main still had 0x8023C.
- Player/Yoshi `TYoshi::getOff`: missed the tongue reset at the end (`sth 0, 0x7c(mTongue)` = mTongue->mState = STATE_IDLE). 95.3 -> 98.1%.
- Player/MarioUpper `TMario::stateMachineUpper`: the pumping sweat check was the JP inline body of isEmitting; US calls the out-of-line `TWaterGun::isEmitting` (which also returns false in demo and talk modes). Under VERSION_GMSE01 like the header's isEmitting. 72.1 -> 100%.
- Player/MarioDraw `TMario::initModel`: the initial model transform's rotation.y read mFaceAngle.y; asm reads 0x9A = mModelFaceAngle (as calcBaseMtx does).

Checked and found to be noise (no change): ModelWaterManager::move and Tongue::movement (TVec3 copies/inline vs call), cameragc perform/loadAfter/calcPosAndAt (MsSqrtf/MsClamp/setLength inlined), execCameraModeChangeProc_ (changeCamModeSpecifyFrame_ inlined), calcInHouseNo_ / checkCurrentPlane / MarioWaistCtrl / MarioHeadCtrl / diving / lensflare / WaterGun::init / checkController (same field through another base register), setStatusToJumping (target multiplies by a literal 1.0), SMS_AddDamageFogEffect (-400 - -700 and 800 - 500 folded to 300), hanging / pulling / updateDemoCamera / initModel loads (reloads), sunmodel (TVec3 set inline), Yoshi::movement (isMarioThrough/isWaterSurface inlined).
Left: 27 NonMatching functions in these directories still show store/call events, all of them in the noise classes above (`scratchpad` rank, re-run `python tools/ctordiff.py --rank Player Camera MarioUtil Strategic -c`).


## MoveBG, NPC, Map (w11, second split)
After the coordinator's split (w11: MoveBG, NPC, Map; w13: Enemy, GC2D, MSound, System, Animal; header of `port/ctordiff_candidates.md`).
Sources: the MoveBG/NPC/Map entries of the candidates file, `ctordiff --rank MoveBG NPC Map` (26 functions, 62 with `-c`) and `aligned_scan.py` + `aligned_isolated.py` over the same directories. `dtk shasum` OK, Aurora build 619/619, smoke runs 2,0 / 3,0 / 5,0 / 5,4 without a crash.

Fixes:
- MoveBG/MapObjPinna `TFerrisWheel::control` (2 sites), `TFerrisWheel::initMapObj`: the US build used `rotate_frame_rate` there; US asm calls `SMSGetAnmFrameRate()` (as JP). In US `rotate_frame_rate`'s sdata slot is read only by TPinnaShell. GMSE01 branch of the VERSION_SELECT changed; PAL left alone. control 83.3 -> 88.6%, initMapObj 97.0 -> 99.8%. Same value at 60 Hz (60 / vsync rate = 1).
- MoveBG/MapObjFlag `TMapObjFlag::update` (inlined into `TMapObjFlagManager::perform`): US plays the flag SE only if `gpMarDirector->mMap != 3` (`lbz 0x7c; cmplwi 3` before the gateCheck), i.e. no flag sound in Ricco Harbor. Under VERSION_GMSE01. 91.6 -> 94.7%.

Not a bug:
- Map/BathWaterManager `TBathWaterManager::perform` (w13's open item "1.0 vs 0.9/10"): dtk merged the literals into a 6-byte symbol `@3422` = `.float 0.9` + `0x4120` (10.0 at +4), so the constants are equal. `tools/ctordiff.py` now decodes `sym+off` literals and no longer reports it.
- `TBathWaterManager::loadAfter` vtables: the target's `__vt__18TBathWaterRenderer` holds the TBathWaterFlatRenderer functions (dtk naming), so the same class is built.
- Inlining/codegen only: TBaseNPC::perform (execMotionBlend_, getAnmOffDist_, isPollutionNpc out of line in GC), TMirrorModelManager::perform (makeMirrorViewMtx inlined, existing TODO), TMapEventSinkBianco::loadAfter (base loadAfter inlined), TBathWaterMeshRenderer::render (drawCap inlined), TNpcParts ctor (zeroing loop unrolled differently), makeMActors (GC loads anim unkC and never uses it), NPCNeckCallBack, TRedCoinSwitch::control and TAmiKing::moveObject (same conditions, different switch/compare order), THideObjPictureTwin::loadAfter and initMonte (string pool offsets), FluffManager / Shine::calc / Bathtub / ModelGate / Mamma mirror (same field through another base), bgIntersectLine, TItemSlotDrum::generateItem (scheduling), getMActor / setMtx / TVec3 / SMatrix calls vs inline.
Open: `TNpcCoin::requestAppearCoin` uses 13650 = 75 * 182 for the 75-degree launch angle; ours `MsSin(75)` folds to 13653 (75 * 182.044). Direction differs by ~0.02 degrees; the original spelling (an integer 182 factor) is unknown, so it was not changed.


## Decomp corrections (w13), round 2

Scan tools now in the main repo: `tools/aligned_scan.py`, `tools/aligned_isolated.py`, `tools/lowmatch.py`, `tools/mwcc_probe.py` (usage in docs/PORT_AGENT_NOTES.md), plus w11's `ctordiff.py --rank -c`.
Area: src/Enemy, GC2D, MSound, System, Animal (MoveBG/NPC/Map handed to w11 mid-round; notes for them at the end of port/ctordiff_candidates.md). GameCube shasum OK after each batch; Aurora build OK, airport 0/0, Ricco 3/0, Pinna Park 13/0 and plaza 1/1 run 45 s without errors.
- Enemy/gatekeeper `TBiancoGateKeeper::startBGM`/`stopBGM` (inlined into the appear/die nerves): the US asm also calls `MSMainProc::setGateKeeperBGMPlayFlag(mVariant, true/false)`, and on stop only stops track 1 when `getGateKeeperBGMStopFlag()`, otherwise fades it with `setTrackVolume(1, 0.0f, 1, 0)`. Guarded `#if defined(VERSION_GMSE01) || defined(VERSION_GMSP01)` in gatekeeper.cpp, and the declarations/definitions in MSoundMainSide.hpp/.cpp got the same explicit guard (they are in the US DOL at 0x802BC540/0x802BC4F8; both match the US asm 100%). BGKAppear/BGKDie store and call sets now equal.
- Enemy/graph `TGraphWeb::getNearestPosOnGraphLink`: the result is point + link * t (asm fmadds link.x * t + point.x); the call `scaleAdd(t, link, point)` gave link + point * t with our TVec3::scaleAdd (b + c * s). Arguments swapped, with a comment.
- Enemy/enemy `TSpineEnemy::doShortCut`: the asm peeks at the top path node (`mData[size - 1]`, no pop) and only pops into unkF4 after the wall check; we popped twice, skipping a node on every shortcut. 94.7 -> 99.6%.
- Enemy/bgpoldrop `TBGPolDrop::move`: the second setBckFromIndex(13) goes to unk54 (the second MActor), not unk50.
- Enemy/bosstelesa `TBossTelesa::loadAfter`: the fruits get map-object flag 0x4000000 (+0xF8, onMapObjFlag), not live flag 0x4000000 (+0xF0).
- System/CardManager `buildHeader_` (inlined into getOptionWriteStream and filledInitData_): the US title/comment tables have 5 entries indexed directly by MSF_LANGUAGE ("%d/%d Save Data" for English, month first; German/French/Spanish/Italian variants), not JP's 7 entries indexed by language + 1. US-only under VERSION_GMSE01. getOptionWriteStream 87.0 -> 100%. This changes the comment of a newly written memory card file to the US one.
- GC2D/Talk2D2 `TTalk2D2::openWindow`: the window rotation is DEG_TO_RAD(-rot - 1.0f) in the US asm; we had the 1.0f missing. Under VERSION_GMSE01 (no JP/PAL asm to check, those keep the old expression). Note: `DEG_TO_RAD(x)` in include/macros.h has no parentheses around x, so the expression needs its own parentheses (no other call site passes a binary expression).
Checked and found to be codegen noise (no change): GCConsole2 perform (jet/coin counters, memset loop, life meter switch), BossHanachan perform (MsWrap out of line), bossgesso perform/doAttackSingle (showMessage, MsGetRotFromZaxisY inlined), bosstelesa PrepareSlot/flashItem, Koopa nerves, fruitsboat GraphWander (tail duplication), conductor isBossDefeated, Option checkInput, Guide perform (setCenteredSize inlined), SelectMenu perform (SMS_getShineID inlined), TabePuku swimTo / BeeHive doWait (TQuat4::slerp shape, a known blocker), bosseel perform, hinokuri2 Landing (dead read), Hx_Test4, MovieDirector dtor, EventWatcher/MAnmSound/MSoundSE (sqrt inlined), Bird init.
Not changed: smallEnemy `TNerveSmallEnemyChange` calls setFrameRate(0) twice in the asm (once before and once after the emitter); the second call has no effect.
Port note: `--nullgfx` (Dawn null device) runs now hit "Out of memory" creating the "port EFB compute output" buffer at stage load; real-GPU runs are fine.



# Low-match review (w11)

`python tools/lowmatch.py --below 85 Player/ Camera/ MarioUtil/ Strategic/ MoveBG/ NPC/ Map/`: 20 functions. Each was read against the asm with `tools/decomp-diff.py --no-collapse` (call sequence, branches and compares, field offsets, literal values, loop structure). `dtk shasum` OK, Aurora build 619/619, smoke runs 2,0 / 13,1 OK.
Result: no logic difference in any of the 20. The low percentages come from inlining (out-of-line TVec3/MsSqrtf/MsClamp/checkStatusType/setLength in GC vs inline in ours, or the reverse), loop unrolling, register allocation and frame size:
- TWarpInCallBack::execute: same three scale() calls (unk468, status timer, particle factor) in the same order.
- TYoshiTongue::movement: TVec3 copies via ctor/operator-= in GC vs inline; same control flow and calls.
- TBaseNPC::perform: execMotionBlend_/getAnmOffDist_/isPartsAnmNpc/isPollutionNpc out of line in GC, inline in ours (hence 2552 vs 2024 bytes); isNerveMaybeDontCalcAnim0/1 the reverse.
- TNpcEvent::reviveOneSunflower, TMapCollisionBase::updateTrans, TMapWarp::watchToWarp: scheduling and reloads only.
- TMapObjPlane::calcNrm: same four cross products; GC multiplies by a loaded 0.0 (an unfolded zero component) where ours folds it away. Identical for finite heights.
- TMapCollisionData::intersectLine: same grid walk, same list order (+0x4, +0x1c, +0x10 = lists 0, 2, 1) and bgIntersectLine calls; the LineInLineXZ part is inlined on both sides.
- TBathtubData::getGravityDir, TBathWaterManager::throwMario: the matrix rows/columns used for the local and world vectors decode to the same offsets on both sides (xDir +0x18.., zDir +0x30..); quaternion math with the same sign pattern.
- TBathWaterMeshRenderer::prerender: identical GX call sequence.
- CPolarSubCamera::calcInHouseNo_ (9-entry loop unrolled 3x3 vs 8+1), perform, isMarioCrabWalk_/isMarioAimWithGun_ (checkStatusType out of line), CLBCalcNearNinePos: inline vs call.
- Already covered: makeMtxRotByAxis, calcTowerCenterPos_ (w13), TMirrorModelManager::perform (existing TODO), TMapEventSinkBianco::loadAfter (base loadAfter inlined).

Fix:
- NPC/NpcCoin `TNpcCoin::requestAppearCoin`: the 75-degree coin launch angle is an immediate `li r3, 13650` (75 * 182, an integer factor) used for both the sin and cos lookups; `MsSin(75)`/`MsCos(75)` fold to 13653. Now `JMASSin(75 * 182)` / `JMASCos(75 * 182)`. 99.9 -> 100%.


## Decomp corrections (w13), round 3

Area: src/Enemy, GC2D, MSound, System, Animal. Went through the rest of the candidates list and `tools/lowmatch.py --below 85` (47 functions), plus `ctordiff.py --rank -c` filtered for differing immediates, constant stores and bit masks. GameCube shasum OK; Aurora build OK; boot run shows the logo screens.
- System/GCLogoDir `TNintendo2D` (constructed in `TGCLogoDir::setup`): the US logo colour is (220, 0, 0, 255), red; the source had (0, 70, 255, 255). Under VERSION_GMSE01 in include/System/GCLogoDir.hpp (other regions unchanged, no asm to check). Checked with a port screenshot: the boot logo is now the red US one.
- Enemy/bosseel `TBossEel::perform`: the vacuum (bck 14) sound is 0x2180 in the US asm, not MSD_SE_BS_UNG_VACUUM (0x8120, which the US build never uses). Under VERSION_GMSE01.
Checked and found to be codegen noise (no change): MSound getDistPowFromCamera (add order), bosspakkun setGroundCollision, rocket setDeadAnm, BossHanachan execWalk/getBodyMaxRotateZ, koopajr/BathtubKiller manager load/loadAfter, TBubbleManager::createEnemyInstance (constructor), TabePuku Drag / Bird WalkOnGround / AnimalBase execWalk (TQuat4::rotate inlined in the asm, JGQuat4.hpp, out of scope), TTPHitActor::updateTerrainCollsion, feetinv (normalize then scale vs one setLength), limitkoopa head receiveMessage (pushNerve out of line), Kukku/KukkuBall, CardLoad perform, Bird init, Kazekun/doLanding (sqrt inlined).
`MSStageCubeFadeDouble::proc` (96.3%): different bool/branch layout only; same condition, same calls in the same order.




# Smoke suite (w10)
`python port/tools/smoke_all.py` runs the Aurora build through a fixed list of scenes and writes `<build>/smoke/report.md` and `report.json`, comparing with the previous run (new failures at the top). Options: `-j N` (default 3), `--quick` (one scenario per area, 45 s), `--asan`/`--ubsan`, `--only <scenes>`, `--repeat N` (intermittent crashes), `--exe <path>` (test another build), `--no-build`, `--no-seed`, `--list`, `--duration`.
- **Isolation:** the exe, PDB, DLLs and pipeline seed are copied to `smoke/bin_<timestamp>/` first, so merges can relink `port/build_aurora/sms_port.exe` during a run. Each scene has its own working directory, memory card (`SMS_PORT_USER_DIR`), pipeline cache and settings file. Real D3D12 with `SMS_PORT_OFFSCREEN=1`: the window is off-screen and never activated; screenshots are EFB readbacks.
- **Scenes (93):** `boot` (logo, intro, title, attract loop); `newgame` (START through title and file select, A after; STUCK unless area 0 gameplay starts); `fileselect_dvdsync` (START into the file select with `SMS_PORT_DVD_SYNC_BLOCK=1`); warps for every area 0-9 with scenarios 0-2, the remaining plaza states, episodes 4/6/7 of each level, Hotel Delfino, Pinna Park, the casino, Noki undersea, every EX course and boss on the disc, Corona. The area table comes from the disc's `stageArc.bin` (area 8 is Pianta Village/`monte`, 9 is Noki Bay/`mare`; earlier w10 notes that called `--warp 8,1` Noki Bay meant Pianta Village episode 2).
- **Input:** A through dialogs, then (from when Mario gets control) run, jump, spray, dive and turn.
- **Per scene:** PASS / FAIL (early exit, `[port] CRASH`, `[aurora FATAL`, stack cookie) / STUCK (never reached gameplay), the crash excerpt, frames reached, last TApplication state, Aurora ERROR/WARNING counts, audio underruns and drops (`SMS_PORT_AUDIO_LOG`), sanitizer report count, mid-run and last screenshots (PNG).

## First run on main (2026-10-08 18:25, 93 scenes x 75 s, -j 3, 39 min)
PASS 91, FAIL 1, STUCK 1. No audio underruns in any scene; 96 Aurora errors and 190 warnings in total (none above 2 per scene).
- **`0,1` (airport1): exited with 0xC0000409 (fast fail) after frame 3600, no trace.** Not reproduced in 6 re-runs. Fast fail bypasses the unhandled-exception filter (abort(), std::terminate, CRT invalid parameter, pure call), so `port_crash.cpp` now traps those too and prints `[port] CRASH: abort() / SIGABRT` (etc.) with a backtrace; the next occurrence will have a stack.
- **`newgame`: STUCK at the file select (w7/w5).** START opens the "create a file on the Memory Card?" prompt; YES writes `01-GMSE-super_mario_sunshine.gci`, the game returns to "Select data." with all three files still "New", and further START presses bring the prompt back instead of starting file A. A only makes Mario jump. Screenshots: `smoke/newgame/`.
- An earlier hand run hit 5 crashes in 6 inside the AMD D3D12 driver (`amdxc64.dll`) from `aurora::gx::build_pipeline` and once from `aurora::gfx::initialize` (`tex_copy_conv::create_pipeline`), while about ten other agents' sms_port instances were running. Not reproduced afterwards (0 in 30+ runs); environmental GPU/driver stress (w6 for awareness).

## `SMS_PORT_DVD_SYNC_BLOCK=1` file-select crash (w7's BOOTLOG 13 report)
Not reproduced on current main: 0 crashes in 30 normal runs (70 s and 180 s), 6 ASan runs and 6 UBSan runs, nor with w7's own current exe (6 runs), all START through the title into the file select. ASan reported nothing in JAudio; UBSan only the known harmless items. Candidates that changed since w7's observation: the JAudio out-of-bounds reads fixed by the sanitizers (`setBusConnect` past `connect_table`, `cmdIIRCutOff` past `CUTOFF_TO_IIR_TABLE`), the `MixConfig` and `JASSeqParser` bit-order fixes. The smoke suite now runs `fileselect_dvdsync` every time, so blocking sync reads can become the default and any regression will show there (use `--only fileselect_dvdsync --repeat 6` to hunt).

## Report summary line, newgame duration, sanitizer flag
- `report.md` starts with one line: `**OK|PROBLEMS: PASS x, FAIL y, STUCK z** (time, N scenes) | NEW FAILURES: ... (or "no new failures") | fixed: ... | not passing: ... | SANITIZER REPORTS in N scenes: ...`.
- Sanitizer reports do not fail a scene (`halt_on_error=0`), but the summary now lists every scene that had any, so they cannot hide behind PASS.
- `newgame` always runs at least 90 s (`min_duration`), `--quick` included: area 0 starts around frame 3000-3600 (55-65 s at -j 3), so the quick 45 s was always STUCK. `--quick` on main's 19:02 exe: 20 of 20 PASS.

## Run on main 2026-10-08 19:13 (today's merges), compared with 18:25
PASS 92, FAIL 0, STUCK 1; no new failures.
- `0,1` fixed (it passed; the 18:25 fast fail was intermittent, see below).
- `newgame` STUCK: that run started before w7's new `NEWGAME_SCRIPT` landed; with it, `newgame` passes (W7-H).
- Aurora 95 errors / 186 warnings in total (96/190 before), 0 underruns in every scene.
- Ricco and Gelato scenes reached 2400 frames instead of 3600 (machine load at the time; 3600 again on a re-run).

## airport1 0xC0000409 hunt
- With the fast-fail tracing (`[port] CRASH: abort() / SIGABRT`, invalid parameter, pure call) in main's exe: `--only 0,1 --repeat 20 -j 4` gave 20 of 20 PASS; no trace has been caught yet.
- The ASan pass of the same (`--asan --only 0,1 --repeat 20`) passed 20 of 20, but every run logged the same global-buffer-overflow:
  - `TConsoleStr::load` reads `scScenarioNameTable[86]` (70 entries) for the airport's shine 0x56. This is the original game's out-of-bounds read; the GameCube asm does the same.
  - On the GMSE01 disc that word is SelectMenu.o's `.data` `@1210[0]` = 0 (0x803C0CB8; GMSJ01 and GMSP01 land on the same object), so the game used scenario message 0.
  - On PC the read returned whatever global the linker put there; an out-of-range index makes `SMSGetMessageData` return null (`"(null)"` text), so it was not a crash.
  - Fix (`src/GC2D/ConsoleStr.cpp`, `#ifdef SMS_PORT`): an index past the table uses message 0, the disc value. GameCube `mario.dol: OK`; ASan airport `0,0`/`0,1` are clean.
- The fast fail stays open. The next one will print a trace if it comes through abort, an invalid parameter or a pure call; a `/GS` cookie failure or `__fastfail` from inside a driver would not.

## Merge safety: `port/tools/merge_check.py`
- `python port/tools/merge_check.py wN` compares every port/ file across three versions (base = main at the worker's last sync, the worker's copy, main's copy) and sorts them:
  - worker-only: safe to copy;
  - main-only: the worker's copy is STALE, do not copy;
  - BOTH: both sides changed it; a line-based 3-way merge says clean or CONFLICT;
  - new-in-worker, deleted-in-worker, new-in-main.
- `--merge-out DIR` writes the 3-way results, with `<<<<<<< worker / ======= / >>>>>>> main` blocks for conflicts.
- `--apply` copies the worker-only files and writes the clean merges into main; conflicts are never applied. `--apply-new` also copies new files.
- `--all` also checks src/include/libs/config, using `.sync_manifest`.
- The base is recorded with `merge_check.py wN --record`: `.sync_port_manifest` (MD5s) and `.sync_port_base/` (text copies) in the worker folder.
  - `tools/agent_sync.ps1 -Port` (w10's copy, to be merged) copies main's port/ into the worker and records the base right after.
  - Without a base, files that differ are listed as UNKNOWN, with a "MAIN NEWER" or "worker newer" hint.


## E15. Copy-filter users, checked in game (`GXSetCopyFilter`, E8)
- Users found with a temporary log in `TEfbCtrlTex::perform` (removed): in every stage the mirror stage `髀｡謠冗判繧ｹ繝・・繧ｸ` (256x256 RGB5A3, `SMSVFilter_flicker`, flags 0x228) and the screen texture `騾壼ｸｸ繧ｷ繝ｼ繝ｳ謠冗判繧ｹ繝・・繧ｸ` (640x448 -> 320x224 RGB565, mip, flicker filter); in the Bowser bath `BathWaterManager::draw_mist` (640x448 -> 320x224, `{21,0,0,22,0,0,21}`); `hx_wiper` (screen wipes). The pollution maps (`graffito`, R8) use no filter.
- **Mirror** (Hotel Delfino `7,0`, Mario walked into the mirror cube at (-2350, 900, 1200) = the lobby pool with `mirror205.bmd`): Mario's legs and FLUDD reflect in the water, wavy: `port/build_aurora/efb_mirror_hotel_pool.png`. Mirror models exist in Bianco (lake), Gelato (beach and the leaning mirrors), Pinna, Noki, Pianta, Corona and the EX courses (`/scene/map/mirror/` in the scene archives).
- **Heat haze** (`TShimmer`, present in Bianco, Ricco, Gelato and Pinna Park; skipped while FLUDD sprays): the Gelato cliffs and shop shimmer in horizontal waves: `efb_heat_haze_gelato.png` (`4,1`).
- **Bath mist** (Bowser, `60,0`): the clouds and Bowser's spikes behind the steam are softly blurred by the half-size filtered copy: `efb_bath_mist_corona.png`. The bath ripple (water surface in the tub) was not on screen in the scripted run; its copies (84x84 Z24X8 height map, 640x448 0x27) run every frame.
- The Boos in the hotel (screen-texture refraction, `telesa.cpp`) render translucent.



## E16. Presentation at other window shapes; band kept out of the presented image
- What decides it: the present pass (Aurora `gpu.cpp`) draws the viewport `calculate_present_viewport(surface, content)` and samples `uv * content_scale` (content = the 640x448 part of the 640x528 EFB target). Windowed 16:9 / 21:9 / portrait, fullscreen and the three aspect modes (4:3, fit, stretch) all go through it; fullscreen only changes the surface size (same `resize_swapchain` path as E9).
- **Leak found and fixed (aurora_efb_size.patch, `gpu.cpp`):** with bilinear upscaling (internal scale below the window, e.g. `internal_scale = 1` in a 1600x900 window) the last presented row sampled half a texel past the content and blended in EFB row 448 (black or junk) as a dark line at the bottom; FXAA (w6's `aurora_visual_options.patch`) samples up to 8 texels around. The bilinear sample is now clamped half a texel inside the content; `fxaa_sample` clamps the same way (**w6:** that line is in your patch's code, keep it when you regenerate). The area sampler already stayed inside.
- Checked runs: 1600x900 stretch, 1890x810 fit, 700x1000 fit, 1600x900 4:3 and 1600x900 fit with internal scale 1 all start and render; the EFB screenshot at internal scale 1 is 640x448. A picture of the presented window could not be taken: with other copies running the screen copy shows the windows on top of ours (even when ours is raised), and PrintWindow is black for D3D12. Fullscreen was not started (it would cover the user's screen).



## E17. `--nullgfx` runs died at stage load (w13)
- Dawn's Null device cannot allocate the readback buffers (`port EFB compute output`: "Out of memory"). With `SMS_PORT_NULLGFX` (port_main.cpp guarantees it means the Null backend) `port_efb.cpp` takes no snapshots and queues no jobs, and `port_aurora_sdk.cpp` queues no peeks/screenshots: GPU copies stay as they are, game memory as the game wrote it, peeks return their defaults and pixel counts read "dirty". Bianco (pollution copies every frame) now runs 3000+ frames with `--nullgfx`.



## E18. Game-frame clock and interpolated frames (for w9's frame-rate interpolation)
- `port_efb.cpp` now ages things in game frames (`sGameFrame`, one per `port_efb_end_frame` of a frame the game drew), not Aurora frames: copy destinations "alive" within 2 game frames and forgotten after 8, counters restarting after 10 unread game frames, readback lateness. With several presented frames per game frame the old Aurora-frame ages would have dropped pollution readbacks and restarted counters.
- **Hook for w9:** `extern "C" void port_efb_set_presentation_only(int on)`, `int port_efb_presentation_only(void)`. Set 1 before recording an extra (interpolated) presented frame, 0 before the next game frame. While 1: `GXCopyTex` copies on the GPU only (no readback, no destination bookkeeping); `GXPeekZ`, `GXPeekARGB`, `GXReadPixMetric` return the latest game-frame values and queue nothing; `GXClearPixMetric` still clears (picture); `port_efb_end_frame` does nothing (finished readbacks are stored at the end of the next game frame, so game memory changes only between game frames). Screenshots still work.
- **Rule for interpolated frames (w9):** they must not re-run draw passes whose results the next frame reads back cumulatively: the pollution layer stamp/decay/copy group (`graffito`, `TPollutionLayer`: the decay TEV pass would run twice per game frame and goop would fade twice as fast), the bath water ripple/height-map copies, and `TModelWaterManager`'s puddle accumulation. Re-drawing from the game frame's recorded GX stream with replaced matrices, or skipping those groups, keeps them once per game frame. The screen texture and mirror copies are per-frame and fine to repeat.
- w9's record/replay change in `port_efb.cpp` ("results applied exactly 3 game frames after the request" while recording or replaying, their BOOTLOG W9-6) is not in main yet; when merged it should use `sGameFrame` too.



## E19. EFB contract for internal resolution, widescreen (Hor+) and MSAA (w6)
What `port_efb.cpp` / `port_aurora_sdk.cpp` assume, and what the new modes must keep:
- **Mapping:** a snapshot of the EFB target covers the logical EFB (640x528, `vi::efb_size`) linearly, no offset: logical (x, y) = target (x * W / 640, y * H / 528). Copies (`map_copy_rect`, `scale_copy_dst`), peeks and counters all rely on it.
  - Internal resolution (any scale, also non-integer like 4.286): fine as is. CPU-read copies are box-filtered on the GPU down to the GC destination size (`kDownsampleWGSL`, taps = scale) and pixel counts are scaled by logical/target area (`toLogical`), so the game sees GC-sized data. w6's `aurora_copy_scale.patch` (rounded copy rectangles, exact 1:1 copies) is right for this: a 1:1 copy must not resample, or the pollution maps drift (seen at 4.286).
  - **Widescreen Hor+:** the main scene may draw into a wider target, but everything the game copies or reads must still see the GC 640x528 EFB. Proposal: keep the EFB target with the GC mapping above for all copy/peek/counter work and give only the final 3D scene pass (and the display copy) the wider viewport, or, if the wide target is shared, export the logical->target transform (offset + scale per axis) so `map_copy_rect`, `port_efb.cpp`'s `logicalSize`/snapshot mapping and the GXPeekZ texel all apply the same offset. Pixel counters must count only inside the mapped 640x528 rectangle (today they count the whole snapshot).
  - Copies that take the screen (screen texture, mirror, mist) must copy the GC rectangle, not the wide one, or the refraction/mirror UVs (computed by the game for 4:3) slide.
- **MSAA:** tested here with Aurora's `msaa = 4` (temporary switch, removed): pollution readbacks, pixel metrics and screenshots keep working (the snapshots are resolved / depth via w6's `aurora_msaa_depth.patch`), no validation errors, edges anti-aliased. Remaining fidelity point: a pixel at a polygon edge resolves to a partial colour, so a counter can count a pixel the GameCube (one sample at the centre) does not; for the axis-aligned pollution quads this is a few pixels. If it matters, count from sample 0 of the unresolved target.
- **FXAA:** post-process only (present pass), never seen by copies or readbacks; see E16 for the edge clamp.
- I'll review w6's EFB-touching changes when they land in BOOTLOG (copy scaling, widescreen mapping, MSAA resolve before copies/peeks).







## R17. Internal resolution: the airstrip goop vanished at 4x and at 2.41x (copy resampling), and counts depended on the scale
- **How it was checked:** airstrip (`warp 0,0`), `SMS_PORT_COPY_LOG=1`, settings files with `internal_scale` 1, 2, 3, 4, 2.4107 (1080 rows) and 3.2143 (1440 rows); pixel metric of the layer counter (token 0x7e) and the CPU readback of the 512x512 `GX_CTF_R8` map.
- **Symptom:** at 4x and 2.41x the counter went 2518 -> 1213 -> 387 -> 7 -> 0 within seconds and then read 0 for good ("clean" after the 30-zero debounce); the CPU map emptied too. 1x/2x/3x/3.21x looked fine but disagreed: 4541 / 4555 / 4546 / 4556.
- **Cause 1 (Aurora):** the EFB target is not an integer multiple of the logical EFB vertically (528 logical rows are 566 target rows at 1x: the frame buffer is scaled for the 480-line display, E9), so 512 rows map to N + f target rows. `copy_tex` mapped the source with the scissor's outward rounding (floor/ceil) but sized the destination with `lround`, so for f < 0.5 the source was one row taller than the destination (2195 into 2194 at 4x) and every copy was resampled with a linear filter. The layer is drawn back with nearest sampling and copied again every frame, so it crept by a fraction of a row per frame and blurred, and the layer's decay TEV pass ate the blurred edge. EFB captures (`SMS_PORT_EFB_DEBUG`) showed texel rows covering 3-5 target rows in an irregular pattern.
- **Fix 1:** `aurora_copy_scale.patch` (AURORA.md): rounded edges for the copy rectangle and, for a 1:1 copy, a destination exactly the size of the source rectangle. A nearest redraw of such a copy is exact for any fraction.
- **Cause 2 (port_efb.cpp, w8's file):** the readback box-filtered each GameCube texel's block of target pixels (4 x 4.29 at 4x, so it mixed in part of the next row) and the counter counted every target pixel and scaled by the area ratio.
- **Fix 2 (marked `w6 (round 5)` in port_efb.cpp, for w8 to review):** a 1:1 CPU-read copy samples each GameCube pixel at its centre (`taps = 1`, as the copy unit does), and the pixel counter counts on the logical 640x528 grid (one sample per GameCube pixel centre, `kCountWGSL` takes logical and target size; `toLogical` = 1). Down-scaling copies (mip copies) keep the box filter.
- **Result:** counter 4548 and readback 4548 non-zero texels (the same texels: the counter counts the map) at 1x, 2x, 3x, 4x, 2.41x, with 4x MSAA at 1x and 2.41x, and at 16:9 (anamorphic target, R20). The object counter (token 0x92: 3D pollution objects drawn with a perspective camera) varies by +-0.3 % between scales (edge pixels: the target pixel holding a GameCube pixel centre is half a target pixel off) and reads about 2 % more with MSAA while partly dirty (an edge pixel with partial coverage resolves non-black; E19). Both read 0 when clean.


## R18. MSAA 4x
- `[graphics] msaa = 4` (F1: "Anti-aliasing (MSAA)", applies at the next start). WebGPU render targets take 1 or 4 samples only, so 2x and 8x are not offered (an ini value > 1 means 4).
- Depth copies from the multisampled EFB were an Aurora fatal: `aurora_msaa_depth.patch` adds multisampled Z8/Z16 copy pipelines (sample 0) and makes Aurora's depth peek follow the sample count. Colour copies, w8's snapshots/peeks, screenshots and the pollution readbacks already work on the resolved target.
- Checked: airstrip (counts R17), Bianco gameplay and pause menu (screen-texture background) `shots/bianco_msaa4_shot_*.png`, Corona boss (Z24X8 and 0x27 depth copies, no fatal or validation error).
- Corona with MSAA logged E_OUTOFMEMORY for two pipelines (5 retries, draws skipped) in 2 of 3 runs, the first runs after enabling MSAA with 8 other game instances on the GPU; a third run had none (pipelines then in Dawn's cache). Same transient class as R7; with fewer instances it did not occur.


## R19. Graphics options (all off = GameCube rendering): anisotropic filtering, trilinear, FXAA, brightness/gamma, copy filter, vsync
- `[graphics]` in port_settings.ini and the F1 Display tab (w6 additions in w9's files, marked `// w6`; **w9:** new fields `msaa, anisotropy, trilinear, fxaa, brightness, gamma, copyFilter, vsync` in `PortSettings`, `port_settings_active_msaa()`, `port_get_aspect_scale()`, `port_get_display_aspect()`, a `[graphics]` ini section, and `applyDisplay()` calls `aurora_set_texture_filtering`, `aurora_set_post_process`, `aurora_set_copy_filter_enabled`, `aurora_enable_vsync`; the aspect enum grew, R20).
  - Anisotropic filtering: "As the game asks" (default: the texture's GX request, Aurora maps GX_ANISO_2/4 to 8x/16x as before) or forced 1/2/4/8/16x for linear-filtered mipmapped textures (nearest-filtered ones, e.g. the pollution maps, are left alone).
  - Trilinear mipmaps: linear mip blending for every mipmapped texture.
  - FXAA: present pass only (copies and readbacks never see it). Brightness 50-150 %, gamma 50-200 %.
  - "Sharper screen effects": skips `GXSetCopyFilter`'s vertical filter on texture copies (screen texture, mirrors, bath ripple), not original.
  - VSync: `aurora_enable_vsync` (default off as before; frames stay paced by `VIWaitForRetrace`). **w9:** coordinate with the frame-rate work.
  - "Reset graphics to GameCube defaults" button.
- Defaults are byte-identical: EFB screenshot of the airstrip at frame 2000 before and after (`shots/ref_s1_shot_2000.png` vs `s1_shot_2000.png`, same hash). FXAA/gamma verified on a window capture (`shots/crop_def.png` vs `crop_post.png`: dress edge smoothed, brighter).
- **Performance** (RX 9070 XT, D3D12, airstrip warp 0,0; GPU 3D-engine busy time of the process from the Windows "GPU Engine\Running Time" counter over 20 s, per game frame at 30 fps; 8 other game instances shared the GPU, so +-5 %):

  | internal rows | base | MSAA 4x | AF 16x | trilinear | FXAA |
  |---|---|---|---|---|---|
  | 448 (1x) | 0.46 ms | 0.58 | | | |
  | 896 (2x) | 0.68 ms | | | | |
  | 1080 (2.41x) | 0.84 ms | 0.94 | 0.88 | 0.83 | 0.84 |
  | 1344 (3x) | 1.01 ms | | | | |
  | 1440 (3.21x) | 1.13 ms | 1.73 | 1.18 | 1.17 | 1.13 |
  | 1792 (4x) | 1.5-1.7 ms | | | | |

  VRAM 383 MB at 1x, 480 MB at 1080, 574 MB at 1440, 640-660 MB at 4x; MSAA adds 60-100 MB. FXAA runs at window size, under the noise here.


## R20. Aspect: 4:3 (default), fit, stretch, and true widescreen 16:9 / 16:10 / 21:9 (64:27) / 32:9 / match window - render side
- Design (agrees with E19): the logical EFB stays 640x528 GameCube pixels and every copy, peek, counter and readback keeps the linear 640x528 -> target mapping. Widescreen gives the render target and the display the wide shape (`VILockAspectRatio(16, 9)` etc. + FIT, or STRETCH for "match window"), so a logical pixel is wider in the target (anamorphic, per-axis scale in `map_logical_viewport`); internal resolution sets the rows. The 3D view gets wider through the game's projection, not through more logical columns, so the screen texture/mirror copies (which copy the GC rectangle) contain the same wide view and their 4:3-computed UVs stay right.
- **Interface for w11 (game side):** `extern "C" float port_get_aspect_scale(void)` (port_settings.h, port_window.cpp): display aspect / (4/3), 1.0 in 4:3, fit and stretch; updated when the setting or (match window) the client area changes; any thread. Multiply the camera's projection aspect (`TLookAtCamera::mAspect`, set in `TMarDirector` setup to 640*0.913/448) by it for Hor+ (the culling calls `SetViewFrustumClipCheckPerspective(gpCamera->getAspect())` follow), and squeeze 4:3 HUD/menus/movies by 1/scale around x = 320 (full-screen fades/wipes unsqueezed). `port_get_display_aspect()` gives the display aspect (0 = stretch).
- Until w11's hook lands, the widescreen modes show the 4:3 view stretched (`shots/wide169_shot_2000.png`, 1280x720 target); the pollution counts stay 4548 (R17).
- Window: the client area is aspect-locked for every fixed aspect (was 4:3 only); "match window" is unlocked.
- Ini: `aspect = 4:3 | fit | stretch | 16:9 | 16:10 | 21:9 | 32:9 | window`.







## W7-H. Blocking sync DVD reads by default; w5's flag-order patches reverted; newgame smoke scene
- **`sdk_dvd.cpp`:** blocking sync reads (hardware behaviour, BOOTLOG 13) are now the default. `SMS_PORT_DVD_SYNC_BLOCK=0` restores the old inline reads.
  - The file-select crash they once exposed no longer reproduces: w10 ran it 48 times, including ASan and UBSan, after the JAudio table/bitfield fixes; here `smoke_all.py --only fileselect_dvdsync --repeat 6` passed 6 of 6.
  - `smoke_all.py --quick`: 19 of 20 pass. The 20th, `newgame`, ran with the old script; it passes with the new one, see below.
- **Reverted to the original source**, since the root fix makes these unnecessary:
  - `JAIGFrameSequence.cpp`: `mLoadingFlag = true` is back after `loadFileAsync`. Blocking sync reads let the caller run before JASDvdThread's read completes, as on hardware.
  - `JAIGFrameStream.cpp`: `DvdLoadFlag = 1` is back after `DVDReadAsyncPrio`. Async callbacks are deferred now.
  - Plaza (warp 1,0) stage BGM averages 19 voices after `startStageBGM`, against 12 when it is refused. GameCube `mario.dol: OK`.
- **`newgame` smoke scene stuck at "Select data.":** not a card bug.
  - After the card file is created, the file select hands Mario control (`PAD_FLAG_MENU_INPUT` off, the blocks get collision). A file is chosen by jumping into its block.
  - Mario starts beside block A, so A alone jumps past it, and START only reopens the create prompt.
  - A step right and a jump hits block B and the new game starts (opening movie, then the airstrip).
  - `NEWGAME_SCRIPT` in `smoke_all.py` now does START through the title and prompt, then steps right and left with jumps. `--only newgame` passes.


## W7-I. State-gated input: airstrip Piranha 竊・shine 竊・arrest movie 竊・plaza (full chain on real input)
- **PADSCRIPT conditions `?<bit>` / `!<bit>`:** an entry applies only while bit `<bit>` of `gPortCond` (`port_crash.cpp`) is set or clear.
  - The bits come from a read-only SMS_PORT hook in `TBiancoGateKeeper::perform` (`gatekeeper.cpp`): bit 0 = head vulnerable (`mHead->mVulnerable`), bit 1 = asleep (Sleep/SleepDamage), bit 2 = defeated, bits 8-15 = hit points.
  - They are cleared on every app state change, and `SMS_PORT_TRACE` logs each change ("cond ... boss hp N").
  - Main's own `RA<n>` token (analog-only R) covers light presses.
- **FLUDD aim sign, measured:** in the spray stance (`MARIO_STATUS_SQUAT`), stick down (`S180`) raises the stream and stick up lowers it. Releasing R for more than a few frames lets the pitch chase back to its default.
- **Airstrip fight (`--warp 0,1`, real input, about 40 s):**
  - `+0@100-300:G-5000;1250,+0@300-900:G-2750;1480` walks to the west ledge.
  - Asleep: R 50 on / 12 off, gated `?1`. The Piranha wakes after about 1100 frames.
  - Head vulnerable: R 120 on / 2 off, gated `?0`, plus a 3-frame `S180` pulse every 50 frames, gated `?0`.
  - Hits: HP 3竊・竊・竊・ within about 1000 frames of the first vulnerable window, then the defeat demo.
  - Then `+d200-1500:G-1500;1500+?2` and `+d200-2500/45:A+?2` walk to and jump for the shine. Shine get: shines 1, director flags 0x8242.
  - Next: the save prompt (director state 11), answered with A. It wrote `build_aurora/user/USA/Card A/01-GMSE-super_mario_sunshine.gci`; the player's card was not touched.
  - Then the arrest movies (openingBA, BB, the save prompt, BC) and Delfino Plaza (area 1/0) loads with its own gatekeeper asleep.
  - No crash anywhere in that chain with `--nodraw`.
- **Plaza (in progress):**
  - Route from the start along the east street: `G6000;-2000 竊・G6000;3000 竊・G5300;4500`, A through Peach's forced talk and demo, then west along z 4630.
  - The goop field around the plaza Piranha (12, 300, 4631) begins at about x 3300.
  - Mario sinks and loses a life at x 1100-1800 unless the ground ahead is sprayed clean first. Two clean-and-step stops reach about x 1900 safely.
  - Waking it from there, the fight, the Bianco portal (stage-change object 3 at (-6500, 850, -2000)) and the Bianco episode 1 shine are not done yet.
- **For w6:** with real D3D12, an access violation inside the AMD driver during pipeline creation (no game frames) happened twice. Both times it was at the airstrip Piranha's appear or defeat demo (`--warp 0,1`, about f1340 and f2820 with the script above). `--nodraw` avoids it.
  The Null backend (`--nullgfx`) now dies with "WebGPU error 3: Out of memory" early in that stage.

## W7-J. Plaza Piranha and the Shadow Mario chase on real input; async DVD transfers off the interrupt lock
No port bug turned up on this stretch of the story: the plaza fight, the kidnapping event and Shadow Mario's replays all behaved as on GameCube. The work was input tooling plus one DVD change.
- **Rescue:** the `gatekeeper.cpp` gPortCond hook from W7-I had not reached main (the merge skipped that file). It is re-applied here.
- **Plaza Piranha (area 1/0), real input, warp `1,0`, rendered:** defeated in 3 of 4 runs. The last runs killed it within about 1000 frames of waking it. Then the defeat demo, director state 9, and plaza 1/1 loads.
  - Correction to W7-I: Mario does not sink in the goop. Standing in it puts him in `OIL_RUN`, and `checkGraffitoSlip` hurts him every few seconds (`floorDamageExec`); goop blobs knock him back into it.
  - The approach that works: a slow walk (`G<x>;<z>;60`) with a light pumped spray (`RA200`, 120 on / 4 off), so he cleans the ground ahead as he goes. Clean-and-step stops failed because the `G` autopilot overshoots, turns back, and then sprays behind him.
  - Waking it: a planted spray held on its body (`R` + `F0;4631`, aim raised for 20 frames).
  - Damage: in the plaza's `TNerveBGKWait`, only head hits in animation 0x12, frames 50-160 (`isHeadHitActive`), do damage. A hit outside that window sends it to animation 7, and it stays there while it is sprayed. So spray only while new bit 3 is set: `R+F0;4631+S180;45+?3`. That sweep raises the aim from level and passes the head (about gun -7000) within the window.
  - Measured in the spray stance: stick Y changes the aim at a rate (`unk36E` integrates `stickY * mRButtonMult`). `S180;45` moves it about -126 per frame up to the clamp at -12945; level is 0. Stick X turns Mario (`squating`).
- **Plaza 1/1, the kidnapping (warp `1,1` with `SMS_PORT_WARP_FLAGS=story1`, which starts the same event):** chased on real input until Shadow Mario's 64 water hits ran out. Then: he goes down (doing 14/15) → Peach's talk → he runs off (16/17) → waits to invite Mario at graph node 7 (20) → runs to the gate (21) → draws the rainbow M on the statue base at (0, 300, 5010) during a demo (23) → disappears into it (10). Screenshot: w7 scratch `s18/shot_8400.png`.
  - The chase takes about 6000 frames: a refill trip to the sea, a ramp detour, drops off the terrace.
  - His graph has 9 nodes: (138,1777,4447) (5250,300,4050) (6000,300,-250) (5650,300,-4750) (3360,700,-4630) (3460,626,-520) (3400,300,3420) (2320,300,3460) (0,300,5010).
  - **Open:** entering the M. Mario stood against it (`G*` to Shadow Mario at the gate) and no stage change came. The trace lists stage exits 0-6 on 1/1, including (-6500, 850, -2000) → 22, which hangs over the canal under the west arch, and (0, 2400, 1100) → 6. Next: find which object the M is (a TMapObjChangeStage the gate activates, or the gate actor) and how Mario enters it (walk, jump or hip drop). Then the Bianco Hills episode 1 shine, and the plaza save prompt on the test card.
- **New PADSCRIPT tokens** (`port_main.cpp`; the logic is in `Application.cpp`, all debug, read-only toward game state):
  - `S<deg>;<pct>`: a partial stick tilt.
  - `G<x>;<z>;<pct>`: a slow walk.
  - `F<x>;<z>`: turn toward a point in the spray stance (stick X only, so an `S` token in the same entry still aims).
  - `G*` / `F*`: the same toward the stage's Shadow Mario.
  - `GN<x>;<z>`: walk along `SMS_PORT_NAV`.
  - `W<x>;<z>|...`: a route.
  - `<a>-<b>/<p>;<on>:`: hold for `<on>` frames of every `<p>` (keeps long scripts under the 32 KB environment limit).
- **`SMS_PORT_NAV="x,y,z x,y,z ...;a-b a>b a^b"`:** a small walk graph: `-` links both ways, `>` one way (a drop), `^` both ways with a jump. `G*` and `GN` walk it breadth-first. Without it, `G*` uses Shadow Mario's own graph (links climbing more than 350 are left out). The plaza graph used here:
  `1500,300,5600 2320,300,3460 3400,300,3420 3500,300,1800 3460,640,-520 3440,700,-3000 3360,700,-4630 5300,300,4400 6000,300,3000 6000,300,-250 5900,300,-2500 5650,300,-4750 7300,100,-250 10600,0,-250 3750,700,-4680 4500,300,-4700;0-1 1-2 2-3 3-4 4-5 5-6 2-7 7-8 8-9 9-10 10-11 9^12 12-13 6-14 14>15 15-11`
- **New condition bits (`?n` / `!n`):**
  - 3: the gatekeeper head hit does damage now.
  - 16: a talk or demo is running.
  - 17: Mario is wading in goop.
  - 18: Shadow Mario is waiting in his replay.
  - 19: he is within 1000 (x/z).
  - 20: he is down.
  - 21: his water hits count now.
  - 22 / 24: he is above / below Mario by more than 250.
  - 23: FLUDD needs a refill (set below 15% water, cleared at 95%).
  - 25: the walk graph says jump now.
  - Refill: in water, hold analog R (`TWaterGun::suck` needs pressure). Swimming alone does not refill.
- **Trace (`SMS_PORT_TRACE`):** stage exits as each stage loads (`MapObjTown.cpp`: position and target), Shadow Mario's state, water hits and position, his graph once per stage, FLUDD pitch and face angle in the position line, and `SMS_PORT_TRACE=aim` for the pitch every 10 frames in the spray stance.
- **The scripts used** (stage clock `+0@`; plaza 1/0 after `1000-1400/40:A` for the judge's prompt):
  - Plaza fight: `+0@0-400:G6000;-2000,+0@400-900:G6000;3000,+0@900-1300:G5300;4500,+0@1300-4000/30:A+?16,+0@2100-2250:G3600;4630,+0@2250-3700:G900;4630;60`.
    Then `RA200` pumped to 3700, then from 3700: `R+F0;4631+?1+!17` (150 / 4, with `S180;45` for the first 20 frames), `R+F0;4631+S180;45+?3+!17`, and `G1150;4630+?17`.
  - Chase on 1/1: `+0@0-150:G1500;6000,+0@150-300:G5300;4500,+0@300-60000:G*+!19+?21+!23,+0@300-60000/404;400:RA255+!19+?21+!23,+0@300-60000/76;70:R+F*+S180;45+?19+?21+!23,+0@300-60000:GN10600;-250+RA255+?23,+0@300-60000/20:A+?25,+0@300-60000:G*+!21+!23,+0@300-60000/30:A+?16` with the graph above.
- **`sdk_dvd.cpp`, async transfers off the interrupt lock (w14's report):**
  - Before: the host file read ran inside the deferred completion, in interrupt context with the interrupt lock held. A big async read stalled every game thread and the VI retrace that drained it.
  - Now a drive thread (a host thread) does the transfers in issue order, starting when the request is issued, without the interrupt lock.
  - The completion only picks up the result. If its transfer has not ended yet, it goes back on the deferred queue (behind the later ones, which do the same, so the order holds), and the drive thread drains again when the transfer ends. With a warm file cache the transfer is done long before the completion runs, so timing in game terms is unchanged.
  - `DVDCancel*` waits out a transfer in progress, and a queued one never starts, so a canceled buffer is never written afterwards.
  - Host reads are serialized (`gReadLock`), so DVDReadPrio's inline path (interrupt context, host threads) never runs beside the drive thread.
  - Every run since uses it: airstrip, plaza and the Shadow Mario chase; the `fileselect_dvdsync`/`newgame` smoke scenes are in the smoke run below.
  - **For w14:** this touches `completeReq`, `queueReq`, `cancelReq` and the inline read in `DVDReadPrio`. That inline line is the one your read-ahead cache patch changes as well.
- GameCube `mario.dol: OK`.


# Mechanics checklist (w12)
Main as of 19:00 plus M1-M4, real D3D12 (Null backend for a few scans), `SMS_PORT_NO_PADS=1`, `SMS_PORT_USER_DIR` in the build folder.
Each mechanic was driven with `SMS_PORT_PADSCRIPT` (stick angles, holds, `+n` stage-relative frames, `G<x>;<z>` autopilot) and read from the new
`SMS_PORT_TRACE=act` lines (every change of Mario's action/nozzle/Yoshi state with position, speeds, HP and water, plus coin/red/blue counters),
`SMS_PORT_AUDIO_LOG` (each distinct sound id, mapped to `MSD_SE_*`) and screenshots.

## New debug knobs
- `SMS_PORT_TRACE=act` (`Application.cpp`, read-only): `[act] f<frame> <old> -> <new status> pos ... fwd vy hp water noz yoshi` and `[act] f<frame> coins/red/blue`.
- `SMS_PORT_DEBUG_CALL=tp<x>;<y>;<z>@<frame>`: puts Mario at a position (to reach an object a script cannot walk to).
- `SMS_PORT_DEBUG_CALL=hatch@<frame>`: hatches the stage's Yoshi egg the way a fed egg does (`appearFromEgg` with a fruit picked from the stage) and puts Mario next to it.

## M1. Every jump made while the stick was held became a spin jump (`TMario::checkStickRotate`)
- **Symptom:** running + A always gave `LEFT_ROTATE_JUMP`, so no normal/second/triple jump from a run.
- **Cause:** the GameCube code really reads `BOOL q[4]` uninitialised (asm: only `li r0,1; stw r0,0x10..0x1C(r1)`, then `cmpwi` each slot with 1). clang treats the unset slots as undefined and folded every `q[i] == 1` to true, so "all four stick quadrants seen" was always true.
- **Fix:** `MarioMove.cpp`, under `SMS_PORT` the slots start at 0, so the result depends only on the stick history (on hardware a slot reads 1 only if an earlier call left exactly that at the same stack address). Now JUMP -> SECOND_JUMP -> ULTRA_JUMP, and the spin jump needs a stick rotation (`RIGHT_ROTATE_JUMP` with a quarter-turn per 2 frames).

## M2. Turbo nozzle never fired (`TMario::checkController`)
- **Symptom:** with the turbo nozzle, holding R made Mario run at normal speed (30) with no water used; `MSD_SE_SY_NEWP_AIR_TAME` (charging) played, the dash never started.
- **Cause:** decomp transcription. The GameCube stores the propeller value at `getCurrentNozzle() + 0x714` (asm `stfs f31, 0x714(r3)`): the turbo nozzle is at TWaterGun+0x18D8, so that is TWaterGun+0x1FEC, 0x21C bytes past the end of the 0x1DD0-byte object (`li r3, 0x1dd0; bl __nw` in `MarioInit`), outside the water gun. The decomp wrote it to `TNozzleTrigger::unk388`, the trigger's pressure fill, resetting it every frame Mario stood or ran.
- **Fix:** `MarioMove.cpp`, under `SMS_PORT` the out-of-object store is not reproduced. Now: turbo dash at speed 80 draining water, across water too, and the turbo/surf jump (`BROAD_JUMP`) out of the water.

## M3. Mario fell off a pole as soon as he grabbed it (`TMario::barWait`)
- **Symptom:** jumping onto a pole (Bianco, (-4050, 9900)) gave BAR_HANG -> BAR_WAIT -> LAND_SAFE_DOWN on the next frame, thrown back 200 units, every time.
- **Cause:** decomp transcription: the asm is `fcmpo f1, f0` against 100.0f, `cror eq, lt, eq`, `bne` past the drop, i.e. drop when `mHolderHeightDiff <= 100.0f` (slid to the bottom). The source had `> 100.0f`. (Main had already corrected the button bit to 0x8000.)
- **Fix:** `MarioSpecial.cpp` (NonMatching unit, no `#ifdef`): `<= 100.0f`. barWait 99.5% -> 99.9%; DOL unchanged. Now grab, climb (stick up), slide down (stick down) and let go at the bottom, and jump off (A, as `WALL_JUMP`).

## M4. Timed missions failed the moment they started (`TGCConsole2::setTimer`)
- **Symptom:** Ricco episode 6, after the red coin switch's message: Mario dies at once (`LOSER_DOWN`, life lost, back to the plaza).
- **Cause:** decomp transcription: `setTimer` ends with `stw r30, 0x500(r31)` (r30 = the time shown, after the 599999 cap); the source stored the argument (`-1` for a running timer). `getFinishedTime()` hands that to the event scripts (`getTime`), which then saw the time as up.
- **Fix:** `GCConsole2.cpp` (NonMatching unit): store `timerValue`. setTimer 99.8%; DOL unchanged. Now the 2:00 red coin timer counts down with 0/8 shown.

## Pass/fail
| Mechanic | Result | Where / how | Notes |
|---|---|---|---|
| Triple jump | pass (after M1) | Bianco 2,0, run + A every 8 frames | JUMP -> SECOND_JUMP -> ULTRA_JUMP, jump voices |
| Side flip | pass | run, stick reversed + A | U_TURN_JUMP |
| Backflip | pass | R + A standing | SQUAT -> BACK_JUMP |
| Long jump | pass (as turbo jump, after M2) | turbo + A / from water | `BROAD_JUMP` needs FLUDD emitting (turbo); R + A with the spray nozzle crouches, as the code says |
| Wall kick | pass | Sirena 6,1 run + A into a wall | WALL_SLIDE -> WALL_JUMP |
| Ground pound | pass | A, then L in the air | HIP_DROP -> HIP_ATTACK_END, `MSD_SE_MA_HIP_ATTACK`; breaks wood boxes |
| Belly slide / dive | pass | run + B | JUMP_CATCH -> slide -> CATCH_LOST, `MSD_SE_MA_SLIP` |
| Spin jump | pass (after M1) | stick circle + A | RIGHT/LEFT_ROTATE_JUMP, `MSD_SE_MA_ROLL_JUMP` |
| Crouch | pass | hold L | SQUAT, SQUAT_STANDUP |
| Swimming, diving, surfacing | pass | Sirena 6,1, Ricco | SWIM_START/PADDLE, B dive (SWIM_DIVE), A up (SWIM_UP), swim sounds |
| Refill FLUDD | pass | Sirena, R in water | 9688 -> 10000 |
| Spray | pass | R | water drops, `MSD_SE_PO_WATER_HI` |
| Hover | pass | X then jump + R | ROCKET status with vy ~0, `MSD_SE_PO_HOVER` |
| Rocket nozzle | pass | plaza 1,8 (story8r + flag 10366), box at (800, 2300) | box stomped (TRAMPLE), item taken, R: vy 140 launch |
| Turbo nozzle | pass (after M2) | plaza box at (-800, 2300) | dash 80, water drains, runs on water |
| Nozzle switching (X) | pass | | 0 <-> 4 / 1 / 5, `MSD_SE_SY_SELECT_POMP(_BACK)`; boxes refill after a nozzle is taken (`resetNozzleBoxesModel`) |
| Yoshi mount | pass | Ricco 3,7 egg (11000, 1500, 1100), `DEBUG_CALL=hatch` | YO_BORN, jump on: Yoshi 8, nozzle 3 |
| Yoshi juice / tongue | pass | R / B | spit status, `YO_TONGUE_RELEASE/BACK` |
| Yoshi eats fruit | pass | hatch eats the fed fruit | `YO_TONGUE_GOKKUN`, `YV_DELICIOUS` |
| Yoshi dismount / water | pass | X; ride into Ricco's harbour | BACK_JUMP off; deep water: Yoshi 3 (drowning) -> egg, Mario swims |
| Feeding the egg by carrying fruit | not tested | | the egg's fruit is random; plaza `eggYoshiEvent` ignores fruit |
| Tightrope / wire | pass | Bianco wire (-3625..-2375, 3300, 5450), tp onto it | WIRE_WAIT, bounce (0x892), hang (0x352/0x35B), moves along |
| Fence climbing | pass | Ricco | FENCE_CATCH, climb, FENCE_MOVE |
| Revolving fence | not reached | Ricco `fence_revolve` | |
| Lily pad | pass | Bianco LeafBoat (9000, 2300, 600) | stands on it, spraying moves it |
| Cannon | not reached | Pinna `demoCannon` is blocked/scripted | |
| Sand birds, slides | not tested | | EX/Gelato/Pinna, see w13's EX runs |
| Spinning platform | not confirmed | Bianco `BiaTurnBridge` stood on, did not turn | it may need its trigger |
| Poles | pass (after M3) | Bianco | |
| Wood boxes | pass | Ricco box (-13400, 300, 1700) | ground pound: `IT_BARREL_CRASH`, coin pops out and is collected |
| Coins | pass | Bianco coin trail | 0 -> 5, `SY_COIN` |
| Blue coin | pass | Bianco (-13701, 1600, 5709) | blue 1, `SY_BLUE_COIN_GET` |
| Red coins | partial (after M4) | Ricco 6 switch at (-15700, 100, 8700) | switch, message, 2:00 timer and 0/8 counter work; no coin reached by script |
| 1-up | pass | plaza (8400, -1300, -2900) | lives 3 -> 4, `SY_1UP` |
| Health / damage | pass | Ricco fence enemy | hp 8 -> 7 |
| Death | pass | sirena_ex0 void (`tp`) | FALL_DEAD, lives 3 -> 2, course restarts |
| Shine get + save prompt | pass | sirena_ex0 `tpshine` | SHINE GET demo, state 11 save prompt (Save and Continue / Save and Quit / Continue), card written, back to the plaza 1/9 |

Open items:
- **w6/w8:** the save prompt's background is flat grey (it should be the frozen stage). Nozzle-box/turbo screens fine.
- **w8/w9:** with 8-10 game instances from other workers running at once, some of my runs nearly stopped presenting frames (main thread in `VIWaitForRetrace`, render worker in `port_efb.cpp` `jobEncode`); with less load the same scripts ran normally. Each EFB readback creates a new buffer (`jobEncode`, about 7 per frame in Bianco); worth checking they are released, since Null-backend runs in Bianco/Ricco die with "WebGPU error 3: Out of memory".
- My level-sweep runs earlier today did not set `SMS_PORT_USER_DIR`, so they used the player's card folder; the card file there was modified at 15:27, during that sweep. A scripted run may have saved over the player's slot.



## R21. Dirty Mario captured (airstrip goop)
- `warp 0,0`, `SMS_PORT_PADSCRIPT=100-1600/30:A,+30-900:G-1882;1136,+900-1400:G-1700;1100`, `SMS_PORT_TRACE=1`: A through Peach's talk, then the autopilot walks Mario into the goop field west of the start. He sinks to y 300 (action 0x0004045C) and loses one HP every 300 frames until a life is lost at f3614.
- `shots/dirty_shot_2400.png`, zoom `shots/dirty_mario_crop.png`: Mario wears the airport dirt layer (R9: `h_ma_rak.bti` pink/orange swirl swapped in for `H_ma_rak_dummy`) over his skin, cap and overalls. The goop's pink/orange/yellow surface and the dark sinking tint render as well.







# Widescreen (game side, w11)

True widescreen (Hor+) on the game side, driven by w6's `port_get_aspect_scale()` (display aspect / (4/3), 1.0 in the 4:3, fit and stretch modes).
Hooks: `port/include/port_widescreen.h` + `port/src/port_widescreen.cpp` (`PORTWideScale`, `PORTWideCameraAspect`, `PORTWideSqueezeRectX[i]`, `PORTWideOrthoX`, `PORTWideSightExtraDeg`, `PORTWidePaneScope<>`).
`SMS_PORT_WIDESCREEN=16:9|21:9|<aspect>` overrides the setting for tests; `SMS_PORT_WIDESCREEN_LOG=1` counts the hook calls and how many changed a value.
All shared edits are `#ifdef SMS_PORT`; `dtk shasum` OK.

Model: the game keeps its 640-wide logical frame; the render side shows it anamorphically (every viewport/scissor x stretched by scale). So the game widens its 3D by scale and squeezes its 4:3 2D by 1/scale, and full-screen 2D drawn with a plain 0..640 projection needs nothing.
- 3D: `PORTWideCameraAspect(&mAspect)` before the projection in `CPolarSubCamera::perform`/`loadAfter` (game, demo, gate and jet-coaster cameras) and `JDrama::TLookAtCamera`/`TPolarCamera::perform` (title/file-select/shine-select cameras). Vertical FOV is the GameCube's. It writes the widened aspect into the camera itself, remembering the game's value per field, so every reader of `gpCamera->getAspect()` follows: the frustum culls (`SetViewFrustumClipCheckPerspective` in TLiveManager::clipActorsAux = all enemy/map-object/NPC managers, conductor, animals, fish, pakkun, gesso, elecNokonoko, enemy attachments), the mirror camera and its projected texture, the water/screen-texture light projections, and JPA particles (`drawInfo.setAspect`). Nothing pops in or freezes at the new edges.
- Gameplay view test with a fixed angle: `TBaseNPC::perform`'s 120-degree "camera sees it" cone (NPCs out of it stop updating) grows by the extra horizontal FOV (`PORTWideSightExtraDeg`). As asked, the wider view slightly changes which actors run versus the GameCube, but only when widescreen is on.
- 2D: `J2DGrafContext::setPort` (viewport) and `setScissor` squeeze x by 1/scale around 320, so every J2D screen (HUD, pause, talk boxes, file select, card screens, shine select, subtitles) keeps its 4:3 shape, centred, and pane clipping stays consistent. `JDrama::TOrthoProj` widens only the 600-wide HUD frames (x 0..600: "Group 2D", menu, shine-select cameras); full-frame orthos (0..render width), the lens-flare ortho and the offscreen ones (graffito, pollution layers) are untouched. The GC/Dolby logo frame is widened where it is created.
- Full-screen 2D inside squeezed screens is widened back: the pause 'mask', the card-save 'mask', the stage-intro letterbox bars (ConsoleStr 'msk1/2'), the shine-select letterbox bars ('msk1/2') and its background gradient quad.
- The fader (TSMSFader rects), hx_wiper wipes and screen-texture quads use 0..640 projections, so they cover the full width already (the iris wipe's circle is stretched by scale, which is cosmetic).
- 4:3: every hook returns at once when scale == 1 (the camera aspect is never touched, the slot table stays empty). `SMS_PORT_WIDESCREEN_LOG=1` in a 4:3 Bianco run: 22200 hook calls, 0 changed a value, also with w6's real setting (`aspect = 4:3`). SMS_PORT_TRACE runs are not frame-deterministic between runs (load timing), so the counter is the equality check.

Verified (screenshots stretched to the display shape; with w6's setting `aspect = 16:9` the hooks report scale 1.3333): Bianco 2,0 at 16:9 and 21:9 (same vertical framing, more on both sides, HUD centred); pause menu (mask full width); plaza 1,1 at 16:9 and 21:9 (talk box and HUD intact, edge NPCs drawn); Petey Piranha (biancoBoss 55,0) and the Mecha-Bowser coaster (58,0) at 16:9; the Dolby logo; the file select (3D wider, J2D boxes stay aligned over the 3D blocks: a Hor+ projection compresses x by exactly 1/scale like the 2D squeeze).

Notes for w6 (render):
- Contract used: every GX viewport and scissor x (and width) stretched by `port_get_aspect_scale()`, the same for 2D and 3D. No separate 2D viewport is needed; the game squeezes its own 2D.
- Movies (title, intro THPs) are still stretched: they are drawn by `sdk_thp.cpp`/`MovieDirector` with a 0..W ortho. Pillarboxing belongs there (the movie subtitles are J2D and are already squeezed to match).
- HUD edge anchoring, lens glow and iris shape: done in "Widescreen round 2 (w11)" below.

Notes for w8 (EFB copies):
- Offscreen passes keep their GameCube orthos and viewports (graffito 512x512, pollution layers, BathWater, bossManta screen pass). If their viewport x is stretched like the screen, the copy source rect must be stretched the same way and the destination keep its GameCube size, so the textures stay identical.
- Screen-space lookups (ModelWaterManager refraction, mirror projection via `C_MTXLightPerspective(gpCamera aspect)`) already use the widened aspect, so a full-frame copy of the wide picture lines up.

## Widescreen round 2 (w11): HUD position option, news ticker, lens glow, iris, movie subtitles
- **HUD option (user request).** `[window] hud_position = centered | 16:9 | match` (w9's `port_settings.*`, marked w11) and F1 > Display > "HUD": 4:3 centered (original, default) / 16:9 edges / Match aspect (screen edges, e.g. 21:9). Applies at once. `SMS_PORT_HUD_EDGES=0/1/2` overrides it for tests.
  `TGCConsole2::perform` puts the HUD layout's top-level groups in a `PORTWideHudEdgesScope` while drawing: each group is classified by the centre of its parts in the 600-unit HUD frame (parts wider than the frame, the ticker's scrolling text boxes, are ignored) and moved by `PORTWideHudShift` = +-300 * (scale - 1) units (16:9 mode: the scale is capped at 16:9's), then put back.
  Left: shine, blue-coin and coin counters, timer, red-coin and goal counters. Right: water gauge (+ FLUDD balloon, juice text), life meter. Centre: Mario/lives, news ticker. Talk boxes, pause, menus and the shine select are other screens and stay centred.
  The water gauge parts GCConsole2 draws itself from pane bounds (`drawWaterBack`, `drawWater`, `drawJuice`: position matrix and the J2DPicture draws) follow their group's shift (`shiftOf`). Fixed on the way: the shift must not go into the texture matrices (it broke the pressure circle, `drawWaterBack` reuses `mtx` for TEXMTX0).
- **News ticker.** Its scissor is a raw `TGraphics::setScissor(unk544)` that J2D's squeeze never sees; it is now squeezed with `PORTWideSqueezeRectXi` (plus the ticker's HUD shift, 0 since it stays centred). The other raw scissor calls (ConsoleStr.cpp 286, CardLoad.cpp 844, Menu.cpp 15, CardSave, PauseMenu2) restore the full-frame scissor from `graphics->getScissor()`, which already spans the wide picture: unchanged.
- **Lens glow.** `TLensGlow` (the sun glow sprite, drawn in the full-frame +-w/2 ortho at the right screen position) gets x scale / `PORTWideScale()`, so it stays round. `TLensFlare` is a 3D model drawn by the widened camera: nothing to do.
- **Iris wipe (hx_wiper).** Horizontal offsets / scale (round), radii * (wide corner distance / 4:3 corner distance), so the fully open iris still reaches the wide picture's corners with the same timing. Game-over and logo wipes keep their shape the same way.
- **Movie subtitles were stretched** (they are not drawn through J2D's viewport: `TMovieSubTitle::draw` only calls `setup2D`, so they use the movie director's 0..640 camera, which is not widened). New `PORTWideOrthoProjBegin/End` / `PORTWideOrthoProjScope` squeezes the current orthographic projection around its centre for that draw; also used for the ending's card-save prompt in the movie director (`TCardSave` with `unk18`; not captured, needs the ending). Now the subtitles sit over w6's pillarboxed movie exactly as at 4:3 (text spans 20%..81% of the movie width at 4:3, 16:9 and 21:9).
  Note: US subtitles are off on a fresh save (the US ROM checks flag 0x90001, the Options "Subtitles" setting, and new files leave it 0). For tests: `SMS_PORT_WARP_FLAGS=90001=1`.
- **4:3 identical:** `SMS_PORT_WIDESCREEN_LOG=1` at 4:3 with `hud_position = match` (plaza 1,8 + ticker, Bianco + pause) and with `16:9`, and the movie with subtitles: 0 changed values (19800 / 14400 / 600 hook calls). GameCube shasum OK.
- **Captures** (`SMS_PORT_SHOT_PRESENT=1`, off-screen windows 1280x720 / 1680x720, real `aspect` setting): plaza ticker at 16:9 centred / match and 21:9 16:9-mode / match (ticker clipped to its bar in all); Bianco 21:9 match and its pause menu; Corona Bowser (60,0) 16:9 match and 21:9 16:9-mode; the exit-area iris at 4:3, 16:9 and 21:9 (round); the intro movie with subtitles at 4:3, 16:9 and 21:9.
  Not captured: the lens glow (no scripted view of the sun found; no C-stick in the pad script). Seen in passing: the iris's black area shows thin horizontal gaps at 4:3 at 720p (GX_LINES rows of the 448-line frame at a higher internal resolution); also at 4:3, so not widescreen related.


## Decomp corrections (w13), round 3

Area: src/Enemy, GC2D, MSound, System, Animal. Went through the rest of the candidates list and `tools/lowmatch.py --below 85` (47 functions), plus `ctordiff.py --rank -c` filtered for differing immediates, constant stores and bit masks. GameCube shasum OK; Aurora build OK; boot run shows the logo screens.
- System/GCLogoDir `TNintendo2D` (constructed in `TGCLogoDir::setup`): the US logo colour is (220, 0, 0, 255), red; the source had (0, 70, 255, 255). Under VERSION_GMSE01 in include/System/GCLogoDir.hpp (other regions unchanged, no asm to check). Checked with a port screenshot: the boot logo is now the red US one.
- Enemy/bosseel `TBossEel::perform`: the vacuum (bck 14) sound is 0x2180 in the US asm, not MSD_SE_BS_UNG_VACUUM (0x8120, which the US build never uses). Under VERSION_GMSE01.
Checked and found to be codegen noise (no change): MSound getDistPowFromCamera (add order), bosspakkun setGroundCollision, rocket setDeadAnm, BossHanachan execWalk/getBodyMaxRotateZ, koopajr/BathtubKiller manager load/loadAfter, TBubbleManager::createEnemyInstance (constructor), TabePuku Drag / Bird WalkOnGround / AnimalBase execWalk (TQuat4::rotate inlined in the asm, JGQuat4.hpp, out of scope), TTPHitActor::updateTerrainCollsion, feetinv (normalize then scale vs one setLength), limitkoopa head receiveMessage (pushNerve out of line), Kukku/KukkuBall, CardLoad perform, Bird init, Kazekun/doLanding (sqrt inlined).
`MSStageCubeFadeDouble::proc` (96.3%): different bool/branch layout only; same condition, same calls in the same order.
Sweep paused here (2026-10-08) for the regional-behaviour task (see "Regional behaviour (w13)" below). Done: the candidates list for Enemy/GC2D/MSound/System/Animal and lowmatch < 85%. Not yet done: the 85-95% band of those directories (aligned_scan + aligned_isolated, then ctordiff -c), and the large GC2D functions (GCConsole2 perform, Guide perform) beyond the spot checks above.


## E20. Screenshots of the presented picture (`SMS_PORT_SHOT_PRESENT=1`), and presentation checked with them
- **New:** `port/patches/aurora_present_capture.patch` adds `aurora::gfx::request_present_capture(callback, userdata)`: one-shot copy of the swapchain texture after the present blit and the ImGui overlay (exactly what the window shows), read back after submit. The surface gets `CopySrc` when its capabilities allow it (D3D12 does).
  `SMS_PORT_SHOT_PRESENT=1` with `SMS_PORT_SHOT=<frames>` writes `present_<frame>.bmp` (window size, bars included) next to the usual `shot_<frame>.bmp` (EFB). Works with the window covered or off-screen (`SMS_PORT_OFFSCREEN=1`, recommended: nothing appears on the desktop) and with other copies running; nothing with `--nullgfx`.
- **Checked (Bianco, off-screen windows; `aspectruns.ps1`-style settings files via `SMS_PORT_SETTINGS`):** content box measured on each picture, last presented rows compared with the rows above.

  | window | aspect | internal | presented content | bottom rows |
  |---|---|---|---|---|
  | 1600x900 | stretch | window | 1600x900, fills | continuous |
  | 1890x810 (21:9) | fit | window | 1157x810, pillarboxed, 1.428 | continuous |
  | 700x1000 (portrait) | fit | window | 700x490, letterboxed | continuous |
  | 700x1000 | stretch | window | 700x1000, fills | continuous |
  | 700x1000 | 4:3 | window | window becomes 700x525 | continuous |
  | 1600x900 | 4:3 | window | window becomes 1200x900 | continuous |
  | 1600x900 | fit | window | 1286x900, pillarboxed | continuous |
  | 1600x900 | fit | native 640x448 | 1286x900 (upscaled) | continuous (E16 clamp) |
  | 1600x900 | stretch | native | 1599x900 | continuous |
  | 2560x1440 (screen-sized) | fit | window | 2057x1440 | continuous after the fix below |
  | 2560x1440 | 4:3 | window | window 1920x1440 | continuous after the fix below |

  No band in any of them. The screen-sized window stands in for fullscreen (same `resize_swapchain` / present path; fullscreen itself was not started).
- **Fix found with it (aurora_efb_size.patch, `gpu.cpp resize_swapchain`):** at 2560x1440 the presented content was 1439 rows plus one black row: the EFB target height was rounded to nearest (1440 * 528 / 448 = 1697.1 -> 1697), so the game's 448 lines covered only 1439.9 target rows and the last row was never rasterised. The target is now rounded up (1698); `SMS_PORT_SHOT` crops round down to match (`port_aurora_sdk.cpp`: 1698 * 448 / 528 = 1440.6 -> 1440). Exact scales (1056, 896, ...) are unchanged.
- **Bath ripple (Bowser, `60,0`):** walked around the rim for 1500 frames; the bath water surface never came into view clearly (Bowser fills the tub, Mario is burned and the run ends with the game-over wipe). The ripple/height-map copies run every frame; screenshot still missing.



## E21. Review of w6's R17 (internal resolution), R18 (MSAA), R20 (widescreen) against E19
- **R17 `aurora_copy_scale.patch`** (rounded copy rectangle, exact 1:1 copies): right; a 1:1 copy must never resample.
- **R17 changes in `port_efb.cpp`** (marked `w6 (round 5)`): accepted as they are. A 1:1 CPU-read copy samples each GameCube pixel at its centre (`taps = 1`), which is what the copy unit does; the pixel counter counts on the 640x528 logical grid, one sample per GameCube pixel centre, so the count is the same at every scale and target shape (`toLogical = 1`). Down-scaling copies keep the box filter.
- Re-checked after E20's rounding change (EFB target rounded up): airstrip counter 4548 and readback 4548 texels at internal scale 3.2143 (1440 rows) and 2.4107 (1080 rows), as in R17.
- **R18 MSAA:** copies, snapshots and peeks use the resolved target (Aurora's `resolve_pass` returns single-sample snapshots); depth copies use sample 0 (`aurora_msaa_depth.patch`). Matches E19; the remaining counter difference at partly covered edge pixels is documented in R17 (about 2 % while dirty, 0 when clean).
- **R20 widescreen:** the logical EFB stays 640x528 and maps linearly (anamorphic, per-axis scale, no offset) to the wide target, and the 3D view is widened through the game's projection, not through extra logical columns. That keeps every assumption of `port_efb.cpp` / `port_aurora_sdk.cpp` (no offset to export) and the 4:3 UVs of the screen-texture and mirror copies. Agreed. Presentation of the new aspect modes still to be captured with `SMS_PORT_SHOT_PRESENT` once w11's camera hook lands.



## E22. Shine-get save prompt on a flat grey background (w12): original behaviour
- After a shine, `TMarDirector` (MarDirectorDirect.cpp, `DIRECTOR_FLAG_SHINE_GET_PENDING`) starts the shine fader (`unkDC->registFadeout(120, 640)`: 5.33 s of the shine demo, then a 1 s fade) and sets `DIRECTOR_FLAG_CARD_SAVE_PENDING`; the card save opens once the director is back in its normal state. The shine fader (`TShineFader`, GC2D/ShineFader.cpp) ends at `FADE_STATUS_FULLY_FADED_OUT`, alpha 0xFF, in the colour `MarDirectorSetup2.cpp` gives it: `TColor(0xD2, 0xD2, 0xD2, 0xFF)`.
- So the save prompt is drawn over an opaque 210/210/210 screen on the GameCube too; no EFB copy is involved (`TCardSave::execIssueGX_` draws only its 2D layout). Measured on the port (EX course `21,0`, `SMS_PORT_DEBUG_CALL=tpshine@1700`): background (210, 210, 210), the shine demo before it renders the stage normally. Not changed.



## E23. EFB readbacks under GPU load; `--nullgfx` (w12)
- **Buffers pooled:** each readback used to create three buffers (map-read readback, compute output, uniform) and a bind group; about 7 jobs per frame in Bianco. The three buffers now come from a pool (keyed by size) and go back to it when the readback has been mapped (the GPU is done with them); the uniform is written with `queue.WriteBuffer`, the output cleared with `ClearBuffer`. Steady state creates no buffers.
- **No pile-up:** at most 4 requests per result (copy destination, peek, counter) and 256 in total are in flight. A result that already has 4 outstanding is not requested again, so there is no snapshot, no pass break and no job; requests fall to the GPU's pace, and the newest finished readback is stored. (Results normally arrive 1-3 frames late, so the cap does not bite on an idle GPU.) Requests whose frame never got submitted are forgotten after 60 game frames.
- Checked: Bianco alone (readbacks and counter as before, 1-2 frames late, counter = readback texels); 4 Bianco instances in parallel with other agents' games on the GPU: all four at full rate (600 frames per 600 retraces after loading), readbacks still 2 frames late.
- **`--nullgfx`:** Bianco and Ricco with spraying run 4800+ frames on the Null backend with the current build (E17's skip covers `port_efb.cpp` snapshots/jobs and `port_aurora_sdk.cpp` peeks/screenshots; Aurora's own copies, including w6's copy-scale path, allocate textures only, which the Null device handles). The "Out of memory" in w12's runs is most likely from a build before E17; if it still happens with today's main, please send the log tail.


## E24. w9's frame-rate interpolation and the EFB: review and a 30 / 60 / unlimited comparison
- **Code review (port_interp.cpp, MarDirectorDirect.cpp `port_mardirector_redraw`, port_main.cpp `displayFrame`, port_efb.cpp, port_aurora_sdk.cpp `GXSetDrawSync`):**
  - Interpolated frames run with `port_efb_set_presentation_only(1)`: copies are GPU-only, nothing is queued, no destination ages, and `port_efb_end_frame` is only called for an Aurora frame that holds a logic draw (after the flag is cleared again). Game memory therefore only changes at the end of game frames.
  - Draw-sync callbacks are not called in interpolated frames, so `GXPeekZ`, `GXPeekARGB` and `GXReadPixMetric` are never reached there. The game's `TDrawSyncManager` still pushes its breakpoints, so its breakpoint ring gets unbalanced. That is harmless: `GXEnableBreakPt` / `GXDisableBreakPt` do nothing on the port.
  - The redraw skips `unk38` / `unk3C`: the pollution layers, the graffito check and the bath-water preprocessor (`initECTGft` puts `TBathWaterPreprocessor` in `unk3C` when the stage has no pollution joint models). The bath water's `render` copies (alpha, depth 0x27, mist) are per-frame copies of the current EFB and are fine to repeat. The water manager has no copy of its own.
  - `port_efb_clear_all` leaves Z mode, colour update and alpha update set. That is harmless: the stage draw sets them first thing (`JDrama::TEfbSetting`, `TEfbCtrl`).
- **Interplay with E23 (pooling, per-key caps):** `[debug] record` defaults to 1, so `port_replay_deterministic()` is true in normal play and w9's fixed-latency path is the normal path. Every result reaches the game exactly 3 game frames after its request. `keyBusy` never coalesces; instead `detEndFrame` waits for a late GPU result (up to 1 s). The per-key cap of 4 only applies with `record = 0`. Pooling works the same on both paths. The clock is game frames on both paths, so interpolated frames add no latency.
- **New test knob:** `SMS_PORT_EFB_TRACE=1` (port_efb.cpp) logs one line per counter value the game reads and one per copy readback stored into game memory (with the game frame, the request frame, non-zero texels and a hash of the stored texture). It also logs each destination copied in an interpolated frame and any read attempted in one.
- **Run:** airstrip `warp 0,0` with a PADSCRIPT that walks Mario into the goop and sprays (`+0-900/45:A,+900-1500:G-2750;1480,+1500-2700:R+S90,+2700-3300:G-1500;1500,+3300-4500:R+S270`). About 9000 frames each, 4 instances at once, at `SMS_PORT_FPS` 30, 30, 60 and unlimited (60 fps: 5000 interpolated frames; unlimited: 44000).
  - Counter values seen by the game (0x7e layer, 0x92 objects; 9300 reads) and the stored 512x512 R8 maps (4600 stores, about 1500 distinct map states as Mario sinks, dies and respawns) are identical between 30 / 60 / unlimited, read for read and store for store, up to the point where PADSCRIPT input lands one game frame apart. (PADSCRIPT counts retraces; a press that lands on the other retrace parity reaches the game a frame later. Two 30 fps runs differ the same way.) Compared as a sequence of map states: 30 vs unlimited are identical over all 1509 states; 30 vs 60 match over 1153 states, until the 60 fps run's walk input landed on retrace 10930 instead of 10929.
  - Store latency: always 3 game frames. No "late" message, even at unlimited with 4 instances on the GPU.
  - Reads attempted in interpolated frames: 0. Copies made in interpolated frames: only the screen texture (640x448 -> 320x224 RGB565), a 256x256 RGB5A3 copy with clear and a 48x48 RGB565 copy at the screen centre, all GPU-only. The pollution maps are never copied in an interpolated frame.
  - With `SMS_PORT_RECORD=0` (the E23 path): no reads in interpolated frames either. Latency is 1-2 game frames at 30 fps and 1 at unlimited, because finished readbacks are picked up at Aurora frame ends and unlimited has more of them per game frame. Each result is still stored only at the end of a game frame, so a GPU result never reaches the game in the frame that requested it.
- Verdict: no change needed in port_efb.cpp for interpolation.


## E25. Review of w6's MSAA sample-0 path (`aurora_msaa_depth.patch`, `snapshot(..., sample0)`)
- Counters (`GXReadPixMetric`) and CPU-read copies take sample 0 of each pixel, which is the GameCube's single sample per pixel; depth peeks and depth copies use sample 0 too. That matches E19.
  - Measured: airstrip at 21:9 with MSAA 4x and 60 fps. Layer counter 5601 -> 4548, as without MSAA (E27 table).
- **Changed (port_aurora_sdk.cpp):** `GXPeekARGB` now also reads sample 0 (`colorSample0 = !screenshot`). Mario's occlusion probe tests `alpha == 0x10` exactly, and a resolved edge pixel would average that away. Screenshots keep the resolve. Without MSAA this is a no-op (Aurora ignores the flag at 1 sample).
- **Changed (port_efb.cpp):** the shared per-callback snapshot (`sSnap`, used by the `GXPeekZ` series) is never a sample-0 snapshot (`shareable && !sample0`), so a later caller cannot get the wrong kind. No current caller combines the two.
- **For w6 (Aurora side, `tex_copy_conv.cpp snapshot_color_sample0`):**
  1. When the compute pipeline could not be created, or the format is not RGBA8/BGRA8, the function returns without writing the snapshot. The pooled snapshot texture then keeps whatever an earlier use left in it, and counters and pollution maps would read stale data silently. Falling back to the resolved copy (the `else` branch in `encoding.cpp`) would keep them correct.
  2. For an sRGB scene format it packs `textureLoad` values (linear) as raw bytes into an sRGB texture. This is harmless today, because the EFB is non-sRGB (`to_linear`), but such formats should be encoded or rejected.
  3. It creates a uniform buffer and a bind group per snapshot (several per frame). This is a performance point only; they could be reused as in E23.
  4. Reusing the one `g_colorSample0Buffer` for several snapshots in a frame is correct: WebGPU orders the compute writes and buffer-to-texture copies within the submission.
- MSAA runs here had no sample-0 or resolve warnings. The 21:9 MSAA run logged R18's transient `E_OUTOFMEMORY` for two pipelines, with 4 instances on the GPU.


## E26. Bath water at the Corona Bowser fight (`warp 60,0`, `SMS_PORT_WARP_FLAGS=story9`)
- Reached: `PADSCRIPT=+200-330:S0` walks Mario off the rim into the tub; the bath water throws him and the camera looks down at the water around Bowser.
  - `port/build_aurora/e24/corona_bath_43_present_880.png` (window) and `..._crop.png` (full resolution): the translucent water with the floor pattern refracted through it, the mist layer (`draw_mist`, RGB565 mip copy) and Bowser's legs behind the mist. No missing or black copy.
- Also at 21:9: `corona_219_present_1300.png` (rim view; the tub interior is not visible from the rim).
- The ripple simulation is CPU-side (`calcWaterModel` on `CUE_MOVE`), and the height/depth copies are made from the current frame (E24). Nothing here depends on readback timing.


## E27. Widescreen (w11's Hor+): screen-texture, mirror and heat-haze copies; pixel counters stay on the GC grid
- **Code:** w11 widens the camera through `TLookAtCamera::mAspect` (`PORTWideCameraAspect`, cameragc.cpp). Everything that projects into a screen-space copy reads that same `gpCamera->getAspect()`: the texgen effect matrix `SMS_GetLightPerspectiveForEffectMtx` (heat haze `TShimmer`, bath water, Boos, the eel, NPC parts), the FLUDD water's `C_MTXLightPerspective` (ModelWaterManager.cpp 1838) and the mirror camera and its lookup (MapMirror.cpp 47, 61, 176).
  - The copies still take the logical 640x448 rectangle (or the mirror's texture size), which maps anamorphically onto the wide target (R20). The copied image therefore holds the widened view in 640 logical columns, and the UVs computed with the widened aspect point at the same place.
  - Logical EFB, copy rectangles, peeks and counters are unchanged (E19, E21).
- **Shots (`SMS_PORT_SHOT_PRESENT=1`, off-screen windows 960x720 4:3, 1280x720 16:9, 1680x720 21:9), in `port/build_aurora/e24/`:**
  - Airstrip: `air_43/169/219_present_6100.png`, and `air_219_msaa60_present_6400.png` (MSAA 4x at 60 fps). The view is wider and the HUD stays 4:3.
  - Bianco: `bianco_43/219_present_1300.png`.
  - FLUDD stream at 21:9: `bianco_219_spray_present_1300.png`, with a full-resolution crop in `bianco_219_spray_stream_crop.png`. The path and grass seen through the stream join the surroundings with only the small warp, and there is no shifted copy of the scene inside the stream. A sliding UV would show one.
  - Heat haze: TShimmer is in the airstrip, Bianco, Ricco, plaza and other scenes, but it is drawn only while Mario has lost his cap (`isWearingCap`). None of these runs reached that state. It shares the effect matrix with the bath water and the FLUDD stream, which line up.
  - The mirror copy (`鏡描画ステージ`, every scene has a `MirrorCamera`) runs in all of these runs without errors; no scene with a visible mirror was captured.
- **Pixel counters:** they count on the logical `efb_size()` grid, one sample per GameCube pixel centre (R17), so nothing outside the GC 640x528 frame can be counted, whatever the target's shape. Airstrip goop script (E24) per aspect:

  | window | aspect | 0x7e (layer) first / min | 0x92 (objects) first / min |
  |---|---|---|---|
  | 640x480 (default) | 4:3 | 5601 / 4548 | 3677 / 2865 |
  | 960x720 | 4:3 | 5601 / 4548 | 3678 / 2824 |
  | 1280x720 | 16:9 | 5601 / 4548 | 3665 / 2851 |
  | 1680x720 | 21:9 | 5601 / 4548 | 3688 / 2894 |
  | 1680x720, MSAA 4x, 60 fps | 21:9 | 5601 / 4548 | 3683 / 2887 |

  The layer counter is identical. The object counter's first reading varies by +-0.3 % (edge pixels, the same spread R17 found between internal scales). The minima differ more because the runs diverge later (PADSCRIPT timing as in E24; the wider view may also change what the game culls).
  If the objects were counted through the widened camera, 16:9 and 21:9 would differ by tens of percent. The object pass uses its own matrices (`initCountObjDegree`), not the camera.


## E28. Corona Bowser: flat bath water and missing rubber ducks (user report). Not the frame rate; three port bugs
- **Symptom:** the bath water was a flat, misty sheet over the tub floor, and Peach's and Bowser Jr.'s rubber ducks were nowhere to be seen. It is the same at 30 and 120 fps (`port/build_aurora/e28/before_30fps_present_760.png`, `before_120fps_present_760.png`), so w9's interpolation is not involved. E26's "no missing copy" was wrong: I took that flat sheet for the real water.
- **How the water works:** `TBathWaterMeshRenderer::prerender` (bath preprocessor) draws the simulated drops (`TBathWater` drops, 160 of them, radius 700) as balls with an orthographic camera looking down at the tub, into the top-left 84x84 of the EFB, depth only. It then copies that depth as `GX_TF_Z24X8` into RAM (`unk800A4`). `render` calls `makeHeightMap`, which decodes the copy on the CPU into the 84x84 height field of the water mesh. `getWaterHeight`, which positions the ducks (`TBathtubBinder`), reads that height field too. Three things broke it:
  1. **The balls were never drawn (decomp bug).** `JGRotation3.hpp setLookDir` builds the camera's y axis as `cross(x, z)`, but the target computes `cross(z, x)` (this unit's asm in `setLookDir`: `f28 = z.y*x.z - z.z*x.y`; decomp-diff shows the opposite operand order). That gives a mirrored basis: with `GX_CULL_BACK` every ball face was culled.
     - Proof: a debug capture of the prerender with colour on was black. Cull off showed the balls (`e28/debug_prerender_balls_cull_none.png`); depth test off changed nothing.
     - Fix in game code (`src/Map/BathWaterManager.cpp`, `TBathWaterMeshRenderer::tmpFake`, under `SMS_PORT`): the same maths with the original cross order.
     - The shared header is not edited. Editing JSystem was refused here, and `setLookDir` has no other user. **For w13 / the JSystem owner:** `fVar1076.cross(fVar958, fVar432)` should be `cross(fVar432, fVar958)` (the unit is NonMatching; the GameCube DOL does not change).
  2. **The depth copy was never read back (my bug).** `port_efb.cpp` read back only `GX_CTF_R8` copies by default and skipped every depth format. E12 even listed the bath copies as GPU-only, which was wrong. Depth copies `GX_TF_Z8/Z16/Z24X8` are now read back:
     - a compute pass reads the R32Float depth snapshot (sample 0 with MSAA) at each GameCube pixel centre and gives z24, with reversed Z handled;
     - `encodeTexture` stores it as the GX texture: Z24X8 as the RGBA8-style (X, Z hi) / (Z mid, Z lo) planes, Z16 big-endian, Z8 the high byte.
     The `SMS_PORT_EFB_TRACE` store line now prints the z range of depth stores.
  3. **Byte order (port gap).** `makeHeightMap` reads the copy's u16 planes natively. The texture is in GameCube (big-endian) order, so under `SMS_PORT` it reads them with `sms_be16`.
- **Result** (`port/build_aurora/e28/`):
  - The height field changes every game frame: z `b59266..ffffff`; 898 stores, 898 distinct, where before it was a single all-`ffffff` map.
  - The tub is full of green, wavy water around Bowser, with Peach's duck on the left and Bowser Jr.'s duck (with his brush) on the right, riding the surface: `after_30fps_present_760.png`, `after_120fps_present_760.png`, `after_30fps_present_600.png` (rim view).
  - The 30 and 120 fps height fields are identical store for store at the same game frames.
  - At 120 fps, consecutive presented frames (`SMS_PORT_SHOT_DISPLAY`, `after_120fps_display_frame0/1.png`) show no flicker. The water mesh is redrawn from the game frame's height field: the preprocessor is skipped in interpolated frames, and `render` only reads the stored map.
- **semdiff** on the unit (`tools/semdiff.py Map/BathWaterManager`): `prerender`'s camera set-up matches the target. The eye at `getThing().y + 3R`, the direction down to `getThing().y - R`, the up vector `(0, 0, @3419)` and the GX call sequence all agree; the remaining differences there are operand order. The `perform` / `throwMario` / `initializeIfYet_` / `calcBathtub` items are the ones w11/w13 already explained (the merged 0.9/10 literal, matrix offsets).
- GameCube `mario.dol: OK`; `smoke_all.py --quick` 20/20 (silent, off-screen).








## R22. Widescreen with w11's game side: movies pillarboxed; title, plaza, boss and pause checked at 16:9 and 21:9
- **THP movies** were stretched to the wide picture. `THPPlayerDrawCurrentFrame` (`sdk_thp.cpp`, marked w6) now squeezes the movie quad by 1/scale around the centre of the 640-wide frame (`PORTWideSqueezeRectX`), for both the movie director's full-frame ortho and `gxYuv2RgbSetup`'s; the rest of the frame stays the black clear. Exact no-op at 4:3. Subtitles are J2D and already squeezed (w11).
  Checked: plaza court movie at 16:9 (`shots/movie169_shot_1400.png`: picture x 160-1120 of 1280 = 960 x 720 = 4:3) and the intro movie at 21:9 (`shots/ws219_title_shot_900.png`: 720 x 540).
- **Shots with main** (MSAA 4x, AF 16x, FXAA on unless noted; window resolution, EFB readback = what the window shows, since the target has the display's shape):
  - Title: `ws219_title3_shot_2100.png` (background full width, logo round).
  - Plaza: `ws169_plaza_shot_2000.png`, `ws219_plaza_shot_2000.png` (warp 1,8 + `story8r`; wider view, HUD 4:3).
  - Boss: `ws169_boss_shot_1900.png`, `ws219_boss_shot_1900.png` (Corona Bowser; bath water copies fine).
  - Pause: `ws169_pause_shot_2600.png`, `ws219_pause_shot_2600.png` (dim background full width, menu 4:3).
- **Issue for w11 (game side):** the plaza news ticker's text runs past its bar in widescreen (`ws169_plaza_shot_2000.png` vs 4:3 `ws43_plaza_shot_2000.png`, where it is clipped to the bar). `TGCConsole2` clips it with `graphics->setScissor(TRect(unk544...))` (`GCConsole2.cpp` around line 4628), a raw `GXSetScissor` in 4:3 HUD coordinates that `J2DGrafContext`'s squeeze does not see. Squeezing that rect (`PORTWideSqueezeRectXi`) should fix it. The other raw `TGraphics::setScissor` users in GC2D (`ConsoleStr.cpp` 276, `CardLoad.cpp` 844, `Menu.cpp` 15) may need the same.
- The window's own presented image (w8's `SMS_PORT_SHOT_PRESENT`) is not in main yet; recheck with it when it lands. w9's frame-rate merge (vsync) is not in main yet either; check the menu after it lands. (Both done in R23 and R26.)


## R23. Presented window checked at 4:3 / 16:9 / 21:9 with every option; EFB floor at native; 1-pixel edge column
- **How:** `SMS_PORT_SHOT_PRESENT=1` + `SMS_PORT_OFFSCREEN=1` (w8's capture of the swapchain), Bianco `warp 2,0` frame 1500, settings files per run; content box and the last rows measured on each `present_*.bmp`. New test knob `SMS_PORT_NO_ASPECT_LOCK=1` (port_window.cpp, w6 line): the window keeps its configured shape, so a fixed aspect shows its bars as in fullscreen/maximized without covering the desktop. Runs in w6's `port/build_aurora/t/<run>/`.

  | run | window | aspect | internal | extras | presented content | EFB target |
  |---|---|---|---|---|---|---|
  | `p43_on169_2x` | 1600x900 (no lock = fullscreen stand-in) | 4:3 | 2x | MSAA 4x, FXAA, AF 16x | 1200x900, bars 200 + 200 | 1195x896 |
  | `p43_2x_ext` | 1200x900 | 4:3 | 2x | MSAA, FXAA, AF | fills | 1280x960 |
  | `p169_win_ext` | 1600x900 | 16:9 | window | MSAA, FXAA, AF | fills | 1600x900 |
  | `p169_1x` / `p169_1x_snap` | 1600x900 | 16:9 | 1x | off | 1599x900 + black column -> fills after (b) below | 796x448 |
  | `p169_on219_win` | 1920x810 (no lock) | 16:9 | window | FXAA | 1440x810, bars 240 + 240 | 1440x810 |
  | `p219_1x` | 1920x810 | 21:9 | 1x | off | fills | 1062x448 |
  | `p219_2x_ext` | 1920x810 | 21:9 | 2x | MSAA, FXAA, AF | fills | 2124x896 |
  | `p219_small_floor` | 640x480 (no lock) | 21:9 | window | off | 640x270, letterboxed | 1062x448 (floor, (a)) |
  | `vk_169_2x_ext` | 1600x900 | 16:9 | 2x | MSAA, FXAA, AF, **Vulkan** | fills | 1593x896 |
  | `movie169` | 1600x900 | 16:9 | window | off | intro movie 1200x864 pillarboxed (4:3), R22 | |

  No band or dark last row in any of them (last presented rows continuous with the rows above). HUD 4:3 and centred in the widescreen shots.
- **(a) EFB floor (Aurora `aurora_efb_floor.patch`, `window.cpp get_window_size`):** below native resolution the CPU-read EFB copies stop matching the GameCube: airstrip (`warp 0,0`, `SMS_PORT_COPY_LOG`) pollution map read back with 4532 non-zero texels at `internal_scale = 0.5` and 4551 at 0.75 instead of 4548 (several GameCube pixels share one target pixel, so the map's draw-copy-redraw cycle runs on a coarser grid). The frame buffer is now never below the render mode (640x448) in either axis; a smaller one is scaled up uniformly. That also covers a widescreen picture in a small window (21:9 in 640x480 was a 640x270 target). With the floor, 0.5 reads 4548 again (`air_s0p5_floor`). So there is **no below-native internal resolution**: it cannot keep the readbacks exact (task skipped as asked; the performance preset uses native 1x instead, R25).
- **(b) Present viewport (`aurora_efb_floor.patch`, `gpu.cpp calculate_present_viewport`):** a 16:9 picture at 1x is a 796x448 target (796.4 rounded), so the present viewport kept the target's own shape and left the right-most column of a 1600x900 window black. When the gap is only that rounding (at most half a target pixel + 1), the viewport now fills the surface. Real bars (4:3 on 16:9: 400 px) are untouched.
- Defaults: unaffected by both (window-sized target, no gap). Default title EFB and presented shots at frame 900 are pixel-identical between main's exe and this build (`old_title` vs `d12_title`; frame 900 is also identical run to run).
- Internal resolution menu: an ini value not in the list (e.g. 2.41) now shows as "Custom (2.41x)" instead of "Window resolution"; tooltip says it never goes below 640x448.
- D3D12 with MSAA 4x logged `E_OUTOFMEMORY` for one or two GX pipelines (5 retries, draws skipped) in 3 of 4 MSAA runs while 4 game instances shared the GPU (R18's transient). The Vulkan MSAA runs had none (R24).


## R24. Graphics API option: Direct3D 12 (default) / Vulkan / Direct3D 11, with fallback
- Aurora's prebuilt Dawn (`v20260807.225922`) has D3D12, Vulkan, D3D11 and Null compiled in (`DAWN_ENABLE_BACKEND_*` in `port_deps/aurora/build`); Aurora already tries `desiredBackend` first and then D3D12, Vulkan, D3D11, Null when one cannot create its adapter/device.
- **New `port/src/port_gfx_backend.cpp` (w6):** `[graphics] backend = d3d12 | vulkan | d3d11` (default d3d12 = what Aurora picked before), F1 Display "Graphics API" (next start) with a status line ("Running on Vulkan", or "... (Direct3D 12 could not start)" when Aurora fell back). **Start guard** for crashes and hangs while starting: right before `aurora_initialize` (`port_gfx_backend_arm()`, one line in port_main.cpp) `<settings file>.gpustart` records the API and the process id; it is removed after 120 presented frames (`port_gfx_backend_frame()`, one line in `port_window_frame`) or at a normal exit. If the next start finds it naming the configured API and that process is gone, the setting switches (D3D12 -> Vulkan, others -> D3D12), is saved, and the menu says why. A guard of a copy that is still running is left alone. `SMS_PORT_BACKEND=d3d12|vulkan|d3d11` overrides for one run (no guard); `SMS_PORT_NULLGFX` unchanged.
  **w9:** small marked additions in your files: `PortBackend` enum, `PortSettings::backend`, the `[graphics] backend` ini line and parse, the `port_gfx_backend_configure()` call in `port_settings_configure`, the menu combo/status line, and `port_gfx_backend_frame()` in `windowFrame`. **w7:** `port_gfx_backend_arm()` before `aurora_initialize` in port_main.cpp.
- **Checked on the RX 9070 XT (driver 32.0.31041.1004):**
  - Vulkan: title (`vk_title`), airstrip (`vk_air`: pollution readback 4548 texels, as D3D12), Bianco (`vk_bianco`), Sirena with MSAA/FXAA/AF/2x (`vk_sirena_msaa`), Corona Bowser with MSAA (`vk_corona`, `vk_corona2`); present capture works. No Aurora errors, no `E_OUTOFMEMORY`. Vulkan reached 4200 frames in 80 s where D3D12 reached 3000 in the same scenes (stage loads finish sooner: pipeline creation is cheaper).
  - D3D11: title (`d11_title`) runs, present capture works.
  - Title frame 900 EFB: D3D12, Vulkan and D3D11 pixel-identical.
  - Start guard (`guard.py` in the scratchpad): a D3D12 start killed after 0.9 s left the guard; the next start logged "Direct3D 12 did not finish starting last time ... switching to Vulkan", ran Vulkan, saved `backend = vulkan` and removed the guard after 120 frames. A guard naming a live process was ignored.
  - F1 menu with Vulkan: `menu_vk/present_1500.png`.
- Whether Vulkan also avoids the AMD D3D12 driver crash under heavy load (R14/R18 class) cannot be proven by a few runs, but it is the obvious thing to try when D3D12 crashes on a player's AMD card, and it had no pipeline `E_OUTOFMEMORY` in the runs where D3D12 had them.


## R25. Performance preset (weak PCs)
- F1 Display "Performance preset (weak PCs)" button: internal resolution 1x (native 640x448 scene), MSAA off, FXAA off, anisotropy as the game asks, trilinear off, frame rate 30 (no interpolated frames). Brightness, gamma, aspect, VSync and the graphics API are kept. Native is the cheapest setting that keeps the CPU-read copies GameCube-exact (R23 (a): nothing below it is offered).
- GPU 3D-engine busy time per game frame (Windows "GPU Engine\Running Time" of the process over 20 s, Bianco with the gameplay script, one run at a time, other agents' games on the GPU too, so roughly +-10 %): default 4:3 1280x960 1.25 ms; 1600x900 16:9 window resolution 1.32 ms; preset in 1600x900 16:9 0.88 ms (Vulkan 0.64 ms); 2x + MSAA + FXAA + AF 16x 2.04 ms (Vulkan 1.78 ms).
- "Reset graphics to GameCube defaults" now also turns VSync off (its default).
- Whole round (R23-R26) on main as of 2026-10-08 23:30: `smoke_all.py --quick --no-build` 20/20 PASS; GameCube DOL shasum OK.


## R26. Frame rate / VSync / Display tab coherence (after w9's merge)
- VSync sits under "Frame rate" (w9), with the note "(VSync caps the frame rate at the monitor's N Hz)" only when it does; Aurora uses FIFO-relaxed with VSync and mailbox/immediate without. The two "next start" options (MSAA, Graphics API) say so in place. The ini comment for vsync no longer contains "(w9)". No other changes needed.
- Present-capture shots of the menu at 1280x960: `menu_vk/present_1500.png` (Display tab: Fullscreen, Aspect, Internal resolution, Frame rate, VSync, Anisotropic filtering, Trilinear, MSAA, Graphics API + status, FXAA, Brightness, Gamma, Sharper screen effects, Reset / Performance preset, overlay).

# Multiplayer player interaction (P2, w8)
Design, wire format and settings: `port/MULTIPLAYER.md`, "Player interaction (P2, w8)". Files: `port/src/port_mp_interact.cpp`, `port/src/port_mp_interact_ui.cpp`. No game-source or w10/w15 file was edited.

## M1. Bump, stomp, ground pound, FLUDD knockback; host modes
- **How it works:** every client resolves only its own Mario.
  - Bumping is local: `TMario::keepDistance` against the other players' drawn positions, which brings in the game's clash knock-back in Full mode.
  - The attacker bounces with `TMario::trampleExec` on a proxy hit actor and sends a hit message (`PORT_MP_CH_INTERACT`, relayed by the host).
  - The victim knocks itself back with `TMario::damageExec`, damage 0.
  - Host options: `reserved[0]` (0 = Full, 1 = Solid only, 2 = Off) and `reserved[4]` (FLUDD pushes players). `reserved[3]` went to P5 meanwhile, so the FLUDD option uses [4]; the allocation is listed in MULTIPLAYER.md.
- **Tests:** `port/build_aurora/mp_interact_run.py` (scratch runner: 2-3 off-screen instances with per-instance env) on loopback, with `SMS_PORT_MP_INTERACT_TEST` teleports and PADSCRIPT.
  - 3 instances, Full mode:
    - Guest 1 ground-pounds the host from 400 above (PADSCRIPT `L`; Z opens the map). Host: `0c400201 -> 00020461` (full knock-down).
    - Guest 2 stomps the host: the guest does a trample bounce; host `-> 00020463` (short knock-down). The bounce lands it on the head again 40 frames later; the second hit leaves the host in the same knock-down.
    - Guest 2 overlapping the host is pushed out to 100 units (2 radii); the host does not move.
    - Guest 1 sprays at the host with guest 2 standing beside it: both get `FLUDD` every 30 logic frames (status `-> 00020464/65`, light stagger). Health stays 8 throughout.
  - 2 instances: `SMS_PORT_MP_INTERACT=solid`: no stomp, the landing player and the overlapping one are pushed apart, no FLUDD hits. `=off`: players overlap and nothing happens. `SMS_PORT_MP_FLUDD_PUSH=0`: spraying sends nothing, stomps still work.
  - Shots: `port/build_aurora/mp_interact_shots/inst*_present_*.png`. Instance 1 at frame 1540 is the pound from above; instance 0 at frame 1930 shows the host after the pound, with damage stars and camera shake. Earlier runs also showed the guest's trample bounce over the host and the host staggered in the guest's FLUDD stream.
- Single-player: the code only runs from w10's game-frame hook, which is only called in a session. The proxy hit actor is a static object (like `TModelWaterManager::mStaticHitActor`). `smoke_all.py --quick` 20/20.
- Not done:
  - Damage as a host option (the `damageExec` damage argument is the hook for it).
  - Stomp/pound detection against a player riding Yoshi uses the normal body size.
  - The victim does not send a confirmation back; the attacker's bounce is always local, as in sm64coopdx.

# Multiplayer chat and emotes (w6, task board P3)
Design, controls, wire format and knobs: `port/MULTIPLAYER.md` "Chat and emotes (P3, w6)". New files only: `port/src/port_mp_chat.cpp`, `port/src/port_mp_emote.cpp`, `port/tools/mp_chat_test.py`; no edits to w10/w15 files, no `port_mp.h` or protocol change.

## MP-C1. Chat
- T / `/` / right-stick click / lobby "Chat" button; input line or on-screen keyboard; reliable `PORT_MP_CH_CHAT`; fading log bottom-left of the picture (above the plaza news ticker); joins/leaves as system lines; 120-byte printable UTF-8 cap; 5 per 10 s sender limit, 8 per 10 s receiver limit; mute per player; host "Chat"/"Emotes" switches in `reserved[1]`; `/me`, `/help`, `/emotes`, `/<emote>`.
- Pads idle while typing through `port_mp_request_input`; the request is reference-counted with no reset at session end, so a watchdog thread releases ours if the session ends with the box open (otherwise single player would be left with idle pads).
- Found while testing: the lobby section's hint line made w10's panel scroll sideways (left edge cut off); now wrapped.

## MP-C2. Emotes
- Seven emotes from Mario's own animation table: Shine pose, Phew (hot-weather wait), Aww (missed-Shine slump), Sleep (the game's `MARIO_STATUS_SLEEPY` chain), Pianta pose (`changeMontemanWaitingAnim`), Yawn, Sit. D-pad = four quick ones, G / left-stick click = menu, chat commands. Rejected after looking: `ANIM_DEMO_GATE_OUT_APPEAR_GET` (Mario scaled to nothing). Mario's table has no wave or Pianta-festival dance.
- Driven through `MARIO_STATUS_NOMOTION` + `TMario::setAnimation` each frame, so w15's capture sends the body BCK/frame/hand as for any game animation: the host sees the guest's puppet in the Shine pose, hands on hips and sitting (`emote_showcase/puppet_emotes_host_view.png`, a strip of the host's presented frames). Local strip: `emote_showcase/local_emotes_sheet.png`.
- Pitfalls found: (1) the pad's `mTrigger` bits are already consumed when registered game-frame callbacks run (two app frames per logic frame), so D-pad and cancel buttons use edges of the held bits; (2) NOMOTION has `MARIO_STATUS_FLAG_UNK1000`, which refuses unforced `changePlayerStatus` (the first build left Mario stuck in NOMOTION): leaving uses force, and since damage knockback is unforced, a hit, the floor moving, a talk/demo, the stick or any action button ends the emote at once; (3) an idle Mario dozes off by himself, so emotes also start from SLEEPY/SLEEP (killing the zzz effect as `wakeup()` does).
- Only from a plain idle stance; refusals are logged and shown as a system line.

## MP-C3. Tests
- `python port/tools/mp_chat_test.py -n 3`: host + 2 guests, 1280x960 off-screen, present captures. Checks: every instance got every other's chat line and emote line, the flood was cut (5 of 6 lines; "Slow down"), "chat off" refused later messages, each instance played its emotes (keyboard-pad D-pad on the host, `/phew`, the emote knob), no crash. ALL PASSED (106 s). Shots: input line (`mp_chat_test/0/present_1800.png`), on-screen keyboard (`1/present_1800.png`), emote menu (`2/present_1800.png`), lobby section (`0/present_3300.png`).
- `mp_test.py -n 2` (w10's checks) passes with these files linked. Final reruns of both silent and hidden (`SMS_PORT_AUDIO=0`, `SMS_PORT_OFFSCREEN=1`; `mp_chat_test.py` sets both itself): ALL PASSED.
- Single player unaffected (the callbacks are registered but only called during a session): `smoke_all.py --quick --no-build` 20/20 PASS; GameCube DOL shasum OK (no shared source touched).

## MP-C4. Speech bubbles hook (added by w15, for w6)
- `port_mp_chat.cpp` keeps each slot's latest chat line or emote (`Said sSaid[]`, under `sMu`, set by `noteSaid` in `sendChat` (own lines), `receive` (chat and emotes) and `port_mp_chat_note_emote`; cleared in `resetSession`) and exports `port_mp_chat_last_said(slot, text, cap, &emote, &atMs)`. `port_mp_emote.cpp`'s `announce` calls `port_mp_chat_note_emote(id)` for the local player's own emote. All lines marked `w15`. w15's `port_mp_overlay.cpp` draws the bubbles (see "Multiplayer puppets (w15)", P9); the chat log itself is unchanged.

# Multiplayer puppets (w15)
Remote players drawn in game: snapshot capture of the local Mario, puppet Marios posed from snapshots, name tags, and a ghost harness that needs no network. Design and the w10/w15 contract: `port/MULTIPLAYER.md` (section "Puppets (w15)"). Files: `port/src/port_mp_puppet.{h,cpp}`, `port/src/port_mp_overlay.cpp`.

## P1. Capture (read-only)
- `port_mp_capture()` fills a `PortMpPuppetState` from `gpMarioOriginal`: position, velocity, face angles, status/arg/flags, the body base matrix (`calcBaseMtx` result, so swim bobbing, slope tilt, surf roll and poles come along), and the animation exactly as `M3UModelMario` holds it: both slots' BCK indices (new/old), blend-calc index, the three frame controls, both motion blend ratios (run/walk mix), the eye BTP, the hand model in use, plus the head/waist rotations `MarioHeadCtrl`/`MarioWaistCtrl` would add (read-only replicas of their choice). FLUDD: nozzle, pressure, water, body/nozzle BCK + frame, first emitter position/direction. Yoshi: state, colour, BCK + frame. Cap/helmet/glasses/shirt flags, held object type, health.
- Hook: `port_mp_logic_frame()` in `TMarDirector::direct` (last simulation tick, before the entry list, `SMS_PORT`), once per entered logic frame (30 Hz). With multiplayer and ghosts off it only counts the frame.

## P2. Puppets
- One per remote player, built like `TMario::initModel` + `TEnemyMario::initModel`: its own `J3DModelData` of `ma_mdl1.bmd` (so hand/shirt shape flags and eye patterns are its own), the hand models, cap (`ma_cap1`/`ma_cap3`), an `M3UModelMario` with its own blend calcs and frame controls that shares the local Mario's BCK and BTP tables, and the FLUDD body/nozzle MActors; diver helmet, sunglasses and Yoshi (`yoshi_model.bmd` + hands, body colour from `TYoshi`'s table, base matrix = Mario's as in `TYoshi::calcAnim`) are loaded on first use. About 0x3AB00 bytes of the stage heap per puppet (`TApplication::mHeap`, freed with the stage; not built when it has under 0x180000 free).
- Posed by writing the snapshot into the `M3UModelMario` (slot BCKs via `changeMtxCalcSIAnmBQAnmTransform`, frame controls at rate 0, blend ratios) and the base matrix, then `perform(CUE_CALC_ANIM)`; head/waist through the puppet model data's own joint callbacks.
- Drawn by a view object pushed into the entry list after "コンダクター" (`MarDirectorPreEntry.cpp`, only when multiplayer or a ghost is on), i.e. the character draw buffers, lit like an MActor (light set 1, light id from the ground's shadow data / shadow cube). Calc + entry on logic frames; w9's interpolated redraws only repeat `CUE_CALC_VIEW` (matrices interpolated by `port_interp`). Circle shadow through `gpBindShadowManager->request`. Puppets farther than 9000 units from the camera are skipped.
- FLUDD spray: a stand-in (no `TModelWaterManager` water, which would hit enemies, clean goop and count as the local player's): droplets simulated per logic frame from the snapshot's emitter, drawn as soft camera-facing blobs from a J3D packet in the translucent buffer.
- Interpolation: per slot a buffer of snapshots, drawn 100 ms behind the newest (network: receive time on `port_mp_time_ms`; ghosts: the local logic clock). Position/matrix (quaternion slerp), frames (same clip, small step), blend ratios, head/waist angles are blended; teleports (a jump over 600 units in one logic frame -> `warpSeq`), stage changes (`stageSeq`) and gaps over 500 ms snap. Hidden when not in our stage+scenario, not visible (pipes, warps), or 3 s without data.
- Never touches game state: no hit actor, no collision, not in any manager, no sound, no rumble, no water, no pollution, no EFB readbacks. With nothing active the entry list is unchanged.

## P3. Name tags
- Head joint + 75 units projected with the current GX projection and viewport, drawn by Dear ImGui (`port_mp_overlay.cpp`, called from `port_window.cpp` before `port_menu_draw`) in the game picture's box (w6's display aspect), fading from 2500 to 5000 units, hidden behind the camera and 0.5 s after the last game frame (movies, loads).

## P4. Ghost harness (no network)
- `SMS_PORT_MP_GHOST=<file>`: records the local `PortMpPuppetState` every logic frame (`SMSGHST1`, u32 record size, records).
- `SMS_PORT_MP_GHOST_PLAY=<file>`: plays it back as puppet(s) when the local player gets control in the same stage+scenario, frame-locked to the local logic clock. `SMS_PORT_MP_GHOST_COPIES=n` (several puppets), `SMS_PORT_MP_GHOST_OFFSET=x,y,z` (per-copy offset, default 150,0,0), `SMS_PORT_MP_GHOST_WIRE=1` (every state goes through the 128-byte network form first, i.e. exactly what a remote machine rebuilds). `SMS_PORT_MP_PUPPETS=1` turns the view object on without a ghost.
- Debug only: `SMS_PORT_MP_DEBUG_YOSHI=<logic frame>` drops the local Mario onto the hatched Yoshi once per stage (with `SMS_PORT_DEBUG_CALL=hatch@...`), to record a ride.
- Checked (off-screen, `SMS_PORT_SHOT_PRESENT`): plaza (warp 1,0) and Bianco (2,0, two copies) running, jumping, flipping, hovering and spraying beside the local Mario with the same pad script; Ricco (3,7) riding a pink Yoshi; all through the wire form. Shots in `port/build_aurora/mp_shots/` (w15 worker).

## P5. Network (w10's session)
- While `port_mp_active()`: the local state is packed into w10's `PortMpSnapshot` (`port_mp_pack_wire`: w10's standard fields plus the w15 block in `animAux`, `nozzleAngle`, `pad0`, `extra[44]`; layout in MULTIPLAYER.md) and handed to `port_mp_set_local_snapshot` every logic frame (VALID clear outside a stage). Remote slots are pulled with `port_mp_get_player` / `port_mp_get_snapshots` (new ticks only, receive time for interpolation); a changed `serial` respawns the puppet; names come from the player table.
- w10's functions are bound with lld-link `/alternatename` fallbacks (no session), so this builds and the ghost harness runs with or without `port_mp_net.cpp` linked.
- Checked with w10's worker code linked into a test build (not merged by me): host + guest on loopback (`SMS_PORT_MP=host/join`, off-screen, isolated user dirs, `SMS_PORT_NO_PADS`), the guest walking away, jumping and spraying in front of the host's camera and the host standing in the guest's view, each with its name tag (`net_*.png`); w10's `mp_test.py -n 2 --no-testsnap` passes with the real capture.
- GameCube build after the shared edits: `build/GMSE01/mario.dol: OK`. Single-player run (warp 1,0, no multiplayer env): no `[mp]` output, no view object.

## P6. Colour palettes (`port_mp_palette.cpp`; groundwork for character customisation)
- Where Mario's colours live (`SMS_PORT_MP_DUMPMAT=1` prints every material's TEV setup and writes the textures as `tex<n>.bin` in the run folder): all 11 body materials run the same 5 TEV stages (pollution mask, `H_ma_new_main_s3tc`, dirt dummy, toon ramp `H_toon_2_4i`, lit colour); their TEV/konst colours are lighting and dirt parameters, not part colours. Cap, shirt, overalls, gloves, shoes, skin, hair and moustache are painted in one 256x256 CMPR atlas, `H_ma_new_main_s3tc`, which the hand and cap models copy too. So colours have to change in the texture; a TEV multiply per material would tint the whole body (one material spans several parts).
- Recolour: each 4x4 CMPR block's two RGB565 endpoints are classified by atlas region (overalls/shirt top left, hair+ear top right, nose+moustache, shoes, sleeve+arm, gloves, cap+M bottom row) and hue/saturation/value, then replaced by the palette colour scaled by the endpoint's brightness relative to the brightest endpoint of that part (keeps the painted shading). Blocks keep their CMPR mode: endpoints are swapped and the 2-bit indices remapped when the new order would flip it. The buttons, eyes, logo patch and anything unclassified stay as they are.
- Palette = w10's `PortMpCharacter` (synced in the player table): `colors[0..5]` = cap, shirt, overalls, gloves, shoes, skin; hair = the 3 bytes after them (`reserved[0..2]`, i.e. `colors[6]` if the struct grows to 7). (0,0,0) keeps a part; all zero = classic. 11 presets (`port_mp_palette_preset*`): Classic, Green (Luigi), Yellow (Wario), Purple (Waluigi), Fire, Shadow, Pink, Aqua, Orange, Gold, Black & white.
- Puppets: their body, hand and cap model data get their own copies of all texture headers (`portMpPaletteOwnHeaders`; before this the puppets' hands/caps wrote the same values into the file's shared headers, harmless only while nothing was recoloured) and a recoloured copy of the atlas (32 KB) in the stage heap, made before the display lists. A colour change recolours the puppet's own atlas copy in place (same cache drop), no rebuild.
- Local Mario: only if the player picked a palette (`port_mp_palette_set_local`, the character menu, or `SMS_PORT_PALETTE=<preset>`): mario.arc's atlas is recoloured in place from a pristine copy on the next logic frame after the palette changes, then Aurora's texture-object cache is dropped (`aurora::gx::clear_static_texture_cache`), so the locked display lists rebind it: the change shows at once, mid-stage too (P8). Classic = the GameCube data, untouched. Shadow Mario and mirror reflections share the local Mario's model data, so they follow the local palette.
- Ghost test knob: `SMS_PORT_MP_GHOST_PALETTE=<preset>[,<preset>...]` per ghost copy.
- In a session, a palette set through `port_mp_palette_set_local` / `SMS_PORT_PALETTE` is published with `port_mp_set_character` (w10's PLAYER_INFO), so the other players' puppets wear it; checked host Green + guest Yellow on loopback with main's netcode (`mp_shots/net_palette_*.png`). The character page (P4) should call `port_mp_palette_set_local` (presets: `port_mp_palette_preset_count/name/preset`).
- Checked (plaza, 3 ghosts through the wire form + local palette): Green, Yellow and Shadow ghosts beside a Fire-palette local Mario (`mp_shots/palette_plaza_1500.png`). Single-player run without the knobs: no `[mp]` output, data untouched.

## P7. Held objects on puppets
- The held object's actor type goes on the wire (puppet block bytes 41-43: low 16 bits + category bit index; the emitter offset at 38-40 is now s8 in 2-unit steps to make room). The receiver looks for a local actor of that type (conductor managers and alone actors), makes its own `J3DModel` of that actor's model data (once per type, up to 4 per puppet and stage) and draws it on the puppet's right hand (`TMario::getTakingMtx`) at the actor's scale. Visual only; the local copy of the object is not touched.
- Checked with a ghost whose recording was given a papaya (`ghost_held.py`), Ricco 3,7: the papaya copy in the puppet's hand (`mp_shots/held_papaya_1000.png`).

## P8. Character menu (task board P4, `port_mp_custom.cpp`)
- One ImGui editor, three ways in: **F4** anywhere (single-player too: file select or a stage), the lobby panel's **Character** section (`port_mp_register_lobby_section`, with a swatch of the applied palette), and `port_mp_custom_draw()` for w10's main-page "Character" stub (one call on w10's side, requested in MULTIPLAYER.md P4). No w10 file edited.
- Contents: the 11 presets and the saved palettes in one list, a front-view swatch diagram of Mario in the edited colours, one colour editor per part (cap, shirt, overalls, gloves, shoes, skin, hair and moustache; untick = keep the original), name + Save as / Delete, Apply / Revert / Close. Keyboard and controller navigation (ImGui nav on while open, like w9's menu; B/Esc closes); the game gets idle pads (`port_mp_request_input`).
- Live preview: every edit recolours the real Mario in the game at once (`port_mp_palette_preview`); Close without Apply puts the applied palette back. No separate rotating render (the game camera shows Mario next to the window, which sits on the right).
- Apply: `port_mp_palette_set_local` (+ `port_mp_set_character` in a session, so the others' puppets change, in place, within a few frames) and saves `port_characters.ini` (next to `port_settings.ini`, or `SMS_PORT_CHAR_INI`): `current=` plus `saved=<hex>:<name>` lines, 7 parts x RRGGBB, `000000` = original. The applied palette is loaded at start (`SMS_PORT_PALETTE` wins). Default (no file): Classic, nothing touched.
- Test knobs: `SMS_PORT_CHAR_OPEN=<frame>` opens the window; `SMS_PORT_CHAR_SCRIPT=<frame>:<preset>[,...]` picks and applies presets; F4 also via `SMS_PORT_KEYSCRIPT`.
- Checked (`SMS_PORT_SHOT_PRESENT`): single-player plaza, window open with Green previewed on Mario, then Yellow applied and the window closed, Mario yellow mid-stage (`mp_shots/char_menu_green_1500.png`, `char_applied_yellow_1720.png`); next run loads Yellow from the file. 2 instances on loopback: the guest applies Green then Purple, the host sees its puppet go red -> green -> purple without a rebuild (`mp_shots/net_char_*.png`); lobby panel section (`mp_shots/lobby_character_section.png`). Single-player without knobs: no `[mp]` output, no file written.

## P9. Chat speech bubbles (`port_mp_overlay.cpp`)
- `port_mp_overlay_get_tag(slot, &x, &y, &visible)` (port_mp_puppet.h): where a player's name tag is this frame, in window pixels; in a session the local Mario has an anchor too (no tag drawn for him). Tags now carry the player slot.
- Each player's latest chat line or emote (w6's `port_mp_chat_last_said`, MP-C4) shows as a white bubble above his name tag (above the head for our own lines): 5 s, fading over the last second, wrapped at 30% of the picture width, kept inside the picture with the tail still pointing at the player, fainter for far players; emotes get a gold star and the emote's name. The bottom-left chat log is untouched. Bound with an `/alternatename` fallback, so it links without w6's file.
- Checked silent and hidden (`SMS_PORT_AUDIO=0`, `SMS_PORT_OFFSCREEN=1`), host + guest on loopback with `SMS_PORT_MP_CHAT_AUTO` / `SMS_PORT_MP_EMOTE_AUTO`: the host's own bubble over his Mario (`mp_shots/bubble_host_own_1320.png`), both players' bubbles on the host (`bubble_host_both_1520.png`), the guest's view (`bubble_guest_1320.png`) and the guest's Shine-pose emote bubble with the star (`bubble_guest_emote_1520.png`).

## P13. Game modes None / Free, host "Start", in-game player list (`port_mp_modes.cpp`)
- `PortMpGameMode`: `PORT_MP_MODE_NONE` (0, the default of every new lobby) and `PORT_MP_MODE_FREE` (1); protocol 5. `gameMode` is the mode running now; `reserved[8]` counts the host's "Start" presses. Both reach late joiners with the settings.
- Lobby panel (`port_mp_menu.cpp`, host): "Running: ..." plus a mode list and a Start button (restart, switch, or back to None). Every client reacts to a new start: toast, then the plaza reload through the game's own path (next area + `DIRECTOR_FLAG_STAGE_TRANSITION_PENDING`), so everyone respawns at the plaza start. A late joiner takes the running mode without reloading. The create page no longer offers a mode.
- None closes every way out: `port_mp_stage_change_blocked` refuses all stages but the plaza (and the title) in `TMarDirector::setNextStage` (pipes, change-stage objects, event scripts), the warp floors (`TMario`'s BG check: skipped instead of hiding Mario) and the gate warp-in (`TMario::warpIn`: Mario steps back out); w11's level select is off in None. A throttled toast says the entrances are closed. Free: unchanged (the free-mode save opens everything).
- Player list: top right of the game picture, in the game's own font (`gpSystemFont` with `J2DPrint`, set up like `J2DTextBox::drawSelf`; `J2DPrint::print(fmt)` is an empty stub, so the `(x, y, fmt)` overload is used, and widths come from the glyph advances), each name in its cap colour with a dark copy behind it. A view object in the "GX Post" perform list (`MarDirectorSetupObjects.cpp`), drawn only in a session and in normal play. It draws over the whole picture with its own projection and sits at the HUD's right edge (w11's `PORTWideHudShift`, so it follows the HUD position setting in widescreen). Shots: `mp_shots/playerlist_4x3_host.png`, `playerlist_16x9_guest.png` (16:9, HUD at the edges).
- Checked: `SMS_PORT_MP_MODE=free@450` (host test knob) starts Free on both instances (toast, plaza reload); `mp_test.py -n 2` passes; GameCube `shasum -c` OK; `smoke_all --quick` 20/20.

## P14. NPC animation sync check; Shine demo double step fixed
- NPC animations (w12's `port_mp_npcs.cpp`), checked on 2 instances in the plaza with enemy sync on: the puppets show the owner's BCK index and rate for every logged NPC, frames free-run in step (resynced when > 4 frames apart), angles agree. Added: the face pattern (BTP) and texture scroll (BTK) indices travel in the state entry's spare bytes (`btp1`/`btk1`, index + 1, 0 = leave alone), so sprayed / talking expressions follow the owner. NPCs the owner runs for others no longer freeze out of his camera's sight (`port_mp_npc_keep_moving`, a hook on the sight cull in `TBaseNPC::perform`): the others saw a frozen pose. Motion blending between two BCKs and the parts animations stay local (short, cosmetic).
- Shine-get demo in a session (P12): the extra performers include the player group (`プレーヤーグループ`: the local Mario, his Yoshi and FLUDD), which the Shine list steps already, so Mario was stepped twice. It is skipped now; the log names every extra performer once. 16 extras remain (map, waves, goop, conductor, managers, water, sky, events, ...). `mp_objx_test.py --only coins100` PASS.

## P15. Game mode Hide and Seek (`port_mp_hideseek.cpp`)
- `PORT_MP_MODE_HIDE` (2) after Free, in the mode list, the vote list and the queue (`kModeShort`); channel `PORT_MP_CH_HIDE` (24). It uses the locked plaza of None (the early plaza scenario, the Shine-less flags, entrances sent back to the plaza; w11's level select off).
- Round (host-run, state to everyone reliably on change and once a second): once the host is back in the reloaded plaza (3 s after the start), the seeker is the host's pick (mode settings) or random; everyone else hides. Hide phase (default 30 s, at least 10): the seeker's screen is black (drawn first by the player list view object, over the whole picture) and his pads are idle (`port_mp_request_input`). Seek phase (default 3 min): the seeker's client sees its Mario overlap a hider's latest snapshot (touching or stomping; 100 units across, 160 up or down) and asks the host, which tags him: he becomes a seeker (host option, default on) or is out ("Found"). Seekers do not see hiders' name tags (`port_mp_hide_hide_tag` in the puppet tag pass). No hider left: the seekers win; time up: the hiders still hidden win. The result shows for 6 s, then everyone goes back to the lobby (`port_mp_modes_round_over`; AutoStart begins the next cycle if on).
- Top-right list: "Hide and Seek: hide! 0:23" / "seek 2:51 (n hiding)" / "seekers win", each player with his role. Host settings in the lobby panel under the mode: hide time, seek time, "Tagged players become seekers", seeker (Random or a player). The settings ride in `CtlState` (0 = default). Test knobs: `SMS_PORT_MP_MODE=hide@<frames>` (host), `SMS_PORT_MP_HIDE_TEST=1` (the seeker jumps onto a hider once).
- Checked (2 instances, silent, hidden): round start (seeker Luigi), the seeker's black screen with the list, seek phase, the test tag ("Luigi tagged Host"), "Everyone was found: the seekers win!" on both. Shot: `mp_shots/hideseek_host_3300.png`.
- Not done this round (sync list): NAME_KURI respawn (w11's enemy sync), fruit spawned during play, more water-reactive objects (w11's `port_mp_mapreact.cpp` covers map-object water hits), carried barrels/crates (puppets show a copy in the hands; throw/break is not shared).

## Not done / next
- Held objects use the generic hand matrix (objects that position themselves differently in Mario's hands may look offset); no copy when no local actor of that type exists in the stage.
- Foot IK callbacks, the FLUDD/cap sway (`TMultiMtxEffect`), Yoshi's tongue and juice blink, dirt and damage fog are not reproduced.
- Palette: the classification is tuned on the US atlas only.
- Player interaction (coopdx-style bumping/stomping) is out of scope here (puppets have no collision).


# Multiplayer (w10)

sm64coopdx-style multiplayer: design, protocol and the w10/w15 interface are in `port/MULTIPLAYER.md`; the shared API is `port/include/port_mp.h`.

## MP1. Transport, sessions, LAN/direct join (`port_mp_net.cpp`)
- Own UDP layer over Winsock (`ws2_32`, linked with `#pragma comment(lib)`), one socket and one network thread per process, so keepalives and timeouts keep running while the game thread loads a stage.
- Host-authoritative: the host owns the player table and session settings and relays snapshots (full rate to players in the same stage+scenario, 2 Hz to the rest) and reliable messages.
- 16-byte header (magic `SMP1`, protocol version, type, slot, connection id, sequence, length); anything malformed, of another version, or from an address that does not own the connection id is dropped.
- Handshake `CONNECT`/`ACCEPT`/`REJECT` with a protocol version check (a mismatched client is told why), reliable channel (ids, cumulative + 32-bit selective acks, resend after max(100 ms, 1.5 RTT), in-order delivery), ping/pong RTT, 10 s timeout, polite `DISCONNECT`.
- LAN discovery: broadcast plus loopback `DISCOVER` on ports 7777-7792; direct join by host name or IP and port. No public server is contacted (open question in MULTIPLAYER.md).

## MP2. Menus and entry (`port_mp_menu.cpp`, hooks in `port_window.cpp`, `port_input.cpp`, `CardLoad.cpp`)
- File select: Mario's left boundary on the save-block screen is x = 846 (measured with `SMS_PORT_TRACE`); running into it (x < 870, stick left, 15 game frames) while `TCardLoad` is in state 0 with the pad out of menu mode opens the multiplayer menu. A "<< Multiplayer" hint is drawn at the left edge there. Nothing else in the file select changes.
- ImGui pages: username (+ Random, for controller users), Create lobby (name, max players, game mode list = Free, shared objectives, port), Join (LAN list, auto-refresh every 3 s, direct IP:port), Character (stub), lobby panel (players, stage, ping, game mode + shared objectives for the host, Leave). Controller navigation like w9's menu; the game gets idle pads while a page is open. F2 opens the lobby panel in a session (main page on the file select); Z in the pause menu also opens it.
- Preferences: `port_multiplayer.ini` next to `port_settings.ini` (`SMS_PORT_MP_INI` overrides).

## MP3. Free mode on an in-memory save (`port_mp_game.cpp`, hooks in `Application.cpp`, `CardManager.cpp`)
- Start: `TFlagManager::firstStart()`, w13's `story2` preset (now reachable as `port_apply_flag_items()` in `Application.cpp`), `saveSuccess()` (so a game over restores the same state), then `setNextStage((1+1)<<8 | 2)` from the file select (or `mNextArea` for the debug auto-start). Plaza scenario 2 (post-game): 104 shines, statue clean, every course entrance open.
- `TCardManager::writeBlock`/`writeOptionBlock`/`createFile`/`format` answer READY without touching the card while the multiplayer save is in memory. Verified: a full session leaves the scratch card folder empty.
- Leaving (menu, host closed, timeout): back to area 15 (`setNextStage(15)`, the title of the file-select stage); the card is the player's again from there.
- Checked GameCube build after the shared edits: `build/GMSE01/mario.dol: OK`.

## MP4. Tests (`port/tools/mp_test.py`)
- `python port/tools/mp_test.py -n 3 --seconds 100 --kill-at 65 --walk`: host + 2 guests on loopback, off-screen, isolated user dirs; all reach the plaza (1/2), ~30 snapshots/s flow both ways (with `SMS_PORT_MP_TESTSNAP=1`), the killed guest is dropped after the 10 s timeout and the other guest is told. ALL PASSED.
- File-select path: `SMS_PORT_PADSCRIPT=650-1700/60:START,1800-2300/60:A,2600-3100:S270` + `SMS_PORT_MP_MENU_AUTO=host:<port>`: Mario runs left, the menu opens (frame ~2650), the lobby is created and the plaza loads.
- Leave path: `SMS_PORT_MP_LEAVE_AT=<frame>` on the host: both instances return to the file-select stage.
- Windows Firewall: no prompt or block seen for loopback; LAN play may prompt for `sms_port.exe` the first time a lobby is created.

## MP5. Shared objectives (stage 4; `port_mp_game.cpp`, `port_mp_net.cpp`, hooks in `Item.cpp`, `ItemManager.cpp`, `MarDirectorDirect.cpp`)
- Host setting "Shared objectives" (default off), synced; latched per stage visit. Design and rules in MULTIPLAYER.md.
- Ids: scene-placed yellow/red coins by index in `gpItemManager` (end of the scene's items reported by `TItemManager::loadAfter` before its coin pool), blue coins by (area, event id), Shines by shine id. Host-authoritative first-claim; apply path credits the count, sends the container message, then `kill()`; no reward/sound for the local Mario; pending removal for red coins not out yet; 100-coin Shine as in `TCoin::taken`.
- Checked: host `SMS_PORT_MP_SHARED=1 SMS_PORT_MP_DEBUG_COLLECT=2000,2300,2600,2900:coin`: the guest logs objectives 20204000-20204003 applied and its coin count goes 1, 2, 3, 4 (`SMS_PORT_TRACE=act`); `:blue`: blue coin 603fe00c applied, guest blue count 1.
- GameCube build after these hooks: `build/GMSE01/mario.dol: OK`.

## MP6. Robustness and scale checks
- `mp_test.py -n 2 --fuzz`: 3000 malformed datagrams at a running host (random bytes, bad lengths, truncated headers, unknown connection ids, short CONNECTs, wrong magic): no crash, no fake player (CONNECT bodies now start with a `JOIN` tag; names are cleaned to printable ASCII / well-formed UTF-8), then a protocol-99 CONNECT gets `REJECT` reason 1 and a `DISCOVER` is still answered. ALL PASSED.
- `mp_test.py -n 8 --nullgfx --kill-at 100 --walk`: 8 players (host + 7) in the plaza, every instance gets ~30 snapshots/s from every other one, the killed guest is dropped after 10 s and the 6 others are told. ALL PASSED.
- `SMS_PORT_MP_DROP=30` (new debug knob: drop 30% of sent and received datagrams) on both ends, shared coins: the handshake still completes and all 6 collected coins reach the guest (host resent 20 reliable messages).

## MP7. Free-mode plaza with w15's puppets (stage 3)
- w15's `port_mp_puppet.{h,cpp}`, `port_mp_overlay.cpp` and its two hooks (`MarDirectorDirect.cpp` `port_mp_logic_frame`, `MarDirectorPreEntry.cpp` view object, plus `port_mp_overlay_draw` in `port_window.cpp`) linked into a w10 test build (then removed again, they are w15's to merge): `mp_test.py -n 3 --walk --no-testsnap --shot 1500,1900,2300,2700 --present` passes with the real capture; the host sees Guest2 by the statue with its name tag, Guest2 (swimming) sees the host's puppet ("Host") on the plaza (`port/build_aurora/mp_shots/stage3_*.png`, w10 worker).
- `SMS_PORT_MP_TESTSNAP` now yields to the real capture (ignored for 2 s after any `port_mp_set_local_snapshot`).

## MP8. Extension points for the multiplayer team; worker rebased on main
- App messages: `port_mp_send(channel, data, len, reliable, to)` / `port_mp_recv` (`PORT_MP_CH_*` in `port_mp.h`), unreliable packet `APP` and reliable kind `APP`, relayed by the host (to everyone or one slot). Checked: `mp_test.py -n 3` with `SMS_PORT_MP_TESTMSG=1` (each instance broadcasts a reliable test message every 2 s): every instance logs messages from both others.
- Feature hooks: `port_mp_register_game_frame`, `port_mp_register_overlay`, `port_mp_register_lobby_section`, `port_mp_request_input`, so the task-board packages (MULTIPLAYER.md "Task board") live in their own files.
- File-select entry now counts presented frames (`TCardLoad::perform` can see `CUE_MOVE` more than once per game frame; smoke_all's newgame script, which taps left for 8 frames, had opened the menu). Now 30 presented frames (0.5 s) of running into the left boundary; the newgame scene no longer opens it.
- Worker rebased on main (w15's puppets, w9's controller work): main's files taken, my hooks re-applied (`MarDirectorDirect.cpp` = main + `port_mp_shine_got`; `port_window.cpp` has exactly one `port_mp_overlay_draw()` (w15's) and my `port_mp_menu_draw()`), base re-recorded. With w15's capture in, `mp_test.py -n 3 --walk --kill-at 80 --fuzz` passes on the merged build; `SMS_PORT_MP_TESTSNAP` is now opt-in (`--testsnap`) and yields to the real capture anyway.
- `mp_test.py` options: `-n`, `--seconds`, `--port`, `--kill-at`, `--walk`, `--shot/--present`, `--fuzz`, `--nullgfx`, `--testsnap`.

## MP9. Join by lobby name: lobby / rendezvous / relay server (`port/server`), client, menus
- `port/server/sms_lobby_server.py`: one-file Python 3 server (stdlib only, Linux-friendly, no disk writes): unique case-insensitive lobby names with heartbeats (15 s expiry, freed at once on UNREGISTER), name lookup, rendezvous (FOUND to the client with the host's observed and LAN endpoints, PUNCH to the host with the client's), relay sessions (30 s idle expiry), limits (1450-byte datagrams, 20 control packets/s per IP, 3 lobbies per IP, 500 lobbies, 200 relays, 256 KiB/s per relay direction), server protocol version check, game protocol check on LOOKUP, logs with hashed address tags only. Protocol table in MULTIPLAYER.md.
- Deployment kit: `port/server/DEPLOY.md` (Oracle Cloud Always Free, Google Cloud e2-micro: VM, security list / VPC firewall rule for UDP 7700, copy + install), `install.sh` (one command: python3, user, /opt/sms-lobby, systemd unit, the VM's own firewall incl. Oracle's iptables), `sms-lobby.service` (hardened unit). Nothing is deployed or contacted; `sh -n install.sh` OK.
- Client (`port_mp_net.cpp`): `port_mp_host2(..., PORT_MP_CONN_SERVER, password)` registers from the game socket (heartbeat 2 s, re-register when the server forgets, LAN-only fallback message when unreachable, "name taken" ends the session with a message); `port_mp_join_name` does LOOKUP + LAN DISCOVER by name at once, CONNECT to every candidate (host public + LAN endpoints, LAN replies), relay after 2.5 s (or `SMS_PORT_MP_FORCE_RELAY=1`) through virtual peer addresses 240.0.0.n; the host follows whichever path the client settles on. Password hash in CONNECT (host checks too). Server status ping (online / offline / not set up), observed public address. Protocol 3.
- Menus: connection type (Server (lobby name) default / Direct IP) on Create and Join; server mode shows no IPs/ports; server status line; "Lobby server address" editor; lobby panel join info with Copy buttons (lobby name in server mode; LAN / virtual-LAN addresses with adapter names, and the Internet address only from our own server's observation, else "needs port forwarding of UDP <port>"); on-screen keyboard ("Keys" next to every text field; D-pad/A, X delete, B/Start done); toasts when a client is dropped (host left, connection lost).
- `PortMpCharacter.colors[7]` (hair = the old reserved[0..2]); one-line accessor change in w15's `port_mp_palette.cpp` (`slotRgb`).
- Tests, all PASS: `mp_server_test.py --phase unit` (12 checks on the server alone: duplicate name, same-host re-register, per-IP limit, versions, oversize drop, lookup normalisation, PUNCH, relay both ways and third-party ignore, name freed after UNREGISTER); `--phase 1` (H1 registers "Bob's Lobby", H2 "BOB'S LOBBY" gets "name taken", G1 joins "bob's lobby" directly, G2 joins through the forced relay, app messages flow host<->relayed guest<->direct guest, "No Such Lobby" refused, H1 killed -> server expires it, guests notice, no client addresses in the server log); `--phase 2` (no server: H3 LAN-only lobby with password, G4 joins by name over LAN, G5 wrong password refused, host leave reported); `--phase 3` (name reusable right after the host leaves). Clipboard: `SMS_PORT_MP_MENU_AUTO=copytest` copied "192.168.1.104:7781" (Direct IP) and "Delfino Fun" (server mode), read back equal, previous clipboard restored. Screenshots: `port/build_aurora/mp_shots/server_*.png` (w10 worker).
- Corona Mountain from plaza scenario 2: the plaza-2 scene has a `MapObjChangeStage` at (-2160, 2850, -12750) (the scenario-9 one at (-2142, 1850, -12859)); putting Mario on it (`SMS_PORT_DEBUG_CALL=tp...`, warp 1,2, story2 flags) loads stage 52 (Corona Mountain). The other plaza-2 change-stage objects lead to Pianta Village (8), Sirena (6), Pinna Park (5) and the secret courses 21-24 and 29.


# Multiplayer stage presence (w11, task board P5)
New file `port/src/port_mp_presence.cpp` only (plus the P5 row, `reserved[3]` and a "Stage presence (P5)" section in `port/MULTIPLAYER.md`); it plugs in through `port_mp.h`'s hooks and `PORT_MP_CH_WARP`, edits none of w10's or w15's files and does nothing outside a session. No shared game source changed (GameCube DOL unaffected).
- Lobby panel section: where everyone is (stage + episode, or file select / movie / menus / loading, plus paused / talking / cutscene), "Warp to" per player, "Return to Delfino Plaza", off-screen arrows on/off, host option "Level select for everyone", and the level select (plaza, main stages with episodes, park interior, casino, Noki undersea, bosses, 33 secret courses).
- Stage changes: `TApplication::mNextArea` + `DIRECTOR_FLAG_STAGE_TRANSITION_PENDING` (setNextStage's path without its plaza gate demos); a request waits for the end of a talk/cutscene and runs from the pause menu once it closes.
- Warp to player: same stage+scenario, then a reliable spawn request to that player, answered with its last grounded position; applied with `TMario::warpRequest`. The request waits until Mario is visible and on the ground: teleporting during his stage-entry warp-in (which hides him until it ends) left him invisible for the rest of the visit (found in the first test, fixed).
- Off-screen arrows for same-stage players (game camera projection, widescreen aspect included).
- Checked with `mp_test.py -n 3 --walk` and `SMS_PORT_MP_PRESENCE_TEST` (all mp_test checks pass): host level select to Bianco 1; Guest1 warps to the host (loads Bianco 1, spawn answer, lands next to the host); Guest2 goes to Ricco 1, then warps to Guest1 in Bianco (spawned at Guest1's position); host opens the panel (screenshot: everyone's stage/episode), switches the guests' level select off, Guest1's level select is refused with a toast; host returns to the plaza; Guest1 warps to the host there (reload to the plaza + spawn next to the host, puppets and name tags show both). Arrows show the other player's direction and distance in Bianco on all three. Presence changes (stage, loading) arrive on every instance. smoke_all --quick: 20/20 (single player unchanged).
- Found in passing (not mine): game frames can stall for several seconds while 3 instances share the GPU (the presence code only calls a 5 s gap "loading"); the US Gelato episode-2 opening talk needs A, so an unattended warp waits there.

## MP10. Sessions never end up on the title screen; in-memory card; reconnection (P6)
- **Bug (user):** beating the game in a session went to the title screen while still in the lobby. Now `port_mp_redirect_title()` (hook before every `TApplication` state change, `Application.cpp`, `SMS_PORT_AURORA`) sends a player heading for the title (DONE, TITLE/MENU, or area 15) back to the session's Delfino Plaza with a toast, after the ending has played. Checked: `SMS_PORT_MP_START_AREA=60,0` + `SMS_PORT_DEBUG_CALL=bathdemo@1500`: movie 14, epilogue and staff roll (three movie states), the save prompt, then `app state 6 -> 4 (area 15/0) would leave for the title: back to the plaza` and state 5 area 1/2, still hosting.
- **Found on the way:** the ending's (and any first) save prompt looped on "File created." in a session, because the old write-blocking answered `createFile` READY without a file existing. Replaced by an in-memory card (`port/src/port_mp_savecard.cpp`; CARD calls renamed in `CardManager.cpp`): one emulated 59-block card per session, so create/open/read/write/status work and the prompt completes. The real card is unmounted before and after; the scratch user dir still has no card file after the session.
- Game over in a session (`SMS_PORT_MP_FLAGS=20001=0`, Bowser's fire): death, save prompt (in memory), `restore()`, back to plaza 1/2 with 3 lives: no redirect needed. Reset combo in a session (`B+X+START`): "Left the lobby (reset)." and the title as usual. Safety net: reaching the title / file select in a session leaves the lobby with a message.
- **Reconnection:** 3 s of silence shows "reconnecting" (toasts, lobby panel), sessions survive 20 s (was 10), the host accepts a known connection id from a new address after a 2 s gap. `SMS_PORT_MP_BLACKOUT=40,8` on one guest: "Connection problem: reconnecting...", then "Reconnected to the lobby." / host "Shorty is back"; `40,26` on another: dropped after 20 s, "Connection to the host lost.", back to the file select. Host leaving cleanly already ends the session for everyone with "The host closed the lobby."; host migration not done (MULTIPLAYER.md P6).
- w11's notes: the lobby panel lost its "Where" column (w11's presence section shows it); `port_mp_menu_close()` closes the panel from a warp button. Ping column shows "reconnecting..." for a silent peer (`PortMpPlayerInfo.lagging`).
- w9's QoL gate verified in a session: with `fast_forward = 1` (Tab held by KEYSCRIPT) and `infinite_water = 1`, single player shows ">> 4x" and water stays 10000; in a session there is no fast-forward and water drops to 9381 while spraying.
- Tests now launch silent and hidden (`SMS_PORT_AUDIO=0`, `SMS_PORT_OFFSCREEN=1`) and pick random ports (20000-40000) so parallel runs by other agents never meet (one earlier run on port 7781 was joined by another agent's "Guest1").
- GameCube build after the `Application.cpp` / `CardManager.cpp` edits: `build/GMSE01/mario.dol: OK`.

# Multiplayer enemy / object sync (w11, task board P8): design + prototype
Design: `port/MULTIPLAYER.md` "Enemy and object sync (P8)" (authority = nearest player per object, chosen by the stage leader; what to sync in which order; ids; spawn/despawn/late join; bandwidth and interpolation; puppet mode; RNG/timing hazards; risks). Prototype: `port/src/port_mp_objects.cpp` + one hook in `src/Strategic/liveactor.cpp` (`TLiveActor::perform`, `SMS_PORT_AURORA` only: while any actor is a puppet, `port_mp_obj_puppet_move` sets its pose and its `moveObject` is skipped). Host setting "Sync enemies: Off (default) / Experimental" (`reserved[2]`).
- Synced: the Strollin' Stu family and moving map objects with collision (lifts, rail and rotating platforms). Owner sends pose + alive at 15 Hz (changes, 1 s keyframes); puppets interpolate 100 ms behind; owner deaths play the puppet's own death (`kill()`), owner respawns `reset()` it.
- Found on the way: `gpConductor` is never cleared, so in a stage without a conductor (the plaza) it points at the previous stage's freed one; reading it crashed the first test. The prototype checks the scene's name tree for the conductor first.
- Checked with `mp_test.py` (2 and 3 instances, Bianco Hills 1, `SMS_PORT_MP_SYNC_ENEMIES=1`, `SMS_PORT_MP_OBJ_LOG=1`, `SMS_PORT_MP_OBJ_TEST`), all mp_test checks pass, no crash: the leader assigns the Stus to the nearest player, the others show them as puppets at the owner's positions (log samples equal on both sides); `near` moves ownership to the player who walks up (Guest2 took over 93 objects); `kill` on the owner (host, then Guest2) shows "died on its owner" on every other instance, and respawns follow; when the host leaves for the plaza the next player becomes leader and runs everything; moving platforms in Ricco Harbor follow the owner's rotation on the others.
- Single player: GameCube `dtk shasum` OK; smoke_all --quick 20/20; a replay recorded with main's current exe (Bianco, keyboard-scripted movement, 1920 game frames) replays on this build with 0 of 15 checkpoints differing (actor and camera hashes included).
# Headless boot log (x64, `build_port.py`)

Running list of boot blockers, one per entry: symptom, then cause, then fix.

Run: `port\build\sms_port.exe` with the working directory `dolphin_test`.
Debug knobs (all off by default):
- `SMS_PORT_STACKS=<s>` dumps every thread's stack after `<s>` seconds, then exits (`port_crash.cpp`).
- `SMS_PORT_THP_SPEED=<n>` plays movies `<n>` times faster, so tests skip the 94 s intro (`sdk_thp.cpp`).

`OSPanic` now prints a backtrace.
`TApplication::proc` logs every state change (`[port] TApplication state N`).

## 1. `JALSystem::init` writes to address 0
- **Symptom:** crash in the inlined `TFlagManager` constructor, called from `MSound::MSound`.
- **Cause:** 64-bit growth.
  `new` returned null because the 0x151800-byte sound `JKRSolidHeap` was full.
  JAudio objects carry 8-byte pointers, and every JKR heap block is rounded up to 16 bytes on x64, so boot needs about 0x16E000.
  `mSoundMax` was correct.
  MWCC null-checks the result of `new`; clang does not.
- **Fix:** `src/MSound/MSound.cpp` creates the heap with 0x200000 bytes under `SMS_PORT`.
  The port arena is 32 MB instead of 24 MB, so this fits.
  `portNew` (`JKRHeap.cpp`) now logs a failed `new` with the heap and its free sizes, so future cases show up straight away.

## 2. Audio thread crash in `TTrack::mainProc` (pointer `0x7800000000`)
- **Symptom:** the audio thread crashes in `updateSeq`; `mChildren[2]` holds `0x0000007800000000`.
- **Cause:** a clang optimisation of undefined behaviour in `TTrack::writeRegParam`.
  For sequence registers 0x20, 0x21, 0x2E and 0x2F, `uVar6` is never set before `setFlag(uVar6)`.
  clang treats that read as unreachable and deletes those four switch cases.
  The register value 0x78 was then stored through `unk0[0x2E]`, which lands in `mChildren[2]`.
  On the GameCube, `uVar6` is r31, which still holds the caller's `TSeqCtrl*`, i.e. the track address (from the asm of `TSeqParser::mainProc`).
- **Fix:** in `JASTrack.cpp`, under `SMS_PORT`:
  - `uVar6` is initialised to the low half of `&mSeqCtrl`, the GameCube value.
  - Register slots past the 6-entry `unk0` array are addressed from the struct base (`SMS_REG_SLOT`), so out-of-bounds indexing cannot be exploited the same way.

## 3. Endless intro movie
- **Symptom:** boot reaches `APP_STATE_DONE`, i.e. `TMovieDirector` with movie 9, and never leaves it.
- **Cause:** missing SDK piece.
  `THPPlayer*` were logging stubs, so `THPPlayerGetState()` stayed at 0.
- **Fix:** new `port/src/sdk_thp.cpp`, a host THPPlayer.
  - It has the same API, the same `ActivePlayer` struct and the same state machine as the decompiled `THPPlayer.c`.
  - It reads the big-endian header.
  - Frames advance at the movie's frame rate from the VI post-retrace callback, and it reaches state 3 (played) at the end.
  - Nothing is decoded or drawn yet.

  Also, the GameCube `TMovieDirector::setupThreadFunc` returns `rsetup()`'s result: it falls off the end with r3 still set, and `direct()` reads it through `OSJoinThread`.
  The `SMS_PORT` version had been changed to `return nullptr`; it now returns `rsetup()`'s value.

## 4. `JPAParticle` constructor writes to null
- **Symptom:** crash in `JPAEmitterManager::JPAEmitterManager` during `TMarDirector::loadResource`.
- **Cause:** 64-bit growth.
  The particle, emitter and field pools are solid heaps sized `count * sizeof(T)`.
  `sizeof(JPAParticle)` is 0xF8 on x64, and each block is rounded up to 0x100.
- **Fix:** `JPAEmitterManager.cpp` sizes each pool element as `ALIGN_NEXT(sizeof(T), 16)` under `SMS_PORT` (`POOL_ELEM_SIZE`).

## 5. `JPAResourceManager::load` writes to `0xA`
- **Symptom:** crash right after the previous fix; the emitter data pointer is null.
- **Cause:** endianness.
  The `.jpa` reads were all native-order: the header magic `JEFF`/`jpa1`, the block walk, the shape blocks and the `TEX1`/`KFA1` sizes.
- **Fix:**
  - `JPA_FILE_S16/U32/F32` macros in `JPADataBlock.hpp` read file fields big-endian under `SMS_PORT`.
    They are used in `JPAEmitterLoader`, `JPADataBlock`, `JPABaseShape`, `JPAExtraShape`, `JPASweepShape` and `JPAExTexShape`, and for the block size in `JPAEmitter`/`JPAField`.
  - The `JPAColorRegAnmKey` frame indices are converted before `makeColorTable`, including the one key past the end that the loop compares against.
  - The `KFA1` key table in the private copy is swapped in the `JPAKeyFrameAnime` constructor.
  - The file data is never modified, so `BEM1`/`FLD1` keep working through the typed (swapping) stream reads.

## 6. `TApplication::mountStageArchive` reads from null
- **Symptom:** `unk30` is null.
  `TNameRefGen::search("繧ｹ繝・・繧ｸ豈弱す繝翫Μ繧ｪ繧｢繝ｼ繧ｫ繧､繝門錐鄒､")` found nothing.
- **Cause:** a port-layer bug.
  The sources are UTF-8. The GameCube build passes them through sjiswrap, so string literals end up Shift-JIS, like the game data.
  clang only supports UTF-8 (`-fexec-charset=shift_jis` is rejected), so all 1965 Japanese literals in 267 files were UTF-8, and every lookup by Japanese name failed.
- **Fix:** new `port/tools/sjis_mirror.py`, used by `build_port.py` and `build_aurora.py`.
  - It writes copies of the affected sources and headers to `port/build*/sjis/` with narrow literals re-encoded as cp932 octal escapes.
  - The build compiles those copies, and the mirror include roots come first.
  - Comments and wide literals are left alone, and quoted includes that resolve next to the original file are made absolute.

## 7. `JKRHeap` aborts with an alloc of 0xB0000000 bytes
- **Symptom:** the abort happens in `TGraphWeb::TGraphWeb`, called from `TConductor::makeGraphGroup` in `setupObjects`.
- **Cause:** endianness.
  The rail file (`scene.ral`: `GraphDesc` table plus `TRailNode[]`) is used in place, and `translateNodes` rewrites it in place.
- **Fix:** `graph.cpp`, `portSwapRail()` under `SMS_PORT` in the `TGraphGroup` constructor.
  It swaps the whole file to native order once, and is idempotent: a first node count of 0x10000 or more means the file is still big-endian.
  `JKRDefaultMemoryErrorRoutine` now logs the heap, size and alignment before it aborts.

# Aurora boot log (`build_aurora.py`)

Run: `python port/tools/build_aurora.py --run --timeout 120`, or `port\build_aurora\sms_port.exe` with `SMS_PORT_DISC` set.
New knobs:
- `SMS_PORT_SHOT=<frame>[,<frame>...]` saves the window as `shot_<frame>.bmp` after presenting each listed frame (`port_main.cpp`; PrintWindow, else a screen copy, so keep the window visible and uncovered).
- `SMS_PORT_GS_DEBUG=1` while running `build_aurora.py` builds game/JSystem units with `-fstack-protector-all` and reports the function whose stack cookie was smashed (`sms_gs_check` in `port_crash.cpp`).
  Debug only; ninja rebuilds everything when it is toggled.
- stdout is unbuffered in the Aurora build, so `sms_port.exe > log.txt 2>&1` keeps OSReport and port/Aurora lines in order.

Build note: `robocopy` keeps the source timestamps, so after a sync the ninja objects and the `build_aurora/sjis` mirror can look newer than the synced sources.
Delete `port/build_aurora/obj`, `port/build_aurora/sjis` and `.ninja_log`/`.ninja_deps` after a sync (otherwise stale objects link, e.g. duplicate `OSReport`).

## 8. Crash in `vfprintf` from `OSReport` in `TApplication::proc` (format pointer `0x3F80000000000000`)
- **Symptom:** the state-change `OSReport` on the second loop iteration crashes in the CRT.
  The format argument itself is `0x3F80000000000000`; the literal's address was kept in `rbx` across the loop and came back changed from `gameLoop()`.
  The CARD "Failed to open file" message just before is harmless (no save file yet) and unrelated.
- **Cause:** a decomp transcription bug.
  `TApplication::gameLoop` and `TApplication::drawDVDErr` declared the projection matrix for `C_MTXOrtho` as `Mtx` (3x4, 48 bytes), but `C_MTXOrtho` writes a `Mtx44`.
  The last row `{0, 0, 0, 1.0f}` overran the local by 16 bytes into the saved registers (on the GameCube it lands in other stack slots).
  The GameCube frames confirm `Mtx44`: in both functions the matrix and the next local are 0x40 bytes apart (`gameLoop` 0x1B0 and 0x1F0, `drawDVDErr` 0xB8 and 0xF8).
- **Fix:** both locals are `Mtx44` in `src/System/Application.cpp`.
  The unit is NonMatching; the match percentages are unchanged, and the DOL is unchanged.
- **How it was found:** a stack-protector build hid the bug (layout change), so a naked-asm `rbx` probe around the calls in `proc` showed that `gameLoop()` returned with `rbx` changed.

## 9. `TSpcBinary::init` reads from `0xC75B7A04`
- **Symptom:** the crash happens in `calcAndStoreKeys`, called from `TEventWatcher::launchScript` in `TMarDirector::setupObjects` on the title stage.
- **Cause:** endianness.
  The SPC event script (`.sb`) is used in place: header, data-offset table, symbol table and bytecode immediates were all read native-order.
- **Fix:** `spcinterp.cpp`, `spcPortSwapBinary()` under `SMS_PORT` in the `TSpcBinary` constructor.
  It swaps the header, the data-offset table and the symbol table to native order once.
  It is idempotent, because several binaries can wrap one resource (`/default.sb`): a text offset of 0x10000 or more means the file is still big-endian, since the text always starts right after the 0x1C-byte header.
  `fetchS32/U32/F32` in `spcinterp.hpp` read the bytecode big-endian through `SPC_FETCH_BYTE`, which is `src[i]` on the GameCube, the same tokens as before.

## 10. Audio thread crash in `JAISystemInterface::setSePortParameter` (track `0x5_8020DD10`)
- **Symptom:** `args->mTrack` has `5` in its upper 32 bits.
- **Cause:** 64-bit layout.
  `JAIPlayerParameter` overlays `TPortArgs` with `f32/u32 mArgsAs*[10]`, and `setSeqPortargsF32/U32` index it as 4-byte words in the GameCube layout (word 1 = `mFlags`).
  On x64 `mTrack` is 8 bytes, so word 1 is the upper half of `mTrack` and every parameter landed 4 bytes early.
- **Fix:** `JAISystemInterface.cpp` maps the word index to the field (`portArgWord`) under `SMS_PORT`: word 0 is `mTrack`, and word n is `mFlags` + 4(n-1).
  `setSeqPortargsPS16` has no caller.

## 11. New game: `TMovieRumble::init` 竊・`Koga::ToolData::GetValue` reads `0x82000004`
- **Symptom:** the crash comes after choosing a New file, in the opening movie's setup thread (`TMovieDirector::rsetup`).
- **Cause:** endianness.
  The movie rumble table (`/subtitle/rnbl/*.bcr`, a JMap/BCSV file) is read in place: header, field table and cells were native-order.
  The intro movie has no `.bcr`, so only the opening movie hit this.
- **Fix:** `src/MarioUtil/ToolData.cpp`, `toolDataSwapToNative()` under `SMS_PORT` in `ToolData::Attach`.
  It swaps the header, the field table (hash, mask, offset), and every entry's numeric cells (LONG/FLOAT/LONG_2/STRING_PTR as 32-bit, SHORT as 16-bit; strings are left alone).
  Each cell offset is swapped once, even when several fields share it.
  It is idempotent: a field count of 0x10000 or more means the file is still big-endian.
- **Verified:** with scripted input, title 竊・file select 竊・New 竊・opening movie (`TMovieDirector` inside `APP_STATE_GAMEPLAY`, area 0) 竊・Delfino airstrip (area 0) gets past this point.
  The user also reported rumble during the movie.

## SMS_PORT_WARP (debug warp)
`SMS_PORT_WARP=<area>,<scenario>`, or `build_aurora.py --run --warp <area>,<scenario>`, is off by default and exists only under `SMS_PORT`.
After the Nintendo logo (`APP_STATE_NLOGO`, once `initialize_nlogoAfter()` has loaded the common archives and created the game heap), `TApplication::proc` skips the intro movie, title and file select.
It does what choosing a New file does: `TFlagManager::firstStart()`, save slot 0, `mNextArea.set(area, scenario, 0)`, then `APP_STATE_GAMEPLAY`.
The stage's own arrival movie still plays through `checkAdditionalMovie()` (use `SMS_PORT_THP_SPEED`).
- Code: `src/System/Application.cpp` (the block after the post-gameLoop switch) and `port_warp_target()` in `port/src/sdk_os_misc.cpp`, so it works in both port builds.
- Examples: `--warp 0,0` (Delfino airstrip), `--warp 1,0` (Delfino Plaza), `--warp 2,0` (Bianco Hills episode 1).
- Checked: `--warp 1,0` plays the plaza arrival movie (state 6 twice) and loads Delfino Plaza (state 5, area 1/0) in about 10 s.
  `--warp 0,0` plays the opening movie and loads the airstrip (state 5, area 0/0, twice). Both then stop at the w6 and w5 blockers below.

## Run and debug knobs added (Aurora build)
- `build_aurora.py --run` uses real D3D12 and a visible window by default; `--nullgfx` selects Aurora's Null backend (no GPU device, software SDL renderer, window off-screen and never activated).
  `SMS_PORT_OFFSCREEN=1` moves a real window off-screen. Aurora ignores negative positions, so +30000 is used.
- `--run` only kills the `sms_port.exe` it started itself (PID in `build_aurora/run_pid.txt`), because the user may be running the same exe.
- `--run` gives the game its own pipeline cache, `SMS_PORT_CACHE_DIR=port/build_aurora/cache`, emptied on every run.
  Aurora recompiles every cached pipeline at startup, so one bad GX state recorded by an earlier run fails the next run before the game starts.
  **The per-user cache** (`%APPDATA%\Super Mario Sunshine`, used by double-click runs) **can be in that state now**: it holds the bad WGSL pipeline recorded by a run that reached the airstrip.
  Hand-started runs then die at boot with the WGSL fatal until that cache is cleared. That folder is outside w7, so I left it alone.
- Double-click launch: with no `SMS_PORT_DISC` and no `./files`, `sdk_dvd.cpp` walks up from the exe's directory looking for `dolphin_test\files`, then `files`.
- Crash, OSPanic and Aurora FATAL text and backtraces are also appended to `sms_port_crash.log` next to the exe.
  On an Aurora FATAL, the game main thread's stack is dumped as well (`port_dump_thread`), because Aurora's FIFO and pipeline workers run on their own threads.
- `SMS_PORT_PADSCRIPT=<frame>:<buttons>,...` presses buttons on pad 0 for 10 frames each (START, A, B, X, Y, Z, L, R, UP/DOWN/LEFT/RIGHT or a hex mask, joined with `+`).
  Game `PADRead` calls are renamed to `port_PADRead` in game objects.
- `SMS_PORT_SHOT=<frame>,...` saves screenshots (`shot_<frame>.bmp`). The shot is a screen copy, so the log line says when another window covers the game window.
- `SMS_PORT_GXSYNC=1` drains Aurora's FIFO before and after every `GXCallDisplayList` (`port_GXCallDisplayList`), so a FIFO error stops inside the game call that queued it, and the fatal handler hex-dumps that display list.
- `SMS_PORT_WATCH=<hex addr>` sets a hardware write watchpoint in all threads. It can wait until a frame (`SMS_PORT_WATCH_FRAME`) or until a GD display list is built over the address (`SMS_PORT_WATCH_GD=1`).
- `SMS_PORT_GS_DEBUG=1` (while building) adds a stack-protector build with a reporting cookie check (entry 8).

## Blockers on the new-game path, outside my area (for w6 and w5)
- **Airstrip, w6: Aurora FATAL `indexed XF load outside array 22: offset=2004408, size=16, array size=36`.**
  Root cause, found with `SMS_PORT_GXSYNC` plus the DL dump: Mario's `J3DShape::mVcdVatCmd` display list.
  `makeVcdVatCmd` writes its commands (0xB4 bytes here) and `GDPadCurr32`, but `J3DShape::draw` calls `GXCallDisplayList(mVcdVatCmd, kVcdVatDLSize)` with 0x180 on Aurora.
  The tail of the buffer (from `J3DShapeFactory::allocVcdVatCmdBuffer`, `new u8[]`, not cleared) still holds stale heap data, matrices left by the previous stage.
  From offset 0x140 those bytes parse as `2B D9 7E 3D` = `LOAD_INDX_B`, index 0xD97E.
  The fix belongs in w6's J3D SMS_PORT code: clear the buffer, or pass the real list length.
  A local test that zeroed the buffer did not, on its own, remove the WGSL fatal below; I reverted it.
- **Plaza (warp 1,0) and airstrip, w6: Aurora FATAL `WebGPU error 2: Error while parsing WGSL: :399:67 error: cannot index into expression of type 'f32'`.**
  It comes from `aurora::gx::build_shader` on the pipeline worker.
  The shader source cannot be dumped (`EnableDebugPrints` is constexpr false in Aurora).
- **w5: two JAudio corruption crashes, intermittent, during the title, file select and airstrip.**
  1. `JASystem::Driver::updatecallLogicalChannel` gets channel `0x8030E487`, an unaligned pointer, via `TTrack::noteOn` 竊・`noteOff`. `TNoteMgr::unk0[]` holds a bad pointer.
  2. `TChannelMgr::getListHead` reads a non-canonical pointer in `TTrack::mainProc` (`mChannelUpdater` list head).
  Both are TTrack memory corruption; I did not find the writer.

## Where boot is now (real D3D12 renders, `port/build_aurora/aurora_*.png`)
- Nintendo logo 竊・intro movie (no picture yet: w8 owns THP decode) 竊・title logo (`aurora_title_logo.png`) 竊・title menu and options (`aurora_title_menu.png`) 竊・file select with Mario on the beach (`aurora_file_select.png`).
- New file 竊・opening movie 竊・Delfino airstrip loads, then stops at the w6 XF fatal above.
- `--warp 1,0` loads Delfino Plaza, which stops at the w6 WGSL fatal.
- The "jumps back to the save select" report: the opening movie plays (the rumble table works after fix 11), but THP frames are not drawn yet, so the last file-select frame stays on screen.

## THP decode (movie video)
- **Symptom:** movies "played" (state machine, timing and rumble cues ran) but the screen kept the previous frame: `sdk_thp.cpp` decoded nothing and `THPPlayerDrawCurrentFrame` drew nothing.
- **Fix:**
  - New `port/src/port_thp_decode.cpp` (`port_thp_video_decode`): a self-written baseline JPEG decoder (Huffman with a 9-bit fast table, float separable IDCT) for THP video frames.
    It follows `THPDec.c`'s bitstream rules: markers SOI/DQT/SOF0/DHT/DRI/SOS (APPn/COM skipped), 3 components with Y 2x2 and U/V 1x1 sampling,
    no 0xFF00 byte stuffing in the scan, and a restart interval only byte-aligns and resets the DC predictors (no RSTn bytes).
    Output is `THPVideoDecode`'s: Y (w x h) and U/V (w/2 x h/2) as GX_TF_I8 8x4-texel tiles of 32 bytes, plain JPEG samples (U/V centred on 128).
  - `sdk_thp.cpp`: Prepare starts a host decoder thread (`std::thread`, created inside `port_host_alloc_push`) standing in for the SDK's read + video decode threads, and waits for the first frame (PrepareReady).
    It walks the frame chain like `THPRead.c` (first word of each frame = next frame size; looping goes back to `movieDataOffsets`), reads into `readBuffer[0]` (or uses `movieData` for on-memory movies) and decodes into a free texture set.
    PlayControl pops decoded sets at the frame-rate clock as before; replaced sets go to a used queue that `THPPlayerDrawDone` (after `GXDrawDone`, which drains Aurora's FIFO) / `THPPlayerPostDrawDone` returns to the decoder, as in the SDK.
    `SMS_PORT_THP_SPEED` skipping moved into the decoder (it still always lands on the last frame of a non-looping movie).
    A decode error is logged instead of setting `videoError` (which would end the movie with state 5).
  - `THPPlayerDrawCurrentFrame` (Aurora build only) issues `THPDraw.c`'s `THPGXYuv2RgbSetup` / `THPGXYuv2RgbDraw` / `THPGXRestore` through GX: three I8 textures and the four TEV stages with the SDK's constants (C0 = {-90, 0, -114, 135}, K0/K1/K2), so Aurora does the YUV to RGB conversion.
    `TModelGate` binds the texture sets directly as J3D textures; they now hold real I8 data in the layout it expects.
  - Logs: `[port] THP open <file>: WxH, N frames ...` and `[port] THP stop: state S, frame F of N`.
  - Audio: not decoded or mixed (the port has no host audio output yet; TODO in `sdk_thp.cpp`). Movies with audio run on the frame-rate clock with `curAudioNumber` following the video.
- **Test:** `python port/tools/test_thp.py --movie openingA --frames 0,150,600 --all` builds `port/tools/test_thp.cpp` with the decoder, decodes every frame (fails on any error) and writes PNGs of the listed frames
  (untiled, converted with the TEV stage constants) to `port/build/test_thp/`.
  openingA (2049 frames), Entrance (2816) and EX128x144_q0 (300) decode without errors at about 0.6, 0.9 and 0.06 ms per frame.
- **In game (Aurora):** the boot intro (`Entrance.thp`) and the new-game opening (`openingA.thp`) show real video (`port/build_aurora/thp_intro_entrance_850.png`, `thp_intro_entrance_1001.png`, `thp_opening_openingA.png`).
  Headless with `SMS_PORT_THP_SPEED=8`, Entrance reaches its last frame (2815) and state 3.
- **Seen on the way (not THP):** every other presented frame is black. The game renders at 30 Hz and waits two retraces per frame; `port_main.cpp`'s `VIWaitForRetrace` ends and presents an Aurora frame on every retrace, so the second present of each game frame is empty
  (consecutive shots 600/601/602/603: black, picture, black, picture; the Nintendo logo too). `SMS_PORT_SHOT` therefore needs pairs of adjacent frames. Likely fix: only end/present the Aurora frame when the game copied a new frame (VISetNextFrameBuffer/VIFlush) since the last present.

## Where boot is now
- The Nintendo logo, the Dolby screen and the intro movie (THP) render: see `port/build_aurora/shot_30.png`, `shot_120.png` and `shot_500.png`.
- With `SMS_PORT_THP_SPEED=8`, boot reaches the title stage (`APP_STATE_GAMEPLAY`, area 15/0): `setupObjects` completes, and particles, rails, J3D models and the event script all load.
  It runs for more than 4000 frames without crashing, and cycles title 遶翫・attract movie (state 6) 遶翫・title.
- One run before fix 10 captured the title-stage sky with seagulls (`shot_1300.png` from that run, now overwritten).
  Since fix 10 every title-stage capture is black, so the next thing to look at is rendering: the fader, EFB copies (`GXCopyTex`), and `GXPeekARGB`.


# Input, window, frame pacing (w9)

## W9-1. Every other presented frame is black (flicker)
- **Symptom:** consecutive `SMS_PORT_SHOT` captures alternate picture/black (Nintendo logo, Dolby screen, title).
- **Cause:** frame pacing in the port layer.
  The game draws at 30 Hz and waits two VI retraces per frame (`JDrama::TVideo::waitForRetrace(2)`), but `VIWaitForRetrace` ended and presented an Aurora frame on every retrace.
  Aurora presents its EFB (its `GXCopyDisp` does nothing), and the second Aurora frame of each game frame had no drawing in it, so it presented a cleared, black EFB.
  On the GameCube the VI keeps scanning out the last XFB until the game copies a new one.
- **Fix:** `port_main.cpp` presents only when the game has called `GXCopyDisp` since the last present.
  `build_aurora.py` renames game/JSystem calls of `GXCopyDisp` to `port_GXCopyDisp`, which sets the flag and forwards.
  On other retraces the previous image stays on screen, and only `SDL_PumpEvents` runs, so the window stays responsive and the open frame is not touched.
  A close request still exits if no frame has been presented for 60 retraces.
  Retrace timing and `gPortFrameCount` (the `SMS_PORT_SHOT`/`SMS_PORT_PADSCRIPT` frame numbers) are unchanged: one count per main-thread retrace.
  Until the first `GXCopyDisp`, and in the `SMS_PORT_GXTEST` smoke test, every retrace presents as before.
  `SMS_PORT_PRESENT_ALL=1` restores the old behaviour for comparison.
- **Screenshot fix:** `SMS_PORT_SHOT` captured `FindWindowA("Super Mario Sunshine")`, which can be another running copy (other agents' runs, off-screen `--nullgfx` windows).
  It now captures this process's own SDL window (its `HWND` from the SDL window properties).
- **Evidence:** shots 180-240 (Dolby fade out/in) step smoothly, two identical captures per game frame, with no black frame in between.
  Shots 400-407 (Nintendo logo) and 1600-1605 and 2400-2403 (title stage, `SMS_PORT_THP_SPEED=8`) are all picture frames.
  With `SMS_PORT_PRESENT_ALL=1` the same run alternates (e.g. 400 black, 401 logo, 402 black, 403 logo).


# Static audit (w10)

Proactive sweep for the three crash classes (unswapped big-endian data, 64-bit layout, clang UB), plus bitfield order.
Tools:
- `python port/tools/build_aurora.py --warnings [path-prefix]`: no build; syntax-checks every unit with the exact Aurora flags plus `-Wreturn-type -Wuninitialized -Wsometimes-uninitialized -Wpointer-to-int-cast -Wint-to-pointer-cast -Warray-bounds -Wshorten-64-to-32 -Wreturn-stack-address -Wunsequenced -Winteger-overflow ...` (`WARN_FLAGS`) and writes `port/build_aurora/warnings.txt` (about 40 s for all 583 units).
- `python port/tools/disc_inventory.py`: every file format on the disc; the format 遶翫・loader 遶翫・status table is in ENDIAN.md "Disc format inventory".
- `python port/tools/test_assets.py`: `.pad` and `.bcr` through the real loaders.

## A1. CodeWarrior semantics as compiler flags (build_aurora.py, game/JSystem units)
`-fcheck-new` (MWCC null-checks `new` before the constructor; JKRHeap's `new` returns null when full, see fix 1), `-fwrapv` (signed overflow wraps on PowerPC), `-fno-strict-return` (a non-void function that falls off its end returns garbage instead of being "unreachable", the JASDvd/`setupThreadFunc` class), `-fno-delete-null-pointer-checks`.
Strict aliasing is already off (clang's MSVC target defaults to relaxed aliasing; checked: no TBAA metadata).
Checked: title loop, `--warp 1,0`, `2,0`, `2,5` run 3000+ frames without a crash.

## A2. `TSpineEnemy::walkToCurPathNode` / `zigzagToCurPathNode` read an uninitialised local (every enemy that walks a graph)
- **Cause:** decomp transcription errors in `src/Enemy/enemy.cpp`: `if (fVar3 == 0.0f)` tested the variable being computed instead of `MsSin(turn_speed)` (asm: `fcmpu f0, f1` on the sin result), and in `zigzag` the turn amount `fVar2` was never set when the node is near (the GC code copies the angle difference first, `fmr f5, f4`).
  clang may fold either read to anything.
- **Fix:** both corrected (plain source fixes in a NonMatching unit; match goes up: walk 95.9 遶翫・97.8 %, zigzag 93.9 遶翫・94.7 %).

## A3. `TMario::slideProcess` reads `turn` uninitialised when the slide has (almost) stopped
- On GC `turn` lives in r31, which its only caller (`doSliding`) holds at 0 (`stopped`); `turn = 0` under `SMS_PORT`.

## A4. JGeometry `TVec3` `operator+`/`operator-` (friend, by-value first argument) and `TVec2::operator+/-` return a reference to a dead object
- `TVec3`'s return a reference to their by-value parameter (clang 22 marks such arguments `dead_on_return`); `TVec2`'s return a reference to a local.
  Used for nearly every `a - b` in game code. Return by value under `SMS_PORT` (`JGVec3.hpp`, `JGVec2.hpp`).

## A5. `.pad` input replays read in place (Shadow Mario chases)
- `port_swap_pad` in `TMarioInputReplay::init` (`src/Player/MarioRecord.cpp`), tested on all 381 files of 20 scenes.

## A6. TEX1 ResTIMG headers whose pad byte is 0x5A on disc were never swapped (Corona boss)
- See ENDIAN.md "TEX1 pad-byte marker": `port_swap_tex1_timgs_force` inside `port_swap_j3d_model`.

## A7. Bitfield order / byte-order overlays (sweep of all src/ and libs/)
MWCC allocates bitfields from the most significant bit, clang/x86 from the least; byte overlays of multi-byte integers flip on little-endian.
- `JASChannel.hpp` `TChannel::MixConfig`: `u16 mWhole` written as numbers (0x150, 0x210, ..., bank data `unk4E[]`) and read through `{u8 u; u8 l0:4; u8 l1:4}`. **Fixed** (fields reversed under `SMS_PORT`: `l1:4, l0:4, u`).
- Already fixed by w7: `JASSeqParser.cpp` note-on command bits, `JASChannel.cpp` `asP` hi/lo union.
- Safe (only accessed by name, never overlaid): `JAIBasic::unk1C` flags, `MuteBit`.
- SDK/MSL bitfields (`PPCArch.h`, `OSCache.c`, `SIBios.c`, `ansi_files.h`): not compiled for PC / not used by game code. No GX register bitfields exist in the headers the port uses (Aurora's own).
- Other byte views checked and safe: `JUtility::TColor` and `J3DTevStage::load` (already `SMS_PORT`), `J3DCluster` DL reads (`sms_be16`), `spcinterp` fetches, `JAIBasic` init data, `PacketUtil`/`JRenderer` float bits.

## Open items found by the audit (not fixed; owners)
- `JASTrack.cpp` `TTrack::mainProc` (w5): the `goto bail` path ORs an uninitialised `r31` into `unk3B4`. On GC r31 is the caller's: for child tracks that is `mainProc`'s loop offset `i*4`, for the root track whatever the player held. Needs a GC-faithful value.
- `spider.cpp` `TSpider::bind`: `unaff_f29` (y offset) is the caller's f29 on the "touching a wall but moving away" path; the GC value comes from far up the call chain. Float only, no crash.
- `SelectShine2.cpp` `TSelectShine::move`: `height` uninitialised for the one frame where `mBobPhase == 4.0f` (visual).
- `JAIGFrameStream.cpp` (w8): `needDecode` read when `movieframe == 0` (result is the same either way through `||`).
- `--warp 4,0` (Ricco, w11): Aurora fatal "unhandled tcg src 21" at about frame 1200 (texgen source 21 is not a valid `GXTexGenSrc`; check whether a material/texgen value is misread).


# Audio (w5)

Sound now comes out: JAudio's DSP voices are rendered on the host and the AI DMA buffers go to SDL3.
Files: `port/src/sdk_audio.cpp` (AR/ARQ/AI/DSP entry points and the DSP mail protocol), `port/src/port_audio_dsp.cpp` (host "microcode"),
`port/src/port_audio_out.cpp` (SDL3 output, WAV capture, extra PCM stream), `port/src/port_audio.h`.

How it works:
- JAudio drives the DSP over the mailbox exactly as on the GameCube (`dsptask.c`, `dspproc.c`, `JASDSPBuf.cpp`): `0x81` DsetupTable (voice blocks, tables, FX lines),
  `0x82` DsyncFrame (sub-frame count, left/right frame buffers, output level), then one `DSPReleaseHalt` per sub-frame (7 x 80 samples = one 560-sample frame).
  `sdk_audio.cpp` decodes the mails; each `DSPReleaseHalt` renders one sub-frame synchronously and then answers `0xFF00` through JAudio's `__DSPHandler`, so the
  audio thread's frame loop is unchanged.
- The voice blocks (`JASystem::DSPInterface::DSPBuffer`) and FX lines (`FXBuffer`) are written by natively compiled game code, so the renderer reads them through the
  game's own structs (native order, x64 layout). The resampling/ADPCM tables are static arrays above 4 GB in the exe, so their 32-bit mail addresses are truncated:
  the renderer takes them from the symbols (`DSPRES_FILTER`, `DSPADPCM_FILTER`).
- Per voice: AFC ADPCM from ARAM (9-byte frames of 16 4-bit samples; 5-byte 2-bit frames) with JAudio's coefficient table, PCM8/PCM16 (big-endian) from ARAM,
  PCM16 from main RAM in a circular buffer (source `0x21`, JAI streams; the renderer keeps `currentPosition`/`samplesBeforeLoop`/`remainingLength` up to date,
  JAIGFrameStream reads them), 64-sample oscillators; loops with the stored ADPCM history; pitch = 4.12 step resampled with JAudio's 4-tap polyphase table;
  FIR (outer/"muffle" filter), IIR (pass-through unless a sequence animates it), distance low-pass; per-bus volume ramps over the sub-frame, or the auto-mixer
  (volume/pan/FX send) form; pause; end requests (ramp to 0, then `done`). Four FX lines (delay buffer in main RAM, 8-tap FIR feedback, two sends).
  Envelopes, vibrato and sequencing are CPU-side JAudio and already ran.
- Output level: DsyncFrame's level is applied as 2.14 fixed point (0x4000 = unity; SMS sends 0x5000 = x1.25). Read as 4.12 (x5) the boot jingle and the title
  ambience saturate constantly; as 2.14 the title stage peaks just under full scale. This is the one tuned constant (see the comment in `port_audio_dsp.cpp`).
- AI: the DMA interrupt (560 frames at 32 kHz = 17.5 ms) stays the game's audio clock. At each tick the buffer that starts playing goes to
  `port_audio_out_ai` (AI order is right/left; swapped for SDL). The next period is trimmed by up to +-2 % in 0.5 % steps to hold the SDL queue near 3 buffers.
- Knobs: `SMS_PORT_AUDIO=0` (no device), `SMS_PORT_WAV=<path>` (record the AI output, 16-bit stereo 32 kHz), `SMS_PORT_AUDIO_LOG=1` (DSP setup, per-voice start
  lines, periodic voice/queue/underrun stats, JAI sound requests), `SMS_PORT_AUDIO_TRACE=<voice>` (per-sub-frame pitch/volume of one voice).
- For w8 (THP movie sound): `port_audio_submit_stream(samples, frames, rate)` queues interleaved stereo s16 (left, right) at any rate on a second SDL stream
  mixed into the same device; `port_audio_clear_stream()` drops it. (The original THPPlayer mixes through `JASystem::Kernel::registerMixCallback(.., 3)`
  instead; that path also works now, since `vframeWork` runs the mix callback into the real DAC buffer.)

Test evidence (2026-10-08, `SMS_PORT_THP_SPEED=8`, 75 s from boot, `SMS_PORT_WAV`):
- 0-1.5 s boot jingle (peak 29138), 20-65 s title stage: title BGM (sequence `0x80010010`, chords of one looped instrument at pitches 0x0d75/0x11f6/0x155c =
  4:3 and ~8:5 ratios) plus the looping wave/seagull ambience (`0x5003-0x5005`, auto-mixer voices). RMS -25..-35 dBFS, 373 clipped samples in 75 s
  (all in the boot jingle), onset autocorrelation peaks at 88/118/171 BPM. PCM16 SE data checked big-endian (sample-to-sample smoothness 0.67-0.83 vs 1.32 swapped);
  the boot jingle decoded offline from `w1stLoad_0.aw` with the same AFC rules is a clean waveform (rms -13 dBFS, full-scale peaks), which is why the
  output level was taken as 2.14.
- Normal-speed 150 s `build_aurora.py --run`: 8400 AI buffers, 0 underruns, 0 queue drops, queue steady at ~1450-1770 frames (45-55 ms), no crash or hang.
- Not heard in gameplay yet: with `SMS_PORT_WARP=1,0` Delfino Plaza renders but JAI gets almost no sound requests (one SE, no BGM), so nothing plays there;
  that is game-side (the warp skips the title/file-select flow), not the mixer. Menu sounds were not reached (title screen START did not open the file select in the test run).

## A-1. JAudio bitfields and byte overlays (shared with w7/w10)
- `JASSeqParser.cpp` `cmdNoteOn`: the command-byte bitfields were read in clang's LSB-first order, so the voice index came from an arbitrary register and
  `TNoteMgr::unk0[voice]` wrote past `TNoteMgr` into `mChannelUpdater` (the intermittent `updatecallLogicalChannel` / `getListHead` audio-thread crashes).
- `JASChannel.hpp` `MixConfig` and `JASChannel.cpp` `playLogicalChannel` `asP` union: bus numbers / mix curves read from the wrong byte (voices routed to bus `0x50`).
## A-2. `.afc` stream header and PCM stream data are big-endian
- `JAIGFrameStream.cpp` `__start`: header fields read with `sms_be32/16`; `__DecodePCM` swaps the PCM16 samples. (Only `title.afc` exists, ADPCM.)
## A-3. `TTrack::mainProc` `goto bail` ORs the caller's r31 into `unk3B4`
- GameCube asm: at `bail` r31 has not been set by `mainProc`, so it is the caller's: `i*4` from the parent's child loop, or `i*0xC` (the `callList` entry
  offset) from `Kernel::subframeCallback` -> `rootCallback` (which never touches r31). Reproduced under `SMS_PORT` with `sPortEntryR31` (JASTrack.cpp) and
  `Kernel::gPortCallbackR31` (JASCallback.cpp). DOL unchanged.

Left to do (audio): THP movie audio (w8, API above); the exact DSP output scaling and the IIR/FIR coefficient semantics are reconstructions;
Dolby surround (back buses are folded into L/R); `JAIBasic` debug request log is `SMS_PORT_AUDIO_LOG` only.

# Memory card (w5)

Saves work through Aurora's CARD (GCI-folder mode): `<user dir>/USA/Card A/01-GMSE-super_mario_sunshine.gci`, a 0x40-byte GCI directory entry plus the
0xE000-byte save file exactly as the game writes it. The user dir is Aurora's `userPath`: by default SDL's pref path `%APPDATA%\Super Mario Sunshine\`
(same folder as Aurora's caches/imgui.ini); `SMS_PORT_USER_DIR=<dir>` moves it (test runs use it so they never touch the player's save).
- The boot log line `[aurora ERROR: aurora::card] Failed to open file: super_mario_sunshine` only means there is no save yet: the game's
  `readOptionBlock` gets `CARD_RESULT_NOFILE` and offers to create the file, as on a fresh GameCube card.
- `CARDUnmount` in Aurora is a stub returning `NOCARD`; the SDK returns `READY`, and `TCardLoad` reads that status back after `unmount()`.
  `CardManager.cpp` (under `SMS_PORT_AURORA`) calls `port_CARDUnmount` (`port_card.cpp`), which returns `READY` while the card folder exists.
- The save blob is native-order on PC (TFlagManager writes raw, see ENDIAN.md), so a PC save is not interchangeable with a real GameCube save (checksums and
  flag words would read wrong); the GCI container itself is standard.
- Self-test: `SMS_PORT_CARDTEST=1` runs `port_card_test()` instead of the game: the game's own `TCardManager` (worker thread, probe/mount, create,
  option block, save slot 2 written twice through the alternating copies with `TFlagManager::save` data + a timestamped marker, unmount, read back,
  bookmark scan). Two runs in a row: first run creates the file and PASSes; the second reports the first run's marker from disk, then PASSes
  (`read-back matches`, `unmount -> 0`). Real in-game save-after-a-Shine was not driven (gameplay input path not reachable unattended yet).



# Rendering (Aurora build, w6)

Test runs: `SMS_PORT_SHOT` (now a GPU readback, R0) plus `SMS_PORT_WARP`, `SMS_PORT_PADSCRIPT` A presses to get past intro dialogs, `SMS_PORT_THP_SPEED=8` for the title.
Screenshots: `port/build_aurora/final/` (title 1800/2700, airstrip 1000/1400, plaza 1950/2550) and `port/build_aurora/stages/` (one run per area).

## R0. EFB readback: GPU screenshots and `GXPeekARGB` (handed over to w8)
Status: written and running in w6; handed over to w8 (EFB features owner) as is. w6 makes no further edits to these functions.
- **Files:** `port/src/port_aurora_sdk.cpp` ("EFB readback" block: `efbRequestRead`, `efbReadEncode`, `efbReadSubmitted`, `efbReadComplete`, `efbPeekARGB`, `port_efb_screenshot`, `port_VIConfigure`, `GXPeekARGB`, `GXPokeAlphaRead`),
  `port/src/port_main.cpp` (`shotListed`, `maybeScreenshotEfb`, called in `VIWaitForRetrace` right before `aurora_end_frame` of a presented frame),
  `port/tools/build_aurora.py` (`VIConfigure` -> `port_VIConfigure` rename; Dawn include dir and `-std=gnu++20` for the files in `PORT_SRC_CXX20`).
- **How:** only Aurora's public API (`aurora/gfx.hpp`), no Aurora patch.
  `resolve_pass({color})` drains the FIFO and snapshots the EFB colour target at the current point of the command stream; an encoder task (`push_encoder_task`) copies a rectangle into a MapRead buffer; its after-submit callback maps it.
  Results arrive 1-3 frames later.
- **Screenshots:** `SMS_PORT_SHOT=<frame>,...` reads the EFB back from the GPU (render-target size, 1280x960 BMP), so covered, minimised or off-screen windows and other running copies no longer spoil shots.
  With w9's present-on-`GXCopyDisp` pacing, a listed frame that presents nothing is captured at the next present (same image as on screen).
  `SMS_PORT_SHOT_WINDOW=1` restores the old window capture.
- **`GXPeekARGB(x, y)`:** each call queues a one-pixel readback at that point of the frame (logical EFB coordinates mapped like `AURORA_VIEWPORT_FIT`, logical size from the game's `VIConfigure`) and returns the newest completed value for that pixel, or for the last peeked pixel if it moved.
  `GXPokeAlphaRead` is honoured (the game sets `GX_READ_NONE` in `Application.cpp`).
  The caller is `TMario::drawSyncCallback` (occlusion probe: alpha 0x10 under Mario means visible); our `GXSetDrawSync` calls the callback synchronously, so the peek happens right after the probe cube, as on hardware, but its answer lags 1-3 frames.
  Not verified in gameplay yet (whether the probe's dst-alpha write survives Aurora's alpha-update handling is untested).
- `GXPeekZ` is Aurora's own (end-of-frame depth snapshot at 30 Hz); `GXCopyTex` is Aurora's own `copy_tex` (keyed by destination pointer); `GXCopyDisp`/`GXSetCopyFilter` are Aurora no-ops. w6 changed none of these.

## R1. Airstrip: Aurora FATAL `indexed XF load outside array 22`
- **Symptom:** see w7's entry above (Mario's `J3DShape::mVcdVatCmd`, stale bytes after the commands parse as `LOAD_INDX_B` index 0xD97E).
- **Cause:** port layer. On the GameCube `makeVcdVatCmd` fills the whole 0xC0 buffer: VCD 21 bytes + 12 arrays x (base + stride CP writes, 12 bytes, also for unused arrays via `GDSetArrayRaw`) + VAT 18 bytes = 183, padded to 0xC0 by `GDPadCurr32`, and `draw()` calls exactly 0xC0.
  On Aurora the buffer is 0x180 (`kVcdVatDLSize`) and an unused array writes only its stride, so the list ends early and `draw()` executed the uninitialised tail of the `new u8[]` buffer.
  Real hardware never sees such a tail, so this is not a GC tolerance question.
- **Fix:** `J3DShape::makeVcdVatCmd` (`libs/JSystem/src/J3D/J3DGraphBase/J3DShape.cpp`, `SMS_PORT`) pads the rest of the buffer with `GX_NOP` (0) after `GDPadCurr32`.
  GameCube DOL unchanged (`shasum` OK).
- **Result:** airstrip (`--warp 0,0`) runs through the Peach dialog into gameplay without a fatal (`final/airstrip_shot_*.png`).

## R2. Plaza/airstrip: WGSL `cannot index into expression of type 'f32'`
- **Symptom:** the WGSL line was `prev.a = clamp(select(0.0, ..., round(tev_overflow_f32(tevreg1.a).r * 255.0) > round(sampled0.a.r * 255.0)) + 0.0, 0.0, 1.0);` (Dawn prints the source line, so no `EnableDebugPrints` build was needed).
- **Cause:** Aurora shader generator. For an alpha combiner in compare mode `GX_TEV_COMP_R8_*`, `GR16_*` or `BGR24_*`, `tev_op` swizzles `.r`/`.rg`/`.rgb` on its A/B inputs, but the alpha path passed the scalar alpha inputs.
  On hardware these alpha compare modes compare the **colour** combiner's A and B inputs (only `A8` = `RGB8` compares alpha); Dolphin's `tevin_a`/`tevin_b` carry the colour inputs in rgb and the alpha input in a.
- **Fix:** Aurora patch `port/patches/aurora_shader.patch` (`lib/gx/shader.cpp`): for those ops the colour stage's A/B inputs are captured into `tevN_ca`/`tevN_cb` before the stage's colour op writes its output register, and the alpha compare uses them.
- **Result:** Delfino Plaza (`--warp 1,0`) loads and plays (`final/plaza_shot_2550.png`).

## R3. Warps 4,0 (reported by w10), 8,0 (Pianta Village) and 9,0 (Noki Bay): Aurora FATAL `unhandled tcg src 21`
(In w6's runs warp 3,0 is Ricco Harbor and 4,0 Gelato Beach.)
- **Cause:** not a misread value. 21 is `GX_MAX_TEXGENSRC`, Aurora's "never configured" default for texcoords at or beyond `numTexGens` (only the first `numTexGens` texgens are copied into the shader config).
  The failing state (logged now): one TEV stage, `tc0 map0` with indirect matrix `GX_ITM_0`, indirect stage 0 on texcoord 1, but `numTexGens` = 1.
  That is the JPA indirect-texture particle setup (`JPADrawSetupTev::setupTev` case 1, a matching unit) with the texgen count lowered later in the same state; the GameCube draws it the same way.
- **Fix:** Aurora patch (same `aurora_shader.patch`): a sampled texcoord that was never generated reads (0, 0), the hardware behaviour as modelled by Dolphin, with a warning that lists the stages.
- **Result:** warps 8,0 and 9,0 run (`stages/a8_*`, `stages/a9_*`); 4,0 ran 3000 frames without the warning in this build's run.

## R4. Visual survey (D3D12, 1280x960)
- Title stage (3D Delfino behind the logo: sky, sun with rays, clouds, seagulls, sea): correct; the `BOOTLOG` "title stage went black" note above is obsolete (it was the black alternate frames fixed by w9, plus window-capture shots).
- Airstrip: Peach, Mario (blue overalls, red shirt, white gloves), the plane, its projected shadow, sea, sand, goop puddle with the swirl texture, coins HUD: correct.
  The "overalls black" from the early gxtest does not happen in game.
- Plaza, Bianco (2,0), Ricco (3,0), Gelato (4,0), Pianta (8,0), Noki (9,0): geometry, textures, transparency, water and HUD look right.
  The first frames after a stage loads miss geometry while Aurora compiles pipelines asynchronously (test runs start with an empty pipeline cache); later frames are complete.
- Not yet compared closely with real-hardware captures: lighting intensity of night/dusk stages (Pianta looks bright), Mario's dirt layer (`H_ma_rak` swap; the striped overalls seen once were the spin-jump effect), FLUDD's water tank (likely uses EFB copies, w8).
- Intermittent, not rendering: `D3D12 create graphics pipeline state failed with E_OUTOFMEMORY` (seen in Sirena 6,0, 7,0 and once in 8,0, while other agents were running several game copies on the same GPU; not reproduced in a rerun of 8,0).
  JAudio crashes (w5) stopped after taking w7's JASChannel/JASSeqParser fixes (now in main).



## W9-2. Settings file, controller semantics, keyboard, window, overlay
New files: `port/src/port_settings.h/.cpp`, `port_input.cpp` and `port_window.cpp`, all inside `#ifdef SMS_PORT_AURORA`.
`port_main.cpp` gets four one-line hooks, each marked `w9:`:
- `port_settings_configure(&config)` before `aurora_initialize`;
- `port_window_frame()` in both branches of `VIWaitForRetrace`;
- `port_input_read()` in `port_PADRead`.

- **Settings:** `port_settings.ini` next to the exe (`SMS_PORT_SETTINGS=<path>` overrides it).
  It is created with commented defaults.
  Window size, position, maximized and fullscreen are saved 1 s after they change, and at exit.
  A saved position on no connected display is ignored.
  Before this, the window always opened at (0,0): `AuroraConfig` was zeroed, and Aurora treats 0 as a real position.
- **Sticks:** Aurora adds a 24% per-axis dead zone before `PADClamp`, which snaps the stick to the axes within about 14・ゑｽｰ.
  It is now `[input] stick_deadzone`/`cstick_deadzone`, default 0, so only the GameCube's own `PADClamp` dead zone (15 counts) applies.
  The DualSense rests at ・ゑｽｱ4, which `PADClamp` turns into 0 (`SMS_PORT_PADLOG=1` shows raw and clamped values).
  Aurora's `PADClamp` already scales a full ・ゑｽｱ127 to the GameCube octagon (72 on the axes, 40/40 on the diagonals).
- **Analog L/R:** Aurora maps the trigger axis onto 0..255, so the clamped analog value (0..150) saturated at 70% of the pull.
  The rest, up to Aurora's emulated click at 95%, did nothing.
  With `linear_triggers` (default), the travel up to the click maps linearly onto raw 30..180, i.e. 0..150 after `PADClamp`.
  The click itself is the digital L/R at full analog, as on a GameCube pad.
- **Buttons:** with `layout = gamecube` (default), `PADSetDefaultMapping` is called before `PADInit` for PS3/PS4/PS5/standard/Xbox pads.
  Buttons go where a GameCube pad has them: PlayStation Cross A, Square B, Circle X, Triangle Y, R1 Z, L2/R2 analog L/R, Options Start.
  Xbox: A A, X B, B X, Y Y, which was already Aurora's Xbox layout.
- **Rumble:** goes through Aurora's `PADControlMotor` to SDL rumble; `[input] rumble` sets the strength (default 50%, Aurora's own default).
- **Keyboard pad** (on top of a controller; controller 1 when none is connected):
  - WASD = main stick; Left Shift held = soft (45% tilt, and light analog L/R without the click);
  - arrow keys = C-stick;
  - Space = A, E = B, Q = X, F = Y, Z = Z, C = L, R = R, Return = Start (not while Alt is held);
  - I/J/K/L = D-pad.
  Every key can be remapped in `[keyboard]` with SDL key names.
  `SMS_PORT_KEYSCRIPT=<frame>:<key>[+key],...` holds keys for 10 frames, for unattended tests.
- **Window:**
  - Alt+Enter / F11 toggles borderless fullscreen; F3 toggles the overlay.
  - `aspect = 4:3` (default): the windowed client area is locked to 4:3 and the picture fills it.
  - When the client area is not 4:3 (fullscreen, maximized), Aurora's FIT policy letterboxes, at the EFB's 640:448 shape.
  - `port/patches/aurora_display_aspect.patch` (for w6) implements Aurora's declared-but-missing `VILockAspectRatio`.
    The port already calls it through a link-time fallback (`/alternatename`), so once the patch is applied, fullscreen is exactly 4:3 too.
  - `internal_scale` goes to `VISetFrameBufferScale` (0 = window resolution, 1 = native 640x448, ...).
- **Overlay (F3):** text through Aurora's ImGui foreground draw list, showing game fps, present interval (avg/max) and VI rate.
  ImGui is declared locally, because imgui.h is not on the port include path.
- **Frame statistics:** `SMS_PORT_FRAMESTATS=<seconds>` or `[debug] frame_stats` prints the VI rate, main-thread retrace gaps and present intervals.


## W9-3. Frame pacing measurements
- **Windowed, title/attract loop, 10 s windows:** VI 59.940-59.942 Hz (no drift), 29.97-30.07 presents/s.
  Present interval 33.3 ms, sd about 1.0 ms, max about 36 ms; the only spikes (50-67 ms) happen during stage loads.
- **Precise sleep:** `sdk_os_thread.cpp` `irqThread` now sleeps 1 ms less than needed and spins (`SwitchToThread`) for the rest (marked `w9`).
  Before, a 1 ms late wake-up delayed the retrace.
  Main-thread retrace gap max: 20.1-21.6 ms before, 18.9-20.0 ms after (ideal 16.68).
  Cost: about 1 ms of spinning per retrace on the interrupt thread.
- **Minimized** (`ShowWindow(SW_MINIMIZE)` for 8 s): the game keeps running at full speed, about 30 presents/s, and the frame counter keeps pace with the retrace count.
  Jitter is higher while minimized (present sd about 5 ms, a few 30 ms retrace gaps).
  It never speeds up or slows down, and nothing is logged.
- **Unfocused:** keeps running; Aurora's `pauseOnFocusLost` is off.
  The controller is ignored while unfocused unless `[input] background_input = 1` (SDL default).
- **High-refresh monitors:** the game is paced by the VI interrupt (59.94 Hz), not by vsync.
  Presents use Dawn's Mailbox mode (vsync off, no tearing), so the game cannot run faster on a 144 Hz display.
  On a 60 Hz display, 59.94 vs 60 Hz repeats one frame about every 17 s.


## W9-4. Test runs no longer open the player's controller
`SMS_PORT_NO_PADS=1` turns off every SDL joystick backend (HIDAPI, RawInput, WGI, DirectInput, XInput, GameInput) and ignores all gamepads (`SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT`).
It is set in `port_settings.cpp` before SDL starts.
Test instances then neither read nor rumble the player's pad; the keyboard, `SMS_PORT_KEYSCRIPT` and `SMS_PORT_PADSCRIPT` still work.
`build_aurora.py --run` sets it unless `--pads` is given.
Checked: with it set, the DualSense is no longer added; without it, it is.


## W9-5. Disc images without extraction (`port_disc.cpp`, hook in `sdk_dvd.cpp`)
- **Formats:** reads the US disc in place from `.iso`/`.gcm` and from CISO (`.ciso`).
  It parses the disc header (game ID, magic 0xC2339F3D, FST offset/size at 0x424) and the FST itself, so entry numbers are in disc order.
  Reads past a file's end return the following disc bytes, as on hardware.
  RVZ/WIA/GCZ are recognised, and the message says how to convert them to ISO in Dolphin; RVZ needs zstd/LZMA plus Dolphin's junk-data generator, so it is left for later.
- **Region check:** the game ID must be GMSE01.
  Other discs get a message box that names the region (Japan GMSJ01, Europe GMSP01, other GameCube game).
  Extracted folders are checked through `sys/boot.bin` next to `files/` when it exists.
- **Search order:**
  1. a path on the command line;
  2. `%SMS_PORT_DISC%`;
  3. `[game] disc =` in port_settings.ini;
  4. auto-detection: next to the exe (folder, or `*.iso/*.gcm/*.ciso`), `./files`, then a walk up from the exe for `dolphin_test\files` and `files`.
  A disc named explicitly (1-3) that is unusable stops the search with its reason, instead of silently picking something else.
  If nothing is found: a message box explaining what is needed, then SDL's file-open dialog; the choice goes to `disc =`.
- **`sdk_dvd.cpp` (w7) hook:** under `SMS_PORT_AURORA`, `build()` asks `port_disc_locate()`, and `doRead()` reads through `port_disc_read()` for images.
  `Entry` gains `discOffset`. `build_port.py` builds are unchanged.
- **Checked:**
  - booted to the title from `orig/GMSE01/game.iso` and from a CISO made from it (`make_ciso.py` in my scratchpad);
  - the folder path still works;
  - the JP ISO on the command line shows the region message box (`port/build_aurora/w9_wrong_region_box.png`);
  - with no data at all, the "not found" box appears.
  The file dialog itself (after clicking OK) was not clicked through.


## W9-6. In-game settings menu (`port_menu.cpp`)
- **Opening:** F1, or the controller's Back/Create/View button, toggles a Dear ImGui window with tabs:
  - Display: fullscreen, aspect, internal resolution, overlay;
  - Controller: layout, dead zones, linear triggers, rumble, background input;
  - Keyboard: enable, soft tilt, and every binding with Rebind (Esc cancels, Backspace clears);
  - Game: disc path.
- **Applying:** changes apply at once, except the button layout, which applies at the next start.
  They are saved to port_settings.ini.
- **Input:** while the menu is open the game sees idle controllers, and ImGui keyboard/gamepad navigation is on.
- **Code:** ImGui comes from Aurora's build (`imgui-src` on the port include path, `IMGUI_USER_CONFIG` as Aurora builds it).
  The F3 overlay now uses the real ImGui API from here too (`port_window.cpp` no longer declares ImGui itself).
- **Checked:** screen capture of the open menu at the intro movie (`port/build_aurora/w9_settings_menu.png`).
  `SMS_PORT_SHOT` now reads back the EFB, so ImGui is not in those captures.


## W9-8. RVZ, WIA and GCZ disc images (`port_disc_wia.cpp`)
- **What it reads:** Dolphin's compressed formats, read as the plain disc.
  - RVZ: zstd or uncompressed; any chunk size; per-group stored/compressed flag; RVZ packing with regenerated padding.
  - WIA: none, purge, bzip2, LZMA or LZMA2 (RVZ also LZMA/LZMA2/bzip2).
  - GCZ: zlib, including stored blocks.
  `port_disc.cpp` tries it before the ISO/CISO path, so the region check, FST and search order are unchanged.
  `*.rvz/*.wia/*.gcz` next to the exe are auto-detected, and the file dialog lists them.
- **Written from the format description**, not from Dolphin's code:
  - file head, disc struct, raw data and group tables (stored compressed);
  - groups covering the disc from each raw entry's offset rounded down to 0x8000;
  - RVZ group flags and packed size; literal/padding records.
- **Padding generator:** Nintendo's lagged Fibonacci generator (521/32 lags, 17-word seed, byte 1 taken 18 bits down, 4 steps before output).
  It is reseeded at every 0x8000-byte disc block, so a record at disc offset o starts (o % 0x8000) bytes into its stream.
  Before writing the C++ reader I checked my own Python version against the real padding of game.iso:
  - the seed recovered at 40 whole blocks regenerates each block byte for byte, and does not continue into the next block (reseeding confirmed);
  - a padding run starting mid-block regenerates with the in-block offset as skip, and not with a wrong skip (control).
- **Libraries, all static, no new DLLs:**
  - zstd 1.5.7 and zlib from Aurora's build (already linked);
  - bzip2 1.0.8 sources, unmodified, in `port_deps/bzip2-1.0.8`, copied from the local Cargo registry; `build_aurora.py` compiles them (`BZ_EXPORT`).
- **LZMA/LZMA2:** liblzma from xz 5.8.4 (`port_deps/xz-5.8.4`, official release tarball, SHA-256 checked, see its SOURCE.txt).
  Only the raw LZMA1/LZMA2 decoder is compiled (`common.c`, `filter_common.c`, `filter_decoder.c`, `lz_decoder.c`, `lzma_decoder.c`, `lzma2_decoder.c`).
  The configuration comes from `build_aurora.py` defines (`HAVE_DECODER_LZMA1/2`, `LZMA_API_STATIC`, ...); nothing from the tree is run.
  `PORT_DISC_HAVE_LZMA` turns on `lzma_raw_decoder` with the properties from the disc struct.
  DolphinTool-made WIA LZMA, WIA LZMA2 and RVZ LZMA2 (128 KiB) images of game.iso are IDENTICAL over the full disc (`test_disc.py --full`).
  The game boots to the title from the RVZ LZMA2 image; LZMA decodes at about 30-45 MB/s, so boot loads take a few seconds longer than with zstd.
- **Test:** `port/tools/test_disc.py` builds `port/tools/test_disc.cpp` with the reader.
  It compares every FST file and, with `--full`, the whole disc in 4 MiB steps plus 3000 random reads against the ISO.
  Test images were made from `orig/GMSE01/game.iso` with the PC's own DolphinTool (`<project>/Documents\Dolphin-x64`).
  All of these are IDENTICAL over the full 1,459,978,240 bytes:

  | Image | Notes |
  |---|---|
  | RVZ zstd 128 KiB | 1.07 GB |
  | RVZ uncompressed 32 KiB | packing only |
  | WIA bzip2 2 MiB | |
  | WIA none | |
  | GCZ 32 KiB | |
  | The user's own `Godzilla - Destroy All Monsters Melee (USA).rvz` (zstd) | compared with DolphinTool's ISO conversion of it |

  The game boots to the title from the zstd RVZ.
  `SMS_PORT_DISCSTATS=1` showed 0.086 s spent in the reader over about 3,800 boot reads.
  The roughly 3-4 s hitch when the title stage loads happens with the extracted folder too (4.3 s there), so it is not the reader.
  The test images were deleted afterwards.

## W9-9. Input recording and replay (`port_replay.cpp`), determinism fixes
- **Recording** (default on; `[debug] record`, `SMS_PORT_RECORD=0/1`, F1 menu Game tab) runs from boot.
  - The pads as the game reads them (stored only when they change); the clamped values follow from the game's own `PADClamp`.
  - The `srand` seed.
  - Every main-thread `OSIsThreadTerminated` answer (loader threads) and `THPPlayerGetState` answer (movie end).
  - A card snapshot (all files in `<user dir>/USA/Card A`) and the disc identity (game ID, header/FST hash and file name, no paths).
  - The debug settings that change the game (`SMS_PORT_WARP`, `SMS_PORT_THP_SPEED`, `SMS_PORT_NODRAW`), re-applied on replay.
  - Mario's position, action and the rand state every 120 game frames (checkpoints).
  Every event carries its game frame (the PADRead count).
  About 2 KB per minute of play; capped at 64 MB.
- **Whole session, not a ring buffer:** the brief asked for the last N minutes, but a replay can only start from boot (no state snapshots), so the whole session is kept.
- **Saving:** F9, the menu button, a crash (a chained unhandled-exception filter) or an abort/Aurora FATAL (SIGABRT) writes a zstd-compressed `sms_replay_<date>_<time>.bin` next to the exe, i.e. next to `sms_port_crash.log`.
- **Replay:** `SMS_PORT_REPLAY=<file>` does the following:
  - restores the card copy into `%TEMP%\sms_replay_<pid>` (removed at exit) and points `SMS_PORT_USER_DIR` there;
  - ignores physical controllers and the keyboard, and never saves port_settings.ini;
  - feeds the recorded answers in order. A recorded "loader finished" that comes early is safe: every caller joins the thread next;
  - compares the checkpoints and prints the first differences; at the end it prints "N of M checkpoints differ";
  - `SMS_PORT_REPLAY_EXIT=1` exits at the end with status 0 when nothing differs.
- **Nondeterminism found and fixed:**
  1. **rand/srand:** the game's calls went to the host UCRT `rand`, which is a different LCG from MSL's and keeps per-thread state.
     The game objects now call `port_rand`/`port_srand` (renamed in `build_aurora.py`): MSL's single global generator (x * 0x41C64E6D + 12345, bits 16..30).
     This is also a GameCube-fidelity fix: random sequences now match the original's.
  2. **Load-screen length:** the game polls its setup threads with `OSIsThreadTerminated` once per frame, so how many frames a load lasts depends on the PC. Journaled.
  3. **Movie length:** THP movies advance with the VI retrace, and the movie director polls `THPPlayerGetState` per frame. Journaled.
  4. **EFB readbacks (port_efb.cpp, w8's file, marked w9):** results arrived "1-3 frames later" depending on GPU timing.
     Bianco Hills' goop collision reads them, so the first replay matched for 1700 game frames and then drifted by 2-15 units (same rand state).
     While recording or replaying, results are now applied exactly 3 game frames after the request, in request order, waiting for the GPU (pumping Dawn's `ProcessEvents`) if it is later.
  5. **Bug found on the way:** a replay used to switch `keyboard` off in the settings, and the exit-time save stored it. Replays now never touch the settings.
  6. **GXPeekARGB (port_aurora_sdk.cpp, w6's file, marked w9):** Mario's occlusion probe (draw-sync callback) feeds the camera; a recording diverged in water at game frame 1800. While recording/replaying, peek N now returns peek N-3's result, waiting for the GPU if needed.
  The EFB delivery counts w8's game-frame clock (`sGameFrame`, E18), so interpolated display frames (W9-10) do not shift it.
- **Test:** `SMS_PORT_WARP=2,0` (Bianco Hills), `SMS_PORT_THP_SPEED=8`, scripted input (Space through the intro, then about 2 minutes of walking in four directions with jumps and FLUDD spraying, F9 at the end).
  The recording is 1.7 KB for 4560 game frames.
  - Replay: 0 of 37 checkpoints differ; Mario's exact float position, action and rand state are equal at every checkpoint.
  - Replay with 24 CPU-burning processes running: again 0 of 37. Only the retrace counts differ (9109 vs 9112).
  - The final build replays the same file with 0 of 37.
  - w7's `SMS_PORT_TRACE` lines sample on retrace buckets, which shift between runs, so they cannot be compared line by line; the checkpoints are keyed on game frames.
  - Card snapshot: a file in the card folder is captured and restored into the replay's folder.
  - Not exercised: the crash path, and replays across different builds or PCs (a GPU-dependent result other than the EFB readbacks would show up as a checkpoint difference).
- **Overhead:** recording costs nothing measurable. The Bianco load gap (frame 600 at retrace about 1000) is the same with recording off.

## W9-10. Frame-rate option: interpolated display frames (`port_interp.cpp`)
- **Setting:** `[window] frame_rate = 30 | 60 | 120 | 144 | display | unlimited` (default 30), F1 menu Display tab "Frame rate"; `SMS_PORT_FPS=<rate>|display|unlimited` overrides it for one run (tests). The F3 overlay now shows display fps and the game's own rate ("logic 29.97 Hz"); the frame-stats log line has "logic frames N/s".
- **The game logic is not touched:** `direct()` still runs its two simulation ticks per call, once per two 59.94 Hz retraces, and a logic frame's picture is presented at its retrace as before. 30 = exactly the old path (no snapshots, no extra frames).
- **How a logic frame is drawn (found on the way):** the last tick of `TMarDirector::direct()` ends with the perform list `unk34` (CALC_VIEW | ENTRY: camera matrices, J3D `viewCalc` of every model, draw-buffer entries) for the new state; the *next* `direct()` call starts with the draw lists, which only render those buffers. A first attempt that re-ran just the draw lists showed the next logic frame's picture (everything is baked in at entry time).
- **Extra display frame** (`port_mardirector_redraw`, `src/System/MarDirectorDirect.cpp`, SMS_PORT):
  1. the view half of `unk34` (`CUE_CALC_VIEW | CUE_SET_PROJECTION`, no entries) with the camera matrices and every J3D model's joint/envelope world matrices replaced by `lerp(S_s, S_s+1, alpha)`; the logic frame's draw-buffer entries stay, and J3D packets read their model's current draw matrices when drawn (`J3DShape` via `mCurrentViewNo`);
  2. the GX draw lists (`unk40`, `mPerformListGX`, silhouette, `mPerformListGXPost`), not `unk38`/`unk3C` (pollution layers + graffiti check, bath-water preprocessor: passes that accumulate or are read back);
  3. GXDrawDone (Aurora reads indexed matrices and arrays on its FIFO thread), then the view half once more with the live values: every model and object is back to what the logic `unk34` left, each model's double-buffered draw matrices on the same buffer;
  4. the screen fader as `gameLoop` draws it, without `TSMSFader::update` (`port_app_redraw`, `src/System/Application.cpp`).
  A second attempt re-ran the whole `unk34` (with entries): packets entered twice before a draw-buffer reset made cyclic J3D packet lists, and the next logic frame overflowed Aurora's 2 MB index buffer (abort a few seconds into Bianco Hills). The view-only passes fixed it.
- **Snapshots:** each logic `unk34` pass stores the camera's view/projection matrices and, through the `J3DModel::viewCalc` hook (`libs/JSystem/.../J3DModel.cpp`, SMS_PORT), every model's matrices, three states deep (frames due between a logic draw and its retrace still need the older pair). A model viewCalc'd twice in one pass with different matrices is instanced and never interpolated. Entries not seen for 6 passes are dropped.
- **Timing:** state S_s's picture is up from P_s, the first retrace after its logic draw; a frame at time t shows `alpha = (t - P_s) / tick` (tick = measured logic period, ~33.37 ms). Original latency. Frames are drawn ahead by their recent maximum cost so they are presented at their due time; a frame that would not finish before the next retrace waits (the retrace starts the next logic frame); the slot of a logic frame's own retrace is left to it. 60 and 120 are locked to the VI period (60 = one frame on every retrace: the logic pictures plus one interpolated frame in between). Unlimited still sleeps 1 ms per frame so lower-priority game threads (memory card, loaders) run.
- **Cuts and teleports:** the camera snaps when it moves > 1000 units or turns > ~45 degrees in one logic frame, or the projection changes kind or fov by > 30 %; a joint snaps when it moves > 500 units or a basis axis turns > 60 degrees.
- **No game state changes from a display frame:** camera and model matrices restored; shadow requests (consumed by the next logic draw) saved/restored; sound effects gated (`mSeGateMask = None`); `rand()` on the main thread draws from a scratch copy (`port_rand_scratch`, port_replay.cpp); w8's `port_efb_set_presentation_only(1)` (E18: GPU-only copies, game-frame peeks/counters, no readbacks); draw-sync callbacks (Mario's occlusion probe, sun peeks, pollution counters) are not called (`port_aurora_sdk.cpp` `GXSetDrawSync`, marked w9); `port_GXCopyDisp` ignores the redraw's display copy; `port_efb_end_frame` runs only for a frame holding a logic draw. If a display frame falls between a logic draw and its retrace, the EFB is cleared (`port_efb_clear_all`) and the frame replaces the logic picture.
- **Not interpolated (30 Hz steps, drawn at the newer state):** particles, FLUDD water, shadows, non-J3D effects, 2D/HUD, mirror reflections. Instanced models (one J3DModel at several places) step too.
- **Files:** new `port/src/port_interp.cpp`; game side (all `#ifdef SMS_PORT`, GameCube `shasum -c` OK): `src/System/MarDirectorDirect.cpp`, `src/System/Application.cpp`, `libs/JSystem/src/J3D/J3DGraphAnimator/J3DModel.cpp`. Others' files, marked w9: `port_main.cpp` (w7: `VIWaitForRetrace` waits with an `OSAlarm` and draws due frames, `displayFrame`, `SMS_PORT_SHOT_DISPLAY`, retrace time stamp, `port_GXCopyDisp` guard), `port_efb.cpp` (w8: `port_efb_clear_all`), `port_aurora_sdk.cpp` (w6: draw-sync callbacks). Both were re-synced from main (w8's E18 presentation-only hook and game-frame clock) and W9-9's deterministic delivery re-applied on top: requests carry `sGameFrame` and are applied 3 game frames later; the deterministic GXPeekARGB returns its last value in presentation-only frames. w9: `port_settings.*`, `port_menu.cpp`, `port_window.cpp` (overlay/logic rate, `port_window_display_frame`), `port_replay.cpp` (`port_rand_scratch`, stronger checkpoints).
- **Evidence** (Bianco Hills, scripted input, other agents' builds running on the same PC):
  - **Same game state at every setting:** replay checkpoints now also hash every live actor's position and live flags and the camera's position, view and projection matrices (`E_CHK2`, 28 bytes; old files still compare Mario + rand). One session recorded at 30 (`sms_replay_20261008_185648.bin`), replayed at 30, 60 and unlimited (after the merge onto main's port_efb/port_aurora_sdk): 0 of 37 checkpoints differ each time. Before the merge the same check at 30, 60, 144 and unlimited: 0 of 37 each.
  - **Frame times** (`SMS_PORT_FRAMESTATS`, steady gameplay windows): 30: 29.97 presents/s (sd 3.3 ms); 60: 59.94/s, interval 16.68 ms, sd 1.3 ms, logic 29.97 Hz; Match display (60 Hz monitor): 59.94/s; 144: 125-138/s (sd 3-4 ms); unlimited: 170-200/s; logic 29.9-30.0 Hz in all. A display frame costs ~4 ms of main-thread time: 1.2 ms view pass + draw lists, 2.5 ms waiting for Aurora's FIFO thread to process them (needed before the restoring pass), 0.1 ms restore + fader, the rest present. So 120/144 reach ~100-138 fps on this (shared) machine; at 144 many logic pictures are replaced by interpolated frames.
  - **Interpolation visible:** 60 Hz captures (`SMS_PORT_SHOT_DISPLAY`) alternate logic/interpolated frames with even differences (mean abs diff ~22 between neighbours instead of pairs of identical images); the interpolated frame shows Mario's pose and the camera between its neighbours.
  - **Cut:** Delfino Plaza intro skipped with A (iris wipe to Mario's camera, camera jumps 7474 units): the frame at the cut is flagged CUT and shows the new camera (diff to the following logic picture 0.10, no in-between smear); the wipe and HUD are drawn in display frames.
  - **Overlay:** F3 shows e.g. "99.9 fps 10.0 ms (max 24.9) logic 30.0 Hz VI 59.95 Hz" (120 setting, window capture).
  - GameCube build: `shasum -c` OK after the game-side changes.
- **Coordinator rule (E18) on accumulating passes:** pollution stamp/decay/copy (`unk38`/`unk3C`) and the bath-water preprocessor (in `unk3C`) are skipped. `TModelWaterManager` has no EFB copy; its view work (`calcDrawVtx`/`calcVMAll`, splat matrices) is recomputed from the particles each pass and its double-buffered quad list ends on the same buffer after the two view passes, so nothing accumulates; Aurora clears the EFB per frame, so no EFB-feedback build-up exists here either.
- **Debug knobs:** `SMS_PORT_INTERP_LOG=1` (a line per frame: alpha, states, models interpolated, camera movement, "over logic", "CUT"), `SMS_PORT_SHOT_DISPLAY=<frame>:<n>` (the next n presented frames as `disp_NNN_L/I.bmp`), `SMS_PORT_INTERP_NORESTORE=1` (skips the restoring pass; diagnosis only).

## W9-11. Frame-rate option rebased onto main (2026-10-08 evening)
- **How:** `agent_sync.ps1 -Worker w9 -Port` (records main's port/ as the base), then every file w9 touches rebuilt as main's current version plus only w9's hunks (hunk-by-hunk, main's side kept for the other agents' changes):
  - `port_main.cpp`: kept w13's `port_disc_early_handoff` and w8's `SMS_PORT_SHOT_PRESENT` / `port_present_screenshot`; re-applied the `VIWaitForRetrace` pacing loop (OSAlarm), `displayFrame`, retrace time stamp, `SMS_PORT_SHOT_DISPLAY`, `port_GXCopyDisp` guard.
  - `port_efb.cpp`: on top of w8's readback pooling and per-key coalescing (`keyBusy`, `sOutstanding`, `JobBuffers`): deterministic delivery (`detFrame`, `sDetQueue`, `detEndFrame`) and `port_efb_clear_all`. New: `keyBusy` never coalesces while recording/replaying (whether a request is made must not depend on the GPU's pace); a det job still balances w8's outstanding counters before `detEndFrame` takes it.
  - `port_aurora_sdk.cpp`: kept w8's present capture and the rounded-down screenshot size; re-applied the deterministic `GXPeekARGB` and the draw-sync suppression in presentation-only frames.
  - `port_disc.cpp/.h`: on top of w13's regions: replay identity without paths (file name only).
  - `build_aurora.py`: on top of `--region`: the rand/srand/OSIsThreadTerminated/THPPlayerGetState rename block and the README frame-rate/F9 text.
  - `MarDirectorDirect.cpp`: kept main's `VERSION_GMSP01` balloon fix (w13); `Application.cpp`, `J3DModel.cpp`, `port_settings/menu/window/input`: w9 hunks only on top of w6's graphics settings.
- **w6's VSync and the frame rate are one setting now:** the VSync checkbox sits under "Frame rate" in the Display tab; with VSync on, the display-frame period is capped at the monitor's refresh (`periodMs`, `port_interp.cpp`), so 120/144/Unlimited never draw frames that would only block in the present; the menu says "(VSync caps the frame rate at the monitor's N Hz)" and the ini comment says so too.
- **Found on the way:** `SMS_PORT_REPLAY_EXIT=1` ended in `abort()` (exit 0xC0000409): `std::exit` ran the atexit destructor of Aurora's joinable `g_pipelineCacheWriterThread`. It now calls `aurora_shutdown()` first, like the window-close path; exit status 0 again.
- **Checks (all on this rebased tree):**
  - Hooks present: `port_efb_end_frame`, `port_sweep_pad_apply`, `port_window_frame`, `port_input_read`, the `RA<0-255>` padscript token, `port_disc_early_handoff`, `SHOT_PRESENT`, w6's msaa/anisotropy/fxaa/vsync, `copy_pipeline_seed`, `SANITIZE`, `--region` (same counts as main, plus w9's).
  - `build_aurora.py`: 622/622, 0 undefined. GameCube: `shasum -c` OK.
  - Replay determinism: a new recording at 30 (Bianco Hills, scripted keys, 4602 game frames) replayed at 30, 60, unlimited and 144 + VSync: 0 of 37 checkpoints differ each time. 60: 59.9 presents/s, logic 29.95-29.99 Hz; unlimited: ~345 presents/s, logic 30; 144 + VSync on a 60 Hz monitor: 59.94 presents/s (capped), logic 30.
  - `smoke_all.py --quick`: PASS 20, FAIL 0, STUCK 0.
  - `merge_check.py w9 --all`: 15 worker-only files + 3 new, no BOTH/STALE/CONFLICT.

## W9-12. DualSense: analog triggers checked, adaptive triggers, lightbar (`port_padfx.cpp`)
- **Test controller without touching the player's pad:** `SMS_PORT_VPAD=ps5|ps4|xbox` attaches a virtual controller through SDL's virtual joystick driver (DualSense 054C:0CE6, DualShock 4 054C:09CC, Xbox Series 045E:0B12).
  Every physical backend stays off as with `SMS_PORT_NO_PADS` (HIDAPI, RawInput, WGI, DirectInput, XInput, GameInput); only the virtual VID/PID passes `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT`.
  `SMS_PORT_VPADSCRIPT=<from>[-<to>]:<ctl>[=<v>][+...]` drives it (LT/RT 0..1, LX/LY/RX/RY, CROSS/SQUARE/CIRCLE/TRIANGLE/OPTIONS/CREATE/LB/RB/D-pad).
  Its callbacks log what SDL hands the controller: `[vpad] SetLED`, `SendEffect` (the 47-byte DS5 effect state), `Rumble`, `RumbleTriggers`.
  So everything up to SDL's HID report is exercised; the report itself (SDL's PS5/PS4 driver) and the feel are not.
- **(a) Analog L/R, end to end** (virtual DualSense, Bianco, `SMS_PORT_PADLOG`, `SMS_PORT_PADFX_LOG=2`): SDL trigger axis -> Aurora binding (value x255) -> `linearTrigger` -> game `PADClamp`.
  RT 0.05/0.10/0.15/0.20/0.25 -> raw 38/45/54/62/70 -> clamped 8/15/24/32/40 (linear, ~158 per unit of travel); RT 0.95 -> clamped 150 with digital R off; RT 1.0 -> digital R on (Aurora's click at 31151/32767 = 95.07%), analog 150.
  FLUDD: half press (RT 0.5: analog 79, R button 0) while walking: forward speed 27-30 with water coming out; full press (RT 1.0 + stick held): R button 1, forward speed 0 (planted aiming). Both as on a GameCube pad.
- **(b) Adaptive triggers** (DualSense only; `SDL_SendGamepadEffect` with SDL's 47-byte DS5 effect state, enable bits 0x04|0x08 only, so rumble and lightbar are untouched):
  - GameCube click: trigger mode 0x21 (feedback, 3-bit force per zone), zones 0-8 light, zone 9 (where the 95% click is) firm; strength 60% = `21 ff 03 49 92 24 21 ...`.
  - Full FLUDD feedback (R): slack (0x05) when the tank / Yoshi's juice is empty and while a trigger nozzle waits for the release (TNozzleTrigger DEAD); a pulse while spraying (0x26 vibration, 10 Hz, amplitude rising from zone 2); hover thrum (15 Hz); turbo buzz (45 Hz); rocket kick (30 Hz) at launch; resistance growing with the rocket/turbo charge. L keeps the click.
  - Updated once per game frame, sent only on change; logged in Bianco: click -> spray pulse at a half press -> click -> hover thrum while hovering -> slack (release) -> click.
  - Settings: `[input] adaptive_triggers = off|gamecube|fludd` (default gamecube), `trigger_strength` (60); F1 Controller tab. Triggers are reset to no effect when the window closes.
- **(c) Lightbar** (DualSense, DualShock 4; Aurora's `PADSetColor`, applied by its next `PADRead`; only on change):
  - Colour: the game's own water colour table `gModelWaterManagerWaterColor[TModelWaterManager::unk5D5F]` (0 = FLUDD water, 1-3 = Yoshi's juices; `TYoshi::mType` is copied there while riding), scaled to full brightness with the saturation raised to >= 80%: FLUDD (51,85,255), orange (255,169,2), purple (156,1,255), pink (255,51,142).
  - Brightness: remaining fluid (TWaterGun current / `getMaxWater()`; riding: TYoshi `unkC / unk8`, as the HUD gauge) through 0.03 + 0.97 * fill^2.2, and 0.012 at empty: FLUDD at 100/75/50/25/10/0% = (51,85,255) (28,46,139) (12,20,61) (4,6,19) (2,3,9) (1,1,3). Times the brightness setting.
  - Dim warm white (15,14,13) at 100% brightness without FLUDD (airstrip before the pickup: checked), in the pause/map/save menus and outside a stage (title, file select, movies).
  - Checked: Bianco spray drains the light 1.000 -> 0.915 in 1-step changes; Ricco 3,7 with `SMS_PORT_DEBUG_CALL=hatch@1500` and `SMS_PORT_PADSCRIPT=...1505-1600:G11000;1100,1540:A,...` (Mario lands on Yoshi at f1576): pink juice (251,50,140) at 0.993, dimming to (210,42,117) at 0.914 while Yoshi spits.
  - Settings: `[input] lightbar = off|fluid|static` (default fluid), `lightbar_brightness`, `lightbar_color`; F1 Controller tab.

## W9-13. HD rumble (`port_haptics.cpp`), Xbox impulse triggers
- `[input] rumble_mode = off|original|hd` (default original), strength = the existing `rumble`, categories `hd_fludd/hd_movement/hd_world/hd_damage`; F1 Controller tab.
- HD: `build_aurora.py` renames the game's `PADControlMotor` to `port_PADControlMotor`, which drops the game's on/off pulses on controller 1 (Original passes them on, Off stops the motor); nothing in the game changes.
  The engine reads, once per game frame: the game's rumble channels (`SMSRumbleMgr`: each active channel's type and this frame's curve value -> one voice per type: hip drop = heavy thud, damage = sharp hit, electric = buzz, MAPOBJ_APPEAR and the unnamed boss/event types = rolling rumble ...), sound-effect starts (a marked `SMS_PORT` hook in `JAIBasic::startSoundBasic` queues SE ids: footsteps by ground from the id ranges of `MSD_SE_MA_WALK_*` - stone, soil, sand/gravel, wood, metal, grass, carpet, water, mud/goop - left/right foot from bit 2, Yoshi steps, wall bumps, water entry/exit, slides, coins, red/blue coins, 1-up, Shine get, Yoshi gulp/juice), Mario's status (landing scaled by the fall speed, wall kick, dive, swim in/out) and FLUDD (spray buzz scaled by the analog R and thinning with the tank, rocket charge and launch, hover hum, turbo rattle).
- Backends: two motors (`SDL_RumbleGamepad`, a low/high mix per voice); Xbox impulse triggers (`SDL_RumbleGamepadTriggers`, FLUDD on the right trigger); DualSense HD haptics (W9-14). A GameCube pad on the adapter keeps the game's pulses (on/off motor).
- Checked (virtual pads, `SMS_PORT_HAPTICS_LOG=1`): footsteps (`step stone`, pairs per stride), landings (amp 0.4-0.6 for jumps), spray, hover; Xbox: `RumbleTriggers 0,12216` while spraying, falling slowly with the water.

## W9-14. DualSense HD haptics over USB and Bluetooth (`port_haptics.cpp`)
- USB: the 4-channel audio device of the controller (name contains "DualSense"/"Wireless Controller"), SDL audio stream at 48 kHz, channels 3-4 = actuators; with HD on, SDL rumble is set to 0 (its rumble emulation would switch the audio haptics off).
- Bluetooth: report 0x32 written with `SDL_hid_write` on the path SDL's driver uses (`SDL_GetGamepadPath`): seq<<4, 0x91/7 `FE 00 00 00 00 FF counter`, 0x92/64 haptic samples, padding, CRC-32 over 0xA2 + 138 bytes; every 32/3000 s on an absolute schedule (high-resolution waitable timer). No state sub-packet, so SDL's 0x31 reports (lightbar, triggers) interleave. Layout from SAxense (MPL-2.0) and LinuxAudio4Dualsense5 (AGPL-3.0), facts only; their two readings of the 64 bytes (stereo 3 kHz / mono 6 kHz) are both implemented (`SMS_PORT_HAPTICS_BT_LAYOUT=mono6k`).
- Tests never open a device; `SMS_PORT_HAPTICS_DUMP=<prefix>` (+ `SMS_PORT_HAPTICS_DUMP_END=<frame>`) records `<prefix>_bt.bin`, `_hd.wav`, `_speaker.wav`.
  Checked with an independent script (zlib CRC): 6188 reports, CRC 0 bad, sequence and counter unbroken, sub-packet headers as specified; the haptic payload follows the events (mean |s8| ~60 of 127 during spray/landings after the gain fix, 0 when idle).

## W9-15. DualSense speaker (`port_speaker.cpp`, libopus, tap in `port_audio_dsp.cpp`)
- `[audio] controller_speaker = off|fludd|fludd_pickups`, `main_mix = keep|duck|remove`, `speaker_volume`; F1 Audio tab.
- Which voices: once per game frame the SE track of every playing FLUDD/pickup sound (JAI `mSeTrack[cat][slot]`, `trackToSeqp`, child tracks) is keyed by its `TChannelMgr`; the mixer tap (w5's `port_audio_dsp.cpp`, marked w9) traces DSP voice i -> `TDSPChannel::DSPCH[i].mSign` (the TChannel) -> its channel manager. Tagged voices are captured from their front-bus contribution (game volume, pan, fades) and kept / ducked (x0.35) / removed in the main mix; the speaker stream advances one DSP sub-frame at a time (silence when nothing plays).
- USB: channels 1-2 of the 4-channel device. Bluetooth: report 0x36 (398 bytes: 0x91 config `FE 40 40 40 40 40 counter`, 0x90/63 state, 0x92/64 haptics, 0x93/200 Opus, CRC over 0xA2 + 394 bytes), 512 samples of the 48 kHz stream resampled to 480 per packet, Opus 48 kHz stereo 10 ms CBR 160 kbit/s (200 bytes). The state sub-packet restates our trigger effects, lightbar and (Original mode) motor so SDL's 0x31 and ours agree; speaker switched on first with an effect state (0xA0/0x80, volume 0x64, audio control 0x30, preamp 0x02) through `SDL_SendGamepadEffect`.
- libopus 1.6.1 float encoder compiled from `build_aurora.py` (own list, no scripts; `OPUS_BUILD USE_ALLOCA HAVE_LRINTF`); `opus_packet_get_nb_frames` (decoder-side helper the encoder calls) is a 10-line RFC 6716 TOC parser in `port_speaker.cpp`.
- Checked: dumped 0x36 stream decoded by a separate full-libopus tool: 6081 packets, 0 bad; decoded speaker audio -31 dBFS RMS while spraying, -53..-57 with FLUDD idle, silence otherwise. Main mix with `SMS_PORT_AUDIO_SE_ONLY=1`: keep ~-27 dBFS, remove ~-42 dBFS (the FLUDD sounds gone).

## W9-16. GameCube adapter, replay exit
- SDL 3.4.10 (Aurora's prebuilt) has the HIDAPI GameCube driver with its libusb/WinUSB backend built in; Aurora marks 057E:0337 as a GameCube pad (no trigger emulation, digital L/R from MISC3/MISC4, on/off rumble with the brake); the port leaves it alone (no linear-trigger remap, no rumble-intensity change, no lightbar/trigger effects, HD keeps the game's pulses). Windows needs the WinUSB driver once (Zadig), documented in the package README. Not tested on hardware (no adapter, and test runs keep every physical backend off).
- `SMS_PORT_REPLAY_EXIT=1` now ends with `TerminateProcess` after removing the card copy: after `aurora_shutdown` the AI interrupt thread used the destroyed audio stream (access violation at exit, seen once). 0 of 37 at unlimited and 30 with every new feature on, exit status 0.

## W9-17. Crash opening the guide map: .bti headers reverted to big-endian by a 2D-archive refill (`JKRMemArchive.cpp`)
- **Symptom:** Bianco Hills, jump + Z (Z opens the guide map; the ground pound is L in the air) -> access violation on Aurora's FIFO thread in `hash_texture_source`, reading 0xA0C80F00.
- **Found with** a temporary `GXInitTexObj` wrapper (renamed for game objects, logging images outside emulated RAM with a backtrace; removed afterwards): `TGuide::resetScore` -> `J2DPicture::changeTexture(unkC8[n]->getTexInfo())` -> `JUTTexture::storeTIMG` -> `GXInitTexObj(image 0xA0C80F00, 9216x9216, RGB5A3)`. The header at 0x80C80F00 was `05 02 00 24 00 24 ... 01 01 00 00 01 00 00 00 00 00 00 20`: `/guide/timg/coin_number_N.bti` (36x36, image offset 0x20) in big-endian order, marker byte 0x19 = 0 (never swapped, or swapped and then overwritten).
- **Cause:** the pause/HUD archive (`game_6`) and the guide archive share one fixed 2D buffer (`TMarDirector::unkD4`); `SMSSwitch2DArchive` / `SMSMountAramArchive` copy the raw archive image from ARAM into it again and `mountFixed` it. `TGuide::load` made its JUTTextures from `JKRGetResource` (swapped on that first fetch); opening the map refills the buffer with the raw image and calls `storeTIMG` on the same headers again, with no `getResource` that would swap them. On the GameCube the data is native, so this sharing is harmless there.
- **Fix (root, swap layer):** `JKRMemArchive::open(void* buffer, ...)` (the fixed-buffer mount) swaps every resource header in the image at mount time, as `getResource` would on first fetch (`port_swap_resource`: `.bti`, `.bfn`; idempotent by their markers). Covers the game_6/guide switch, the PAL language reload of `cmn2d`, and every other `mountFixed`.
- **Sweep:** the archives that are refilled in place are `game_6` (9 `.blo`, 108 `.bti`) and `guide` (1 `.blo`, 97 `.bti`, 1 `.bmg`): only `.bti` headers are swapped in place (`.blo` is parsed through big-endian streams, `.bmg` is never swapped). Other `mountFixed` users mount freshly loaded buffers before their objects are built. A quick smoke (20 scenes) with the debug wrapper logged no texture image outside emulated RAM.
- **Checked:** the Bianco repro runs on (guide drawn with its coin digits, HUD intact after closing it); 4 hip drops each in Bianco 2,0, Ricco 3,0, Gelato 4,0, Pinna 5,0, Sirena 6,0 and the plaza 1,1, with the guide opened and closed in each; GameCube `shasum -c` OK.

## W9-18. Quality-of-life tab (`port_qol.cpp`, F1 > Quality of life, `[qol]`)
- Every option is off by default (= the original). Options: skip boot logos + intro movie; Start skips any cutscene/movie; dialogue text speed normal/fast/instant; fast-forward while a key is held (Tab, rebindable; 2x/4x); auto-save after a Shine; continue in the level after a Shine; pause on focus loss; camera invert X / Y and speed 50-200 %; music / SFX / voice volume; blue-coin tracker and Shine checklist (pause screen, F4 toggles both any time); infinite lives / FLUDD water.
- **Replays:** the options that change the game (skip flags, text speed, auto-save, continue, camera, cheats, the fast-forward speed active this frame) are packed once per game frame (`port_qol_frame`, before `port_replay_pads`) into a 16-byte state; a recording stores it as event `E_QOL` (7) when it changes, a replay applies the recorded state instead of the player's settings. Older replays have no `E_QOL` and play as all-off. Volume, trackers and focus pause do not touch game state and are not recorded; focus pause is off during a replay.
- **Multiplayer (w10 `port_mp_active`)**: fast-forward, focus pause, cheats, auto-save and continue-after-Shine are forced off.
- **`PORT_SKIP_CUTSCENES` is now opt-in in the Aurora build**: the Start-skips-in-engine-cutscene code (`MarDirectorDirect.cpp` updateGameMode case 3/4, `CameraDemo.cpp skipDemoCamera`) runs only with "Start skips any cutscene" on; before it was always on in the PC build, which is not original behaviour.
- **Edits in other agents' files (small, marked w9):** `sdk_audio.cpp` (w5) `aiTick` returns early while paused for focus; `port_audio_dsp.cpp` (w5) scales a voice by `port_qol_voice_gain(tag)` when a volume is below 100 % (tags from `port_speaker.cpp`: SE id of the voice, 0 = music/stream), and the speaker capture now asks `port_speaker_wants(tag)` (the tag exists for every SE while volumes are active); `port_main.cpp` VIWaitForRetrace (pause loop, fast-forward); `port_interp.cpp` (no interpolation while fast-forwarding); `port_menu.cpp` (tab + `port_qol_draw`). **w5:** please keep the two hooks if you rework the mixer loop.

## W9-19. QoL game-side hooks (all `SMS_PORT_AURORA`, GameCube `shasum -c` OK)
- Skip intro: `TGCLogoDir::direct` returns `APP_STATE_DONE` at once; `TApplication` after the logo goes to area 15 (the title stage, `APP_STATE_GAMEPLAY`) instead of `APP_STATE_DONE` (Entrance.thp). (`APP_STATE_MENU` is the debug stage select, not the title.)
- Skip cutscenes: `MovieDirector.cpp` both `MSF_FMV_WATCHED_BASE` checks go through `PORT_QOL_MOVIE_SKIPPABLE(watched)`; the stage-intro flyover also lets Start skip the plaza 1,1 / 1,9 intros; in-engine demos as above.
- Text speed (`Talk2D2.cpp` openNormalWindow / moveTalkWindow): fast = the game's own A-held path (`unk26C = 1`, alpha step 0x80, no per-character pause); instant = alpha 0xFF, a whole line shown at once, and each line's slide-in snapped when the line before it is complete (the original resets a line to 1.0 while the previous one is unfinished, so snapping all lines at once left lines 2-3 empty; fixed).
- Auto-save (`MarDirectorDirect.cpp portAutoSave`): on `CARD_SAVE_PENDING` after a Shine (`unk261 == 6`): `getBookmarkInfos`; if the loaded file (`mSaveFile`) is valid and the card's copy is ours (not broken, same last-save time), `saveBookmark(file)` -> `saveSuccess` -> option block -> unmount, toast "Game saved"; any other case falls back to the normal prompt (`STATE_CARD_SAVE`).
- Continue after a Shine: at `SHINE_GET_PENDING`, if allowed (option on, Mario in `WIN_DEMO`, not an ex-stage, not airstrip 0 / plaza 1 / Corona 60) the white fade-out, `STAGE_TRANSITION_PENDING` and `decideNextStage` are skipped; Shine flag, count and save happen as usual. When the demo and the save are over: `SHINE_TAKEN` off, Mario `changePlayerStatus(WAIT, 0, true)` (forced: `WIN_DEMO` carries flag 0x1000, which blocks an unforced change, as `readBillboard` does) and `MSMainProc::startStageBGM`. The Shine object took `HIT_MESSAGE_TAKE`, so it cannot be collected again.
- Camera (`MarioGamePad.cpp updateMeaning`): `port_qol_camera_stick(&mCompSPos[6], &mCompSPos[7])` after the C-stick copy (invert, scale).
- Fast-forward (`port_main.cpp`): every 2nd/4th main-thread VIWaitForRetrace returns without waiting and counts as a retrace (`TVideo::waitForRetrace` loops until the count advances, so without the virtual retrace 4x stayed at 2x).
- Cheats (`port_qol_frame`, in a stage): `MSF_LIFE_COUNT` kept >= 3; `mCurrentWater` kept at `getMaxWater()` while Mario has FLUDD.

## W9-20. QoL checks (scripted runs, isolated settings/user/cache dirs, `SMS_PORT_NO_PADS=1`, audio off)
- Skip intro: title screen at frame 701 (Dolby logo still up at 150 without it). Skip cutscenes: first-time openingA.thp stopped by Start at movie frame 280 (not skippable with the option off); Shine-get demo ended at frame 2077 instead of 2237; plaza 1,1 intro to control at tick 366 instead of 1170.
- Text: screenshot grids normal / fast / instant of Peach's airstrip lines (fast ~2x; instant all three lines at the first readable frame).
- Fast-forward (Tab via `SMS_PORT_KEYSCRIPT`, `SMS_PORT_FRAMESTATS`): logic 29.97/s -> 59.94/s (2x), ~116/s (4x); ">> 4x" badge in `SMS_PORT_SHOT_PRESENT`.
- Focus pause (new test knob `SMS_PORT_FOCUSSCRIPT=<frame>:<ms>`): "[qol] paused" ... "resumed" after 6 s, game thread stopped in between.
- Camera: C-stick right 30 frames: turns one way, the other with invert X, about twice as far at 200 %.
- Volumes (WAV): all three at 0 -> RMS 0 (every DSP voice goes through the gain); title screen music 0 -> peaks 803 -> 263 (the ambience SFX remain).
- Trackers: pause screen with blue coins (stage names, x/30, total 240) and the Bianco episode list; F4 shows them without pausing.
- Cheats: water 10000 after 24 s of spraying (9381 without); lives 2 -> 3 after a fall in a secret course.
- Auto-save / continue (Gelato 4,2 `tpshine`, card seeded from the smoke new-game card): continue + auto-save -> "Game saved", "continuing in stage 4", Mario walks and jumps afterwards, Shine count 1, no stage change; auto-save alone -> saved, back to the plaza.
- Replays: QoL-off recording replayed with QoL off and with everything on: 0 of 21 differ each; recording with FF 4x / camera / cheats / instant text replayed with QoL off: 0 of 22; a recording made by main's pre-QoL exe (copied to the scratchpad) replayed in this build with every option on: 0 of 18 (same as main replaying it).
- smoke_all --quick 20/20 PASS; GameCube `shasum -c` OK.
- Not covered: multiplayer gating only by code (no session run); the voice/SFX split is by SE id range (0x78xx/0x88xx = voices), not verified per sound; continue-after-Shine checked in one stage (Gelato 4,2) only, boss stages rely on the exclusion list and the WIN_DEMO condition.

## Interpolation: ownership and hooks (w9, `port/src/port_interp.cpp`)
Display frames between two 30 Hz logic frames (frame rate above 30) show lerp(S_s, S_s+1). w9 owns the core; others plug in through the hooks below without editing it.
- **Covered by w9:** camera (view/projection, cuts snap); J3D models (joints in their parent's space, slerped; envelopes recomputed by `J3DModel::calcWeightEnvelopeMtx`; instanced models per viewCalc occurrence); texture matrices baked in material display lists (camera/joint dependent env and projection maps, SRT scroll animations); `TMBindShadowManager` circle/square shadow requests. 30 fps runs none of it.
- **Not covered yet:** JPA particles and emitters, FLUDD water streams, lens flare, screen-texture effects (w5 takes these), multiplayer puppets (w15), `TModelShadow` (tree/ship shadows), CPU-skinned meshes (`mSkinDeform`), objects drawn with their own GX matrices outside J3D, 2D.
- **State:** `port_interp_drawing()` true inside a display frame (whole redraw), `port_interp_interpolating()` true while its interpolated view pass and draw lists run; `port_interp_alpha()` the position between the two states (0..1). Logic frames: `port_interp_logic_draw_begin()` (state `sDrawState`), the logic unk34 between `port_interp_entry_begin/end` (state `sEntryTick`).
- **Recipe for a new system:** on the logic frame (not drawing) keep a copy of what you draw, keyed by your object; in a display frame where `port_interp_interpolating()`, write lerp(previous copy, current value, `port_interp_alpha()`) into what you draw; put the live value back after the draw lists (`port_interp_redraw_draw_end` drains the GPU first: memory a queued draw reads may be rewritten after it). Never change game state in a display frame (rand is a scratch copy, sound effects are gated, draw-sync callbacks are not called).
- **Hooks the game calls (SMS_PORT):** `J3DModel::viewCalc` -> `port_interp_model_begin/end`, `port_interp_model_texmtx`; `J3DMaterial::calc/makeDisplayList`, `J3DTexMtx::load` -> `port_interp_note_*`; `TMBindShadowManager::perform` -> `port_interp_shadow_requests`; `port_mardirector_redraw` (MarDirectorDirect.cpp) runs the display frame.
- **Comparison knobs:** `SMS_PORT_INTERP_JOINTS=world`, `SMS_PORT_INTERP_TEXMTX=0`, `SMS_PORT_INTERP_SHADOWS=0` (old behaviour per part), `SMS_PORT_INTERP_LOG=1` (per frame: models, texture matrices patched/recorded/rejected, joints the old lerp would have snapped, largest joint turn), `SMS_PORT_INTERP_SHADOWLOG=1`, `SMS_PORT_SHOT_DISPLAY=[+]<frame>:<count>` (every presented frame; "+" counts from stage control).
- **w8 (bath water / duckies):** the bath's water texture animation and shimmer were among the stepping/back-and-forth cases fixed here (W9-22), and instanced models (W9-21) are now interpolated; worth re-checking that report on this build.

## W9-28. Belly slide / dive pose; setLookDir (user report: Mario sunk into the floor or on his side)
- **Not interpolation** (30 fps too). `TMario::calcBaseMtx` (MarioDraw.cpp, NonMatching unit, so not in the GameCube DOL): the ground-aligned body matrix used while sliding / slipping (`UNK114_FLAG_UNK8`, set in MarioRun.cpp's slip states) stored its third column as `bin.z, bin.x, bin.y`; the asm stores `bin.x, bin.y, bin.z` (stfs of the three fmsubs results to 0x8/0x18/0x28 in order). The skewed matrix put Mario into the floor or onto his side. Fixed. (Tried and reverted: swapping the ground-normal cross order; the A/B screenshots showed the original order is right.)
- **`JGRotation3::setLookDir`** (user-approved JSystem edit): y = z cross x (was x cross z, a mirrored basis), as w8 found from BathWaterManager's asm. Its only caller is `BathWaterManager`, whose SMS_PORT workaround (same maths) is removed. GameCube `shasum -c` OK (the caller's unit is NonMatching). Bowser's bath (60,0, story9) looks as before.
- Checked at 30 fps (Gelato sand, dive toward the camera): Mario flat on his belly through the slide (before: sunk / sideways). Yoshi walking (Ricco 3,7, hatched egg) looks the same in a build from before today's merges and looks plausible side-on; no change made there. Riding Yoshi down a slope uses the same slip matrix (fixed with it).

## W9-27. Present pacing above 30 fps (w14's work, brought in)
- From workers\w14 (pacing part only): every frame carries its slot as Aurora's present tag (logic frames: their retrace; display frames: their due time) and Aurora's render worker holds the Present until slot + an adaptive delay (90th percentile of slot-to-ready over 128 frames + 0.5 ms, 0.5 ms .. min(8 ms, 0.8 periods); late frames go at once). `port_interp_lead` +2 ms while holding (marked in port_interp.cpp). Aurora side: `aurora_present_pacing.patch` (already in the shared libs: `aurora_set_present_tag`, `aurora_set_present_hooks`). w14's profiler call (`port_prof_present`) left out (not in main). Off at 30 fps and at an unlimited rate; `SMS_PORT_INTERP_NOHOLD=1` turns it off, `SMS_PORT_PRESENT_DELAY_MS` fixes the delay, `SMS_PORT_PRESENT_LOG=1` logs every Present.
- Measured (Present timestamps, Bianco, intervals < 50 ms; this test machine, offscreen): 60 fps: sd 4.20 -> 1.06 ms, frames within 1.5 ms of the 16.68 ms slot 11 % -> 90 %. 120 fps: on the grid 21 % -> 52 %, but this machine does not sustain 120 (median 9.29 ms with the hold vs 8.70 without). 144 fps: not reachable here (median 9.85 ms). Replays: presentation only; 30 fps replay 0 of 21 checkpoints differ.

## W9-26. Flags, waves, wires and splash rings in display frames
These draw straight from their own data at 30 Hz (outside J3D), so above 30 fps they stepped. Each draw is now wrapped (logic frames: copy; interpolated display frames: blended values while drawing, live values put back right after):
- **Flags** (`TMapObjFlag::draw`, cloth rows `unk78[r]`): `port_interp_points_begin_s` per row (the original draw is `portDraw`).
- **Waves / sea surface** (`TMapObjWave::perform` CUE_DRAW): `port_interp_values` (new: float sets, wrap-aware) on the phases and scrolls `unk64..unk70` (wrap at 2 pi / 1), wave heights `unk3C/unk40`, alpha `unk54`.
- **Wires** (`TMapWireManager::perform` CUE_DRAW): each wire's `TMapWirePoint::mPosition` (strided: `port_interp_points_begin_s` with the struct stride).
- **Splash rings** (`TSplashManager::makeDL`): `port_interp_track(splash, &unk0, age = lifetime - countdown)`. Also: `makeDL` ends splashes that are off-screen (`unk10 = 0`); a display frame (interpolated camera) no longer does that, it is game state.
- Checked at 120 fps: Ricco 3,0 ~470 and Gelato 4,0 ~1060 tracked positions/values per display frame, no crash; replay recorded at 120 fps: 0 of 18 checkpoints differ replayed at 120. `TModelShadow` (tree/ship shadows) left as is (trees static, ships slow). w14's pacing work not taken in this batch.

## W9-25. JPA particles in display frames
- **JPA particles** (`JPAEmitterManager::drawBase`, all JPA effects: dust, sparkles, splashes, FLUDD mist): they were drawn at the newer logic state in display frames (30 Hz steps, ahead of their emitters). `port_interp_track(particle, &mGlobalPosition, mAge)` per particle before each emitter draws: remembered on logic draws; in an interpolated display frame, same address with an older age remembered (at most 8 frames older) -> drawn between the two places; `port_interp_track_end` restores after the emitter (the draw visitors read positions through `getGlobalPosition`).
- **FLUDD's water drops** are already interpolated in main by w15 (`TModelWaterManager::portInterpCalcView`, with the drops followed through `garbageCollect`); not touched here.
- Also added for other systems: `port_interp_points_begin/end/move/invalidate` (an xyz array drawn between its last two logic states, slots followed through compaction), see the hooks note.
- Checked (Bianco 120 fps, spraying side-on): ~200 particles interpolated per display frame, spray and effects coherent in every presented frame; replay recorded at 120 fps: 0 of 18 checkpoints differ replayed at 120. Knob `SMS_PORT_INTERP_PARTICLES=0`; `SMS_PORT_INTERP_LOG` counts them ("points").
- w5's unfinished `port_interp_fx.cpp` (same goal) is not taken: superseded by this and w15's water. w14's pacing work (render-worker hold) not taken in this batch: frame-time spread, not flicker.
- Still 30 Hz steps (no flicker): vertex-animated objects outside J3D (flags `MapObjFlag`, `MapObjWave`, wires `MapWireManager`, splash rings), `TModelShadow` (trees static; ships slow).

## W9-21. Mario's feet and fast turns: joints interpolated in their parent's space
- **Cause:** every joint and envelope world matrix was lerped on its own, and a matrix turning more than 60 degrees in one logic frame snapped to the newer state. In the spin jump about 58 of Mario's joints turn ~90 degrees per logic frame (`SMS_PORT_INTERP_LOG`: "58 would snap"), so the whole body stayed at 30 Hz; and with some joints snapped and their neighbours lerped (envelopes lerped separately too), skinned parts such as the feet were pulled far from the legs.
- **Fix:** the joint tree (`port_j3d_joint_parents`, J3DModel.cpp) gives each joint's local transform `L = inv(P) * W`; L of both states is split `R * U` (QR: rotation, scale/shear), R slerped by quaternion (shortest way), U and translation lerped, the world matrix rebuilt from the parent's interpolated one. Both endpoints are exact, limbs keep their length, turns follow the arc. The envelopes are recomputed from the interpolated joints by the game's `calcWeightEnvelopeMtx` (viewCalc, `portInterp == 2`), and everything is put back after the viewCalc as before. A root moving > 500 units in a frame snaps (teleports).
- **Instanced models** (one J3DModel viewCalc'd at several places per pass, previously left at 30 Hz) are snapshotted per occurrence and matched by order; an occurrence is only interpolated when its live matrices equal the newer snapshot.
- Checked at 120 fps: spin jump in-between frames show in-between orientations (before: the newer pose repeated); no joint turns > 180 degrees per logic frame in the spin jump, triple/side flip runs (one-frame turnarounds blend).

## W9-22. Water shimmer and texture animations: texture matrices in display frames
- **Cause:** texture matrices are computed at entry (`J3DJoint::entryIn` -> `J3DMaterial::calc`) from the logic frame's camera, joint and SRT animation and baked into the model's packet display list; display frames reused the lists, so environment/projection maps kept the newer camera while the geometry moved with the interpolated one (back-and-forth every logic frame), and scrolling textures stepped at 30 Hz.
- **Fix:** each `J3DTexMtx::load` inside `makeDisplayList` of the logic unk34 is recorded (display-list address of its 8/12 words, the joint matrix, view branch, SRT). A record is kept only if recomputing it with the live inputs (`port_j3d_texmtx_recalc`, J3DMaterial.cpp: the per-mode setup of `J3DTexGenBlockBasic::calc` + `J3DTexMtx::calc`) gives exactly the words the game bakes. A display frame recomputes it with the interpolated camera, the model's (interpolated) joint and the SRT between the two logic frames (a wrapped scroll is not swept back) and writes it into the list; the original words are restored after the frame's draws are drained.
- Checked (Ricco 3,0, 120 fps, frame-to-frame difference of a water patch): before, the water changed at one present per logic frame (2.2 / 0.1 alternating); after, evenly (1.2-1.5 every present). Bianco: 43-70 texture matrices patched per frame, 0 rejected.

## W9-23. Shadows blinking at 30 Hz
- **Cause:** `port_mardirector_redraw` reset the shadow request count to its value before the logic unk34, expecting the view-only pass to make those requests again; they are made under CUE_ENTRY, which that pass does not run. Display frames drew 9 of 26 requests in Bianco: Mario's and most objects' shadows were missing from every display frame.
- **Fix:** after the interpolated view pass the GX list gets the logic frame's whole request list; `port_interp_shadow_requests` (`TMBindShadowManager::perform`, before `calcVtx`) keeps the requests of each logic draw and, in a display frame, moves each request (position, radii, rotation) between its match in the previous logic frame (same index, else the nearest of the same kind within 16 entries / 200 units) and its current place. Restored after the frame as before.
- Checked: 26 requests in display frames; Mario's shadow present and following him in every presented frame at 120 fps (before: absent from most display frames).

## W9-24. Lightbar FLUDD colour, checks
- Lightbar: FLUDD's water at full fill is #00C9FF (user's choice; the game's water table gave 51,85,255), same dimming curve; Yoshi's juices unchanged.
- 30 fps unchanged: the same replay through main's exe and this build gives byte-identical EFB screenshots at 5 frames, 0 of 21 checkpoints differ. Replay recorded at 120 fps with play: 0 of 18 differ replayed at 120, 30 and 144. 60 / 144 / unlimited runs with spin jumps: no crash, logic steady at 29.97/s.
- `SMS_PORT_SHOT_DISPLAY` takes "+<frame>" (from stage control). Test runs here: silent, offscreen, own cache.
- Seen once each in two runs: an access violation inside Dawn (bind-group creation) at boot, with a pipeline cache shared by many consecutive test runs of changing builds; gone with a fresh cache. Not reproduced since.
- smoke_all --quick (offscreen, silent): 20/20 PASS; GameCube `shasum -c` OK. One earlier smoke pass had the three title-screen scenes exit with `std::bad_alloc` in Aurora's `resolve_pass_into` (EFB copy) at start-up; they passed on the rerun with this build and with main's exe, and the next full pass was 20/20 (transient, likely memory pressure from parallel instances).

## W9-7. Release package (`build_aurora.py --package <dir>`)
- **Contents:**
  - `sms_port.exe`, with the PE subsystem switched to Windows GUI, so no console window opens;
  - the Dawn/DXC/SDL/libpng/zlib DLLs;
  - the Visual C++ runtime DLLs that the exe and those DLLs import (msvcp140, msvcp140_atomic_wait, vcruntime140, vcruntime140_1), from the VS redist folder;
  - a README (game requirement, controls, hotkeys, where settings/saves live, licences).
  The UCRT is part of Windows 10/11. No game data is ever copied. The target folder must be empty.
- **Checked:** packaged into my scratchpad (46 MB, 12 files) and started from there with the ISO on the command line, outside the repo.
  It reached the title, with no console window (`port/build_aurora/w9_package_run_iso.png`).



## A-4. Output scale (calibration)
- `DSPRES_FILTER`'s layout and JAudio's volume paths give these anchors: every DSP volume is Q15 (normal mix buses: `TChannel` volume x
  `inputGainDown` x 16384; auto mixer: volume x 32767.5; JAI streams: 0x7FFF / 0x5A7E straight from `JAIGFrameStream`), and the DsyncFrame level
  defaults to 0x4000 (`dspproc.c`). Kept as 2.14 (0x4000 = unity; SMS's `DsetMixerLevel(5.0)` = 0x5000 = x1.25) because the stream path rules out
  4.12: `title.afc` is mastered to -0.7 dBFS peaks / -21 dBFS RMS (decoded offline), and at volume 0x7FFF a x5 output would clip it constantly, as it
  did the boot jingle (a full-volume auto-mixer voice). With 2.14 a full-volume SE or stream plays at x1.25 and a full-volume music channel at x0.5.
- Measured after the A-5 fix (WAV, `SMS_PORT_THP_SPEED=8`): title BGM -30..-35 dBFS RMS; Bianco Hills stage BGM -28 dBFS; jump/FLUDD/footstep
  windows -15..-17 dBFS (peaks touch full scale, 5 clipped samples in 50 s); THP movie sound (CPU-mixed at unity by the SDK's MixAudio) -14..-20 dBFS.
  Open: sequence music sits 10-15 dB under the movie and SFX. That follows from JAudio's own numbers (music capped at 0.802 x 0.5 of full scale)
  and was not tuned away; if a real-hardware recording shows otherwise, the music bus (not the output level) is where a factor is missing.
- Auto-mixer pan law now uses the quarter-sine table JAudio passes the DSP (`DSPRES_FILTER` s16 512-639) instead of `cos`/`sin`.
- Filters: no FIR (outer "muffle") or animated IIR coefficients were seen in the title, Delfino Plaza or Bianco Hills runs (`SMS_PORT_AUDIO_LOG` reports the first ones).

## A-5. Resampling table was read as 128 phases; it has 64
- `DSPRES_FILTER` is 640 s16: 0-255 = 64 phases x 4 taps (phase 0 `{0x0C39, 0x66AD, 0x0D46, -0x21}`, phase 63 its mirror), 256-511 = four 64-sample
  oscillator tables (sine, triangle, two single-pulse), 512-639 = a quarter sine. The renderer used 128 phases, so half of all output samples were filtered
  with the oscillator tables as "taps": broadband noise on every pitched voice (spectral flatness 0.2-0.5, now 0.00-0.03 on the title music; harmonic lines and
  seagull chirps are now clean in the spectrogram). Fixed: 64 phases (`frac >> 10`); oscillator voices use the DSP's own tables.

## A-6. Gameplay sound with `SMS_PORT_WARP` (no warp change needed)
- `SMS_PORT_WARP=1,0` is a fresh save's first plaza visit: `checkAdditionalMovie` plays the court movies (openingBA/BB), then `TMovieDirector` shows the
  "save?" prompt over the plaza scene. The screenshot that looked like gameplay was that prompt, so no stage sound flow had run (no
  `MSMainProc::setMSoundEnterStage`). Same as the natural path. Answering it (`SMS_PORT_PADSCRIPT=1300:A,1500:A,1700:A`) plays openingBC, then the
  plaza starts: demo BGM `0x80010017`, stage BGM `0x80010001`, ambience, jump/voice (`0x78xx`), FLUDD (`0x0024`/`0x6800`), footsteps (`0x18xx`).
- `SMS_PORT_WARP=2,0` (Bianco Hills) goes straight to the stage: demo BGM `0x80010027` then stage BGM `0x80010002`; with
  `SMS_PORT_PADSCRIPT=1000-1400:S0,1100:A,1250:A,1450-1600:R,1650:A,1700-1900:S90+R,1950:A,2000-2200:S180,2100:B` 76 distinct sounds were requested
  (jumps, spray, footsteps, landing, voices), all rendered (levels in A-4). Coins were not collected in the scripted runs.
- Debug logs added (`SMS_PORT_AUDIO_LOG`): `MSMainProc` stage sound setup and `startStageBGM`, `MSBgm::startBGM`, SE-gate drops (`MSound::gateCheck`),
  and each distinct JAI sound id once (`JAIBasic::startSoundBasic`).

## A-7. THP movie audio
- `sdk_thp.cpp` now decodes and plays movie sound like the SDK: the decoder thread runs `THPAudioDecode` (Aurora's, SDK API, right/left order) on each
  frame's audio component (selected track) into the SDK's three audio buffers; `THPPlayerInit` registers `audioCallbackWithMSound` as JAudio's mode-3 mix
  callback (removed again in `THPPlayerStop`/`Quit`, as in `THPPlayer.c`); `MixAudio` (VolumeTable, volume ramp) runs from `JASystem::Kernel::vframeWork`
  on the AI clock; `PlayControl` only advances the picture while `curVideoNumber - curAudioNumber <= 1`, and a non-looping movie ends after its last audio frame.
- `SMS_PORT_THP_SPEED > 1` turns movie audio off (frames are skipped) and keeps the old video-clock behaviour.
- Evidence: normal-speed boot, `Entrance.thp` (2 ch, 32 kHz, 2 tracks): tonal music -14..-20 dBFS RMS (spectral flatness ~0.002), no silent 50 ms window
  from 8 s to 96 s, the movie ends (state 3, frame 2815 of 2816) about 95 s after it opens (2816 frames / 29.97 fps = 94 s), 0 AI underruns.

Left to do (audio): music-vs-SFX balance against a real recording (A-4); IIR/FIR semantics are reconstructions (not exercised yet);
Dolby surround (back buses are folded into L/R).


## R5. Sirena (warp 6,0): Aurora FATAL `unmapped vtx attr 13`
- **Cause:** a texgen with source `GX_TG_TEX0` while the vertex format has no `GX_VA_TEX0`; Aurora's shader generator had no fallback.
  Dolphin's vertex shader generator uses (0, 0) for a texgen whose texture coordinate is not in the vertex.
- **Fix:** `aurora_shader.patch` (3/3): same fallback, with a warning naming the attribute. Sirena now plays (`stages2/a6_*`: sunset beach).


## R6. D3D12 `CreateGraphicsPipelineState` E_OUTOFMEMORY (warps 6,0 / 7,0 / 8,0)
- **Pipeline count:** not unbounded. A temporary probe in `find_pipeline` counted distinct GX pipeline configs, configs with the alpha-compare refs zeroed, and configs with `dstAlpha` normalised: about 100 in Sirena after the load and under 200 after 75 s, and the normalised counts were identical (no per-draw value in the key).
  Process memory stayed flat (private bytes 713 MB, GPU dedicated about 250 MB over 100 s), so nothing leaks either.
- **Nature:** intermittent and per pipeline, not per shader. The failing pipeline (`GX Pipeline 8857a2bda610474b main`, a one-stage texture shader) compiled fine on the next runs; 3 more runs of 6,0 and 2 of 7,0 did not fail.
  It came while 5-7 other `sms_port` copies (other agents) shared the GPU. Dawn reports it as error type 4, **Internal**, not OutOfMemory.
- **Fix:** `aurora_pipeline_wait.patch` (c): `gx::build_pipeline` captures Internal and OutOfMemory errors with error scopes and retries up to 5 times with back-off (20-320 ms); if it still fails, that pipeline is dropped (error logged, its draws skipped) instead of aborting the game.
  Debug aid added on the way: `AURORA_SHADER_DUMP=<dir>` writes every GX pipeline's WGSL, named like the pipeline in Dawn's errors.


## R7. Missing geometry for the first frames after a load
- **Cause:** Aurora compiles pipelines on a worker thread and `gx::render` skipped any draw whose pipeline was not ready; with an empty cache a new stage drew without most of its map for a few frames (`final/plaza_shot_1950.png` of the previous round).
- **Fix:** `aurora_pipeline_wait.patch` (a): `gx::render` calls `wait_pipeline`, which moves a queued or prewarming pipeline to the front of the queue and waits for it. Nothing is ever dropped, as on the GameCube; the cost is a load-time hitch with a cold cache.
  This also covers w8's EFB-copy case (their `aurora_copy_pass_pipelines.patch` waited only in passes whose result is consumed; same `wait_pipeline` API, so that patch is no longer needed).
- **Persistent cache:** hand-started runs use the per-user cache, which persists (checked: two runs on one cache directory; the second run loads it and prunes stale Dawn entries), and every known pipeline is compiled in the background at startup.
  The old failure mode (one bad recorded pipeline aborting the next start) is gone: `aurora_pipeline_wait.patch` (b) turns shader/pipeline validation errors into a logged, skipped pipeline.
- **Result:** plaza warp with an empty cache: the "Delfino Plaza" intro camera (frame 1950, the same frame that lacked the map before) shows the full street, buildings and NPCs.


## R8. Exact 4:3 display (w9's `VILockAspectRatio`)
- Applied w9's proposed `aurora_display_aspect.patch` by hand to Aurora (`lib/window.hpp/.cpp`, `lib/dolphin/vi/vi.cpp`); the file in `port/patches/` is now the exact diff.
  `port_window.cpp` picks it up automatically: the log now says `window: aspect 4:3` without the "no Aurora aspect lock" note.
- Note for w8 (EFB readback owner): with the aspect locked, the render target is 4:3 while the logical EFB is 640x448, and Aurora maps logical coordinates with separate x/y scales and no offset (`map_logical_viewport`).
  `efbPeekARGB`'s `mapLogical` (R0) assumes a uniform scale with centring, which was already slightly off before (1280x960 target, 640x448 EFB); it should be `x * targetW / logicalW`, `y * targetH / logicalH`.


## R9. Warp 5,0 and visual checks
- **Warp 5,0 (Pinna Park beach):** renders and plays now (`shot_1200`: beach, park entrance, Mario, HUD); the all-black frames of the previous round do not reproduce with today's main plus these patches.
- **Night/dusk lighting:** the Hotel Delfino interior (7,0) renders with its dark-blue night lighting and Sirena (6,0) with its sunset tint, so channel/ambient/light colours reach the shaders.
  Plaza and Ricco walls show their baked vertex-colour shading. Pianta's brighter look is therefore left as it is; it needs a real-hardware capture to judge, and nothing in the pipeline points to a port bug.
- **Mario's dirt layer:** the swap `H_ma_rak_dummy` -> scene `h_ma_rak.bti` (airport: CMPR 128x128 pink/orange swirl) goes through `SMS_ChangeTextureAll` + `J3DTexture::setResTIMG` with the archive-swapped header and s32 offsets, and the DL is rebuilt every frame, so the right image is bound. A dirty Mario was not captured yet.
- **Fog:** Aurora's fog path is used by the stages surveyed (Noki and Pianta distances) with no visible errors; not compared with hardware.
- Stage survey after these patches (`port/build_aurora/stages2/`): 2,0 3,0 4,0 6,0 7,0 8,0 9,0 run without a fatal.




## 12. Airstrip: first goop clean crashes in `TPollutionLayer::cleaned` (wild read at line 189)
- **Symptom:** in airstrip scenario 1 (after FLUDD), spraying goop crashes within seconds: `TPollutionLayer::cleaned` 遶翫・`stamp` 遶翫・`TPollutionManager::clean` 遶翫・`TModelWaterManager::move`.
- **Cause:** an original-game bug that is harmless on the GameCube.
  The effect ring buffer is `static TVec3 pos[10]`, but the wrap test is `if (now_pos_no > 10)`, so `pos[10]` is written.
  On the GameCube, `pos$2426` (0x803F8A40, 0x78 bytes) is followed by `@3094` (0x803F8AB8), the next `.bss` object of this unit. Nothing references it (`build/GMSE01/asm/Map/PollutionLayer.s`), so the stray write is lost.
  On PC the 12 bytes landed on live statics (the `counter_x`/`counter_z` table indices), which sent `x_offset_table[counter_x]` out of bounds.
- **Fix:** `src/Map/PollutionLayer.cpp`, under `SMS_PORT`: `pos[11]`, whose 11th slot absorbs the write the way `@3094` does.
  The `> 10` behaviour is kept, so the effect positions cycle exactly as on the GameCube.


## Open note for w6 from w8 (E25): `snapshot_color_sample0` fallback (`aurora_msaa_depth.patch`)
- `tex_copy_conv.cpp snapshot_color_sample0` returns without writing the snapshot when its compute pipeline could not be created (the `failed` path) or the scene format is not RGBA8/BGRA8. The pooled snapshot texture then keeps whatever an earlier use left in it, so pixel counters and the CPU pollution maps would read stale data silently with MSAA on.
- Suggested fix: make it return `bool`, and in `encoding.cpp` fall back to the existing resolved-copy branch (`CopyTextureToTexture` from `copySourceTexture`) when it returns false. A resolved count is slightly off at polygon edges; a stale one is wrong.
- Smaller points:
  - For an sRGB scene format the packed bytes are linear values. That is harmless now because the EFB is `to_linear`; encode or reject such formats.
  - A uniform buffer and a bind group are created per snapshot. They could be pooled as in w8's E23 (performance only).
- Whoever owns the Aurora patches after w6 moves to multiplayer can pick this up; port_efb.cpp needs no change for it.


# Sanitizers (w10)

## Builds
- `python port/tools/build_aurora.py --asan [--run ...]` 遶翫・`port/build_aurora_asan/`, `--ubsan` 遶翫・`port/build_aurora_ubsan/` (both: `_asan_ubsan`). Separate directories, so the normal build is never invalidated.
- Only game/JSystem units are instrumented; port/ host code and Aurora are not.
- ASan: `-fsanitize=address -fsanitize-recover=address`, linked against VS18 LLVM's `clang_rt.asan_dynamic-x86_64` (+ `asan_dynamic_runtime_thunk`, `/wholearchive`; the static thunk clashes with ucrt's malloc); the DLL is copied next to the exe.
- UBSan: `-fsanitize=undefined -fno-sanitize=vptr,function,alignment,shift-base` with `clang_rt.ubsan_standalone` + onecore/shell32/dbghelp. Microsoft's ubsan runtime needs two ASan "continue on error" symbols; the build generates a tiny shim (`gen/ubsan_coe_shim.cpp`). `ubsan_standalone_cxx` is excluded (/MT, only for vptr).
- `--run` sets `halt_on_error=0`, so one run collects every report, written to `<out>/san.<pid>` (both runtimes share one `log_path`; it must be relative, a drive letter's ':' ends the option).
- **Game heaps (`SMS_PORT_ASAN`, `port/include/port_asan.h`):** `JKRExpHeap` poisons every free block (header included) and the slack after each allocation (beyond `ALIGN_NEXT(size, 4)`, the GameCube rounding); `JKRSolidHeap` poisons its free range and alignment slack, and re-poisons on `freeAll`/`freeTail`/`restoreState`. The two heap units are compiled uninstrumented (they walk their own poisoned headers) and call `__asan_(un)poison_memory_region` directly. So overruns between game allocations, use-after-free inside JKR heaps, and reads of free heap memory are reported like malloc bugs.
- `SMS_PORT_ASAN_SELFTEST=1` (asan builds, `src/main.cpp`) reads one byte past a 20-byte game allocation and one from a freed block: two `use-after-poison` reports prove the poisoning works.

## Runs
`--nullgfx`, 75 s each (about 4200 frames): title loop (with `SMS_PORT_THP_SPEED=8`: intro, title, attract movie, title) and `--warp N,0` for N = 1..9. All runs reach gameplay and keep going; none crashed.

## Fixed (each found by a sanitizer, each checked against the GC asm)
- S1 **`JASystem::DSPInterface::DSPBuffer::setBusConnect` reads past `connect_table[12]`** (ASan, audio thread at boot): sequences select bus 12..15 through `cmdBusConnect`. On the GameCube the read continues into the next `.data` object, `JASystem::Driver::FORCE_STOP_TABLE {0xF, 0xF, 0}`; under `SMS_PORT` the table carries those bytes as entries 12..19 (0 beyond, where the GameCube would read `Dvd::audioRootPath`). (JAudio: w5's area.)
- S2 **`SMS_InitChangeNpcColor` reads past `sMonteW_GlassBColorBuf[3]`** (ASan, Bianco Hills): Monte W NPCs with an individual-difference index 3..5. Under `SMS_PORT` the table is extended with the bytes the GameCube reads there (the following `.rodata`: `sMonteW_GlassBColorChange` and the `"_megane_mat"` string), so the glasses keep their GameCube colours (`src/NPC/NpcInitData.cpp`).
- S3 **`TAmenbo` (pond skater) used material indices as joint indices** (ASan heap overrun in `updateRipple`): `init` looked `cJointNames` up in the material name table; the GC asm reads `J3DModelData+0xB0` (`getJointName()`). Same transcription error in `TFireWanwan::init` for `"center"`/`"jnt_head"` (found by cross-checking every `getIndex` against the asm offsets). Plain fixes in NonMatching units.
- S4 **`TFireWanwanTailHit::performNodes` read node 5 of 5** (ASan): the second loop is `i < 4` in the GC asm (`cmpwi r30, 4`), the decomp had 5.
- S5 **`TGCConsole2` indexes `unk334[23]` of 22** (UBSan, every level): `unk330` starts at 0x17. On the GameCube `unk334[23]` is the next member `unk390` (a `TBoundPane*`) and `isVisible()/hide()` touch the high byte of its `unk4.x2`; on x64 the J2DPane layout made it the low byte, so the plain code saw "visible" and corrupted the rectangle. `SMS_PORT` helpers reproduce the GameCube access (`src/GC2D/GCConsole2.cpp`).
- S6 **float 遶翫・u32 conversions** (UBSan): MWCC converts through `__cvt_fp2unsigned` (negative 遶翫・0, 遶包ｽ･ 2^32 遶翫・0xFFFFFFFF); a plain cast of a negative float wraps on x86. New `sms_cvt_fp2unsigned()` (`port/include/port_mwcc.h`, C and C++). Applied where it bites:
  - `TObjHitCheck::getTableIndex`: actors near the origin went to hit-check bucket 0xBE instead of 0 (missed actor-actor hits in every level).
  - `hx_wiper.c` (all 14 float sites): `Hxs2_Circle` skipped the whole iris loop when the radius exceeded half the screen; `Hx_Test4`'s `rstep` wrapped to ~4 billion once it went below 0.
  The other `bl __cvt_fp2unsigned` sites in game/JSystem code (listed below) only see non-negative values in the runs so far.

## Reported, not fixed
- `TMActorKeeper::createMActor` / `TModelDataKeeper` (ASan stack-use-after-scope, most levels): `registerDataAndJoinNewNode` stores the caller's `name` pointer, and `TMapObjBase::makeMActors` passes a stack buffer (`"%s.bmd"`). Later lookups `strcmp` against that dead stack slot. This is the original game's behaviour (same dangling pointer on the GameCube); with the same call depth the slot holds the same string, otherwise a model is loaded twice. Left as is.
- `J3DTevBlock4::load` (UBSan bounds, w6): loops 16 over `mTevKColorSel[4]`/`mTevKAlphaSel[4]` into the following members. Matching unit, identical byte-only layout on PC, so it reads the same bytes as the GameCube.
- `JASBNKParser.cpp:169` (`header->mInstOffsets - 1`, then `[i + 1]`): pointer arithmetic idiom, no out-of-range access.
- `RumbleChannelDataMgr::getChannelDataTbl` called on a null manager (`TMovieRumble`): the method never uses `this`. Harmless.
- Enum loads (`VITVMode` -1 in JDrama video, `J3DMdlDataFlag` combinations), `J3DModel.cpp:690` null + offset: harmless.
- float 遶翫・s16/u8/u16 out of range (angles 32768..65535, colours): MWCC does `fctiwz` and keeps the low bits; clang's x86 code does the same for any value that fits in an int, so behaviour matches. Not changed.
- (The remaining `__cvt_fp2unsigned` sites are now all converted, see S7.)

## S7. Every float 竊・u32 site now converts like MWCC
All 60 `bl __cvt_fp2unsigned` call sites in the GameCube game/JSystem asm were mapped back to source; each one now goes through `sms_cvt_fp2unsigned` under `SMS_PORT` (`#ifdef` or a file-local macro that is the plain cast on GC):
- `CLBLinearInbetween<u32/unsigned long>` (`include/Camera/cameralib.hpp`, via `CLBPortFromFloat<T>`),
- `TGCConsole2::setTimer` (timer from stopwatch ticks),
- `TSelectDir::direct` ﾃ・, `TMarDirector::nextStateInitialize`, `TMovieDirector::direct` (fade-out frame counts), `TMarioGamePad::reset` ﾃ・ (button repeat),
- `hx_wiper.c` `Hx_GameOver` ﾃ・ (bounce timers; the other 14 were S6),
- `J3DDrawBuffer::entryZSort` (z bucket),
- `JAIBasic::checkNextFrameSe` ﾃ・ and `sendPlayingSeCommand` ﾃ・ (`JAIGFrameSe.cpp`, priority and distance wait),
- `JAISound` `rand_u32` (inlined into all five `setSeInter*`; a product 竕･ 2^32 now saturates) and `setSeInterPitch`'s random range,
- `TOscillator::calc` release-table index (a negative index was clamped to 15 instead of 0),
- `JPADrawCalcColorAnmFrameRepeat::calc`, `J3DGDSetFog` (`JRenderer.cpp`) and `FifoSetFog` (`PacketUtil.cpp`): the fog mantissa for B < 0,
- `MSSetSoundTL<T>::startSoundSetDyna` (random delay).
- Not changed: JGadget `TVector::GetSize_extend_` (`std-vector.hpp`, 6 template instances): `size_t` is 64-bit on PC and the value is a positive growth size, so the result is the same.

## Gameplay sweep (real D3D12 rendering, scripted input)
`--asan` and `--ubsan` (in parallel), `--warp A,E` for areas 1..9 and episodes 0..2 (27 runs each, 90 s, `SMS_PORT_THP_SPEED=8`), with
`SMS_PORT_PADSCRIPT="100-1500/60:A,+0-600:S0,+0-2400/45:A,+600-1200:S90+R,+1200-1800:S180,+1800-2400:S270,+1800-2400/50:B,+2400-3000:S45+R"`
(A through the intro dialogs, then run, jump, spray, dive and turn once Mario has control). Found and fixed:
- S8 **`TSleepBossHanachanManager` crashes in `clipEnemies` (null `mSaveParams`)** (ASan + UBSan, `--warp 4,1`): the sleeping Wiggler's manager and actor are built by constructors that are inlined into the MarNameRefGen creator on the GameCube; the fabricated `LINK_ALL_SOURCE` constructors in `SleepBossHanachan.cpp` left out `mSaveParams = new TDemoBossHanachanSaveParams("/enemy/sleepBossHanachan.prm")` and the zeroing of `mShinePosition` / `unk15C` (all visible in the `MarNameRefGen_BossEnemy` asm). Added.
- S9 **`JKRArchive::findFsResource` crashes on `"/../map/pad3/Setting.prm"`** (ASan + UBSan, `--warp 8,1`, `TEnemyMario::TSettingParams`): the path follows the root's `..` entry, whose directory index is 0xFFFFFFFF. On the GameCube `mDirectories + 0xFFFFFFFF` wraps to `mDirectories[-1]` (the tail of the RARC info block); on x64 it pointed 64 GB away. New `JKRArchive::portDirEntry()` (`SMS_PORT`, `JKRArchive.hpp` / `JKRArchivePri.cpp`) reproduces the 32-bit wrap and reads that tail the way the GameCube does (big-endian `mNum`/`mFirstIdx` from the unswapped flag/padding bytes).
- S10 **`TSeqParser::cmdIIRCutOff` reads past `CUTOFF_TO_IIR_TABLE[128]`** (ASan, Delfino Plaza): sequences pass index 128+. The GameCube read continues into `Player::s_key_table`, so under `SMS_PORT` indices 128..159 return the big-endian halves of those floats (same literals), 160+ return 0 (`JASSeqParser.cpp`, JAudio: w5's area).
Re-runs of `1,0`, `1,1`, `4,1`, `7,1`, `8,1` with the fixes: no new reports, no crash.
Reported, not fixed:
- **w6:** `--warp 7,1` under ASan once crashed inside the AMD D3D12 driver (`amdxc64.dll`, thread T21) from `aurora::gx::build_pipeline` (`port_deps/aurora/lib/gx/gx.cpp:463`, `create_pipeline` 竊・pipeline worker). Not reproduced on the re-run; possibly ASan's interceptors in the driver's allocations, possibly a real bad pipeline state.
- The `createMActor` dangling-name report shows up in most levels (original behaviour, see above).

float 竊・s32 (`fctiwz`): PowerPC saturates (竕･ 2^31 竊・0x7FFFFFFF), x86 `cvttss2si` returns 0x80000000 for any out-of-range value; the two differ only for values 竕･ 2^31 (and for narrow casts of such values). The UBSan sweeps never saw a float竊段nt value beyond ﾂｱ1.5e5, so no site was changed; `sms_cvt_fp2int()` (`port_mwcc.h`) is there for any that needs it.



## A-8. Stage BGM never started after the first sequence (loading flag race); output scale revisited
- **Cause of the quiet "music":** after a warp, the stage BGM (`0x80010002` Bianco, `0x80010001` plaza) stayed in JAI state 0 forever; only the
  first sequence of each slot (entrance demo, title BGM) ever played. `JAIBasic::checkEntriedSeq` sets `mLoadingFlag = true` *after*
  `Vload::loadFileAsync`, and `checkDvdLoadArc` clears it when the load completes. On the GameCube the DVD read always takes a while; in the port
  `sdk_dvd.cpp` completes reads at once, so the (higher-priority) JAudio DVD thread finished and cleared the flag before the game thread set it, the
  slot looked "loading" forever and `checkEntriedSeq` returned early for every later sequence. Same class: `JAIGFrameStream` `LoadADPCM` sets
  `DvdLoadFlag` after `DVDReadAsyncPrio`, whose callback clears it. Fixed under `SMS_PORT` by setting both flags before issuing the read
  (`JAIGFrameSequence.cpp`, `JAIGFrameStream.cpp`). With it the stage BGM starts after the demo (slot 0: `80010002` state 4, loading 0).
  The root cause (synchronous completion of async disc reads in `sdk_dvd.cpp`) can bite other game code that sets a "busy" flag after an async read;
  not changed here (not w5's file).
- **Volume chain traced** (debug: `SMS_PORT_AUDIO_VOLDUMP=1` prints every root track tree and note-on factors): stage BGM root timed volume 0.866
  (squared, mode 0) x JAI outer 0.443 (= `MSBgm::smMainVolume` 0.75 x the seq table volume) = 0.332; child tracks 0.4-1.0 (squared); note volume =
  (velocity/127)^2 x instrument volume (~1.0); then x `MAX_MIXERLEVEL` (0.802 x 16384) and the pan curve. Every factor matches the JAudio code
  (`writeTimeParam` matches the GameCube object; `updateTrack`, `updateEffectorParam`, `checkPlayingSeqTrack` differ only in register allocation),
  nothing is applied twice and no u8/u16 or byte-order misread was found in this chain.
- **Output scale:** with the BGM actually playing, the A-4 reading left sequence music ~13 dB under the THP movie music (mixed at unity by the SDK)
  and the SFX. The scale is now taken literally from JAudio: DsyncFrame level in 4.12 (`DsetMixerLevel(x)` = x * 4096; the default 0x4000 = x4
  makes JAudio's default chain unity, inputGainDown 0.5 -> 0.25 x 4), and the auto mixer (which gets volume x 32767.5 with no channel mixer level)
  gets base gain 0x1FFF (0.25), the constant stored right after the oscillator tables in `DSPRES_FILTER` and exactly the normal path's cap at
  JAudio's default. (`title.afc`, the A-4 counter-argument, is only reachable as `MSD_STR_SPACEWORLD`, a Spaceworld-demo leftover.)
- **Measured** (`SMS_PORT_THP_SPEED=8`, WAV): Bianco stage BGM alone -17..-19 dBFS RMS; with jump/FLUDD/footsteps -13.7..-16; Entrance.thp music
  -14..-20; title stage -25 (music + ambience); clipping 0-177 samples per minute (system SEs, boot coin jingle). L/R balanced (-16.6/-16.5 dBFS).

## A-9. Filters and the downmix, checked against the microcode
- **IIR:** the routine at jdsp word 0x0C11 (entered when `filterMode & 0x20` and the four coefficients differ from {0x7FFF,0,0,0}, words 0x032A-0x034A)
  was run in a small emulator (`iiremu.py`, w5 scratch): its impulse response equals `y = c0 x + c1 x[n-1] + c2 y[n-1] + c3 y[n-2]` (Q15), the form
  `port_audio_dsp.cpp` uses (the microcode adds one sample of latency). Underwater coefficients (forced with `SMS_PORT_FORCE_UNDERWATER=1`, which makes
  `MSSeCallBack::setWaterCameraFir` act as if the camera were in water): {5521, 2476, 17028, 13932}, a low-pass with +12.9 dB at DC and -21.5 dB at
  Nyquist. In Noki Bay and Bianco the sequences apply it to SE/ambient voices only (24 % of voice-sub-frames under water, none dry); the stage BGM keeps
  {0x7FFF,0,0,0}, so the overall spectrum barely changes (energy above 2 kHz 4.3 % -> 2.7 % in Bianco).
- **FIR / distance low-pass:** not used by anything in the title, plaza, Bianco or Noki runs (statistics in `SMS_PORT_AUDIO_LOG`).
- **Downmix:** the microcode mixes the other buses into the front pair itself (jdsp 0x00EF-0x0114 at the start of a sub-frame, 0x03F4-0x0434 after
  the voices): 0x0F40 (previous sub-frame) -> L x1.0, R x-0.5615; 0x0CA0 (40 samples late) -> L x-0.5615, R x1.0; 0x09A0 -> both x0.7071; 0x0FA0 -> R x-1.0.
  Implemented as decoded (before: back buses were simply added to L/R, which put all FX-line returns sent to 0x0F40 on the left).
- **Reverb (FX lines):** SMS sets them once from FX scene 0 (no per-stage change exists in the code): all four on, 48 blocks (120 ms), returns to
  0x0D00/0x0D60 and 0x0F40 at 4096 (lines 0-1) or 2047 (lines 2-3), FIR {0, 0, 846, 846, 4715, 8343, 13421, -4096}. Caves/indoors differ only by the
  voices' own FX sends (sequence/SE parameters).

## A-10. Pause, coins, 1-up, Shine
- Pause (`SMS_PORT_PADSCRIPT=...:START`): `MSD_SE_SY_PAUSE_ON`, then `MSound::pauseOn` fades every BGM track to 0 over 60 frames and mutes the SE
  categories except system sounds (category 4): measured BGM -15 -> -41 -> -62 dBFS within 2 s, system SEs still audible; START again plays
  `PAUSE_OFF` and the BGM returns within ~10 frames. That is the game's behaviour (full mute, not a duck).
- Coin `0x4811`, red coin `0x4846`, 1-up `0x4841`, blue coin `0x4845`, Shine-get SE `0x4814` and the Shine-get fanfare BGM `0x80010009`, started with
  the new debug hook `SMS_PORT_SE_TEST=<call>:<hex id>,...` (`MSound::mainLoop`), all render: -11..-15 dBFS RMS over the BGM's -18 (blue coin clips 185
  samples). The boot jingle is the coin sound `MSD_SE_MV_COIN` (0x7915). Mario's voice clips (`0x78xx`) play in gameplay; movie voices come with
  the THP audio (A-7). Not done: collecting real coins/Shines through gameplay, Peach/Toadsworth in-engine cutscene voices.

Left to do (audio): a real-hardware reference for the absolute balance (A-8 is derived from JAudio's constants, not a recording); FIR and FX-line
tap order are not verified against the microcode (only the IIR and the downmix are); `sdk_dvd.cpp`'s synchronous async reads (A-8).



## E6. Airstrip goop vanished: Aurora's EFB was smaller than the GameCube's
- **Symptom:** the airstrip pollution map (512x512 `GX_CTF_R8`, drawn and copied at EFB (0,0)-(512,512)) emptied within a few frames (readback 5703 polluted texels, then 0) and its counter read "clean" after the 30-zero debounce. Plaza and Bianco maps (256 rows or less) were fine.
- **Cause:** Aurora's EFB render target was the configured frame buffer, 640x448 logical for SMS's render mode; the GameCube EFB is 640x528 and the game draws and copies anywhere in it before the display copy. Rows past 448 were clamped and the copy rescaled, so the map slid every frame.
- **Fix:** `port/patches/aurora_efb_size.patch` (shared Aurora tree, libs rebuilt): `vi::efb_size()` (at least 640x528), used by `gx::logical_fb_size()`; the render target is allocated at the presented frame buffer's scale extended to the whole EFB (1280x896 presented -> 1280x1056 target); presenting uses only the configured top-left part (present viewport and resample UVs).
  Port side: `port_efb.cpp` maps against `vi::efb_size()`; `port_aurora_sdk.cpp` peeks map against the EFB size and `SMS_PORT_SHOT` captures only the presented part (`port_efb_screenshot_ex(path, true)` captures the whole EFB, used by `SMS_PORT_EFB_DEBUG`).
- **Result (warp 0,0):** the map keeps its goop (memory 6306 polluted texels, readback 5601 then a stable 4548; pixel metric 4548). Mario walking into it at (-1882, 1136) is hurt (life meter, CPU collision map) and the goop renders (`port/build_aurora/efb_airstrip_goop.png`). Display is unchanged (`shot_2600`: airstrip intro at the right aspect, no extra rows).
- **Cleaning (warp 1,0, plaza goop around (576, 4992), `PADSCRIPT +60-800:G6200;2500,+800-1800:G700;4400,+1700-3200:R`):** spraying cleans a patch (`port/build_aurora/efb_plaza_cleaning.png`), the layer's CPU map goes 3333 -> 3252 -> 3113 polluted texels (sampled every 2nd texel) and the counter 13346 -> 13011 -> 12453 pixels, i.e. it follows the real map and never reads "clean" while goop is left. Cleaning a whole layer to "clean" was not done; the airstrip needs FLUDD picked up first.
- The `TPollutionLayer` world-position probe used to find the goop was a temporary local edit, removed.


## E7. Mario's silhouette behind walls
- With the scripted plaza walk above, Mario ended up behind a building wall: the game draws its silhouette effect (dark Mario shape inside a darkened circle) over the wall: `port/build_aurora/efb_silhouette_plaza.png`. The occlusion test (E4) and the silhouette pass work; no fix needed.


## E8. `GXSetCopyFilter` (vertical copy filter)
- Was a no-op in Aurora. `port/patches/aurora_copy_filter.patch`: the seven coefficients become weights for the EFB lines above (0+1), at (2+3+4) and below (5+6), out of 64; `vf == GX_FALSE` is the hardware's identity filter. They travel through the FIFO (`GX_AURORA_LOAD_COPY_FILTER`) into the copy's uniforms and `sample_efb` in the texture-copy shader applies them one logical EFB line apart; a filtered copy always takes the shader blit.
- Users: the screen texture and mirror copies (`SMSVFilter_flicker`, via `JDrama::IssueGXSetCopyFilter` with flag 0x20) and `BathWaterManager`'s ripple copy (`{21,0,0,22,0,0,21}`). The pollution copies pass no filter. Bianco and its pause menu render as before (slightly softer pause background).





## A-11. Voice mixer gain law, read from the microcode (replaces the A-8 auto-mixer base and the in-sub-frame ramps)
- **Q16, not Q15.** The microcode starts in M0 (`8b00` at word 0x0059; every `M2` site restores M0 before returning), and neither voice mixer sets
  M2, so a channel volume multiplies as `(gain * sample) >> 16`. The bus store is saturating (40-bit mode). The output stage (0x0D22-0x0D31) is M0
  too, followed by `ASRNRX` by 4, which gives `(bus * level) >> 12`: the 4.12 output level of A-8 stands. All voice paths are therefore 6 dB quieter
  than in round 3. A full-volume music voice is now x0.625 at SMS's level 5.0. Bianco, the hotel and the title now peak at 13 000-22 000 with no
  clipping (the hotel peaked at 32 768 before).
- **Volume changes are smoothed, not ramped per sub-frame** (normal channels, 0x0386-0x03B6, mixer 0x0D0A):
  - The step is `(target - current) << 16 >> 5`. The first 64 samples use `current` and the last 16 use `current + step`.
  - The stored current becomes `current + floor((target - current) / 32)`, so a channel approaches its target with a 32-sub-frame (80 ms) time
    constant.
  - Note starts are not affected: `setMixerInitVolume` sets current = target.
  - Previously the port ramped linearly to the target within each sub-frame.
- **Channel delay:**
  - The level word's low byte delays a channel by that many samples (the source pointer moves back).
  - The low byte moves one step per sub-frame toward the high byte.
  - It is implemented, with 255 samples of history per voice, but unused in every run so far (`channel delays 0` in the `SMS_PORT_AUDIO_LOG` stats).
- **End request, normal channels** (0x0370-0x0385):
  - Each sub-frame, every channel's target becomes `current >> 1`.
  - The voice is done only when all six current volumes are 0.
  - Before, the port set done at once.
- **End request, auto mixer:** target = `current >> 1` (the auto mixer copies target to current every sub-frame, so this halves per sub-frame). The
  voice is done at current 0, about 15 sub-frames. Voice counts did not change (hotel: 24.5 average, 43 maximum, before and after).
- **Auto mixer** (0x0D32-0x0DC0, bus table at 0x0DD6), as decoded:
  - Position word: high byte left/right, low 7 bits front/back.
  - `P = {L*F, R*F, L*B, R*B} >> 16` from the quarter-sine table, where `X` = sin[i] and its mirror sin[i ^ 0x7F].
  - Direct gains `G = P * vol >> 16` go to 0x0D00, 0x0D60, 0x0F40 and 0x0CA0.
  - FX gains `G * (fx * vol >> 15) >> 16` go to **0x0E80, 0x0EE0, 0x0C00 and 0x0C50**, i.e. FX lines 2-3 plus two back FX buses. The port used to send
    to lines 0-1.
  - Ramp step `(P * (target - vol) >> 16) << 11`, the FX one times `fx >> 16`. Then vol = target.
  - At the start of each sub-frame, 0x0C00 (previous sub-frame) and 0x0C50 (40 samples late) are folded into lines 2 and 3 with the same x1.0 / x-0.5615
    spread as the main back buses (0x01A6-0x01C9, 0x01F0-0x0204).
  - The old 0x1FFF "auto base" is gone. The exact products give 0x1FFC at full volume, centre, which is still 0.125, the same as the normal path at
    JAudio's cap.


## A-12. FIR and FX-line tap order, checked against the microcode
- **Voice FIR** (0x0BF6-0x0C10, called from 0x0300-0x0328): the N history samples (VPB 0x0440, oldest first) are copied in front of the sub-frame,
  and `y[j] = sum c[k] * x[j - N + k]`. `c[0]` takes the oldest sample and `c[N-1]` takes `x[j-1]`, so there is one sample of latency. The rounded
  product (MOVPZ) is used, and the last N inputs go back to the history. The port had the taps reversed (newest first, including the current sample).
- **FX line** (per sub-frame, before the voices: 0x0172-0x01A5 with 0x012D; write-back after the voices: 0x0205-0x0230 with 0x01D6). Each line has
  an 88-word DSP buffer (bus 0x0DC0 + 0x60 * line):
  1. Words 80-87 move to 0-7.
  2. The block at the current delay position (written `blocks` sub-frames ago) is DMAd to words 8-87.
  3. If mode bit 0 is clear, the 8-tap FIR (0x0BD7, same tap order as the voice FIR) runs in place over words 0-79.
  4. Words 0-79 are mixed into the two return buses (0x00D8, Q15).
  5. If mode bit 1 is clear, the FIR runs again.
  6. The voices then add their sends to words 0-79, and that sum is written back to the delay line. The position then advances.

  SMS uses mode 2: the FIR runs before the returns, and the delay line holds `send + FIR(delayed)`. That matches the old port model except for the tap
  order. The port now emulates the buffer exactly, including the case where an earlier line's return into a later line's bus is overwritten.
- `port_audio_dsp.cpp` is changed for all of the above (`fxLinesRead` / `fxLinesWrite` / `fxFir`; the voice FIR block).


## A-13. Reverb sends: Hotel Delfino (warp 7,0), Corona Mountain (warp 60,0)
- Stage ids are from `stageArc.bin`: 7 = delfino (the hotel), 14 = casino, 16 = mareUndersea, 60 = coronaBoss. The FX config is the same everywhere
  (A-9). The per-place difference is the SE send: `MSHandle::setSeDistanceParameters` sets fxmix from `getMapInfoFxParameter(ground)`.
- **Hotel:**
  - Auto-mixed SEs carry fx `0x0E00`-`0x1C00`, and lines 2-3 get them: mean |in| 15-43, |out| 1-4.
  - No sequence voice sends to lines 0-1 there.
  - WAV -22.3 dBFS RMS, peak 19 935, 0 underruns.
- **Corona Mountain:**
  - fx `0x3100`-`0x7200`, mostly `0x4800` (about 4x the hotel).
  - Lines 2-3: mean |in| up to 118, |out| up to 11. Lines 0-1 carry the stage BGM's sends.
  - The WAV's autocorrelation has a small positive peak at 3 800-3 880 samples (the 120 ms delay plus the return path) in both 8 s windows tested.
- **Bianco:** lines 0-1 carry the stage BGM sends (|in| 57-170, |out| 10-31); lines 2-3 carry the SEs.
- New `SMS_PORT_AUDIO_LOG` statistics every 2000 sub-frames: `fx mean |in| ... |out| ...` per line, plus channel delay and end-fade counts.


## A-14. NPC voices in in-engine scenes
- **Debug aids:**
  - `SMS_PORT_AUDIO_SE_ONLY=1` silences the normal-mixer voices (the sequence music) in the capture. Volumes and fades still run.
  - `SMS_PORT_AUDIO_LOG` now logs every NPC sound start (0x80xx, 0x88xx-0x89xx) with its time in the `SMS_PORT_WAV` capture
    (`JAIBasic::startSoundBasic`, under `SMS_PORT`; `port_audio_out_wav_seconds`).
- **Airstrip intro (warp 0,0, after openingA):**
  - Peach `MSD_SE_NPC_VM_PEACH_WORRY` (0x8852) is requested at 72.2 s, together with Pianta voices `VA_MONTM_ANGRY1/2` and `DANCE1/2`, footsteps and
    ukulele.
  - The SE-only capture shows a pitch-gliding harmonic burst at 72.2-72.8 s (`port/build_aurora/audio_airstrip_peach.png`).
  - Toadsworth does not speak before Mario moves. His `VM_KINOJ_NORMAL` (0x8856), started in the airstrip with `SMS_PORT_SE_TEST`, renders as
    0.7 s bursts at -17 dBFS at 75.1 s and 81.7 s, with silence around them (`audio_airstrip_toadsworth.png`). So his voice data is in the airstrip
    scene bank and plays.
- **Plaza arrival (warp 1,0, `SMS_PORT_THP_SPEED=8`, A every 80 frames):**
  - After openingBA/BB/BC the plaza scene requests Pianta voices `VA_MONTM_ANGRY1/2` about every 10.7 s, plus `MONTM_SINK` and `MONTW_SINK`.
  - A burst at -20 dBFS lines up with the 46.2 s request (`audio_plaza_pianta.png`).
  - No Peach or Toadsworth line in this scene.
- **Bianco episode 1 intro (warp 2,0):** Pianta `VA_MONTM_DANCE1/2` from 3.6 s on, plus 0x8913, 0x896A and 0x8979.


## A-15. ARQ completion is asynchronous; ASan hunt
- `ARQPostRequest` and `ARStartDMA` used to complete inside the call. The copy still happens at once, but the callback now comes from the interrupt
  thread (`port_irq_raise`) after a FIFO "bus" delay of 20 us + 12 ns per byte, the same style as w7's DVD fix (`sdk_audio.cpp`). Bianco, plaza and
  hotel runs: BGM fine, 0 underruns, no "pool exhausted".
- **ASan builds** (`build_aurora.py --asan`, `ASAN_OPTIONS=halt_on_error=0`):
  - Runs: plaza 200 s with input, Bianco 160 s, title/attract with SE tests.
  - No finding in audio code, and no `TChannelMgr::getListHead` crash; those were most likely the round-1 bitfield bugs.
  - One non-audio report in both stages: stack-buffer-overflow / stack-use-after-scope in `strcmp` from `TMActorKeeper::createMActor`
    (`src/Strategic/ObjModel.cpp:164`), via `TCoin::initMapObj` <- `TNpcCoin` <- `TBaseNPC::setIndividualDifference_`. Reported, not touched.

Left to do (audio): a real-hardware reference for the absolute level (A-8 and A-11 come from the microcode and JAudio's constants, not a recording).
Bus overflow with the FX-return mixer (0x00D8 runs in 16-bit mode and its stores may wrap, not saturate) is clamped instead. Unconfirmed: the junk
low accumulator bits that can delay `done` by a few sub-frames in the normal-channel end fade are not modelled. `sdk_dvd.cpp`'s synchronous async
reads (A-8) belong to w7.
- w14 note for w5: the AR/ARQ completion delivery in `sdk_audio.cpp` was changed by w14 (marked `w14`): one ordered queue that also waits until the posting thread has moved past the code after `ARQPostRequest` (a lost-update hang in `JASystem::Dvd::loadToAramDvdTMain`), and the default cost is now the fixed 20 us (`SMS_PORT_ARQ_BUS_TIME=1` restores 20 us + 12 ns/byte). Details: "Performance (w14)", "ARQ race".




## R10. Pinna Park (13,2): Aurora FATAL `unsupported logic op 10`, then every later start dies
- **Cause:** `GX_BM_LOGIC` with `GX_LO_INV`: `TMario::drawLogic` draws Mario in white with `mBlendLogicOp`, and JPA particles pick ops from `JPABaseShape::stLogicOp`. Aurora's `to_blend_state` handled only CLEAR, COPY and NOOP.
  Worse, the pipeline config was recorded in the persistent cache before it failed, so every later start died while prewarming it (the first QA sweep lost every stage after Pinna Park).
- **Fix:** `aurora_pipeline_wait.patch` (e): all 16 logic ops approximated with blend factors (WebGPU has no logic ops); exact for 0/1 colours and a white source, which is how the game uses them.


## R11. 2044 GX pipelines per full sweep, startup E_OUTOFMEMORY retries
- **Cause:** the pipeline key contained the constant destination-alpha value (`GXSetDstAlpha(GX_TRUE, a)`), although only "off / zero / non-zero" changes the pipeline; the value is the blend constant. Masking fields of the 2044 recorded configs showed 1310 differing only there (fading silhouettes and effects).
  With a cache that large, startup prewarm made the driver return E_OUTOFMEMORY for many pipelines (the R6 retries kicked in dozens of times per stage).
- **Fix:** `aurora_pipeline_wait.patch` (f): normalised in `populate_pipeline_config`; `GXPipelineConfigVersion` 13 -> 14 so per-value duplicates in existing caches are pruned. 899 pipelines in the re-run sweep, no OOM retries.
  Other key fields that still multiply pipelines a little: the POS array stride (315 extra; different vertex formats) and the alpha-compare references (134; would need uniforms).


## R12. Corona: Aurora FATAL `convert_texture: unknown texture format 22`
- **Cause:** `GX_TF_Z24X8` (the Corona bath water's depth copy, `BathWaterManager`) bound as a texture before any `GXCopyTex` reached it, so Aurora tried to decode it from RAM.
- **Fix:** `aurora_depth_tex_formats.patch`: Z8 / Z16 / Z24X8 decode with the I8 / IA8 / RGBA8 tile layouts they share.


## R13. Bundled pipeline seed and QA sweep
- **Seed:** `port/data/initial_pipeline_cache.db` (899 pipelines), generated by `port/tools/qa_sweep.ps1` + `port/tools/make_pipeline_seed.py`; `build_aurora.py` copies it next to the exe and into `--package` (marked w6 additions). Aurora merges it read-only into the per-user cache (`Seeded pipeline cache ... 899 rows merged`).
  Cold-start hitch, plaza warp, retraces for frames 1200-1800 (600 = none): 1397 without the seed, 801 with it, 607 with a warm cache. Bianco's first 600 frames still cost about +330 retraces: the single pipeline thread is busy prewarming when the stage asks for its own pipelines.
- **QA sweep:** `port/build_aurora/qa_sweep/` (31 stage/scenario runs, 5 shots each) with `index.md` listing what looks right and wrong.
  Open visual items: the black band under every shot (w8's 640x528 EFB, screenshot/presentation crop), the faint FLUDD water stream (refraction pass built from EFB copies, w8), Mario as a pink glowing blob undersea, Pianta's brightness, the offset silhouette shadow on water beds.
  Gameplay: Bianco boss (55,0) crashes in `TPollutionLayer::cleaned` from `TMario::gunExec` (not rendering).
- **Dirty Mario:** not captured. The airstrip goop floats on the sea and the scripted walks ended in water; the dirt texture path was checked statically in R9.





# Level sweep C (w13): Delfino Plaza story states, secret courses, Corona Mountain, Bowser

Stage numbers (from `data/stageArc.bin`, the `ScenarioArchiveNamesInStage` table `TApplication::mountStageArchive` reads):
1 = Delfino Plaza (`dolpic0, dolpic1, dolpic10, -, -, dolpic5..9`: scenarios 0, 1, 2, 5, 6, 7, 8, 9; 3 and 4 are `none.arc`),
20-27 = `dolpic_ex0..7` (only 0-4 on the disc; `SMS_isExMap()` is 21-52, so 20 is not an EX map), 28/29 = `bia_ex0/1`, 30/31 = `rico_ex0/1`,
32/33 = `mam_ex0/1`, 34-39 = `pinna_ex0..5` (none on the disc), 40/41 = `sirena_ex0/1`, 42/43 = `monte_ex0/1`, 44/45 = `mare_ex0/1`,
46-52 = `coro_ex0..6` (52 = Corona Mountain, in `TMarDirector::changeState`'s banner list), 60 = `coronaBoss` (Bowser's bath).
On the disc: 20-24, 29-33, 40-42, 44, 46-48, 50-52, 60.

How the plaza layout is picked: `TMarDirector::moveStage()` turns a next area of (1, 0xFF) into `decideNextScenario(1)`
(`MarDirectorDirect.cpp`): `MSF_CORONA_VISITED` -> 2 (post-game); all seven Shadow Mario shines (6, 0x10, 0x1A, 0x24, 0x2E, 0x38, 0x42) -> 9;
`MSF_BOWSER_BOAT_CUTSCENE_TRIGGERED` -> 8; M on boathouse and lighthouse -> 7 with 10+ shines, else 6; M on statue -> 5;
`MSF_BIANCO_POLLUTED_PIRANHA_DEFEATED` -> 1; else 0. `TMarDirector::setupObjects()` then reads more flags: scenario 5 starts the Ricco
(3+ shines, no boathouse M) and Gelato (5+, no lighthouse M) unlock demos, scenario 8 sets `MSF_SHADOW_MARIO_EVENT`
(1: shine 0x21 and no Yoshi; 2: Yoshi, no plaza turbo nozzle, 25+ shines; 3: turbo but no rocket, 30+ shines) and `MSF_NOKI_AVAILABLE` (20+).
So `--warp 1,<scenario>` picks the layout directly, and the flags below make the objects match that story state.

## Knobs added (`Application.cpp`, `SMS_PORT`)
- `SMS_PORT_WARP_FLAGS=<items>` (with `SMS_PORT_WARP`): sets save flags on the warp's fresh save, after `firstStart()`.
  Items: `<hex flag>` (bool true / int 1), `<hex flag>=<dec>`, `S<hex>[-<hex>]` (`setShineFlag`, keeps the count right), and cumulative presets
  `story1`, `story5`, `story6`, `story7`, `story8` (no Shadow Mario event), `story8y` / `story8t` / `story8r` (events 1 / 2 / 3), `story9`, `story2`
  (each sets the flags `decideNextScenario`/`setupObjects` read for that plaza scenario plus plausible shines, visits and watched FMVs).
  Examples: `SMS_PORT_WARP=1,8 SMS_PORT_WARP_FLAGS=story8r`; `SMS_PORT_WARP_FLAGS=story9,20001=0` (no spare lives, to test the game over).
- `SMS_PORT_DEBUG_CALL=<what>@<frame>` (once per stage): `bathdemo` calls `TBathtub::startDemo()` in the Bowser fight (what the fifth super hip
  drop does), so the ending (streaming movie 14, epilogue, staff roll, omake, save prompt, title) can be driven; `tpshine` moves Mario onto the object
  named `郢ｧ・ｷ郢晢ｽ｣郢ｧ・､郢晢ｽｳ` (the shine at the end of a secret course), so the touch -> shine-get demo -> plaza path runs.

## C1. Corona Bowser fight: Mario falls through the bathtub (`JGeometry::TMatrix34::concat`)
- **Symptom:** `--warp 60,0`: Mario drops from y 5401 with no floor below (floor -11000) and dies (`MARIO_STATUS_FALL_DEAD`) about 150 frames after
  the intro; on later lives he only sometimes lands.
- **Cause:** decomp transcription bug in a JSystem header. `TMatrix34::concat(a, b)` (`JGMatrix34.hpp`) read `a.at(3, n)` / `b.at(3, n)`, past the 3x4
  matrix, and transposed the product; the 1-argument `concat(b)` transposed the rotation. The GameCube code (traced with a symbolic evaluator over the
  target asm of `TBathtubGrip::calcRootMatrix` and `TKoopaJrSubmarine::calcRootMatrix`) is a plain row-by-column 3x4 product (`m[i][j] = sum_k a[i][k] b[k][j]`,
  plus `a[i][3]` in the translation column). `TBathtubGrip::calcRootMatrix` and `TBathtub::setupCollisions_` build the tub's grip and inside-wall
  collision matrices with it, so the collisions were placed at garbage positions.
- **Fix:** both `concat` forms compute the product under `SMS_PORT`. Other callers fixed by this: `BathWaterManager` (drop matrices),
  `JDrama::TPolarCamera::perform`, `JDrama::TSmJ3DAct::perform`, `TKoopaJrSubmarine::calcRootMatrix` (the submarine was rotated the wrong way).
- **Verified:** Mario stands on the tub (floor 5400), takes Bowser's fire (`FIRE_DOWN`, hp 8 -> 1) and plays 6600+ frames over four lives.

## C2. Corona Bowser fight: Aurora FATAL `convert_texture: unknown texture format 22` at load
- **Cause:** port gap. `TBathWaterMeshRenderer`'s constructor makes `clear_z_TX`, a 4x4 `GX_TF_Z24X8` texture; Aurora cannot decode Z formats.
  It is sampled in `prerender` with colour and alpha updates off. `--nodraw` / `--nullgfx` do not get past it (not a J3D display list).
- **Fix:** `BathWaterManager.cpp` (`SMS_PORT_AURORA`) initialises it as `GX_TF_RGBA8`: read as colour, a Z24X8 texel is the RGBA8 texel with the
  same bytes (same 4x4 AR/GB tile layout), so the sampled values are the GameCube's.
- Not checked (w8): the depth copy `GXCopyTex(unk800A4, GX_TF_Z24X8)` that `makeHeightMap` reads back on the CPU (no crash).

## Found here, already fixed in main by w10 (taken from main, my versions dropped)
- Plaza goop cleaning crash in `TPollutionLayer::cleaned` (`pos[10]` written by the original `> 10` wrap; clang put `counter_x` there): main's entry 12.
- Plaza scenario 8, third Shadow Mario event: `JKRArchive::findFsResource` on `"/../map/pad3/Setting.prm"` (root `..` index 0xFFFFFFFF): main's S9
  (`portDirEntry()`, reproduces the 32-bit wrap).
- Game over never finished: `Hx_GameOver`'s `boundtimer` loads `boundtable`'s -8.0 as u32 (`__cvt_fp2unsigned` gives 0 on the GameCube, x64 gave
  0xFFFFFFF8, so the game-over wipe never ended and the director stayed in state 7): main's `HX_F2U` on those two loads.

## Status (Aurora build, real D3D12, `SMS_PORT_THP_SPEED=16`, test saves in a scratch `SMS_PORT_USER_DIR`)
Input for the long runs: A presses, stick sweeps and a G-autopilot tour of the plaza (ten waypoints between +-7000/+-9000, R spray every 90 frames,
A every 200). "Frames" are `[port] frame` counts (2 per game frame). Last full pass on main as of 16:33 plus C1/C2.

| Stage / state | Flags | Result |
|---|---|---|
| Plaza 0 (goop, first visit; arrival movies BA/BB/BC + save prompt) | fresh | 7200-8400 frames with the tour |
| Plaza 1 (kagemario movie, reload) | `story1` | 8400 frames, tour |
| Plaza 2 (post-game) | `story2` | 8400 frames, tour |
| Plaza 5 (statue M; Ricco + Gelato unlock demos) | `story5` | 7800 frames, tour |
| Plaza 6 (boathouse + lighthouse M, < 10 shines) | `story6` | 7800 frames, tour |
| Plaza 7 (10+ shines) | `story7` | 8400 frames, tour |
| Plaza 8, no event / event 1 (Yoshi pineapple) / event 2 (turbo) / event 3 (rocket) | `story8` / `8y` / `8t` / `8r` | 7200-8400 frames each, tour |
| Plaza 9 (flooded, all Shadow Marios beaten) | `story9` | 7800-8400 frames, tour |
| Plaza 3 / 4 | - | `none.arc`, not reachable in the game (crash in `TGuide::load`, not pursued) |
| EX 20-24, 29-33, 40-42, 44, 46-48, 50-51 | `story7`/`story9` | 3600-4200 frames each with deaths and restarts; no crash |
| EX shine-get -> plaza (`tpshine`) | `story9` | 21, 29, 31, 32, 40, 41, 42, 46, 47, 48, 50, 51: get demo, state 11, back to plaza 1/9 (20, 22-24, 30, 33, 44: no object named `郢ｧ・ｷ郢晢ｽ｣郢ｧ・､郢晢ｽｳ`) |
| Corona Mountain (52) | `story9` | 9600 frames, deaths in lava |
| Bowser fight (60) | `story9` | 7800 frames, four lives (C1, C2) |
| Game over (60, `20001=0`) | `story9` | death -> game-over wipe -> save prompt -> `TFlagManager::restore()` -> airstrip, as for an unsaved GameCube file |
| Ending (`bathdemo`) | `story9` | movie 14 -> epilogue -> staff roll -> omakeA -> save prompt -> title stage, 9600 frames (needs w6's `mVcdVatCmd` NOP padding, now in main) |

Seen, not mine:
- `GXPeekARGB` / `GXReadPixMetric` still log "unimplemented on Aurora" in the plaza (w8, if not already covered by `port_efb.cpp`).
- Environment: with two or more D3D12 windows open, one run failed with `D3D12 create graphics pipeline state failed with E_OUTOFMEMORY` and one crashed
  inside the AMD driver; both passed when rerun. Screen-copy screenshots pick up other agents' windows when they overlap.
- Not a crash, for whoever polishes the Corona boss units: `tools/semdiff.py` still shows logic differences in `BathWaterManager` (`perform`,
  `throwMario`, `initializeIfYet_`), `BathtubKiller` and `MapObjCorona` (`calcBathtubData`, `updatePosture_`).





## 13. DVD reads completed before the request call returned (stage BGM refused, flags stuck)
- **Symptom (found by w5):** after a stage's entrance demo, the stage BGM never started.
  `JAIBasic::checkEntriedSeq` refused every sequence because a slot's `mLoadingFlag` stayed set.
- **Cause:** `sdk_dvd.cpp` did every read inline.
  - `DVDReadAsync` ran its callback before returning, so code that sets a "loading" flag after issuing the read (as `JAIGFrameStream` does) never saw the flag cleared.
  - `DVDReadPrio` also returned instantly, so a higher-priority loader thread finished its whole request, callback included, before the lower-priority caller ran its next statement.
    That is the `JAIGFrameSequence` case: `loadFileAsync` hands the read to `JASDvdThread`.
  - On hardware, a request only starts the drive. A sync read blocks only its own thread, other threads run, and completion comes later as an interrupt.
- **Fix (port side only, `sdk_dvd.cpp` and `sdk_os_thread.cpp`):**
  - Async reads and `DVDPrepareStreamAsync` go into one queue, in issue order.
  - They complete in interrupt context through `port_retrace_defer`, at the first of:
    - no game thread runnable: `switchOut` raises an immediate one-shot interrupt that drains the queue;
    - the next VI retrace: `port_retrace_run()` from `viRetrace` in `port_main.cpp`;
    - a transfer-time bound of 0.2 ms + length at 20 MB/s (`port_defer_kick`).
  - Work deferred from inside a handler (chained reads) waits for the next drain.
  - The callback never runs before the caller's next statement.
  - Block state is WAITING while queued behind others, BUSY while first in line, then END or FATAL_ERROR.
  - `DVDGetDriveStatus` reports BUSY while anything is queued. `DVDCancel` and `DVDCancelAsync` cancel a queued request, whose callback gets `DVD_RESULT_CANCELED`.
  - Code that cannot sleep reads inline: interrupt context, or a host thread that is not a game thread (THP reader, card worker; `port_cannot_block`).
- **Sync reads:**
  - **Default:** a sync read first waits for the async requests issued before it (one drive), then reads inline.
  - **`SMS_PORT_DVD_SYNC_BLOCK=1`:** the hardware behaviour. The request joins the same queue and the caller sleeps until it completes, so other threads run meanwhile.
    - This fixes w5's `JAIGFrameSequence` case at the root, with the original flag order: plaza (warp 1,0) stage BGM averages 23 voices against 12 without it.
    - It is off by default because it exposes an intermittent JAudio crash at the file select (2 of 6 runs with START presses through the title; 0 of 6 by default).
      - The crash: `JAIBasic::processFrameWork` dereferences `mSeSequence == nullptr`.
      - A hardware watchpoint shows `checkStoppedSeq` cleared the handle because `checkSeqActiveFlag` returned 0 for the SE sequence 0x80000800.
      - No child of that track ended through `TTrack::mainProc`.
      - The rate moved with unrelated code-layout changes (debug prints that never fired took it to 0 of 4), which points to undefined behaviour in JAudio that the new timing exposes.
      - Next: w10's UBSan/ASan builds with `SMS_PORT_DVD_SYNC_BLOCK=1`, then make it the default.
- **w5's two flag-order patches:**
  - The `JAIGFrameStream.cpp` one (`DvdLoadFlag = 1` before the read) is no longer needed: async callbacks are deferred now. Revert it.
  - The `JAIGFrameSequence.cpp` one (`mLoadingFlag` before `loadFileAsync`) is still needed while sync reads are inline by default. It becomes unnecessary once `SMS_PORT_DVD_SYNC_BLOCK` is the default.
- **Tested in the default mode, real D3D12:**
  - Boot 遶翫・intro 遶翫・title 遶翫・file select: 6 of 6 runs, no crash.
  - Warps 1,0, 2,0 and 6,0 load and start their stage BGM request, with no crash.
  - The airstrip new-game path is unchanged.
- **Determinism:** the idle and retrace points are deterministic in game terms. The 20 MB/s bound uses host time, so w9's input replay may want it off (it only matters while another thread runs a long frame).
- **ARQ/AR DMA (w5's `sdk_audio.cpp`) still completes inline**, with the same hazard. Suggested change, using the same helper:
  - `ARQPostRequest` and `ARStartDMA` should do the copy and the callback in a handler passed to `port_retrace_defer`.
  - That handler should copy at completion time, not at post time.



# Performance (w14)

Goal: hold the original 30 fps with steady pacing and load fast, without changing game behaviour.
Measured on the user's i9-12900K + RX 9070 XT (D3D12) while 8 other agents' `sms_port.exe` and builds ran, so absolute numbers are noisy; A/B runs alternate both variants of the same binary through environment switches.

## Profiler: `SMS_PORT_PROFILE=1`
`port/include/port_profile.h`, `port/src/port_profile.cpp`. Off by default (each hook is one branch on a global).
Every `SMS_PORT_PROFILE_N` presented frames (default 300) stderr gets the present-to-present interval (avg, sd, min, p1, p99, max, frames more than 2 ms off), the whole-process CPU use, and avg/p99/max ms per frame plus calls per frame for:

| Row | What |
|---|---|
| `direct` | `TApplication::gameLoop` `mDirector->direct()`; in a stage split into `move` (simulation ticks, perform lists) and `gxgen` (draw perform lists: J3D/GX command generation) |
| `setupObj` | `TMarDirector::setupObjects` (main thread, at a stage load) |
| `viwait` / `wakelat` | main thread asleep in `VIWaitForRetrace` / retrace interrupt to main thread running again |
| `fifo` | waiting for Aurora's FIFO worker: the game's per-frame `GXDrawDone` (`THPPlayerDrawDone`) and the drain before the present |
| `endframe` / `beginframe` | `aurora_end_frame` / event pump + `aurora_begin_frame` (frame-slot wait = GPU back-pressure) |
| `audio` | `port_dsp_render_subframe` |
| `handoff` / `preempt` | host thread switch latency / time a readied thread waited for a lower-priority one to reach a scheduling point (the GameCube preempts at once) |
| `intrwait` / `irqlate` | contended `gIntr` (interrupts disabled elsewhere) / VI retrace interrupt lateness |
| `dvd`, `yaz0`, `swap`, `heap` | DVD reads (bytes too), Yaz0/Yay0 decompression (incl. its reads), J3D model/anim swap passes, `JKRHeap::alloc/free` |

At each `TApplication` state change a load measurement starts; when the stage's first frame is presented it prints `[prof] load: ... first stage frame presented N ms after the state change`, the load's DVD/Yaz0/swap/heap/setupObjects/audio totals and the read-ahead cache counters.
`SMS_PORT_PROFILE=2` also prints a backtrace where a thread ends a > 4 ms stretch without a scheduling point.
`SMS_PORT_PROFILE=3` instead dumps the main thread's stack (and the thread holding the CPU; every game thread with `SMS_PORT_PROFILE_ALL=1`) when no frame was presented for 150 ms (`SMS_PORT_PROFILE_HITCH_MS`), then resumes the threads. Debugging only (it suspends threads to walk them).

## What the profile showed
- The game is far from CPU-bound: main thread busy 2-8 ms of the 33.4 ms frame, game logic + GX generation 1-6 ms, audio 0.2-0.3 ms. 30.0 fps everywhere once loaded.
- Swap passes run once per file (magic marker): 2-3 ms per stage load in total. DVD: ~1450 reads / 15 MB per stage load. Heap: ~44000 calls, 2-4 ms. No debug logging in any per-frame path.
- `-O1` vs `-O2`: per-frame game time within noise; load CPU work about 5-10% lower. Clean build time unchanged (~50 s at `-j 16`).
- Plaza's 0.5 s extra load is the original's: `TMarDirector::thpInit` waits 0.5 s (`OSGetTick` loop) after preparing the plaza TV movie; it is in the US DOL (`802B3654`), so it stays.
- Port-caused problems found:
  1. VI retrace up to 1.5 ms late (millisecond condition-variable timeout; w9's later "sleep 1 ms short, spin" fixed the lateness by spinning up to 2 ms before every interrupt).
  2. A higher-priority thread readied by an interrupt (the audio thread on every AI/DSP interrupt) waited until the running thread reached a scheduling point: 4-20 ms while the main thread drew, 30-80 ms during `setupObjects`, 50-110 ms behind big synchronous DVD reads at boot, 220 ms behind `THPPlayerPrepare` on a cold file cache. The GameCube switches at the interrupt.
  3. The main thread held the CPU while Aurora waited for the GPU in `aurora_begin_frame` (100-270 ms seen under GPU contention), starving the audio thread the same way.
  4. The present moved one retrace later whenever a frame's CPU work exceeded one retrace (16.7 ms), so intervals alternated 50/17 ms. With the EFB readbacks (`port_efb.cpp`) draining Aurora's FIFO mid-frame, Bianco's `gxgen` is 4-6 ms (p99 13-16 ms) and this hit 1.5-3% of frames.
  5. ARQ completion bus-time model (w5, A-15: 20 us + 12 ns/byte): JASystem's DVD thread posts 32 KB chunks and spins until each completes while `setupObjects` (`MSound::loadGroupWave`) blocks on its queue. Bianco `setupObjects` 44 -> 200 ms, plaza +170 ms. Fixed after the restart (see "ARQ race" below).

## Changes
- **Interrupt timer** (`sdk_os_thread.cpp`): high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) + wake event; only the last 0.5 ms before a due time is spun. About the same lateness as w9's spin version, 6-15% of a core less process CPU.
- **Scheduling points** (`port_sched_point()`): `port_GXCallDisplayList`, `JKRHeap::alloc`, the inline read of `DVDReadPrio`. A pending switch to a higher-priority thread is taken there (only with interrupts enabled), as the GameCube would have taken it at the interrupt.
- **CPU release around host blocking work** (`port_blocking_begin/end`): `DVDReadPrio`'s inline read when 64 KB or more (the file handle is opened first; since w7 made `SMS_PORT_DVD_SYNC_BLOCK` the default, that path only runs with `SMS_PORT_DVD_SYNC_BLOCK=0`: sync readers now sleep on w7's queue, which releases the CPU by itself), and `THPPlayerPrepare`'s wait for the first decoded frame. Like a GameCube thread asleep during a DMA, other game threads run meanwhile. When no thread is runnable it calls w7's `onIdle()` like `switchOut`.
- **In-place preemption** (`port_preemptible_begin/end`): around the Aurora present and `THPPlayerDrawDone`'s `GXDrawDone`, threads of priority <= 9 (JUTException 0, audio 2, JASystem DVD 3, JKRAram/AramStream/Decomp 8) take the CPU at once. Lower-priority threads still wait: Aurora is not reentrant, and the draw-sync thread (15) peeks the EFB for the lens flare.
- **Present rule** (`port_main.cpp`, w9's area): present on the retrace after `TVideo::waitForRetrace`'s `VIFlush` (renamed to `port_VIFlush` by `build_aurora.py`), where the GameCube VI flips. Steady cadence for any frame CPU time below 33 ms; one retrace more latency than before, the same as the GameCube. `SMS_PORT_PRESENT_EARLY=1` restores the old rule.
- **DVD read-ahead cache** (new `port/src/port_dvd_cache.cpp` + `.h`; one marked hook per read path in `sdk_dvd.cpp`'s `doRead`): reads below 256 KB are served from 256 KB chunks (aligned in the host file for extracted folders, on the disc for images, so a compressed RVZ/WIA/GCZ block is decoded once and then copied from memory) in a 16-chunk LRU (4 MB); a read that continues the previous read of the same source prefetches the next chunk on a background host thread (at most 2 in flight; a read of a chunk in flight waits for it). Reads of 256 KB or more go straight to the destination. It changes only how fast the host copy is made: when a read completes for the game is still decided by `sdk_dvd.cpp` (w7's queue, `port_retrace_defer`, `SMS_PORT_DVD_SYNC_BLOCK`), and the bytes are identical (`SMS_PORT_DVD_READAHEAD=verify` compares every read with a direct one: 0 mismatches on folder, .iso and .rvz). Thread safety: own lock, never held during I/O; `port_disc_read` is thread-safe (w9). `SMS_PORT_DVD_READAHEAD=0` turns it off.
- **Build** (`build_aurora.py`): `-O2` (`SMS_PORT_OPT` overrides). No `-march` (x86-64-v3 would let clang fuse multiply-adds and change float results). ThinLTO is not usable: the `llvm-objcopy --redefine-syms` pass cannot rewrite bitcode objects.
- `SMS_PORT_W14_LEGACY=1` turns the scheduler changes off (w9's timer, no extra scheduling points, no CPU release) for A/B runs.

## Results
Pacing, same binary, warm pipeline cache, 2 rounds x 2 runs (before = `SMS_PORT_W14_LEGACY=1 SMS_PORT_PRESENT_EARLY=1`, i.e. main as of 17:00; steady-state windows after the load):

| Scene | Present interval sd (ms) | p1 / p99 (ms) | Frames > 2 ms off | Process CPU | `preempt` ms/frame |
|---|---|---|---|---|---|
| Title (attract) | 1.3-6.3 -> 0.40-0.46 (one run 5.7) | 30.7-31.8 / 34.8-58 -> 31.5-32.1 / 34.7-51 | 4-16 -> 1-12 per 600 | 26-38% -> 22-23% | 0.25-0.95 -> 0.06-0.20 |
| Delfino Plaza (1,0) | 0.18-7.0 -> 0.46-1.27 | 23.8-32.5 / 34.1-43 -> 31.0-32.2 / 34.6-36.0 | 5-91 -> 9-33 per 1800 | 44-58% -> 35-46% | 0.60-3.45 -> 0.40-0.79 |
| Bianco Hills (2,0) | 1.47-3.62 -> 0.55-0.81 | 30.1-31.3 / 34.9-43.8 -> 30.8-31.7 / 34.8-35.8 | 14-46 -> 12-31 per 1800 | 54-116% -> 42-52% | 0.93-2.16 -> 0.63-0.98 |

Plaza's own frames rarely pass 16.7 ms of CPU, so its gain is mostly the preemption and CPU columns.

Stage loads (state change to first stage frame), read-ahead off -> on (same binary; the rest of each load is game work, `setupObjects` waiting for the ARQ wave uploads, and plaza's 0.5 s THP wait):

| Source | Bianco load | Bianco DVD time | Bianco Yaz0 | Plaza load | Plaza DVD time |
|---|---|---|---|---|---|
| extracted folder | 271-272 -> 271-275 ms | 12.3-13.5 -> 3.8-4.0 ms | 47-48 -> 42-44 ms | 775 -> 779 ms | 12.4 -> 6.7 ms |
| .iso | 226-229 -> 226-271 ms | 6.7-7.3 -> 2.6-6.0 ms | 37-43 -> 35-46 ms | 773 -> 791 ms | 12.9 -> 6.1 ms |
| .rvz (zstd 5, 128 KB blocks) | 292-305 -> 258-297 ms | 27-30 -> 6.3-6.4 ms | 66-72 -> 50 ms | 807 -> 788 ms | 26.2 -> 6.9 ms |

Cache counters per Bianco load: ~1450 hits, 6-9 misses, ~60 chunks prefetched. The DVD layer was never the bottleneck (the folder/iso host reads were already 4-13 ms per load); RVZ gains most.
Earlier, against the pre-w9 timer (plaza, older tree): VI `irqlate` 0.24 -> 0.06 ms/frame, `preempt` 0.50 -> 0.15 ms/frame, present sd 0.48 -> 0.35 ms.

First visits with the bundled pipeline seed (w6, `initial_pipeline_cache.db`, empty per-run cache): Bianco 0 and 56 retraces lost after the load (no freeze); plaza 18 retraces in one run, but 672 retraces (a 6.7 s `beginframe` stall) in the other, probably while the 899 seeded pipelines were still being prewarmed on a busy machine. Without the seed the plaza lost ~575 retraces (9.6 s).

## Left as is (other owners)
- ~~ARQ bus time (w5)~~: fixed by the restart below ("ARQ race").
- **EFB readbacks drain the FIFO mid-frame (w8, `port_efb.cpp`):** the largest per-frame cost now (Bianco `gxgen` 1.2 -> 4-6 ms, p99 13-16 ms). Far below 33 ms, and the present rule keeps it from showing as jitter.
- **Async DVD reads in interrupt context (w7):** `completeReq` reads on the interrupt thread with the interrupt lock held; a big async read there delays the VI retrace for its duration (the read-ahead cache helps for small reads only).
- Boot: `initialize_bootAfter` waits ~150 ms on `loadWave` (main thread, game design).
- The emulated 59.94 Hz VI runs on the host clock, not the display's refresh (w9).
- Several runs here died inside the AMD D3D12 driver (driver thread) while 8 other instances shared the GPU, with and without these changes.
- `SMS_PORT_THP_SPEED` > 1 stalls the intro movie for ~3 s (the decoder skipping through the file); normal speed plays smoothly.

## Restart (w14, 2026-10-08 evening): rebased onto main, w9's frame-rate option
The predecessor's work was rescued to `workers\w14_rescue\` (its scratch copies and rebase scripts in `_predecessor\`) and re-applied by hand onto main as of 22:29 (w9's interpolation, w13 regions, w8 present capture, w15 multiplayer hooks; every hook in RESTART_BRIEF kept).
- **Present rule vs. w9's interpolation** (`port_main.cpp`): the flip-on-`VIFlush` rule applies while no display frames are drawn (frame rate 30, the default, and outside stages at any rate). While `port_interp_active()`, w9's pacing places the logic frame's present as before: its segments start at the first retrace after the logic draw, and `interpWait` defers the logic slot to that retrace, so presenting a retrace later would put display frames out of step.
- Display frames (`displayFrame`) are counted by the profiler and their `aurora_end_frame`/`beginFrame` are a preemptible block like the logic present; the main thread's retrace sleeps (`interpWait` too) are timed as `viwait`, so display-frame rendering counts as busy time.
- Profiler: "off by >2 ms" is measured against the window's median interval when it is not ~33.4 ms (frame rate 60 and up); load lines also print the ARQ counters.

## ARQ race (w14 edit in w5's `sdk_audio.cpp`, marked; w5 please keep)
`JASystem::Dvd::loadToAramDvdTMain` posts each 32 KB chunk and then does `++bufferFull` (a plain read-modify-write of a volatile); the completion `aramDmaFinish` does `--bufferFull` on the host interrupt thread, which runs truly in parallel. If the poster's host thread is descheduled inside its `++` for longer than the completion delay, one update is lost and the DVD thread (priority 3) spins forever in `while (bufferFull != 0)`, starving every lower-priority thread (the predecessor saw 3 hangs in ~20 runs with a 20 us delay; with w5's 12 ns/byte model the window is ~0.4 ms, so rarer, not gone).
Fix: AR/ARQ completions go through one FIFO queue (`arqComplete`) and are delivered after their transfer time **and** once the posting game thread has certainly gone past the code after `ARQPostRequest`: it passed a scheduling point since posting (new `port_thread_sched_epoch`, bumped in `checkPreempt`, i.e. every interrupt-enabled `OSRestoreInterrupts`/`OSEnableInterrupts`, `port_os_poll`, `port_sched_point`; for the DVD thread that is the next chunk's `DVDReadPrio` or `DVDClose`), or it is no longer the running game thread, or its host thread used 200k CPU cycles (~50 us) since posting (the busy-waits with no SDK call in between; time spent descheduled does not count). Otherwise the head is re-checked every 50 us. Posts from interrupt context or host threads only wait for the transfer time. `SMS_PORT_ARQ_UNORDERED=1` (debug) delivers on time alone.
With the ordering guaranteed, the per-request cost is the fixed 20 us by default (`SMS_PORT_ARQ_BUS_TIME=1` restores w5's 20 us + 12 ns/byte): the game only ever waits on these completions, nothing reads the DMA duration.
Verification (same exe, extracted folder, `SMS_PORT_PROFILE`, scripted walking after the load, 4-6 instances in parallel on a machine shared with other agents): 30 alternating Bianco (2,0) / plaza (1,2) loads, 0 hangs; with the A/B and stress runs below, about 85 loads with the fast timing and no ARQ hang. The load lines show 2-21 "held" re-checks per load (completions that were due before their poster had moved on). 24 runs with `SMS_PORT_ARQ_UNORDERED=1` did not hang either, so the race stays rare even unguarded.
Two early runs (fresh pipeline caches, 3 instances right after the smoke suite) stopped presenting ~0.5 s after Mario got control and never recovered within the 35 s run. Not reproduced in ~60 later runs, cold caches included. The cold-cache runs with `SMS_PORT_PROFILE=3` showed 4-12 s stalls, each one with the main thread inside `aurora_begin_frame` 竊・`wait_for_gpu_progress` (GPU/pipeline back-pressure, not a game-thread spin; an ARQ spin would leave main in `waitForCpu`), so these were most likely GPU stalls longer than the rest of the run.

## Re-measured after the restart (w14 exe as merged; before = `SMS_PORT_W14_LEGACY=1 SMS_PORT_PRESENT_EARLY=1 SMS_PORT_ARQ_BUS_TIME=1 SMS_PORT_DVD_READAHEAD=0`)
Same binary, variants interleaved, 2 runs per cell, 60 s each, 4 instances at a time, warm per-slot pipeline caches, `SMS_PORT_FPS` = w9's frame rate; pacing = medians over the 300-presented-frame windows after the load (scripted walking/camera):

| Scene, rate | Load (ms) | setupObjects (ms) | Present interval sd (ms) | p1 / p99 / max (ms) | Process CPU (% of a core) | `preempt` ms/frame |
|---|---|---|---|---|---|---|
| Bianco 2,0 @30 | 266 -> 99 | 193 -> 56 | 1.72 -> 0.76 | 30.6/34.9/50.2 -> 31.7/35.4/38.6 | 57 -> 47 | 0.86 -> 0.67 |
| Plaza 1,2 @30 | 792 -> 650 | 210 -> 66 | 0.60 -> 0.60 | 32.1/34.2/37.5 -> 31.7/34.6/37.9 | 47 -> 37 | 0.81 -> 0.63 |
| Bianco 2,0 @60 | 275 -> 99 | 197 -> 55 | 3.58 -> 3.88 | 8.7/24.3/28.6 -> 8.7/24.8/28.2 | 79 -> 68 | 0.77 -> 0.59 |
| Plaza 1,2 @60 | 783 -> 650 | 207 -> 70 | 3.01 -> 3.50 | 10.2/22.7/23.8 -> 9.9/23.5/24.9 | 70 -> 57 | 0.87 -> 0.72 |

59.93-59.94 fps at 60 and 29.97 at 30 in every cell. Plaza's load includes the original 0.5 s `thpInit` wait; 1,2 is used because 1,0/1,1 on a fresh save start with a movie director (the load line then measures the movie) and 1,3 crashes in `TGuide::load` (`J2DTextBox::setFont` on a null font, warp-only, not looked into).
Further load runs (30 s each, 5 per cell): Bianco 283 (before) / 266 (bus time only) / 99-115 (now) ms; the ARQ timing is nearly the whole gain, the read-ahead and scheduler changes the rest.
**For w9 (frame pacing at 60):** the interval sd of ~3.5 ms at 60 is w9's pacing, not w14's (the same with the w14 changes off): a display frame is started `port_interp_lead()` (= recent maximum cost + 1.2 ms, decaying 3%/frame) before its due time and presented as soon as it is drawn, so it lands early by the lead minus its cost (logged pairs: logic -> display ~15 ms, display -> logic ~18.5 ms; p1 8.7 ms after a cost spike). Holding the finished display frame until its due time (`aurora_end_frame` at `target`, waiting with the CPU released to priority <= 9 threads) should bring it close to the 30 fps sd.

# Level sweep A (w11): Bianco Hills, Ricco Harbor, Gelato Beach, Pinna Park

How the levels were run: `SMS_PORT_WARP=<area>,<scenario>` (w7's warp), scripted input (below), and an 85 s run per scenario, with a crash log and Mario's position every 10 s.
Area numbers come from `/data/stageArc.bin` (the "ScenarioArchiveNamesInStage" table `TApplication::mountStageArchive` indexes):
- 2 = Bianco Hills (scenario 0-7 = bianco0, 1, 2, 3, 4, 7, 6, 5), 55 = biancoBoss
- 3 = Ricco Harbor (ricco0-7), 59 = ricco8
- 4 = Gelato Beach (mamma0-7)
- 5 = Pinna Park beach (pinnaBeach0, 1, 3, 2, 4, 4, 4, 4), 13 = Pinna Park (pinnaParco0-7; 8-10 = pinnaDemo0-2), 58 = pinnaBoss0/1 (jet coaster)
- secret courses are 28-39 (bia_ex*, rico_ex*, mam_ex*, pinna_ex*): w12's area.

New debug knobs (all off by default):
- `SMS_PORT_STICKSCRIPT` (`port/src/port_sweep_input.cpp`, hooked with one line in `port_main.cpp`'s `port_PADRead`):
  `<from>-<to>:<x>,<y>[,<buttons>]` holds the main stick and buttons on pad 0 for those frames;
  `auto:<from>[-<to>]` walks Mario in a slowly turning circle, sprays (R) every other 2 s and jumps every 4 s.
  (`SMS_PORT_PADSCRIPT` has since gained `S<degrees>` stick tokens; the `auto` walk could be folded into it.)
- `SMS_PORT_LOGMARIO=1` (`MarDirectorDirect.cpp`): Mario's position, status and HP every 10 s of gameplay.
- `TMarDirector::changeState` logs every director state change (`[port] TMarDirector state 1 -> 3`), like `TApplication::proc` does.

## Found in the sweep and fixed by someone else in parallel (nothing of w11's left to merge)
- Fatal WGSL "cannot index into expression of type 'f32'" on every polluted stage (alpha compare `GX_TEV_COMP_R8_GT` from `PollutionCount.cpp`): `aurora_shader.patch` 1/3 (w6). w11's patch was the same.
- Fatal "unhandled tcg src 21" on every Gelato Beach episode (an indirect stage reading a texcoord past `numTexGens`): `aurora_shader.patch` 2/3 (w6). w11 used texcoord 0 (Dolphin's quirk); w6 uses (0, 0).
- Fatal "unsupported logic op 10" in Pinna Park episodes 3/4: `aurora_logic_ops.patch` in `port_deps`.
- Audio thread crashes in `TChannelMgr::receiveAllChannels` / `TNoteMgr::getChannel(28)` (note-on bitfield order in `JASSeqParser.cpp`): w7.
- biancoBoss / ricco8 crash in `TPollutionLayer::cleaned` (`pos[10]` written past the array; on the GameCube it lands in an unused destructor-chain record): w7 (`pos[11]`).
- Gelato Beach episode 2 crash in `TDemoBossHanachanManager::clipEnemies` (empty `LINK_ALL_SOURCE` constructors): w10 S8.
- Gelato Beach episode 5 (Il Piantissimo) crash in `JKRArchive::findFsResource` for `"/../map/pad/Setting.prm"` (root `..` = index 0xFFFFFFFF): w10 S9. w11 had checked on `params.szs` that the GameCube's wrapped lookup finds nothing.
- Gelato Beach episode 5: a 32768x32768 texture at `0xC0928E60` (TEX1 header with 0x5A in the pad byte on disc): w10 (`port_swap_tex1_timgs_force`).
- Pinna Park episodes 1 and 5 crash in `TMarioInputReplay::play` (Shadow Mario `.pad` replays): w10 (`port_swap_pad`).
- Pinna Park episode 7 -> coaster: Aurora "draw vertex data overrun" from the stale tail of the `J3DShape` VCD/VAT list: w6 (NOP padding).

## A1. Jet coaster camera (pinnaBoss 0/1, also entered from Pinna Park episode 7) becomes NaN on its first frame: crash in `matan`, then in `TMapCollisionData::checkGround`
- **Symptom:** `CPolarSubCamera::calcExternalData_` -> `CLBCrossToPolar` -> `matan` reads `atntable[0x80000000]`.
  With `matan` fixed alone, the next NaN consumer (`TMapObjWave::getHeight` -> `checkGround` grid index) crashed.
- **Cause:** an uninitialised read that differs between the platforms.
  `ctrlJetCoasterCamera_` (L-button mode, the default) builds the camera from the Torocco's joint-2 matrix (`getToroccoMtx_`) before the Torocco model has had its first `calc()`.
  `J3DModel::entryModelData` allocates `mNodeMatrices` uninitialised. On the GameCube the heap still holds the previous stage's data, but the PC arena starts out zeroed.
  The zero up axis is normalised to NaN (as `frsqrte(0)` would on hardware), and the camera chases NaN for the rest of the stage.
- **Fix:**
  - `J3DModel.cpp`, under `SMS_PORT`: `mNodeMatrices` start as identity matrices; the first `calc()` overwrites them as before.
  - `MathUtil.cpp` `GetAtanTable`, under `SMS_PORT`: NaN or out-of-range ratios follow the GameCube's `fctiwz` saturation and 32-bit address arithmetic (index 0x80000000 * 2 wraps to `atntable[0]`), instead of reading 4 GB away.
- Verified: `58,0`, `58,1` and `13,6` (into the coaster) run to the end of the test with Mario moving along the track.

## Notes from the sweep
- `13,8`-`13,10` (pinnaDemo0-2) have no archive on the disc (`/data/scene` has no `pinnaDemo*.szs`), so `mountStageArchive` returns null; they are not reachable in the game. Not a port bug.
- With several `sms_port` instances on one GPU, two runs died inside the AMD D3D12 driver while compiling a pipeline (`E_OUTOFMEMORY`, and a crash in the driver's shader compiler); a rerun passed both times. (`aurora_pipeline_wait.patch` now retries `E_OUTOFMEMORY`.)
- With main's rendering fixes (synced 16:3x), gameplay screenshots of these levels show the stage, Mario and the HUD (e.g. Bianco 1 and the pinnaBoss coaster at frame 2400). Before that sync they were black.

Status after the main sync (one 85 s run each, scripted walk/spray/jump, no crash): Bianco 2,0-2,7 and 55,0; Ricco 3,0-3,7 and 59,0; Gelato 4,0-4,7; Pinna beach 5,0-5,7; Pinna Park 13,0-13,7; pinnaBoss 58,0/58,1.
(Full sweep on the build before the main sync; a 10-scenario smoke set on the final one.)


# Level sweep B (w12): Sirena Beach, Hotel Delfino, casino, Noki Bay, Pianta Village
Areas (from `data/stageArc.bin`, the table order is the area number): 6 Sirena (`sirena0-7`), 7 Hotel Delfino (`delfino0-4`), 14 casino (`casino0-1`),
56 `delfinoBoss`, 9 Noki Bay (`mare0-7`), 16 `mareUndersea`, 57 `mareBoss`, 8 Pianta Village (`monte*`, scenario n loads the archive the table lists, not `monte<n>`).
The bottle level `mare_ex0` (area 44, the diving secret) is entered from `--warp 9,2` by the scripted input below; the EX courses belong to w13.

How it was run: `SMS_PORT_WARP=<area>,<scenario>`, Null backend, 75-130 s per scenario (4200-7200 presented frames),
`SMS_PORT_PADSCRIPT` pressing A every 40 frames from frame 240 to 1200 (skips the intro dialogue), B twice, and w11's `SMS_PORT_STICKSCRIPT=auto:<frame>`
(`port_sweep_input.cpp`, walks in a turning circle, sprays, jumps; hooked into `port_PADRead` the same way as in w11).
Mario often loses a life and the game returns to Delfino Plaza (area 1) before the run ends; that is the game's own behaviour.
Real D3D12 shots of Sirena 6,1, Hotel 7,0, casino 14,0, Noki 9,0 and Pianta 8,0 at frame 1800 all render the level, HUD, Mario and NPCs.

## B1. `JKRArchive::findFsResource` reads at `mDirectories + 0xFFFFFFFF` (Noki Bay episode 5, Shadow Mario)
- **Symptom:** `--warp 9,4` crashes in `setupObjects`: `TEMario::load` 遶翫・`TEnemyMario::initEnemyValues` 遶翫・`TSettingParams` 遶翫・`TParams::load` 遶翫・`findFsResource`, reading `0x10_81F8003A`.
- **Cause:** 64-bit pointer arithmetic on an index the GameCube wraps.
  `TEnemyMario` loads `"/../map/pad/Setting.prm"` (also `pad2`, `pad3`). The scene archive misses (from `/map/params`, `..` is `/map`, which has no `map`), so `TParams::load` falls back to `params.szs` from its root, and the root's `..` entry has directory index 0xFFFFFFFF.
  On the GameCube `mDirectories + 0xFFFFFFFF` wraps to the 16 bytes before the node table (the info block tail, read as 256 entries from file entry 0); `params.szs` has no `map` among them, so the lookup returns null and Shadow Mario keeps the default settings. On x64 the index is not wrapped.
- **Fix:** `libs/JSystem/src/JKernel/JKRArchivePri.cpp`, under `SMS_PORT`: `findFsResource` returns null for a directory index past `num_nodes`, which is the GameCube result for the only path that gets there. GameCube DOL unchanged (shasum OK).

## Crashes in these levels fixed by other workers (picked up from main / their trees while sweeping)
- Every stage start: audio-thread crash in `TChannelMgr::getListHead` (list head `0x0003000000000000`). Found with a watchpoint: `TTrack::noteOn` stored `TNoteMgr::unk20[index]` with a voice index read from the wrong bits of the note-on byte (bitfield order). Fixed the same way by w5/w7 (`JASSeqParser.cpp`, in main).
- Hotel Delfino episode 4 (`--warp 7,3`): `TMarioInputReplay::play` reads `0xE0A56AC0`, the `.pad` Shadow Mario replay is big-endian. w10's `port_swap_pad` (A5).
- Sirena episode 6 (`--warp 6,5`) with spraying: `TPollutionLayer::cleaned` reads through a corrupted `counter_x`, the original's `pos[10]` overrun. w7/w11's `pos[11]` (in main).
- Noki Bay episode 5: Aurora reads a texture past the end of RAM (`0xC09E5160` = an unswapped ResTIMG image offset). Gone with w10's TEX1 header fix (A6).
- Bottle level (area 44, from `--warp 9,2`): Aurora fatal `draw vertex data overrun` in `J3DShape::draw`, the stale tail of `mVcdVatCmd` (w7's note). Fixed by the NOP padding in main's `J3DShape.cpp`.
- Aurora fatals that stopped Sirena 6,0 (`unmapped vtx attr 13`), Noki/Pianta (`unhandled tcg src 21`), the WGSL alpha-compare error, and `unsupported logic op 10` (Mario/Shadow Mario silhouette, `GXSetBlendMode(GX_BM_LOGIC, ..., GX_LO_INV)` with colour and alpha updates off, `MarioDraw.cpp`) are all handled by the shared Aurora as of 16:00.

## Status (final pass, main as of 16:00 plus B1)
No crash in any scenario. Each row: loads, frames presented when the run was stopped (all runs alive), and where the run ended.
| Warp | Level | Result |
|---|---|---|
| 6,0-6,7 | Sirena Beach episodes 1-8 | all load, 4800 frames, alive; 6,5 ends in the plaza (life lost) |
| 7,0-7,4 | Hotel Delfino (5 scenarios, incl. 7,3 Shadow Mario replay) | all load, 4800 frames, alive |
| 14,0 / 14,1 | casino | load, 4800 frames, alive |
| 56,0 | `delfinoBoss` | loads, 4800 frames, alive |
| 9,0-9,7 | Noki Bay episodes 1-8 | all load, 4800 frames, alive; 9,0/9,1/9,3 end in the plaza; 9,2 enters the bottle level (area 44) and keeps running |
| 16,0 | `mareUndersea` | loads, 4800 frames, alive |
| 57,0 | `mareBoss` | loads, 4800 frames, alive (ends in the plaza) |
| 8,0-8,7 | Pianta Village episodes 1-8 | all load, 4800 frames, alive; with the circling stick input Mario loses a life and the run ends in the plaza (with buttons only, all 8 stay in the village for 3000 frames) |

## Open items
- `--warp 60,0` (`coronaBoss`, now w13): Aurora fatal `convert_texture: unknown texture format 22` right after the stage loads (seen before the scope change).
- Most Noki/Pianta runs end in the plaza after Mario loses a life, so later parts of those levels (bosses that need progress, e.g. the Pianta Village chain chomp / Noki episode objectives) were only reached through `mareBoss`/`delfinoBoss` warps, not by playing.


(Note: B1 was not merged; main's JKRArchive::portDirEntry from w10 S9 covers the same case.)

# Constructor audit (w11)

Scope (shared with w13: w11 owns constructors and factories): every class built in the name-ref factories (`TMarNameRefGen::getNameRef*`), the enemy managers' `createEnemyInstance`, every `__ct__` function, and every function that constructs a class whose constructor the GameCube inlines (no `__ct__` symbol).

Method:
- `tools/ctordiff.py <unit> [function]` (new): compares target and our object function by function as a multiset of initialisation events, ignoring registers and scheduling.
  Events are calls, and stores through non-stack bases with offset and constant/relocated value (vtables show up as `@__vt__X`).
  `semdiff.py` drops offset stores as noise, so it cannot see missing field initialisation.
- Factories: per-`strcmp`-block structural diff of `decomp-diff --no-collapse` output (a block whose class has an inlined constructor shows its base-ctor call, vtable and field stores).
- All units were swept: 449 classes in the factories, about 180 inlined-constructor sites in non-100% functions, and all `__ct__` functions present in both objects. Also checked: no game `__ct__` exists in a target object without a copy in ours, apart from `TTelesaSlot` (ours is inlined; same stores, checked by hand).
- Noise left out: nerve singletons (`@instance$N` naming), relocations against anonymous data (`@L` vs a named array), indexed stores (`stwx`, not modelled), and the same field reached from a different base register.

Fixes (all plain decomp corrections; the GameCube DOL check still passes):
1. **Mario's and Yoshi's animation sounds were plain `MAnmSound`.** `TMario::initValues` (MarioInit.s) and `TYoshi::init` (2x, Yoshi.s) construct `MAnmSound` and then store `__vt__14MAnmSoundMario` at +0x94.
   So they are `MAnmSoundMario`, whose `startAnimSound` override plays Mario's surface-dependent animation sounds. Ours built the base class, so the override never ran.
   `MarioInit.cpp`, `Yoshi.cpp`: `new MAnmSoundMario(...)`. `TMario::initValues` is now 100% fuzzy (was 98.8), `TYoshi::init` 99.6.
2. **"HideObj" built a `THideObjBase`.** The factory (MarNameRefGen_MapObj.s 0x31A0) calls `THideObjBase(name)` and then stores the `THideObj` vtable.
   `THideObj` had a declared but undefined constructor, so the source used the base class, which lacks `THideObj::touchPlayer` (touch-to-reveal objects never revealed).
   `MapObjHide.hpp`: inline `THideObj` constructor; factory uses `new THideObj`.
3. **"WatermelonStatic" missing from `getNameRef_MapObj`.** The target has an entry between "WaterMelon" and "ShiningStone" (0x1C28): `new(0x138)`, `TMapObjBase("陜暦ｽｺ陞ｳ螢ｹ縺帷ｹｧ・､郢ｧ・ｫ")`, `TWatermelonStatic` vtable.
   Ours returned null for it. `MapObjMamma.hpp`: inline constructor; `MarNameRefGen_MapObj.cpp`: the entry.
4. `TMapObjStartDemo`: the inlined constructor (0x624) also zeroes `unk138` (`MapObjTown.hpp`).
5. `TPictureTelesa`: the inlined constructor (0x2828) clears `unk174` (`stb 0`) (`MapObjSirena.hpp`).
6. `TBossEel` from the factory gets the name `"?"` (`@3235`, like the other boss entries), not the header default (`MarNameRefGen_BossEnemy.cpp`).
- Effect on matching: `getNameRef_MapObj` 96.7 -> 97.5%. What remains in the factories is out-of-line versus inlined constructors (our constructors are separate functions where the GameCube inlines them; checked block by block to make the same calls and stores), plus register allocation.

Checked, no change needed:
- `TTelesaSlot`, `TFruitBasket` (`unk150 = 0`), `TSimpleEffect` (`unk44 = 1`), `TGateShadow`, `TBasketReverse`, `TWaterHit/FruitHit/HipDropHideObj`, `TMewManager`, `TDiffusionNameKuriManager`, `TNameKuriLauncher`, `TNpcParts` (24 pointers zeroed), the TBubble/TBossEel/TBossPakkun/TBossGesso/TBossWanwan/TTinKoopa/TKoopaJr part constructors, and the Sleep/DemoBossHanachan stand-ins (w10 S8).
- `TBathWaterManager::loadAfter`: same final vtables. `symbols.txt` names the flat renderer's vtable `__vt__18TBathWaterRenderer` (its entries are the `TBathWaterFlatRenderer` functions), so the names differ but not the behaviour.
- `getNameRef_NPC` and `getNameRef_Map` match; `getNameRef` (MarNameRefGen.cpp) has no structural differences.

Port check: `4,7` (watermelon festival), `2,0` and `1,0` warps run.
`1,0` (Delfino Plaza) never leaves director state 0 within 40 s. Main's own exe (17:05) does the same, so it is not caused by these changes (w7's area).


## R14. SRTG texgens used the vertex colour instead of the lit colour (Mario's toon shading; also an AMD driver crash)
- **Found via:** "Mario as a pink blob undersea". Of the 99 shaders of the Noki undersea stage, 22 had an SRTG texgen whose coordinate came from `in_clr0`, or from `vec4f(0.0)` when the vertex has no colour.
- **GX semantics:** `GX_TG_SRTG` with source `GX_TG_COLOR0/1` takes S/T from the colour channel's output after lighting (Dolphin: `o.colors_N.xy`). SMS uses it for toon shading: the lit colour indexes `H_toon_*` ramp textures (seen in Mario's materials, the diving helmet top and the pollution-ground material of the maps; 22 of the 99 undersea pipelines).
  Aurora used the raw vertex colour, so those ramps were sampled at one fixed texel and the toon shading was flat.
- **Fix:** `aurora_shader.patch` (4/4): the texgen reads the lit colour, and `shader_info` marks the channel as used so its lighting and uniforms exist even if no TEV stage rasterizes it.
  The first version read `out.ccN` back from the vertex output struct. The AMD D3D12 driver then crashed (access violation inside `CreateGraphicsPipelineState`, from Aurora's pipeline thread) in 3 of 4 Noki undersea runs. The lighting now goes to a local `ccN_lit` and is copied to `out.ccN`; 3 clean runs followed.
- **Result:** Mario's cap and clothes now show the toon ramp: a lit top, a darker band and the highlight (`port/build_aurora/shots/mario_srtg_after.png` vs `final/airstrip_shot_1400.png`).


## R15. "Mario as a pink glowing blob" in Noki undersea / Noki boss: original look
- The blob is the diving helmet `/mario/watergun2/body/diver_helm.bmd`, seen from the top-down underwater camera. Material `_mat_diverhelm_helm` has no texture.
  Stage 0: `prev = rasC0 - 0.5`; colour channel 0 is lit with diffuse SIGN and spot attenuation.
  Stage 1: `prev = 2 * (lerp(C2 = (105, 93, 178), C1 = (44, 52, 97), prev) + rasC1)`; channel 1 is the specular light (light 2, `GX_AF_SPEC`).
  The result is a translucent lavender/blue glass with a white specular spot. Mario's red cap seen through it reads as pink.
  Aurora's shader for this material implements exactly that (TEV colours from the material, SIGN diffuse, SPEC attenuation, channel outputs clamped like the hardware).
- The helmet's top cap (`_mat_diverhelm_top`, toon ramp `H_toon_2_4i` via SRTG) was affected by R14 and is fixed.


## R16. Mario's shadow as a sharp silhouette offset from him on river/sea beds, and Pianta's brightness: original look
- **Shadow:** `TMBindShadowManager::perform` sets `mLightDir = normalize(gpLightManager->getLightPos())`, and the shadow volume extrudes Mario's silhouette along that direction (`drawShadowVolume`, `calcVtx`).
  The sun is not overhead, so on a floor below the water surface the shadow lands away from Mario; it is a sharp silhouette by design. The volume passes (alpha add/subtract, then a colour pass) render as the game sets them.
- **Pianta:** the ground material `_green1` is `texture x vertex colour` with lighting off (shader: `mix(0, tex, rast0)`; `rast0 = in_clr0`).
  Its texture `S_green_mon1_mip` (CMPR 256x256) decodes to an average of RGB (0, 133, 0): pure green with darker patches.
  The map's vertex colours average (118, 132, 209), a blue night tint, and are fetched byte-exact. So the saturated green with zero red/blue is in the data.
  The night stages that are dark (Hotel Delfino, Sirena dusk) are dark through the same path.
- **Indirect texturing (Aurora vs GX):** the paths SMS uses were read in the generated shaders. They are `ITF_8` with STU bias -128, static matrices `ITM_0..2` (s.10 elements, scale exponent), wrap off, texcoord scale applied before the indirect scale, and the indirect texture read as `.abg` (as Dolphin does).
  The texcoord-beyond-`numTexGens` case is R3. No discrepancy was found. The undersea caustics were not isolated in a capture.






## W7-G. Gameplay driving: scripted input reference and airstrip boss status
- **PADSCRIPT entry forms** (`port_main.cpp`):
  - `<f>:` holds the input for 10 frames.
  - `<a>-<b>:` holds it over a range.
  - `<a>-<b>/<p>:` taps it for 5 frames every `p` frames.
  - `+<f>`: counted from when the current stage gave Mario control.
  - `+<n>@<f>`: the n-th stage of the run (0 = first). This clock keeps counting through the movies after it.
  - `+d<f>`: counted from the end of the latest demo or talk (director mode back to 0), e.g. a boss's appear demo.
- **Tokens:** buttons or a hex mask; `S<deg>` (main stick, 0 up, 90 right); `G<x>;<z>` (autopilot to world x,z; `Application.cpp`).
- **`SMS_PORT_TRACE=1`:** state changes, Mario's position, action, water, stick and speed every 120 frames, and a heartbeat every 600 frames.
- **`--warp 0,1`** loads the post-FLUDD airstrip directly (control at about f207), the fastest way into the Piranha fight.
- **FLUDD aiming (from the source):**
  - While R is fully held and Mario stands still, he is in `MARIO_STATUS_SQUAT` (0x0C008220).
  - There the pitch follows `unk36E + stickY * mRButtonMult`, clamped and chased, so stick Y tilts the stream.
  - The `G` autopilot holds stick Y near +1, which pins the aim low.
  - The nozzle only emits while R's ramp (`unk372`) catches up, so a held stream dies after about 450 frames. Pump R: 60 frames on, 2 off.
- **Airstrip Piranha, real rendering:**
  - Walk to the west ledge with `+0@100-300:G-5000;1250,+0@300-900:G-2750;1480`.
  - Pump R; it wakes at about +1130 and plays its appear demo.
  - One real-input hit (HP 3 to 2, AwakeDamage, animation 0xC) came from pumping R from the ledge with 16 four-frame `S0` pulses spread over about 3500 frames.
    That combination did not hit again in 3 more runs: the mouth timing has to match the stream.
  - Wait2 lasts about 600-1000 frames before Dive, and only the first appearance plays a demo.
  - The 10000-unit tank lasts about 6500 frames of spraying.
  - Still open: the remaining 2 hits (needs a refill route or spraying only while the head is vulnerable), the shine get, the arrest movie and the transition to the plaza.
- **Seen once and left alone (w6):** an access violation inside the AMD D3D12 driver during pipeline creation (no game frames), while many GPU instances from other agents were running.
- **Headless `build_port.py` link (`gPortCallbackR31`):** not a type mismatch.
  - Within each build the definition and the reference mangle the same way (`unsigned long` in headless, `unsigned int` in Aurora).
  - The failure came from a stale object or census list, and a fresh census plus rebuild links.


# Decomp corrections (w13)

Plain source fixes (no `SMS_PORT`) in NonMatching units where the source computed something other than the GameCube asm. They fix the PC port and the all-source GameCube DOL alike; `dtk shasum` stays OK.
Found with `tools/semdiff.py`, an aligned objdiff scan (instruction pairs whose difference is an immediate, a field offset on the same base register, a data value, a `cror` condition or a call only on one side), and w11's `port/ctordiff_candidates.md`. Field offsets were resolved with MWCC itself (`offsetof` probes compiled with the project flags). Each fix was rebuilt and re-diffed: the differing instruction pair is gone (match % in the entries). Original-game behaviour was left alone.
Checked and found to be codegen noise (no change): `setStatusToJumping` trample switch, `doRunning`, `setCheckData`, `calcBathtubData`, `throwMario`, `makeKillerVelocity`, `TKoopa` nerve statics, `makeMtxRotByAxis`, `calcTowerCenterPos_`, `TMareEventDepressWall::initCommon`, `execCameraModeChangeProc_` (inlining differences only).
Open: `TEnemyMario::setStickToAngle` multiplies by an sdata2 constant 0.0f in every inlined copy (we have 64.0f, and a literal 0.0f is folded by MWCC, so the real expression is unknown; TODO in source). `TMario::checkController` stores to nozzle+0x714 (left as is, see the existing TODO). `BathWaterManager::perform` still differs in its constants (1.0 vs 0.9/10); not resolved.

- Player/MarioRun `TMario::catching`: jump during a catch compared mForwardVel with mDeParams.mClashSpeed (+0x8E8); asm reads +0xCD0 = mJumpParams.mRotBroadEnableV (offset probed with MWCC). Now 99.9 (frame only).
- Player/MarioRun `TMario::surfing`: wall clash on the shell missed the system SE MSD_SE_SY_DAMAGE (0x4823, gateCheck + startSoundSystemSE in asm). Now 100%.
- Player/MarioRun `TMario::running` (2 sites): brake condition was normal.y <= cos 80 deg; asm `cror eq, gt, eq` = >=.
- Player/MarioSpecial `TMario::wireSWait`: used mWireBounceVelPrev (+0x448) where asm uses mWireBounceVel (+0x444) (read and the 5.0 store), and input mask 0x10000 where asm tests 0x8000.
- Player/MarioSpecial `TMario::barWait`: input mask 0x10000 -> 0x8000 (asm rlwinm bit 16).
- Player/MarioWait `TMario::waitingCommonEvents`: checkFlag(0x200000 IS_PERFORMING) -> 0x4000 (asm bit 17), TAKE_POSE input 0x10000 -> 0x8000.
- Player/MarioWait `TMario::sleeping`: wake-up voices used startSoundActor; asm calls startVoice. Now 100%.
- Player/MarioSpecial `TMario::hanging`: wall check before a ledge shuffle used newPos.y; asm uses 10.0f + newPos.y (same as record2).
- Player/MarioSpecial `TMario::fenceMove`: sideways fence movement used the wall normal (x, z) itself; asm uses it turned 90 deg (-z, x) / (z, -x), i.e. along the fence.
- Player/Yoshi `TYoshi::movement` (2 sites): the ground under Yoshi/the egg was one checkGround; asm then re-checks up to 5 times from groundY - 1 while the floor isMarioThrough() and not water (see-through floors). Also the B-button tongue in `movement` (an emitTongue() copy) must not set unkDC = 3: the asm of movement has no store there, unlike doSearch's copy, so movement now has the body without it (match 93.5 -> 90.4% from inlining isMarioThrough/isWaterSurface; logic now equal).
- System/MarDirectorDirect: `THPPlayerPlay()` for the plaza (stage 1) was in `currentStateFinalize` STATE_UNK0; the asm has it at the end of `nextStateInitialize` STATE_UNK2. `nextStateInitialize` STATE_CARD_SAVE also missed `if (3 <= unk261 < 5)` MSD_SE_SY_COLLECT_DELIGHT. currentStateFinalize now 100%, nextStateInitialize 95.2 -> 99.4%.
- Map/MapEventDolpic `TDolpicEventRiccoMammaGate::watch`: after SMS_MarioWarpRequest the asm plays MSD_SE_SY_CLEAR_SIGN_BIG (0x484D) through gateCheck; missing. 92.3 -> 99.8%.
- Enemy/walkerEnemy `TWalkerEnemy::moveObject`: the jump-start added 5.0f to mRotation.y (+0x30); asm adds it to mPosition.y (+0x14).
- MoveBG/MapObjMamma `TShiningStone::endDemo` (inlined into `TLeanMirror::control`): after each of the first two lights the asm shows balloon 0x32 (unk74 == 1) or 0x33; missing. LeanMirror::control 90.3 -> 99.9%.
- MoveBG/MapObjMamma `startCameraShakeSE` (demo camera callback of the Hanachan fall): asm is `li r3, 0; blr`; the source played MSD_SE_OBJ_QUAKE. Now 100%.
- MSound/MSound `MSound::talkModeIn`: BGM duck volume 0.6 -> 0.48 (US asm; under VERSION_GMSE01, others unchanged).
- GC2D/Talk2D2 `TTalk2D2::makeBoxLine`: line-full threshold 1.1 -> 1.05 (US asm; VERSION_GMSE01, like the neighbouring US offsets).
- Enemy/bossgesso `TNerveBGDie::execute`: death jump calcVelocityToJumpToY height 0.0 -> 50.0 (asm @4708).
- MoveBG/MapObjBall `TMapObjBall::touchWall`: wall bounce used -(-1 + restitution); asm adds +1.0 (reflection v += -(1 + e) (v.n) n). Affects every rolling map object (balls, fruit, watermelons).


## E9. Black band under every shot (rows 448-528)
- **Cause:** the EFB render target is sized in `webgpu::resize_swapchain`, which Aurora's `window.cpp resize_swapchain()` only called when the window size changed. The target was allocated while the boot render mode was 640x480 and was never re-sized when SMS switched to 640x448, so the presented part (448/528 of a target sized for 480) left a black band at the bottom.
- **Fix:** `aurora_efb_size.patch` gains a `lib/window.cpp` hunk: with an unchanged window size it still forwards to `webgpu::resize_swapchain` (a no-op when nothing changed), so the target follows render-mode changes. Every aspect and fullscreen go through the same path (present viewport and resample UVs use `present_content_width/height`). `SMS_PORT_SHOT` crops round to the nearest pixel (`port_aurora_sdk.cpp`): Bianco shots are now 1280x960 at 4:3, with no band (were 1280x896 with the band, then 1280x959).



## E10. Pollution counters: a fully cleaned layer read "never clean"; stage changes
- **Residual:** `TPollutionCounterLayer` subtracts `polygons * 4` from `GXReadPixMetric` (asm: an unconditional `subf`) because the GameCube counter counts 4 pixels per rasterized polygon even when every pixel fails the alpha test. The port counts only passing pixels, so a clean layer (2 polygons) read -8 = 0xFFFFFFF8, which the unsigned "cleaned" checks (`cmplw` against `mCleanedDegree`) treat as dirty forever. `src/Map/PollutionCount.cpp`: the subtraction is skipped under `#ifdef SMS_PORT` (GC build unchanged). A forced full wipe now reads degree 0.
- **Metric state per stage (`port_efb.cpp`):** draw-sync tokens are reused by every stage; a token not read for more than 10 frames starts over (result, zero streak and "seen non-zero" cleared; results read before the restart are dropped). The zero debounce is 2 reads once the counter has read non-zero this stage (goop being cleaned), still 30 for a counter that never did (stage start).
- **Copy readback after a CPU write:** readbacks of copies issued before the game rewrote a destination on the CPU (or before a destination was first seen) are dropped, so an old GPU copy cannot overwrite a fresh CPU wipe (`DestInfo::discardUpTo`).



## E11. Cleaned events and the airstrip
- The airstrip's progression is boss-based, not counter-based: `TAirportEventSink::watch` fires when the gatekeeper (Proto Piranha) is dead (demo camera, `layer(0)->startDecay()`, shine). With the gatekeeper marked dead by a temporary test aid, the demo camera ran, the layer decayed to 0 through the GPU decay pass, the counter read degree 0 and the Piantas cheered (`port/build_aurora/efb_airstrip_cleaned_event.png`). Test aids removed.
- Real spraying lowers the counts as on GC (airstrip row 4548 -> 3708, standing sweeps 4548 -> 2588; plaza 13346 -> 12453). A full scripted clean of the whole airstrip was not done: the goop count also rises (enemies, regrowth) and the scripted Mario dies. No script queries the pollution level in airstrip 0/1 or plaza 1/0.
- The 6306 (memory) vs 4548 (counter) difference on the airstrip is the game's own TEV rule: texels below the threshold decay by a step every frame, so the faint edges of the initial map fade. Not a port bug.



## E12. EFB-copy readback latency
- The CPU readers of pollution maps (`isPolluted`, goop collision, the cleaning effect at >100, `PollutionAction`, `updateDepthMap`) see a copy 1-2 frames later than on GC; none of them depends on a copy made in the same frame (each frame reads what the previous frames drew, and the map only changes through the game's own stamp/decay passes). The Manta and bath-water copies are GPU-only; `initTexImage` writes the CPU maps only at load. No copy needs to be synchronous.
- **Note:** main's `port_main.cpp` (as of 17:36) had lost the `port_efb_end_frame()` call before `aurora_end_frame()`. Without it finished copy readbacks are never stored, so the CPU pollution maps stay at their load-time contents (goop collision, cleaning effects and `isPolluted` ignore spraying). Restored in w8's `port_main.cpp`.



## E13. FLUDD water stream (reported as faint)
- Drawn by `TModelWaterManager::drawRefracAndSpec` over one camera-facing quad per flying particle (`TDLTexQuad`, stretched 4x along the view-space velocity): pass 1 = the screen texture (640x448 -> 320x224 RGB565 copy, made just before) through an indirect warp from `waterref.bti` (IA8, +-9 texels at 0.07), alpha = `waterMask.bti` (I8); pass 2 = water colour 0 at alpha 0x14 x mask; pass 3 = additive `C1 + C0*spec` with alpha = `waterSpec.bti` (I8, a small highlight at the top left).
- Checked one pass at a time on Bianco with temporary overrides: the quads rasterize where expected (solid red), the mask samples correctly (opaque grey shows a white body with dark edges), the warp shifts the background inside the stream (opaque pass 1 vs unwarped), and the highlight lands at the quad's front end. Copy order (pollution, shadow, screen texture, then the refraction) and the indirect matrix/bias/scale maths match the hardware (Dolphin's `>> 3`, s0.10 matrix, 1/128-texel offsets = Aurora's texel offsets). The water uses 1 particle per frame (pow 40, size 50); 3-11 quads are in flight in a standing spray. Turning the depth test off adds little.
- Result: no port-side difference found; the stream renders as the TEV setup describes (subtle refraction + highlights). Not changed. A side-by-side with real hardware or Dolphin is needed to say what "faint" differs in. Screenshots: `port/build_aurora/fludd_stream.png` (crop, standing spray), `fludd_stream_mask.png` (mask pass forced opaque).



## E14. `GXPeekARGB` / `GXReadPixMetric` "unimplemented" messages (w13)
- Main's Aurora build logs no "unimplemented"/"missing" line for them in the plaza or Bianco (`port_efb.cpp` / `port_aurora_sdk.cpp` implement both). The stubs in `stubs_auto.cpp` belong to the headless build. Most likely a stale `port/build_aurora/gen/stubs_aurora.cpp` in that worker: delete it and relink.
- PADSCRIPT `RA<0-255>` (`port_main.cpp`): analog R alone, without the digital R click (FLUDD: a light press sprays while walking; a full R press makes Mario stand still).






# Store/call audit (w11)

Method: `tools/ctordiff.py --rank` (now covering every NonMatching function in `report.json`, not only constructors) compares the target and our object per function as a multiset of calls, field stores (offset + constant), float literals and, with `-c`, field loads, `li` immediates and bit masks. Registers and scheduling are ignored.
Ranked lists: `--rank Player Camera MarioUtil Strategic -c` for w11's directories; everything else went to w13 in `port/ctordiff_candidates.md` (180 functions plus a false-positive guide).
Plain decomp corrections (no `SMS_PORT`); `dtk shasum` OK after every batch; Aurora build 619/619; smoke runs 2,0 / 3,0 / 4,0 / 5,1 / 13,1 without a crash.
w13 found several of the same bugs independently (catching, surfing SE, wireSWait, barWait, waitingCommonEvents, hanging, Yoshi see-through floors and the movement tongue): main's versions (w13 section above) were kept for those and are not repeated here.

Fixes (each re-diffed: the differing event is gone):
- Camera/CameraNotice `getNoticeActor_`: MsIsInSight got the Mario angle through DEG2SHORTANGLE; asm multiplies by 0.00549316 = 180/32768, i.e. SHORTANGLE2DEG.
- Camera/CameraNotice `calcNoticeTargetYrot_`: the angle ratio used DEG2SHORTANGLE(1.0f) (182.04); asm constant is 3.05176e-05 = 1/32768.
- Strategic/MirrorActor `TMirrorActor::checkIsInMirror` (2 sites): both "mirror model absent and not unk1A & 4" conditions also require !(unk1A & 0x20) (asm rlwinm bit 26). 95.3 -> 98.9%.
- Player/MarioAutodemo `TMario::electricDamage`: the end status is 0x88D (`li r4, 0x88d`), not MARIO_STATUS_WAIT. Now 100%.
- Player/Yoshi `TYoshi::changeHand`, `TYoshi::thinkAnimation`: hip-attack-end check compared 0x8023C; asm `addis -128; cmplwi 572` = 0x80023C (MARIO_STATUS_HIP_ATTACK_END). Main still had 0x8023C.
- Player/Yoshi `TYoshi::getOff`: missed the tongue reset at the end (`sth 0, 0x7c(mTongue)` = mTongue->mState = STATE_IDLE). 95.3 -> 98.1%.
- Player/MarioUpper `TMario::stateMachineUpper`: the pumping sweat check was the JP inline body of isEmitting; US calls the out-of-line `TWaterGun::isEmitting` (which also returns false in demo and talk modes). Under VERSION_GMSE01 like the header's isEmitting. 72.1 -> 100%.
- Player/MarioDraw `TMario::initModel`: the initial model transform's rotation.y read mFaceAngle.y; asm reads 0x9A = mModelFaceAngle (as calcBaseMtx does).

Checked and found to be noise (no change): ModelWaterManager::move and Tongue::movement (TVec3 copies/inline vs call), cameragc perform/loadAfter/calcPosAndAt (MsSqrtf/MsClamp/setLength inlined), execCameraModeChangeProc_ (changeCamModeSpecifyFrame_ inlined), calcInHouseNo_ / checkCurrentPlane / MarioWaistCtrl / MarioHeadCtrl / diving / lensflare / WaterGun::init / checkController (same field through another base register), setStatusToJumping (target multiplies by a literal 1.0), SMS_AddDamageFogEffect (-400 - -700 and 800 - 500 folded to 300), hanging / pulling / updateDemoCamera / initModel loads (reloads), sunmodel (TVec3 set inline), Yoshi::movement (isMarioThrough/isWaterSurface inlined).
Left: 27 NonMatching functions in these directories still show store/call events, all of them in the noise classes above (`scratchpad` rank, re-run `python tools/ctordiff.py --rank Player Camera MarioUtil Strategic -c`).


## MoveBG, NPC, Map (w11, second split)
After the coordinator's split (w11: MoveBG, NPC, Map; w13: Enemy, GC2D, MSound, System, Animal; header of `port/ctordiff_candidates.md`).
Sources: the MoveBG/NPC/Map entries of the candidates file, `ctordiff --rank MoveBG NPC Map` (26 functions, 62 with `-c`) and `aligned_scan.py` + `aligned_isolated.py` over the same directories. `dtk shasum` OK, Aurora build 619/619, smoke runs 2,0 / 3,0 / 5,0 / 5,4 without a crash.

Fixes:
- MoveBG/MapObjPinna `TFerrisWheel::control` (2 sites), `TFerrisWheel::initMapObj`: the US build used `rotate_frame_rate` there; US asm calls `SMSGetAnmFrameRate()` (as JP). In US `rotate_frame_rate`'s sdata slot is read only by TPinnaShell. GMSE01 branch of the VERSION_SELECT changed; PAL left alone. control 83.3 -> 88.6%, initMapObj 97.0 -> 99.8%. Same value at 60 Hz (60 / vsync rate = 1).
- MoveBG/MapObjFlag `TMapObjFlag::update` (inlined into `TMapObjFlagManager::perform`): US plays the flag SE only if `gpMarDirector->mMap != 3` (`lbz 0x7c; cmplwi 3` before the gateCheck), i.e. no flag sound in Ricco Harbor. Under VERSION_GMSE01. 91.6 -> 94.7%.

Not a bug:
- Map/BathWaterManager `TBathWaterManager::perform` (w13's open item "1.0 vs 0.9/10"): dtk merged the literals into a 6-byte symbol `@3422` = `.float 0.9` + `0x4120` (10.0 at +4), so the constants are equal. `tools/ctordiff.py` now decodes `sym+off` literals and no longer reports it.
- `TBathWaterManager::loadAfter` vtables: the target's `__vt__18TBathWaterRenderer` holds the TBathWaterFlatRenderer functions (dtk naming), so the same class is built.
- Inlining/codegen only: TBaseNPC::perform (execMotionBlend_, getAnmOffDist_, isPollutionNpc out of line in GC), TMirrorModelManager::perform (makeMirrorViewMtx inlined, existing TODO), TMapEventSinkBianco::loadAfter (base loadAfter inlined), TBathWaterMeshRenderer::render (drawCap inlined), TNpcParts ctor (zeroing loop unrolled differently), makeMActors (GC loads anim unkC and never uses it), NPCNeckCallBack, TRedCoinSwitch::control and TAmiKing::moveObject (same conditions, different switch/compare order), THideObjPictureTwin::loadAfter and initMonte (string pool offsets), FluffManager / Shine::calc / Bathtub / ModelGate / Mamma mirror (same field through another base), bgIntersectLine, TItemSlotDrum::generateItem (scheduling), getMActor / setMtx / TVec3 / SMatrix calls vs inline.
Open: `TNpcCoin::requestAppearCoin` uses 13650 = 75 * 182 for the 75-degree launch angle; ours `MsSin(75)` folds to 13653 (75 * 182.044). Direction differs by ~0.02 degrees; the original spelling (an integer 182 factor) is unknown, so it was not changed.


## Decomp corrections (w13), round 2

Scan tools now in the main repo: `tools/aligned_scan.py`, `tools/aligned_isolated.py`, `tools/lowmatch.py`, `tools/mwcc_probe.py` (usage in docs/PORT_AGENT_NOTES.md), plus w11's `ctordiff.py --rank -c`.
Area: src/Enemy, GC2D, MSound, System, Animal (MoveBG/NPC/Map handed to w11 mid-round; notes for them at the end of port/ctordiff_candidates.md). GameCube shasum OK after each batch; Aurora build OK, airport 0/0, Ricco 3/0, Pinna Park 13/0 and plaza 1/1 run 45 s without errors.
- Enemy/gatekeeper `TBiancoGateKeeper::startBGM`/`stopBGM` (inlined into the appear/die nerves): the US asm also calls `MSMainProc::setGateKeeperBGMPlayFlag(mVariant, true/false)`, and on stop only stops track 1 when `getGateKeeperBGMStopFlag()`, otherwise fades it with `setTrackVolume(1, 0.0f, 1, 0)`. Guarded `#if defined(VERSION_GMSE01) || defined(VERSION_GMSP01)` in gatekeeper.cpp, and the declarations/definitions in MSoundMainSide.hpp/.cpp got the same explicit guard (they are in the US DOL at 0x802BC540/0x802BC4F8; both match the US asm 100%). BGKAppear/BGKDie store and call sets now equal.
- Enemy/graph `TGraphWeb::getNearestPosOnGraphLink`: the result is point + link * t (asm fmadds link.x * t + point.x); the call `scaleAdd(t, link, point)` gave link + point * t with our TVec3::scaleAdd (b + c * s). Arguments swapped, with a comment.
- Enemy/enemy `TSpineEnemy::doShortCut`: the asm peeks at the top path node (`mData[size - 1]`, no pop) and only pops into unkF4 after the wall check; we popped twice, skipping a node on every shortcut. 94.7 -> 99.6%.
- Enemy/bgpoldrop `TBGPolDrop::move`: the second setBckFromIndex(13) goes to unk54 (the second MActor), not unk50.
- Enemy/bosstelesa `TBossTelesa::loadAfter`: the fruits get map-object flag 0x4000000 (+0xF8, onMapObjFlag), not live flag 0x4000000 (+0xF0).
- System/CardManager `buildHeader_` (inlined into getOptionWriteStream and filledInitData_): the US title/comment tables have 5 entries indexed directly by MSF_LANGUAGE ("%d/%d Save Data" for English, month first; German/French/Spanish/Italian variants), not JP's 7 entries indexed by language + 1. US-only under VERSION_GMSE01. getOptionWriteStream 87.0 -> 100%. This changes the comment of a newly written memory card file to the US one.
- GC2D/Talk2D2 `TTalk2D2::openWindow`: the window rotation is DEG_TO_RAD(-rot - 1.0f) in the US asm; we had the 1.0f missing. Under VERSION_GMSE01 (no JP/PAL asm to check, those keep the old expression). Note: `DEG_TO_RAD(x)` in include/macros.h has no parentheses around x, so the expression needs its own parentheses (no other call site passes a binary expression).
Checked and found to be codegen noise (no change): GCConsole2 perform (jet/coin counters, memset loop, life meter switch), BossHanachan perform (MsWrap out of line), bossgesso perform/doAttackSingle (showMessage, MsGetRotFromZaxisY inlined), bosstelesa PrepareSlot/flashItem, Koopa nerves, fruitsboat GraphWander (tail duplication), conductor isBossDefeated, Option checkInput, Guide perform (setCenteredSize inlined), SelectMenu perform (SMS_getShineID inlined), TabePuku swimTo / BeeHive doWait (TQuat4::slerp shape, a known blocker), bosseel perform, hinokuri2 Landing (dead read), Hx_Test4, MovieDirector dtor, EventWatcher/MAnmSound/MSoundSE (sqrt inlined), Bird init.
Not changed: smallEnemy `TNerveSmallEnemyChange` calls setFrameRate(0) twice in the asm (once before and once after the emitter); the second call has no effect.
Port note: `--nullgfx` (Dawn null device) runs now hit "Out of memory" creating the "port EFB compute output" buffer at stage load; real-GPU runs are fine.



# Low-match review (w11)

`python tools/lowmatch.py --below 85 Player/ Camera/ MarioUtil/ Strategic/ MoveBG/ NPC/ Map/`: 20 functions. Each was read against the asm with `tools/decomp-diff.py --no-collapse` (call sequence, branches and compares, field offsets, literal values, loop structure). `dtk shasum` OK, Aurora build 619/619, smoke runs 2,0 / 13,1 OK.
Result: no logic difference in any of the 20. The low percentages come from inlining (out-of-line TVec3/MsSqrtf/MsClamp/checkStatusType/setLength in GC vs inline in ours, or the reverse), loop unrolling, register allocation and frame size:
- TWarpInCallBack::execute: same three scale() calls (unk468, status timer, particle factor) in the same order.
- TYoshiTongue::movement: TVec3 copies via ctor/operator-= in GC vs inline; same control flow and calls.
- TBaseNPC::perform: execMotionBlend_/getAnmOffDist_/isPartsAnmNpc/isPollutionNpc out of line in GC, inline in ours (hence 2552 vs 2024 bytes); isNerveMaybeDontCalcAnim0/1 the reverse.
- TNpcEvent::reviveOneSunflower, TMapCollisionBase::updateTrans, TMapWarp::watchToWarp: scheduling and reloads only.
- TMapObjPlane::calcNrm: same four cross products; GC multiplies by a loaded 0.0 (an unfolded zero component) where ours folds it away. Identical for finite heights.
- TMapCollisionData::intersectLine: same grid walk, same list order (+0x4, +0x1c, +0x10 = lists 0, 2, 1) and bgIntersectLine calls; the LineInLineXZ part is inlined on both sides.
- TBathtubData::getGravityDir, TBathWaterManager::throwMario: the matrix rows/columns used for the local and world vectors decode to the same offsets on both sides (xDir +0x18.., zDir +0x30..); quaternion math with the same sign pattern.
- TBathWaterMeshRenderer::prerender: identical GX call sequence.
- CPolarSubCamera::calcInHouseNo_ (9-entry loop unrolled 3x3 vs 8+1), perform, isMarioCrabWalk_/isMarioAimWithGun_ (checkStatusType out of line), CLBCalcNearNinePos: inline vs call.
- Already covered: makeMtxRotByAxis, calcTowerCenterPos_ (w13), TMirrorModelManager::perform (existing TODO), TMapEventSinkBianco::loadAfter (base loadAfter inlined).

Fix:
- NPC/NpcCoin `TNpcCoin::requestAppearCoin`: the 75-degree coin launch angle is an immediate `li r3, 13650` (75 * 182, an integer factor) used for both the sin and cos lookups; `MsSin(75)`/`MsCos(75)` fold to 13653. Now `JMASSin(75 * 182)` / `JMASCos(75 * 182)`. 99.9 -> 100%.


## Decomp corrections (w13), round 3

Area: src/Enemy, GC2D, MSound, System, Animal. Went through the rest of the candidates list and `tools/lowmatch.py --below 85` (47 functions), plus `ctordiff.py --rank -c` filtered for differing immediates, constant stores and bit masks. GameCube shasum OK; Aurora build OK; boot run shows the logo screens.
- System/GCLogoDir `TNintendo2D` (constructed in `TGCLogoDir::setup`): the US logo colour is (220, 0, 0, 255), red; the source had (0, 70, 255, 255). Under VERSION_GMSE01 in include/System/GCLogoDir.hpp (other regions unchanged, no asm to check). Checked with a port screenshot: the boot logo is now the red US one.
- Enemy/bosseel `TBossEel::perform`: the vacuum (bck 14) sound is 0x2180 in the US asm, not MSD_SE_BS_UNG_VACUUM (0x8120, which the US build never uses). Under VERSION_GMSE01.
Checked and found to be codegen noise (no change): MSound getDistPowFromCamera (add order), bosspakkun setGroundCollision, rocket setDeadAnm, BossHanachan execWalk/getBodyMaxRotateZ, koopajr/BathtubKiller manager load/loadAfter, TBubbleManager::createEnemyInstance (constructor), TabePuku Drag / Bird WalkOnGround / AnimalBase execWalk (TQuat4::rotate inlined in the asm, JGQuat4.hpp, out of scope), TTPHitActor::updateTerrainCollsion, feetinv (normalize then scale vs one setLength), limitkoopa head receiveMessage (pushNerve out of line), Kukku/KukkuBall, CardLoad perform, Bird init, Kazekun/doLanding (sqrt inlined).
`MSStageCubeFadeDouble::proc` (96.3%): different bool/branch layout only; same condition, same calls in the same order.




# Smoke suite (w10)
`python port/tools/smoke_all.py` runs the Aurora build through a fixed list of scenes and writes `<build>/smoke/report.md` and `report.json`, comparing with the previous run (new failures at the top). Options: `-j N` (default 3), `--quick` (one scenario per area, 45 s), `--asan`/`--ubsan`, `--only <scenes>`, `--repeat N` (intermittent crashes), `--exe <path>` (test another build), `--no-build`, `--no-seed`, `--list`, `--duration`.
- **Isolation:** the exe, PDB, DLLs and pipeline seed are copied to `smoke/bin_<timestamp>/` first, so merges can relink `port/build_aurora/sms_port.exe` during a run. Each scene has its own working directory, memory card (`SMS_PORT_USER_DIR`), pipeline cache and settings file. Real D3D12 with `SMS_PORT_OFFSCREEN=1`: the window is off-screen and never activated; screenshots are EFB readbacks.
- **Scenes (93):** `boot` (logo, intro, title, attract loop); `newgame` (START through title and file select, A after; STUCK unless area 0 gameplay starts); `fileselect_dvdsync` (START into the file select with `SMS_PORT_DVD_SYNC_BLOCK=1`); warps for every area 0-9 with scenarios 0-2, the remaining plaza states, episodes 4/6/7 of each level, Hotel Delfino, Pinna Park, the casino, Noki undersea, every EX course and boss on the disc, Corona. The area table comes from the disc's `stageArc.bin` (area 8 is Pianta Village/`monte`, 9 is Noki Bay/`mare`; earlier w10 notes that called `--warp 8,1` Noki Bay meant Pianta Village episode 2).
- **Input:** A through dialogs, then (from when Mario gets control) run, jump, spray, dive and turn.
- **Per scene:** PASS / FAIL (early exit, `[port] CRASH`, `[aurora FATAL`, stack cookie) / STUCK (never reached gameplay), the crash excerpt, frames reached, last TApplication state, Aurora ERROR/WARNING counts, audio underruns and drops (`SMS_PORT_AUDIO_LOG`), sanitizer report count, mid-run and last screenshots (PNG).

## First run on main (2026-10-08 18:25, 93 scenes x 75 s, -j 3, 39 min)
PASS 91, FAIL 1, STUCK 1. No audio underruns in any scene; 96 Aurora errors and 190 warnings in total (none above 2 per scene).
- **`0,1` (airport1): exited with 0xC0000409 (fast fail) after frame 3600, no trace.** Not reproduced in 6 re-runs. Fast fail bypasses the unhandled-exception filter (abort(), std::terminate, CRT invalid parameter, pure call), so `port_crash.cpp` now traps those too and prints `[port] CRASH: abort() / SIGABRT` (etc.) with a backtrace; the next occurrence will have a stack.
- **`newgame`: STUCK at the file select (w7/w5).** START opens the "create a file on the Memory Card?" prompt; YES writes `01-GMSE-super_mario_sunshine.gci`, the game returns to "Select data." with all three files still "New", and further START presses bring the prompt back instead of starting file A. A only makes Mario jump. Screenshots: `smoke/newgame/`.
- An earlier hand run hit 5 crashes in 6 inside the AMD D3D12 driver (`amdxc64.dll`) from `aurora::gx::build_pipeline` and once from `aurora::gfx::initialize` (`tex_copy_conv::create_pipeline`), while about ten other agents' sms_port instances were running. Not reproduced afterwards (0 in 30+ runs); environmental GPU/driver stress (w6 for awareness).

## `SMS_PORT_DVD_SYNC_BLOCK=1` file-select crash (w7's BOOTLOG 13 report)
Not reproduced on current main: 0 crashes in 30 normal runs (70 s and 180 s), 6 ASan runs and 6 UBSan runs, nor with w7's own current exe (6 runs), all START through the title into the file select. ASan reported nothing in JAudio; UBSan only the known harmless items. Candidates that changed since w7's observation: the JAudio out-of-bounds reads fixed by the sanitizers (`setBusConnect` past `connect_table`, `cmdIIRCutOff` past `CUTOFF_TO_IIR_TABLE`), the `MixConfig` and `JASSeqParser` bit-order fixes. The smoke suite now runs `fileselect_dvdsync` every time, so blocking sync reads can become the default and any regression will show there (use `--only fileselect_dvdsync --repeat 6` to hunt).

## Report summary line, newgame duration, sanitizer flag
- `report.md` starts with one line: `**OK|PROBLEMS: PASS x, FAIL y, STUCK z** (time, N scenes) | NEW FAILURES: ... (or "no new failures") | fixed: ... | not passing: ... | SANITIZER REPORTS in N scenes: ...`.
- Sanitizer reports do not fail a scene (`halt_on_error=0`), but the summary now lists every scene that had any, so they cannot hide behind PASS.
- `newgame` always runs at least 90 s (`min_duration`), `--quick` included: area 0 starts around frame 3000-3600 (55-65 s at -j 3), so the quick 45 s was always STUCK. `--quick` on main's 19:02 exe: 20 of 20 PASS.

## Run on main 2026-10-08 19:13 (today's merges), compared with 18:25
PASS 92, FAIL 0, STUCK 1; no new failures.
- `0,1` fixed (it passed; the 18:25 fast fail was intermittent, see below).
- `newgame` STUCK: that run started before w7's new `NEWGAME_SCRIPT` landed; with it, `newgame` passes (W7-H).
- Aurora 95 errors / 186 warnings in total (96/190 before), 0 underruns in every scene.
- Ricco and Gelato scenes reached 2400 frames instead of 3600 (machine load at the time; 3600 again on a re-run).

## airport1 0xC0000409 hunt
- With the fast-fail tracing (`[port] CRASH: abort() / SIGABRT`, invalid parameter, pure call) in main's exe: `--only 0,1 --repeat 20 -j 4` gave 20 of 20 PASS; no trace has been caught yet.
- The ASan pass of the same (`--asan --only 0,1 --repeat 20`) passed 20 of 20, but every run logged the same global-buffer-overflow:
  - `TConsoleStr::load` reads `scScenarioNameTable[86]` (70 entries) for the airport's shine 0x56. This is the original game's out-of-bounds read; the GameCube asm does the same.
  - On the GMSE01 disc that word is SelectMenu.o's `.data` `@1210[0]` = 0 (0x803C0CB8; GMSJ01 and GMSP01 land on the same object), so the game used scenario message 0.
  - On PC the read returned whatever global the linker put there; an out-of-range index makes `SMSGetMessageData` return null (`"(null)"` text), so it was not a crash.
  - Fix (`src/GC2D/ConsoleStr.cpp`, `#ifdef SMS_PORT`): an index past the table uses message 0, the disc value. GameCube `mario.dol: OK`; ASan airport `0,0`/`0,1` are clean.
- The fast fail stays open. The next one will print a trace if it comes through abort, an invalid parameter or a pure call; a `/GS` cookie failure or `__fastfail` from inside a driver would not.

## Merge safety: `port/tools/merge_check.py`
- `python port/tools/merge_check.py wN` compares every port/ file across three versions (base = main at the worker's last sync, the worker's copy, main's copy) and sorts them:
  - worker-only: safe to copy;
  - main-only: the worker's copy is STALE, do not copy;
  - BOTH: both sides changed it; a line-based 3-way merge says clean or CONFLICT;
  - new-in-worker, deleted-in-worker, new-in-main.
- `--merge-out DIR` writes the 3-way results, with `<<<<<<< worker / ======= / >>>>>>> main` blocks for conflicts.
- `--apply` copies the worker-only files and writes the clean merges into main; conflicts are never applied. `--apply-new` also copies new files.
- `--all` also checks src/include/libs/config, using `.sync_manifest`.
- The base is recorded with `merge_check.py wN --record`: `.sync_port_manifest` (MD5s) and `.sync_port_base/` (text copies) in the worker folder.
  - `tools/agent_sync.ps1 -Port` (w10's copy, to be merged) copies main's port/ into the worker and records the base right after.
  - Without a base, files that differ are listed as UNKNOWN, with a "MAIN NEWER" or "worker newer" hint.


## E15. Copy-filter users, checked in game (`GXSetCopyFilter`, E8)
- Users found with a temporary log in `TEfbCtrlTex::perform` (removed): in every stage the mirror stage `鬮・｡隰蜀怜愛郢ｧ・ｹ郢昴・繝ｻ郢ｧ・ｸ` (256x256 RGB5A3, `SMSVFilter_flicker`, flags 0x228) and the screen texture `鬨ｾ螢ｼ・ｸ・ｸ郢ｧ・ｷ郢晢ｽｼ郢晢ｽｳ隰蜀怜愛郢ｧ・ｹ郢昴・繝ｻ郢ｧ・ｸ` (640x448 -> 320x224 RGB565, mip, flicker filter); in the Bowser bath `BathWaterManager::draw_mist` (640x448 -> 320x224, `{21,0,0,22,0,0,21}`); `hx_wiper` (screen wipes). The pollution maps (`graffito`, R8) use no filter.
- **Mirror** (Hotel Delfino `7,0`, Mario walked into the mirror cube at (-2350, 900, 1200) = the lobby pool with `mirror205.bmd`): Mario's legs and FLUDD reflect in the water, wavy: `port/build_aurora/efb_mirror_hotel_pool.png`. Mirror models exist in Bianco (lake), Gelato (beach and the leaning mirrors), Pinna, Noki, Pianta, Corona and the EX courses (`/scene/map/mirror/` in the scene archives).
- **Heat haze** (`TShimmer`, present in Bianco, Ricco, Gelato and Pinna Park; skipped while FLUDD sprays): the Gelato cliffs and shop shimmer in horizontal waves: `efb_heat_haze_gelato.png` (`4,1`).
- **Bath mist** (Bowser, `60,0`): the clouds and Bowser's spikes behind the steam are softly blurred by the half-size filtered copy: `efb_bath_mist_corona.png`. The bath ripple (water surface in the tub) was not on screen in the scripted run; its copies (84x84 Z24X8 height map, 640x448 0x27) run every frame.
- The Boos in the hotel (screen-texture refraction, `telesa.cpp`) render translucent.



## E16. Presentation at other window shapes; band kept out of the presented image
- What decides it: the present pass (Aurora `gpu.cpp`) draws the viewport `calculate_present_viewport(surface, content)` and samples `uv * content_scale` (content = the 640x448 part of the 640x528 EFB target). Windowed 16:9 / 21:9 / portrait, fullscreen and the three aspect modes (4:3, fit, stretch) all go through it; fullscreen only changes the surface size (same `resize_swapchain` path as E9).
- **Leak found and fixed (aurora_efb_size.patch, `gpu.cpp`):** with bilinear upscaling (internal scale below the window, e.g. `internal_scale = 1` in a 1600x900 window) the last presented row sampled half a texel past the content and blended in EFB row 448 (black or junk) as a dark line at the bottom; FXAA (w6's `aurora_visual_options.patch`) samples up to 8 texels around. The bilinear sample is now clamped half a texel inside the content; `fxaa_sample` clamps the same way (**w6:** that line is in your patch's code, keep it when you regenerate). The area sampler already stayed inside.
- Checked runs: 1600x900 stretch, 1890x810 fit, 700x1000 fit, 1600x900 4:3 and 1600x900 fit with internal scale 1 all start and render; the EFB screenshot at internal scale 1 is 640x448. A picture of the presented window could not be taken: with other copies running the screen copy shows the windows on top of ours (even when ours is raised), and PrintWindow is black for D3D12. Fullscreen was not started (it would cover the user's screen).



## E17. `--nullgfx` runs died at stage load (w13)
- Dawn's Null device cannot allocate the readback buffers (`port EFB compute output`: "Out of memory"). With `SMS_PORT_NULLGFX` (port_main.cpp guarantees it means the Null backend) `port_efb.cpp` takes no snapshots and queues no jobs, and `port_aurora_sdk.cpp` queues no peeks/screenshots: GPU copies stay as they are, game memory as the game wrote it, peeks return their defaults and pixel counts read "dirty". Bianco (pollution copies every frame) now runs 3000+ frames with `--nullgfx`.



## E18. Game-frame clock and interpolated frames (for w9's frame-rate interpolation)
- `port_efb.cpp` now ages things in game frames (`sGameFrame`, one per `port_efb_end_frame` of a frame the game drew), not Aurora frames: copy destinations "alive" within 2 game frames and forgotten after 8, counters restarting after 10 unread game frames, readback lateness. With several presented frames per game frame the old Aurora-frame ages would have dropped pollution readbacks and restarted counters.
- **Hook for w9:** `extern "C" void port_efb_set_presentation_only(int on)`, `int port_efb_presentation_only(void)`. Set 1 before recording an extra (interpolated) presented frame, 0 before the next game frame. While 1: `GXCopyTex` copies on the GPU only (no readback, no destination bookkeeping); `GXPeekZ`, `GXPeekARGB`, `GXReadPixMetric` return the latest game-frame values and queue nothing; `GXClearPixMetric` still clears (picture); `port_efb_end_frame` does nothing (finished readbacks are stored at the end of the next game frame, so game memory changes only between game frames). Screenshots still work.
- **Rule for interpolated frames (w9):** they must not re-run draw passes whose results the next frame reads back cumulatively: the pollution layer stamp/decay/copy group (`graffito`, `TPollutionLayer`: the decay TEV pass would run twice per game frame and goop would fade twice as fast), the bath water ripple/height-map copies, and `TModelWaterManager`'s puddle accumulation. Re-drawing from the game frame's recorded GX stream with replaced matrices, or skipping those groups, keeps them once per game frame. The screen texture and mirror copies are per-frame and fine to repeat.
- w9's record/replay change in `port_efb.cpp` ("results applied exactly 3 game frames after the request" while recording or replaying, their BOOTLOG W9-6) is not in main yet; when merged it should use `sGameFrame` too.



## E19. EFB contract for internal resolution, widescreen (Hor+) and MSAA (w6)
What `port_efb.cpp` / `port_aurora_sdk.cpp` assume, and what the new modes must keep:
- **Mapping:** a snapshot of the EFB target covers the logical EFB (640x528, `vi::efb_size`) linearly, no offset: logical (x, y) = target (x * W / 640, y * H / 528). Copies (`map_copy_rect`, `scale_copy_dst`), peeks and counters all rely on it.
  - Internal resolution (any scale, also non-integer like 4.286): fine as is. CPU-read copies are box-filtered on the GPU down to the GC destination size (`kDownsampleWGSL`, taps = scale) and pixel counts are scaled by logical/target area (`toLogical`), so the game sees GC-sized data. w6's `aurora_copy_scale.patch` (rounded copy rectangles, exact 1:1 copies) is right for this: a 1:1 copy must not resample, or the pollution maps drift (seen at 4.286).
  - **Widescreen Hor+:** the main scene may draw into a wider target, but everything the game copies or reads must still see the GC 640x528 EFB. Proposal: keep the EFB target with the GC mapping above for all copy/peek/counter work and give only the final 3D scene pass (and the display copy) the wider viewport, or, if the wide target is shared, export the logical->target transform (offset + scale per axis) so `map_copy_rect`, `port_efb.cpp`'s `logicalSize`/snapshot mapping and the GXPeekZ texel all apply the same offset. Pixel counters must count only inside the mapped 640x528 rectangle (today they count the whole snapshot).
  - Copies that take the screen (screen texture, mirror, mist) must copy the GC rectangle, not the wide one, or the refraction/mirror UVs (computed by the game for 4:3) slide.
- **MSAA:** tested here with Aurora's `msaa = 4` (temporary switch, removed): pollution readbacks, pixel metrics and screenshots keep working (the snapshots are resolved / depth via w6's `aurora_msaa_depth.patch`), no validation errors, edges anti-aliased. Remaining fidelity point: a pixel at a polygon edge resolves to a partial colour, so a counter can count a pixel the GameCube (one sample at the centre) does not; for the axis-aligned pollution quads this is a few pixels. If it matters, count from sample 0 of the unresolved target.
- **FXAA:** post-process only (present pass), never seen by copies or readbacks; see E16 for the edge clamp.
- I'll review w6's EFB-touching changes when they land in BOOTLOG (copy scaling, widescreen mapping, MSAA resolve before copies/peeks).







## R17. Internal resolution: the airstrip goop vanished at 4x and at 2.41x (copy resampling), and counts depended on the scale
- **How it was checked:** airstrip (`warp 0,0`), `SMS_PORT_COPY_LOG=1`, settings files with `internal_scale` 1, 2, 3, 4, 2.4107 (1080 rows) and 3.2143 (1440 rows); pixel metric of the layer counter (token 0x7e) and the CPU readback of the 512x512 `GX_CTF_R8` map.
- **Symptom:** at 4x and 2.41x the counter went 2518 -> 1213 -> 387 -> 7 -> 0 within seconds and then read 0 for good ("clean" after the 30-zero debounce); the CPU map emptied too. 1x/2x/3x/3.21x looked fine but disagreed: 4541 / 4555 / 4546 / 4556.
- **Cause 1 (Aurora):** the EFB target is not an integer multiple of the logical EFB vertically (528 logical rows are 566 target rows at 1x: the frame buffer is scaled for the 480-line display, E9), so 512 rows map to N + f target rows. `copy_tex` mapped the source with the scissor's outward rounding (floor/ceil) but sized the destination with `lround`, so for f < 0.5 the source was one row taller than the destination (2195 into 2194 at 4x) and every copy was resampled with a linear filter. The layer is drawn back with nearest sampling and copied again every frame, so it crept by a fraction of a row per frame and blurred, and the layer's decay TEV pass ate the blurred edge. EFB captures (`SMS_PORT_EFB_DEBUG`) showed texel rows covering 3-5 target rows in an irregular pattern.
- **Fix 1:** `aurora_copy_scale.patch` (AURORA.md): rounded edges for the copy rectangle and, for a 1:1 copy, a destination exactly the size of the source rectangle. A nearest redraw of such a copy is exact for any fraction.
- **Cause 2 (port_efb.cpp, w8's file):** the readback box-filtered each GameCube texel's block of target pixels (4 x 4.29 at 4x, so it mixed in part of the next row) and the counter counted every target pixel and scaled by the area ratio.
- **Fix 2 (marked `w6 (round 5)` in port_efb.cpp, for w8 to review):** a 1:1 CPU-read copy samples each GameCube pixel at its centre (`taps = 1`, as the copy unit does), and the pixel counter counts on the logical 640x528 grid (one sample per GameCube pixel centre, `kCountWGSL` takes logical and target size; `toLogical` = 1). Down-scaling copies (mip copies) keep the box filter.
- **Result:** counter 4548 and readback 4548 non-zero texels (the same texels: the counter counts the map) at 1x, 2x, 3x, 4x, 2.41x, with 4x MSAA at 1x and 2.41x, and at 16:9 (anamorphic target, R20). The object counter (token 0x92: 3D pollution objects drawn with a perspective camera) varies by +-0.3 % between scales (edge pixels: the target pixel holding a GameCube pixel centre is half a target pixel off) and reads about 2 % more with MSAA while partly dirty (an edge pixel with partial coverage resolves non-black; E19). Both read 0 when clean.


## R18. MSAA 4x
- `[graphics] msaa = 4` (F1: "Anti-aliasing (MSAA)", applies at the next start). WebGPU render targets take 1 or 4 samples only, so 2x and 8x are not offered (an ini value > 1 means 4).
- Depth copies from the multisampled EFB were an Aurora fatal: `aurora_msaa_depth.patch` adds multisampled Z8/Z16 copy pipelines (sample 0) and makes Aurora's depth peek follow the sample count. Colour copies, w8's snapshots/peeks, screenshots and the pollution readbacks already work on the resolved target.
- Checked: airstrip (counts R17), Bianco gameplay and pause menu (screen-texture background) `shots/bianco_msaa4_shot_*.png`, Corona boss (Z24X8 and 0x27 depth copies, no fatal or validation error).
- Corona with MSAA logged E_OUTOFMEMORY for two pipelines (5 retries, draws skipped) in 2 of 3 runs, the first runs after enabling MSAA with 8 other game instances on the GPU; a third run had none (pipelines then in Dawn's cache). Same transient class as R7; with fewer instances it did not occur.


## R19. Graphics options (all off = GameCube rendering): anisotropic filtering, trilinear, FXAA, brightness/gamma, copy filter, vsync
- `[graphics]` in port_settings.ini and the F1 Display tab (w6 additions in w9's files, marked `// w6`; **w9:** new fields `msaa, anisotropy, trilinear, fxaa, brightness, gamma, copyFilter, vsync` in `PortSettings`, `port_settings_active_msaa()`, `port_get_aspect_scale()`, `port_get_display_aspect()`, a `[graphics]` ini section, and `applyDisplay()` calls `aurora_set_texture_filtering`, `aurora_set_post_process`, `aurora_set_copy_filter_enabled`, `aurora_enable_vsync`; the aspect enum grew, R20).
  - Anisotropic filtering: "As the game asks" (default: the texture's GX request, Aurora maps GX_ANISO_2/4 to 8x/16x as before) or forced 1/2/4/8/16x for linear-filtered mipmapped textures (nearest-filtered ones, e.g. the pollution maps, are left alone).
  - Trilinear mipmaps: linear mip blending for every mipmapped texture.
  - FXAA: present pass only (copies and readbacks never see it). Brightness 50-150 %, gamma 50-200 %.
  - "Sharper screen effects": skips `GXSetCopyFilter`'s vertical filter on texture copies (screen texture, mirrors, bath ripple), not original.
  - VSync: `aurora_enable_vsync` (default off as before; frames stay paced by `VIWaitForRetrace`). **w9:** coordinate with the frame-rate work.
  - "Reset graphics to GameCube defaults" button.
- Defaults are byte-identical: EFB screenshot of the airstrip at frame 2000 before and after (`shots/ref_s1_shot_2000.png` vs `s1_shot_2000.png`, same hash). FXAA/gamma verified on a window capture (`shots/crop_def.png` vs `crop_post.png`: dress edge smoothed, brighter).
- **Performance** (RX 9070 XT, D3D12, airstrip warp 0,0; GPU 3D-engine busy time of the process from the Windows "GPU Engine\Running Time" counter over 20 s, per game frame at 30 fps; 8 other game instances shared the GPU, so +-5 %):

  | internal rows | base | MSAA 4x | AF 16x | trilinear | FXAA |
  |---|---|---|---|---|---|
  | 448 (1x) | 0.46 ms | 0.58 | | | |
  | 896 (2x) | 0.68 ms | | | | |
  | 1080 (2.41x) | 0.84 ms | 0.94 | 0.88 | 0.83 | 0.84 |
  | 1344 (3x) | 1.01 ms | | | | |
  | 1440 (3.21x) | 1.13 ms | 1.73 | 1.18 | 1.17 | 1.13 |
  | 1792 (4x) | 1.5-1.7 ms | | | | |

  VRAM 383 MB at 1x, 480 MB at 1080, 574 MB at 1440, 640-660 MB at 4x; MSAA adds 60-100 MB. FXAA runs at window size, under the noise here.


## R20. Aspect: 4:3 (default), fit, stretch, and true widescreen 16:9 / 16:10 / 21:9 (64:27) / 32:9 / match window - render side
- Design (agrees with E19): the logical EFB stays 640x528 GameCube pixels and every copy, peek, counter and readback keeps the linear 640x528 -> target mapping. Widescreen gives the render target and the display the wide shape (`VILockAspectRatio(16, 9)` etc. + FIT, or STRETCH for "match window"), so a logical pixel is wider in the target (anamorphic, per-axis scale in `map_logical_viewport`); internal resolution sets the rows. The 3D view gets wider through the game's projection, not through more logical columns, so the screen texture/mirror copies (which copy the GC rectangle) contain the same wide view and their 4:3-computed UVs stay right.
- **Interface for w11 (game side):** `extern "C" float port_get_aspect_scale(void)` (port_settings.h, port_window.cpp): display aspect / (4/3), 1.0 in 4:3, fit and stretch; updated when the setting or (match window) the client area changes; any thread. Multiply the camera's projection aspect (`TLookAtCamera::mAspect`, set in `TMarDirector` setup to 640*0.913/448) by it for Hor+ (the culling calls `SetViewFrustumClipCheckPerspective(gpCamera->getAspect())` follow), and squeeze 4:3 HUD/menus/movies by 1/scale around x = 320 (full-screen fades/wipes unsqueezed). `port_get_display_aspect()` gives the display aspect (0 = stretch).
- Until w11's hook lands, the widescreen modes show the 4:3 view stretched (`shots/wide169_shot_2000.png`, 1280x720 target); the pollution counts stay 4548 (R17).
- Window: the client area is aspect-locked for every fixed aspect (was 4:3 only); "match window" is unlocked.
- Ini: `aspect = 4:3 | fit | stretch | 16:9 | 16:10 | 21:9 | 32:9 | window`.







## W7-H. Blocking sync DVD reads by default; w5's flag-order patches reverted; newgame smoke scene
- **`sdk_dvd.cpp`:** blocking sync reads (hardware behaviour, BOOTLOG 13) are now the default. `SMS_PORT_DVD_SYNC_BLOCK=0` restores the old inline reads.
  - The file-select crash they once exposed no longer reproduces: w10 ran it 48 times, including ASan and UBSan, after the JAudio table/bitfield fixes; here `smoke_all.py --only fileselect_dvdsync --repeat 6` passed 6 of 6.
  - `smoke_all.py --quick`: 19 of 20 pass. The 20th, `newgame`, ran with the old script; it passes with the new one, see below.
- **Reverted to the original source**, since the root fix makes these unnecessary:
  - `JAIGFrameSequence.cpp`: `mLoadingFlag = true` is back after `loadFileAsync`. Blocking sync reads let the caller run before JASDvdThread's read completes, as on hardware.
  - `JAIGFrameStream.cpp`: `DvdLoadFlag = 1` is back after `DVDReadAsyncPrio`. Async callbacks are deferred now.
  - Plaza (warp 1,0) stage BGM averages 19 voices after `startStageBGM`, against 12 when it is refused. GameCube `mario.dol: OK`.
- **`newgame` smoke scene stuck at "Select data.":** not a card bug.
  - After the card file is created, the file select hands Mario control (`PAD_FLAG_MENU_INPUT` off, the blocks get collision). A file is chosen by jumping into its block.
  - Mario starts beside block A, so A alone jumps past it, and START only reopens the create prompt.
  - A step right and a jump hits block B and the new game starts (opening movie, then the airstrip).
  - `NEWGAME_SCRIPT` in `smoke_all.py` now does START through the title and prompt, then steps right and left with jumps. `--only newgame` passes.


## W7-I. State-gated input: airstrip Piranha 遶翫・shine 遶翫・arrest movie 遶翫・plaza (full chain on real input)
- **PADSCRIPT conditions `?<bit>` / `!<bit>`:** an entry applies only while bit `<bit>` of `gPortCond` (`port_crash.cpp`) is set or clear.
  - The bits come from a read-only SMS_PORT hook in `TBiancoGateKeeper::perform` (`gatekeeper.cpp`): bit 0 = head vulnerable (`mHead->mVulnerable`), bit 1 = asleep (Sleep/SleepDamage), bit 2 = defeated, bits 8-15 = hit points.
  - They are cleared on every app state change, and `SMS_PORT_TRACE` logs each change ("cond ... boss hp N").
  - Main's own `RA<n>` token (analog-only R) covers light presses.
- **FLUDD aim sign, measured:** in the spray stance (`MARIO_STATUS_SQUAT`), stick down (`S180`) raises the stream and stick up lowers it. Releasing R for more than a few frames lets the pitch chase back to its default.
- **Airstrip fight (`--warp 0,1`, real input, about 40 s):**
  - `+0@100-300:G-5000;1250,+0@300-900:G-2750;1480` walks to the west ledge.
  - Asleep: R 50 on / 12 off, gated `?1`. The Piranha wakes after about 1100 frames.
  - Head vulnerable: R 120 on / 2 off, gated `?0`, plus a 3-frame `S180` pulse every 50 frames, gated `?0`.
  - Hits: HP 3遶翫・遶翫・遶翫・ within about 1000 frames of the first vulnerable window, then the defeat demo.
  - Then `+d200-1500:G-1500;1500+?2` and `+d200-2500/45:A+?2` walk to and jump for the shine. Shine get: shines 1, director flags 0x8242.
  - Next: the save prompt (director state 11), answered with A. It wrote `build_aurora/user/USA/Card A/01-GMSE-super_mario_sunshine.gci`; the player's card was not touched.
  - Then the arrest movies (openingBA, BB, the save prompt, BC) and Delfino Plaza (area 1/0) loads with its own gatekeeper asleep.
  - No crash anywhere in that chain with `--nodraw`.
- **Plaza (in progress):**
  - Route from the start along the east street: `G6000;-2000 遶翫・G6000;3000 遶翫・G5300;4500`, A through Peach's forced talk and demo, then west along z 4630.
  - The goop field around the plaza Piranha (12, 300, 4631) begins at about x 3300.
  - Mario sinks and loses a life at x 1100-1800 unless the ground ahead is sprayed clean first. Two clean-and-step stops reach about x 1900 safely.
  - Waking it from there, the fight, the Bianco portal (stage-change object 3 at (-6500, 850, -2000)) and the Bianco episode 1 shine are not done yet.
- **For w6:** with real D3D12, an access violation inside the AMD driver during pipeline creation (no game frames) happened twice. Both times it was at the airstrip Piranha's appear or defeat demo (`--warp 0,1`, about f1340 and f2820 with the script above). `--nodraw` avoids it.
  The Null backend (`--nullgfx`) now dies with "WebGPU error 3: Out of memory" early in that stage.

## W7-J. Plaza Piranha and the Shadow Mario chase on real input; async DVD transfers off the interrupt lock
No port bug turned up on this stretch of the story: the plaza fight, the kidnapping event and Shadow Mario's replays all behaved as on GameCube. The work was input tooling plus one DVD change.
- **Rescue:** the `gatekeeper.cpp` gPortCond hook from W7-I had not reached main (the merge skipped that file). It is re-applied here.
- **Plaza Piranha (area 1/0), real input, warp `1,0`, rendered:** defeated in 3 of 4 runs. The last runs killed it within about 1000 frames of waking it. Then the defeat demo, director state 9, and plaza 1/1 loads.
  - Correction to W7-I: Mario does not sink in the goop. Standing in it puts him in `OIL_RUN`, and `checkGraffitoSlip` hurts him every few seconds (`floorDamageExec`); goop blobs knock him back into it.
  - The approach that works: a slow walk (`G<x>;<z>;60`) with a light pumped spray (`RA200`, 120 on / 4 off), so he cleans the ground ahead as he goes. Clean-and-step stops failed because the `G` autopilot overshoots, turns back, and then sprays behind him.
  - Waking it: a planted spray held on its body (`R` + `F0;4631`, aim raised for 20 frames).
  - Damage: in the plaza's `TNerveBGKWait`, only head hits in animation 0x12, frames 50-160 (`isHeadHitActive`), do damage. A hit outside that window sends it to animation 7, and it stays there while it is sprayed. So spray only while new bit 3 is set: `R+F0;4631+S180;45+?3`. That sweep raises the aim from level and passes the head (about gun -7000) within the window.
  - Measured in the spray stance: stick Y changes the aim at a rate (`unk36E` integrates `stickY * mRButtonMult`). `S180;45` moves it about -126 per frame up to the clamp at -12945; level is 0. Stick X turns Mario (`squating`).
- **Plaza 1/1, the kidnapping (warp `1,1` with `SMS_PORT_WARP_FLAGS=story1`, which starts the same event):** chased on real input until Shadow Mario's 64 water hits ran out. Then: he goes down (doing 14/15) 竊・Peach's talk 竊・he runs off (16/17) 竊・waits to invite Mario at graph node 7 (20) 竊・runs to the gate (21) 竊・draws the rainbow M on the statue base at (0, 300, 5010) during a demo (23) 竊・disappears into it (10). Screenshot: w7 scratch `s18/shot_8400.png`.
  - The chase takes about 6000 frames: a refill trip to the sea, a ramp detour, drops off the terrace.
  - His graph has 9 nodes: (138,1777,4447) (5250,300,4050) (6000,300,-250) (5650,300,-4750) (3360,700,-4630) (3460,626,-520) (3400,300,3420) (2320,300,3460) (0,300,5010).
  - **Open:** entering the M. Mario stood against it (`G*` to Shadow Mario at the gate) and no stage change came. The trace lists stage exits 0-6 on 1/1, including (-6500, 850, -2000) 竊・22, which hangs over the canal under the west arch, and (0, 2400, 1100) 竊・6. Next: find which object the M is (a TMapObjChangeStage the gate activates, or the gate actor) and how Mario enters it (walk, jump or hip drop). Then the Bianco Hills episode 1 shine, and the plaza save prompt on the test card.
- **New PADSCRIPT tokens** (`port_main.cpp`; the logic is in `Application.cpp`, all debug, read-only toward game state):
  - `S<deg>;<pct>`: a partial stick tilt.
  - `G<x>;<z>;<pct>`: a slow walk.
  - `F<x>;<z>`: turn toward a point in the spray stance (stick X only, so an `S` token in the same entry still aims).
  - `G*` / `F*`: the same toward the stage's Shadow Mario.
  - `GN<x>;<z>`: walk along `SMS_PORT_NAV`.
  - `W<x>;<z>|...`: a route.
  - `<a>-<b>/<p>;<on>:`: hold for `<on>` frames of every `<p>` (keeps long scripts under the 32 KB environment limit).
- **`SMS_PORT_NAV="x,y,z x,y,z ...;a-b a>b a^b"`:** a small walk graph: `-` links both ways, `>` one way (a drop), `^` both ways with a jump. `G*` and `GN` walk it breadth-first. Without it, `G*` uses Shadow Mario's own graph (links climbing more than 350 are left out). The plaza graph used here:
  `1500,300,5600 2320,300,3460 3400,300,3420 3500,300,1800 3460,640,-520 3440,700,-3000 3360,700,-4630 5300,300,4400 6000,300,3000 6000,300,-250 5900,300,-2500 5650,300,-4750 7300,100,-250 10600,0,-250 3750,700,-4680 4500,300,-4700;0-1 1-2 2-3 3-4 4-5 5-6 2-7 7-8 8-9 9-10 10-11 9^12 12-13 6-14 14>15 15-11`
- **New condition bits (`?n` / `!n`):**
  - 3: the gatekeeper head hit does damage now.
  - 16: a talk or demo is running.
  - 17: Mario is wading in goop.
  - 18: Shadow Mario is waiting in his replay.
  - 19: he is within 1000 (x/z).
  - 20: he is down.
  - 21: his water hits count now.
  - 22 / 24: he is above / below Mario by more than 250.
  - 23: FLUDD needs a refill (set below 15% water, cleared at 95%).
  - 25: the walk graph says jump now.
  - Refill: in water, hold analog R (`TWaterGun::suck` needs pressure). Swimming alone does not refill.
- **Trace (`SMS_PORT_TRACE`):** stage exits as each stage loads (`MapObjTown.cpp`: position and target), Shadow Mario's state, water hits and position, his graph once per stage, FLUDD pitch and face angle in the position line, and `SMS_PORT_TRACE=aim` for the pitch every 10 frames in the spray stance.
- **The scripts used** (stage clock `+0@`; plaza 1/0 after `1000-1400/40:A` for the judge's prompt):
  - Plaza fight: `+0@0-400:G6000;-2000,+0@400-900:G6000;3000,+0@900-1300:G5300;4500,+0@1300-4000/30:A+?16,+0@2100-2250:G3600;4630,+0@2250-3700:G900;4630;60`.
    Then `RA200` pumped to 3700, then from 3700: `R+F0;4631+?1+!17` (150 / 4, with `S180;45` for the first 20 frames), `R+F0;4631+S180;45+?3+!17`, and `G1150;4630+?17`.
  - Chase on 1/1: `+0@0-150:G1500;6000,+0@150-300:G5300;4500,+0@300-60000:G*+!19+?21+!23,+0@300-60000/404;400:RA255+!19+?21+!23,+0@300-60000/76;70:R+F*+S180;45+?19+?21+!23,+0@300-60000:GN10600;-250+RA255+?23,+0@300-60000/20:A+?25,+0@300-60000:G*+!21+!23,+0@300-60000/30:A+?16` with the graph above.
- **`sdk_dvd.cpp`, async transfers off the interrupt lock (w14's report):**
  - Before: the host file read ran inside the deferred completion, in interrupt context with the interrupt lock held. A big async read stalled every game thread and the VI retrace that drained it.
  - Now a drive thread (a host thread) does the transfers in issue order, starting when the request is issued, without the interrupt lock.
  - The completion only picks up the result. If its transfer has not ended yet, it goes back on the deferred queue (behind the later ones, which do the same, so the order holds), and the drive thread drains again when the transfer ends. With a warm file cache the transfer is done long before the completion runs, so timing in game terms is unchanged.
  - `DVDCancel*` waits out a transfer in progress, and a queued one never starts, so a canceled buffer is never written afterwards.
  - Host reads are serialized (`gReadLock`), so DVDReadPrio's inline path (interrupt context, host threads) never runs beside the drive thread.
  - Every run since uses it: airstrip, plaza and the Shadow Mario chase; the `fileselect_dvdsync`/`newgame` smoke scenes are in the smoke run below.
  - **For w14:** this touches `completeReq`, `queueReq`, `cancelReq` and the inline read in `DVDReadPrio`. That inline line is the one your read-ahead cache patch changes as well.
- GameCube `mario.dol: OK`.


# Mechanics checklist (w12)
Main as of 19:00 plus M1-M4, real D3D12 (Null backend for a few scans), `SMS_PORT_NO_PADS=1`, `SMS_PORT_USER_DIR` in the build folder.
Each mechanic was driven with `SMS_PORT_PADSCRIPT` (stick angles, holds, `+n` stage-relative frames, `G<x>;<z>` autopilot) and read from the new
`SMS_PORT_TRACE=act` lines (every change of Mario's action/nozzle/Yoshi state with position, speeds, HP and water, plus coin/red/blue counters),
`SMS_PORT_AUDIO_LOG` (each distinct sound id, mapped to `MSD_SE_*`) and screenshots.

## New debug knobs
- `SMS_PORT_TRACE=act` (`Application.cpp`, read-only): `[act] f<frame> <old> -> <new status> pos ... fwd vy hp water noz yoshi` and `[act] f<frame> coins/red/blue`.
- `SMS_PORT_DEBUG_CALL=tp<x>;<y>;<z>@<frame>`: puts Mario at a position (to reach an object a script cannot walk to).
- `SMS_PORT_DEBUG_CALL=hatch@<frame>`: hatches the stage's Yoshi egg the way a fed egg does (`appearFromEgg` with a fruit picked from the stage) and puts Mario next to it.

## M1. Every jump made while the stick was held became a spin jump (`TMario::checkStickRotate`)
- **Symptom:** running + A always gave `LEFT_ROTATE_JUMP`, so no normal/second/triple jump from a run.
- **Cause:** the GameCube code really reads `BOOL q[4]` uninitialised (asm: only `li r0,1; stw r0,0x10..0x1C(r1)`, then `cmpwi` each slot with 1). clang treats the unset slots as undefined and folded every `q[i] == 1` to true, so "all four stick quadrants seen" was always true.
- **Fix:** `MarioMove.cpp`, under `SMS_PORT` the slots start at 0, so the result depends only on the stick history (on hardware a slot reads 1 only if an earlier call left exactly that at the same stack address). Now JUMP -> SECOND_JUMP -> ULTRA_JUMP, and the spin jump needs a stick rotation (`RIGHT_ROTATE_JUMP` with a quarter-turn per 2 frames).

## M2. Turbo nozzle never fired (`TMario::checkController`)
- **Symptom:** with the turbo nozzle, holding R made Mario run at normal speed (30) with no water used; `MSD_SE_SY_NEWP_AIR_TAME` (charging) played, the dash never started.
- **Cause:** decomp transcription. The GameCube stores the propeller value at `getCurrentNozzle() + 0x714` (asm `stfs f31, 0x714(r3)`): the turbo nozzle is at TWaterGun+0x18D8, so that is TWaterGun+0x1FEC, 0x21C bytes past the end of the 0x1DD0-byte object (`li r3, 0x1dd0; bl __nw` in `MarioInit`), outside the water gun. The decomp wrote it to `TNozzleTrigger::unk388`, the trigger's pressure fill, resetting it every frame Mario stood or ran.
- **Fix:** `MarioMove.cpp`, under `SMS_PORT` the out-of-object store is not reproduced. Now: turbo dash at speed 80 draining water, across water too, and the turbo/surf jump (`BROAD_JUMP`) out of the water.

## M3. Mario fell off a pole as soon as he grabbed it (`TMario::barWait`)
- **Symptom:** jumping onto a pole (Bianco, (-4050, 9900)) gave BAR_HANG -> BAR_WAIT -> LAND_SAFE_DOWN on the next frame, thrown back 200 units, every time.
- **Cause:** decomp transcription: the asm is `fcmpo f1, f0` against 100.0f, `cror eq, lt, eq`, `bne` past the drop, i.e. drop when `mHolderHeightDiff <= 100.0f` (slid to the bottom). The source had `> 100.0f`. (Main had already corrected the button bit to 0x8000.)
- **Fix:** `MarioSpecial.cpp` (NonMatching unit, no `#ifdef`): `<= 100.0f`. barWait 99.5% -> 99.9%; DOL unchanged. Now grab, climb (stick up), slide down (stick down) and let go at the bottom, and jump off (A, as `WALL_JUMP`).

## M4. Timed missions failed the moment they started (`TGCConsole2::setTimer`)
- **Symptom:** Ricco episode 6, after the red coin switch's message: Mario dies at once (`LOSER_DOWN`, life lost, back to the plaza).
- **Cause:** decomp transcription: `setTimer` ends with `stw r30, 0x500(r31)` (r30 = the time shown, after the 599999 cap); the source stored the argument (`-1` for a running timer). `getFinishedTime()` hands that to the event scripts (`getTime`), which then saw the time as up.
- **Fix:** `GCConsole2.cpp` (NonMatching unit): store `timerValue`. setTimer 99.8%; DOL unchanged. Now the 2:00 red coin timer counts down with 0/8 shown.

## Pass/fail
| Mechanic | Result | Where / how | Notes |
|---|---|---|---|
| Triple jump | pass (after M1) | Bianco 2,0, run + A every 8 frames | JUMP -> SECOND_JUMP -> ULTRA_JUMP, jump voices |
| Side flip | pass | run, stick reversed + A | U_TURN_JUMP |
| Backflip | pass | R + A standing | SQUAT -> BACK_JUMP |
| Long jump | pass (as turbo jump, after M2) | turbo + A / from water | `BROAD_JUMP` needs FLUDD emitting (turbo); R + A with the spray nozzle crouches, as the code says |
| Wall kick | pass | Sirena 6,1 run + A into a wall | WALL_SLIDE -> WALL_JUMP |
| Ground pound | pass | A, then L in the air | HIP_DROP -> HIP_ATTACK_END, `MSD_SE_MA_HIP_ATTACK`; breaks wood boxes |
| Belly slide / dive | pass | run + B | JUMP_CATCH -> slide -> CATCH_LOST, `MSD_SE_MA_SLIP` |
| Spin jump | pass (after M1) | stick circle + A | RIGHT/LEFT_ROTATE_JUMP, `MSD_SE_MA_ROLL_JUMP` |
| Crouch | pass | hold L | SQUAT, SQUAT_STANDUP |
| Swimming, diving, surfacing | pass | Sirena 6,1, Ricco | SWIM_START/PADDLE, B dive (SWIM_DIVE), A up (SWIM_UP), swim sounds |
| Refill FLUDD | pass | Sirena, R in water | 9688 -> 10000 |
| Spray | pass | R | water drops, `MSD_SE_PO_WATER_HI` |
| Hover | pass | X then jump + R | ROCKET status with vy ~0, `MSD_SE_PO_HOVER` |
| Rocket nozzle | pass | plaza 1,8 (story8r + flag 10366), box at (800, 2300) | box stomped (TRAMPLE), item taken, R: vy 140 launch |
| Turbo nozzle | pass (after M2) | plaza box at (-800, 2300) | dash 80, water drains, runs on water |
| Nozzle switching (X) | pass | | 0 <-> 4 / 1 / 5, `MSD_SE_SY_SELECT_POMP(_BACK)`; boxes refill after a nozzle is taken (`resetNozzleBoxesModel`) |
| Yoshi mount | pass | Ricco 3,7 egg (11000, 1500, 1100), `DEBUG_CALL=hatch` | YO_BORN, jump on: Yoshi 8, nozzle 3 |
| Yoshi juice / tongue | pass | R / B | spit status, `YO_TONGUE_RELEASE/BACK` |
| Yoshi eats fruit | pass | hatch eats the fed fruit | `YO_TONGUE_GOKKUN`, `YV_DELICIOUS` |
| Yoshi dismount / water | pass | X; ride into Ricco's harbour | BACK_JUMP off; deep water: Yoshi 3 (drowning) -> egg, Mario swims |
| Feeding the egg by carrying fruit | not tested | | the egg's fruit is random; plaza `eggYoshiEvent` ignores fruit |
| Tightrope / wire | pass | Bianco wire (-3625..-2375, 3300, 5450), tp onto it | WIRE_WAIT, bounce (0x892), hang (0x352/0x35B), moves along |
| Fence climbing | pass | Ricco | FENCE_CATCH, climb, FENCE_MOVE |
| Revolving fence | not reached | Ricco `fence_revolve` | |
| Lily pad | pass | Bianco LeafBoat (9000, 2300, 600) | stands on it, spraying moves it |
| Cannon | not reached | Pinna `demoCannon` is blocked/scripted | |
| Sand birds, slides | not tested | | EX/Gelato/Pinna, see w13's EX runs |
| Spinning platform | not confirmed | Bianco `BiaTurnBridge` stood on, did not turn | it may need its trigger |
| Poles | pass (after M3) | Bianco | |
| Wood boxes | pass | Ricco box (-13400, 300, 1700) | ground pound: `IT_BARREL_CRASH`, coin pops out and is collected |
| Coins | pass | Bianco coin trail | 0 -> 5, `SY_COIN` |
| Blue coin | pass | Bianco (-13701, 1600, 5709) | blue 1, `SY_BLUE_COIN_GET` |
| Red coins | partial (after M4) | Ricco 6 switch at (-15700, 100, 8700) | switch, message, 2:00 timer and 0/8 counter work; no coin reached by script |
| 1-up | pass | plaza (8400, -1300, -2900) | lives 3 -> 4, `SY_1UP` |
| Health / damage | pass | Ricco fence enemy | hp 8 -> 7 |
| Death | pass | sirena_ex0 void (`tp`) | FALL_DEAD, lives 3 -> 2, course restarts |
| Shine get + save prompt | pass | sirena_ex0 `tpshine` | SHINE GET demo, state 11 save prompt (Save and Continue / Save and Quit / Continue), card written, back to the plaza 1/9 |

Open items:
- **w6/w8:** the save prompt's background is flat grey (it should be the frozen stage). Nozzle-box/turbo screens fine.
- **w8/w9:** with 8-10 game instances from other workers running at once, some of my runs nearly stopped presenting frames (main thread in `VIWaitForRetrace`, render worker in `port_efb.cpp` `jobEncode`); with less load the same scripts ran normally. Each EFB readback creates a new buffer (`jobEncode`, about 7 per frame in Bianco); worth checking they are released, since Null-backend runs in Bianco/Ricco die with "WebGPU error 3: Out of memory".
- My level-sweep runs earlier today did not set `SMS_PORT_USER_DIR`, so they used the player's card folder; the card file there was modified at 15:27, during that sweep. A scripted run may have saved over the player's slot.



## R21. Dirty Mario captured (airstrip goop)
- `warp 0,0`, `SMS_PORT_PADSCRIPT=100-1600/30:A,+30-900:G-1882;1136,+900-1400:G-1700;1100`, `SMS_PORT_TRACE=1`: A through Peach's talk, then the autopilot walks Mario into the goop field west of the start. He sinks to y 300 (action 0x0004045C) and loses one HP every 300 frames until a life is lost at f3614.
- `shots/dirty_shot_2400.png`, zoom `shots/dirty_mario_crop.png`: Mario wears the airport dirt layer (R9: `h_ma_rak.bti` pink/orange swirl swapped in for `H_ma_rak_dummy`) over his skin, cap and overalls. The goop's pink/orange/yellow surface and the dark sinking tint render as well.







# Widescreen (game side, w11)

True widescreen (Hor+) on the game side, driven by w6's `port_get_aspect_scale()` (display aspect / (4/3), 1.0 in the 4:3, fit and stretch modes).
Hooks: `port/include/port_widescreen.h` + `port/src/port_widescreen.cpp` (`PORTWideScale`, `PORTWideCameraAspect`, `PORTWideSqueezeRectX[i]`, `PORTWideOrthoX`, `PORTWideSightExtraDeg`, `PORTWidePaneScope<>`).
`SMS_PORT_WIDESCREEN=16:9|21:9|<aspect>` overrides the setting for tests; `SMS_PORT_WIDESCREEN_LOG=1` counts the hook calls and how many changed a value.
All shared edits are `#ifdef SMS_PORT`; `dtk shasum` OK.

Model: the game keeps its 640-wide logical frame; the render side shows it anamorphically (every viewport/scissor x stretched by scale). So the game widens its 3D by scale and squeezes its 4:3 2D by 1/scale, and full-screen 2D drawn with a plain 0..640 projection needs nothing.
- 3D: `PORTWideCameraAspect(&mAspect)` before the projection in `CPolarSubCamera::perform`/`loadAfter` (game, demo, gate and jet-coaster cameras) and `JDrama::TLookAtCamera`/`TPolarCamera::perform` (title/file-select/shine-select cameras). Vertical FOV is the GameCube's. It writes the widened aspect into the camera itself, remembering the game's value per field, so every reader of `gpCamera->getAspect()` follows: the frustum culls (`SetViewFrustumClipCheckPerspective` in TLiveManager::clipActorsAux = all enemy/map-object/NPC managers, conductor, animals, fish, pakkun, gesso, elecNokonoko, enemy attachments), the mirror camera and its projected texture, the water/screen-texture light projections, and JPA particles (`drawInfo.setAspect`). Nothing pops in or freezes at the new edges.
- Gameplay view test with a fixed angle: `TBaseNPC::perform`'s 120-degree "camera sees it" cone (NPCs out of it stop updating) grows by the extra horizontal FOV (`PORTWideSightExtraDeg`). As asked, the wider view slightly changes which actors run versus the GameCube, but only when widescreen is on.
- 2D: `J2DGrafContext::setPort` (viewport) and `setScissor` squeeze x by 1/scale around 320, so every J2D screen (HUD, pause, talk boxes, file select, card screens, shine select, subtitles) keeps its 4:3 shape, centred, and pane clipping stays consistent. `JDrama::TOrthoProj` widens only the 600-wide HUD frames (x 0..600: "Group 2D", menu, shine-select cameras); full-frame orthos (0..render width), the lens-flare ortho and the offscreen ones (graffito, pollution layers) are untouched. The GC/Dolby logo frame is widened where it is created.
- Full-screen 2D inside squeezed screens is widened back: the pause 'mask', the card-save 'mask', the stage-intro letterbox bars (ConsoleStr 'msk1/2'), the shine-select letterbox bars ('msk1/2') and its background gradient quad.
- The fader (TSMSFader rects), hx_wiper wipes and screen-texture quads use 0..640 projections, so they cover the full width already (the iris wipe's circle is stretched by scale, which is cosmetic).
- 4:3: every hook returns at once when scale == 1 (the camera aspect is never touched, the slot table stays empty). `SMS_PORT_WIDESCREEN_LOG=1` in a 4:3 Bianco run: 22200 hook calls, 0 changed a value, also with w6's real setting (`aspect = 4:3`). SMS_PORT_TRACE runs are not frame-deterministic between runs (load timing), so the counter is the equality check.

Verified (screenshots stretched to the display shape; with w6's setting `aspect = 16:9` the hooks report scale 1.3333): Bianco 2,0 at 16:9 and 21:9 (same vertical framing, more on both sides, HUD centred); pause menu (mask full width); plaza 1,1 at 16:9 and 21:9 (talk box and HUD intact, edge NPCs drawn); Petey Piranha (biancoBoss 55,0) and the Mecha-Bowser coaster (58,0) at 16:9; the Dolby logo; the file select (3D wider, J2D boxes stay aligned over the 3D blocks: a Hor+ projection compresses x by exactly 1/scale like the 2D squeeze).

Notes for w6 (render):
- Contract used: every GX viewport and scissor x (and width) stretched by `port_get_aspect_scale()`, the same for 2D and 3D. No separate 2D viewport is needed; the game squeezes its own 2D.
- Movies (title, intro THPs) are still stretched: they are drawn by `sdk_thp.cpp`/`MovieDirector` with a 0..W ortho. Pillarboxing belongs there (the movie subtitles are J2D and are already squeezed to match).
- HUD edge anchoring, lens glow and iris shape: done in "Widescreen round 2 (w11)" below.

Notes for w8 (EFB copies):
- Offscreen passes keep their GameCube orthos and viewports (graffito 512x512, pollution layers, BathWater, bossManta screen pass). If their viewport x is stretched like the screen, the copy source rect must be stretched the same way and the destination keep its GameCube size, so the textures stay identical.
- Screen-space lookups (ModelWaterManager refraction, mirror projection via `C_MTXLightPerspective(gpCamera aspect)`) already use the widened aspect, so a full-frame copy of the wide picture lines up.

## Widescreen round 2 (w11): HUD position option, news ticker, lens glow, iris, movie subtitles
- **HUD option (user request).** `[window] hud_position = centered | 16:9 | match` (w9's `port_settings.*`, marked w11) and F1 > Display > "HUD": 4:3 centered (original, default) / 16:9 edges / Match aspect (screen edges, e.g. 21:9). Applies at once. `SMS_PORT_HUD_EDGES=0/1/2` overrides it for tests.
  `TGCConsole2::perform` puts the HUD layout's top-level groups in a `PORTWideHudEdgesScope` while drawing: each group is classified by the centre of its parts in the 600-unit HUD frame (parts wider than the frame, the ticker's scrolling text boxes, are ignored) and moved by `PORTWideHudShift` = +-300 * (scale - 1) units (16:9 mode: the scale is capped at 16:9's), then put back.
  Left: shine, blue-coin and coin counters, timer, red-coin and goal counters. Right: water gauge (+ FLUDD balloon, juice text), life meter. Centre: Mario/lives, news ticker. Talk boxes, pause, menus and the shine select are other screens and stay centred.
  The water gauge parts GCConsole2 draws itself from pane bounds (`drawWaterBack`, `drawWater`, `drawJuice`: position matrix and the J2DPicture draws) follow their group's shift (`shiftOf`). Fixed on the way: the shift must not go into the texture matrices (it broke the pressure circle, `drawWaterBack` reuses `mtx` for TEXMTX0).
- **News ticker.** Its scissor is a raw `TGraphics::setScissor(unk544)` that J2D's squeeze never sees; it is now squeezed with `PORTWideSqueezeRectXi` (plus the ticker's HUD shift, 0 since it stays centred). The other raw scissor calls (ConsoleStr.cpp 286, CardLoad.cpp 844, Menu.cpp 15, CardSave, PauseMenu2) restore the full-frame scissor from `graphics->getScissor()`, which already spans the wide picture: unchanged.
- **Lens glow.** `TLensGlow` (the sun glow sprite, drawn in the full-frame +-w/2 ortho at the right screen position) gets x scale / `PORTWideScale()`, so it stays round. `TLensFlare` is a 3D model drawn by the widened camera: nothing to do.
- **Iris wipe (hx_wiper).** Horizontal offsets / scale (round), radii * (wide corner distance / 4:3 corner distance), so the fully open iris still reaches the wide picture's corners with the same timing. Game-over and logo wipes keep their shape the same way.
- **Movie subtitles were stretched** (they are not drawn through J2D's viewport: `TMovieSubTitle::draw` only calls `setup2D`, so they use the movie director's 0..640 camera, which is not widened). New `PORTWideOrthoProjBegin/End` / `PORTWideOrthoProjScope` squeezes the current orthographic projection around its centre for that draw; also used for the ending's card-save prompt in the movie director (`TCardSave` with `unk18`; not captured, needs the ending). Now the subtitles sit over w6's pillarboxed movie exactly as at 4:3 (text spans 20%..81% of the movie width at 4:3, 16:9 and 21:9).
  Note: US subtitles are off on a fresh save (the US ROM checks flag 0x90001, the Options "Subtitles" setting, and new files leave it 0). For tests: `SMS_PORT_WARP_FLAGS=90001=1`.
- **4:3 identical:** `SMS_PORT_WIDESCREEN_LOG=1` at 4:3 with `hud_position = match` (plaza 1,8 + ticker, Bianco + pause) and with `16:9`, and the movie with subtitles: 0 changed values (19800 / 14400 / 600 hook calls). GameCube shasum OK.
- **Captures** (`SMS_PORT_SHOT_PRESENT=1`, off-screen windows 1280x720 / 1680x720, real `aspect` setting): plaza ticker at 16:9 centred / match and 21:9 16:9-mode / match (ticker clipped to its bar in all); Bianco 21:9 match and its pause menu; Corona Bowser (60,0) 16:9 match and 21:9 16:9-mode; the exit-area iris at 4:3, 16:9 and 21:9 (round); the intro movie with subtitles at 4:3, 16:9 and 21:9.
  Not captured: the lens glow (no scripted view of the sun found; no C-stick in the pad script). Seen in passing: the iris's black area shows thin horizontal gaps at 4:3 at 720p (GX_LINES rows of the 448-line frame at a higher internal resolution); also at 4:3, so not widescreen related.


## Decomp corrections (w13), round 3

Area: src/Enemy, GC2D, MSound, System, Animal. Went through the rest of the candidates list and `tools/lowmatch.py --below 85` (47 functions), plus `ctordiff.py --rank -c` filtered for differing immediates, constant stores and bit masks. GameCube shasum OK; Aurora build OK; boot run shows the logo screens.
- System/GCLogoDir `TNintendo2D` (constructed in `TGCLogoDir::setup`): the US logo colour is (220, 0, 0, 255), red; the source had (0, 70, 255, 255). Under VERSION_GMSE01 in include/System/GCLogoDir.hpp (other regions unchanged, no asm to check). Checked with a port screenshot: the boot logo is now the red US one.
- Enemy/bosseel `TBossEel::perform`: the vacuum (bck 14) sound is 0x2180 in the US asm, not MSD_SE_BS_UNG_VACUUM (0x8120, which the US build never uses). Under VERSION_GMSE01.
Checked and found to be codegen noise (no change): MSound getDistPowFromCamera (add order), bosspakkun setGroundCollision, rocket setDeadAnm, BossHanachan execWalk/getBodyMaxRotateZ, koopajr/BathtubKiller manager load/loadAfter, TBubbleManager::createEnemyInstance (constructor), TabePuku Drag / Bird WalkOnGround / AnimalBase execWalk (TQuat4::rotate inlined in the asm, JGQuat4.hpp, out of scope), TTPHitActor::updateTerrainCollsion, feetinv (normalize then scale vs one setLength), limitkoopa head receiveMessage (pushNerve out of line), Kukku/KukkuBall, CardLoad perform, Bird init, Kazekun/doLanding (sqrt inlined).
`MSStageCubeFadeDouble::proc` (96.3%): different bool/branch layout only; same condition, same calls in the same order.
Sweep paused here (2026-10-08) for the regional-behaviour task (see "Regional behaviour (w13)" below). Done: the candidates list for Enemy/GC2D/MSound/System/Animal and lowmatch < 85%. Not yet done: the 85-95% band of those directories (aligned_scan + aligned_isolated, then ctordiff -c), and the large GC2D functions (GCConsole2 perform, Guide perform) beyond the spot checks above.


## Regional behaviour (w13)

### Stages 1-2 (merged earlier)
- Every `VERSION_*` site in src/include was classified: D disc data (message ids, file names, text, the disc's 2D layouts, sound ids that follow the disc's sound tables), B pure game-code behaviour, V video (PAL 50 Hz / EURGB60), L struct layout, G GameCube-only (map file names, OSPanic line numbers, the process meter).
- One exe per disc region: `build_aurora.py --region us|jp|eu|all` (sms_port.exe, sms_port_jp.exe, sms_port_eu.exe), each compiled with its region's `VERSION_*` defines, so D/V/L sites are right for the disc. A disc of another region is handed to the sibling exe before any window opens (`port_disc_early_handoff`). The EU disc was refused with a message until its PAL decomp was done; it plays now (see "PAL decomp follow-up" below; `SMS_PORT_EU_DISC=0` still refuses it). 174 `VERSION_GMSE01` guards were flipped to `VERSION_GMSP01` so the PAL build takes the US code where the PAL release has it. `smoke_all.py --region/--disc`.

### Stage 3: "Game behaviour" (run-time choice of the class B differences)
- **Mechanism:** `include/version.h` `SMS_BEHAVIOUR_IS(US|JP|EU)` and `SMS_BEHAVIOUR_PAL_FAMILY()` (= not JP). In the port they call `SMSGetBehaviour()` (`port/src/port_behaviour.cpp`, read once: `%SMS_PORT_BEHAVIOUR%` disc/us/jp/eu, else `[game] behaviour`, else the disc's own region). Every B site keeps its GameCube `#if` text in an `#elif` after a new `#ifdef SMS_PORT` branch, so the GameCube build is unchanged (`shasum -c` OK after every step). `port_compat.h` includes version.h for every unit.
- **Converted B sites** (US/PAL code vs JP code; the "PAL" column is the evidence that the PAL release has the US code: function sizes in the three symbol tables, or a normalised instruction compare of the US and PAL splits, `fncmp.py`):

  | Site | Difference | PAL = US? |
  |---|---|---|
  | AmiNoko `init`, `perform`, `calcRootMatrix` (PAL/JP bodies both compiled), `creepToCurPathNode`, die nerve | effects/direction, path creep, hit filter | sizes equal (US 0x224/0x7c/0x2c0), nerve identical but for the shifted live flag |
  | BombHei `clipEnemies` | US/PAL never clip Bob-ombs | (header inline) |
  | SmallEnemy `isHitValid` | hide on message 0xB | (header inline) |
  | bosstelesa / Sirena `getForcastResult` | 10000-step cap, rounding | sizes equal |
  | enemyAttachment `moveObject` | `setBehavior()` | equal |
  | gatekeeper `startBGM`/`stopBGM` | gatekeeper BGM flags | inlined, not measurable |
  | popo `kill`, explosion nerve | kill the nozzle, explosion sound | equal; nerve identical but for the live flag |
  | GCConsole2 Mario-icon timer (`unk3AE`), pressure flash colour, bathtub rocket nozzle, Yoshi tank hide, life-meter alpha, water-gauge quad | HUD | `drawWaterBack` identical, `startDisappearTank`/`startInsertLife` equal |
  | Talk2D2 `setMessageID`/`openNormalWindow`/`eraseNormalWindow` (`unk340`), `makeBoxLine` (char spacing, -0x28/-0x25, 1.05/1.1), `openWindow` (-1 degree) | talk balloon layout | equal except **makeBoxLine: PAL has its own version** (glyph characters, -0x26 offset; now decompiled for the EU build, see below): EU behaviour on a US/JP disc uses the US one, as the PAL one belongs to the PAL message files (experimental) |
  | MActor `setLightData` | shadow light only near the ground | equal |
  | MapEventSink Bianco, Airport `watch` | clear-sign sound | equal |
  | PollutionLayer `initLayerInfo` | electric goop thresholds | equal |
  | Item `TShine::appear*` | fanfare | equal |
  | ItemManager `resetNozzleBoxesModel` | no smoke sound | equal |
  | MapObjBall `touchGround` | no melon slip sound | equal |
  | MapObjBlock `TJuiceBlock::touchActor` (override compiled in every build) | kill on map objects | equal |
  | MapObjDolpic demo cannon | voice timing | equal |
  | MapObjFlag `update` | **no flag sound in Ricco Harbor** | (inlined) |
  | MapObjMare cogwheel | sound threshold/volume | equal |
  | MapObjMonte hanging bridge | rope heights, rope ends | equal |
  | MapObjPinna ferris wheel (`FERRIS_WHEEL_RATE`, fast-spin condition) | **EU-only code** (from the PAL decomp) | PAL differs, EU branch |
  | MapObjPinna `TAmiKing` (`calc` vs `calcRootMatrix`, both compiled) | effect timing | equal |
  | MapObjSirena slot drum, curtain | slot sounds, talk-mode check | equal |
  | MapStaticObject sounds, `mRandPlayIdx` (compiled in every build) | ambient sounds | equal |
  | NpcWalkTurn `isCanWalk` | arrival radius | (inlined) |
  | MarioJump `askStrongGroundTouch`, `landing`, `boardJumping`; MarioMove sniper sound, `stateMachine` loop guard, fall voice, `getOffYoshi`; MarioUpper / WaterGun out-of-line `isEmitting` (compiled in every build); Tongue `findTarget`, `calcAnim` | player | equal / identical |
  | MSound water filter, seq-sync cases 40/110/121-126, `mainLoop`, `talkModeIn` volume, Mario voice repeat check (`unk94`, compiled in every build) | sound | equal |
  | MSoundMainSide `MSStageCubeFadeDouble` (compiled in every build) | Noki stage sound | equal |
  | MarDirectorLoadResource `thpInit` half-second wait | | US 0x11c / PAL 0x18c (PAL also picks the 50 Hz THP) |
  | MenuDir first start (120 vs 30 frames) | | identical |
  | MovieDirector `TEndingString` (compiled in every build) | closing text over the last movies | US/PAL discs only: Japanese discs have no `ending_1.blo`, so it is skipped there whatever the behaviour |
  | GCLogoDir `TNintendo2D` | **logo colour** red / blue | setup identical but for the PAL ProgSelect size |
- **Left per disc on purpose (D):** message ids (balloons, boss and NPC messages, CardLoad/CardSave tables), text and encodings (GCConsole2 balloon text, ProgSelect, DVD error text, card comments), language handling (FlagManager, MovieSubtitle, Option's subtitle unit, PauseMenu2), `SelectMenu` stage name (the US 'map' pane has a 16-byte buffer: the JP strncpy would overflow it), the sound-id swaps where the US uses sounds the JP tables do not have (bosseel 0x2180, EventWatcher 0x485a/0x485b, GCConsole2 0x405C) and the `startMontemanBGM`/`startMontemanFanfare` script symbols the US/PAL disc scripts call. V and L sites stay per build (PAL 50 Hz, the shifted PAL live flags, PAL-only layout fields).
- **EU behaviour on a US or JP disc is experimental:** besides the PAL `makeBoxLine`, the PAL release changes these functions in ways the decomp does not have yet (sizes differ from US where US = JP): fishoid `checkHitActors`, Koopa get-down/tumble nerves, bosseel bubble/heart-coin `perform`, enemyMario `consider`/`emReplay`, limitkoopa neck callback, BathWaterManager `loadAfter`, `TBathtub::perform`, `TLiveActor::perform` (integer animation frames), `fireGetStar`, `getNameRef_BossEnemy` (GCConsole2 `processAppearStar`/`checkDolpic8`/`perform` and Guide are done now, below). Several look like 50 Hz adjustments. Until the PAL decomp has them, "Europe" = the US code plus the known EU-only branches, on the EU disc too.
- **Setting:** `[game] behaviour = disc|us|japan|europe` (port_settings, w9's files, marked w13), F1 > Game > "Game behaviour" (Match disc / US / Japan / Europe (experimental), tooltip). It applies at the next start: the menu shows "Restart the game to apply the change" and a "Restart now" button. `port_restart_now` (called from `pumpEvents`, outside an Aurora frame) saves the settings, shuts Aurora down and starts the exe again with the same command line; a hand-off child exits with `PORT_EXIT_RESTART` and its parent starts it again (`handOff`, port_disc.cpp). Test knob `SMS_PORT_RESTART_AT=<frame>[:<behaviour>]`.
- **Replays:** the ENV chunk now carries `SMS_PORT_BEHAVIOUR` (and on the EU build the PAL settings); older replays play back with the disc's behaviour, not the player's setting.
- **PAL settings (EU build):** `[game] pal_video = 60|50|ask` and `pal_language = 0-4` (F1 Game tab on sms_port_eu.exe only), the SRAM values the EU game reads (`OSGetEuRgb60Mode`, `OSGetLanguage`, sdk_os_misc.cpp). Used with the EU disc (below).
- **README:** a "Regions and game behaviour" section; Europe marked experimental.
- **Checks:**
  - GameCube `shasum -c` OK; `build_aurora.py --region us|jp|eu` all link (752/752, 0 undefined).
  - US build, Match disc = the old behaviour, by replay: sessions recorded with the new sms_port.exe (scripted keys: Ricco Harbor 3,0 walking/jumping/spraying, 2640 game frames; Pianta Village 8,0 with the hanging bridge, 2583) replayed with main's sms_port.exe from before stage 3: 0 of 21 and 0 of 16 checkpoints differ (Mario's position/action, rand state, every live actor's position and flags, camera matrices). Replayed again with the final build after re-syncing main's newer files: 0 of 21, 0 of 16.
  - Visible differences on the US disc: Nintendo logo red (US, Europe) / blue (Japan), `port/build_aurora/w13_behaviour_logo.png`; airstrip talk balloon (makeBoxLine) text sits 3 px lower with Japan behaviour, text correct in all three, `w13_behaviour_talk.png`; the Ricco Harbor flag sound starts only with Japan behaviour (temporary log in `TMapObjFlag::update`, removed again).
  - `smoke_all.py --quick`: US disc + US build 20/20; JP disc + JP build 20/20; US disc with `SMS_PORT_BEHAVIOUR=jp` 20/20 and `=eu` 20/20; JP disc with `=us` 20/20 (newgame scenes reach area 0 with dialogs; no wrong text seen in the airstrip dialog shots).
  - Restart: `SMS_PORT_RESTART_AT=300:japan` on the US disc: the game restarts and logs "game behaviour: Japan (settings)", the ini has `behaviour = japan`; on the JP disc via sms_port.exe -> sms_port_jp.exe hand-off with `:us`: "sms_port_jp.exe restarts", then "game behaviour: US (settings; disc build Japan)".

### PAL decomp follow-up: the European disc (GMSP01) plays
PAL-only code (`#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)`, so the US and JP builds and the GameCube build are unchanged) written against the split PAL DOL (`configure.py --version GMSP01`, `decomp-diff.py`; percentages are per-function matches of the PAL objects, all these units are NonMatching):
- **2D directory:** `SMS_COMMON_2D_DIR` (version.h): PAL `/cmn2d/`, else `/common/2d/` (CardSave, GCConsole2, Guide, ScrnFader, SelectMenu, Talk2D2, ConsoleStr).
- **ConsoleStr** (first crash, `TConsoleStr::load`): PAL layout (header), per-language "Go!"/"Shine Get"/"Too bad" picture counts, two-digit pane names, PAL `processGo` and `processMiss` rotation table. 97-100%.
- **Option** (`TOptionRumbleUnit` crash): PAL `TArrowControl`, per-language textures (`/option/timg/<lang>/`) on the rumble/sound/subtitle units, new `TOptionLanguageUnit`, PAL `TOptionControl` (language option, top message with colour codes, card writes). 94-100%.
- **CardLoad** (`waitForChoice` crash, `loadmessage.bmg` missing): PAL layout (up to 15 "PRESS START" letters, everything after shifted by 0x5C; 5-language arrays from 0x7D4), `loadmessage_<lang>.bmg` and `load_score_<lang>.blo` for all languages, title per language, `loadLangTexture`/`changeLanguage`/`resetScoreScreenObjects` (language switch after the option screen), `resizeBox` (message windows fitted to the text: `drawMessage`, `waitForAnyKey`, `waitForStart`), PAL `cMessageID` (ids from 0x1C one higher), title letter timing 600/400, copy onto an empty file skips the overwrite question, format-done message timing. 97-100% except new helpers (register allocation).
- **GCLogoDir** `direct_nlogo`: the 60 Hz question (PAL `TProgSelect`, already decompiled) on a PAL television when the console is set to 60 Hz or B is held a second, the card's options loaded for its language, 50 Hz otherwise. 99.4%.
- **Talk2D2:** `message_<lang>.bmg`, single-byte text with glyph colours ('@' '#' '%' '+' '<' '>' 0xA5 '$' '*', '$' in a bigger font), PAL `makeBoxLine` (glyph spacing and baseline, -0x26, 1.05), choice balloon follows the talk side.
- **GCConsole2:** '*' glyph colour in balloon text (D, EU build only).
- **New B sites from the PAL compare** (US code = JP code; run-time `SMS_BEHAVIOUR_IS(EU)` in the port): GCConsole2 `perform` refreshes the water tank's nozzle picture when the counters reappear; `processAppearStar` shows the hundreds digit at exactly 100 shines during a demo with flag 0x10; `checkDolpic8` (Delfino news ticker): **the US release has no `break` after the a == 0 cases** (retail asm: each case falls into the next and into a == 1, so the "A" variants never show); the decomp had the breaks, so the port showed PAL behaviour on the US disc. Now US/JP fall through like the US retail code (JP assumed: no JP split), EU breaks. Guide: PAL treats map 20 as stage 0 and shows no Mario marker there.
- **PAL video in the port (V):** the game's `VIGetTvFormat` is renamed to `port_VIGetTvFormat` (port_behaviour.cpp): NTSC for US/JP; on the EU build PAL at boot (the logo always is) and then whatever the game configures (`port_VIConfigure` reports `viTVmode`): PAL 50 Hz or EURGB60. The retrace interrupt follows (`port_vi_set_retrace_ns`, port_main.cpp, 20 ms or 16.68 ms; the interpolation timing reads the same period). `pal_video`: 60 = no question, stays 60 Hz; 50 = no question, 50 Hz; ask = the game's own question.
- **EU disc accepted:** `euDiscBlocked` refuses it only with `SMS_PORT_EU_DISC=0`; README says experimental.
- **Checks:** EU disc, sms_port_eu.exe, new save: logo, title, "create a file?" (window fitted to the text), file select with the per-language pictures, opening movie, airstrip, Peach's talk in English with correct spacing (`port/build_aurora/w13_eu_talk.png`, `w13_eu_fileselect.png`); `pal_video ask` shows the question in English (`w13_eu_60hz_prompt.png`), `60` skips it (log: PAL 50 Hz for the logo, then PAL 60 Hz), `50` stays at 50 Hz. GameCube US `shasum -c` OK after all of it; all three exes link.
- **Next up, deferred (coordinator, 2026-10-09: effort moved to visuals and multiplayer sync; not started):** the PAL-only functions listed above under "EU behaviour ... experimental" (fishoid, Koopa nerves, bosseel, enemyMario, limitkoopa, BathWaterManager, Bathtub, TLiveActor, fireGetStar, getNameRef_BossEnemy); Option/CardLoad register-allocation mismatches; PAL THP at 50 Hz (`MarDirectorLoadResource` already picks it) not checked in game.


## E20. Screenshots of the presented picture (`SMS_PORT_SHOT_PRESENT=1`), and presentation checked with them
- **New:** `port/patches/aurora_present_capture.patch` adds `aurora::gfx::request_present_capture(callback, userdata)`: one-shot copy of the swapchain texture after the present blit and the ImGui overlay (exactly what the window shows), read back after submit. The surface gets `CopySrc` when its capabilities allow it (D3D12 does).
  `SMS_PORT_SHOT_PRESENT=1` with `SMS_PORT_SHOT=<frames>` writes `present_<frame>.bmp` (window size, bars included) next to the usual `shot_<frame>.bmp` (EFB). Works with the window covered or off-screen (`SMS_PORT_OFFSCREEN=1`, recommended: nothing appears on the desktop) and with other copies running; nothing with `--nullgfx`.
- **Checked (Bianco, off-screen windows; `aspectruns.ps1`-style settings files via `SMS_PORT_SETTINGS`):** content box measured on each picture, last presented rows compared with the rows above.

  | window | aspect | internal | presented content | bottom rows |
  |---|---|---|---|---|
  | 1600x900 | stretch | window | 1600x900, fills | continuous |
  | 1890x810 (21:9) | fit | window | 1157x810, pillarboxed, 1.428 | continuous |
  | 700x1000 (portrait) | fit | window | 700x490, letterboxed | continuous |
  | 700x1000 | stretch | window | 700x1000, fills | continuous |
  | 700x1000 | 4:3 | window | window becomes 700x525 | continuous |
  | 1600x900 | 4:3 | window | window becomes 1200x900 | continuous |
  | 1600x900 | fit | window | 1286x900, pillarboxed | continuous |
  | 1600x900 | fit | native 640x448 | 1286x900 (upscaled) | continuous (E16 clamp) |
  | 1600x900 | stretch | native | 1599x900 | continuous |
  | 2560x1440 (screen-sized) | fit | window | 2057x1440 | continuous after the fix below |
  | 2560x1440 | 4:3 | window | window 1920x1440 | continuous after the fix below |

  No band in any of them. The screen-sized window stands in for fullscreen (same `resize_swapchain` / present path; fullscreen itself was not started).
- **Fix found with it (aurora_efb_size.patch, `gpu.cpp resize_swapchain`):** at 2560x1440 the presented content was 1439 rows plus one black row: the EFB target height was rounded to nearest (1440 * 528 / 448 = 1697.1 -> 1697), so the game's 448 lines covered only 1439.9 target rows and the last row was never rasterised. The target is now rounded up (1698); `SMS_PORT_SHOT` crops round down to match (`port_aurora_sdk.cpp`: 1698 * 448 / 528 = 1440.6 -> 1440). Exact scales (1056, 896, ...) are unchanged.
- **Bath ripple (Bowser, `60,0`):** walked around the rim for 1500 frames; the bath water surface never came into view clearly (Bowser fills the tub, Mario is burned and the run ends with the game-over wipe). The ripple/height-map copies run every frame; screenshot still missing.



## E21. Review of w6's R17 (internal resolution), R18 (MSAA), R20 (widescreen) against E19
- **R17 `aurora_copy_scale.patch`** (rounded copy rectangle, exact 1:1 copies): right; a 1:1 copy must never resample.
- **R17 changes in `port_efb.cpp`** (marked `w6 (round 5)`): accepted as they are. A 1:1 CPU-read copy samples each GameCube pixel at its centre (`taps = 1`), which is what the copy unit does; the pixel counter counts on the 640x528 logical grid, one sample per GameCube pixel centre, so the count is the same at every scale and target shape (`toLogical = 1`). Down-scaling copies keep the box filter.
- Re-checked after E20's rounding change (EFB target rounded up): airstrip counter 4548 and readback 4548 texels at internal scale 3.2143 (1440 rows) and 2.4107 (1080 rows), as in R17.
- **R18 MSAA:** copies, snapshots and peeks use the resolved target (Aurora's `resolve_pass` returns single-sample snapshots); depth copies use sample 0 (`aurora_msaa_depth.patch`). Matches E19; the remaining counter difference at partly covered edge pixels is documented in R17 (about 2 % while dirty, 0 when clean).
- **R20 widescreen:** the logical EFB stays 640x528 and maps linearly (anamorphic, per-axis scale, no offset) to the wide target, and the 3D view is widened through the game's projection, not through extra logical columns. That keeps every assumption of `port_efb.cpp` / `port_aurora_sdk.cpp` (no offset to export) and the 4:3 UVs of the screen-texture and mirror copies. Agreed. Presentation of the new aspect modes still to be captured with `SMS_PORT_SHOT_PRESENT` once w11's camera hook lands.



## E22. Shine-get save prompt on a flat grey background (w12): original behaviour
- After a shine, `TMarDirector` (MarDirectorDirect.cpp, `DIRECTOR_FLAG_SHINE_GET_PENDING`) starts the shine fader (`unkDC->registFadeout(120, 640)`: 5.33 s of the shine demo, then a 1 s fade) and sets `DIRECTOR_FLAG_CARD_SAVE_PENDING`; the card save opens once the director is back in its normal state. The shine fader (`TShineFader`, GC2D/ShineFader.cpp) ends at `FADE_STATUS_FULLY_FADED_OUT`, alpha 0xFF, in the colour `MarDirectorSetup2.cpp` gives it: `TColor(0xD2, 0xD2, 0xD2, 0xFF)`.
- So the save prompt is drawn over an opaque 210/210/210 screen on the GameCube too; no EFB copy is involved (`TCardSave::execIssueGX_` draws only its 2D layout). Measured on the port (EX course `21,0`, `SMS_PORT_DEBUG_CALL=tpshine@1700`): background (210, 210, 210), the shine demo before it renders the stage normally. Not changed.



## E23. EFB readbacks under GPU load; `--nullgfx` (w12)
- **Buffers pooled:** each readback used to create three buffers (map-read readback, compute output, uniform) and a bind group; about 7 jobs per frame in Bianco. The three buffers now come from a pool (keyed by size) and go back to it when the readback has been mapped (the GPU is done with them); the uniform is written with `queue.WriteBuffer`, the output cleared with `ClearBuffer`. Steady state creates no buffers.
- **No pile-up:** at most 4 requests per result (copy destination, peek, counter) and 256 in total are in flight. A result that already has 4 outstanding is not requested again, so there is no snapshot, no pass break and no job; requests fall to the GPU's pace, and the newest finished readback is stored. (Results normally arrive 1-3 frames late, so the cap does not bite on an idle GPU.) Requests whose frame never got submitted are forgotten after 60 game frames.
- Checked: Bianco alone (readbacks and counter as before, 1-2 frames late, counter = readback texels); 4 Bianco instances in parallel with other agents' games on the GPU: all four at full rate (600 frames per 600 retraces after loading), readbacks still 2 frames late.
- **`--nullgfx`:** Bianco and Ricco with spraying run 4800+ frames on the Null backend with the current build (E17's skip covers `port_efb.cpp` snapshots/jobs and `port_aurora_sdk.cpp` peeks/screenshots; Aurora's own copies, including w6's copy-scale path, allocate textures only, which the Null device handles). The "Out of memory" in w12's runs is most likely from a build before E17; if it still happens with today's main, please send the log tail.


## E24. w9's frame-rate interpolation and the EFB: review and a 30 / 60 / unlimited comparison
- **Code review (port_interp.cpp, MarDirectorDirect.cpp `port_mardirector_redraw`, port_main.cpp `displayFrame`, port_efb.cpp, port_aurora_sdk.cpp `GXSetDrawSync`):**
  - Interpolated frames run with `port_efb_set_presentation_only(1)`: copies are GPU-only, nothing is queued, no destination ages, and `port_efb_end_frame` is only called for an Aurora frame that holds a logic draw (after the flag is cleared again). Game memory therefore only changes at the end of game frames.
  - Draw-sync callbacks are not called in interpolated frames, so `GXPeekZ`, `GXPeekARGB` and `GXReadPixMetric` are never reached there. The game's `TDrawSyncManager` still pushes its breakpoints, so its breakpoint ring gets unbalanced. That is harmless: `GXEnableBreakPt` / `GXDisableBreakPt` do nothing on the port.
  - The redraw skips `unk38` / `unk3C`: the pollution layers, the graffito check and the bath-water preprocessor (`initECTGft` puts `TBathWaterPreprocessor` in `unk3C` when the stage has no pollution joint models). The bath water's `render` copies (alpha, depth 0x27, mist) are per-frame copies of the current EFB and are fine to repeat. The water manager has no copy of its own.
  - `port_efb_clear_all` leaves Z mode, colour update and alpha update set. That is harmless: the stage draw sets them first thing (`JDrama::TEfbSetting`, `TEfbCtrl`).
- **Interplay with E23 (pooling, per-key caps):** `[debug] record` defaults to 1, so `port_replay_deterministic()` is true in normal play and w9's fixed-latency path is the normal path. Every result reaches the game exactly 3 game frames after its request. `keyBusy` never coalesces; instead `detEndFrame` waits for a late GPU result (up to 1 s). The per-key cap of 4 only applies with `record = 0`. Pooling works the same on both paths. The clock is game frames on both paths, so interpolated frames add no latency.
- **New test knob:** `SMS_PORT_EFB_TRACE=1` (port_efb.cpp) logs one line per counter value the game reads and one per copy readback stored into game memory (with the game frame, the request frame, non-zero texels and a hash of the stored texture). It also logs each destination copied in an interpolated frame and any read attempted in one.
- **Run:** airstrip `warp 0,0` with a PADSCRIPT that walks Mario into the goop and sprays (`+0-900/45:A,+900-1500:G-2750;1480,+1500-2700:R+S90,+2700-3300:G-1500;1500,+3300-4500:R+S270`). About 9000 frames each, 4 instances at once, at `SMS_PORT_FPS` 30, 30, 60 and unlimited (60 fps: 5000 interpolated frames; unlimited: 44000).
  - Counter values seen by the game (0x7e layer, 0x92 objects; 9300 reads) and the stored 512x512 R8 maps (4600 stores, about 1500 distinct map states as Mario sinks, dies and respawns) are identical between 30 / 60 / unlimited, read for read and store for store, up to the point where PADSCRIPT input lands one game frame apart. (PADSCRIPT counts retraces; a press that lands on the other retrace parity reaches the game a frame later. Two 30 fps runs differ the same way.) Compared as a sequence of map states: 30 vs unlimited are identical over all 1509 states; 30 vs 60 match over 1153 states, until the 60 fps run's walk input landed on retrace 10930 instead of 10929.
  - Store latency: always 3 game frames. No "late" message, even at unlimited with 4 instances on the GPU.
  - Reads attempted in interpolated frames: 0. Copies made in interpolated frames: only the screen texture (640x448 -> 320x224 RGB565), a 256x256 RGB5A3 copy with clear and a 48x48 RGB565 copy at the screen centre, all GPU-only. The pollution maps are never copied in an interpolated frame.
  - With `SMS_PORT_RECORD=0` (the E23 path): no reads in interpolated frames either. Latency is 1-2 game frames at 30 fps and 1 at unlimited, because finished readbacks are picked up at Aurora frame ends and unlimited has more of them per game frame. Each result is still stored only at the end of a game frame, so a GPU result never reaches the game in the frame that requested it.
- Verdict: no change needed in port_efb.cpp for interpolation.


## E25. Review of w6's MSAA sample-0 path (`aurora_msaa_depth.patch`, `snapshot(..., sample0)`)
- Counters (`GXReadPixMetric`) and CPU-read copies take sample 0 of each pixel, which is the GameCube's single sample per pixel; depth peeks and depth copies use sample 0 too. That matches E19.
  - Measured: airstrip at 21:9 with MSAA 4x and 60 fps. Layer counter 5601 -> 4548, as without MSAA (E27 table).
- **Changed (port_aurora_sdk.cpp):** `GXPeekARGB` now also reads sample 0 (`colorSample0 = !screenshot`). Mario's occlusion probe tests `alpha == 0x10` exactly, and a resolved edge pixel would average that away. Screenshots keep the resolve. Without MSAA this is a no-op (Aurora ignores the flag at 1 sample).
- **Changed (port_efb.cpp):** the shared per-callback snapshot (`sSnap`, used by the `GXPeekZ` series) is never a sample-0 snapshot (`shareable && !sample0`), so a later caller cannot get the wrong kind. No current caller combines the two.
- **For w6 (Aurora side, `tex_copy_conv.cpp snapshot_color_sample0`):**
  1. When the compute pipeline could not be created, or the format is not RGBA8/BGRA8, the function returns without writing the snapshot. The pooled snapshot texture then keeps whatever an earlier use left in it, and counters and pollution maps would read stale data silently. Falling back to the resolved copy (the `else` branch in `encoding.cpp`) would keep them correct.
  2. For an sRGB scene format it packs `textureLoad` values (linear) as raw bytes into an sRGB texture. This is harmless today, because the EFB is non-sRGB (`to_linear`), but such formats should be encoded or rejected.
  3. It creates a uniform buffer and a bind group per snapshot (several per frame). This is a performance point only; they could be reused as in E23.
  4. Reusing the one `g_colorSample0Buffer` for several snapshots in a frame is correct: WebGPU orders the compute writes and buffer-to-texture copies within the submission.
- MSAA runs here had no sample-0 or resolve warnings. The 21:9 MSAA run logged R18's transient `E_OUTOFMEMORY` for two pipelines, with 4 instances on the GPU.


## E26. Bath water at the Corona Bowser fight (`warp 60,0`, `SMS_PORT_WARP_FLAGS=story9`)
- Reached: `PADSCRIPT=+200-330:S0` walks Mario off the rim into the tub; the bath water throws him and the camera looks down at the water around Bowser.
  - `port/build_aurora/e24/corona_bath_43_present_880.png` (window) and `..._crop.png` (full resolution): the translucent water with the floor pattern refracted through it, the mist layer (`draw_mist`, RGB565 mip copy) and Bowser's legs behind the mist. No missing or black copy.
- Also at 21:9: `corona_219_present_1300.png` (rim view; the tub interior is not visible from the rim).
- The ripple simulation is CPU-side (`calcWaterModel` on `CUE_MOVE`), and the height/depth copies are made from the current frame (E24). Nothing here depends on readback timing.


## E27. Widescreen (w11's Hor+): screen-texture, mirror and heat-haze copies; pixel counters stay on the GC grid
- **Code:** w11 widens the camera through `TLookAtCamera::mAspect` (`PORTWideCameraAspect`, cameragc.cpp). Everything that projects into a screen-space copy reads that same `gpCamera->getAspect()`: the texgen effect matrix `SMS_GetLightPerspectiveForEffectMtx` (heat haze `TShimmer`, bath water, Boos, the eel, NPC parts), the FLUDD water's `C_MTXLightPerspective` (ModelWaterManager.cpp 1838) and the mirror camera and its lookup (MapMirror.cpp 47, 61, 176).
  - The copies still take the logical 640x448 rectangle (or the mirror's texture size), which maps anamorphically onto the wide target (R20). The copied image therefore holds the widened view in 640 logical columns, and the UVs computed with the widened aspect point at the same place.
  - Logical EFB, copy rectangles, peeks and counters are unchanged (E19, E21).
- **Shots (`SMS_PORT_SHOT_PRESENT=1`, off-screen windows 960x720 4:3, 1280x720 16:9, 1680x720 21:9), in `port/build_aurora/e24/`:**
  - Airstrip: `air_43/169/219_present_6100.png`, and `air_219_msaa60_present_6400.png` (MSAA 4x at 60 fps). The view is wider and the HUD stays 4:3.
  - Bianco: `bianco_43/219_present_1300.png`.
  - FLUDD stream at 21:9: `bianco_219_spray_present_1300.png`, with a full-resolution crop in `bianco_219_spray_stream_crop.png`. The path and grass seen through the stream join the surroundings with only the small warp, and there is no shifted copy of the scene inside the stream. A sliding UV would show one.
  - Heat haze: TShimmer is in the airstrip, Bianco, Ricco, plaza and other scenes, but it is drawn only while Mario has lost his cap (`isWearingCap`). None of these runs reached that state. It shares the effect matrix with the bath water and the FLUDD stream, which line up.
  - The mirror copy (`髀｡謠冗判繧ｹ繝・・繧ｸ`, every scene has a `MirrorCamera`) runs in all of these runs without errors; no scene with a visible mirror was captured.
- **Pixel counters:** they count on the logical `efb_size()` grid, one sample per GameCube pixel centre (R17), so nothing outside the GC 640x528 frame can be counted, whatever the target's shape. Airstrip goop script (E24) per aspect:

  | window | aspect | 0x7e (layer) first / min | 0x92 (objects) first / min |
  |---|---|---|---|
  | 640x480 (default) | 4:3 | 5601 / 4548 | 3677 / 2865 |
  | 960x720 | 4:3 | 5601 / 4548 | 3678 / 2824 |
  | 1280x720 | 16:9 | 5601 / 4548 | 3665 / 2851 |
  | 1680x720 | 21:9 | 5601 / 4548 | 3688 / 2894 |
  | 1680x720, MSAA 4x, 60 fps | 21:9 | 5601 / 4548 | 3683 / 2887 |

  The layer counter is identical. The object counter's first reading varies by +-0.3 % (edge pixels, the same spread R17 found between internal scales). The minima differ more because the runs diverge later (PADSCRIPT timing as in E24; the wider view may also change what the game culls).
  If the objects were counted through the widened camera, 16:9 and 21:9 would differ by tens of percent. The object pass uses its own matrices (`initCountObjDegree`), not the camera.


## E28. Corona Bowser: flat bath water and missing rubber ducks (user report). Not the frame rate; three port bugs
- **Symptom:** the bath water was a flat, misty sheet over the tub floor, and Peach's and Bowser Jr.'s rubber ducks were nowhere to be seen. It is the same at 30 and 120 fps (`port/build_aurora/e28/before_30fps_present_760.png`, `before_120fps_present_760.png`), so w9's interpolation is not involved. E26's "no missing copy" was wrong: I took that flat sheet for the real water.
- **How the water works:** `TBathWaterMeshRenderer::prerender` (bath preprocessor) draws the simulated drops (`TBathWater` drops, 160 of them, radius 700) as balls with an orthographic camera looking down at the tub, into the top-left 84x84 of the EFB, depth only. It then copies that depth as `GX_TF_Z24X8` into RAM (`unk800A4`). `render` calls `makeHeightMap`, which decodes the copy on the CPU into the 84x84 height field of the water mesh. `getWaterHeight`, which positions the ducks (`TBathtubBinder`), reads that height field too. Three things broke it:
  1. **The balls were never drawn (decomp bug).** `JGRotation3.hpp setLookDir` builds the camera's y axis as `cross(x, z)`, but the target computes `cross(z, x)` (this unit's asm in `setLookDir`: `f28 = z.y*x.z - z.z*x.y`; decomp-diff shows the opposite operand order). That gives a mirrored basis: with `GX_CULL_BACK` every ball face was culled.
     - Proof: a debug capture of the prerender with colour on was black. Cull off showed the balls (`e28/debug_prerender_balls_cull_none.png`); depth test off changed nothing.
     - Fix in game code (`src/Map/BathWaterManager.cpp`, `TBathWaterMeshRenderer::tmpFake`, under `SMS_PORT`): the same maths with the original cross order.
     - The shared header is not edited. Editing JSystem was refused here, and `setLookDir` has no other user. **For w13 / the JSystem owner:** `fVar1076.cross(fVar958, fVar432)` should be `cross(fVar432, fVar958)` (the unit is NonMatching; the GameCube DOL does not change).
  2. **The depth copy was never read back (my bug).** `port_efb.cpp` read back only `GX_CTF_R8` copies by default and skipped every depth format. E12 even listed the bath copies as GPU-only, which was wrong. Depth copies `GX_TF_Z8/Z16/Z24X8` are now read back:
     - a compute pass reads the R32Float depth snapshot (sample 0 with MSAA) at each GameCube pixel centre and gives z24, with reversed Z handled;
     - `encodeTexture` stores it as the GX texture: Z24X8 as the RGBA8-style (X, Z hi) / (Z mid, Z lo) planes, Z16 big-endian, Z8 the high byte.
     The `SMS_PORT_EFB_TRACE` store line now prints the z range of depth stores.
  3. **Byte order (port gap).** `makeHeightMap` reads the copy's u16 planes natively. The texture is in GameCube (big-endian) order, so under `SMS_PORT` it reads them with `sms_be16`.
- **Result** (`port/build_aurora/e28/`):
  - The height field changes every game frame: z `b59266..ffffff`; 898 stores, 898 distinct, where before it was a single all-`ffffff` map.
  - The tub is full of green, wavy water around Bowser, with Peach's duck on the left and Bowser Jr.'s duck (with his brush) on the right, riding the surface: `after_30fps_present_760.png`, `after_120fps_present_760.png`, `after_30fps_present_600.png` (rim view).
  - The 30 and 120 fps height fields are identical store for store at the same game frames.
  - At 120 fps, consecutive presented frames (`SMS_PORT_SHOT_DISPLAY`, `after_120fps_display_frame0/1.png`) show no flicker. The water mesh is redrawn from the game frame's height field: the preprocessor is skipped in interpolated frames, and `render` only reads the stored map.
- **semdiff** on the unit (`tools/semdiff.py Map/BathWaterManager`): `prerender`'s camera set-up matches the target. The eye at `getThing().y + 3R`, the direction down to `getThing().y - R`, the up vector `(0, 0, @3419)` and the GX call sequence all agree; the remaining differences there are operand order. The `perform` / `throwMario` / `initializeIfYet_` / `calcBathtub` items are the ones w11/w13 already explained (the merged 0.9/10 literal, matrix offsets).
- GameCube `mario.dol: OK`; `smoke_all.py --quick` 20/20 (silent, off-screen).








## R22. Widescreen with w11's game side: movies pillarboxed; title, plaza, boss and pause checked at 16:9 and 21:9
- **THP movies** were stretched to the wide picture. `THPPlayerDrawCurrentFrame` (`sdk_thp.cpp`, marked w6) now squeezes the movie quad by 1/scale around the centre of the 640-wide frame (`PORTWideSqueezeRectX`), for both the movie director's full-frame ortho and `gxYuv2RgbSetup`'s; the rest of the frame stays the black clear. Exact no-op at 4:3. Subtitles are J2D and already squeezed (w11).
  Checked: plaza court movie at 16:9 (`shots/movie169_shot_1400.png`: picture x 160-1120 of 1280 = 960 x 720 = 4:3) and the intro movie at 21:9 (`shots/ws219_title_shot_900.png`: 720 x 540).
- **Shots with main** (MSAA 4x, AF 16x, FXAA on unless noted; window resolution, EFB readback = what the window shows, since the target has the display's shape):
  - Title: `ws219_title3_shot_2100.png` (background full width, logo round).
  - Plaza: `ws169_plaza_shot_2000.png`, `ws219_plaza_shot_2000.png` (warp 1,8 + `story8r`; wider view, HUD 4:3).
  - Boss: `ws169_boss_shot_1900.png`, `ws219_boss_shot_1900.png` (Corona Bowser; bath water copies fine).
  - Pause: `ws169_pause_shot_2600.png`, `ws219_pause_shot_2600.png` (dim background full width, menu 4:3).
- **Issue for w11 (game side):** the plaza news ticker's text runs past its bar in widescreen (`ws169_plaza_shot_2000.png` vs 4:3 `ws43_plaza_shot_2000.png`, where it is clipped to the bar). `TGCConsole2` clips it with `graphics->setScissor(TRect(unk544...))` (`GCConsole2.cpp` around line 4628), a raw `GXSetScissor` in 4:3 HUD coordinates that `J2DGrafContext`'s squeeze does not see. Squeezing that rect (`PORTWideSqueezeRectXi`) should fix it. The other raw `TGraphics::setScissor` users in GC2D (`ConsoleStr.cpp` 276, `CardLoad.cpp` 844, `Menu.cpp` 15) may need the same.
- The window's own presented image (w8's `SMS_PORT_SHOT_PRESENT`) is not in main yet; recheck with it when it lands. w9's frame-rate merge (vsync) is not in main yet either; check the menu after it lands. (Both done in R23 and R26.)


## R23. Presented window checked at 4:3 / 16:9 / 21:9 with every option; EFB floor at native; 1-pixel edge column
- **How:** `SMS_PORT_SHOT_PRESENT=1` + `SMS_PORT_OFFSCREEN=1` (w8's capture of the swapchain), Bianco `warp 2,0` frame 1500, settings files per run; content box and the last rows measured on each `present_*.bmp`. New test knob `SMS_PORT_NO_ASPECT_LOCK=1` (port_window.cpp, w6 line): the window keeps its configured shape, so a fixed aspect shows its bars as in fullscreen/maximized without covering the desktop. Runs in w6's `port/build_aurora/t/<run>/`.

  | run | window | aspect | internal | extras | presented content | EFB target |
  |---|---|---|---|---|---|---|
  | `p43_on169_2x` | 1600x900 (no lock = fullscreen stand-in) | 4:3 | 2x | MSAA 4x, FXAA, AF 16x | 1200x900, bars 200 + 200 | 1195x896 |
  | `p43_2x_ext` | 1200x900 | 4:3 | 2x | MSAA, FXAA, AF | fills | 1280x960 |
  | `p169_win_ext` | 1600x900 | 16:9 | window | MSAA, FXAA, AF | fills | 1600x900 |
  | `p169_1x` / `p169_1x_snap` | 1600x900 | 16:9 | 1x | off | 1599x900 + black column -> fills after (b) below | 796x448 |
  | `p169_on219_win` | 1920x810 (no lock) | 16:9 | window | FXAA | 1440x810, bars 240 + 240 | 1440x810 |
  | `p219_1x` | 1920x810 | 21:9 | 1x | off | fills | 1062x448 |
  | `p219_2x_ext` | 1920x810 | 21:9 | 2x | MSAA, FXAA, AF | fills | 2124x896 |
  | `p219_small_floor` | 640x480 (no lock) | 21:9 | window | off | 640x270, letterboxed | 1062x448 (floor, (a)) |
  | `vk_169_2x_ext` | 1600x900 | 16:9 | 2x | MSAA, FXAA, AF, **Vulkan** | fills | 1593x896 |
  | `movie169` | 1600x900 | 16:9 | window | off | intro movie 1200x864 pillarboxed (4:3), R22 | |

  No band or dark last row in any of them (last presented rows continuous with the rows above). HUD 4:3 and centred in the widescreen shots.
- **(a) EFB floor (Aurora `aurora_efb_floor.patch`, `window.cpp get_window_size`):** below native resolution the CPU-read EFB copies stop matching the GameCube: airstrip (`warp 0,0`, `SMS_PORT_COPY_LOG`) pollution map read back with 4532 non-zero texels at `internal_scale = 0.5` and 4551 at 0.75 instead of 4548 (several GameCube pixels share one target pixel, so the map's draw-copy-redraw cycle runs on a coarser grid). The frame buffer is now never below the render mode (640x448) in either axis; a smaller one is scaled up uniformly. That also covers a widescreen picture in a small window (21:9 in 640x480 was a 640x270 target). With the floor, 0.5 reads 4548 again (`air_s0p5_floor`). So there is **no below-native internal resolution**: it cannot keep the readbacks exact (task skipped as asked; the performance preset uses native 1x instead, R25).
- **(b) Present viewport (`aurora_efb_floor.patch`, `gpu.cpp calculate_present_viewport`):** a 16:9 picture at 1x is a 796x448 target (796.4 rounded), so the present viewport kept the target's own shape and left the right-most column of a 1600x900 window black. When the gap is only that rounding (at most half a target pixel + 1), the viewport now fills the surface. Real bars (4:3 on 16:9: 400 px) are untouched.
- Defaults: unaffected by both (window-sized target, no gap). Default title EFB and presented shots at frame 900 are pixel-identical between main's exe and this build (`old_title` vs `d12_title`; frame 900 is also identical run to run).
- Internal resolution menu: an ini value not in the list (e.g. 2.41) now shows as "Custom (2.41x)" instead of "Window resolution"; tooltip says it never goes below 640x448.
- D3D12 with MSAA 4x logged `E_OUTOFMEMORY` for one or two GX pipelines (5 retries, draws skipped) in 3 of 4 MSAA runs while 4 game instances shared the GPU (R18's transient). The Vulkan MSAA runs had none (R24).


## R24. Graphics API option: Direct3D 12 (default) / Vulkan / Direct3D 11, with fallback
- Aurora's prebuilt Dawn (`v20260807.225922`) has D3D12, Vulkan, D3D11 and Null compiled in (`DAWN_ENABLE_BACKEND_*` in `port_deps/aurora/build`); Aurora already tries `desiredBackend` first and then D3D12, Vulkan, D3D11, Null when one cannot create its adapter/device.
- **New `port/src/port_gfx_backend.cpp` (w6):** `[graphics] backend = d3d12 | vulkan | d3d11` (default d3d12 = what Aurora picked before), F1 Display "Graphics API" (next start) with a status line ("Running on Vulkan", or "... (Direct3D 12 could not start)" when Aurora fell back). **Start guard** for crashes and hangs while starting: right before `aurora_initialize` (`port_gfx_backend_arm()`, one line in port_main.cpp) `<settings file>.gpustart` records the API and the process id; it is removed after 120 presented frames (`port_gfx_backend_frame()`, one line in `port_window_frame`) or at a normal exit. If the next start finds it naming the configured API and that process is gone, the setting switches (D3D12 -> Vulkan, others -> D3D12), is saved, and the menu says why. A guard of a copy that is still running is left alone. `SMS_PORT_BACKEND=d3d12|vulkan|d3d11` overrides for one run (no guard); `SMS_PORT_NULLGFX` unchanged.
  **w9:** small marked additions in your files: `PortBackend` enum, `PortSettings::backend`, the `[graphics] backend` ini line and parse, the `port_gfx_backend_configure()` call in `port_settings_configure`, the menu combo/status line, and `port_gfx_backend_frame()` in `windowFrame`. **w7:** `port_gfx_backend_arm()` before `aurora_initialize` in port_main.cpp.
- **Checked on the RX 9070 XT (driver 32.0.31041.1004):**
  - Vulkan: title (`vk_title`), airstrip (`vk_air`: pollution readback 4548 texels, as D3D12), Bianco (`vk_bianco`), Sirena with MSAA/FXAA/AF/2x (`vk_sirena_msaa`), Corona Bowser with MSAA (`vk_corona`, `vk_corona2`); present capture works. No Aurora errors, no `E_OUTOFMEMORY`. Vulkan reached 4200 frames in 80 s where D3D12 reached 3000 in the same scenes (stage loads finish sooner: pipeline creation is cheaper).
  - D3D11: title (`d11_title`) runs, present capture works.
  - Title frame 900 EFB: D3D12, Vulkan and D3D11 pixel-identical.
  - Start guard (`guard.py` in the scratchpad): a D3D12 start killed after 0.9 s left the guard; the next start logged "Direct3D 12 did not finish starting last time ... switching to Vulkan", ran Vulkan, saved `backend = vulkan` and removed the guard after 120 frames. A guard naming a live process was ignored.
  - F1 menu with Vulkan: `menu_vk/present_1500.png`.
- Whether Vulkan also avoids the AMD D3D12 driver crash under heavy load (R14/R18 class) cannot be proven by a few runs, but it is the obvious thing to try when D3D12 crashes on a player's AMD card, and it had no pipeline `E_OUTOFMEMORY` in the runs where D3D12 had them.


## R25. Performance preset (weak PCs)
- F1 Display "Performance preset (weak PCs)" button: internal resolution 1x (native 640x448 scene), MSAA off, FXAA off, anisotropy as the game asks, trilinear off, frame rate 30 (no interpolated frames). Brightness, gamma, aspect, VSync and the graphics API are kept. Native is the cheapest setting that keeps the CPU-read copies GameCube-exact (R23 (a): nothing below it is offered).
- GPU 3D-engine busy time per game frame (Windows "GPU Engine\Running Time" of the process over 20 s, Bianco with the gameplay script, one run at a time, other agents' games on the GPU too, so roughly +-10 %): default 4:3 1280x960 1.25 ms; 1600x900 16:9 window resolution 1.32 ms; preset in 1600x900 16:9 0.88 ms (Vulkan 0.64 ms); 2x + MSAA + FXAA + AF 16x 2.04 ms (Vulkan 1.78 ms).
- "Reset graphics to GameCube defaults" now also turns VSync off (its default).
- Whole round (R23-R26) on main as of 2026-10-08 23:30: `smoke_all.py --quick --no-build` 20/20 PASS; GameCube DOL shasum OK.


## R26. Frame rate / VSync / Display tab coherence (after w9's merge)
- VSync sits under "Frame rate" (w9), with the note "(VSync caps the frame rate at the monitor's N Hz)" only when it does; Aurora uses FIFO-relaxed with VSync and mailbox/immediate without. The two "next start" options (MSAA, Graphics API) say so in place. The ini comment for vsync no longer contains "(w9)". No other changes needed.
- Present-capture shots of the menu at 1280x960: `menu_vk/present_1500.png` (Display tab: Fullscreen, Aspect, Internal resolution, Frame rate, VSync, Anisotropic filtering, Trilinear, MSAA, Graphics API + status, FXAA, Brightness, Gamma, Sharper screen effects, Reset / Performance preset, overlay).

# Multiplayer player interaction (P2, w8)
Design, wire format and settings: `port/MULTIPLAYER.md`, "Player interaction (P2, w8)". Files: `port/src/port_mp_interact.cpp`, `port/src/port_mp_interact_ui.cpp`. No game-source or w10/w15 file was edited.

## M1. Bump, stomp, ground pound, FLUDD knockback; host modes
- **How it works:** every client resolves only its own Mario.
  - Bumping is local: `TMario::keepDistance` against the other players' drawn positions, which brings in the game's clash knock-back in Full mode.
  - The attacker bounces with `TMario::trampleExec` on a proxy hit actor and sends a hit message (`PORT_MP_CH_INTERACT`, relayed by the host).
  - The victim knocks itself back with `TMario::damageExec`, damage 0.
  - Host options: `reserved[0]` (0 = Full, 1 = Solid only, 2 = Off) and `reserved[4]` (FLUDD pushes players). `reserved[3]` went to P5 meanwhile, so the FLUDD option uses [4]; the allocation is listed in MULTIPLAYER.md.
- **Tests:** `port/build_aurora/mp_interact_run.py` (scratch runner: 2-3 off-screen instances with per-instance env) on loopback, with `SMS_PORT_MP_INTERACT_TEST` teleports and PADSCRIPT.
  - 3 instances, Full mode:
    - Guest 1 ground-pounds the host from 400 above (PADSCRIPT `L`; Z opens the map). Host: `0c400201 -> 00020461` (full knock-down).
    - Guest 2 stomps the host: the guest does a trample bounce; host `-> 00020463` (short knock-down). The bounce lands it on the head again 40 frames later; the second hit leaves the host in the same knock-down.
    - Guest 2 overlapping the host is pushed out to 100 units (2 radii); the host does not move.
    - Guest 1 sprays at the host with guest 2 standing beside it: both get `FLUDD` every 30 logic frames (status `-> 00020464/65`, light stagger). Health stays 8 throughout.
  - 2 instances: `SMS_PORT_MP_INTERACT=solid`: no stomp, the landing player and the overlapping one are pushed apart, no FLUDD hits. `=off`: players overlap and nothing happens. `SMS_PORT_MP_FLUDD_PUSH=0`: spraying sends nothing, stomps still work.
  - Shots: `port/build_aurora/mp_interact_shots/inst*_present_*.png`. Instance 1 at frame 1540 is the pound from above; instance 0 at frame 1930 shows the host after the pound, with damage stars and camera shake. Earlier runs also showed the guest's trample bounce over the host and the host staggered in the guest's FLUDD stream.
- Single-player: the code only runs from w10's game-frame hook, which is only called in a session. The proxy hit actor is a static object (like `TModelWaterManager::mStaticHitActor`). `smoke_all.py --quick` 20/20.
- Not done:
  - Damage as a host option (the `damageExec` damage argument is the hook for it).
  - Stomp/pound detection against a player riding Yoshi uses the normal body size.
  - The victim does not send a confirmation back; the attacker's bounce is always local, as in sm64coopdx.

## Handoff (P1b, shared objectives follow-ups; stopped for usage limit)
- **Done:** `port/src/port_mp_objectives_extra.cpp` and `_ui.cpp`, test script `port/tools/mp_objx_test.py`, hooks under `SMS_PORT_AURORA` in `Item.cpp`, `ItemManager.cpp`, `MapObjTown.cpp`, `MarDirectorDirect.cpp` and `EventWatcher.cpp`. App channel 19 (`PORT_MP_CH_OBJX`), host option `reserved[7]` (remote-Shine banner); renumbered from 16 / [5] by w15 when merging (both were taken by w6's goop sync).
  - Features: red-coin switch press and start shared (the remote instruction talk is auto-closed, late joiners' timer is moved on); run-time coins credit everyone's total, so the 100-coin Shine appears for all; remote "Shine Get" banner option.
  - `mp_objx_test.py`: redcoin, latejoin, coins100 and blue PASS. In loadtime, load-time coin arbitration is implemented but not exercised: no test stage found.
- P2 fix: a hit on a Mario in an emote (NOMOTION) forces WAIT first (`port_mp_interact.cpp`).
- GameCube `mario.dol: OK`. Not run since these changes: `smoke_all --quick`.
- **Not written yet:** the MULTIPLAYER.md section (channel 19, `reserved[7]`, P1b owner, design notes). The header comment of `port_mp_objectives_extra.cpp` holds the design.
- **Found, not mine:** a warp to plaza scenario 3 crashes in `TGuide::load` (`J2DTextBox::setFont` on a null pane `a_tx`), single-player too. One guest instance died once inside Dawn Vulkan.
- **Playtest list (user):** a real red-coin challenge with 2 players (talk, timer sync, failure when time runs out on both); the 100-coin Shine with real coin pickups; the look of the remote-Shine banner.

# Multiplayer chat and emotes (w6, task board P3)
Design, controls, wire format and knobs: `port/MULTIPLAYER.md` "Chat and emotes (P3, w6)". New files only: `port/src/port_mp_chat.cpp`, `port/src/port_mp_emote.cpp`, `port/tools/mp_chat_test.py`; no edits to w10/w15 files, no `port_mp.h` or protocol change.

## MP-C1. Chat
- T / `/` / right-stick click / lobby "Chat" button; input line or on-screen keyboard; reliable `PORT_MP_CH_CHAT`; fading log bottom-left of the picture (above the plaza news ticker); joins/leaves as system lines; 120-byte printable UTF-8 cap; 5 per 10 s sender limit, 8 per 10 s receiver limit; mute per player; host "Chat"/"Emotes" switches in `reserved[1]`; `/me`, `/help`, `/emotes`, `/<emote>`.
- Pads idle while typing through `port_mp_request_input`; the request is reference-counted with no reset at session end, so a watchdog thread releases ours if the session ends with the box open (otherwise single player would be left with idle pads).
- Found while testing: the lobby section's hint line made w10's panel scroll sideways (left edge cut off); now wrapped.

## MP-C2. Emotes
- Seven emotes from Mario's own animation table: Shine pose, Phew (hot-weather wait), Aww (missed-Shine slump), Sleep (the game's `MARIO_STATUS_SLEEPY` chain), Pianta pose (`changeMontemanWaitingAnim`), Yawn, Sit. D-pad = four quick ones, G / left-stick click = menu, chat commands. Rejected after looking: `ANIM_DEMO_GATE_OUT_APPEAR_GET` (Mario scaled to nothing). Mario's table has no wave or Pianta-festival dance.
- Driven through `MARIO_STATUS_NOMOTION` + `TMario::setAnimation` each frame, so w15's capture sends the body BCK/frame/hand as for any game animation: the host sees the guest's puppet in the Shine pose, hands on hips and sitting (`emote_showcase/puppet_emotes_host_view.png`, a strip of the host's presented frames). Local strip: `emote_showcase/local_emotes_sheet.png`.
- Pitfalls found: (1) the pad's `mTrigger` bits are already consumed when registered game-frame callbacks run (two app frames per logic frame), so D-pad and cancel buttons use edges of the held bits; (2) NOMOTION has `MARIO_STATUS_FLAG_UNK1000`, which refuses unforced `changePlayerStatus` (the first build left Mario stuck in NOMOTION): leaving uses force, and since damage knockback is unforced, a hit, the floor moving, a talk/demo, the stick or any action button ends the emote at once; (3) an idle Mario dozes off by himself, so emotes also start from SLEEPY/SLEEP (killing the zzz effect as `wakeup()` does).
- Only from a plain idle stance; refusals are logged and shown as a system line.

## MP-C3. Tests
- `python port/tools/mp_chat_test.py -n 3`: host + 2 guests, 1280x960 off-screen, present captures. Checks: every instance got every other's chat line and emote line, the flood was cut (5 of 6 lines; "Slow down"), "chat off" refused later messages, each instance played its emotes (keyboard-pad D-pad on the host, `/phew`, the emote knob), no crash. ALL PASSED (106 s). Shots: input line (`mp_chat_test/0/present_1800.png`), on-screen keyboard (`1/present_1800.png`), emote menu (`2/present_1800.png`), lobby section (`0/present_3300.png`).
- `mp_test.py -n 2` (w10's checks) passes with these files linked. Final reruns of both silent and hidden (`SMS_PORT_AUDIO=0`, `SMS_PORT_OFFSCREEN=1`; `mp_chat_test.py` sets both itself): ALL PASSED.
- Single player unaffected (the callbacks are registered but only called during a session): `smoke_all.py --quick --no-build` 20/20 PASS; GameCube DOL shasum OK (no shared source touched).

## MP-C4. Speech bubbles hook (added by w15, for w6)
- `port_mp_chat.cpp` keeps each slot's latest chat line or emote (`Said sSaid[]`, under `sMu`, set by `noteSaid` in `sendChat` (own lines), `receive` (chat and emotes) and `port_mp_chat_note_emote`; cleared in `resetSession`) and exports `port_mp_chat_last_said(slot, text, cap, &emote, &atMs)`. `port_mp_emote.cpp`'s `announce` calls `port_mp_chat_note_emote(id)` for the local player's own emote. All lines marked `w15`. w15's `port_mp_overlay.cpp` draws the bubbles (see "Multiplayer puppets (w15)", P9); the chat log itself is unchanged.

# Multiplayer puppets (w15)
Remote players drawn in game: snapshot capture of the local Mario, puppet Marios posed from snapshots, name tags, and a ghost harness that needs no network. Design and the w10/w15 contract: `port/MULTIPLAYER.md` (section "Puppets (w15)"). Files: `port/src/port_mp_puppet.{h,cpp}`, `port/src/port_mp_overlay.cpp`.

## P1. Capture (read-only)
- `port_mp_capture()` fills a `PortMpPuppetState` from `gpMarioOriginal`: position, velocity, face angles, status/arg/flags, the body base matrix (`calcBaseMtx` result, so swim bobbing, slope tilt, surf roll and poles come along), and the animation exactly as `M3UModelMario` holds it: both slots' BCK indices (new/old), blend-calc index, the three frame controls, both motion blend ratios (run/walk mix), the eye BTP, the hand model in use, plus the head/waist rotations `MarioHeadCtrl`/`MarioWaistCtrl` would add (read-only replicas of their choice). FLUDD: nozzle, pressure, water, body/nozzle BCK + frame, first emitter position/direction. Yoshi: state, colour, BCK + frame. Cap/helmet/glasses/shirt flags, held object type, health.
- Hook: `port_mp_logic_frame()` in `TMarDirector::direct` (last simulation tick, before the entry list, `SMS_PORT`), once per entered logic frame (30 Hz). With multiplayer and ghosts off it only counts the frame.

## P2. Puppets
- One per remote player, built like `TMario::initModel` + `TEnemyMario::initModel`: its own `J3DModelData` of `ma_mdl1.bmd` (so hand/shirt shape flags and eye patterns are its own), the hand models, cap (`ma_cap1`/`ma_cap3`), an `M3UModelMario` with its own blend calcs and frame controls that shares the local Mario's BCK and BTP tables, and the FLUDD body/nozzle MActors; diver helmet, sunglasses and Yoshi (`yoshi_model.bmd` + hands, body colour from `TYoshi`'s table, base matrix = Mario's as in `TYoshi::calcAnim`) are loaded on first use. About 0x3AB00 bytes of the stage heap per puppet (`TApplication::mHeap`, freed with the stage; not built when it has under 0x180000 free).
- Posed by writing the snapshot into the `M3UModelMario` (slot BCKs via `changeMtxCalcSIAnmBQAnmTransform`, frame controls at rate 0, blend ratios) and the base matrix, then `perform(CUE_CALC_ANIM)`; head/waist through the puppet model data's own joint callbacks.
- Drawn by a view object pushed into the entry list after "繧ｳ繝ｳ繝繧ｯ繧ｿ繝ｼ" (`MarDirectorPreEntry.cpp`, only when multiplayer or a ghost is on), i.e. the character draw buffers, lit like an MActor (light set 1, light id from the ground's shadow data / shadow cube). Calc + entry on logic frames; w9's interpolated redraws only repeat `CUE_CALC_VIEW` (matrices interpolated by `port_interp`). Circle shadow through `gpBindShadowManager->request`. Puppets farther than 9000 units from the camera are skipped.
- FLUDD spray: a stand-in (no `TModelWaterManager` water, which would hit enemies, clean goop and count as the local player's): droplets simulated per logic frame from the snapshot's emitter, drawn as soft camera-facing blobs from a J3D packet in the translucent buffer.
- Interpolation: per slot a buffer of snapshots, drawn 100 ms behind the newest (network: receive time on `port_mp_time_ms`; ghosts: the local logic clock). Position/matrix (quaternion slerp), frames (same clip, small step), blend ratios, head/waist angles are blended; teleports (a jump over 600 units in one logic frame -> `warpSeq`), stage changes (`stageSeq`) and gaps over 500 ms snap. Hidden when not in our stage+scenario, not visible (pipes, warps), or 3 s without data.
- Never touches game state: no hit actor, no collision, not in any manager, no sound, no rumble, no water, no pollution, no EFB readbacks. With nothing active the entry list is unchanged.

## P3. Name tags
- Head joint + 75 units projected with the current GX projection and viewport, drawn by Dear ImGui (`port_mp_overlay.cpp`, called from `port_window.cpp` before `port_menu_draw`) in the game picture's box (w6's display aspect), fading from 2500 to 5000 units, hidden behind the camera and 0.5 s after the last game frame (movies, loads).

## P4. Ghost harness (no network)
- `SMS_PORT_MP_GHOST=<file>`: records the local `PortMpPuppetState` every logic frame (`SMSGHST1`, u32 record size, records).
- `SMS_PORT_MP_GHOST_PLAY=<file>`: plays it back as puppet(s) when the local player gets control in the same stage+scenario, frame-locked to the local logic clock. `SMS_PORT_MP_GHOST_COPIES=n` (several puppets), `SMS_PORT_MP_GHOST_OFFSET=x,y,z` (per-copy offset, default 150,0,0), `SMS_PORT_MP_GHOST_WIRE=1` (every state goes through the 128-byte network form first, i.e. exactly what a remote machine rebuilds). `SMS_PORT_MP_PUPPETS=1` turns the view object on without a ghost.
- Debug only: `SMS_PORT_MP_DEBUG_YOSHI=<logic frame>` drops the local Mario onto the hatched Yoshi once per stage (with `SMS_PORT_DEBUG_CALL=hatch@...`), to record a ride.
- Checked (off-screen, `SMS_PORT_SHOT_PRESENT`): plaza (warp 1,0) and Bianco (2,0, two copies) running, jumping, flipping, hovering and spraying beside the local Mario with the same pad script; Ricco (3,7) riding a pink Yoshi; all through the wire form. Shots in `port/build_aurora/mp_shots/` (w15 worker).

## P5. Network (w10's session)
- While `port_mp_active()`: the local state is packed into w10's `PortMpSnapshot` (`port_mp_pack_wire`: w10's standard fields plus the w15 block in `animAux`, `nozzleAngle`, `pad0`, `extra[44]`; layout in MULTIPLAYER.md) and handed to `port_mp_set_local_snapshot` every logic frame (VALID clear outside a stage). Remote slots are pulled with `port_mp_get_player` / `port_mp_get_snapshots` (new ticks only, receive time for interpolation); a changed `serial` respawns the puppet; names come from the player table.
- w10's functions are bound with lld-link `/alternatename` fallbacks (no session), so this builds and the ghost harness runs with or without `port_mp_net.cpp` linked.
- Checked with w10's worker code linked into a test build (not merged by me): host + guest on loopback (`SMS_PORT_MP=host/join`, off-screen, isolated user dirs, `SMS_PORT_NO_PADS`), the guest walking away, jumping and spraying in front of the host's camera and the host standing in the guest's view, each with its name tag (`net_*.png`); w10's `mp_test.py -n 2 --no-testsnap` passes with the real capture.
- GameCube build after the shared edits: `build/GMSE01/mario.dol: OK`. Single-player run (warp 1,0, no multiplayer env): no `[mp]` output, no view object.

## P6. Colour palettes (`port_mp_palette.cpp`; groundwork for character customisation)
- Where Mario's colours live (`SMS_PORT_MP_DUMPMAT=1` prints every material's TEV setup and writes the textures as `tex<n>.bin` in the run folder): all 11 body materials run the same 5 TEV stages (pollution mask, `H_ma_new_main_s3tc`, dirt dummy, toon ramp `H_toon_2_4i`, lit colour); their TEV/konst colours are lighting and dirt parameters, not part colours. Cap, shirt, overalls, gloves, shoes, skin, hair and moustache are painted in one 256x256 CMPR atlas, `H_ma_new_main_s3tc`, which the hand and cap models copy too. So colours have to change in the texture; a TEV multiply per material would tint the whole body (one material spans several parts).
- Recolour: each 4x4 CMPR block's two RGB565 endpoints are classified by atlas region (overalls/shirt top left, hair+ear top right, nose+moustache, shoes, sleeve+arm, gloves, cap+M bottom row) and hue/saturation/value, then replaced by the palette colour scaled by the endpoint's brightness relative to the brightest endpoint of that part (keeps the painted shading). Blocks keep their CMPR mode: endpoints are swapped and the 2-bit indices remapped when the new order would flip it. The buttons, eyes, logo patch and anything unclassified stay as they are.
- Palette = w10's `PortMpCharacter` (synced in the player table): `colors[0..5]` = cap, shirt, overalls, gloves, shoes, skin; hair = the 3 bytes after them (`reserved[0..2]`, i.e. `colors[6]` if the struct grows to 7). (0,0,0) keeps a part; all zero = classic. 11 presets (`port_mp_palette_preset*`): Classic, Green (Luigi), Yellow (Wario), Purple (Waluigi), Fire, Shadow, Pink, Aqua, Orange, Gold, Black & white.
- Puppets: their body, hand and cap model data get their own copies of all texture headers (`portMpPaletteOwnHeaders`; before this the puppets' hands/caps wrote the same values into the file's shared headers, harmless only while nothing was recoloured) and a recoloured copy of the atlas (32 KB) in the stage heap, made before the display lists. A colour change recolours the puppet's own atlas copy in place (same cache drop), no rebuild.
- Local Mario: only if the player picked a palette (`port_mp_palette_set_local`, the character menu, or `SMS_PORT_PALETTE=<preset>`): mario.arc's atlas is recoloured in place from a pristine copy on the next logic frame after the palette changes, then Aurora's texture-object cache is dropped (`aurora::gx::clear_static_texture_cache`), so the locked display lists rebind it: the change shows at once, mid-stage too (P8). Classic = the GameCube data, untouched. Shadow Mario and mirror reflections share the local Mario's model data, so they follow the local palette.
- Ghost test knob: `SMS_PORT_MP_GHOST_PALETTE=<preset>[,<preset>...]` per ghost copy.
- In a session, a palette set through `port_mp_palette_set_local` / `SMS_PORT_PALETTE` is published with `port_mp_set_character` (w10's PLAYER_INFO), so the other players' puppets wear it; checked host Green + guest Yellow on loopback with main's netcode (`mp_shots/net_palette_*.png`). The character page (P4) should call `port_mp_palette_set_local` (presets: `port_mp_palette_preset_count/name/preset`).
- Checked (plaza, 3 ghosts through the wire form + local palette): Green, Yellow and Shadow ghosts beside a Fire-palette local Mario (`mp_shots/palette_plaza_1500.png`). Single-player run without the knobs: no `[mp]` output, data untouched.

## P7. Held objects on puppets
- The held object's actor type goes on the wire (puppet block bytes 41-43: low 16 bits + category bit index; the emitter offset at 38-40 is now s8 in 2-unit steps to make room). The receiver looks for a local actor of that type (conductor managers and alone actors), makes its own `J3DModel` of that actor's model data (once per type, up to 4 per puppet and stage) and draws it on the puppet's right hand (`TMario::getTakingMtx`) at the actor's scale. Visual only; the local copy of the object is not touched.
- Checked with a ghost whose recording was given a papaya (`ghost_held.py`), Ricco 3,7: the papaya copy in the puppet's hand (`mp_shots/held_papaya_1000.png`).

## P8. Character menu (task board P4, `port_mp_custom.cpp`)
- One ImGui editor, three ways in: **F4** anywhere (single-player too: file select or a stage), the lobby panel's **Character** section (`port_mp_register_lobby_section`, with a swatch of the applied palette), and `port_mp_custom_draw()` for w10's main-page "Character" stub (one call on w10's side, requested in MULTIPLAYER.md P4). No w10 file edited.
- Contents: the 11 presets and the saved palettes in one list, a front-view swatch diagram of Mario in the edited colours, one colour editor per part (cap, shirt, overalls, gloves, shoes, skin, hair and moustache; untick = keep the original), name + Save as / Delete, Apply / Revert / Close. Keyboard and controller navigation (ImGui nav on while open, like w9's menu; B/Esc closes); the game gets idle pads (`port_mp_request_input`).
- Live preview: every edit recolours the real Mario in the game at once (`port_mp_palette_preview`); Close without Apply puts the applied palette back. No separate rotating render (the game camera shows Mario next to the window, which sits on the right).
- Apply: `port_mp_palette_set_local` (+ `port_mp_set_character` in a session, so the others' puppets change, in place, within a few frames) and saves `port_characters.ini` (next to `port_settings.ini`, or `SMS_PORT_CHAR_INI`): `current=` plus `saved=<hex>:<name>` lines, 7 parts x RRGGBB, `000000` = original. The applied palette is loaded at start (`SMS_PORT_PALETTE` wins). Default (no file): Classic, nothing touched.
- Test knobs: `SMS_PORT_CHAR_OPEN=<frame>` opens the window; `SMS_PORT_CHAR_SCRIPT=<frame>:<preset>[,...]` picks and applies presets; F4 also via `SMS_PORT_KEYSCRIPT`.
- Checked (`SMS_PORT_SHOT_PRESENT`): single-player plaza, window open with Green previewed on Mario, then Yellow applied and the window closed, Mario yellow mid-stage (`mp_shots/char_menu_green_1500.png`, `char_applied_yellow_1720.png`); next run loads Yellow from the file. 2 instances on loopback: the guest applies Green then Purple, the host sees its puppet go red -> green -> purple without a rebuild (`mp_shots/net_char_*.png`); lobby panel section (`mp_shots/lobby_character_section.png`). Single-player without knobs: no `[mp]` output, no file written.

## P9. Chat speech bubbles (`port_mp_overlay.cpp`)
- `port_mp_overlay_get_tag(slot, &x, &y, &visible)` (port_mp_puppet.h): where a player's name tag is this frame, in window pixels; in a session the local Mario has an anchor too (no tag drawn for him). Tags now carry the player slot.
- Each player's latest chat line or emote (w6's `port_mp_chat_last_said`, MP-C4) shows as a white bubble above his name tag (above the head for our own lines): 5 s, fading over the last second, wrapped at 30% of the picture width, kept inside the picture with the tail still pointing at the player, fainter for far players; emotes get a gold star and the emote's name. The bottom-left chat log is untouched. Bound with an `/alternatename` fallback, so it links without w6's file.
- Checked silent and hidden (`SMS_PORT_AUDIO=0`, `SMS_PORT_OFFSCREEN=1`), host + guest on loopback with `SMS_PORT_MP_CHAT_AUTO` / `SMS_PORT_MP_EMOTE_AUTO`: the host's own bubble over his Mario (`mp_shots/bubble_host_own_1320.png`), both players' bubbles on the host (`bubble_host_both_1520.png`), the guest's view (`bubble_guest_1320.png`) and the guest's Shine-pose emote bubble with the star (`bubble_guest_emote_1520.png`).

## P10. FLUDD water sync: real water particles (`port_mp_water.cpp`)
- Sender: `gPortWaterEmitHook` (ModelWaterManager.cpp `emitRequest`, `SMS_PORT`) sees every emit request.
  The local Mario's own (TWaterGun::mEmitInfo for FLUDD and Yoshi juice, the wet-Mario drips `unk158`, the water lost on damage `unk154`) are sent once per logic frame on channel 20 (`PORT_MP_CH_WATER`, unreliable, 64 bytes per emit).
  Each emit carries the count actually emitted, direction, power, size, tremble, particle type, lifetime and flag, a position relative to Mario, and the simulation tick it came from.
- Receiver: emits queue per player and play when the puppet's playback (100 ms behind) reaches their snapshot tick, at the puppet's position.
  `TModelWaterManager::portEmitRemote` builds makeEmit's particles in the local manager with `PORT_REMOTE_FLAG` (0x8000), so streams, drops, hover/rocket/turbo jets, splashes, puddles and wall runs look exactly like the local water.
- Visual only:
  - `move()` runs the remote particles in a pass of their own after the local ones, so the local ones see the same carried-over `local_248`/`f31`. Puddles merge only with their own kind.
  - The remote pass sends no hit messages (ground, wall or roof actors), cleans no pollution and starts no sounds. Its random numbers come from `portRand()` (xorshift), never `rand()`.
  - Remote splashes leave at least 16 of the splash pool's 64 entries free.
  - `TObjHitCheck::checkWater`, `askHitWaterParticleOnGround` (the local Mario slipping on puddles) and w8's FLUDD-knockback scan (`port_mp_interact.cpp`, marked w15) skip remote particles.
- Budget: on the PC the SOA arrays are `MODEL_WATER_ARRAY_NUM` = 512, which adds 38 KB to the stage heap (about 10.9 MB was free in the plaza).
  `emitRequest` counts only the local particles against `unk10` (256), so the local water never runs short. Remote particles use the other 256 (`PORT_REMOTE_MAX`).
  The water manager drops anything beyond 5000 units of the local Mario, so a puppet that far away emits nothing.
  Measured while spraying: the local water peaks at about 90 particles, and remote water stays in the same range per player.
- Frame rate (w9's `port_interp_interpolating()` / `port_interp_alpha()` from main; `/alternatename` fallbacks in `port_mp_water.cpp`):
  - `portInterpCalcView` keeps each particle's position from the last two logic view passes. GC compaction carries them along.
  - The interpolated pass of an extra display frame builds the quads and puddle matrices from the blended positions, then restores the live ones.
  - All water now moves smoothly at high frame rates, local included. Puppets already were smooth: they are J3D models viewCalc'd in the entry pass.
- Ghost harness: `SMS_PORT_MP_GHOST` also writes `<file>.wtr` and `_PLAY` replays it. `SMS_PORT_MP_WATER_LOG=1` logs particle counts and where the local and remote water sits.
- The soft spray stand-in (`stepSpray`, `TPuppetSprayPacket`) is removed.
- Checks (silent, hidden; GC `shasum -c` OK):
  - Plaza ghost side by side (`mp_shots/water_ghost_side_1750.png`); the hover jets also matched in the ghost run.
  - Record-then-replay of a standing spray on the same spot: comparable counts and landing distance (local 470-590, remote 550-630 units ahead).
  - Three instances on loopback, the host spraying, Luigi spraying, Wario hovering (`water_net3_host_1140.png`: Wario's remote hover jets; `water_net3_wario_1060.png` / `water_net3_luigi_1060.png`: the host's remote puddles).
- Channel note: w6's goop stream and w8's `port_mp_objectives_extra.cpp` both use channel 16.

## P11. Playtest desyncs in the free-mode plaza (fruit, birds, grates, red X, nozzle boxes)
- World events (w10's `port_mp_events.cpp`) are now on by default: `reserved[6]` 0 = on, 1 = off (the lobby checkbox and `SMS_PORT_MP_EVENTS` follow). The plaza's 4 nozzle boxes were already covered; the user's session had the option off.
- New event kinds: `EV_MANHOLE` (7), the plaza's 13 grates opened by a hip drop or from below (hooks in `TManhole::touchPlayer`, replay `port_mp_manhole_open` in `MapObjTown.cpp`: the opening animation and sound); `EV_PICTURE` (8), water-hit pictures sprayed clear (the plaza's red X graffiti and the posters are `TWaterHitPictureHideObj`, not goop: the plaza has no pollution layers; hook in `forward()`, replay `port_mp_picture_clear` in `MapObjHide.cpp`, which runs `afterFinishedAnim`, so its coin appears for everyone).
- Goop sync (w6) default changed from "Auto (only with enemy sync)" to On (`reserved[5]` 0 = on, 2 = off).
- New `port_mp_ambient.cpp` on `PORT_MP_CH_AMBIENT` (22, unreliable): fruit and animals. The stage leader (lowest slot in the stage) sends animals (birds, butterflies: position, yaw) and loose fruit (position, velocity, "waiting to appear" after Yoshi ate it or it rotted) every 3 logic frames when they move, a keyframe each second; the others steer their copies. A player whose Mario holds a fruit, or threw it less than 2 s ago, claims it and sends it instead; on the other screens that fruit is hidden while his puppet carries its copy and reappears where he throws it.
- Checked (2 instances, plaza 1/2, silent, hidden): world events on without any setting; host-triggered nozzle boxes and 4 grates applied on the guest; ambient: 7 animals and 36 fruit registered on both, the guest follows all 43. GameCube `shasum -c` OK, `smoke_all --quick` 20/20.
- Not covered: fruit a Pianta carries on one screen only (NPC sync, w12, owns NPCs); run-time spawned fruit. w11 owns the generic "sprayed object reacts" forwarding; `EV_PICTURE` only shares the finished state (a picture already cleared by forwarded hits ignores it).

## P12. Shine-get demo in a session; fruit carried by NPCs
- The Shine-get demo (`DEMO_FLAG_SHINE_GET_STOP_THE_WORLD`) no longer freezes the world in a session: `portShineDemoPerform` (`MarDirectorDirect.cpp`, `SMS_PORT_AURORA`) runs the Shine lists as before, then every entry of the full Movement / CalcAnim list that the Shine list does not hold (each performer once, so Mario, the Shine and the camera are not stepped twice). Outside a session only the Shine list runs, as on the GameCube. Checked with `mp_objx_test.py --only coins100` (the guest takes the 100-coin Shine): `[mp] shine demo: the world keeps running (17 more performers of "PerformList Movement")` on both, scenario PASS.
- Fruit carried by an NPC (`port_mp_ambient.cpp`): the authority flags it (`F_NPC`); where the same fruit sits in one of our NPCs' hands but not there, our NPC lets go of it (the game's own release, as rotting does: `HIT_MESSAGE_UNK8` to the holder); with NPC sync on (w12, "Sync enemies: Experimental") the fruit is put into the NPC puppet's hands at the owner's position, without it the fruit is hidden here while their NPC carries it.
- Not done: fruit spawned during play (not cheap: no scene id); the remote Yoshi juice colour (the water manager draws every particle in one colour, `unk5D5F`).

## Not done / next
- Held objects use the generic hand matrix (objects that position themselves differently in Mario's hands may look offset); no copy when no local actor of that type exists in the stage.
- Foot IK callbacks, the FLUDD/cap sway (`TMultiMtxEffect`), Yoshi's tongue and juice blink, dirt and damage fog are not reproduced.
- Palette: the classification is tuned on the US atlas only.
- Player interaction (coopdx-style bumping/stomping) is out of scope here (puppets have no collision).


# Multiplayer (w10)

sm64coopdx-style multiplayer: design, protocol and the w10/w15 interface are in `port/MULTIPLAYER.md`; the shared API is `port/include/port_mp.h`.

## MP1. Transport, sessions, LAN/direct join (`port_mp_net.cpp`)
- Own UDP layer over Winsock (`ws2_32`, linked with `#pragma comment(lib)`), one socket and one network thread per process, so keepalives and timeouts keep running while the game thread loads a stage.
- Host-authoritative: the host owns the player table and session settings and relays snapshots (full rate to players in the same stage+scenario, 2 Hz to the rest) and reliable messages.
- 16-byte header (magic `SMP1`, protocol version, type, slot, connection id, sequence, length); anything malformed, of another version, or from an address that does not own the connection id is dropped.
- Handshake `CONNECT`/`ACCEPT`/`REJECT` with a protocol version check (a mismatched client is told why), reliable channel (ids, cumulative + 32-bit selective acks, resend after max(100 ms, 1.5 RTT), in-order delivery), ping/pong RTT, 10 s timeout, polite `DISCONNECT`.
- LAN discovery: broadcast plus loopback `DISCOVER` on ports 7777-7792; direct join by host name or IP and port. No public server is contacted (open question in MULTIPLAYER.md).

## MP2. Menus and entry (`port_mp_menu.cpp`, hooks in `port_window.cpp`, `port_input.cpp`, `CardLoad.cpp`)
- File select: Mario's left boundary on the save-block screen is x = 846 (measured with `SMS_PORT_TRACE`); running into it (x < 870, stick left, 15 game frames) while `TCardLoad` is in state 0 with the pad out of menu mode opens the multiplayer menu. A "<< Multiplayer" hint is drawn at the left edge there. Nothing else in the file select changes.
- ImGui pages: username (+ Random, for controller users), Create lobby (name, max players, game mode list = Free, shared objectives, port), Join (LAN list, auto-refresh every 3 s, direct IP:port), Character (stub), lobby panel (players, stage, ping, game mode + shared objectives for the host, Leave). Controller navigation like w9's menu; the game gets idle pads while a page is open. F2 opens the lobby panel in a session (main page on the file select); Z in the pause menu also opens it.
- Preferences: `port_multiplayer.ini` next to `port_settings.ini` (`SMS_PORT_MP_INI` overrides).

## MP3. Free mode on an in-memory save (`port_mp_game.cpp`, hooks in `Application.cpp`, `CardManager.cpp`)
- Start: `TFlagManager::firstStart()`, w13's `story2` preset (now reachable as `port_apply_flag_items()` in `Application.cpp`), `saveSuccess()` (so a game over restores the same state), then `setNextStage((1+1)<<8 | 2)` from the file select (or `mNextArea` for the debug auto-start). Plaza scenario 2 (post-game): 104 shines, statue clean, every course entrance open.
- `TCardManager::writeBlock`/`writeOptionBlock`/`createFile`/`format` answer READY without touching the card while the multiplayer save is in memory. Verified: a full session leaves the scratch card folder empty.
- Leaving (menu, host closed, timeout): back to area 15 (`setNextStage(15)`, the title of the file-select stage); the card is the player's again from there.
- Checked GameCube build after the shared edits: `build/GMSE01/mario.dol: OK`.

## MP4. Tests (`port/tools/mp_test.py`)
- `python port/tools/mp_test.py -n 3 --seconds 100 --kill-at 65 --walk`: host + 2 guests on loopback, off-screen, isolated user dirs; all reach the plaza (1/2), ~30 snapshots/s flow both ways (with `SMS_PORT_MP_TESTSNAP=1`), the killed guest is dropped after the 10 s timeout and the other guest is told. ALL PASSED.
- File-select path: `SMS_PORT_PADSCRIPT=650-1700/60:START,1800-2300/60:A,2600-3100:S270` + `SMS_PORT_MP_MENU_AUTO=host:<port>`: Mario runs left, the menu opens (frame ~2650), the lobby is created and the plaza loads.
- Leave path: `SMS_PORT_MP_LEAVE_AT=<frame>` on the host: both instances return to the file-select stage.
- Windows Firewall: no prompt or block seen for loopback; LAN play may prompt for `sms_port.exe` the first time a lobby is created.

## MP5. Shared objectives (stage 4; `port_mp_game.cpp`, `port_mp_net.cpp`, hooks in `Item.cpp`, `ItemManager.cpp`, `MarDirectorDirect.cpp`)
- Host setting "Shared objectives" (default off), synced; latched per stage visit. Design and rules in MULTIPLAYER.md.
- Ids: scene-placed yellow/red coins by index in `gpItemManager` (end of the scene's items reported by `TItemManager::loadAfter` before its coin pool), blue coins by (area, event id), Shines by shine id. Host-authoritative first-claim; apply path credits the count, sends the container message, then `kill()`; no reward/sound for the local Mario; pending removal for red coins not out yet; 100-coin Shine as in `TCoin::taken`.
- Checked: host `SMS_PORT_MP_SHARED=1 SMS_PORT_MP_DEBUG_COLLECT=2000,2300,2600,2900:coin`: the guest logs objectives 20204000-20204003 applied and its coin count goes 1, 2, 3, 4 (`SMS_PORT_TRACE=act`); `:blue`: blue coin 603fe00c applied, guest blue count 1.
- GameCube build after these hooks: `build/GMSE01/mario.dol: OK`.

## MP6. Robustness and scale checks
- `mp_test.py -n 2 --fuzz`: 3000 malformed datagrams at a running host (random bytes, bad lengths, truncated headers, unknown connection ids, short CONNECTs, wrong magic): no crash, no fake player (CONNECT bodies now start with a `JOIN` tag; names are cleaned to printable ASCII / well-formed UTF-8), then a protocol-99 CONNECT gets `REJECT` reason 1 and a `DISCOVER` is still answered. ALL PASSED.
- `mp_test.py -n 8 --nullgfx --kill-at 100 --walk`: 8 players (host + 7) in the plaza, every instance gets ~30 snapshots/s from every other one, the killed guest is dropped after 10 s and the 6 others are told. ALL PASSED.
- `SMS_PORT_MP_DROP=30` (new debug knob: drop 30% of sent and received datagrams) on both ends, shared coins: the handshake still completes and all 6 collected coins reach the guest (host resent 20 reliable messages).

## MP7. Free-mode plaza with w15's puppets (stage 3)
- w15's `port_mp_puppet.{h,cpp}`, `port_mp_overlay.cpp` and its two hooks (`MarDirectorDirect.cpp` `port_mp_logic_frame`, `MarDirectorPreEntry.cpp` view object, plus `port_mp_overlay_draw` in `port_window.cpp`) linked into a w10 test build (then removed again, they are w15's to merge): `mp_test.py -n 3 --walk --no-testsnap --shot 1500,1900,2300,2700 --present` passes with the real capture; the host sees Guest2 by the statue with its name tag, Guest2 (swimming) sees the host's puppet ("Host") on the plaza (`port/build_aurora/mp_shots/stage3_*.png`, w10 worker).
- `SMS_PORT_MP_TESTSNAP` now yields to the real capture (ignored for 2 s after any `port_mp_set_local_snapshot`).

## MP8. Extension points for the multiplayer team; worker rebased on main
- App messages: `port_mp_send(channel, data, len, reliable, to)` / `port_mp_recv` (`PORT_MP_CH_*` in `port_mp.h`), unreliable packet `APP` and reliable kind `APP`, relayed by the host (to everyone or one slot). Checked: `mp_test.py -n 3` with `SMS_PORT_MP_TESTMSG=1` (each instance broadcasts a reliable test message every 2 s): every instance logs messages from both others.
- Feature hooks: `port_mp_register_game_frame`, `port_mp_register_overlay`, `port_mp_register_lobby_section`, `port_mp_request_input`, so the task-board packages (MULTIPLAYER.md "Task board") live in their own files.
- File-select entry now counts presented frames (`TCardLoad::perform` can see `CUE_MOVE` more than once per game frame; smoke_all's newgame script, which taps left for 8 frames, had opened the menu). Now 30 presented frames (0.5 s) of running into the left boundary; the newgame scene no longer opens it.
- Worker rebased on main (w15's puppets, w9's controller work): main's files taken, my hooks re-applied (`MarDirectorDirect.cpp` = main + `port_mp_shine_got`; `port_window.cpp` has exactly one `port_mp_overlay_draw()` (w15's) and my `port_mp_menu_draw()`), base re-recorded. With w15's capture in, `mp_test.py -n 3 --walk --kill-at 80 --fuzz` passes on the merged build; `SMS_PORT_MP_TESTSNAP` is now opt-in (`--testsnap`) and yields to the real capture anyway.
- `mp_test.py` options: `-n`, `--seconds`, `--port`, `--kill-at`, `--walk`, `--shot/--present`, `--fuzz`, `--nullgfx`, `--testsnap`.

## MP9. Join by lobby name: lobby / rendezvous / relay server (`port/server`), client, menus
- `port/server/sms_lobby_server.py`: one-file Python 3 server (stdlib only, Linux-friendly, no disk writes): unique case-insensitive lobby names with heartbeats (15 s expiry, freed at once on UNREGISTER), name lookup, rendezvous (FOUND to the client with the host's observed and LAN endpoints, PUNCH to the host with the client's), relay sessions (30 s idle expiry), limits (1450-byte datagrams, 20 control packets/s per IP, 3 lobbies per IP, 500 lobbies, 200 relays, 256 KiB/s per relay direction), server protocol version check, game protocol check on LOOKUP, logs with hashed address tags only. Protocol table in MULTIPLAYER.md.
- Deployment kit: `port/server/DEPLOY.md` (Oracle Cloud Always Free, Google Cloud e2-micro: VM, security list / VPC firewall rule for UDP 7700, copy + install), `install.sh` (one command: python3, user, /opt/sms-lobby, systemd unit, the VM's own firewall incl. Oracle's iptables), `sms-lobby.service` (hardened unit). Nothing is deployed or contacted; `sh -n install.sh` OK.
- Client (`port_mp_net.cpp`): `port_mp_host2(..., PORT_MP_CONN_SERVER, password)` registers from the game socket (heartbeat 2 s, re-register when the server forgets, LAN-only fallback message when unreachable, "name taken" ends the session with a message); `port_mp_join_name` does LOOKUP + LAN DISCOVER by name at once, CONNECT to every candidate (host public + LAN endpoints, LAN replies), relay after 2.5 s (or `SMS_PORT_MP_FORCE_RELAY=1`) through virtual peer addresses 240.0.0.n; the host follows whichever path the client settles on. Password hash in CONNECT (host checks too). Server status ping (online / offline / not set up), observed public address. Protocol 3.
- Menus: connection type (Server (lobby name) default / Direct IP) on Create and Join; server mode shows no IPs/ports; server status line; "Lobby server address" editor; lobby panel join info with Copy buttons (lobby name in server mode; LAN / virtual-LAN addresses with adapter names, and the Internet address only from our own server's observation, else "needs port forwarding of UDP <port>"); on-screen keyboard ("Keys" next to every text field; D-pad/A, X delete, B/Start done); toasts when a client is dropped (host left, connection lost).
- `PortMpCharacter.colors[7]` (hair = the old reserved[0..2]); one-line accessor change in w15's `port_mp_palette.cpp` (`slotRgb`).
- Tests, all PASS: `mp_server_test.py --phase unit` (12 checks on the server alone: duplicate name, same-host re-register, per-IP limit, versions, oversize drop, lookup normalisation, PUNCH, relay both ways and third-party ignore, name freed after UNREGISTER); `--phase 1` (H1 registers "Bob's Lobby", H2 "BOB'S LOBBY" gets "name taken", G1 joins "bob's lobby" directly, G2 joins through the forced relay, app messages flow host<->relayed guest<->direct guest, "No Such Lobby" refused, H1 killed -> server expires it, guests notice, no client addresses in the server log); `--phase 2` (no server: H3 LAN-only lobby with password, G4 joins by name over LAN, G5 wrong password refused, host leave reported); `--phase 3` (name reusable right after the host leaves). Clipboard: `SMS_PORT_MP_MENU_AUTO=copytest` copied "192.168.1.104:7781" (Direct IP) and "Delfino Fun" (server mode), read back equal, previous clipboard restored. Screenshots: `port/build_aurora/mp_shots/server_*.png` (w10 worker).
- Corona Mountain from plaza scenario 2: the plaza-2 scene has a `MapObjChangeStage` at (-2160, 2850, -12750) (the scenario-9 one at (-2142, 1850, -12859)); putting Mario on it (`SMS_PORT_DEBUG_CALL=tp...`, warp 1,2, story2 flags) loads stage 52 (Corona Mountain). The other plaza-2 change-stage objects lead to Pianta Village (8), Sirena (6), Pinna Park (5) and the secret courses 21-24 and 29.


# Multiplayer stage presence (w11, task board P5)
New file `port/src/port_mp_presence.cpp` only (plus the P5 row, `reserved[3]` and a "Stage presence (P5)" section in `port/MULTIPLAYER.md`); it plugs in through `port_mp.h`'s hooks and `PORT_MP_CH_WARP`, edits none of w10's or w15's files and does nothing outside a session. No shared game source changed (GameCube DOL unaffected).
- Lobby panel section: where everyone is (stage + episode, or file select / movie / menus / loading, plus paused / talking / cutscene), "Warp to" per player, "Return to Delfino Plaza", off-screen arrows on/off, host option "Level select for everyone", and the level select (plaza, main stages with episodes, park interior, casino, Noki undersea, bosses, 33 secret courses).
- Stage changes: `TApplication::mNextArea` + `DIRECTOR_FLAG_STAGE_TRANSITION_PENDING` (setNextStage's path without its plaza gate demos); a request waits for the end of a talk/cutscene and runs from the pause menu once it closes.
- Warp to player: same stage+scenario, then a reliable spawn request to that player, answered with its last grounded position; applied with `TMario::warpRequest`. The request waits until Mario is visible and on the ground: teleporting during his stage-entry warp-in (which hides him until it ends) left him invisible for the rest of the visit (found in the first test, fixed).
- Off-screen arrows for same-stage players (game camera projection, widescreen aspect included).
- Checked with `mp_test.py -n 3 --walk` and `SMS_PORT_MP_PRESENCE_TEST` (all mp_test checks pass): host level select to Bianco 1; Guest1 warps to the host (loads Bianco 1, spawn answer, lands next to the host); Guest2 goes to Ricco 1, then warps to Guest1 in Bianco (spawned at Guest1's position); host opens the panel (screenshot: everyone's stage/episode), switches the guests' level select off, Guest1's level select is refused with a toast; host returns to the plaza; Guest1 warps to the host there (reload to the plaza + spawn next to the host, puppets and name tags show both). Arrows show the other player's direction and distance in Bianco on all three. Presence changes (stage, loading) arrive on every instance. smoke_all --quick: 20/20 (single player unchanged).
- Found in passing (not mine): game frames can stall for several seconds while 3 instances share the GPU (the presence code only calls a 5 s gap "loading"); the US Gelato episode-2 opening talk needs A, so an unattended warp waits there.

## MP10. Sessions never end up on the title screen; in-memory card; reconnection (P6)
- **Bug (user):** beating the game in a session went to the title screen while still in the lobby. Now `port_mp_redirect_title()` (hook before every `TApplication` state change, `Application.cpp`, `SMS_PORT_AURORA`) sends a player heading for the title (DONE, TITLE/MENU, or area 15) back to the session's Delfino Plaza with a toast, after the ending has played. Checked: `SMS_PORT_MP_START_AREA=60,0` + `SMS_PORT_DEBUG_CALL=bathdemo@1500`: movie 14, epilogue and staff roll (three movie states), the save prompt, then `app state 6 -> 4 (area 15/0) would leave for the title: back to the plaza` and state 5 area 1/2, still hosting.
- **Found on the way:** the ending's (and any first) save prompt looped on "File created." in a session, because the old write-blocking answered `createFile` READY without a file existing. Replaced by an in-memory card (`port/src/port_mp_savecard.cpp`; CARD calls renamed in `CardManager.cpp`): one emulated 59-block card per session, so create/open/read/write/status work and the prompt completes. The real card is unmounted before and after; the scratch user dir still has no card file after the session.
- Game over in a session (`SMS_PORT_MP_FLAGS=20001=0`, Bowser's fire): death, save prompt (in memory), `restore()`, back to plaza 1/2 with 3 lives: no redirect needed. Reset combo in a session (`B+X+START`): "Left the lobby (reset)." and the title as usual. Safety net: reaching the title / file select in a session leaves the lobby with a message.
- **Reconnection:** 3 s of silence shows "reconnecting" (toasts, lobby panel), sessions survive 20 s (was 10), the host accepts a known connection id from a new address after a 2 s gap. `SMS_PORT_MP_BLACKOUT=40,8` on one guest: "Connection problem: reconnecting...", then "Reconnected to the lobby." / host "Shorty is back"; `40,26` on another: dropped after 20 s, "Connection to the host lost.", back to the file select. Host leaving cleanly already ends the session for everyone with "The host closed the lobby."; host migration not done (MULTIPLAYER.md P6).
- w11's notes: the lobby panel lost its "Where" column (w11's presence section shows it); `port_mp_menu_close()` closes the panel from a warp button. Ping column shows "reconnecting..." for a silent peer (`PortMpPlayerInfo.lagging`).
- w9's QoL gate verified in a session: with `fast_forward = 1` (Tab held by KEYSCRIPT) and `infinite_water = 1`, single player shows ">> 4x" and water stays 10000; in a session there is no fast-forward and water drops to 9381 while spraying.
- Tests now launch silent and hidden (`SMS_PORT_AUDIO=0`, `SMS_PORT_OFFSCREEN=1`) and pick random ports (20000-40000) so parallel runs by other agents never meet (one earlier run on port 7781 was joined by another agent's "Guest1").
- GameCube build after the `Application.cpp` / `CardManager.cpp` edits: `build/GMSE01/mario.dol: OK`.

# Multiplayer enemy / object sync (w11, task board P8): design + prototype
Design: `port/MULTIPLAYER.md` "Enemy and object sync (P8)" (authority = nearest player per object, chosen by the stage leader; what to sync in which order; ids; spawn/despawn/late join; bandwidth and interpolation; puppet mode; RNG/timing hazards; risks). Prototype: `port/src/port_mp_objects.cpp` + one hook in `src/Strategic/liveactor.cpp` (`TLiveActor::perform`, `SMS_PORT_AURORA` only: while any actor is a puppet, `port_mp_obj_puppet_move` sets its pose and its `moveObject` is skipped). Host setting "Sync enemies: Off (default) / Experimental" (`reserved[2]`).
- Synced: the Strollin' Stu family and moving map objects with collision (lifts, rail and rotating platforms). Owner sends pose + alive at 15 Hz (changes, 1 s keyframes); puppets interpolate 100 ms behind; owner deaths play the puppet's own death (`kill()`), owner respawns `reset()` it.
- Found on the way: `gpConductor` is never cleared, so in a stage without a conductor (the plaza) it points at the previous stage's freed one; reading it crashed the first test. The prototype checks the scene's name tree for the conductor first.
- Checked with `mp_test.py` (2 and 3 instances, Bianco Hills 1, `SMS_PORT_MP_SYNC_ENEMIES=1`, `SMS_PORT_MP_OBJ_LOG=1`, `SMS_PORT_MP_OBJ_TEST`), all mp_test checks pass, no crash: the leader assigns the Stus to the nearest player, the others show them as puppets at the owner's positions (log samples equal on both sides); `near` moves ownership to the player who walks up (Guest2 took over 93 objects); `kill` on the owner (host, then Guest2) shows "died on its owner" on every other instance, and respawns follow; when the host leaves for the plaza the next player becomes leader and runs everything; moving platforms in Ricco Harbor follow the owner's rotation on the others.
- Single player: GameCube `dtk shasum` OK; smoke_all --quick 20/20; a replay recorded with main's current exe (Bianco, keyboard-scripted movement, 1920 game frames) replays on this build with 0 of 15 checkpoints differing (actor and camera hashes included).
- Not done (next steps in the design): nerve/HP in the state and hit forwarding to the owner, more enemy classes, switches/Shine gates as events, goop cleaning stream, run-time spawned objects.


# Episode objectives (w12)
Stopped at the coordinator's refocus (NPC sync for multiplayer). Runs: real D3D12 or the Null backend (`--null`), `SMS_PORT_AUDIO=0`, `SMS_PORT_OFFSCREEN=1`, each run with its own `SMS_PORT_USER_DIR` (isolated card); the player's card was not touched.
Warp/WARP_FLAGS only set the start point; every objective below was driven with scripted input (`SMS_PORT_ROUTE`).

## New debug tools (all read-only or input-only, `SMS_PORT` only)
- **`SMS_PORT_ROUTE=<file>`** (`Application.cpp`, hook `port_route_pad()` in `port_PADRead`): state-driven input. One step per line, held until a condition holds: `frames N`, `ctl`, `talk`, `go X Z [R]` (autopilot), `go @<name>`, `near X Y Z R`, `stage A`, `shine`, `red/coins/blue N`, `land`, `air`, `act HEX`, `x< x> y< y> z< z> V`, `cond/ncond B`. Modifiers: `+BTN` held, `^BTN/P` tapped, `%BTN/P` held with a 4-frame release every P (FLUDD pumping), `S<deg>`, `T<x>;<z>` (stick toward a world point), `Q<n>` (stick strength; below ~50 the game's dead zone leaves Mario creeping), `thru` (steer through a target, for fast vehicles), `max N` (timeout), `stop`. Logs `[route] f<frame> step <n> ... done|TIMEOUT`.
- **`SMS_PORT_DEBUG_CALL`**: several calls separated by `|` (up to 512), plus `hmap<step>[;<y>[;x0;z0;x1;z1]]` (writes a heightmap: ground height, BG type and a wall-within-50 flag per cell, `hmap_<area>_<sc>_<y>_f<frame>.bin`) and `npcs` (lists the talk candidates `findNearestTalkNPC` sees, with live flags, and the actors Mario is touching).
- **`SMS_PORT_TRACE=obj`**: one line per actor loaded (`TLiveActor::load`, `TSpineEnemy::load`, `TMapObjBase::load`): kind, model, position, SJIS name in hex. `SMS_PORT_TRACE=pos`: Mario's trace line every game frame, with face and intended yaw.
- Scratch tools (not in the repo): heightmap renderer with objects/trail, A* walking planner (jumps, drops, walls), sweep-and-merge of heightmaps (map objects add collision only near Mario), a Blooper kinematics planner, an SPC (event script `.sb`) disassembler.

## Results
| Objective | Warp / start | Result | Notes |
|---|---|---|---|
| Boathouse blue coins -> Shine | 1,8 + story8, 10 plaza blue-coin flags | **pass** | Walk to the west pier, the boathouse door is a `BG_TYPE_MAP_WARP` floor on its south side (the "M" swirl on the wall is decoration), talk to the raccoon with B (talk = B, `MEANING_TALK_B`, needs `MARIO_STATUS_WAIT`), trade, Shine get demo (15 shines, blue counter 10 -> 0), save prompt answered, card written, plaza reloaded |
| Guide map (Z) | 1,1 2,0 3,0 4,0 5,0 6,0 7,0 8,0 9,0 13,0 14,0 | **pass** | Opens (iris transition), cursor moves, closes; no crash in 11 stages |
| Ricco 6 red coins (after M4) | 3,5 | partial | Switch ground-pound, message, 2:00 timer and counter work; Blooper surfing works; 2/8 coins in one run. The red Blooper keeps ~46 units/frame and turns 200 s16/frame (turning radius ~2200; checked against the asm: `doSurfing`/`surfing`/`checkCurrentPlane` match, so this is original), crashes kill at >30 degrees, boats/cranes/buoys add collision only near Mario, coin 7 is at y 400 (surf jump timing). A planner route reaches 2 coins; not finished |
| Pianta 2 Il Piantissimo race | 8,1 | partial (race lost) | Race start (within 200 of him, `__forceStartTalkExceptNpc`), Ready-Go, route up the mountain to the flag at 45.9 s works; Il Piantissimo still arrives first ("You are nothing", `startMiss`, life lost, back to the plaza: original script) |
| Other episodes listed | 窶・| not started | Objects, heightmaps and event scripts surveyed for 2,2 2,3 3,2 3,3 4,0 4,3 5,2 6,0 6,1 8,0 8,1 9,0 9,1 |

No port bug found on these paths. Checked and ruled out: red Blooper speed/turning (original code), "blocked" approach to Il Piantissimo (Q < 50 stick in the dead zone, not collision), boathouse door graffiti (not a cleaning target).

# Multiplayer NPC sync (w12)
Coordinator refocus: NPC sync (P8 sub-package). Design and wire: MULTIPLAYER.md "NPC sync (P8, w12)".
- New `port/src/port_mp_npcs.cpp` (registry of `TBaseNPC`s per stage visit, leader ownership with hysteresis, 15 Hz state with animation/flags/goop, puppets, talk/hold local overrides), channel `PORT_MP_CH_NPCS = 17` in `port_mp.h`, one `SMS_PORT_AURORA` hook in `TBaseNPC::perform` (`NpcBase.cpp`). Off unless a session's host chose "Sync enemies: Experimental".
- Checked with 2-3 silent, hidden instances (Delfino Plaza free mode, Pianta Village 1): positions, angles and BCKs agree, ownership follows the nearest player, a spray reaction on the owner shows on the other client's puppet; no crash. GameCube `mario.dol: OK`, `smoke_all.py --quick` 20/20.
- Found on the way (w11's file, reported, not changed): `port_mp_objects.cpp` builds its list on the 2nd game frame of a visit, while the stage is still loading; in the free-mode plaza `gpConductor` is still null then ("no conductor, nothing to sync"). Waiting for director state 4 fixes it (done that way in `port_mp_npcs.cpp`).
- Episode playthroughs stopped by the user's rule; what is left is listed in `port/PLAYTEST_EPISODES.md`.
## MP-G1. Goop cleaning sync (P8 item 4): HANDOFF (stopped for the usage limit)
- What it does: clean stamps (and goop that spreads by itself) are sent as delta-coded tasks on channel 16 and replayed through `pushStampTask`. Late joiners get a snapshot. After a quiet period every peer reconciles, but only inside the areas cleaned this visit. Host setting `reserved[5]` (0 Auto = with enemy sync, 1 On, 2 Off). Two `SMS_PORT_AURORA` hooks: `TPollutionTexStamp::pushTask` (PollutionCount.cpp) and `spread()` (PollutionAction.cpp). GameCube shasum OK after both.
- Files: `port/src/port_mp_goop.cpp`, `port/src/port_mp_goop_ui.cpp`, `port/tools/mp_goop_test.py` (new); `src/Map/PollutionCount.cpp`, `src/Map/PollutionAction.cpp` (hooks).
- Tested with `mp_goop_test.py -n 3 --backend vulkan` (silent, hidden; D3D12 lost its device with 12 other instances on the GPU):
  - Airstrip 0,1: all checks pass. Degrees 3809 / 3779 / 3779; 30 texels differ, which is the host Mario's own goop trail (not sent).
  - Bianco 2,0: the cleaned patch matches on all three. Maps are about 1 % apart from each client's own enemy goop. Diff images: `port/build_aurora/goop_airstrip_diff.png`, `goop_bianco_diff.png`.
  - About 4 bytes per stamp, roughly 2 KB/s while spraying.
- Not done:
  - MULTIPLAYER.md section, and channel 16 still to be listed in `port_mp.h` (w10).
  - Quick smoke run on this build.
  - Optionally sending Mario's own goop trail (`MarioRun.cpp` / `MarioMove.cpp` pollute calls).
  - w11 (P8): `gPortMpGoopForward` exists for owned enemies' goop but nothing sets it yet.
- Next step: rebuild, run `smoke_all.py --quick`, write the MULTIPLAYER.md section, then run merge_check.


# Multiplayer enemy sync, phase 2 (w11, P8a)
`port/src/port_mp_objects.cpp` (rewritten), new `port/include/port_mp_obj.h`, `SMS_PORT_AURORA` hooks in `src/Player/MarioCheckCol.cpp` (hitNormal, hitHipDrop), `src/Player/MarioCollision.cpp` (trampleExec), `src/Player/ModelWaterManager.cpp` (move), `src/Enemy/bossgesso.cpp` and `src/Enemy/bosspakkun.cpp` (damage functions); details in `port/MULTIPLAYER.md` "Enemy sync, phase 2". GameCube `dtk shasum` OK.
- Nerve (as exe-image offset, same build only) and HP in every state; take-over continues in the owner's nerve with its HP.
- Hits from a non-owner (stomp, hip drops, punch, water) are forwarded to the owner and applied there with the game's receiveMessage.
- More families: Stu variants, NAME_KURI, Electrokoopa, Cataquack, Bloopers, TAMA_NOKO, BOMB_HEI, rolling enemies. Goop Piranhas and Boos were tried and left out: their dead flag also means "hidden", so puppets played real deaths (seen in the first Bianco run).
- Bosses Petey Piranha / Gooper Blooper: owned by the nearest player, followers run the AI but take nerve changes, HP and pose; damage functions forward to the owner. Found: forcing a fight nerve on Petey's copy that is not in its fight (Bianco 1's windmill Petey, manager flag set) crashed in `TBossPakkun::changeBck`; such copies are now never forced or damaged.
- Short checks only (`mp_test.py -n 2`, `SMS_PORT_MP_OBJ_TEST`, no fights played), all mp_test checks pass, no crash:
  - Bianco 1: Guest1's stomp on a host-run Stu -> "hit 0 ... applied" on the host -> the Stu dies there -> "died on its owner" on Guest1; Guest1's water -> applied on the host, HP 5 -> 4; Guest1 walks up -> "taken over (HP 4, nerve set)", nerve offsets equal on both.
  - Petey Piranha (55,0): Guest1's forced hip-drop damage -> sent to the host -> applied (HP 3 -> 2) -> Guest1's copy HP 2; the host's own forced damage -> HP 1 on both.
  - Gooper Blooper (3,0): Guest1's forced beak damage -> applied on the host (HP 2 -> 1) -> Guest1's copy HP 1.
- Single player: smoke_all --quick 20/20; a replay recorded with main's current exe (Bianco, keyboard input incl. spraying and jumps) replays on this build with 0 of 14 checkpoints differing.
- Needs a real fight (user playtest list in the w11 report): full Petey and Gooper fights with two players, tentacle pulling, Electrokoopa stomps.
- Object list built once the director is in its play state (w12's report: on the 2nd frame of a visit the free-mode plaza's conductor was not there yet). The free-mode plaza now syncs its 72 moving map objects. NPCs stay with w12's port_mp_npcs.cpp.

## MP11. Handoff (w10, stopped for the usage limit)
- Done and merge-ready (tested): world events `port/src/port_mp_events.cpp`, hooks in Item.cpp, MapObjTown.cpp, MapObjRicco.cpp, MapObjBlock.cpp, MarDirectorEvent.cpp, plus `reserved[5]`, `PORT_MP_CH_EVENTS` and the `SMS_PORT_MP_EVENTS` knob. Sync checks passed for the super-hip-drop slab, fruit switches, a nozzle box and bricks, guest and late joiner. GameCube DOL OK.
- Done and merge-ready: host migration, protocol 4 (`port_mp_net.cpp`, `port_mp.h`; server `TAKEOVER` and 30 s lobby expiry in `sms_lobby_server.py`). Passes `mp_test.py --migrate-at` (direct IP) and `mp_server_test.py` phases 2 and 4 (killed host); `mp_chat_test` passes.
- Last full `mp_server_test` run: 36 of 37 passed. The one failure (phase 2) was an outdated expectation, now fixed and passing on rerun.
- Not done: events for sand castles/birds, Shine gates, doors and cages; runtime-spawned objects.
- Playtest list: pound switch and red-coin switch events with real players (none in the scenes checked); host migration with real NATs.
- Next step: merge_check apply, then the remaining event classes.
- Merged by w15 (2026-10-09): world events moved to `PORT_MP_CH_EVENTS` = 18 and `reserved[6]` (16 / [5] went to w6's goop sync); w8's red-coin switch hook sits next to the event hook in `TRedCoinSwitch::receiveMessage`. Lobby names now keep up to 31 bytes (`sanitizeName` capped them at the player-name size, 23; w14's note).

# Multiplayer: the world keeps running through local-only stops (w11; enemy sync phase 3)
- User report: pausing in multiplayer froze NPCs and enemies for that player only, which desynced the instances. New `port/src/port_mp_world.cpp`:
  - In a session, the pause menu, guide map and save prompt no longer take `CUE_MOVE` / `CUE_CALC_ANIM` out of the movement pass (`MarDirectorDirect.cpp`). `TMario::perform` holds only the local Mario still (`MarioMain.cpp`).
  - An NPC talk no longer stops enemies or map objects (`smallEnemy.cpp`, `enemyAttachment.cpp`, `MapObjBase.cpp`, `Item.cpp`, `tamaNoko.cpp`, `gatekeeper.cpp`). Mario is invincible while he talks anyway.
  - All hooks are under `SMS_PORT_AURORA` and are off outside a session. GameCube `dtk shasum` OK.
- Check: `mp_test.py --host-pad 3600:START,4200:START` (new option) in Bianco with w14's synccheck. During the host's 10 s pause its world kept moving (about 30 actors move each second) and agreement stayed at 95%, the same as outside the pause. The host's Mario stood still.
- Still local-only stops (details in MULTIPLAYER.md): the Shine-get demo (separate stop-the-world perform list), demo-camera cutscenes, `TMapObjBase::isDemo()` users, and the Boo block in a talk.
- Enemy sync phase 3 (P8a):
  - Goop Piranha Plants and Boos are synced, with the hidden flag mirrored (STATE flags bit 2).
  - Pokeys and Wigglers run in follow mode.
  - w14's `port_mp_synccheck.cpp` is taken in unchanged.
  - w7's moving-platform module is not merged: platforms stay in `port_mp_objects.cpp`, and breakables go to P8c.
- Smoke `--quick` (before re-syncing with main): 19/20. 13,0 crashed in the render thread inside the Vulkan driver (dawn, `Mtx_unlock`), single player, not in this change.
# Multiplayer map-object reactions (w11)
- User report (2-player plaza playtest): spraying the rooster weathervane, a hanging sign or a coin spot reacted only for the sprayer, coin included. New `port/src/port_mp_mapreact.cpp` (channel 21):
  - Our water hits on these objects are counted and sent. Every receiver applies the same hits through the object's own `receiveMessage`.
  - The coin each reveal throws is linked to its object. Taking it (`TCoin::taken` hook) removes the other screens' copies. w8's run-time credit counts it once.
  - Hooks: the `port_mp_obj.h` water/hit gate also opens on `gPortMpMapReact`, a dispatch at the top of `port_mp_obj_hit`, one line in `TCoin::taken`.
- The camera holds still behind the pause menu in a session (`CPolarSubCamera::perform`: no `CUE_MOVE` while the local Mario waits): no more C-stick turning behind the menu.
- Check: plaza 1/2, 2 instances, `SMS_PORT_MP_MAPREACT_TEST`. The sign swing and the weathervane spin were applied on the guest, both screens dropped each coin, the guest took the weathervane coin and the host's copy was removed. GameCube `dtk shasum` OK; smoke `--quick` 20/20.

## W12. Bug reports: the last 20 s as video + log + replay (`port_bugreport.cpp`)
- User request: F9 or a crash saves what the player saw and heard in the last 20 s and a log of it, so testers can hand in crashes and visual/audio problems.
- `bug_reports\<yyyy-mm-dd_hh-mm-ss>_<f9|menu|crash>\` next to the exe: `video.mp4` (H.264 + AAC, 640x480 at 30 fps by default), `log.txt` (port build, Windows version, GPU + backend, window/internal resolution, settings summary, stage/scenario/app state, Mario position/action/health, fps over the last 5 and 20 s, multiplayer state, the crash backtrace, then the last 20 s of stdout/stderr lines each stamped `[hh:mm:ss.mmm f<presented frame>]`) and the replay (`port_replay_save_dir`). A toast (port_qol) names the folder.
- Picture: once per capture period the main thread arms Aurora's present capture (`aurora_present_capture.patch`, as `SMS_PORT_SHOT_PRESENT`); the readback is asynchronous and the mapped BGRA image is shrunk to NV12 (nearest sampling) on the completing thread (~0.5 ms), then encoded by Microsoft's software H.264 MFT on a worker thread (low latency, CBR, keyframe every second) into a memory ring of `seconds` + 2 s (about 6 MB). Sound: `port_audio_out_ai` hands the stereo mix to a PCM ring (works without an audio device too). Log: stdout and stderr are routed through pipes (`_dup2`) whose reader threads forward every byte to the original handles and keep the lines in a 4 MB / `seconds` + 2 s ring.
- Save: the encoder thread muxes from the first keyframe inside the window with an `IMFSinkWriter` (video passed through with the SPS/PPS as `MF_MT_MPEG_SEQUENCE_HEADER`; audio resampled 32 kHz -> 48 kHz, AAC-encoded by the sink writer; interleaved in 100 ms audio chunks). F9/menu return at once (the mux finishes in the background within ~0.2 s); a crash (`port_crash.cpp`'s filter, before its stack scan; `fatalTrace`; `port_replay.cpp`'s SIGABRT handler) waits for it at most 8 s and always writes log.txt + the replay. If the crashing thread is the encoder thread the report has no video.
- Settings `[bugreport]`: `enabled` (1), `seconds` (20, 5..60), `video_width` (640), `video_height` (480; 0 = the window's shape), `fps` (30, 10..60); `SMS_PORT_BUGREPORT=0/1` overrides `enabled`. Off: nothing is installed (no pipes, no capture).
- Test knobs: `SMS_PORT_TEST_F9=<frame>` saves a report at that presented frame and exits 300 frames later; `SMS_PORT_TEST_CRASH=<frame>` null-writes at that frame. Verified hidden (w9r runner): the f9 folder holds a 19.9 s 640x480 mp4 (597 frames, 48 kHz stereo AAC, read back with `IMFSourceReader`), log.txt and the replay; the crash folder the same with the backtrace in log.txt (mux done in 0.17 s).
- Found on the way: `port_crash.cpp`'s "stack scan" after the backtrace can end the process by itself (a second exception inside its `__try` re-enters the filter whose once-guard calls ExitProcess), which is why the bug report is saved before it. Not changed otherwise.
- Cost (plaza, 1280x960 window, 30 fps setting, hidden runs with `frame_stats`): see the report; the presents stay at 29.97/s with the same 33.3 ms interval.

## W12-1. Jitter above 30 fps: display frames that lag the newest logic state (water, wires, unlimited)
- **Cause:** `direct()` runs the logic ticks and the next unk34 before it waits for the retrace that puts the previous logic draw up. Display frames drawn in that wait have `sEntryTick == sSegState + 2`. The camera and J3D models use their kept snapshots, but texture matrices, point/value sets (wires, flags, waves), particle tracks and shadow requests required `sEntryTick == sSegState + 1` and otherwise drew the live (newest) state: one logic frame (33 ms) ahead of the camera and models, then back at the next retrace. Measured (Bianco, 144 fps): 38 % of display frames (1349 of 3521 wire draws); texture matrices recorded but not patched: 30 % at 144 fps, 27 % at unlimited, 0.4 % at 60 (frames on the retrace grid). The more display frames per logic period, the larger the share, hence "a lot more jitter at unlimited".
- **Fix** (`port/src/port_interp.cpp`, `lagNow()`): in that state the newer end comes from the copies kept at the logic draws (point sets, value sets, track records with a second generation, shadow request snapshot, SRT history with a third generation). `port_interp_lag()` for systems with their own copies (`TModelWaterManager::portInterpCalcView`, new `mPortPrev2Pos`). After: 0 % texture matrices unpatched at 60 / 144 / unlimited. `SMS_PORT_INTERP_LAG=0` restores the old behaviour (comparison).
- Pacing at unlimited measured separately: alpha is monotonic and never above 1 (sd of alpha rate 0.003), present-time residual 0.77 ms of motion (sd). Holding presents to a slot at unlimited (tried) halves the frame rate (Aurora's frame slots fill), so it was left as is.

## W12-2. Discord Rich Presence + invites (`port_discord.cpp`)

- **What**: Rich Presence, Join buttons, ask-to-join, with no Discord SDK. The game speaks Discord's documented local RPC IPC itself: named pipe `\.\pipe\discord-ipc-0..9`, frames `[u32 opcode][u32 length][JSON]` (0 HANDSHAKE, 1 FRAME, 2 CLOSE, 3 PING, 4 PONG). Commands used: `SET_ACTIVITY`, `SUBSCRIBE` (`ACTIVITY_JOIN`, `ACTIVITY_JOIN_REQUEST`), `SEND_ACTIVITY_JOIN_INVITE`, `CLOSE_ACTIVITY_REQUEST`. A 100-line JSON reader/writer lives in the file.
- **Threads**: one worker thread owns the pipe (connect with 5 s..30 s backoff, so Discord may start or stop at any time, handshake timeout, PING/PONG, `CLOSE` handling; a bad client id backs off 2 min). The game thread only runs `port_discord_frame()` (hook after `port_mp_menu_draw()` in `port_window.cpp`): while Discord is not connected that is a few atomic reads. When connected it copies the area, shine and blue-coin counts (`TFlagManager`, read only) and the multiplayer party into a snapshot once a second, hands finished join events to `port_mp_join_name` and draws the ask-to-join prompt. Presence is sent at most every 4 s and only when it changed.
- **Content**: details = English stage name (`stageLabel` of `port_mp_presence.cpp`, exported as `port_mp_stage_label`), state = `Episode N - X Shines, Y Blue Coins` ("Exploring" in Delfino Plaza; title/file select/cutscene have their own text), `timestamps.start` = session start, large image `logo`, small image = world key (delfino, bianco, ricco, gelato, pinna, sirena, monte, noki, corona). Online: `Co-op - ...`, party id `sms-<lobby>`, size `[players, max]`.
- **Join**: in a lobby-server session without password the activity carries `secrets.join = "sms1:<lobby name>"` (new `port_mp_lobby_has_password()` in `port_mp_net.cpp`). `ACTIVITY_JOIN` -> toast + `port_mp_join_name(name, "")` (same as the menu's Join by name). Direct IP sessions and password lobbies publish no secret. `ACTIVITY_JOIN_REQUEST` -> ImGui prompt, F7 accept / F8 ignore, expires after 30 s.
- **Settings**: `[discord] enabled (1), client_id (empty = idle, one log line says how to set it), show_stage (1), allow_join (1)`; Esc > Game > Discord has the toggles and a status line. Test hooks: `SMS_PORT_DISCORD_APP_ID` (overrides client_id), `SMS_PORT_DISCORD_PIPE` (pipe name prefix, for a fake server).
- **Not possible without the SDK**: launching the game from Discord (the "register command" is `Discord_Register`/a registry key in the SDK; the RPC has no equivalent), so Join works while the game is running.
