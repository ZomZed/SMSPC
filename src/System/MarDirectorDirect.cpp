#include <System/MarDirector.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JDrama/JDRCamera.hpp>
#include <System/Application.hpp>
#include <System/MSoundMainSide.hpp>
#include <System/MarioGamePad.hpp>
#include <System/PerformList.hpp>
#include <System/StageUtil.hpp>
#include <System/FlagManager.hpp>
#include <System/CardManager.hpp>
#include <Player/Mario.hpp>
#include <Player/WaterGun.hpp>
#include <Strategic/ObjHitCheck.hpp>
#include <MarioUtil/DrawUtil.hpp>
#include <MarioUtil/RumbleMgr.hpp>
#include <THPPlayer/THPPlayer.h>
#include <GC2D/GCConsole2.hpp>
#include <GC2D/ConsoleStr.hpp>
#include <GC2D/ScrnFader.hpp>
#include <GC2D/PauseMenu2.hpp>
#include <GC2D/CardSave.hpp>
#include <GC2D/Guide.hpp>
#include <GC2D/SunGlass.hpp>
#include <GC2D/Talk2D2.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/MarioPositionObj.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/MapObjDolpic.hpp>
#include <NPC/NpcBase.hpp>
#include <dolphin/gx.h>
#ifdef SMS_PORT
#include <stdlib.h> // getenv (SMS_PORT_LOGMARIO)
#endif

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>
#include <Map/MapCollisionEntry.hpp>

#ifdef SMS_PORT
#include "port_profile.h" // w14: SMS_PORT_PROFILE=1 frame/load profiler
#endif

extern OSThread gSetupThread;

#ifdef SMS_PORT_AURORA
// w9 quality of life (port/src/port_qol.cpp, every option off by default).
extern "C" int port_qol_skip_cutscenes(void);
extern "C" int port_qol_autosave(void);
extern "C" int port_qol_continue_shine(void);
extern "C" void port_qol_toast(const char* text);
extern void saveBookmark(u32 idx); // GC2D/CardSave.cpp

// "Auto-save after each Shine": what the save prompt's "Save" does (TCardSave
// PROGRESS_UNK2B -> UNK16 -> UNK1), without the prompt. 0 = still saving, 1 = saved,
// -1 = not possible here (no card, another game's file, broken file): the normal prompt
// is shown instead, so the player decides as on the GameCube.
static int sPortAutoSaveStep;
static TCardBookmarkInfo sPortBookmarks[3];
static int portAutoSave()
{
	s32 status;
	switch (sPortAutoSaveStep) {
	case 0:
		gpCardManager->getBookmarkInfos(sPortBookmarks);
		sPortAutoSaveStep = 1;
		return 0;
	case 1: {
		status = gpCardManager->getLastStatus();
		if (status == CARD_RESULT_BUSY)
			return 0;
		const s8 file = SMSGetApplication()->mSaveFile;
		if (status != CARD_RESULT_READY || file < 0 || file > 2) {
			sPortAutoSaveStep = 0;
			return -1;
		}
		const TCardBookmarkInfo& bm = sPortBookmarks[file];
		if (bm.unk0 == 1
		    || (bm.unk18 != 0 && bm.unk8 != TFlagManager::getInstance()->getLastSaveTime())) {
			gpCardManager->unmount();
			sPortAutoSaveStep = 0;
			return -1;
		}
		saveBookmark(file);
		sPortAutoSaveStep = 2;
		return 0;
	}
	case 2:
		status = gpCardManager->getLastStatus();
		if (status == CARD_RESULT_BUSY)
			return 0;
		if (status != CARD_RESULT_READY) {
			TFlagManager::getInstance()->saveFail();
			gpCardManager->unmount();
			sPortAutoSaveStep = 0;
			return -1;
		}
		TFlagManager::getInstance()->saveSuccess();
		{
			JSUMemoryOutputStream stream;
			gpCardManager->getOptionWriteStream(&stream);
			TFlagManager::getInstance()->saveOption(stream);
			gpCardManager->writeOptionBlock();
		}
		sPortAutoSaveStep = 3;
		return 0;
	case 3:
		if (gpCardManager->getLastStatus() == CARD_RESULT_BUSY)
			return 0;
		gpCardManager->unmount();
		sPortAutoSaveStep = 0;
		return 1;
	}
	return -1;
}

// "Continue after collecting a Shine": the Shine is counted and saved (save prompt or
// auto-save as usual), but the stage is not left. Where the next stage or scenario
// depends on the Shine, the original return is kept: secret courses (the course ends),
// the airstrip and Delfino Plaza (their scenario follows the story flags), Corona
// Mountain, and Shines given by scripts rather than touched by Mario.
static bool sPortContinueShine;
static TMarDirector* sPortContinueDir; // the stage that decided it (a stale flag never leaks:
static void* sPortContinueShineObj;    // a new stage has unk25C == nullptr until its own Shine)
static bool portCanContinueAfterShine(TShine* shine)
{
	if (!port_qol_continue_shine() || shine == nullptr || gpMarioOriginal == nullptr
	    || gpMarioOriginal->mStatus != MARIO_STATUS_WIN_DEMO)
		return false;
	const u8 stage = SMSGetApplication()->mCurrArea.getStage();
	if (SMS_isExMap() || stage == 0 || stage == 1 || stage == 60)
		return false;
	return true;
}

// After the Shine demo and the save: back to normal play.
static void portContinueAfterShine(TMarDirector* d)
{
	sPortContinueShine = false;
	d->offFlag(TMarDirector::DIRECTOR_FLAG_SHINE_TAKEN);
	if (gpMarioOriginal != nullptr && gpMarioOriginal->mStatus == MARIO_STATUS_WIN_DEMO)
		gpMarioOriginal->changePlayerStatus(MARIO_STATUS_WAIT, 0, true); // demo flag: forced, as in readBillboard
	const TGameSequence& area = SMSGetApplication()->mCurrArea;
	MSMainProc::startStageBGM(area.getStage(), area.getScenario());
	OSReport("[port] QoL: continuing in stage %d after the Shine\n", area.getStage());
}
#endif

#ifdef SMS_PORT
// PC frame-rate option (port/src/port_interp.cpp, w9). A logic frame here: the ticks of
// direct() end with unk34 (CUE_CALC_VIEW | CUE_ENTRY: camera, J3D viewCalc of every model,
// draw-buffer entries for the new state), and the draw half at the start of the next
// direct() call renders those buffers. Between two logic frames,
// port_mardirector_redraw() draws an extra display frame: the view half of unk34
// (CUE_CALC_VIEW | CUE_SET_PROJECTION, no new entries) with the camera and the models'
// joint matrices interpolated between the last two entered states, the GX draw lists,
// then the view half once more with the live values, so that every model and object is
// left as the logic frame's unk34 left it. The shadow requests the next logic draw
// consumes and the sound-effect gate are put back too, and the pollution and graffiti
// lists (EFB copies the game logic reads back) are not repeated.
#include <MarioUtil/ShadowUtil.hpp>
#include <stdlib.h>
#include <string.h>
extern "C" void port_interp_entry_begin(const f32* view, const f32* proj);
extern "C" void port_interp_entry_end(void);
extern "C" void port_interp_logic_draw_begin(void);
extern "C" void port_interp_logic_draw_end(void);
extern "C" int port_interp_redraw_begin(f32* view, f32* proj);
extern "C" int port_interp_redraw_draw_end(f32* view, f32* proj);
extern "C" void port_mp_logic_frame(void); // port/src/port_mp_puppet.cpp (w15)
extern "C" void port_mp_shine_got(int shineId); // port/src/port_mp_game.cpp (w10)
extern "C" int port_mp_world_runs_behind(int state); // port/src/port_mp_world.cpp
extern "C" void port_mp_objx_shine_got(int shineId); // port/src/port_mp_objectives_extra.cpp (w8)
static JDrama::TGraphics sPortDrawGraphics;  // as the draw half starts
static JDrama::TGraphics sPortEntryGraphics; // as unk34 starts
static TMarDirector* sPortDrawDirector;
static TCircleShadowRequest* sPortShadowRequests;

#ifdef SMS_PORT_AURORA
// w15 multiplayer: the Shine-get demo switches to its own perform lists (Mario, the Shine,
// the camera...) and stops the rest of the world. In a session the other players' world
// must keep going, so the entries of the full list that the Shine list does not hold are
// performed too (each performer once: those in both lists only through the Shine list).
// Outside a session only the Shine list runs, as on the GameCube.
extern "C" int port_mp_active(void);
static void portShineDemoPerform(TPerformList* shine, TPerformList* all, u32 cue,
                                 JDrama::TGraphics* graphics)
{
	shine->perform(cue, graphics);
	if (all == nullptr || !port_mp_active())
		return;
	static bool sLogged;
	// The player group holds the local Mario (and his Yoshi, FLUDD), which the Shine list steps
	// already: never run it twice (checked by the performer names logged below).
	static JDrama::TViewObj* sPlayerGroup;
	static TMarDirector* sPlayerGroupDir;
	if (sPlayerGroupDir != gpMarDirector) {
		sPlayerGroupDir = gpMarDirector;
		sPlayerGroup    = (JDrama::TViewObj*)JDrama::TNameRefGen::search("プレーヤーグループ");
	}
	int extra = 0;
	for (JGadget::TSingleLinkList<TPerformLink, 0>::iterator it = all->getChildren().begin();
	     it != all->getChildren().end(); it++) {
		bool inShine = false;
		for (JGadget::TSingleLinkList<TPerformLink, 0>::iterator jt = shine->getChildren().begin();
		     jt != shine->getChildren().end() && !inShine; jt++)
			inShine = jt->mPerformer == it->mPerformer;
		if (it->mPerformer == sPlayerGroup)
			inShine = true;
		if (!inShine) {
			it->perform(cue, graphics);
			++extra;
			if (!sLogged) // which performers (to check Mario is not among them)
				OSReport("[mp] shine demo: also runs \"%s\"\n", it->mPerformer != nullptr ? it->mPerformer->getName() : "?");
		}
	}
	if (!sLogged) {
		sLogged = true;
		OSReport("[mp] shine demo: the world keeps running (%d more performers of \"%s\")\n", extra,
		         all->getName());
	}
}
#endif

// Frame-rate option: draw lists that clear their own draw buffers. Most buffers are filled by
// unk34's CUE_ENTRY in a game frame and keep their packets until the next one, so the display
// frames between game frames draw them again. But some lists draw a buffer and then clear it
// with a link of CUE_DRAW_INIT alone (GXPost: "DrawBuf AfterIndirect Opa / Xlu", which hold the
// sea's sun sparkle among others): the logic draw emptied them and the display frames drew
// nothing there, so the water flickered at every rate above 30. Above 30 fps such clears are
// held back (logic and display draws) and done just before the next game frame's entries, as
// the GameCube's single draw per frame did. At 30 fps the lists run as they always did.
#include <JSystem/JDrama/JDRDrawBufObj.hpp>
#include <string.h>
extern "C" int port_interp_rate(void); // port/src/port_interp.cpp
static JDrama::TDrawBufObj* sPortHeldClears[32];
static int sPortHeldClearNum;
static TMarDirector* sPortHeldClearDir;

static void portPerformDraw(TPerformList* list, JDrama::TGraphics* graphics)
{
	// SMS_PORT_HOLD_CLEARS=0: the lists run unchanged (comparison runs)
	static const bool sNoHold = getenv("SMS_PORT_HOLD_CLEARS") != nullptr && *getenv("SMS_PORT_HOLD_CLEARS") == '0';
	if (sNoHold || port_interp_rate() == 30) {
		list->perform(CUE_ALL, graphics);
		return;
	}
	for (JGadget::TSingleLinkList<TPerformLink, 0>::iterator it = list->getChildren().begin();
	     it != list->getChildren().end(); it++) {
		JDrama::TViewObj* o = it->mPerformer;
		const char* name    = o != nullptr ? o->getName() : nullptr;
		if (it->mCueFilter == CUE_DRAW_INIT && name != nullptr && strncmp(name, "DrawBuf ", 8) == 0) {
			if (sPortHeldClearDir != gpMarDirector) {
				sPortHeldClearDir = gpMarDirector;
				sPortHeldClearNum = 0;
			}
			bool held = false;
			for (int i = 0; i < sPortHeldClearNum; i++)
				held = held || sPortHeldClears[i] == (JDrama::TDrawBufObj*)o;
			if (!held && sPortHeldClearNum < 32)
				sPortHeldClears[sPortHeldClearNum++] = (JDrama::TDrawBufObj*)o;
			continue;
		}
		it->perform(CUE_ALL, graphics);
	}
}

// Just before a game frame's entries (unk34 CUE_ALL): the clears held back since the last one.
static void portFlushHeldClears()
{
	if (sPortHeldClearDir == gpMarDirector)
		for (int i = 0; i < sPortHeldClearNum; i++)
			sPortHeldClears[i]->getDrawBuffer()->frameInit();
	sPortHeldClearNum = 0;
}

extern "C" int port_mardirector_redraw(void)
{
	TMarDirector* d = gpMarDirector;
	if (d == nullptr || d != sPortDrawDirector || !d->mSetupDone || gpCamera == nullptr)
		return 0;
	f32* view = &gpCamera->getUnk1EC()[0][0];
	f32* proj = &gpCamera->getUnk16C()[0][0];
	if (!port_interp_redraw_begin(view, proj))
		return 0;

	// Shadow requests made by the last ticks are drawn and cleared by the next logic draw.
	TMBindShadowManager* shadows = gpBindShadowManager;
	int requestNum = 0, quadAryNum = 0, squareNum = 0;
	u16 modelShadowNum = 0;
	u8 unk49 = 0;
	bool unk65 = false;
	TModelShadowInfo modelShadow;
	if (shadows != nullptr) {
		if (sPortShadowRequests == nullptr)
			sPortShadowRequests = (TCircleShadowRequest*)malloc(0x200 * sizeof(TCircleShadowRequest));
		requestNum     = shadows->mRequestNum;
		quadAryNum     = shadows->mQuadAryNum;
		squareNum      = shadows->mSquareShadowNum;
		modelShadowNum = shadows->mModelShadowNum;
		unk49          = shadows->unk49;
		unk65          = shadows->unk65;
		memcpy(sPortShadowRequests, shadows->mRequests, requestNum * sizeof(TCircleShadowRequest));
		memcpy(&modelShadow, shadows->mModelShadows, sizeof(TModelShadowInfo));
	}

	// No sound effects may start from a redraw (it is not a game frame).
	const u8 gate                = SMSGetMSound()->mSeGateMask;
	SMSGetMSound()->mSeGateMask = MSSeGate_None;

	// Only the view half of unk34: the draw buffers keep the logic frame's entries, whose
	// J3D packets read their model's current draw matrices when drawn.
	const u32 viewCues = CUE_CALC_VIEW | CUE_SET_PROJECTION;
	JDrama::TGraphics entry = sPortEntryGraphics;
	d->unk34->perform(viewCues, &entry); // interpolated
	if (shadows != nullptr) {
		// W9-23: the view pass does not make the requests again that the logic unk34 made under
		// CUE_ENTRY (Mario's and most objects' shadows), so a display frame drew without them
		// and the shadows blinked at 30 Hz. The GX list gets the logic frame's whole list
		// (port_interp_shadow_requests moves each request to its interpolated place).
		memcpy(shadows->mRequests, sPortShadowRequests, requestNum * sizeof(TCircleShadowRequest));
		memcpy(shadows->mModelShadows, &modelShadow, sizeof(TModelShadowInfo));
		shadows->mRequestNum     = requestNum;
		shadows->mModelShadowNum = modelShadowNum;
	}
	JDrama::TGraphics local_140 = sPortDrawGraphics;
	portPerformDraw(d->unk40, &local_140);
	// (unk38, unk3C: pollution layers and the graffiti check, logic frames only)
	portPerformDraw(d->mPerformListGX, &local_140);
	if ((gpSilhouetteManager->unk48 > 0.0f ? true : false) || gpCamera->unk2C8 != -1)
		portPerformDraw(d->mPerformListSilhouette, &local_140);
	portPerformDraw(d->mPerformListGXPost, &local_140);
	GXInvalidateTexAll();

	if (port_interp_redraw_draw_end(view, proj)) { // live camera and models from here
		entry = sPortEntryGraphics;
		d->unk34->perform(viewCues, &entry);
	}

	SMSGetMSound()->mSeGateMask = gate;
	if (shadows != nullptr) {
		memcpy(shadows->mRequests, sPortShadowRequests, requestNum * sizeof(TCircleShadowRequest));
		memcpy(shadows->mModelShadows, &modelShadow, sizeof(TModelShadowInfo));
		shadows->mRequestNum      = requestNum;
		shadows->mQuadAryNum      = quadAryNum;
		shadows->mSquareShadowNum = squareNum;
		shadows->mModelShadowNum  = modelShadowNum;
		shadows->unk49            = unk49;
		shadows->unk65            = unk65;
	}
	return 1;
}
#endif

int TMarDirector::direct()
{
	int dt = 600 / (int)SMSGetVSyncTimesPerSec();

	if (!mSetupDone) {
		if (!OSIsThreadTerminated(&gSetupThread))
			return 0;

		void* local_40;
		OSJoinThread(&gSetupThread, &local_40);
		if (local_40)
			return 4;

#ifdef SMS_PORT
		{
			PORT_PROF_SCOPE(PP_SETUP); // w14
			setupObjects();
		}
		port_prof_stage_ready(); // w14: the stage's own frames start now
		// A new stage: draw-buffer clears held back in the last one refer to its freed objects
		// (a new director can sit at the old one's address, so that check alone is not enough).
		sPortHeldClearNum = 0;
		sPortHeldClearDir = this;
#else
		setupObjects();
#endif
		mSetupDone = true;
	}

	u32 desiredAppState = TApplication::APP_STATE_DEFAULT;

	JDrama::TGraphics local_140;

	u8 prevSeGateMask = SMSGetMSound()->mSeGateMask;
	mPendingSimulationTime += dt;

	int i = 0;
	for (;;) {
#ifdef SMS_PORT
		// w14 SMS_PORT_PROFILE: simulation ticks vs. draw perform lists
		PORT_PROF_SCOPE(checkFlag(DIRECTOR_FLAG_LAST_SIMULATION_TICK) ? PP_GXGEN
		                                                               : PP_MOVE);
#endif
		if (!checkFlag(DIRECTOR_FLAG_LAST_SIMULATION_TICK)) {
			++i;
			if (i == 1)
				onFlag(DIRECTOR_FLAG_FIRST_SIMULATION_TICK);
			mPendingSimulationTime -= 5;
			if (mPendingSimulationTime < 5)
				onFlag(DIRECTOR_FLAG_LAST_SIMULATION_TICK);

			// inline?
			u32 blacklistedCues = 0;
			u8 seGateMask       = prevSeGateMask;
			if (checkFlag(DIRECTOR_FLAG_LAST_SIMULATION_TICK)) {
				if (unk258)
					unk258->stageLoop();
			} else {
				blacklistedCues |= CUE_CALC_ANIM;
				seGateMask &= ~MSSeGate_Continuous;
			}
			SMSGetMSound()->mSeGateMask = seGateMask;

			switch (mState) {
			case STATE_PAUSE_MENU:
			case STATE_CARD_SAVE:
			case STATE_UNK12:
#ifdef SMS_PORT_AURORA
				// multiplayer: the world keeps running behind the menu; TMario::perform
				// holds the local Mario still (port/src/port_mp_world.cpp)
				if (port_mp_world_runs_behind(mState))
					break;
#endif
				blacklistedCues |= CUE_CALC_ANIM;
				blacklistedCues |= CUE_MOVE;
				break;

			case STATE_GUIDE:
#ifdef SMS_PORT_AURORA
				if (port_mp_world_runs_behind(mState))
					break;
#endif
				blacklistedCues |= CUE_CALC_ANIM;
				blacklistedCues |= CUE_MOVE;
				break;
			}

			if (!(blacklistedCues & CUE_MOVE))
				++mMoveTickCount;
			++mTickCount;
			if (checkFlag(DIRECTOR_FLAG_FIRST_SIMULATION_TICK)) {
				if (mState == STATE_UNK4 || mState == STATE_UNK7)
					SMSRumbleMgr->update();
			} else {
				for (int i = 0; i < 4; ++i) {
					TMarioGamePad* pad = unk18[i];
					pad->resetButtons();

					unk18[i]->updateMeaning();
					unk18[i]->offFlag(TMarioGamePad::PAD_FLAG_0x40);
				}
			}

			u32 tmp = 0;
			if (checkFlag(DIRECTOR_FLAG_FIRST_SIMULATION_TICK))
				tmp |= 1;
			if (checkFlag(DIRECTOR_FLAG_LAST_SIMULATION_TICK))
				tmp |= 2;
			local_140.unk0 = tmp;

			// inline
			bool bVar1 = true;
			if ((mMoveTickCount & 1) || (mMoveTickCount & 2))
				bVar1 = false;

			if (bVar1)
				gpObjHitCheck->checkActorsHit();
			else
				gpObjHitCheck->clearHitNum();

			u32 movementCue = ~blacklistedCues;
			if (mMoveTickCount & 1)
				movementCue &= ~CUE_MOVEMENT_GATE_A;
			if (mMoveTickCount & 2)
				movementCue &= ~CUE_MOVEMENT_GATE_B;
			if (checkDemoFlag(DEMO_FLAG_SHINE_GET_STOP_THE_WORLD))
#ifdef SMS_PORT_AURORA
				portShineDemoPerform(mShinePfLstMov, mPerformListMovement, movementCue,
				                     &local_140); // w15
#else
				mShinePfLstMov->perform(movementCue, &local_140);
#endif
			else
				mPerformListMovement->perform(movementCue, &local_140);

			u32 someCue = 0;
			if (!checkFlag(DIRECTOR_FLAG_LAST_SIMULATION_TICK))
				someCue |= CUE_CALC_ANIM;
			u32 unk30Cue = ~someCue;
			unk30->perform(unk30Cue, &local_140);
			movement();
			if (!(blacklistedCues & CUE_CALC_ANIM)) {
				if (checkDemoFlag(DEMO_FLAG_SHINE_GET_STOP_THE_WORLD))
#ifdef SMS_PORT_AURORA
					portShineDemoPerform(mShinePfLstAnm, mPerformListCalcAnim, ~blacklistedCues,
					                     &local_140); // w15
#else
					mShinePfLstAnm->perform(~blacklistedCues, &local_140);
#endif
				else
					mPerformListCalcAnim->perform(~blacklistedCues, &local_140);
			}

			if (checkFlag(DIRECTOR_FLAG_LAST_SIMULATION_TICK)) {
				local_140.unk0 = 0;
#ifdef SMS_PORT
				port_mp_logic_frame(); // w15 multiplayer: local snapshot, puppet poses
				sPortEntryGraphics = local_140; // port_mardirector_redraw
				portFlushHeldClears(); // draw buffers the last draws left filled (frame-rate option)
				if (gpCamera != nullptr)
					port_interp_entry_begin(&gpCamera->getUnk1EC()[0][0], &gpCamera->getUnk16C()[0][0]);
				else
					port_interp_entry_begin(nullptr, nullptr);
#endif
				unk34->perform(CUE_ALL, &local_140);
#ifdef SMS_PORT
				port_interp_entry_end();
#endif
				break;
			}
		} else {
			local_140.unk0 = 0;
#ifdef SMS_PORT
			sPortDrawGraphics = local_140; // port_mardirector_redraw
			sPortDrawDirector = this;
			port_interp_logic_draw_begin();
#endif
#ifdef SMS_PORT
			portPerformDraw(unk40, &local_140);
			portPerformDraw(unk38, &local_140);
			portPerformDraw(unk3C, &local_140);
			portPerformDraw(mPerformListGX, &local_140);
			if ((gpSilhouetteManager->unk48 > 0.0f ? true : false)
			    || gpCamera->unk2C8 != -1) {
				portPerformDraw(mPerformListSilhouette, &local_140);
			}
			portPerformDraw(mPerformListGXPost, &local_140);
#else
			unk40->perform(CUE_ALL, &local_140);
			unk38->perform(CUE_ALL, &local_140);
			unk3C->perform(CUE_ALL, &local_140);
			mPerformListGX->perform(CUE_ALL, &local_140);
			if ((gpSilhouetteManager->unk48 > 0.0f ? true : false)
			    || gpCamera->unk2C8 != -1) {
				mPerformListSilhouette->perform(CUE_ALL, &local_140);
			}
			mPerformListGXPost->perform(CUE_ALL, &local_140);
#endif
			GXInvalidateTexAll();
#ifdef SMS_PORT
			port_interp_logic_draw_end();
#endif
		}
		desiredAppState = changeState();
		offFlag(DIRECTOR_FLAG_LAST_SIMULATION_TICK
		        | DIRECTOR_FLAG_FIRST_SIMULATION_TICK);
	}

	gpMSound->mSeGateMask = prevSeGateMask;
#ifdef SMS_PORT
	// SMS_PORT_LOGMARIO=1 (debug, off by default): Mario's position, status and HP
	// every 10 s, so unattended level runs show that the game is actually playing.
	static const bool sLogMario = getenv("SMS_PORT_LOGMARIO") != nullptr;
	if (sLogMario && mState == STATE_UNK4 && gpMarioPos != nullptr && mTickCount % 1200 < 2)
		OSReport("[port] tick %d: Mario at (%.0f, %.0f, %.0f) status %08x HP %d\n", mTickCount,
		         gpMarioPos->x, gpMarioPos->y, gpMarioPos->z, SMS_GetMarioStatus(),
		         SMS_GetMarioHP());
#endif
	return desiredAppState;
}

static void decideNextStage()
{
	TGameSequence local_3C;

	int stage = SMS_getShineStage(SMSGetApplication()->mCurrArea.getStage());
	switch (stage) {
	case 0:
		local_3C.set(1, 0xff, JDrama::TFlagT<u16>());
		break;
	case 1:
	case 2:
	default:
		local_3C.set(1, 0xff, JDrama::TFlagT<u16>());
		break;
	}
	SMSGetApplication()->setNextArea(local_3C);
}

static void decideNextStageOfMiss()
{
	const TGameSequence& curArea = SMSGetApplication()->mCurrArea;
	if (SMS_isExMap() || curArea.getStage() == 0 || curArea.getStage() == 60)
		SMSGetApplication()->mNextArea.set(curArea.getStage(), 0, 0);
	else
		decideNextStage();
}

static bool checkDefeatShadowMarioAll()
{
	static u8 stages[] = { 0x6, 0x10, 0x1A, 0x24, 0x2E, 0x38, 0x42 };

	for (int i = 0; i < 7; ++i)
		if (!TFlagManager::getInstance()->getShineFlag(stages[i]))
			return false;

	return true;
}

static int decideNextScenario(u8 param_1)
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	int result = 0;
	switch (param_1) {
	case 1:
		if (TFlagManager::getInstance()->getBool(MSF_CORONA_VISITED)) {
			result = 2;
		} else if (checkDefeatShadowMarioAll()) {
			result = 9;
		} else if (TFlagManager::getInstance()->getBool(
		               MSF_BOWSER_BOAT_CUTSCENE_TRIGGERED)) {
			result = 8;
		} else if (TFlagManager::getInstance()->getBool(
		               MSF_M_PAINTED_ON_BOATHOUSE)
		           && TFlagManager::getInstance()->getBool(
		               MSF_M_PAINTED_ON_LIGHTHOUSE)) {
			if (TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT) >= 10)
				result = 7;
			else
				result = 6;
		} else if (TFlagManager::getInstance()->getBool(
		               MSF_M_PAINTED_ON_STATUE)) {
			result = 5;
		} else if (TFlagManager::getInstance()->getBool(
		               MSF_BIANCO_POLLUTED_PIRANHA_DEFEATED)) {
			result = 1;
		} else {
			result = 0;
		}
		break;
	}
	return result;
}

int TMarDirector::changeState()
{
	int desiredAppState = TApplication::APP_STATE_DEFAULT;
	u8 nextState        = mState;
	switch (mState) {
	case STATE_UNK0:
		switch (SMSGetApplication()->mCurrArea.getStage()) {
		case 0xf:
			nextState = STATE_UNK4;
			onTransitionFlag(TRANSITION_FLAG_STAGE_BGM_STARTED);
			break;

		case 0x2:
		case 0x3:
		case 0x4:
		case 0x5:
		case 0x6:
		case 0x8:
		case 0x9:
		case 0x34:
			nextState = STATE_UNK1;
			onTransitionFlag(TRANSITION_FLAG_GO_BANNER_PENDING
			                 | TRANSITION_FLAG_SCENARIO_NAME_BANNER_PENDING);
			break;

		case 1:
			if (checkDemoFlag(DEMO_FLAG_CAMERA_DEMO_ON_START)) {
				nextState = STATE_UNK1;
				onTransitionFlag(
				    TRANSITION_FLAG_GO_BANNER_PENDING
				    | TRANSITION_FLAG_SCENARIO_NAME_BANNER_PENDING);
			} else {
				nextState = STATE_UNK2;
			}
			break;

		default:
			if (JKRGetResource("/scene/map/camera/startcamera.bck")) {
				nextState = STATE_UNK1;
				onTransitionFlag(TRANSITION_FLAG_GO_BANNER_PENDING
				                 | TRANSITION_FLAG_OPENING_WIPE_PENDING);
			} else {
				nextState = STATE_UNK4;
			}
			break;
		}
		break;

	case STATE_UNK1:
		if (checkDemoFlag(DEMO_FLAG_CAMERA_DEMO_WIPE_STARTED)) {
			if (getConsole()->unk94->unk2BC == 4) {
				nextState = STATE_UNK3;
				offDemoFlag(DEMO_FLAG_CAMERA_DEMO_WIPE_STARTED);
			}
		} else {
			TGameSequence& curArea = SMSGetApplication()->mCurrArea;
			TConsoleStr* str       = getConsole()->unk94;
			f32 iVar2              = gpCamera->getRestDemoFrames() / 120.0f;
#ifdef SMS_PORT_AURORA
			// w9 quality of life: with "Start skips any cutscene" the plaza intros
			// (1,1 and 1,9) can be skipped too.
			if (iVar2 <= str->getWipeCloseTime()
			    || ((port_qol_skip_cutscenes()
			         || ((curArea.getStage() != 1 || curArea.getScenario() != 1)
			             && (curArea.getStage() != 1 || curArea.getScenario() != 9)))
			        && unk18[0]->checkFrameMeaning(
			            TMarioGamePad::MEANING_START
			            | TMarioGamePad::MEANING_MENU_A
			            | TMarioGamePad::MEANING_MENU_B))) {
#else
			if (iVar2 <= str->getWipeCloseTime()
			    || ((curArea.getStage() != 1 || curArea.getScenario() != 1)
			        && (curArea.getStage() != 1 || curArea.getScenario() != 9)
			        && unk18[0]->checkFrameMeaning(
			            TMarioGamePad::MEANING_START
			            | TMarioGamePad::MEANING_MENU_A
			            | TMarioGamePad::MEANING_MENU_B))) {
#endif
				onDemoFlag(DEMO_FLAG_CAMERA_DEMO_WIPE_STARTED);
				getConsole()->unk94->startCloseWipe(
				    checkTransitionFlag(TRANSITION_FLAG_OPENING_WIPE_PENDING)
				    != FALSE);
				offTransitionFlag(TRANSITION_FLAG_OPENING_WIPE_PENDING);
			}
		}
		break;

	case STATE_UNK3:
		if (SMSGetApplication()->mCurrArea.getStage() == 1) {
			if (getConsole()->unk94->unk2BC == 6)
				nextState = STATE_UNK2;
		} else if (getConsole()->unk94->unk2BC == 6
		           && !gpMarioOriginal->checkStatusType(
		               MARIO_STATUS_FLAG_UNK1000)) {
			nextState = STATE_UNK4;
		}
		break;

	case STATE_UNK2:
		if (!unkE0->unk26
		    && !gpMarioOriginal->checkStatusType(MARIO_STATUS_FLAG_UNK1000))
			nextState = STATE_UNK4;
		break;

	case STATE_UNK4:
		nextState = updateGameMode();
		break;

	case STATE_PAUSE_MENU:
		switch (unkAC->getNextState()) {
		case 0:
			nextState = STATE_UNK4;
			break;
		case 1:
			unkE4     = 4;
			nextState = STATE_UNK12;
			unkB4     = TApplication::APP_STATE_DONE;
			break;
		case 5:
			decideNextStage();
			mFlags &= ~DIRECTOR_FLAG_MOVIE_PENDING;
			moveStage();
			unkE4     = 2;
			nextState = STATE_UNK9;
			break;
		}
		break;

	case STATE_GUIDE:
		if (unk78->unkC4 && SMSGetApplication()->getFader()->isFullyFadedIn())
			nextState = STATE_UNK4;
		break;

	case STATE_CARD_SAVE: {
		switch (unkAC->mCardSave->getNextState()) {
		case 0:
			if (unk261 == 7) {
				TFlagManager::getInstance()->restore();
				TFlagManager::getInstance()->setBool(true, MSF_UNK30001);
				if (!TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT)) {
					SMSGetApplication()->mNextArea.set(0, 0, 0);
				} else {
					SMSGetApplication()->mNextArea.set(1, 0xff, 0);
				}
				mFlags &= ~DIRECTOR_FLAG_MOVIE_PENDING;
				moveStage();
				SMSGetApplication()->getFader()->setFadeStatus(
				    TSMSFader::FADE_STATUS_FULLY_FADED_OUT);
				desiredAppState = TApplication::APP_STATE_GAMEPLAY;
			} else {
				nextState = STATE_UNK4;
			}
			break;
		case 1:
			if (unk261 == 7) {
				SMSGetApplication()->getFader()->setFadeStatus(
				    TSMSFader::FADE_STATUS_FULLY_FADED_OUT);
				desiredAppState = TApplication::APP_STATE_DONE;
			} else {
				unkE4     = 4;
				nextState = STATE_UNK12;
				unkB4     = TApplication::APP_STATE_DONE;
			}
			break;
		}
		break;
	}

	case STATE_UNK7:
		if (SMSGetApplication()->getFader()->isFullyFadedOut()
		    && (MSBgm::getHandle(2) == 0 || mTickCount - unk60 >= 1200)) {
			if (TFlagManager::getInstance()->getFlag(MSF_LIFE_COUNT) >= 0) {
				TFlagManager::getInstance()->setBool(
				    true, MSF_LOST_LIFE_IN_PREV_STAGE);
				decideNextStageOfMiss();
				mFlags &= ~DIRECTOR_FLAG_MOVIE_PENDING;
				moveStage();
				unkE4 = 0xf;
				SMSGetApplication()->getFader()->setColor(
				    JUtility::TColor(0, 0, 0, 0xff));
				nextState = STATE_UNK12;
			} else {
				SMSGetApplication()->getFader()->startWipe(0xE, 0.3f, 0.0f);
				SMSGetApplication()->getFader()->setColor(
				    JUtility::TColor(0, 0, 0, 0xff));
				unk261    = 7;
				nextState = STATE_CARD_SAVE;
				getConsole()->startDisappearStar();
				getConsole()->startDisappearCoin();
			}
		}
		break;

	case STATE_UNK9:
	case STATE_UNK12:
		if (SMSGetApplication()->getFader()->isFullyFadedOut()
		    && gpMSound->checkWaveOnAram(MS_WAVE_DEFAULT))
			desiredAppState = unkB4;
		break;
	}

	if (unk18[0]->isSomethingPushed()
	    && gpCardManager->getLastStatus() != CARD_RESULT_BUSY
	    && (mFlags & DIRECTOR_FLAG_LAST_SIMULATION_TICK)
	    && !checkTransitionFlag(TRANSITION_FLAG_RESET_HANDLED)) {
		nextState = STATE_UNK12;
		onTransitionFlag(TRANSITION_FLAG_RESET_HANDLED);
		unkE4 = 4;
		unkB4 = TApplication::APP_STATE_DONE;
	}

	if (nextState != mState) {
#ifdef SMS_PORT
		OSReport("[port] TMarDirector state %d -> %d (tick %d)\n", mState, nextState, mTickCount);
#endif
		currentStateFinalize(nextState);
		nextStateInitialize(nextState);
		mState = nextState;
	}

	return desiredAppState;
}

void TMarDirector::currentStateFinalize(u8 next_state)
{
	switch (mState) {
	case STATE_UNK0:
		static_cast<JDrama::TViewObj*>(JDrama::TNameRefGen::search("Group 2D"))
		    ->unkC.off(CUE_MOVE | CUE_CALC_ANIM | CUE_DRAW);
		static_cast<JDrama::TViewObj*>(JDrama::TNameRefGen::search("Guide"))
		    ->unkC.on(CUE_MOVE | CUE_CALC_ANIM | CUE_DRAW);

		SMSGetApplication()->getFader()->startWipe(unkE4, 0.4f, 0.0f);
		SMSRumbleMgr->reset();
		break;

	case STATE_UNK1:
		unk18[0]->offFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		gpCamera->endDemoCamera();
		getConsole()->unk94->startOpenWipe();
		MSMainProc::endStageEntranceDemo(
		    SMSGetApplication()->mCurrArea.getStage(),
		    SMSGetApplication()->mCurrArea.getScenario());
		break;

	case STATE_UNK4:
		if (unk124 == 0)
			OSStopStopwatch(&unkE8);
		unk18[0]->offFlag(TMarioGamePad::PAD_FLAG_GAME_INPUT);
		break;

	case STATE_PAUSE_MENU:
		unk18[0]->offFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		SMSRumbleMgr->finishPause();
		if (SMSGetApplication()->mCurrArea.getStage() == 1)
			THPPlayerPlay();
		break;

	case STATE_GUIDE:
		unk18[0]->offFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		SMSRumbleMgr->finishPause();

		static_cast<JDrama::TViewObj*>(JDrama::TNameRefGen::search("Group 2D"))
		    ->unkC.off(CUE_MOVE | CUE_CALC_ANIM | CUE_DRAW);
		static_cast<JDrama::TViewObj*>(JDrama::TNameRefGen::search("Guide"))
		    ->unkC.on(CUE_MOVE | CUE_CALC_ANIM | CUE_DRAW);

		SMSSwitch2DArchive("guide", gArBkConsole);
		if (SMSGetApplication()->mCurrArea.getStage() == 1)
			THPPlayerPlay();
		break;

	case STATE_CARD_SAVE:
		unk18[0]->offFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		SMSRumbleMgr->finishPause();
		if (SMSGetApplication()->mCurrArea.getStage() == 1)
			THPPlayerPlay();
		switch (unk261) {
		case 3:
#ifdef VERSION_GMSP01
			getConsole()->startAppearBalloon(0x4B, true);
#else
			getConsole()->startAppearBalloon(0xE0048, true);
#endif
			break;

		case 4:
#ifdef VERSION_GMSP01
			getConsole()->startAppearBalloon(0x4C, true);
#else
			getConsole()->startAppearBalloon(0xE0049, true);
#endif
			break;
		}
		break;
	}
}

void TMarDirector::setMario()
{
	bool cVar4 = false;
	if (TFlagManager::getInstance()->getBool(MSF_GOT_SHINE_IN_PREV_STAGE)) {
		TFlagManager::getInstance()->setBool(false,
		                                     MSF_GOT_SHINE_IN_PREV_STAGE);
		cVar4 = true;
	}

	u8 uVar10 = unkD0;

	TMarioPositionObj* marioSetPosition = static_cast<TMarioPositionObj*>(
	    JDrama::TNameRefGen::search("マリオセット位置"));
	if (!marioSetPosition || marioSetPosition->unkD0 == 0)
		uVar10 = 0;

	switch (unkD1) {
	case 1: {
		f32 fVar1 = 0.0f;

		const JGeometry::TVec3<f32>* pos = nullptr;

		if (uVar10) {
			fVar1 = marioSetPosition->getUnk70(uVar10 - 1).y;
			pos   = &marioSetPosition->getUnk10(uVar10 - 1);
		}
		gpMarioOriginal->rollingStart(pos, fVar1);
	} break;

	case 2: {
		int iVar9 = 0;
		switch (SMS_getShineStage(SMSGetApplication()->mPrevArea.getStage())) {
		case 5:
		case 6:
		case 7:
			iVar9 = 1;
			break;
		case 8:
			iVar9 = 2;
			break;
		}
		f32 fVar1 = 0.0f;

		const JGeometry::TVec3<f32>* pos = nullptr;

		if (uVar10) {
			fVar1 = marioSetPosition->getUnk70(uVar10 - 1).y;
			pos   = &marioSetPosition->getUnk10(uVar10 - 1);
		}
		gpMarioOriginal->returnStart(pos, fVar1, cVar4, iVar9);
	} break;

	case 4:
		gpMarioOriginal->toroccoStart();
		break;

	case 0:
	default: {
		const JGeometry::TVec3<f32>* pos = nullptr;
		if (uVar10)
			pos = &marioSetPosition->getUnk10(uVar10 - 1);
		gpMarioOriginal->waitingStart(pos, 0.0f);
	} break;
	}

	TMario* mario                = gpMarioOriginal;
	const TGameSequence& curArea = SMSGetApplication()->mCurrArea;
	if (mario->checkFlag(MARIO_FLAG_HAS_FLUDD)) {
		switch (curArea.getStage()) {
		case 1:
		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 8:
		case 9:
		case 0x10:
		case 0x2C:
		case 0x34:
		case 0x39:
			break;

		case 0x3C:
			mario->mWaterGun->changeNozzle(TWaterGun::Rocket, true);
			break;

		default: {
			TWaterGun* gun = mario->mWaterGun;
			gun->changeNozzle(
			    (TWaterGun::TNozzleType)TFlagManager::getInstance()->getFlag(
			        MSF_SECONDARY_NOZZLE),
			    true);
			gpMarioOriginal->mWaterGun->changeNozzle(TWaterGun::Spray, true);
		} break;
		}
	}

	u8 uVar6 = SMS_getShineIDofExStage(curArea.getStage());
	if (uVar6 != 0xff && TFlagManager::getInstance()->getShineFlag(uVar6) == 0)
		gpMarioOriginal->offFlag(MARIO_FLAG_HAS_FLUDD);
}

void TMarDirector::nextStateInitialize(u8 next_state)
{
	TGameSequence& currSeq = SMSGetApplication()->mCurrArea;

	switch (next_state) {
	case STATE_UNK1: {
		const char* demoName = "startcamera";
		unk18[0]->onFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		unk68 = 0;
		if (currSeq.getStage() == 1
		    && checkDemoFlag(DEMO_FLAG_CAMERA_DEMO_ON_START)) {
			if (currSeq.getScenario() == 8) {
				switch (TFlagManager::getInstance()->getFlag(
				    MSF_SHADOW_MARIO_EVENT)) {
				case 0:
					if (TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT)
					    >= 0x14)
						demoName = "mareopen_startcamera";
					break;
				case 1:
					demoName = "yoshi_startcamera";
					break;
				case 2:
					demoName = "turbo_startcamera";
					break;
				case 3:
					demoName = "rocket_startcamera";
					break;
				}
			} else {
				if (TFlagManager::getInstance()->getBool(
				        MSF_RICCO_UNLOCKABLE)) {
					demoName = "sinkricco";
				} else {
					if (TFlagManager::getInstance()->getBool(
					        MSF_GELATO_UNLOCKABLE))
						demoName = "sinkmamma";
				}
			}
		}
		gpCamera->startDemoCamera(demoName, nullptr, -1, 0.0f, true);
		if (checkTransitionFlag(TRANSITION_FLAG_SCENARIO_NAME_BANNER_PENDING)) {
			getConsole()->unk94->startAppearScenario();
			offTransitionFlag(TRANSITION_FLAG_SCENARIO_NAME_BANNER_PENDING);
		}
		MSMainProc::startStageEntranceDemo(currSeq.getStage(),
		                                   currSeq.getScenario());
		break;
	}

	case STATE_UNK3:
		unk68 = 0;
		if (!checkTransitionFlag(TRANSITION_FLAG_STAGE_BGM_STARTED)) {
			MSMainProc::startStageBGM(currSeq.getStage(),
			                          currSeq.getScenario());
			setMario();
			onTransitionFlag(TRANSITION_FLAG_STAGE_BGM_STARTED);
		}
		break;

	case STATE_UNK2:
		if (!checkTransitionFlag(TRANSITION_FLAG_STAGE_BGM_STARTED)) {
			MSMainProc::startStageBGM(currSeq.getStage(),
			                          currSeq.getScenario());
			setMario();
			onTransitionFlag(TRANSITION_FLAG_STAGE_BGM_STARTED);
		}
		if (mMap != 0xf)
			getConsole()->unkC.off(CUE_MOVE | CUE_CALC_ANIM | CUE_DRAW);
		if (currSeq.getStage() == 1)
			THPPlayerPlay();
		break;

	case STATE_UNK4:
		if (mState <= STATE_UNK3 && mMap != 0xf)
			getConsole()->unkC.off(CUE_MOVE | CUE_CALC_ANIM | CUE_DRAW);
		if (checkTransitionFlag(TRANSITION_FLAG_GO_BANNER_PENDING)) {
			getConsole()->unk94->startAppearGo();
			offTransitionFlag(TRANSITION_FLAG_GO_BANNER_PENDING);
		}
		if (!checkTransitionFlag(TRANSITION_FLAG_STAGE_BGM_STARTED)) {
			MSMainProc::startStageBGM(currSeq.getStage(),
			                          currSeq.getScenario());
			setMario();
			onTransitionFlag(TRANSITION_FLAG_STAGE_BGM_STARTED);
		}
		if (!unk124)
			OSStartStopwatch(&unkE8);
		unk18[0]->onFlag(TMarioGamePad::PAD_FLAG_GAME_INPUT);
		break;

	case STATE_UNK12:
		if (currSeq.getStage() == 1)
			THPPlayerStop();
	// !!!fallthrough!!!
	case STATE_UNK9: {
		SMSGetApplication()->getFader()->startWipe(unkE4, 0.4f, 0.0f);
		if (unkE4 == 8)
			SMSGetMSound()->startSoundSystemSE(MSD_SE_MA_INTO_DOKAN, 0, nullptr,
			                                   0);
		MSound* sound = gpMSound;
#ifdef SMS_PORT // float -> u32 through __cvt_fp2unsigned, as on GC
		sound->fadeOutAllSound(sms_cvt_fp2unsigned(SMSGetVSyncTimesPerSec() * 0.4f));
#else
		sound->fadeOutAllSound(SMSGetVSyncTimesPerSec() * 0.4f);
#endif
		SMSRumbleMgr->reset();
		for (int i = 0; i < 4; ++i)
			JUTGamePad::CRumble::stopMotor(unk18[i]->mPortNum);
		break;
	}

	case STATE_PAUSE_MENU:
		if (currSeq.getStage() == 1)
			THPPlayerPause();
		SMSRumbleMgr->startPause();
		unkAC->setDrawStart();
		for (int i = 0; i < 4; ++i)
			JUTGamePad::CRumble::stopMotor(unk18[i]->mPortNum);
		unk18[0]->onFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		break;

	case STATE_GUIDE:
		if (currSeq.getStage() == 1)
			THPPlayerPause();
		SMSRumbleMgr->startPause();
		for (int i = 0; i < 4; ++i)
			JUTGamePad::CRumble::stopMotor(unk18[i]->mPortNum);
		unk18[0]->onFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		static_cast<JDrama::TViewObj*>(JDrama::TNameRefGen::search("Group 2D"))
		    ->unkC.on(CUE_MOVE | CUE_CALC_ANIM | CUE_DRAW);
		static_cast<JDrama::TViewObj*>(JDrama::TNameRefGen::search("Guide"))
		    ->unkC.off(CUE_MOVE | CUE_CALC_ANIM | CUE_DRAW);
		SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_WIPE_IN, 0, nullptr, 0);
		SMSGetApplication()->getFader()->startWipe(6, 1.0f, 0.0f);
		unk78->setup(nullptr);
		unk78->startMoveCursor();
		break;

	case STATE_CARD_SAVE:
		if (currSeq.getStage() == 1)
			THPPlayerPause();
		SMSRumbleMgr->startPause();
		unkAC->mCardSave->init(unk261);
		for (int i = 0; i < 4; ++i)
			JUTGamePad::CRumble::stopMotor(unk18[i]->mPortNum);
		switch (unk261) {
		case 3:
		case 4:
			SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_COLLECT_DELIGHT, 0,
			                                   nullptr, 0);
			break;
		}
		unk18[0]->onFlag(TMarioGamePad::PAD_FLAG_MENU_INPUT);
		break;

	case STATE_UNK7:
		SMSGetMarDirector()->getConsole()->unk94->startAppearMiss();
		TFlagManager::getInstance()->decFlag(MSF_LIFE_COUNT, 1);
		unk60 = mTickCount;
		SMSGetApplication()->getFader()->setColor(
		    JUtility::TColor(0, 0, 0, 0xff));
		if (TFlagManager::getInstance()->getFlag(MSF_LIFE_COUNT) >= 0) {
			MSBgm::startBGM(MSD_BGM_BOSS);
			if (checkDemoFlag(DEMO_FLAG_HELL_DEAD))
				SMSGetApplication()->getFader()->startWipe(2, 0.0f, 2.0f);
			else
				SMSGetApplication()->getFader()->startWipe(10, 0.0f, 2.2f);
		} else {
			MSBgm::startBGM(MSD_BGM_BOSSHANA_2ND3RD);
			SMSGetApplication()->getFader()->startWipe(0xD, 0.0f, 2.0f);
		}
		break;
	}
}

u8 TMarDirector::updateGameMode()
{
	u8 r29 = mState;
#ifdef SMS_PORT_AURORA
	// w9 quality of life: the Shine demo and its save are over: play on.
	if (sPortContinueShine && sPortContinueDir == this && sPortContinueShineObj == (void*)unk25C
	    && unk124 == 0
	    && !checkFlag(DIRECTOR_FLAG_CARD_SAVE_PENDING | DIRECTOR_FLAG_DEMO_PENDING
	                  | DIRECTOR_FLAG_SHINE_GET_PENDING))
		portContinueAfterShine(this);
#endif

	switch (unk124) {
	case 0:
		if (!checkFlag(~(DIRECTOR_FLAG_SHINE_TAKEN
		                 | DIRECTOR_FLAG_LAST_SIMULATION_TICK
		                 | DIRECTOR_FLAG_FIRST_SIMULATION_TICK))) {
			if (SMS_CheckMarioFlag(MARIO_FLAG_GAME_OVER)) {
				mFlags |= DIRECTOR_FLAG_GAME_OVER_PENDING;
				break;
			}

			if (mMap != 15) {
				if (unk18[0]->testTrigger(0x10)) {
					r29 = STATE_GUIDE;
					break;
				}

				if (unk18[0]->checkFrameMeaning(TMarioGamePad::MEANING_START)) {
					if (gpMarioOriginal->checkActionThing3()) {
						r29 = STATE_PAUSE_MENU;
						break;
					}

					SMSGetMSound()->startSoundSystemSE(MSD_SE_SY_NOT_COLLECT, 0,
					                                   nullptr, 0);
				}
			}
		} else {
			if (checkFlag(DIRECTOR_FLAG_GAME_OVER_PENDING)) {
				offFlag(DIRECTOR_FLAG_GAME_OVER_PENDING);
				r29 = STATE_UNK7;
				TFlagManager::getInstance()->setFlag(MSF_GOLD_COIN_COUNT, 0);
				break;
			}

			if (checkFlag(DIRECTOR_FLAG_SHINE_GET_PENDING)) {
				offFlag(DIRECTOR_FLAG_SHINE_GET_PENDING);
				unk126 = 3;

				TGCConsole2* console = gpMarDirector->mConsole;
				console->unk94->startAppearShineGet();
				console->unk47 = 1;
				MSBgm::startBGM(MSD_BGM_CHUBOSS);
				TFlagManager::getInstance()->setBool(
				    true, MSF_GOT_SHINE_IN_PREV_STAGE);
				TFlagManager::getInstance()->setShineFlag(unk25C->getEventId());
#ifdef SMS_PORT_AURORA
				port_mp_shine_got(unk25C->getEventId()); // w10: shared objectives
				port_mp_objx_shine_got(unk25C->getEventId()); // w8 (P1b): remote Shine banner
				// w9 quality of life: stay in the stage (no white fade-out, no next area);
				// the save prompt / auto-save still follows the demo.
				sPortContinueShine = portCanContinueAfterShine(unk25C);
				sPortContinueDir      = this;
				sPortContinueShineObj = unk25C;
				if (sPortContinueShine) {
					onFlag(DIRECTOR_FLAG_SHINE_TAKEN | DIRECTOR_FLAG_CARD_SAVE_PENDING);
					unk261 = 6;
					break;
				}
#endif
				f32 fVar3     = unkDC->mRate;
				f32 fadeInSec = 1.0f;
				unkDC->registFadeout(fadeInSec * fVar3, fVar3 * 5.3333333f);
				onFlag(DIRECTOR_FLAG_SHINE_TAKEN
				       | DIRECTOR_FLAG_CARD_SAVE_PENDING
				       | DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
				unk261 = 6;
				decideNextStage();
				break;
			}

			if (checkFlag(DIRECTOR_FLAG_DEMO_PENDING)) {
				unk126 = 3;
				break;
			}

			if (checkFlag(DIRECTOR_FLAG_CARD_SAVE_PENDING)) {
#ifdef SMS_PORT_AURORA
				// w9 quality of life: save without the prompt after a Shine.
				if (unk261 == 6 && port_qol_autosave()) {
					const int saved = portAutoSave();
					if (saved == 0)
						break; // the card is being written
					if (saved == 1) {
						offFlag(DIRECTOR_FLAG_CARD_SAVE_PENDING);
						port_qol_toast("Game saved");
						break;
					}
					// not possible here: the normal prompt
				}
#endif
				offFlag(DIRECTOR_FLAG_CARD_SAVE_PENDING);
				r29 = STATE_CARD_SAVE;
				break;
			}

			if (checkFlag(DIRECTOR_FLAG_GATE_DEMO_STAGE_TRANSITION_PENDING)) {
				offFlag(DIRECTOR_FLAG_GATE_DEMO_STAGE_TRANSITION_PENDING);
				onFlag(DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
				unk126                        = 3;
				const TGameSequence& nextArea = SMSGetApplication()->mNextArea;
				if (nextArea.getStage() == 5) {
					fireStartDemoCamera("hodai_dpt_pinna1", nullptr, -1, 0.0f,
					                    false, nullptr, 0, nullptr, 0);
					if (unk254 != nullptr)
						unk254->startDemo();
					break;
				}

				if (nextArea.getStage() == 6) {
					fireStartDemoCamera("camera_sirena_gate_in", nullptr, -1,
					                    0.0f, false, nullptr, 0, nullptr, 0);
					break;
				}

				if (nextArea.getStage() == 8) {
					fireStartDemoCamera("camera_monte_gate_in", nullptr, -1,
					                    0.0f, false, nullptr, 0, nullptr, 0);
					break;
				}

				break;
			}

			if (checkFlag(DIRECTOR_FLAG_ACTOR_DEMO_STAGE_TRANSITION_PENDING)) {
				offFlag(DIRECTOR_FLAG_ACTOR_DEMO_STAGE_TRANSITION_PENDING);
				onFlag(DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
				unk126 = 3;
				fireStartDemoCamera(nullptr, nullptr, -1, 0.0f, false, nullptr,
				                    0, unk250, 1);
				break;
			}

			if (checkFlag(DIRECTOR_FLAG_STAGE_TRANSITION_PENDING)) {
				moveStage();
				r29 = STATE_UNK9;
				break;
			}
		}
		break;

	case 2:
		if (checkFlag(DIRECTOR_FLAG_DEMO_PENDING)) {
			unk126 = 4;
		} else {
			if (unkB0->unk248 == 0)
				unk126 = 0;
		}
		break;

	case 3:
	case 4: {
#ifdef PORT_SKIP_CUTSCENES
		// PORT (not in the original): Start (press edge) skips the current
		// in-engine cutscene. The camera is fast-forwarded to its end so the
		// natural end-of-demo path below runs on a following frame. Start
		// meaning is edge-triggered, so this press can't also open the pause
		// menu once the game resumes.
#ifdef SMS_PORT_AURORA
		// w9: now the quality-of-life option "Start skips any cutscene" (off by default,
		// so the original behaviour is the default; port/src/port_qol.cpp).
		if (port_qol_skip_cutscenes()
		    && unk18[0]->checkFrameMeaning(TMarioGamePad::MEANING_START))
			gpCamera->skipDemoCamera();
#else
		if (unk18[0]->checkFrameMeaning(TMarioGamePad::MEANING_START))
			gpCamera->skipDemoCamera();
#endif
#endif
		bool bVar5  = false;
		bool uVar15 = 0;
		if (checkFlag(DIRECTOR_FLAG_END_DEMO_PENDING)) {
			uVar15 = 1;
			bVar5  = true;
			offFlag(DIRECTOR_FLAG_END_DEMO_PENDING);
		} else {
			if (!gpCamera->getRestDemoFrames()) {
				if (!MSBgm::getHandle(2) || mTickCount - unk60 >= 720) {
					bVar5  = true;
					uVar15 = mDemoQueue[mDemoQueueHead].unk10;
				}
			}
		}

		if (bVar5) {
			u32 prev = mDemoQueueHead++;
			mDemoQueueHead &= 0x7;
			TDemoInfo* info = &mDemoQueue[prev];
			if (mDemoQueueHead != mDemoQueueTail) {
				gpCamera->endDemoCamera();
				if (info->unk14 != nullptr)
					(*info->unk14)(info->unk18, 1);

				TDemoInfo* next = &mDemoQueue[mDemoQueueHead];
				gpCamera->startDemoCamera(next->unk0, next->unk4, next->unk8,
				                          next->unkC, next->unk10);
				if (next->unk14 != nullptr)
					(*next->unk14)(next->unk18, 0);
			} else {
				offFlag(DIRECTOR_FLAG_DEMO_PENDING);
				unk126 = unk124 == 4 ? 2 : 0;
				if (uVar15 != 0)
					gpCamera->endDemoCamera();
				if (info->unk14 != nullptr)
					(*info->unk14)(info->unk18, 1);
			}
		}
	} break;
	}

	if (mDemoQueueHead == mDemoQueueTail)
		offFlag(DIRECTOR_FLAG_DEMO_PENDING);

	unk125 = unk124;

	if (unk124 != unk126) {
		switch (unk124) {
		case 2:
			if (unk126 == 0) {
				unkA0 = 0;
				unkA4 = 0;
				unk18[0]->offFlag(TMarioGamePad::PAD_FLAG_TALK_SELECT);
				OSStartStopwatch(&unkE8);
			}
			break;

		case 3:
		case 4:
			if (unk124 == 4)
				MSMainProc::fromTalkingCameraDemo(unk124 == 4);
			else
				MSMainProc::fromInnerCameraDemo();
			unk18[0]->offFlag(TMarioGamePad::PAD_FLAG_NO_INPUT);
			OSStartStopwatch(&unkE8);
			break;
		}

		switch (unk126) {
		case 0:
			break;

		case 1:
			unkA0->onLiveFlag(LIVE_FLAG_UNK40000);
			unkA0->unkC.off(CUE_MOVE | CUE_CALC_ANIM);
			unk18[0]->onFlag(TMarioGamePad::PAD_FLAG_TALK_SELECT);
			OSStopStopwatch(&unkE8);
			break;

		case 2:
			if (unk124 == 1)
				unkB0->openTalkWindow(unkA0);
			break;

		case 3:
		case 4:
			if (unk126 == 4)
				MSMainProc::toTalkingCameraDemo();
			else
				MSMainProc::toInnerCameraDemo();
			unk18[0]->onFlag(TMarioGamePad::PAD_FLAG_NO_INPUT);
			if ((int)mDemoQueue[mDemoQueueHead].unk20.get() == 1) {
				gpCamera->startGateDemoCamera(mDemoQueue[mDemoQueueHead].unk1C);
			} else {
				gpCamera->startDemoCamera(mDemoQueue[mDemoQueueHead].unk0,
				                          mDemoQueue[mDemoQueueHead].unk4,
				                          mDemoQueue[mDemoQueueHead].unk8,
				                          mDemoQueue[mDemoQueueHead].unkC,
				                          mDemoQueue[mDemoQueueHead].unk10);
				if (mDemoQueue[mDemoQueueHead].unk14 != nullptr)
					(*mDemoQueue[mDemoQueueHead].unk14)(
					    mDemoQueue[mDemoQueueHead].unk18, 0);
			}
			OSStopStopwatch(&unkE8);
			unk60 = mTickCount;
			break;
		}

		unk124 = unk126;
	}

	if (unk128 & 0x1) {
		unk128 &= ~0x1;
		unk128 |= 0x2;
	} else {
		unk128 &= ~0x2;
	}

	return r29;
}

void TMarDirector::moveStage()
{
	unkB4 = TApplication::APP_STATE_GAMEPLAY;
	unkE4 = 15;
	SMSGetApplication()->getFader()->setColor(JUtility::TColor(0, 0, 0, 0xff));

	u8 sVar4 = SMS_getShineStage(SMSGetApplication()->mNextArea.getStage());
	u8 sVar5 = SMS_getShineStage(SMSGetApplication()->mCurrArea.getStage());
	if (sVar4 != sVar5)
		TFlagManager::getInstance()->setFlag(MSF_GOLD_COIN_COUNT, 0);

	TGameSequence& nextArea = SMSGetApplication()->mNextArea;

	if (nextArea.getScenario() == 0xff)
		switch (nextArea.getStage()) {
		case 1:
			unkE4         = 2;
			nextArea.unk1 = decideNextScenario(nextArea.getStage());
			TFlagManager::getInstance()->setFlag(MSF_EPISODE, 0);
			break;

		case 13: {
			unkE4     = 2;
			u32 thing = 0;
			switch (TFlagManager::getInstance()->getFlag(MSF_EPISODE)) {
			case 0:
				thing = 0;
				break;
			case 2:
				thing = 1;
				break;
			case 4:
				thing = 2;
				break;
			case 5:
				thing = 3;
				break;
			case 6:
				thing = 4;
				break;
			case 7:
				thing = 5;
				break;
			}
			nextArea.unk1 = thing;
			break;
		}

		case 0x3A: {
			unkE4     = 2;
			u32 thing = 0;
			switch (TFlagManager::getInstance()->getFlag(MSF_EPISODE)) {
			case 0:
				thing = 1;
				break;
			case 7:
				thing = 0;
				break;
			}
			nextArea.unk1 = thing;
			break;
		}

		case 7: {
			unkE4     = 2;
			u32 thing = 0;
			switch (TFlagManager::getInstance()->getFlag(MSF_EPISODE)) {
			case 1:
				thing = 0;
				break;
			case 2:
				thing = 1;
				break;
			case 3:
			case 4:
				thing = 2;
				break;
			case 6:
				thing = 3;
				break;
			case 7:
				thing = 4;
				break;
			}
			nextArea.unk1 = thing;
			break;
		}

		case 14: {
			unkE4     = 2;
			u32 thing = 0;
			switch (TFlagManager::getInstance()->getFlag(MSF_EPISODE)) {
			case 3:
				thing = 0;
				break;
			case 4:
				thing = 1;
				break;
			}
			nextArea.unk1 = thing;
			break;
		}

		case 2:
		case 3:
		case 4:
		case 5:
		case 6:
		case 8:
			unkE4 = 8;
			unkB4 = TApplication::APP_STATE_TITLE;
			break;

		case 9:
			SMSGetApplication()->getFader()->setColor(
			    JUtility::TColor(0xD2, 0xD2, 0xD2, 0xFF));
			unkB4 = TApplication::APP_STATE_TITLE;
			break;

		case 0x34:
			unkE4         = 8;
			nextArea.unk1 = 0;
			TFlagManager::getInstance()->setFlag(MSF_EPISODE, 0);
			break;

		case 0:
			nextArea.unk1 = 0;
			TFlagManager::getInstance()->setFlag(MSF_EPISODE, 0);
			break;

		default:
			unkE4         = 2;
			nextArea.unk1 = 0;
			break;
		}

	if (nextArea.getScenario() != 0xff) {
		if (mFlags & DIRECTOR_FLAG_MOVIE_PENDING) {
			unkE4 = 15;
			SMSGetApplication()->getFader()->setColor(
			    JUtility::TColor(0, 0, 0, 0xff));
			unkB4 = TApplication::APP_STATE_MOVIE;
		} else {
			unkB4 = TApplication::APP_STATE_GAMEPLAY;
		}
	}

	if (gpMarioOriginal->checkFlag(MARIO_FLAG_HAS_FLUDD)) {
		int nozzle = gpMarioOriginal->mWaterGun->mSecondNozzle;
		if (nozzle == TWaterGun::Yoshi)
			nozzle = TWaterGun::Hover;
		TFlagManager::getInstance()->setFlag(MSF_SECONDARY_NOZZLE, nozzle);
	}
}

JStage::TObject* TMarDirector::JSGFindObject(const char* param_1,
                                             JStage::TEObject param_2) const
{
	if (strcmp("cam_int1", param_1) == 0) {
		TMarDirector* casted = const_cast<TMarDirector*>(this);
		JDrama::TCamera* cam = (JDrama::TCamera*)casted->search("camera 1");
		return cam;
	}

	if (strcmp("mario", param_1) == 0) {
		TMarDirector* casted  = const_cast<TMarDirector*>(this);
		JDrama::TActor* mario = (JDrama::TActor*)casted->search("マリオ");
		return mario;
	}

	return JDrama::TDirector::JSGFindObject(param_1, param_2);
}
