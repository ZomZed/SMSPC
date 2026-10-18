#include <Enemy/BathtubPeach.hpp>
#include <Enemy/EnemyManager.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/MActorAnm.hpp>
#include <MoveBG/MapObjCorona.hpp>
#include <Player/MarioAccess.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <System/Application.hpp>
#include <Strategic/Spine.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/Strategy.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JGeometry/JGVec2.hpp>
#include <math.h>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>
#include <Map/MapCollisionEntry.hpp>

// NOTE: this TU is compiled with -inline deferred, so functions are emitted in
// the reverse order of their definitions here.

class TNervePeachEscape : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNervePeachEscape& theNerve()
	{
		static TNervePeachEscape nerve;
		return nerve;
	}
};

class TNervePeachStagger : public TNerveBase<TLiveActor> {
public:
	virtual BOOL execute(TSpineBase<TLiveActor>* spine) const;

	static const TNervePeachStagger& theNerve()
	{
		static TNervePeachStagger nerve;
		return nerve;
	}
};

static const char* bathtubpeach_bastable[] = {
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	"/scene/bathtubpeach/bas/peach_wait.bas",
	nullptr,
};

BOOL TNervePeachStagger::execute(TSpineBase<TLiveActor>* spine) const
{
	TBathtubPeach* peach = (TBathtubPeach*)spine->getBody();
	peach->changeAnm(0, 0, 2.0f);
	if (peach->getMActor()->curAnmEndsNext(0, nullptr))
		return true;
	return false;
}

static inline void* BPGetSaveParam1(TEnemyManager* m) { return m->getSaveParam(); }
static inline void* BPGetSaveParam2(TEnemyManager* m) { return BPGetSaveParam1(m); }

inline void TBathtubPeach::goTo(const JGeometry::TVec3<f32>& pos)
{
	JGeometry::TVec2<f32> delta(pos.x - mPosition.x, pos.z - mPosition.z);
	f32 speed = getParam()->speed.get();
	if (delta.x * delta.x + delta.y * delta.y >= speed * speed) {
		delta.setLength(
		    ((TBathtubPeachParams*)BPGetSaveParam2((TEnemyManager*)mManager))
		        ->speed.get());
	}
	mPosition.x += delta.x;
	mPosition.z += delta.y;
}

inline void TBathtubPeach::faceTo(const JGeometry::TVec3<f32>& pos, f32 max_turn)
{
	f32 dz = pos.z - mPosition.z;
	f32 dx = pos.x - mPosition.x;
	if (!(dx * dx + dz * dz <= JGeometry::TUtil<f32>::epsilon())) {
		s16 angle  = matan(dz, dx);
		f32 deg    = (360.0f / 65536.0f) * angle;
		f32 target = deg - 90.0f;
		f32 diff
		    = -180.0f
		      + std::fmodf(360.0f + ((target - mRotation.y) - -180.0f),
		                   360.0f);
		if (diff < -max_turn) {
			mRotation.y
			    = -180.0f
			      + std::fmodf(360.0f + ((mRotation.y - max_turn) - -180.0f),
			                   360.0f);
		} else if (diff > max_turn) {
			mRotation.y
			    = -180.0f
			      + std::fmodf(360.0f + ((mRotation.y + max_turn) - -180.0f),
			                   360.0f);
		} else {
			mRotation.y = target;
		}
	}
}

static inline void peachGoTo(TBathtubPeach* p, const JGeometry::TVec3<f32>& g)
{
	p->goTo(g);
}

BOOL TNervePeachEscape::execute(TSpineBase<TLiveActor>* spine) const
{
	TBathtubPeach* peach = (TBathtubPeach*)spine->getBody();
	TBathtub* bathtub
	    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));

	// no escaping during the bathtub demo
	if (bathtub->unk29A != 0)
		return false;

	if (!(spine->getTime() & 4)) {
		if (bathtub->mBathtubData.unk64 != 0)
			spine->pushNerve(&TNervePeachStagger::theNerve());
		return false;
	}

	peach->changeAnm(1, 1, 2.0f);

	MtxPtr tubMtx = *bathtub->getRootJointMtx();
	f32 tubX      = tubMtx[0][3];
	f32 tubZ      = tubMtx[2][3];

	// angle of mario and of peach around the middle of the bathtub
	JGeometry::TVec3<f32> marioPos = *gpMarioPos;
	f32 marioAngle
	    = (360.0f / 65536.0f) * matan(marioPos.z - tubZ, marioPos.x - tubX);
	s16 peachAngle = matan(peach->mPosition.z - tubZ, peach->mPosition.x - tubX);
	f32 peachDeg   = (360.0f / 65536.0f) * peachAngle;

	// TODO: the original calls JGeometry::TUtil<f32>::mod here, which our
	// JGeometry headers do not declare yet (same body as std::fmodf)
	f32 target;
	if (-180.0f
	        + std::fmodf(360.0f + ((peachDeg - marioAngle) - -180.0f), 360.0f)
	    < 0.0f) {
		target = -180.0f
		         + std::fmodf(
		             360.0f
		                 + ((marioAngle - peach->getParam()->angle.get())
		                    - -180.0f),
		             360.0f);
	} else {
		target = -180.0f
		         + std::fmodf(
		             360.0f
		                 + ((marioAngle + peach->getParam()->angle.get())
		                    - -180.0f),
		             360.0f);
	}

	f32 radius = peach->getParam()->radius.get();
	JGeometry::TVec3<f32> goal(radius * MsSin(target) + tubX, 0.0f,
	                           radius * MsCos(target) + tubZ);
	peachGoTo(peach, goal);
	peach->faceTo(*gpMarioPos, peach->getParam()->turnSpeed2.get());
	return false;
}

TBathtubPeach::TBathtubPeach(const char* name)
    : TSpineEnemy(name)
{
	onLiveFlag(LIVE_FLAG_AIRBORNE);
	offLiveFlag(LIVE_FLAG_UNK100);
	offLiveFlag(LIVE_FLAG_UNK10);
}

void TBathtubPeach::changeAnm(int bck, int btp, f32 speed)
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

const char** TBathtubPeach::getBasNameTable() const
{
	return bathtubpeach_bastable;
}

void TBathtubPeach::init(TLiveManager* manager)
{
	TSpineEnemy::init(manager);
	mSpine->initWith(&TNervePeachEscape::theNerve());
	initAnmSound();
	reset();
	mScaling.set(2.0f, 2.0f, 2.0f);
}

void TBathtubPeach::reset()
{
	mPosition.scale(0.21f);
	mBathtubBinder.init(50.0f, 50.0f, 50.0f, 50.0f, 0.0f);
	unk130 = 0;
	mScaling.set(1.5f, 1.5f, 1.5f);
	mBinder = &mBathtubBinder;
	TSpineEnemy::reset();
	changeAnm(1, 1, 2.0f);
}

void TBathtubPeach::perform(u32 cue, JDrama::TGraphics* graphics)
{
	TSpineEnemy::perform(cue, graphics);
}

Mtx* TBathtubPeach::getRootJointMtx() const
{
	return (Mtx*)getModel()->getBaseTRMtx();
}

BOOL TBathtubPeach::receiveMessage(THitActor* sender, u32 message)
{
	return TSpineEnemy::receiveMessage(sender, message);
}

void TBathtubPeach::calcRootMatrix()
{
	TBathtub* bathtub
	    = static_cast<TBathtub*>(JDrama::TNameRefGen::search("バスタブ"));
	if (bathtub != nullptr && bathtub->unk29A != 0) {
		MtxPtr dst = getModel()->getBaseTRMtx();
		MTXCopy(bathtub->getPeachMtxInDemo(), dst);
		return;
	}
	TLiveActor::calcRootMatrix();
}

TBathtubPeachParams* TBathtubPeach::getParam() const
{
	return (TBathtubPeachParams*)((TEnemyManager*)mManager)->unk38;
}

TBathtubPeachManager::TBathtubPeachManager(const char* name)
    : TEnemyManager(name)
{
}

TSpineEnemy* TBathtubPeachManager::createEnemyInstance() { return nullptr; }

void TBathtubPeachManager::createModelData()
{
	static TModelDataLoadEntry entry[] = {
		{ "ahiru_peach.bmd", 0x14240000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

void TBathtubPeachManager::load(JSUMemoryInputStream& stream)
{
	TEnemyManager::load(stream);
	unk38 = new TBathtubPeachParams("/enemy/bathtubpeach.prm");
}
