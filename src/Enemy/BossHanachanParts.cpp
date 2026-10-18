#include <Enemy/BossHanachanParts.hpp>
#include <Enemy/BossHanachan.hpp>
#include <Enemy/BossHanachanChangeSaveParams.hpp>
#include <Camera/cameralib.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/MActorAnm.hpp>
#include <Map/MapCollisionEntry.hpp>
#include <Map/MapData.hpp>
#include <MarioUtil/DrawUtil.hpp>
#include <MarioUtil/MapUtil.hpp>
#include <MarioUtil/ShadowUtil.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/ObjModel.hpp>
#include <Strategic/Spine.hpp>
#include <Strategic/Strategy.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#ifdef SMS_PORT
#include <stdlib.h> // getenv (SMS_PORT_BOSSTRACE, w5)
#endif

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

const char* cMapCollisionJointName = "center";
const char* cBodyMapCollisionFileName
    = "/scene/bosshanachan/hanabody_col.col";
const char* cHeadMapCollisionFileName
    = "/scene/bosshanachan/hanahead_col.col";
const char* cLegJointName_L3     = "leg_L3";
const char* cLegJointName_R3     = "leg_R3";
const char* cNoseHallJointName_L = "L_hall";
const char* cNoseHallJointName_R = "R_hall";

static void CalcMtxPtrFromJointName(JUTNameTab* name_tab, const char* name,
                                    J3DModel* model, MtxPtr* out)
{
	u16 idx = name_tab->getIndex(name);
	*out    = model->getAnmMtx(idx);
}

TBossHanachanPartsBase::TBossHanachanPartsBase(TBossHanachan* owner,
                                               u32 actor_type, int model_index,
                                               const char* name)
    : TLiveActor(name)
    , unkF4(BOSS_HANACHAN_ANM_UNK18)
    , unkF8(BOSS_HANACHAN_ANM_UNK18)
    , unkFC(owner)
    , unk100(nullptr)
    , unk104(nullptr)
    , unk108(nullptr)
    , unk10C(0)
    , unk110(nullptr)
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	mMActorKeeper = unkFC->mMActorKeeper;
	mMActor       = mMActorKeeper->createMActorFromNthData(model_index, 0);
	mMActor->initNormalMotionBlend();
	initHitActor(actor_type, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f);
	onHitFilter(HIT_FILTER_NO_COLLISION);
	switch (actor_type) {
	case ACTOR_TYPE_BOSS_UNK15:
		mScaledBodyRadius = unkFC->unk1BC->mSLBodyShadowSize.get();
		break;
	case ACTOR_TYPE_BOSS_UNK14:
		mScaledBodyRadius = unkFC->unk1BC->mSLHeadShadowSize.get();
		break;
	}
	onLiveFlag(LIVE_FLAG_UNK8);
	initAnmSound();
	mMActor->setLightType(1);
	unk110 = new TNpcInbetween(
	    1, CLBPalFrame(unkFC->unk1BC->mSLMotionBlendFrames.get()));
}

TBossHanachanPartsBody::TBossHanachanPartsBody(TBossHanachan* owner,
                                               const char* name)
    : TBossHanachanPartsBase(owner, ACTOR_TYPE_BOSS_UNK15, 0, name)
    , unk114(0)
    , unk120(0.0f)
    , unk124(0.0f, 0.0f, 0.0f)
    , unk130(0.0f, 0.0f, 0.0f)
    , unk13C(0.0f)
    , unk140(0.0f)
    , unk144(0.0f)
    , unk148(0.0f)
    , unk154(0.0f, 0.0f, 0.0f)
{
	J3DModel* model    = getModel();
	JUTNameTab* jnames = model->getModelData()->getJointName();
	CalcMtxPtrFromJointName(jnames, cLegJointName_L3, model, &unk14C[0]);
	CalcMtxPtrFromJointName(jnames, cLegJointName_R3, model, &unk14C[1]);
}

TBossHanachanPartsHead::TBossHanachanPartsHead(TBossHanachan* owner,
                                               const char* name)
    : TBossHanachanPartsBase(owner, ACTOR_TYPE_BOSS_UNK14, 1, name)
{
	J3DModel* model    = getModel();
	JUTNameTab* jnames = model->getModelData()->getJointName();
	CalcMtxPtrFromJointName(jnames, cNoseHallJointName_L, model, &unk114);
	CalcMtxPtrFromJointName(jnames, cNoseHallJointName_R, model, &unk118);
}

void TBossHanachanPartsBase::initMapCollisionAndHitActor_(TIdxGroupObj* group)
{
	TBossHanachanCommonSaveParams* params = unkFC->unk1BC;

	const char* colName = cBodyMapCollisionFileName;
	f32 attackRadius    = params->mSLBodyAttackRadius.get();
	f32 attackHeight    = params->mSLBodyAttackHeight.get();
	f32 damageRadius    = params->mSLBodyDamageRadius.get();
	f32 damageHeight    = params->mSLBodyDamageHeight.get();
	f32 hitOffsetY      = params->mSLBodyHitOffsetY.get();
	switch (mActorType) {
	case ACTOR_TYPE_BOSS_UNK15:
		break;
	case ACTOR_TYPE_BOSS_UNK14:
		colName      = cHeadMapCollisionFileName;
		attackRadius = params->mSLHeadAttackRadius.get();
		attackHeight = params->mSLHeadAttackHeight.get();
		damageRadius = params->mSLHeadDamageRadius.get();
		damageHeight = params->mSLHeadDamageHeight.get();
		hitOffsetY   = params->mSLHeadHitOffsetY.get();
		break;
	}

	u16 centerIdx = getMActor()
	                    ->getModel()
	                    ->getModelData()
	                    ->getJointName()
	                    ->getIndex(cMapCollisionJointName);
	unk108        = getMActor()->getModel()->getAnmMtx(centerIdx);

	unk104 = new TMapCollisionMove;
	unk104->init(colName, 0x8000, this);

	unk100 = new TWaterHitActor("ボスハナチャンパーツ");
	unk100->initHitActor(mActorType, 1, HIT_CATEGORY_PLAYER, attackRadius,
	                     attackHeight, damageRadius, damageHeight);
	group->getChildren().push_back(unk100);
	unk100->offHitFilter(HIT_FILTER_NO_COLLISION);
	unk100->mPosition.set(unk108[0][3], unk108[1][3] - hitOffsetY,
	                      unk108[2][3]);
}

void TBossHanachanPartsBody::initFootHitActor_(TIdxGroupObj* group)
{
	static const char* sFootJointName[] = { "foot_L", "foot_R" };

	TBossHanachanCommonSaveParams* params = unkFC->unk1BC;
	JUTNameTab* jnames
	    = getMActor()->getModel()->getModelData()->getJointName();
	int jointIndex[2];
	for (int i = 0; i < 2; ++i) {
		jointIndex[i] = jnames->getIndex(sFootJointName[i]);
		unk118[i]     = new TFootHitActor("ボスハナチャンの足");
		unk118[i]->initHitActor(mActorType, 1, HIT_CATEGORY_PLAYER,
		                        params->mSLFootAttackRadius.get(),
		                        params->mSLFootAttackHeight.get(),
		                        params->mSLFootDamageRadius.get(),
		                        params->mSLFootDamageHeight.get());
		group->getChildren().push_back(unk118[i]);
		unk118[i]->offHitFilter(HIT_FILTER_NO_COLLISION);
		MtxPtr mtx = getMActor()->getModel()->getAnmMtx((u16)jointIndex[i]);
		unk118[i]->unk6C = mtx;
		unk118[i]->mPosition.set(mtx[0][3], mtx[1][3], mtx[2][3]);
	}
}

void TBossHanachanPartsBase::restartBck_()
{
	getMActor()->getFrameCtrl(ANM_TYPE_BCK)->setFrame(0.0f);
}

void TBossHanachanPartsBase::changeTumbleAnmRate_()
{
	J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(ANM_TYPE_BCK);
	switch (unkF4) {
	case BOSS_HANACHAN_ANM_UNK16:
	case BOSS_HANACHAN_ANM_UNK17:
	{
		f32 frame = ctrl->getFrame();
		if (frame <= 40.0f)
			break;
		f32 frames = ctrl->getEnd() - frame;
		f32 rate   = ctrl->getRate();
		CLBChaseConstantSpecifyFrame(&rate, SMSGetAnmFrameRate(), frames);
		ctrl->setRate(rate);
		break;
	}
	default:
		ctrl->setRate(SMSGetAnmFrameRate());
		break;
	}
}

void TBossHanachanPartsBase::moveMapCollision_()
{
	JGeometry::TVec3<f32> pos(unk108[0][3], unk108[1][3], unk108[2][3]);
	unk104->moveTrans(pos);
}

static inline const TNerveBase<TLiveActor>*
getLatestNerveOf(TBossHanachan* boss)
{
	return boss->mSpine->getLatestNerve();
}

void TBossHanachanPartsBase::entryCircleShadow_()
{
	if (unkFC->mSpine->getCurrentNerve() != &TNerveBossHanachanDead::theNerve()
	    || unkFC->mSpine->getTime() <= 200) {
		TCircleShadowRequest request;
		request.mPosition.set(unk108[0][3], unk108[1][3], unk108[2][3]);
		request.mRadiusX = request.mRadiusZ = mScaledBodyRadius;
		u32 type = mActorType;
		gpBindShadowManager->forceRequest(request, type);
	}
}

void TBossHanachanPartsBase::setDamageFog_(JDrama::TGraphics* graphics)
{
	bool changeMaterials = true;
	if (mActorType == ACTOR_TYPE_BOSS_UNK14)
		changeMaterials = false;

	MtxPtr mtx                 = unk108;
	J3DModelData* modelData    = getMActor()->getModel()->getModelData();
	u16 materialNum            = modelData->getMaterialNum();
	JGeometry::TVec3<f32> pos(mtx[0][3], mtx[1][3], mtx[2][3]);
	if (getLatestNerveOf(unkFC)
	    == &TNerveBossHanachanDamage::theNerve()) {
		SMS_AddDamageFogEffect(modelData, pos, graphics);
		if (changeMaterials)
			for (u16 i = 0; i < materialNum; ++i)
				modelData->getMaterialNodePointer(i)->change();
		if (unk10C == 0)
			getMActor()->getModel()->unlock();
	} else {
		SMS_ResetDamageFogEffect(modelData);
	}
}

bool TBossHanachanPartsBase::isCurBckAlreadyEnd_() const
{
	bool result = true;
	if (mMActor != nullptr) {
		J3DFrameCtrl* ctrl = mMActor->getFrameCtrl(ANM_TYPE_BCK);
		if (ctrl != nullptr) {
			bool ended = true;
			bool stopped
			    = ctrl->checkState(J3DFrameCtrl::STATE_COMPLETED_ONCE)
			      || ctrl->checkState(J3DFrameCtrl::STATE_LOOPED_ONCE);
			if (!stopped && !(ctrl->getFrame() + 0.1f >= ctrl->getEnd()))
				ended = false;
			result = ended;
		}
	}
	return result;
}

void TBossHanachanPartsBase::copyFrameFromOldAnmToNewAnm_()
{
    // FAKEMATCH(frame): 16 bytes; real construct unknown
    volatile u32 pad[4]; (void)pad;
	J3DAnmTransformKey* anm = getMActor()->getBckAnm();
	J3DFrameCtrl* ctrl      = getMActor()->getFrameCtrl(ANM_TYPE_BCK);
	if (anm != nullptr && ctrl != nullptr) {
		f32 frame = getMActor()->getBckOldMotionBlendFrame();
		anm->setFrame(frame);
		ctrl->setFrame(frame);
	}
}

const TLiveActor* TBossHanachanPartsBase::getSandActor_() const
{
	const TLiveActor* actor
	    = SMS_GetGroundActor(mGroundPlane, ACTOR_TYPE_SAND_BOMB_BASE00);
	if (actor == nullptr)
		actor = SMS_GetGroundActor(mGroundPlane, ACTOR_TYPE_SAND_LEAF_BASE00);
	return actor;
}

bool TBossHanachanPartsBase::isMarioOn_() const
{
	bool result = false;
	if (SMS_IsMarioTouchGround4cm()) {
		const TBGCheckData* plane = *gpMarioGroundPlane;
		if (plane != nullptr && plane->getActor() == this)
			result = true;
	}
	return result;
}

void TBossHanachanPartsBase::calcRotateZWhenGetUp_()
{
	if (unk10C != 0)
		return;

	switch (unkF4) {
	case BOSS_HANACHAN_ANM_UNK8:
	case BOSS_HANACHAN_ANM_UNK11: {
		J3DFrameCtrl* ctrl = getMActor()->getFrameCtrl(ANM_TYPE_BCK);
		f32 frames         = 2.0f * (ctrl->getEnd() - ctrl->getFrame());
		// TODO: the out-of-line call to CLBChaseConstantSpecifyFrame in the
		// target is preceded by its own early-out check.
		if (frames < 0.001f)
			mRotation.z = 0.0f;
		else
			CLBChaseConstantSpecifyFrame(&mRotation.z, 0.0f, frames);
		break;
	}
	}
}

// fabricated
static inline bool isRotateZTumbled(const f32& rot_z)
{
	bool tumbled = true;
	if (-179.0f != rot_z && 179.0f != rot_z)
		tumbled = false;
	return tumbled ? true : false;
}

bool TBossHanachanPartsBase::isReactToTrampleOrHipDrop_() const
{
	bool result = false;
	const TNerveBase<TLiveActor>* nerve = getLatestNerveOf(unkFC);
	if (nerve == &TNerveBossHanachanTumble::theNerve()
	    || nerve == &TNerveBossHanachanDown::theNerve()) {
		if (mActorType == ACTOR_TYPE_BOSS_UNK15) {
			if (isRotateZTumbled(mRotation.z))
				result = true;
		} else {
			result = true;
		}
	}
	return result;
}

void TBossHanachanPartsBase::considerSetAnm_(EnumBossHanachanNerveAnm anm)
{
	if (anm == BOSS_HANACHAN_NERVE_ANM_UNK0) {
		switch (unkF4) {
		case BOSS_HANACHAN_ANM_UNK5:
		case BOSS_HANACHAN_ANM_UNK6:
		case BOSS_HANACHAN_ANM_UNK13:
		case BOSS_HANACHAN_ANM_UNK16:
		case BOSS_HANACHAN_ANM_UNK17:
			if (isCurBckAlreadyEnd_())
				setAnm_(BOSS_HANACHAN_ANM_UNK3,
				        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
			break;
		}
	} else if (anm == BOSS_HANACHAN_NERVE_ANM_UNK1) {
		bool marioOn = isMarioOn_();
		int curAnm   = unkF4;
		switch (curAnm) {
		case BOSS_HANACHAN_ANM_UNK5:
		case BOSS_HANACHAN_ANM_UNK6:
		case BOSS_HANACHAN_ANM_UNK13:
		case BOSS_HANACHAN_ANM_UNK16:
		case BOSS_HANACHAN_ANM_UNK17:
			if (isCurBckAlreadyEnd_()) {
				if (mActorType == ACTOR_TYPE_BOSS_UNK15 && marioOn)
					setAnm_(BOSS_HANACHAN_ANM_UNK2,
					        BOSS_HANACHAN_STOP_MOTION_BLEND_ON);
				else
					setAnm_(BOSS_HANACHAN_ANM_UNK3,
					        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
			}
			break;
		default:
			if (mActorType == ACTOR_TYPE_BOSS_UNK15) {
				bool blending = true;
				if (!unk110->isMotionBlending()
				    && !unk110->isForcedBlendRatio())
					blending = false;
				if (!blending) {
					if (curAnm == BOSS_HANACHAN_ANM_UNK2) {
						if (!marioOn)
							setAnm_(BOSS_HANACHAN_ANM_UNK3,
							        BOSS_HANACHAN_STOP_MOTION_BLEND_ON);
					} else if (marioOn) {
						setAnm_(BOSS_HANACHAN_ANM_UNK2,
						        BOSS_HANACHAN_STOP_MOTION_BLEND_ON);
					}
				}
			}
			break;
		}
	} else if (anm == BOSS_HANACHAN_NERVE_ANM_UNK2) {
		if (unk10C > 0)
			--unk10C;
		if (unk10C == 0) {
			bool ended = isCurBckAlreadyEnd_();
			switch (unkF4) {
			case BOSS_HANACHAN_ANM_UNK7:
				if (ended)
					setAnm_(BOSS_HANACHAN_ANM_UNK8,
					        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
				break;
			case BOSS_HANACHAN_ANM_UNK8:
				if (ended)
					setAnm_(BOSS_HANACHAN_ANM_UNK9,
					        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
				break;
			case BOSS_HANACHAN_ANM_UNK9:
				break;
			case BOSS_HANACHAN_ANM_UNK10:
				if (ended)
					setAnm_(BOSS_HANACHAN_ANM_UNK11,
					        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
				break;
			case BOSS_HANACHAN_ANM_UNK11:
				if (ended)
					setAnm_(BOSS_HANACHAN_ANM_UNK12,
					        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
				break;
			case BOSS_HANACHAN_ANM_UNK12:
				break;
			default:
				if (mRotation.z < 0.0f)
					setAnm_(BOSS_HANACHAN_ANM_UNK7,
					        BOSS_HANACHAN_STOP_MOTION_BLEND_ON);
				else
					setAnm_(BOSS_HANACHAN_ANM_UNK10,
					        BOSS_HANACHAN_STOP_MOTION_BLEND_ON);
				break;
			}
		}
	} else if (anm == BOSS_HANACHAN_NERVE_ANM_UNK3) {
		if (unk10C > 0) {
			--unk10C;
			if (unk10C == 0)
				setAnm_(BOSS_HANACHAN_ANM_UNK6,
				        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
		} else if (unkF4 == BOSS_HANACHAN_ANM_UNK6) {
			if (isCurBckAlreadyEnd_())
				setAnm_(BOSS_HANACHAN_ANM_UNK4,
				        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
		}
	} else if (anm == BOSS_HANACHAN_NERVE_ANM_UNK4) {
		if (unk10C > 0)
			--unk10C;
		if (unk10C == 0 && unkF4 != BOSS_HANACHAN_ANM_UNK14)
			setAnm_(BOSS_HANACHAN_ANM_UNK14,
			        BOSS_HANACHAN_STOP_MOTION_BLEND_ON);
	} else if (anm == BOSS_HANACHAN_NERVE_ANM_UNK5) {
		if (unk10C > 0)
			--unk10C;
		if (unk10C == 0)
			setAnm_(BOSS_HANACHAN_ANM_UNK15,
			        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
	}
}

bool TBossHanachanPartsBody::setAnm_(
    EnumBossHanachanAnmKind kind, EnumBossHanachanStopMotionBlendOnOff blend)
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	static const int sBodyBckIndex[] = {
		19, 15, 10, 13, 0, 12, 9, 2, 3, 4, 5, 6, 7, 8, 16, 1, 17, 18,
	};

	bool changed = false;
	if (unkF4 != kind) {
		unkF8   = unkF4;
		unkF4   = kind;
		if (sBodyBckIndex[kind] != getMActor()->getCurAnmIdx(ANM_TYPE_BCK)) {
			int bck = sBodyBckIndex[kind];
			// The weak body part plays different down animations
			if (unk114 == unkFC->unk174) {
				switch (kind) {
				case BOSS_HANACHAN_ANM_UNK2:
					bck = 11;
					break;
				case BOSS_HANACHAN_ANM_UNK3:
					bck = 14;
					break;
				}
			}
			getMActor()->setBckFromIndex(bck);
			changed = true;
			if (blend == BOSS_HANACHAN_STOP_MOTION_BLEND_ON)
				unk110->startMotionBlend();
			else
				unk110->stopMotionBlend();
			setCurAnmSound();
		}

		if (kind == BOSS_HANACHAN_ANM_UNK15) {
			getMActor()->setBrkFromIndex(0);
			getMActor()->getFrameCtrl(ANM_TYPE_BRK)->setAttribute(
			    J3DFrameCtrl::ATTR_ONCE);
			getMActor()->getModel()->unlock();
		}
	}
	return changed;
}

bool TBossHanachanPartsHead::setAnm_(
    EnumBossHanachanAnmKind kind, EnumBossHanachanStopMotionBlendOnOff blend)
{
	static const int sHeadBckIndex[] = {
		36, 32, 29, 31, 20, 30, 28, 22, 23, 24, 25, 26, 27, 28, 33, 21, 34, 35,
	};
	static const int sHeadBtpIndex[] = {
		0, 0, 1, 2, 1, 2, 2, 0, 0, 0, 0, 0, 0, 2, 0, 0, 1, 1,
	};
	static const int sHeadBtkIndex[] = {
		0, 0, 0, 1, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	};

	bool changed = false;
	if (unkF4 != kind) {
		unkF8   = unkF4;
		unkF4   = kind;
		if (sHeadBckIndex[kind] != getMActor()->getCurAnmIdx(ANM_TYPE_BCK)) {
			getMActor()->setBckFromIndex(sHeadBckIndex[kind]);
			changed = true;
			if (blend == BOSS_HANACHAN_STOP_MOTION_BLEND_ON)
				unk110->startMotionBlend();
			else
				unk110->stopMotionBlend();
			setCurAnmSound();
		}

		if (sHeadBtpIndex[kind] != getMActor()->getCurAnmIdx(ANM_TYPE_BTP))
			getMActor()->setBtpFromIndex(sHeadBtpIndex[kind]);

		if (sHeadBtkIndex[kind] != getMActor()->getCurAnmIdx(ANM_TYPE_BTK))
			getMActor()->setBtkFromIndex(sHeadBtkIndex[kind]);

		if (kind == BOSS_HANACHAN_ANM_UNK15) {
			getMActor()->setBrkFromIndex(1);
			getMActor()->getFrameCtrl(ANM_TYPE_BRK)->setAttribute(
			    J3DFrameCtrl::ATTR_ONCE);
		}
	}
	return changed;
}

BOOL TBossHanachanPartsBody::receiveMessage(THitActor* sender, u32 message)
{
#ifdef SMS_PORT
	if ((message == HIT_MESSAGE_HIP_DROP || message == HIT_MESSAGE_TRAMPLE) && getenv("SMS_PORT_BOSSTRACE"))
		OSReport("[boss] wiggler segment %d got %s (weak %d, anm %d, reacts %d)\n", unk114,
		         message == HIT_MESSAGE_HIP_DROP ? "a hip drop" : "a trample", unkFC->unk174, unkF4,
		         (int)isReactToTrampleOrHipDrop_());
#endif
	if (gpMarDirector->isThing())
		return false;

	bool result = false;
	if (isReactToTrampleOrHipDrop_()) {
		switch (message) {
		case HIT_MESSAGE_TRAMPLE:
			if (unkF4 == BOSS_HANACHAN_ANM_UNK5)
				restartBck_();
			else
				setAnm_(BOSS_HANACHAN_ANM_UNK5,
				        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
			result = true;
			break;
		case HIT_MESSAGE_HIP_DROP: {
			bool isWeak = unk114 == unkFC->unk174 ? true : false;
			int curAnm = unkF4;
			switch (curAnm) {
			case BOSS_HANACHAN_ANM_UNK2:
			case BOSS_HANACHAN_ANM_UNK3:
			case BOSS_HANACHAN_ANM_UNK5:
			case BOSS_HANACHAN_ANM_UNK13:
			case BOSS_HANACHAN_ANM_UNK16:
			case BOSS_HANACHAN_ANM_UNK17:
				if (isWeak) {
					setAnm_(BOSS_HANACHAN_ANM_UNK6,
					        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
					unkFC->execDamage();
				} else if (curAnm == BOSS_HANACHAN_ANM_UNK13) {
					restartBck_();
				} else {
					setAnm_(BOSS_HANACHAN_ANM_UNK13,
					        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
				}
				result = true;
				break;
			}
			break;
		}
		}
	}
	return result;
}

BOOL TBossHanachanPartsHead::receiveMessage(THitActor* sender, u32 message)
{
	if (gpMarDirector->isThing())
		return false;

	bool result = false;
	if (isReactToTrampleOrHipDrop_()) {
		switch (message) {
		case HIT_MESSAGE_TRAMPLE:
			if (unkF4 == BOSS_HANACHAN_ANM_UNK5)
				restartBck_();
			else
				setAnm_(BOSS_HANACHAN_ANM_UNK5,
				        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
			result = true;
			break;
		case HIT_MESSAGE_HIP_DROP:
			setAnm_(BOSS_HANACHAN_ANM_UNK6,
			        BOSS_HANACHAN_STOP_MOTION_BLEND_OFF);
			unk100->onWaterHitCounter();
			result = true;
			break;
		}
	}
	return result;
}
