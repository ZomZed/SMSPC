// port_mp_puppet.cpp - multiplayer remote players ("puppets"), w15.
//
// 1. Capture: port_mp_capture() reads the local TMario (gpMarioOriginal) into a
//    PortMpPuppetState. Read-only; the only hook is port_mp_logic_frame() in
//    TMarDirector::direct (once per entered logic frame).
// 2. Puppets: per remote player, an extra Mario built like Shadow Mario's
//    (TEnemyMario::initModel) and TMario::initModel: its own J3DModelData of
//    /mario/bmd/ma_mdl1.bmd (so hand / shirt shape flags and eye patterns are its
//    own), hand and cap models, an M3UModelMario that shares the local Mario's BCK
//    table, and the FLUDD body + nozzle MActors. Posed from snapshots buffered
//    ~100 ms and interpolated; snaps on teleports and stage changes. Drawn from a
//    view object pushed into the stage's entry list after "コンダクター" (the
//    character draw buffers, lit through the light manager like MActors), with a
//    circle shadow. Their FLUDD water is real water of the local water manager,
//    visual only (port_mp_water.cpp).
// 3. Puppets never touch game state: no collision, no hit actor, not in any
//    manager, no sound, no rumble, no pollution (their water particles are flagged
//    remote and only drawn). Models live in the
//    stage heap (allocated after setup) and die with it.
// 4. Ghost harness: SMS_PORT_MP_GHOST=<file> records the local snapshots,
//    SMS_PORT_MP_GHOST_PLAY=<file> plays them back as puppets in a later run.
#include "port_mp_puppet.h"
#include "port_mp.h"
extern "C" uint32_t port_mp_tag_rgb(int slot); // port_mp_palette.cpp (name-tag colours)
extern "C" void port_mp_dance_pose_slot(int slot, void* model, unsigned bck); // port_mp_dance.cpp

#include <Player/Mario.hpp>
#include <Player/MarioCap.hpp>
#include <Player/WaterGun.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/NozzleBase.hpp>
#include <Player/Yoshi.hpp>
#include <Player/MarioAnimeData.hpp>
#include <M3DUtil/M3UModelMario.hpp>
#include <M3DUtil/M3UJoint.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/MActorData.hpp>
#include <MarioUtil/ModelUtil.hpp>
#include <MarioUtil/DrawUtil.hpp>
#include <MarioUtil/PacketUtil.hpp>
#include <MarioUtil/TexUtil.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <MarioUtil/MtxUtil.hpp>
#include <MarioUtil/LightUtil.hpp>
#include <MarioUtil/ShadowUtil.hpp>
#include <Camera/Camera.hpp>
#include <Camera/CubeManagerBase.hpp>
#include <Map/Map.hpp>
#include <Map/MapData.hpp>
#include <NPC/NpcBase.hpp>
#include <Enemy/Conductor.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/MarDirector.hpp>
#include <System/Application.hpp>
#include <System/PerformList.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DMaterialAnm.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DPacket.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JDrama/JDRViewObj.hpp>
#include <JSystem/JMath.hpp>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// ---------------------------------------------------------------------------
// small helpers

namespace {

const char* envStr(const char* name)
{
	const char* v = getenv(name);
	return (v != nullptr && v[0] != '\0') ? v : nullptr;
}

template <typename T> T* heapNew(JKRHeap* heap)
{
	void* p = JKRHeap::alloc(sizeof(T), 0x10, heap);
	return p != nullptr ? new (p) T() : nullptr;
}

template <typename T> T* heapNewArray(JKRHeap* heap, int n)
{
	T* p = (T*)JKRHeap::alloc(sizeof(T) * n, 0x10, heap);
	if (p != nullptr)
		for (int i = 0; i < n; ++i)
			new (&p[i]) T();
	return p;
}

f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }
s16 lerpAngle(s16 a, s16 b, f32 t) { return (s16)(a + (s16)(b - a) * t); }

struct Quat {
	f32 x, y, z, w;
};

void mtxToQuat(const f32 m[3][4], Quat& q)
{
	const f32 tr = m[0][0] + m[1][1] + m[2][2];
	if (tr > 0.0f) {
		f32 s = std::sqrt(tr + 1.0f) * 2.0f;
		q.w   = 0.25f * s;
		q.x   = (m[2][1] - m[1][2]) / s;
		q.y   = (m[0][2] - m[2][0]) / s;
		q.z   = (m[1][0] - m[0][1]) / s;
	} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
		f32 s = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
		q.w   = (m[2][1] - m[1][2]) / s;
		q.x   = 0.25f * s;
		q.y   = (m[0][1] + m[1][0]) / s;
		q.z   = (m[0][2] + m[2][0]) / s;
	} else if (m[1][1] > m[2][2]) {
		f32 s = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
		q.w   = (m[0][2] - m[2][0]) / s;
		q.x   = (m[0][1] + m[1][0]) / s;
		q.y   = 0.25f * s;
		q.z   = (m[1][2] + m[2][1]) / s;
	} else {
		f32 s = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
		q.w   = (m[1][0] - m[0][1]) / s;
		q.x   = (m[0][2] + m[2][0]) / s;
		q.y   = (m[1][2] + m[2][1]) / s;
		q.z   = 0.25f * s;
	}
}

// Column scales of the 3x3 part (the base matrix is a pure TR, but keep it general).
void mtxScale(const f32 m[3][4], f32 s[3])
{
	for (int c = 0; c < 3; ++c)
		s[c] = std::sqrt(m[0][c] * m[0][c] + m[1][c] * m[1][c] + m[2][c] * m[2][c]);
}

void lerpMtx(const f32 a[3][4], const f32 b[3][4], f32 t, f32 out[3][4])
{
	if (t <= 0.0f) {
		memcpy(out, a, sizeof(f32) * 12);
		return;
	}
	if (t >= 1.0f) {
		memcpy(out, b, sizeof(f32) * 12);
		return;
	}
	f32 sa[3], sb[3];
	mtxScale(a, sa);
	mtxScale(b, sb);
	f32 na[3][4], nb[3][4];
	for (int r = 0; r < 3; ++r)
		for (int c = 0; c < 3; ++c) {
			na[r][c] = sa[c] > 1e-6f ? a[r][c] / sa[c] : a[r][c];
			nb[r][c] = sb[c] > 1e-6f ? b[r][c] / sb[c] : b[r][c];
		}
	Quat qa, qb;
	mtxToQuat(na, qa);
	mtxToQuat(nb, qb);
	f32 d = qa.x * qb.x + qa.y * qb.y + qa.z * qb.z + qa.w * qb.w;
	if (d < 0.0f) {
		qb.x = -qb.x, qb.y = -qb.y, qb.z = -qb.z, qb.w = -qb.w;
		d    = -d;
	}
	Quat q;
	if (d > 0.9995f) { // nlerp
		q.x = lerpf(qa.x, qb.x, t), q.y = lerpf(qa.y, qb.y, t);
		q.z = lerpf(qa.z, qb.z, t), q.w = lerpf(qa.w, qb.w, t);
	} else {
		f32 th = std::acos(d), s = std::sin(th);
		f32 wa = std::sin((1.0f - t) * th) / s, wb = std::sin(t * th) / s;
		q.x = qa.x * wa + qb.x * wb, q.y = qa.y * wa + qb.y * wb;
		q.z = qa.z * wa + qb.z * wb, q.w = qa.w * wa + qb.w * wb;
	}
	f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
	if (n > 1e-6f)
		q.x /= n, q.y /= n, q.z /= n, q.w /= n;
	const f32 xx = q.x * q.x, yy = q.y * q.y, zz = q.z * q.z;
	const f32 xy = q.x * q.y, xz = q.x * q.z, yz = q.y * q.z;
	const f32 wx = q.w * q.x, wy = q.w * q.y, wz = q.w * q.z;
	f32 rot[3][3] = { { 1 - 2 * (yy + zz), 2 * (xy - wz), 2 * (xz + wy) },
		              { 2 * (xy + wz), 1 - 2 * (xx + zz), 2 * (yz - wx) },
		              { 2 * (xz - wy), 2 * (yz + wx), 1 - 2 * (xx + yy) } };
	for (int c = 0; c < 3; ++c) {
		f32 sc = lerpf(sa[c], sb[c], t);
		for (int r = 0; r < 3; ++r)
			out[r][c] = rot[r][c] * sc;
	}
	for (int r = 0; r < 3; ++r)
		out[r][3] = lerpf(a[r][3], b[r][3], t);
}

// A frame of a looping/once animation: blend only when both sides play the same
// clip and the step is small (a loop wrap or a restart is taken as is).
f32 lerpFrame(f32 a, f32 b, f32 t)
{
	if (std::fabs(b - a) > 8.0f)
		return t < 0.5f ? a : b;
	return lerpf(a, b, t);
}

} // namespace

// ---------------------------------------------------------------------------
// 1. capture

namespace {

u8 sStageSeq;      // +1 per stage setup (preEntry)
u8 sWarpSeq;       // +1 per detected teleport of the local Mario
u32 sLogicFrame;   // logic frames since boot (port_mp_logic_frame calls)
bool sHaveLast;
PortMpPuppetState sLast;
JGeometry::TVec3<f32> sLastPos;
u32 sLastStatus;

// The rotation MarioHeadCtrl concatenates onto jnt_head, as RPH(a,b,c) * RPH(0,0,d)
// (degrees as s16 angles). Read-only replica of the callback's choice.
void captureHeadRot(TMario* m, s16 out[4])
{
	out[0] = out[1] = out[2] = out[3] = 0;
	if (m->mStatus == MARIO_STATUS_READ_BILLBOARD) {
		if (gpMarDirector == nullptr || gpMarDirector->unkA0 == nullptr)
			return;
		JGeometry::TVec3<f32> f   = gpMarDirector->unkA0->getFocalPoint();
		JGeometry::TVec3<f32> pos = m->mPosition;
		pos.y += 112.0f;
		JGeometry::TVec3<f32> o = f - pos;
		f32 mult                = std::sqrt(o.x * o.x + o.z * o.z);
		out[2]                  = -matan(mult, o.y);
	} else if (m->mStatus == MARIO_STATUS_ROCKET && m->mWaterGun != nullptr
	           && m->mWaterGun->isEmitting() == true) {
		out[2] = m->mUpperBodyParams.mHoverHeadAngle.get();
	} else if (m->mStatus == MARIO_STATUS_WAIT
	           && (m->unk370 > m->mDeParams.mFeelDeep.get() ? true : false)) {
		out[2] = m->mUpperBodyParams.mFeelDeepHeadAngle.get();
	} else if (m->mWallPlane != nullptr && m->isWallInFront()) {
		out[2] = m->mUpperBodyParams.mFrontWallHeadAngle.get();
	} else {
		f32 anmSpeed = m->fabricatedIsPumping() ? m->mBodyAngleParamsWaterGun.mHeadRot.get()
		                                        : m->mBodyAngleParamsFree.mHeadRot.get();
		out[1] = (s16)(-m->unk100 * anmSpeed);
		if (m->mWaterGun != nullptr) {
			s16 gunAngle = m->mWaterGun->getCurrentNozzle()->getGunAngle() / 2;
			if (gunAngle < 0)
				out[3] = gunAngle;
		}
	}
}

// The rotation MarioWaistCtrl concatenates onto chn_chest, as RPH(a,b,c).
void captureWaistRot(TMario* m, s16 out[3])
{
	out[0] = out[1] = out[2] = 0;
	if (gpCamera != nullptr && gpCamera->isLButtonCamera() == true && m->canBendBody() != 0
	    && gpCamera->mCurrentTarget.mPitch > 0) {
		out[0] = -m->unk100;
		out[2] = gpCamera->mCurrentTarget.mPitch;
	} else if (m->checkStatusType(MARIO_FLAG_HAS_FLUDD) && !m->onYoshi()) {
		if (m->mWaterGun != nullptr) {
			s16 gunAngle = m->mWaterGun->getCurrentNozzle()->getGunAngle();
			if (gunAngle > 0)
				out[2] = gunAngle;
		}
	} else if (m->mAnimationId == TMario::ANIM_RUN1 || m->mAnimationId == TMario::ANIM_RUN2
	           || m->mAnimationId == TMario::ANIM_RIDE_SHELL
	                  && !m->checkFlag(MARIO_FLAG_FLUDD_EMITTING)) {
		out[0] = (s16)m->mWaistRoll;
		out[2] = (s16)m->mWaistPitch;
	}
}

int handMode(TMario* m)
{
	J3DModelData* d = m->mModel->getModel()->getModelData();
	if (m->mHandModels[0][0] == nullptr || d->getShapeNum() <= 6)
		return 0;
	if (!d->getShapeNodePointer(5)->checkFlag(J3DShpFlag_Visible))
		return 0; // body fists shown
	J3DModelData* h2 = m->mHandModels[0][0]->getModelData();
	if (h2->getShapeNum() > 0 && !h2->getShapeNodePointer(0)->checkFlag(J3DShpFlag_Visible))
		return 1;
	return 2;
}

} // namespace

extern "C" int port_mp_capture(PortMpPuppetState* out)
{
	TMario* m     = gpMarioOriginal;
	TMarDirector* d = gpMarDirector;
	if (m == nullptr || d == nullptr || m->mModel == nullptr || !d->mSetupDone)
		return 0;
	PortMpPuppetState s;
	memset(&s, 0, sizeof(s));
	s.version  = PORT_MP_PUPPET_STATE_VERSION;
	s.size     = sizeof(PortMpPuppetState);
	s.seq      = sLogicFrame;
	s.stage    = d->mMap;
	s.scenario = d->unk7D;
	s.stageSeq = sStageSeq;
	s.warpSeq  = sWarpSeq;

	u32 fl = 0;
	if ((m->unk114 & TMario::UNK114_FLAG_VISIBLE) && !m->checkFlag(MARIO_FLAG_UNK4))
		fl |= PMS_VISIBLE;
	if (m->checkFlag(MARIO_FLAG_HAS_FLUDD) && m->mWaterGun != nullptr) {
		fl |= PMS_HAS_FLUDD;
		if (m->mWaterGun->isEmitting())
			fl |= PMS_SPRAYING;
	}
	if (m->onYoshi())
		fl |= PMS_ON_YOSHI;
	if (m->mCap != nullptr) {
		if (m->mCap->isModelActive(TMarioCap::E_CAP_MODEL_HAT))
			fl |= PMS_CAP;
		if (m->mCap->isModelActive(TMarioCap::E_CAP_MODEL_HELMET))
			fl |= PMS_HELMET;
		if (m->mCap->isModelActive(TMarioCap::E_CAP_MODEL_SUNGLASSES))
			fl |= PMS_GLASSES;
	} else {
		fl |= PMS_CAP;
	}
	if (!m->checkFlag(MARIO_FLAG_HAS_SHIRT))
		fl |= PMS_SHIRT;
	if (m->mRHand4ndModel != nullptr && m->mRHand4ndModel->getModelData()->getShapeNum() > 0
	    && !m->mRHand4ndModel->getModelData()->getShapeNodePointer(0)->checkFlag(J3DShpFlag_Visible))
		fl |= PMS_HAND4;
	if (d->mState != TMarDirector::STATE_UNK4 || d->isTalkModeNow() || d->isDemoModeNow())
		fl |= PMS_IN_DEMO;
	if (m->mHeldObject != nullptr)
		fl |= PMS_HOLDING;
	if (m->checkFlag(MARIO_FLAG_IN_ANY_WATER))
		fl |= PMS_IN_WATER;
	if (0x4B0 > d->mMoveTickCount || m->isUpperPumpingStyle())
		fl |= PMS_FX_OFF; // TMario::addCallBack's rule for the joint springs
	s.flags = fl;

	s.status     = m->mStatus;
	s.statusArg  = m->mStatusArg;
	s.marioFlags = m->mFlag;
	for (int i = 0; i < 3; ++i) {
		s.pos[i] = (&m->mPosition.x)[i];
		s.vel[i] = (&m->mVel.x)[i];
	}
	s.fwdVel         = m->mForwardVel;
	s.faceAngle[0]   = m->mFaceAngle.x;
	s.faceAngle[1]   = m->mFaceAngle.y;
	s.faceAngle[2]   = m->mFaceAngle.z;
	s.modelFaceAngle = m->mModelFaceAngle;
	memcpy(s.baseMtx, m->mModel->getModel()->getBaseTRMtx(), sizeof(s.baseMtx));

	M3UModelMario* mm = m->mModel;
	s.animId          = m->mAnimationId;
	for (int slot = 0; slot < 2; ++slot) {
		s.bck[slot][0] = mm->unk24[slot].unk4[0];
		s.bck[slot][1] = mm->unk24[slot].unk4[1];
		s.calc[slot]   = mm->unk24[slot].unk3;
		s.blend[slot]  = mm->unk20->unk18[slot].mMotionBlendRatio;
	}
	for (int i = 0; i < 3; ++i) {
		s.frame[i] = mm->getFrameCtrl(i).getFrame();
		s.rate[i]  = mm->getFrameCtrl(i).getRate();
	}
	s.btp  = mm->unk1C != nullptr ? mm->unk1C->unk0 : 0xFF;
	s.hand = (u8)handMode(m);
	captureHeadRot(m, s.headRot);
	captureWaistRot(m, s.waistRot);
	s.health = m->mHealth;

	s.fluddBck = s.nozzleBck = -1;
	if (TWaterGun* g = m->mWaterGun) {
		s.nozzle       = g->mCurrentNozzle;
		s.secondNozzle = g->mSecondNozzle;
		s.pressure     = g->mCurrentPressure;
		s.water    = g->mCurrentWater;
		if (g->mFluddModel != nullptr) {
			s.fluddBck   = (s16)g->mFluddModel->getCurAnmIdx(ANM_TYPE_BCK);
			J3DFrameCtrl* fc = g->mFluddModel->getFrameCtrl(ANM_TYPE_BCK);
			s.fluddFrame = fc != nullptr ? fc->getFrame() : 0.0f;
		}
		if (MActor* nz = g->getCurrentNozzle()->getMActor()) {
			s.nozzleBck      = (s16)nz->getCurAnmIdx(ANM_TYPE_BCK);
			J3DFrameCtrl* fc = nz->getFrameCtrl(ANM_TYPE_BCK);
			s.nozzleFrame    = fc != nullptr ? fc->getFrame() : 0.0f;
		}
		for (int i = 0; i < 3; ++i)
			s.emitPos[i] = (&g->mEmitPos[0].x)[i];
		s.emitDir[2] = 1.0f;
		if (!m->onYoshi() || m->mYoshi != nullptr) {
			MtxPtr em = g->getEmitMtx(0);
			if (em != nullptr) {
				s.emitDir[0] = em[0][0];
				s.emitDir[1] = em[1][0];
				s.emitDir[2] = em[2][0];
			}
		}
	}

	s.yoshiBck = -1;
	if (TYoshi* y = m->mYoshi) {
		s.yoshiState = y->mState;
		s.yoshiColor = y->mType;
		if (y->mActor != nullptr) {
			s.yoshiBck       = (s16)y->mActor->getCurAnmIdx(ANM_TYPE_BCK);
			J3DFrameCtrl* fc = y->mActor->getFrameCtrl(ANM_TYPE_BCK);
			s.yoshiFrame     = fc != nullptr ? fc->getFrame() : 0.0f;
		}
	}
	if (m->mHeldObject != nullptr)
		s.heldActorType = m->mHeldObject->getActorType();
	*out = s;
	return 1;
}

extern "C" int port_mp_last_local_state(PortMpPuppetState* out)
{
	if (!sHaveLast)
		return 0;
	*out = sLast;
	return 1;
}

// ---------------------------------------------------------------------------
// 1b. wire form: w10's 128-byte PortMpSnapshot (port_mp.h). Standard fields as w10
// defines them; the rest of the pose goes into the w15 block: `animAux`,
// `nozzleAngle`, `pad0` and `extra[44]` (layout: MULTIPLAYER.md "Puppet block").

namespace {

#pragma pack(push, 1)
struct WireExtra {
	u8 bck[4];          // body new/old, upper new/old (BCK indices < 199)
	u16 upperFrame;     // x32
	u8 eyeFrame;        // whole frames (eye patterns are short)
	u8 stageSeq;
	u8 blend[2];        // x255
	s16 quat[3];        // base matrix rotation (x,y,z; w >= 0), x32767
	s16 baseOfs[3];     // base matrix translation - pos, x4
	s8 head[4];         // headRot >> 8
	s8 waist[3];        // waistRot >> 8
	s8 fluddBck, nozzleBck, yoshiBck;
	u16 fluddFrame, nozzleFrame, yoshiFrame; // x32
	s8 emitOfs[3];      // (emitPos - pos) / 2
	u8 heldLo[2];       // held object's actor type, low 16 bits (little-endian)
	u8 heldCat;         // its category bit index + 1 (0 = nothing held, 0xFF = no category bit)
};
#pragma pack(pop)
static_assert(sizeof(WireExtra) == 44, "puppet block must fill PortMpSnapshot::extra");
static_assert(sizeof(PortMpSnapshot) == 128, "w10 wire snapshot");

u16 packFrame(f32 f)
{
	f32 v = f * 32.0f;
	return v <= 0.0f ? 0 : (v >= 65535.0f ? 65535 : (u16)(v + 0.5f));
}
s16 clampS16(f32 v) { return v <= -32768.0f ? -32768 : (v >= 32767.0f ? 32767 : (s16)v); }
s8 bckS8(s16 b) { return b < 0 || b > 127 ? (s8)-1 : (s8)b; }

} // namespace

extern "C" void port_mp_pack_wire(const PortMpPuppetState* in, PortMpSnapshot* out)
{
	memset(out, 0, sizeof(*out));
	const PortMpPuppetState& s = *in;
	out->tick     = s.seq;
	out->stage    = s.stage;
	out->scenario = s.scenario;
	u16 f         = PORT_MP_SNAP_VALID;
	if (s.flags & PMS_HOLDING)
		f |= PORT_MP_SNAP_HOLDING;
	if (s.flags & PMS_ON_YOSHI)
		f |= PORT_MP_SNAP_ON_YOSHI;
	if (s.flags & PMS_SPRAYING)
		f |= PORT_MP_SNAP_SPRAYING;
	if (s.flags & PMS_HAS_FLUDD)
		f |= PORT_MP_SNAP_HAS_FLUDD;
	if (!(s.flags & PMS_VISIBLE))
		f |= PORT_MP_SNAP_HIDDEN;
	if (s.flags & PMS_IN_WATER)
		f |= PORT_MP_SNAP_IN_WATER;
	if (s.flags & PMS_IN_DEMO)
		f |= PORT_MP_SNAP_PAUSED;
	if (s.flags & PMS_FX_OFF)
		f |= PORT_MP_SNAP_FX_OFF;
	out->flags = f;
	for (int k = 0; k < 3; ++k) {
		out->pos[k] = s.pos[k];
		out->vel[k] = s.vel[k];
	}
	out->forwardVel = s.fwdVel;
	for (int k = 0; k < 3; ++k)
		out->faceAngle[k] = s.faceAngle[k];
	out->modelFaceAngle = s.modelFaceAngle;
	out->action         = s.status;
	out->actionArg      = s.statusArg;
	out->marioFlags     = s.marioFlags;
	out->animId         = s.animId;
	out->animFrame      = s.frame[0];
	out->animRate       = s.rate[0];
	out->nozzle         = s.nozzle;
	out->secondNozzle   = s.secondNozzle;
	out->water          = (u16)(s.water < 0 ? 0 : (s.water > 0xFFFF ? 0xFFFF : s.water));
	out->sprayStrength  = s.pressure / 255.0f;
	out->health         = (u8)(s.health < 0 ? 0 : (s.health > 255 ? 255 : s.health));
	out->yoshiState     = s.yoshiState;
	out->yoshiColor     = s.yoshiColor;
	out->pad0           = s.warpSeq;
	// animAux: hand 2 bits, eye pattern 5 bits (31 = none), calc0/calc1 2 bits each
	// (3 = off), then cap, helmet, glasses, shirt, 4th hand.
	u16 aux = (u16)(s.hand & 3);
	aux |= (u16)((s.btp < 31 ? s.btp : 31) << 2);
	aux |= (u16)((s.calc[0] <= 2 ? s.calc[0] : 3) << 7);
	aux |= (u16)((s.calc[1] <= 2 ? s.calc[1] : 3) << 9);
	if (s.flags & PMS_CAP)
		aux |= 1u << 11;
	if (s.flags & PMS_HELMET)
		aux |= 1u << 12;
	if (s.flags & PMS_GLASSES)
		aux |= 1u << 13;
	if (s.flags & PMS_SHIRT)
		aux |= 1u << 14;
	if (s.flags & PMS_HAND4)
		aux |= 1u << 15;
	out->animAux = aux;
	// nozzleAngle: emit direction as yaw / pitch.
	{
		const f32 x = s.emitDir[0], y = s.emitDir[1], z = s.emitDir[2];
		out->nozzleAngle[0] = (s16)(std::atan2(x, z) * (32768.0f / 3.14159265f));
		out->nozzleAngle[1] = (s16)(std::atan2(y, std::sqrt(x * x + z * z)) * (32768.0f / 3.14159265f));
	}
	WireExtra e;
	memset(&e, 0, sizeof(e));
	for (int i = 0; i < 4; ++i) {
		u16 b    = s.bck[i >> 1][i & 1];
		e.bck[i] = (u8)(b < 255 ? b : 255);
	}
	e.upperFrame = packFrame(s.frame[1]);
	e.eyeFrame   = (u8)(s.frame[2] <= 0.0f ? 0 : (s.frame[2] >= 255.0f ? 255 : s.frame[2]));
	e.stageSeq   = s.stageSeq;
	for (int i = 0; i < 2; ++i) {
		f32 b      = s.blend[i] < 0.0f ? 0.0f : (s.blend[i] > 1.0f ? 1.0f : s.blend[i]);
		e.blend[i] = (u8)(b * 255.0f + 0.5f);
	}
	{
		Quat q;
		mtxToQuat(s.baseMtx, q);
		f32 n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
		if (n > 1e-6f)
			q.x /= n, q.y /= n, q.z /= n, q.w /= n;
		if (q.w < 0.0f)
			q.x = -q.x, q.y = -q.y, q.z = -q.z;
		e.quat[0] = clampS16(q.x * 32767.0f);
		e.quat[1] = clampS16(q.y * 32767.0f);
		e.quat[2] = clampS16(q.z * 32767.0f);
	}
	for (int k = 0; k < 3; ++k) {
		e.baseOfs[k] = clampS16((s.baseMtx[k][3] - s.pos[k]) * 4.0f);
		const f32 eo = (s.emitPos[k] - s.pos[k]) * 0.5f;
		e.emitOfs[k] = (s8)(eo <= -128.0f ? -128 : (eo >= 127.0f ? 127 : eo));
	}
	if (s.heldActorType != 0) {
		const u32 cat = s.heldActorType & 0xFFFF0000u;
		e.heldLo[0]   = (u8)s.heldActorType;
		e.heldLo[1]   = (u8)(s.heldActorType >> 8);
		e.heldCat     = 0xFF;
		for (int b = 16; b < 32; ++b)
			if (cat == (1u << b))
				e.heldCat = (u8)(b + 1);
	}
	for (int k = 0; k < 4; ++k)
		e.head[k] = (s8)(s.headRot[k] >> 8);
	for (int k = 0; k < 3; ++k)
		e.waist[k] = (s8)(s.waistRot[k] >> 8);
	e.fluddBck    = bckS8(s.fluddBck);
	e.nozzleBck   = bckS8(s.nozzleBck);
	e.yoshiBck    = bckS8(s.yoshiBck);
	e.fluddFrame  = packFrame(s.fluddFrame);
	e.nozzleFrame = packFrame(s.nozzleFrame);
	e.yoshiFrame  = packFrame(s.yoshiFrame);
	memcpy(out->extra, &e, sizeof(e));
}

extern "C" int port_mp_unpack_wire(const PortMpSnapshot* in, PortMpPuppetState* out)
{
	if (!(in->flags & PORT_MP_SNAP_VALID))
		return 0;
	PortMpPuppetState s;
	memset(&s, 0, sizeof(s));
	s.version  = PORT_MP_PUPPET_STATE_VERSION;
	s.size     = sizeof(s);
	s.seq      = in->tick;
	s.stage    = in->stage;
	s.scenario = in->scenario;
	s.warpSeq  = in->pad0;
	u32 f      = 0;
	if (!(in->flags & PORT_MP_SNAP_HIDDEN))
		f |= PMS_VISIBLE;
	if (in->flags & PORT_MP_SNAP_HOLDING)
		f |= PMS_HOLDING;
	if (in->flags & PORT_MP_SNAP_ON_YOSHI)
		f |= PMS_ON_YOSHI;
	if (in->flags & PORT_MP_SNAP_SPRAYING)
		f |= PMS_SPRAYING;
	if (in->flags & PORT_MP_SNAP_HAS_FLUDD)
		f |= PMS_HAS_FLUDD;
	if (in->flags & PORT_MP_SNAP_IN_WATER)
		f |= PMS_IN_WATER;
	if (in->flags & PORT_MP_SNAP_PAUSED)
		f |= PMS_IN_DEMO;
	if (in->flags & PORT_MP_SNAP_FX_OFF)
		f |= PMS_FX_OFF;
	const u16 aux = in->animAux;
	if (aux & (1u << 11))
		f |= PMS_CAP;
	if (aux & (1u << 12))
		f |= PMS_HELMET;
	if (aux & (1u << 13))
		f |= PMS_GLASSES;
	if (aux & (1u << 14))
		f |= PMS_SHIRT;
	if (aux & (1u << 15))
		f |= PMS_HAND4;
	s.flags = f;
	for (int k = 0; k < 3; ++k) {
		s.pos[k] = in->pos[k];
		s.vel[k] = in->vel[k];
	}
	s.fwdVel = in->forwardVel;
	for (int k = 0; k < 3; ++k)
		s.faceAngle[k] = in->faceAngle[k];
	s.modelFaceAngle = in->modelFaceAngle;
	s.status         = in->action;
	s.statusArg      = in->actionArg;
	s.marioFlags     = in->marioFlags;
	s.animId         = in->animId;
	s.frame[0]       = in->animFrame;
	s.rate[0]        = in->animRate;
	s.nozzle         = in->nozzle;
	s.secondNozzle   = in->secondNozzle;
	s.water          = in->water;
	s.pressure       = (u8)(in->sprayStrength * 255.0f + 0.5f);
	s.health         = in->health;
	s.yoshiState     = in->yoshiState;
	s.yoshiColor     = in->yoshiColor;
	s.hand           = aux & 3;
	s.btp            = ((aux >> 2) & 31) == 31 ? 0xFF : (aux >> 2) & 31;
	s.calc[0]        = ((aux >> 7) & 3) == 3 ? 0xFF : (aux >> 7) & 3;
	s.calc[1]        = ((aux >> 9) & 3) == 3 ? 0xFF : (aux >> 9) & 3;
	{
		const f32 yaw   = in->nozzleAngle[0] * (3.14159265f / 32768.0f);
		const f32 pitch = in->nozzleAngle[1] * (3.14159265f / 32768.0f);
		s.emitDir[0]    = std::cos(pitch) * std::sin(yaw);
		s.emitDir[1]    = std::sin(pitch);
		s.emitDir[2]    = std::cos(pitch) * std::cos(yaw);
	}
	WireExtra e;
	memcpy(&e, in->extra, sizeof(e));
	for (int i = 0; i < 4; ++i)
		s.bck[i >> 1][i & 1] = e.bck[i];
	s.frame[1] = e.upperFrame / 32.0f;
	s.frame[2] = e.eyeFrame;
	s.stageSeq = e.stageSeq;
	s.blend[0] = e.blend[0] / 255.0f;
	s.blend[1] = e.blend[1] / 255.0f;
	{
		const f32 x = e.quat[0] / 32767.0f, y = e.quat[1] / 32767.0f, z = e.quat[2] / 32767.0f;
		f32 ww      = 1.0f - x * x - y * y - z * z;
		const f32 w = ww > 0.0f ? std::sqrt(ww) : 0.0f;
		const f32 xx = x * x, yy = y * y, zz = z * z, xy = x * y, xz = x * z, yz = y * z;
		const f32 wx = w * x, wy = w * y, wz = w * z;
		s.baseMtx[0][0] = 1 - 2 * (yy + zz), s.baseMtx[0][1] = 2 * (xy - wz), s.baseMtx[0][2] = 2 * (xz + wy);
		s.baseMtx[1][0] = 2 * (xy + wz), s.baseMtx[1][1] = 1 - 2 * (xx + zz), s.baseMtx[1][2] = 2 * (yz - wx);
		s.baseMtx[2][0] = 2 * (xz - wy), s.baseMtx[2][1] = 2 * (yz + wx), s.baseMtx[2][2] = 1 - 2 * (xx + yy);
	}
	for (int k = 0; k < 3; ++k) {
		s.baseMtx[k][3] = s.pos[k] + e.baseOfs[k] / 4.0f;
		s.emitPos[k]    = s.pos[k] + e.emitOfs[k] * 2.0f;
	}
	if (e.heldCat != 0)
		s.heldActorType = (u32)(e.heldLo[0] | e.heldLo[1] << 8)
		                  | (e.heldCat == 0xFF ? 0u : 1u << (e.heldCat - 1));
	for (int k = 0; k < 4; ++k)
		s.headRot[k] = (s16)(e.head[k] * 256);
	for (int k = 0; k < 3; ++k)
		s.waistRot[k] = (s16)(e.waist[k] * 256);
	s.fluddBck    = e.fluddBck;
	s.nozzleBck   = e.nozzleBck;
	s.yoshiBck    = e.yoshiBck;
	s.fluddFrame  = e.fluddFrame / 32.0f;
	s.nozzleFrame = e.nozzleFrame / 32.0f;
	s.yoshiFrame  = e.yoshiFrame / 32.0f;
	*out = s;
	return 1;
}

// w10's session API (port_mp_net.cpp). Until that file is linked in, these
// fall back to "no session" (lld-link /alternatename), so the puppets and the
// ghost harness build and run on their own.
extern "C" {
int port_mp_w15_no_active(void) { return 0; }
int port_mp_w15_no_slot(void) { return -1; }
int port_mp_w15_no_player(int, PortMpPlayerInfo*) { return 0; }
int port_mp_w15_no_snapshots(int, PortMpSnapshotRx*, int) { return 0; }
void port_mp_w15_no_set_local(const PortMpSnapshot*) { }
double port_mp_w15_time_ms(void)
{
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
}
#pragma comment(linker, "/alternatename:port_mp_active=port_mp_w15_no_active")
#pragma comment(linker, "/alternatename:port_mp_local_slot=port_mp_w15_no_slot")
#pragma comment(linker, "/alternatename:port_mp_get_player=port_mp_w15_no_player")
#pragma comment(linker, "/alternatename:port_mp_get_snapshots=port_mp_w15_no_snapshots")
#pragma comment(linker, "/alternatename:port_mp_set_local_snapshot=port_mp_w15_no_set_local")
#pragma comment(linker, "/alternatename:port_mp_time_ms=port_mp_w15_time_ms")

// ---------------------------------------------------------------------------
// 2. puppet models

namespace {

const u32 kMarioMdlFlags = J3DMLF_MaterialPEFull | (16 << J3DMLF_TevStageNumShift);
const u32 kGunMdlFlags   = J3DMLF_MaterialPEFull | (4 << J3DMLF_TevStageNumShift);
const char kDirtyFile[]  = "/scene/map/pollution/H_ma_rak.bti";
const char kDirtyTex[]   = "H_ma_rak_dummy";

struct PuppetModel {
	J3DModelData* bodyData;
	J3DModel* body;
	J3DModel* hands[2][2]; // [hnd2, hnd3][r, l]
	J3DModel* hand4;
	J3DModel* cap[2];      // ma_cap1 (hat), ma_cap3 (no hat)
	M3UModelMario* model;
	M3UModelCommonMario* common;
	u8 jntCenter, jntChest, jntChnChest, jntHandR, jntHandL, jntHead, jntMHead;
	u8 jntChnFootR, jntFootR, jntChnFootL, jntFootL;
	// The joint "time lag" springs TMario, TMarioCap and TWaterGun put on their models
	// (TMultiMtxEffect: chest and upper arms, the cap, FLUDD's body and swinging handle).
	TMultiMtxEffect* bodyFx;
	TMultiMtxEffect* capFx[2];
	TMultiMtxEffect* fluddFx;
	u8 fxWarpSeq, fxStageSeq;
	bool fxHaveSeq;
	Mtx prevBase; // last frame's base matrix (the mid-frame pass interpolates towards it)
	bool havePrevBase;
	s16 propAngle, propSpeed; // the turbo nozzle's propeller (TWaterGun::unk1CD0 / unk1CD2)
	MActor* fludd;
	u8 fluddNozzleJoint;
	MActor* nozzles[6];
	bool nozzleTried[6];
	int lastHand;
	// loaded on first use
	J3DModel* helmet;  // diver_helm.bmd on jnt_head
	J3DModel* glasses; // ma_glass1.bmd on M_head
	bool helmetTried, glassesTried;
	MActor* yoshi;     // yoshi_model.bmd (TYoshi::init)
	J3DModel* yoshiHands[2];
	bool yoshiTried;
	u8 yoshiColor;     // colour applied to the materials, 0xFF = none yet
	// held object: a visual copy (own J3DModel of a local actor's model data)
	struct Held {
		u32 type;
		J3DModel* model; // null if no local actor of that type has a model
		f32 scale[3];
	} held[4];
	int heldNum;
	J3DModel* heldNow; // this frame's held copy, or null
	void* atlas;       // own copy of the colour atlas (recoloured in place on changes)
};

// The pose a puppet is drawn with this frame (interpolated snapshot).
struct Pose {
	PortMpPuppetState s;
	bool valid;
	bool havePlay; // playSeq is set
	u32 playSeq;   // newest snapshot tick the playback has passed (water replay)
	u32 seqA, seqB; // the two snapshots this pose lies between (seq as pushed) and where
	f32 lerpT;      // (0 = seqA, 1 = seqB); for the pose-dump harness
};

// Read by the head/waist joint callbacks while one puppet's body is calculated.
const Pose* sCalcPose;
int sCalcSlot = -1; // the player whose puppet calcPuppet works on (dance emotes)

int PuppetHeadCtrl(J3DNode*, int param)
{
	if (param == 0 && sCalcPose != nullptr) {
		const s16* r = sCalcPose->s.headRot;
		if (r[0] | r[1] | r[2] | r[3]) {
			Mtx t;
			MsMtxSetRotRPH(t, SHORTANGLE2DEG(r[0]), SHORTANGLE2DEG(r[1]), SHORTANGLE2DEG(r[2]));
			if (r[3] != 0) {
				Mtx g;
				MsMtxSetRotRPH(g, 0.0f, 0.0f, SHORTANGLE2DEG(r[3]));
				MTXConcat(t, g, t);
			}
			MTXConcat(J3DSys::mCurrentMtx, t, J3DSys::mCurrentMtx);
		}
	}
	return 1;
}

int PuppetWaistCtrl(J3DNode*, int param)
{
	if (param == 0 && sCalcPose != nullptr) {
		const s16* r = sCalcPose->s.waistRot;
		if (r[0] | r[1] | r[2]) {
			Mtx t;
			MsMtxSetRotRPH(t, SHORTANGLE2DEG(r[0]), SHORTANGLE2DEG(r[1]), SHORTANGLE2DEG(r[2]));
			MTXConcat(J3DSys::mCurrentMtx, t, J3DSys::mCurrentMtx);
		}
	}
	return 1;
}

// Feet on the ground (MarioFootPos*/Dir*Ctrl in MarioDraw.cpp): while Mario stands (status type
// waiting, not the brake end, not on Yoshi, not sleeping) each foot is dropped onto the ground
// under it (up to 15 units) and turned to the ground's normal; the puppet does the same with
// its own joints from the snapshot's status (the stage's collision is the same on every client).
bool sPuppetFinalPass = true; // TMario installs the foot callbacks on the last simulation tick only

bool puppetFeetOnGround()
{
	if (sCalcPose == nullptr || gpMap == nullptr || !sPuppetFinalPass)
		return false;
	const u32 st = sCalcPose->s.status;
	return (st & MARIO_STATUS_TYPE_MASK) == MARIO_STATUS_TYPE_WAITING && st != MARIO_STATUS_BRAKE_END
	       && !(sCalcPose->s.flags & PMS_ON_YOSHI) && st != MARIO_STATUS_SLEEPY && st != MARIO_STATUS_SLEEP;
}

int puppetFootPos(J3DNode* node, int param)
{
	if (param != 0 || !puppetFeetOnGround())
		return 1;
	MtxPtr footMtx = j3dSys.getModel()->getAnmMtx(((J3DJoint*)node)->getJntNo());
	const TBGCheckData* plane;
	const f32 dist = gpMap->checkGround(footMtx[0][3], footMtx[1][3], footMtx[2][3], &plane);
	if (plane == nullptr || plane->isIllegalData())
		return 1;
	f32 move = dist - footMtx[1][3] + 10.0f;
	move     = move > 15.0f ? 15.0f : (move < -15.0f ? -15.0f : move);
	J3DSys::mCurrentMtx[1][3] += move;
	return 1;
}

int puppetFootDir(J3DNode* node, int param)
{
	if (param != 0 || !puppetFeetOnGround())
		return 1;
	MtxPtr footMtx = j3dSys.getModel()->getAnmMtx(((J3DJoint*)node)->getJntNo());
	const TBGCheckData* plane;
	gpMap->checkGround(footMtx[0][3], footMtx[1][3], footMtx[2][3], &plane);
	if (plane == nullptr || plane->isIllegalData())
		return 1;
	Vec dir = { J3DSys::mCurrentMtx[0][0], J3DSys::mCurrentMtx[1][0], J3DSys::mCurrentMtx[2][0] };
	Vec n   = { -plane->getNormal().x, -plane->getNormal().y, -plane->getNormal().z };
	Vec c1, c2;
	PSVECCrossProduct(&dir, &n, &c1);
	PSVECCrossProduct(&n, &c1, &c2);
	PSVECNormalize(&c1, &c1);
	PSVECNormalize(&c2, &c2);
	f32(*m)[4] = J3DSys::mCurrentMtx;
	m[0][0] = c2.x, m[1][0] = c2.y, m[2][0] = c2.z;
	m[0][1] = n.x, m[1][1] = n.y, m[2][1] = n.z;
	m[0][2] = c1.x, m[1][2] = c1.y, m[2][2] = c1.z;
	// the game's stored foot matrix keeps its old [2][1] (MarioFootDir*Ctrl writes [2][2] twice):
	// the same here, so the foot mesh is drawn exactly as the original draws it
	footMtx[0][0] = c2.x, footMtx[1][0] = c2.y, footMtx[2][0] = c2.z;
	footMtx[0][1] = n.x, footMtx[1][1] = n.y;
	footMtx[0][2] = c1.x, footMtx[1][2] = c1.y, footMtx[2][2] = c1.z;
	return 1;
}

void setAllShapes(J3DModelData* d, bool visible)
{
	for (u16 i = 0; i < d->getShapeNum(); ++i) {
		if (visible)
			d->getShapeNodePointer(i)->offFlag(J3DShpFlag_Visible);
		else
			d->getShapeNodePointer(i)->onFlag(J3DShpFlag_Visible);
	}
}

// TMario::changeHand on the puppet's own models.
void puppetChangeHand(PuppetModel* p, int idx)
{
	if (p->hands[0][0] == nullptr || p->lastHand == idx)
		return;
	p->lastHand     = idx;
	J3DModelData* d = p->bodyData;
	switch (idx) {
	default:
	case 0:
		d->getShapeNodePointer(5)->offFlag(J3DShpFlag_Visible);
		d->getShapeNodePointer(6)->offFlag(J3DShpFlag_Visible);
		for (int i = 0; i < 2; ++i)
			for (int j = 0; j < 2; ++j)
				setAllShapes(p->hands[i][j]->getModelData(), false);
		break;
	case 1:
		d->getShapeNodePointer(5)->onFlag(J3DShpFlag_Visible);
		d->getShapeNodePointer(6)->onFlag(J3DShpFlag_Visible);
		for (int j = 0; j < 2; ++j) {
			setAllShapes(p->hands[0][j]->getModelData(), true);
			setAllShapes(p->hands[1][j]->getModelData(), false);
		}
		break;
	case 2:
		d->getShapeNodePointer(5)->onFlag(J3DShpFlag_Visible);
		d->getShapeNodePointer(6)->onFlag(J3DShpFlag_Visible);
		for (int j = 0; j < 2; ++j) {
			setAllShapes(p->hands[0][j]->getModelData(), false);
			setAllShapes(p->hands[1][j]->getModelData(), true);
		}
		break;
	}
}

J3DModel* loadPartsModel(const char* path, J3DModelData* body, ResTIMG* dirty)
{
	J3DModel* m = SMS_CreatePartsModel((char*)path, kMarioMdlFlags);
	if (m == nullptr)
		return nullptr;
	// As TMario::initModel: the parts share the body's skin texture. Own headers first,
	// so the copy (maybe a recoloured atlas) does not land in the file the local
	// Mario's parts read too.
	portMpPaletteOwnHeaders(m->getModelData());
	m->getModelData()->getTexture()->setResTIMG(0, *body->getTexture()->getResTIMG(0));
	DCFlushRange(m->getModelData()->getTexture()->getResTIMG(0), 0x20);
	if (dirty != nullptr)
		SMS_ChangeTextureAll(m->getModelData(), kDirtyTex, *dirty);
	return m;
}

MActor* loadMActor(JKRHeap* heap, const char* dir, const char* bmd, u32 flags)
{
	void* res = JKRFileLoader::getGlbResource(bmd);
	if (res == nullptr)
		return nullptr;
	MActorAnmData* ad = heapNew<MActorAnmData>(heap);
	if (ad == nullptr)
		return nullptr;
	ad->init(dir, nullptr);
	void* mem = JKRHeap::alloc(sizeof(MActor), 0x10, heap);
	if (mem == nullptr)
		return nullptr;
	MActor* a       = new (mem) MActor(ad);
	J3DModelData* d = J3DModelLoaderDataBase::load(res, flags);
	void* mmem      = JKRHeap::alloc(sizeof(J3DModel), 0x10, heap);
	if (d == nullptr || mmem == nullptr)
		return nullptr;
	a->setModel(new (mmem) J3DModel(d, 0, 1), 0);
	return a;
}

// Everything a puppet loads (including what J3D/MActor allocate inside) goes to the
// stage heap, TApplication::mHeap, which is freed with the stage.
struct StageHeapScope {
	JKRHeap* prev;
	StageHeapScope()
	    : prev(nullptr)
	{
		JKRHeap* stage = SMSGetApplication()->mHeap;
		if (stage != nullptr && JKRGetCurrentHeap() != stage)
			prev = stage->becomeCurrentHeap();
	}
	~StageHeapScope()
	{
		if (prev != nullptr)
			prev->becomeCurrentHeap();
	}
};

// Debug: SMS_PORT_MP_DUMPMAT=1 prints the TEV setup of every material of a model.
void dumpMaterials(const char* what, J3DModelData* d)
{
	OSReport("[mp] materials of %s: %d\n", what, d->getMaterialNum());
	for (u16 i = 0; i < d->getTexture()->getNum(); ++i) {
		ResTIMG* t = d->getTexture()->getResTIMG(i);
		OSReport("[mp] tex %d %s fmt %d %dx%d mips %d\n", i, d->getTextureName()->getName(i), t->format,
		         t->width, t->height, t->mipmapCount);
		char path[64];
		snprintf(path, sizeof(path), "tex%d.bin", i);
		if (FILE* f = fopen(path, "wb")) {
			fwrite(t, 0x20, 1, f);
			fwrite((u8*)t + t->imageDataOffset, (u32)t->width * t->height, 1, f); // >= any 4/8 bpp size
			fclose(f);
		}
	}
	for (u16 i = 0; i < d->getMaterialNum(); ++i) {
		J3DMaterial* m = d->getMaterialNodePointer(i);
		J3DTevBlock* t = m->getTevBlock();
		OSReport("[mp] mat %d %s stages %d", i, d->getMaterialName()->getName(i), t->getTevStageNum());
		for (int k = 0; k < 4; ++k) {
			J3DGXColorS10* c = t->getTevColor(k);
			J3DGXColor* kc   = t->getTevKColor(k);
			if (c != nullptr)
				OSReport(" c%d=%d,%d,%d,%d", k, c->color.r, c->color.g, c->color.b, c->color.a);
			if (kc != nullptr)
				OSReport(" k%d=%02x%02x%02x%02x", k, kc->color.r, kc->color.g, kc->color.b, kc->color.a);
		}
		if (J3DGXColor* mc = m->getMatColor(0))
			OSReport(" mat0=%02x%02x%02x%02x", mc->color.r, mc->color.g, mc->color.b, mc->color.a);
		if (J3DColorChan* ch = m->getColorChan(0))
			OSReport(" chan0 en%d src%d", ch->getEnable(), ch->getMatSrc());
		OSReport("\n");
		for (int s = 0; s < t->getTevStageNum(); ++s) {
			J3DTevOrder* o = t->getTevOrder(s);
			J3DTevStage* st = t->getTevStage(s);
			u16 tex         = o->mTexMap < 8 ? t->getTexNo(o->mTexMap) : 0xFFFF;
			OSReport("[mp]    st%d map%d(%s) chan%d C=%x%x%x%x op%d out%d A=%x%x%x%x ksel%02x\n", s,
			         o->mTexMap, tex != 0xFFFF ? d->getTextureName()->getName(tex) : "-",
			         o->mColorChan, st->getTevColorA(), st->getTevColorB(), st->getTevColorC(),
			         st->getTevColorD(), st->getTevColorOp(), st->getTevColorOutReg(), st->getAlphaA(),
			         st->getAlphaB(), st->getAlphaC(), 0, t->getTevKColorSel(s));
		}
	}
}

// Minimum free bytes in the stage heap before a puppet is built (measured cost of
// one puppet is logged on the first build).
u32 sMinFree = 0x180000;

// A TMultiMtxEffect as TMario / TMarioCap / TWaterGun build theirs (joint callbacks with a
// spring state per bone, parameters from "/<prm>/MtxEffect<bone>.prm"). Allocated with the
// game's operator new: the caller is inside the stage heap scope, like the rest of the puppet.
// The .prm files live in the "params" archive, which TMarDirector only mounts while the stage's
// objects are set up (TParams::init / finalize), so a puppet built during play cannot load them:
// the values are copied from the local Mario's own effects (`src`, same bones in the same order),
// which loaded the same files at stage setup.
TMultiMtxEffect* makeMtxEffect(J3DModel* model, const char* prm, int n, const u16* bones, const u8* types,
                               const TMultiMtxEffect* src)
{
	TMultiMtxEffect* fx = new TMultiMtxEffect;
	fx->mNumBones       = (u16)n;
	u16* ids            = new u16[n];
	u8* ty              = new u8[n];
	for (int i = 0; i < n; ++i)
		ids[i] = bones[i], ty[i] = types[i];
	fx->mBoneIDs       = ids;
	fx->mMtxEffectType = ty;
	fx->setup(model, prm);
	for (int i = 0; src != nullptr && i < n && i < src->mNumBones; ++i) {
		if (src->mMtxEffectType[i] != types[i] || src->mMtxEffectTbl[i] == nullptr)
			continue;
		if (types[i] == TMTX_EFFECT_TIME_LAG) {
			TMtxTimeLag::TDeParams& d       = ((TMtxTimeLag*)fx->mMtxEffectTbl[i])->mParams;
			const TMtxTimeLag::TDeParams& s = ((const TMtxTimeLag*)src->mMtxEffectTbl[i])->mParams;
			d.mPosAccel.value = s.mPosAccel.value, d.mPosBrake.value = s.mPosBrake.value;
			d.mPosLimit.value = s.mPosLimit.value, d.mQuatAccel.value = s.mQuatAccel.value;
			d.mQuatBrake.value = s.mQuatBrake.value;
		} else {
			TMtxSwingRZ::TDeParams& d       = ((TMtxSwingRZ*)fx->mMtxEffectTbl[i])->mParams;
			const TMtxSwingRZ::TDeParams& s = ((const TMtxSwingRZ*)src->mMtxEffectTbl[i])->mParams;
			d.mAcc.value = s.mAcc.value, d.mL.value = s.mL.value;
			d.mBrake.value = s.mBrake.value, d.mVelScale.value = s.mVelScale.value;
		}
	}
	return fx;
}

// TMario::addCallBack's rules for the springs, from the snapshot: off while the sender says so
// (PMS_FX_OFF: a stage's first 0x4B0 movement ticks, pumping); on for animations flagged for it
// (gMarioAnimeData unk6 bit 1: body, bit 2: FLUDD's handle) while the upper body is idle. The
// springs restart (flag 2) when the puppet snaps.
bool puppetMtxEffects(PuppetModel* p, const PortMpPuppetState& s)
{
	const bool snap = !p->fxHaveSeq || p->fxWarpSeq != s.warpSeq || p->fxStageSeq != s.stageSeq;
	p->fxHaveSeq    = true;
	p->fxWarpSeq    = s.warpSeq;
	p->fxStageSeq   = s.stageSeq;
	const bool upperIdle = s.calc[1] == 0xFF;
	const bool off       = (s.flags & PMS_FX_OFF) != 0;
	const u8 anmFlags    = s.animId < 336 ? gMarioAnimeData[s.animId].unk6 : 0;
	const bool bodyOn    = !off && (anmFlags & 2) != 0 && upperIdle;
	const bool capOn     = !off;
	const bool handleOn  = (anmFlags & 4) != 0 && upperIdle;
	TMultiMtxEffect* all[4] = { p->bodyFx, p->capFx[0], p->capFx[1], p->fluddFx };
	for (int i = 0; i < 4; ++i)
		if (all[i] != nullptr && snap)
			all[i]->flagOn(0x2);
	if (p->bodyFx != nullptr)
		bodyOn ? p->bodyFx->flagOn(0x1) : p->bodyFx->flagOff(0x1);
	for (int i = 0; i < 2; ++i)
		if (p->capFx[i] != nullptr)
			capOn ? p->capFx[i]->flagOn(0x1) : p->capFx[i]->flagOff(0x1);
	if (p->fluddFx != nullptr && p->fluddFx->mNumBones > 1) {
		if (handleOn)
			p->fluddFx->mMtxEffectTbl[1]->onFlag(0x1);
		else
			p->fluddFx->mMtxEffectTbl[1]->offFlag(0x1);
	}
	return snap;
}

// FLUDD on the chest, as TWaterGun::setBaseTRMtx with its constant angles (unk1D04 0, unk1D06
// -0x1800), then its body calc (the nozzle follows in calcPuppet).
void calcPuppetFludd(PuppetModel* p, const PortMpPuppetState& s)
{
	MtxPtr chest = p->body->getAnmMtx(p->jntChest);
	f32 a        = chest[1][0] < 0.0f ? -chest[1][0] : chest[1][0];
	s16 angle    = (s16)(a * (0 - -0x1800) + -0x1800);
	Mtx rot, base;
	MsMtxSetRotRPH(rot, 0.0f, 0.0f, SHORTANGLE2DEG(angle));
	MTXConcat(chest, rot, base);
	p->fludd->getModel()->setBaseTRMtx(base);
	if (s.fluddBck >= 0) {
		if (!p->fludd->checkCurBckFromIndex(s.fluddBck))
			p->fludd->setBckFromIndex(s.fluddBck);
		if (J3DFrameCtrl* fc = p->fludd->getFrameCtrl(ANM_TYPE_BCK)) {
			fc->setRate(0.0f);
			fc->setFrame(s.fluddFrame);
		}
	}
	p->fludd->calc();
}

PuppetModel* buildPuppet(const PortMpCharacter* pal)
{
	TMario* m = gpMarioOriginal;
	if (m == nullptr || m->mModel == nullptr || m->mModel->unk4 == nullptr)
		return nullptr;
	StageHeapScope scope;
	JKRHeap* heap = JKRGetCurrentHeap();
	if (heap == nullptr)
		return nullptr;
	const u32 freeBefore = heap->getFreeSize();
	if (freeBefore < sMinFree) {
		OSReport("[mp] puppet: not built, stage heap has 0x%x free (need 0x%x)\n", freeBefore,
		         sMinFree);
		return nullptr;
	}
	PuppetModel* p = heapNew<PuppetModel>(heap);
	if (p == nullptr)
		return nullptr;
	memset(p, 0, sizeof(*p));
	p->lastHand   = -1;
	p->yoshiColor = 0xFF;

	void* bodyRes = JKRFileLoader::getGlbResource("/mario/bmd/ma_mdl1.bmd");
	if (bodyRes == nullptr)
		return nullptr;
	J3DModelData* d = J3DModelLoaderDataBase::load(bodyRes, kMarioMdlFlags);
	if (d == nullptr)
		return nullptr;
	p->bodyData    = d;
	p->jntCenter   = d->getJointName()->getIndex("center");
	p->jntChnChest = d->getJointName()->getIndex("chn_chest");
	p->jntChest    = d->getJointName()->getIndex("jnt_chest");
	p->jntHandR    = d->getJointName()->getIndex("jnt_hand_R");
	p->jntHandL    = d->getJointName()->getIndex("jnt_hand_L");
	p->jntHead     = d->getJointName()->getIndex("jnt_head");
	p->jntMHead    = d->getJointName()->getIndex("M_head");

	if (envStr("SMS_PORT_MP_DUMPMAT") != nullptr)
		dumpMaterials("ma_mdl1", d);
	// Own texture headers (the loaded data points into the shared file), then the
	// player's colours (a recoloured copy of the atlas; nothing for classic).
	portMpPaletteOwnHeaders(d);
	p->atlas = portMpPaletteApplyModel(d, pal);
	ResTIMG* dirty = (ResTIMG*)JKRFileLoader::getGlbResource(kDirtyFile);
	if (dirty != nullptr)
		SMS_ChangeTextureAll(d, kDirtyTex, *dirty);

	void* bm = JKRHeap::alloc(sizeof(J3DModel), 0x10, heap);
	if (bm == nullptr)
		return nullptr;
	p->body = new (bm) J3DModel(d, 0, 1);

	static const char* const kHands[2][2] = {
		{ "/mario/bmd/ma_hnd2r.bmd", "/mario/bmd/ma_hnd2l.bmd" },
		{ "/mario/bmd/ma_hnd3r.bmd", "/mario/bmd/ma_hnd3l.bmd" },
	};
	bool handsOk = true;
	for (int i = 0; i < 2; ++i)
		for (int j = 0; j < 2; ++j)
			if ((p->hands[i][j] = loadPartsModel(kHands[i][j], d, dirty)) == nullptr)
				handsOk = false;
	p->hand4 = loadPartsModel("/mario/bmd/ma_hnd4r.bmd", d, dirty);
	if (!handsOk || p->hand4 == nullptr) {
		for (int i = 0; i < 2; ++i)
			for (int j = 0; j < 2; ++j)
				p->hands[i][j] = nullptr;
		p->hand4 = nullptr;
	}
	d->getShapeNodePointer(4)->onFlag(J3DShpFlag_Visible);

	// Eye materials animate (blink patterns), as in TMario::initModel.
	u16 eyeL = d->getMaterialName()->getIndex("_mat_eye_L");
	u16 eyeR = d->getMaterialName()->getIndex("_mat_eye_R");
	d->getMaterialNodePointer(eyeL)->change();
	d->getMaterialNodePointer(eyeL)->setMaterialAnm(heapNew<J3DMaterialAnm>(heap));
	d->getMaterialNodePointer(eyeR)->change();
	d->getMaterialNodePointer(eyeR)->setMaterialAnm(heapNew<J3DMaterialAnm>(heap));

	// Animation: own blend calcs and frame controls, the local Mario's BCK and BTP
	// tables (as TEnemyMario::initModel does).
	M3UModelCommonMario* common = heapNew<M3UModelCommonMario>(heap);
	M3UModelCommon* src         = m->mModel->unk4;
	common->unk4                = src->unk4;
	common->unk8                = src->unk8;
	common->unkC                = src->unkC;
	common->unk18               = heapNewArray<M3UMtxCalcSIAnmBlendQuat>(heap, 2);
	common->unk18[0].mMotionBlendRatio = 0.0f;
	p->common                   = common;

	M3UModelMario* mm = heapNew<M3UModelMario>(heap);
	mm->unk8          = p->body;
	mm->unk4          = common;
	mm->unk20         = common;
	mm->unkC          = heapNewArray<J3DFrameCtrl>(heap, 3);
	mm->unkC[2].setRate(SMSGetAnmFrameRate());
	SomeModelMarioStruct* setInfo = heapNewArray<SomeModelMarioStruct>(heap, 2);
	setInfo[0]                    = (SomeModelMarioStruct) { 0, 2, 0, { 0x14, 0x41 }, 0 };
	setInfo[1]                    = (SomeModelMarioStruct) { p->jntChest, 2, 1, { 0, 0 }, 1 };
	mm->unk10                     = 2;
	mm->unk24                     = setInfo;
	M3UModel::Unk1CStruct* tex    = heapNew<M3UModel::Unk1CStruct>(heap);
	tex->unk0                     = 0;
	tex->unk1                     = 2;
	mm->unk1C                     = tex;
	mm->changeMtxCalcSIAnmBQAnmTransform(0, 0, 0x3e);
	mm->changeMtxCalcSIAnmBQAnmTransform(1, 0, 0x41);
	mm->unkC[1].setRate(0.0f);
	common->unk18[1].mMotionBlendRatio = 0.0f;
	p->model                           = mm;

	Mtx id;
	MTXIdentity(id);
	p->body->setBaseTRMtx(id);
	mm->updateInMotion();
	p->body->calc();
	mm->updateOut();

	// TMario::finalDrawInitialize
	puppetChangeHand(p, 0);
	SMS_MakeDLAndLock(p->body);
	for (int i = 0; i < d->getMaterialNum(); ++i)
		if (i == eyeL || i == eyeR)
			p->body->getMatPacketArray()[i].offFlag(0x1);
	for (int i = 0; i < d->getMaterialNum(); ++i) {
		d->getMaterialNodePointer(i)->getTevBlock()->getTevKColor(0)->color.a = 0; // clean
		SMS_InitPacket_OneTevKColorAndFog(p->body, i, GX_KCOLOR0, nullptr);
	}

	// Head / waist rotations and the feet on the ground, as the local Mario's joint callbacks.
	d->getJointNodePointer(p->jntHead)->setCallBack(PuppetHeadCtrl);
	d->getJointNodePointer(p->jntChnChest)->setCallBack(PuppetWaistCtrl);
	p->jntChnFootR = d->getJointName()->getIndex("chn_foot_R");
	p->jntFootR    = d->getJointName()->getIndex("jnt_foot_R");
	p->jntChnFootL = d->getJointName()->getIndex("chn_foot_L");
	p->jntFootL    = d->getJointName()->getIndex("jnt_foot_L");
	d->getJointNodePointer(p->jntChnFootR)->setCallBack(puppetFootPos);
	d->getJointNodePointer(p->jntFootR)->setCallBack(puppetFootDir);
	d->getJointNodePointer(p->jntChnFootL)->setCallBack(puppetFootPos);
	d->getJointNodePointer(p->jntFootL)->setCallBack(puppetFootDir);
	// The time-lag springs on the chest and upper arms (TMario::initModel, "/Mario/MtxEffect*.prm"):
	// without them a puppet's arms snap where Mario's trail behind in jumps and dives.
	{
		const u16 bones[3] = { (u16)p->jntChest, d->getJointName()->getIndex("jnt_arm_R1"),
			                   d->getJointName()->getIndex("jnt_arm_L1") };
		const u8 types[3]  = { TMTX_EFFECT_TIME_LAG, TMTX_EFFECT_TIME_LAG, TMTX_EFFECT_TIME_LAG };
		p->bodyFx          = makeMtxEffect(p->body, "Mario", 3, bones, types, m->mMultiMtxEffect);
	}

	// Cap: as TMarioCap::TMarioCap (hat and bare-head models).
	static const char* const kCaps[2] = { "/mario/bmd/ma_cap1.bmd", "/mario/bmd/ma_cap3.bmd" };
	for (int i = 0; i < 2; ++i) {
		void* res = JKRFileLoader::getGlbResource(kCaps[i]);
		if (res == nullptr)
			continue;
		J3DModelData* cd = J3DModelLoaderDataBase::load(res, kMarioMdlFlags);
		portMpPaletteOwnHeaders(cd);
		cd->getTexture()->setResTIMG(0, *d->getTexture()->getResTIMG(0));
		DCFlushRange(cd->getTexture()->getResTIMG(0), sizeof(ResTIMG));
		void* cm = JKRHeap::alloc(sizeof(J3DModel), 0x10, heap);
		if (cm == nullptr)
			continue;
		p->cap[i] = new (cm) J3DModel(cd, 0, 1);
		if (dirty != nullptr) {
			SMS_ChangeTextureAll(cd, kDirtyTex, *dirty);
			SMS_MakeDLAndLock(p->cap[i]);
		}
		for (u16 k = 0; k < cd->getMaterialNum(); ++k) {
			cd->getMaterialNodePointer(k)->getTevBlock()->getTevKColor(0)->color.a = 0;
			SMS_InitPacket_OneTevKColorAndFog(p->cap[i], k, GX_KCOLOR0, nullptr);
		}
		// the cap's own time lag (TMarioCap::TMarioCap, bone 1)
		const u16 capBone = 1;
		const u8 capType  = TMTX_EFFECT_TIME_LAG;
		if (cd->getJointNum() > 1)
			p->capFx[i] = makeMtxEffect(p->cap[i], "Mario/MarioCap", 1, &capBone, &capType,
			                            m->mCap != nullptr ? (i == 0 ? m->mCap->unk20 : m->mCap->unk24) : nullptr);
	}

	// FLUDD body (TWaterGun::init); nozzles are loaded on first use.
	p->fludd = loadMActor(heap, "/mario/watergun2/body", "/mario/watergun2/body/wg_mdl1.bmd",
	                      kGunMdlFlags);
	if (p->fludd != nullptr) {
		J3DModelData* fd    = p->fludd->getModel()->getModelData();
		p->fluddNozzleJoint = fd->getJointName()->getIndex("nozzle_center");
		// TWaterGun::init: the body's time lag and the swinging handle
		const u16 bones[2] = { 0, fd->getJointName()->getIndex("jnt_G_handle") };
		const u8 types[2]  = { TMTX_EFFECT_TIME_LAG, TMTX_EFFECT_SWING_RZ };
		p->fluddFx         = makeMtxEffect(p->fludd->getModel(), "Mario/WaterGun", 2, bones, types,
		                                   m->mWaterGun != nullptr ? m->mWaterGun->unk1CDC : nullptr);
	}

	OSReport("[mp] puppet built: 0x%x bytes of the stage heap (0x%x left)\n",
	         freeBefore - heap->getFreeSize(), heap->getFreeSize());
	return p;
}

// The nozzle joint callbacks TWaterGun::init installs (WaterGun.cpp): the spray nozzle's muzzle
// and the turbo nozzle's neck turn with the gun angle while it aims down (NozzleCtrl; the snapshot
// carries half of it in headRot[3], MarioHeadCtrl's share), the turbo nozzle's propeller spins
// (RotateCtrl, TWaterGun::unk1CD0: the puppet keeps its own angle, driven by the pressure).
int puppetNozzleCtrl(J3DNode*, int param)
{
	if (param == 0 && sCalcPose != nullptr && sCalcPose->s.headRot[3] < 0) {
		Mtx t;
		MsMtxSetRotRPH(t, 0.0f, 0.0f, SHORTANGLE2DEG((s16)(sCalcPose->s.headRot[3] * 2)));
		MTXConcat(J3DSys::mCurrentMtx, t, J3DSys::mCurrentMtx);
	}
	return 1;
}

int puppetPropCtrl(J3DNode* node, int param)
{
	PuppetModel* p = (PuppetModel*)node->getCallBackUserData();
	if (param == 0 && p != nullptr) {
		Mtx t;
		MsMtxSetRotRPH(t, 0.005493164f * p->propAngle, 0.0f, 0.0f);
		MTXConcat(J3DSys::mCurrentMtx, t, J3DSys::mCurrentMtx);
	}
	return 1;
}

void puppetNozzleCallbacks(PuppetModel* p, int type, J3DModelData* d)
{
	struct Hook {
		int type;
		const char* joint;
		J3DNodeCallBack fn;
	};
	static const Hook kHooks[] = {
		{ 0, "chn_muzzle_1", puppetNozzleCtrl },          // Spray
		{ 5, "chn_back_nozzle_prop", puppetPropCtrl },    // Turbo
		{ 5, "jnt_back_nozzle_neck", puppetNozzleCtrl },  // Turbo
	};
	for (const Hook& h : kHooks) {
		if (h.type != type)
			continue;
		const int j = d->getJointName()->getIndex(h.joint);
		if (j < 0 || j >= d->getJointNum())
			continue;
		d->getJointNodePointer(j)->setCallBack(h.fn);
		d->getJointNodePointer(j)->setCallBackUserData(p);
	}
}

// TWaterGun::rotateProp, once per simulation tick: the propeller's speed from the trigger fill.
void puppetPropStep(PuppetModel* p, const PortMpPuppetState& s)
{
	TMario* m = gpMarioOriginal;
	if (m == nullptr || m->mWaterGun == nullptr)
		return;
	const TWaterGunParams& wp = m->mWaterGun->mWatergunParams;
	if (s.nozzle == 5) {
		f32 sp = p->propSpeed + (s.pressure / 255.0f) * wp.mNozzleAngleYSpeed.get();
		sp *= wp.mNozzleAngleYBrake.get();
		if ((f32)wp.mNozzleAngleYSpeedMax.get() < sp)
			sp = (f32)wp.mNozzleAngleYSpeedMax.get();
		p->propSpeed = (s16)sp;
		p->propAngle = (s16)(p->propAngle + p->propSpeed);
	} else {
		p->propSpeed = 0;
		p->propAngle = 0;
	}
}

MActor* puppetNozzle(PuppetModel* p, int type)
{
	if (type < 0 || type >= 6 || p->fludd == nullptr)
		return nullptr;
	if (!p->nozzleTried[type]) {
		p->nozzleTried[type] = true;
		const char* dir      = nozzleBmdData.getPath(type);
		const char* bmd      = nozzleBmdData.getBmdPath(type);
		if (dir != nullptr && bmd != nullptr) {
			StageHeapScope scope;
			JKRHeap* heap = JKRGetCurrentHeap();
			if (heap != nullptr && heap->getFreeSize() > 0x40000) {
				MActor* a = loadMActor(heap, dir, bmd, kGunMdlFlags);
				if (a != nullptr) {
					SMS_ChangeTextureAll(
					    a->getModel()->getModelData(), "H_watergun_main_dummy",
					    *p->fludd->getModel()->getModelData()->getTexture()->getResTIMG(1));
					a->initDL();
					puppetNozzleCallbacks(p, type, a->getModel()->getModelData());
				}
				p->nozzles[type] = a;
			}
		}
	}
	return p->nozzles[type];
}

J3DModel* loadSimpleModel(const char* path, u32 flags)
{
	StageHeapScope scope;
	JKRHeap* heap = JKRGetCurrentHeap();
	void* res     = JKRFileLoader::getGlbResource(path);
	if (heap == nullptr || res == nullptr || heap->getFreeSize() < 0x40000)
		return nullptr;
	J3DModelData* d = J3DModelLoaderDataBase::load(res, flags);
	void* mem       = JKRHeap::alloc(sizeof(J3DModel), 0x10, heap);
	if (d == nullptr || mem == nullptr)
		return nullptr;
	return new (mem) J3DModel(d, 0, 1);
}

J3DModel* puppetHelmet(PuppetModel* p)
{
	if (!p->helmetTried) {
		p->helmetTried = true;
		p->helmet      = loadSimpleModel("/mario/watergun2/body/diver_helm.bmd", kMarioMdlFlags);
	}
	return p->helmet;
}

J3DModel* puppetGlasses(PuppetModel* p)
{
	if (!p->glassesTried) {
		p->glassesTried = true;
		p->glasses      = loadSimpleModel("/mario/bmd/ma_glass1.bmd", kMarioMdlFlags);
	}
	return p->glasses;
}

// Yoshi as TYoshi::init builds it (model, motion blend, no DL; hands).
MActor* puppetYoshi(PuppetModel* p)
{
	if (p->yoshiTried)
		return p->yoshi;
	p->yoshiTried = true;
	StageHeapScope scope;
	JKRHeap* heap = JKRGetCurrentHeap();
	if (heap == nullptr || heap->getFreeSize() < 0x80000
	    || JKRFileLoader::getGlbResource("/yoshi/yoshi_model.bmd") == nullptr)
		return nullptr;
	MActor* a = loadMActor(heap, "yoshi", "/yoshi/yoshi_model.bmd", kGunMdlFlags);
	if (a == nullptr)
		return nullptr;
	a->initNormalMotionBlend();
	a->offMakeDL();
	static const char* const kHands[2] = { "/yoshi/yoshi_hand2_l.bmd", "/yoshi/yoshi_hand2_r.bmd" };
	for (int i = 0; i < 2; ++i) {
		J3DModel* h = SMS_CreatePartsModel((char*)kHands[i], kGunMdlFlags);
		if (h != nullptr) {
			h->getModelData()->getTexture()->setResTIMG(
			    0, *a->getModel()->getModelData()->getTexture()->getResTIMG(0));
			DCFlushRange(h->getModelData()->getTexture()->getResTIMG(0), sizeof(ResTIMG));
		}
		p->yoshiHands[i] = h;
	}
	p->yoshi = a;
	return a;
}

// A local actor of that type with a model (managers and alone actors of the conductor).
TLiveActor* findActorOfType(u32 type)
{
	if (gpConductor == nullptr || type == 0)
		return nullptr;
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin();
	     it != gpConductor->unk10.end(); ++it) {
		TLiveManager* mgr = *it;
		if (mgr == nullptr)
			continue;
		for (int i = 0; i < mgr->getObjNum(); ++i) {
			TLiveActor* a = mgr->getObj(i);
			if (a != nullptr && (u32)a->getActorType() == type && a->getMActor() != nullptr
			    && a->getMActor()->getModel() != nullptr)
				return a;
		}
	}
	for (JGadget::TList<TLiveActor*>::iterator it = gpConductor->unk30.begin();
	     it != gpConductor->unk30.end(); ++it) {
		TLiveActor* a = *it;
		if (a != nullptr && (u32)a->getActorType() == type && a->getMActor() != nullptr
		    && a->getMActor()->getModel() != nullptr)
			return a;
	}
	return nullptr;
}

// The puppet's copy of a held object of that type (built once per type and stage).
PuppetModel::Held* puppetHeld(PuppetModel* p, u32 type)
{
	for (int i = 0; i < p->heldNum; ++i)
		if (p->held[i].type == type)
			return &p->held[i];
	if (p->heldNum >= 4)
		return nullptr;
	PuppetModel::Held& h = p->held[p->heldNum++];
	h.type               = type;
	h.model              = nullptr;
	h.scale[0] = h.scale[1] = h.scale[2] = 1.0f;
	TLiveActor* a = findActorOfType(type);
	if (a == nullptr) {
		OSReport("[mp] held object %08x: no local actor of that type\n", type);
		return &h;
	}
	StageHeapScope scope;
	JKRHeap* heap = JKRGetCurrentHeap();
	void* mem     = heap != nullptr && heap->getFreeSize() > 0x40000
	                    ? JKRHeap::alloc(sizeof(J3DModel), 0x10, heap)
	                    : nullptr;
	if (mem == nullptr)
		return &h;
	h.model = new (mem) J3DModel(a->getMActor()->getModel()->getModelData(), 0, 1);
	for (int k = 0; k < 3; ++k)
		h.scale[k] = (&a->getScaling().x)[k];
	return &h;
}

bool showYoshi(const PortMpPuppetState& s)
{
	return (s.flags & PMS_ON_YOSHI) != 0 && s.yoshiBck >= 0;
}

// TYoshi's body colours (Yoshi.cpp bodyColor), by TYoshi::Color.
const GXColor kYoshiBody[4] = {
	{ 0x40, 0xA1, 0x24, 0xFF },
	{ 0xFF, 0x8C, 0x1C, 0xFF },
	{ 0xAA, 0x4C, 0xFF, 0xFF },
	{ 0xFF, 0xA0, 0xBE, 0xFF },
};

void yoshiSetColor(PuppetModel* p, u8 color)
{
	if (p->yoshiColor == color || p->yoshi == nullptr)
		return;
	p->yoshiColor = color;
	const GXColor c = kYoshiBody[color & 3];
	J3DGXColorS10 tev;
	tev.color.r = c.r, tev.color.g = c.g, tev.color.b = c.b, tev.color.a = 0xFF;
	J3DModelData* d = p->yoshi->getModel()->getModelData();
	for (u16 i = 0; i < d->getMaterialNum(); ++i)
		d->getMaterialNodePointer(i)->getTevBlock()->setTevColor(2, tev);
	for (int i = 0; i < 2; ++i)
		if (p->yoshiHands[i] != nullptr)
			p->yoshiHands[i]->getModelData()->getMaterialNodePointer(0)->getTevBlock()->setTevColor(
			    2, tev);
}

} // namespace

// ---------------------------------------------------------------------------
// 3. slots, buffering, interpolation

namespace {

const int kBuf = 48;

struct Slot {
	bool active;
	bool logicClock; // ghost playback: timestamps on the local logic clock
	bool fromNet;    // fed from w10's session (port_mp_get_snapshots)
	u32 serial;      // w10 player serial (a change = another player)
	u32 lastTick;    // newest w10 tick taken
	char name[32];
	u32 rgb;
	struct Entry {
		double t;
		PortMpPuppetState s;
	} buf[kBuf];
	int count; // entries, oldest first
	u32 lastFrame;
	bool haveFrame;
	double lastPush;
	PuppetModel* model; // this stage's model (stage heap), null until built
	u8 modelStageSeq;
	bool buildFailed;
	bool culled;        // too far from the camera this logic frame
	PortMpCharacter pal; // colours (w10's player table / ghost knob)
	u32 palHash;         // portMpPaletteHash(pal)
	u32 modelPalHash;    // the colours the current model was built with
	u8 palRebuilds;      // model rebuilds for colour changes this stage (bounded)
	Pose pose;
};

Slot sSlots[PORT_MP_MAX_PUPPETS];
bool sEnabled;     // port_mp_puppets_enable / env
bool sEnvChecked;
int sActiveCount;
const double kDelayMs = 100.0;
const double kTimeoutMs = 3000.0;

double logicMs() { return sLogicFrame * (1000.0 / 30.0); }

void checkEnv()
{
	if (sEnvChecked)
		return;
	sEnvChecked = true;
	if (envStr("SMS_PORT_MP_GHOST_PLAY") != nullptr || envStr("SMS_PORT_MP_PUPPETS") != nullptr)
		sEnabled = true;
}

bool wanted()
{
	checkEnv();
	return sEnabled || sActiveCount > 0 || port_mp_active();
}

void pushEntry(Slot& sl, double t, const PortMpPuppetState& s)
{
	if (sl.haveFrame && sl.count > 0 && (s32)(s.seq - sl.lastFrame) <= 0
	    && s.stageSeq == sl.buf[sl.count - 1].s.stageSeq)
		return; // duplicate or late
	if (sl.count == kBuf) {
		memmove(&sl.buf[0], &sl.buf[1], sizeof(sl.buf[0]) * (kBuf - 1));
		--sl.count;
	}
	sl.buf[sl.count].t = t;
	sl.buf[sl.count].s = s;
	++sl.count;
	sl.lastFrame = s.seq;
	sl.haveFrame = true;
	sl.lastPush  = t;
}

// Interpolated pose at render time rt.
void samplePose(Slot& sl, double rt, Pose& out)
{
	out.valid    = false;
	out.havePlay = false;
	out.seqA = out.seqB = 0;
	out.lerpT           = 0.0f;
	if (sl.count == 0)
		return;
	int i = 0;
	while (i < sl.count && sl.buf[i].t <= rt)
		++i;
	// drop entries we will never need again (keep one before rt)
	if (i >= 2) {
		int drop = i - 1;
		memmove(&sl.buf[0], &sl.buf[drop], sizeof(sl.buf[0]) * (sl.count - drop));
		sl.count -= drop;
		i -= drop;
	}
	if (i == 0) { // all in the future: hold the oldest
		out.s     = sl.buf[0].s;
		out.valid = true;
		out.seqA = out.seqB = out.s.seq;
		return;
	}
	out.havePlay = true;
	out.playSeq  = sl.buf[i - 1].s.seq;
	if (i >= sl.count) { // past the newest: hold it
		out.s     = sl.buf[sl.count - 1].s;
		out.valid = true;
		out.seqA = out.seqB = out.s.seq;
		return;
	}
	const PortMpPuppetState& a = sl.buf[i - 1].s;
	const PortMpPuppetState& b = sl.buf[i].s;
	const double span       = sl.buf[i].t - sl.buf[i - 1].t;
	f32 t                   = span > 0.0 ? (f32)((rt - sl.buf[i - 1].t) / span) : 1.0f;
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	out.seqA  = a.seq;
	out.seqB  = b.seq;
	out.lerpT = t;
	// Teleport, stage change or a gap: no blending.
	if (a.stageSeq != b.stageSeq || a.warpSeq != b.warpSeq || a.stage != b.stage
	    || a.scenario != b.scenario || span > 500.0) {
		out.s     = t < 1.0f ? a : b;
		out.valid = true;
		return;
	}
	PortMpPuppetState s = t < 0.5f ? a : b; // discrete fields from the nearer one
	for (int k = 0; k < 3; ++k) {
		s.pos[k] = lerpf(a.pos[k], b.pos[k], t);
		s.vel[k] = lerpf(a.vel[k], b.vel[k], t);
		s.emitPos[k] = lerpf(a.emitPos[k], b.emitPos[k], t);
		s.emitDir[k] = lerpf(a.emitDir[k], b.emitDir[k], t);
	}
	lerpMtx(a.baseMtx, b.baseMtx, t, s.baseMtx);
	for (int slot = 0; slot < 2; ++slot) {
		if (a.bck[slot][0] == b.bck[slot][0] && a.calc[slot] == b.calc[slot])
			s.frame[slot] = lerpFrame(a.frame[slot], b.frame[slot], t);
		s.blend[slot] = lerpf(a.blend[slot], b.blend[slot], t);
	}
	if (a.btp == b.btp)
		s.frame[2] = lerpFrame(a.frame[2], b.frame[2], t);
	if (a.fluddBck == b.fluddBck)
		s.fluddFrame = lerpFrame(a.fluddFrame, b.fluddFrame, t);
	if (a.nozzleBck == b.nozzleBck && a.nozzle == b.nozzle)
		s.nozzleFrame = lerpFrame(a.nozzleFrame, b.nozzleFrame, t);
	if (a.yoshiBck == b.yoshiBck)
		s.yoshiFrame = lerpFrame(a.yoshiFrame, b.yoshiFrame, t);
	for (int k = 0; k < 4; ++k)
		s.headRot[k] = lerpAngle(a.headRot[k], b.headRot[k], t);
	for (int k = 0; k < 3; ++k)
		s.waistRot[k] = lerpAngle(a.waistRot[k], b.waistRot[k], t);
	for (int k = 0; k < 3; ++k)
		s.faceAngle[k] = lerpAngle(a.faceAngle[k], b.faceAngle[k], t);
	s.modelFaceAngle = lerpAngle(a.modelFaceAngle, b.modelFaceAngle, t);
	out.s     = s;
	out.valid = true;
}

bool sameStage(const PortMpPuppetState& s)
{
	TMarDirector* d = gpMarDirector;
	return d != nullptr && s.stage == d->mMap && s.scenario == d->unk7D;
}

} // namespace

namespace {

Slot& activate(int slot, bool logicClock, bool fromNet)
{
	Slot& sl = sSlots[slot];
	if (!sl.active || sl.logicClock != logicClock || sl.fromNet != fromNet) {
		if (!sl.active)
			++sActiveCount;
		sl.active     = true;
		sl.count      = 0;
		sl.haveFrame  = false;
		sl.logicClock = logicClock;
		sl.fromNet    = fromNet;
		sl.lastTick   = 0;
		if (sl.rgb == 0)
			sl.rgb = 0xFFFFFF;
	}
	return sl;
}

} // namespace

extern "C" void port_mp_puppet_push(int slot, const PortMpPuppetState* snap, double tMs)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PUPPETS || snap == nullptr
	    || snap->version != PORT_MP_PUPPET_STATE_VERSION || snap->size != sizeof(PortMpPuppetState))
		return;
	pushEntry(activate(slot, false, false), tMs, *snap);
}

extern "C" void port_mp_puppet_set_name(int slot, const char* name)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PUPPETS)
		return;
	snprintf(sSlots[slot].name, sizeof(sSlots[slot].name), "%s", name ? name : "");
}

extern "C" void port_mp_puppet_set_color(int slot, uint32_t rgb)
{
	if (slot >= 0 && slot < PORT_MP_MAX_PUPPETS)
		sSlots[slot].rgb = rgb & 0xFFFFFF;
}

extern "C" void port_mp_puppet_remove(int slot)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PUPPETS || !sSlots[slot].active)
		return;
	sSlots[slot].active = false;
	sSlots[slot].count  = 0;
	--sActiveCount;
	port_mp_water_clear(slot);
}

extern "C" void port_mp_puppet_remove_all(void)
{
	for (int i = 0; i < PORT_MP_MAX_PUPPETS; ++i)
		port_mp_puppet_remove(i);
}

extern "C" int port_mp_puppet_count(void) { return sActiveCount; }

extern "C" void port_mp_puppets_enable(int on)
{
	checkEnv();
	sEnabled = on != 0;
}

// ---------------------------------------------------------------------------
// 3b. pose-dump harness (SMS_PORT_MP_POSE_DUMP=<file>): the joint matrices of the local
// Mario after each logic frame's capture (kind 0, keyed by the snapshot's seq) and of every
// puppet after its calc (kind 1, with the two snapshots it was interpolated between), plus
// the ghost playback's map from local push frame to recorded frame (kind 2). A recording run
// and a playback run of the same ghost give two files; port/tools/mp_pose_compare.py pairs
// the puppet's frames with the recorded Mario's and lists where the pose differs
// (MULTIPLAYER.md, "Puppets", pose fidelity).

namespace {

#pragma pack(push, 1)
struct PoseRec {
	u8 kind, slot;          // 0 local Mario, 1 puppet, 2 ghost map (seqA = local, seqB = recorded)
	u16 nBody, nFludd, nNozzle; // matrices following the header, 12 floats each
	u32 seqA, seqB;
	f32 t;
	u32 status, flags;
	u16 animId;
	u16 bck[2][2];
	u8 calc[2], hand, btp;
	f32 frame[3], blend[2];
	s16 head[4], waist[3], pad;
	f32 pos[3];
	u8 nozzle, pad1;
	s16 fluddBck, nozzleBck;
	f32 fluddFrame, nozzleFrame;
};
#pragma pack(pop)
static_assert(sizeof(PoseRec) == 104, "pose record");

FILE* sPoseDump;
bool sPoseDumpChecked;
u8 sPoseTicks; // kind 0: simulation ticks of this logic frame (PoseRec::pad1)

FILE* poseDumpFile()
{
	if (!sPoseDumpChecked) {
		sPoseDumpChecked = true;
		if (const char* path = envStr("SMS_PORT_MP_POSE_DUMP")) {
			sPoseDump = fopen(path, "wb");
			if (sPoseDump != nullptr) {
				fwrite("SMSPOSE1", 1, 8, sPoseDump);
				OSReport("[mp] pose dump to %s\n", path);
			}
		}
	}
	return sPoseDump;
}

void poseRecFromState(PoseRec& r, const PortMpPuppetState& s)
{
	r.status = s.status;
	r.flags  = s.flags;
	r.animId = s.animId;
	memcpy(r.bck, s.bck, sizeof(r.bck));
	r.calc[0] = s.calc[0], r.calc[1] = s.calc[1];
	r.hand = s.hand, r.btp = s.btp;
	memcpy(r.frame, s.frame, sizeof(r.frame));
	memcpy(r.blend, s.blend, sizeof(r.blend));
	memcpy(r.head, s.headRot, sizeof(r.head));
	memcpy(r.waist, s.waistRot, sizeof(r.waist));
	memcpy(r.pos, s.pos, sizeof(r.pos));
	r.nozzle      = s.nozzle;
	r.fluddBck    = s.fluddBck;
	r.nozzleBck   = s.nozzleBck;
	r.fluddFrame  = s.fluddFrame;
	r.nozzleFrame = s.nozzleFrame;
}

void poseDumpMtx(FILE* f, J3DModel* m, u16 n)
{
	for (u16 j = 0; j < n; ++j)
		fwrite(m->getAnmMtx(j), sizeof(f32), 12, f);
}

u16 jointCount(J3DModel* m) { return m != nullptr ? m->getModelData()->getJointNum() : 0; }

void poseDump(u8 kind, u8 slot, const PortMpPuppetState& s, u32 seqA, u32 seqB, f32 t, J3DModel* body,
              J3DModel* fludd, J3DModel* nozzle)
{
	FILE* f = poseDumpFile();
	if (f == nullptr)
		return;
	PoseRec r;
	memset(&r, 0, sizeof(r));
	r.kind = kind, r.slot = slot;
	r.nBody   = jointCount(body);
	r.nFludd  = jointCount(fludd);
	r.nNozzle = jointCount(nozzle);
	r.seqA = seqA, r.seqB = seqB, r.t = t;
	r.pad1 = sPoseTicks;
	poseRecFromState(r, s);
	fwrite(&r, sizeof(r), 1, f);
	poseDumpMtx(f, body, r.nBody);
	poseDumpMtx(f, fludd, r.nFludd);
	poseDumpMtx(f, nozzle, r.nNozzle);
}

// The local Mario this logic frame (his model was calculated in this frame's movement pass).
void poseDumpLocal(const PortMpPuppetState& s)
{
	if (poseDumpFile() == nullptr || gpMarioOriginal == nullptr || gpMarioOriginal->mModel == nullptr)
		return;
	TMario* m       = gpMarioOriginal;
	J3DModel* fludd = nullptr, *nozzle = nullptr;
	static bool sNamesLogged;
	if (!sNamesLogged) { // once: the joint table (index, parent, name) for reading the reports
		sNamesLogged      = true;
		J3DModelData* d   = m->mModel->getModel()->getModelData();
		const int n       = d->getJointNum();
		for (int j = 0; j < n; ++j) {
			int parent     = -1;
			J3DJoint* jnt  = d->getJointNodePointer(j);
			for (int k = 0; k < n && parent < 0; ++k)
				for (J3DNode* c = d->getJointNodePointer(k)->getChild(); c != nullptr; c = c->getYounger())
					if (c == jnt)
						parent = k;
			fprintf(stderr, "[mp] pose dump: joint %2d parent %2d %s\n", j, parent, d->getJointName()->getName(j));
		}
	}
	if ((s.flags & PMS_HAS_FLUDD) && m->mWaterGun != nullptr) {
		if (m->mWaterGun->mFluddModel != nullptr)
			fludd = m->mWaterGun->mFluddModel->getModel();
		if (MActor* nz = m->mWaterGun->getCurrentNozzle()->getMActor())
			nozzle = nz->getModel();
	}
	// kind 0 carries the simulation ticks this logic frame ran (Mario's model, and so his joint
	// springs, are calculated once per tick) in `pad1`
	static int sLastTicks;
	const int ticks = gpMarDirector->mMoveTickCount - sLastTicks;
	sLastTicks      = gpMarDirector->mMoveTickCount;
	sPoseTicks      = (u8)(ticks < 0 ? 0 : (ticks > 255 ? 255 : ticks));
	poseDump(0, 0xFF, s, s.seq, s.seq, 0.0f, m->mModel->getModel(), fludd, nozzle);
	sPoseTicks = 0;
}

void poseDumpMap(u32 localSeq, u32 recordedSeq)
{
	FILE* f = poseDumpFile();
	if (f == nullptr)
		return;
	PoseRec r;
	memset(&r, 0, sizeof(r));
	r.kind = 2, r.slot = 0xFF;
	r.seqA = localSeq, r.seqB = recordedSeq;
	fwrite(&r, sizeof(r), 1, f);
}

} // namespace

// ---------------------------------------------------------------------------
// 4. ghost harness

namespace {

// File: "SMSGHST1", u32 snapshot size, then PortMpPuppetState records.
FILE* sGhostRec;
bool sGhostRecChecked;

struct GhostTrack {
	PortMpPuppetState* snaps;
	int count;
	int next;        // next snapshot to feed
	u32 startFrame;  // local logic frame the current stage's playback started
	int stageStart;  // index of the first snapshot of the stage being played
	bool running;
	u8 playStageSeq;
};
GhostTrack sGhost;
int sGhostCopies = 1;
f32 sGhostOffset[3] = { 150.0f, 0.0f, 0.0f };
bool sGhostLoaded;
bool sGhostWire; // SMS_PORT_MP_GHOST_WIRE=1: through the 128-byte network form

// Ghost copies use the top slots, away from w10's player slots.
int ghostSlot(int copy) { return PORT_MP_MAX_PUPPETS - 1 - copy; }

void ghostLoad()
{
	if (sGhostLoaded)
		return;
	sGhostLoaded     = true;
	const char* path = envStr("SMS_PORT_MP_GHOST_PLAY");
	if (path == nullptr)
		return;
	FILE* f = fopen(path, "rb");
	if (f == nullptr) {
		OSReport("[mp] ghost: cannot open %s\n", path);
		return;
	}
	char magic[8];
	u32 sz = 0;
	if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "SMSGHST1", 8) != 0 || fread(&sz, 4, 1, f) != 1
	    || sz != sizeof(PortMpPuppetState)) {
		OSReport("[mp] ghost: %s is not a v%d ghost file\n", path, PORT_MP_PUPPET_STATE_VERSION);
		fclose(f);
		return;
	}
	fseek(f, 0, SEEK_END);
	long n = (ftell(f) - 12) / (long)sz;
	fseek(f, 12, SEEK_SET);
	sGhost.snaps = (PortMpPuppetState*)malloc(sizeof(PortMpPuppetState) * (n > 0 ? n : 1));
	sGhost.count = (int)fread(sGhost.snaps, sz, n, f);
	fclose(f);
	if (const char* c = envStr("SMS_PORT_MP_GHOST_COPIES"))
		sGhostCopies = atoi(c) < 1 ? 1 : (atoi(c) > PORT_MP_MAX_PUPPETS ? PORT_MP_MAX_PUPPETS : atoi(c));
	if (const char* o = envStr("SMS_PORT_MP_GHOST_OFFSET"))
		sscanf(o, "%f,%f,%f", &sGhostOffset[0], &sGhostOffset[1], &sGhostOffset[2]);
	sGhostWire = envStr("SMS_PORT_MP_GHOST_WIRE") != nullptr;
	// SMS_PORT_MP_GHOST_PALETTE=<preset>[,<preset>...]: colours of copy 1, 2, ... (the
	// last one repeats); preset indices as port_mp_palette_preset.
	if (const char* pl = envStr("SMS_PORT_MP_GHOST_PALETTE")) {
		int preset = 0;
		for (int c = 0; c < sGhostCopies; ++c) {
			if (*pl != '\0') {
				preset = atoi(pl);
				while (*pl != '\0' && *pl != ',')
					++pl;
				if (*pl == ',')
					++pl;
			}
			Slot& sl = sSlots[ghostSlot(c)];
			port_mp_palette_preset(preset, &sl.pal);
			sl.palHash = portMpPaletteHash(&sl.pal);
		}
	}
	OSReport("[mp] ghost: %d snapshots from %s, %d cop%s, offset %.0f %.0f %.0f\n", sGhost.count,
	         path, sGhostCopies, sGhostCopies == 1 ? "y" : "ies", sGhostOffset[0], sGhostOffset[1],
	         sGhostOffset[2]);
	for (int c = 0; c < sGhostCopies; ++c) {
		char nm[32];
		snprintf(nm, sizeof(nm), sGhostCopies == 1 ? "Ghost" : "Ghost %d", c + 1);
		port_mp_puppet_set_name(ghostSlot(c), nm);
		static const u32 kColors[] = { 0x60C0FF, 0xFF8060, 0x80FF80, 0xFFE060, 0xE080FF };
		port_mp_puppet_set_color(ghostSlot(c), kColors[c % 5]);
	}
}

void ghostRecord(const PortMpPuppetState& s)
{
	if (!sGhostRecChecked) {
		sGhostRecChecked = true;
		if (const char* path = envStr("SMS_PORT_MP_GHOST")) {
			sGhostRec = fopen(path, "wb");
			if (sGhostRec != nullptr) {
				fwrite("SMSGHST1", 1, 8, sGhostRec);
				u32 sz = sizeof(PortMpPuppetState);
				fwrite(&sz, 4, 1, sGhostRec);
				OSReport("[mp] ghost: recording to %s\n", path);
			}
		}
	}
	if (sGhostRec != nullptr) {
		fwrite(&s, sizeof(s), 1, sGhostRec);
		if ((s.seq & 31) == 0)
			fflush(sGhostRec);
	}
}

// Feed the recorded snapshots of the local stage into the ghost slots on the local
// logic clock: the recording's frame f plays at (stage start + f - first frame).
void ghostPlay(const PortMpPuppetState* local)
{
	ghostLoad();
	if (sGhost.count == 0 || local == nullptr)
		return;
	TMarDirector* d = gpMarDirector;
	if (d == nullptr)
		return;
	// (Re)start on a new stage once the local player has control.
	if (!sGhost.running || sGhost.playStageSeq != sStageSeq) {
		if (d->mState != TMarDirector::STATE_UNK4)
			return;
		int k = -1;
		for (int i = 0; i < sGhost.count; ++i) {
			const PortMpPuppetState& g = sGhost.snaps[i];
			if (g.stage == d->mMap && g.scenario == d->unk7D && !(g.flags & PMS_IN_DEMO)) {
				k = i;
				break;
			}
		}
		if (k < 0)
			return;
		sGhost.running      = true;
		sGhost.playStageSeq = sStageSeq;
		sGhost.stageStart   = k;
		sGhost.next         = k;
		sGhost.startFrame   = sLogicFrame;
		for (int c = 0; c < sGhostCopies; ++c)
			sSlots[ghostSlot(c)].count = 0;
		OSReport("[mp] ghost: playing from snapshot %d (stage %d/%d) at logic frame %u\n", k,
		         d->mMap, d->unk7D, sLogicFrame);
	}
	const u32 base = sGhost.snaps[sGhost.stageStart].seq;
	while (sGhost.next < sGhost.count) {
		const PortMpPuppetState& g = sGhost.snaps[sGhost.next];
		if (g.seq - base > sLogicFrame - sGhost.startFrame)
			break;
		for (int c = 0; c < sGhostCopies; ++c) {
			PortMpPuppetState s = g;
			s.seq            = sLogicFrame;
			s.stageSeq       = sStageSeq; // the ghost lives in this stage
			const f32 off[3] = { sGhostOffset[0] * (c + 1), sGhostOffset[1] * (c + 1),
				                 sGhostOffset[2] * (c + 1) };
			for (int k = 0; k < 3; ++k) {
				s.pos[k] += off[k];
				s.baseMtx[k][3] += off[k];
				s.emitPos[k] += off[k];
			}
			if (sGhostWire) { // what a remote machine rebuilds from the packet
				PortMpSnapshot w;
				port_mp_pack_wire(&s, &w);
				if (!port_mp_unpack_wire(&w, &s))
					continue;
			}
			pushEntry(activate(ghostSlot(c), true, false), logicMs(), s);
			port_mp_water_ghost_feed(g.seq, s.seq, ghostSlot(c));
			if (c == 0)
				poseDumpMap(s.seq, g.seq);
		}
		++sGhost.next;
	}
}

} // namespace

// ---------------------------------------------------------------------------
// 4b. w10 session: send the local state, pull every remote player's snapshots

namespace {

void netExchange(const PortMpPuppetState* local)
{
	// Send: the packed local state, or "no Mario" (VALID clear) outside gameplay so
	// the others hide our puppet.
	PortMpSnapshot w;
	if (local != nullptr) {
		port_mp_pack_wire(local, &w);
	} else {
		memset(&w, 0, sizeof(w));
		w.tick  = sLogicFrame;
		w.stage = w.scenario = 0xFF;
	}
	port_mp_set_local_snapshot(&w);

	// Receive.
	const int me = port_mp_local_slot();
	for (int slot = 0; slot < PORT_MP_MAX_PUPPETS && slot < PORT_MP_MAX_PLAYERS; ++slot) {
		Slot& sl = sSlots[slot];
		PortMpPlayerInfo info;
		if (slot == me || !port_mp_get_player(slot, &info) || !info.used || info.isLocal) {
			if (sl.active && sl.fromNet)
				port_mp_puppet_remove(slot);
			continue;
		}
		if (!sl.active || !sl.fromNet || sl.serial != info.serial) {
			if (sl.active)
				port_mp_puppet_remove(slot); // another player: despawn the old puppet
			activate(slot, false, true);
			sl.serial = info.serial;
		}
		snprintf(sl.name, sizeof(sl.name), "%s", info.name);
		sl.rgb     = port_mp_tag_rgb(slot) & 0xFFFFFF; // the name tag's colour (Customize)
		sl.pal     = info.character;
		sl.palHash = portMpPaletteHash(&sl.pal);
		PortMpSnapshotRx rx[8];
		const int n = port_mp_get_snapshots(slot, rx, 8);
		for (int i = n - 1; i >= 0; --i) { // oldest first
			if (sl.haveFrame && (s32)(rx[i].snap.tick - sl.lastTick) <= 0)
				continue;
			sl.lastTick = rx[i].snap.tick;
			PortMpPuppetState st;
			if (!port_mp_unpack_wire(&rx[i].snap, &st)) {
				// Not in a stage: keep the slot, draw nothing.
				sl.count     = 0;
				sl.haveFrame = true;
				sl.lastPush  = rx[i].rxMs;
				continue;
			}
			pushEntry(sl, rx[i].rxMs, st);
		}
	}
}

} // namespace

// ---------------------------------------------------------------------------
// 5. per-frame hook

// Debug only (test recordings of a Yoshi ride): SMS_PORT_MP_DEBUG_YOSHI=<logic frame>
// drops the local Mario 150 units above the hatched, unridden Yoshi once per stage, so
// he lands on it and mounts the normal way (pair with SMS_PORT_DEBUG_CALL=hatch@...).
static void debugDropOnYoshi()
{
	static const char* sAt = getenv("SMS_PORT_MP_DEBUG_YOSHI");
	static u8 sDoneSeq     = 0xFF;
	if (sAt == nullptr || sDoneSeq == sStageSeq || gpMarioOriginal == nullptr
	    || gpMarioOriginal->mYoshi == nullptr)
		return;
	TYoshi* y = gpMarioOriginal->mYoshi;
	if (sLogicFrame < (u32)atoi(sAt) || y->mState != TYoshi::STATE_UNMOUNTED)
		return;
	sDoneSeq                   = sStageSeq;
	gpMarioOriginal->mPosition = y->getTranslation();
	gpMarioOriginal->mPosition.y += 150.0f;
	OSReport("[mp] debug: Mario dropped onto Yoshi at %.0f %.0f %.0f\n",
	         gpMarioOriginal->mPosition.x, gpMarioOriginal->mPosition.y,
	         gpMarioOriginal->mPosition.z);
}

extern "C" void port_isle_logic_frame(void); // port_isle.cpp (island test, single player too)

extern "C" void port_mp_logic_frame(void)
{
	++sLogicFrame;
	port_isle_logic_frame();
	checkEnv();
	port_mp_palette_local_frame(); // the local Mario's colours (a stage's first frame only)
	debugDropOnYoshi();
	static const bool rec  = envStr("SMS_PORT_MP_GHOST") != nullptr;
	static const bool play = envStr("SMS_PORT_MP_GHOST_PLAY") != nullptr;
	const bool net         = port_mp_active() != 0;
	if (!net) // the session ended: drop its puppets
		for (int i = 0; i < PORT_MP_MAX_PUPPETS; ++i)
			if (sSlots[i].active && sSlots[i].fromNet)
				port_mp_puppet_remove(i);
	if (!wanted() && !rec) {
		port_mp_water_local_frame(0, 0, nullptr, 0, 0); // stop collecting
		return;
	}

	PortMpPuppetState s;
	const bool captured = port_mp_capture(&s) != 0;
	if (captured) {
		// Teleport: a jump no movement makes in one logic frame (2 ticks).
		JGeometry::TVec3<f32> p(s.pos[0], s.pos[1], s.pos[2]);
		if (sHaveLast && sLast.stageSeq == s.stageSeq) {
			JGeometry::TVec3<f32> dlt = p - sLastPos;
			if (dlt.length() > 600.0f) {
				++sWarpSeq;
				s.warpSeq = sWarpSeq;
			}
		}
		sLastPos    = p;
		sLastStatus = s.status;
		sLast       = s;
		sHaveLast   = true;
		poseDumpLocal(s);
		if (rec)
			ghostRecord(s);
		if (play)
			ghostPlay(&s);
	}

	// The local water of this frame: to the session and/or the ghost recording.
	port_mp_water_local_frame(captured, captured ? s.seq : 0, captured ? s.pos : nullptr, net,
	                          rec);
	if (net) {
		netExchange(captured ? &s : nullptr);
		port_mp_water_receive();
	}

	// Sample every active slot's pose for this frame, and play its water up to there.
	const double nowWall = port_mp_time_ms(), nowLogic = logicMs();
	const TMario* me     = gpMarioOriginal;
	const f32 waterRange = gpModelWaterManager != nullptr ? gpModelWaterManager->unk5E08 : 0.0f;
	for (int i = 0; i < PORT_MP_MAX_PUPPETS; ++i) {
		Slot& sl = sSlots[i];
		if (!sl.active) {
			sl.pose.valid = false;
			continue;
		}
		const double now = sl.logicClock ? nowLogic : nowWall;
		if (now - sl.lastPush > kTimeoutMs) {
			sl.pose.valid = false;
			port_mp_water_replay(i, 0, 0, nullptr, 0);
			continue;
		}
		samplePose(sl, now - kDelayMs, sl.pose);
		// Water the move() of the water manager would drop at once (beyond its range from
		// the local Mario) is not made at all.
		const PortMpPuppetState& ps = sl.pose.s;
		bool show = sl.pose.valid && (ps.flags & PMS_VISIBLE) && sameStage(ps) && me != nullptr;
		if (show) {
			const f32 dx = ps.pos[0] - me->mPosition.x, dz = ps.pos[2] - me->mPosition.z;
			show         = dx * dx + dz * dz < waterRange * waterRange;
		}
		port_mp_water_replay(i, sl.pose.havePlay, sl.pose.playSeq, ps.pos, show);
	}
}

// ---------------------------------------------------------------------------
// 6. drawing (view object in the stage entry list)

namespace {

// Light selection as MActor::setLightData (light set 1, MActor's default type).
s16 puppetLightId(const JGeometry::TVec3<f32>& pos)
{
	if (gpCubeShadow != nullptr && gpCubeShadow->getInCubeNo(pos) != -1)
		return 1;
	if (gpMap == nullptr)
		return 0;
	const TBGCheckData* plane = nullptr;
	gpMap->checkGround(pos.x, pos.y + 80.0f, pos.z, &plane);
	if (plane == nullptr)
		return 0;
	if (plane->isShadow()) {
		f32 groundY = -(pos.x * plane->mNormal.x + pos.z * plane->mNormal.z + plane->mPlaneDistance)
		              / plane->mNormal.y;
		if (groundY + 200.0f > pos.y)
			return plane->getData() < 0 ? 0 : plane->getData();
	}
	return 0;
}

void applyAnim(PuppetModel* p, const PortMpPuppetState& s)
{
	M3UModelMario* mm = p->model;
	J3DAnmTransform** tbl = p->common->unk4;
	for (int slot = 0; slot < 2; ++slot) {
		SomeModelMarioStruct& info = mm->unk24[slot];
		u16 bNew = s.bck[slot][0], bOld = s.bck[slot][1];
		if (bNew >= 199 || tbl[bNew] == nullptr)
			bNew = 0x3e;
		if (bOld >= 199 || tbl[bOld] == nullptr)
			bOld = bNew;
		mm->changeMtxCalcSIAnmBQAnmTransform(slot, 0, bNew);
		info.unk4[1] = bOld;
		info.unk3    = s.calc[slot] <= 1 ? s.calc[slot] : 0xFF;
		J3DFrameCtrl& fc = mm->getFrameCtrl(info.unk8);
		fc.setRate(0.0f); // the frame comes from the snapshot
		f32 f = s.frame[slot];
		if (f < 0.0f)
			f = 0.0f;
		if (f > fc.getEnd())
			f = fc.getEnd();
		fc.setFrame(f);
		p->common->unk18[slot].mMotionBlendRatio = s.blend[slot];
	}
	if (s.btp < 0x18 && p->common->unk8 != nullptr && p->common->unk8[s.btp] != nullptr) {
		if (mm->unk1C->unk0 != s.btp)
			mm->changeAnmTexPattern(0, s.btp);
		J3DFrameCtrl& fc = mm->getFrameCtrl(2);
		fc.setRate(0.0f);
		fc.setFrame(s.frame[2] < fc.getEnd() ? s.frame[2] : 0.0f);
	}
}

// J3DModel::calc of the puppet's body, hands, cap and FLUDD (TMario::calcAnim and
// TWaterGun::perform without their side effects).
void calcPuppet(PuppetModel* p, const Pose& pose)
{
	const PortMpPuppetState& s = pose.s;
	// Mario's model is calculated on every simulation tick (TMario::perform's CUE_MOVE pass;
	// TMarDirector runs kSimTicks of them per logic frame, measured constant with the pose
	// dump), and his joint springs (chest, arms, cap, FLUDD) and frame controls step with each.
	// A puppet calculated once per frame would lag four times as long, so the body, cap and
	// FLUDD are calculated kSimTicks times: the first passes at the intermediate poses (body
	// frame k rate steps back, base matrix interpolated from last frame's), the last at the
	// snapshot's. The springs restart on a snap (warp / stage change).
	const int kSimTicks = 4;
	sCalcPose           = &pose;
	const bool snap     = puppetMtxEffects(p, s);
	if (p->bodyFx != nullptr) {
		sPuppetFinalPass = false; // the feet are aligned on the last tick only (addCallBack)
		for (int k = 1; k < kSimTicks; ++k) {
			PortMpPuppetState mid = s;
			f32 f                 = s.frame[0] - s.rate[0] * (f32)(kSimTicks - k);
			if (f < 0.0f)
				f = 0.0f;
			mid.frame[0] = f;
			applyAnim(p, mid);
			if (p->havePrevBase && !snap)
				lerpMtx(p->prevBase, s.baseMtx, (f32)k / (f32)kSimTicks, p->body->getBaseTRMtx());
			else
				MTXCopy(s.baseMtx, p->body->getBaseTRMtx());
			p->model->perform(CUE_CALC_ANIM, nullptr);
			const int capIdx = (s.flags & PMS_CAP) ? 0 : 1;
			if (p->cap[capIdx] != nullptr) {
				p->cap[capIdx]->setBaseTRMtx(p->body->getAnmMtx(p->jntMHead));
				p->cap[capIdx]->calc();
			}
			if ((s.flags & PMS_HAS_FLUDD) && p->fludd != nullptr)
				calcPuppetFludd(p, s);
		}
		sPuppetFinalPass = true;
	}
	MTXCopy(s.baseMtx, p->prevBase);
	p->havePrevBase = true;
	applyAnim(p, s);
	MTXCopy(s.baseMtx, p->body->getBaseTRMtx());
	p->model->perform(CUE_CALC_ANIM, nullptr); // updateIn (frame step 0), calc, updateOut
	for (int k = 0; k < kSimTicks; ++k) // TWaterGun::movement runs once per tick
		puppetPropStep(p, s);
	port_mp_dance_pose_slot(sCalcSlot, p->body, s.bck[0][0]); // that player's dance (port_mp_dance.cpp)
	// M3UModel::updateIn advanced the eye frame control by its rate (0 here).

	puppetChangeHand(p, s.hand);
	if (p->hands[0][0] != nullptr) {
		for (int i = 0; i < 2; ++i) {
			p->hands[i][0]->setBaseTRMtx(p->body->getAnmMtx(p->jntHandR));
			p->hands[i][1]->setBaseTRMtx(p->body->getAnmMtx(p->jntHandL));
			p->hands[i][0]->calc();
			p->hands[i][1]->calc();
		}
		setAllShapes(p->hand4->getModelData(), (s.flags & PMS_HAND4) != 0);
		p->hand4->setBaseTRMtx(p->body->getAnmMtx(p->jntHandR));
		p->hand4->calc();
	}
	// Shirt (TMario::thinkAloha): shape 10 hidden while wearing the shirt.
	if (p->bodyData->getShapeNum() > 10) {
		if (s.flags & PMS_SHIRT)
			p->bodyData->getShapeNodePointer(10)->onFlag(J3DShpFlag_Visible);
		else
			p->bodyData->getShapeNodePointer(10)->offFlag(J3DShpFlag_Visible);
	}
	const int capIdx = (s.flags & PMS_CAP) ? 0 : 1;
	for (int i = 0; i < 2; ++i) {
		if (p->cap[i] == nullptr)
			continue;
		setAllShapes(p->cap[i]->getModelData(), i == capIdx);
		if (i == capIdx) {
			p->cap[i]->setBaseTRMtx(p->body->getAnmMtx(p->jntMHead));
			p->cap[i]->calc();
		}
	}
	if ((s.flags & PMS_HAS_FLUDD) && p->fludd != nullptr) {
		calcPuppetFludd(p, s);
		if (MActor* nz = puppetNozzle(p, s.nozzle)) {
			nz->getModel()->setBaseTRMtx(p->fludd->getModel()->getAnmMtx(p->fluddNozzleJoint));
			if (s.nozzleBck >= 0) {
				if (!nz->checkCurBckFromIndex(s.nozzleBck))
					nz->setBckFromIndex(s.nozzleBck);
				if (J3DFrameCtrl* fc = nz->getFrameCtrl(ANM_TYPE_BCK)) {
					fc->setRate(0.0f);
					fc->setFrame(s.nozzleFrame);
				}
			}
			nz->calc();
		}
	}
	// Held object: a copy on the right hand (TMario::getTakingMtx), at the scale of the
	// local actor its model came from.
	p->heldNow = nullptr;
	if (s.heldActorType != 0) {
		PuppetModel::Held* h = puppetHeld(p, s.heldActorType);
		if (h != nullptr && h->model != nullptr) {
			Mtx sc, base;
			MTXScale(sc, h->scale[0], h->scale[1], h->scale[2]);
			MTXConcat(p->body->getAnmMtx(p->jntHandR), sc, base);
			h->model->setBaseTRMtx(base);
			h->model->calc();
			p->heldNow = h->model;
		}
	}
	// Diver helmet on jnt_head, sunglasses on M_head (TMario::calcAnim / TMarioCap).
	if (s.flags & PMS_HELMET) {
		if (J3DModel* h = puppetHelmet(p)) {
			h->setBaseTRMtx(p->body->getAnmMtx(p->jntHead));
			h->calc();
		}
	}
	if (s.flags & PMS_GLASSES) {
		if (J3DModel* g = puppetGlasses(p)) {
			g->setBaseTRMtx(p->body->getAnmMtx(p->jntMHead));
			g->calc();
		}
	}
	// Yoshi while mounted: TYoshi::calcAnim puts him on Mario's base matrix.
	if (showYoshi(s)) {
		if (MActor* y = puppetYoshi(p)) {
			yoshiSetColor(p, s.yoshiColor);
			MTXCopy(s.baseMtx, y->getModel()->getBaseTRMtx());
			if (!y->checkCurBckFromIndex(s.yoshiBck))
				y->setBckFromIndex(s.yoshiBck);
			y->setMotionBlendRatioForBck(0.0f);
			if (J3DFrameCtrl* fc = y->getFrameCtrl(ANM_TYPE_BCK)) {
				fc->setRate(0.0f);
				fc->setFrame(s.yoshiFrame);
			}
			const bool open = s.yoshiBck == 10 || s.yoshiBck == 12 || s.yoshiBck == 15;
			J3DModelData* yd = y->getModel()->getModelData();
			for (u16 i = 0; i < 2 && i < yd->getShapeNum(); ++i) {
				if (open)
					yd->getShapeNodePointer(i)->onFlag(J3DShpFlag_Visible);
				else
					yd->getShapeNodePointer(i)->offFlag(J3DShpFlag_Visible);
			}
			y->calc();
			static const int kHandJnt[2] = { 37, 32 };
			for (int i = 0; i < 2; ++i) {
				J3DModel* h = p->yoshiHands[i];
				if (h == nullptr)
					continue;
				setAllShapes(h->getModelData(), open);
				h->setBaseTRMtx(y->getModel()->getAnmMtx(kHandJnt[i]));
				h->calc();
			}
		}
	}
	sCalcPose = nullptr; // the nozzle callbacks read it too
}

void viewCalcPuppet(PuppetModel* p, const PortMpPuppetState& s)
{
	p->body->viewCalc();
	if (p->hands[0][0] != nullptr) {
		for (int i = 0; i < 2; ++i)
			for (int j = 0; j < 2; ++j)
				p->hands[i][j]->viewCalc();
		p->hand4->viewCalc();
	}
	const int capIdx = (s.flags & PMS_CAP) ? 0 : 1;
	if (p->cap[capIdx] != nullptr)
		p->cap[capIdx]->viewCalc();
	if ((s.flags & PMS_HAS_FLUDD) && p->fludd != nullptr) {
		p->fludd->viewCalc();
		if (s.nozzle < 6 && p->nozzles[s.nozzle] != nullptr)
			p->nozzles[s.nozzle]->viewCalc();
	}
	if (p->heldNow != nullptr)
		p->heldNow->viewCalc();
	if ((s.flags & PMS_HELMET) && p->helmet != nullptr)
		p->helmet->viewCalc();
	if ((s.flags & PMS_GLASSES) && p->glasses != nullptr)
		p->glasses->viewCalc();
	if (showYoshi(s) && p->yoshi != nullptr) {
		p->yoshi->viewCalc();
		for (int i = 0; i < 2; ++i)
			if (p->yoshiHands[i] != nullptr)
				p->yoshiHands[i]->viewCalc();
	}
}

void entryPuppet(PuppetModel* p, const PortMpPuppetState& s)
{
	JGeometry::TVec3<f32> pos(s.pos[0], s.pos[1], s.pos[2]);
	const s16 light = puppetLightId(pos);
	TLightWithDBSet* set = gpLightManager != nullptr ? gpLightManager->getLightSet(1) : nullptr;
	if (set != nullptr)
		set->changeLightDrawBuffer(light);
	p->model->perform(CUE_ENTRY, nullptr); // eye pattern in, body entry, out
	if (p->hands[0][0] != nullptr) {
		for (int i = 0; i < 2; ++i)
			for (int j = 0; j < 2; ++j)
				p->hands[i][j]->entry();
		p->hand4->entry();
	}
	const int capIdx = (s.flags & PMS_CAP) ? 0 : 1;
	if (p->cap[capIdx] != nullptr)
		p->cap[capIdx]->entry();
	if ((s.flags & PMS_HAS_FLUDD) && p->fludd != nullptr) {
		p->fludd->entry();
		if (s.nozzle < 6 && p->nozzles[s.nozzle] != nullptr)
			p->nozzles[s.nozzle]->entry();
	}
	if (p->heldNow != nullptr)
		p->heldNow->entry();
	if ((s.flags & PMS_HELMET) && p->helmet != nullptr)
		p->helmet->entry();
	if ((s.flags & PMS_GLASSES) && p->glasses != nullptr)
		p->glasses->entry();
	if (showYoshi(s) && p->yoshi != nullptr) {
		p->yoshi->entry();
		for (int i = 0; i < 2; ++i)
			if (p->yoshiHands[i] != nullptr)
				p->yoshiHands[i]->entry();
	}
	if (set != nullptr)
		set->resetLightDrawBuffer();
}

// ---- name tags

} // namespace
extern "C" int port_mp_hide_hide_tag(int slot); // port_mp_hideseek.cpp
extern "C" int port_mp_freeze_tag_rgb(int slot, u32* rgb); // port_mp_freezetag.cpp
extern "C" int port_mp_cnr_tag_rgb(int slot, u32* rgb, int* jailed); // port_mp_cnr.cpp
extern "C" void port_mp_cnr_project(void* view);                     // port_mp_cnr.cpp (the prison marker)
extern "C" int port_mp_prop_hide_tag(int slot);            // port_mp_prophunt.cpp
extern "C" int port_mp_prop_replaces(int slot);
extern "C" void port_mp_prop_perform(unsigned cue, void* graphics);
extern "C" void port_mp_shinerun_perform(unsigned cue, void* graphics); // port_mp_shinerun.cpp
namespace {

PortMpNameTag sTags[PORT_MP_MAX_PUPPETS + 1]; // + the local Mario's (bubble anchor only)
int sTagCount;

// head: the head joint's world matrix (after calc), so the tag follows jumps and flips.
void addTag(int slot, bool local, u32 rgb, const char* name, MtxPtr head, MtxPtr view)
{
	if (!local && (port_mp_hide_hide_tag(slot) || port_mp_prop_hide_tag(slot)))
		return; // Hide and Seek: a hider's tag is hidden from the seekers
	if (sTagCount >= PORT_MP_MAX_PUPPETS + 1)
		return;
	f32 proj[7], vp[6];
	GXGetProjectionv(proj);
	GXGetViewportv(vp);
	const f32 wx = head[0][3], wy = head[1][3] + 75.0f, wz = head[2][3];
	// Distance to the camera (view space z).
	const f32 vz = view[2][0] * wx + view[2][1] * wy + view[2][2] * wz + view[2][3];
	if (vz > -10.0f)
		return; // behind the camera
	f32 sx, sy, sz;
	GXProject(wx, wy, wz, view, proj, vp, &sx, &sy, &sz);
	if (vp[2] <= 0.0f || vp[3] <= 0.0f)
		return;
	const f32 dist = -vz;
	f32 alpha      = 1.0f;
	if (dist > 2500.0f)
		alpha = 1.0f - (dist - 2500.0f) / 2500.0f;
	if (alpha <= 0.02f)
		return;
	PortMpNameTag& t = sTags[sTagCount++];
	t.x              = (sx - vp[0]) / vp[2];
	t.y              = (sy - vp[1]) / vp[3];
	t.alpha          = alpha;
	t.rgb            = rgb;
	t.slot           = slot;
	t.local          = local ? 1 : 0;
	snprintf(t.name, sizeof(t.name), "%s", name != nullptr && name[0] ? name : "Player");
	if (!local && port_mp_freeze_tag_rgb(slot, &t.rgb)) // Freeze Tag: a frozen player's tag, ice blue
		snprintf(t.name, sizeof(t.name), "%s (frozen)", name != nullptr && name[0] ? name : "Player");
	int jailed = 0;
	if (!local && port_mp_cnr_tag_rgb(slot, &t.rgb, &jailed) && jailed) // Cops and Robbers: cops blue, prisoners grey
		snprintf(t.name, sizeof(t.name), "%s (jailed)", name != nullptr && name[0] ? name : "Player");
}

class TMpPuppetViewObj : public JDrama::TViewObj {
public:
	TMpPuppetViewObj()
	    : JDrama::TViewObj("MP Puppets")
	{
	}
	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
};

alignas(16) unsigned char sViewObjMem[sizeof(TMpPuppetViewObj)];
TMpPuppetViewObj* sViewObj;

bool puppetDrawable(Slot& sl)
{
	if (!sl.active || !sl.pose.valid)
		return false;
	const PortMpPuppetState& s = sl.pose.s;
	if (!sameStage(s) || !(s.flags & PMS_VISIBLE))
		return false;
	if (sl.model != nullptr && sl.modelStageSeq == sStageSeq && sl.modelPalHash != sl.palHash) {
		// The player changed colours: recolour the puppet's own atlas in place (Aurora
		// rebinds it); a model without one is rebuilt (bounded: the old one stays in the
		// stage heap until the stage ends).
		OSReport("[mp] puppet %s: new colours (cap %02x%02x%02x)\n", sl.name, sl.pal.colors[0][0],
		         sl.pal.colors[0][1], sl.pal.colors[0][2]);
		if (sl.model->atlas != nullptr) {
			portMpPaletteRecolorInto(sl.model->atlas, &sl.pal);
			sl.modelPalHash = sl.palHash;
		} else if (sl.palRebuilds < 4) {
			++sl.palRebuilds;
			sl.model = nullptr;
		}
	}
	if (sl.model == nullptr || sl.modelStageSeq != sStageSeq) {
		if (sl.modelStageSeq != sStageSeq)
			sl.palRebuilds = 0;
		sl.model = nullptr;
		if (sl.buildFailed && sl.modelStageSeq == sStageSeq)
			return false;
		sl.modelStageSeq = sStageSeq;
		sl.model         = buildPuppet(&sl.pal);
		sl.modelPalHash  = sl.palHash;
		sl.buildFailed   = sl.model == nullptr;
	}
	return sl.model != nullptr;
}

bool drawnThisFrame(const Slot& sl)
{
	return sl.active && sl.pose.valid && sl.model != nullptr && sl.modelStageSeq == sStageSeq
	       && !sl.culled && sameStage(sl.pose.s) && (sl.pose.s.flags & PMS_VISIBLE);
}

// Puppets farther than this from the camera are not calculated or drawn (a Mario is a
// few pixels tall there); the name tag fades out well before.
const f32 kCullDist = 9000.0f;

bool inRange(const Slot& sl, MtxPtr view)
{
	const PortMpPuppetState& s = sl.pose.s;
	const f32 vz = view[2][0] * s.pos[0] + view[2][1] * s.pos[1] + view[2][2] * s.pos[2] + view[2][3];
	return vz > -kCullDist;
}

void TMpPuppetViewObj::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (gpMarioOriginal == nullptr || gpMarDirector == nullptr)
		return;
	const bool logic = (cue & CUE_ENTRY) != 0; // redraws only repeat CUE_CALC_VIEW
	if (logic) {
		for (int i = 0; i < PORT_MP_MAX_PUPPETS; ++i) {
			Slot& sl = sSlots[i];
			sl.culled = false;
			if (!puppetDrawable(sl))
				continue;
			if (!inRange(sl, graphics->mViewMtx) && sPoseDump == nullptr) { // the dump measures off-screen too
				sl.culled = true;
				continue;
			}
			if (port_mp_prop_replaces(i) == 2) // Prop Hunt disguise: no Mario to pose
				continue;
			sCalcSlot = i;
			calcPuppet(sl.model, sl.pose);
			sCalcSlot = -1;
			if (sPoseDump != nullptr) {
				PuppetModel* p   = sl.model;
				const auto& ps   = sl.pose.s;
				J3DModel* fludd  = (ps.flags & PMS_HAS_FLUDD) && p->fludd != nullptr ? p->fludd->getModel() : nullptr;
				J3DModel* nozzle = fludd != nullptr && ps.nozzle < 6 && p->nozzles[ps.nozzle] != nullptr
				                       ? p->nozzles[ps.nozzle]->getModel()
				                       : nullptr;
				poseDump(1, (u8)i, ps, sl.pose.seqA, sl.pose.seqB, sl.pose.lerpT, p->body, fludd, nozzle);
			}
		}
	}
	if (cue & CUE_CALC_VIEW) {
		MTXCopy(graphics->mViewMtx, j3dSys.mViewMtx);
		sTagCount = 0;
		for (int i = 0; i < PORT_MP_MAX_PUPPETS; ++i) {
			Slot& sl = sSlots[i];
			if (!drawnThisFrame(sl))
				continue;
			if (port_mp_prop_replaces(i) != 0) // Prop Hunt hider: port_mp_prop_perform draws him
				continue;
			viewCalcPuppet(sl.model, sl.pose.s);
			// Circle shadow under the puppet (TLiveActor::requestShadow style).
			if (gpBindShadowManager != nullptr) {
				TCircleShadowRequest req;
				req.mPosition.set(sl.pose.s.pos[0], sl.pose.s.pos[1], sl.pose.s.pos[2]);
				req.mRadiusX = req.mRadiusZ = 45.0f;
				req.mRotationY             = 0.0f;
				gpBindShadowManager->request(req, 0);
			}
			addTag(i, false, sl.rgb, sl.name, sl.model->body->getAnmMtx(sl.model->jntHead),
			       graphics->mViewMtx);
		}
		// The local Mario's anchor (no name tag drawn; speech bubbles use it).
		TMario* lm = gpMarioOriginal;
		if (port_mp_active() && lm->mModel != nullptr && (lm->unk114 & TMario::UNK114_FLAG_VISIBLE)
		    && !lm->checkFlag(MARIO_FLAG_UNK4))
			addTag(port_mp_local_slot(), true, 0xFFFFFF, "",
			       lm->mModel->getModel()->getAnmMtx(lm->mJointIdHead), graphics->mViewMtx);
		if (logic) {
			port_mp_overlay_set_tags(sTags, sTagCount);
			port_mp_cnr_project(graphics->mViewMtx);
		}
	}
	if (logic) {
		for (int i = 0; i < PORT_MP_MAX_PUPPETS; ++i) {
			Slot& sl = sSlots[i];
			if (!drawnThisFrame(sl) || port_mp_prop_replaces(i) != 0)
				continue;
			entryPuppet(sl.model, sl.pose.s);
		}
	}
	port_mp_prop_perform(cue, graphics); // Prop Hunt: hiders as Shadow Mario or their disguise
	port_mp_shinerun_perform(cue, graphics); // Shine Runners: knocked-out Shines bouncing around
}

} // namespace

// port_mp_prophunt.cpp: a player's posed body this frame (J3DModel*), and where he is drawn.
extern "C" void* port_mp_puppet_body(int slot)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PUPPETS)
		return nullptr;
	Slot& sl = sSlots[slot];
	return drawnThisFrame(sl) ? sl.model->body : nullptr;
}
extern "C" int port_mp_puppet_pos(int slot, float pos[3])
{
	if (slot < 0 || slot >= PORT_MP_MAX_PUPPETS)
		return 0;
	const Slot& sl = sSlots[slot];
	if (!sl.active || !sl.pose.valid || !sameStage(sl.pose.s))
		return 0;
	pos[0] = sl.pose.s.pos[0];
	pos[1] = sl.pose.s.pos[1];
	pos[2] = sl.pose.s.pos[2];
	return 1;
}

extern "C" void* port_mp_preentry_viewobj(void)
{
	++sStageSeq; // a new stage: every puppet model is rebuilt from its heap
	sHaveLast = false;
	for (int i = 0; i < PORT_MP_MAX_PUPPETS; ++i) {
		sSlots[i].model       = nullptr;
		sSlots[i].buildFailed = false;
	}
	sGhost.running = false;
	if (!wanted())
		return nullptr;
	if (sViewObj == nullptr)
		sViewObj = new (sViewObjMem) TMpPuppetViewObj();
	OSReport("[mp] puppets: view object added to the entry list (stage seq %d)\n", sStageSeq);
	return sViewObj;
}
