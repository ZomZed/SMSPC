#include <Animal/Bird.hpp>
#include <Animal/AnimalBase.hpp>
#include <Enemy/Graph.hpp>
#include <Enemy/WireBinder.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoaderFlags.hpp>
#include <M3DUtil/MActor.hpp>
#include <Strategic/ObjModel.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <MSound/SoundEffects.hpp>
#include <Map/Map.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/PacketUtil.hpp>
#include <MarioUtil/RandomUtil.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjBase.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/Spine.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/Particles.hpp>

// rogue includes needed for matching sinit & bss
#include <M3DUtil/InfectiousStrings.hpp>
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>

static const char* bird_bastable[] = {
	nullptr,
	"/scene/bird/bas/bird_fly.bas",
	"/scene/bird/bas/bird_open.bas",
	nullptr,
	nullptr,
	"/scene/bird/bas/bird_start.bas",
	"/scene/bird/bas/bird_stop.bas",
	nullptr,
	nullptr,
};

namespace {

// Animations randomly picked by TNerveAnimalBirdActionOnGround, 8 means walk
const int cRandomAnims[] = { 7, 4, 0, 2, 8 };

const char* const cMatName = "_mat_body1";

// Body tint, indexed by TAnimalBird::mColorIdx
const GXColorS10 cColorTable[] = {
	{ 0, 100, 255, 0 }, // blue coin
	{ 0, 200, 0, 0 },   // other items
	{ 255, 200, 0, 0 }, // shine
	{ 255, 0, 0, 0 },   // red coin
};

} // namespace

TAnimalBird::TAnimalBird(const char* name)
    : TSpineEnemy(name)
    , mItem(nullptr)
    , mWireBinder(nullptr)
{
}

void TAnimalBird::initParams()
{
	mInitialPos.set(mPosition);
	mInitialPos.y += 90.0f;
	mInitialRot.set(mRotation);
	mHitPoints       = getMaxHitPoints();
	mWaterproofTimer = 0;
	mFloatingTimer   = 0;
	mWalkDir         = 1.0f;
	offLiveFlag(LIVE_FLAG_AIRBORNE);
	mSizeRate = 1.0f - 0.1f * (MsRandF() - 0.5f);

	if (TWireBinder::isOnWire(mPosition)) {
		mWireBinder = new TWireBinder;
		mWireBinder->init(mPosition);
	}
}

// The ROM calls initParams() out of line; two one-statement wrappers push it
// past the auto-inline budget of init().
static inline void birdInitParams2(TAnimalBird* bird) { bird->initParams(); }
static inline void birdInitParams(TAnimalBird* bird) { birdInitParams2(bird); }

void TAnimalBird::init(TLiveManager* manager)
{
	mManager = manager;
	mManager->manageActor(this);
	mMActorKeeper = new TMActorKeeper(mManager, 1);
	mMActor       = mMActorKeeper->createMActor("bird_man.bmd", 0);
	mSpine->initWith(&TNerveAnimalBirdWaitOnGround::theNerve());
	birdInitParams(this);
	initCollision();
	initAnmSound();
}

void TAnimalBird::initTevColor(const GXColorS10* color)
{
	int idx = getModel()->getModelData()->getMaterialName()->getIndex(cMatName);
	SMS_InitPacket_OneTevColor(getModel(), idx, GX_TEVREG1, color);
}

void TAnimalBird::initCollision()
{
	initHitActor(ACTOR_TYPE_ENEMY_UNK32, 0, 0, 50.0f, 50.0f, 70.0f, 80.0f);
	onHitFilter(HIT_FILTER_NO_ATTACK);
	offHitFilter(HIT_FILTER_NO_COLLISION);
	mScaledBodyRadius = 35.0f;
}

void TAnimalBird::load(JSUMemoryInputStream& stream)
{
	// FAKEMATCH(frame): 16 bytes; real construct unknown
	volatile u32 pad[4]; (void)pad;
	TSpineEnemy::load(stream);

	s32 eventId;
	stream >> eventId;

	if (eventId >= 0)
		mItem = TMapObjBaseManager::newAndRegisterObjByEventID(eventId,
		                                                       "鳥用");
	else
		mItem = TMapObjBaseManager::newAndRegisterObjByEventID(100, "");

	switch (mItem->mActorType) {
	default:
		mColorIdx = 1;
		break;
	case ACTOR_TYPE_SHINE:
		mColorIdx = 2;
		break;
	case ACTOR_TYPE_COIN_RED:
		mColorIdx = 3;
		break;
	case ACTOR_TYPE_COIN_BLUE:
		mColorIdx = 0;
		checkNotAppear(eventId);
		break;
	}

	initTevColor(&cColorTable[mColorIdx]);
}

void TAnimalBird::loadAfter()
{
	// FAKEMATCH(frame): 24 bytes; real construct unknown
	volatile u32 pad[6]; (void)pad;
	JDrama::TNameRef::loadAfter();
	MSoundSESystem::MSRandPlay::registerTrans(MSD_SE_OBJ_BIRD_DOL_FLYING1,
	                                          &mPosition);
	MSoundSESystem::MSRandPlay::registerTrans(MSD_SE_OBJ_BIRD_DOL_CHUN,
	                                          &mPosition);
}

BOOL TAnimalBird::receiveMessage(THitActor* sender, u32 message)
{
	if (checkLiveFlag(LIVE_FLAG_DEAD))
		return FALSE;

	if (message == HIT_MESSAGE_SPRAYED_BY_WATER) {
		SMS_EasyEmitParticle(PARTICLE_MS_ENM_WATHIT, &sender->mPosition,
		                     nullptr, JGeometry::TVec3<f32>(1.0f, 1.0f, 1.0f));
		gpMSound->startSoundSet(MSD_SE_EN_COMMON_W_HIT_OK, &sender->mPosition,
		                        0, 0.0f, 0, 0, 4);
		behaveHitWater();
		return TRUE;
	}

	if (message == HIT_MESSAGE_TAKE && mHolder == nullptr) {
		onHitFilter(HIT_FILTER_NO_COLLISION);
		mHolder = (TTakeActor*)sender;
		SMS_EasyEmitParticle(PARTICLE_MS_ENM_WATHIT, &sender->mPosition,
		                     nullptr, JGeometry::TVec3<f32>(1.0f, 1.0f, 1.0f));
		return TRUE;
	}

	if ((message == HIT_MESSAGE_PUT || message == HIT_MESSAGE_THROWN)
	    && mHolder == sender) {
		mHolder = nullptr;
		offHitFilter(HIT_FILTER_NO_COLLISION);
		return TRUE;
	}

	if (message == HIT_MESSAGE_UNKB) {
		mHolder = nullptr;
		if (mSpine->getLatestNerve()
		    != &TNerveAnimalBirdChangeToCoin::theNerve()) {
			mSpine->reset();
			mSpine->setNext(&TNerveAnimalBirdChangeToCoin::theNerve());
		} else {
			kill();
		}
		return TRUE;
	}

	if (message == HIT_MESSAGE_TRAMPLE
	    && sender->isActorType(ACTOR_TYPE_POPO)) {
		if (mSpine->getLatestNerve()
		    != &TNerveAnimalBirdChangeToCoin::theNerve()) {
			mSpine->reset();
			mSpine->setNext(&TNerveAnimalBirdChangeToCoin::theNerve());
		} else {
			receiveMessage(this, HIT_MESSAGE_SPRAYED_BY_WATER);
		}
		return TRUE;
	}

	return TSpineEnemy::receiveMessage(sender, message);
}

void TAnimalBird::calcRootMatrix()
{
	if (mHolder != nullptr)
		getModel()->setBaseTRMtx(mHolder->getTakingMtx());
	else
		TSpineEnemy::calcRootMatrix();

	getModel()->getBaseTRMtx()[1][3] += 35.0f;
}

void TAnimalBird::moveObject()
{
	if (mWaterproofTimer > 0)
		--mWaterproofTimer;

	checkFalling();
	checkChangeToItem();
	updateSound();
	TLiveActor::moveObject();
}

void TAnimalBird::bind()
{
	// FAKEMATCH(frame): 8 bytes; real construct unknown
	volatile u32 pad[2]; (void)pad;
	if (!isCheckWithWireBinder())
		TLiveActor::bind();
	else
		mWireBinder->bind(this);
}

const char** TAnimalBird::getBasNameTable() const { return bird_bastable; }

void TAnimalBird::behaveHitWater()
{
	if (mWaterproofTimer <= 0) {
		mWaterproofTimer = getBirdParams()->mWaterproofTimerMax.get();
		if (checkLiveFlag(LIVE_FLAG_AIRBORNE))
			decHitPoints();
	}
}

bool TAnimalBird::isOnGroundNerve() const
{
	const TSpineBase<TLiveActor>* spine = mSpine;
	return spine->getLatestNerve() == &TNerveAnimalBirdWaitOnGround::theNerve()
	       || spine->getLatestNerve()
	              == &TNerveAnimalBirdActionOnGround::theNerve()
	       || spine->getLatestNerve()
	              == &TNerveAnimalBirdWalkOnGround::theNerve();
}

void TAnimalBird::checkFalling()
{
	if (isOnGroundNerve()) {
		if (checkLiveFlag(LIVE_FLAG_AIRBORNE)) {
			if (getBirdParams()->mFloatingTimerMax.get() < ++mFloatingTimer) {
				mSpine->reset();
				mSpine->setNext(&TNerveAnimalBirdTakeoff::theNerve());
			}
		} else {
			mFloatingTimer = 0;
		}
	}
}

void TAnimalBird::checkChangeToItem()
{
	// TODO: the target materialises isChanged() as a bool (cntlzw) here
	if (!isChanged() && mHitPoints == 0) {
		mSpine->reset();
		mSpine->setNext(&TNerveAnimalBirdChangeToCoin::theNerve());
	}
}

void TAnimalBird::checkNotAppear(long event_id)
{
	u8 map = gpMarDirector->mMap;
	if (TFlagManager::getInstance()->getBlueCoinFlag(map, event_id))
		onLiveFlag(LIVE_FLAG_DEAD);
}

void TAnimalBird::updateSound()
{
	if (isFlying())
		gpMSound->startSeRandPlay(MSD_SE_OBJ_BIRD_DOL_FLYING1, mInstanceIndex);

	if (isOnGroundNerve())
		gpMSound->startSeRandPlay(MSD_SE_OBJ_BIRD_DOL_CHUN, mInstanceIndex);
}

bool TAnimalBird::isWantToFly() const
{
	bool wantToFly = false;
	bool alarmed   = mWaterproofTimer > 0 || isFindMario();
	if (alarmed && MsRandF() < 0.5f)
		wantToFly = true;
	return wantToFly;
}

bool TAnimalBird::isWantToAction() const
{
	s32 time = mSpine->getTime() - getBirdParams()->mActionTimer.get();
	if (time < 0)
		return false;

	f32 rate = (f32)time / getBirdParams()->mActionTimerAdd.get();
	return MsRandF() < rate;
}

// UNUSED (Size: 0x58 in MAP)
bool TAnimalBird::isWantToRest() const { return false; }

bool TAnimalBird::isFindMario() const
{
	f32 diffY = fabsf(gpMarioPos->y - mPosition.y);
	if (getBirdParams()->mSearchHeight.get() < diffY)
		return false;

	return isInSight(*gpMarioPos,
	                 mSizeRate * getBirdParams()->mSearchLength.get(),
	                 mSizeRate * getBirdParams()->mSearchAngle.get(),
	                 mSizeRate * getBirdParams()->mSearchAware.get())
	       != FALSE;
}

bool TAnimalBird::isChangeToItem() const
{
	// TODO: UNUSED (Size: 0x98 in MAP), guessed from checkChangeToItem()
	return !isChanged() && mHitPoints == 0;
}

// UNUSED (Size: 0x8 in MAP)
bool TAnimalBird::isGroundShaken() const { return false; }

bool TAnimalBird::isCheckWithWireBinder() const
{
	return mWireBinder != nullptr
	       && (isOnGroundNerve()
	           || mSpine->getLatestNerve()
	                  == &TNerveAnimalBirdPreLanding::theNerve());
}

bool TAnimalBird::isChanged() const
{
	return mSpine->isNerve(&TNerveAnimalBirdChangeToCoin::theNerve());
}

bool TAnimalBird::isFlying() const
{
	const TNerveBase<TLiveActor>* nerve = mSpine->getLatestNerve();
	return nerve == &TNerveAnimalBirdGraphWander::theNerve()
	       || nerve == &TNerveAnimalBirdComeback::theNerve();
}

void TAnimalBird::doDropCoin()
{
	onLiveFlag(LIVE_FLAG_DEAD);

	TMapObjBase* item = mItem;
	if (item->isActorType(ACTOR_TYPE_SHINE)) {
		item->JSGSetTranslation(mPosition);
		((TShine*)mItem)->appearWithDemo("鳥シャインカメラ");
	} else {
		TMapObjBase* obj = item->isActorType(ACTOR_TYPE_COIN)
		                       ? gpItemManager->makeObjAppear(ACTOR_TYPE_COIN)
		                       : item;

		if (obj != nullptr) {
			obj->appear();
			obj->JSGSetTranslation(mPosition);
			obj->setVelocityAndFlag10(0.0f, -10.0f, 0.0f);
			obj->onLiveFlag(LIVE_FLAG_AIRBORNE);
		}
	}
}

void TAnimalBird::doFlyToCurPathNode()
{
	JGeometry::TVec3<f32> diff = getUnkF4().getPoint();
	diff -= mPosition;

	f32 dist = diff.length();
	if (dist < 100.0f)
		return;

	f32 marchSpeed = getMyMarchSpeed();
	f32 turnSpeed  = getBirdParams()->mTurnSpeed.get();
	turnSpeed      = turnSpeed * SMSGetAnmFrameRate();

	if (dist <= 2.0f * calcMinimumTurnRadius(marchSpeed, turnSpeed))
		turnSpeed = calcTurnSpeedToReach(marchSpeed, 0.5f * dist);

	TAnimalBase::getRotationFlyToDir(&mRotation, diff, marchSpeed, turnSpeed);

	// TODO: the target inlines TQuat4::rotate() here (and in doWalk() and
	// doLanding()) with a single TVec4 temporary; the current JSystem header's
	// rotate() has two and is not inlined. That also accounts for the missing
	// local TVec3<f32>::set<f32> copy.
	JGeometry::TQuat4<f32> quat = SMS_Eular2Quat(mRotation);
	JGeometry::TVec3<f32> delta(0.0f, 0.0f, marchSpeed);
	quat.rotate(delta);

	delta.scale(1.0f - getWaterDamageRate());
	delta.y -= getWaterPowerY();
	mPositionDelta = delta;
}

// One extra inline level: doWalk() is auto-inlined into the walk nerve, where
// the ROM keeps TVec4::TVec4() and TVec3::set<f> out of line.
static inline void birdRotate(const JGeometry::TQuat4<f32>& quat,
                              JGeometry::TVec3<f32>& v)
{
	quat.rotate(v, v);
}

void TAnimalBird::doWalk()
{
	mGravity = 0.15f;

	f32 torque  = getBirdParams()->mWalkingTorqueY.get();
	torque      = torque * SMSGetAnmFrameRate();
	mRotation.y = MsWrap(mRotation.y + mWalkDir * torque, 0.0f, 360.0f);

	JGeometry::TQuat4<f32> quat = SMS_Eular2Quat(mRotation);
	JGeometry::TVec3<f32> delta(0.0f, 0.0f,
	                            getBirdParams()->mWalkingSpeed.get());
	birdRotate(quat, delta);
	mPositionDelta = delta;
}

// By-value copy + inline levels: the ROM copies mVelocity, inlines dot() and
// calls TUtil<f32>::sqrt out of line.
static inline f32 birdSqrt(f32 m) { return JGeometry::TUtil<f32>::sqrt(m); }
static inline f32 birdVecLength(JGeometry::TVec3<f32> v)
{
	return birdSqrt(v.dot(v));
}

// The ROM keeps the unused getPoint() result below the caller temporaries.
static inline void birdGetWirePoint(TWireBinder* binder,
                                    const JGeometry::TVec3<f32>& pos)
{
	// NOTE: the result is never used in the original either
	JGeometry::TVec3<f32> point;
	binder->getPoint(&point, pos);
}

bool TAnimalBird::doLanding(bool first)
{
	if (first) {
		f32 marchSpeed = getMyMarchSpeed();

		JGeometry::TQuat4<f32> quat = SMS_Eular2Quat(mRotation);
		JGeometry::TVec3<f32> vel(0.0f, 0.0f, marchSpeed);
		quat.rotate(vel);
		mVelocity = JGeometry::TVec3<f32>(vel.x, 0.0f, vel.z);
	}

	bool onGround = false;
	JGeometry::TVec3<f32> delta(0.0f, 0.0f, 0.0f);

	if (mWireBinder != nullptr) {
		birdGetWirePoint(mWireBinder, mInitialPos);
	} else {
		gpMap->checkGround(mPosition, &mGroundPlane);
	}

	if (checkLiveFlag(LIVE_FLAG_AIRBORNE))
		delta.y = -getBirdParams()->mLandingGravityY.get();
	else
		onGround = true;

	mRotation.x = mInitialRot.x;
	mRotation.z = mInitialRot.z;

	f32 torque = getBirdParams()->mLandingTorqueY.get();
	torque     = torque * SMSGetAnmFrameRate();
	f32 turn   = JGeometry::TUtil<f32>::clamp(
        MsAngleDiff(mInitialRot.y, mRotation.y), -torque, torque);
	mRotation.y = MsWrap(mRotation.y + turn, 0.0f, 360.0f);

	mPositionDelta = delta;

	JGeometry::TVec3<f32> newVel(0.0f, 0.0f, birdVecLength(mVelocity));
	newVel.scale(getBirdParams()->mLandingFric.get());

	SMS_Eular2Quat(mRotation).rotate(newVel, newVel);
	mVelocity = newVel;

	return onGround && fabsf(turn) < 0.01f;
}

void TAnimalBird::doGotoRandomNextGraphNode()
{
	goToRandomNextGraphNode();

	JGeometry::TVec3<f32> point = getUnk104().getPoint();
	point.x += 200.0f * (MsRandF() - 0.5f);
	point.y += 200.0f * (MsRandF() - 0.5f);
	point.z += 200.0f * (MsRandF() - 0.5f);
	setGoalPath(TPathNode(point));

	if (mPosition.y <= getUnkF4().getPoint().y)
		setBckAnm(1);
	else
		setBckAnm(3);
}

void TAnimalBird::setGoalToComeback() { setGoalPath(TPathNode(mInitialPos)); }

void TAnimalBird::setParamsOnFloating()
{
	onLiveFlag(LIVE_FLAG_AIRBORNE);
	mGravity       = 0.0f;
	mFloatingTimer = 0;
}

// UNUSED (Size: 0x4 in MAP)
void TAnimalBird::setParamsOnLanding() { }

void TAnimalBird::setBckAnm(int idx)
{
	mMActor->setBckFromIndex(idx);
	setCurAnmSound();
}

f32 TAnimalBird::getMyMarchSpeed() const
{
	f32 speed = mSizeRate * getBirdParams()->mMarchSpeed.get();
	return speed * SMSGetAnmFrameRate();
}

f32 TAnimalBird::getWaterDamageRate() const
{
	return (f32)mWaterproofTimer / getBirdParams()->mWaterproofTimerMax.get();
}

f32 TAnimalBird::getWaterPowerY() const
{
	f32 rate = getWaterDamageRate();
	return getBirdParams()->mWaterPowerY.get() * rate;
}

// UNUSED (Size: 0x50 in MAP)
f32 TAnimalBird::getFootGroundHeight() { return 0.0f; }

TAnimalBirdParams::TAnimalBirdParams(const char* path)
    : TSpineEnemyParams(path)
    , PARAM_INIT(mMarchSpeed, 5.0f)
    , PARAM_INIT(mTurnSpeed, 0.1f)
    , PARAM_INIT(mReturnTimer, 1800)
    , PARAM_INIT(mSearchLength, 800.0f)
    , PARAM_INIT(mSearchHeight, 600.0f)
    , PARAM_INIT(mSearchAware, 400.0f)
    , PARAM_INIT(mSearchAngle, 90.0f)
    , PARAM_INIT(mActionTimer, 100)
    , PARAM_INIT(mWaterproofTimerMax, 45)
    , PARAM_INIT(mFloatingTimerMax, 30)
    , PARAM_INIT(mLandingGravityY, 1.0f)
    , PARAM_INIT(mLandingTorqueY, 2.0f)
    , PARAM_INIT(mWalkingTorqueY, 0.5f)
    , PARAM_INIT(mWalkingSpeed, 2.0f)
    , PARAM_INIT(mWalkTimer, 100)
    , PARAM_INIT(mLandingFric, 0.95f)
    , PARAM_INIT(mActionTimerAdd, 300)
    , PARAM_INIT(mWaterPowerY, 15.0f)
{
	load(mPrmPath);
}

TAnimalBirdManager::TAnimalBirdManager(const char* name)
    : TEnemyManager(name)
{
}

void TAnimalBirdManager::load(JSUMemoryInputStream& stream)
{
	unk38 = new TAnimalBirdParams("/Animal/bird.prm");
	TEnemyManager::load(stream);
}

void TAnimalBirdManager::loadAfter()
{
	// FAKEMATCH(frame): 32 bytes; real construct unknown
	volatile u32 pad[8]; (void)pad;
	TEnemyManager::loadAfter();
	MSoundSESystem::MSRandPlay::createRandPlayVec(MSD_SE_OBJ_BIRD_DOL_FLYING1,
	                                              mObjNum);
	MSoundSESystem::MSRandPlay::createRandPlayVec(MSD_SE_OBJ_BIRD_DOL_CHUN,
	                                              mObjNum);
}

void TAnimalBirdManager::createModelData()
{
	static const TModelDataLoadEntry entry[] = {
		{ "bird_man.bmd",
		  J3DMLF_MaterialPEFull | J3DMLF_UseUniqueMaterials
		      | (1 << J3DMLF_TevStageNumShift),
		  0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

DEFINE_NERVE(TNerveAnimalBirdWaitOnGround, TLiveActor)
{
	TAnimalBird* bird = (TAnimalBird*)spine->getBody();

	if (spine->getTime() == 0)
		bird->setBckAnm(7);

	if (bird->isWantToFly()) {
		spine->pushAfterCurrent(&TNerveAnimalBirdTakeoff::theNerve());
		return TRUE;
	}

	if (bird->checkCurAnmEnd(0)) {
		if (bird->isWantToAction()) {
			spine->pushAfterCurrent(
			    &TNerveAnimalBirdActionOnGround::theNerve());
			return TRUE;
		}
	}

	return FALSE;
}

DEFINE_NERVE(TNerveAnimalBirdActionOnGround, TLiveActor)
{
	TAnimalBird* bird = (TAnimalBird*)spine->getBody();

	if (spine->getTime() == 0) {
		int anm = cRandomAnims[(int)(5.0f * MsRandF())];
		if (anm == 8) {
			spine->pushAfterCurrent(&TNerveAnimalBirdWalkOnGround::theNerve());
			return TRUE;
		}

		bird->getMActor()->setBck(anm);
	}

	if (bird->isWantToFly()) {
		spine->pushAfterCurrent(&TNerveAnimalBirdTakeoff::theNerve());
		return TRUE;
	}

	if (bird->checkCurAnmEnd(0)) {
		spine->pushAfterCurrent(&TNerveAnimalBirdWaitOnGround::theNerve());
		return TRUE;
	}

	return FALSE;
}

DEFINE_NERVE(TNerveAnimalBirdWalkOnGround, TLiveActor)
{
	TAnimalBird* bird = (TAnimalBird*)spine->getBody();

	if (spine->getTime() == 0) {
		bird->mWalkDir *= -1.0f;
		bird->setBckAnm(8);
	}

	if (bird->isWantToFly()) {
		spine->pushAfterCurrent(&TNerveAnimalBirdTakeoff::theNerve());
		return TRUE;
	}

	bird->doWalk();

	if (bird->getBirdParams()->mWalkTimer.get() < spine->getTime()) {
		spine->pushAfterCurrent(&TNerveAnimalBirdWaitOnGround::theNerve());
		return TRUE;
	}

	return FALSE;
}

DEFINE_NERVE(TNerveAnimalBirdTakeoff, TLiveActor)
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	TAnimalBird* bird = (TAnimalBird*)spine->getBody();

	if (spine->getTime() == 0) {
		bird->setBckAnm(5);
		bird->setParamsOnFloating();
		J3DFrameCtrl* ctrl = bird->getMActor()->getFrameCtrl(ANM_TYPE_BCK);
		ctrl->setRate(3.0f * ctrl->getRate());
		gpMSound->startSoundActor(MSD_SE_OBJ_BIRD_DOL_TO_FLY1,
		                          &bird->mPosition, 0, nullptr, 0, 4);
	}

	if (bird->checkCurAnmEnd(0)) {
		spine->pushAfterCurrent(&TNerveAnimalBirdGraphWander::theNerve());
		bird->setParamsOnFloating();
		return TRUE;
	}

	return FALSE;
}

DEFINE_NERVE(TNerveAnimalBirdGraphWander, TLiveActor)
{
	TAnimalBird* bird = (TAnimalBird*)spine->getBody();

	if (spine->getTime() == 0) {
		bird->mVelocity = JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f);
		bird->getTracer()->reset();
		bird->goToShortestNextGraphNode();
	}

	if (spine->getTime() == 0 || bird->isReachedToGoal())
		bird->doGotoRandomNextGraphNode();

	// TODO: result is discarded in the original too
	bird->checkCurAnmEnd(0);

	if (bird->getBirdParams()->mReturnTimer.get() < bird->mSpine->getTime()) {
		spine->pushAfterCurrent(&TNerveAnimalBirdComeback::theNerve());
		return TRUE;
	}

	bird->doFlyToCurPathNode();
	return FALSE;
}

DEFINE_NERVE(TNerveAnimalBirdChangeToCoin, TLiveActor)
{
	TAnimalBird* bird = (TAnimalBird*)spine->getBody();

	if (spine->getTime() == 0)
		bird->doDropCoin();

	return TRUE;
}

DEFINE_NERVE(TNerveAnimalBirdComeback, TLiveActor)
{
	TAnimalBird* bird = (TAnimalBird*)spine->getBody();

	if (spine->getTime() == 0) {
		bird->setGoalToComeback();
		bird->setBckAnm(3);
	}

	bird->doFlyToCurPathNode();

	if (bird->isFindMario()) {
		spine->pushAfterCurrent(&TNerveAnimalBirdGraphWander::theNerve());
		return TRUE;
	}

	if (bird->isReachedToGoal()) {
		spine->pushAfterCurrent(&TNerveAnimalBirdPreLanding::theNerve());
		return TRUE;
	}

	return FALSE;
}

DEFINE_NERVE(TNerveAnimalBirdPreLanding, TLiveActor)
{
	TAnimalBird* bird = (TAnimalBird*)spine->getBody();

	if (spine->getTime() == 0) {
		bird->setBckAnm(1);
		J3DFrameCtrl* ctrl = bird->getMActor()->getFrameCtrl(ANM_TYPE_BCK);
		ctrl->setRate(1.5f * ctrl->getRate());
		bird->doLanding(true);
	}

	if (bird->isFindMario()) {
		spine->pushAfterCurrent(&TNerveAnimalBirdGraphWander::theNerve());
		return TRUE;
	}

	if (bird->doLanding(false)) {
		spine->pushAfterCurrent(&TNerveAnimalBirdLanding::theNerve());
		return TRUE;
	}

	return FALSE;
}

DEFINE_NERVE(TNerveAnimalBirdLanding, TLiveActor)
{
	TAnimalBird* bird  = (TAnimalBird*)spine->getBody();
	J3DFrameCtrl* ctrl = bird->getMActor()->getFrameCtrl(ANM_TYPE_BCK);

	if (spine->getTime() == 0) {
		bird->mVelocity = JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f);
		bird->setBckAnm(5);
		ctrl->setAttribute(J3DFrameCtrl::ATTR_ONCE_AND_RESET);
		ctrl->setFrame(ctrl->getEnd());
		ctrl->setRate(-1.0f * ctrl->getRate());
	}

	if (bird->isFindMario() == true) {
		spine->pushAfterCurrent(&TNerveAnimalBirdGraphWander::theNerve());
		return TRUE;
	}

	if (ctrl->checkState(J3DFrameCtrl::STATE_COMPLETED_ONCE)) {
		spine->pushAfterCurrent(&TNerveAnimalBirdWaitOnGround::theNerve());
		return TRUE;
	}

	return FALSE;
}
