#include <JSystem/JDrama/JDRCamera.hpp>
#include <dolphin/mtx.h>
#include <dolphin/gx.h>
#include <macros.h>
#ifdef SMS_PORT
#include <port_widescreen.h> // w11: widescreen (Hor+)
#endif

using namespace JDrama;

TCamera::~TCamera() { }
u32 TCamera::JSGGetFlag() const { return mFlag.get(); }
void TCamera::JSGSetFlag(u32 flag) { mFlag.set(flag); }
float TCamera::JSGGetProjectionNear() const { return mNear; }
void TCamera::JSGSetProjectionNear(float near) { mNear = near; }
float TCamera::JSGGetProjectionFar() const { return mFar; }
void TCamera::JSGSetProjectionFar(float far) { mFar = far; }

void TPolarCamera::load(JSUMemoryInputStream& stream)
{
	TPlacement::load(stream);
	unk3C = stream.readF32();
	unk38 = stream.readF32();
}
// Fabricated: an identity33 whose zero stores are one assignment chain. With
// the matrix scalar-replaced the chain survives as register copies (f31/f30),
// which plain stores (TRotation3::identity33) do not.
static inline void identity33Chain(TPosition3f& m)
{
	m.ref(0, 1) = m.ref(1, 0) = m.ref(2, 0) = m.ref(2, 1) = m.ref(0, 2)
	    = m.ref(1, 2) = 0.0f;
	m.ref(0, 0) = m.ref(1, 1) = m.ref(2, 2) = 1.0f;
}

void TPolarCamera::perform(u32 cue, TGraphics* graphics)
{
	if (!(cue & (CUE_CALC_VIEW | CUE_SET_PROJECTION)))
		return;

	MtxPtr projMtx = graphics->mProjMtx.mMtx;
#ifdef SMS_PORT
	PORTWideCameraAspect(&mAspect); // widescreen: same vertical FOV, wider view
#endif
	C_MTXPerspective(projMtx, mFovy, mAspect, mNear, mFar);
	graphics->mNearPlane = mNear;
	graphics->mFarPlane  = mFar;

	TPosition3f tmp;
	identity33Chain(tmp);
	tmp.ref(0, 3) = 0.0f;
	tmp.ref(1, 3) = 0.0f;
	tmp.ref(2, 3) = -unk44;

	TPosition3f local_D4;
	local_D4.setEularZ(DEG_TO_RAD(-unk40));
	local_D4.setTrans(0.0f, 0.0f, 0.0f);

	TPosition3f local_A4;
	local_A4.concat(tmp, local_D4);

	tmp.setEularY(DEG_TO_RAD(-unk3C));
	tmp.setTrans(0.0f, 0.0f, 0.0f);
	local_D4.concat(tmp, local_A4);

	tmp.setEularX(DEG_TO_RAD(unk38));
	tmp.setTrans(0.0f, 0.0f, 0.0f);
	local_A4.concat(local_D4, tmp);

	MTXCopy(local_A4, graphics->mViewMtx);
	if (cue & CUE_SET_PROJECTION)
		GXSetProjection(projMtx, GX_PERSPECTIVE);
}
JStage::TECameraProjection TPolarCamera::JSGGetProjectionType() const
{
	return JStage::TECAMERAPROJECTION_Unk1;
}
void TPolarCamera::JSGSetProjectionType(JStage::TECameraProjection) { }
float TPolarCamera::JSGGetProjectionFovy() const { return mFovy; }
void TPolarCamera::JSGSetProjectionFovy(float fovy) { mFovy = fovy; }
float TPolarCamera::JSGGetProjectionAspect() const { return mAspect; }
void TPolarCamera::JSGSetProjectionAspect(float aspect) { mAspect = aspect; }

void TLookAtCamera::perform(u32 cue, TGraphics* graphics)
{
	if (!(cue & (CUE_CALC_VIEW | CUE_SET_PROJECTION)))
		return;

	MtxPtr projMtx = graphics->mProjMtx.mMtx;
#ifdef SMS_PORT
	PORTWideCameraAspect(&mAspect); // widescreen: same vertical FOV, wider view
#endif
	C_MTXPerspective(projMtx, mFovy, mAspect, mNear, mFar);
	graphics->mNearPlane = mNear;
	graphics->mFarPlane  = mFar;
	C_MTXLookAt(graphics->mViewMtx, &mPosition, &mUp, &mTarget);

	if (cue & CUE_SET_PROJECTION)
		GXSetProjection(projMtx, GX_PERSPECTIVE);
}
JStage::TECameraProjection TLookAtCamera::JSGGetProjectionType() const
{
	return JStage::TECAMERAPROJECTION_Unk1;
}
void TLookAtCamera::JSGSetProjectionType(JStage::TECameraProjection) { }
float TLookAtCamera::JSGGetProjectionFovy() const { return mFovy; }
void TLookAtCamera::JSGSetProjectionFovy(float fovy) { mFovy = fovy; }
float TLookAtCamera::JSGGetProjectionAspect() const { return mAspect; }
void TLookAtCamera::JSGSetProjectionAspect(float aspect) { mAspect = aspect; }
void TLookAtCamera::JSGGetViewPosition(Vec* pos) const { *pos = mPosition; }
void TLookAtCamera::JSGSetViewPosition(const Vec& pos) { mPosition.set(pos); }
void TLookAtCamera::JSGGetViewUpVector(Vec* up) const { *up = mUp; }
void TLookAtCamera::JSGSetViewUpVector(const Vec& up) { mUp.set(up); }
void TLookAtCamera::JSGGetViewTargetPosition(Vec* tgt) const { *tgt = mTarget; }
void TLookAtCamera::JSGSetViewTargetPosition(const Vec& tgt)
{
	mTarget.set(tgt);
}

void TOrthoProj::load(JSUMemoryInputStream& stream)
{
	TPlacement::load(stream);
	stream >> mField[0] >> mField[1] >> mField[2] >> mField[3];
}
void TOrthoProj::perform(u32 cue, TGraphics* graphics)
{
	if (!(cue & (CUE_CALC_VIEW | CUE_SET_PROJECTION)))
		return;

	MtxPtr projMtx = graphics->mProjMtx.mMtx;
#ifdef SMS_PORT
	// widescreen: the 600-wide 2D/HUD frames ("Group 2D", menus, shine select) keep their
	// 4:3 shape; full-frame orthos (0..render width) and offscreen ones stay as they are.
	f32 left = mField[0], right = mField[2];
	if (right - left == 600.0f)
		PORTWideOrthoX(&left, &right);
	C_MTXOrtho(projMtx, mField[1], mField[3], left, right, mNear, mFar);
#else
	C_MTXOrtho(projMtx, mField[1], mField[3], mField[0], mField[2], mNear,
	           mFar);
#endif
	graphics->mNearPlane = mNear;
	graphics->mFarPlane  = mFar;
	MTXTrans(graphics->mViewMtx, mPosition.x, mPosition.y, mPosition.z);

	if (cue & CUE_SET_PROJECTION)
		GXSetProjection(projMtx, GX_ORTHOGRAPHIC);
}
JStage::TECameraProjection TOrthoProj::JSGGetProjectionType() const
{
	return JStage::TECAMERAPROJECTION_Unk1;
}
void TOrthoProj::JSGSetProjectionType(JStage::TECameraProjection) { }
void TOrthoProj::JSGGetProjectionField(float* field) const
{
	for (int i = 0; i < 4; ++i)
		field[i] = mField[i];
}
void TOrthoProj::JSGSetProjectionField(const float* field)
{
	for (int i = 0; i < 4; ++i)
		mField[i] = field[i];
}
