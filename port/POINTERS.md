# 64-bit pointer hazards (x86_64-pc-windows-msvc)

The port targets x64 because Aurora + Dawn (WebGPU) only ship for windows-amd64.
Windows x64 is LLP64: `int`, `long`, `s32`/`u32` (`long`) stay 4 bytes;
pointers, `size_t`, `intptr_t`/`uintptr_t` and `ptrdiff_t` are 8.

The census (`python port/tools/census.py`, clang `-fsyntax-only`) passes 575/575
on x64, but it runs with `-w`. In MS-compatibility mode clang only *warns* about
`(u32)ptr`, `(T*)u32value` and 64-to-32 narrowing, so the census does not see
the real problems. This file lists them.

How the list was made: every census unit was compiled with
`-Wshorten-64-to-32 -Wpointer-to-int-cast -Wvoid-pointer-to-int-cast
-Wint-to-pointer-cast -Wint-to-void-pointer-cast`. Hits that are pointer
differences, `size_t` lengths or alignment masks were dropped, and file-mapped
structs were found by reading every `JSUConvertOffsetToPtr` user and every
in-place "pointer patching" loop.

To rerun the scan, add those flags to `census.COMMON`, remove `-w
-Wno-everything`, and grep the output for `-Wint-to-pointer-cast`,
`-Wint-to-void-pointer-cast`, `-Wpointer-to-int-cast` and
`'uintptr_t' to 'u32'`.

Status legend: **FIXED** (in this change, under `#ifdef SMS_PORT` or as a
type-identical spelling), **TODO** (open; recommended fix given).

---

## 1. File-mapped structs with pointer members (TODO, biggest job)

The loaders cast the file buffer to a struct and read it in place. Some members
are declared as pointers, but in the file they are 4-byte offsets. Others are
4-byte slots that the loader overwrites with a pointer. On x64 a pointer member
is 8 bytes and 8-aligned, which causes two problems:
- every later field moves, so `sizeof`, array strides and reads stop matching the file;
- writing a pointer back stores 8 bytes into a 4-byte slot and overwrites the next field or entry.

| Where | Struct / code | What happens on x64 |
|---|---|---|
| `libs/JSystem/include/JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp:9-60` | `J3DModelInfoBlock`, `J3DVertexBlock`, `J3DEnvelopBlock`, `J3DDrawBlock`, `J3DMaterialDLBlock`, `J3DTextureBlock`: offsets declared `void*` | **FIXED**: members are `JSU_FILE_PTR(T)` (`T*` on GC, `JSUFileOffset<T>` = `u32` on PC, see below), plus `static_assert`s on every block size. `readVertex`'s `(uintptr_t)` differences (`:254`/`:273`) are within one file buffer, so the `u32` narrowing is safe. |
| `.../J3DGraphLoader/J3DShapeFactory.hpp:35-50` | `J3DShapeBlock`: `J3DShapeInitData*`, `u16*`, `ResNTAB*`, `GXVtxDescList*`... | **FIXED** the same way (`static_assert` 0x2C, init-data structs asserted too). `(u32)block.mpX` now reads the `u32`. |
| `.../J3DGraphLoader/J3DJointFactory.hpp:19-29` | `J3DJointBlock` | **FIXED** (0x18; `J3DJointInitData` 0x40). |
| `.../J3DGraphLoader/J3DMaterialFactory.hpp:27-59`, `J3DMaterialFactory_v21.hpp:26` | `J3DMaterialBlock(_v21)`: about 30 `void*` offsets | **FIXED** (MAT3 0x84, MAT2 0x78; `J3DMaterialInitData` 0x14C, `J3DIndInitData` 0x138 and every MAT3 table element asserted). |
| `libs/JSystem/include/JSystem/JSupport.hpp:16-24` | `JSUConvertOffsetToPtr<T>(const void*, const void* offset)` | **FIXED**: on PC it adds `(s32)(intptr_t)offset`, the low 32 bits of the value the caller cast to `void*`. The J3D block call sites now pass a `JSUFileOffset`, which converts to `u32` and picks the `u32` overload. New in `JSupport.hpp`: `JSU_FILE_PTR(T)` / `JSUFileOffset<T>` (implicit `u32` conversion, `== nullptr` / `!= nullptr`), for any other file struct that declares offsets as pointers. JAudio's two users (`JASBNKParser.hpp`, `JASWSParser.hpp`) are unchanged. |
| `libs/JSystem/src/J3D/J3DGraphLoader/J3DAnmLoader.cpp:124-451` (about 80 sites); `J3DAnimation.hpp:122-126` | Each `JSUConvertOffsetToPtr<T>(data, (void*)data->mXOffset)`; `J3DAnmVtxColorIndexData { u16 mNum; void* mpData; }` patched in place at `:217-224` | **FIXED**. The `(void*)s32` sites are left as they are (GC codegen) and are correct through the fixed `const void*` overload (clang still warns `-Wint-to-void-pointer-cast`; harmless). The anim headers are asserted to their file sizes. `readAnmVtxColor` (full and key) builds a native `J3DAnmVtxColorIndexData` array from the 8-byte file entries (`portMakeVtxColorIndexData`) instead of patching the file. |
| `libs/JSystem/include/JSystem/JKernel/JKRArchive.hpp:59-66` | RARC `SDIFileEntry { ...; void* mData; }` (0x14 bytes in the file) | Was: `mFileEntries` points into the file, so entry *i* was read at stride 0x18, and `fileEntry->mData = ...` wrote 8 bytes. **FIXED**: on PC the slot is `u32 mDataSlot` (unused, `static_assert(sizeof == 0x14)`). The pointers live in `JKRArchive::mFileData` (`void**`, one per entry, zero-filled CRT memory), allocated by `portAllocFileData()` right after each subclass sets up its tables (`JKRMemArchive::open` x2, `JKRDvdArchive::open`, `JKRCompArchive::open`, `JKRAramArchive::open`) and freed by `portFreeFileData()` in `~JKRArchive` and `JKRMemArchive::unmountFixed`. Every access goes through `JKR_ARC_ENTRY_DATA(entry)` (`mFileData[entry - mFileEntries]` on PC, `(entry)->mData` on GC): `fetchResource` x2 in all four subclasses, their destructors and `removeResourceAll`, `removeResource`/`detachResource`, `findPtrResource`. The other lookups (`findFsResource`, `findIdxResource`, `findIdResource`, `findNameResource`, `findTypeResource`, `findDirectory`, `countFile`, `countResource`, `getFileAttribute`) only needed the 0x14 stride. Tested through the real `JKRMemArchive` in `port/tools/test_archive.cpp`. |
| `include/Map/MapCollisionEntry.hpp:110-118`; `src/Map/MapCollisionEntry.cpp:80-95` | `TMapCollisionGroup { ...; s16* mIndices; u8* mSoundMaterials; u8* unk10; s16* mAdditionalDatas; }` from the .col file, patched in place with `(int)ptr + hdr` and a `WAS_PATCHED` flag | Wrong stride, 8-byte writes, and `(int)` truncation. Collision would not work at all. **FIXED** (copy-into-native-structs pattern, section 1 recommendation 3): on PC `TMapCollisionBase::init` swaps the file once (`port_swap_col`), reads the 0x18-byte file groups (`PortColGroupFile`, `u32` offsets) and builds a per-object native `TMapCollisionGroup[]` whose pointers point into the file (the shared resource is never patched, so `WAS_PATCHED` is only set on the native copy). Vertices stay `Vec*` into the (swapped) file. Tested by `python port/tools/test_col.py` (225 files, 39381 triangles). |
| `include/Map/PollutionManager.hpp:18-21`; `include/Map/PollutionLayer.hpp:47`; `src/Map/PollutionManager.cpp:142-151` | `TPollutionInfo::mLayerInfos`, `TPollutionLayerInfo::mHeightMap` patched with `+ (u32)info` | **FIXED**: on PC `setDataAddress` reads `ymap.ymp` with `sms_be16/32` (the file is big-endian and no swap pass covers it) and builds a native `TPollutionLayerInfo[]` (0x30 bytes on x64, 0x2C in the file); `mHeightMap` points into the file. `TPollutionInfo::mLayerInfos` is a `u32 mLayerInfosOffset` on PC, and `initPollutionInfo` reads the layer count with `sms_be16`. |
| `include/Strategic/spcinterp.hpp:290-296` | SPC script binary `TSpcSymbol::mNativeCall` (`u32`), filled at bind time with a function address | **FIXED**, see section 3. The struct stays 0x14 bytes. |
| `libs/JSystem/include/JSystem/J3D/J3DGraphBase/J3DTexture.hpp:28-39`, `src/Map/MapMirror.cpp:346-347`, `src/MoveBG/ModelGate.cpp:299-306` | `ResTIMG::imageDataOffset` / `paletteOffset` (`u32`) set to *heap-relative* deltas such as `&timg - &mResources[i]` | The layout is fine (only `u32` fields), but a negative delta wraps in `u32`. Readers (`J3DTevs.cpp:373/385`, `JUTTexture.cpp:57/89/111`, `PollutionLayer.cpp:363` and others) add it zero-extended to a 64-bit pointer and land about 4 GB away. **FIXED**: on PC both fields are `s32` (`ResTIMG.hpp`), so every `(u8*)img + img->off` reader sign-extends without per-site changes, and the writers' `uintptr_t` differences truncate to the right signed delta. Safe because all game memory is the 32 MB arena (section 4). |

Formats that are already pointer-size clean (no change needed):
- **JAudio BNK/WS** (`JASBNKParser.hpp:13-19`, `TOffset<T>` holding a `u32` offset).
- **J3D anim headers** (`J3DAnmLoader.hpp`, `s32` offsets).
- **JPA**: `JPABaseShape.cpp:214` and others parse by byte offsets.
- **Scene .bin / .prm**: read through `JSUMemoryInputStream`.

BMG (`SMSGetMessageData`) is still a stub. Implement it with byte offsets.

**Recommended fix pattern.** In priority order:
1. **Offset structs plus an accessor** (preferred for read-only blocks). Under
   `#ifdef SMS_PORT`, declare the file member as `u32` (or a `TOffset<T>`, as
   JASBNKParser does) and change the `JSUConvertOffsetToPtr(block, block->mpX)`
   call sites to pass the `u32`. Add a `static_assert(sizeof(J3DShapeBlock) ==
   0x2C)` and similar asserts per struct so drift is caught at compile time.
   This covers every J3D `*Block`, the anim loaders, and `JSUConvertOffsetToPtr`'s
   `const void*` overload (make it take `u32`/`s32` on PC).
2. **Side table for runtime pointers kept in file entries.** RARC
   `SDIFileEntry::mData` and `TMapCollisionGroup` pointers are written at
   runtime. Keep the file struct at 0x14 bytes with `u32 mData` (unused on PC)
   and hold the real pointers in a native array allocated at mount/load time
   (`void** mFileData`, indexed by entry). For collision, the cleaner option is
   to copy each `TMapCollisionGroup` into a native struct at load, since the
   group count is small.
3. **Copy into native structs at load** when the game also writes the data
   (collision groups, pollution layer infos, `J3DAnmVtxColorIndexData`):
   allocate a native array, convert each record once, and point the game at
   the copy. This also gives a natural place for the big-endian byte swaps of
   milestone 4.
4. **Heap-relative ResTIMG offsets.** Sign-extend at the readers
   (`(u8*)img + (s32)img->imageDataOffset`) and keep all game memory inside one
   arena under 2 GB, so any delta fits in `s32` (see section 4).

## 2. Hard-coded pointer sizes and offsets

| Where | Code | Status |
|---|---|---|
| `libs/JSystem/src/JAudio/JASystem/JASDvdThread.cpp:33` | `typedef u8 FabricatedCallstack[0x40]` holds a function pointer plus a copy of `TDvdCall`. On GC that is 4 + 0x38 bytes; on x64 it is 8 + 0x48 = 0x50, so it overflowed into the next slot. | **FIXED**: 0x60 on PC, plus a `static_assert` |
| `JASDvdThread.cpp:133` (`dvdProc`) | Arguments are written at `(void**)cs + 1` (+8 on x64) but read at `(u8*)cs + 4` | **FIXED**: reads at `(void**)cs + 1` on PC |
| `libs/JSystem/include/JSystem/JKernel/JKRExpHeap.hpp:25-28` | `CMemBlock::getBlock(p)` returns `p - 0x10`, but `sizeof(CMemBlock)` is 0x18 on x64 (`getContent()` uses `this + 1`) | **FIXED** (section 4): `(CMemBlock*)p - 1` on PC |
| `include/MarioUtil/AlphaDebug.hpp:264` | Debug helper reads `*(J3DDisplayListObj**)(rm + 0x3C)` (a `J3DMaterial` offset) | TODO: use the member. Debug-only code. |
| `libs/JSystem/src/JKernel/JKRExpHeap.cpp:75` | Comment `// = 0x90` for `sizeof(JKRExpHeap)` | Harmless; the code uses `sizeof` |
| `JSupport.hpp`, J3D loaders | Struct sizes implied by `/* 0xNN */` comments only | **FIXED for J3D** (model blocks, init data, MAT3 table elements, anim block headers; under `#ifdef SMS_PORT`). Other file structs: TODO |

No `ARRAY_COUNT`-over-pointer-array I/O, and no `* 4` pointer-table strides used
for file I/O, were found. `TSpcBinary::getData` (`mDataNum * 4`) indexes a
`u32` table in the file, which is correct.

## 3. Pointers in 32-bit slots: callbacks, user data, VM values

| Where | Code | Status |
|---|---|---|
| `include/Strategic/spcinterp.hpp:290-296`, `src/Strategic/spcinterp.cpp` `bindSystemDataToSymbol`, `dispatchBuiltinDefault`, `TSpcTypedInterp::dispatchBuiltin`; about 130 `bindSystemDataToSymbol(name, (u32)&fn)` calls in `spcinterp.cpp`, `liveinterp.cpp`, `enemyinterp.cpp`, `NpcEvent.cpp`, `EventWatcher.cpp` | Script builtin function pointers stored in the file slot `TSpcSymbol::mNativeCall` (`u32`) | **FIXED.** On PC the slot holds a 1-based handle into a host table (`SpcPortRegisterNative`/`SpcPortGetNative`), read through `SPC_NATIVE_CALL(sym)`. `bindSystemDataToSymbol` takes `uintptr_t`, and the callers cast `(uintptr_t)&fn` (that type is `u32` on GC, so the GC build is unchanged). |
| `libs/dolphin/include/dolphin/arq.h` | `ARQCallback(u32 pointerToARQRequest)`; `ARQRequest::owner/source/dest` and `ARQPostRequest(..., u32 owner, ..., u32 source, u32 dest, ...)` carry main-memory pointers | **FIXED**: `ARQAddr` = `uintptr_t` on PC (`u32` on GC). `JKRAramPiece::doneDMA` and `JASystem::Dvd::aramDmaFinish(2)` take `uintptr_t`. JASDvdThread no longer narrows the owner `(u32)(uintptr_t)call`. |
| `libs/dolphin/include/dolphin/ai.h` | `AIInitDMA(u32 start_addr, ...)` / `AIGetDMAStartAddr()`; JASAiCtrl passes DAC buffer pointers | **FIXED**: `uintptr_t` on PC |
| `src/MarioUtil/DrawUtil.cpp:807` | Material ID (a pointer, or pointer >> 4) copied through a `u32` local | **FIXED**: the local is `uintptr_t` |
| `J3DMaterial.hpp:124` / `J3DPacket.hpp:181` | `DIFF_FLAG = 1 << 63` (int overflow, a compile error on x64) | **FIXED**: `enum : uintptr_t` on PC (the flags stay in the top bits of the pointer-sized ID) |
| Already pointer-sized before this change | `J3DPacket::setUserArea`/`getUserArea` (`void*`), `JPABaseEmitter::setUserWork` (`uintptr_t`), `J3DMatPacket::unk3C/unk44`, `JKRAMCommand::mSrc/mDst`, `JKRArchive::mEntryNum` (`intptr_t`) | OK |
| `include/Strategic/spcinterp.hpp:60-67` (`TSpcSlice` `int asInt`); `src/System/EventWatcher.cpp:74, 86, 108, 244, 1243, 1278`; `src/NPC/NpcEvent.cpp:27, 39, 60, 128, 150, 181, 191, 199, 208, 217, 226, 256` | The event script VM passes actor and NameRef **pointers as `int`** on its stack (`push((int)ref)`, `(TBaseNPC*)pop().getDataInt()`) | **FIXED.** `SPC_PTR2INT(p)` / `SPC_INT2PTR(T, i)` (`spcinterp.hpp`). On PC they call `SpcPortPtrToInt` / `SpcPortIntToPtr` (`spcinterp.cpp`): a 1-based handle table (up to 4096 objects, hashed by address in static storage, so equal pointers give equal handles and 0 is null). On GC they expand to the original `(int)` / `(T)` casts, so the GC object is unchanged. `asInt` is not widened, so script arithmetic keeps 32-bit wraparound. Handles are never recycled or reset (there is no scene-change hook that is safe for every script owner). The table is keyed by address, so it only grows with distinct objects that scripts name; it aborts with a message if 4096 are exceeded. Converted sites: `EventWatcher.cpp` (`evGetNameRef`, `evGetNameRefName`, `getNameRefPtr`, `evGetTalkNPC`, `evWarpFrontToMario`, `evIsWaterMelonIsReached`) and `NpcEvent.cpp` (all 14 pops and the `viewObj` push). `liveinterp.cpp`, `enemyinterp.cpp` and `spcinterp.cpp` had no pointer-as-int sites. |
| `src/MoveBG/MapObjLib.cpp:647` | `getWaterID()` returned `(intptr_t)mHolder` as `int` | **FIXED** (not an identity value): the actor is `TModelWaterManager::mStaticHitActor`, and the word at 0x68 is the water particle index (`TWaterHitActor::unk68`), used to index `unk2914`, `mParticleVelocitySOA` and so on. It overlapped `TTakeActor::mHolder` only by accident of layout. On PC the code reads `((TWaterHitActor*)actor)->unk68`. |
| `libs/JSystem/src/JAudio/JASystem/JASBankMgr.cpp:158` (`TChannel::unk14 = (u32)wave`), `JASSeqParser.cpp:203, 657` (track registers hold addresses), `JASHeapCtrl.cpp:42, 88` / `JASWaveArcLoader.cpp:22` / `MSound.cpp:103` / `JASSystemHeap.cpp:57` (`THeap` base passed as `u32`), `JASDSPInterface.cpp:75, 170, 181`, `dsptask.c:649-652`, `osdsp_task.c:130-135` | JAudio mixes ARAM addresses (genuinely 32-bit) with main-memory pointers in the same `u32` fields | TODO with milestone 6 (audio). Decide whether emulated ARAM is a host buffer addressed by `u32` offsets, which is recommended because then these stay `u32`, or real pointers. Then widen only the fields that hold main-memory pointers. |
| `libs/JSystem/src/JKernel/JKRDvdFile.cpp:153/159` | `OSMessage` (`void*`) carrying an `s32` result | OK: the value round-trips |
| `libs/JSystem/src/JKernel/JKRThread.cpp:35`, `JKRSolidHeap.cpp:52-53`, `JKRExpHeap.cpp:183/338`, `JKRHeap.cpp:74`, `J3DModelLoader.cpp:244/263`, `Map/MapMakeList.cpp:239` | Pointer *differences* narrowed to `u32` | OK while the heap arena is under 4 GB (section 4) |

## 4. The heap (JKRHeap / JKRExpHeap / JKRSolidHeap) (FIXED except the budget)

Status:
- **FIXED** `CMemBlock` is `alignas(16)` on PC: 0x20 bytes on x64
  (`static_assert`), so content stays 16-aligned and
  `ALIGN_NEXT(size, alignof(CMemBlock))` rounds sizes to 16. `getBlock()` is
  `(CMemBlock*)data - 1`. The alignment offset kept in `mFlags & 0x7f` is now
  0 or 16, which still fits.
- **FIXED** Minimum alignment 16: `JKRExpHeap::alloc` treats `|alignment| <= 16`
  as "no extra alignment" (every content address already is 16-aligned);
  `JKRSolidHeap::alloc` clamps to 16 instead of 4. `JKRStdHeap` goes through
  `OSAllocFromHeap` (32-byte cells in `sdk_osalloc.cpp`), already fine.
  `JKRAramHeap` hands out ARAM addresses, not host memory.
- **FIXED** Arena: `sdk_os_misc.cpp` maps 32 MB of main RAM at 0x80000000
  (below 4 GB, not below 2 GB). `initArena` runs unchanged; `OSRoundUp32B` /
  `OSRoundDown32B` are `uintptr_t` on PC. The root heap and every child heap
  live there, so the `u32` pointer differences in section 3 and the s32
  ResTIMG deltas fit.
- **FIXED** Global `operator new`/`delete` (`JKRHeap.cpp`). On PC they replace
  the executable's global new/delete, so host code goes through them too:
  - before the root heap exists, or inside `port_host_alloc_push/pop`, `new`
    uses the CRT heap;
  - `delete` routes by address: game RAM goes to `JKRHeap::free` (as on GC),
    anything else to `free()`;
  - the sized deletes are routed explicitly;
  - `sdk_dvd.cpp` (the FST table) and `sdk_os_thread.cpp` (`HostThread`) now
    allocate inside a host scope, so they neither eat game heap nor die with a
    scene heap.
- Tested by `port/tools/test_archive.cpp` on the real JKernel units:
  - 2000 random alloc/free/resize with alignments -64..64;
  - `check()`, and full coalescing back to one free block;
  - solid heap alignment;
  - the new/delete routing.
- **TODO** Memory budget (below).

- **Block header size.** `JKRExpHeap::CMemBlock` (`JKRExpHeap.hpp:30-35`: `u16,
  u8, u8, int, CMemBlock*, CMemBlock*`) is 0x10 on GC and 0x18 on x64.
  `getBlock()` hard-codes `-0x10` (`:27`) while `getContent()` uses
  `this + 1`, so every `free`, `resize` and `getSize` finds the wrong header.
  **Fix:** under `SMS_PORT`, make `getBlock` return `(CMemBlock*)data - 1`, and
  pad `CMemBlock` to 0x20 (`alignas(16)` or explicit padding) so content
  addresses stay 16-aligned.
- **Minimum alignment.** `JKRExpHeap::alloc` treats `alignment <= 4` as "no
  alignment" (`JKRExpHeap.cpp:143/153`), `JKRSolidHeap::alloc` clamps to 4
  (`JKRSolidHeap.cpp:74/76`), and the global `operator new`/`new[]` pass 4
  (`JKRHeap.cpp:255-279`). x64 objects that contain pointers or doubles need 8.
  The host CRT and clang assume 16 for `new`, and `memcpy`/vectorized copies
  may use aligned SSE moves. **Fix:** under `SMS_PORT`, raise the floor to 16
  (`if (alignment < 16) alignment = 16;` and similar), and round sizes with
  `ALIGN_NEXT(size, 16)` instead of `alignof(CMemBlock)`.
- **Arena setup.** `JKRHeap::initArena` (`JKRHeap.cpp:56-76`) uses
  `OSRoundUp32B`/`OSRoundDown32B` (`dolphin/os.h:165-166`, which cast to `u32`)
  and `OSPhysicalToCached(0)` (`os.h:175`). **Fix:** on PC, reserve one
  contiguous arena with `VirtualAlloc`, keeping the GameCube's 24 MB budget
  plus headroom. Place it below 2 GB if possible: either pass a hint address,
  or keep it small and use `s32` offsets. Make the `OSRound*` macros use
  `uintptr_t` under `SMS_PORT`. A single small arena also makes every
  heap-relative `u32` difference above, and the ResTIMG deltas in section 1,
  safe.
- **Memory budget.** Many structs grow on x64 (pointer members, vtable
  pointers, `JKRAMCommand`, J3D packets). Scene heaps sized for the GameCube
  (for example `JKRExpHeap::create(0x...)` in the directors) may run out.
  Scale those sizes on PC (for example ×2) rather than changing game logic.

## 5. Graphics-side addresses (TODO with milestone 5)

GX commands encode **32-bit physical addresses**:
- `J3DLoadArrayBasePtr` writes `(u32)ptr & 0x7fffffff` (`J3DTevs.cpp:44-50`).
- `J3DGDSetTexImgPtr` / `J3DGDLoadTlut` write `OSPhysicalToCached(image) >> 5`
  into display lists (`JRenderer.cpp:279-320`).
- `GDSetArray` is called from `J3DShape.cpp:198`.
- Material display lists are patched with texture addresses at runtime.

A 64-bit address cannot round-trip through these. With Aurora, route them
through Aurora's pointer-taking APIs: `GXSetArray` and `GXInitTexObj` with a
real pointer, instead of raw BP/CP register writes into a DL. Alternatively,
keep a small table of registered buffers and encode an index in the DL.
`JUTException.cpp:193-316` reads GC-only fixed addresses (`0x800030C8`) and
should be compiled out on PC.

---

## Recommended order of work

1. ~~Heap (section 4).~~ Done (budget scaling still open).
2. ~~RARC `SDIFileEntry` (section 1).~~ Done, plus the ResTIMG offsets.
   Related port fix found while testing: the UCRT `_tolower` macro is a bare
   `c - 'A' + 'a'`. `port_compat.h` now maps it to `tolower`, as MSL does, so
   `JKRFileLoader::fetchVolumeName` no longer breaks every `"/volume/..."`
   lookup.
3. J3D model/material/shape/joint blocks, then the anim loaders (section 1).
   Do this together with the big-endian swap work (milestone 4), because both
   touch the same loaders.
4. Collision and pollution patching (section 1).
5. SPC VM pointer handles (section 3).
6. JAudio address space (section 3, milestone 6) and GX addresses (section 5,
   milestone 5).

## Merged notes from worker (w6)

3. ~~J3D model/material/shape/joint blocks, then the anim loaders (section 1).~~
   **Done** together with the J3D swap passes (`port_j3d_swap.h`, ENDIAN.md);
   tested by `port/tools/test_j3d.py` on 480 models and 1552 animations.
   Not covered: the GX-side addresses of section 5 (milestone 5).

## Merged notes from worker (w6)

## 6. Re-scan status (`port/tools/find_ptr32.py`)
`python port/tools/find_ptr32.py [filter] [-o out.txt]` compiles every census
unit with `-Wshorten-64-to-32 -Wpointer-to-int-cast -Wint-to-pointer-cast
-Wvoid-pointer-to-int-cast -Wint-to-void-pointer-cast`, drops `port/` and
`libs/dolphin/`, and de-duplicates by file and line.
Result: 201 hits before this pass, 179 after (port/dolphin hits excluded).
Triage of the 179 that remain (counts are approximate, grouped by cause):
- **Harmless, about 140.**
  - 88 `J3DAnmLoader.cpp` `(void*)s32` offsets: correct through the `const void*` overload of `JSUConvertOffsetToPtr`.
  - 46 more, of three kinds:
    - Differences within one buffer or heap, narrowed to `u32`/`s32`/`int`: `JKR*Heap`, `JKRThread`, the `J3DModelLoader` reads, the JKR archive and ripper code, `J3DMaterial/Packet/GDUtil` display-list sizes, `JUTGamePad`, `BathWaterManager`, `Option` and `JASHeapCtrl` pointer differences.
    - `size_t` lengths: `strlen`, `sizeof`, `getDataSize`, `JUTFont::drawString`.
    - Genuinely 32-bit values: `MarDirector::getRestTime` (`s64` ticks, small ms), `J2DPane::setUserInfo(u64)` into a `u32` tag, the `ResTIMG` deltas, `JKRDvdFile` message round trip, and `mEntryNum` (`intptr_t` holding an entry number).
- **Real, fixed here (22 hits removed from the list).** Event VM (see section 3), `getWaterID`, `ymap.ymp`, and `TMapCollisionData::updateCheckListNode` (`MapMakeList.cpp`: `u32 offset = (u32)l - (u32)r` then `(char*)p - offset` wraps to ~4 GB when `l < r`; now `intptr_t` on PC).
- **Real, owned by others, about 30 hits.**
  - Collision (w5): `MapCollisionEntry.cpp:83-91`, 4 hits.
  - GX addresses (w8): `J3DTevs.cpp:49`, `JRenderer.cpp:283/319` (2 hits each), `DrawUtil`-style material IDs, 5 hits in total.
  - Audio ARAM: `JASDSPInterface.cpp:75/76/170/181`, `JASHeapCtrl.cpp:42/88`, `JASBankMgr.cpp:161`, `JASSeqParser.cpp:203/657`, `JASSystemHeap.cpp:57`, `JASWaveArcLoader.cpp:22`, `MSound.cpp:103`, `dsptask.c:649/652`, `osdsp_task.c:130-135`, `JASBNKParser.cpp:51/61`, and so on.
- **Real, GameCube-only code, 4 hits.** `JUTException.cpp:193/248/261/316` cast fixed GameCube addresses (`0x800030C8` and so on). The crash handler never runs on PC; compile it out when `JUTException` is wired up (section 5).
Remaining open list: only the owned areas above (collision, GX, audio ARAM) and the GameCube-only `JUTException` hits.
The `AlphaDebug.hpp:264` debug offset (section 2) is not flagged by clang and is still open.

## Merged notes from worker (w5)

- **JPA**: `JPABaseShape.cpp:214` and others parse by byte offsets (and read the big-endian fields through `JPA_RD_S16/F32/U32`, see ENDIAN.md; the loader's block walk used to fail on PC because `'JEFF'` compared against a little-endian read).
4. ~~Collision and pollution patching (section 1).~~ **Done** (native copies of the groups / layer infos; tests `test_col.py`, `test_ymp.py`; also `.ral` rails and `.jpa` particles, see ENDIAN.md "Collision, particles, rails and pollution status").
