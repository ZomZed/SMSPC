#include <Enemy/Kukku.hpp>
#include <Animal/AnimalBase.hpp>
#include <Enemy/Graph.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JGeometry.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/InfectiousStrings.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <MSound/SoundEffects.hpp>
#include <Map/Map.hpp>
#include <Map/MapCollisionData.hpp>
#include <Map/PollutionManager.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/RandomUtil.hpp>
#include <MarioUtil/TexUtil.hpp>
#include <MoveBG/ItemManager.hpp>
#include <MoveBG/MapObjBase.hpp>
#include <MoveBG/MapObjManager.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/Strategy.hpp>
#include <System/Particles.hpp>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>

static const char* tori_bastable[] = {
	"/scene/tori/bas/tori_back.bas",     "/scene/tori/bas/tori_down.bas",
	nullptr,                             "/scene/tori/bas/tori_fall_end.bas",
	"/scene/tori/bas/tori_hit.bas",      "/scene/tori/bas/tori_wait.bas",
};

namespace {
static const int cDropCoinNumTable[] = { 3, 3, 1, 2 };
}

// UNUSED (Size: 0x70 in MAP)
TKukkuBall::TKukkuBall(MActor* mactor)
    : THitActor("クック玉")
    , mMActor(mactor)
    , mBallFlags(KUKKU_BALL_FLAG_DEAD)
    , unk7C(0)
{
}

void TKukkuBall::init()
{
	initHitActor(ACTOR_TYPE_ENEMY_UNK2E, 1, HIT_CATEGORY_PLAYER, 30.0f, 30.0f,
	             0.0f, 0.0f);
	onHitFilter(HIT_FILTER_NO_COLLISION);
	onHitFilter(HIT_FILTER_NO_DAMAGE);

	static_cast<TIdxGroupObj*>(JDrama::TNameRefGen::search("敵グループ"))
	    ->getChildren()
	    .push_back(this);

	ResTIMG* img = (ResTIMG*)JKRFileLoader::getGlbResource(
	    "/scene/map/pollution/H_ma_rak.bti");
	if (img != nullptr)
		SMS_ChangeTextureAll(mMActor->getModel()->getModelData(),
		                     "K_name_dummy", *img);
}

void TKukkuBall::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (mBallFlags & KUKKU_BALL_FLAG_DEAD)
		return;

	if (cue & 2) {
		TPosition3f mtx;
		mtx.identity33();
		mtx.setTrans(mPosition);
		mMActor->getModel()->setBaseScale(mScaling);
		mMActor->getModel()->setBaseTRMtx(mtx);
		mMActor->getModel()->calc();
	}

	// TODO: probably moveObject() in the original, but calling it here stops
	// checkHitActors() and bind() from being inlined, unlike the target.
	if (cue & 1) {
		checkHitActors();
		bind();
	}

	if (!(mBallFlags & KUKKU_BALL_FLAG_HIDDEN))
		mMActor->perform(cue, graphics);
}

// UNUSED (Size: 0x3C in MAP)
void TKukkuBall::moveObject()
{
	checkHitActors();
	bind();
}

// UNUSED (Size: 0x128 in MAP)
void TKukkuBall::bind()
{
	mVelocity.y -= 0.9f;
	mVelocity.scale(0.94f);

	JGeometry::TVec3<f32> pos = mPosition;
	pos += mVelocity;

	const TBGCheckData* ground;
	f32 groundY = gpMap->checkGround(pos.x, mAttackHeight + pos.y, pos.z,
	                                 &ground);
	if (pos.y <= 0.05f + (1.0f + groundY))
		kill();

	gpMap->isTouchedOneWallAndMoveXZ(&pos.x, mAttackHeight + pos.y, &pos.z,
	                                 mAttackRadius);
	mPosition = pos;
}

// UNUSED (Size: 0xC0 in MAP)
void TKukkuBall::checkHitActors()
{
	THitActor** end = mCollisions + mColCount;
	for (THitActor** it = mCollisions; it != end; ++it) {
		switch ((*it)->getActorType()) {
		case ACTOR_TYPE_MARIO:
			SMS_SendMessageToMario(this, HIT_MESSAGE_ATTACK);
			kill();
			break;
		}
	}
}

// UNUSED (Size: 0x4C in MAP)
void TKukkuBall::startToMove(const JGeometry::TVec3<f32>& position,
                             const JGeometry::TVec3<f32>& velocity)
{
	offHitFilter(HIT_FILTER_NO_COLLISION);
	mBallFlags &= ~KUKKU_BALL_FLAG_DEAD;
	mPosition = position;
	mVelocity = velocity;
}

// UNUSED (Size: 0x50 in MAP)
void TKukkuBall::kill()
{
	onHitFilter(HIT_FILTER_NO_COLLISION);
	mBallFlags |= KUKKU_BALL_FLAG_DEAD;
	gpPollution->pollute(mPosition.x, mPosition.y, mPosition.z, 500.0f);
}

// UNUSED (Size: 0x28 in MAP)
bool TKukkuBall::isDead() const
{
	return (mBallFlags & KUKKU_BALL_FLAG_DEAD) && unk7C == 0;
}

// UNUSED (Size: 0x44 in MAP)
// TODO: deadstripped and never inlined anywhere, body unknown
TEnemyCoinUnit::TEnemyCoinUnit(int num) { }

// UNUSED (Size: 0xA8 in MAP)
// TODO: deadstripped and never inlined anywhere, body unknown
void TEnemyCoinUnit::init() { }

// UNUSED (Size: 0x48 in MAP)
// TODO: deadstripped and never inlined anywhere, body unknown
TMapObjBase* TEnemyCoinUnit::getUnusedItem() { return nullptr; }

TKukku::TKukku(const char* name)
    : TSmallEnemy(name)
    , m1upMushroom(nullptr)
{
}

void TKukku::init(TLiveManager* manager)
{
	mManager = manager;
	mManager->manageActor(this);
	mMActorKeeper = new TMActorKeeper(mManager, 4);
	mMActor       = mMActorKeeper->createMActor("tori.bmd", 0);
	mSpine->initWith(&TNerveKukkuGraphWander::theNerve());
	m1upMushroom = TMapObjBaseManager::newAndRegisterObj("mushroom1upR");
	initBalls();
	initCollision();
	initAnmSound();
	initParticle();
	mCenterJointIndex
	    = getModel()->getModelData()->getJointName()->getIndex("center");
	reset();
}

// UNUSED (Size: 0xC4 in MAP)
void TKukku::initBalls()
{
	for (TKukkuBall** it = mBalls; it != mBalls + 3; ++it) {
		*it = new TKukkuBall(mMActorKeeper->createMActor("torifun.bmd", 3));
		(*it)->init();
	}
}

// UNUSED (Size: 0x58 in MAP)
void TKukku::initCollision()
{
	initHitActor(ACTOR_TYPE_ENEMY_UNK2E, 1, HIT_CATEGORY_PLAYER, 30.0f, 30.0f,
	             100.0f, 100.0f);
	offHitFilter(HIT_FILTER_NO_COLLISION);
}

// UNUSED (Size: 0x8C in MAP)
void TKukku::initParticle()
{
	SMS_LoadParticle("/scene/tori/jpa/ms_cooc_ase.jpa", KUKKU_JPA_MS_COOC_ASE);
	SMS_LoadParticle("/scene/tori/jpa/ms_cooc_hane.jpa",
	                 KUKKU_JPA_MS_COOC_HANE);
}

void TKukku::reset()
{
	unk1A4          = 0;
	mShootTimer     = 0;
	mGravity        = 0.0f;
	mDroppedCoinNum = 0;
	onLiveFlag(LIVE_FLAG_AIRBORNE);
	mScaledBodyRadius = 75.0f;
}

BOOL TKukku::receiveMessage(THitActor* sender, u32 message)
{
	if (checkLiveFlag(LIVE_FLAG_DEAD))
		return false;

	switch (message) {
	case HIT_MESSAGE_TRAMPLE:
	case HIT_MESSAGE_HIP_DROP:
		behaveHitTrample();
		return true;
	}

	return TSmallEnemy::receiveMessage(sender, message);
}

void TKukku::control()
{
	if (getUnk1A4() > 0)
		unk1A4 -= 1;

	TLiveActor::control();
}

void TKukku::calcRootMatrix()
{
	if (isDying()) {
		JGeometry::TVec3<f32> up;
		if (mGroundPlane != nullptr) {
			up.set(mGroundPlane->getNormal());
			up.normalize();
		} else {
			up.set(0.0f, 1.0f, 0.0f);
		}

		JGeometry::TQuat4<f32> yaw;
		yaw.setEulerY(0.017453294f * mRotation.y);

		JGeometry::TQuat4<f32> quat;
		quat.setRotate(JGeometry::TVec3<f32>(0.0f, 1.0f, 0.0f), up, 1.0f);
		quat.mul(quat, yaw);

		TPosition3f mtx;
		mtx.setQT(quat, mPosition);
		getModel()->setBaseTRMtx(mtx);
		getModel()->setBaseScale(mScaling);
	} else {
		TSpineEnemy::calcRootMatrix();
	}

	updateEffect();
}

void TKukku::bind() { TLiveActor::bind(); }

void TKukku::perform(u32 cue, JDrama::TGraphics* graphics)
{
	TSmallEnemy::perform(cue, graphics);
	for (TKukkuBall** it = mBalls; it != mBalls + 3; ++it)
		(*it)->perform(cue, graphics);
}

// UNUSED (Size: 0x130 in MAP)
void TKukku::updateEffect()
{
	if (isFalling())
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    KUKKU_JPA_MS_COOC_HANE, getModel()->getAnmMtx(mCenterJointIndex),
		    1, this);

	if (mMActor->checkCurAnm("tori_back", 0))
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    KUKKU_JPA_MS_COOC_ASE, getModel()->getAnmMtx(mCenterJointIndex), 1,
		    this);
}

void TKukku::behaveToWater(THitActor* actor)
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	if (!isFalling() && !isRecoveringGraph()) {
		mSpine->reset();
		mSpine->setNext(&TNerveKukkuFall::theNerve());
	}
}

// UNUSED (Size: 0x58 in MAP)
void TKukku::behaveHitTrample()
{
	mSpine->reset();
	mSpine->setNext(&TNerveSmallEnemyDie::theNerve());
}

// UNUSED (Size: 0x2C8 in MAP)
void TKukku::doFlyToCurPathNode()
{
	updateRotation();
	mPositionDelta = calcMomentum(getParams()->mMarchSpeed.get());
}

void TKukku::updateRotation()
{
	JGeometry::TVec3<f32> diff = getUnkF4().getPoint();
	diff -= mPosition;

	f32 dist = diff.length();
	if (dist < 100.0f)
		return;

	f32 marchSpeed = getParams()->mMarchSpeed.get();
	f32 turnSpeed  = getParams()->mTurnSpeed.get();
	if (dist <= 2.0f * calcMinimumTurnRadius(marchSpeed, turnSpeed))
		turnSpeed = calcTurnSpeedToReach(marchSpeed, 0.5f * dist);

	TAnimalBase::getRotationFlyToDir(&mRotation, diff, marchSpeed, turnSpeed);

	// Keep the body level while flying, but not while falling
	mRotation.x *= isFalling() ? 0.0f : 1.0f;
	mRotation.z *= isFalling() ? 0.0f : 1.0f;
}

JGeometry::TVec3<f32> TKukku::calcMomentum(f32 speed)
{
	JGeometry::TQuat4<f32> quat(SMS_Eular2Quat(mRotation));
	JGeometry::TVec3<f32> momentum(0.0f, 0.0f, speed);
	quat.rotate(momentum, momentum);
	return momentum;
}

// UNUSED (Size: 0xA0 in MAP)
void TKukku::doRecoverToCurPathNode() { doFlyToCurPathNode(); }

// UNUSED (Size: 0xD8 in MAP)
// TODO: deadstripped and never inlined anywhere, body unknown
void TKukku::doHabataki() { }

// UNUSED (Size: 0x7C in MAP)
void TKukku::decideFlyingAnm()
{
	if (mMActor->checkCurAnm("tori_back", 0))
		changeBck("tori_back");
	else
		changeBck("tori_wait");
}

// UNUSED (Size: 0x20C in MAP)
void TKukku::shotBall()
{
	if (getShootTimer() >= 0) {
		--mShootTimer;
		return;
	}

	TKukkuBall* ball = getUnusedBall();
	if (ball == nullptr)
		return;

	JGeometry::TVec3<f32> velocity(0.0f, 1.0f, 0.0f);
	velocity.setLength(getParams()->mShootSpeed.get());

	// TODO: the result is unused, the ball spawns at the body position
	getModel()->getModelData()->getJointName()->getIndex("null_osen");

	JGeometry::TVec3<f32> position = mPosition;
	ball->startToMove(position, velocity);
	mShootTimer = getParams()->mShootInterval.get();
	SMSGetMSound()->startSoundActor(MSD_SE_EN_TORI_SHIT, &mPosition, 0,
	                                nullptr, 0, 4);
}

// fabricated: keeps TVec3::set<f> out of line like the ROM
static inline void KukkuSetVec2(JGeometry::TVec3<f32>& v, f32 x, f32 y, f32 z)
{
	v.set(x, y, z);
}
static inline void KukkuSetVec1(JGeometry::TVec3<f32>& v, f32 x, f32 y, f32 z)
{
	KukkuSetVec2(v, x, y, z);
}
static inline void KukkuSetVec(JGeometry::TVec3<f32>& v, f32 x, f32 y, f32 z)
{
	KukkuSetVec1(v, x, y, z);
}

// fabricated: the ROM copies the velocity with lfs/stfs pairs (set(const Vec&))
static inline void KukkuSetCoinVelocity(TMapObjBase* coin,
                                        const JGeometry::TVec3<f32>& v)
{
	coin->mVelocity.set(v);
	coin->offLiveFlag(LIVE_FLAG_UNK10);
}

void TKukku::dropCoins()
{
	if (mDroppedCoinNum > 10)
		return;

	if (mDroppedCoinNum == 10 && m1upMushroom != nullptr) {
		++mDroppedCoinNum;
		m1upMushroom->appear();
		m1upMushroom->JSGSetTranslation(mPosition);
		m1upMushroom->setVelocityAndFlag10(0.0f, 0.0f, 0.0f);
		return;
	}

	int idx = 4.0f * MsRandF();
	if (idx < 1)
		idx = 1;
	else if (idx > 3)
		idx = 3;

	int coinNum = cDropCoinNumTable[idx];

	f32 halfStep = 0.5f * (6.2831855f / coinNum);
	f32 stepSin  = sinf(halfStep);
	f32 stepCos  = cosf(halfStep);
	JGeometry::TQuat4<f32> step(0.0f, stepSin, 0.0f, stepCos);

	f32 halfPitch = 0.5f * (3.1415927f * getParams()->mDropAngleX.get());
	f32 pitchSin  = sinf(halfPitch);
	f32 pitchCos  = cosf(halfPitch);
	JGeometry::TQuat4<f32> pitch(pitchSin, 0.0f, 0.0f, pitchCos);

	f32 ry    = mRotation.y;
	f32 speed = getParams()->mDropSpeed.get();
	JGeometry::TVec3<f32> dir;
	KukkuSetVec(dir, speed * MsSin(ry), 0.0f, speed * MsCos(ry));

	JGeometry::TVec3<f32> velocity = dir;
	pitch.rotate(velocity, velocity);
	step.rotate(velocity, velocity);

	for (int i = 0; i < coinNum; ++i) {
		TMapObjBase* coin = gpItemManager->makeObjAppear(ACTOR_TYPE_COIN);
		if (coin == nullptr)
			return;

		coin->appear();
		coin->JSGSetTranslation(mPosition);
		KukkuSetCoinVelocity(coin, velocity);

		if (++mDroppedCoinNum == 10)
			return;

		step.rotate(velocity, velocity);
	}
}

// UNUSED (Size: 0xAC in MAP)
bool TKukku::isFalling() const
{
	const TNerveBase<TLiveActor>* nerve = mSpine->getLatestNerve();
	return nerve == &TNerveKukkuFall::theNerve()
	       || nerve == &TNerveKukkuPostFall::theNerve();
}

// UNUSED (Size: 0x98 in MAP)
bool TKukku::isRecoveringGraph() const
{
	if (mSpine->getLatestNerve() != &TNerveKukkuRecoverGraph::theNerve())
		return false;

	return true;
}

// UNUSED (Size: 0x4C in MAP)
bool TKukku::isDying() const
{
	return mSpine->getLatestNerve() == &TNerveSmallEnemyDie::theNerve();
}

// UNUSED (Size: 0xB4 in MAP)
bool TKukku::isFindOutMario() const
{
	JGeometry::TVec3<f32> diff = *gpMarioPos;
	diff -= mPosition;
	diff.y = 0.0f;

	f32 range = getParams()->mSearchRange.get();
	return diff.squared() < range * range;
}

// UNUSED (Size: 0x38 in MAP)
void TKukku::changeBck(const char* name)
{
	mMActor->setBck(name);
	setCurAnmSound();
}

void TKukku::setDeadAnm()
{
	mMActor->setBck("tori_down");
	setCurAnmSound();
}

void TKukku::setAfterDeadEffect()
{
	TSmallEnemy::setAfterDeadEffect();
	gpPollution->stamp(((TSmallEnemyManager*)mManager)->getUnk58(),
	                   mPosition.x, mPosition.y, mPosition.z, 1000.0f);
}

// UNUSED (Size: 0x8 in MAP)
// TODO: deadstripped and never inlined anywhere, the returned value is a guess
f32 TKukku::getWaterDamageRate() const { return 0.0f; }

// UNUSED (Size: 0x38 in MAP)
f32 TKukku::getWaterPowerY() const
{
	return getParams()->mWaterPowerY.get();
}

// UNUSED (Size: 0x4C in MAP)
TKukkuBall* TKukku::getUnusedBall()
{
	for (TKukkuBall** it = mBalls; it != mBalls + 3; ++it)
		if ((*it)->isDead())
			return *it;

	return nullptr;
}

// UNUSED (Size: 0x250 in MAP)
TKukkuParams::TKukkuParams(const char* path)
    : TSmallEnemyParams(path)
    , PARAM_INIT(mMarchSpeed, 3.0f)
    , PARAM_INIT(mTurnSpeed, 0.2f)
    , PARAM_INIT(mWaterPowerY, 12.0f)
    , PARAM_INIT(mShootSpeed, 1.0f)
    , PARAM_INIT(mShootInterval, 60)
    , PARAM_INIT(mSearchRange, 800.0f)
    , PARAM_INIT(mHabatakiTimer, 45)
    , PARAM_INIT(mAirFric, 0.97f)
    , PARAM_INIT(mUpperVelocityY, 0.0f)
    , PARAM_INIT(mDropSpeed, 5.0f)
    , PARAM_INIT(mDropAngleX, -0.25f)
{
	TParams::load(mPrmPath);
}

TKukkuManager::TKukkuManager(const char* name)
    : TSmallEnemyManager(name)
{
}

void TKukkuManager::load(JSUMemoryInputStream& stream)
{
	TKukkuParams* params = new TKukkuParams("/enemy/kukku.prm");
	unk38                = params;

	params->mSLAttackRadius.set(30);
	params->mSLAttackHeight.set(30);
	params->mSLDamageRadius.set(100);
	params->mSLDamageHeight.set(100);
	params->mSLBodyRadius.set(5.0f);

	TSmallEnemyManager::load(stream);
}

// UNUSED (Size: 0x4 in MAP)
void TKukkuManager::initCoins() { }

void TKukkuManager::createModelData()
{
	static const TModelDataLoadEntry entry[] = {
		{ "tori.bmd", 0x10210000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

const char** TKukku::getBasNameTable() const { return tori_bastable; }

DEFINE_NERVE(TNerveKukkuGraphWander, TLiveActor)
{
	TKukku* self = (TKukku*)spine->getBody();

	if (spine->getTime() == 0) {
		self->mVelocity = JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f);
		self->getTracer()->reset();
		self->goToShortestNextGraphNode();
		self->decideFlyingAnm();
	}

	if (self->isReachedToGoal()) {
		self->goToRandomNextGraphNode();
		if (!self->mMActor->checkCurAnm("tori_wait", 0))
			self->changeBck("tori_wait");
	}

	if (self->isFindOutMario())
		self->shotBall();

	self->doFlyToCurPathNode();
	return false;
}

// TODO: this nerve is deadstripped and never inlined anywhere; only the
// "tori_hit" animation name survives in .rodata, the body is a guess.
DEFINE_NERVE(TNerveKukkuHit, TLiveActor)
{
	TKukku* self = (TKukku*)spine->getBody();

	if (spine->getTime() == 0)
		self->changeBck("tori_hit");

	if (self->checkCurAnmEnd(0)) {
		spine->pushAfterCurrent(&TNerveKukkuGraphWander::theNerve());
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveKukkuFall, TLiveActor)
{
	TKukku* self = (TKukku*)spine->getBody();

	if (spine->getTime() == 0) {
		self->changeBck("tori_wait");
		J3DFrameCtrl* ctrl = self->mMActor->getFrameCtrl(0);
		ctrl->setRate(2.0f * SMSGetAnmFrameRate());
		self->mVelocity = JGeometry::TVec3<f32>(
		    0.0f, -self->getParams()->mWaterPowerY.get(), 0.0f);
		self->dropCoins();
	}

	if (!self->checkLiveFlag(LIVE_FLAG_AIRBORNE)) {
		SMS_EasyEmitParticle(PARTICLE_MS_POPO_BOMB_A, &self->mPosition, nullptr,
		                     JGeometry::TVec3<f32>(1.0f));
		SMS_EasyEmitParticle(PARTICLE_MS_POPO_BOMB_B, &self->mPosition, nullptr,
		                     JGeometry::TVec3<f32>(1.0f));
		spine->pushAfterCurrent(&TNerveSmallEnemyDie::theNerve());
		return true;
	}

	JGeometry::TVec3<f32> velocity = self->mVelocity;
	velocity.scale(self->getParams()->mAirFric.get());
	velocity.y += self->getParams()->mUpperVelocityY.get();

	bool stopped = false;
	if (-0.1f < velocity.y) {
		stopped    = true;
		velocity.y = 0.0f;
	}
	self->mVelocity = velocity;

	if (stopped) {
		spine->pushAfterCurrent(&TNerveKukkuRecoverGraph::theNerve());
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveKukkuPostFall, TLiveActor)
{
	TKukku* self = (TKukku*)spine->getBody();

	if (spine->getTime() == 0) {
		self->changeBck("tori_back");
		self->mVelocity = JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f);
	}

	if (self->checkCurAnmEnd(0)
	    && self->getParams()->mHabatakiTimer.get() < spine->getTime()) {
		spine->pushAfterCurrent(&TNerveKukkuGraphWander::theNerve());
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveKukkuRecoverGraph, TLiveActor)
{
	TKukku* self = (TKukku*)spine->getBody();

	if (spine->getTime() == 0) {
		self->changeBck("tori_back");
		self->mVelocity = JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f);
	}

	if (self->getParams()->mHabatakiTimer.get() < spine->getTime()) {
		spine->pushAfterCurrent(&TNerveKukkuGraphWander::theNerve());
		return true;
	}

	self->doRecoverToCurPathNode();
	return false;
}
