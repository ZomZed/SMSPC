#include <Enemy/BossTelesa.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/HamuKuri.hpp>
#include <Enemy/Telesa.hpp>
#include <Camera/Camera.hpp>
#include <Camera/CameraShake.hpp>
#include <GC2D/GCConsole2.hpp>
#include <Map/Map.hpp>
#include <Map/MapCollisionEntry.hpp>
#include <MarioUtil/DrawUtil.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/MtxUtil.hpp>
#include <MarioUtil/PacketUtil.hpp>
#include <MarioUtil/RandomUtil.hpp>
#include <MarioUtil/RumbleMgr.hpp>
#include <MarioUtil/ScreenUtil.hpp>
#include <MarioUtil/TexUtil.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/SDLModel.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <MSound/SoundEffects.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/Mario.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/SharedParts.hpp>
#include <Strategic/Strategy.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>
#include <System/Particles.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DCluster.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <JSystem/JUtility/JUTTexture.hpp>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

f32 TBossTelesa::mEnemyGenRate           = 0.5f;
f32 TBossTelesa::mItemGenRate            = 0.1f;
u8 TBossTelesa::mNormalAlpha             = 150;
f32 TBossTelesa::mBaseHoseiPosY          = -300.0f;
f32 TBossTelesa::mRouletteUpRate         = 0.03f;
int TBossTelesa::mTelesaGenerateInterval = 400;
f32 TBossTelesa::mCameraMoveLimit        = 1000.0f;
f32 TBossTelesa::mCameraMoveSp           = 0.02f;
bool TBossTelesa::mGenerateTelesa;
bool TBossTelesa::mGenerateBubble;
int TBossTelesa::mEnemyTestType;

static const char* btelesa_bastable[] = {
	"/scene/btelesa/bas/btelesa_appear.bas",
	"/scene/btelesa/bas/btelesa_bero_hit.bas",
	"/scene/btelesa/bas/btelesa_damage.bas",
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	"/scene/btelesa/bas/btelesa_lick.bas",
	nullptr,
	nullptr,
	nullptr,
	"/scene/btelesa/bas/btelesa_roll.bas",
	"/scene/btelesa/bas/btelesa_spicy.bas",
	nullptr,
	nullptr,
	"/scene/btelesa/bas/btelesa_wait.bas",
	"/scene/btelesa/bas/btelesa_wet.bas",
};

TBubbleSaveLoadParams::TBubbleSaveLoadParams(const char* path)
    : TWalkerEnemyParams(path)
    , PARAM_INIT(mSLLiveTime, 200)
    , PARAM_INIT(mSLNumDivision, 5)
    , PARAM_INIT(mSLMaxScale, 1.5f)
    , PARAM_INIT(mSLAddPosBase, 50.0f)
    , PARAM_INIT(mSLRateExpand, 1.001f)
    , PARAM_INIT(mSLDeadHeight, 300.0f)
{
	TParams::load(mPrmPath);
}

TBubbleManager::TBubbleManager(const char* name)
    : TSmallEnemyManager(name)
{
}

void TBubbleManager::load(JSUMemoryInputStream& stream)
{
	unk38 = new TBubbleSaveLoadParams("/enemy/bubble.prm");
	TSmallEnemyManager::load(stream);
}

TSpineEnemy* TBubbleManager::createEnemyInstance()
{
	return new TBubble();
}

void TBubbleManager::createModelData()
{
	static TModelDataLoadEntry entry[] = {
		{ "btelesa_osenbubbles_ind.bmd", 0x11020000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

TBubble::TBubble(const char* name)
    : TWalkerEnemy(name)
    , mCarried(nullptr)
    , mPosOffsetY(0.0f)
    , mIsSplit(false)
    , unk1D1(false)
{
}

void TBubble::init(TLiveManager* manager)
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;

	TWalkerEnemy::init(manager);
	mActorType = ACTOR_TYPE_EFFECT_OBJ;
	unk150     = 0x11;
	mParams    = (TBubbleSaveLoadParams*)getSaveParam();
	mSpine->initWith(&TNerveBubbleLive::theNerve());
	mMActor->setLightType(3);
	SMS_ChangeTextureAll(mMActor->getModel()->getModelData(), "H_ma_rak_dummy",
	                     *static_cast<TScreenTexture*>(
	                          JDrama::TNameRefGen::search("スクリーンテクスチャ"))
	                          ->getTexture()
	                          ->getTexInfo());
}

void TBubble::reset()
{
	TWalkerEnemy::reset();
	onLiveFlag(LIVE_FLAG_UNK8);
	mPosOffsetY = TMsRange<f32>(50.0f, 150.0f).rand();
	mIsSplit    = false;
	unk1D1      = true;
	unk1D2      = false;
	mCarried    = nullptr;
	mSpine->initWith(&TNerveBubbleLive::theNerve());
}

void TBubble::split()
{
	int num = mParams->mSLNumDivision.get();
	for (int i = 0; i < num; ++i) {
		TBubble* bubble = (TBubble*)gpConductor->makeOneEnemyAppear(
		    mPosition, "バブルマネージャー", 1);
		if (bubble == nullptr)
			break;

		bubble->reset();
		bubble->mPosition = mPosition;
		bubble->mPosition.y += mPosOffsetY;
		bubble->mPosOffsetY = 0.0f;
		bubble->mIsSplit    = true;

		TMsRange<f32> range(-2.0f, 2.0f);
		JGeometry::TVec3<f32> velocity;
		velocity.x        = range.rand();
		velocity.y        = range.rand();
		velocity.z        = range.rand();
		bubble->mVelocity = velocity;
	}
}

f32 TBubble::getGravityY() const
{
	if (mIsSplit) {
		if (unk1D1)
			return 0.001f;
		return 0.0f;
	}
	return mGravity;
}

void TBubble::kill()
{
	if (checkLiveFlag(LIVE_FLAG_DEAD))
		return;

	if (mCarried != nullptr) {
		if (unk1D2)
			mCarried->receiveMessage(this, HIT_MESSAGE_THROWN);
		else
			mCarried->kill();
		mCarried = nullptr;
	}

	mHitPoints = 1;
	if (mSpine->getCurrentNerve() != &TNerveSmallEnemyDie::theNerve()) {
		mSpine->reset();
		mSpine->setNext(&TNerveSmallEnemyDie::theNerve());
		mSpine->pushAfterCurrent(mSpine->getDefault());
		onLiveFlag(LIVE_FLAG_UNK20000);
	}
	onLiveFlag(LIVE_FLAG_UNK40);
}

void TBubble::behaveToWater(THitActor*)
{
	if (mSpine->getCurrentNerve() == &TNerveBubbleLive::theNerve()
	    && mMActor->checkCurBckFromIndex(10)) {
		kill();
		TMapObjBase* item = gpItemManager->makeObjAppear(
		    mPosition.x, 20.0f + mPosition.y, mPosition.z,
		    ACTOR_TYPE_BOTTLE_LARGE, true);
		if (item != nullptr)
			((TItem*)item)->killByTimer(1200);
	}
}

void TBubble::attackToMario()
{
	sendAttackMsgToMario();
	kill();
}

void TBubble::calcRootMatrix()
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;

	if (!isEaten()) {
		mPosition.y = 150.0f + (mGroundHeight + mPosOffsetY);
		MsMtxSetXYZRPH(mMActor->getModel()->getBaseTRMtx(), mPosition.x,
		               mPosition.y, mPosition.z, mRotation.x, mRotation.y,
		               mRotation.z);
		mMActor->getModel()->setBaseScale(mScaling);
	}
}

void TBubble::setDeadAnm() { setBckAnm(9); }

MtxPtr TBubble::getTakingMtx() { return mMActor->getModel()->getBaseTRMtx(); }

const char** TBubble::getBasNameTable() const { return btelesa_bastable; }

const TNerveBubbleLive& TNerveBubbleLive::theNerve()
{
	static TNerveBubbleLive instance;
	return instance;
}

void TBubble::appendItem()
{
	mCarried          = nullptr;
	TMapObjBase* item = gpItemManager->makeObjAppear(
	    mPosition.x, mPosition.y, mPosition.z, ACTOR_TYPE_ITEM_UNK8, true);
	if (item != nullptr && item->receiveMessage(this, HIT_MESSAGE_TAKE)) {
		item->appear();
		item->mPosition = mPosition;
		item->setVelocityAndFlag10(0.0f, 15.0f, 0.0f);
		mHeldObject = item;
		mCarried    = item;
	}
}

void TBubble::appendEnemy()
{
	mCarried = nullptr;

	f32 rnd = TMsRange<f32>(0.0f, 100.0f).rand();
	TSmallEnemy* enemy;
	if (rnd < 50.0f) {
		enemy = (TSmallEnemy*)gpConductor->makeOneEnemyAppear(
		    mPosition, "ポポマネージャー", 1);
		enemy->unk154 = 0.6f;
		enemy->reset();
	} else if (rnd < 100.0f) {
		enemy = (TSmallEnemy*)gpConductor->makeOneEnemyAppear(
		    mPosition, "ボム兵マネージャー", 1);
		enemy->unk154 = 0.3f;
		enemy->reset();
	} else if (rnd < 150.0f) {
		enemy = (TSmallEnemy*)gpConductor->makeOneEnemyAppear(
		    mPosition, "テレサマネージャー", 1);
		if (enemy == nullptr)
			return;
		enemy->unk154 = 0.6f;
		enemy->reset();
		((TTelesa*)enemy)->setAttacker();
	} else {
		enemy = (TSmallEnemy*)gpConductor->makeOneEnemyAppear(
		    mPosition, "パックンマネージャー", 1);
		enemy->unk154 = 0.6f;
		enemy->reset();
	}

	if (enemy != nullptr && enemy->receiveMessage(this, HIT_MESSAGE_TAKE)) {
		enemy->onHitFilter(HIT_CATEGORY_BOSS);
		mHeldObject = enemy;
		JGeometry::TVec3<f32> velocity(0.0f, 2.0f, 10.0f);
		enemy->mVelocity = velocity;
		enemy->onLiveFlag(LIVE_FLAG_AIRBORNE);
		mCarried = enemy;
	}
}

BOOL TNerveBubbleLive::execute(TSpineBase<TLiveActor>* spine) const
{
	TBubble* self = (TBubble*)spine->getBody();
	if (spine->getTime() == 0) {
		self->offHitFilter(HIT_FILTER_NO_COLLISION);
		if (!self->mIsSplit) {
			self->setBckAnm(8);
		} else {
			self->setBckAnm(10);
			self->setGoalPath(TPathNode((THitActor*)gpMarioAddress));
		}
		J3DFrameCtrl* ctrl = self->mMActor->getFrameCtrl(ANM_TYPE_BCK);
		ctrl->setFrame(TMsRange<f32>(0.0f, 20.0f).rand());
		self->onLiveFlag(LIVE_FLAG_UNK8);
	} else if (self->checkCurAnmEnd(0)) {
		self->offHitFilter(HIT_FILTER_NO_COLLISION);
		self->setBckAnm(10);
	}

	if (!self->mIsSplit) {
		if (self->mPosOffsetY < self->mParams->mSLAddPosBase.get())
			self->mPosOffsetY += 2.0f;
	} else {
		if (spine->getTime() > 40 && self->unk1D1) {
			JGeometry::TVec3<f32> velocity = self->mVelocity;
			JGeometry::TVec3<f32> scaled;
			scaled.set(velocity);
			scaled.scale(0.98f);
			self->mVelocity = scaled;
		} else {
			self->walkBehavior(0, 1.0f);
		}

		if (spine->getTime() == 80) {
			self->unk1D1 = false;
			self->mVelocity = JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f);
		}
	}

	self->mPosOffsetY += 0.001f;
	if (self->mPosOffsetY
	    > self->mPosition.y + self->mParams->mSLDeadHeight.get()) {
		self->unk1D2 = false;
		self->kill();
	}

	if (self->mScaling.x < self->mParams->mSLMaxScale.get()) {
		f32 scale = self->mScaling.z * self->mParams->mSLRateExpand.get();
		self->mScaling.x = self->mScaling.y = self->mScaling.z = scale;
	}

	if (spine->getTime() > self->mParams->mSLLiveTime.get()) {
		spine->pushAfterCurrent(&TNerveBubbleSplit::theNerve());
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveBubbleSplit, TLiveActor)
{
	TBubble* self = (TBubble*)spine->getBody();
	if (spine->getTime() == 0) {
		self->onHitFilter(HIT_FILTER_NO_COLLISION);
		self->split();
	}

	if (spine->getTime() == 10)
		self->setBckAnm(9);

	if (self->checkCurAnmEnd(0) && self->mMActor->checkCurBckFromIndex(9)) {
		self->unk1D2 = false;
		self->kill();
	}

	return false;
}

TBossTelesaSaveLoadParams::TBossTelesaSaveLoadParams(const char* path)
    : TSpineEnemyParams(path)
    , PARAM_INIT(mSLDamageRadius, 200)
    , PARAM_INIT(mSLDamageHeight, 100)
    , PARAM_INIT(mSLAttackRadius, 220)
    , PARAM_INIT(mSLAttackHeight, 120)
    , PARAM_INIT(mSLGenAttackerTime, 500)
    , PARAM_INIT(mSLGenBubbleTime, 600)
    , PARAM_INIT(mSLHitAngle, 20.0f)
    , PARAM_INIT(mSLNumGenBubble, 5)
    , PARAM_INIT(mSL1stBubbleSp, 10.0f)
    , PARAM_INIT(mSLHideAreaRadius, 500.0f)
    , PARAM_INIT(mSLSlotItemNum, 5)
    , PARAM_INIT(mSLSlotFruitNum, 10)
    , PARAM_INIT(mSLSlotFirstHitCollectRate, 0.1f)
    , PARAM_INIT(mSLSlotHitCollectRate, 0.1f)
    , PARAM_INIT(mSLTransYOffset, 350.0f)
    , PARAM_INIT(mSLStopSlotTime0, 3000)
    , PARAM_INIT(mSLStopSlotTime1, 2000)
    , PARAM_INIT(mSLStopSlotTime2, 1000)
    , PARAM_INIT(mSLSpicyTime, 2000)
{
	TParams::load(mPrmPath);
}

TBossTelesaManager::TBossTelesaManager(const char* name)
    : TEnemyManager(name)
{
}

void TBossTelesaManager::load(JSUMemoryInputStream& stream)
{
	unk38 = new TBossTelesaSaveLoadParams("/enemy/bosstelesa.prm");
	TEnemyManager::load(stream);
}

TSpineEnemy* TBossTelesaManager::createEnemyInstance()
{
	return new TBossTelesa("ボステレサ");
}

void TBossTelesaManager::createModelData()
{
	static TModelDataLoadEntry entry[] = {
		{ "btelesa.bmd", 0x15300000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

void TBossTelesaManager::perform(u32 cue, JDrama::TGraphics* graphics)
{
	TEnemyManager::perform(cue, graphics);
}

BOOL TBossTelesaBody::receiveMessage(THitActor* sender, u32 message)
{
	return mOwner->checkMessage(sender, message);
}

void TBossTelesaBody::checkHit()
{
	unk6C = 0;
	for (int i = 0; i < mColCount; ++i) {
		THitActor* col = mCollisions[i];
		if (col->isActorType(ACTOR_TYPE_MARIO))
			SMS_SendMessageToMario(this, HIT_MESSAGE_ATTACK);
		else
			mOwner->checkHitObject(col);
	}
}

J3DAnmTransform* MActor::getCurBckAnmPtr()
{
	if (!mAnmBck)
		return nullptr;

	return mAnmBck->unk24;
}

void TBossTelesaTongue::checkHit()
{
	for (int i = 0; i < mColCount; ++i) {
		THitActor* col = mCollisions[i];
		if (col->isActorType(ACTOR_TYPE_MARIO)) {
			SMS_SendMessageToMario(this, HIT_MESSAGE_ATTACK);
		} else if (col->isActorType(ACTOR_TYPE_RED_PEPPER)) {
			mOwner->setSpicy((TLiveActor*)col);
		} else if (100.0f + mPosition.y < col->mPosition.y) {
			mOwner->checkHitObject(col);
		}
	}
}

BOOL TBossTelesaTongue::receiveMessage(THitActor*, u32 message)
{
	if (message == HIT_MESSAGE_SPRAYED_BY_WATER)
		mOwner->tongueHitWater();
	return true;
}

void TBossTelesaKillSmallEnemy::checkHit()
{
	unk6C = 0;
	for (int i = 0; i < mColCount; ++i) {
		THitActor* col = mCollisions[i];
		if (col->isHitCategory(HIT_CATEGORY_ENEMY)) {
			TLiveActor* live = (TLiveActor*)col;
			if (col->getActorType() == ACTOR_TYPE_DORO_HAMU_KURI)
				((THamuKuri*)live)->selectCapHolder();
			live->kill();
		}
	}

	JGeometry::TVec3<f32> diff = SMS_GetMarioPos();
	diff.x -= mPosition.x;
	diff.y -= mPosition.y;
	diff.z -= mPosition.z;
	diff.y = 0.0f;
	if (MsVECMag2(&diff) < 300.0f) {
		mOwner->forceHide();
		unk6C = 1;
	}
}

void TTelesaSlot::initMapObj()
{
	TSlotDrum::initMapObj();
	onLiveFlag(LIVE_FLAG_UNK10);
	unk14C = 160.0f;
	unk150 = mPosition.y;
	unk154 = 2.0f;
	unk158 = 2.0f;
	unk15C = 0.01f;
	unk160 = 0.5f;
	unk164 = 0;
	unk168 = 45;
	unk140 = mDamageRadius / 3.0f;
	unk144 = mDamageHeight;
	unk1DC = new TMapCollisionMove;
	unk1DC->init(2, 0, 0, nullptr);
	// TODO: original inlines randomReset here but calls it out of line from
	// the Appear nerve; body duplicated to reproduce that
	{
		TMsRange<s32> range(0, 8);
		for (int i = 0; i < 3; ++i) {
			unk13C[i] = unk168 * range.rand();
			unk198[i] = false;
		}
	}
}

#pragma dont_inline on
void TTelesaSlot::randomReset()
{
	TMsRange<s32> range(0, 8);
	for (int i = 0; i < 3; ++i) {
		unk13C[i] = unk168 * range.rand();
		unk198[i] = false;
	}
}

#pragma dont_inline reset

void TTelesaSlot::calcRootMatrix()
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;

	u8 spinning = 0;
	for (int i = 0; i < 3; ++i)
		if (unk138[i] != 0.0f)
			spinning = 1;

	if (spinning) {
		if (unk1E0)
			SMSGetMSound()->startSoundActor(MSD_SE_OBJ_SLOT_SPIN, &mPosition, 0,
			                                nullptr, 0, 4);
		unk1E0 = 1 - unk1E0;
	}

	TSlotDrum::calcRootMatrix();
}

void TTelesaSlot::moveObject()
{
	TLiveActor::moveObject();
	for (int i = 0; i < unk148; ++i) {
		if (unk1A8[i] && unk1A4 == getForcastResult(i)) {
			unk198[i] = false;
			unk1A8[i] = false;
		}

		if (unk138[i] != 0.0f) {
			if (fabsf(unk138[i]) > unk160) {
				unk13C[i] += unk138[i];
				if (!unk198[i]) {
					if (unk138[i] > 0.0f)
						unk138[i] -= unk15C;
					else
						unk138[i] += unk15C;
				}
				if (unk13C[i] >= 360.0f)
					unk13C[i] -= 360.0f;
				if (unk13C[i] <= 0.0f)
					unk13C[i] += 360.0f;
			} else {
				unk13C[i] += unk138[i];
				if (unk13C[i] >= 360.0f)
					unk13C[i] -= 360.0f;
				if (unk13C[i] <= 0.0f)
					unk13C[i] += 360.0f;
				if (!unk198[i] && (int)fabsf(unk13C[i]) % unk168 == 0) {
					unk13C[i] = (f32)(unk168 * (int)(unk13C[i] / (f32)unk168));
					unk138[i] = 0.0f;
					SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_SLT_STOP,
					                                &mPosition, 0, nullptr, 0,
					                                4);
					for (int j = 0; j < unk148; ++j) {
						if (unk198[j]) {
							TMsRange<f32> r(0.0f, 1.0f);
							f32 collectRate = mOwner->mParams->mSLSlotHitCollectRate.get();
							if (r.rand() <= collectRate)
								unk1A8[j] = true;
							else
								unk198[j] = false;
						}
					}

					bool allStopped = true;
					if (unk138[0] != 0.0f)
						allStopped = false;
					if (unk138[1] != 0.0f)
						allStopped = false;
					if (unk138[2] != 0.0f)
						allStopped = false;
					if (allStopped)
						mOwner->fanfale();
				}
			}
		}
	}
}

void TTelesaSlot::moveStart()
{
	unk19C = true;
	unk19B = true;
	for (int i = 0; i < 3; ++i) {
		unk198[i] = true;
		unk1A8[i] = false;
		f32 sign  = 1.0f;
		if (i == 0)
			sign = -1.0f;
		if (i == 1)
			sign = -0.8f;
		unk138[i] = sign * unk158;
	}
}

u32 TTelesaSlot::touchWater(THitActor*) { return 0; }

void TTelesaSlot::forceStopSlot(int idx)
{
	TMsRange<f32> range(0.0f, 1.0f);
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	if (!unk19C)
		return;

	f32 rate = mOwner->mParams->mSLSlotFirstHitCollectRate.get();
	if (SMS_GetMarioHP() == 1)
		rate = 0.9f;

	if (range.rand() <= rate) {
		unk1A4 = 2;
		if (SMS_GetMarioHP() <= 3)
			unk1A4 = 0;
		unk1A8[idx] = true;
	} else {
		unk1A4      = getForcastResult(idx);
		unk198[idx] = false;
	}

	if (unk1A4 == mOwner->mSlotResult)
		unk1A4 = 3;

	if (unk1A4 == 0) {
		if (mOwner->unk370 == 0)
			unk1A4 = 1;
		else if (SMS_GetMarioHP() >= 6)
			unk1A4 = 3;
	}

	unk19C = false;
}

bool TTelesaSlot::isRollDrum()
{
	if (unk198[0])
		return true;
	if (unk198[1])
		return true;
	if (unk198[2])
		return true;
	unk19B = false;
	return false;
}

int TTelesaSlot::getSlotResult()
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;

	int result = getDrumResult(0);
	for (int i = 1; i < 3; ++i)
		if (result != getDrumResult(i))
			return -1;

	return result;
}

int TTelesaSlot::getDrumResult(int i) { return getResultFromAng(unk13C[i]); }

int TTelesaSlot::getForcastResult(int idx)
{
	f32 angle = unk13C[idx];
	f32 speed = unk138[idx];
#ifdef SMS_PORT
	// w13: US/PAL give up after 10000 steps and round only when the slot stops; JP
	// loops until it stops and rounds afterwards.
	const bool pal = SMS_BEHAVIOUR_PAL_FAMILY();
	for (int i = 0;;) {
		if (fabsf(speed) > unk160) {
			angle += speed;
			if (speed > 0.0f)
				speed -= unk15C;
			else
				speed += unk15C;
			if (angle >= 360.0f)
				angle -= 360.0f;
			if (angle <= 0.0f)
				angle += 360.0f;
		} else {
			angle += speed;
			if (angle >= 360.0f)
				angle -= 360.0f;
			if (angle <= 0.0f)
				angle += 360.0f;
			if ((int)fabsf(angle) % unk168 == 0) {
				if (pal)
					angle = (int)(angle / unk168) * unk168;
				break;
			}
		}
		if (pal && ++i > 10000)
			break;
	}
	if (!pal)
		angle = (int)(angle / unk168) * unk168;
	return getResultFromAng(angle);
#else
#ifdef VERSION_GMSP01
	int i = 0;
	do {
#else
	for (;;) {
#endif
		if (fabsf(speed) > unk160) {
			angle += speed;
			if (speed > 0.0f)
				speed -= unk15C;
			else
				speed += unk15C;
			if (angle >= 360.0f)
				angle -= 360.0f;
			if (angle <= 0.0f)
				angle += 360.0f;
		} else {
			angle += speed;
			if (angle >= 360.0f)
				angle -= 360.0f;
			if (angle <= 0.0f)
				angle += 360.0f;
			if ((int)fabsf(angle) % unk168 == 0) {
#ifdef VERSION_GMSP01
				angle = (int)(angle / unk168) * unk168;
#endif
				break;
			}
		}
#ifdef VERSION_GMSP01
	} while (++i <= 10000);
#else
	}
	angle = (int)(angle / unk168) * unk168;
#endif
	return getResultFromAng(angle);
#endif // SMS_PORT
}

int TTelesaSlot::getResultFromAng(f32 ang)
{
	if (ang < 44.0f)
		return 0;
	if (ang < 89.0f)
		return 1;
	if (ang < 134.0f)
		return 3;
	if (ang < 179.0f)
		return 2;
	if (ang < 224.0f)
		return 0;
	if (ang < 269.0f)
		return 1;
	if (ang < 314.0f)
		return 3;
	if (ang < 359.0f)
		return 2;
	return 2;
}

// TODO: UNUSED in the map (0x19C bytes) and never inlined into a linked
// function, so the body is fabricated. The only real evidence is the
// xzTable static, which the map places in this region of the TU.
void TTelesaSlot::calcObjCollision()
{
	static const f32 xzTable[4][2] = {
		{ 1.0f, -1.0f },
		{ 1.0f, 1.0f },
		{ -1.0f, 1.0f },
		{ -1.0f, -1.0f },
	};

	for (int i = 0; i < 4; ++i)
		unk1AC[i].set(mPosition.x + xzTable[i][0] * unk140, mPosition.y,
		              mPosition.z + xzTable[i][1] * unk140);
}

void TTelesaSlot::entryObjCollision() { }

TBossTelesa::TBossTelesa(const char* name)
    : TSpineEnemy(name)
    , unk150(true)
    , unk154(0)
    , unk158(0)
    , mParams(nullptr)
    , mCurBckAnm(-1)
    , mPrevBckAnm(-1)
    , mBlendRatio(0.0f)
    , mBody(nullptr)
    , mSlot(nullptr)
    , mSlotFrame(nullptr)
    , unk18C(false)
    , mSlotResult(0)
    , mItemNum(0)
    , mIsSpicy(false)
    , mTelesaManager(nullptr)
    , mMarioHP(0)
    , unk35A(true)
    , unk35B(true)
    , unk35C(0)
    , mCameraOffsetY(0.0f)
    , unk364(0.0f)
    , unk368(0)
    , unk36C(0)
    , unk370(3)
    , mBGMStarted(false)
{
}

void TBossTelesa::init(TLiveManager* manager)
{
	mManager = manager;
	manager->manageActor(this);
	mMActorKeeper = new TMActorKeeper(mManager, 1);
	mMActor       = mMActorKeeper->createMActor("btelesa.bmd", 3);
	onLiveFlag(LIVE_FLAG_DEAD);
	onHitFilter(HIT_FILTER_NO_COLLISION);

	TSpineEnemyParams* params = getSaveParam();
	if (params != nullptr) {
		mBodyRadius       = params->mSLBodyRadius.get();
		mWallRadius       = params->mSLWallRadius.get();
		mHeadHeight       = params->mSLHeadHeight.get();
		mScaledBodyRadius = mBodyScale * mBodyRadius;
		mHitPoints        = getMaxHitPoints();
	}

	initHitActor(0, 5, 0x98000000, mBodyRadius, mHeadHeight, mBodyRadius,
	             mHeadHeight);
	mGroundPlane = TMap::getIllegalCheckData();
	setGoalPath(TPathNode((THitActor*)gpMarioAddress));
	initAnmSound();

	mParams    = (TBossTelesaSaveLoadParams*)getSaveParam();
	mActorType = ACTOR_TYPE_BOSS_UNK13;
	onHitFilter(0xD0000000);
	mSpine->initWith(&TNerveBossTelesaFallDemo::theNerve());
	onLiveFlag(LIVE_FLAG_UNK8);

	mMActor->setLightType(1);
	mMActor->unk40 = true;
	mMActor->initNormalMotionBlend();
	J3DModel* model = mMActor->getModel();
	if (model->getSkinDeform() == nullptr)
		model->setSkinDeform(new J3DSkinDeform, J3D_DEFORM_ATTACH_FLAG_UNK_1);
	mMActor->resetDL();

	unk348.a = 255;
	unk348.b = 255;
	unk348.g = 255;
	unk348.r = 255;
	const u8 normalAlpha = mNormalAlpha;
	unk34C.a = normalAlpha;
	unk34C.b = normalAlpha;
	unk34C.g = normalAlpha;
	unk34C.r = normalAlpha;
	for (int i = 0;
	     i < mMActor->getModel()->getModelData()->getMaterialNum(); ++i) {
		if (i
		    == getModel()->getModelData()->getMaterialName()->getIndex(
		        "_mat_body"))
			SMS_InitPacket_TwoTevKColor(mMActor->getModel(), i, GX_KCOLOR0,
			                            &unk34C, GX_KCOLOR1, &unk348);
		else
			SMS_InitPacket_OneTevKColor(mMActor->getModel(), i, GX_KCOLOR0,
			                            &unk34C);
	}

	reset();

	for (int i = 0; i < gpMapObjManager->getObjNum(); ++i) {
		if (gpMapObjManager->getObj(i)->isActorType(ACTOR_TYPE_CASINORULET)) {
			unk154    = 0;
			unk158    = 0;
			mPosition = gpMapObjManager->getObj(i)->mPosition;
		}
	}

	if (mInstanceIndex == 0) {
		for (u8 i = 0; i < mMActor->getModel()->getModelData()->getJointNum();
		     ++i)
			;
	}

	mBody = new TBossTelesaBody("ボステレサ体コリジョン");
	static_cast<TIdxGroupObj*>(JDrama::TNameRefGen::search("敵グループ"))
	    ->getChildren()
	    .push_back(mBody);
	mBody->initHitActor(ACTOR_TYPE_BOSS_UNK13, 5, 0xD1000000, 350.0f, 550.0f,
	                    300.0f, 500.0f);
	mBody->mOwner = this;
	mBody->offHitFilter(HIT_FILTER_NO_COLLISION);

	mTongue = new TBossTelesaTongue("ボステレサ舌コリジョン");
	static_cast<TIdxGroupObj*>(JDrama::TNameRefGen::search("敵グループ"))
	    ->getChildren()
	    .push_back(mTongue);
	mTongue->initHitActor(ACTOR_TYPE_BOSS_UNK13, 5, 0xC0000000, 180.0f,
	                      350.0f, 180.0f, 350.0f);
	mTongue->mOwner = this;
	mTongue->offHitFilter(HIT_FILTER_NO_COLLISION);

	mKillSmallEnemy
	    = new TBossTelesaKillSmallEnemy("ボステレサ雑魚敵死コリジョン");
	static_cast<TIdxGroupObj*>(JDrama::TNameRefGen::search("敵グループ"))
	    ->getChildren()
	    .push_back(mKillSmallEnemy);
	mKillSmallEnemy->initHitActor(ACTOR_TYPE_TELESA, 5, 0x10000000, 400.0f,
	                              300.0f, 400.0f, 300.0f);
	mKillSmallEnemy->offHitFilter(HIT_FILTER_NO_COLLISION);
	mKillSmallEnemy->mOwner = this;

	mMActor->setLightType(3);
	SMS_ChangeTextureAll(mMActor->getModel()->getModelData(), "H_ma_rak_dummy",
	                     *static_cast<TScreenTexture*>(
	                          JDrama::TNameRefGen::search("スクリーンテクスチャ"))
	                          ->getTexture()
	                          ->getTexInfo());
	mMarioHP = SMS_GetMarioHP();
}

void TBossTelesa::loadAfter()
{
	int n;
	void* resource;
	SDLModelData* modelData;
	TLiveActor* gesso;
	if (gpMapObjManager->getObjNumWithActorType(ACTOR_TYPE_CASINORULET) != 0) {
		int n = 0;
		for (int i = 0; i < gpMapObjManager->getObjNum(); ++i) {
			TMapObjBase* obj = gpMapObjManager->getObj(i);
			if (obj->isActorType(ACTOR_TYPE_CASINORULET)) {
				mRoulette[n] = (TRoulette*)obj;
				++n;
			}
		}
	}

	if (gpMapObjManager->getObjNumWithActorType(ACTOR_TYPE_SROT_RULET) != 0) {
		for (int i = 0; i < gpMapObjManager->getObjNum(); ++i) {
			TMapObjBase* obj = gpMapObjManager->getObj(i);
			if (obj->isActorType(ACTOR_TYPE_SROT_RULET)) {
				mSlot         = (TTelesaSlot*)obj;
				mSlot->mOwner = this;
			}
		}
	}

	n = 0;
	for (int i = 0; i < 6; ++i)
		mFruits[n++] = TMapObjBaseManager::newAndRegisterObj("FruitCoconut");
	for (int i = 0; i < 6; ++i)
		mFruits[n++] = TMapObjBaseManager::newAndRegisterObj("FruitPapaya");
	for (int i = 0; i < 2; ++i)
		mFruits[n++] = TMapObjBaseManager::newAndRegisterObj("FruitPine");
	for (int i = 0; i < 6; ++i)
		mFruits[n++] = TMapObjBaseManager::newAndRegisterObj("FruitDurian");

	for (int i = 0; i < 20; ++i) {
		mFruits[i]->onMapObjFlag(TMapObjBase::MAP_OBJ_FLAG_UNK4000000);
		mFruits[i]->makeObjDead();
	}

	mChilis[0] = (TMapObjBase*)JDrama::TNameRefGen::search("唐辛子 0");
	mChilis[1] = (TMapObjBase*)JDrama::TNameRefGen::search("唐辛子 1");
	mChilis[2] = (TMapObjBase*)JDrama::TNameRefGen::search("唐辛子 2");
	mChilis[3] = (TMapObjBase*)JDrama::TNameRefGen::search("唐辛子 3");
	mChilis[4] = (TMapObjBase*)JDrama::TNameRefGen::search("唐辛子 4");
	mChilis[5] = (TMapObjBase*)JDrama::TNameRefGen::search("唐辛子 5");
	mChilis[6] = (TMapObjBase*)JDrama::TNameRefGen::search("唐辛子 6");
	mChilis[7] = (TMapObjBase*)JDrama::TNameRefGen::search("唐辛子 7");
	mChilis[8] = (TMapObjBase*)JDrama::TNameRefGen::search("唐辛子 8");
	mChilis[9] = (TMapObjBase*)JDrama::TNameRefGen::search("唐辛子 9");
	for (int i = 0; i < 10; ++i)
		mChilis[i]->makeObjDead();

	mCoins[0] = (TMapObjBase*)JDrama::TNameRefGen::search("コイン 0");
	mCoins[1] = (TMapObjBase*)JDrama::TNameRefGen::search("コイン 1");
	mCoins[2] = (TMapObjBase*)JDrama::TNameRefGen::search("コイン 2");
	mCoins[3] = (TMapObjBase*)JDrama::TNameRefGen::search("コイン 3");
	mCoins[4] = (TMapObjBase*)JDrama::TNameRefGen::search("コイン 4");
	mCoins[5] = (TMapObjBase*)JDrama::TNameRefGen::search("コイン 5");
	mCoins[6] = (TMapObjBase*)JDrama::TNameRefGen::search("コイン 6");
	mCoins[7] = (TMapObjBase*)JDrama::TNameRefGen::search("コイン 7");
	mCoins[8] = (TMapObjBase*)JDrama::TNameRefGen::search("コイン 8");
	mCoins[9] = (TMapObjBase*)JDrama::TNameRefGen::search("コイン 9");
	for (int i = 0; i < 10; ++i)
		mCoins[i]->makeObjDead();

	mSlot->unk1E4[0] = 0.0f;
	mSlot->unk1E4[1] = 0.0f;
	mSlot->unk1E4[2] = 0.0f;

	mTelesaManager
	    = (TTelesaManager*)JDrama::TNameRefGen::search("テレサマネージャー");

	for (int i = 0; i < 5; ++i)
		TMapObjBaseManager::newAndRegisterObj("bottle_large");

	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_fhit.jpa", 0xD7);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_fhit_pe.jpa", 0xD8);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_fhit_gr.jpa", 0xD9);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_fhit_or.jpa", 0xDA);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_damage.jpa", 0xDB);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_down.jpa", 0xDC);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_down_pe.jpa", 0xDD);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_down_gr.jpa", 0xDE);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_down_or.jpa", 0xDF);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_spicy_hit.jpa", 0xE0);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_fubuki.jpa", 0xE1);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_yodare1.jpa", 0x19E);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_yodare2.jpa", 0x19F);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_yodare3.jpa", 0x1A0);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_ase.jpa", 0x1A1);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_spicy_a.jpa", 0x1A2);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_spicy_b.jpa", 0x1A3);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_spicy_d.jpa", 0x1A4);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_chika_a.jpa", 0x1A5);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_chika_b.jpa", 0x1A6);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_glow.jpa", 0x1A7);
	SMS_LoadParticle("/scene/btelesa/jpa/ms_btls_spicy_c.jpa", 0x1F0);

	resource = JKRFileLoader::getGlbResource("/scene/btelesa/srot_waku.bmd");
	modelData = new SDLModelData(J3DModelLoaderDataBase::load(resource, 0x10220000));
	mSlotFrame = new TSharedParts(mSlot, 0, modelData, 3, "<TSharedParts>");

	gesso = (TLiveActor*)JDrama::TNameRefGen::search("ゲッソー 0");
	if (gesso != nullptr)
		gesso->onLiveFlag(LIVE_FLAG_DEAD);

	gesso = (TLiveActor*)JDrama::TNameRefGen::search("ゲッソー 1");
	if (gesso != nullptr)
		gesso->onLiveFlag(LIVE_FLAG_DEAD);

	TSpineEnemy::loadAfter();
}

void TBossTelesa::reset()
{
	TSpineEnemy::reset();
	onHitFilter(HIT_FILTER_NO_COLLISION);
	onLiveFlag(LIVE_FLAG_UNK8);
	onLiveFlag(LIVE_FLAG_UNK10);
	onLiveFlag(LIVE_FLAG_HIDDEN);
	unk18C = false;
	TBossTelesaSaveLoadParams* params = mParams;
	setHitParams(params->mSLAttackRadius.get(), params->mSLAttackHeight.get(),
	             params->mSLDamageRadius.get(), params->mSLDamageHeight.get());
	TMarDirector* director = gpMarDirector;
	director->fireStartDemoCamera("btelesa_roll_camera", nullptr, -1, 0.0f,
	                              true, nullptr, 0, nullptr,
	                              JDrama::TFlagT<u16>(0));
}

// fabricated: extra inline levels keep set/sub out of line like the original
// Copy through the Vec base: the copy ctor's *(Vec*)this cast keeps the local
// in memory; a base copy lets MWCC fuse x*x in squared() like the original.
static inline void TelesaCopyVec(Vec& dst, const Vec& src) { dst = src; }

static inline void TelesaSetCamPos0(JGeometry::TVec3<f32>& v)
{
	v.set(*(const Vec*)gpCamera->unk124);
}
static inline void TelesaSetCamPos1(JGeometry::TVec3<f32>& v)
{
	TelesaSetCamPos0(v);
}
static inline void TelesaSetCamPos(JGeometry::TVec3<f32>& v)
{
	TelesaSetCamPos1(v);
}
static inline void TelesaSub0(JGeometry::TVec3<f32>& v,
                              const JGeometry::TVec3<f32>& o)
{
	v.sub(o);
}
static inline void TelesaSub1(JGeometry::TVec3<f32>& v,
                              const JGeometry::TVec3<f32>& o)
{
	TelesaSub0(v, o);
}
static inline void TelesaSub(JGeometry::TVec3<f32>& v,
                             const JGeometry::TVec3<f32>& o)
{
	TelesaSub1(v, o);
}

void TBossTelesa::moveObject()
{
	if (checkLiveFlag(LIVE_FLAG_DEAD))
		return;

	JGeometry::TVec3<f32> camPos;
	TelesaSetCamPos(camPos);
	JGeometry::TVec3<f32> marioToCam = SMS_GetMarioPos();
	TelesaSub(marioToCam, camPos);
	JGeometry::TVec3<f32> diff;
	TelesaCopyVec(diff, marioToCam);
	f32 dist = diff.length();
	if (dist < mCameraMoveLimit) {
		mCameraOffsetY
		    += mCameraMoveSp * (SMS_GetMarioPos().y - gpCamera->unk148.y);
		gpCamera->unk290 = mCameraOffsetY;
	} else if (fabsf(mCameraOffsetY) > 1.0f) {
		mCameraOffsetY   = mCameraOffsetY * mCameraMoveSp;
		gpCamera->unk290 = mCameraOffsetY;
	}

	if (SMS_CheckMarioFlag(0x400))
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_V_LAUGH1, &mPosition,
		                                0, nullptr, 0, 4);

	if (mSpine->getCurrentNerve() == &TNerveBossTelesaFallDemo::theNerve()) {
		if (mRoulette[0]->mPosition.y > 5.0f + mRoulette[1]->mPosition.y) {
			mSlot->mPosition = mRoulette[0]->mPosition;
			mSlot->mPosition.y += 300.0f;
		}
	}

	if (mSpine->getCurrentNerve() != &TNerveBossTelesaPrepareSlot::theNerve()) {
		mMActor->setFrameRate(0.0f, ANM_TYPE_BTP);
		mMActor->getFrameCtrl(ANM_TYPE_BTP)->setFrame(0.0f);
	}

	if (mSpine->getCurrentNerve() == &TNerveBossTelesaDie::theNerve()) {
		u8 alpha = MsClamp<u32>((u8)(mNormalAlpha + (getHitPointMax() - mHitPoints) * 30), 0, 254);
		if ((mMActor->checkCurBckFromIndex(7)
		     && mMActor->getFrameCtrl(ANM_TYPE_BCK)->getFrame() > 50.0f)
		    || mMActor->checkCurBckFromIndex(6)) {
			if (unk34C.a > alpha)
				unk34C.a--;
		} else {
			if (unk34C.a < 255)
				unk34C.a++;
		}
	}

	int rolling = 0;
	JGeometry::TVec3<f32> camPos2;
	TelesaSetCamPos(camPos2);
	JGeometry::TVec3<f32> marioToCam2 = SMS_GetMarioPos();
	TelesaSub(marioToCam2, camPos2);
	JGeometry::TVec3<f32> diff2 = marioToCam2;
	mRouletteSoundPos           = mRoulette[0]->mPosition;
	mRouletteSoundPos.x += 0.67f * diff2.x;
	mRouletteSoundPos.z += 0.67f * diff2.z;
	if (mRoulette[0]->unk13C != 0.0f)
		rolling = 1;
	if (mRoulette[1]->unk13C != 0.0f)
		rolling++;
	if (mRoulette[2]->unk13C != 0.0f)
		rolling++;

	switch (rolling) {
	case 1:
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_RLT_MOVE1,
		                                &mRouletteSoundPos, 0, nullptr, 0, 4);
		break;
	case 2:
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_RLT_MOVE2,
		                                &mRouletteSoundPos, 0, nullptr, 0, 4);
		break;
	case 3:
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_RLT_MOVE3,
		                                &mRouletteSoundPos, 0, nullptr, 0, 4);
		break;
	}

	mPositionDelta.z = 0.0f;
	mPositionDelta.y = 0.0f;
	mPositionDelta.x = 0.0f;
	mRotationDelta.z = 0.0f;
	mRotationDelta.y = 0.0f;
	mRotationDelta.x = 0.0f;
	control();

	for (int i = 0; i < mColCount; ++i)
		if (mCollisions[i]->isActorType(ACTOR_TYPE_MARIO))
			SMS_SendMessageToMario(this, HIT_MESSAGE_ATTACK);

	bind();
	mPosition.x += mPositionDelta.x;
	mPosition.y += mPositionDelta.y;
	mPosition.z += mPositionDelta.z;
	mPosition.y = 300.0f + mGroundHeight;

	f32 ratio = mBlendRatio - 0.05f;
	if (ratio > 1.0f)
		ratio = 1.0f;
	else if (ratio < 0.0f)
		ratio = 0.0f;
	mBlendRatio = ratio;
	mMActor->setMotionBlendRatioForBck(mBlendRatio);

	if (checkLiveFlag(LIVE_FLAG_CLIPPED_OUT)) {
		mBody->mPosition           = mPosition;
		mTongue->mPosition         = mPosition;
		mKillSmallEnemy->mPosition = mPosition;
	} else {
		MtxPtr bodyMtx = mMActor->getModel()->getAnmMtx(1);
		mBody->mPosition.set(bodyMtx[0][3], bodyMtx[1][3] - 200.0f,
		                     bodyMtx[2][3]);
		mKillSmallEnemy->mPosition.set(bodyMtx[0][3], mPosition.y - 350.0f,
		                               bodyMtx[2][3]);
		MtxPtr tongueMtx = mMActor->getModel()->getAnmMtx(7);
		mTongue->mPosition.set(tongueMtx[0][3], tongueMtx[1][3] - 350.0f,
		                       tongueMtx[2][3]);
	}

	mBody->checkHit();
	mTongue->checkHit();
	mKillSmallEnemy->checkHit();
}

void TBossTelesa::kill()
{
	if (mSpine->getCurrentNerve() != &TNerveBossTelesaDie::theNerve())
		mSpine->pushNerve(&TNerveBossTelesaDie::theNerve());
}

MtxPtr TBossTelesa::getTakingMtx()
{
    // FAKEMATCH(frame): 16 bytes; real construct unknown
    volatile u32 pad[4]; (void)pad;

	mTakingMtx.set(mRoulette[0]->mMActor->getModel()->getAnmMtx(1));
	mTakingMtx.mMtx[1][3] = mRoulette[0]->mPosition.y - 120.0f;
	return mTakingMtx;
}

void TBossTelesa::prepareGenerate() { }

// fabricated: extra inline levels keep identity33 out of line
static inline void TelesaIdentity0(TPosition3f& m) { m.identity33(); }
static inline void TelesaIdentity1(TPosition3f& m) { TelesaIdentity0(m); }
static inline void TelesaIdentity(TPosition3f& m) { TelesaIdentity1(m); }

void TBossTelesa::calcRootMatrix()
{
	J3DModel* model = mMActor->getModel();
	model->setBaseScale(mScaling);

	f32 hosei = unk364;
	f32 z     = mPosition.z;
	f32 yOffs = mParams->mSLTransYOffset.get();
	f32 lim   = mBaseHoseiPosY;
	if (hosei > 0.0f)
		hosei = 0.0f;
	else if (hosei < lim)
		hosei = lim;
	f32 x = mPosition.x;
	f32 y = mPosition.y + yOffs + hosei;

	TPosition3f mtx;
	TelesaIdentity(mtx);
	mtx.setTrans(x, y, z);
	Mtx rot;
	MsMtxSetRotRPH(rot, mRotation.x, mRotation.y, mRotation.z);
	MTXConcat(mtx, rot, mtx);
	MTXCopy(mtx, mMActor->getModel()->getBaseTRMtx());

	if (mSlot != nullptr) {
		mSlot->mRotation = mRotation;
		f32 offsetY      = -700.0f;
		MtxPtr jointMtx;
		if (mMActor->checkCurBckFromIndex(4)
		    || mMActor->checkCurBckFromIndex(0)
		    || mMActor->checkCurBckFromIndex(2)
		    || mMActor->checkCurBckFromIndex(5)) {
			jointMtx = mMActor->getModel()->getAnmMtx(1);
			if (!mMActor->checkCurBckFromIndex(0))
				offsetY = -2400.0f;
		} else {
			jointMtx = mMActor->getModel()->getAnmMtx(0);
		}
		mSlot->mPosition.x = jointMtx[0][3];
		mSlot->mPosition.y = unk364 + (jointMtx[1][3] + offsetY);
		mSlot->mPosition.z = jointMtx[2][3];
	}

	for (u16 i = 0; i < mMActor->getModel()->getModelData()->getMaterialNum();
	     ++i) {
		Mtx44 effectMtx;
		SMS_GetLightPerspectiveForEffectMtx(effectMtx);
		mMActor->getModel()
		    ->getModelData()
		    ->getMaterialNodePointer(i)
		    ->getTexGenBlock()
		    ->getTexMtx(1)
		    ->setEffectMtx(effectMtx);
	}

	mEffectPos.set(0.0f, 0.0f, 0.0f);
	gpMarioParticleManager->emit(0x1A5, &mEffectPos, 1, this);
	gpMarioParticleManager->emit(0x1A6, &mEffectPos, 1, this);
	MtxPtr headMtx = mMActor->getModel()->getAnmMtx(1);
	mEffectPos.set(headMtx[0][3], headMtx[1][3], headMtx[2][3]);
	gpMarioParticleManager->emitAndBindToPosPtr(0x1A7, &mEffectPos, 1, this);

	if (!mMActor->checkCurBckFromIndex(4) && !mMActor->checkCurBckFromIndex(6)
	    && !mMActor->checkCurBckFromIndex(12)
	    && !mMActor->checkCurBckFromIndex(13)
	    && !mMActor->checkCurBckFromIndex(3)) {
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x19E, mMActor->getModel()->getAnmMtx(5), 1, this);
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x19F, mMActor->getModel()->getAnmMtx(5), 1, this);
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x1A0, mMActor->getModel()->getAnmMtx(10), 1, this);
	}

	if (mMActor->checkCurBckFromIndex(1)
	    && mMActor->getFrameCtrl(ANM_TYPE_BCK)->getFrame() < 20.0f)
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0xE0, mMActor->getModel()->getAnmMtx(5), 0, nullptr);

	if (mMActor->checkCurBckFromIndex(12)) {
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_FIRE, &mPosition, 0,
		                                nullptr, 0, 4);
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x1A1, mMActor->getModel()->getAnmMtx(1), 1, this);
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x1A2, mMActor->getModel()->getAnmMtx(9), 1, this);
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x1A3, mMActor->getModel()->getAnmMtx(9), 1, this);
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x1A4, mMActor->getModel()->getAnmMtx(9), 1, this);
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x1F0, mMActor->getModel()->getAnmMtx(9), 3, this);
	}

	if (mMActor->checkCurBckFromIndex(14)
	    && mMActor->getFrameCtrl(ANM_TYPE_BCK)->checkPass(40.0f)) {
		if (mSpine->getCurrentNerve()
		    == &TNerveBossTelesaSpitSlotItem::theNerve()) {
			generateSlotItem();
			return;
		}
		genAttacker();
	}
}

#ifdef SMS_PORT
#include <stdlib.h>
extern "C" u32 port_frame_count(void); // port_main.cpp
extern "C" float gPortBossAux[3];       // port_bosstrace.cpp (SMS_PORT_BOSSTRACE debug bots)
extern "C" unsigned gPortBossAuxFrame;
extern "C" int gPortBossWant;
extern "C" float gPortThrowAt[3];
extern "C" float gPortItems[20][3]; // items the THROW bot may fetch (it picks one)
extern "C" int gPortItemCount;
static bool gPortMarioHolding() { return gpMarioOriginal->mHeldObject != nullptr; }
// SMS_PORT_BOSSTRACE (debug, w5): what the PADSCRIPT bots should do against King Boo.
// Slot items out (PrepareSlot): fetch a chili and throw it into his mouth, then, while he
// is spicy, fetch a fruit and throw it at him (want 4, gPortItems = the items to choose
// from, gPortThrowAt = mouth / body). Otherwise stand in front and spray (want 1: sprays
// his tongue when it is out, Appear).
static void portKingBooBot(TBossTelesa* b)
{
	static const bool sOn = getenv("SMS_PORT_BOSSTRACE") != nullptr;
	if (!sOn || gpMarioOriginal == nullptr)
		return;
	const void* n = b->mSpine->getCurrentNerve();
	// Default: the spray bot's spot in front of him (also keeps Mario off the middle:
	// King Boo only comes back while nothing stands in his kill area, HideWait).
	int want = 1;
	static const void* sLastNerve;
	if (n != sLastNerve) {
		int chilis = 0, fruits = 0, coins = 0;
		for (int i = 0; i < 10; ++i) {
			chilis += b->mChilis[i] && !b->mChilis[i]->checkLiveFlag(LIVE_FLAG_DEAD);
			coins += b->mCoins[i] && !b->mCoins[i]->checkLiveFlag(LIVE_FLAG_DEAD);
		}
		for (int i = 0; i < 20; ++i)
			fruits += b->mFruits[i] && !b->mFruits[i]->checkLiveFlag(LIVE_FLAG_DEAD);
		OSReport("[boss] f%u king boo: slot result %d, alive: %d chilis %d fruits %d coins, spicy %d\n",
		         port_frame_count(), b->mSlot ? (int)b->mSlot->getSlotResult() : -1, chilis, fruits, coins,
		         (int)b->mIsSpicy);
		for (int i = 0; i < 10; ++i)
			if (b->mChilis[i] && !b->mChilis[i]->checkLiveFlag(LIVE_FLAG_DEAD))
				OSReport("[boss]   chili at %.0f %.0f %.0f holder %p state %d\n", b->mChilis[i]->mPosition.x,
				         b->mChilis[i]->mPosition.y, b->mChilis[i]->mPosition.z, (void*)b->mChilis[i]->mHolder,
				         b->mChilis[i]->mState);
		sLastNerve = n;
	}
	if (n == &TNerveBossTelesaPrepareSlot::theNerve()) {
		TMapObjBase* const* list = b->mIsSpicy ? b->mFruits : b->mChilis;
		const int count          = b->mIsSpicy ? 20 : 10;
		gPortItemCount           = 0;
		for (int i = 0; i < count && gPortItemCount < 20; ++i) {
			TMapObjBase* o = list[i];
			if (o == nullptr || o->checkLiveFlag(LIVE_FLAG_DEAD) || o->mHolder != nullptr)
				continue;
			gPortItems[gPortItemCount][0] = o->mPosition.x;
			gPortItems[gPortItemCount][1] = o->mPosition.y;
			gPortItems[gPortItemCount][2] = o->mPosition.z;
			++gPortItemCount;
		}
		if (gPortItemCount != 0 || gPortMarioHolding()) {
			want = 4;
			const JGeometry::TVec3<f32>& at = b->mIsSpicy ? b->mBody->mPosition : b->mTongue->mPosition;
			gPortThrowAt[0]                 = at.x;
			gPortThrowAt[1]                 = at.y;
			gPortThrowAt[2]                 = at.z;
		}
	}
	gPortBossAuxFrame = port_frame_count();
	gPortBossWant     = want;
}
#endif

void TBossTelesa::perform(u32 cue, JDrama::TGraphics* graphics)
{
#ifdef SMS_PORT
	if (cue & CUE_MOVE)
		portKingBooBot(this);
#endif
	if ((cue & CUE_ENTRY)
	    && !checkLiveFlag(LIVE_FLAG_DEAD | LIVE_FLAG_CLIPPED_OUT)) {
		if (mSpine->getCurrentNerve() == &TNerveBossTelesaDie::theNerve()
		    && mIsSpicy) {
			mMActor->offMakeDL();
			SMS_AddDamageFogEffect(mMActor->getModel()->getModelData(),
			                       mPosition, graphics);
		}
	}

	offLiveFlag(LIVE_FLAG_CLIPPED_OUT);
	TSpineEnemy::perform(cue, graphics);
	mBody->THitActor::perform(cue, graphics);
	mTongue->THitActor::perform(cue, graphics);
	mKillSmallEnemy->THitActor::perform(cue, graphics);
	if (mSlot != nullptr)
		mSlot->testPerform(cue, graphics);
	if (mSlotFrame != nullptr)
		mSlotFrame->testPerform(cue, graphics);
}

BOOL TBossTelesa::receiveMessage(THitActor*, u32) { return false; }

BOOL TBossTelesa::checkMessage(THitActor* sender, u32 message)
{
	if (message == HIT_MESSAGE_TRAMPLE) {
		if (!sender->isActorType(ACTOR_TYPE_MARIO)) {
			if (mSpine->getCurrentNerve()
			    == &TNerveBossTelesaPrepareSlot::theNerve())
				mSpine->pushNerve(&TNerveBossTelesaSpit::theNerve());
		}
	}

	if (message == HIT_MESSAGE_SPRAYED_BY_WATER) {
		if (mSpine->getCurrentNerve()
		    == &TNerveBossTelesaPrepareSlot::theNerve())
			mSpine->pushNerve(&TNerveBossTelesaFreeze::theNerve());
	}

	return true;
}

void TBossTelesa::checkHitObject(THitActor* obj)
{
	unk380 = -1;
	if (obj->getHitCategory() != HIT_CATEGORY_MAP_OBJECT)
		return;

	if (mSpine->getCurrentNerve() != &TNerveBossTelesaPrepareSlot::theNerve())
		return;

	switch (obj->mActorType) {
	case ACTOR_TYPE_FRUIT_COCONUT:
		unk348.r = 230;
		unk348.g = 100;
		unk348.b = 180;
		unk380   = 0xD8;
		kill();
		break;
	case ACTOR_TYPE_RED_PEPPER:
		break;
	case ACTOR_TYPE_FRUIT_PAPAYA:
	case ACTOR_TYPE_FRUIT_PINE:
		unk348.r = 230;
		unk348.g = 180;
		unk348.b = 0;
		unk380   = 0xDA;
		kill();
		break;
	case ACTOR_TYPE_FRUIT_DURIAN:
		unk348.r = 150;
		unk348.g = 50;
		unk348.b = 230;
		unk380   = 0xD9;
		kill();
		break;
	default:
		return;
	}

	if (!mIsSpicy && obj->getActorType() != ACTOR_TYPE_RED_PEPPER) {
		if (unk35A) {
			unk35A = false;
			gpMarDirector->getConsole()->startAppearBalloon(0xF, true);
		}
		unk35C++;
		if (unk35C > 2)
			gpMarDirector->getConsole()->startAppearBalloon(0x10, true);
	} else {
		unk35C = 0;
	}

	mEffectPos = obj->mPosition;
	gpMarioParticleManager->emit(0xD7, &mEffectPos, 0, nullptr);
	if (unk380 >= 0)
		gpMarioParticleManager->emit(unk380, &mEffectPos, 0, nullptr);

	if (mIsSpicy)
		gpMarioParticleManager->emit(0xDB, &mEffectPos, 0, nullptr);
	else
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_FRUIT_HIT, &mPosition,
		                                0, nullptr, 0, 4);

	((TMapObjBase*)obj)->makeObjDead();
}

void TBossTelesa::setSpicy(TLiveActor* chili)
{
	if (mSpine->getCurrentNerve() != &TNerveBossTelesaSpitSlotItem::theNerve()
	    && !mMActor->checkCurBckFromIndex(1)) {
		mIsSpicy = true;
		unk36C   = 0;
		setBckAnm(1);
		if (unk35B) {
			unk35B = false;
			gpMarDirector->getConsole()->startAppearBalloon(0x11, true);
		}
		chili->kill();
	}
}

void TBossTelesa::damageRecover()
{
	for (int i = 0; i < 20; ++i) {
		TMapObjBase** fruit = &mFruits[i];
		if (!(*fruit)->checkLiveFlag(LIVE_FLAG_DEAD)) {
			if ((*fruit)->mHolder == nullptr)
				SMS_SendMessageToMario(*fruit, HIT_MESSAGE_UNK8);
			(*fruit)->makeObjDead();
			gpMarioParticleManager->emit(0xCD, &(*fruit)->mPosition, 0,
			                             nullptr);
			(*fruit)->mPosition.set(0.0f, 0.0f, 0.0f);
		}
	}

	for (int i = 0; i < 10; ++i) {
		if (!mChilis[i]->checkLiveFlag(LIVE_FLAG_DEAD)) {
			if (mChilis[i]->mHolder == nullptr)
				SMS_SendMessageToMario(mChilis[i], HIT_MESSAGE_UNK8);
			mChilis[i]->makeObjDead();
			gpMarioParticleManager->emit(0xCD, &mChilis[i]->mPosition, 0,
			                             nullptr);
			mChilis[i]->mPosition.set(0.0f, 0.0f, 0.0f);
		}

		if (!mCoins[i]->checkLiveFlag(LIVE_FLAG_DEAD)) {
			mCoins[i]->makeObjDead();
			gpMarioParticleManager->emit(0xCD, &mCoins[i]->mPosition, 0,
			                             nullptr);
			mCoins[i]->mPosition.set(0.0f, 0.0f, 0.0f);
		}
	}

	if (mIsSpicy)
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_ESCAPE, &mPosition, 0,
		                                nullptr, 0, 4);
	else
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_DISAPPEAR, &mPosition,
		                                0, nullptr, 0, 4);

	mSpine->pushAfterCurrent(&TNerveBossTelesaHide::theNerve());
	unk368 = 0;
}

void TBossTelesa::tongueHitWater()
{
	if (mSpine->getCurrentNerve() == &TNerveBossTelesaAppear::theNerve())
		mSpine->pushNerve(&TNerveBossTelesaSlotStart::theNerve());
}

bool TBossTelesa::rouletteFall()
{
    // FAKEMATCH(frame): 32 bytes; real construct unknown
    volatile u32 pad[8]; (void)pad;

	if (mRoulette[0]->mPosition.y > mRoulette[1]->mPosition.y) {
		mRoulette[0]->mPosition.y -= 2.0f;
		mRoulette[0]->mMActor->setBck("rulet00");
		if (mRoulette[0]->mPosition.y > 3.0f + mRoulette[1]->mPosition.y) {
			if (SMS_SendMessageToMario(this, HIT_MESSAGE_TAKE))
				mHeldObject = (TTakeActor*)SMS_GetMarioHitActor();
		} else if (SMS_SendMessageToMario(this, HIT_MESSAGE_UNK8)) {
			SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_RLT_SET,
			                                &mPosition, 0, nullptr, 0, 4);
			mHeldObject = nullptr;
		}
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_V_LAUGH1B, &mPosition,
		                                0, nullptr, 0, 4);
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_RLT_DOWN, &mPosition,
		                                0, nullptr, 0, 4);
		gpMarioOriginal->mGamePad->onNeutralMarioKey();
		return false;
	}

	mRoulette[0]->mPosition.y = mRoulette[1]->mPosition.y;
	return true;
}

bool TBossTelesa::slotFall()
{
	if (mSlot->mPosition.y > mRoulette[0]->mPosition.y - 800.0f) {
		mSlot->mPosition.y -= 5.0f;
		return false;
	}

	mSlot->mPosition.y -= 1.0f;
	if (mSlot->mPosition.y < mRoulette[0]->mPosition.y - 900.0f) {
		int rolling = 0;
		if (mRoulette[0]->unk13C != 0.0f)
			rolling = 1;
		if (mRoulette[1]->unk13C != 0.0f)
			rolling++;
		if (mRoulette[2]->unk13C != 0.0f)
			rolling++;
		if (rolling != 3)
			rouletteStart();
	}

	if (mSlot->mPosition.y < mRoulette[0]->mPosition.y - 1100.0f)
		return true;

	mRoulette[0]->unk150->setHitParams(280.0f, 100.0f, 280.0f, 100.0f);
	mRoulette[1]->unk150->setHitParams(280.0f, 100.0f, 280.0f, 100.0f);
	return false;
}

void TBossTelesa::openWaterPlace() { }

void TBossTelesa::flashItem(int time)
{
	int phase = time % 16;
	for (int i = 0; i < 20; ++i) {
		TMapObjBase* fruit = mFruits[i];
		u32* flag = &fruit->mLiveFlag;
		if (!fruit->checkLiveFlag(LIVE_FLAG_DEAD) && fruit->mHolder == nullptr) {
			if (phase < 8)
				*flag |= LIVE_FLAG_HIDDEN;
			else
				*flag &= ~LIVE_FLAG_HIDDEN;
		}
	}

	for (int i = 0; i < 10; ++i) {
		TMapObjBase* chili = mChilis[i];
		u32* flag = &chili->mLiveFlag;
		if (!chili->checkLiveFlag(LIVE_FLAG_DEAD) && chili->mHolder == nullptr) {
			if (phase < 8)
				*flag |= LIVE_FLAG_HIDDEN;
			else
				*flag &= ~LIVE_FLAG_HIDDEN;
		}

		TMapObjBase* coin = mCoins[i];
		u32* flag2 = &coin->mLiveFlag;
		if (!coin->checkLiveFlag(LIVE_FLAG_DEAD)) {
			if (phase < 8)
				*flag2 |= LIVE_FLAG_HIDDEN;
			else
				*flag2 &= ~LIVE_FLAG_HIDDEN;
		}
	}
}

void TBossTelesa::onAllCollision()
{
	offHitFilter(HIT_FILTER_NO_COLLISION);
	mBody->offHitFilter(HIT_FILTER_NO_COLLISION);
	mTongue->offHitFilter(HIT_FILTER_NO_COLLISION);
}

void TBossTelesa::offAllCollision()
{
	onHitFilter(HIT_FILTER_NO_COLLISION);
	mBody->onHitFilter(HIT_FILTER_NO_COLLISION);
	mTongue->onHitFilter(HIT_FILTER_NO_COLLISION);
}

const char** TBossTelesa::getBasNameTable() const { return btelesa_bastable; }

void TBossTelesa::genAttacker()
{
	if (unk150) {
		TTelesa* telesa = (TTelesa*)gpConductor->makeOneEnemyAppear(
		    mPosition, "テレサマネージャー", 1);
		if (telesa != nullptr)
			telesa->initAttacker(this);
		return;
	}

	int num         = mParams->mSLNumGenBubble.get();
	MtxPtr mouthMtx = mMActor->getModel()->getAnmMtx(5);
	f32 step        = 180.0f / num;
	f32 halfWidth   = step * num * 0.5f;
	for (int i = 0; i < num; ++i) {
		TBubble* bubble = (TBubble*)gpConductor->makeOneEnemyAppear(
		    mPosition, "バブルマネージャー", 1);
		if (bubble == nullptr)
			return;

		JGeometry::TVec3<f32> velocity(0.0f, 0.0f, -50.0f);
		Mtx rot;
		MsMtxSetRotRPH(rot, mRotation.x, step * i + (mRotation.y - halfWidth),
		               mRotation.z);
		MTXMultVec(rot, &velocity, &velocity);
		MsVECNormalize(&velocity, &velocity);
		velocity.y = 2.0f;
		f32 speed  = mParams->mSL1stBubbleSp.get();
		velocity.x *= speed;
		velocity.z *= speed;

		bubble->mPosition.set(mouthMtx[0][3], mouthMtx[1][3] - 50.0f,
		                      mouthMtx[2][3]);
		bubble->mVelocity = velocity;
		bubble->mPosition.y += 10.0f;
		bubble->onLiveFlag(LIVE_FLAG_AIRBORNE);

		TMsRange<f32> range(0.0f, 1.0f);
		if (range.rand() < mItemGenRate)
			bubble->appendItem();
		else if (range.rand() < mEnemyGenRate)
			bubble->appendEnemy();
	}
}

void TBossTelesa::setBckAnm(int index)
{
	mPrevBckAnm = mMActor->getCurAnmIdx(ANM_TYPE_BCK);
	mCurBckAnm  = index;
	mBlendRatio = 1.0f;
	// FAKEMATCH(frame): 16 bytes per inlined call; real construct unknown
	volatile u32 pad[4];
	(void)pad;
	MActor* actor = mMActor;
	mMActor->setBckOldMotionBlendAnmPtr(actor->getCurBckAnmPtr());
	mMActor->setBckFromIndex(index);
	mMActor->setMotionBlendRatioForBck(mBlendRatio);
	setAnmSound(getBas(index));
}

bool TBossTelesa::isInDamage() { return false; }

void TBossTelesa::rouletteStart()
{
	int rolling = 0;
	if (mRoulette[0]->unk13C != 0.0f)
		rolling = 1;
	if (mRoulette[1]->unk13C != 0.0f)
		rolling++;
	if (mRoulette[2]->unk13C != 0.0f)
		rolling++;
	(void)rolling;
	TMsRange<f32> speedRange(0.05f, 0.1f);
	TMsRange<f32> dirRange(-1.0f, 1.0f);
	f32 dir    = dirRange.rand();
	f32 upRate = mRouletteUpRate * (getHitPointMax() - mHitPoints);
	for (int i = 0; i < 3; ++i) {
		f32 sign;
		if (dir > 0.0f) {
			sign = -1.0f;
			if (i == 0 || i == 2)
				sign = 1.0f;
		} else {
			sign = 1.0f;
			if (i == 0 || i == 2)
				sign = -1.0f;
		}
		mRoulette[i]->unk144 = sign * (upRate + speedRange.rand());
		mSlot->unk1E4[i]     = sign * (upRate + speedRange.rand());
	}

	rollRouletteCircle();
	SMSRumbleMgr->start(0x14, 0xF, (f32*)nullptr);
	gpCameraShake->startShake(CAM_SHAKE_MODE_UNK23, 1.0f);
}

void TBossTelesa::slotStart()
{
	unk18C = true;
	mSlot->moveStart();
	mTelesaManager->telesaForceKill();
}

void TBossTelesa::slotStop() { mSlot->forceStopSlot(1); }

bool TBossTelesa::checkSlotResult()
{
	if (!mSlot->isRollDrum() && unk18C) {
		unk18C = false;
		mSpine->pushAfterCurrent(&TNerveBossTelesaSpitSlotItem::theNerve());
		return true;
	}
	return false;
}

// fabricated: by-value clamp keeps the loaded count in its final register
static inline int BTClampMax(int v, int max)
{
	if (v > max)
		v = max;
	return v;
}

void TBossTelesa::generateSlotItem()
{
	static const char* manNameTable[] = {
		"バブルマネージャー",       "ハムクリマネージャー",
		"ヤキグリマネージャー",     "ボム兵マネージャー",
		"ポイハナマネージャー",     "電気ノコノコマネージャー",
		"ポポマネージャー",         "ゲッソーマネージャー",
		"とびプクマネージャー",
	};

	unk368      = 0;
	mItemNum    = 0;
	mSlotResult = mSlot->getSlotResult();

	int itemNum = mParams->mSLSlotItemNum.get();
	MtxPtr mouthMtx = mMActor->getModel()->getAnmMtx(5);
	f32 step = 120.0f / itemNum;
	f32 halfWidth = step * itemNum * 0.5f;

	if (mSlotResult == 2) {
		int fruitNum = BTClampMax(mParams->mSLSlotFruitNum.get(), 20);

		int idx = TMsRange<s32>(0, fruitNum).rand();
		for (int i = 0; i < fruitNum; ++i) {
			if (mFruits[i]->mHolder != nullptr)
				continue;

			JGeometry::TVec3<f32> offset(0.0f, 0.0f, 200.0f);
			Mtx rot;
			MsMtxSetRotRPH(rot, mRotation.x,
			               (160.0f / fruitNum) * idx
			                   + (mRotation.y - halfWidth),
			               mRotation.z);
			int next = idx + i;
			if (fruitNum <= 0) {
				next = 0;
			} else {
				while (next >= fruitNum)
					next -= fruitNum;
				while (next < 0)
					next += fruitNum;
			}
			idx = next;
			MTXMultVec(rot, &offset, &offset);
			JGeometry::TVec3<f32> dir;
			MsVECNormalize(&offset, &dir);

			TMsRange<f32> speed(6.0f, 10.0f);
			if (i == 0 || i == 4) {
				mChilis[i]->makeObjAppeared();
				mChilis[i]->offLiveFlag(LIVE_FLAG_HIDDEN);
				mChilis[i]->setVelocityAndFlag10(dir.x * speed.rand(), -2.0f,
				                                 dir.z * speed.rand());
				if (i == 0)
					mChilis[i]->setVelocityAndFlag10(
					    2.0f * (dir.x * speed.rand()), -2.0f,
					    2.0f * (dir.z * speed.rand()));
				mChilis[i]->mRotation.set(0.0f, 90.0f, 0.0f);
				mItems[mItemNum] = mChilis[i];
			} else {
				mFruits[i]->makeObjAppeared();
				mFruits[i]->offLiveFlag(LIVE_FLAG_HIDDEN);
				mFruits[i]->setVelocityAndFlag10(dir.x * speed.rand(), -2.0f,
				                                 dir.z * speed.rand());
				mItems[mItemNum] = mFruits[i];
			}

			// TODO: indexes with i rather than mItemNum, likely an original
			// bug
			mItems[i]->onHitFilter(HIT_FILTER_NO_COLLISION);
			mItems[i]->mScaling.set(1.5f, 1.5f, 1.5f);
			mItems[mItemNum]->mPosition.set(mouthMtx[0][3] + offset.x,
			                                mouthMtx[1][3] - 50.0f,
			                                mouthMtx[2][3] + offset.z);
			mItemNum++;
		}
		return;
	}

	if (mSlotResult == 0) {
		int coinNum = BTClampMax(mParams->mSLSlotFruitNum.get(), 10);

		f32 coinStep = 120.0f / coinNum;
		f32 coinHalf = coinStep * coinNum * 0.5f;
		if (unk370 != 0)
			unk370--;
		else
			unk370 = 0;

		for (int i = 0; i < coinNum; ++i) {
			Mtx rot;
			f32 speed;
			TMapObjBase* coin;
			if (i >= 10)
				return;

			JGeometry::TVec3<f32> velocity(0.0f, 0.0f, 250.0f);
			MsMtxSetRotRPH(rot, mRotation.x,
			               coinStep * i + (mRotation.y - coinHalf),
			               mRotation.z);
			MTXMultVec(rot, &velocity, &velocity);
			MsVECNormalize(&velocity, &velocity);
			TMsRange<f32> range(0.8f, 3.5f);
			velocity.y = 10.0f;
			speed = mParams->mSL1stBubbleSp.get();
			velocity.x *= speed * range.rand();
			velocity.z *= speed * range.rand();

			coin = gpItemManager->makeObjAppeared(ACTOR_TYPE_COIN);
			coin->mPosition.set(mouthMtx[0][3], mouthMtx[1][3] - 250.0f,
			                    mouthMtx[2][3]);
			coin->setVelocityAndFlag10(velocity.x, velocity.y, velocity.z);
			coin->mRotation.set(0.0f, 0.0f, 0.0f);
			((TItem*)coin)->killByTimer(960);

			mItems[mItemNum] = mCoins[i];
			mItems[mItemNum]->offLiveFlag(LIVE_FLAG_HIDDEN);
			mItemNum++;
		}
		return;
	}

	int count = itemNum;
	int type = 0;
	switch (mSlotResult) {
	case 3:
		break;
	case -1:
		count *= 2;
		type = 0;
		break;
	case 1:
		if (mHitPoints > 2)
			type = 1;
		else
			type = 2;
		break;
	}

	int maxType = 7;
	if (mHitPoints == 1)
		maxType = 8;

	int roll = TMsRange<s32>(1, maxType).rand();
	for (int i = 0; i < count; ++i) {
		if (mSlotResult == 3) {
			if (i % 2 == 0)
				roll++;
			if (roll > maxType)
				roll = 1;
			type = roll;
		}

		TSpineEnemy* enemy = gpConductor->makeOneEnemyAppear(mPosition, manNameTable[type], 2);
		if (enemy == nullptr)
			continue;

		if (type != 0) {
			mItems[mItemNum] = enemy;
			mItemNum++;
		}

		JGeometry::TVec3<f32> velocity(0.0f, 0.0f, 200.0f);
		Mtx rot;
		MsMtxSetRotRPH(rot, mRotation.x, step * i + (mRotation.y - halfWidth),
		               mRotation.z);
		MTXMultVec(rot, &velocity, &velocity);
		MsVECNormalize(&velocity, &velocity);
		velocity.y = 2.0f;
		f32 speed = mParams->mSL1stBubbleSp.get();
		TMsRange<f32> range(0.5f, 1.0f);
		velocity.x *= speed * range.rand();
		velocity.y *= 2.0f + range.rand();
		velocity.z *= speed * range.rand();

		enemy->mPosition.set(mouthMtx[0][3], mouthMtx[1][3] - 250.0f,
		                     mouthMtx[2][3]);
		enemy->mVelocity = velocity;
		enemy->mPosition.y += 10.0f;
		enemy->onLiveFlag(LIVE_FLAG_AIRBORNE);
		MTXCopy(mouthMtx, enemy->mMActor->getModel()->getBaseTRMtx());
		enemy->mMActor->calc();
		((TWalkerEnemy*)enemy)->initAttacker(this);
	}
}

void TBossTelesa::fruitCollisionOn()
{
	for (int i = 0; i < mItemNum; ++i) {
		TLiveActor* item = mItems[i];
		if (!item->checkLiveFlag(LIVE_FLAG_DEAD)) {
			if (!item->isActorType(ACTOR_TYPE_COIN)
			    && !item->isActorType(ACTOR_TYPE_BOTTLE_LARGE))
				item->offHitFilter(HIT_FILTER_NO_COLLISION);
		}
	}
}

void TBossTelesa::checkSlot() { }

bool TBossTelesa::checkAllItemDead()
{
	if (mSlotResult == -1)
		return true;

	for (int i = 0; i < mItemNum; ++i)
		if (!mItems[i]->checkLiveFlag(LIVE_FLAG_DEAD))
			return false;

	return true;
}

// TODO: dont_inline is a workaround, original calls these out of line
#pragma dont_inline on
void TBossTelesa::forceAllItemKill()
{
    // FAKEMATCH(frame): 16 bytes; real construct unknown
    volatile u32 pad[4]; (void)pad;

	for (int i = 0; i < mItemNum; ++i) {
		if (mItems[i]->mHolder != nullptr) {
			SMS_SendMessageToMario(mItems[i], HIT_MESSAGE_UNK8);
			mItems[i]->mHolder = nullptr;
		}
		mItems[i]->mPosition.set(0.0f, 0.0f, 0.0f);
		mItems[i]->onHitFilter(HIT_FILTER_NO_COLLISION);
		if (!mItems[i]->checkLiveFlag(LIVE_FLAG_DEAD)) {
			mItems[i]->kill();
			gpMarioParticleManager->emit(0xCD, &mItems[i]->mPosition, 0,
			                             nullptr);
		}
	}
}

#pragma dont_inline reset

void TBossTelesa::rollRouletteCircle()
{
	for (int i = 0; i < 3; ++i)
		mRoulette[i]->setRollSp(mSlot->unk1E4[i]);
}

bool TBossTelesa::isForceRestart() { return false; }

#pragma dont_inline on
void TBossTelesa::forceHide()
{
	if (mSpine->getCurrentNerve() != &TNerveBossTelesaDie::theNerve()
	    && !mMActor->checkCurBckFromIndex(4)
	    && !mMActor->checkCurBckFromIndex(0)) {
		forceAllItemKill();
		unk368 = 0;
		if (mIsSpicy)
			SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_ESCAPE, &mPosition,
			                                0, nullptr, 0, 4);
		else
			SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_DISAPPEAR,
			                                &mPosition, 0, nullptr, 0, 4);
		mSpine->reset();
		mSpine->setNext(&TNerveBossTelesaHide::theNerve());
	}
}

#pragma dont_inline reset

void TBossTelesa::fanfale()
{
	if (mSlot->getSlotResult() == 2 || mSlot->getSlotResult() == 0) {
		mEffectPos.set(0.0f, 0.0f, 0.0f);
		gpMarioParticleManager->emit(0xE1, &mEffectPos, 0, nullptr);
		if (mSlot->getSlotResult() == 2)
			SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_FANFALE_1,
			                                &mPosition, 0, nullptr, 0, 4);
		else
			SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_FANFALE_2,
			                                &mPosition, 0, nullptr, 0, 4);
	} else {
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_FANFALE_3, &mPosition,
		                                0, nullptr, 0, 4);
	}
}

DEFINE_NERVE(TNerveBossTelesaDie, TLiveActor)
{
	TBossTelesa* self = (TBossTelesa*)spine->getBody();
	if (spine->getTime() == 0) {
		self->unk388 = 0;
		if (self->mIsSpicy)
			self->decHitPoints();
		self->offAllCollision();
		if (self->mHitPoints != 0) {
			if (self->mIsSpicy) {
				self->mMActor->setBrkFromIndex(1);
				self->setBckAnm(2);
				gpCameraShake->startShake(CAM_SHAKE_MODE_UNK1F, 1.0f);
			} else {
				self->setBckAnm(5);
				self->mMActor->setBrkFromIndex(0);
				gpCameraShake->startShake(CAM_SHAKE_MODE_UNK20, 1.0f);
			}
		} else {
			MSBgm::stopBGM(MSD_BGM_MAP_SELECT, 10);
			gpCameraShake->startShake(CAM_SHAKE_MODE_UNK21, 1.0f);
			self->setBckAnm(3);
			self->mMActor->setBrkFromIndex(1);
			self->mSlot->mScaling.set(0.0f, 0.0f, 0.0f);
			SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_DOWN,
			                                &self->mPosition, 0, nullptr, 0, 4);
		}
	}

	if (self->mHitPoints == 0) {
		if (self->checkCurAnmEnd(0)) {
			if (self->unk388 == 0) {
				MtxPtr mtx = self->mMActor->getModel()->getAnmMtx(1);
				self->mEffectPos.set(mtx[0][3], mtx[1][3], mtx[2][3]);
				gpMarioParticleManager->emit(0xDC, &self->mEffectPos, 0,
				                             nullptr);
				if (self->unk380 == 0xD8)
					gpMarioParticleManager->emit(0xDD, &self->mEffectPos, 0,
					                             nullptr);
				else if (self->unk380 == 0xD9)
					gpMarioParticleManager->emit(0xDE, &self->mEffectPos, 0,
					                             nullptr);
				else
					gpMarioParticleManager->emit(0xDF, &self->mEffectPos, 0,
					                             nullptr);
				self->forceAllItemKill();
				self->mRoulette[0]->unk144 = 0.0f;
				self->mSlot->unk1E4[0]     = 0.0f;
				self->mRoulette[1]->unk144 = 0.0f;
				self->mSlot->unk1E4[1]     = 0.0f;
				self->mRoulette[2]->unk144 = 0.0f;
				self->mSlot->unk1E4[2]     = 0.0f;
				self->rollRouletteCircle();
			}

			if (self->unk388 > 240) {
				self->unk388 = 0;
				gpItemManager->makeShineAppearWithDemo(
				    "シャイン（ボス用）", "ボスシャインカメラ",
				    self->mPosition.x, self->mPosition.y, self->mPosition.z);
				self->onLiveFlag(LIVE_FLAG_DEAD);
				self->onLiveFlag(LIVE_FLAG_UNK8);
				self->offLiveFlag(LIVE_FLAG_UNK10000);
				self->mHolder = nullptr;
				self->offAllCollision();
				self->stopAnmSound();
				spine->reset();
				return true;
			}
			self->unk388++;
		}
		return false;
	}

	if (self->checkCurAnmEnd(0)) {
		if (self->mMActor->checkCurBckFromIndex(5)) {
			self->mMActor->setBrkFromIndex(2);
			self->setBckAnm(7);
			return false;
		}

		if (self->mMActor->checkCurBckFromIndex(7)) {
			self->setBckAnm(6);
			return false;
		}

		SMS_ResetDamageFogEffect(self->mMActor->getModel()->getModelData());
		if (self->mMActor->checkCurBckFromIndex(6)) {
			self->onAllCollision();
			self->setBckAnm(15);
			self->mMActor->setBtpFromIndex(2);
			spine->reset();
			spine->setNext(&TNerveBossTelesaPrepareSlot::theNerve());
			spine->pushAfterCurrent(&TNerveBossTelesaPrepareSlot::theNerve());
		} else {
			self->damageRecover();
		}
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveBossTelesaSpit, TLiveActor)
{
	TBossTelesa* self = (TBossTelesa*)spine->getBody();
	if (spine->getTime() == 0 || !self->mMActor->checkCurBckFromIndex(14))
		self->setBckAnm(14);
	else if (self->mMActor->getFrameCtrl(ANM_TYPE_BCK)->checkPass(40.0f))
		self->genAttacker();

	if (self->checkCurAnmEnd(0))
		return true;

	return false;
}

DEFINE_NERVE(TNerveBossTelesaHide, TLiveActor)
{
	TBossTelesa* self = (TBossTelesa*)spine->getBody();
	if (!self->mMActor->checkCurBckFromIndex(4)) {
		self->setBckAnm(4);
		self->mMActor->setBtpFromIndex(2);
	}

	if (self->checkCurAnmEnd(0)) {
		self->offAllCollision();
		SMSRumbleMgr->start(0x14, 0xF, (f32*)nullptr);
		self->rouletteStart();
		spine->pushAfterCurrent(&TNerveBossTelesaHideWait::theNerve());
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveBossTelesaHideWait, TLiveActor)
{
	TBossTelesa* self = (TBossTelesa*)spine->getBody();
	if (spine->getTime() == 0) {
		self->onLiveFlag(LIVE_FLAG_HIDDEN);
		self->mIsSpicy = false;
		self->unk34C.a = MsClamp<u32>((u8)(TBossTelesa::mNormalAlpha + (self->getHitPointMax() - self->mHitPoints) * 30), 0, 254);
		self->mSlot->mScaling.set(0.0f, 0.0f, 0.0f);
		self->mMActor->setBrkFromIndex(2);
		self->getMActor()
		    ->getFrameCtrl(ANM_TYPE_BRK)
		    ->setFrame(self->mMActor->getFrameCtrl(ANM_TYPE_BRK)->getEnd());
	} else {
		// TODO: frame-only (diff slot 4 bytes off, frame 0x20 short); unused; the original copies mPosition and calls TVec3::sub
		// out of line here without the copy into diff
		JGeometry::TVec3<f32> diff = self->mPosition;
		TelesaSub(diff, SMS_GetMarioPos());
		if (spine->getTime() > 400 && !self->mKillSmallEnemy->unk6C) {
			spine->pushAfterCurrent(&TNerveBossTelesaAppear::theNerve());
			self->offLiveFlag(LIVE_FLAG_HIDDEN);
			return true;
		}
	}

	return false;
}

DEFINE_NERVE(TNerveBossTelesaAppear, TLiveActor)
{
	TBossTelesa* self = (TBossTelesa*)spine->getBody();
	if (spine->getTime() == 0 && !self->mMActor->checkCurBckFromIndex(0)) {
		self->setBckAnm(0);
		if (!self->mBGMStarted) {
			self->mBGMStarted = true;
			MSBgm::startBGM(MSD_BGM_MAP_SELECT);
		}
		self->mSlot->mScaling.set(1.0f, 1.0f, 1.0f);
		self->mSlot->randomReset();
		self->onAllCollision();
	} else if (self->checkCurAnmEnd(0)
	           && !self->mMActor->checkCurBckFromIndex(15)) {
		self->setBckAnm(15);
		self->mMActor->setBtpFromIndex(2);
	}

	if (self->mMActor->checkCurBckFromIndex(0)
	    && self->mMActor->getFrameCtrl(ANM_TYPE_BCK)->checkPass(40.0f)) {
		gpCameraShake->startShake(CAM_SHAKE_MODE_UNK22, 1.0f);
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_SLT_LAND,
		                                &self->mPosition, 0, nullptr, 0, 4);
	}

	if (spine->getTime() > 800) {
		if (spine->getTime()
		        % (TBossTelesa::mTelesaGenerateInterval
		           + (self->getHitPointMax() - self->mHitPoints) * 100)
		    == 1)
			self->setBckAnm(14);
	}

	self->unk364 *= 0.9f;
	return false;
}

DEFINE_NERVE(TNerveBossTelesaSlotStart, TLiveActor)
{
	TBossTelesa* self = (TBossTelesa*)spine->getBody();
	if (spine->getTime() == 0)
		self->setBckAnm(11);

	if (self->mMActor->checkCurBckFromIndex(11)) {
		if (self->mMActor->getFrameCtrl(ANM_TYPE_BCK)->checkPass(53.0f))
			self->slotStart();

		if (self->checkCurAnmEnd(0)) {
			self->setBckAnm(15);
			self->mMActor->setBtpFromIndex(2);
			self->slotStop();
		}
	}

	if (self->checkSlotResult())
		return true;

	self->unk364 *= 0.99f;
	return false;
}

DEFINE_NERVE(TNerveBossTelesaSpitSlotItem, TLiveActor)
{
	TBossTelesa* self = (TBossTelesa*)spine->getBody();
	if (!self->mMActor->checkCurBckFromIndex(14)
	    && self->unk364 < TBossTelesa::mBaseHoseiPosY - 300.0f) {
		self->setBckAnm(14);
		return false;
	}

	if (self->checkCurAnmEnd(0) && spine->getTime() > 600) {
		spine->pushAfterCurrent(&TNerveBossTelesaPrepareSlot::theNerve());
		self->unk368 = 0;
		self->fruitCollisionOn();
		return true;
	}

	if (spine->getTime() > 200)
		self->unk364 -= 2.0f;

	return false;
}

DEFINE_NERVE(TNerveBossTelesaPrepareSlot, TLiveActor)
{
	TBossTelesa* self = (TBossTelesa*)spine->getBody();
	if (spine->getTime() == 0) {
		self->setBckAnm(15);
		self->mMActor->setBtpFromIndex(2);
	}

	if (self->mIsSpicy) {
		self->unk36C++;
		if (self->checkCurAnmEnd(0)) {
			if (self->mMActor->checkCurBckFromIndex(1)) {
				self->setBckAnm(12);
				self->mMActor->setBtpFromIndex(1);
			} else if (self->mMActor->checkCurBckFromIndex(12)) {
				if (self->unk36C > self->mParams->mSLSpicyTime.get())
					self->setBckAnm(13);
			} else {
				self->unk36C   = 0;
				self->mIsSpicy = false;
				self->unk34C.a = MsClamp<u32>((u8)(TBossTelesa::mNormalAlpha + (self->getHitPointMax() - self->mHitPoints) * 30), 0, 254);
				self->setBckAnm(15);
				self->mMActor->setBtpFromIndex(2);
			}
		}
	}

	self->unk368++;
	int stopTime = self->mParams->mSLStopSlotTime0.get();
	if (self->mHitPoints == 2)
		stopTime = self->mParams->mSLStopSlotTime1.get();
	if (self->mHitPoints == 1)
		stopTime = self->mParams->mSLStopSlotTime2.get();
	if (self->mSlot->isSlotResult(0))
		stopTime = stopTime * 0.5f;

	if (self->unk368 > stopTime - 120)
		self->flashItem(stopTime - self->unk368);

	if (self->mMActor->checkCurBckFromIndex(15)) {
		if (self->checkAllItemDead() || self->unk368 > stopTime) {
			self->unk368 = 0;
			self->forceAllItemKill();
			if (self->mIsSpicy)
				SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_ESCAPE,
				                                &self->mPosition, 0, nullptr, 0,
				                                4);
			else
				SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_DISAPPEAR,
				                                &self->mPosition, 0, nullptr, 0,
				                                4);
			spine->reset();
			spine->setNext(&TNerveBossTelesaHide::theNerve());
			spine->pushAfterCurrent(&TNerveBossTelesaHide::theNerve());
			return true;
		}
	}

	return false;
}

DEFINE_NERVE(TNerveBossTelesaFreeze, TLiveActor)
{
	TBossTelesa* self = (TBossTelesa*)spine->getBody();
	if (self->mMActor->checkCurBckFromIndex(16)) {
		if (self->checkCurAnmEnd(0)) {
			self->mIsSpicy = false;
			self->unk34C.a = MsClamp<u32>((u8)(TBossTelesa::mNormalAlpha + (self->getHitPointMax() - self->mHitPoints) * 30), 0, 254);
			return true;
		}
	} else {
		self->setBckAnm(16);
		SMSGetMSound()->startSoundActor(MSD_SE_BS_TELESA_THANKYOU,
		                                &self->mPosition, 0, nullptr, 0, 4);
	}

	return false;
}

DEFINE_NERVE(TNerveBossTelesaFallDemo, TLiveActor)
{
	// FAKEMATCH(frame): 32 bytes; real construct unknown
	volatile u32 pad[8];
	(void)pad;
	TBossTelesa* self = (TBossTelesa*)spine->getBody();
	if (spine->getTime() == 0) {
		self->onLiveFlag(LIVE_FLAG_HIDDEN);
		if (SMS_SendMessageToMario(self, HIT_MESSAGE_TAKE))
			self->mHeldObject = (TTakeActor*)SMS_GetMarioHitActor();
		self->mMActor->setFrameRate(0.0f, ANM_TYPE_BCK);
		self->mSlot->mScaling.set(0.0f, 0.0f, 0.0f);
	}

	if (self->rouletteFall()) {
		JGeometry::TVec3<f32> diff = self->mPosition;
		TelesaSub(diff, SMS_GetMarioPos());
		if (self->slotFall()) {
			self->onAllCollision();
			spine->reset();
			spine->setNext(&TNerveBossTelesaHideWait::theNerve());
			spine->pushAfterCurrent(&TNerveBossTelesaHideWait::theNerve());
			return true;
		}
	}

	return false;
}

