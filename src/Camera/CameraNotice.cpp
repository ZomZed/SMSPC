#include <Camera/Camera.hpp>
#include <Camera/CameraKindParam.hpp>
#include <Camera/CameraMarioData.hpp>
#include <Camera/camerasave.hpp>
#include <Camera/cameralib.hpp>
#include <Enemy/Conductor.hpp>
#include <Strategic/LiveManager.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/WaterGun.hpp>
#include <System/MarioGamePad.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/JMath.hpp>
#include <Enemy/Enemy.hpp>

static const char* dummyMactorStringValue1 = "\0\0\0\0\0\0\0\0\0\0\0";
static const char* SMS_NO_MEMORY_MESSAGE   = "メモリが足りません\n";

static const char* sNoticeActorManagerName[] = {
	"ヒノクリマネージャー",
	"ヒノクリ２マネージャー",
	"ボスパックンマネージャー",
	nullptr,
};

const char* bossGesoViewObjName = "ボスゲッソー";

void CPolarSubCamera::setNoticeInfo()
{
	unk2A0       = new TLiveActor*[0x10];
	mNoticeActor = nullptr;
	unk29C       = 0;

	for (int i = 0; sNoticeActorManagerName[i] != nullptr; ++i) {
		TLiveManager* mgr
		    = gpConductor->getManagerByName(sNoticeActorManagerName[i]);

		if (mgr != nullptr) {
			s32 e = mgr->getObjNum();
			for (int j = 0; j < e; ++unk29C, ++j)
				unk2A0[unk29C] = mgr->getObj(j);
		}
	}

	unk2A8 = static_cast<TLiveActor*>(
	    JDrama::TNameRefGen::search(bossGesoViewObjName));
}

// fabricated
static inline bool isInClipRatio(const CPolarSubCamera* camera, const Vec& pos,
                                 f32 ratio)
{
	JGeometry::TVec2<f32> clipPos;
	CLBCalc2DFPos(&clipPos, camera->unk16C, camera->unk1EC, pos, nullptr,
	              false);

	bool inClip  = false;
	bool inClipY = false;
	bool inClipX = false;
	if (-ratio <= clipPos.x && clipPos.x <= ratio)
		inClipX = true;
	if (inClipX && -ratio <= clipPos.y)
		inClipY = true;
	if (inClipY && clipPos.y <= ratio)
		inClip = true;
	if (inClip)
		return true;
	return false;
}

static inline f32 noticeDistSq(const JGeometry::TVec3<f32>& a, const Vec& b)
{
	f32 dx = a.x - b.x;
	f32 dy = a.y - b.y;
	f32 dz = a.z - b.z;
	f32 sx = dx * dx;
	f32 sy = dy * dy;
	f32 sz = dz * dz;
	return sz + (sx + sy);
}

TLiveActor* CPolarSubCamera::getNoticeActor_()
{
	if (mNoticeActor != nullptr && !mNoticeActor->checkLiveFlag(LIVE_FLAG_DEAD)
	    && !mNoticeActor->checkLiveFlag(LIVE_FLAG_HIDDEN)) {

		f32 noticeDist2 = noticeDistSq(mNoticeActor->mPosition, SMS_GetMarioPos());
		if (noticeDist2 < CLBSquared<f32>(mSaveNotice->mOffDist.get())) {
			if (isInClipRatio(this, mNoticeActor->mPosition,
			                  mSaveNotice->mOffClipRatio.get()))
				return mNoticeActor;
		}
	}

	TLiveActor* noticeActor = nullptr;
	f32 closestDist2        = CLBSquared<f32>(mSaveNotice->mOnDist.get());

	for (int i = 0; i < unk29C; i++) {
		if (unk2A0[i]->checkLiveFlag(LIVE_FLAG_DEAD)
		    || unk2A0[i]->checkLiveFlag(LIVE_FLAG_HIDDEN))
			continue;

		if (mNoticeActor != nullptr && mNoticeActor == unk2A0[i])
			continue;

		f32 dist2 = noticeDistSq(unk2A0[i]->mPosition, SMS_GetMarioPos());
		if (!(dist2 < closestDist2))
			continue;

		if (!isInClipRatio(this, unk2A0[i]->mPosition,
		                   mSaveNotice->mOnClipRatio.get()))
			continue;

		if (!MsIsInSight(*gpMarioPos, SHORTANGLE2DEG(SMS_GetMarioAngleY()),
		                 unk2A0[i]->mPosition, dist2,
		                 mSaveNotice->mOnDegree.get(), -1.0f))
			continue;

		closestDist2 = dist2;
		noticeActor  = unk2A0[i];
	}

	return noticeActor;
}

void CPolarSubCamera::execNoticeOnOffProc_(EnumNoticeOnOffMode mode)
{
	switch (mode) {
	case NOTICE_MODE_UNK0:
		mNoticeActor = nullptr;
		unk64 &= ~CAMERA_FLAG_NOTICE_ACTIVE;
		break;
	case NOTICE_MODE_UNK1: {
		TLiveActor* actor = getNoticeActor_();
		if (actor != mNoticeActor && actor == nullptr) {
			mNoticeActor = nullptr;
			unk64 &= ~CAMERA_FLAG_NOTICE_ACTIVE;
		}
		break;
	}
	case NOTICE_MODE_UNK2: {
		TLiveActor* actor = getNoticeActor_();
		if (actor != mNoticeActor && actor != nullptr) {
			mNoticeActor = actor;
			unk64 |= CAMERA_FLAG_NOTICE_ACTIVE;
		}
		break;
	}
	}
}

void CPolarSubCamera::calcNoticeTargetYrot_(const Vec& target)
{
	Vec mPos     = gpCameraMario->unk0;
	f32 dist2    = CLBSquared<f32>(mPos.x - target.x)
	          + CLBSquared<f32>(mPos.z - target.z);
	f32 farClip2 = CLBSquared<f32>(mSaveNotice->mRotateMinDistXZ.get());
	f32 near2    = CLBSquared<f32>(mSaveNotice->mRotateFastMinDistXZ.get());

	if (dist2 > farClip2) {
		JGeometry::TVec3<f32> diff(mPos.x - target.x, mPos.y - target.y,
		                           mPos.z - target.z);
		MsVECNormalize(&diff, &diff);
		// TODO: many inlines from cameralib maybe?
		f32 dx       = diff.x * 500.0f + mPos.x;
		f32 dz       = diff.z * 500.0f + mPos.z;
		f32 ax       = dx - mCurrentTarget.mTarget.x;
		f32 az       = dz - mCurrentTarget.mTarget.z;
		s16 ang      = matan(az, ax);
		s16 angDiff  = mCurrentTarget.mYaw - ang;
		int absAngle = angDiff >= 0 ? angDiff : -angDiff;
		f32 ratio    = (1.0f / 32768.0f) * (f32)absAngle;

		f32 chase;
		if (dist2 > near2) {
			chase = 1.0f;
		} else {
			chase = CLBCalcRatio<f32>(farClip2, near2, dist2);
		}
		f32 base = CLBLinearInbetween<f32>(
		    1.0f, mSaveNotice->mRotateMagnifXmax.get(), mCurrentTarget.unk28);
		f32 speed
		    = unk288
		      * (base
		         * (chase * (ratio * (f32)mSaveNotice->mRotateYSpeed.get())));
		if (speed > 32766.998f)
			speed = 32766.998f;
		s16 delta = CLBRoundf<s16>(speed);
		CLBChaseGeneralConstantSpecifySpeed<s16>(&mCurrentTarget.mYaw, ang,
		                                         delta);
	}
}

void CPolarSubCamera::getNozzleTopPos_(JGeometry::TVec3<f32>* out) const
{
	// FAKEMATCH(frame): 24 bytes; real construct unknown
	volatile u32 pad[6]; (void)pad;
	if (SMS_GetMarioWaterGun() == nullptr) {
		out->set(gpCameraMario->unk0);
	} else {
		MtxPtr mtx = SMS_GetMarioWaterGun()->getNozzleMtx();
		out->x     = mtx[0][3];
		out->y     = mtx[1][3];
		out->z     = mtx[2][3];

		JGeometry::TVec3<f32> dir(mtx[0][1], mtx[1][1], mtx[2][1]);
		dir.normalize();
		dir *= 30.0f;
		*out += dir;
	}
}

void CPolarSubCamera::ctrlLButtonCamera_()
{
	f32 stickX = -unk120->mCompSPos[4];
	f32 stickY = -unk120->mCompSPos[5];

	if (mTargetFreezeFrames == 0) {
		if (!SMS_CheckMarioFlag(MARIO_FLAG_HAS_FLUDD))
			mCurrentTarget.mTarget.set(gpCameraMario->unk0);
		else
			getNozzleTopPos_(&mCurrentTarget.mTarget);
	}

	if (mPosFreezeFrames == 0) {
		if (unk64 & CAMERA_FLAG_NOTICE_ACTIVE) {
			if (stickX != 0.0f) {
				rotateY_ByStickX_(stickX);
			} else {
				calcNoticeTargetYrot_(mNoticeActor->mPosition);
			}
		} else {
			rotateY_ByStickX_(stickX);
		}
		rotateX_ByStickY_(stickY);
	}

	calcPosAndAt_();
}
