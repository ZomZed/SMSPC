#include <Enemy/Koopa.hpp>
#include <Enemy/EnemyManager.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/MActorAnm.hpp>
#include <MSound/MAnmSound.hpp>
#include <JSystem/JParticle/JPAEmitter.hpp>
#include <MoveBG/MapObjCorona.hpp>
#include <Player/MarioAccess.hpp>
#include <Camera/CameraShake.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/RumbleMgr.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/Particles.hpp>
#include <System/Application.hpp>
#include <Strategic/Spine.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/Strategy.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/JGeometry/JGMatrix34.hpp>
#include <JSystem/JGeometry/JGQuat4.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <math.h>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

// NOTE: this TU is compiled with -inline deferred, so functions are emitted in
// the reverse order of their definitions here.

class TNerveKoopaTurn : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const = 0;

	// fabricated
	static TKoopaParams* paramB(TKoopa* koopa)
	{
		return (TKoopaParams*)((TEnemyManager*)koopa->mManager)
		    ->getSaveParam();
	}
	static TKoopaParams* paramC(TKoopa* koopa) { return paramB(koopa); }
	static TKoopaParams* paramA(TKoopa* koopa) { return paramC(koopa); }
	static f32 modB(f32 v, f32 m) { return JGeometry::TUtil<f32>::mod(v, m); }
	static f32 modA(f32 v, f32 m) { return modB(v, m); }

	// fabricated
	static void turn(TKoopa* koopa, f32 angle)
	{
		if (angle > 0.0f)
			koopa->changeAnm(11, 0, angle * paramA(koopa)->turnAnim.get());
		else
			koopa->changeAnm(10, 0, -angle * paramA(koopa)->turnAnim.get());

		koopa->mRotation.y
		    = -180.0f
		      + modA(360.0f + (koopa->mRotation.y + angle - -180.0f), 360.0f);
	}
};

class TNerveKoopaTurnR : public TNerveKoopaTurn {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const
	{
		TKoopa* koopa = (TKoopa*)spine->getBody();
		f32 diff
		    = -180.0f
		      + std::fmodf(
		          360.0f + (koopa->mTargetDir - koopa->mRotation.y - -180.0f),
		          360.0f);

		bool turning;
		if (diff > koopa->getParam()->turnSpeed.get()) {
			turn(koopa, paramA(koopa)->turnSpeed.get());
			turning = true;
		} else if (diff > 0.0f) {
			turn(koopa, diff);
			turning = true;
		} else {
			turning = false;
		}

		if (turning)
			return false;

		return true;
	}

	static const TNerveKoopaTurnR& theNerve()
	{
		static TNerveKoopaTurnR nerve;
		return nerve;
	}
};

class TNerveKoopaTurnL : public TNerveKoopaTurn {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const
	{
		TKoopa* koopa = (TKoopa*)spine->getBody();
		f32 diff
		    = -180.0f
		      + std::fmodf(
		          360.0f + (koopa->mTargetDir - koopa->mRotation.y - -180.0f),
		          360.0f);

		bool turning;
		if (diff < -koopa->getParam()->turnSpeed.get()) {
			turn(koopa, -paramA(koopa)->turnSpeed.get());
			turning = true;
		} else if (diff < 0.0f) {
			turn(koopa, diff);
			turning = true;
		} else {
			turning = false;
		}

		if (turning)
			return false;

		return true;
	}

	static const TNerveKoopaTurnL& theNerve()
	{
		static TNerveKoopaTurnL nerve;
		return nerve;
	}
};

class TNerveKoopaTumble : public TNerveKoopaTurn {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveKoopaTumble& theNerve()
	{
		static TNerveKoopaTumble nerve;
		return nerve;
	}
};

class TNerveKoopaWait : public TNerveKoopaTurn {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveKoopaWait& theNerve()
	{
		static TNerveKoopaWait nerve;
		return nerve;
	}
};

class TNerveKoopaFlame : public TNerveKoopaTurn {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveKoopaFlame& theNerve()
	{
		static TNerveKoopaFlame nerve;
		return nerve;
	}
};

class TNerveKoopaProvoke : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveKoopaProvoke& theNerve()
	{
		static TNerveKoopaProvoke nerve;
		return nerve;
	}
};

class TNerveKoopaGetDown : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveKoopaGetDown& theNerve()
	{
		static TNerveKoopaGetDown nerve;
		return nerve;
	}
};

class TNerveKoopaStagger : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveKoopaStagger& theNerve()
	{
		static TNerveKoopaStagger nerve;
		return nerve;
	}
};

class TNerveKoopaFall : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveKoopaFall& theNerve()
	{
		static TNerveKoopaFall nerve;
		return nerve;
	}
};

class TNerveKoopaGetShowered : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNerveKoopaGetShowered& theNerve()
	{
		static TNerveKoopaGetShowered nerve;
		return nerve;
	}
};

static const char* koopa_bastable[] = {
	"/scene/koopa/bas/koopa_down.bas",
	nullptr,
	"/scene/koopa/bas/koopa_fall.bas",
	"/scene/koopa/bas/koopa_fire_end.bas",
	"/scene/koopa/bas/koopa_fire_loop.bas",
	"/scene/koopa/bas/koopa_fire_start.bas",
	"/scene/koopa/bas/koopa_first.bas",
	"/scene/koopa/bas/koopa_getup.bas",
	"/scene/koopa/bas/koopa_hipdrop.bas",
	"/scene/koopa/bas/koopa_stagger.bas",
	"/scene/koopa/bas/koopa_turn_l.bas",
	"/scene/koopa/bas/koopa_turn_r.bas",
	"/scene/koopa/bas/koopa_wait.bas",
	nullptr,
	"/scene/koopa/bas/koopa_waterhit.bas",
};

// fabricated: the original seems to have inlined this everywhere except where
// changeAnm() is called
inline void TKoopa::setAnm_(int bck, int btp, f32 speed)
{
	if (!getMActor()->checkCurBckFromIndex(bck)) {
		getMActor()->setBckFromIndex(bck);
		setAnmSound(getBas(bck));
	}

	if (btp != getMActor()->getCurAnmIdx(3))
		getMActor()->setBtpFromIndex(btp);

	J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(0);
	ctrl->setRate(0.5f * (speed * SMSGetAnmFrameRate()));
}

// fabricated
inline void TKoopa::pushNerve(const TNerveBase<TLiveActor>* nerve)
{
	mSpine->pushNerve(nerve);
}

// fabricated: body of checkMarioWhichSide(), which the original inlined into
// the nerves but our compiler refuses to auto-inline
static inline void KoopaSetMarioSpeed0(JGeometry::TVec3<f32>& v)
{
	v.set(*gpMarioSpeedX, *gpMarioSpeedY, *gpMarioSpeedZ);
}

static inline void KoopaSetMarioSpeed1(JGeometry::TVec3<f32>& v)
{
	KoopaSetMarioSpeed0(v);
}

static inline void KoopaSetMarioSpeed(JGeometry::TVec3<f32>& v)
{
	KoopaSetMarioSpeed1(v);
}

static inline TKoopaParams* KoopaSaveParam0(TEnemyManager* manager)
{
	return (TKoopaParams*)manager->getSaveParam();
}

static inline TKoopaParams* KoopaSaveParam1(TEnemyManager* manager)
{
	return KoopaSaveParam0(manager);
}

static inline TKoopaParams* KoopaSaveParam2(TKoopa* koopa)
{
	return KoopaSaveParam1((TEnemyManager*)koopa->getManager());
}

static inline f32 KoopaTargetDirW2(const TKoopa* k, const JGeometry::TVec3<f32>& p)
{
	return k->getTargetDir(p);
}
static inline f32 KoopaTargetDirW1(const TKoopa* k, const JGeometry::TVec3<f32>& p)
{
	return KoopaTargetDirW2(k, p);
}

static inline bool KoopaCheckMarioWhichSide(TKoopa* koopa)
{
	TBathtub* bathtub
	    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));

	JGeometry::TVec3<f32> marioSpeed;
	KoopaSetMarioSpeed(marioSpeed);
	f32 estimation = KoopaSaveParam2(koopa)->marioEstimationWait.get();
	JGeometry::TVec3<f32> offset;
	offset.x = marioSpeed.x * estimation;
	offset.y = marioSpeed.y * estimation;
	offset.z = marioSpeed.z * estimation;

	bool found
	    = bathtub->getNextGrip(*gpMarioPos, offset,
	                           koopa->getParam()->waitRange.get(),
	                           &koopa->mTargetDir);
	if (!found) {
		JGeometry::TVec3<f32> marioSpeed2;
		KoopaSetMarioSpeed(marioSpeed2);
		f32 estimation2 = KoopaSaveParam2(koopa)->marioEstimationFire.get();
		offset.x          = marioSpeed2.x * estimation2;
		offset.y          = marioSpeed2.y * estimation2;
		offset.z          = marioSpeed2.z * estimation2;
		koopa->mTargetDir = bathtub->getNextJuncture(*gpMarioPos, offset);
	}

	return found;
}

inline int TKoopa::getTargetSide() const
{
	f32 diff = -180.0f
	           + std::fmodf(360.0f + (mTargetDir - mRotation.y - -180.0f),
	                        360.0f);
	f32 range = getParam()->focusRange.get();
	if (diff < -range)
		return -1;
	if (diff > range)
		return 1;
	return 0;
}

// fabricated
static inline bool KoopaIsBreathingInFirst(const TKoopa* koopa)
{
	if (koopa->getAnmIndex() == 6) {
		f32 frame = koopa->getAnmFrame();
		if (68.0f <= frame && frame <= 164.0f)
			return true;
	}
	return false;
}

namespace {
BOOL KoopaNeckCallBack(J3DNode* node, int calcTiming);
}

// fabricated: body of passesAnmFrame()
static inline bool KoopaPassesAnmFrame(const TKoopa* koopa, f32 frame)
{
	bool result = false;
	if (koopa->getMActor()->getFrameCtrl(0)->getFrame() <= frame) {
		J3DFrameCtrl* ctrl = koopa->getMActor()->getFrameCtrl(0);
		if (frame <= 0.005f + (ctrl->getFrame() + ctrl->getRate()))
			result = true;
	}
	return result;
}

// fabricated: body of canTumble(), which the original inlined into the wait
// nerve but our compiler refuses to auto-inline
static inline bool KoopaCanTumble(const TKoopa* koopa)
{
	bool result;
	if (koopa->getMActor()->getCurAnmIdx(0) != 12)
		result = false;
	else if (koopa->getMActor()->curAnmEndsNext(0, nullptr))
		result = true;
	else if (KoopaPassesAnmFrame(koopa, 2.0f))
		result = true;
	else if (KoopaPassesAnmFrame(koopa, 400.0f))
		result = true;
	else if (KoopaPassesAnmFrame(koopa, 700.0f))
		result = true;
	else
		result = false;

	if (result) {
		TBathtub* bathtub
		    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));
		if (bathtub->allowsTumble())
			return true;
	}
	return false;
}

BOOL TNerveKoopaWait::execute(TSpineBase<TLiveActor>* spine) const
{
	TKoopa* koopa = (TKoopa*)spine->getBody();

	if (koopa->ignoresMario()) {
		koopa->setAnm_(12, 1, koopa->getParam()->waitSpeed.get());
		return false;
	}

	bool grip = KoopaCheckMarioWhichSide(koopa);
	int side  = koopa->getTargetSide();
	if (grip) {
		switch (side) {
		case -1:
			spine->pushNerve(&TNerveKoopaTurnL::theNerve());
			break;
		case 1:
			spine->pushNerve(&TNerveKoopaTurnR::theNerve());
			break;
		default:
			koopa->setAnm_(12, 1, koopa->getParam()->waitSpeed.get());
			if (KoopaCanTumble(koopa))
				spine->pushNerve(&TNerveKoopaTumble::theNerve());
			break;
		}
		return false;
	}

	switch (side) {
	case -1:
		spine->pushNerve(&TNerveKoopaTurnL::theNerve());
		break;
	case 1:
		spine->pushNerve(&TNerveKoopaTurnR::theNerve());
		break;
	default: {
		f32 targetDir = KoopaTargetDirW1(koopa, *gpMarioPos);
		koopa->mFlameReversed
		    = -180.0f
		          + std::fmodf(
		              360.0f + (targetDir - koopa->mTargetDir - -180.0f),
		              360.0f)
		      < 0.0f;
		spine->setNext(&TNerveKoopaFlame::theNerve());
		break;
	}
	}

	return false;
}

BOOL TNerveKoopaTumble::execute(TSpineBase<TLiveActor>* spine) const
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	TKoopa* koopa = (TKoopa*)spine->getBody();
	koopa->setAnm_(8, 0, koopa->getParam()->tumbleSpeed.get());
	koopa->getAnmFrame();

	if (spine->getTime() == 190) {
		gpCameraShake->startShake((EnumCamShakeMode)0x27, 1.0f);
		static TBathtub* bathtub
		    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));
		gpMarioParticleManager->emitAndBindToMtx(
		    0xF5, *bathtub->getRootJointMtx(), 0, this);
		if (SMS_IsMarioTouchGround4cm())
			SMSRumbleMgr->start(1, (f32*)nullptr);
	}

	if (koopa->endsAnm())
		return true;
	return false;
}

BOOL TNerveKoopaFall::execute(TSpineBase<TLiveActor>* spine) const
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	TKoopa* koopa = (TKoopa*)spine->getBody();
	koopa->setAnm_(2, 0, koopa->getParam()->fallSpeed.get());
	return false;
}

BOOL TNerveKoopaFlame::execute(TSpineBase<TLiveActor>* spine) const
{
	TKoopa* koopa = (TKoopa*)spine->getBody();

	switch (koopa->getAnmIndex()) {
	case 5:
		if (koopa->endsAnm()) {
			koopa->setAnm_(4, 0, 2.0f);
			spine->setNext(&TNerveKoopaFlame::theNerve());
		} else {
			koopa->mFlameHitMario = false;
		}
		break;

	case 3:
		if (koopa->endsAnm()) {
			if (koopa->mFlameHitMario) {
				gpMSound->startSoundActor(0x89AD, &koopa->mSoundPos, 0,
				                          nullptr, 0, 4);
				koopa->mFlameHitMario = false;
			}

			if (koopa->ignoresMario()) {
				spine->setNext(&TNerveKoopaWait::theNerve());
				return false;
			}

			bool grip = KoopaCheckMarioWhichSide(koopa);
			int side  = koopa->getTargetSide();
			if (grip) {
				spine->setNext(&TNerveKoopaWait::theNerve());
			} else {
				switch (side) {
				case -1:
					spine->pushNerve(&TNerveKoopaTurnL::theNerve());
					break;
				case 1:
					spine->pushNerve(&TNerveKoopaTurnR::theNerve());
					break;
				case 0: {
					f32 targetDir = KoopaTargetDirW1(koopa, *gpMarioPos);
					koopa->mFlameReversed
					    = -180.0f
					          + std::fmodf(360.0f
					                           + (targetDir - koopa->mTargetDir
					                              - -180.0f),
					                       360.0f)
					      < 0.0f;
					koopa->setAnm_(5, 0, koopa->getParam()->fireSpeed.get());
					spine->setNext(&TNerveKoopaFlame::theNerve());
					break;
				}
				}
			}
		}
		break;

	case 4: {
		int time = spine->getTime();
		if (time >= koopa->getParam()->flameCount.get()) {
			if ((time & 7) == 0) {
				bool grip = KoopaCheckMarioWhichSide(koopa);
				int side  = koopa->getTargetSide();
				if (grip || side != 0)
					koopa->setAnm_(3, 0, koopa->getParam()->fireSpeed.get());
			} else if (koopa->endsAnm()) {
				if (spine->getTime()
				    >= koopa->getParam()->flameFocusEndStep.get())
					koopa->setAnm_(3, 0, koopa->getParam()->fireSpeed.get());
			}
		}
		break;
	}

	default:
		koopa->setAnm_(5, 0, koopa->getParam()->fireSpeed.get());
		break;
	}

	return false;
}

BOOL TNerveKoopaProvoke::execute(TSpineBase<TLiveActor>* spine) const
{
	TKoopa* koopa = (TKoopa*)spine->getBody();
	koopa->setAnm_(6, 0, 2.0f);
	if (koopa->endsAnm()) {
		spine->setNext(&TNerveKoopaWait::theNerve());
		return false;
	}
	return false;
}

BOOL TNerveKoopaStagger::execute(TSpineBase<TLiveActor>* spine) const
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	TKoopa* koopa = (TKoopa*)spine->getBody();
	koopa->setAnm_(9, 0, koopa->getParam()->staggerSpeed.get());
	if (koopa->endsAnm())
		return true;
	return false;
}

BOOL TNerveKoopaGetShowered::execute(TSpineBase<TLiveActor>* spine) const
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	TKoopa* koopa = (TKoopa*)spine->getBody();
	koopa->setAnm_(14, 0, koopa->getParam()->waterhitSpeed.get());
	if (koopa->endsAnm())
		return true;
	return false;
}

BOOL TNerveKoopaGetDown::execute(TSpineBase<TLiveActor>* spine) const
{
	TKoopa* koopa = (TKoopa*)spine->getBody();

	switch (koopa->getAnmIndex()) {
	case 0:
		if (koopa->endsAnm())
			koopa->setAnm_(1, 0, koopa->getParam()->downSpeed.get());
		break;
	case 1: {
		TBathtub* bathtub
		    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));
		int time = spine->getTime();
		f32 step = koopa->getParam()->downStep.get();
		if ((f32)(time * (bathtub->getNumGripsDead() + 2)) >= step
		    && koopa->endsAnm())
			koopa->setAnm_(7, 0, koopa->getParam()->downSpeed.get());
		break;
	}
	case 7:
		if (koopa->endsAnm())
			return true;
		break;
	default: {
		koopa->setAnm_(0, 0, koopa->getParam()->downSpeed.get());
		static TBathtub* bathtub
		    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));
		gpMarioParticleManager->emitAndBindToMtx(
		    0xF5, *bathtub->getRootJointMtx(), 0, this);
		break;
	}
	}

	return false;
}

TKoopaParts::TKoopaParts(const char* name, u32 actor_type, TKoopa* owner,
                         f32 radius)
    : THitActor(name)
    , mOwner(owner)
{
	static_cast<TIdxGroupObj*>(JDrama::TNameRefGen::search("敵グループ"))
	    ->getChildren()
	    .push_back(this);
	initHitActor(actor_type, 5, HIT_CATEGORY_BOSS | HIT_CATEGORY_PLAYER,
	             radius, radius, radius, radius);
	onHitFilter(HIT_FILTER_NO_ATTACK);
	onHitFilter(HIT_FILTER_NO_DAMAGE);
	onHitFilter(HIT_FILTER_NO_COLLISION);
	onHitFilter(0x10000000);
	onHitFilter(0x08000000);
}

void TKoopaParts::perform(u32 cue, JDrama::TGraphics* graphics)
{
	THitActor::perform(cue, graphics);
	if (cue & 1) {
		control();
		for (int i = 0; i < getColNum(); ++i)
			attack_(getCollision(i));
	}
}

void TKoopaParts::set(const JGeometry::TVec3<f32>& pos, f32 radius, f32 height)
{
	mPosition.set(pos);
	offHitFilter(HIT_FILTER_NO_ATTACK);
	offHitFilter(HIT_FILTER_NO_DAMAGE);
	offHitFilter(HIT_FILTER_NO_COLLISION);
	setHitParams(radius, height, radius, height);
}

void TKoopaParts::remove()
{
	onHitFilter(HIT_FILTER_NO_ATTACK);
	onHitFilter(HIT_FILTER_NO_DAMAGE);
	onHitFilter(HIT_FILTER_NO_COLLISION);
}

TKoopaFlame::TKoopaFlame(TKoopa* owner)
    : TKoopaParts("クッパの吐く炎", ACTOR_TYPE_BOSS_UNK29, owner, 100.0f)
{
	mMaxLength = 0.0f;
	mLength    = 1.0f;
}

bool TKoopaFlame::isAlive() const { return mLength < mMaxLength; }

f32 TKoopaFlame::getLength() const { return mLength; }

void TKoopaFlame::resetFlame()
{
	mMaxLength = 0.0f;
	mLength    = 1.0f;
}

void TKoopaFlame::fire(const JGeometry::TVec3<f32>& pos,
                       const JGeometry::TVec3<f32>& dir, f32 speed,
                       f32 max_length, f32 radius, f32 height)
{
	mPosition.set(pos);
	mDir.set(dir);
	mStartPos.set(pos);
	mSpeed     = speed;
	mMaxLength = max_length;
	mLength    = 0.0f;
	mRadius    = radius;
	mHeight    = height;
}

void TKoopaFlame::control()
{
	if (!isAlive()) {
		remove();
		return;
	}

	mLength += mSpeed;

	f32 height = mHeight;
	JGeometry::TVec3<f32> pos;
	pos.x      = mDir.x * mLength + mStartPos.x;
	pos.y      = mDir.y * mLength + mStartPos.y;
	pos.z      = mDir.z * mLength + mStartPos.z;
	f32 radius = mRadius;
	if (height <= 0.0f)
		height = 2.0f * radius;

	set(pos, radius, height);
}

BOOL TKoopaFlame::receiveMessage(THitActor* sender, u32 message)
{
	switch (message) {
	case HIT_MESSAGE_SPRAYED_BY_WATER:
		return false;
	}
	return true;
}

void TKoopaFlame::attack_(THitActor* target)
{
	if (target->receiveMessage(this, HIT_MESSAGE_UNKA)
	    && target == gpMarioAddress) {
		f32 jump = mOwner->getParam()->flameJump.get();
		JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
		SMS_ThrowMario(up, jump);
		mOwner->mFlameHitMario = true;
		mOwner->changeAnm(3, 0, mOwner->getParam()->fireSpeed.get());
		mOwner->setIgnoreMario(240);
	}
}

TKoopaHand::TKoopaHand(TKoopa* owner)
    : TKoopaParts("クッパの手", HIT_CATEGORY_BOSS | 0x2B, owner, 100.0f)
{
}

BOOL TKoopaHand::receiveMessage(THitActor* sender, u32 message)
{
	return true;
}

void TKoopaHand::attack_(THitActor* target)
{
	target->receiveMessage(this, HIT_MESSAGE_ATTACK);
}

TKoopaHead::TKoopaHead(TKoopa* owner)
    : TKoopaParts("クッパの頭", ACTOR_TYPE_BOSS_UNK2A, owner, 100.0f)
{
}

BOOL TKoopaHead::receiveMessage(THitActor* sender, u32 message)
{
	switch (message) {
	case HIT_MESSAGE_SPRAYED_BY_WATER:
		if (mOwner->getShowered()) {
			gpMarioParticleManager->emit(0xE7, &sender->mPosition, 0, nullptr);
			gpMSound->startSoundSet(0x6802, &mOwner->mPosition, 0, 0.0f, 0, 0,
			                        4);
		}
		break;
	case HIT_MESSAGE_ATTACK:
		if (sender->getActorType() == ACTOR_TYPE_BATHTUB_KILLER)
			mOwner->stagger(false);
		break;
	}
	return true;
}

void TKoopaHead::attack_(THitActor* target)
{
	if (target->receiveMessage(this, HIT_MESSAGE_ATTACK)
	    && target == SMS_GetMarioHitActor()) {
		JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
		SMS_ThrowMario(up, 60.0f);
	}
}

TKoopaBody::TKoopaBody(TKoopa* owner)
    : TKoopaParts("クッパの体", ACTOR_TYPE_BOSS_UNK2A, owner, 100.0f)
{
}

BOOL TKoopaBody::receiveMessage(THitActor* sender, u32 message)
{
	switch (message) {
	case HIT_MESSAGE_SPRAYED_BY_WATER:
		break;
	case HIT_MESSAGE_ATTACK:
		if (sender->getActorType() == ACTOR_TYPE_BATHTUB_KILLER)
			mOwner->stagger(false);
		break;
	}
	return true;
}

void TKoopaBody::attack_(THitActor* target)
{
	if (target->receiveMessage(this, HIT_MESSAGE_ATTACK)
	    && target == SMS_GetMarioHitActor()) {
		JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
		SMS_ThrowMario(up, 60.0f);
	}
}

f32 TKoopa::getFlameDirDegree() const
{
    // FAKEMATCH(frame): 16 bytes; real construct unknown
    volatile u32 pad[4]; (void)pad;
	f32 dir = getFlameDirRate() * getParam()->flameNeckRange.get();
	return mRotation.y + (mFlameReversed ? -dir : dir);
}

namespace {

// a literal on the right of fmuls stays on the right only through a parameter
static inline f32 KoopaMul(f32 a, f32 b) { return a * b; }

// the ROM calls TUtil::sqrt out of line here
static inline f32 KoopaSqrt(f32 v) { return JGeometry::TUtil<f32>::sqrt(v); }

// Turns the head joint to face mario (and sweeps it while breathing fire)
BOOL KoopaNeckCallBack(J3DNode* node, int calcTiming)
{
	if (calcTiming != 0)
		return TRUE;

	TKoopa* koopa = (TKoopa*)node->getCallBackUserData();
	TPosition3f& mtx
	    = (TPosition3f&)*j3dSys.getModel()->getAnmMtx(((J3DJoint*)node)->getJntNo());

	JGeometry::TVec3<f32> toMario(*gpMarioPos);
	toMario.y += 85.0f;
	JGeometry::TVec3<f32> trans;
	mtx.getTrans(trans);
	toMario.sub(trans);

	if (koopa->isFlaming()) {
		f32 yaw = KoopaMul(koopa->getFlameDirRate(), 6.2831855f);
		yaw      = yaw * koopa->getParam()->flameNeckRange.get() / 360.0f;
		f32 pitch = yaw * koopa->getParam()->flameNeckDownRate.get();
		if (pitch > 0.0f)
			pitch = -pitch;
		if (koopa->mFlameReversed)
			yaw = -yaw;

		f32 s = sinf(yaw);
		f32 c = cosf(yaw);
		TPosition3f rotY;
		rotY.setXDir(c, 0.0f, -s);
		rotY.setYDir(0.0f, 1.0f, 0.0f);
		rotY.setZDir(s, 0.0f, c);
		rotY.setTrans(0.0f, 0.0f, 0.0f);
		mtx.concat(rotY, mtx);

		s = sinf(pitch);
		c = cosf(pitch);
		TPosition3f rotZ;
		rotZ.setXDir(c, s, 0.0f);
		rotZ.setYDir(-s, c, 0.0f);
		rotZ.setZDir(0.0f, 0.0f, 1.0f);
		rotZ.setTrans(0.0f, 0.0f, 0.0f);
		mtx.concat(rotZ);
	}

	f32 focus = koopa->getNeckFocus();

	JGeometry::TVec3<f32> yAxis;
	mtx.getYDir(yAxis);
	JGeometry::TVec3<f32> xAxis;
	mtx.getXDir(xAxis);

	// project the direction to mario onto the plane of the neck
	f32 d = yAxis.dot(toMario);
	d     = -d;
	JGeometry::TVec3<f32> dir;
	dir.scaleAdd(d, toMario, yAxis);
	dir.normalize();
	toMario.normalize();

	f32 facing = dir.dot(xAxis);
	if (facing < 0.5)
		focus *= (1.0f + facing) / 1.5f;

	JGeometry::TQuat4<f32> rot;
	rot.setRotate(xAxis, dir, focus);
	rot.normalize();

	if (!koopa->isFlaming()) {
		JGeometry::TVec3<f32> cross;
		cross.cross(dir, toMario);
		f32 len   = KoopaSqrt(cross.squared());
		f32 angle = fabsf(atan2f(len, dir.dot(toMario)));
		if (toMario.dot(yAxis) < 0.0f)
			angle = -angle;

		JGeometry::TVec3<f32> axis;
		rot.getZDir(axis);
		JGeometry::TQuat4<f32> pitchRot;
		pitchRot.setRotate(axis, angle * focus);
		pitchRot.normalize();
		rot.mul(pitchRot, rot);
	}

	TPosition3f quatMtx;
	quatMtx.setQuat(rot);
	quatMtx.setTrans(0.0f, 0.0f, 0.0f);
	mtx.concat(quatMtx);

	PSMTXCopy(mtx, J3DSys::mCurrentMtx);
	return TRUE;
}

} // namespace

void TKoopa::stopFlame()
{
	mFlameHitMario = true;
	changeAnm(3, 0, getParam()->fireSpeed.get());
	setIgnoreMario(240);
}

void TKoopa::breathFlame()
{
	int freeIdx  = -1;
	bool tooNear = false;
	for (int i = 0; i < 10; ++i) {
		if (!mFlames[i]->isAlive())
			freeIdx = i;
		else if (mFlames[i]->getLength()
		         < 2.0f * getParam()->flameRadius.get())
			tooNear = true;
	}

	if (!tooNear && freeIdx >= 0) {
		MtxPtr mtx = getMActor()->getModel()->getAnmMtx(mHeadJointIdx);
		JGeometry::TVec3<f32> dir(mtx[0][0], 0.0f, mtx[2][0]);
		JGeometry::TVec3<f32> pos(mtx[0][3], mtx[1][3] - 500.0f, mtx[2][3]);
		dir.normalize();

		mFlames[freeIdx]->fire(pos, dir, getParam()->flameVelocity.get(),
		                       4000.0f, getParam()->flameRadius.get(),
		                       getParam()->flameHeight.get());
	}
}

void TKoopa::resetFlame_()
{
	for (int i = 0; i < 10; ++i)
		mFlames[i]->resetFlame();
}

void TKoopa::setUpHitActors()
{
	// TODO: these bodies are breathFlame() and resetFlame_(), which the
	// original inlined here but our compiler refuses to.
	if (isBreathing()) {
		int freeIdx  = -1;
		bool tooNear = false;
		for (int i = 0; i < 10; ++i) {
			if (!mFlames[i]->isAlive())
				freeIdx = i;
			else if (mFlames[i]->getLength()
			         < 2.0f * getParam()->flameRadius.get())
				tooNear = true;
		}

		if (!tooNear && freeIdx >= 0) {
			MtxPtr mtx = getMActor()->getModel()->getAnmMtx(mHeadJointIdx);
			JGeometry::TVec3<f32> dir(mtx[0][0], 0.0f, mtx[2][0]);
			JGeometry::TVec3<f32> pos(mtx[0][3], mtx[1][3] - 500.0f,
			                          mtx[2][3]);
			dir.normalize();

			mFlames[freeIdx]->fire(pos, dir, getParam()->flameVelocity.get(),
			                       4000.0f, getParam()->flameRadius.get(),
			                       getParam()->flameHeight.get());
		}
	} else {
		for (int i = 0; i < 10; ++i)
			mFlames[i]->resetFlame();
	}

	MtxPtr agoMtx = getMActor()->getModel()->getAnmMtx(mAgoJointIdx);
	f32 radius    = getParam()->headRadius.get();
	JGeometry::TVec3<f32> headPos(agoMtx[0][3], agoMtx[1][3] - 200.0f,
	                              agoMtx[2][3]);
	mHead->set(headPos, radius, 2.0f * radius);
	mBody->set(mPosition, 800.0f, 2000.0f);
}

// TODO: the original inlines this body into the nerves (direct calls) but
// keeps changeAnm() out of line in TNerveKoopaTurnR/L and TKoopaFlame::attack_
// (stopFlame). Giving changeAnm() ten statements (e.g. `f32 rate; rate = speed
// * SMSGetAnmFrameRate(); rate = 0.5f * rate;`) reproduces that without the
// pragma and matches changeAnm() 100%, but costs 8 bytes of frame in
// TNerveKoopaProvoke::execute, so the fabricated setAnm_() split is kept.
#pragma dont_inline on
void TKoopa::changeAnm(int bck, int btp, f32 speed)
{
	setAnm_(bck, btp, speed);
}
#pragma dont_inline reset

int TKoopa::getAnmIndex() const { return getMActor()->getCurAnmIdx(0); }

bool TKoopa::endsAnm() const
{
	return getMActor()->curAnmEndsNext(0, nullptr);
}

void TKoopa::laugh() { setAnm_(6, 0, 2.0f); }

bool TKoopa::isBreathing() const
{
	if (getAnmIndex() == 4)
		return true;
	if (getAnmIndex() == 5 && getAnmFrame() >= 85.0f)
		return true;
	return false;
}

bool TKoopa::ignoresMario() const { return mIgnoreMarioTimer > 0; }

void TKoopa::setIgnoreMario(long time) { mIgnoreMarioTimer = time; }

f32 TKoopa::getFlameDirRate() const
{
	f32 frame = getMActor()->getFrameCtrl(0)->getFrame();
	f32 end;
	end = getMActor()->getFrameCtrl(0)->getEnd();
	int endStep;

	TKoopaParams* params = getParam();
	int time;
	f32 overStart = params->flameOverStart.get();
	int startStep = params->flameFocusStartStep.get();
	endStep = params->flameFocusEndStep.get();
	time = mSpine->getTime();

	int idx = getAnmIndex();
	switch (idx) {
	case 5:
		return -(frame * overStart / end);
	case 3:
	case 4: {
		f32 rate;
		if (mSpine->getTime() <= startStep)
			rate = -overStart;
		else if (time <= endStep)
			rate = (1.0f + overStart) * (f32)(time - startStep)
			           / (f32)(endStep - startStep)
			       - overStart;
		else
			rate = 1.0f;

		if (idx == 3)
			rate *= 1.0f - frame / end;
		return rate;
	}
	default:
		return 0.0f;
	}
}

bool TKoopa::isFlaming() const
{
	switch (getAnmIndex()) {
	case 3:
	case 4:
	case 5:
		return true;
	}
	return false;
}

bool TKoopa::isProvoking() const
{
	if (&TNerveKoopaProvoke::theNerve() == mSpine->getCurrentNerve())
		return true;
	return false;
}

// How much the neck should track mario, depending on the current animation
f32 TKoopa::getNeckFocus() const
{
	int idx            = getAnmIndex();
	J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(0);
	f32 end            = ctrl->getEnd();
	f32 frame          = ctrl->getFrame();
	f32 focus          = 1.0f;

	// case order follows the layout of the original code
	switch (idx) {
	case 6:
		focus = 0.0f;
		if (frame >= 164.0f)
			focus = (frame - 164.0f) / (end - 164.0f);
		break;
	case 2:
		focus = 0.0f;
		break;
	case 0:
		if (frame <= 40.0f)
			focus = 1.0f - frame / 40.0f;
		else
			focus = 0.0f;
		break;
	case 1:
		focus = 0.0f;
		break;
	case 7:
		if (frame <= 125.0f)
			focus = 0.0f;
		else
			focus = (frame - 125.0f) / (end - 125.0f);
		break;
	case 9:
		if (frame <= 30.0f)
			focus = 1.0f - frame / 30.0f;
		else if (frame <= 65.0f)
			focus = 0.0f;
		else
			focus = (frame - 65.0f) / (end - 65.0f);
		break;
	case 8:
		if (frame <= 30.0f)
			focus = 1.0f - frame / 30.0f;
		else if (frame <= 170.0f)
			focus = 0.0f;
		else
			focus = (frame - 170.0f) / (end - 170.0f);
		break;
	case 14:
		if (frame <= 20.0f)
			focus = 1.0f - frame / 20.0f;
		else if (frame <= 40.0f)
			focus = 0.0f;
		else
			focus = (frame - 40.0f) / (end - 40.0f);
		break;
	case 4:
		focus = 0.0f;
		break;
	case 5:
		if (frame <= 103.0f)
			focus = 1.0f - frame / 103.0f;
		else
			focus = 0.0f;
		break;
	case 3:
		focus = frame / end;
		break;
	case 12:
		if (frame <= 200.0f) {
		} else if (frame <= 255.0f) {
			focus = 1.0f - (frame - 200.0f) / 55.0f;
		} else if (frame <= 330.0f) {
			focus = 0.0f;
		} else if (frame <= 390.0f) {
			focus = (frame - 330.0f) / 60.0f;
		} else if (frame <= 440.0f) {
		} else if (frame <= 480.0f) {
			focus = 1.0f - (frame - 440.0f) / 40.0f;
		} else if (frame <= 555.0f) {
			focus = 0.0f;
		} else if (frame <= 615.0f) {
			focus = (frame - 555.0f) / 60.0f;
		}
		break;
	}

	return focus;
}

bool TKoopa::allowsLaunch() const
{
	if (&TNerveKoopaTumble::theNerve() == mSpine->getCurrentNerve())
		return false;
	return true;
}

void TKoopa::getDown()
{
	if (&TNerveKoopaFall::theNerve() == mSpine->getCurrentNerve())
		return;
	if (&TNerveKoopaProvoke::theNerve() == mSpine->getCurrentNerve())
		return;
	if (&TNerveKoopaTumble::theNerve() == mSpine->getCurrentNerve())
		return;

	if (&TNerveKoopaStagger::theNerve() == mSpine->getCurrentNerve())
		mSpine->setNext(&TNerveKoopaGetDown::theNerve());

	if (&TNerveKoopaGetShowered::theNerve() == mSpine->getCurrentNerve())
		mSpine->setNext(&TNerveKoopaGetDown::theNerve());

	pushNerve(&TNerveKoopaGetDown::theNerve());
}

bool TKoopa::effectsTumble() const
{
	if (&TNerveKoopaTumble::theNerve() == mSpine->getCurrentNerve()) {
		int time = mSpine->getTime();
		if (time < 900 && time > 190)
			return true;
	}
	return false;
}

bool TKoopa::getShowered()
{
	if (&TNerveKoopaFall::theNerve() == mSpine->getCurrentNerve())
		return false;
	if (&TNerveKoopaProvoke::theNerve() == mSpine->getCurrentNerve())
		return false;
	if (&TNerveKoopaTumble::theNerve() == mSpine->getCurrentNerve())
		return false;
	if (&TNerveKoopaGetDown::theNerve() == mSpine->getCurrentNerve())
		return false;
	if (&TNerveKoopaGetShowered::theNerve() == mSpine->getCurrentNerve())
		return true;

	if (&TNerveKoopaStagger::theNerve() == mSpine->getCurrentNerve()) {
		mSpine->setNext(&TNerveKoopaGetShowered::theNerve());
		return true;
	}

	if (&TNerveKoopaFlame::theNerve() == mSpine->getCurrentNerve()) {
		mSpine->setNext(&TNerveKoopaWait::theNerve());
		return false;
	}

	pushNerve(&TNerveKoopaGetShowered::theNerve());
	return true;
}

void TKoopa::stagger(bool force)
{
	if (&TNerveKoopaFall::theNerve() == mSpine->getCurrentNerve())
		return;
	if (&TNerveKoopaProvoke::theNerve() == mSpine->getCurrentNerve())
		return;
	if (!force && &TNerveKoopaFlame::theNerve() == mSpine->getCurrentNerve())
		return;
	if (&TNerveKoopaTumble::theNerve() == mSpine->getCurrentNerve())
		return;
	if (&TNerveKoopaGetDown::theNerve() == mSpine->getCurrentNerve())
		return;
	if (&TNerveKoopaGetShowered::theNerve() == mSpine->getCurrentNerve())
		return;

	pushNerve(&TNerveKoopaStagger::theNerve());
}

bool TKoopa::isTumbling() const
{
	f32 frame = getAnmFrame();
	bool tumbling
	    = (&TNerveKoopaTumble::theNerve() == mSpine->getCurrentNerve()
	       && frame >= getParam()->tumbleStartFrame.get())
	      && frame <= getParam()->tumbleEndFrame.get();
	return tumbling;
}

// Returns the yaw of pos in the local space of the bathtub
f32 TKoopa::getTargetDir(const JGeometry::TVec3<f32>& pos) const
{
	TBathtub* bathtub
	    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));
	MtxPtr mtx = *bathtub->getRootJointMtx();

	f32 dy = pos.y - mtx[1][3];
	f32 dx = pos.x - mtx[0][3];
	f32 dz = pos.z - mtx[2][3];

	f32 localX = mtx[2][0] * dz + (mtx[0][0] * dx + mtx[1][0] * dy);
	f32 localZ = mtx[2][2] * dz + (mtx[0][2] * dx + mtx[1][2] * dy);
	return (360.0f / 65536.0f) * matan(localZ, localX);
}

f32 TKoopa::getAnmFrame() const
{
	return getMActor()->getFrameCtrl(0)->getFrame();
}

f32 TKoopa::getAnmFrameNext() const
{
	J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(0);
	return ctrl->getFrame() + ctrl->getRate();
}

bool TKoopa::passesAnmFrame(f32 frame) const
{
	return KoopaPassesAnmFrame(this, frame);
}

bool TKoopa::canTumble() const { return KoopaCanTumble(this); }

void TKoopa::fall()
{
	mRotation.y = 180.0f;
	mSpine->setNext(&TNerveKoopaFall::theNerve());
}

void TKoopa::updateAnmSound()
{
	if (getAnmIndex() == 8) {
		mSoundPos.x = mPosition.x;
		mSoundPos.y = mPosition.y;
		mSoundPos.z = mPosition.z;
	} else {
		MtxPtr mtx = getHeadMtx();
		mSoundPos.set(mtx[0][3], mtx[1][3], mtx[2][3]);
	}

	if (mAnmSound && mAnmSoundPath) {
		J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(0);
		mAnmSound->animeLoop(&mSoundPos, ctrl->getFrame(), ctrl->getRate(), 0,
		                     4);
	}
}

f32 TKoopa::getAnmEnd() const { return getMActor()->getFrameCtrl(0)->getEnd(); }

TKoopa::TKoopa(const char* name)
    : TSpineEnemy(name)
{
	onLiveFlag(LIVE_FLAG_AIRBORNE);
	offLiveFlag(LIVE_FLAG_UNK100);
	onLiveFlag(LIVE_FLAG_UNK10);
}

void TKoopa::load(JSUMemoryInputStream& stream) { TSpineEnemy::load(stream); }

void TKoopa::loadAfter()
{
	TSpineEnemy::loadAfter();

	for (int i = 0; i < 10; ++i)
		mFlames[i] = new TKoopaFlame(this);

	for (int i = 0; i < 2; ++i)
		mHands[i] = new TKoopaHand(this);

	mHead = new TKoopaHead(this);
	mBody = new TKoopaBody(this);
}

// same as TKoopa::getTargetDir; the original inlines it into init only
static inline f32 KoopaTargetDir(const JGeometry::TVec3<f32>& pos)
{
	TBathtub* bathtub
	    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));
	MtxPtr mtx = *bathtub->getRootJointMtx();

	JGeometry::TVec3<f32> trans(mtx[0][3], mtx[1][3], mtx[2][3]);
	JGeometry::TVec3<f32> d;
	JGeometry::TVec3<f32> axisX(mtx[0][0], mtx[1][0], mtx[2][0]);
	JGeometry::TVec3<f32> axisZ(mtx[0][2], mtx[1][2], mtx[2][2]);
	d.sub(pos, trans);

	f32 localX = axisX.z * d.z + (axisX.x * d.x + axisX.y * d.y);
	f32 localZ = axisZ.z * d.z + (axisZ.x * d.x + axisZ.y * d.y);
	return (360.0f / 65536.0f) * matan(localZ, localX);
}

void TKoopa::init(TLiveManager* manager)
{
	mBodyRadius = 800.0f;
	mHeadHeight = 2000.0f;
	TSpineEnemy::init(manager);
	onHitFilter(HIT_FILTER_NO_COLLISION);
	onHitFilter(HIT_FILTER_NO_DAMAGE);
	offHitFilter(HIT_FILTER_NO_ATTACK);
	mSpine->initWith(&TNerveKoopaProvoke::theNerve());

	setAnm_(12, 1, 2.0f);
	setAnm_(6, 0, 2.0f);
	getMActor()->initSimpleMotionBlend(16);

	mTargetDir = KoopaTargetDir(*gpMarioPos);

	initAnmSound();
	reset();

	JUTNameTab* jointNames = getModel()->getModelData()->getJointName();
	// the original loops over the joint names here (body stripped)
	for (u16 i = 0; i < jointNames->getNameNum(); i++) { }
	mAgoJointIdx           = jointNames->getIndex("ago");
	mHeadJointIdx          = jointNames->getIndex("head");
	mNeckJointIdx          = jointNames->getIndex("neck");

	J3DJoint* headJoint
	    = getModel()->getModelData()->getJointNodePointer(mHeadJointIdx);
	headJoint->setCallBack(KoopaNeckCallBack);
	headJoint->setCallBackUserData(this);

	mNeckFocus     = 1.0f;
	mFlameHitMario = false;
}

const char** TKoopa::getBasNameTable() const { return koopa_bastable; }

MtxPtr TKoopa::getHeadMtx() const
{
	return getMActor()->getModel()->getAnmMtx(mHeadJointIdx);
}

void TKoopa::reset()
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	TSpineEnemy::reset();
	setAnm_(12, 1, getParam()->waitSpeed.get());
	setIgnoreMario(600);
}

// fabricated: the original emitted the four fire effects through two inline
// levels (TVec3::set is called out of line at those sites)
static inline void KoopaEmitFlameEffect(TKoopa* koopa, int id,
                                        const JGeometry::TVec3<f32>& scale)
{
	JPABaseEmitter* emitter = gpMarioParticleManager->emitAndBindToMtxPtr(
	    id, koopa->getHeadMtx(), 1, koopa);
	if (emitter)
		emitter->setGlobalScale(scale);
}

// fabricated
static inline void KoopaEmitFlameEffects(TKoopa* koopa)
{
	f32 s = koopa->getParam()->flameScale.get();
	JGeometry::TVec3<f32> scale(s, s, s);

	JPABaseEmitter* emitter = gpMarioParticleManager->emitAndBindToMtxPtr(
	    0x1F3, koopa->getHeadMtx(), 3, koopa);
	if (emitter)
		emitter->setGlobalScale(scale);

	KoopaEmitFlameEffect(koopa, 0x1C3, scale);
	KoopaEmitFlameEffect(koopa, 0x1C2, scale);
	KoopaEmitFlameEffect(koopa, 0x1C1, scale);
	KoopaEmitFlameEffect(koopa, 0x1C0, scale);
}

void TKoopa::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (cue & 1) {
		mNeckFocus = getNeckFocus();
		if (mIgnoreMarioTimer > 0)
			mIgnoreMarioTimer -= 1;
	}

	TSpineEnemy::perform(cue, graphics);

	for (int i = 0; i < 10; ++i)
		mFlames[i]->perform(cue, graphics);

	mHead->perform(cue, graphics);
	mHands[0]->perform(cue, graphics);
	mHands[1]->perform(cue, graphics);
	mBody->perform(cue, graphics);

	if (cue & 1) {
		if (isTumbling()) {
			TBathtub* bathtub = static_cast<TBathtub*>(
			    JDrama::TNameRefGen::search("バスタブ"));
			bathtub->tumble(mRotation.y, getParam()->tumbleWeight.get());
		}
		setUpHitActors();
	}

	if (cue & 2) {
		if (isBreathing() || KoopaIsBreathingInFirst(this)) {
			getMActor()->calc();

			KoopaEmitFlameEffects(this);
		}
	}
}

BOOL TKoopa::receiveMessage(THitActor* sender, u32 message)
{
	return TSpineEnemy::receiveMessage(sender, message);
}

// Koopa is attached to the bathtub, 1500 units below its root joint
void TKoopa::calcRootMatrix()
{
	TBathtub* bathtub
	    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));
	TPosition3f& tub = (TPosition3f&)*bathtub->getRootJointMtx();

	JGeometry::TVec3<f32> off;
	tub.getYDir(off);
	off.scale(-1500.0f);

	TPosition3f mtx;
	MsMtxSetRotRPH(mtx, 0.0f, mRotation.y, 0.0f);
	mtx.setTrans(0.0f, 0.0f, 0.0f);
	mtx.concat(tub, mtx);

	mtx.getTrans(mPosition);
	mPosition += off;
	mtx.setTrans(mPosition);

	getModel()->setBaseTRMtx(mtx);
	JGeometry::TVec3<f32> scale(1.0f, 1.0f, 1.0f);
	getModel()->setBaseScale(scale);
	mScaling.set(1.0f, 1.0f, 1.0f);
}

bool TKoopa::checkMarioWhichSide() { return KoopaCheckMarioWhichSide(this); }

TKoopaParams* TKoopa::getParam() const
{
	return (TKoopaParams*)((TEnemyManager*)mManager)->unk38;
}

TKoopaManager::TKoopaManager(const char* name)
    : TEnemyManager(name)
{
}

TSpineEnemy* TKoopaManager::createEnemyInstance() { return nullptr; }

void TKoopaManager::createModelData()
{
	static TModelDataLoadEntry entry[] = {
		{ "koopa_model.bmd", 0x14240000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

void TKoopaManager::load(JSUMemoryInputStream& stream)
{
	TEnemyManager::load(stream);
	unk38 = new TKoopaParams("/enemy/koopa.prm");
}

void TKoopaManager::loadAfter()
{
	TEnemyManager::loadAfter();
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_fire_a.jpa", 0x1C0);
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_fire_b.jpa", 0x1C1);
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_fire_c.jpa", 0x1C2);
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_fire_d.jpa", 0x1C3);
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_hipdrop.jpa", 0xF5);
	SMS_LoadParticle("/scene/koopa/jpa/ms_kp_fire_e.jpa", 0x1F3);
}
