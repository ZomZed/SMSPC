# ctordiff candidates (from w11, for w13)

Generated 2026-10-08 17:29 with `python tools/ctordiff.py --rank` on main as of that time (GMSE01 build + build/GMSE01/report.json).
It lists every **NonMatching** function whose *store set* (offset + stored constant / relocated address) or *call set* differs from the GameCube asm, ignoring registers and scheduling.
Ranked by the number of differing events (score). `target-only` = in the GameCube code but not ours; `ours-only` = the reverse.
Events: `call X`; `stw/sth/stb/stfs/stfd +off = value` (non-stack stores; `?` = value not a known constant, `f` = FP store, `@sym` = relocated address); `fconst v` = a float literal loaded from .sdata2/.rodata.

**Ownership (split updated by the coordinator):**
- **w11:** `src/Player`, `src/Camera`, `src/MarioUtil`, `src/Strategic` (done, see BOOTLOG "Store/call audit (w11)") and now also **`src/MoveBG`, `src/NPC` and `src/Map`** (including Map/Pollution*, the water managers and graph/collision): the `MoveBG/`, `NPC/` and `Map/` entries below, plus w11's own `--rank` over those directories.
- **w13:** `src/Enemy`, `src/GC2D`, `src/MSound`, `src/System` and `src/Animal`: every other entry below.

How to read it (typical false positives):
- `call __ct__TVec3` / `set`/`scale`/`add`/`MsSqrtf` vs inline arithmetic: the GameCube calls a small inline that ours expands (or the reverse). Not a behaviour difference.
- The same field reached from a different base register shows up as two different offsets (for example `+0x384` vs `+0x714`).
- Different local-symbol numbering in vtable names (`@__vt__...` vs ``).
- Indexed stores (`stwx`) are not modelled.
Real differences: a missing call (sound, collision check), a different float literal (`fconst`), a missing or extra field store, or a store to a different offset with the same base.
The `--rank`/`fconst`/`-c` version of the tool is merged in `tools/ctordiff.py`.
Per-function detail: `python tools/ctordiff.py <unit> <function substring>` (`-c` also compares integer immediates).
###  60  Enemy/BossHanachanMain :: perform__13TBossHanachanFUlPQ26JDrama9TGraphics  (84.2%)
    target-only 3 call MsWrap<f>__Ffff
    target-only 2 fconst -180
    target-only 1 fconst 0
    target-only 3 fconst 180
    target-only 2 stfs +0x0 = f
    target-only 1 stfs +0x4 = f
    target-only 1 stfs +0x8 = f
    target-only 2 stw +0x0 = ?
    ours-only   1 fconst -200
    ours-only   1 fconst 360
    ours-only   1 stfs +0x10 = f
    ours-only   4 stfs +0x13c = f
    ours-only   1 stfs +0x14 = f
    ours-only   4 stfs +0x140 = f
    ours-only   4 stfs +0x148 = f
    ours-only   1 stfs +0x18 = f
    ours-only   2 stfs +0x38 = f
    ours-only   4 stw +0x124 = ?
    ours-only   4 stw +0x128 = ?
    ours-only   4 stw +0x12c = ?
    ours-only   4 stw +0x130 = ?
    ours-only   4 stw +0x134 = ?
    ours-only   4 stw +0x138 = ?
    ours-only   2 stw +0x64 = ?
###  56  System/MarNameRefGen :: InsertRaw__Q27JGadget66TVector<14TCameraMapTool,Q27JGadget28TAllocator<14TCameraMapTool>>FP14TCameraMapToolUl  (60.2%)
    target-only 2 sth +-0x28 = ?
    target-only 1 sth +-0x58 = ?
    target-only 1 sth +-0x88 = ?
    target-only 1 sth +-0xb8 = ?
    target-only 2 stw +-0x10 = ?
    target-only 2 stw +-0x14 = ?
    target-only 2 stw +-0x18 = ?
    target-only 2 stw +-0x1c = ?
    target-only 2 stw +-0x20 = ?
    target-only 2 stw +-0x24 = ?
    target-only 2 stw +-0x2c = ?
    target-only 1 stw +-0x34 = ?
    target-only 1 stw +-0x38 = ?
    target-only 1 stw +-0x3c = ?
    target-only 2 stw +-0x4 = ?
    target-only 1 stw +-0x40 = ?
    target-only 1 stw +-0x44 = ?
    target-only 1 stw +-0x48 = ?
    target-only 1 stw +-0x4c = ?
    target-only 1 stw +-0x50 = ?
    target-only 1 stw +-0x54 = ?
    target-only 1 stw +-0x5c = ?
    target-only 1 stw +-0x64 = ?
    target-only 1 stw +-0x68 = ?
    target-only 1 stw +-0x6c = ?
    target-only 1 stw +-0x70 = ?
    target-only 1 stw +-0x74 = ?
    target-only 1 stw +-0x78 = ?
    target-only 1 stw +-0x7c = ?
    target-only 2 stw +-0x8 = ?
    target-only 1 stw +-0x80 = ?
    target-only 1 stw +-0x84 = ?
    target-only 1 stw +-0x8c = ?
    target-only 1 stw +-0x94 = ?
    target-only 1 stw +-0x98 = ?
    target-only 1 stw +-0x9c = ?
    target-only 1 stw +-0xa0 = ?
    target-only 1 stw +-0xa4 = ?
    target-only 1 stw +-0xa8 = ?
    target-only 1 stw +-0xac = ?
    target-only 1 stw +-0xb0 = ?
    target-only 1 stw +-0xb4 = ?
    target-only 1 stw +-0xbc = ?
    target-only 2 stw +-0xc = ?
    ours-only   1 call __as__14TCameraMapToolFRC14TCameraMapTool
###  55  GC2D/GCConsole2 :: perform__11TGCConsole2FUlPQ26JDrama9TGraphics  (79.3%)
    target-only 10 call changeTexture__10J2DPictureFPC7ResTIMGUc
    target-only 1 call getFlag__12TFlagManagerCFUl
    target-only 1 call set__7JUTRectFiiii
    target-only 2 fconst 0.01
    target-only 2 fconst d4.5036e+15
    target-only 2 stb +0x0 = 0
    target-only 2 stb +0x1 = 0
    target-only 2 stb +0x2 = 0
    target-only 2 stb +0x3 = 0
    target-only 2 stb +0x4 = 0
    target-only 2 stb +0x5 = 0
    target-only 2 stb +0x6 = 0
    target-only 2 stb +0x7 = 0
    target-only 3 stb +0xc = 0
    target-only 3 stb +0xc = 1
    target-only 1 stfs +0x2b8 = f
    target-only 1 stw +0x18 = 9
    target-only 1 stw +0x274 = ?
    target-only 1 stw +0x288 = ?
    target-only 1 stw +0x2c = ?
    target-only 1 stw +0x6c = 999
    target-only 2 stw +0x6c = ?
    ours-only   5 call copy__7JUTRectFRC7JUTRect
    ours-only   2 call memset
    ours-only   1 call seek__20JSURandomInputStreamFl17JSUStreamSeekFrom
    ours-only   1 call setBlueCoinDigits<10TBlendPane>__FPP10TBlendPanePP10JUTTexturei
###  40  System/MarNameRefGen_MapObj :: getNameRef_MapObj__14TMarNameRefGenCFPCc  (97.5%)
    target-only 1 call __ct__11TMapObjBaseFPCc
    target-only 1 call __ct__11TTelesaSlotFPCc
    target-only 4 call __ct__12THideObjBaseFPCc
    target-only 1 call __ct__Q26JDrama10TFlagT<Us>FUs
    target-only 1 call __ct__Q26JDrama8TNameRefFPCc
    target-only 1 stw +0x0 = @__vt__11TGateShadow
    target-only 1 stw +0x0 = @__vt__12TFruitBasket
    target-only 1 stw +0x0 = @__vt__14TBasketReverse
    target-only 1 stw +0x0 = @__vt__15THipDropHideObj
    target-only 2 stw +0x0 = @__vt__16TFruitHitHideObj
    target-only 1 stw +0x0 = @__vt__16TWaterHitHideObj
    target-only 1 stw +0x0 = @__vt__Q26JDrama8TViewObj
    target-only 1 stw +0x150 = 0
    target-only 1 stw +0x20 = @__vt__12TFruitBasket+36
    target-only 1 stw +0x20 = @__vt__14TBasketReverse+36
    target-only 1 stw +0x20 = @__vt__15THipDropHideObj+36
    target-only 2 stw +0x20 = @__vt__16TFruitHitHideObj+36
    target-only 1 stw +0x20 = @__vt__16TWaterHitHideObj+36
    ours-only   1 call __construct_array
    ours-only   1 call __ct__11TGateShadowFPCc
    ours-only   1 call __ct__12TFruitBasketFPCc
    ours-only   1 call __ct__14TBasketReverseFPCc
    ours-only   1 call __ct__15THipDropHideObjFPCc
    ours-only   1 call __ct__16TFruitHitHideObjFPCc
    ours-only   1 call __ct__16TWaterHitHideObjFPCc
    ours-only   1 call __ct__9TSlotDrumFPCc
    ours-only   1 stb +0x19b = 0
    ours-only   1 stb +0x19c = 1
    ours-only   1 stb +0x1a8 = ?
    ours-only   1 stb +0x1a9 = ?
    ours-only   1 stb +0x1aa = ?
    ours-only   1 stb +0x1e0 = ?
    ours-only   1 stw +0x0 = @__vt__11TTelesaSlot
    ours-only   1 stw +0x1a4 = 2
    ours-only   1 stw +0x20 = @__vt__11TTelesaSlot+36
###  32  Enemy/Koopa :: perform__6TKoopaFUlPQ26JDrama9TGraphics  (88.9%)
    target-only 8 call set<f>__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
    ours-only   4 stfs +0x154 = f
    ours-only   4 stfs +0x158 = f
    ours-only   4 stfs +0x15c = f
    ours-only   4 stfs +0x174 = f
    ours-only   4 stfs +0x178 = f
    ours-only   4 stfs +0x17c = f
###  32  Enemy/limitkoopa :: perform__11TLimitKoopaFUlPQ26JDrama9TGraphics  (83.3%)
    target-only 8 call set<f>__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
    ours-only   4 stfs +0x154 = f
    ours-only   4 stfs +0x158 = f
    ours-only   4 stfs +0x15c = f
    ours-only   4 stfs +0x174 = f
    ours-only   4 stfs +0x178 = f
    ours-only   4 stfs +0x17c = f
###  31  GC2D/Guide :: perform__6TGuideFUlPQ26JDrama9TGraphics  (79.4%)
    target-only 3 call setPaneAlpha__7TExPaneFlss
    target-only 1 call setPaneOffset__7TExPaneFlllll
    target-only 1 call setPaneSize__7TExPaneFlllll
    ours-only   2 call setValue__8TCoord2DFlffff
    ours-only   1 fconst -12.75
    ours-only   2 fconst 0
    ours-only   1 fconst 12.75
    ours-only   1 fconst 255
    ours-only   1 fconst 4
    ours-only   1 fconst d4.5036e+15
    ours-only   1 stb +0x4e = ?
    ours-only   1 stb +0x4f = 1
    ours-only   1 stb +0x50 = 1
    ours-only   2 stb +0x50 = ?
    ours-only   2 stb +0xcc = 0
    ours-only   1 stb +0xcc = 255
    ours-only   3 stfs +0x44 = f
    ours-only   3 stfs +0x48 = f
    ours-only   1 sth +0x4c = 0
    ours-only   1 sth +0x4c = 255
    ours-only   1 sth +0x4c = 80
###  28  Animal/Bird :: init__11TAnimalBirdFP12TLiveManager  (8.9%)
    target-only 1 call initParams__11TAnimalBirdFv
    ours-only   1 call __ct__7TBinderFv
    ours-only   1 call __nw__FUl
    ours-only   1 call init__11TWireBinderFRCQ29JGeometry8TVec3<f>
    ours-only   1 call isOnWire__11TWireBinderFRCQ29JGeometry8TVec3<f>
    ours-only   1 call rand
    ours-only   1 fconst 0.1
    ours-only   1 fconst 0.5
    ours-only   2 fconst 1
    ours-only   1 fconst 3.05176e-05
    ours-only   1 fconst 90
    ours-only   1 fconst d4.5036e+15
    ours-only   1 stb +0x13c = 1
    ours-only   1 stfs +0x158 = f
    ours-only   2 stfs +0x15c = f
    ours-only   1 stfs +0x160 = f
    ours-only   1 stfs +0x164 = f
    ours-only   1 stfs +0x168 = f
    ours-only   1 stfs +0x16c = f
    ours-only   1 stfs +0x170 = f
    ours-only   1 stfs +0x174 = f
    ours-only   1 stw +0x0 = @__vt__11TWireBinder
    ours-only   1 stw +0x154 = ?
    ours-only   1 stw +0x178 = 0
    ours-only   1 stw +0x17c = 0
    ours-only   1 stw +0xf0 = ?
###  26  System/MarNameRefGen_Enemy :: getNameRef_Enemy__14TMarNameRefGenCFPCc  (97.8%)
    target-only 1 call __ct__16TNameKuriManagerFPCc
    target-only 1 call __ct__18TAnimalManagerBaseFPCc
    target-only 1 call __ct__9TLauncherFPCc
    target-only 2 call __ct__Q26JDrama6TActorFPCc
    target-only 2 call __ct__Q29JGeometry64TRotation3<Q29JGeometry38TMatrix34<Q29JGeometry13SMatrix34C<f>>>Fv
    target-only 2 stb +0x44 = 1
    target-only 1 stw +0x0 = @__vt__11TMewManager
    target-only 2 stw +0x0 = @__vt__13TSimpleEffect
    target-only 1 stw +0x0 = @__vt__17TNameKuriLauncher
    target-only 1 stw +0x0 = @__vt__18TEffectPinnaFunsui
    target-only 1 stw +0x0 = @__vt__19TEffectBiancoFunsui
    target-only 1 stw +0x0 = @__vt__25TDiffusionNameKuriManager
    target-only 2 stw +0x20 = @__vt__13TSimpleEffect+36
    target-only 1 stw +0x20 = @__vt__17TNameKuriLauncher+36
    target-only 1 stw +0x20 = @__vt__18TEffectPinnaFunsui+36
    target-only 1 stw +0x20 = @__vt__19TEffectBiancoFunsui+36
    ours-only   1 call __ct__11TMewManagerFPCc
    ours-only   1 call __ct__17TNameKuriLauncherFPCc
    ours-only   1 call __ct__18TEffectPinnaFunsuiFPCc
    ours-only   1 call __ct__19TEffectBiancoFunsuiFPCc
    ours-only   1 call __ct__25TDiffusionNameKuriManagerFPCc
###  20  JSystem/JParticle/JPAEmitter :: createParticle__14JPABaseEmitterFv  (96.9%)
    target-only 1 stb +0x17c = 0
    target-only 1 stb +0x17c = 1
    target-only 3 sth +0x174 = ?
    target-only 1 sth +0x176 = 0
    target-only 1 sth +0x17a = ?
    target-only 1 stw +0x16c = 0
    target-only 1 stw +0x170 = 1
    target-only 1 stw +0x170 = ?
    ours-only   1 stb +0x0 = 0
    ours-only   1 stb +0x0 = 1
    ours-only   1 sth +0x0 = 0
    ours-only   4 sth +0x0 = ?
    ours-only   1 stw +0x0 = 0
    ours-only   1 stw +0x0 = 1
    ours-only   1 stw +0x0 = ?
###  20  MoveBG/ModelGate :: perform__10TModelGateFUlPQ26JDrama9TGraphics  (98.0%)
    target-only 2 stb +0x0 = 8
    target-only 8 stb +0x0 = ?
    ours-only   1 stb +0x11 = ?
    ours-only   1 stb +0x41 = 8
    ours-only   1 stb +0x5 = 8
    ours-only   3 stb +0x5 = ?
    ours-only   1 stb +0x6 = ?
    ours-only   1 stb +0x7 = ?
    ours-only   2 stb +0x8 = ?
###  19  Map/MapMirror :: perform__19TMirrorModelManagerFUlPQ26JDrama9TGraphics  (53.7%)
    target-only 1 call C_MTXLookAt
    target-only 1 call __ct__Q29JGeometry8TVec3<f>Fv
    target-only 6 call dot__Q29JGeometry8TVec3<f>CFRCQ29JGeometry8TVec3<f>
    target-only 3 call scaleAdd__Q29JGeometry8TVec3<f>FfRCQ29JGeometry8TVec3<f>RCQ29JGeometry8TVec3<f>
    target-only 2 call set<f>__Q29JGeometry8TVec3<f>Ffff
    target-only 5 fconst -2
    ours-only   1 call makeMirrorViewMtx__13TMirrorCameraFv
###  18  Enemy/bosseel :: perform__8TBossEelFUlPQ26JDrama9TGraphics  (97.2%)
    target-only 1 call begin__Q27JGadget38TVector<Pv,Q27JGadget14TAllocator<Pv>>Fv
    target-only 1 stfs +0x10 = f
    target-only 1 stfs +0x14 = f
    target-only 2 stfs +0x24 = f
    target-only 2 stfs +0x28 = f
    target-only 2 stfs +0x2c = f
    target-only 1 stfs +0xc = f
    ours-only   2 stfs +0x0 = f
    ours-only   3 stfs +0x4 = f
    ours-only   3 stfs +0x8 = f
###  18  NPC/NpcParts :: __ct__9TNpcPartsFUlPC13J3DGXColorS10P8TBaseNPC  (88.0%)
    target-only 1 stw +0x10 = 0
    target-only 1 stw +0x14 = 0
    target-only 1 stw +0x18 = 0
    target-only 1 stw +0x1c = 0
    target-only 1 stw +0x4 = 0
    target-only 1 stw +0x8 = 0
    target-only 1 stw +0xc = 0
    ours-only   5 stw +0x0 = ?
    ours-only   6 stw +0x30 = ?
###  16  System/MarNameRefGen_BossEnemy :: getNameRef_BossEnemy__14TMarNameRefGenCFPCc  (92.9%)
    target-only 1 call __ct__11TSpineEnemyFPCc
    target-only 1 call __ct__13TEnemyManagerFPCc
    target-only 1 call __ct__27TDemoBossHanachanSaveParamsFPCc
    target-only 1 call __nw__FUl
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
    target-only 1 fconst 0
    target-only 1 stw +0x0 = @__vt__17TDemoBossHanachan
    target-only 1 stw +0x0 = @__vt__18TSleepBossHanachan
    target-only 1 stw +0x0 = @__vt__24TDemoBossHanachanManager
    target-only 1 stw +0x0 = @__vt__25TSleepBossHanachanManager
    target-only 1 stw +0x15c = 0
    target-only 1 stw +0x20 = @__vt__17TDemoBossHanachan+36
    target-only 1 stw +0x20 = @__vt__18TSleepBossHanachan+36
    target-only 1 stw +0x54 = ?
    ours-only   1 call __ct__18TSleepBossHanachanFPCc
    ours-only   1 call __ct__25TSleepBossHanachanManagerFPCc
###  15  NPC/NpcBase :: perform__8TBaseNPCFUlPQ26JDrama9TGraphics  (63.4%)
    target-only 2 call execMotionBlend___8TBaseNPCFv
    target-only 1 call getAnmOffDist___8TBaseNPCFv
    target-only 1 call isPartsAnmNpc__8TBaseNPCCFv
    target-only 1 call isPollutionNpc__8TBaseNPCCFv
    ours-only   2 call execMotionBlend__13TNpcInbetweenFP6MActor
    ours-only   1 call isNerveMaybeDontCalcAnim0__8TBaseNPCCFv
    ours-only   1 call isNerveMaybeDontCalcAnim1__8TBaseNPCCFv
    ours-only   2 call setKeepAnm___8TBaseNPCFv
    ours-only   2 fconst 0
    ours-only   2 stw +0x0 = -1
###  13  Enemy/Koopa :: execute__16TNerveKoopaFlameCFP24TSpineBase<10TLiveActor>  (83.9%)
    target-only 4 call getSaveParam__13TEnemyManagerCFv
    target-only 1 call getTargetDir__6TKoopaCFRCQ29JGeometry8TVec3<f>
    target-only 4 call set<f>__Q29JGeometry8TVec3<f>Ffff
    ours-only   1 call calcKeyCode__Q26JDrama8TNameRefFPCc
    ours-only   1 call matan__Fff
    ours-only   1 fconst 0.00549316
    ours-only   1 fconst d4.5036e+15
###  13  Map/BathWaterManager :: render__22TBathWaterMeshRendererFPQ26JDrama9TGraphicsRC12TBathtubDataPP10TBathWaterPP16TBathWaterParamsi  (93.8%)
    target-only 1 call drawCap__FRCQ29JGeometry8TVec3<f>f
    ours-only   1 call GXBegin
    ours-only   2 call cosf
    ours-only   1 call sinf
    ours-only   1 fconst 0
    ours-only   1 fconst 0.20944
    ours-only   1 fconst 0.5
    ours-only   2 stb +-0x8000 = ?
    ours-only   3 stfs +-0x8000 = f
###  12  Enemy/BathtubKiller :: perform__14TBathtubKillerFUlPQ26JDrama9TGraphics  (81.3%)
    target-only 1 call theNerve__28TNerveBathtubKillerExplosionFv
    target-only 5 stw +0x0 = ?
    ours-only   1 call __register_global_object
    ours-only   1 stw +0x208 = ?
    ours-only   1 stw +0x20c = ?
    ours-only   1 stw +0x210 = ?
    ours-only   1 stw +0x214 = ?
    ours-only   1 stw +0x218 = ?
###  12  Enemy/enemyMario :: consider__11TEnemyMarioFv  (94.1%)
    target-only 1 call dot__Q29JGeometry8TVec3<f>CFRCQ29JGeometry8TVec3<f>
    target-only 2 call getGraph__12TGraphTracerCFv
    target-only 1 call getPoint__9TPathNodeCFv
    target-only 4 fconst 0
    ours-only   4 fconst 64
###  12  Enemy/wireTrap :: checkHitActors__9TWireTrapFv  (87.2%)
    target-only 4 call getWireDir__9TWireTrapCFv
    ours-only   4 call getDir__11TWireBinderCFv
    ours-only   4 call getWireBinder__9TWireTrapCFv
###  12  GC2D/SelectMenu :: perform__11TSelectMenuFUlPQ26JDrama9TGraphics  (90.1%)
    target-only 4 call SMS_getShineID__FUlUlb
    target-only 8 call set__Q28JUtility6TColorFUcUcUcUc
###  12  MoveBG/MapObjPlane :: calcNrm__12TMapObjPlaneFii  (55.3%)
    target-only 4 call dot__Q29JGeometry8TVec3<f>CFRCQ29JGeometry8TVec3<f>
    target-only 4 call scale__Q29JGeometry8TVec3<f>FfRCQ29JGeometry8TVec3<f>
    target-only 4 fconst 0
###  11  Enemy/bossgesso :: doAttackSingle__10TBossGessoFv  (89.6%)
    target-only 2 call matan__Fff
    target-only 1 fconst -90
    target-only 1 fconst 0
    target-only 2 fconst 0.00549316
    target-only 1 fconst 180
    target-only 1 fconst 90
    target-only 2 fconst d4.5036e+15
    ours-only   1 call MsGetRotFromZaxisY__FRCQ29JGeometry8TVec3<f>
###  11  MoveBG/MapObjMonte :: control__13TFluffManagerFv  (99.3%)
    target-only 2 stfs +0xd0 = f
    target-only 2 stfs +0xd4 = f
    target-only 2 stfs +0xd8 = f
    ours-only   1 stfs +0x0 = f
    ours-only   2 stfs +0x4 = f
    ours-only   2 stfs +0x8 = f
###   9  Enemy/Koopa :: execute__15TNerveKoopaWaitCFP24TSpineBase<10TLiveActor>  (84.9%)
    target-only 2 call getSaveParam__13TEnemyManagerCFv
    target-only 1 call getTargetDir__6TKoopaCFRCQ29JGeometry8TVec3<f>
    target-only 2 call set<f>__Q29JGeometry8TVec3<f>Ffff
    ours-only   1 call calcKeyCode__Q26JDrama8TNameRefFPCc
    ours-only   1 call matan__Fff
    ours-only   1 fconst 0.00549316
    ours-only   1 fconst d4.5036e+15
###   8  Enemy/BathtubKiller :: isCollidMove__14TBathtubKillerFP9THitActor  (72.8%)
    target-only 3 call theNerve__28TNerveBathtubKillerExplosionFv
    ours-only   2 call __ct__24TNerveBase<10TLiveActor>Fv
    ours-only   3 call __register_global_object
###   8  Enemy/bossgesso :: execute__12TNerveBGRollCFP24TSpineBase<10TLiveActor>  (92.8%)
    target-only 8 call getMActorAnmData__13TMActorKeeperCFv
###   8  Enemy/enemy :: goToExclusiveNextGraphNode__11TSpineEnemyFv  (87.5%)
    target-only 2 call __as__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
    ours-only   1 stw +0x100 = ?
    ours-only   1 stw +0x108 = ?
    ours-only   1 stw +0x10c = ?
    ours-only   1 stw +0x110 = ?
    ours-only   1 stw +0xf8 = ?
    ours-only   1 stw +0xfc = ?
###   6  Animal/AnimalBase :: resetRandomCurPathNode__11TAnimalBaseFv  (96.5%)
    target-only 1 stw +0x100 = ?
    target-only 1 stw +0xf8 = ?
    target-only 1 stw +0xfc = ?
    ours-only   1 stw +0x0 = ?
    ours-only   1 stw +0x4 = ?
    ours-only   1 stw +0x8 = ?
###   6  Enemy/BathtubKiller :: receiveMessage__14TBathtubKillerFP9THitActorUl  (66.2%)
    target-only 2 call theNerve__28TNerveBathtubKillerExplosionFv
    ours-only   2 call __ct__24TNerveBase<10TLiveActor>Fv
    ours-only   2 call __register_global_object
###   6  Enemy/BossHanachanMain :: moveObject__13TBossHanachanFv  (93.2%)
    target-only 1 stfs +0x188 = f
    target-only 1 stfs +0x18c = f
    target-only 1 stfs +0x190 = f
    ours-only   1 stw +0x188 = ?
    ours-only   1 stw +0x18c = ?
    ours-only   1 stw +0x190 = ?
###   6  Enemy/bosstelesa :: generateSlotItem__11TBossTelesaFv  (94.7%)
    target-only 1 stw +0xac = ?
    target-only 1 stw +0xb0 = ?
    target-only 1 stw +0xb4 = ?
    ours-only   1 stfs +0xac = f
    ours-only   1 stfs +0xb0 = f
    ours-only   1 stfs +0xb4 = f
###   6  Enemy/bosstelesa :: genAttacker__11TBossTelesaFv  (90.8%)
    target-only 1 stw +0xac = ?
    target-only 1 stw +0xb0 = ?
    target-only 1 stw +0xb4 = ?
    ours-only   1 stfs +0xac = f
    ours-only   1 stfs +0xb0 = f
    ours-only   1 stfs +0xb4 = f
###   6  Enemy/bosstelesa :: moveObject__11TBossTelesaFv  (93.0%)
    target-only 2 call set__Q29JGeometry8TVec3<f>FRC3Vec
    target-only 2 call sub__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
    ours-only   2 fconst 0
###   6  Enemy/bosstelesa :: execute__16TNerveBubbleLiveCFP24TSpineBase<10TLiveActor>  (92.5%)
    target-only 1 stw +0xac = ?
    target-only 1 stw +0xb0 = ?
    target-only 1 stw +0xb4 = ?
    ours-only   1 stfs +0xac = f
    ours-only   1 stfs +0xb0 = f
    ours-only   1 stfs +0xb4 = f
###   6  Enemy/bosstelesa :: appendEnemy__7TBubbleFv  (91.0%)
    target-only 1 stw +0xac = ?
    target-only 1 stw +0xb0 = ?
    target-only 1 stw +0xb4 = ?
    ours-only   1 stfs +0xac = f
    ours-only   1 stfs +0xb0 = f
    ours-only   1 stfs +0xb4 = f
###   6  Enemy/popo :: calcRootMatrix__5TPopoFv  (97.5%)
    target-only 1 stfs +0x20c = f
    target-only 1 stfs +0x21c = f
    target-only 1 stfs +0x22c = f
    ours-only   1 stfs +0x1c = f
    ours-only   1 stfs +0x2c = f
    ours-only   1 stfs +0xc = f
###   6  Enemy/tinkoopa :: perform__9TTinKoopaFUlPQ26JDrama9TGraphics  (96.4%)
    target-only 3 stw +0x0 = ?
    ours-only   1 stw +0x178 = ?
    ours-only   1 stw +0x17c = ?
    ours-only   1 stw +0x180 = ?
###   5  Enemy/BathtubKiller :: bind__14TBathtubKillerFv  (82.4%)
    target-only 2 call theNerve__28TNerveBathtubKillerExplosionFv
    ours-only   1 call __ct__24TNerveBase<10TLiveActor>Fv
    ours-only   2 call __register_global_object
###   5  Enemy/bosstelesa :: execute__27TNerveBossTelesaPrepareSlotCFP24TSpineBase<10TLiveActor>  (90.7%)
    target-only 1 call getSlotResult__11TTelesaSlotFv
    target-only 1 stb +0x34f = 0
    ours-only   2 call getResultFromAng__11TTelesaSlotFf
    ours-only   1 stb +0x34f = 254
###   5  Enemy/bosswanwan :: execute__19TNerveBWGraphWanderCFP24TSpineBase<10TLiveActor>  (93.1%)
    target-only 3 call getMActorAnmData__13TMActorKeeperCFv
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
    target-only 1 fconst 1
###   5  Enemy/Koopa :: receiveMessage__10TKoopaBodyFP9THitActorUl  (86.5%)
    target-only 1 call pushNerve__24TSpineBase<10TLiveActor>FPC24TNerveBase<10TLiveActor>
    ours-only   1 stw +0x14 = ?
    ours-only   1 stw +0x1c = ?
    ours-only   1 stw +0x20 = 0
    ours-only   1 stw +0x8 = ?
###   5  Enemy/Koopa :: receiveMessage__10TKoopaHeadFP9THitActorUl  (89.1%)
    target-only 1 call pushNerve__24TSpineBase<10TLiveActor>FPC24TNerveBase<10TLiveActor>
    ours-only   1 stw +0x14 = ?
    ours-only   1 stw +0x1c = ?
    ours-only   1 stw +0x20 = 0
    ours-only   1 stw +0x8 = ?
###   5  Enemy/limitkoopa :: receiveMessage__15TLimitKoopaHeadFP9THitActorUl  (83.8%)
    target-only 1 call pushNerve__24TSpineBase<10TLiveActor>FPC24TNerveBase<10TLiveActor>
    ours-only   1 stw +0x14 = ?
    ours-only   1 stw +0x1c = ?
    ours-only   1 stw +0x20 = 0
    ours-only   1 stw +0x8 = ?
###   5  Enemy/TabePuku :: execute__18TNerveTabePukuDragCFP24TSpineBase<10TLiveActor>  (74.2%)
    target-only 1 call __ct__Q29JGeometry8TVec4<f>Fv
    target-only 1 call dot__Q29JGeometry8TVec3<f>CFRCQ29JGeometry8TVec3<f>
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
    target-only 1 fconst 0
    ours-only   1 call rotate__Q29JGeometry9TQuat4<f>CFRCQ29JGeometry8TVec3<f>RQ29JGeometry8TVec3<f>
###   5  GC2D/CardLoad :: perform__9TCardLoadFUlPQ26JDrama9TGraphics  (97.9%)
    target-only 1 call __ct__14JSUInputStreamFv
    target-only 1 call __dt__20JSURandomInputStreamFv
    ours-only   1 call __ct__10JSUIosBaseFv
    ours-only   1 call __dt__14JSUInputStreamFv
    ours-only   1 stw +0x0 = @__vt__14JSUInputStream
###   5  MoveBG/MapObjPinna :: control__12TFerrisWheelFv  (83.3%)
    target-only 2 call SMSGetAnmFrameRate__Fv
    target-only 1 call getFrameCtrl__6MActorFi
    ours-only   2 fconst 1
###   4  Animal/BeeHive :: doWait__8TBeeHiveFv  (93.4%)
    target-only 1 fconst 0.01
    target-only 1 fconst 0.99
    ours-only   1 fconst -3.8147e-06
    ours-only   1 fconst 1
###   4  Enemy/amiNoko :: execute__24TNerveAmiNokoWalkOnFenceCFP24TSpineBase<10TLiveActor>  (94.5%)
    target-only 1 call sqrt__Q29JGeometry8TUtil<f>Ff
    ours-only   1 fconst 0
    ours-only   1 fconst 0.5
    ours-only   1 fconst 3
###   4  Enemy/bgtentacle :: perform__10TBGTakeHitFUlPQ26JDrama9TGraphics  (92.5%)
    target-only 1 call add__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
    ours-only   1 stfs +0x10 = f
    ours-only   1 stfs +0x14 = f
    ours-only   1 stfs +0xc = f
###   4  Enemy/bossgesso :: execute__11TNerveBGDieCFP24TSpineBase<10TLiveActor>  (93.0%)
    target-only 2 call getMActorAnmData__13TMActorKeeperCFv
    target-only 1 fconst 50
    ours-only   1 fconst 0
###   4  Enemy/BossHanachanSub :: moveHead__11TSphereLinkFRCQ29JGeometry8TVec3<f>  (85.7%)
    target-only 2 call isIllegalData__12TBGCheckDataCFv
    target-only 1 stfs +0x4 = f
    ours-only   1 stfs +0x10 = f
###   4  Enemy/feetinv :: FeetInvCalc__FP8J3DModelUsUsUsf  (77.1%)
    target-only 4 fconst 1
###   4  Enemy/gatekeeper :: execute__12TNerveBGKDieCFP24TSpineBase<10TLiveActor>  (88.8%)
    target-only 1 call getGateKeeperBGMStopFlag__10MSMainProcFv
    target-only 1 call setGateKeeperBGMPlayFlag__10MSMainProcFUlb
    target-only 1 call setTrackVolume__5MSBgmFUcfUlUc
    target-only 1 fconst 0
###   4  Enemy/Kazekun :: doAttackPose__8TKazekunFb  (84.5%)
    target-only 1 call sqrt__Q29JGeometry8TUtil<f>Ff
    ours-only   1 fconst 0
    ours-only   1 fconst 0.5
    ours-only   1 fconst 3
###   4  Enemy/Koopa :: KoopaNeckCallBack__9@unnamed@FP7J3DNodei  (72.2%)
    target-only 1 call sqrt__Q29JGeometry8TUtil<f>Ff
    ours-only   1 fconst 0
    ours-only   1 fconst 0.5
    ours-only   1 fconst 3
###   4  Enemy/koopajr :: makeKillerVelocity__17TKoopaJrSubmarineFP14TBathtubKillerQ29JGeometry8TVec3<f>  (94.3%)
    target-only 1 fconst 0.15708
    target-only 1 fconst 0.314159
    ours-only   1 fconst -0.15708
    ours-only   1 fconst -0.314159
###   4  GC2D/GCConsole2 :: countShine__11TGCConsole2Fv  (77.3%)
    target-only 1 call set__7JUTRectFiiii
    target-only 2 fconst 0.01
    target-only 1 fconst d4.5036e+15
###   4  GC2D/Option :: checkInput__14TOptionControlFv  (91.6%)
    target-only 1 call SMSGetMSound__Fv
    target-only 1 call begin__Q29@unnamed@16ArrayWrapper<Ul>CFv
    target-only 1 call stop__8JAISoundFUl
    ours-only   1 call stopSound__16TOptionSoundUnitFv
###   4  GC2D/Option :: __ct__17TOptionRumbleUnitFP9J2DScreen  (99.1%)
    target-only 1 stw +0x18 = @cSubtitleToggleItems__9@unnamed@
    target-only 1 stw +0x4 = @L
    ours-only   1 stw +0x18 = @cStopRumbleAnm__9@unnamed@
    ours-only   1 stw +0x4 = @cRumbleToggleItems__9@unnamed@
###   4  GC2D/SelectDir :: __dt__10TSelectDirFv  (99.8%)
    target-only 1 stw +0x0 = @selectNames$
    target-only 1 stw +0xc = @selectNames$+32
    ours-only   1 stw +0x0 = @__vt__10TSelectDir
    ours-only   1 stw +0xc = @__vt__10TSelectDir+32
###   4  GC2D/Talk2D2 :: setTagParam__8TTalk2D2FR20JSUMemoryInputStreamR10J2DTextBoxPiPi  (94.3%)
    target-only 1 stw +0x11c = ?
    target-only 1 stw +0x120 = ?
    target-only 1 stw +0xf0 = ?
    target-only 1 stw +0xf4 = ?
###   4  Map/BathWaterManager :: perform__17TBathWaterManagerFUlPQ26JDrama9TGraphics  (94.0%)
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
    target-only 1 fconst 1
    ours-only   1 fconst 0.9
    ours-only   1 fconst 10
###   4  Map/BathWaterManager :: loadAfter__17TBathWaterManagerFv  (100.0%)
    target-only 2 stw +0x0 = @__vt__22TBathWaterPreprocessor
    ours-only   1 stw +0x0 = @__vt__18TBathWaterRenderer
    ours-only   1 stw +0x0 = @__vt__22TBathWaterFlatRenderer
###   4  MSound/MAnmSound :: startAnimSound__12MAnmSoundNPCFPvUlPP8JAISoundP8JAIActorUc  (82.3%)
    target-only 1 call sqrtf__3stdFf
    ours-only   1 fconst 0
    ours-only   1 fconst d0.5
    ours-only   1 fconst d3
###   4  MSound/MSoundSE :: startSoundActorWithInfo__Q214MSoundSESystem8MSoundSEFUlPC3VecP3VecfUlUlPP8JAISoundUlUc  (82.9%)
    target-only 1 call sqrtf__3stdFf
    ours-only   1 fconst 0
    ours-only   1 fconst d0.5
    ours-only   1 fconst d3
###   4  System/EventWatcher :: evIsNearActors__FP32TSpcTypedInterp<13TEventWatcher>Ul  (90.7%)
    target-only 1 call sqrt__Q29JGeometry8TUtil<f>Ff
    ours-only   1 fconst 0
    ours-only   1 fconst 0.5
    ours-only   1 fconst 3
###   4  System/EventWatcher :: evIsNearSameActors__FP32TSpcTypedInterp<13TEventWatcher>Ul  (93.3%)
    target-only 1 call sqrt__Q29JGeometry8TUtil<f>Ff
    ours-only   1 fconst 0
    ours-only   1 fconst 0.5
    ours-only   1 fconst 3
###   4  System/MovieDirector :: __dt__14TMovieDirectorFv  (99.9%)
    target-only 1 stw +0x0 = @movies$
    target-only 1 stw +0xc = @movies$+32
    ours-only   1 stw +0x0 = @__vt__14TMovieDirector
    ours-only   1 stw +0xc = @__vt__14TMovieDirector+32
###   3  Animal/AnimalBase :: execWalk__11TAnimalBaseFb  (82.4%)
    target-only 1 call MsClamp<f>__Ffff
    target-only 1 call __ct__Q29JGeometry8TVec4<f>Fv
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
###   3  Animal/Bird :: execute__28TNerveAnimalBirdWalkOnGroundCFP24TSpineBase<10TLiveActor>  (80.1%)
    target-only 1 call __ct__Q29JGeometry8TVec4<f>Fv
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
    ours-only   1 call rotate__Q29JGeometry9TQuat4<f>CFRQ29JGeometry8TVec3<f>
###   3  Animal/Bird :: doLanding__11TAnimalBirdFb  (83.9%)
    target-only 1 call sqrt__Q29JGeometry8TUtil<f>Ff
    ours-only   1 fconst 0.5
    ours-only   1 fconst 3
###   3  Enemy/BathtubKiller :: behaveToWater__14TBathtubKillerFP9THitActor  (61.8%)
    target-only 1 call theNerve__28TNerveBathtubKillerExplosionFv
    ours-only   1 call __ct__24TNerveBase<10TLiveActor>Fv
    ours-only   1 call __register_global_object
###   3  Enemy/bossgesso :: execute__11TNerveBGTugCFP24TSpineBase<10TLiveActor>  (95.8%)
    target-only 3 call getMActorAnmData__13TMActorKeeperCFv
###   3  Enemy/bossgesso :: execute__12TNerveBGWaitCFP24TSpineBase<10TLiveActor>  (95.0%)
    target-only 3 call getMActorAnmData__13TMActorKeeperCFv
###   3  Enemy/bossgesso :: perform__10TBossGessoFUlPQ26JDrama9TGraphics  (93.0%)
    target-only 1 call showMessage__10TBossGessoFUl
    ours-only   1 call startAppearBalloon__11TGCConsole2FUlb
    ours-only   1 stw +0x198 = ?
###   3  Enemy/bosstelesa :: calcRootMatrix__11TBossTelesaFv  (90.8%)
    target-only 1 call identity33__Q29JGeometry64TRotation3<Q29JGeometry38TMatrix34<Q29JGeometry13SMatrix34C<f>>>Fv
    ours-only   1 fconst 0
    ours-only   1 fconst 1
###   3  Enemy/bosswanwan :: execute__11TNerveBWDieCFP24TSpineBase<10TLiveActor>  (96.7%)
    target-only 1 call getMActorAnmData__13TMActorKeeperCFv
    target-only 1 call setMtx__17TMapCollisionBaseFPA4_f
    ours-only   1 call PSMTXCopy
###   3  Enemy/cannon :: init__7TCannonFP12TLiveManager  (97.4%)
    target-only 1 call __ct__Q37JGadget36TList<Pv,Q27JGadget14TAllocator<Pv>>8iteratorFPQ37JGadget36TList<Pv,Q27JGadget14TAllocator<Pv>>6TNode_
    ours-only   1 call __ct__Q37JGadget26TList_pointer<P9THitActor>8iteratorFQ37JGadget36TList<Pv,Q27JGadget14TAllocator<Pv>>8iterator
    ours-only   1 call end__Q27JGadget36TList<Pv,Q27JGadget14TAllocator<Pv>>Fv
###   3  Enemy/fireWanwan :: execute__28TNerveFireWanwanRecoverGraphCFP24TSpineBase<10TLiveActor>  (87.2%)
    target-only 1 call __ct__Q29JGeometry8TVec4<f>Fv
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
    ours-only   1 call rotate__Q29JGeometry9TQuat4<f>CFRCQ29JGeometry8TVec3<f>RQ29JGeometry8TVec3<f>
###   3  Enemy/fireWanwan :: execute__25TNerveFireWanwanFindMarioCFP24TSpineBase<10TLiveActor>  (86.2%)
    target-only 1 call __ct__Q29JGeometry8TVec4<f>Fv
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
    ours-only   1 call rotate__Q29JGeometry9TQuat4<f>CFRCQ29JGeometry8TVec3<f>RQ29JGeometry8TVec3<f>
###   3  Enemy/fireWanwan :: perform__18TFireWanwanTailHitFUlPQ26JDrama9TGraphics  (92.0%)
    target-only 1 call __vc__Q29@unnamed@34ArrayWrapper<Q211TTailRubber4Node>CFi
    target-only 1 call size__Q29@unnamed@34ArrayWrapper<Q211TTailRubber4Node>CFv
    target-only 1 fconst 4
###   3  Enemy/fruitsboat :: execute__27TNerveFruitsBoatGraphWanderCFP24TSpineBase<10TLiveActor>  (89.8%)
    target-only 1 fconst 1
    ours-only   2 call gateCheck__6MSoundFUl
###   3  Enemy/hanasambo :: init__10THanaSamboFP12TLiveManager  (96.2%)
    target-only 1 call __ct__Q37JGadget36TList<Pv,Q27JGadget14TAllocator<Pv>>8iteratorFPQ37JGadget36TList<Pv,Q27JGadget14TAllocator<Pv>>6TNode_
    ours-only   1 call __ct__Q37JGadget26TList_pointer<P9THitActor>8iteratorFQ37JGadget36TList<Pv,Q27JGadget14TAllocator<Pv>>8iterator
    ours-only   1 call end__Q27JGadget36TList<Pv,Q27JGadget14TAllocator<Pv>>Fv
###   3  Enemy/TabePuku :: swimTo__9TTabePukuFRCQ29JGeometry8TVec3<f>  (92.2%)
    ours-only   2 fconst -3.8147e-06
    ours-only   1 fconst 0
###   3  Enemy/wireTrap :: load__9TWireTrapFR20JSUMemoryInputStream  (86.5%)
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
    target-only 1 fconst 0
    target-only 1 fconst 1
###   3  Map/MapEventSink :: loadAfter__19TMapEventSinkBiancoFv  (75.3%)
    target-only 1 call loadAfter__24TMapEventSinkInPollutionFv
    ours-only   1 call loadAfter__Q26JDrama8TNameRefFv
    ours-only   1 call registerPollutionObj__20TPollutionCounterObjFP13TPollutionObjPUl
###   3  System/Application :: drawDVDErr__12TApplicationFv  (99.8%)
    target-only 1 fconst d3.05176e-05
    ours-only   1 fconst 0.5
    ours-only   1 fconst d4.5036e+15
###   3  System/MarDirectorDirect :: nextStateInitialize__12TMarDirectorFUc  (95.2%)
    target-only 1 call THPPlayerPlay
    target-only 1 call gateCheck__6MSoundFUl
    target-only 1 call startSoundSystemSE__Q214MSoundSESystem8MSoundSEFUlUlPP8JAISoundUl
###   2  Animal/Bird :: receiveMessage__11TAnimalBirdFP9THitActorUl  (96.8%)
    target-only 2 call __ct__24TNerveBase<10TLiveActor>Fv
###   2  Enemy/BathtubPeach :: execute__17TNervePeachEscapeCFP24TSpineBase<10TLiveActor>  (86.9%)
    target-only 1 call dot__Q29JGeometry8TVec2<f>CFRCQ29JGeometry8TVec2<f>
    target-only 1 call getSaveParam__13TEnemyManagerCFv
###   2  Enemy/bosstelesa :: execute__22TNerveBossTelesaFreezeCFP24TSpineBase<10TLiveActor>  (93.1%)
    target-only 1 stb +0x34f = 0
    ours-only   1 stb +0x34f = 254
###   2  Enemy/bosstelesa :: execute__24TNerveBossTelesaHideWaitCFP24TSpineBase<10TLiveActor>  (88.4%)
    target-only 1 stb +0x34f = 0
    ours-only   1 stb +0x34f = 254
###   2  Enemy/bosstelesa :: slotFall__11TBossTelesaFv  (95.3%)
    ours-only   2 fconst 0
###   2  Enemy/bosstelesa :: loadAfter__11TBossTelesaFv  (96.9%)
    target-only 1 stw +0xf8 = ?
    ours-only   1 stw +0xf0 = ?
###   2  Enemy/bosstelesa :: calcRootMatrix__11TTelesaSlotFv  (89.3%)
    ours-only   2 fconst 0
###   2  Enemy/bosswanwan :: init__11TBossWanwanFP12TLiveManager  (98.0%)
    target-only 1 call __ct__Q29JGeometry13SMatrix34C<f>Fv
    target-only 1 call getMActorAnmData__13TMActorKeeperCFv
###   2  Enemy/bosswanwan :: moveRequest__9TBWPicketFRCQ29JGeometry8TVec3<f>  (90.0%)
    target-only 2 call getLatestNerve__24TSpineBase<10TLiveActor>CFv
###   2  Enemy/cannon :: killerShoot__7TCannonFv  (92.1%)
    target-only 2 call sub__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
###   2  Enemy/enemy :: doShortCut__11TSpineEnemyFv  (94.7%)
    target-only 1 fconst 0
    ours-only   1 call pop__23TSolidStack<9TPathNode>Fv
###   2  Enemy/enemyMario :: hitWater__11TEnemyMarioFP9THitActor  (99.9%)
    target-only 1 fconst 30
    ours-only   1 fconst 0
###   2  Enemy/enemyMario :: emWalkAround__11TEnemyMarioFv  (98.9%)
    target-only 1 fconst 0
    ours-only   1 fconst 64
###   2  Enemy/enemyMario :: emJumping__11TEnemyMarioFv  (99.7%)
    target-only 1 fconst 0
    ours-only   1 fconst 64
###   2  Enemy/enemyMario :: emWaiting__11TEnemyMarioFv  (99.6%)
    target-only 1 fconst 0
    ours-only   1 fconst 64
###   2  Enemy/fireWanwan :: execute__20TNerveFireWanwanTurnCFP24TSpineBase<10TLiveActor>  (99.1%)
    target-only 1 stw +0x0 = ?
    ours-only   1 stw +0x8 = ?
###   2  Enemy/Kukku :: dropCoins__6TKukkuFv  (79.4%)
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
    target-only 1 fconst 0
###   2  Enemy/smallEnemy :: execute__22TNerveSmallEnemyChangeCFP24TSpineBase<10TLiveActor>  (93.3%)
    target-only 1 call setFrameRate__6MActorFfi
    target-only 1 fconst 0
###   2  Enemy/walkerEnemy :: moveObject__12TWalkerEnemyFv  (99.8%)
    target-only 1 stfs +0x14 = f
    ours-only   1 stfs +0x34 = f
###   2  GC2D/hx_wiper :: Hx_Test5  (98.7%)
    ours-only   1 fconst -1
    ours-only   1 fconst 0.0625
###   2  GC2D/hx_wiper :: Hx_Door  (99.8%)
    ours-only   2 fconst d4.5036e+15
###   2  GC2D/Option :: movementOption__14TOptionControlFv  (96.3%)
    target-only 1 call __ct__7JUTRectFiiii
    ours-only   1 call set__7JUTRectFiiii
###   2  GC2D/Talk2D2 :: makeBoxLine__8TTalk2D2FScPc  (93.8%)
    target-only 1 fconst 1.05
    ours-only   1 fconst 1.1
###   2  JSystem/JDrama/JDRCamera :: perform__Q26JDrama12TPolarCameraFUlPQ26JDrama9TGraphics  (65.8%)
    target-only 1 fconst 0
    target-only 1 fconst 1
###   2  Map/MapEventDolpic :: watch__26TDolpicEventRiccoMammaGateFv  (92.3%)
    target-only 1 call gateCheck__6MSoundFUl
    target-only 1 call startSoundSystemSE__Q214MSoundSESystem8MSoundSEFUlUlPP8JAISoundUl
###   2  Map/MapStaticObject :: init__13TMapStaticObjFPCc  (99.8%)
    target-only 1 call setMtx__17TMapCollisionBaseFPA4_f
    ours-only   1 call PSMTXCopy
###   2  MoveBG/MapObjBall :: touchWall__11TMapObjBallFPQ29JGeometry8TVec3<f>P18TBGWallCheckRecord  (98.9%)
    target-only 1 fconst 1
    ours-only   1 fconst -1
###   2  MoveBG/MapObjBase :: makeObjAppeared__11TMapObjBaseFv  (99.8%)
    target-only 1 call setMtx__17TMapCollisionBaseFPA4_f
    ours-only   1 call PSMTXCopy
###   2  MoveBG/MapObjMamma :: control__11TLeanMirrorFv  (90.3%)
    target-only 2 call startAppearBalloon__11TGCConsole2FUlb
###   2  MoveBG/MapObjMamma :: startCameraShakeSE__FUlUl  (0.0%)
    ours-only   1 call gateCheck__6MSoundFUl
    ours-only   1 call startSoundActor__Q214MSoundSESystem8MSoundSEFUlPC3VecUlPP8JAISoundUlUc
###   2  MoveBG/MapObjPinna :: initMapObj__12TFerrisWheelFv  (97.0%)
    target-only 1 call SMSGetAnmFrameRate__Fv
    ours-only   1 fconst 1
###   2  MSound/MSound :: talkModeIn__6MSoundFb  (99.9%)
    target-only 1 fconst 0.48
    ours-only   1 fconst 0.6
###   2  NPC/NpcNerve :: execute__20TNerveNPCGraphWanderCFP24TSpineBase<10TLiveActor>  (97.6%)
    target-only 1 call getCurGraphIndex__12TGraphTracerCFv
    target-only 1 call getGraph__12TGraphTracerCFv
###   1  Enemy/BathtubKiller :: attackToMario__14TBathtubKillerFv  (93.8%)
    target-only 1 call __ct__24TNerveBase<10TLiveActor>Fv
###   1  Enemy/bgtentacle :: __ct__11TBGTentacleFP10TBossGessoii  (97.9%)
    target-only 1 call __ct__Q29JGeometry13SMatrix34C<f>Fv
###   1  Enemy/bombhei :: execute__21TNerveBombHeiGenerateCFP24TSpineBase<10TLiveActor>  (92.8%)
    target-only 1 fconst 0
###   1  Enemy/bossgesso :: execute__15TNerveBGPolDropCFP24TSpineBase<10TLiveActor>  (96.7%)
    target-only 1 call getMActorAnmData__13TMActorKeeperCFv
###   1  Enemy/bossgesso :: execute__15TNerveBGPolluteCFP24TSpineBase<10TLiveActor>  (95.8%)
    target-only 1 call getMActorAnmData__13TMActorKeeperCFv
###   1  Enemy/bossgesso :: execute__22TNerveBGTentacleDamageCFP24TSpineBase<10TLiveActor>  (97.6%)
    target-only 1 call getMActorAnmData__13TMActorKeeperCFv
###   1  Enemy/bossgesso :: execute__18TNerveBGBeakDamageCFP24TSpineBase<10TLiveActor>  (94.8%)
    target-only 1 call getMActorAnmData__13TMActorKeeperCFv
###   1  Enemy/bossgesso :: execute__17TNerveBGEyeDamageCFP24TSpineBase<10TLiveActor>  (96.1%)
    target-only 1 call getMActorAnmData__13TMActorKeeperCFv
###   1  Enemy/bossgesso :: moveObject__10TBossGessoFv  (98.8%)
    target-only 1 call SMS_GetMarioPos__Fv
###   1  Enemy/bossgesso :: init__10TBossGessoFP12TLiveManager  (99.0%)
    target-only 1 call __ct__Q29JGeometry13SMatrix34C<f>Fv
###   1  Enemy/BossHanachanMain :: execWalk__13TBossHanachanFb  (75.8%)
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
###   1  Enemy/bosstelesa :: execute__24TNerveBossTelesaFallDemoCFP24TSpineBase<10TLiveActor>  (90.2%)
    target-only 1 call sub__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
###   1  Enemy/bosstelesa :: rouletteStart__11TBossTelesaFv  (93.2%)
    target-only 1 fconst 0
###   1  Enemy/bosstelesa :: createEnemyInstance__14TBubbleManagerFv  (70.7%)
    target-only 1 call __ct__Q29JGeometry38TMatrix34<Q29JGeometry13SMatrix34C<f>>Fv
###   1  Enemy/bosswanwan :: execute__13TNerveBWShakeCFP24TSpineBase<10TLiveActor>  (93.9%)
    target-only 1 call getMActorAnmData__13TMActorKeeperCFv
###   1  Enemy/bosswanwan :: execute__14TNerveBWWakeupCFP24TSpineBase<10TLiveActor>  (90.8%)
    target-only 1 call getMActorAnmData__13TMActorKeeperCFv
###   1  Enemy/bosswanwan :: execute__12TNerveBWBarkCFP24TSpineBase<10TLiveActor>  (96.1%)
    target-only 1 call getMActorAnmData__13TMActorKeeperCFv
###   1  Enemy/cannon :: execute__18TNerveCannonSearchCFP24TSpineBase<10TLiveActor>  (93.1%)
    target-only 1 call sub__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
###   1  Enemy/cannon :: moveObject__7TCannonFv  (90.5%)
    target-only 1 call add__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
###   1  Enemy/chuuhana :: behaveToWater__9TChuuHanaFP9THitActor  (95.5%)
    target-only 1 fconst 0
###   1  Enemy/elecNokonoko :: shoot__13TElecCarapaceFv  (93.0%)
    target-only 1 call dot__Q29JGeometry8TVec3<f>CFRCQ29JGeometry8TVec3<f>
###   1  Enemy/enemyMario :: emReplayJumpToNearestNode__11TEnemyMarioFv  (93.6%)
    ours-only   1 fconst 1
###   1  Enemy/fireWanwan :: updateRumble__11TFireWanwanFv  (94.2%)
    target-only 1 call isTaken__10TTakeActorCFv
###   1  Enemy/fruitsboat :: moveObject__11TFruitsBoatFv  (88.0%)
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
###   1  Enemy/gatekeeper :: execute__15TNerveBGKAppearCFP24TSpineBase<10TLiveActor>  (97.5%)
    target-only 1 call setGateKeeperBGMPlayFlag__10MSMainProcFUlb
###   1  Enemy/hamukuri :: generateBody__18TBossDangoHamuKuriFv  (96.3%)
    target-only 1 call __ct__24TNerveBase<10TLiveActor>Fv
###   1  Enemy/hanasambo :: checkGenCoin__20TSamboFlowerCoinUnitFv  (88.1%)
    target-only 1 call add__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
###   1  Enemy/koopajr :: init__17TKoopaJrSubmarineFP12TLiveManager  (94.4%)
    ours-only   1 stw +0x0 = ?
###   1  Enemy/Kumokun :: execute__19TNerveKumokunSearchCFP24TSpineBase<10TLiveActor>  (97.9%)
    ours-only   1 fconst 0.5
###   1  Enemy/Kumokun :: decideTargetAtDir__8TKumokunFRCQ29JGeometry8TVec3<f>  (89.4%)
    ours-only   1 fconst 0
###   1  Enemy/limitkoopa :: attack___16TLimitKoopaFlameFP9THitActor  (88.2%)
    target-only 1 call getSaveParam__13TEnemyManagerCFv
###   1  Enemy/limitkoopa :: execute__20TNerveLimitKoopaWaitCFP24TSpineBase<10TLiveActor>  (99.3%)
    target-only 1 call getSaveParam__13TEnemyManagerCFv
###   1  Enemy/TabePuku :: execute__20TNerveTabePukuAttackCFP24TSpineBase<10TLiveActor>  (96.2%)
    target-only 1 call dot__Q29JGeometry8TVec3<f>CFRCQ29JGeometry8TVec3<f>
###   1  Enemy/tinkoopa :: init__9TTinKoopaFP12TLiveManager  (94.6%)
    ours-only   1 stw +0x0 = ?
###   1  Enemy/tobiPuku :: execute__26TNerveTobiPukuReturnLaunchCFP24TSpineBase<10TLiveActor>  (85.7%)
    target-only 1 call sub__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
###   1  GC2D/GCConsole2 :: countBlueCoin__11TGCConsole2Fv  (75.6%)
    ours-only   1 call copy__7JUTRectFRC7JUTRect
###   1  GC2D/GCConsole2 :: load__11TGCConsole2FR20JSUMemoryInputStream  (99.7%)
    ours-only   1 stw +0x2ac = ?
###   1  GC2D/hx_wiper :: Hxs1_Test2  (94.0%)
    ours-only   1 fconst d4.5036e+15
###   1  GC2D/hx_wiper :: Hx_Logo  (95.1%)
    ours-only   1 fconst 8
###   1  GC2D/hx_wiper :: Hxs_PenDraw  (86.3%)
    ours-only   1 fconst d4.5036e+15
###   1  GC2D/Talk2D2 :: openWindow__8TTalk2D2FScf  (99.3%)
    target-only 1 fconst 1
###   1  GC2D/Talk2D2 :: loadAfter__8TTalk2D2Fv  (99.8%)
    ours-only   1 fconst -18
###   1  Map/BathWaterManager :: throwMario__17TBathWaterManagerFf  (76.6%)
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
###   1  Map/BathWaterManager :: render__22TBathWaterFlatRendererFPQ26JDrama9TGraphicsRC12TBathtubDataPP10TBathWaterPP16TBathWaterParamsi  (99.1%)
    target-only 1 call __ct__Q29JGeometry13SMatrix34C<f>Fv
###   1  Map/MapWarp :: watchToWarp__8TMapWarpFv  (66.4%)
    target-only 1 call add__Q29JGeometry8TVec3<f>FRCQ29JGeometry8TVec3<f>
###   1  MoveBG/MapObjCorona :: __ct__8TBathtubFPCc  (86.8%)
    target-only 1 call __ct__Q29JGeometry13SMatrix33R<f>Fv
###   1  MoveBG/MapObjLib :: makeMtxRotByAxis__11TMapObjBaseFRCQ29JGeometry8TVec3<f>fPA4_f  (65.9%)
    target-only 1 fconst 0
###   1  MoveBG/MapObjMare :: control__15TMapObjGrowTreeFv  (97.9%)
    target-only 1 call getMActor__10TLiveActorCFv
###   1  MoveBG/MapObjMare :: touchWater__15TMapObjGrowTreeFP9THitActor  (93.9%)
    target-only 1 call getMActor__10TLiveActorCFv
###   1  MoveBG/MapObjRailBlock :: setGroundCollision__11TNormalLiftFv  (95.7%)
    target-only 1 call __ct__Q29JGeometry13SMatrix34C<f>Fv
###   1  MoveBG/MapObjRicco :: fireObj__14TFruitLauncherFv  (99.0%)
    target-only 1 call getMActor__10TLiveActorCFv
###   1  NPC/NpcEvent :: evCheckLatestNerve4Npc__FP32TSpcTypedInterp<13TEventWatcher>Ul  (95.2%)
    target-only 1 call getLatestNerve__24TSpineBase<10TLiveActor>CFv
###   1  NPC/NpcWalkTurn :: execWalk__8TBaseNPCFb  (92.3%)
    target-only 1 call set<f>__Q29JGeometry8TVec3<f>Ffff
###   1  System/Application :: gameLoop__12TApplicationFv  (98.9%)
    ours-only   1 fconst d4.5036e+15
###   1  System/CardManager :: filledInitData___12TCardManagerFP12CARDFileInfo  (94.3%)
    ours-only   1 call getFlag__12TFlagManagerCFUl
###   1  System/CardManager :: getOptionWriteStream__12TCardManagerFP21JSUMemoryOutputStream  (87.0%)
    ours-only   1 call getFlag__12TFlagManagerCFUl
###   1  System/MarDirectorDirect :: currentStateFinalize__12TMarDirectorFUc  (97.7%)
    ours-only   1 call THPPlayerPlay

## w13 notes for w11 (MoveBG / NPC / Map, handed over 2026-10-08)
Already fixed in workers/w13 (skip these):
- MoveBG/MapObjPinna TFerrisWheel::control / initMapObj: the US build calls SMSGetAnmFrameRate(), not `rotate_frame_rate` (which is PAL only; the US symbols.txt label "rotate_frame_rate" at 0x8040C8B0 is really TShellCup::mOpenRotMax = 90.0f).
Already checked by w13 and found to be codegen only (no fix needed):
- Map/MapEventSink TMapEventSinkBianco::loadAfter (the GameCube calls TMapEventSinkInPollution::loadAfter out of line; ours inlines the same loop).
- MoveBG/MapObjPinna TAmiKing::moveObject (same 3/5/4/6 state test, different branch layout); MoveBG/MapObjTown TRedCoinSwitch::control (switch layout).
- NPC/NpcCallback NPCNeckCallBack (li 0/1 is a bool layout difference; condition is the same).
- Earlier, also noise: MapObjBall/Corona scaleAdd sites, Map/MapMirror TMirrorModelManager::perform, MapObjLib makeMtxRotByAxis, BathWaterManager calcBathtubData/throwMario, MapEventMare initCommon.
Not looked at yet by w13: everything else in MoveBG/NPC/Map above.
