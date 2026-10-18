#include <Enemy/BossHanachan.hpp>
#include <Enemy/BossHanachanChangeSaveParams.hpp>
#include <Enemy/BossHanachanParts.hpp>
#include <Enemy/BossHanachanSub.hpp>
#include <Camera/CameraShake.hpp>
#include <Camera/SunMgr.hpp>
#include <Camera/cameralib.hpp>
#include <M3DUtil/MActor.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/RumbleMgr.hpp>
#include <MSound/MSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <MSound/SoundEffects.hpp>
#include <Player/MarioAccess.hpp>
#include <Strategic/Spine.hpp>
#include <System/MarDirector.hpp>
#include <System/Particles.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <stdlib.h>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

static const f32 sEmitSandFrameFoot[2] = { 14.0f, 34.0f };
static const f32 sSnortStepFrames[3]   = { 21.0f, 36.0f, 55.0f };

void TBossHanachan::staticLoadParticle()
{
	SMS_LoadParticle("ms_boha_sandsmo.jpa", 0x76);
	SMS_LoadParticle("ms_boha_sand.jpa", 0x77);
	SMS_LoadParticle("ms_boha_jouki_r_a.jpa", 0x78);
	SMS_LoadParticle("ms_boha_jouki_r_b.jpa", 0x79);
	SMS_LoadParticle("ms_boha_jouki_l_a.jpa", 0x7A);
	SMS_LoadParticle("ms_boha_jouki_l_b.jpa", 0x7B);
	SMS_LoadParticle("ms_boha_hamon_a.jpa", 0x7C);
	SMS_LoadParticle("ms_boha_hamon_b.jpa", 0x7D);
	SMS_LoadParticle("ms_boha_crash_a.jpa", 0x7E);
	SMS_LoadParticle("ms_boha_jouki2_r_a.jpa", 0x169);
	SMS_LoadParticle("ms_boha_jouki2_r_b.jpa", 0x16A);
	SMS_LoadParticle("ms_boha_jouki2_l_a.jpa", 0x16B);
	SMS_LoadParticle("ms_boha_jouki2_l_b.jpa", 0x16C);
	SMS_LoadParticle("ms_boha_sandsmo_sl.jpa", 0x16D);
	SMS_LoadParticle("ms_boha_sand_sl.jpa", 0x16E);
	SMS_LoadParticle("ms_boha_kizetsu.jpa", 0x16F);
}

static inline s16 getHitCnt(TWaterHitActor* a) { return a->mHitCounter; }

void TBossHanachan::emitParticle_()
{
	const TNerveBase<TLiveActor>* nerve = mSpine->getLatestNerve();
	if (nerve == &TNerveBossHanachanDead::theNerve())
		return;

	f32 seaLevel = gpSunMgr->unk20;

	if (nerve != &TNerveBossHanachanSnort::theNerve()
	    && getHitCnt(unk170->unk100) > 0) {
		gpMarioParticleManager->emitAndBindToMtxPtr(0x169, unk170->unk118, 1,
		                                            unk170);
		gpMarioParticleManager->emitAndBindToMtxPtr(0x16B, unk170->unk114, 1,
		                                            unk170);
		gpMarioParticleManager->emitAndBindToMtxPtr(0x16A, unk170->unk118, 1,
		                                            unk170);
		gpMarioParticleManager->emitAndBindToMtxPtr(0x16C, unk170->unk114, 1,
		                                            unk170);
	}

	if (nerve == &TNerveBossHanachanTumble::theNerve()
	    || (nerve == &TNerveBossHanachanDamage::theNerve()
	        && mMarchSpeed > 0.001f)) {
		for (int i = 0; i < 8; ++i) {
			JGeometry::TVec3<f32> pos = unk178->unk4[i].unkC;
			if (pos.y > seaLevel)
				gpMarioParticleManager->emitAndBindToPosPtr(
				    0x16E, &unk150[i]->unk154, 1, unk150[i]);
		}
		for (int i = 0; i < 8; ++i) {
			JGeometry::TVec3<f32> pos = unk178->unk4[i].unkC;
			if (pos.y > seaLevel)
				gpMarioParticleManager->emitAndBindToPosPtr(
				    0x16D, &unk150[i]->unk154, 1, unk150[i]);
		}
	}

	if (nerve == &TNerveBossHanachanGraphWander::theNerve()) {
		for (int i = 0; i < 8; ++i) {
			J3DFrameCtrl* ctrl
			    = unk150[i]->getMActor()->getFrameCtrl(ANM_TYPE_BCK);
			for (int j = 0; j < 2; ++j) {
				if (ctrl->checkPass(sEmitSandFrameFoot[j])) {
					MtxPtr footMtx = unk150[i]->unk118[j]->unk6C;
					if ((rand() * (1.0f / 32768.0f))
					    < unk1C0->mSLParticleProbability.get()) {
						if (footMtx[1][3] < seaLevel) {
							MtxPtr legMtx = unk150[i]->unk14C[j];
							JGeometry::TVec3<f32> pos;
							pos.set(legMtx[0][3], seaLevel, legMtx[2][3]);
							gpMarioParticleManager->emit(0x7C, &pos, 0,
							                             nullptr);
							gpMarioParticleManager->emit(0x7D, &pos, 0,
							                             nullptr);
						} else {
							JGeometry::TVec3<f32> pos;
							pos.set(footMtx[0][3], footMtx[1][3],
							        footMtx[2][3]);
							gpMarioParticleManager->emit(0x77, &pos, 0,
							                             nullptr);
							gpMarioParticleManager->emit(0x76, &pos, 0,
							                             nullptr);
						}
					}
				}
			}
		}
	} else if (nerve == &TNerveBossHanachanSnort::theNerve()) {
		if (unk170->getMActor()->getFrameCtrl(ANM_TYPE_BCK)->checkPass(
		        134.0f)) {
			gpMarioParticleManager->emitAndBindToMtxPtr(0x78, unk170->unk118,
			                                            0, nullptr);
			gpMarioParticleManager->emitAndBindToMtxPtr(0x7A, unk170->unk114,
			                                            0, nullptr);
			gpMarioParticleManager->emitAndBindToMtxPtr(0x79, unk170->unk118,
			                                            0, nullptr);
			gpMarioParticleManager->emitAndBindToMtxPtr(0x7B, unk170->unk114,
			                                            0, nullptr);
		}
	} else if (nerve == &TNerveBossHanachanDown::theNerve()) {
		gpMarioParticleManager->emitAndBindToMtxPtr(0x16F, unk170->unk108, 1,
		                                            unk170);
	}
}

void TBossHanachan::emitOneTimeSandPillar_(TBossHanachanPartsBody* body)
{
    // FAKEMATCH(frame): 8 bytes; real construct unknown
    volatile u32 pad[2]; (void)pad;
	onLiveFlag(LIVE_FLAG_UNK10000);
	MtxPtr mtx = body->unk108;
	unk1A0.set(mtx[0][3], mPosition.y, mtx[2][3]);

	const TLiveActor* sand = body->getSandActor_();
	if (sand != nullptr) {
		unk1A0.x = 0.5f * (unk1A0.x + sand->mPosition.x);
		unk1A0.z = 0.5f * (unk1A0.z + sand->mPosition.z);
		unk1A0.y = sand->mPosition.y;
	}

	gpMarioParticleManager->emit(0x7E, &unk1A0, 0, nullptr);

	MtxPtr baseMtx = unk19C->getModel()->getBaseTRMtx();
	baseMtx[0][3]  = unk1A0.x;
	baseMtx[1][3]  = unk1A0.y;
	baseMtx[2][3]  = unk1A0.z;
	unk19C->setBckFromIndex(0x25);
	unk19C->setBtkFromIndex(2);
	unk19C->setBrkFromIndex(2);

	gpCameraShake->startShake((EnumCamShakeMode)8, 1.0f);
	SMSGetMSound()->startSoundActor(MSD_SE_BS_HANA_TUMBLE, &unk1A0, 0, nullptr,
	                                0, 4);
}

void TBossHanachan::emitCamShake_()
{
	if (gpMarDirector->mState == TMarDirector::STATE_UNK1)
		return;

	const TNerveBase<TLiveActor>* nerve = mSpine->getLatestNerve();
	J3DFrameCtrl* ctrl = unk150[0]->getMActor()->getFrameCtrl(ANM_TYPE_BCK);
	bool marioOnGround = SMS_IsMarioTouchGround4cm();

	if (nerve == &TNerveBossHanachanGraphWander::theNerve()) {
		f32 dist = MsSqrtf(mDistToMarioSquared);
		f32 power;
		if (dist <= unk1BC->mSLCamShakeMaxDist.get()) {
			power = 1.0f;
		} else if (dist >= unk1BC->mSLCamShakeZeroDist.get()) {
			power = 0.0f;
		} else {
			power = MsClamp(CLBCalcRatio(unk1BC->mSLCamShakeZeroDist.get(),
			                             unk1BC->mSLCamShakeMaxDist.get(),
			                             dist),
			                0.0f, 1.0f);
		}

		for (int i = 0; i < 2; ++i) {
			if (ctrl->checkPass(sEmitSandFrameFoot[i])) {
				gpCameraShake->startShake((EnumCamShakeMode)0xB, power);
				if (marioOnGround)
					for (int j = 0; j < 8; ++j)
						SMSRumbleMgr->start(8, &unk150[j]->unk154);
			}
		}
	} else if (nerve == &TNerveBossHanachanSnort::theNerve()) {
		for (int i = 0; i < 3; ++i) {
			if (ctrl->checkPass(sSnortStepFrames[i])) {
				gpCameraShake->startShake((EnumCamShakeMode)0xA, 1.0f);
				if (marioOnGround)
					SMSRumbleMgr->start(8, (f32*)nullptr);
			}
		}
	} else if (nerve == &TNerveBossHanachanGetUp::theNerve()) {
		switch (unk150[0]->unkF4) {
		case BOSS_HANACHAN_ANM_UNK9:
		case BOSS_HANACHAN_ANM_UNK12:
			if (ctrl->checkPass(9.0f)) {
				gpCameraShake->startShake((EnumCamShakeMode)0xC, 1.0f);
				if (marioOnGround)
					SMSRumbleMgr->start(8, (f32*)nullptr);
				if (checkLiveFlag(LIVE_FLAG_UNK20000))
					MSBgm::stopTrackBGM(1, 30);
			}
			break;
		}
	}
}
