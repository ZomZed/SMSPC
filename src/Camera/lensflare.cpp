// rogue include needed for matching .rodata (the retail pool starts with the dummy strings)
#include <System/DummyStrings.hpp>
#include <Camera/LensFlare.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JGeometry.hpp>
#include <JSystem/JMath.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <Camera/Camera.hpp>
#include <Camera/CameraMarioData.hpp>
#include <Camera/SunMgr.hpp>
#include <Camera/cameralib.hpp>
#include <Camera/SunModel.hpp>
#include <stdio.h>

#ifdef LINK_ALL_SOURCE
const char* cSunVolumeName    = "/scene/sun";
const char* cSunsetVolumeName = "/scene/sunset";
#endif

static inline void calcDirDeep2(JGeometry::TVec3<f32>& dir, const Vec& pos,
                                f32 lx, f32 ly, f32 lz)
{
	JGeometry::TVec3<f32> finalPos(pos);
	dir.set(lx - finalPos.x, ly - finalPos.y, lz - finalPos.z);
}

static inline void calcDirDeep1(JGeometry::TVec3<f32>& dir, const Vec& pos,
                                f32 lx, f32 ly, f32 lz)
{
	calcDirDeep2(dir, pos, lx, ly, lz);
}

static inline void calcNearNinePosVec2(JGeometry::TVec3<f32>* out_grid,
                                       S16Vec* out_euler,
                                       JGeometry::TVec3<f32> origin,
                                       JGeometry::TVec3<f32> lookat, s16 roll,
                                       f32 near_dist, f32 fovy, f32 aspect)
{
	CLBCalcNearNinePos(out_grid, out_euler, origin, lookat, roll, near_dist,
	                   fovy, aspect);
}

static inline void calcNearNinePosVec(JGeometry::TVec3<f32>* out_grid,
                                      S16Vec* out_euler, const Vec& origin,
                                      const Vec& lookat, s16 roll,
                                      f32 near_dist, f32 fovy, f32 aspect)
{
	calcNearNinePosVec2(out_grid, out_euler, origin, lookat, roll, near_dist,
	                    fovy, aspect);
}

// fabricated: scale/pos are read inside the wrapper, after the angle calls
static inline void setFlareMtx(MtxPtr mtx, const JGeometry::TVec3<f32>& pos,
                               f32 rx, f32 ry,
                               const JGeometry::TVec3<f32>& scale)
{
	MsMtxSetTRS(mtx, pos.x, pos.y, pos.z, rx, ry, 0.0f, scale.x, scale.y,
	            scale.z);
}

TLensFlare::TLensFlare(const char* name)
    : JDrama::TViewObj(name)
    , unk10(nullptr)
    , unk14(nullptr)
    , unk18(60.0f, 60.0f, 80.0f)
    , unk24(0.0f)
    , unk28(0.0f)
    , unk2C(0.04f)
    , unk30(0.005f)
    , unk34(0.04f)
    , unk38(0.04f)
    , unk3C(30000.0f)
    , unk40(3.0f)
    , unk44(1.75f)
    , unk48(75.0f)
{
	if (gpSunMgr->isThing())
		return;

	char buf[0x100];
	snprintf(buf, 0x100, "%s/%s", cSunVolumeName, "sun_lensfx.bmd");

	unk10 = J3DModelLoaderDataBase::load(JKRGetResource(buf),
	                                     J3DMLF_MaterialPEFull
	                                         | (2 << J3DMLF_TevStageNumShift));
	unk14 = new J3DModel(unk10, 0, 1);
}

void TLensFlare::perform(u32 cue, JDrama::TGraphics*)
{
	if (gpSunMgr->isThing())
		return;

	bool sunInBounds;
	if (gpCameraMario->isMarioIndoor()) {
		sunInBounds = false;
	} else {
		sunInBounds = gpSunModel->isInBounds(unk40);
	}

	if (cue & CUE_MOVE) {
		if (!gpSunModel->isInBounds(unk44)) {
			unk28 = 0.0f;
		} else {
			f32 hiddenCount = gpSunModel->calcHiddenRatio();

			unk28 = CLBEaseOutInbetween<f32>(unk48 * (1.0f - hiddenCount),
			                                 255.0f, gpSunModel->getUnk194());
		}

		f32 chase;
		if (unk24 < unk28) {
			if (gpSunModel->unk194 == 0.0f)
				chase = unk30;
			else
				chase = unk2C;
		} else {
			if (gpSunModel->unk194 == 0.0f)
				chase = unk38;
			else
				chase = unk34;
		}
		CLBChaseDecrease(&unk24, unk28, chase, 0.0f);
	}

	if (!sunInBounds)
		return;

	if (cue & CUE_CALC_ANIM) {
		JGeometry::TVec3<f32> sunWorldPos = gpSunModel->unk198;

		// TODO: a mystery is happening here with the args, but it's definitely
		// this inline (maybe one more inlining layer?)
		JGeometry::TVec3<f32> near9grid[9];
		S16Vec euler;
		calcNearNinePosVec(near9grid, &euler, gpCamera->unk124,
		                   gpCamera->unk148, gpCamera->getFinalAngleZ(),
		                   gpCamera->getNear(), gpCamera->getFovy(),
		                   gpCamera->getAspect());

		const JGeometry::TVec2<f32>& sunScreen = gpSunModel->unkF8[0];
		f32 tx = unk3C * -sunScreen.x;
		f32 ty = unk3C * -sunScreen.y;
		f32 dx1 = (near9grid[5].x - near9grid[4].x) * tx;
		f32 dy1 = (near9grid[5].y - near9grid[4].y) * tx;
		f32 dz1 = (near9grid[5].z - near9grid[4].z) * tx;
		f32 dx2 = (near9grid[1].x - near9grid[4].x) * ty;
		f32 dy2 = (near9grid[1].y - near9grid[4].y) * ty;
		f32 dz2 = (near9grid[1].z - near9grid[4].z) * ty;
		f32 lx  = near9grid[4].x + dx1 + dx2;
		f32 ly  = near9grid[4].y + dy1 + dy2;
		f32 lz  = near9grid[4].z + dz1 + dz2;

		JGeometry::TVec3<f32> dir;
		calcDirDeep1(dir, sunWorldPos, lx, ly, lz);

		JGeometry::TVec3<f32> rot = MsGetRotFromZaxis(dir);
		Mtx mtx;
		s16 rx = CLBDegToShortAngle(rot.x);
		s16 ry = CLBDegToShortAngle(rot.y);
		setFlareMtx(mtx, sunWorldPos, rx * (360.0f / 65536.0f),
		            ry * (360.0f / 65536.0f), unk18);
		unk14->setBaseTRMtx(mtx);
		unk14->calc();
	}

	if (cue & CUE_ENTRY) {
		int matCount = unk10->getMaterialNum();
		for (u16 i = 0; i < matCount; ++i) {
			unk10->getMaterialNodePointer(i)->change();
			J3DGXColorS10 c;
			c         = *unk10->getMaterialNodePointer(i)->getTevColor(0);
			c.color.a = unk24;
			unk10->getMaterialNodePointer(i)->setTevColor(0, &c);
		}
		unk14->entry();
	}

	if (cue & CUE_CALC_VIEW)
		unk14->viewCalc();
}
