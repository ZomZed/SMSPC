# Endianness plan (milestone 4)

All game data on disc is big-endian.
The PC build is little-endian (i686 today, x86_64 next).
The code reads most data in place, through structs, `JSUConvertOffsetToPtr` and `JSUMemoryInputStream`.
This file lists every loader that interprets file bytes, says how each one reads its data, and recommends a fix.

Helpers live in `port/include/port_endian.h`.
`port_compat.h` includes it, so shared code can call them under `#ifdef SMS_PORT` without adding an include.

## Approach

1. **Streams:** typed reads are byte-swapped under `SMS_PORT`. *Done.*
   This covers `JSUInputStream::readU16/S16/U32/S32/U64/F32`, `read16b/read32b`, the typed `read(T&)` overloads, the `operator>>` overloads, the u16 length prefix in `read(char*)`/`readString()`, and `JSURandomInputStream::peekU32`.
   The untyped `read(void*, size)` stays a raw copy.
   Raw reads of multi-byte values therefore need a fix at each call site (listed below).
2. **In-place formats:** each one gets a `port_swap_<format>(void* data)` pass.
   The pass runs once, right after the file is loaded (or fetched from its archive) and before any offset→pointer conversion.
   It must be idempotent, or guarded by a marker, because some buffers get mounted or initialised twice.
   Use the magic as the marker where the format has one: after a swap the magic reads correctly through a native u32 (`'RARC'`, `'J3D2'`, `'JEFF'`, `'MESG'`).
   Formats patched in place (collision `WAS_PATCHED`, J3D) must swap before the patch and never after it.
3. **Leave payloads big-endian when the GPU layer wants them that way.**
   Texture pixels, palettes, GX display lists and vertex arrays go to GX untouched.
   Whether to swap them depends on the GX backend (milestone 5):
   Aurora's GX reads GameCube-order data for textures and display lists.
   Decide this before writing the J3D pass, so vertex arrays are swapped once at most.
4. **64-bit:** the port is moving to x86_64.
   Formats marked **ptr** below store pointers inside the loaded file (offset→pointer patched in place, or a runtime pointer slot in an on-disc struct).
   On a 64-bit build they need wider structs or side tables, on top of the byte swap.
   Formats that only *compute* `base + offset` into separate variables are pointer-size clean.

## Loader survey

Legend: **stream** = read through `JSUInputStream`, fixed by layer 1. **in-place** = needs a swap pass. **bytes** = assembles big-endian values byte by byte, already endian-safe. **ptr** = see the 64-bit note above.

### Kernel (layer 1, done)

| Format | Entry | Access | Status |
|---|---|---|---|
| Yaz0 `.szs` | `JKRDecomp::decodeSZS`; `decompSZS_subroutine` in `JKRDvdRipper.cpp`, `JKRDvdAramRipper.cpp`, `JKRAram.cpp`; `JKRDecompExpandSize` | header read as `*(u32*)` / `SYaz0Header::length`; payload bytes | **Done:** header read with `sms_be32` under `SMS_PORT`. `decodeSZP` (Yay0) and `JKRDecompExpandSize` were already byte-wise. |
| RARC `.arc` / `.szs` | `JKRMemArchive::open` (both overloads), `JKRDvdArchive::open`, `JKRCompArchive::open` (NONE/YAZ0 and YAY0 paths), `JKRAramArchive::open` | in-place header, info block, node table, file entries | **Done:** `port_swap_rarc_header` / `port_swap_rarc_info` / `port_swap_rarc` run right after each load. The header swap is idempotent (keyed on the magic). File contents are left alone. **ptr:** `SDIFileEntry::mData` (+0x10) is a runtime pointer slot inside the 0x14-byte on-disc entry. On x86_64 `sizeof(SDIFileEntry)` becomes 0x18 and `mFileEntries + i` goes wrong, so this needs a side array of data pointers. |
| `JSUInputStream` family | `JSUInputStream.hpp`, `JSUInputStream.cpp`, `JSURandomInputStream.hpp` | stream | **Done** (see Approach 1). |

### Stream-based (fixed by layer 1)

- **JDrama scene graph** (`scene.bin`, `tables.bin`, `PerformLists.bin`, `stageArc.bin`): `TNameRefGen::load`, `TNameRef::getType/genObject/load`, and every `load(JSUMemoryInputStream&)` override in `libs/JSystem/src/JDrama` and `src/` (actors, managers, MapObj*, NPC, Enemy, Camera, etc.).
- **J2D `.blo` screens:** `J2DScreen::makeHierarchyPanes`, and the `J2DPane`/`J2DPicture`/`J2DTextBox`/`J2DWindow` stream constructors. Their textures and fonts are in-place resources (see below).
- **`.prm` params:** `TParams::load(JSUMemoryInputStream&)` (count, key code, name). **Exception:** `TParamT<T>::load` (`src/System/ParamInst.cpp`) reads the value with a raw `read(&value, sizeof(T))`. It needs a per-type swap (u8 none, s16, s32, f32, `TVec3<f32>` = 3 × f32).
- **Collision grid header:** `TMapCollisionData::init` (`operator>>`).
- **BMG text, partly:** `MessageUtil.cpp` (`readS32/readU16`), `MessageLoader.cpp:104`, `Talk2D2` tag parser (`readU16`).
- **JPA fields:** `JPAField.cpp` typed reads.

### In-place (need a swap pass), in priority order for the title screen

The boot path is: logo (`GCLogoDir`) → `common.szs`/`mario.arc` mounts → title `TMarDirector` setup with scene.bin, 2D screens, BMG and models.

1. **ResTIMG `.bti`** (`JUTTexture::storeTIMG`, `J2DPicture`, `GCLogoDir.cpp:91`): 0x20-byte header (u16 width/height, u16 palette count, u32 palette/image offsets, mip fields). `port_swap_timg`. Texel/palette payload stays as GX expects. Pointer-size clean: offsets are computed. Embedded TIMGs inside BMD TEX1 and `.bfn` use the same header.
2. **ResFONT `.bfn`** (`JUTResFont::protected_initiate`, `countBlock`, `setBlock`): FONT header plus INF1/WID1/GLY1/MAP1 blocks, addressed through block pointers into the file. `port_swap_font`, walking blocks by their (swapped) size. Clean: pointers are kept in separate arrays.
3. **BMG `.bmg`** (`TMessageLoader` / `MessageUtil` / `Talk2D2::loadMessageData`): the INF1 entry table and DAT1 offsets are read in place after the stream header. `port_swap_bmg` (INF1 entries u32 offset + u16 attributes; DAT1 text is bytes, and inline control-tag u16s are read via `readU16` = stream). Keep the stream reads and swap only the in-place tables, or the pass would double-swap.
4. **J3D `.bmd/.bdl`** (`J3DModelLoader::load` → `readInformation/readJoint/readMaterial(_v21)/readShape/readTexture/readTextureName/readVertex/readEnvelop/readDraw`, `J3DMaterialFactory(_v21)`, `J3DShapeFactory`, `J3DJointFactory`, `JUTNameTab`): block headers, index tables, init-data structs (u16/s16/u32/f32), name tables. `port_swap_j3d`, walking blocks by magic with a per-block layout table. **ptr:** `JSUConvertOffsetToPtr` (25 + 29 + 26 + 7 + 2 call sites) patches offsets into pointer fields of the on-disc structs. On 64-bit those fields cannot hold a pointer; convert into side structures instead.
5. **J3D animations** `.bck/.btk/.brk/.bpk/.btp/.bva/.blk` (`J3DAnmLoaderDataBase::load`, `J3DAnmFullLoader_v15`, `J3DAnmKeyLoader_v15`): header plus key/value tables (s16, f32, u16 indices). `port_swap_j3danm`. **ptr:** 82 `JSUConvertOffsetToPtr` sites. `J3DClusterLoader` (`.bls`, 3 sites) is the same kind.
6. **Scene `stageArc.bin` / `tables.bin` extras:** mostly stream (see above). Any struct-cast access found during bring-up goes here.
7. **Collision `.col`** (`TMapCollisionBase::init`, `MapCollisionEntry.cpp`): header (vertex count/offset, group count/offset), `Vec` vertices (f32), groups with s16 index lists, u8 sound materials and s16 extra data. `port_swap_col`, run before the `WAS_PATCHED` check and skipped when the flag is already set. **ptr:** the group structs get offsets replaced by pointers in place (`mIndices`, `mSoundMaterials`, `unk10`, `mAdditionalDatas`).
8. **JPA particles `.jpa`** (`JPAEmitterLoaderDataBase::load`, `JPAEmitterLoader_v10::load`; blocks BEM1/BSP1/ESP1/SSP1/ETX1/FLD1/KFA1/TEX1 read by `JPADataBlock`, `JPABaseShape`, `JPAExtraShape`, `JPASweepShape`, `JPAExTexShape`, `JPAKeyFrameAnime`, `JPATextureResource::registration`): `*(u32*)` block walk plus in-place block structs. `port_swap_jpa`. The JPAField typed reads are stream-based, so exclude FLD1 from the pass or switch it to raw. Clean as far as surveyed (objects keep pointers into the data, nothing is written back); re-check during bring-up.
9. **JAudio data** (`port/include/port_jaudio_swap.h`, tested by `python port/tools/test_jaudio.py`). *Done*, see "JAudio status" below.
   **Sequences** (`JASSeqCtrl::read16/read24/read32`, `JASSeqParser`) are **bytes** and already safe, with two exceptions (oscillator envelope tables and the FIR filter table embedded in sequence data), also covered below.
   ADPCM wave data in ARAM is consumed by the DSP replacement (milestone 6), which decodes it big-endian.
10. **THP video:** the `THPPlayer` library is replaced on the host (census skips it), so the host decoder reads the THP header big-endian itself.

### Native-order data (keep raw)

- **Save data** (`TFlagManager::save/load/saveOption/loadOption`, `TCardManager`): written and read with raw `write/read`, so it stays in native order.
  One spot read a native header through typed reads (`TCardManager` preview, `CardManager.cpp:175`). It is now read raw under `SMS_PORT`, so PC saves round-trip.
  To load GameCube `.gci` saves, the save writer/reader must go big-endian instead. That decision belongs to milestone 7.
- `JSUMemoryOutputStream` has no typed writes (only `operator<<(u8)` and raw `write`), so nothing written at runtime is byte-swapped.

### Raw `read(&x, sizeof(x))` on file data (need per-site swaps)

- `TParamT<T>::load` (`src/System/ParamInst.cpp`), as noted above.
- Before a scene loader goes live, audit it for `read(&v, 2|4)` on big-endian file data with `grep "\.read(&"`.

## Pass status (title-screen priority list)

| Format | Entry | Called from | Status |
|---|---|---|---|
| `.prm` raw value | `sms_swap_param_value` | `TParamT<T>::load` (`src/System/ParamInst.cpp`) | **Done**, tested on all 342 `.prm` in `params.szs` (u8, s16, s32/f32, `TVec3<f32>`). |
| Other raw stream reads | `sms_swap32/16_inplace`, `sms_swap_elems` | `J2DScreen::makeHiearachyPanes` (SCRN magic/size/tag peeks), `J2DPane` (kind, tag), `TViewport::load`, `TWireTrap::load`, `TEnemyMario` link data (node count), `JPAEmitter` base-shape scale/trans/rot/direction | **Done** (found by grepping `read(&` / `peek(`). Other raw `read(&` hits are native-order save data or single bytes. |
| ResTIMG `.bti` | `port_swap_timg` (marker byte 0x19 = 0x5A, 0 on disc in all 374 shipped files) | `JKRArchive::getResource` x4 and `readResource` x4 via `port_swap_resource` (by `.bti` extension, covers `JKRGetResource`, J2D `'TIMG'` lookups, direct users such as BathWaterManager); J3D `TEX1` via `port_swap_tex1_timgs` in `J3DModelLoader::readTexture/readTextureTable`; JPA via `JPATexture` ctor | **Done**, header only (texels/palettes stay GX order). Native headers built at run time (`JUTTexture(w,h,fmt)`) set the marker. Copies made by `J3DTexture::setResTIMG` keep it. |
| ResFONT `.bfn` | `port_swap_font` (idempotent on the `'FONT'` magic) | `port_swap_resource` (`.bfn`), and `JUTResFont::protected_initiate` as the catch-all | **Done**: header, INF1, WID1, GLY1, MAP1 (all entries). Glyph pages stay GX order. |
| BMG `.bmg` | none (by design) | `TMessageLoader` (`loadMessageData`, `readHeader`, `parseBlock`, `readInfoBlock`) | **Done** differently: the BMG buffer **stays big-endian**. `SMSGetMessageData`, `Talk2D2` and the tag parser already read it through typed (swapping) stream reads, and DAT1 carries inline big-endian tag words, so an in-place swap would double-swap them. `TMessageLoader` had raw reads (header words, block magic and size, INF1 count, 12-byte entries); they now decode big-endian, and the entries it copies into its own table (`unk8[]`) are swapped there. DAT1 text is single-byte in US. MID1 is not used. |
| JAudio (IBNK, WSYS, JAI tables) | | | **Done**, see "JAudio status" below. |

Test: `python port/tools/test_archive.py [--target x86_64-pc-windows-msvc]` also links the real `MessageLoader.cpp`, `MessageUtil.cpp` and `ParamInst.cpp`, extracts `option/common/params/mario/title/game_6/guide/select.szs` with `dtk vfs cp`, and checks every `.bti` (374), the `TEX1` of 42 models, `test_fontex.bfn`, `loadmessage.bmg` (message 0x1D is "Select data.") and all `.prm` files.

Notes for the later passes:
- `port_swap_j3d` must not swap the 0x20-byte `ResTIMG` headers in `TEX1` itself (call `port_swap_timg`), or the marker check is bypassed.
- A future `port_swap_jpa` must leave the block data big-endian where `JPAEmitter`/`JPAField` read it through streams; `JPATexture` already swaps its embedded `ResTIMG` header.
- On 64-bit nothing here stores pointers in the file data: `JUTResFont` keeps block pointers in separate arrays, `J3DTexture` computes `base + imageDataOffset`.

## Layer 1 status

- `port/include/port_endian.h`: `sms_bswap16/32/64`, `sms_be16/32`, `sms_swap*_inplace`, `sms_swap_value`, `port_swap_rarc_header/info`, `port_swap_rarc`.
- Test: `python port/tools/test_archive.py [--target x86_64-pc-windows-msvc]`. It decodes `common.szs`, swaps the RARC, walks it with JKRArchive's lookup logic (hash check on every entry), and compares all 64 file sizes with `dtk vfs ls -r`. It also links the real `JSUInputStream.cpp`/`JSUMemoryStream.cpp` and checks the typed reads.

## JAudio status

All passes live in `port/include/port_jaudio_swap.h` (included by `port_compat.h`).
They never store a pointer into the file; offsets stay offsets and the parsers add the base at use (`TOffset<T>::ptr`, `JSUConvertOffsetToPtr`, both `intptr_t`-clean).
Shared sub-structures are swapped once through a per-pass visited map.

| Data | Where it is swapped | Pass |
|---|---|---|
| `mSound.aaf` command list (u32 stream) | `JAIBasic::checkInitDataOnMemory`, `SMS_PORT` version: every word is read with `sms_be32` and native tables are built (see below) | none needed |
| IBNK banks (BANK header, inst/osc/rand/sense/keymap/vmap, PERC/PER2 pmaps, env tables) | `JASystem::BankMgr::registBankBNK`, before `*(u32*)bank + 2` is read | `port_swap_ibnk` (idempotent, the `IBNK` magic is the marker) |
| WSYS wave systems (WINF archives and TWave, WBCT/SCNE/C-DF; C-EX and C-ST only count + offsets) | `JASystem::WaveBankMgr::registWaveBankWS`, before `getGroupCount` | `port_swap_wsys` (idempotent, `WSYS` magic) |
| SE / sequence / stream info tables (u16 header, `JAISoundInfo[]`) | after the table copy in `checkInitDataOnMemory`, and after `Dvd::loadFile` in `JAIData::initInfoDataWork` / `reloadInfoDataWork` | `port_swap_jaisoundtable(p, size)` (once) |
| FX scene table (`FxlineConfig_` x4 per scene) | after the copy in `checkInitDataOnMemory` | `port_swap_fxscene` (once) |
| BARC sequence archive header (`VLArc` / `VLArcEntry`) | after the copy in `checkInitDataOnMemory`, and in `Vload::initHeaderM` when it loads the file itself | `port_swap_barc` (once; names and magic stay bytes) |
| Stream list (`FabricatedStreamEntry`) | after the copy in `checkInitDataOnMemory` | `port_swap_stream_entries` (once) |
| `.bas` animation sound data (`JAIAnimeSoundData`, 8314 files in the game) | `JAIAnimeSound::initActorAnimSound`, the single entry point | `port_swap_bas` (idempotent through byte 7, which is 0 in every file; set to 0xA5) |
| Sequence-embedded oscillator envelope tables (`TTrack::oscSetupFull`, `oscSetupSimpleEnv`) | at the use site | `seq_osc_table_native` returns a cached native copy, revalidated against the raw bytes so a reused sequence buffer is not stale |
| Sequence-embedded FIR filter (`TOuterParam::setFirFilter`) | at the copy loop | `sms_be16` per element |

Not swapped, with the reason:
- ADPCM wave data (`.aw` files) and the AFC stream (`title.afc`): never read by the CPU, only DMA'd to ARAM / streamed.
  Their headers are consumed by the DSP (milestone 6).
  `JASHardStream` only opens and streams the file through the DVD stream API, so there is no in-place header.
- `JAInter::StreamLib` is not decompiled (stubbed); it receives `&FabricatedStreamEntry::unk20`, a block of u32s that is swapped.
- `mSound.asn` (`TAsnData::asnData`) is set but never read.
- IBNK header bytes `0x224..0x3B4` (`THeader::unk224`) are not used by the parser and are left alone.
- `TCtrlWave` entries beyond `unk0`, `TWave::unk24`, and the unused `INST` `unk0[8]`: never read.
- `JAIBasic::checkInitListFile` (`JaiInit.bld`): the file does not exist in SMS, so the u16 header read there is unreachable.

### Pointer-in-file-slot cases (64-bit)

The `.aaf` loader used to patch file slots with pointers.
On x86_64 those were wrong (a 12-byte on-disc `FabricatedBankEntry` is 24 bytes natively, the scene table's pointer array overran its own offsets, ...).
The `SMS_PORT` loader builds native arrays instead:
- `mBankList` / `mWaveBankList`: heap arrays of native entries, zero-terminated.
- `mSoundSceneList`: native `JAICategoryInfo*` array pointing into the copied table.
- `mSeqArchiveHeader` / `mStreamListHeader`: native headers.
- `FabricatedFxSceneTable` offsets stay offsets (`base + offset` at use in `JAIData::initData`).
- `JASystem::Vload::VLArc::unk8` is a runtime pointer slot inside the 0x20-byte on-disc header.
  It is a `u32` under `SMS_PORT` (never read; always set to null) so `&arc[i] + 1` keeps the 0x20 stride.
- `JAIData::FabricatedStreamEntry::unk20` is a `void*` in the decomp header but a file field.
  It is a `u32` under `SMS_PORT` so the entry stays 0x30 bytes.
- Still open: ARAM addresses held as `void*`/`u32` in `JASWaveArcLoader` and `THeap` (POINTERS.md section 3, milestone 6).

### Also fixed here

- `JASystem::Dvd::openDvd` fell off the end without a return (the PowerPC code left the `DVDOpen` result in r3).
  clang treats that as unreachable and ran into the following data (`finfo`), crashing JAudio init.
  Under `SMS_PORT` it now returns the `DVDOpen` / `DVDFastOpen` result.

## Merged notes from worker (w6)

   Texture pixels, palettes and GX display lists go to GX untouched:
   Aurora's GX reads GameCube-order data for textures and display lists
   (its FIFO is big-endian, `GXCallDisplayList` copies the DL into it as is).
   **Vertex arrays are the exception: the J3D pass swaps them to native order.**
   Decision, from Aurora's code (`port_deps/aurora`):
   - `GXSetArray(attr, data, size, stride, bool le)` (`lib/dolphin/gx/GXGeometry.cpp`) records an endianness flag per array,
     and the vertex shader fetches every component through `load_u16/load_u24/load_u32(.., le)` (`lib/gx/shader.cpp`), byte-swapping only when `le` is false.
     So Aurora reads either order; it does not require big-endian arrays.
   - The game reads and writes vertex arrays on the CPU: `J3DSkinDeform`, the `J3DCluster` deformers (write deformed positions/normals into buffers that are then drawn),
     `DrawUtil` (copies position arrays), `MapMirror` and `PollutionCount` (read positions).
     Keeping file arrays big-endian would need a swap at each of those sites, and CPU-made buffers would be native anyway.
   - So VTX1 arrays become native (per component type from the VTX1 format list: F32 as 4 bytes, U16/S16 and RGB565/RGBA4 as 2, RGBA6 as 3, 8-bit types untouched),
     and the GX layer (milestone 5) must set up every array with `le = true` (`GXSETARRAY(.., true)`, which is also Aurora's default).
     The index data inside the display lists stays big-endian, as Aurora's DL parser expects.
   **Done**: `port_swap_j3d_model` (pass status below); the offset members are `JSU_FILE_PTR` (POINTERS.md section 1).
   **Done**: `port_swap_j3d_anm` (pass status below). `J3DClusterLoader.cpp` here only holds explicit `JSUConvertOffsetToPtr` instantiations (no `.bls` loader is decompiled), covered by the `JSupport.hpp` fix.
9. **JAudio banks and wave systems** (`JASystem::BNKParser::createBasicBank`, `WSParser::createBasicWaveBank/createSimpleWaveBank`, `JASBankMgr`, `JASWaveBankMgr`, `JASWaveArcLoader`; `JAIData`/`JAIAsnData` init tables): offset-linked structs read through `TOffset<T>::ptr(header)`. `port_swap_ibnk`, `port_swap_wsys`, `port_swap_jaidata`. Clean: `ptr()` computes `base + offset` without writing it back.
   **Sequences** (`JASSeqCtrl::read16/read24/read32`, `JASSeqParser`) are **bytes** and already safe.
Test: `python port/tools/test_j3d.py` extracts `mario/common/yoshi/title` and the `dolpic0/bianco0/airport0/mamma0` scenes,
runs both J3D passes on every model (480: `.bmd` + `.bmt`) and animation (1552: `.bck/.btk/.brk/.bpk/.btp`; 20 empty `common/camera/dummy*.bck` are skipped),
and compares what the real `J3DJointFactory`/`J3DShapeFactory`/`J3DMaterialFactory`/`JUTNameTab`/`J3DAnmLoaderDataBase::load` and the file structs see after the swap
with values read big-endian by Python (`level_export/tools/bmd2obj.py` for models).
It checks counts (joints, materials, shapes, vertices per array, textures), every joint transform, per-material tables, every shape packet, every texture header and the animation tables and values,
plus idempotency, untouched display lists and texels, and key-frame ordering. All files match.
How the J3D passes find table lengths: tables have no stored length, so a table runs from its offset to the next larger offset in the same block header (or the block end), as `readVertex` and bmd2obj do.
When several header slots share one offset, the tool wrote empty tables where the next table starts, so the **last** slot in header order owns the table
(most models share MAT3 light/texGenNum, a few texCoord/texNo/tevOrder).
The MAT3 indirect slot instead uses `J3DMaterialFactory`'s own test (`(ind - name) > 4`); otherwise it holds the name table's offset.
Some shipped `.bmt` files declare more blocks than they contain; the passes clamp to the file size (the GC loader reads past the end there, but never writes).
- `port_swap_j3d_model` does not swap the 0x20-byte `ResTIMG` headers in `TEX1` itself (it calls `port_swap_tex1_timgs`), so the marker check holds.
- Milestone 5: `J3DShape`/`J3DLoadArrayBasePtr` array setup must pass `le = true` to Aurora's `GXSetArray` (vertex arrays are native after the J3D pass); display lists stay GC format.
  J3D is covered in `POINTERS.md` section 1 (offset members are `JSU_FILE_PTR`, the one in-place patch, `J3DAnmVtxColorIndexData`, now builds a native copy).

## Merged notes from worker (w5)

## Collision, particles, rails and pollution status (worker w5)
All passes live in `port/include/port_map_swap.h` (included by `port_compat.h`, self-contained C), or are accessors inside the JPA sources.
Each has a host test that extracts real files from `dolphin_test/files` with `dtk vfs cp` and drives the real game loaders.
Notes:
- `Map/MapCollisionData.cpp` is in `config/GMSE01/link_all_skip.txt` (the US build links the original object), but its source is complete for the PC build: `TMapCollisionData::init(stream)` (grid header, read through the swapping stream), `initGrid`, `initMoveCollision`, the constructor and the statics are all there, and the rest of `TMapCollisionData` lives in `MapMakeList.cpp`, `MapCheck.cpp` and `MapArea.cpp`, all of which the test links and runs. Gaps found: none that block PC. Two original quirks are kept as they are (no game-bug fixes): `loadCollisionData` does not use its `buffer` when the path already contains `.col`, and `TMapCollisionBase::update()` uses a 350-vertex stack buffer.
- `TMapCollisionBase::update` / `initCheckData` (without flag 4) put `350 * 3` floats on the stack, so a move collision whose `.col` has more than 350 vertices would overflow. Nothing in the game does this.
- Not done (still in-place on PC): SPC `.sb` event scripts (`EventWatcher.cpp:1427`, `TSpcBinary`), `.pad` replay data (`enemyMario.cpp:475/535/543`), `.bcr` rumble (`MovieRumble.cpp:24`), `stageArc.bin` / `tables.bin` struct-cast access beyond the stream loaders. `MarNameRefGen` and `TMapObjManager` read their tables through the swapping streams (no raw cast was found).

## Disc format inventory (worker w10)

`python port/tools/disc_inventory.py [--ext .bck] [--paths]` walks `dolphin_test/files` and every RARC in it (`dtk vfs ls -r`).
48318 files: 174 loose, 122 archives.
Every format the game reads, its loader, and its status on PC:

| Ext | Files | Loader | Status |
|---|---|---|---|
| `.bck` `.btp` `.btk` `.brk` `.bpk` | 11080, 8478, 950, 371, 126 | `J3DAnmLoaderDataBase::load` | Swapped (`port_swap_j3d_anm`). No `.bca/.blk/.bva/.bls` exist in SMS. 20 empty `common/camera/dummy*.bck` are only existence-checked. |
| `.bmd` `.bdl` `.bmt` | 8784, 1, 512 | `J3DModelLoaderDataBase::load/loadMaterialTable` | Swapped (`port_swap_j3d_model`). TEX1 headers are now swapped unconditionally inside that pass (see below). |
| `.bas` | 8476 | `JAIAnimeSound::initActorAnimSound` | Swapped (`port_swap_bas`). |
| `.col` | 4094 | `TMapCollisionBase::init` | Swapped + native groups. |
| `.jpa` | 2236 | `JPAResourceManager::load` | Big-endian accessors. Embedded TIMG pad byte is 0 in every file (checked), so its marker is safe. |
| `.bti` | 754 | `port_swap_resource` / J2D | Swapped; pad byte 0x19 is 0 in all 754 `.bti` on the disc (checked). |
| `.prm` | 501 | `TParams::load` / `TParamT<T>::load` | Stream + per-value swap. The 142 per-scene `map/params/**.prm` use the same loader. |
| `.me` | 494 | none | `delete.me` placeholders (6-7 bytes), never loaded. |
| `.pad` | 381 (20 scenes) | `TMarioInputReplay::init` (`TEnemyMario` replays: Shadow Mario chases, the dolpic1 run-away/gate replays) | **New:** `port_swap_pad` (`port/include/port_asset_swap.h`), idempotent (first table offset is 0x40). |
| `.bin` | 108 `scene.bin`, 108 `tables.bin`, `stageArc.bin`, `scenecmn.bin`, `PerformLists.bin` | JDrama `TNameRef::genObject` / `load(stream)` | Stream. No struct-cast access left (`grep read(&` re-checked: every multi-byte raw read is swapped or native save data). |
| `.bin` | 20 `pad*/linkdata.bin` | `TEnemyMario::initValues` | Stream + `sms_swap32_inplace(&nodeCount)`. |
| `.sb` | 151 | `TSpcBinary` | Swapped. |
| `.ral` | 108 | `TGraphGroup` | Swapped. |
| `.ymp` | 91 | `TPollutionManager::setDataAddress` | Big-endian reads. |
| `.bmp` | 92 | `TPollutionLayer`, `TMapObjPlane` | Bytes (the header is assembled byte by byte, pixels are u8). Safe. |
| `.bmg` | 191 | `TMessageLoader`, `SMSGetMessageData`, `Talk2D2` | Stays big-endian by design. |
| `.blo` | 23 | J2D | Stream. |
| `.bfn` | 4 | `JUTResFont` | Swapped. |
| `.bcr` | 9 (`subtitle.szs` rumble) | `Koga::ToolData::Attach` | Swapped (w7's `toolDataSwapToNative`). |
| `.aaf` `.arc` `.aw` `.afc` `.asn` | 1, 1, 24, 1, 1 | JAudio | See "JAudio status". |
| `.thp` | 21 | host THP player | w8. |
| `.bnr` | 1 | none | Not loaded by the game. |

Test: `python port/tools/test_assets.py` extracts all 381 `.pad` and 9 `.bcr`, plays every replay through the real `TMarioInputReplay` (init twice, then `play()` every frame, FNV hash of every frame's outputs) and reads every `.bcr` entry through the real `ToolData`, against a big-endian Python decode. All match.

### TEX1 pad-byte marker (fixed)
`port_swap_timg` uses the pad byte 0x19 as its "already swapped" marker (0x5A).
That holds for `.bti` and `.jpa` (always 0 on disc), but not inside J3D `TEX1` blocks: model headers carry tool garbage there (203 distinct values in 34 archives), and `coronaBoss` `map/map/water.bmd` texture 0 has 0x5A on disc, so it stayed big-endian (width read as 32770).
`port_swap_j3d_model` now calls `port_swap_tex1_timgs_force` (`port_endian.h`): it swaps every TEX1 header unconditionally, which is safe because the model pass itself runs once per file (file magic), and sets the marker for the later `readTexture` calls.
Found by running `test_j3d.py` on 34 archives (all levels plus `bosshanachanJpa/select/option/endsave/guide/game_6`): 1535 models and 3926 animations match after the fix.
The one remaining report, `pinnaBeach0/mapobj/basket.bck` "badkeys 4", is the test's monotonic-key heuristic tripping on the disc data itself (its Y translation keys are not time-ordered in the file); all values match.

## Merged notes from worker (w8)

- Milestone 5 (done, w8): `J3DShape`/`J3DLoadArrayBasePtr` array setup passes `le = true` and the VTX1 array size to Aurora (`port/include/port_gx.h`, AURORA.md "GX path"); display lists stay GC format.
