#include <Camera/SunMgr.hpp>
#ifdef SMS_PORT_AURORA
extern "C" int port_mp_plaza_exit_blocked(void); // port/src/port_mp_modes.cpp: multiplayer, the plaza closed
#include <dolphin/os.h>
#include <math.h>
#include <Camera/CameraKindParam.hpp>
#endif
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <System/PositionHolder.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <Camera/SunModel.hpp>
#include <Camera/Camera.hpp>
#include <Player/MarioAccess.hpp>
#include <MSound/MSound.hpp>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>

static const char* dummyMactorStringValue1 = "\0\0\0\0\0\0\0\0\0\0\0";
static const char* SMS_NO_MEMORY_MESSAGE   = "メモリが足りません\n";

const char* cSunWarpPointName = "太陽ワープポイント";

TSunMgr* gpSunMgr;

TSunMgr::TSunMgr(const char* name)
    : JDrama::TViewObj(name)
    , unk14(0)
    , unk15(0)
    , unk20(0.0f)
    , unk24(0.0f, 0.0f, 0.0f)
{
	gpSunMgr = this;
}

void TSunMgr::load(JSUMemoryInputStream& stream)
{
	JDrama::TViewObj::load(stream);

	u32 local_24;
	stream >> local_24;
	u32 local_20;
	stream >> local_20;
	u32 local_1c;
	stream >> local_1c;
	u32 local_18;
	stream >> local_18;
	stream >> unk20;

	u32 col1 = local_24 << 8 | local_20;
	u32 col2 = local_1c << 8 | local_18;
	unk18.set(col1);
	unk1C.set(col2);

	TSunModel* sun
	    = static_cast<TSunModel*>(JDrama::TNameRefGen::search("太陽モデル"));
	if (sun != nullptr) {
		unk14 = 1;
	} else {
		sun = static_cast<TSunModel*>(
		    JDrama::TNameRefGen::search("夕日モデル"));
		if (sun != nullptr) {
			unk14 = 1;
			unk15 |= 0x2;
		}
	}

#ifdef SMS_PORT_AURORA
	// port: what the sun warp (Noki Bay entrance) sees when the stage loads; goes into bug reports
	OSReport("[sunwarp] load: sun model %d map %d noki_available %d shines %d\n", unk14,
	         gpMarDirector->getCurrentMap(), TFlagManager::getInstance()->getBool(MSF_NOKI_AVAILABLE) ? 1 : 0,
	         (int)TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT));
#endif
	if (unk14 != 0 && gpMarDirector->getCurrentMap() == 1
	    && TFlagManager::getInstance()->getBool(MSF_NOKI_AVAILABLE)) {
		unk15 |= 0x1;
		TStagePositionInfo* sunWarpPoint
		    = (TStagePositionInfo*)gpPositionHolder->searchF(
		        JDrama::TNameRef::calcKeyCode(cSunWarpPointName),
		        cSunWarpPointName);
		unk24 = sunWarpPoint->unkC;
	}
}

void TSunMgr::perform(u32 cue, JDrama::TGraphics* graphics)
{
    // FAKEMATCH(frame): 40 bytes; real construct unknown
    volatile u32 pad[10]; (void)pad;
#ifdef SMS_PORT_AURORA
	// port: once a second near the plaza's sun warp point (2090, -3000 in dolpic tables.bin), log every
	// condition of the Noki Bay warp below; the log goes into F9 bug reports.
	if ((cue & CUE_MOVE) && gpCamera != nullptr && gpSunModel != nullptr && gpMarDirector->getCurrentMap() == 1) {
		static u32 sTick;
		f32 tdx = SMS_GetMarioPos().x - 2090.0f, tdz = SMS_GetMarioPos().z + 3000.0f;
		f32 td2 = tdx * tdx + tdz * tdz;
		if (td2 < 1440000.0f && (graphics->unk0 & 2) && (sTick++ % 30) == 0) { // the warp runs on the last tick only
			const JGeometry::TVec2<f32>& sp = gpSunModel->portSunScreenPos();
			OSReport("[sunwarp] enabled %d (noki_available %d, shines %d, sun model %d) dist %.0f (need <400) gfx %d Lcam %d moving %d sun %.2f,%.2f (need |x|,|y|<=0.30)\n",
			         (unk15 & 1) ? 1 : 0, TFlagManager::getInstance()->getBool(MSF_NOKI_AVAILABLE) ? 1 : 0,
			         (int)TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT), unk14, sqrtf(td2),
			         (graphics->unk0 & 2) ? 1 : 0, gpCamera->isLButtonCamera() ? 1 : 0,
			         gpCamera->isNowInbetween() ? 1 : 0, sp.x, sp.y);
			// camera vs sun angles: can the first-person view reach the sun at all?
			const JGeometry::TVec3<f32>& cp = gpCamera->getUnk124();
			f32 sx = gpSunModel->mPosition.x - cp.x, sy = gpSunModel->mPosition.y - cp.y,
			    sz = gpSunModel->mPosition.z - cp.z;
			f32 lk = gpCamera->unk25C.y;
			lk = lk > 1.0f ? 1.0f : (lk < -1.0f ? -1.0f : lk);
			OSReport("[sunwarp] cam: look elev %.1f deg, pitch ratio %.3f, polar X %d (%.1f deg), X limits %d..%d (%.1f..%.1f deg), fovy %.1f, aspect %.3f; sun elev %.1f deg, sun pos %.0f %.0f %.0f\n",
			         asinf(lk) * 57.2958f, gpCamera->mCurrentTarget.unk28, (int)gpCamera->unk256,
			         gpCamera->unk256 * (360.0f / 65536.0f), (int)gpCamera->mCurrentParams->mXAngleMin,
			         (int)gpCamera->mCurrentParams->mXAngleMax, gpCamera->mCurrentParams->mXAngleMin * (360.0f / 65536.0f),
			         gpCamera->mCurrentParams->mXAngleMax * (360.0f / 65536.0f), gpCamera->mFovy, gpCamera->mAspect,
			         atan2f(sy, sqrtf(sx * sx + sz * sz)) * 57.2958f, gpSunModel->mPosition.x, gpSunModel->mPosition.y,
			         gpSunModel->mPosition.z);
		}
	}
#endif
	if (!(unk15 & 1))
		return;
	if (!(cue & CUE_MOVE))
		return;
	if (!(graphics->unk0 & 2))
		return;

	if (!gpCamera->isThing2())
		return;

	// Transition to noki bay
	f32 dx = SMS_GetMarioPos().x - unk24.x;
	f32 dz = SMS_GetMarioPos().z - unk24.z;
	if (dx * dx + dz * dz < 160000.0f && gpSunModel->isInBounds(0.3f)
#ifdef SMS_PORT_AURORA
	    && !port_mp_plaza_exit_blocked()
#endif
	) {
		gpMarDirector->setNextStage(9, nullptr);
		MSound* sound = SMSGetMSound();
		if (sound->unk7C != nullptr) {
			sound->unk7C->setVolume(0.0f, 100, 0);
			sound->unk7C->setPitch(1.3f, 100, 0);
		}
	}
}

int TSunMgr::getAddColor() const
{
	int result = 0;
	if (unk14)
		result = gpSunModel->getUnkAC();
	return result;
}

void TSunMgr::drawSyncCallback(u16)
{
	if (unk14)
		gpSunModel->getZBufValue();
}
