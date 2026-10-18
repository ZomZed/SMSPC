#include "JSystem/JGeometry/JGVec3.hpp"
#include <dolphin/types.h>
#include <Player/Tongue.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JDrama/JDRViewObjPtrList.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <MarioUtil/ModelUtil.hpp>
#include <MarioUtil/MtxUtil.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/DrawUtil.hpp>
#include <MSound/MSound.hpp>
#include <Strategic/MirrorActor.hpp>
#include <Map/Map.hpp>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>

static const char* dummyMactorStringValue1 = "\0\0\0\0\0\0\0\0\0\0\0";
static const char* SMS_NO_MEMORY_MESSAGE   = "メモリが足りません\n";
static const char cDirtyFileName[] = "/scene/map/pollution/H_ma_rak.bti";
static const char cDirtyTexName[]  = "H_ma_rak_dummy";

void TYoshiTongue::init(TYoshi* yoshi)
{
	J3DModelData* modelData = J3DModelLoaderDataBase::load(
	    JKRGetResource("/mario/bmd/yoshi_tongue.bmd"),
	    J3DMLF_MaterialPEFull | (4 << J3DMLF_TevStageNumShift));

	mYoshi = yoshi;
	mModel = new J3DModel(modelData, 0x10000, 1);

	J3DModelData* modelData2 = mModel->getModelData();
	for (u16 i = 0; i < modelData2->getShapeNum(); ++i)
		modelData2->getShapeNodePointer(i)->onFlag(J3DShpFlag_Visible);

	mTipModel = new J3DModel(
	    J3DModelLoaderDataBase::load(
	        JKRGetResource("/mario/bmd/yoshi_tongue_tip.bmd"),
	        J3DMLF_MaterialPEFull | (4 << J3DMLF_TevStageNumShift)),
	    0x10000, 1);

	J3DModelData* modelData3 = mTipModel->getModelData();
	for (u16 i = 0; i < modelData3->getShapeNum(); ++i)
		modelData3->getShapeNodePointer(i)->onFlag(J3DShpFlag_Visible);

	mState       = STATE_IDLE;
	mProgress    = 0;
	mMaxProgress = 60;
	unk82        = 600;

	mInitialSpeed    = 10.0f;
	mExtendAmount    = 0.05f;
	mRetractAmount   = 0.7f;
	mPullAmount      = 0.01f;
	mRetractedLength = 80.0f;
	mMaxReach        = 500.0f + mRetractedLength;
	mElasticity      = 0.8f;

	mHeadPos.set(0.0f, 0.0f, 0.0f);
	mTipPos.set(0.0f, 0.0f, 0.0f);
	mInitialVelocity.set(0.0f, 0.0f, 0.0f);

	mActorTypeInMouth = 0;
	unkD4             = 0;

	initHitActor(ACTOR_TYPE_YOSHI_TONGUE, 5U,
	             HIT_CATEGORY_MAP_OBJECT | HIT_CATEGORY_ITEM
	                 | HIT_CATEGORY_ENEMY,
	             1000.0f, 500.0f, 50.0f, 500.0f);
	offHitFilter(HIT_FILTER_NO_COLLISION);
}

void TYoshiTongue::initInLoadAfter()
{
	JDrama::TViewObjPtrListT<JDrama::TViewObj>* grp
	    = static_cast<JDrama::TViewObjPtrListT<JDrama::TViewObj>*>(
	        JDrama::TNameRefGen::search("敵グループ"));
	grp->getChildren().push_back(this);

	TMirrorActor* ma = new TMirrorActor("ヨッシー舌in鏡");
	ma->init(mModel, 4);

	TMirrorActor* mb = new TMirrorActor("ヨッシー舌先in鏡");
	mb->init(mTipModel, 4);
}

void TYoshiTongue::emit(const JGeometry::TVec3<f32>& src,
                        const JGeometry::TVec3<f32>& dir,
                        const JGeometry::TVec3<f32>& vel)
{
	if (mState == STATE_IDLE) {
		mProgress = 0;
		mState    = STATE_EXTENDING;

		mTipPos     = src;
		mPosition   = src;
		mPosition.y = -((0.5f * mAttackHeight) - mPosition.y);

		mHeadPos = src;
		mHeadDir = dir;

		mInitialVelocity = mInitialSpeed * dir;
		mInitialVelocity += vel * 0.5f;

		if (mInitialVelocity.y < -50.0f)
			mInitialVelocity.y = -50.0f;
		if (mInitialVelocity.y > 50.0f)
			mInitialVelocity.y = 50.0f;
	}
}

void TYoshiTongue::rest(const JGeometry::TVec3<f32>& a,
                        const JGeometry::TVec3<f32>& b)
{
}

#pragma dont_inline on
BOOL TYoshiTongue::canGo()
{
	JGeometry::TVec3<f32> toTip = mTipPos - mHeadPos;

	if (toTip.dot(mHeadDir) < 0.0f)
		return false;

	BOOL touched = gpMap->isTouchedOneWallAndMoveXZ(
	    &mTipPos.x, 10.0f + mTipPos.y, &mTipPos.z, 50.0f);
	// TODO: "> 0" reproduces the original's ble; real spelling unknown
	if (touched > 0)
		return false;

	const TBGCheckData* ground;
	f32 groundY = gpMap->checkGround(mTipPos.x, mTipPos.y, mTipPos.z, &ground);
	if (50.0f + groundY > mTipPos.y) {
		mTipPos.y = 50.0f + groundY;
		return true;
	}

	const TBGCheckData* roof;
	f32 roofY
	    = gpMap->checkRoof(mTipPos.x, mTipPos.y, mTipPos.z, &roof) - 50.0f;
	if (roofY < mTipPos.y) {
		mTipPos.y = roofY;
		return true;
	}

	return true;
}
#pragma dont_inline off

// Copy through the Vec base: the copy ctor's *(Vec*)this cast keeps the local
// in memory; a base copy lets MWCC fuse x*x in squared() like the original.
static inline void TGCopyVec(Vec& dst, const Vec& src) { dst = src; }

THitActor* TYoshiTongue::findTarget(bool allowExtra, bool checkForward)
{
	THitActor* best = nullptr;
	f32 bestDist    = 10000.0f;

#if !defined(VERSION_GMSP01) || defined(SMS_PORT)
	// TODO: not present in GMSE01; unverified for other versions
#ifdef SMS_PORT
	if (SMS_BEHAVIOUR_IS(JP)) // w13: JP behaviour only
#endif
	for (s32 i = 0; i < mColCount; ++i) {
		s32 type = mCollisions[i]->mActorType;
		if (type == ACTOR_TYPE_SEAL || type == ACTOR_TYPE_BASKET_REVERSE) {
			mState = STATE_RETRACTING;
			return nullptr;
		}
	}
#endif

	for (s32 i = 0; i < mColCount; ++i) {
		THitActor* actor = mCollisions[i];
		s32 type         = actor->mActorType;
		int ok           = 0;

		if (type == ACTOR_TYPE_FRUIT_COCONUT)
			ok = 1;
		if (type == ACTOR_TYPE_FRUIT_PAPAYA)
			ok = 1;
		if (type == ACTOR_TYPE_FRUIT_PINE)
			ok = 1;
		if (type == ACTOR_TYPE_FRUIT_DURIAN)
			ok = 1;
		if (type == ACTOR_TYPE_FRUIT_BANANA)
			ok = 1;
		if (type == ACTOR_TYPE_RED_PEPPER)
			ok = 1;
		if (type == ACTOR_TYPE_FRUIT_COVER_PINE)
			ok = 1;

		if (allowExtra == true) {
			if (type == ACTOR_TYPE_BOTTLE_SHORT)
				ok = 1;
			if (type == ACTOR_TYPE_BOTTLE_LARGE)
				ok = 1;
			if (type == ACTOR_TYPE_WOOD_BARREL)
				ok = 1;
#ifdef SMS_PORT
			if (!SMS_BEHAVIOUR_PAL_FAMILY()) {
				if (type & HIT_CATEGORY_ENEMY ? true : false)
					ok = 1;
			} else
#endif
#if defined(VERSION_GMSP01) || defined(SMS_PORT)
			if (type & HIT_CATEGORY_ENEMY ? true : false) {
				switch (type) {
				case ACTOR_TYPE_SEAL:
				case ACTOR_TYPE_MAP_OBJECT_UNK5:
				case ACTOR_TYPE_BASKET_REVERSE:
					break;
				default:
					ok = 1;
					break;
				}
			}
#else
			if (type & HIT_CATEGORY_ENEMY ? true : false)
				ok = 1;
#endif
		}

		if (ok != 1)
			continue;

		JGeometry::TVec3<f32> targetPos = actor->mPosition;
		targetPos.y += 0.5f * actor->mDamageHeight;
		JGeometry::TVec3<f32> delta;
		TGCopyVec(delta, targetPos - mTipPos);

		// TODO: local bool reproduces the original's mfcr/extrwi. shape
		bool zero = delta.isZero();
		if (zero)
			continue;

		f32 dist = delta.length();
		delta.normalize();

		if (checkForward && !(delta.dot(mHeadDir) > 0.5f))
			continue;

		if (dist < bestDist) {
			bestDist = dist;
			best     = mCollisions[i];
		}
	}

	return best;
}

// ROM: in `a + (b - c) * k` the inner operator- sits one inline pass deeper than
// ours (its copy ctor / operator-= stay out of line): emulate the depth with two
// wrappers around a one-statement body.
static inline JGeometry::TVec3<f32> TGSubC(const JGeometry::TVec3<f32>& a,
                                            const JGeometry::TVec3<f32>& b)
{
	JGeometry::TVec3<f32> r(a);
	r -= b;
	return r;
}
#ifdef SMS_PORT
#define TG_SUB_RESULT JGeometry::TVec3<f32>
#else
#define TG_SUB_RESULT const JGeometry::TVec3<f32>&
#endif
static inline TG_SUB_RESULT TGSubB(const JGeometry::TVec3<f32>& a,
                                   const JGeometry::TVec3<f32>& b)
{
	return TGSubC(a, b);
}
static inline TG_SUB_RESULT TGSub(const JGeometry::TVec3<f32>& a,
                                  const JGeometry::TVec3<f32>& b)
{
	return TGSubB(a, b);
}

void TYoshiTongue::movement()
{
	if (unkD4 != 0) {
		mColCount = mSavedColCount;
		unkD4--;
	} else if (mColCount != 0) {
		mSavedColCount = mColCount;
		unkD4          = 3;
	}

	switch (mState) {
	case STATE_IDLE:
	case STATE_GRABBED:
	default:
		mAttackRadius = 300.0f;
		calcEntryRadius();
		offHitFilter(HIT_FILTER_NO_ATTACK);
		break;

	case STATE_EXTENDING:
		mAttackRadius = 1000.0f;
		calcEntryRadius();
		mProgress += 1;

		if (mProgress >= mMaxProgress)
			mState = STATE_RETRACTING;
		else if (!canGo())
			mState = STATE_RETRACTING;
		break;

	case STATE_RETRACTING: {
		mAttackRadius = 10.0f;
		calcEntryRadius();

		JGeometry::TVec3<f32> diff;
		TGCopyVec(diff, mTipPos - mHeadPos);

		if (mHeldObject != nullptr) {
			if (diff.length() < mMaxReach) {
				f32 k                          = mElasticity;
				JGeometry::TVec3<f32>& scaling = mHeldObject->mScaling;
				JGeometry::TVec3<f32> scale    = scaling;
				scale *= k;
				if (scale.x < 0.01f)
					scale.set(0.01f, 0.01f, 0.01f);
				scaling = scale;
			}
		}

		if (diff.length() < mRetractedLength) {
			if (mHeldObject != nullptr) {
				mActorTypeInMouth = mHeldObject->mActorType;
				mHeldObject->receiveMessage(this, HIT_MESSAGE_UNK8);
				mHeldObject->receiveMessage(this, HIT_MESSAGE_UNKB);
				mHeldObject = nullptr;
			}
			mState = STATE_IDLE;
		}
		break;
	}

	case STATE_UNK5:
		mAttackRadius = 10.0f;
		calcEntryRadius();
		mProgress++;
		if (mProgress >= unk82)
			mState = STATE_RETRACTING;
		break;

	case STATE_PULLING:
	case STATE_PULLING_SLOW:
		mProgress++;
		break;
	}

	switch (mState) {
	default:
		mTipPos = mHeadPos;
		break;

	case STATE_EXTENDING: {
		THitActor* target = findTarget(true, true);
		if (target != nullptr && mHeldObject == nullptr) {
			JGeometry::TVec3<f32> tpos = target->mPosition;
			tpos.y += 0.5f * target->mDamageHeight;
			JGeometry::TVec3<f32> old = mTipPos;
			mTipPos          = mTipPos + TGSub(tpos, mTipPos) * mExtendAmount;
			mInitialVelocity = mTipPos - old;

			if (JGeometry::TVec3<f32>(tpos - mTipPos).length() < 200.0f
			    && target->receiveMessage(this, HIT_MESSAGE_TAKE) == true) {
				mHeldObject = (TTakeActor*)target;
				SMSGetMSound()->startSoundActor(MSD_SE_YV_PERON, &mTipPos, 0,
				                                nullptr, 0, 4);
				mProgress = 0;
				mState    = STATE_GRABBED;
			}
		} else {
			mTipPos += mInitialVelocity;
		}
		break;
	}

	case STATE_GRABBED:
		mProgress += 1;
		if (mProgress > 10)
			mState = STATE_RETRACTING;
		break;

	case STATE_RETRACTING: {
		mTipPos = mHeadPos + TGSub(mTipPos, mHeadPos) * mRetractAmount;
		break;
	}

	case STATE_PULLING:
	case STATE_PULLING_SLOW: {
		JGeometry::TVec3<f32> diff;
		TGCopyVec(diff, mTipPos - mHeadPos);
		f32 len                    = diff.length();

		MtxPtr mtx = getTakingMtx();
		JGeometry::TVec3<f32> step;
		step.x = -mtx[0][0];
		step.y = -mtx[1][0];
		step.z = -mtx[2][0];

		f32 amount = len * mPullAmount;
		if (mState == STATE_PULLING_SLOW)
			amount *= 0.5f;
		step *= amount;

		JGeometry::TVec3<f32> next = mTipPos + step;
		if (mHolder->moveRequest(next) == true)
			mTipPos += step;
		break;
	}
	}

	checkTaking();
	mPosition   = mTipPos;
	mPosition.y = -((0.5f * mAttackHeight) - mPosition.y);
}

void TYoshiTongue::calcAnim(MtxPtr mtx)
{
	mHeadPos.x = mtx[0][3];
	mHeadPos.y = mtx[1][3];
	mHeadPos.z = mtx[2][3];
	mHeadDir.x = mtx[0][0];
	mHeadDir.y = mtx[1][0];
	mHeadDir.z = mtx[2][0];

#ifdef SMS_PORT
	// w13: the "visible" case is STATE_IDLE in US/PAL and STATE_EXTENDING in JP
	switch (mState == (SMS_BEHAVIOUR_PAL_FAMILY() ? STATE_IDLE : STATE_EXTENDING) ? 1 : 0) {
	case 1: {
#else
	switch (mState) {
#ifdef VERSION_GMSP01
	// TODO: verified against GMSE01 only; other versions unverified
	case STATE_IDLE: {
#else
	case STATE_EXTENDING: {
#endif
#endif
		J3DModelData* modelData = mModel->getModelData();
		for (u16 i = 0; i < modelData->getShapeNum(); ++i)
			modelData->getShapeNodePointer(i)->onFlag(J3DShpFlag_Visible);

		J3DModelData* modelData2 = mTipModel->getModelData();
		for (u16 i = 0; i < modelData2->getShapeNum(); ++i)
			modelData2->getShapeNodePointer(i)->onFlag(J3DShpFlag_Visible);
		break;
	}
	default:
		J3DModelData* modelData = mModel->getModelData();
		for (u16 i = 0; i < modelData->getShapeNum(); ++i)
			modelData->getShapeNodePointer(i)->offFlag(J3DShpFlag_Visible);

		J3DModelData* modelData2 = mTipModel->getModelData();
		for (u16 i = 0; i < modelData2->getShapeNum(); ++i)
			modelData2->getShapeNodePointer(i)->offFlag(J3DShpFlag_Visible);

		JGeometry::TVec3<f32> tip = mTipPos;
		tip.y += 50.0f;
		SMS_MakeJointsToArc(mModel, mHeadPos, mHeadDir, tip);

		u16 jointNum = mModel->getModelData()->getJointNum();
		MtxPtr a     = mModel->getAnmMtx(jointNum - 2);
		MtxPtr b     = mModel->getAnmMtx(jointNum - 1);

		JGeometry::TVec3<f32> dir;
		dir.x = b[0][3] - a[0][3];
		dir.y = 0.0f;
		dir.z = b[2][3] - a[2][3];
		MsVECNormalize(&dir, &dir);

		JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
		JGeometry::TVec3<f32> tmp;
		tmp.cross(up, dir);

		Mtx modelMtx;
		modelMtx[0][0] = tmp.x;
		modelMtx[0][1] = up.x;
		modelMtx[0][2] = dir.x;
		modelMtx[0][3] = tip.x;

		modelMtx[1][0] = tmp.y;
		modelMtx[1][1] = up.y;
		modelMtx[1][2] = dir.y;
		modelMtx[1][3] = tip.y;

		modelMtx[2][0] = tmp.z;
		modelMtx[2][1] = up.z;
		modelMtx[2][2] = dir.z;
		modelMtx[2][3] = tip.z;

		mTipModel->setBaseTRMtx(modelMtx);
		mTipModel->calc();
		break;
	}
}

void TYoshiTongue::viewCalc()
{
	switch (mState) {
	case STATE_IDLE:
		break;
	default:
		mModel->viewCalc();
		mTipModel->viewCalc();
		break;
	}
}

void TYoshiTongue::entry()
{
	switch (mState) {
	case STATE_IDLE:
		break;
	default:
		mModel->entry();
		mTipModel->entry();
		break;
	}
}
