#include <NPC/NpcEvent.hpp>
#include <stdio.h>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <System/MarDirector.hpp>
#include <System/FlagManager.hpp>
#include <Strategic/Spine.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjHide.hpp>
#include <MSound/MSound.hpp>
#include <NPC/NpcBase.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/Mario.hpp>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

extern const TNerveBase<TLiveActor>* NerveGetByIndex(int index);

int TNpcEvent::mDownSunflowerNum = 0;

static void IsNpcFlagOn_(TSpcTypedInterp<TEventWatcher>* interp, u32 arg_num,
                         u32 flag)
{
	interp->verifyArgNum(1, &arg_num);
	TBaseNPC* npc = SPC_INT2PTR(TBaseNPC*, interp->pop().getDataInt());
	int result    = 0;
	if (npc->checkLiveFlag(flag))
		result = 1;
	interp->push(result);
}

static inline const TNerveBase<TLiveActor>* NpcLatestNerveA(TBaseNPC* npc)
{
	return npc->mSpine->getLatestNerve();
}

static inline const TNerveBase<TLiveActor>* NpcLatestNerve(TBaseNPC* npc)
{
	return NpcLatestNerveA(npc);
}

static void CheckNerve4Npc_(TSpcTypedInterp<TEventWatcher>* interp, u32 arg_num,
                            bool param_3)
{
	interp->verifyArgNum(2, &arg_num);
	int nerveId   = interp->pop().getDataInt();
	TBaseNPC* npc = SPC_INT2PTR(TBaseNPC*, interp->pop().getDataInt());
	int result    = 0;
	const TNerveBase<TLiveActor>* expected = NerveGetByIndex(nerveId);
	// TODO: the target calls getLatestNerve() out of line (the inliner ran out
	// of depth there), while getCurrentNerve() is inlined. Some wrapper inline
	// is probably missing around this.
	const TNerveBase<TLiveActor>* actual = param_3
	                                           ? NpcLatestNerve(npc)
	                                           : npc->mSpine->getCurrentNerve();
	if (actual == expected)
		result = 1;
	interp->push(result);
}

static void evGetAddressFromViewObjName(TSpcTypedInterp<TEventWatcher>* interp,
                                        u32 arg_num)
{
	interp->verifyArgNum(1, &arg_num);
	const char* name = interp->pop().getDataString();
	JDrama::TViewObj* viewObj
	    = static_cast<JDrama::TViewObj*>(JDrama::TNameRefGen::search(name));
	interp->push(SPC_PTR2INT(viewObj));
}

static void evCheckCurNerve4Npc(TSpcTypedInterp<TEventWatcher>* interp,
                                u32 arg_num)
{
	CheckNerve4Npc_(interp, arg_num, false);
}

static void evCheckLatestNerve4Npc(TSpcTypedInterp<TEventWatcher>* interp,
                                   u32 arg_num)
{
	CheckNerve4Npc_(interp, arg_num, true);
}

static void evIsNpcSinkBottom(TSpcTypedInterp<TEventWatcher>* interp,
                              u32 arg_num)
{
	IsNpcFlagOn_(interp, arg_num, TBaseNPC::LIVE_FLAG_SINK_BOTTOM);
}

static void evIsGameModeNormal(TSpcTypedInterp<TEventWatcher>* interp,
                               u32 arg_num)
{
	interp->verifyArgNum(0, &arg_num);
	int result = 0;
	if (gpMarDirector->unk124 == 0)
		result = 1;
	interp->push(result);
}

static void ev__ForceStartTalkExceptNpc(TSpcTypedInterp<TEventWatcher>* interp,
                                        u32 arg_num)
{
	interp->verifyArgNum(1, &arg_num);
	int result = 0;
	// TODO: uuuh...
	// TODO: the target copies the popped slice's data word into a second,
	// otherwise unused stack slot here (like a by-value TSpcSlice argument of
	// an inline that only reads mData).
	(void)interp->pop();

	if (!gpMarDirector->isTalkOrDemoModeNow() && SMS_IsMarioTouchGround4cm()
	    && !gpMarioOriginal->checkStatusType(MARIO_STATUS_FLAG_JUMPING)) {

		TBaseNPC* dummyNpc = static_cast<TBaseNPC*>(
		    JDrama::TNameRefGen::search("ダミーＮＰＣ"));

		if (dummyNpc) {
			gpMarDirector->unkA0  = dummyNpc;
			gpMarDirector->unk126 = 1;

			result = 1;
		}
	}
	interp->push(result);
}

static void ev__ForceStartTalk(TSpcTypedInterp<TEventWatcher>* interp,
                               u32 arg_num)
{
	interp->verifyArgNum(1, &arg_num);

	int result = 0;

	if (!gpMarDirector->isTalkOrDemoModeNow() && SMS_IsMarioTouchGround4cm()
	    && !gpMarioOriginal->checkStatusType(MARIO_STATUS_FLAG_JUMPING)) {

		gpMarDirector->unkA0  = SPC_INT2PTR(TBaseNPC*, interp->pop().getDataInt());
		gpMarDirector->unk126 = 1;

		result = 1;
	} else {
		interp->pop();
	}

	interp->push(result);
}

static void evConnectDummyNpc(TSpcTypedInterp<TEventWatcher>* interp,
                              u32 arg_num)
{
	interp->verifyArgNum(1, &arg_num);

	int result = 0;

	TBaseNPC* dummyNpc
	    = static_cast<TBaseNPC*>(JDrama::TNameRefGen::search("ダミーＮＰＣ"));
	if (dummyNpc != nullptr) {
		const JDrama::TActor* actor
		    = SPC_INT2PTR(const JDrama::TActor*, interp->pop().getDataInt());
		dummyNpc->setDummyConnectActor(actor);
		result = 1;
	} else {
		interp->pop();
	}

	interp->push(result);
}

static void evOnTalkToDummyNpc(TSpcTypedInterp<TEventWatcher>* interp,
                               u32 arg_num)
{
	interp->verifyArgNum(0, &arg_num);
	TBaseNPC* dummyNpc
	    = static_cast<TBaseNPC*>(JDrama::TNameRefGen::search("ダミーＮＰＣ"));
	if (dummyNpc != nullptr) {
		dummyNpc->offLiveFlag(LIVE_FLAG_DEAD);
		dummyNpc->offLiveFlag(LIVE_FLAG_UNK40000);
		dummyNpc->mTalkForbidCount = 120;
	}

	interp->push();
}

static void evSetNpcBalloonMessage(TSpcTypedInterp<TEventWatcher>* interp,
                                   u32 arg_num)
{
	interp->verifyArgNum(3, &arg_num);
	int fVar1     = interp->pop().getDataInt();
	int fVar2     = interp->pop().getDataInt();
	TBaseNPC* npc = SPC_INT2PTR(TBaseNPC*, interp->pop().getDataInt());
	npc->setBalloonMessage(fVar2, fVar1);
	interp->push();
}

static void evSetNpcTalkForbidCount(TSpcTypedInterp<TEventWatcher>* interp,
                                    u32 arg_num)
{
	interp->verifyArgNum(2, &arg_num);
	u16 count             = interp->pop().getDataInt();
	TBaseNPC* npc         = SPC_INT2PTR(TBaseNPC*, interp->pop().getDataInt());
	npc->mTalkForbidCount = count;
	interp->push();
}

static void evNpcDanceOn(TSpcTypedInterp<TEventWatcher>* interp, u32 arg_num)
{
	interp->verifyArgNum(1, &arg_num);
	TBaseNPC* npc = SPC_INT2PTR(TBaseNPC*, interp->pop().getDataInt());
	npc->npcDanceIn();
	interp->push();
}

static void evNpcDanceOffHappyOn(TSpcTypedInterp<TEventWatcher>* interp,
                                 u32 arg_num)
{
	interp->verifyArgNum(1, &arg_num);
	TBaseNPC* npc = SPC_INT2PTR(TBaseNPC*, interp->pop().getDataInt());
	npc->offActionFlag(TBaseNPC::NPC_ACTION_DANCE);
	npc->npcHappyIn(2);
	interp->push();
}

static void evResetFruitNum(TSpcTypedInterp<TEventWatcher>* interp, u32 arg_num)
{
	interp->verifyArgNum(1, &arg_num);
	TFruitBasketEvent* basket = SPC_INT2PTR(TFruitBasketEvent*, interp->pop().getDataInt());
	basket->reset();
	interp->push();
}

static void evGetFruitNum(TSpcTypedInterp<TEventWatcher>* interp, u32 arg_num)
{
	interp->verifyArgNum(2, &arg_num);
	int fVar4                 = interp->pop().getDataInt();
	TFruitBasketEvent* basket = SPC_INT2PTR(TFruitBasketEvent*, interp->pop().getDataInt());

	int iVar3 = 0;
	switch (fVar4) {
	case 0:
		iVar3 = 0;
		break;

	case 1:
		iVar3 = 4;
		break;

	case 2:
		iVar3 = 3;
		break;

	case 3:
		iVar3 = 1;
		break;
	}

	int num = basket->getFruitNum(iVar3);
	interp->push(num);
}

static void evSetFruitType(TSpcTypedInterp<TEventWatcher>* interp, u32 arg_num)
{
	interp->verifyArgNum(3, &arg_num);
	int fVar5                 = interp->pop().getDataInt();
	int fVar4                 = interp->pop().getDataInt();
	TFruitBasketEvent* basket = SPC_INT2PTR(TFruitBasketEvent*, interp->pop().getDataInt());

	if (fVar5 != 0) {
		int r28;
		switch (fVar4) {
		case 0:
			r28 = ACTOR_TYPE_FRUIT_BANANA;
			break;

		case 1:
			r28 = ACTOR_TYPE_FRUIT_COCONUT;
			break;

		case 2:
			r28 = ACTOR_TYPE_FRUIT_PINE;
			break;

		case 3:
			r28 = ACTOR_TYPE_FRUIT_DURIAN;
			break;
		}
		basket->unk150 = r28;
	} else {
		basket->unk150 = 0;
	}
	interp->push();
}

static void evFireStartDemoCamera(TSpcTypedInterp<TEventWatcher>* interp,
                                  u32 arg_num)
{
	interp->verifyArgNum(1, &arg_num);
	const char* cameraName = interp->pop().getDataString();
	SMSGetMarDirector()->fireStartDemoCamera(cameraName, nullptr, -1, 0.0f,
	                                         true, nullptr, 0, nullptr,
	                                         JDrama::TFlagT<u16>());
	interp->push();
}

static void evIsDemoMode(TSpcTypedInterp<TEventWatcher>* interp, u32 arg_num)
{
	interp->verifyArgNum(0, &arg_num);
	int result = 0;
	if (gpMarDirector->isDemoModeNow())
		result = 1;
	interp->push(result);
}

static void evCheckMonteClear(TSpcTypedInterp<TEventWatcher>* interp,
                              u32 arg_num)
{
	interp->verifyArgNum(1, &arg_num);
	int fVar1 = interp->pop().getDataInt();

	char buffer[32];
	snprintf(buffer, 32, "モンテ%d", fVar1);
	TBaseNPC* npc = static_cast<TBaseNPC*>(JDrama::TNameRefGen::search(buffer));

	int b;
	if (!npc->checkLiveFlag(LIVE_FLAG_UNK400000) && npc->isClean())
		b = true;
	else
		b = false;

	interp->push(b);
}

void TNpcEvent::initNpcBuiltin(TSpcTypedBinary<TEventWatcher>* param_1)
{
	// clang-format off
  param_1->bindSystemDataToSymbol("getAddressFromViewObjName", (uintptr_t)&evGetAddressFromViewObjName);
  param_1->bindSystemDataToSymbol("checkCurNerve4Npc", (uintptr_t)&evCheckCurNerve4Npc);
  param_1->bindSystemDataToSymbol("checkLatestNerve4Npc", (uintptr_t)&evCheckLatestNerve4Npc);
  param_1->bindSystemDataToSymbol("isNpcSinkBottom", (uintptr_t)&evIsNpcSinkBottom);
  param_1->bindSystemDataToSymbol("isGameModeNormal", (uintptr_t)&evIsGameModeNormal);
  param_1->bindSystemDataToSymbol("__forceStartTalk", (uintptr_t)&ev__ForceStartTalk);
  param_1->bindSystemDataToSymbol("__forceStartTalkExceptNpc", (uintptr_t)&ev__ForceStartTalkExceptNpc);
  param_1->bindSystemDataToSymbol("connectDummyNpc", (uintptr_t)&evConnectDummyNpc);
  param_1->bindSystemDataToSymbol("onTalkToDummyNpc", (uintptr_t)&evOnTalkToDummyNpc);
  param_1->bindSystemDataToSymbol("setNpcBalloonMessage", (uintptr_t)&evSetNpcBalloonMessage);
  param_1->bindSystemDataToSymbol("setNpcTalkForbidCount", (uintptr_t)&evSetNpcTalkForbidCount);
  param_1->bindSystemDataToSymbol("npcDanceOn", (uintptr_t)&evNpcDanceOn);
  param_1->bindSystemDataToSymbol("npcDanceOffHappyOn", (uintptr_t)&evNpcDanceOffHappyOn);
  param_1->bindSystemDataToSymbol("resetFruitNum", (uintptr_t)&evResetFruitNum);
  param_1->bindSystemDataToSymbol("getFruitNum", (uintptr_t)&evGetFruitNum);
  param_1->bindSystemDataToSymbol("setFruitType", (uintptr_t)&evSetFruitType);
  param_1->bindSystemDataToSymbol("fireStartDemoCamera", (uintptr_t)&evFireStartDemoCamera);
  param_1->bindSystemDataToSymbol("isDemoMode", (uintptr_t)&evIsDemoMode);
  param_1->bindSystemDataToSymbol("checkMonteClear", (uintptr_t)&evCheckMonteClear);
	// clang-format on
}

void TNpcEvent::initDownSunflowerNum()
{
	if (TFlagManager::getInstance()->getBool(MSF_SUNFLOWERS_LEFT_TO_RESCUE))
		mDownSunflowerNum = 5;
	else
		mDownSunflowerNum = 0;
}

static s32 ReviveSunflowerCallBack(uintptr_t param_1, u32 param_2)
{
	// FAKEMATCH(frame): 16 bytes; real construct unknown
	volatile u32 pad[4]; (void)pad;
	if (param_2 == 0) {
		TBaseNPC* sunflower = (TBaseNPC*)param_1;
		sunflower->sunflowerReviveIn();
		u32 sound = MSD_SE_NPC_MONTE_C_CLEANUP;
		if (TNpcEvent::mDownSunflowerNum == 0)
			sound = MSD_SE_SY_PROBLEM_SOLVED;
		if (SMSGetMSound()->gateCheck(sound))
			MSoundSESystem::MSoundSE::startSoundNpcActor(
			    sound, &sunflower->mPosition, 0, nullptr, 0, 4);
	}

	return 1;
}

void TNpcEvent::reviveOneSunflower()
{
	if (mDownSunflowerNum > 0) {
		static const char* sViewObjName = "ひまわり";

		char acStack_50[0x40];

		int idx = 5 - mDownSunflowerNum;
		snprintf(acStack_50, 0x40, "%s%d", sViewObjName, idx);

		TBaseNPC* npc
		    = static_cast<TBaseNPC*>(JDrama::TNameRefGen::search(acStack_50));
		TMarDirector* director = gpMarDirector;
		--mDownSunflowerNum;
		JGeometry::TVec3<f32>* position = &npc->unk1B8;

		static const char* sCameraNames[] = {
			"ひまわりカメラ0", "ひまわりカメラ1", "ひまわりカメラ2",
			"ひまわりカメラ3", "ひまわりカメラ4",
		};

		// FAKEMATCH(frame): 8 bytes; real construct unknown
		volatile u32 pad[2]; (void)pad;
		director->fireStartDemoCamera(sCameraNames[idx], position, -1,
		                                   0.0f, true, &ReviveSunflowerCallBack,
		                                   (uintptr_t)npc, nullptr, 0);

		if (mDownSunflowerNum == 0) {
			gpItemManager->makeShineAppearWithDemo(
			    "ひまわり用シャイン", "ひまわりシャインカメラ", position->x,
			    position->y + 500.0f, position->z);
			TFlagManager::getInstance()->setBool(false,
			                                     MSF_SUNFLOWERS_LEFT_TO_RESCUE);
		}
	}
}
