#include <NPC/NpcInbetween.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <M3DUtil/MActor.hpp>

void TNpcInbetween::execPosInbetween(JGeometry::TVec3<f32>* cur_pos)
{
	mCurrentPos.set(*cur_pos);
	if (mPosInbetweenTimer >= 2) {
		mPosInbetweenTimer -= 1;
		calcInbetweenPos(cur_pos);
	} else {
		mTargetPos.set(*cur_pos);
		mPosInbetweenTimer = 0;
	}
}

void TNpcInbetween::execMotionBlend(MActor* mactor)
{
	f32 ratio = 0.0f;
	if (isForcedBlendRatio()) {
		mMotionBlendTimer = 0;

		J3DAnmTransform* anm = mactor->getBckOldMotionBlendAnmPtr();

		if (anm != nullptr) {
			J3DFrameCtrl ctrl = *mactor->getFrameCtrl(ANM_TYPE_BCK);
			ctrl.update();
			anm->setFrame(ctrl.getFrame());
			ratio = mForcedBlendRatio;
		}
	} else if (isMotionBlending()) {
		if (mMotionBlendTimer > 0)
			mMotionBlendTimer -= 1;
		if (mMotionBlendTimer > 0) {
			J3DAnmTransform* anm = mactor->getBckOldMotionBlendAnmPtr();

			if (anm != nullptr)
				anm->setFrame(mactor->getBckOldMotionBlendFrame());

			ratio = mMotionBlendTimer * (1.0f / mMotionBlendFrame);
		}
	}

	mactor->setMotionBlendRatioForBck(ratio);
}
