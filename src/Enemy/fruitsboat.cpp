#include <Enemy/FruitsBoat.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/Graph.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/MActorData.hpp>
#include <Map/Map.hpp>
#include <Map/MapCollisionManager.hpp>
#include <Map/MapData.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/ShadowUtil.hpp>
#include <MoveBG/MapObjWave.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <MSound/SoundEffects.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/Yoshi.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/question.hpp>
#include <System/Application.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTransform.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <dolphin/mtx.h>
#include <string.h>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

TFruitsBoatParams::TFruitsBoatParams(const char* path)
    : TSpineEnemyParams(path)
    , PARAM_INIT(mSLMoveSpeed, 4.0f)
    , PARAM_INIT(mSLRotSpeed, 0.1f)
    , PARAM_INIT(mSLBckMoveSpeed, 0.2f)
{
	TParams::load(mPrmPath);
}

TFruitsBoat::TFruitsBoat(const char* name)
    : TSpineEnemy(name)
    , unk150(0)
    , mShadowRadiusX(800.0f)
    , mShadowRadiusZ(800.0f)
    , mTrackAnm(nullptr)
    , mTrackCtrl(nullptr)
    , mTiltAxis(1.0f, 0.0f, 0.0f)
    , mTiltAngle(0.0f)
    , mTiltSpeed(0.0f)
{
	onLiveFlag(LIVE_FLAG_UNK10);
}

int TFruitsBoat::setBckTrack(const char* name)
{
	MActorAnmDataEach<J3DAnmTransformKey>* anms
	    = mManager->getMActorAnmData()->getUnk2C();
	for (int i = 0; i < anms->getAnmNum(); ++i) {
		if (strcmp(name, anms->getName(i)) == 0) {
			mTrackAnm  = anms->getAnmPtr(i);
			mTrackCtrl = new J3DFrameCtrl;
			mTrackCtrl->init(mTrackAnm->getFrameMax());
			mTrackCtrl->setAttribute(mTrackAnm->getAttribute());
			f32 speed = getParams()->mSLBckMoveSpeed.get();
			mTrackCtrl->setRate(speed * SMSGetAnmFrameRate());
			return 0;
		}
	}
	return -1;
}

void TFruitsBoat::setJumpReaction()
{
	const char* name;
	switch (getBoatType()) {
	case 0:
		name = "shipdolpic";
		break;
	case 1:
		name = "shipdolpic2";
		break;
	case 2:
		name = "shipdolpic3";
		break;
	case 3:
	default:
		goto end;
	}
	if (!mMActor->checkCurAnm(name, ANM_TYPE_BCK) || mMActor->curAnmEndsNext())
		mMActor->setBck(name);
end:
	offLiveFlag(LIVE_FLAG_UNK20000);
}

void TFruitsBoat::traceBckTrack()
{
	mTrackCtrl->update();
	mTrackAnm->setFrame(mTrackCtrl->getFrame());

	J3DTransformInfo a;
	J3DTransformInfo b;
	mTrackAnm->getTransform(0, &a);
	mTrackAnm->getTransform(1, &b);

	mPosition.x = a.mTranslate.x + b.mTranslate.x;
	mPosition.y = a.mTranslate.y + b.mTranslate.y;
	mPosition.z = a.mTranslate.z + b.mTranslate.z;

	mRotation.x = (360.0f / 65536.0f) * (a.mRotation.x + b.mRotation.x);
	mRotation.y = (360.0f / 65536.0f) * (a.mRotation.y + b.mRotation.y);
	mRotation.z = (360.0f / 65536.0f) * (a.mRotation.z + b.mRotation.z);

	mScaling.x = a.mScale.x * b.mScale.x;
	mScaling.y = a.mScale.y * b.mScale.y;
	mScaling.z = a.mScale.z * b.mScale.z;
}

int TFruitsBoat::getBoatType() const
{
	return static_cast<TFruitsBoatManager*>(mManager)->mBoatType;
}

Mtx* TFruitsBoat::getRootJointMtx() const
{
	return (Mtx*)mMActor->getModel()->getAnmMtx(0);
}

// fabricated: the original tests this through a bool-returning helper
static inline BOOL BoatHasSpline(TGraphTracer* tracer)
{
	return tracer->getGraph()->getSplineRail() != nullptr ? TRUE : FALSE;
}

inline void TFruitsBoat::rowToCurPathNode(f32 speed)
{
	TGraphTracer* tracer = unk124;
	if (BoatHasSpline(tracer)) {
		f32 splineSpeed = tracer->calcSplineSpeed(speed);
		unk124->traceSpline(splineSpeed);

		JGeometry::TVec3<f32> pos;
		JGeometry::TVec3<f32> rot;
		unk124->getGraph()->getSplineRail()->getPosAndRot(unk124->unk14, &pos,
		                                                  &rot);
		pos.x -= mPosition.x;
		pos.y -= mPosition.y;
		pos.z -= mPosition.z;
		mPositionDelta.x += pos.x;
		mPositionDelta.y += pos.y;
		mPositionDelta.z += pos.z;
		mRotation.y = rot.y;
		if (splineSpeed < 0.0f)
			mRotation.y = MsWrap<f32>(180.0f + mRotation.y, 0.0f, 360.0f);
	} else {
		walkToCurPathNode(speed, mTurnSpeed, 0.0f);
	}

	SMSGetMSound()->startSoundActor(MSD_SE_OBJ_PONPONSEN, &mPosition, 0,
	                                nullptr, 0, 4);
}

void TFruitsBoat::load(JSUMemoryInputStream& stream)
{
	JDrama::TActor::load(stream);

	char managerName[256];
	stream.readString(managerName, 256);
	TLiveManager* manager = static_cast<TLiveManager*>(
	    JDrama::TNameRefGen::search(managerName));

	char graphName[256];
	stream.readString(graphName, 256);
	TGraphWeb* graph = gpConductor->getGraphByName(graphName);
	unk124->init(graph);

	mGroundPlane = TMap::getIllegalCheckData();
	init(manager);

	if ((graph == nullptr || graph->isDummy()) && setBckTrack(graphName) == 0)
		mSpine->initWith(&TNerveFruitsBoatBckTrace::theNerve());
}

void TFruitsBoat::init(TLiveManager* manager)
{
	mManager = manager;
	mManager->manageActor(this);
	mMActorKeeper = new TMActorKeeper(mManager, 1);
	initHitActor(ACTOR_TYPE_MAP_OBJECT_UNK7B, 1,
	             HIT_CATEGORY_PLAYER | HIT_CATEGORY_MAP_OBJECT, 0.0f, 0.0f,
	             0.0f, 0.0f);
	offHitFilter(HIT_FILTER_NO_COLLISION);
	unk124->reset();
	goToShortestNextGraphNode();
	if (unk124->getGraph()->unk14 != nullptr) {
		mPosition = unk124->getCurrentPos();
		unk124->moveToShortestNext();
	}

	switch (getBoatType()) {
	case 0:
		mMapCollisionManager
		    = new TMapCollisionManager(1, "/scene/fruitsboat", this);
		mMActor = mMActorKeeper->createMActor("ShipDolpic.bmd", 0);
		mMapCollisionManager->init("ShipDolpic.col", 1, nullptr);
		mAttackRadius = 850.0f;
		calcEntryRadius();
		mAttackHeight = 600.0f;
		calcEntryRadius();
		break;
	case 1:
		mMapCollisionManager
		    = new TMapCollisionManager(1, "/scene/fruitsboatb", this);
		mMActor = mMActorKeeper->createMActor("ShipDolpic2.bmd", 0);
		mMapCollisionManager->init("ShipDolpic2.col", 1, nullptr);
		mAttackRadius = 750.0f;
		calcEntryRadius();
		mAttackHeight = 480.0f;
		calcEntryRadius();
		break;
	case 2:
		mMapCollisionManager
		    = new TMapCollisionManager(1, "/scene/fruitsboatc", this);
		mMActor = mMActorKeeper->createMActor("ShipDolpic3.bmd", 0);
		mMapCollisionManager->init("ShipDolpic3.col", 1, nullptr);
		mAttackRadius = 1000.0f;
		calcEntryRadius();
		mAttackHeight = 300.0f;
		calcEntryRadius();
		break;
	case 3:
	default:
		mMapCollisionManager
		    = new TMapCollisionManager(1, "/scene/fruitsboatd", this);
		mMActor = mMActorKeeper->createMActor("ShipDolpic4.bmd", 0);
		mMapCollisionManager->init("ShipDolpic4.col", 1, nullptr);
		mAttackRadius = 760.0f;
		calcEntryRadius();
		mAttackHeight = 270.0f;
		calcEntryRadius();
		break;
	}

	mMapCollisionManager->setUpActiveCollisionTRS(mPosition, mRotation,
	                                              mScaling);

	mSpine->initWith(&TNerveFruitsBoatGraphWander::theNerve());

	TGraphWeb* graph = unk124->getGraph();
	if (graph == nullptr)
		onLiveFlag(LIVE_FLAG_UNK10000);
	else if (graph->getGraphNode(unk124->getCurGraphIndex()).checkFlag(0x80))
		onLiveFlag(LIVE_FLAG_UNK10000);
	else
		offLiveFlag(LIVE_FLAG_UNK10000);

	mMarchSpeed = getParams()->mSLMoveSpeed.get();
	mTurnSpeed  = getParams()->mSLRotSpeed.get();
	offLiveFlag(LIVE_FLAG_CLIPPED_OUT);
	onLiveFlag(LIVE_FLAG_UNK20);
	offLiveFlag(LIVE_FLAG_UNK100);
	mMActor->setLightType(2);
	calcRootMatrix();
	mMActor->calc();
}

BOOL TFruitsBoat::receiveMessage(THitActor* sender, u32 message)
{
	return false;
}

void TFruitsBoat::setGroundCollision()
{
	JGeometry::TVec3<f32> toMario = mPosition;
	toMario.sub(SMS_GetMarioPos());

	if (getColNum() != 0 || toMario.length() < 1000.0f
	    || (SMS_GetYoshi()->isHatched()
	        && mPosition.x - 1000.0f < SMS_GetYoshi()->getTranslation().x
	        && mPosition.x + 1000.0f > SMS_GetYoshi()->getTranslation().x
	        && mPosition.z - 1000.0f < SMS_GetYoshi()->getTranslation().z
	        && mPosition.z + 1000.0f > SMS_GetYoshi()->getTranslation().z)) {
		mMapCollisionManager->moveActiveCollisionMtx(getModel()->getAnmMtx(0));
	}
}

void TFruitsBoat::calcRootMatrix()
{
	J3DModel* model = getModel();
	MtxPtr mtx      = model->getBaseTRMtx();
	MsMtxSetRotRPH(mtx, mRotation.x, mRotation.y, mRotation.z);

	Mtx tilt;
	MTXRotAxisRad(tilt, (Vec*)&mTiltAxis, 0.017453292f * mTiltAngle);
	MTXConcat(tilt, mtx, mtx);
	MTXTransApply(mtx, mtx, mPosition.x, mPosition.y, mPosition.z);
	model->setBaseScale(mScaling);
}

static inline JGeometry::TVec3<f32> makeBowOffset(f32 x, f32 z)
{
	return JGeometry::TVec3<f32>(x, 0.0f, z);
}

static inline f32 boatInvSqrt2(f32 x) { return JGeometry::TUtil<f32>::inv_sqrt(x); }
static inline f32 boatInvSqrt(f32 x) { return boatInvSqrt2(x); }

// The target keeps the squared length of toMario live from length() into the
// normalize() that follows (one squared(), two sqrt paths); spell it out.
static inline void boatNormalizeSq(JGeometry::TVec3<f32>& v, f32 sq)
{
	if (sq <= JGeometry::TUtil<f32>::epsilon())
		v.zero();
	else
		v.scale(JGeometry::TUtil<f32>::one() * boatInvSqrt(sq));
}

// MsAngleDiff with alpha re-read from memory after the MsWrap call.
static inline f32 boatAngleDiff(const f32& alpha, f32 beta)
{
	return alpha - MsWrap(beta, alpha - 180.0f, alpha + 180.0f);
}

void TFruitsBoat::moveObject()
{
	JGeometry::TVec3<f32> offset = makeBowOffset(
	    300.0f * MsSin(mRotation.y), 300.0f * MsCos(mRotation.y));

	JGeometry::TVec3<f32> pos   = mPosition;
	JGeometry::TVec3<f32> front = pos;
	front += offset;
	JGeometry::TVec3<f32> back = pos;
	back -= offset;
	front.y = gpMapObjWave->getWaveHeight(front.x, front.z);
	back.y  = gpMapObjWave->getWaveHeight(back.x, back.z);

	JGeometry::TVec3<f32> bowDir    = front - back;
	JGeometry::TVec3<f32> targetRot = MsGetRotFromZaxis(bowDir);
	targetRot.x *= 0.5f;

	f32 pitchStep = boatAngleDiff(targetRot.x, mRotation.x);
	if (pitchStep >= 0.0f)
		pitchStep = MsMin(pitchStep, 1.0f);
	else
		pitchStep = MsMax(pitchStep, -1.0f);
	mRotation.x += pitchStep;

	const TBGCheckData* plane = SMS_GetMarioGrPlane();
	if (!checkLiveFlag(LIVE_FLAG_UNK20000)) {
		if (plane != nullptr && plane->getActor() == this
		    && SMS_IsMarioTouchGround4cm()) {
			JGeometry::TVec3<f32> toMario = SMS_GetMarioPos();
			toMario.sub(mPosition);
			toMario.y = 0.0f;
			f32 sq    = toMario.squared();
			f32 dist  = JGeometry::TUtil<f32>::sqrt(sq);
			if (dist != 0.0f) {
				static JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
				boatNormalizeSq(toMario, sq);
				mTiltAxis.cross2(up, toMario);
				mTiltAxis.normalize();
				mTiltSpeed += 0.0003f * dist;
			}
			onLiveFlag(LIVE_FLAG_UNK20000);
			offLiveFlag(LIVE_FLAG_UNK10000);
			if (plane->isBounceOnLanding())
				setJumpReaction();
		}
	} else if (plane == nullptr || plane->getActor() != this
	           || !SMS_IsMarioTouchGround4cm()) {
		offLiveFlag(LIVE_FLAG_UNK20000);
	}

	if (checkLiveFlag(LIVE_FLAG_UNK20000)) {
		JGeometry::TVec3<f32> toMario = SMS_GetMarioPos();
		toMario.sub(mPosition);
		toMario.y = 0.0f;
		f32 sq    = toMario.squared();
		f32 dist  = JGeometry::TUtil<f32>::sqrt(sq);
		if (dist != 0.0f) {
			boatNormalizeSq(toMario, sq);
			static JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
			JGeometry::TVec3<f32> axis;
			axis.cross(up, toMario);
			axis.normalize();
			f32 dx = 0.1f * (axis.x - mTiltAxis.x);
			mTiltAxis.x += dx;
			f32 dy = 0.1f * (axis.y - mTiltAxis.y);
			mTiltAxis.y += dy;
			f32 dz = 0.1f * (axis.z - mTiltAxis.z);
			mTiltAxis.z += dz;
		}
	}

	mTiltSpeed += 0.01f * -MsSin(mTiltAngle);
	mTiltAngle += mTiltSpeed;
	if (mTiltAngle < -8.0f) {
		mTiltAngle = -8.0f;
		mTiltSpeed = -mTiltSpeed;
	} else if (mTiltAngle > 8.0f) {
		mTiltAngle = 8.0f;
		mTiltSpeed = -mTiltSpeed;
	}
	mTiltSpeed *= 0.99f;

	TLiveActor::moveObject();
}

void TFruitsBoat::requestShadow()
{
	if (checkLiveFlag(LIVE_FLAG_DEAD | LIVE_FLAG_HIDDEN | LIVE_FLAG_UNK8))
		return;

	if (!checkLiveFlag(LIVE_FLAG_CLIPPED_OUT | LIVE_FLAG_UNK200)
	    || checkLiveFlag(LIVE_FLAG_FORCE_SHADOW)) {
		TCircleShadowRequest request;
		request.mPosition   = mPosition;
		request.mRadiusX    = mShadowRadiusX;
		request.mRadiusZ    = mShadowRadiusZ;
		request.mShadowType = SHADOW_TYPE_SHIP;
		request.mRotationY  = (s16)mRotation.y;
		if (checkLiveFlag(LIVE_FLAG_FORCE_SHADOW))
			gpBindShadowManager->forceRequest(request, getActorType());
		else
			gpBindShadowManager->request(request, getActorType());
	}

	if (!checkLiveFlag(LIVE_FLAG_CLIPPED_OUT | LIVE_FLAG_UNK200)) {
		if (!isHitCategory(HIT_CATEGORY_MAP_OBJECT))
			gpQuestionManager->request(mPosition, mScaledBodyRadius);
	}
}

TFruitsBoatManager::TFruitsBoatManager(int boat_type, const char* name)
    : TEnemyManager(name)
    , mBoatType(boat_type)
{
}

void TFruitsBoatManager::createModelData()
{
	switch (mBoatType) {
	case 0: {
		static const TModelDataLoadEntry entry[] = {
			{ "ShipDolpic.bmd", 0x10210000, 0 },
			{ nullptr, 0, 0 },
		};
		createModelDataArray(entry);
		break;
	}
	case 1: {
		static const TModelDataLoadEntry entry[] = {
			{ "ShipDolpic2.bmd", 0x10210000, 0 },
			{ nullptr, 0, 0 },
		};
		createModelDataArray(entry);
		break;
	}
	case 2: {
		static const TModelDataLoadEntry entry[] = {
			{ "ShipDolpic3.bmd", 0x10210000, 0 },
			{ nullptr, 0, 0 },
		};
		createModelDataArray(entry);
		break;
	}
	case 3:
	default: {
		static const TModelDataLoadEntry entry[] = {
			{ "ShipDolpic4.bmd", 0x10210000, 0 },
			{ nullptr, 0, 0 },
		};
		createModelDataArray(entry);
		break;
	}
	}
}

void TFruitsBoatManager::load(JSUMemoryInputStream& stream)
{
	unk38 = new TFruitsBoatParams("/enemy/fruitsBoat.prm");
	TEnemyManager::load(stream);
}

TSpineEnemy* TFruitsBoatManager::createEnemyInstance() { return nullptr; }

// TODO: fake
static inline JGeometry::TVec3<f32> polarXZ(f32 theta, f32 radius)
{
	f32 c = radius * MsCos(theta);
	f32 s = radius * MsSin(theta);
	return JGeometry::TVec3<f32>(s, 0.0f, c);
}

DEFINE_NERVE(TNerveFruitsBoatGraphWander, TLiveActor)
{
	TFruitsBoat* self = (TFruitsBoat*)spine->getBody();
	TGraphWeb* graph  = self->unk124->getGraph();
	if (graph == nullptr || graph->isDummy())
		return false;

	if (self->isReachedToGoal()) {
		TGraphTracer* tracer = self->unk124;
		TGraphNode& node = tracer->getCurrent();
		if (node.checkFlag(0x100))
			self->onLiveFlag(LIVE_FLAG_UNK10000);
		if (node.checkFlag(0x400))
			self->unk150 ^= 1;

		self->goToDirectedNextGraphNode(polarXZ(self->mRotation.y, 1.0f));

		if (!self->checkLiveFlag(LIVE_FLAG_UNK10000))
			self->rowToCurPathNode(self->mMarchSpeed);

		spine->pushAfterCurrent(&TNerveFruitsBoatGraphWander::theNerve());
		return true;
	}

	if (self->checkLiveFlag(LIVE_FLAG_UNK10000))
		return false;

	self->rowToCurPathNode(self->mMarchSpeed);
	return false;
}

DEFINE_NERVE(TNerveFruitsBoatBckTrace, TLiveActor)
{
	TFruitsBoat* self = (TFruitsBoat*)spine->getBody();
	self->traceBckTrack();
	return false;
}
