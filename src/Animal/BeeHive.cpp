#include <Animal/BeeHive.hpp>
#include <Animal/boid.hpp>
#include <Enemy/Conductor.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoaderFlags.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JGeometry.hpp>
#include <JSystem/JMath.hpp>
#include <M3DUtil/MActor.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjBase.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/ObjManager.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/Spine.hpp>
#include <Strategic/Strategy.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/Particles.hpp>
#include <dolphin/mtx.h>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

namespace {

// Initialized dynamically (in __sinit) in the original, value is 3*PI/8
const f32 cAngleLimit = JGeometry::TUtil<f32>::PI() * 0.375f;

bool SMS_IsMarioInWater()
{
	bool inWater = SMS_CheckMarioFlag(MARIO_FLAG_VISIBLE)
	               || SMS_CheckMarioFlag(MARIO_FLAG_IN_SHALLOW_WATER)
	               || SMS_GetMarioGroundPlane()->isWaterSurface()
	               || SMS_CheckMarioFlag(MARIO_FLAG_IN_WATER);
	return inWater;
}

void SMS_EmitWaterHitParticleAndSound(JGeometry::TVec3<f32>* pos)
{
	TPosition3f mtx;
	mtx.translation(pos->x, pos->y, pos->z);
	gpMarioParticleManager->emitAndBindToMtx(PARTICLE_MS_ENM_WATHIT, mtx, 0,
	                                         nullptr);
	gpMSound->startSoundSet(0x6802, pos, 0, 0.0f, 0, 0, 4);
}

// TODO: almost certainly JGeometry::TPosition3<T>::setSQT (as in TP/SMG), which
// is missing from our JGeometry headers. Keeping it here so as to not touch
// JSystem. The extra inline level is what keeps setSQ out of line.
inline void setSQT(TPosition3f& mtx, const JGeometry::TVec3<f32>& scale,
                   const JGeometry::TQuat4<f32>& quat,
                   const JGeometry::TVec3<f32>& trans)
{
	mtx.setSQ(scale, quat);
	mtx.setTrans(trans);
}

} // namespace

TBeeHiveParams::TBeeHiveParams(const char* path)
    : TSpineEnemyParams(path)
    , PARAM_INIT(mGiveupTimer, 600)
    , PARAM_INIT(mGiveupRange, 1750.0f)
    , PARAM_INIT(mDecrimentTimer, 60)
    , PARAM_INIT(mRebound, 0.007f)
    , PARAM_INIT(mDecay, 0.98f)
    , PARAM_INIT(mAngleMaxAdd, 0.08f)
    , PARAM_INIT(mShakePower, 0.003f)
    , PARAM_INIT(mFallAngularVel, 0.01f)
    , PARAM_INIT(mSearchRange, 800.0f)
{
	TParams::load(mPrmPath);
}

TBee::TBee(MActor* actor, TBeeHive* hive)
    : TRealoidActor(actor)
{
	unkA8 = hive;
}

void TBee::init()
{
	initHitActor(ACTOR_TYPE_ENEMY_UNK2F, 1, 0x80000000, 20.0f, 20.0f, 50.0f,
	             50.0f);

	TIdxGroupObj* group
	    = static_cast<TIdxGroupObj*>(JDrama::TNameRefGen::search("敵グループ"));
	group->getChildren().push_back(this);

	onHitFilter(HIT_FILTER_NO_COLLISION);
}

BOOL TBee::receiveMessage(THitActor* sender, u32 message)
{
	switch (message) {
	case HIT_MESSAGE_TAKE:
		if (mHolder == nullptr) {
			mHolder = (TTakeActor*)sender;
			SMS_EasyEmitParticle(PARTICLE_MS_ENM_WATHIT, &mPosition, nullptr,
			                     JGeometry::TVec3<f32>(1.0f, 1.0f, 1.0f));
			return TRUE;
		}
		break;
	case HIT_MESSAGE_UNK8:
		if (mHolder != nullptr) {
			mHolder = nullptr;
			return TRUE;
		}
		break;
	case HIT_MESSAGE_UNKB:
		unkA8->receiveMessageFromChild(this);
		return TRUE;
	}

	return FALSE;
}

// TODO: UNUSED in the final game (0xFC bytes in the map), the real body is
// unknown. Probably the bee-side counterpart of
// TBeeHive::receiveMessageFromChild.
void TBee::behaveToEat()
{
	onFlag(TRealoidActor::FLAG_UNK4);
	onHitFilter(HIT_FILTER_NO_COLLISION);
}

TBeeHive::TBeeHive(const char* name)
    : TRealoid(name)
{
	mItem = nullptr;
}

void TBeeHive::init(TLiveManager* manager)
{
	mManager = manager;
	mManager->manageActor(this);
	onLiveFlag(LIVE_FLAG_UNK10 | LIVE_FLAG_UNK8);
	mSpine->initWith(&TNerveBeeHiveWait::theNerve());
	mInitialPos  = mPosition;
	mBeeNum      = 0;
	mCoinIdx     = 0;
	initCollision();
}

void TBeeHive::reset()
{
	mPosition = mInitialPos;

	TPosition3f mtx;
	MsMtxSetRotRPH(mtx, mRotation.x, 0.0f, mRotation.z);
	mtx.getQuat(mBaseQuat);

	mCurQuat.setEulerY(mRotation.y);
	mShakeQuat = mCurQuat;

	mSwingAngle    = 0.0f;
	mSwingSpeed    = 0.0f;
	mSwingLimit    = 0.015707964f;
	mCollisionIdx  = 0;
	onLiveFlag(LIVE_FLAG_UNK10 | LIVE_FLAG_AIRBORNE);
	mMActor = mMActorKeeper->getMActor("bee_nest.bmd");
	offHitFilter(HIT_FILTER_NO_COLLISION);
}

void TBeeHive::initCollision()
{
	initHitActor(ACTOR_TYPE_ENEMY_UNK31, 0, 0x80000000, 50.0f, 50.0f, 100.0f,
	             100.0f);
	onHitFilter(HIT_FILTER_NO_ATTACK);
}

void TBeeHive::initBoids()
{
	for (int i = 0; i < unk150->mNumBoids; ++i) {
		TRealoidActor* bee = unk154[i];
		bee->init();
		bee->onFlag(TRealoidActor::FLAG_UNK2);
	}
}

void TBeeHive::setBoidBaseParams()
{
	unk150->mBaseSpeed         = 25.0f;
	unk150->mNeighborRadius    = 80.0f;
	unk150->mYawSpeed          = 8.0f;
	unk150->mPitchSpeed        = 8.0f;
	unk150->mMaxPitch          = 85.0f;
	unk150->mAlignmentStrength = 0.001f;
}

void TBeeHive::setBoidParamOnAttacking()
{
	setBoidBaseParams();
	unk150->setGoal(TPathNode((THitActor*)gpMarioAddress));
	unk150->mGoalOffset = JGeometry::TVec3<f32>(0.0f, 200.0f, 0.0f);
}

void TBeeHive::setBoidParamOnWaiting()
{
	setBoidBaseParams();
	unk150->setGoal(TPathNode(mPosition));
	unk150->mGoalOffset = JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f);
}

void TBeeHive::setBoidParamOnMarioWaterIn()
{
	setBoidBaseParams();
	unk150->setGoal(TPathNode((THitActor*)gpMarioAddress));
	unk150->mGoalOffset = JGeometry::TVec3<f32>(0.0f, 500.0f, 0.0f);
}

void TBeeHive::receiveMessageFromChild(TBee* bee)
{
	if (bee->checkFlag(TRealoidActor::FLAG_UNK4))
		return;

	bee->onFlag(TRealoidActor::FLAG_UNK4);
	bee->onHitFilter(HIT_FILTER_NO_COLLISION);

	TMapObjBase* coin = mCoins[mCoinIdx];
	if (mCoinIdx != unk150->getBoidNum() - 1)
		coin = gpItemManager->makeObjAppear(ACTOR_TYPE_COIN);

	if (coin != nullptr) {
		coin->appear();
		coin->mPosition = bee->mPosition;
		coin->mVelocity.x = 0.0f;
		coin->mVelocity.y = 15.0f;
		coin->mVelocity.z = 0.0f;
		coin->offLiveFlag(LIVE_FLAG_UNK10);
	}

	++mCoinIdx;
}

void TBeeHive::load(JSUMemoryInputStream& stream)
{
	loadDefault(stream, "bee_body.bmd", 2);
	loadCoin(stream);
	mMActorKeeper->createMActor("bee_nest_break.bmd", 3);
	mMActor = mMActorKeeper->createMActor("bee_nest.bmd", 3);
	initBoids();
	reset();
}

void TBeeHive::loadCoin(JSUMemoryInputStream& stream)
{
	u32 itemEventId;
	u32 coinEventId;
	stream >> itemEventId;
	stream >> coinEventId;

	mItem  = TMapObjBaseManager::newAndRegisterObjByEventID(itemEventId, "");
	mCoins = new TMapObjBase*[unk150->mNumBoids];

	TMapObjBase** it  = mCoins;
	TMapObjBase** end = mCoins + unk150->mNumBoids;
	--end;
	for (; it != end; ++it)
		*it = TMapObjBaseManager::newAndRegisterObj("coin");

	*it = TMapObjBaseManager::newAndRegisterObjByEventID(coinEventId, "");
	if (*it == nullptr)
		*it = TMapObjBaseManager::newAndRegisterObj("coin");
}

TRealoidActor* TBeeHive::createRealoidActor(MActor* actor)
{
	return new TBee(actor, this);
}

BOOL TBeeHive::receiveMessage(THitActor* sender, u32 message)
{
	switch (message) {
	case HIT_MESSAGE_SPRAYED_BY_WATER: {
		JGeometry::TVec3<f32> diff = mPosition;
		diff -= SMS_GetMarioPos();
		setShakePower(diff);
		SMS_EmitWaterHitParticleAndSound(&sender->mPosition);
		return TRUE;
	}

	case HIT_MESSAGE_TRAMPLE:
	case HIT_MESSAGE_HIP_DROP:
	case HIT_MESSAGE_PUNCH:
		if (isWaiting()) {
			mSpine->reset();
			mSpine->setNext(&TNerveBeeHiveFall::theNerve());
		}
		return TRUE;
	}

	return FALSE;
}

void TBeeHive::perform(u32 cue, JDrama::TGraphics* graphics)
{
	TRealoid::perform(cue, graphics);
	TSpineEnemy::perform(cue, graphics);
}

void TBeeHive::control()
{
	controlCollision();
	TLiveActor::control();
	controlSound();
}

void TBeeHive::bind()
{
	if (checkLiveFlag(LIVE_FLAG_UNK10))
		return;

	JGeometry::TVec3<f32> pos = mPosition;
	pos += mPositionDelta;
	pos += mVelocity;

	mVelocity.y -= getGravityY();
	if (mVelocity.y < mVelocityMinY)
		mVelocity.y = mVelocityMinY;

	f32 offset = -60.0f
	             * (-1.0f
	                + JMASCos((s16)(182.04445f * (57.295776f * mSwingAngle))));

	mGroundHeight = gpMap->checkGround(pos.x, offset + (pos.y + mHeadHeight),
	                                   pos.z, &mGroundPlane);
	mGroundHeight += 1.0f;

	if (pos.y + offset <= 0.05f + mGroundHeight) {
		if (mGroundPlane->checkFlag(BG_CHECK_FLAG_ILLEGAL))
			kill();
		offLiveFlag(LIVE_FLAG_AIRBORNE);
		mVelocity.x = 0.0f;
		mVelocity.y = 0.0f;
		mVelocity.z = 0.0f;
		pos.y       = mGroundHeight - offset;
	} else {
		onLiveFlag(LIVE_FLAG_AIRBORNE);
	}

	gpMap->isTouchedOneWallAndMoveXZ(&pos.x, pos.y + mHeadHeight, &pos.z,
	                                 mBodyRadius);

	mPositionDelta = pos - mPosition;
}

void TBeeHive::controlCollision()
{
	int idx            = mCollisionIdx;
	int num            = mBeeNum;
	TRealoidActor* bee = getRealoid(idx);
	bee->checkHitActors();
	bee->onHitFilter(HIT_FILTER_NO_ATTACK);

	// TODO: the target computes and compares `idx + 1` against `num` here and
	// then discards the result, most likely a statement whose body was
	// compiled out (debug code?). This guess does not reproduce the dead
	// compare, but the extra statement keeps this function from being
	// auto-inlined into control(), as in the target.
	if (num <= idx + 1) { }

	if (num <= ++mCollisionIdx)
		mCollisionIdx = 0;

	int next = mCollisionIdx;
	if (num <= next)
		next = 0;

	TRealoidActor* nextBee = getRealoid(next);
	if (nextBee->checkFlag(TRealoidActor::FLAG_UNK2_OR_UNK4))
		return;

	nextBee->offHitFilter(HIT_FILTER_NO_ATTACK);
}

void TBeeHive::controlSound()
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;

	if (mBeeNum != 0) {
		f32 x     = 0.0f;
		f32 y     = 0.0f;
		f32 z     = 0.0f;
		int count = 0;
		for (int i = 0; i < mBeeNum; ++i) {
			TRealoidActor* bee = unk154[i];
			if (!(bee->mFlags & TRealoidActor::FLAG_UNK2_OR_UNK4)) {
				++count;
				x += bee->mPosition.x;
				y += bee->mPosition.y;
				z += bee->mPosition.z;
			}
		}

		if (count != 0) {
			f32 inv = 1.0f / count;
			x *= inv;
			y *= inv;
			z *= inv;
			mSoundPos.set(x, y, z);
			gpMSound->startBeeSe(&mSoundPos, count);
		}
	}
}

void TBeeHive::calcRootMatrix()
{
	JGeometry::TQuat4<f32> rot = mBaseQuat;

	JGeometry::TQuat4<f32> swing;
	swing.setEulerX(mSwingAngle);

	rot.mul(rot, mCurQuat);
	rot.mul(rot, swing);

	TPosition3f mtx;
	setSQT(mtx, mScaling, rot, mPosition);
	mtx.ref(1, 3) += 120.0f;
	getModel()->setBaseTRMtx(mtx);
}

void TBeeHive::prepareWait()
{
	onLiveFlag(LIVE_FLAG_UNK10 | LIVE_FLAG_AIRBORNE);
	if (mBeeNum < 3) {
		for (int i = 0; i < 3; ++i)
			appearBee(i);
		mBeeNum = 3;
	}
	setBoidParamOnWaiting();
}

bool TBeeHive::doWait()
{
	f32 prevSpeed = mSwingSpeed;
	f32 accel     = mSwingAngle * -getParams()->mRebound.get();
	mSwingSpeed += accel;
	mSwingSpeed *= getParams()->mDecay.get();
	mSwingAngle += mSwingSpeed;

	if (mSwingAngle < -mSwingLimit || mSwingLimit < mSwingAngle) {
		mSwingLimit += getParams()->mAngleMaxAdd.get();
		mSwingLimit = JGeometry::TUtil<f32>::clamp(
		    mSwingLimit, 0.0f, JGeometry::TUtil<f32>::halfPI());
		mSwingAngle
		    = JGeometry::TUtil<f32>::clamp(mSwingAngle, -mSwingLimit, mSwingLimit);
		mSwingSpeed = 0.0f;
		gpMSound->startSoundActorWithInfo(0x28F7, &mPosition, nullptr,
		                                  fabsf(mSwingAngle), 0, 0, nullptr, 0,
		                                  4);
	}

	f32 eps      = JGeometry::TUtil<f32>::epsilon();
	bool stopped = JGeometry::TUtil<f32>::epsilonEquals(0.0f, mSwingSpeed, eps);
	if (!stopped && prevSpeed * mSwingSpeed <= 0.0f) {
		gpMSound->startSoundActorWithInfo(0x28F7, &mPosition, nullptr,
		                                  fabsf(mSwingAngle), 0, 0, nullptr, 0,
		                                  4);
	}

	mCurQuat.slerp(mShakeQuat, 0.01f);
	mCurQuat.normalize();

	JGeometry::TVec3<f32> diff = SMS_GetMarioPos();
	diff -= mPosition;
	f32 range = getParams()->mSearchRange.get();
	if (diff.squared() <= range * range)
		setBoidParamOnAttacking();
	else
		setBoidParamOnWaiting();

	return cAngleLimit <= fabsf(mSwingAngle);
}

void TBeeHive::prepareFall()
{
	offLiveFlag(LIVE_FLAG_UNK10);
	onLiveFlag(LIVE_FLAG_AIRBORNE);
	gpMSound->startSoundActor(0x28F4, &mPosition, 0, nullptr, 0, 4);

	JGeometry::TVec3<f32> dir;
	mCurQuat.getZDir(dir);
	dir.scale(1.0f);

	mSwingSpeed = getParams()->mFallAngularVel.get();
	mVelocity   = dir;
}

bool TBeeHive::doFall()
{
	mSwingAngle += mSwingSpeed;
	return !checkLiveFlag(LIVE_FLAG_AIRBORNE);
}

void TBeeHive::appearBee(int idx)
{
	TRealoidActor* bee = getRealoid(idx);
	if (bee->checkFlag(TRealoidActor::FLAG_UNK4))
		return;
	if (!bee->checkFlag(TRealoidActor::FLAG_UNK2))
		return;

	bee->offFlag(TRealoidActor::FLAG_UNK2);
	bee->offHitFilter(HIT_FILTER_NO_COLLISION);
	TBoid* boid     = unk150->getBoid(idx);
	boid->mPosition = mPosition;
}

void TBeeHive::disappearBee(int idx)
{
	TRealoidActor* bee = getRealoid(idx);
	if (!bee->checkFlag(TRealoidActor::FLAG_UNK2)) {
		bee->onFlag(TRealoidActor::FLAG_UNK2);
		bee->onHitFilter(HIT_FILTER_NO_COLLISION);
	}
}

void TBeeHive::decrimentBee()
{
	if (mBeeNum > 3) {
		--mBeeNum;
		disappearBee(mBeeNum);
	}
}

void TBeeHive::appearAllBees()
{
	for (int i = 0; i < unk150->mNumBoids; ++i)
		appearBee(i);
	mBeeNum = unk150->mNumBoids;
}

void TBeeHive::dropCoin()
{
	TMapObjBase* item = mItem;
	if (item != nullptr) {
		if (item->isActorType(ACTOR_TYPE_COIN))
			item = gpItemManager->makeObjAppear(ACTOR_TYPE_COIN);

		if (item != nullptr) {
			item->appear();
			item->JSGSetTranslation(mPosition);
			item->mVelocity.x = 0.0f;
			item->mVelocity.y = 0.0f;
			item->mVelocity.z = 0.0f;
			item->offLiveFlag(LIVE_FLAG_UNK10);
			item->onLiveFlag(LIVE_FLAG_AIRBORNE);
			mItem = nullptr;
		}
	}
}

void TBeeHive::prepareBreak()
{
	mMActor = mMActorKeeper->getMActor("bee_nest_break.bmd");
	mMActor->setBck("bee_nest_break");
	onHitFilter(HIT_FILTER_NO_COLLISION);
	SMS_EasyEmitParticle(PARTICLE_MS_ENM_DISAP_A, &mPosition, this,
	                     JGeometry::TVec3<f32>(2.0f, 2.0f, 2.0f));
	SMS_EasyEmitParticle(PARTICLE_MS_ENM_DISAP_B, &mPosition, this,
	                     JGeometry::TVec3<f32>(2.0f, 2.0f, 2.0f));
	gpMSound->startSoundActor(0x28F6, &mPosition, 0, nullptr, 0, 4);
}

// TODO: UNUSED in the final game, contents unknown
void TBeeHive::prepareVanish() { onLiveFlag(LIVE_FLAG_HIDDEN); }

void TBeeHive::prepareReset()
{
	reset();
	offLiveFlag(LIVE_FLAG_HIDDEN);
	mScaling.x = 0.0f;
	mScaling.y = 0.0f;
	mScaling.z = 0.0f;
}

bool TBeeHive::doScaling()
{
	bool done = false;
	mScaling.x += 0.01f;
	if (1.0f <= mScaling.x) {
		mScaling.x = 1.0f;
		done       = true;
		mScaling.y = 1.0f;
		mScaling.z = 1.0f;
	} else {
		mScaling.y += 0.01f;
		mScaling.z += 0.01f;
	}
	return done;
}

bool TBeeHive::isMissMario() const
{
	if (mBeeNum == 0)
		return false;

	JGeometry::TVec3<f32> diff = SMS_GetMarioPos();
	diff -= getCenterOfGravity();
	f32 range = getParams()->mGiveupRange.get();
	return range * range <= diff.squared();
}

void TBeeHive::setShakePower(const JGeometry::TVec3<f32>& dir)
{
	if (isFalling())
		return;

	JGeometry::TVec3<f32> axis = dir;
	axis.y                     = 0.0f;
	axis.normalize();

	f32 sign = fabsf(mSwingSpeed) < 0.0001f
	               ? 1.0f
	               : (f32)(getSwingSpeedC() > 0.0f   ? 1
	                       : mSwingSpeed < 0.0f ? -1
	                                            : 0);

	JGeometry::TVec3<f32> front;
	front.set(0.0f, 0.0f, 1.0f);
	mShakeQuat.setRotate(front, axis);
	mSwingSpeed += sign * getParams()->mShakePower.get();
}

JGeometry::TVec3<f32> TBeeHive::getCenterOfGravity() const
{
	int num = mBeeNum;
	if (num == 0)
		return JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f);

	JGeometry::TVec3<f32> center(0.0f, 0.0f, 0.0f);
	TBoidLeader* boids = unk150;
	for (int i = 0; i < num; ++i)
		center += boids->getBoid(i)->mPosition;

	center.scale(1.0f / num);
	// address-exposed return copy: the ROM reloads getBoid() after every
	// store to center, so center is pinned to memory there
	return *(const JGeometry::TVec3<f32>*)&center;
}

bool TBeeHive::isFalling() const
{
	return mSpine->getLatestNerve() == &TNerveBeeHiveFall::theNerve();
}

bool TBeeHive::isWaiting() const
{
	return mSpine->getLatestNerve() == &TNerveBeeHiveWait::theNerve();
}

bool TBeeHive::isNotBreakOnceYet() const
{
	return !checkLiveFlag(LIVE_FLAG_HIDDEN);
}

TBeeHiveManager::TBeeHiveManager(const char* name)
    : TEnemyManager(name)
{
}

void TBeeHiveManager::load(JSUMemoryInputStream& stream)
{
	unk38 = new TBeeHiveParams("/Animal/beehive.prm");
	TEnemyManager::load(stream);
}

void TBeeHiveManager::createModelData()
{
	static const TModelDataLoadEntry entry[] = {
		{ "bee_body.bmd",
		  J3DMLF_MaterialPEFull | J3DMLF_UseUniqueMaterials
		      | (1 << J3DMLF_TevStageNumShift),
		  0 },
		{ "bee_nest.bmd",
		  J3DMLF_MaterialPEFull | J3DMLF_UseUniqueMaterials
		      | (1 << J3DMLF_TevStageNumShift),
		  0 },
		{ "bee_nest_break.bmd",
		  J3DMLF_MaterialPEFull | J3DMLF_UseUniqueMaterials
		      | (1 << J3DMLF_TevStageNumShift),
		  0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

DEFINE_NERVE(TNerveBeeHiveWait, TLiveActor)
{
	TBeeHive* hive = (TBeeHive*)spine->getBody();
	if (spine->getTime() == 0)
		hive->prepareWait();

	if (hive->doWait()) {
		spine->pushAfterCurrent(&TNerveBeeHiveFall::theNerve());
		return TRUE;
	}

	if (hive->getParams()->mDecrimentTimer.get() < spine->getTime()) {
		hive->decrimentBee();
		spine->pushAfterCurrent(this);
		return TRUE;
	}

	return FALSE;
}

DEFINE_NERVE(TNerveBeeHiveFall, TLiveActor)
{
	TBeeHive* hive = (TBeeHive*)spine->getBody();
	if (spine->getTime() == 0) {
		hive->prepareFall();
		hive->dropCoin();
	}

	if (hive->doFall()) {
		spine->pushAfterCurrent(&TNerveBeeHiveBreak::theNerve());
		return TRUE;
	}

	return FALSE;
}

DEFINE_NERVE(TNerveBeeHiveBreak, TLiveActor)
{
	TBeeHive* hive = (TBeeHive*)spine->getBody();
	if (spine->getTime() == 0) {
		hive->prepareBreak();
		hive->appearAllBees();
		hive->setBoidParamOnAttacking();
	}

	if (hive->checkCurAnmEnd(0)) {
		spine->pushAfterCurrent(&TNerveBeeHiveAttack::theNerve());
		hive->onLiveFlag(LIVE_FLAG_HIDDEN);
		return TRUE;
	}

	return FALSE;
}

DEFINE_NERVE(TNerveBeeHiveAttack, TLiveActor)
{
	TBeeHive* hive = (TBeeHive*)spine->getBody();
	if (spine->getTime() == 0)
		hive->setBoidParamOnAttacking();

	if (SMS_IsMarioInWater()) {
		spine->pushAfterCurrent(&TNerveBeeHiveMarioWaterIn::theNerve());
		return TRUE;
	}

	if (hive->isMissMario()) {
		spine->pushAfterCurrent(&TNerveBeeHiveReset::theNerve());
		return TRUE;
	}

	return FALSE;
}

DEFINE_NERVE(TNerveBeeHiveMarioWaterIn, TLiveActor)
{
	TBeeHive* hive = (TBeeHive*)spine->getBody();
	if (spine->getTime() == 0)
		hive->setBoidParamOnMarioWaterIn();

	if (hive->getParams()->mGiveupTimer.get() < spine->getTime()) {
		spine->pushAfterCurrent(&TNerveBeeHiveReset::theNerve());
		return TRUE;
	}

	if (!SMS_IsMarioInWater()) {
		spine->pushAfterCurrent(&TNerveBeeHiveAttack::theNerve());
		return TRUE;
	}

	return FALSE;
}

DEFINE_NERVE(TNerveBeeHiveReset, TLiveActor)
{
	TBeeHive* hive = (TBeeHive*)spine->getBody();
	if (spine->getTime() == 0)
		hive->prepareReset();

	if (hive->doScaling()) {
		spine->pushAfterCurrent(&TNerveBeeHiveWait::theNerve());
		return TRUE;
	}

	return FALSE;
}
