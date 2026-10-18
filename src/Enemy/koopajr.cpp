#include <Enemy/KoopaJr.hpp>
#include <Enemy/BathtubBinder.hpp>
#include <Enemy/BathtubKiller.hpp>
#include <Enemy/Koopa.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JGeometry/JGQuat4.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <M3DUtil/MActor.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <Map/MapCollisionEntry.hpp>
#include <MoveBG/MapObjCorona.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/Watergun.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/Strategy.hpp>
#include <System/FlagManager.hpp>
#include <System/Particles.hpp>
#include <stdlib.h>
#include <math.h>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

static const char* koopajr_bastable[] = {
	"/scene/koopajr/bas/koopajr_damage.bas",
	"/scene/koopajr/bas/koopajr_shoot.bas",
	nullptr,
	"/scene/koopajr/bas/koopajr_yahoo.bas",
};

static const char* TKoopaJr_jointNameTable[] = {
	"KoopaJr_null", "killer_null00", "killer_null03",
	"killer_null01", "killer_null04",
};

static int TKoopaJr_jointIndexTable[5];

static const char* koopajrsubmarine_bastable[1] = { nullptr };

// fabricated, counterpart of JGeometry::max
template <class T> static inline T KJMin(T a, T b) { return a >= b ? b : a; }

// fabricated
static inline f32 KJWrapMod(f32 value, f32 min, f32 range)
{
	return min + JGeometry::TUtil<f32>::mod(range + (value - min), range);
}

// fabricated
// TODO: the original never inlines std::fmodf: its weak copy is 0x5C bytes,
// the same body as JGeometry::TUtil<f32>::mod, so our MSL math.h version
// (a ::fmod wrapper) differs from the one SMS was built with.
static inline f32 KJWrapFmod(f32 value, f32 min, f32 range)
{
	return min + std::fmodf(range + (value - min), range);
}

// fabricated
// TODO: the original keeps TDirectionCalc::calcNearerDirection out of line in
// TKoopaJrSubmarine::makeRelativeAngle, which these helpers reproduce. The
// real helpers were probably inline TDirectionCalc members.
static inline f32 KJAbsNearer(TDirectionCalc& dir, f32 direction)
{
	return fabsf(dir.unk0 - dir.calcNearerDirection(direction));
}

// fabricated
static inline f32 KJTurn(TDirectionCalc& dir, f32 target, f32 speed)
{
	dir.normalize();
	f32 nearer = dir.calcNearerDirection(target);
	if (nearer > dir.unk0) {
		f32 diff = nearer - dir.unk0;
		if (diff < speed)
			speed = diff;
		return dir.unk0 + speed;
	}

	f32 diff = dir.unk0 - nearer;
	if (diff < speed)
		speed = diff;
	return dir.unk0 - speed;
}

TDirectionCalc::TDirectionCalc() { unk0 = 0.0f; }

TDirectionCalc::TDirectionCalc(f32 direction) { unk0 = direction; }

TDirectionCalc::TDirectionCalc(JGeometry::TVec3<f32> vec)
{
	makeDirection(vec);
}

void TDirectionCalc::normalize() { unk0 = KJWrapMod(unk0, 0.0f, 6.2831855f); }

f32 TDirectionCalc::calcNearerDirection(f32 direction)
{
    // FAKEMATCH(frame): 16 bytes; real construct unknown
    volatile u32 pad[4]; (void)pad;
	unk0 = KJWrapFmod(unk0, 0.0f, 6.2831855f);
	if (direction >= unk0) {
		f32 diff = direction - unk0;
		if (6.2831855f - diff < diff)
			direction -= 6.2831855f;
	} else {
		f32 diff = unk0 - direction;
		if (6.2831855f - diff < diff)
			direction += 6.2831855f;
	}
	return direction;
}

f32 TDirectionCalc::sub(f32 direction)
{
	normalize();
	if (direction >= unk0) {
		f32 diff = direction - unk0;
		if (6.2831855f - diff < diff)
			direction -= 6.2831855f;
	} else {
		f32 diff = unk0 - direction;
		if (6.2831855f - diff < diff)
			direction += 6.2831855f;
	}
	return unk0 - direction;
}

f32 TDirectionCalc::calcTurnDirection(f32 target, f32 speed)
{
	unk0 = KJWrapFmod(unk0, 0.0f, 6.2831855f);
	normalize();
	if (target >= unk0) {
		f32 diff = target - unk0;
		if (6.2831855f - diff < diff)
			target -= 6.2831855f;
	} else {
		f32 diff = unk0 - target;
		if (6.2831855f - diff < diff)
			target += 6.2831855f;
	}

	f32 step = speed;
	if (target > unk0) {
		f32 diff = target - unk0;
		if (diff < speed)
			step = diff;
		return unk0 + step;
	}

	f32 diff = unk0 - target;
	if (diff < speed)
		step = diff;
	return unk0 - step;
}

void TDirectionCalc::makeDirection(JGeometry::TVec3<f32> vec)
{
	unk0 = atan2(vec.x, vec.z);
}

JGeometry::TVec3<f32> TDirectionCalc::calcDirectionVector()
{
	return JGeometry::TVec3<f32>(sin(unk0), 0.0f, cos(unk0));
}

f32 TDirectionCalc::absDirection(f32 direction)
{
    // FAKEMATCH(frame): 24 bytes; real construct unknown
    volatile u32 pad[6]; (void)pad;
	f32 diff = sub(direction);
	return fabsf(diff);
}

f32 TDirectionCalc::d2r(f32 degrees)
{
	return (degrees * JGeometry::TUtil<f32>::PI()) / 180.0f;
}

f32 TDirectionCalc::r2d(f32 radians) { return (180.0f * radians) / 3.1415927f; }

TKoopaJrParams::TKoopaJrParams(const char* path)
    : TSpineEnemyParams(path)
    , PARAM_INIT(mSLLaunchKillerLimit, 10000.0f)
    , PARAM_INIT(mSLDamageRadius, 1000.0f)
    , PARAM_INIT(mSLDamageHeight, 4000.0f)
    , PARAM_INIT(mSLKoopaJrScale, 1.6f)
    , PARAM_INIT(mSLFastLaunchDistance, 10000.0f)
    , PARAM_INIT(mSLDamagePeriod, 360)
    , PARAM_INIT(mSLLaunchKillerPeriod, 1200)
    , PARAM_INIT(mSLLaunchKillerPeriodFast, 360)
{
	TParams::load(mPrmPath);
	mSLLaunchKillerLimit.set(4200.0f);
	mSLDamageRadius.set(100.0f);
	mSLDamageHeight.set(300.0f);
	mSLKoopaJrScale.set(2.0f);
	mSLFastLaunchDistance.set(4600.0f);
	mSLDamagePeriod.set(240);
	mSLLaunchKillerPeriodFast.set(360);
	mSLLaunchKillerPeriod.set(840);
}

TKoopaJrSubmarineParams::TKoopaJrSubmarineParams(const char* path)
    : TSpineEnemyParams(path)
    , PARAM_INIT(killerTargetDistanceMin, 500.0f)
    , PARAM_INIT(killerTargetDistance, 500.0f)
    , PARAM_INIT(bottomHeight, 0.0f)
    , PARAM_INIT(centerZ, 0.0f)
    , PARAM_INIT(aboidKoopaFlameAngle, 0.31415927f)
    , PARAM_INIT(traceMarioAngle, 0.31415927f)
    , PARAM_INIT(mSLWavePhaseVelocity, 0.31415927f)
    , PARAM_INIT(mSLWaveAmplitudeMin, 0.37699112f)
    , PARAM_INIT(mSLWaveAmplitudeMaxLaunch, 0.37699112f)
    , PARAM_INIT(mSLWaveAmplitudeMax, 0.37699112f)
    , PARAM_INIT(mSLSwingPhaseVelocity, 0.31415927f)
    , PARAM_INIT(mSLSwingAmplitudeMin, 0.37699112f)
    , PARAM_INIT(mSLSwingAmplitudeMax, 0.37699112f)
    , PARAM_INIT(mSLRoundAngleVelocity, 0.05f)
    , PARAM_INIT(mSLRoundDistance, 1.0f)
    , PARAM_INIT(mSLAcceleration, 1.0f)
    , PARAM_INIT(mSLRotationSpeed, 1.0f)
    , PARAM_INIT(mSLSpeedMax, 8.0f)
    , PARAM_INIT(mSLKoopaJrSubmarineScale, 1.6f)
    , PARAM_INIT(mSLDamageRadius, 1000.0f)
    , PARAM_INIT(mSLDamageHeight, 4000.0f)
    , PARAM_INIT(shineKillerProbability0, 0.0f)
    , PARAM_INIT(shineKillerProbability1, 0.0f)
    , PARAM_INIT(mSLKillerIntervalFast, 30)
    , PARAM_INIT(mSLKillerInterval, 30)
{
	TParams::load(mPrmPath);
	killerTargetDistanceMin.set(500.0f);
	killerTargetDistance.set(700.0f);
	bottomHeight.set(0.0f);
	centerZ.set(200.0f);
	aboidKoopaFlameAngle.set(0.2f * JGeometry::TUtil<f32>::PI());
	traceMarioAngle.set(0.1f * JGeometry::TUtil<f32>::PI());
	mSLWavePhaseVelocity.set(0.03f * JGeometry::TUtil<f32>::PI());
	mSLWaveAmplitudeMin.set(0.04f * JGeometry::TUtil<f32>::PI());
	mSLWaveAmplitudeMaxLaunch.set(0.12f * JGeometry::TUtil<f32>::PI());
	mSLWaveAmplitudeMax.set(0.1f * JGeometry::TUtil<f32>::PI());
	mSLSwingPhaseVelocity.set(0.06f * JGeometry::TUtil<f32>::PI());
	mSLSwingAmplitudeMin.set(0.04f * JGeometry::TUtil<f32>::PI());
	mSLSwingAmplitudeMax.set(0.18f * JGeometry::TUtil<f32>::PI());
	mSLRoundAngleVelocity.set(0.12f);
	mSLRoundDistance.set(2000.0f);
	mSLAcceleration.set(1.0f);
	mSLRotationSpeed.set(1.0f);
	mSLSpeedMax.set(5.0f);
	mSLKoopaJrSubmarineScale.set(2.0f);
	mSLDamageRadius.set(240.0f);
	mSLDamageHeight.set(120.0f);
	shineKillerProbability0.set(0.5f);
	shineKillerProbability1.set(0.125f);
	mSLKillerInterval.set(90);
	mSLKillerIntervalFast.set(30);
}

int TKoopaJr_getJointIndex(int index) { return TKoopaJr_jointIndexTable[index]; }

TCallbackHitActor::TCallbackHitActor(const char* name, u32 actor_type,
                                     f32 damage_radius, f32 damage_height,
                                     THitActor* owner)
    : THitActor(name)
    , mOwner(owner)
{
	initHitActor(actor_type, 0, 0, 0.0f, 0.0f, damage_radius, damage_height);
	offHitFilter(HIT_FILTER_NO_COLLISION);
	static_cast<TIdxGroupObj*>(JDrama::TNameRefGen::search("敵グループ"))
	    ->getChildren()
	    .push_back(this);
}

BOOL TCallbackHitActor::receiveMessage(THitActor* sender, u32 message)
{
	return mOwner->receiveMessage(sender, message);
}

// fabricated: see TKoopa::pushNerve
inline void TKoopaJr::pushNerve(const TNerveBase<TLiveActor>* nerve)
{
	mSpine->pushNerve(nerve);
}

// fabricated: see TKoopa::pushNerve
inline void TKoopaJrSubmarine::pushNerve(const TNerveBase<TLiveActor>* nerve)
{
	mSpine->pushNerve(nerve);
}

TKoopaJr::TKoopaJr(const char* name)
    : TSpineEnemy(name)
    , mBathtub(nullptr)
    , mKoopa(nullptr)
    , mSubmarine(nullptr)
    , mSubmarineManager(nullptr)
    , mKillerManager(nullptr)
{
	onLiveFlag(LIVE_FLAG_UNK10);
	offLiveFlag(LIVE_FLAG_UNK100);
}

void TKoopaJr::init(TLiveManager* manager)
{
	// FAKEMATCH(frame): 8 bytes; real construct unknown
	volatile u32 pad[2]; (void)pad;
	mManager = manager;
	mManager->manageActor(this);
	mMActorKeeper = new TMActorKeeper(mManager, 1);
	mMActor       = mMActorKeeper->createMActor("koopajr_model.bmd", 0);
	mMActor->setLightType(1);
	initAnmSound();
	f32 damageHeight = getParams()->mSLDamageHeight.get();
	initHitActor(ACTOR_TYPE_BOSS_UNK28, 1, 0, 0.0f, 0.0f,
	             getParams()->mSLDamageRadius.get(), damageHeight);
	offHitFilter(HIT_FILTER_NO_COLLISION);
	mSpine->initWith(&TNerveKoopaJrWait::theNerve());
	mKillerManager = (TBathtubKillerManager*)JDrama::TNameRefGen::search(
	    "バスタブキラーマネージャー");
	mKillerManager->getActiveObjNum();
	if (mSubmarineManager == nullptr)
		mSubmarineManager
		    = (TKoopaJrSubmarineManager*)JDrama::TNameRefGen::search(
		        "クッパジュニアサブマリンマネージャー");
	mSubmarineManager->getActiveObjNum();
	f32 scale = getParams()->mSLKoopaJrScale.get();
	mScaling.set(scale, scale, scale);
	resetKoopaJr();
}

void TKoopaJr::reset()
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	TSpineEnemy::reset();
	resetKoopaJr();
}

void TKoopaJr::resetKoopaJr()
{
	mSpine->reset();
	unk150[0] = 0;
	unk150[1] = 0;
	unk150[2] = 0;
	unk150[1] = getParams()->mSLLaunchKillerPeriod.get();
	unk150[2] = 0;
}

void TKoopaJr::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (mSubmarine == nullptr) {
		mSubmarine = (TKoopaJrSubmarine*)mSubmarineManager->getObj(0);
		mSubmarine->setKoopaJr(this);
	}

	if (mKoopa == nullptr) {
		TEnemyManager* manager = static_cast<TEnemyManager*>(
		    JDrama::TNameRefGen::search("クッパマネージャー"));
		mKoopa = (TKoopa*)manager->getObj(0);
	}

	if (mBathtub == nullptr)
		mBathtub
		    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));

	// TODO: inlining checkSubmarineSwing() here puts startDamageNerve() one
	// inline level too deep, the original must have spelled the check out
	if (cue & CUE_CALC_ANIM
	    && !(mSubmarine->unk190
	         < 0.5f * mSubmarine->getParams()->mSLSwingAmplitudeMax.get()))
		damageKoopaJr();

	if (cue & CUE_MOVE) {
		updateTimers();
		checkNerve();
	}

	TSpineEnemy::perform(cue, graphics);
}

void TKoopaJr::calcRootMatrix()
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	J3DModel* model = getModel();
	if (mBathtub->unk29A) {
		getModel()->setBaseTRMtx(mBathtub->getKoopaJrMtxInDemo());
	} else {
		mSubmarine->getJointTransByIndex(TKoopaJr_jointIndexTable[0],
		                                 &mPosition);
		MsMtxSetXYZRPH(model->getBaseTRMtx(), mPosition.x, mPosition.y,
		               mPosition.z, mRotation.x, mRotation.y, mRotation.z);
	}
	model->setBaseScale(mScaling);
}

// TODO: unknown contents, LimitKoopaJr's copy is empty too
void TKoopaJr::startKoopaJrMessage(u32 message) { }

void TKoopaJr::emitKoopaJrEffects() { }

void TKoopaJr::setAnimationIndex(int index)
{
	getMActor()->setBckFromIndex(index);
	setAnmSound(getBas(index));
}

void TKoopaJr::updateTimers()
{
	for (int i = 0; i < 3; i++)
		if (unk150[i] > 0)
			unk150[i]--;
}

const char** TKoopaJr::getBasNameTable() const { return koopajr_bastable; }

BOOL TKoopaJr::receiveMessage(THitActor* sender, u32 message)
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	if (message == HIT_MESSAGE_SPRAYED_BY_WATER) {
		gpMarioParticleManager->emit(0xE7, &sender->mPosition, 0, nullptr);
		gpMSound->startSoundSet(MSD_SE_EN_COMMON_W_HIT_OK, &mPosition, 0, 0.0f,
		                        0, 0, 4);
		damageKoopaJr();
		return TRUE;
	}
	return FALSE;
}

void TKoopaJr::damageKoopaJr()
{
	unk150[0] = getParams()->mSLDamagePeriod.get();
	startDamageNerve();
}

void TKoopaJr::checkSubmarineSwing()
{
	if (mSubmarine->unk190
	    < 0.5f * mSubmarine->getParams()->mSLSwingAmplitudeMax.get())
		return;

	damageKoopaJr();
}

void TKoopaJr::startDamageNerve()
{
	if (mSpine->getCurrentNerve() == &TNerveKoopaJrWait::theNerve())
		mSpine->pushNerve(&TNerveKoopaJrDamage::theNerve());

	if (mSpine->getCurrentNerve() == &TNerveKoopaJrLaunch::theNerve()
	    || mSpine->getCurrentNerve() == &TNerveKoopaJrYahoo::theNerve())
		mSpine->setNext(&TNerveKoopaJrDamage::theNerve());
}

void TKoopaJr::checkNerve()
{
	if (mBathtub->unk29A)
		return;

	// TODO: dead code, the demo check above already returned
	if (mBathtub->unk29A) {
		if (mSpine->getCurrentNerve() != &TNerveKoopaJrDemo::theNerve())
			mSpine->pushNerve(&TNerveKoopaJrDemo::theNerve());
	}

	if (mSpine->getCurrentNerve() == &TNerveKoopaJrWait::theNerve()) {
		checkNerveKillerLaunchNormal();
		checkNerveKillerLaunchFast();
		checkNerveKillerHit();
	}

	JGeometry::TVec3<f32> toMario;
	toMario.sub(SMS_GetMarioPos(), mPosition);
	toMario.y = 0.0f;
	// TODO: the original copies toMario only once (as in makeDirection
	// below), but the makeDirection form puts checkNerve over the inline
	// statement limit and perform no longer inlines it
	TDirectionCalc direction(toMario);
	mRotation.y = TDirectionCalc::r2d(direction.unk0);
}

void TKoopaJr::checkNerveKillerLaunchNormal()
{
	if (unk150[1] > 0)
		return;

	int num = mBathtub->getNumKillerLaunchable();
	if (num == 0)
		return;

	mSubmarine->prepareKillerLaunch(num);
	pushNerve(&TNerveKoopaJrLaunch::theNerve());
	mSubmarine->pushNerve(
	    &TNerveKoopaJrSubmarineCannonOpenClose::theNerve());
	mSubmarine->setAnimationIndex(0);
}

void TKoopaJr::checkNerveKillerLaunchFast()
{
	if (unk150[2] > 0)
		return;

	int num = mBathtub->getNumKillerBurstable();
	if (num == 0)
		return;

	mSubmarine->prepareKillerLaunchFast(num);
	pushNerve(&TNerveKoopaJrLaunch::theNerve());
	mSubmarine->pushNerve(
	    &TNerveKoopaJrSubmarineCannonOpenClose::theNerve());
	mSubmarine->setAnimationIndex(0);
}

void TKoopaJr::checkNerveKillerHit()
{
	for (int i = 0; i < mKillerManager->getActiveObjNum(); ++i) {
		TBathtubKiller* killer = (TBathtubKiller*)mKillerManager->getObj(i);
		if (killer->unk21C == 1) {
			pushNerve(&TNerveKoopaJrYahoo::theNerve());
			return;
		}
	}
}

f32 TKoopaJr::getBathtubY() { return (*mBathtub->getRootJointMtx())[1][3]; }

DEFINE_NERVE(TNerveKoopaJrWait, TLiveActor)
{
	TKoopaJr* koopaJr = (TKoopaJr*)spine->getBody();
	if (spine->getTime() == 0) {
		koopaJr->setAnimationIndex(2);
		koopaJr->unk150[1]
		    = koopaJr->getParams()->mSLLaunchKillerPeriod.get();
		koopaJr->unk150[2]
		    = koopaJr->getParams()->mSLLaunchKillerPeriodFast.get();
	}
	return FALSE;
}

DEFINE_NERVE(TNerveKoopaJrDamage, TLiveActor)
{
	TKoopaJr* koopaJr = (TKoopaJr*)spine->getBody();
	if (spine->getTime() == 0)
		koopaJr->setAnimationIndex(0);

	if (koopaJr->unk150[0] <= 0
	    && koopaJr->getMActor()->isCurAnmAlreadyEnd(0))
		return TRUE;

	return FALSE;
}

DEFINE_NERVE(TNerveKoopaJrDemo, TLiveActor)
{
	TKoopaJr* koopaJr = (TKoopaJr*)spine->getBody();
	if (spine->getTime() == 0)
		koopaJr->setAnimationIndex(0);

	return FALSE;
}

DEFINE_NERVE(TNerveKoopaJrLaunch, TLiveActor)
{
	TKoopaJr* koopaJr = (TKoopaJr*)spine->getBody();
	if (spine->getTime() == 0)
		koopaJr->setAnimationIndex(1);

	if (koopaJr->getMActor()->isCurAnmAlreadyEnd(0))
		return TRUE;

	return FALSE;
}

DEFINE_NERVE(TNerveKoopaJrYahoo, TLiveActor)
{
	TKoopaJr* koopaJr = (TKoopaJr*)spine->getBody();
	if (spine->getTime() == 0)
		koopaJr->setAnimationIndex(3);

	if (koopaJr->getMActor()->isCurAnmAlreadyEnd(0))
		return TRUE;

	return FALSE;
}

TKoopaJrManager::TKoopaJrManager(const char* name)
    : TEnemyManager(name)
{
}

void TKoopaJrManager::createModelData()
{
	static const TModelDataLoadEntry entry[] = {
		{ "koopajr_model.bmd", 0x54220000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

void TKoopaJrManager::load(JSUMemoryInputStream& stream)
{
	TEnemyManager::load(stream);
	unk38 = new TKoopaJrParams("/enemy/koopajr.prm");
}

void TKoopaJrManager::loadAfter()
{
	static const char* onetimeFilenames[1] = {
		"/scene/koopajr/jpa/ms_koopajr_killer.jpa",
	};
	for (int i = 0; i < 1; ++i)
		SMS_LoadParticle(onetimeFilenames[i], 0xEF + i);
}

TSpineEnemy* TKoopaJrManager::createEnemyInstance() { return nullptr; }

// TODO: the original keeps this constructor out of line (it is called from
// TKoopaJrSubmarineManager::createEnemyInstance)
#pragma dont_inline on
TKoopaJrSubmarine::TKoopaJrSubmarine(const char* name)
    : TSpineEnemy(name)
    , unk188(0.0f)
    , mKoopaJr(nullptr)
{
	offLiveFlag(LIVE_FLAG_UNK10);
	offLiveFlag(LIVE_FLAG_UNK100);
}
#pragma dont_inline off

void TKoopaJrSubmarine::init(TLiveManager* manager)
{
	mManager = manager;
	mManager->manageActor(this);
	initAnmSound();
	f32 damageHeight = getParams()->mSLDamageHeight.get();
	initHitActor(ACTOR_TYPE_BOSS_UNK20, 0, 0, 0.0f, 0.0f,
	             getParams()->mSLDamageRadius.get(), damageHeight);
	onHitFilter(HIT_FILTER_NO_COLLISION);

	mRearBody = new TCallbackHitActor(
	    "サブマリンリアボディ", ACTOR_TYPE_BOSS_UNK2D,
	    getParams()->mSLDamageRadius.get(), getParams()->mSLDamageHeight.get(),
	    this);
	mFrontBody = new TCallbackHitActor(
	    "サブマリンフロントボディ", ACTOR_TYPE_BOSS_UNK2D,
	    getParams()->mSLDamageRadius.get(), getParams()->mSLDamageHeight.get(),
	    this);

	mMActorKeeper = new TMActorKeeper(mManager, 1);
	mMActor       = mMActorKeeper->createMActor("LastKoopaJrSubmarine.bmd", 0);
	mMActor->setLightType(1);
	mSpine->initWith(&TNerveKoopaJrSubmarineWait::theNerve());

	JUTNameTab* jointNames = getModel()->getModelData()->getJointName();
	for (int i = 0; i < 5; ++i)
		TKoopaJr_jointIndexTable[i]
		    = jointNames->getIndex(TKoopaJr_jointNameTable[i]);

	f32 scale = getParams()->mSLKoopaJrSubmarineScale.get();
	mScaling.set(scale, scale, scale);
	mBathtubBinder = new TBathtubBinder;
	mBinder        = mBathtubBinder;
	resetKoopaJrSubmarine();
}

void TKoopaJrSubmarine::reset()
{
	TSpineEnemy::reset();
	resetKoopaJrSubmarine();
}

void TKoopaJrSubmarine::resetKoopaJrSubmarine()
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	mSpine->reset();
	unk150[0] = 0;
	setAnimationIndex(0);
	unk188 = getMActor()->getFrameCtrl(0)->getRate();
	unk180 = 0;
	unk184 = 0;
	for (int i = 0; i < 8; ++i)
		unk178[i] = 0;
	unk154      = 0.0f;
	unk158      = 0.0f;
	unk15C      = 0.0f;
	unk160      = 1.0f;
	unk16C.unk0 = 0.0f;
	unk164.unk0 = 0.0f;
	unk170      = 0;
	unk18C      = 0;
	unk190      = 0.0f;
	unk194      = 0.0f;
	unk198      = 0.0f;
	unk19C      = 0.0f;
	f32 bottomHeight = getParams()->bottomHeight.get();
	mBathtubBinder->init(150.0f, 100.0f, 150.0f, 100.0f, bottomHeight);
}

void TKoopaJrSubmarine::perform(u32 cue, JDrama::TGraphics* graphics)
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	if (cue & CUE_CALC_ANIM) {
		checkKillerLaunch();
		makeCollisionPositions();
		moveSwing();
	}

	if (cue & CUE_MOVE) {
		updateTimers();
		checkNerve();
	}

	TSpineEnemy::perform(cue, graphics);
	mRearBody->perform(cue, graphics);
	mFrontBody->perform(cue, graphics);
}

// TODO: the original keeps this out of line (it is called from perform)
#pragma dont_inline on
void TKoopaJrSubmarine::makeCollisionPositions()
{
    // FAKEMATCH(frame): 40 bytes; real construct unknown
    volatile u32 pad[10]; (void)pad;
	JGeometry::TVec3<f32> center(0.0f, 0.0f, 0.0f);
	for (int i = 0; i < 2; ++i) {
		MtxPtr mtx = getModel()->getAnmMtx(TKoopaJr_getJointIndex(i + 3));
		center.x += mtx[0][3];
		center.y += mtx[1][3];
		center.z += mtx[2][3];
	}
	center *= 0.5f;
	mRearBody->mPosition.set(center);
	getJointTransByIndex(TKoopaJr_getJointIndex(0), &mFrontBody->mPosition);
}
#pragma dont_inline off

void TKoopaJrSubmarine::moveSwing()
{
	if (unk18C) {
		unk18C = 0;
		unk190 += 0.06283186f;
		unk190 = KJMin(getParams()->mSLSwingAmplitudeMax.get(), unk190);
	}

	unk190 -= 0.009424779f;
	unk190 = JGeometry::max(getParams()->mSLSwingAmplitudeMin.get(), unk190);
	if (unk190 <= 0.0f)
		unk194 = 0.0f;

	unk194 = KJWrapFmod(unk194 + getParams()->mSLSwingPhaseVelocity.get(), 0.0f,
	                    6.2831855f);

	f32 speedRate = mVelocity.length() / getParams()->mSLSpeedMax.get();

	if (unk150[0] > 0) {
		unk198 += 0.03141593f;
		unk198 = KJMin(getParams()->mSLWaveAmplitudeMaxLaunch.get(), unk198);
	}

	if (speedRate > 0.5f) {
		unk198 += 0.03141593f;
		unk198 = KJMin(getParams()->mSLWaveAmplitudeMax.get(), unk198);
	}

	unk198 -= 0.018849557f;
	unk198 = JGeometry::max(getParams()->mSLWaveAmplitudeMin.get(), unk198);
	if (unk198 <= 0.0f)
		unk19C = 0.0f;

	unk19C = KJWrapFmod(unk19C + getParams()->mSLWavePhaseVelocity.get(), 0.0f,
	                    6.2831855f);
}

f32 TKoopaJrSubmarine::getSwingAngle() { return unk190 * sin(unk194); }

f32 TKoopaJrSubmarine::getWaveAngle() { return unk198 * sin(unk19C); }

void TKoopaJrSubmarine::bind()
{
	JGeometry::TVec3<f32> next = mPosition;
	next += mPositionDelta;
	next += mVelocity;
	mPositionDelta = next - mPosition;
	mBathtubBinder->bind(this);
}

void TKoopaJrSubmarine::calcRootMatrix()
{
	if (mKoopaJr->mBathtub->unk29A) {
		getModel()->setBaseTRMtx(mKoopaJr->mBathtub->getSubmarineMtxInDemo());
	} else {
		JGeometry::TQuat4<f32> swing;
		swing.setEulerZ(getSwingAngle());
		JGeometry::TQuat4<f32> wave;
		wave.setEulerX(getWaveAngle());
		JGeometry::TQuat4<f32> direction;
		direction.setEulerY(unk16C.unk0);

		JGeometry::TQuat4<f32> rotation;
		rotation.mul(direction, swing);
		rotation.mul(wave);

		JGeometry::TVec3<f32> center(0.0f, 0.0f, getParams()->centerZ.get());
		TPosition3f offset;
		offset.translation(center);

		JGeometry::TVec3<f32> translation = center;
		translation.negate();
		translation += mPosition;

		TPosition3f mtx;
		mtx.setQT(rotation, translation);
		mtx.concat(offset);
		getModel()->setBaseTRMtx(mtx);
	}

	getModel()->setBaseScale(mScaling);
}

BOOL TKoopaJrSubmarine::receiveMessage(THitActor* sender, u32 message)
{
	if (message == HIT_MESSAGE_SPRAYED_BY_WATER) {
		gpMarioParticleManager->emit(0xE7, &sender->mPosition, 0, nullptr);
		gpMSound->startSoundSet(MSD_SE_EN_COMMON_W_HIT_OK, &mPosition, 0, 0.0f,
		                        0, 0, 4);
		damageKoopaJrSubmarine();
		return TRUE;
	}
	return FALSE;
}

void TKoopaJrSubmarine::damageKoopaJrSubmarine() { unk18C = 1; }

void TKoopaJrSubmarine::setAnimationIndex(int index)
{
	getMActor()->setBckFromIndex(index);
	setAnmSound(getBas(index));
}

void TKoopaJrSubmarine::prepareKillerLaunch(int num)
{
	if (num > 8)
		num = 8;

	unk180 = 0;
	unk184 = num;
	for (int i = 0; i < unk184; ++i)
		unk178[i] = 0;

	if (appearShineKiller(unk184))
		unk178[unk184 - 1] = 1;
}

void TKoopaJrSubmarine::prepareKillerLaunchFast(int num)
{
	if (num > 8)
		num = 8;

	unk180 = 0;
	unk184 = num;
	for (int i = 0; i < unk184; ++i)
		unk178[i] = 2;

	if (appearShineKiller(unk184))
		unk178[unk184 - 1] = 1;
}

bool TKoopaJrSubmarine::appearShineKiller(int)
{
	f32 probability;
	if (SMS_GetMarioWaterGun()->getCurrentWater() == 0) {
		probability = 0.5f;
	} else if (mKoopaJr->mKillerManager->unk60
	           == TFlagManager::smInstance->getFlag(0x20001)) {
		probability = 0.5f;
	} else {
		s32 maxWater = SMS_GetMarioWaterGun()->getMaxWater();
		s32 water    = SMS_GetMarioWaterGun()->getCurrentWater();
		f32 prob0    = getParams()->shineKillerProbability0.get();
		probability
		    = ((f32)water / (f32)maxWater)
		          * (getParams()->shineKillerProbability1.get() - prob0)
		      + prob0;
	}

	bool result = false;
	if ((1.0f / 32768.0f) * rand() < probability)
		result = true;
	return result;
}

void TKoopaJrSubmarine::checkKillerLaunch()
{
	if (mSpine->getCurrentNerve()
	        == &TNerveKoopaJrSubmarineLaunchKiller::theNerve()
	    && unk180 < unk184 && unk150[0] <= 0) {
		launchKiller();
		if ((s32)unk178[unk180] == 2)
			unk150[0] = getParams()->mSLKillerIntervalFast.get();
		else
			unk150[0] = getParams()->mSLKillerInterval.get();
		++unk180;
	}
}

void TKoopaJrSubmarine::launchKiller()
{
	int cannon = unk180 % 4;
	TBathtubKiller* killer
	    = (TBathtubKiller*)mKoopaJr->mKillerManager->getDeadEnemy();
	if (killer == nullptr)
		return;

	killer->unk194 = unk178[unk180];
	killer->reset();

	int joint  = cannon + 1;
	MtxPtr mtx = getModel()->getAnmMtx(TKoopaJr_getJointIndex(joint));
	killer->mPosition.set(mtx[0][3], mtx[1][3], mtx[2][3]);
	JGeometry::TVec3<f32> dir;
	dir.x = mtx[0][2];
	dir.y = mtx[1][2];
	dir.z = mtx[2][2];
	makeKillerVelocity(killer, dir);
	SMSGetMSound()->startSoundActor(MSD_SE_EN_KILLER_FIRE, &killer->mPosition,
	                                0, nullptr, 0, 4);
}

void TKoopaJrSubmarine::makeKillerVelocity(TBathtubKiller* killer,
                                           JGeometry::TVec3<f32> velocity)
{
	if ((s32)unk178[unk180] == 2) {
		velocity.set(0.0f, 1.0f, 0.0f);

		JGeometry::TVec3<f32> marioPos = SMS_GetMarioPos();
		JGeometry::TVec3<f32> toMario;
		toMario.sub(marioPos, killer->mPosition);
		toMario.y = 0.0f;
		toMario.normalize();

		JGeometry::TVec3<f32> axis;
		f32 angle = 0.62831855f;
		axis.cross(velocity, toMario);
		axis.normalize();

		JGeometry::TQuat4<f32> rotation;
		rotation.setRotate(axis, 0.62831855f);
		rotation.rotate(velocity, velocity);

		axis = toMario;

		int cannon = unk180 % 4;
		if (cannon == 0)
			angle = -TDirectionCalc::d2r(9.0f);
		else if (cannon == 1)
			angle = TDirectionCalc::d2r(9.0f);
		else if (cannon == 2)
			angle = -TDirectionCalc::d2r(18.0f);
		else if (cannon == 3)
			angle = TDirectionCalc::d2r(18.0f);

		rotation.setRotate(axis, angle);
		rotation.rotate(velocity, velocity);
		velocity.normalize();
		velocity.scale(killer->unk198.unkC);
	} else {
		velocity.normalize();
		velocity.scale(killer->unk198.unkC);

		JGeometry::TVec3<f32> target = SMS_GetMarioPos();
		target.y = mKoopaJr->getBathtubY();

		JGeometry::TVec3<f32> toTarget(target.x - killer->mPosition.x, 0.0f,
		                               target.z - killer->mPosition.z);
		f32 distance = toTarget.length();
		f32 offset   = distance - getParams()->killerTargetDistance.get();
		offset = JGeometry::max(offset, getParams()->killerTargetDistanceMin.get());
		toTarget.normalize();
		toTarget.scale(offset);

		JGeometry::TVec3<f32> landing;
		landing.add(killer->mPosition, toTarget);
		landing.y = target.y;
		velocity  = calcVelocityToJumpToY(
            landing, velocity.y,
            killer->getSaveParam2()->mSLFlyingGravityY.get());
	}

	killer->makeInitialVelocity(velocity);
}

void TKoopaJrSubmarine::emitKoopaJrSubmarineEffects() { }

void TKoopaJrSubmarine::updateTimers()
{
	for (int i = 0; i < 1; i++)
		if (unk150[i] > 0)
			unk150[i]--;
}

void TKoopaJrSubmarine::setKoopaJr(TKoopaJr* koopa_jr) { mKoopaJr = koopa_jr; }

const char** TKoopaJrSubmarine::getBasNameTable() const
{
	return koopajrsubmarine_bastable;
}

void TKoopaJrSubmarine::makeRelativeAngle()
{
	f32 flameDir
	    = TDirectionCalc::d2r(mKoopaJr->mKoopa->getFlameDirDegree());
	f32 flameDiff = KJAbsNearer(unk164, flameDir);

	JGeometry::TVec3<f32> toMario;
	toMario.sub(SMS_GetMarioPos(), mKoopaJr->mBathtub->mPosition);
	toMario.y = 0.0f;
	TDirectionCalc marioDir;
	marioDir.makeDirection(toMario);

	f32 marioDiff = KJAbsNearer(unk164, marioDir.unk0);
	f32 target    = unk164.unk0;

	if (mKoopaJr->mKoopa->isFlaming()
	    && flameDiff <= getParams()->aboidKoopaFlameAngle.get())
		target = flameDir + JGeometry::TUtil<f32>::PI();
	else if (marioDiff > getParams()->traceMarioAngle.get())
		target = marioDir.unk0;

	unk164.unk0 = KJTurn(unk164, target,
	                     0.017453294f
	                         * getParams()->mSLRoundAngleVelocity.get());
}

void TKoopaJrSubmarine::makeRoundVelocity()
{
	JGeometry::TVec3<f32> offset = unk164.calcDirectionVector();
	offset.scale(unk168);

	const JGeometry::TVec3<f32>& center = mKoopaJr->mBathtub->mPosition;
	JGeometry::TVec3<f32> toTarget(center.x + offset.x - mPosition.x, 0.0f,
	                               center.z + offset.z - mPosition.z);
	if (toTarget.length() < 100.0f) {
		unk170 = 1;
		return;
	}

	unk170 = 0;
	toTarget.normalize();
	toTarget.scale(getParams()->mSLAcceleration.get());
	mVelocity += toTarget;

	f32 speed    = mVelocity.length();
	f32 speedMax = getParams()->mSLSpeedMax.get();
	if (speed > speedMax) {
		mVelocity.normalize();
		mVelocity.scale(speedMax);
	}
}

void TKoopaJrSubmarine::makeDirection()
{
	JGeometry::TVec3<f32> velocity = mVelocity;
	velocity.normalize();
	TDirectionCalc direction;
	direction.makeDirection(velocity);
	unk16C.unk0 = unk16C.calcTurnDirection(
	    direction.unk0,
	    TDirectionCalc::d2r(getParams()->mSLRotationSpeed.get()));
}

void TKoopaJrSubmarine::checkNerve()
{
	if (mKoopaJr->mSpine->getCurrentNerve() == &TNerveKoopaJrWait::theNerve()) {
		makeRelativeAngle();
		unk168 = getParams()->mSLRoundDistance.get();
		makeRoundVelocity();
	}

	mVelocity.scale(0.95f);

	if (!unk170)
		makeDirection();

	if (mSpine->getCurrentNerve() != &TNerveKoopaJrSubmarineWait::theNerve()) {
		if (mSpine->getCurrentNerve()
		    == &TNerveKoopaJrSubmarineCannonOpenClose::theNerve()) {
			if (unk180 == 0) {
				J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(0);
				if (ctrl->checkPass(30.0f)) {
					ctrl->setRate(0.0f);
					pushNerve(
					    &TNerveKoopaJrSubmarineLaunchKiller::theNerve());
				}
			}
		} else if (mSpine->getCurrentNerve()
		           == &TNerveKoopaJrSubmarineLaunchKiller::theNerve()) {
			emitKoopaJrSubmarineEffects();
		}
	}
}

DEFINE_NERVE(TNerveKoopaJrSubmarineWait, TLiveActor)
{
	TKoopaJrSubmarine* submarine = (TKoopaJrSubmarine*)spine->getBody();
	if (spine->getTime() == 0) {
		submarine->setAnimationIndex(0);
		submarine->getMActor()->getFrameCtrl(0)->setRate(0.0f);
	}
	return FALSE;
}

DEFINE_NERVE(TNerveKoopaJrSubmarineCannonOpenClose, TLiveActor)
{
	TKoopaJrSubmarine* submarine = (TKoopaJrSubmarine*)spine->getBody();
	if (spine->getTime() == 0) {
		J3DFrameCtrl* ctrl = submarine->getMActor()->getFrameCtrl(0);
		ctrl->setRate(submarine->unk188);
	}

	if (submarine->getMActor()->isCurAnmAlreadyEnd(0))
		return TRUE;

	return FALSE;
}

DEFINE_NERVE(TNerveKoopaJrSubmarineLaunchKiller, TLiveActor)
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	TKoopaJrSubmarine* submarine = (TKoopaJrSubmarine*)spine->getBody();
	if (submarine->unk180 == submarine->unk184 && submarine->unk150[0] <= 0) {
		J3DFrameCtrl* ctrl = submarine->getMActor()->getFrameCtrl(0);
		ctrl->setRate(submarine->unk188);
		return TRUE;
	}
	return FALSE;
}

TKoopaJrSubmarineManager::TKoopaJrSubmarineManager(const char* name)
    : TEnemyManager(name)
{
}

void TKoopaJrSubmarineManager::createModelData()
{
	static const TModelDataLoadEntry entry[] = {
		{ "LastKoopaJrSubmarine.bmd", 0x54220000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

void TKoopaJrSubmarineManager::load(JSUMemoryInputStream& stream)
{
	getActiveObjNum();
	TEnemyManager::load(stream);
	unk38 = new TKoopaJrSubmarineParams("/enemy/koopajrsubmarine.prm");
	getActiveObjNum();
}

void TKoopaJrSubmarineManager::loadAfter()
{
	TEnemyManager::loadAfter();
	getActiveObjNum();
}

TSpineEnemy* TKoopaJrSubmarineManager::createEnemyInstance()
{
	return new TKoopaJrSubmarine;
}
