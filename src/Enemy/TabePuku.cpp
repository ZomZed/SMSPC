#include <Enemy/TabePuku.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/Enemy.hpp>
#include <Enemy/EnemyManager.hpp>
#include <Enemy/Graph.hpp>
#include <Enemy/SmallEnemy.hpp>
#include <JSystem/JDrama/JDRActor.hpp>
#include <JSystem/JDrama/JDRNameRef.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JDrama/JDRViewObjPtrList.hpp>
#include <JSystem/JGeometry.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <JSystem/JSupport/JSUInputStream.hpp>
#include <JSystem/JSupport/JSUList.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <M3DUtil/MActor.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <Map/Map.hpp>
#include <Map/MapCollisionData.hpp>
#include <Map/MapData.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/ObjManager.hpp>
#include <Strategic/Spine.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/MarDirector.hpp>
#include <System/ParamInst.hpp>
#include <System/Params.hpp>
#include <System/Particles.hpp>
#include <stdlib.h>

// rogue includes needed for matching sinit & bss
#include <M3DUtil/InfectiousStrings.hpp>
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>

namespace {
// NOTE: unused, only ever written to by the static initializer
f32 cAngleMax = JGeometry::TUtil<f32>::PI() / 8.0f;
} // namespace

TTabePukuParams::TTabePukuParams(const char* path)
    : TSmallEnemyParams(path)
    , PARAM_INIT(mMarchSpeed, 0.15f)
    , PARAM_INIT(mAttackSpeed, 0.22f)
    , PARAM_INIT(mDiveSpeed, 0.4f)
    , PARAM_INIT(mWaterFric, 0.95f)
    , PARAM_INIT(mTurnSlerpRate, 0.05f)
    , PARAM_INIT(mApartHeight, 500.0f)
    , PARAM_INIT(mCorrectY, -40.0f)
    , PARAM_INIT(mCorrectZ, 150.0f)
    , PARAM_INIT(mTerritoryRange, 1000.0f)
    , PARAM_INIT(mDragLength, 2500.0f)
{
	TParams::load(mPrmPath);
}

TTPHitActor::TTPHitActor(TTabePuku& owner)
    : THitActor("タベプク用当たり")
    , mOwner(&owner)
{
}

void TTPHitActor::init()
{
	initHitActor(ACTOR_TYPE_ENEMY_UNK35, 1, 0x80000000, 10.0f, 10.0f, 10.0f,
	             10.0f);
	offHitFilter(HIT_FILTER_NO_COLLISION);
	onHitFilter(HIT_FILTER_NO_DAMAGE);
	JDrama::TViewObjPtrListT<JDrama::TViewObj>* grp
	    = static_cast<JDrama::TViewObjPtrListT<JDrama::TViewObj>*>(
	        JDrama::TNameRefGen::search("敵グループ"));
	grp->getChildren().push_back(this);
}

BOOL TTPHitActor::receiveMessage(THitActor* sender, u32 message)
{
	return mOwner->receiveMessage(sender, message);
}

void TTPHitActor::updateObjCollision()
{
	setHitParams(mOwner->getParams()->getSLAttackRadius(),
	             mOwner->getParams()->getSLAttackHeight(),
	             mOwner->getParams()->getSLDamageRadius(),
	             mOwner->getParams()->getSLDamageHeight());
}

// TODO: instruction scheduling differs, the target computes and stores the new
// position one component at a time.
void TTPHitActor::updateTerrainCollsion()
{
	JGeometry::TVec3<f32> delta;
	JGeometry::TQuat4<f32> quat = mOwner->mQuat;

	JGeometry::TVec3<f32> up;
	quat.getYDir(up);
	JGeometry::TVec3<f32> center;

	f32 depth = 0.6666667f * mAttackHeight;
	unk78     = mAttackHeight;
	unk7C     = mAttackRadius;

	if (mOwner->isCatching()) {
		depth += mOwner->getHeldObject()->mDamageHeight;
		unk78 += mOwner->getHeldObject()->mDamageHeight;
		unk7C += mOwner->getHeldObject()->mDamageRadius;
	}
	center.scaleAdd(0.5f * mAttackHeight, mOwner->mPosition, up);
	JGeometry::TVec3<f32> pos;
	pos.scaleAdd(depth, center, JGeometry::TVec3<f32>(0.0f, -1.0f, 0.0f));

	delta = pos;
	delta -= mPosition;
	unk6C     = delta;
	mPosition = pos;
}

void TTPHitActor::bind()
{
	JGeometry::TVec3<f32> pos = mPosition;
	pos += unk6C;
	pos += JGeometry::TVec3<f32>(mOwner->getVelocity());
	pos += mOwner->mPositionDelta;

	unk80 = gpMap->checkGroundIgnoreWaterSurface(pos.x, pos.y + unk78, pos.z,
	                                             &unk84);
	unk80 += 1.0f;
	if (pos.y <= 0.05f + unk80) {
		unk88                             = 0;
		const JGeometry::TVec3<f32>& normal = unk84->getNormal();
		JGeometry::TVec3<f32> ground(pos.x, unk80, pos.z);
		f32 push = 1.0f - (normal.dot(pos) - normal.dot(ground));
		if (push > 0.0f) {
			pos.x += normal.x * push;
			pos.y += normal.y * push;
			pos.z += normal.z * push;
		}
		pos.y = unk80;
	} else {
		unk88 = 1;
	}

	if (0.0f <= pos.y + unk78)
		pos.y = -unk78;

	TBGWallCheckRecord record(pos.x, pos.y, pos.z, unk7C, 1, 0);
	bool touched = gpMap->isTouchedWallsAndMoveXZ(&record);
	pos.x        = record.mCenter.x;
	pos.z        = record.mCenter.z;

	unk6C     = pos - mPosition;
	unk89     = touched;
	mPosition = pos;
}

void TTPHitActor::checkHitActors()
{
	THitActor** end = mCollisions + mColCount;
	for (THitActor** it = mCollisions; it != end; ++it) {
		switch ((*it)->getActorType()) {
		case ACTOR_TYPE_MARIO:
			mOwner->attackToMario();
			break;
		}
	}
}

TTabePuku::TTabePuku(const char* name)
    : TSmallEnemy(name)
{
	onLiveFlag(LIVE_FLAG_UNK1000);
}

void TTabePuku::init(TLiveManager* manager)
{
	mManager = manager;
	mManager->manageActor(this);
	setMActorAndKeeper();
	mSpine->initWith(&TNerveTabePukuGraphWander::theNerve());
	initCollision();
	initParams();
	initAnmSound();
}

void TTabePuku::reset() { mScaledBodyRadius = 130.0f; }

void TTabePuku::initCollision()
{
	initHitActor(ACTOR_TYPE_ENEMY_UNK35, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
	onHitFilter(HIT_FILTER_NO_COLLISION);
	mHitActor = new TTPHitActor(*this);
	mHitActor->init();
	mHitActor->mPosition = mPosition;
}

void TTabePuku::initParams()
{
	mQuat.set(SMS_Eular2Quat(mRotation));
	mMouthJointIdx = (u16)getModel()->getModelData()->getJointName()->getIndex(
	    "jnt_mouth_up");
}

void TTabePuku::perform(u32 cue, JDrama::TGraphics* graphics)
{
	mHitActor->perform(cue, graphics);
	TSmallEnemy::perform(cue, graphics);
}

void TTabePuku::control()
{
	TLiveActor::control();
	mHitActor->checkHitActors();
	updateSound();
}

void TTabePuku::updateSound()
{
	if (isBiting())
		SMSGetMSound()->startSoundActor(MSD_SE_EN_TOBIPUKU_CHEW, &mPosition, 0,
		                                nullptr, 0, 4);
}

void TTabePuku::bind()
{
	mHitActor->updateObjCollision();
	mHitActor->updateTerrainCollsion();
	mHitActor->bind();
	mPositionDelta = mHitActor->unk6C;
	unk1DC         = mHitActor->unk89;
	if (mHitActor->isAirborne())
		onLiveFlag(LIVE_FLAG_AIRBORNE);
	else
		offLiveFlag(LIVE_FLAG_AIRBORNE);
	mGroundPlane  = mHitActor->unk84;
	mGroundHeight = mHitActor->unk80;
}

void TTabePuku::calcRootMatrix()
{
	if (isTaken()) {
		TSpineEnemy::calcRootMatrix();
		return;
	}

	TPosition3f mtx;
	mtx.setQT(mQuat, mPosition);
	getModel()->setBaseScale(mScaling);
	MtxPtr m = mtx;
	J3DModel* model = getModel();
	MTXCopy(m, model->getBaseTRMtx());
	emitEffects();
}

void TTabePuku::emitEffects()
{
	JPABaseEmitter* emitter = SMS_EasyEmitParticle(
	    PARTICLE_MS_PUKU_AWA, getModel()->getAnmMtx(mMouthJointIdx), this,
	    JGeometry::TVec3<f32>(1.0f, 1.0f, 1.0f));
	if (emitter) {
		f32 depth = -mPosition.y / 100.0f;
		if (depth <= 0.0f)
			depth = 0.0f;
		int lifetime = (int)depth * 20 + 2;
		if (lifetime > 200)
			lifetime = 200;
		emitter->setLifeTime(lifetime);
		if (isAttacking())
			emitter->setRate(0.1f);
	}
}

BOOL TTabePuku::receiveMessage(THitActor* sender, u32 message)
{
	switch (message) {
	case HIT_MESSAGE_TRAMPLE:
	case HIT_MESSAGE_HIP_DROP:
		return false;
	}
	return TSmallEnemy::receiveMessage(sender, message);
}

MtxPtr TTabePuku::getTakingMtx()
{
	mTakingMtx.setQuat(mQuat);

	JGeometry::TVec3<f32> yDir;
	JGeometry::TVec3<f32> zDir;
	mTakingMtx.getZDir(zDir);
	mTakingMtx.getYDir(yDir);

	JGeometry::TVec3<f32> pos;
	pos.scaleAdd(getParams()->mCorrectZ.get(), mPosition, zDir);
	pos.scaleAdd(getParams()->mCorrectY.get(), pos, yDir);
	mTakingMtx.setTrans(pos);
	return mTakingMtx;
}

static const char* tabepuku_bastable[] = {
	"/scene/tabepuku/bas/pukupuku_chase.bas",
	"/scene/tabepuku/bas/pukupuku_search.bas",
	"/scene/tabepuku/bas/pukupuku_swim.bas",
};

const char** TTabePuku::getBasNameTable() const { return tabepuku_bastable; }

void TTabePuku::attackToMario()
{
	if (isBiting())
		return;

	if (isGraphWander())
		return;

	if (SMS_SendMessageToMario(this, HIT_MESSAGE_TAKE)) {
		mHeldObject = (TTakeActor*)SMS_GetMarioHitActor();
		mSpine->reset();
		mSpine->setNext(&TNerveTabePukuBite::theNerve());
	}
}

void TTabePuku::behaveToWater(THitActor*) { }

void TTabePuku::forceKill() { }

bool TTabePuku::isFindMario(f32 param_1)
{
	return isFindMarioFromParam(param_1);
}

bool TTabePuku::doKeepDistance() { return !(isAttacking() || isBiting()); }

static inline f32 TPLengthOutOfLine(const JGeometry::TVec3<f32>& v) { return v.length(); }

bool TTabePuku::isMissMario() const
{
	if (fabsf(SMS_GetMarioPos().y - mPosition.y)
	    > getParams()->getSLGiveUpHeight())
		return true;

	f32 giveUpLength = getParams()->getSLGiveUpLength();
	if (TPLengthOutOfLine(getUnk104().getPoint() - mPosition) > giveUpLength)
		return true;

	JGeometry::TVec3<f32> nearest
	    = unk124->getGraph()->getNearestPosOnGraphLink(mPosition);
	nearest -= mPosition;
	f32 range = getParams()->mTerritoryRange.get();
	if (range * range <= nearest.squared())
		return true;
	return false;
}

bool TTabePuku::isTouchedPlane() const
{
	bool result = true;
	if (isAirborne() && !unk1DC)
		result = false;
	return result;
}

bool TTabePuku::isGraphWander() const
{
	const TNerveBase<TLiveActor>* nerve = mSpine->getLatestNerve();
	return nerve == &TNerveTabePukuGraphWander::theNerve()
	       || nerve == &TNerveTabePukuRecoverGraph::theNerve();
}

bool TTabePuku::isAttacking() const
{
	return mSpine->getLatestNerve() == &TNerveTabePukuAttack::theNerve();
}

bool TTabePuku::isBiting() const
{
	const TNerveBase<TLiveActor>* nerve = mSpine->getLatestNerve();
	return nerve == &TNerveTabePukuBite::theNerve()
	       || nerve == &TNerveTabePukuDive::theNerve()
	       || nerve == &TNerveTabePukuDrag::theNerve();
}

void TTabePuku::swimToCurPathNode(const JGeometry::TVec3<f32>& offset)
{
	JGeometry::TVec3<f32> dir = getUnk104().getPoint();
	dir -= mPosition;
	dir += offset;
	swimTo(dir);
}

void TTabePuku::doBite() { }

void TTabePuku::prepareDive()
{
	mDiveStartY = mPosition.y;
	setBckAnm(2);
	getMActor()->getFrameCtrl(0)->setRate(2.0f * SMSGetAnmFrameRate());
	mMarchSpeed = getParams()->mDiveSpeed.get();
}

bool TTabePuku::doDive()
{
	swimTo(JGeometry::TVec3<f32>(0.0f, mGroundHeight - mPosition.y, 0.0f));
	if (mPosition.y - mDiveStartY < -getParams()->mApartHeight.get()
	    || mPosition.y - mGroundHeight < 200.0f || !isAirborne())
		return true;
	return false;
}

// Pick a random horizontal drag direction. In the target this is one inline
// level deeper than the rest of prepareDrag (TQuat4::rotate is inlined with
// its TVec4 ctor and TVec3::set out of line).
inline void TTabePuku::decideDragDir()
{
	mDragDir.set(0.0f, 0.0f, 1.0f);

	JGeometry::TQuat4<f32> rot;
	rot.setEulerY(MsRandF() * 6.2831855f);
	rot.rotate(mDragDir, mDragDir);
}

void TTabePuku::prepareDrag()
{
	decideDragDir();

	setGoalPath(TPathNode(mPosition));
	mMarchSpeed = getParams()->mDiveSpeed.get();
}

// TODO: in the target the distance check calls TVec3::dot out of line (one
// inline level deeper than here), see also isMissMario.
static inline f32 TPDot1(const JGeometry::TVec3<f32>& a, const JGeometry::TVec3<f32>& b) { return a.dot(b); }
static inline f32 TPDot2(const JGeometry::TVec3<f32>& a, const JGeometry::TVec3<f32>& b) { return TPDot1(a, b); }
static inline f32 TPDragLen(const JGeometry::TVec3<f32>& v) { return JGeometry::TUtil<f32>::sqrt(TPDot2(v, v)); }

bool TTabePuku::doDrag()
{
	swimTo(mDragDir);
	if (unk1DC || !isAirborne()
	    || TPDragLen(getUnk104().getPoint() - mPosition)
	           > getParams()->mDragLength.get()) {
		detach();
		return true;
	}
	return false;
}

void TTabePuku::swimTo(const JGeometry::TVec3<f32>& target)
{
	JGeometry::TVec3<f32> dir = target;
	if (JGeometry::TUtil<f32>::epsilonEquals(0.0f, target.squared(), JGeometry::TUtil<f32>::epsilon())) {
		setMomentumFromQuat();
		return;
	}

	dir.normalize();

	JGeometry::TQuat4<f32> rot;
	JGeometry::TVec3<f32> front(0.0f, 0.0f, 1.0f);
	if (JGeometry::TUtil<f32>::epsilonEquals(-1.0f, front.dot(dir), JGeometry::TUtil<f32>::epsilon()))
		rot.setEulerY(JGeometry::TUtil<f32>::PI());
	else
		rot.setRotate(front, dir, 1.0f);

	mQuat.slerp(rot, getParams()->mTurnSlerpRate.get());
	mQuat.normalize();
	setMomentumFromQuat();
}

void TTabePuku::setMomentumFromQuat()
{
	JGeometry::TVec3<f32> front;
	mQuat.getZDir(front);
	front.scale(mMarchSpeed);

	JGeometry::TVec3<f32> velocity = mVelocity;
	velocity.scale(getParams()->mWaterFric.get());
	velocity += front;
	mVelocity   = velocity;
	mRotation.y = MsGetRotFromZaxisY(velocity);
}

void TTabePuku::detach()
{
	SMS_SendMessageToMario(this, HIT_MESSAGE_UNK8);
	mHeldObject = nullptr;
}

TTabePukuManager::TTabePukuManager(const char* name)
    : TSmallEnemyManager(name)
{
}

void TTabePukuManager::load(JSUMemoryInputStream& stream)
{
	unk38 = new TTabePukuParams("/enemy/tabepuku.prm");
	TSmallEnemyManager::load(stream);
}

void TTabePukuManager::createModelData()
{
	static const TModelDataLoadEntry entry[] = {
		{ "tabepuku.bmd", 0x10210000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

DEFINE_NERVE(TNerveTabePukuGraphWander, TLiveActor)
{
	TTabePuku* self = (TTabePuku*)spine->getBody();
	if (spine->getTime() == 0) {
		self->getTracer()->reset();
		self->goToShortestNextGraphNode();
		self->setBckAnm(2);
		self->mMarchSpeed = self->getParams()->mMarchSpeed.get();
	}

	if (self->isReachedToGoal())
		self->goToRandomNextGraphNode();

	if (self->isFindMario(1.0f)) {
		spine->pushAfterCurrent(&TNerveTabePukuFound::theNerve());
		return true;
	}

	self->swimToCurPathNode(JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f));
	return false;
}

DEFINE_NERVE(TNerveTabePukuFound, TLiveActor)
{
	TTabePuku* self = (TTabePuku*)spine->getBody();
	if (spine->getTime() == 0) {
		self->setBckAnm(1);
		self->mMarchSpeed = 0.0f;
	}

	self->setMomentumFromQuat();

	if (self->checkCurAnmEnd(0)) {
		spine->pushAfterCurrent(&TNerveTabePukuAttack::theNerve());
		return true;
	}
	return false;
}

DEFINE_NERVE(TNerveTabePukuRecoverGraph, TLiveActor)
{
	TTabePuku* self = (TTabePuku*)spine->getBody();
	if (spine->getTime() == 0) {
		self->getTracer()->reset();
		self->getTracer()->reset2();
		self->goToShortestNextGraphNode();
		self->mMarchSpeed = self->getParams()->mMarchSpeed.get();
	}

	if (self->isReachedToGoal()) {
		spine->pushAfterCurrent(&TNerveTabePukuGraphWander::theNerve());
		return true;
	}

	JGeometry::TVec3<f32> offset;
	if (self->isTouchedPlane())
		offset.set(0.0f, 10000.0f, 0.0f);
	else
		offset.zero();
	self->swimToCurPathNode(offset);
	return false;
}

DEFINE_NERVE(TNerveTabePukuAttack, TLiveActor)
{
	TTabePuku* self = (TTabePuku*)spine->getBody();
	if (spine->getTime() == 0) {
		self->setBckAnm(0);
		self->setGoalPath(TPathNode((THitActor*)gpMarioAddress));
		self->mMarchSpeed = self->getParams()->mAttackSpeed.get();
	}

	if (self->isMissMario() || self->unk1DC) {
		spine->pushAfterCurrent(&TNerveTabePukuRecoverGraph::theNerve());
		return true;
	}

	self->swimToCurPathNode(JGeometry::TVec3<f32>(0.0f, 150.0f, 0.0f));
	return false;
}

DEFINE_NERVE(TNerveTabePukuBite, TLiveActor)
{
	TTabePuku* self = (TTabePuku*)spine->getBody();
	self->setBckAnm(2);
	SMSGetMSound()->startSoundActor(MSD_SE_EN_TOBIPUKU_BITE, &self->mPosition,
	                                0, nullptr, 0, 4);
	self->doBite();
	spine->pushAfterCurrent(&TNerveTabePukuDive::theNerve());
	return true;
}

DEFINE_NERVE(TNerveTabePukuDive, TLiveActor)
{
	TTabePuku* self = (TTabePuku*)spine->getBody();
	if (spine->getTime() == 0)
		self->prepareDive();

	if (self->doDive()) {
		spine->pushAfterCurrent(&TNerveTabePukuDrag::theNerve());
		return true;
	}
	return false;
}

DEFINE_NERVE(TNerveTabePukuDrag, TLiveActor)
{
	TTabePuku* self = (TTabePuku*)spine->getBody();
	if (spine->getTime() == 0)
		self->prepareDrag();

	if (self->doDrag()) {
		spine->pushAfterCurrent(&TNerveTabePukuRecoverGraph::theNerve());
		return true;
	}
	return false;
}
