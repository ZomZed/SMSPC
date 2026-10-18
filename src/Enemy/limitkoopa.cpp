#include <Enemy/LimitKoopa.hpp>
#include <Enemy/EnemyManager.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/MActorAnm.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <MoveBG/MapObjCorona.hpp>
#include <Player/MarioAccess.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/Particles.hpp>
#include <System/Application.hpp>
#include <Strategic/Spine.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/Strategy.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <math.h>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

// NOTE: this TU is compiled with -inline deferred, so functions are emitted in
// the reverse order of their definitions here.

class TNerveLimitKoopaTurn : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const = 0;
};

class TNerveLimitKoopaWait : public TNerveLimitKoopaTurn {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveLimitKoopaWait& theNerve()
	{
		static TNerveLimitKoopaWait nerve;
		return nerve;
	}
};

class TNerveLimitKoopaTumble : public TNerveLimitKoopaTurn {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveLimitKoopaTumble& theNerve()
	{
		static TNerveLimitKoopaTumble nerve;
		return nerve;
	}
};

class TNerveLimitKoopaFlame : public TNerveLimitKoopaTurn {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveLimitKoopaFlame& theNerve()
	{
		static TNerveLimitKoopaFlame nerve;
		return nerve;
	}
};

class TNerveLimitKoopaGetDown : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveLimitKoopaGetDown& theNerve()
	{
		static TNerveLimitKoopaGetDown nerve;
		return nerve;
	}
};

class TNerveLimitKoopaStagger : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveLimitKoopaStagger& theNerve()
	{
		static TNerveLimitKoopaStagger nerve;
		return nerve;
	}
};

class TNerveLimitKoopaFall : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveLimitKoopaFall& theNerve()
	{
		static TNerveLimitKoopaFall nerve;
		return nerve;
	}
};

class TNerveLimitKoopaGetShowered : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveLimitKoopaGetShowered& theNerve()
	{
		static TNerveLimitKoopaGetShowered nerve;
		return nerve;
	}
};

class TNerveLimitKoopaHipDropStart : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveLimitKoopaHipDropStart& theNerve()
	{
		static TNerveLimitKoopaHipDropStart nerve;
		return nerve;
	}
};

class TNerveLimitKoopaHipDropJump : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveLimitKoopaHipDropJump& theNerve()
	{
		static TNerveLimitKoopaHipDropJump nerve;
		return nerve;
	}
};

namespace {
BOOL KoopaNeckCallBack(J3DNode* node, int calcTiming);
}

BOOL TNerveLimitKoopaHipDropJump::execute(TSpineBase<TLiveActor>* spine) const
{
	TLimitKoopa* koopa = (TLimitKoopa*)spine->getBody();
	koopa->moveHipDrop();
	if (koopa->mLanded) {
		spine->pushAfterCurrent(&TNerveLimitKoopaWait::theNerve());
		return true;
	}
	return false;
}

BOOL TNerveLimitKoopaHipDropStart::execute(TSpineBase<TLiveActor>* spine) const
{
	TLimitKoopa* koopa = (TLimitKoopa*)spine->getBody();
	if (spine->getTime() == 0) {
		koopa->changeBck(5, 2.0f);
		koopa->unk150[1] = 30;
	}

	if (koopa->unk150[1] <= 0) {
		koopa->startHipDrop();
		koopa->mLanded = false;
		spine->pushAfterCurrent(&TNerveLimitKoopaHipDropJump::theNerve());
		return true;
	}
	return false;
}

BOOL TNerveLimitKoopaWait::execute(TSpineBase<TLiveActor>* spine) const
{
	TLimitKoopa* koopa = (TLimitKoopa*)spine->getBody();
	if (spine->getTime() == 0) {
		koopa->changeBck(10, 2.0f);
		koopa->unk150[0] = 240;
	}

	koopa->moveTurn();
	if (koopa->finishedTurn() && koopa->getMActor()->isCurAnmAlreadyEnd(0)) {
		spine->pushAfterCurrent(&TNerveLimitKoopaHipDropStart::theNerve());
		return true;
	}
	return false;
}

BOOL TNerveLimitKoopaTumble::execute(TSpineBase<TLiveActor>* spine) const
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	TLimitKoopa* koopa = (TLimitKoopa*)spine->getBody();
	koopa->changeBck(8, koopa->getParam()->tumbleSpeed.get());
	if (koopa->getMActor()->curAnmEndsNext(0, nullptr))
		return true;
	return false;
}

BOOL TNerveLimitKoopaFall::execute(TSpineBase<TLiveActor>* spine) const
{
	// TODO: UNUSED, body guessed from TNerveKoopaFall (no fall speed param)
	TLimitKoopa* koopa = (TLimitKoopa*)spine->getBody();
	koopa->changeBck(2, 2.0f);
	return false;
}

BOOL TNerveLimitKoopaFlame::execute(TSpineBase<TLiveActor>* spine) const
{
	// TODO: UNUSED, original body unknown
	return false;
}

BOOL TNerveLimitKoopaStagger::execute(TSpineBase<TLiveActor>* spine) const
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	TLimitKoopa* koopa = (TLimitKoopa*)spine->getBody();
	koopa->changeBck(9, koopa->getParam()->staggerSpeed.get());
	if (koopa->getMActor()->curAnmEndsNext(0, nullptr))
		return true;
	return false;
}

BOOL TNerveLimitKoopaGetShowered::execute(TSpineBase<TLiveActor>* spine) const
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	TLimitKoopa* koopa = (TLimitKoopa*)spine->getBody();
	koopa->changeBck(14, koopa->getParam()->waterhitSpeed.get());
	if (koopa->getMActor()->curAnmEndsNext(0, nullptr))
		return true;
	return false;
}

BOOL TNerveLimitKoopaGetDown::execute(TSpineBase<TLiveActor>* spine) const
{
    // FAKEMATCH(frame): 80 bytes; real construct unknown
    volatile u32 pad[20]; (void)pad;
	TLimitKoopa* koopa = (TLimitKoopa*)spine->getBody();

	switch (koopa->getMActor()->getCurAnmIdx(0)) {
	case 0:
		if (koopa->getMActor()->curAnmEndsNext(0, nullptr))
			koopa->changeBck(1, koopa->getParam()->downSpeed.get());
		break;
	case 1:
		if (koopa->getMActor()->curAnmEndsNext(0, nullptr))
			koopa->changeBck(7, koopa->getParam()->downSpeed.get());
		break;
	case 7:
		if (koopa->getMActor()->curAnmEndsNext(0, nullptr))
			return true;
		break;
	default:
		koopa->changeBck(0, koopa->getParam()->downSpeed.get());
		break;
	}

	return false;
}

TLimitKoopaParts::TLimitKoopaParts(const char* name, u32 actor_type,
                                   TLimitKoopa* owner, f32 radius)
    : TLiveActor(name)
    , mOwner(owner)
{
	static_cast<TIdxGroupObj*>(JDrama::TNameRefGen::getInstance()
	                               ->getRootNameRef()
	                               ->search("敵グループ"))
	    ->getChildren()
	    .push_back(this);
	initHitActor(actor_type, 5, HIT_CATEGORY_PLAYER, radius, radius, radius,
	             radius);
	onHitFilter(HIT_FILTER_NO_ATTACK);
	onHitFilter(HIT_FILTER_NO_DAMAGE);
	onHitFilter(HIT_FILTER_NO_COLLISION);
}

void TLimitKoopaParts::perform(u32 cue, JDrama::TGraphics* graphics)
{
	TLiveActor::perform(cue, graphics);
	if (cue & 1) {
		for (int i = 0; i < getColNum(); ++i)
			attack_(mCollisions[i]);
	}
}

void TLimitKoopaParts::set(const JGeometry::TVec3<f32>& pos, f32 radius,
                           f32 height)
{
	mPosition.set(pos);
	offHitFilter(HIT_FILTER_NO_ATTACK);
	offHitFilter(HIT_FILTER_NO_DAMAGE);
	offHitFilter(HIT_FILTER_NO_COLLISION);
	setHitParams(radius, height, radius, height);
}

void TLimitKoopaParts::remove()
{
	onHitFilter(HIT_FILTER_NO_ATTACK);
	onHitFilter(HIT_FILTER_NO_DAMAGE);
	onHitFilter(HIT_FILTER_NO_COLLISION);
}

TLimitKoopaFlame::TLimitKoopaFlame(TLimitKoopa* owner)
    : TLimitKoopaParts("クッパの吐く炎", HIT_CATEGORY_BOSS | 0x30, owner, 100.0f)
{
}

BOOL TLimitKoopaFlame::receiveMessage(THitActor* sender, u32 message)
{
	switch (message) {
	case HIT_MESSAGE_SPRAYED_BY_WATER:
		return false;
	}
	return true;
}

void TLimitKoopaFlame::attack_(THitActor* target)
{
	if (target->receiveMessage(this, HIT_MESSAGE_UNKA))
		mOwner->stopFlame();
}

TLimitKoopaHand::TLimitKoopaHand(TLimitKoopa* owner)
    : TLimitKoopaParts("クッパの手", HIT_CATEGORY_BOSS | 0x32, owner, 100.0f)
{
}

BOOL TLimitKoopaHand::receiveMessage(THitActor* sender, u32 message)
{
	return true;
}

void TLimitKoopaHand::attack_(THitActor* target)
{
	target->receiveMessage(this, HIT_MESSAGE_ATTACK);
}

TLimitKoopaHead::TLimitKoopaHead(TLimitKoopa* owner)
    : TLimitKoopaParts("クッパの頭", HIT_CATEGORY_BOSS | 0x31, owner, 100.0f)
{
}

BOOL TLimitKoopaHead::receiveMessage(THitActor* sender, u32 message)
{
	switch (message) {
	case HIT_MESSAGE_SPRAYED_BY_WATER:
		mOwner->getShowered();
		break;
	}
	return true;
}

void TLimitKoopaHead::attack_(THitActor* target)
{
	target->receiveMessage(this, HIT_MESSAGE_ATTACK);
}

TLimitKoopaBody::TLimitKoopaBody(TLimitKoopa* owner)
    : TLimitKoopaParts("クッパの体", HIT_CATEGORY_BOSS | 0x33, owner, 100.0f)
{
}

BOOL TLimitKoopaBody::receiveMessage(THitActor* sender, u32 message)
{
	return true;
}

void TLimitKoopaBody::attack_(THitActor* target)
{
	target->receiveMessage(this, HIT_MESSAGE_ATTACK);
}

namespace {

// TODO: unlike TKoopa, the neck is not turned towards mario in this version
BOOL KoopaNeckCallBack(J3DNode* node, int calcTiming) { return TRUE; }

} // namespace

TLimitKoopa::TLimitKoopa(const char* name)
    : TSpineEnemy(name)
{
	onLiveFlag(LIVE_FLAG_AIRBORNE);
	offLiveFlag(LIVE_FLAG_UNK100);
	onLiveFlag(LIVE_FLAG_UNK10);
	mScaledBodyRadius = 1600.0f;
}

void TLimitKoopa::load(JSUMemoryInputStream& stream)
{
	TSpineEnemy::load(stream);
}

void TLimitKoopa::loadAfter()
{
	TSpineEnemy::loadAfter();

	for (int i = 0; i < 10; ++i)
		mFlames[i] = new TLimitKoopaFlame(this);

	for (int i = 0; i < 2; ++i)
		mHands[i] = new TLimitKoopaHand(this);

	mHead = new TLimitKoopaHead(this);
	mBody = new TLimitKoopaBody(this);
}

void TLimitKoopa::init(TLiveManager* manager)
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	mBodyRadius = 800.0f;
	mHeadHeight = 2000.0f;
	TSpineEnemy::init(manager);
	onHitFilter(HIT_FILTER_NO_COLLISION);
	onHitFilter(HIT_FILTER_NO_DAMAGE);
	offHitFilter(HIT_FILTER_NO_ATTACK);
	mSpine->initWith(&TNerveLimitKoopaWait::theNerve());

	getMActor()->initSimpleMotionBlend(16);
	unk170 = 0.0f;
	reset();

	JUTNameTab* jointNames = getModel()->getModelData()->getJointName();
	// the original loops over the joint names here (body stripped)
	for (u16 i = 0; i < jointNames->getNameNum(); i++) { }
	mAgoJointIdx = jointNames->getIndex("ago");
	mHeadJointIdx          = jointNames->getIndex("head");
	mNeckJointIdx          = jointNames->getIndex("neck");

	J3DModelData* modelData = getModel()->getModelData();
	J3DJoint* headJoint     = modelData->getJointNodePointer(mHeadJointIdx);
	headJoint->setCallBack(KoopaNeckCallBack);
	headJoint->setCallBackUserData(this);
}

void TLimitKoopa::reset()
{
    // FAKEMATCH(frame): 32 bytes; real construct unknown
    volatile u32 pad[8]; (void)pad;
	TSpineEnemy::reset();
	changeBck(12, getParam()->waitSpeed.get());
	resetLimitKoopa();
}

void TLimitKoopa::resetLimitKoopa()
{
	mSpine->reset();
	unk150[0]         = 0;
	unk150[1]         = 0;
	unk150[2]         = 0;
	mLanded           = true;
	mDirection.unk0 = 0.0f;
}

// fabricated: the original emitted the four fire effects through two inline
// levels (TVec3::set is called out of line at those sites), as in Koopa.cpp
static inline void LimitKoopaEmitFlameEffect(TLimitKoopa* koopa, int id,
                                             const JGeometry::TVec3<f32>& scale)
{
	JPABaseEmitter* emitter = gpMarioParticleManager->emitAndBindToMtxPtr(
	    id, koopa->getHeadMtx(), 1, koopa);
	if (emitter)
		emitter->setGlobalScale(scale);
}

// fabricated
static inline void LimitKoopaEmitFlameEffects(TLimitKoopa* koopa)
{
	f32 s = koopa->getParam()->flameScale.get();
	JGeometry::TVec3<f32> scale(s, s, s);

	JPABaseEmitter* emitter = gpMarioParticleManager->emitAndBindToMtxPtr(
	    0x1F3, koopa->getHeadMtx(), 3, koopa);
	if (emitter)
		emitter->setGlobalScale(scale);

	LimitKoopaEmitFlameEffect(koopa, 0x1C3, scale);
	LimitKoopaEmitFlameEffect(koopa, 0x1C2, scale);
	LimitKoopaEmitFlameEffect(koopa, 0x1C1, scale);
	LimitKoopaEmitFlameEffect(koopa, 0x1C0, scale);
}

void TLimitKoopa::perform(u32 cue, JDrama::TGraphics* graphics)
{
	TSpineEnemy::perform(cue, graphics);

	for (int i = 0; i < 10; ++i)
		mFlames[i]->perform(cue, graphics);

	mHead->perform(cue, graphics);
	mHands[0]->perform(cue, graphics);
	mHands[1]->perform(cue, graphics);
	mBody->perform(cue, graphics);

	if (cue & 1) {
		setUpHitActors();
		updateTimers();
	}

	if (cue & 2) {
		if (isBreathing())
			LimitKoopaEmitFlameEffects(this);
	}
}

void TLimitKoopa::bind()
{
	JGeometry::TVec3<f32> next = mPosition;
	next.add(mPositionDelta);
	next.add(mVelocity);
	mVelocity.add(mAcceleration);

	mGroundHeight = 3500.0f;
	mGroundHeight += 1.0f;
	if (next.y <= 0.05f + mGroundHeight) {
		next.y  = mGroundHeight;
		mLanded = true;
		mAcceleration.set(0.0f, 0.0f, 0.0f);
		mVelocity.set(0.0f, 0.0f, 0.0f);
	}

	mPositionDelta = next - mPosition;
}

void TLimitKoopa::moveStop()
{
	// TODO: UNUSED, original body unknown
}

// TODO: the original keeps this out of line, but our compiler inlines it into
// the HipDropStart nerve
static inline f32 lkInvSqrt2(f32 x) { return JGeometry::TUtil<f32>::inv_sqrt(x); }
static inline f32 lkInvSqrt(f32 x) { return lkInvSqrt2(x); }

#pragma dont_inline on
void TLimitKoopa::startHipDrop()
{
	JGeometry::TVec3<f32> jump(0.0f, 1.0f, 0.0f);
	jump.scale(getParam()->hipDropInitialSpeedY.get());

	JGeometry::TVec3<f32> marioPos = *gpMarioPos;
	marioPos.y                     = mGroundHeight;
	JGeometry::TVec3<f32> diff;
	diff.sub(marioPos, mPosition);
	JGeometry::TVec3<f32> target;
	target.add(mPosition, diff);

	jump = calcVelocityToJumpToY(target, jump.y,
	                             getParam()->hipDropGravityY.get());
	f32 sq = jump.squared();
	if (JGeometry::TUtil<f32>::sqrt(sq) > 200.0f) {
		if (sq <= JGeometry::TUtil<f32>::epsilon())
			jump.zero();
		else
			jump.scale(JGeometry::TUtil<f32>::one() * lkInvSqrt(sq), jump);
		jump.scale(200.0f);
	}
	mVelocity.set(jump);
}
#pragma dont_inline off

void TLimitKoopa::moveHipDrop()
{
	mAcceleration.set(0.0f, -getGravityY(), 0.0f);
}

void TLimitKoopa::moveTurn() { makeDirection(calcTargetDirection()); }

bool TLimitKoopa::finishedTurn()
{
	if (mDirection.absDirection(calcTargetDirection()) > 0.31415927f)
		return false;
	return true;
}

f32 TLimitKoopa::calcTargetDirection()
{
	TDirectionCalc direction;
	JGeometry::TVec3<f32> offset;
	offset.sub(*gpMarioPos, mPosition);
	offset.y = 0.0f;
	direction.makeDirection(offset);
	return direction.unk0;
}

void TLimitKoopa::makeDirection(f32 target)
{
	mDirection.unk0 = mDirection.calcTurnDirection(
	    target, TDirectionCalc::d2r(getParam()->rotationSpeed.get()));
}

f32 TLimitKoopa::getGravityY() const { return getParam()->hipDropGravityY.get(); }

BOOL TLimitKoopa::receiveMessage(THitActor* sender, u32 message)
{
	return TSpineEnemy::receiveMessage(sender, message);
}

void TLimitKoopa::calcRootMatrix()
{
    // FAKEMATCH(frame): 16 bytes; real construct unknown
    volatile u32 pad[4]; (void)pad;
	f32 scale = getParam()->bodyScale.get();
	mScaling.set(scale, scale, scale);
	mRotation.y = TDirectionCalc::r2d(mDirection.unk0);
	TSpineEnemy::calcRootMatrix();
}

static inline TSpineEnemyParams* LkSave3(TEnemyManager* m) { return m->getSaveParam(); }
static inline TSpineEnemyParams* LkSave2(TEnemyManager* m) { return LkSave3(m); }
static inline TSpineEnemyParams* LkSave1(TEnemyManager* m) { return LkSave2(m); }

void TLimitKoopa::stopFlame()
{
	changeBck(3, ((TLimitKoopaParams*)LkSave1((TEnemyManager*)mManager))->fireSpeed.get());
}

void TLimitKoopa::breathFlame()
{
	MtxPtr mtx = getMActor()->getModel()->getAnmMtx(mHeadJointIdx);
	if (isBreathing()) {
		f32 rate = 1.0f;
		if (getAnmIndex() == 5) {
			J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(0);
			rate = (ctrl->getFrame() - 125.0f) / (ctrl->getEnd() - 125.0f);
		}

		for (int i = 0; i < 10; ++i) {
			f32 height = getParam()->flameHeight.get();
			f32 radius = getParam()->flameRadius.get();
			f32 dist   = 0.8f * ((2.0f + 2 * i) * radius) * rate;

			JGeometry::TVec3<f32> local(dist, 0.0f, 0.0f);
			JGeometry::TVec3<f32> pos;
			((TMtx34f*)mtx)->mult(local, pos);
			pos.y = mPosition.y;
			if (height <= 0.0f)
				height = 2.0f * radius;
			mFlames[i]->set(pos, radius, height);
		}
	} else {
		for (int i = 0; i < 10; ++i)
			mFlames[i]->remove();
	}
}

// TODO: the body of breathFlame() is pasted here because our compiler refuses
// to inline it (it contains a loop); the original inlined it
#pragma dont_inline on
void TLimitKoopa::setUpHitActors()
{
	MtxPtr mtx = getMActor()->getModel()->getAnmMtx(mHeadJointIdx);
	if (isBreathing()) {
		f32 rate = 1.0f;
		if (getAnmIndex() == 5) {
			J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(0);
			rate = (ctrl->getFrame() - 125.0f) / (ctrl->getEnd() - 125.0f);
		}

		for (int i = 0; i < 10; ++i) {
			f32 height = getParam()->flameHeight.get();
			f32 radius = getParam()->flameRadius.get();
			f32 dist   = 0.8f * ((2.0f + 2 * i) * radius) * rate;

			JGeometry::TVec3<f32> local(dist, 0.0f, 0.0f);
			JGeometry::TVec3<f32> pos;
			((TMtx34f*)mtx)->mult(local, pos);
			pos.y = mPosition.y;
			if (height <= 0.0f)
				height = 2.0f * radius;
			mFlames[i]->set(pos, radius, height);
		}
	} else {
		for (int i = 0; i < 10; ++i)
			mFlames[i]->remove();
	}

	MtxPtr agoMtx = getMActor()->getModel()->getAnmMtx(mAgoJointIdx);
	f32 radius    = getParam()->headRadius.get();
	JGeometry::TVec3<f32> headPos(agoMtx[0][3], agoMtx[1][3] - 200.0f,
	                              agoMtx[2][3]);
	mHead->set(headPos, radius, 2.0f * radius);
}
#pragma dont_inline off

void TLimitKoopa::setAnimationIndex(int index)
{
	MActor* actor = getMActor();
	if (!actor->checkCurBckFromIndex(index))
		actor->setBckFromIndex(index);
}

void TLimitKoopa::changeBck(int bck, f32 speed)
{
	setAnimationIndex(bck);
	getMActor()->getFrameCtrl(0)->setRate(speed);
}

int TLimitKoopa::getAnmIndex() const { return getMActor()->getCurAnmIdx(0); }

bool TLimitKoopa::endsAnm() const
{
	return getMActor()->curAnmEndsNext(0, nullptr) ? true : false;
}

bool TLimitKoopa::isBreathing() const
{
	if (getAnmIndex() == 4)
		return true;
	if (getAnmIndex() == 5 && getAnmFrame() >= 127.0f)
		return true;
	return false;
}

f32 TLimitKoopa::getFlameDir() const
{
	// TODO: UNUSED, original body unknown
	return 0.0f;
}

bool TLimitKoopa::isFlaming() const
{
	switch (getAnmIndex()) {
	case 3:
	case 4:
	case 5:
		return true;
	}
	return false;
}

f32 TLimitKoopa::getNeckFocus() const
{
	// TODO: UNUSED, original body unknown
	return 1.0f;
}

// fabricated: the parts' receiveMessage calls getShowered() with
// TSpineBase::pushNerve out of line, which needs one more inline level than a
// direct mSpine->pushNerve() (as in Koopa.cpp)
inline void TLimitKoopa::pushNerve(const TNerveBase<TLiveActor>* nerve)
{
	mSpine->pushNerve(nerve);
}

void TLimitKoopa::getDown()
{
	// TODO: UNUSED, body guessed from TKoopa::getDown
	if (&TNerveLimitKoopaTumble::theNerve() == mSpine->getCurrentNerve())
		return;

	if (&TNerveLimitKoopaStagger::theNerve() == mSpine->getCurrentNerve())
		mSpine->setNext(&TNerveLimitKoopaGetDown::theNerve());

	if (&TNerveLimitKoopaGetShowered::theNerve() == mSpine->getCurrentNerve())
		mSpine->setNext(&TNerveLimitKoopaGetDown::theNerve());

	pushNerve(&TNerveLimitKoopaGetDown::theNerve());
}

bool TLimitKoopa::getShowered()
{
	if (&TNerveLimitKoopaTumble::theNerve() == mSpine->getCurrentNerve())
		return false;
	if (&TNerveLimitKoopaGetDown::theNerve() == mSpine->getCurrentNerve())
		return false;

	if (&TNerveLimitKoopaStagger::theNerve() == mSpine->getCurrentNerve())
		mSpine->setNext(&TNerveLimitKoopaGetShowered::theNerve());

	pushNerve(&TNerveLimitKoopaGetShowered::theNerve());
	return true;
}

void TLimitKoopa::stagger(bool force)
{
	// TODO: UNUSED, body guessed from TKoopa::stagger
	if (&TNerveLimitKoopaTumble::theNerve() == mSpine->getCurrentNerve())
		return;
	if (&TNerveLimitKoopaGetDown::theNerve() == mSpine->getCurrentNerve())
		return;
	if (&TNerveLimitKoopaGetShowered::theNerve() == mSpine->getCurrentNerve())
		return;

	pushNerve(&TNerveLimitKoopaStagger::theNerve());
}

bool TLimitKoopa::isTumbling() const { return false; }

f32 TLimitKoopa::getAnmFrame() const
{
	return getMActor()->getFrameCtrl(0)->getFrame();
}

void TLimitKoopa::fall()
{
	// TODO: UNUSED, body guessed from TKoopa::fall
	mRotation.y = 180.0f;
	mSpine->setNext(&TNerveLimitKoopaFall::theNerve());
}

f32 TLimitKoopa::getAnmEnd() const
{
	return getMActor()->getFrameCtrl(0)->getEnd();
}

MtxPtr TLimitKoopa::getHeadMtx() const
{
	return getMActor()->getModel()->getAnmMtx(mHeadJointIdx);
}

bool TLimitKoopa::checkMarioWhichSide()
{
	// TODO: UNUSED, original body unknown
	return false;
}

void TLimitKoopa::updateTimers()
{
	for (int i = 0; i < 3; i++)
		if (unk150[i] > 0)
			unk150[i]--;
}

TLimitKoopaParams* TLimitKoopa::getParam() const
{
	return (TLimitKoopaParams*)((TEnemyManager*)mManager)->unk38;
}

TLimitKoopaManager::TLimitKoopaManager(const char* name)
    : TEnemyManager(name)
{
}

TSpineEnemy* TLimitKoopaManager::createEnemyInstance() { return nullptr; }

void TLimitKoopaManager::createModelData()
{
	static TModelDataLoadEntry entry[] = {
		{ "koopa_model.bmd", 0x14240000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

void TLimitKoopaManager::load(JSUMemoryInputStream& stream)
{
	TEnemyManager::load(stream);
	unk38 = new TLimitKoopaParams("/enemy/limitkoopa.prm");
}

void TLimitKoopaManager::loadAfter()
{
	TEnemyManager::loadAfter();
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_fire_a.jpa", 0x1C0);
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_fire_b.jpa", 0x1C1);
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_fire_c.jpa", 0x1C2);
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_fire_d.jpa", 0x1C3);
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_fire_e.jpa", 0x1F3);
}
