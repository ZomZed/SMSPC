# Super Mario Sunshine PC port: plan

The PC port compiles the same decompiled source as the GameCube build (`src/`,
`libs/JSystem/`) with a modern compiler, and replaces the GameCube-only
libraries (Dolphin SDK, MSL libc, MetroTRK, THP player) with host code in `port/`.

The GameCube build (`configure.py` / ninja) must stay byte-identical. PC-only
code paths in shared source use `#ifdef SMS_PORT` (defined by
`port/include/port_compat.h`, which is force-included into every PC
translation unit and never seen by the GameCube build).

## Toolchain
- Compiler: the clang bundled with Visual Studio 18
  (`VC\Tools\Llvm\x64\bin\clang.exe`), targeting `x86_64-pc-windows-msvc`.
- C++17, host UCRT / MSVC STL headers. CodeWarrior's MSL libc is not used.
- **64-bit.** The graphics layer (Aurora + Dawn/WebGPU) only ships for
  windows-amd64, so the port is x64 (LLP64: `long`/`u32` stay 32-bit, pointers
  are 8 bytes). The game stores pointers in structs loaded straight from files
  (`JSUConvertOffsetToPtr`) and in 32-bit slots in places; those hazards do not
  show up as compile errors and are tracked in `port/POINTERS.md`.

## Milestones
1. **Compile census.** Every game and JSystem unit passes `clang -fsyntax-only`
   (`python port/tools/census.py` writes `port/CENSUS.md`). *In progress.*
2. **Object build and link census.** A real compile of every unit plus a link
   step. A generated `port/src/stubs_auto.cpp` provides every still-missing
   Dolphin SDK / asm symbol as a logging stub, so the link succeeds and the list of
   missing pieces is explicit.
3. **Headless boot.** `main()` runs the game loop with no graphics or audio:
   - DVD reads go to the extracted disc folder (`dolphin_test/files`);
   - OS timers, threads and the heap run on the host;
   - PAD is read from the keyboard or XInput.

   Goal: reach the title screen state machine and log scene changes.
4. **Endianness.** All game data files are big-endian and are read in place.
   Add byte-swapping at each loader (J3D model/anim/texture, BMG, JPA particles,
   scene .bin, collision, .prm params, JAudio banks). This is the biggest
   chunk of work.
5. **Graphics.** Implement GX / VI on a modern API. The most practical route is
   an existing open-source GX reimplementation such as **Aurora** (used by
   other decomp ports). It provides GX/VI/PAD/OS on WebGPU/SDL, but it has to
   be downloaded (it needs git and CMake, neither installed yet). Otherwise,
   write a minimal GX→D3D11 or OpenGL translator.
6. **Audio.** JAudio's DSP/AI/ARAM backend goes to a host mixer.
7. **Memory card → save files, options, widescreen, 60 fps, and so on.**

## Rules for agents working on the port
- Prefer standard-conforming fixes in shared code that leave the GameCube
  object byte-identical. Otherwise guard with `#ifdef SMS_PORT`. After any
  shared-source edit, the GameCube build must still pass
  `build\tools\dtk.exe shasum -c config\GMSE01\build.sha1`.
- PowerPC asm functions (`asm void f() { ... }`) get a C/C++ equivalent under
  `#ifdef SMS_PORT` (for paired-single math, use the C_MTX/C_VEC versions).
- Never change game behaviour. The port must play exactly like the GameCube game.
- An hourly decomp routine edits `src/` at the same time. Work in a worker
  copy and merge with `tools/agent_merge.ps1` like the decomp agents do.
