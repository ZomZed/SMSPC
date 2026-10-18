#include <Enemy/BossWanwan.hpp>
#include <Enemy/Conductor.hpp>
#include <Enemy/EffectObj.hpp>
#include <Enemy/Graph.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/MActorAnm.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <Map/MapCollisionManager.hpp>
#include <MoveBG/ItemManager.hpp>
#include <GC2D/GCConsole2.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Camera/CameraShake.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/MtxUtil.hpp>
#include <MarioUtil/RumbleMgr.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundSE.hpp>
#include <System/EmitterViewObj.hpp>
#include <System/MarDirector.hpp>
#include <System/Particles.hpp>
#include <Strategic/Spine.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/Strategy.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DCluster.hpp>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

static const char* bwanwan_bastable[] = {
	"/scene/bwanwan/bas/bwanwan_bark.bas",
	nullptr,
	"/scene/bwanwan/bas/bwanwan_shake.bas",
	nullptr,
	"/scene/bwanwan/bas/bwanwan_wait.bas",
	"/scene/bwanwan/bas/bwanwan_wait2.bas",
	nullptr,
};

static JGeometry::TVec3<f32> BW_BATH_POS(-1000.0f, 4.5f, -6217.2f);
static JGeometry::TVec3<f32> BW_PICKET_START(6012.84f, 0.0f, 7323.15f);
static JGeometry::TVec3<f32> BW_HEAD_START(5741.72f, -100.0f, 6311.62f);

// fabricated
static inline JGeometry::TVec3<f32> fromPolar(f32 theta, f32 radius)
{
	return JGeometry::TVec3<f32>(radius * JMASSin(theta * (65536.0f / 360.0f)),
	                             0.0f,
	                             radius * JMASCos(theta * (65536.0f / 360.0f)));
}

// fabricated: the target inlines these loops at the angle sites while still
// calling MsWrap<f32> out of line elsewhere.
static inline f32 wrapAngle(f32 angle)
{
	while (angle >= 360.0f)
		angle -= 360.0f;
	while (angle < 0.0f)
		angle += 360.0f;
	return angle;
}

// fabricated
static inline bool isEnemyThroughBG(const TBGCheckData* data)
{
	if (data->mBGType == BG_TYPE_EVERYTHING_BUT_MAP_OBJECTS_PHASE_THROUGH
	    || data->mBGType == BG_TYPE_ONLY_ENEMIES_PHASE_THROUGH)
		return true;
	return false;
}

TBWParams::TBWParams(const char* path)
    : TSpineEnemyParams(path)
    , PARAM_INIT(mSLMarchSpeed, 6.0f)
    , PARAM_INIT(mSLTurnSpeed, 1.0f)
    , PARAM_INIT(mSLLeashNodeLen, 120.0f)
    , PARAM_INIT(mSLPicketHeight, 100.0f)
    , PARAM_INIT(mSLPicketRadius, 100.0f)
    , PARAM_INIT(mSLChainHitHeight, 100.0f)
    , PARAM_INIT(mSLChainHitRadius, 100.0f)
    , PARAM_INIT(mSLChainGroundRadius, 60.0f)
    , PARAM_INIT(mSLPullLimit, 1.0f)
    , PARAM_INIT(mSLAttackSpeed, 10.0f)
    , PARAM_INIT(mSLStunTimer, 4000)
    , PARAM_INIT(mSLSearchLength, 10000.0f)
    , PARAM_INIT(mSLSearchAngle, 60.0f)
    , PARAM_INIT(mSLBWHitPointMax, 0xFF)
    , PARAM_INIT(mSLHeadGap, 150.0f)
    , PARAM_INIT(mSLShakeLengthMax, 3000.0f)
    , PARAM_INIT(mSLShakeLengthMaxHP0, 2000.0f)
{
	TParams::load(mPrmPath);
}

TBWLeashNode::TBWLeashNode(TBWLeash* owner, int index, const char* name)
    : THitActor(name)
    , mOwner(owner)
    , mMActor(nullptr)
    , mTemperature(0.0f)
    , mIndex(index)
{
	TBossWanwan* wanwan = mOwner->mOwner;
	mMActor = wanwan->getActorKeeper()->createMActor("bwanwan_chain.bmd", 0);
	mMActor->setBrkFromIndex(2);
	f32 radius = wanwan->getParams()->mSLChainHitRadius.get();
	f32 height = wanwan->getParams()->mSLChainHitHeight.get();
	initHitActor(ACTOR_TYPE_BOSS_UNKC, 1, HIT_CATEGORY_PLAYER, 1.2f * radius,
	             1.2f * height, radius, height);
}

void TBWLeashNode::calcTemperature()
{
	if (mIndex == 0)
		return;

	f32 diff = mOwner->getNode(mIndex - 1)->getTemperature() - mTemperature;
	f32 step;
	if (diff < 0.0f) {
		if (diff < -0.1f)
			step = -0.02f;
		else
			step = -0.005f;
	} else {
		if (diff > 0.1f)
			step = 0.02f;
		else
			step = 0.005f;
	}

	mTemperature += step;
	if (mTemperature < 0.0f)
		mTemperature = 0.0f;
	if (mTemperature > 1.0f)
		mTemperature = 1.0f;
}

void TBWLeashNode::calcMatrix()
{
	MtxPtr mtx = mMActor->getModel()->getBaseTRMtx();
	int index;
	TRope* rope = mOwner->mRope;
	TRopePoint* points = rope->mPoints;
	index = mIndex;
	JGeometry::TVec3<f32> pos = points[index].unkC;
	JGeometry::TVec3<f32> dir;
	if (index < rope->mNumPoints - 1) {
		const JGeometry::TVec3<f32>& next = points[index + 1].unkC;
		dir                               = next;
		dir -= pos;
	} else {
		const JGeometry::TVec3<f32>& prev = points[index - 1].unkC;
		dir                               = prev;
		dir -= pos;
		dir.negate();
	}
	VECNormalize(&dir, &dir);

	JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
	JGeometry::TVec3<f32> side;
	side.cross(up, dir);
	VECNormalize(&side, &side);
	up.cross(dir, side);
	VECNormalize(&up, &up);

	mtx[0][2] = dir.x;
	mtx[1][2] = dir.y;
	mtx[2][2] = dir.z;
	if (mIndex & 1) {
		mtx[0][0] = side.x;
		mtx[1][0] = side.y;
		mtx[2][0] = side.z;
		mtx[0][1] = up.x;
		mtx[1][1] = up.y;
		mtx[2][1] = up.z;
	} else {
		mtx[0][0] = up.x;
		mtx[1][0] = up.y;
		mtx[2][0] = up.z;
		mtx[0][1] = side.x;
		mtx[1][1] = side.y;
		mtx[2][1] = side.z;
	}
	mtx[0][3] = pos.x;
	mtx[1][3] = pos.y + 30.0f;
	mtx[2][3] = pos.z;
	mPosition = pos;
}

void TBWLeashNode::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (cue & CUE_MOVE) {
		calcTemperature();
		calcMatrix();
		if (mOwner->mOwner->mHitPoints != 0 && mIndex < 8) {
			for (int i = 0; i < getColNum(); ++i) {
				THitActor* other = getCollision(i);
				if (other->getActorType() == ACTOR_TYPE_MARIO)
					other->receiveMessage(this, HIT_MESSAGE_UNKA);
			}
		}
	}

	if (cue & CUE_CALC_ANIM) {
		J3DFrameCtrl* ctrl = mMActor->getFrameCtrl(5);
		if (ctrl != nullptr) {
			f32 frame = mTemperature * (ctrl->getEnd() - 1);
			f32 rate;
			if (mIndex < 5) {
				rate = mIndex * 0.25f
				       + (f32)mOwner->mOwner->mHitPoints
				             / (f32)mOwner->mOwner->getParams()
				                   ->mSLBWHitPointMax.get();
			} else {
				u16 num = mOwner->mRope->mNumPoints;
				if (mIndex >= num - 10)
					rate = (num - mIndex) / 10.0f;
				else
					rate = 1.0f;
			}

			if (rate > 1.0f)
				rate = 1.0f;
			else if (rate < 0.0f)
				rate = 0.0f;

			ctrl->setFrame(frame * rate);
			ctrl->setRate(0.0f);
		}
	}

	if (mIndex < mOwner->mRope->mNumPoints - 1)
		mMActor->perform(cue, graphics);
}

TBWLeash::TBWLeash(TBossWanwan* owner, int node_num, const char* name)
    : JDrama::TViewObj(name)
    , mOwner(owner)
    , mRope(nullptr)
    , mNodes(nullptr)
{
	f32 nodeLen      = mOwner->getParams()->mSLLeashNodeLen.get();
	f32 groundRadius = mOwner->getParams()->mSLChainGroundRadius.get();
	mRope = new TRope(node_num, mOwner->mPosition, nodeLen, groundRadius, 0.7f,
	                  -2.0f);
	mNodes = new TBWLeashNode*[node_num];
	for (int i = 0; i < node_num; ++i)
		mNodes[i] = new TBWLeashNode(this, i, "鎖部品");

	TIdxGroupObj* group = static_cast<TIdxGroupObj*>(
	    JDrama::TNameRefGen::search("敵グループ"));
	for (int i = 0; i < node_num; ++i) {
		group->getChildren().push_back(mNodes[i]);

		if (i < node_num - 2)
			mNodes[i]->offHitFilter(HIT_FILTER_NO_COLLISION);
		else
			mNodes[i]->onHitFilter(HIT_FILTER_NO_COLLISION);

		if (i < 2)
			mRope->mPoints[i].unk28 |= 1;
		else
			mRope->mPoints[i].unk28 &= ~1;
	}
}

void TBWLeash::invalidateAllCollision()
{
	for (int i = 0; i < mRope->mNumPoints; ++i)
		mNodes[i]->onHitFilter(HIT_FILTER_NO_COLLISION);
}

void TBWLeash::pullTail(const JGeometry::TVec3<f32>& pos)
{
	// FAKEMATCH(frame): 32 bytes; real construct unknown
	volatile u32 pad[8];
	(void)pad;
	JGeometry::TVec3<f32> pull;
	pull = mRope->mPoints[0].unkC;
	mRope->constraintTail(pos);
	pull -= mRope->mPoints[0].unkC;
	pull.negate();
	mOwner->mPullVec = pull;
}

void TBWLeash::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (cue & CUE_MOVE) {
		JGeometry::TVec3<f32> headPos;
		mOwner->getJointTransByIndex(5, &headPos);
		mRope->moveHead(headPos);
		mOwner->unk188 = 0;
		if (mOwner->mIsTied) {
			pullTail(mOwner->mPicket->mPosition);

			JGeometry::TVec3<f32> diff = mRope->mPoints[0].unkC;
			JGeometry::TVec3<f32> body = mOwner->mPosition;
			body.y += 500.0f;
			diff -= body;
			if (VECMag(&diff) > 650.0f) {
				VECNormalize(&diff, &diff);
				diff.scale(20.0f);
				diff.y = 0.0f;
				mOwner->mPosition += diff;
				mOwner->unk188 = 1;
			}

			f32 target = wrapAngle(MsGetRotFromZaxisY(diff) + 180.0f);
			f32 delta  = MsAngleDiff(target, mOwner->mRotation.y);
			if (delta > 0.0f)
				delta = MsMin(delta, 1.5f * mOwner->mTurnSpeed);
			else
				delta = MsMax(delta, 1.5f * -mOwner->mTurnSpeed);
			mOwner->mRotation.y = wrapAngle(mOwner->mRotation.y + delta);
		}
	}

	if (cue & CUE_CALC_ANIM) {
		JGeometry::TVec3<f32> center;
		mOwner->getJointTransByIndex(1, &center);
		for (int i = 0; i < 15; ++i) {
			TRopePoint& point          = mRope->mPoints[i];
			JGeometry::TVec3<f32> diff = point.unkC;
			diff -= center;
			f32 len = VECMag(&diff);
			if (len < 500.0f) {
				diff.scale(500.0f / len);
				diff += center;
				point.unk18.zero();
				point.unkC = diff;
				point.unk0 = point.unkC;
			}
		}
	}

	for (int i = 0; i < mRope->mNumPoints; ++i)
		mNodes[i]->testPerform(cue, graphics);
}

TBWPicket::TBWPicket(TBossWanwan* owner, const char* name)
    : TTakeActor(name)
    , mOwner(owner)
    , mMActor(nullptr)
{
	MTXIdentity(mTakingMtx);
	initHitActor(ACTOR_TYPE_BOSS_UNKD, 1, HIT_CATEGORY_PLAYER,
	             mOwner->getParams()->mSLPicketRadius.get(),
	             mOwner->getParams()->mSLPicketHeight.get(),
	             mOwner->getParams()->mSLPicketRadius.get(),
	             mOwner->getParams()->mSLPicketHeight.get());
	static_cast<TIdxGroupObj*>(JDrama::TNameRefGen::search("敵グループ"))
	    ->getChildren()
	    .push_back(this);
	offHitFilter(HIT_FILTER_NO_COLLISION);
	mMActor
	    = mOwner->getActorKeeper()->createMActor("bwanwan_picket.bmd", 0);
}

BOOL TBWPicket::receiveMessage(THitActor* sender, u32 message)
{
	if (sender->getActorType() == ACTOR_TYPE_MARIO) {
		if (message == HIT_MESSAGE_HIP_DROP) {
			mOwner->mIsTied = 1;
			mOwner->unk184  = 0;
			SMSGetMSound()->startSoundActor(MSD_SE_BS_WANWAN_LOCK, &mPosition, 0,
			                                nullptr, 0, 4);
			return true;
		}

		if (message == HIT_MESSAGE_TAKE) {
			mOwner->releasePicket();
			mHolder = (TTakeActor*)sender;
			return true;
		}

		if (message == HIT_MESSAGE_THROWN || message == HIT_MESSAGE_UNK8) {
			mHolder = nullptr;
			return true;
		}
	}

	return false;
}

// fabricated wrappers: the ROM calls getLatestNerve out of line here
static inline const TNerveBase<TLiveActor>*
BWLatestNerve1(const TSpineBase<TLiveActor>* spine)
{
	return spine->getLatestNerve();
}

static inline const TNerveBase<TLiveActor>*
BWLatestNerve2(const TSpineBase<TLiveActor>* spine)
{
	return BWLatestNerve1(spine);
}

BOOL TBWPicket::moveRequest(const JGeometry::TVec3<f32>& where_to)
{
	if (BWLatestNerve2(mOwner->mSpine) == &TNerveBWJumpToBath::theNerve()
	    || BWLatestNerve2(mOwner->mSpine) == &TNerveBWDie::theNerve())
		return false;

	if (mOwner->mHitPoints != 0)
		return false;

	mOwner->mLeash->pullTail(where_to);
	return true;
}

MtxPtr TBWPicket::getTakingMtx() { return mTakingMtx; }

void TBWPicket::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (cue & CUE_MOVE) {
		TRope* rope = mOwner->mLeash->mRope;
		mPosition   = rope->mPoints[rope->mNumPoints - 1].unkC;
		ensureTakeSituation();
		if (mHolder != nullptr) {
			TBWLeash* leash = mOwner->mLeash;
			JGeometry::TVec3<f32> dir;
			dir.set(mHolder->mPosition);
			dir -= leash->mRope->mPoints[0].unkC;

			JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
			JGeometry::TVec3<f32> side;
			side.cross(up, dir);
			VECNormalize(&side, &side);
			up.cross(dir, side);
			VECNormalize(&up, &up);
			dir.cross(side, up);
			VECNormalize(&dir, &dir);

			mTakingMtx.mMtx[0][0] = side.x;
			mTakingMtx.mMtx[1][0] = side.y;
			mTakingMtx.mMtx[2][0] = side.z;
			mTakingMtx.mMtx[0][1] = up.x;
			mTakingMtx.mMtx[1][1] = up.y;
			mTakingMtx.mMtx[2][1] = up.z;
			mTakingMtx.mMtx[0][2] = dir.x;
			mTakingMtx.mMtx[1][2] = dir.y;
			mTakingMtx.mMtx[2][2] = dir.z;
			mTakingMtx.mMtx[0][3] = mPosition.x;
			mTakingMtx.mMtx[1][3] = mPosition.y;
			mTakingMtx.mMtx[2][3] = mPosition.z;
			mHolder->moveRequest(mPosition);
		}
	}

	if (cue & CUE_CALC_ANIM) {
		MtxPtr mtx = mMActor->getModel()->getBaseTRMtx();
		MTXIdentity(mtx);

		TBWLeash* leash = mOwner->mLeash;
		JGeometry::TVec3<f32> dir;
		dir.set(mPosition);
		TRope* rope = leash->mRope;
		dir -= rope->mPoints[rope->mNumPoints - 3].unkC;
		if (dir.isZero())
			dir.set(0.0f, 0.0f, 1.0f);

		JGeometry::TVec3<f32> up(0.0f, 1.0f, 0.0f);
		JGeometry::TVec3<f32> side;
		side.cross(up, dir);
		VECNormalize(&side, &side);
		up.cross(dir, side);
		VECNormalize(&up, &up);
		dir.cross(side, up);
		VECNormalize(&dir, &dir);

		mtx[0][0] = side.x;
		mtx[1][0] = side.y;
		mtx[2][0] = side.z;
		mtx[0][1] = up.x;
		mtx[1][1] = up.y;
		mtx[2][1] = up.z;
		mtx[0][2] = dir.x;
		mtx[1][2] = dir.y;
		mtx[2][2] = dir.z;
		mtx[0][3] = mPosition.x;
		mtx[1][3] = mPosition.y;
		mtx[2][3] = mPosition.z;
		if (!mOwner->mIsTied)
			mtx[1][3] += 70.0f;

		if (isTaken()) {
			mtx[0][3] -= 50.0f * dir.x;
			mtx[2][3] -= 50.0f * dir.z;
		} else {
			mtx[0][3] -= 60.0f * dir.x;
			mtx[2][3] -= 60.0f * dir.z;
		}
	}

	mMActor->perform(cue, graphics);
	THitActor::perform(cue, graphics);
}

TBWHit::TBWHit(TBossWanwan* owner, int joint_index, const char* name)
    : THitActor(name)
    , mOwner(owner)
    , mJointIndex(joint_index)
{
}

BOOL TBWHit::receiveMessage(THitActor* sender, u32 message)
{
	return mOwner->receiveMessage(sender, message);
}

void TBWHit::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (cue & CUE_MOVE) {
		if (mJointIndex >= 0)
			mOwner->getJointTransByIndex(mJointIndex, &mPosition);

		for (int i = 0; i < getColNum(); ++i) {
			THitActor* other = getCollision(i);
			if (mOwner->mHitPoints != 0
			    && other->getActorType() == ACTOR_TYPE_MARIO)
				other->receiveMessage(mOwner, HIT_MESSAGE_UNKA);
		}
	}

	THitActor::perform(cue, graphics);
}

TBWBinder::TBWBinder() { }

static inline void BWCopyVec(Vec& d, const Vec& s) { d = s; }

// fabricated: keeps TVec3::set<f> out of line inside the auto-inlined
// rollNextGraphNode like the ROM
static inline const JGeometry::TVec3<f32>& BWSetVec0(JGeometry::TVec3<f32>& v,
                                                     f32 x, f32 y, f32 z)
{
	v.set(x, y, z);
	return v;
}
static inline const JGeometry::TVec3<f32>& BWSetVec(JGeometry::TVec3<f32>& v,
                                                    f32 x, f32 y, f32 z)
{
	return BWSetVec0(v, x, y, z);
}

void TBWBinder::bind(TLiveActor* actor)
{
	TBossWanwan* wanwan = (TBossWanwan*)actor;

	JGeometry::TVec3<f32> delta = wanwan->mPositionDelta;
	JGeometry::TVec3<f32> next  = wanwan->mPosition;
	next += delta;

	if (wanwan->isAirborne()) {
		JGeometry::TVec3<f32> velocity = wanwan->mVelocity;
		next += velocity;
		velocity.y -= wanwan->getGravityY();
		if (velocity.y < TLiveActor::mVelocityMinY)
			velocity.y = TLiveActor::mVelocityMinY;
		wanwan->mVelocity = velocity;
	}

	if (wanwan->mSpine->isNerve(&TNerveBWJumpToBath::theNerve())
	    || wanwan->mSpine->isNerve(&TNerveBWDie::theNerve())) {
		wanwan->mPositionDelta = next - wanwan->mPosition;
	} else {
		if (wanwan->isAirborne()) {
			const TBGCheckData* ground;
			f32 nextY = next.y;
			f32 nextZ = next.z;
			f32 groundY = 1.0f
			              + gpMap->checkGround(next.x,
			                                   nextY + wanwan->getHeadHeight(),
			                                   nextZ, &ground);
			if (wanwan->mPosition.y > nextY && !isEnemyThroughBG(ground)) {
				const TBGCheckData* ground2;
				f32 groundY2
				    = 1.0f
				      + gpMap->checkGround(next.x,
				                           wanwan->mPosition.y
				                               + wanwan->getHeadHeight(),
				                           nextZ, &ground2);
				if (groundY2 > groundY) {
					groundY = groundY2;
					ground  = ground2;
				}
			}

			if (nextY <= groundY && !ground->checkFlag(BG_CHECK_FLAG_ILLEGAL)
			    && !isEnemyThroughBG(ground)) {
				next.y            = groundY;
				wanwan->mVelocity = JGeometry::TVec3<f32>(0.0f, 0.0f, 0.0f);
				wanwan->offLiveFlag(LIVE_FLAG_AIRBORNE);
				wanwan->offLiveFlag(LIVE_FLAG_UNK8000);
			} else {
				wanwan->onLiveFlag(LIVE_FLAG_AIRBORNE);
			}

			wanwan->mGroundHeight = groundY;
			wanwan->mGroundPlane  = ground;
		}

		delta = next - wanwan->mPosition;

		if (!wanwan->isAirborne()) {
			TGraphTracer* tracer = wanwan->getTracer();
			if (tracer->getGraph() != nullptr) {
				int a = tracer->mCurrIdx;
				int b = tracer->mPrevIdx;
				if (a >= 0 && b >= 0 && a != b) {
					TGraphWeb* web = tracer->getGraph();
					TGraphNode* nodeA = &web->getGraphNode(a);
					TGraphNode* nodeB = &web->getGraphNode(b);
					JGeometry::TVec3<f32> pa;
					nodeA->getPoint(&pa);
					JGeometry::TVec3<f32> pb;
					nodeB->getPoint(&pb);
					pa -= pb;
					VECNormalize(&pa, &pa);

					f32 t  = 0.0f;
					f32 sq = pa.squared();
					if (sq != 0.0f)
						t = delta.dot(pa) / sq;

					f32 s = t;
					if (t < 0.0f) {
						if (t > -3.0f)
							s = -3.0f;
					} else if (t > 0.0f && t < 3.0f) {
						s = 3.0f;
					}

					delta = pa;
					delta.scale(s);
				}
			}
		}

		if (!wanwan->isAirborne()) {
			JGeometry::TVec3<f32> d = delta;
			f32 len                 = VECMag(&d);
			if (len != 0.0f) {
				f32 angle = 360.0f * (len / 3141.5928f);
				JGeometry::TVec3<f32> fwd = fromPolar(wanwan->mRotation.y, 1.0f);
				if (fwd.dot(delta) < 0.0f)
					angle = -angle;
				angle *= 2.0f;

				if (wanwan->mIsRolling) {
					wanwan->mRollAngle
					    = MsAngleWrap(angle + wanwan->mRollAngle);
				} else if (wanwan->mRollAngle != 0.0f) {
					f32 roll = wanwan->mRollAngle + angle;
					if (roll > 360.0f)
						roll = 0.0f;
					wanwan->mRollAngle = roll;
				}
			}
		}

		if (wanwan->mIsTied) {
			TBWLeash* leash            = wanwan->mLeash;
			JGeometry::TVec3<f32> pos  = wanwan->mPosition;
			JGeometry::TVec3<f32> np   = pos;
			JGeometry::TVec3<f32> from = pos;
			np += delta;
			JGeometry::TVec3<f32> anchor;
			BWCopyVec(anchor, leash->mRope->mPoints[3].unkC);
			np -= anchor;
			if (VECMag(&np) > 860.0f) {
				VECNormalize(&np, &np);
				np.scale(860.0f);
				np += anchor;
				np -= from;
				delta = np;
			}
		}

		if (!wanwan->isAirborne()) {
			TGraphTracer* tracer = wanwan->getTracer();
			TGraphWeb* web       = tracer->getGraph();
			TGraphNode* nodeA = &web->getGraphNode(tracer->mCurrIdx);
			TGraphNode* nodeB = &web->getGraphNode(tracer->mPrevIdx);
			JGeometry::TVec3<f32> pa;
			nodeA->getPoint(&pa);
			JGeometry::TVec3<f32> pb;
			nodeB->getPoint(&pb);
			JGeometry::TVec3<f32> foot
			    = MsPerpendicFootToLineR(pb, pa, wanwan->mPosition);
			JGeometry::TVec3<f32> toFoot = foot;
			pa -= pb;
			toFoot -= wanwan->mPosition;
			f32 len = VECMag(&toFoot);
			if (len > 10.0f)
				len = 10.0f;
			if (len < 0.0001f) {
				toFoot.zero();
			} else {
				VECNormalize(&toFoot, &toFoot);
				toFoot.scale(len);
			}
			delta += toFoot;
		}

		wanwan->mPositionDelta = delta;
	}
}

TBossWanwanMtxCalc::TBossWanwanMtxCalc(TBossWanwan* owner)
    : M3UMtxCalcSIAnmBlendQuat()
    , mOwner(owner)
{
	mMotionBlendRatio = 0.0f;
}

// fabricated wrapper: the ROM calls getMActorAnmData out of line when joinAnm is inlined
static inline MActorAnmData* BWGetAnmData(const TMActorKeeper* keeper)
{
	return keeper->getMActorAnmData();
}

void TBossWanwanMtxCalc::joinAnm(int param_1)
{
	M3UMtxCalcSIAnmBlendQuat::joinAnm(
	    BWGetAnmData(mOwner->getActorKeeper())->getUnk2C()->getAnmPtr(
	        param_1));
}

void TBossWanwanMtxCalc::calc(u16 joint)
{
	if (joint == 0 && mOwner->isAirborne()) {
		J3DTransformInfo info;
		j3dSys.setCurrentMtxCalc(this);
		if (mNewAnm != nullptr)
			mNewAnm->getTransform(joint, &info);
		else
			info = j3dSys.getModel()
			           ->getModelData()
			           ->getJointNodePointer(joint)
			           ->getTransformInfo();
		info.mTranslate.x = 0.0f;
		info.mTranslate.y = 0.0f;
		info.mTranslate.z = 0.0f;
		calcTransform(joint, info);
		return;
	}

	M3UMtxCalcSIAnmBlendQuat::calc(joint);
	if (joint == 1) {
		TMtx34f rot;
		rot.zero();
		MsMtxSetRotX(rot, mOwner->mRollAngle);
		MtxPtr mtx = mOwner->getModel()->getAnmMtx(joint);
		MTXConcat(mtx, rot, mtx);
		MTXCopy(mtx, J3DSys::mCurrentMtx);
	}
}

TBossWanwan::TBossWanwan(const char* name)
    : TSpineEnemy(name)
    , mMtxCalc(nullptr)
    , mLeash(nullptr)
    , mPicket(nullptr)
    , mRollAngle(0.0f)
    , mIsRolling(0)
    , mIsTied(0)
    , unk180(0)
    , unk184(0)
    , unk188(0)
    , mIsDead(false)
    , mShineAppeared(false)
    , unk190(0)
    , unk194(true)
    , mDived(false)
    , mMessageFlags(0)
    , unk19C(0)
    , unk1A0(0)
    , mEffectPos(0.0f, 0.0f, 0.0f)
    , unk1B0(0)
    , unk1B4(0)
{
	mBinder = new TBWBinder;
}

void TBossWanwan::init(TLiveManager* manager)
{
	mManager = manager;
	mManager->manageActor(this);
	mMActorKeeper = new TMActorKeeper(mManager, 0x11);
	mMActor       = mMActorKeeper->createMActor("bwanwan_body.bmd", 0);

	TGraphWeb* graph = gpConductor->getGraphByName("bwanwan");
	graph->initGoalIndex(BW_BATH_POS);
	unk124->setGraph(graph);

	mSpine->initWith(&TNerveBWGraphWander::theNerve());
	mMarchSpeed = getParams()->mSLMarchSpeed.get();
	mTurnSpeed  = getParams()->mSLTurnSpeed.get();
	mPosition   = BW_HEAD_START;
	reset();

	mLeash  = new TBWLeash(this, 15, "ボスワンワン鎖");
	mPicket = new TBWPicket(this, "ボスワンワンつかみ");
	mPicket->mPosition = BW_PICKET_START;
	mIsTied            = 1;
	unk184             = 0;
	goToRandomNextGraphNode();

	initHitActor(ACTOR_TYPE_BOSS_UNKB, 1, HIT_CATEGORY_PLAYER, 0.0f, 0.0f,
	             0.0f, 0.0f);
	mHits[0] = new TBWHit(this, 3, "ボスワンワンヒット");
	mHits[0]->initHitActor(ACTOR_TYPE_BOSS_UNKB, 3,
	                       HIT_CATEGORY_PLAYER | HIT_CATEGORY_ITEM, 500.0f,
	                       500.0f, 450.0f, 500.0f);
	mHits[1] = new TBWHit(this, -1, "ボスワンワンヒット");
	mHits[1]->initHitActor(ACTOR_TYPE_BOSS_UNKB, 3,
	                       HIT_CATEGORY_PLAYER | HIT_CATEGORY_ITEM, 300.0f,
	                       500.0f, 270.0f, 500.0f);

	TIdxGroupObj* group = static_cast<TIdxGroupObj*>(
	    JDrama::TNameRefGen::search("敵グループ"));
	for (int i = 0; i < 2; ++i) {
		group->getChildren().push_back(mHits[i]);
		mHits[i]->offHitFilter(HIT_FILTER_NO_COLLISION);
	}

	initAnmSound();
	mScaledBodyRadius = 500.0f;
	mMtxCalc          = new TBossWanwanMtxCalc(this);
	getMActor()->setCalcForBck(mMtxCalc);
	getMActor()->calc();
	offLiveFlag(LIVE_FLAG_UNK100);
	changeBck(4);
	getMActor()->setBrkFromIndex(0);
	getMActor()->setLightType(1);

	J3DModel* model = getMActor()->getModel();
	if (!model->getSkinDeform())
		model->setSkinDeform(new J3DSkinDeform, J3D_DEFORM_ATTACH_FLAG_UNK_1);

	mHitPoints = getParams()->mSLBWHitPointMax.get();
	mPullVec.zero();

	mMapCollisionManager = new TMapCollisionManager(1, "/scene/bwanwan", this);
	mMapCollisionManager->init("bwanwan_ofuro_col.col", 2, nullptr);
	mMapCollisionManager->setUpActiveCollisionTRS(mPosition, mRotation,
	                                              mScaling);
	mMapCollisionManager->removeActiveCollision();
}

void TBossWanwan::shakeCamera(int mode)
{
	if (!SMS_IsMarioTouchGround4cm())
		return;

	f32 dist   = MsSqrtf(mDistToMarioSquared);
	f32 lenMax = getParams()->mSLShakeLengthMax.get();
	f32 lenHP0 = getParams()->mSLShakeLengthMaxHP0.get();
	f32 rate;
	if (getMActor()->checkCurBckFromIndex(0))
		rate = 1.0f;
	else
		rate = getHitPointRate();

	f32 len  = lenMax * rate + lenHP0 * (1.0f - rate);
	f32 diff = len - dist;
	if (diff < 0.0f)
		return;

	f32 power = diff / len;
	if (power > 1.0f)
		power = 1.0f;

	gpCameraShake->startShake((EnumCamShakeMode)mode, power * rate);
	SMSRumbleMgr->start(8, &mPosition);
}

BOOL TBossWanwan::receiveMessage(THitActor* sender, u32 message)
{
    // FAKEMATCH(frame): 32 bytes; real construct unknown
    volatile u32 pad[8]; (void)pad;
	if (sender->getActorType() == ACTOR_TYPE_MARIO)
		return false;

	if (sender->getActorType() == ACTOR_TYPE_WATER) {
		if (mIsDead)
			return true;

		gpMarioParticleManager->emit(PARTICLE_MS_ENM_WATHIT, &sender->mPosition,
		                             0, nullptr);
		if (mHitPoints == 0) {
			SMSGetMSound()->startSoundActor(MSD_SE_BS_WANWAN_COOL_MORE,
			                                &mPosition, 0, nullptr, 0, 4);
		} else if (mHitPoints == 1) {
			gpMarioParticleManager->emitAndBindToMtxPtr(
			    0xB0, getModel()->getAnmMtx(1), 0, nullptr);
			SMSGetMSound()->startSoundActor(MSD_SE_BS_WANWAN_TO_COOL,
			                                &mPosition, 0, nullptr, 0, 4);
		} else {
			SMSGetMSound()->startSoundActor(MSD_SE_BS_WANWAN_COOL, &mPosition,
			                                0, nullptr, 0, 4);
		}
		decHitPoints();
		unk190 += 1;
		return true;
	}

	if (sender->isActorType(ACTOR_TYPE_WOOD_BARREL)) {
		sender->receiveMessage(this, HIT_MESSAGE_HIP_DROP);
		mHitPoints = 0;
		unk190 += 1;
		if (unk1A0 == 0)
			unk1A0 += 1;
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0xB0, getModel()->getAnmMtx(1), 0, nullptr);
		SMSGetMSound()->startSoundActor(MSD_SE_BS_WANWAN_TO_COOL, &mPosition, 0,
		                                nullptr, 0, 4);
	}

	return TSpineEnemy::receiveMessage(sender, message);
}

void TBossWanwan::takeBath()
{
	TEffectColumWater* column
	    = (TEffectColumWater*)gpConductor->makeOneEnemyAppear(
	        mPosition, "エフェクト水柱マネージャー", 1);
	if (column) {
		JGeometry::TVec3<f32> scale(5.0f, 5.0f, 5.0f);
		JGeometry::TVec3<f32> pos(mPosition.x, 500.0f + mPosition.y - 30.0f,
		                          mPosition.z);
		column->generate(pos, scale);
		mDived = true;
	}

	SMSGetMSound()->startSoundActor(MSD_SE_BS_WANWAN_DIVE, &mPosition, 0,
	                                nullptr, 0, 4);
}

void TBossWanwan::releasePicket()
{
	if (mIsTied) {
		JPABaseEmitter* emitter
		    = gpMarioParticleManager->emit(0xAE, &mPicket->mPosition, 0, nullptr);
		if (emitter) {
			emitter->mGlobalDynamicsScale.x = 0.3f;
			emitter->mGlobalDynamicsScale.y = 0.5f;
			emitter->mGlobalDynamicsScale.z = 0.3f;
			emitter->mGlobalParticleScale.x = 0.3f;
			emitter->mGlobalParticleScale.y = 0.5f;
			emitter->mGlobalParticleScale.z = 0.3f;
		}
	}
	unk194  = false;
	mIsTied = 0;
}

void TBossWanwan::changeBck(int idx)
{
	mMtxCalc->joinAnm(idx);
	getMActor()->setFrameCtrlForBck(idx);
	mBlendRate = 10.0f / getMActor()->getFrameCtrl(0)->getEnd();
	setAnmSound(bwanwan_bastable[idx]);
}

void TBossWanwan::startGoldBrk() { }

// TODO: guessed from the symbol size, no inlined copy seen
bool TBossWanwan::isBurning() { return mHitPoints != 0 ? true : false; }

// TODO: guessed from the symbol size, no inlined copy seen
bool TBossWanwan::isTailBurning()
{
	return mLeash->mNodes[0]->getTemperature() > 0.0f ? true : false;
}

bool TBossWanwan::isMarioInSight() { return false; }

BOOL TBossWanwan::isHeadPulled()
{
	if (mPicket->isTaken())
		return mPullVec.squared() < getParams()->mSLPullLimit.get() ? FALSE
		                                                             : TRUE;
	return FALSE;
}

void TBossWanwan::calcRootMatrix()
{
	getModel()->setBaseScale(mScaling);
	MsMtxSetXYZRPH(getModel()->getBaseTRMtx(), mPosition.x,
	               mPosition.y + 500.0f, mPosition.z, mRotation.x, mRotation.y,
	               mRotation.z);
}

// NOTE: TGraphTracer's mCurrIdx/mPrevIdx field names appear swapped w.r.t.
// how this TU uses them (offset 0x8 is the node currently being walked to).
void TBossWanwan::reverseNextGraphNode()
{
	TGraphTracer* tracer = getTracer();
	int cur              = tracer->mPrevIdx;
	TGraphWeb* web       = tracer->getGraph();
	JGeometry::TVec3<f32> toMario = SMS_GetMarioPos();
	toMario -= mPosition;
	int next = web->getAimToDirNextIndex(cur, tracer->mCurrIdx, toMario,
	                                     mPosition, 0xFFFFFFFF);
	getTracer()->mPrevIdx = next;
	getTracer()->mCurrIdx = cur;
	setGoalPathFromGraph();
	unk128 = 0;
	unk12C = 0.0f;
}

void TBossWanwan::rollNextGraphNode()
{
	TGraphTracer* tracer = getTracer();
	int cur              = tracer->mPrevIdx;
	int prev             = tracer->mCurrIdx;
	TGraphWeb* web       = tracer->getGraph();
	if (cur >= 0 && web->getGraphNode(cur).getRailNode()->mConnectionNum >= 2
	    && gpMarDirector->mMoveTickCount >= 14400)
		showMessage(0x1C);

	JGeometry::TVec3<f32> dir;
	getTracer()->moveTo(web->getEscapeDirLimited(
	    prev, cur,
	    BWSetVec(dir, 1.0f * MsSin(mRotation.y), 0.0f, 1.0f * MsCos(mRotation.y)),
	    mPosition, 100.0f, 0xFFFFFFFF));
	setGoalPathFromGraph();
	unk128 = 0;
	unk12C = 0.0f;
}

void TBossWanwan::slideToCurPathNode(f32 speed, f32 turn_speed)
{
	JGeometry::TVec3<f32> diff = getUnkF4().getPoint();
	diff -= mPosition;
	f32 dist = VECMag(&diff);

	f32 target = wrapAngle(MsGetRotFromZaxisY(diff));
	f32 delta  = MsAngleDiff(target, mRotation.y);
	if (delta > 0.0f)
		delta = MsMin(delta, turn_speed);
	else
		delta = MsMax(delta, -turn_speed);
	mRotation.y = wrapAngle(mRotation.y + delta);

	JGeometry::TVec3<f32> posDelta = mPositionDelta;
	if (dist > 0.0f)
		diff.scale(speed / dist);
	posDelta += diff;
	mPositionDelta = posDelta;
}

void TBossWanwan::control()
{
	TLiveActor::control();
	if (mIsTied || isHeadPulled()) {
		mPositionDelta += mPullVec;

		JGeometry::TVec3<f32> diff = mPosition;
		diff -= mLeash->mRope->mPoints[3].unkC;

		f32 target = wrapAngle(MsGetRotFromZaxisY(diff));
		f32 delta  = MsAngleDiff(target, mRotation.y);
		if (delta > 0.0f)
			delta = MsMin(delta, 4.0f * mTurnSpeed);
		else
			delta = MsMax(delta, 4.0f * -mTurnSpeed);
		mRotation.y = wrapAngle(mRotation.y + delta);
	}

	mPullVec.zero();
	updateSquareToMario();
}

void TBossWanwan::emitEffects()
{
	BOOL landed = false;
	if (getMActor()->checkCurBckFromIndex(4)
	    || getMActor()->checkCurBckFromIndex(5)) {
		if (getMActor()->checkBckPass(8.0f))
			landed = TRUE;
	} else if (getMActor()->checkCurBckFromIndex(2)
	           && getMActor()->checkBckPass(38.0f)) {
		landed = TRUE;
	}

	if (landed) {
		gpMarioParticleManager->emit(0xAD, &mPosition, 0, nullptr);
		gpMarioParticleManager->emit(0xAE, &mPosition, 0, nullptr);
		if (mHitPoints == 0) {
			SMSGetMSound()->startSoundActor(MSD_SE_M_BOSW_CHAIN_CAL_F,
			                                &mLeash->mRope->mPoints[0].unkC, 0,
			                                nullptr, 0, 4);
			SMSGetMSound()->startSoundActor(MSD_SE_M_BOSW_CHAIN_CAL_R,
			                                &mPicket->mPosition, 0, nullptr, 0,
			                                4);
		} else {
			SMSGetMSound()->startSoundActor(MSD_SE_M_BOSW_CHAIN_ANG_F,
			                                &mLeash->mRope->mPoints[0].unkC, 0,
			                                nullptr, 0, 4);
			SMSGetMSound()->startSoundActor(MSD_SE_M_BOSW_CHAIN_ANG_R,
			                                &mPicket->mPosition, 0, nullptr, 0,
			                                4);
		}
	}

	BOOL sparks = false;
	if (getMActor()->checkCurBckFromIndex(0)) {
		if (getMActor()->checkBckPass(72.0f))
			sparks = TRUE;
	} else if (getMActor()->checkCurBckFromIndex(4)
	           || getMActor()->checkCurBckFromIndex(5)) {
		if (getMActor()->checkBckPass(6.0f) || getMActor()->checkBckPass(12.0f))
			sparks = TRUE;
	} else if (getMActor()->checkCurBckFromIndex(2)
	           && getMActor()->checkBckPass(4.0f)) {
		sparks = TRUE;
	}

	if (sparks)
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0xAF, getModel()->getAnmMtx(1), 0, this);

	if ((getMActor()->checkCurBckFromIndex(4)
	     || getMActor()->checkCurBckFromIndex(5))
	    && getMActor()->checkBckPass(10.0f))
		shakeCamera(0x16);

	if (getMActor()->checkCurBckFromIndex(0)) {
		J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(0);
		if (ctrl->checkPass(60.0f) || ctrl->checkPass(127.0f)) {
			shakeCamera(0x16);
			gpMarioParticleManager->emit(0xAD, &mPosition, 0, nullptr);
			gpMarioParticleManager->emit(0xAE, &mPosition, 0, nullptr);
		}

		if (ctrl->checkPass(202.0f)) {
			shakeCamera(0x17);
			gpMarioParticleManager->emit(0xAD, &mPosition, 0, nullptr);
			gpMarioParticleManager->emit(0xAE, &mPosition, 0, nullptr);
		}
	}

	if (getMActor()->checkCurBckFromIndex(2)
	    && getMActor()->checkBckPass(40.0f))
		shakeCamera(0x16);

	if (mHitPoints != 0)
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x1EE, getModel()->getAnmMtx(1), 3, this);

	if (unk190 != 0 && mHitPoints != 0) {
		gpMarioParticleManager->emitAndBindToMtxPtr(
		    0x167, getModel()->getAnmMtx(1), 1, this);
		unk190 = 0;
	}
}

void TBossWanwan::showMessage(u32 message)
{
	u32 flag = 1 << (message - 0x1A);
	if (!(mMessageFlags & flag))
		gpMarDirector->getConsole()->startAppearBalloon(message, true);
	mMessageFlags |= flag;
}

void TBossWanwan::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (mIsDead) {
		mHits[0]->perform(cue, graphics);
		TSpineEnemy::perform(cue, graphics);
		if (cue & CUE_MOVE)
			mMtxCalc->advanceMotionBlend(-mBlendRate);

		if (cue & 0x200) {
			J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(5);
			if (ctrl != nullptr && ctrl->getFrame() > 0.5f * ctrl->getEnd()) {
				mEffectPos = mPosition;
				mEffectPos.y += 500.0f;
				gpMarioParticleManager->emitAndBindToPosPtr(
				    BWANWAN_JPA_MS_BWAN_KIRA, &mEffectPos, 1, this);
			}
		}
		return;
	}

	if (cue & CUE_CALC_ANIM) {
		J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(5);
		f32 rate           = getHitPointRate();
		ctrl->setFrame(rate * (ctrl->getEnd() - 1));
		ctrl->setRate(0.0f);
		if (mHitPoints == 0
		    || mHitPoints == getParams()->mSLBWHitPointMax.get())
			mLeash->mNodes[0]->mTemperature = rate;
		emitEffects();
	}

	mHits[0]->perform(cue, graphics);
	if (cue & CUE_MOVE) {
		mHits[1]->mPosition.set(mHits[0]->mPosition.x,
		                        mHits[0]->mPosition.y - 500.0f,
		                        mHits[0]->mPosition.z);
	}
	mHits[1]->perform(cue, graphics);

	if ((cue & CUE_MOVE) && !unk194) {
		unk19C += 1;
		if (unk19C < 0)
			unk19C = 1;
		if (unk19C > 600)
			showMessage(0x1A);
	}

	if ((cue & CUE_MOVE) && unk1A0 == 0
	    && gpMarDirector->mMoveTickCount >= 28800)
		showMessage(0x1D);

	if (cue & CUE_MOVE) {
		if (!mSpine->isNerve(&TNerveBWDie::theNerve())
		    && !mSpine->isNerve(&TNerveBWJumpToBath::theNerve())) {
			if (mHitPoints != 0) {
				if (mIsTied && !unk194) {
					unk184 += 1;
					if (unk184 > 600) {
						mSpine->pushNerve(&TNerveBWShake::theNerve());
						unk184 = 0;
					}
				}
			} else {
				unk184 = 0;
			}

			if (mHitPoints == 0
			    && mSpine->getLatestNerve() != &TNerveBWBark::theNerve()) {
				unk180 += 1;
				if (unk180 > 2400
				    && mSpine->getLatestNerve() != &TNerveBWBark::theNerve())
					mSpine->setNext(&TNerveBWBark::theNerve());
			} else {
				if (gpMarDirector->mMoveTickCount % 20 == 0
				    && mHitPoints < getParams()->mSLBWHitPointMax.get())
					mHitPoints += 1;
				unk180 = 0;
			}
		}
	}

	if (cue & CUE_MOVE) {
		if (mSpine->isNerve(&TNerveBWGraphWander::theNerve())
		    && mPicket->isTaken()) {
			SMSGetMSound()->startSoundActorWithInfo(
			    MSD_SE_BS_WANWAN_M_DRAG, &mLeash->mRope->mPoints[6].unkC,
			    nullptr, mPullVec.length(), 0, 0, nullptr, 0, 4);
		}
	}

	TSpineEnemy::perform(cue, graphics);
	mLeash->testPerform(cue, graphics);
	mPicket->testPerform(cue, graphics);
	if (cue & CUE_MOVE)
		mMtxCalc->advanceMotionBlend(-mBlendRate);
}

TBossWanwanManager::TBossWanwanManager(const char* name)
    : TEnemyManager(name)
{
}

void TBossWanwanManager::initJParticle()
{
	SMS_LoadParticle("/scene/bwanwan/jpa/ms_bwan_jump_rock.jpa", 0xAD);
	SMS_LoadParticle("/scene/bwanwan/jpa/ms_bwan_jump_smoke.jpa", 0xAE);
	SMS_LoadParticle("/scene/bwanwan/jpa/ms_bwan_downyuge.jpa", 0xB0);
	SMS_LoadParticle("/scene/bwanwan/jpa/ms_bwan_hibana.jpa", 0xAF);
	SMS_LoadParticle("/scene/bwanwan/jpa/ms_bwan_deadyuge.jpa", 0xB1);
	SMS_LoadParticle("/scene/bwanwan/jpa/ms_bwan_yugami.jpa", 0x1EE);
	SMS_LoadParticle("/scene/bwanwan/jpa/ms_bwan_hityuge.jpa", 0x167);
	SMS_LoadParticle("/scene/bwanwan/jpa/ms_bwan_kira.jpa", 0x168);
}

TSpineEnemy* TBossWanwanManager::createEnemyInstance()
{
	return new TBossWanwan("ボスワンワン");
}

void TBossWanwanManager::createModelData()
{
	static const TModelDataLoadEntry entry[] = {
		{ "bwanwan_body.bmd", 0x10220000, 0 },
		{ "bwanwan_chain.bmd", 0x10220000, 0 },
		{ "bwanwan_picket.bmd", 0x10220000, 0 },
		{ nullptr, 0, 0 },
	};
	createModelDataArray(entry);
}

void TBossWanwanManager::load(JSUMemoryInputStream& stream)
{
	unk38 = new TBWParams("/enemy/bosswanwan.prm");
	TEnemyManager::load(stream);
	initJParticle();
}

DEFINE_NERVE(TNerveBWGraphWander, TLiveActor)
{
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	if (spine->getTime() == 0) {
		wanwan->mIsRolling = 0;
		if (!wanwan->getMActor()->checkCurBckFromIndex(4)
		    && !wanwan->getMActor()->checkCurBckFromIndex(4)) {
			if (wanwan->mHitPoints == 0)
				wanwan->changeBck(5);
			else
				wanwan->changeBck(4);

			J3DFrameCtrl* ctrl = wanwan->getMActor()->getFrameCtrl(0);
			ctrl->setFrame(0.0f);
			ctrl->setRate(SMSGetAnmFrameRate());
		}
		wanwan->getMActor()->setBtpFromIndex(0);
		J3DFrameCtrl* ctrl = wanwan->getMActor()->getFrameCtrl(3);
		ctrl->setFrame(0.0f);
		ctrl->setRate(0.0f);
	}

	if (wanwan->getMActor()->curAnmEndsNext(0, nullptr)
	    && wanwan->mHitPoints == 0
	    && !wanwan->getMActor()->checkCurBckFromIndex(5)) {
		wanwan->changeBck(5);
		J3DFrameCtrl* ctrl = wanwan->getMActor()->getFrameCtrl(0);
		ctrl->setFrame(0.0f);
		ctrl->setRate(SMSGetAnmFrameRate());
	}

	if (!wanwan->mIsTied && wanwan->isHeadPulled()) {
		TGraphTracer* tracer = wanwan->getTracer();
		int cur              = tracer->mPrevIdx;
		TGraphWeb* web       = tracer->getGraph();
		JGeometry::TVec3<f32> diff;
		web->getGraphNode(cur).getPoint(&diff);
		diff -= wanwan->mPosition;
		if (VECMag(&diff) < 400.0f && cur == web->unk10) {
			spine->pushAfterCurrent(&TNerveBWJumpToBath::theNerve());
			return true;
		}

		if (VECMag(&diff) < 400.0f)
			wanwan->reverseNextGraphNode();

		wanwan->slideToCurPathNode(
		    3.0f * (wanwan->getHitPointRate() * wanwan->mMarchSpeed),
		    wanwan->mTurnSpeed);
		return false;
	}

	if (wanwan->isReachedToGoal()) {
		if (wanwan->jumpToNextGraphNode() >= 0) {
			spine->pushAfterCurrent(&TNerveBWGraphWander::theNerve());
			spine->pushAfterCurrent(&TNerveBWJump::theNerve());
		} else {
			spine->pushAfterCurrent(&TNerveBWGraphWander::theNerve());
			wanwan->rollNextGraphNode();
		}
		return true;
	}

	wanwan->mIsRolling = 0;
	f32 rate           = wanwan->getHitPointRate();
	if (wanwan->mPicket->isTaken()) {
		JGeometry::TVec3<f32> dir
		    = fromPolar(gpMarioOriginal->getIntendedYaw(), 1.0f);
		JGeometry::TVec3<f32> toWanwan = wanwan->mPosition;
		toWanwan -= gpMarioOriginal->mPosition;
		VECNormalize(&toWanwan, &toWanwan);
		f32 pull = (f32)(gpMarioOriginal->getIntendedMag() / 32.0f * 0.75)
		           * -toWanwan.dot(dir);
		f32 speed = 1.0f - pull;
		if (speed < 0.0f)
			speed = 0.0f;
		else if (1.5f < speed)
			speed = 1.5f;
		wanwan->slideToCurPathNode(speed * (rate * wanwan->mMarchSpeed),
		                           wanwan->mTurnSpeed);
	} else {
		wanwan->slideToCurPathNode(rate * wanwan->mMarchSpeed + 0.2f,
		                           wanwan->mTurnSpeed);
	}
	return false;
}

DEFINE_NERVE(TNerveBWRoll, TLiveActor)
{
    // FAKEMATCH(frame): 208 bytes; real construct unknown
    volatile u32 pad[52]; (void)pad;
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	if (spine->getTime() == 0) {
		J3DFrameCtrl* ctrl = wanwan->getMActor()->getFrameCtrl(0);
		ctrl->setFrame(0.0f);
		ctrl->setRate(0.0f);
		wanwan->mIsRolling = 1;
	}

	if (wanwan->isReachedToGoal()) {
		spine->pushAfterCurrent(&TNerveBWGraphWander::theNerve());
		J3DFrameCtrl* ctrl = wanwan->getMActor()->getFrameCtrl(0);
		ctrl->setRate(SMSGetAnmFrameRate());
		return true;
	}

	wanwan->walkToCurPathNode(wanwan->getParams()->mSLAttackSpeed.get(),
	                          wanwan->mTurnSpeed, 0.0f);
	return false;
}

DEFINE_NERVE(TNerveBWBark, TLiveActor)
{
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	if (spine->getTime() == 0) {
		wanwan->changeBck(0);
		wanwan->mIsRolling = 0;
		wanwan->mRollAngle = 0.0f;
		if (!wanwan->unk194) {
			wanwan->releasePicket();
			SMSGetMSound()->startSoundActor(MSD_SE_BS_WANWAN_ESCAPE1,
			                                &wanwan->mPicket->mPosition, 0,
			                                nullptr, 0, 4);
		}
	}

	if (spine->getTime() == 280)
		wanwan->mHitPoints = wanwan->getParams()->mSLBWHitPointMax.get();

	if (wanwan->getMActor()->curAnmEndsNext(0, nullptr)) {
		spine->pushAfterCurrent(&TNerveBWGraphWander::theNerve());
		wanwan->showMessage(0x1B);
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveBWJump, TLiveActor)
{
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	if (spine->getTime() == 0) {
		const JGeometry::TVec3<f32>& target = wanwan->getUnk104().getPoint();
		f32 speed                           = wanwan->getTracer()->unkC;
		wanwan->mVelocity
		    = wanwan->calcVelocityToJumpToY(target, speed, wanwan->getGravityY());
		wanwan->onLiveFlag(LIVE_FLAG_AIRBORNE);
		wanwan->mIsRolling = 0;
	}

	if (wanwan->isReachedToGoal()) {
		spine->pushAfterCurrent(&TNerveBWGraphWander::theNerve());
		return true;
	}

	wanwan->walkToCurPathNode(0.0f, wanwan->mTurnSpeed, 0.0f);
	return false;
}

DEFINE_NERVE(TNerveBWStun, TLiveActor)
{
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	if (wanwan->isHeadPulled()) {
		TGraphTracer* tracer = wanwan->getTracer();
		int cur              = tracer->mPrevIdx;
		TGraphWeb* web       = tracer->getGraph();
		JGeometry::TVec3<f32> diff;
		web->getGraphNode(cur).getPoint(&diff);
		diff -= wanwan->mPosition;
		if (VECMag(&diff) < 100.0f) {
			if (cur == web->unk10) {
				spine->pushAfterCurrent(&TNerveBWJumpToBath::theNerve());
				return true;
			}
			wanwan->reverseNextGraphNode();
		}
		return false;
	}

	if (spine->getTime() > wanwan->getParams()->mSLStunTimer.get()) {
		wanwan->mHitPoints = wanwan->getParams()->mSLBWHitPointMax.get();
		spine->pushAfterCurrent(&TNerveBWWakeup::theNerve());
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveBWWakeup, TLiveActor)
{
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	if (spine->getTime() == 0) {
		wanwan->changeBck(6);
		wanwan->getMActor()->setBtpFromIndex(2);
		wanwan->mIsRolling = 0;
		wanwan->mRollAngle = 0.0f;
	}

	if (wanwan->getMActor()->curAnmEndsNext(0, nullptr)) {
		spine->pushAfterCurrent(&TNerveBWGraphWander::theNerve());
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveBWJumpToBath, TLiveActor)
{
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	if (spine->getTime() == 0) {
		JGeometry::TVec3<f32> velocity = wanwan->calcVelocityToJumpToY(
		    BW_BATH_POS, 10.0f, wanwan->getGravityY());
		wanwan->setGoalPath(TPathNode(BW_BATH_POS));
		wanwan->mVelocity = velocity;
		wanwan->onLiveFlag(LIVE_FLAG_AIRBORNE);
		wanwan->mIsRolling = 0;
	}

	if (spine->getTime() > 120 && !wanwan->mDived
	    && wanwan->mPosition.y < BW_BATH_POS.y + 10.0f)
		wanwan->takeBath();

	JGeometry::TVec3<f32> bath;
	bath = BW_BATH_POS;
	JGeometry::TVec3<f32> diff;
	diff = bath;
	diff -= wanwan->mPosition;
	if (diff.squared() < 10000.0f && wanwan->mPosition.y <= BW_BATH_POS.y) {
		wanwan->mPosition = bath;
		spine->pushAfterCurrent(&TNerveBWDie::theNerve());
		return true;
	}

	wanwan->walkToCurPathNode(0.0f, wanwan->mTurnSpeed, 0.0f);
	return false;
}

// fabricated: wrap the inline setUpActiveCollisionTRS body so TMapCollisionBase::setMtx stays out of line like the original
static inline void BWDieSetMtx0(TMapCollisionBase* c, MtxPtr mtx)
{
	c->setMtx(mtx);
}
static inline void BWDieSetMtx(TMapCollisionBase* c, MtxPtr mtx)
{
	BWDieSetMtx0(c, mtx);
}
static inline void BWDieSetUpMtx(TMapCollisionBase* c, MtxPtr mtx)
{
	BWDieSetMtx(c, mtx);
	c->setUp();
}
static inline void BWDieSetUpActiveTRS(TMapCollisionManager* m,
                                       const JGeometry::TVec3<f32>& trans,
                                       const JGeometry::TVec3<f32>& rot,
                                       const JGeometry::TVec3<f32>& scale)
{
	Mtx mtx;
	MsMtxSetTRS(mtx, trans.x, trans.y, trans.z, rot.x, rot.y, rot.z, scale.x,
	            scale.y, scale.z);
	BWDieSetUpMtx(m->getActiveCollision(), mtx);
}

DEFINE_NERVE(TNerveBWDie, TLiveActor)
{
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	MActor* actor       = wanwan->getMActor();
	if (spine->getTime() == 0) {
		JGeometry::TVec3<f32> zero(0.0f, 0.0f, 0.0f);
		wanwan->mVelocity      = zero;
		wanwan->mPositionDelta = zero;
		wanwan->onLiveFlag(LIVE_FLAG_UNK10);
		wanwan->offLiveFlag(LIVE_FLAG_AIRBORNE);
		gpMarioParticleManager->emit(0xB1, &wanwan->mPosition, 0, nullptr);
	}

	if (wanwan->mHitPoints != 0) {
		wanwan->decHitPoints();
		J3DFrameCtrl* ctrl = actor->getFrameCtrl(0);
		ctrl->setFrame(0.0f);
		ctrl->setRate(0.0f);
		spine->pushAfterCurrent(&TNerveBWDie::theNerve());
		return true;
	}

	if (spine->getTime() == 0) {
		gpMarDirector->fireStartDemoCamera("bwanwan_down_camera", nullptr, -1,
		                                   0.0f, true, nullptr, 0, nullptr,
		                                   JDrama::TFlagT<u16>(0));
		wanwan->mIsRolling = 0;
		wanwan->mRollAngle = 0.0f;
		wanwan->mIsDead    = true;
		wanwan->mPosition  = BW_BATH_POS;

		JGeometry::TVec3<f32> pos = wanwan->mPosition;
		pos.y += 500.0f;
		JGeometry::TVec3<f32> scale = wanwan->mScaling;
		scale.x *= 1.1f;
		scale.y *= 1.1f;
		scale.z *= 1.1f;
		BWDieSetUpActiveTRS(wanwan->getMapCollisionManager(),
		    pos, wanwan->mRotation, scale);

		wanwan->mHits[0]->onHitFilter(HIT_FILTER_NO_COLLISION);
		wanwan->mHits[1]->onHitFilter(HIT_FILTER_NO_COLLISION);
		wanwan->mLeash->invalidateAllCollision();
		wanwan->mPicket->onHitFilter(HIT_FILTER_NO_COLLISION);
		wanwan->changeBck(1);
		actor->setBtpFromIndex(0);
		actor->setBrkFromIndex(1);
	}

	if (spine->getTime() > 60 && !wanwan->mShineAppeared
	    && gpMarDirector->unk124 != 3) {
		gpItemManager->makeShineAppearWithDemo(
		    "シャイン（ボス用）", "ボスシャインカメラ", wanwan->mPosition.x,
		    wanwan->mPosition.y, wanwan->mPosition.z);
		wanwan->mShineAppeared = true;
	}

	if (actor->curAnmEndsNext(5, nullptr))
		actor->getFrameCtrl(5)->setRate(0.0f);

	return false;
}

DEFINE_NERVE(TNerveBWJumpAway, TLiveActor)
{
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	if (spine->getTime() == 0) {
		JGeometry::TVec3<f32> velocity = wanwan->calcVelocityToJumpToY(
		    BW_HEAD_START, 40.0f, wanwan->getGravityY());
		wanwan->setGoalPath(TPathNode(BW_HEAD_START));
		wanwan->mVelocity = velocity;
		wanwan->onLiveFlag(LIVE_FLAG_AIRBORNE);
		wanwan->mIsRolling = 0;
	}

	if (wanwan->unk188) {
		spine->pushAfterCurrent(&TNerveBWFall::theNerve());
		return true;
	}

	if (wanwan->isReachedToGoal()) {
		wanwan->mPosition = BW_HEAD_START;
		wanwan->getTracer()->mPrevIdx = -1;
		wanwan->getTracer()->mCurrIdx = -1;
		wanwan->goToShortestNextGraphNode();
		spine->pushAfterCurrent(&TNerveBWGraphWander::theNerve());
		return true;
	}

	wanwan->walkToCurPathNode(0.0f, wanwan->mTurnSpeed, 0.0f);
	return false;
}

DEFINE_NERVE(TNerveBWShake, TLiveActor)
{
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	MActor* actor       = wanwan->getMActor();
	if (spine->getTime() == 0)
		wanwan->changeBck(2);

	if (actor->curAnmEndsNext(0, nullptr)) {
		wanwan->releasePicket();
		SMSGetMSound()->startSoundActor(MSD_SE_BS_WANWAN_ESCAPE2,
		                                &wanwan->mPicket->mPosition, 0, nullptr,
		                                0, 4);
		return true;
	}

	return false;
}

DEFINE_NERVE(TNerveBWFall, TLiveActor)
{
	TBossWanwan* wanwan = (TBossWanwan*)spine->getBody();
	if (spine->getTime() == 0) {
		const JGeometry::TVec3<f32>& picketPos = wanwan->mPicket->mPosition;
		JGeometry::TVec3<f32> velocity
		    = wanwan->calcVelocityToJumpToY(picketPos, 5.0f, wanwan->getGravityY());
		wanwan->setGoalPath(TPathNode(wanwan->mPicket->mPosition));
		wanwan->mVelocity = velocity;
		wanwan->onLiveFlag(LIVE_FLAG_AIRBORNE);
		wanwan->mIsRolling = 0;
	}

	if (wanwan->isReachedToGoal()) {
		wanwan->mPosition             = wanwan->mPicket->mPosition;
		wanwan->getTracer()->mPrevIdx = -1;
		wanwan->getTracer()->mCurrIdx = -1;
		wanwan->goToShortestNextGraphNode();
		spine->pushAfterCurrent(&TNerveBWGraphWander::theNerve());
		return true;
	}

	return false;
}
