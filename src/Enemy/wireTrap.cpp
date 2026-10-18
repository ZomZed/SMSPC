#include <Enemy/WireTrap.hpp>
#include <Enemy/Launcher.hpp>
#include <Enemy/WireBinder.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JMath.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <JSystem/JSupport/JSUInputStream.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <M3DUtil/MActor.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <MSound/SoundEffects.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/MtxUtil.hpp>
#include <MarioUtil/PacketUtil.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/LiveManager.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/Spine.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/Particles.hpp>
#include <math.h>

// rogue includes needed for matching sinit & bss
#include <M3DUtil/InfectiousStrings.hpp>
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>

namespace {

// UNUSED
static void SMSReportVec(const char*, const JGeometry::TVec3<f32>&) { }

const char cMatName[]       = "_mat_1";
const GXColorS10 cRedColor  = { 0xD2, 0x14, 0x0F, 0x00 };
const GXColorS10 cBlueColor = { 0x0F, 0x14, 0xD2, 0x00 };

} // namespace

TWireTrap::TWireTrap(const char* name)
    : TSpineEnemy(name)
    , unk160(0)
    , mWaterHitTimer(0)
{
}

void TWireTrap::init(TLiveManager* manager)
{
	mManager = manager;
	mManager->manageActor(this);
	mMActorKeeper = new TMActorKeeper(mManager, 1);
	mMActor       = mMActorKeeper->createMActor("wire_trap.bmd", 0);
	mBinder       = new TWireBinder;
	mSpine->initWith(&TNerveWaitForever<TLiveActor>::theNerve());
	initCollision();
	initParticle();
}

// fabricated: keeps TVec3::set<f> out of line like the ROM
static inline void WireSetVec2(JGeometry::TVec3<f32>& v, f32 x, f32 y, f32 z)
{
	v.set(x, y, z);
}
static inline void WireSetVec1(JGeometry::TVec3<f32>& v, f32 x, f32 y, f32 z)
{
	WireSetVec2(v, x, y, z);
}
static inline void WireSetVec(JGeometry::TVec3<f32>& v, f32 x, f32 y, f32 z)
{
	WireSetVec1(v, x, y, z);
}

static inline void WireSetDir(JGeometry::TVec3<f32>& v, f32 len, s16 angle)
{
	WireSetVec(v, len * JMASSin(angle), 0.0f, len * JMASCos(angle));
}

void TWireTrap::load(JSUMemoryInputStream& stream)
{
	TSpineEnemy::load(stream);

	s32 speed;
	s32 mode;
	s32 color;
	stream.read(&speed, 4);
	stream.read(&mode, 4);
	stream.read(&mWaitTime, 4);
	stream.read(&color, 4);
#ifdef SMS_PORT
	// raw reads of big-endian scene data
	sms_swap32_inplace(&speed);
	sms_swap32_inplace(&mode);
	sms_swap32_inplace(&mWaitTime);
	sms_swap32_inplace(&color);
#endif

	mSpeed = speed / 10.0f;

	if (mode == -1)
		mode = 0;
	mMode = mode;

	if (color == -1)
		initThisColor(&cRedColor);
	else
		initThisColor(&cBlueColor);

	initWire();
}

void TWireTrap::initCollision()
{
	initHitActor(ACTOR_TYPE_ENEMY_UNK26, 2, 0x90000000, 20.0f, 30.0f, 40.0f,
	             40.0f);
	offHitFilter(HIT_FILTER_NO_COLLISION);
}

void TWireTrap::initParticle()
{
	SMS_LoadParticle("/scene/wireTrap/jpa/ms_wrt_biri_a.jpa",
	                 WIRETRAP_JPA_MS_WRT_BIRI_A);
	SMS_LoadParticle("/scene/wireTrap/jpa/ms_wrt_biri_b.jpa",
	                 WIRETRAP_JPA_MS_WRT_BIRI_B);
}

void TWireTrap::initWire()
{
	getWireBinder()->init(mPosition);
	setMoveMode(mMode);

	const JGeometry::TVec3<f32>& wireDir = getWireDir();
	s16 angle                            = DEG2SHORTANGLE(mRotation.y);

	// TODO: the target calls TVec3::set<f32> out of line here with
	// `1.0f * sin` and `1.0f * cos` as arguments, which suggests one more
	// inline level (with a length parameter) that is still missing.
	JGeometry::TVec3<f32> facing;
	WireSetDir(facing, 1.0f, angle);

	mDirection = 0.0f <= facing.dot(wireDir) ? 1.0f : -1.0f;
	mScale     = 1.0f;
}

void TWireTrap::initThisColor(const GXColorS10* color)
{
	s32 index
	    = getModel()->getModelData()->getMaterialName()->getIndex(cMatName);
	SMS_InitPacket_OneTevColor(getModel(), index, GX_TEVREG2, color);
}

BOOL TWireTrap::receiveMessage(THitActor* sender, u32 message)
{
	switch (message) {
	case HIT_MESSAGE_SPRAYED_BY_WATER:
		behaveHitWater(sender);
		return true;

	case HIT_MESSAGE_TAKE:
		if (mHolder == nullptr) {
			onHitFilter(HIT_FILTER_NO_COLLISION);
			mHolder = (TTakeActor*)sender;
			return true;
		}
		break;

	case HIT_MESSAGE_THROWN:
	case HIT_MESSAGE_UNK8:
		if (mHolder != nullptr) {
			mHolder = nullptr;
			return true;
		}
		break;

	case HIT_MESSAGE_UNKB:
		kill();
		return true;
	}

	return TSpineEnemy::receiveMessage(sender, message);
}

void TWireTrap::behaveHitWater(THitActor* sender)
{
	SMS_EasyEmitParticle(PARTICLE_MS_ENM_WATHIT, &mPosition, nullptr,
	                     JGeometry::TVec3<f32>(1.0f, 1.0f, 1.0f));
	gpMSound->startSoundSet(MSD_SE_EN_COMMON_W_HIT_OK, &mPosition, 0, 0.0f, 0,
	                        0, 4);
	mWaterHitTimer = 30;

	JGeometry::TVec3<f32> fromMario = mPosition;
	fromMario -= *gpMarioPos;

	JGeometry::TVec3<f32> dir = getWireDir();
	dir.scale(mDirection);

	if (0.0f <= fromMario.dot(dir))
		mWaterPower = getParams()->mInWaterPowerRate.get();
	else
		mWaterPower = -getParams()->mInWaterPowerRate.get();
}

void TWireTrap::kill()
{
	if (checkLiveFlag(LIVE_FLAG_DEAD))
		return;

	onLiveFlag(LIVE_FLAG_DEAD);
	onHitFilter(HIT_FILTER_NO_COLLISION);
	mSpine->reset();
	mSpine->setNext(&TNerveWaitForever<TLiveActor>::theNerve());
	SMS_EasyEmitParticle(PARTICLE_MS_ENM_DISAP_A, &mPosition, nullptr,
	                     JGeometry::TVec3<f32>(1.0f, 1.0f, 1.0f));
	SMS_EasyEmitParticle(PARTICLE_MS_ENM_DISAP_B, &mPosition, nullptr,
	                     JGeometry::TVec3<f32>(1.0f, 1.0f, 1.0f));
}

static inline JGeometry::TVec3<f32> WTCalcMomentum2(TWireTrap* trap)
{
	return trap->calcMomentum();
}
static inline JGeometry::TVec3<f32> WTCalcMomentum1(TWireTrap* trap)
{
	return trap->calcMomentum();
}

void TWireTrap::behaveHitWireTrap(TWireTrap* other,
                                  const JGeometry::TVec3<f32>& my_momentum,
                                  const JGeometry::TVec3<f32>& other_momentum)
{
	if (getHitTimer() > 0 || mMode != WIRE_TRAP_MODE_RETURN)
		return;

	mHitTimer = 30;

	// NOTE: the results of these are discarded
	WTCalcMomentum1(this);
	WTCalcMomentum1(other);

	if (my_momentum.dot(other_momentum) < 0.0f)
		mDirection *= -1.0f;

	mSpine->pushNerve(&TNerveWireTrapWait::theNerve());
}

// fabricated
static inline f32 WrapAngle(f32 value, f32 min, f32 range)
{
	return min + std::fmodf(range + (value - min), range);
}

// Local copy of TQuat4::mul with the statement order the target schedules.
static inline void wireQuatMul(JGeometry::TQuat4<f32>& q,
                               const JGeometry::TQuat4<f32>& o)
{
	// clang-format off
	f32 _y = q.y * o.w + q.w * o.y + q.z * o.x - q.x * o.z;
	f32 _x = q.x * o.w + q.w * o.x + q.y * o.z - q.z * o.y;
	f32 _z = q.z * o.w + q.w * o.z + q.x * o.y - q.y * o.x;
	f32 _w = q.w * o.w - q.x * o.x - q.y * o.y - q.z * o.z;
	// clang-format on
	q.set(_x, _y, _z, _w);
}

void TWireTrap::calcRootMatrix()
{
	if (mHolder != nullptr) {
		TSpineEnemy::calcRootMatrix();
		return;
	}
	JGeometry::TVec3<f32> dir;

	emitEffects();

	TPosition3f mtx;
	JGeometry::TQuat4<f32> spinQuat;
	JGeometry::TQuat4<f32> dirQuat;
	dir = getDirAtWirePos();
	dir.scale(mDirection);
	SMS_CalcToDirMatrix(mtx, dir, JGeometry::TVec3<f32>(0.0f, 1.0f, 0.0f));
	mtx.getQuat(dirQuat);

	// TODO: the target calls an out-of-line std::fmodf (weak, 0x5C bytes) whose
	// body differs from our MSL header's (fabs check + long long truncation).
	mRotation.z += -17.75f;
	mRotation.z = WrapAngle(mRotation.z, 0.0f, 360.0f);

	spinQuat.setRotate(JGeometry::TVec3<f32>(0.0f, 0.0f, 1.0f),
	                   0.017453294f * mRotation.z);
	wireQuatMul(dirQuat, spinQuat);

	mtx.setQuat(dirQuat);
	mtx.setTrans(mPosition);
	getModel()->setBaseTRMtx(mtx);

	JGeometry::TVec3<f32> scale = mScaling;
	scale.scale(mScale);
	getModel()->setBaseScale(scale);
}

void TWireTrap::moveObject()
{
	if (getWaterHitTimer() > 0) {
		--mWaterHitTimer;
		if (mWaterHitTimer <= 0)
			mWaterPower = 0.0f;
	}

	updateCollision();
	checkHitActors();
	TLiveActor::moveObject();
	SMSGetMSound()->startSoundActor(MSD_SE_EN_WIRETRAP_WAIT, &mPosition, 0,
	                                nullptr, 0, 4);
}

JGeometry::TVec3<f32> TWireTrap::calcMomentum()
{
	JGeometry::TVec3<f32> momentum = getWireDir();
	f32 rate = mWaterHitTimer > 0 ? 1.0f + getWaterPow() : 1.0f;
	momentum *= mDirection * rate;
	momentum *= mSpeed;
	return momentum;
}

f32 TWireTrap::getWaterPow() const
{
	return mWaterPower * mWaterHitTimer / 30.0f;
}

bool TWireTrap::doReturnMove()
{
	if (getHitTimer() > 0)
		--mHitTimer;

	if (isEndWire()) {
		mDirection *= -1.0f;
		return true;
	}

	mPositionDelta = calcMomentum();
	return false;
}

bool TWireTrap::doOnewayMove()
{
	if (getHitTimer() > 0)
		--mHitTimer;

	if (isEndWire())
		return true;

	mPositionDelta = calcMomentum();
	return false;
}

bool TWireTrap::doSearchMove()
{
	if (!SMS_IsMarioOnWire())
		return false;

	if (getHitTimer() > 0)
		--mHitTimer;

	JGeometry::TVec3<f32> toMario = *gpMarioPos;
	toMario -= mPosition;

	f32 dot = toMario.dot(getWireDir());
	mDirection = dot > 0.0f ? 1 : dot < 0.0f ? -1 : 0;

	mPositionDelta = calcMomentum();

	// TODO: this never reports being done, so the search nerve never
	// transitions into its wait nerve. Kept as in the original.
	return false;
}

bool TWireTrap::doScaleUp()
{
	onHitFilter(HIT_FILTER_NO_COLLISION);
	mScale += 1.0f / getParams()->mScaleTimerMax.get();
	if (1.0f <= mScale) {
		mScale = 1.0f;
		offHitFilter(HIT_FILTER_NO_COLLISION);
		return true;
	}
	return false;
}

bool TWireTrap::doScaleDown()
{
	onHitFilter(HIT_FILTER_NO_COLLISION);
	mScale -= 1.0f / getParams()->mScaleTimerMax.get();
	if (mScale <= 0.0f) {
		mScale = 0.0f;
		offHitFilter(HIT_FILTER_NO_COLLISION);
		return true;
	}
	return false;
}

void TWireTrap::doResetToEdge()
{
	f32 startPos = 0.0f < mDirection ? 0.0f : 1.0f;
	getWireBinder()->getPoint(&mPosition, startPos + 0.01f * mDirection);
}

void TWireTrap::checkHitActors()
{
	THitActor** end;
	THitActor** it;

	it  = mCollisions;
	end = mCollisions + mColCount;
	for (; it != end; ++it) {
		switch ((*it)->getActorType()) {
		case ACTOR_TYPE_MARIO:
			SMS_SendMessageToMario(this, HIT_MESSAGE_UNK9);
			break;

		case ACTOR_TYPE_ENEMY_UNK26:
			if (*it != this) {
				TWireTrap* other = (TWireTrap*)*it;

				JGeometry::TVec3<f32> myMomentum    = calcMomentum();
				JGeometry::TVec3<f32> otherMomentum = other->calcMomentum();

				behaveHitWireTrap(other, myMomentum, otherMomentum);
				// TODO: passes `other` to itself rather than `this`, the
				// target asm computes other's momentum twice in here.
				other->behaveHitWireTrap(other, otherMomentum, myMomentum);
			}
			break;
		}
	}
}

void TWireTrap::updateCollision()
{
	setHitParams(20.0f * mScaling.x, 30.0f * mScaling.y, 40.0f * mScaling.x,
	             40.0f * mScaling.y);
}

void TWireTrap::emitEffects()
{
	if (JPABaseEmitter* emitter = gpMarioParticleManager->emitAndBindToPosPtr(
	        WIRETRAP_JPA_MS_WRT_BIRI_A, &mPosition, 1, this))
		emitter->setGlobalScale(mScaling);

	if (JPABaseEmitter* emitter = gpMarioParticleManager->emitAndBindToPosPtr(
	        WIRETRAP_JPA_MS_WRT_BIRI_B, &mPosition, 1, this))
		emitter->setGlobalScale(mScaling);
}

void TWireTrap::setMoveMode(int mode)
{
	mSpine->reset();
	mSpine->setNext(getNerveFromMode(mode));
}

JGeometry::TVec3<f32> TWireTrap::getDirAtWirePos() const
{
	return getWireBinder()->getDirAtPos(mPosition, mDirection);
}

const JGeometry::TVec3<f32>& TWireTrap::getWireDir() const
{
	return getWireBinder()->getDir();
}

// UNUSED, TODO: 0x14 bytes in the map, not inlined anywhere
void TWireTrap::isReflect() const { }

const TNerveBase<TLiveActor>* TWireTrap::getNerveFromMode(int mode)
{
	switch (mode) {
	case WIRE_TRAP_MODE_RETURN:
		return &TNerveWireTrapReturnMove::theNerve();
	case WIRE_TRAP_MODE_ONEWAY:
		return &TNerveWireTrapOnewayMove::theNerve();
	case WIRE_TRAP_MODE_SEARCH:
		return &TNerveWireTrapSearch::theNerve();
	}
	return nullptr;
}

// UNUSED, TODO: 0x2C bytes in the map, not inlined anywhere. Probably
// returns getWireBinder()->getRangePos(mPosition).
void TWireTrap::getRangePosInWire() const { }

TWireBinder* TWireTrap::getWireBinder() { return (TWireBinder*)mBinder; }

TWireBinder* TWireTrap::getWireBinder() const { return (TWireBinder*)mBinder; }

// UNUSED, TODO: 0x30 bytes in the map (same as isEndWire), not inlined
// anywhere. Probably wraps TWireBinder::isStartWire like isEndWire does.
void TWireTrap::isStartWire() const { }

bool TWireTrap::isEndWire() const
{
	return getWireBinder()->isEndWire(mPosition, mDirection);
}

TWireTrapParams::TWireTrapParams(const char* path)
    : TSpineEnemyParams(path)
    , PARAM_INIT(mInWaterPowerRate, 0.9f)
    , PARAM_INIT(mScaleTimerMax, 60)
    , PARAM_INIT(mGoTimerMax, 90)
{
	TParams::load(mPrmPath);
}

TWireTrapManager::TWireTrapManager(const char* name)
    : TEnemyManager(name)
{
}

void TWireTrapManager::load(JSUMemoryInputStream& stream)
{
	unk38 = new TWireTrapParams("/enemy/wiretrap.prm");
	TEnemyManager::load(stream);
}

void TWireTrapManager::createModelData()
{
	static const TModelDataLoadEntry entry[] = {
		{ "wire_trap.bmd", 0x10210000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

DEFINE_NERVE(TNerveWireTrapReturnMove, TLiveActor)
{
	TWireTrap* self = (TWireTrap*)spine->getBody();
	if (self->doReturnMove()) {
		spine->pushAfterCurrent(this);
		spine->pushAfterCurrent(&TNerveWireTrapWait::theNerve());
		return true;
	}
	return false;
}

DEFINE_NERVE(TNerveWireTrapOnewayMoveStart, TLiveActor)
{
	TWireTrap* self = (TWireTrap*)spine->getBody();
	if (self->doScaleUp())
		return true;
	return false;
}

DEFINE_NERVE(TNerveWireTrapOnewayMove, TLiveActor)
{
	TWireTrap* self = (TWireTrap*)spine->getBody();
	if (spine->getTime() == 0 && self->mScale < 1.0f) {
		spine->pushAfterCurrent(this);
		spine->pushAfterCurrent(&TNerveWireTrapGoWait::theNerve());
		spine->pushAfterCurrent(&TNerveWireTrapOnewayMoveStart::theNerve());
		return true;
	}

	if (self->doOnewayMove()) {
		spine->pushAfterCurrent(this);
		spine->pushAfterCurrent(&TNerveWireTrapWait::theNerve());
		spine->pushAfterCurrent(&TNerveWireTrapOnewayMoveEnd::theNerve());
		return true;
	}
	return false;
}

DEFINE_NERVE(TNerveWireTrapOnewayMoveEnd, TLiveActor)
{
	TWireTrap* self = (TWireTrap*)spine->getBody();
	if (self->doScaleDown()) {
		self->doResetToEdge();
		return true;
	}
	return false;
}

DEFINE_NERVE(TNerveWireTrapSearch, TLiveActor)
{
	TWireTrap* self = (TWireTrap*)spine->getBody();
	if (self->doSearchMove()) {
		spine->pushAfterCurrent(this);
		spine->pushAfterCurrent(&TNerveWireTrapWait::theNerve());
		return true;
	}
	return false;
}

DEFINE_NERVE(TNerveWireTrapWait, TLiveActor)
{
	TWireTrap* self = (TWireTrap*)spine->getBody();
	if (self->mWaitTime < spine->getTime())
		return true;
	return false;
}

DEFINE_NERVE(TNerveWireTrapGoWait, TLiveActor)
{
	TWireTrap* self = (TWireTrap*)spine->getBody();
	if (self->getParams()->mGoTimerMax.get() < spine->getTime())
		return true;
	return false;
}

// link-all-source support: the original game has no libm fmod; its weak
// std::fmodf (formerly emitted from this unit, 0x5C bytes) computes
// |y| > |x| ? x : x - y * (f32)(s64)(x / y) in single precision. Our MSL header's
// std::fmodf calls ::fmod, so provide exactly that behaviour here and force an
// out-of-line std::fmodf (the original Hanachan objects call it).
extern "C" double fmod(double x, double y)
{
	f32 fx = (f32)x;
	f32 fy = (f32)y;
	if (fabs((double)fy) > fabs((double)fx))
		return fx;
	return fx - fy * (f32)(s64)(fx / fy);
}
// std::fmodf is declared out of line in math.h; the original is a weak copy.
// (The PC port uses the host C library's std::fmodf.)
#ifndef SMS_PORT
namespace std {
#pragma dont_inline on
float fmodf(float x, float y)
{
	if (::fabs(y) > ::fabs(x))
		return x;
	return x - y * (s64)(x / y);
}
#pragma dont_inline off
} // namespace std
#endif
