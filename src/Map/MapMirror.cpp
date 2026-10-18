#include <Map/MapMirror.hpp>
#include <Map/MapData.hpp>
#include <M3DUtil/MActor.hpp>
#include <M3DUtil/MActorData.hpp>
#include <M3DUtil/MActorUtil.hpp>
#include <System/MarDirector.hpp>
#include <Player/MarioAccess.hpp>
#include <Camera/Camera.hpp>
#include <Camera/CubeManagerBase.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DMaterial.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoaderFlags.hpp>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>
#include <M3DUtil/InfectiousStrings.hpp>

typedef JGeometry::TVec3<f32> MMVec;
struct MMPlane {
	MMVec n;
	f32 d;
	MMPlane() { }
};

static inline void MMSet2(MMVec& v, f32 x, f32 y, f32 z) { v.set(x, y, z); }
static inline void MMSet1(MMVec& v, f32 x, f32 y, f32 z) { MMSet2(v, x, y, z); }
static inline f32 MMDot2(const MMVec& a, const MMVec& b) { return a.dot(b); }
static inline f32 MMDot1(const MMVec& a, const MMVec& b) { return MMDot2(a, b); }

static inline void MMInitPlane(MMPlane& pl, const MMVec& n, f32 d)
{
	f32 negD = -d;
	pl.n.set(n.x, n.y, n.z);
	pl.d = negD;
}

static inline void MMReflect(MMVec& out, const MMPlane& pl, const MMVec& v)
{
	f32 f = -2.0f * (MMDot2(pl.n, v) - pl.d);
	out.scaleAdd(f, v, pl.n);
}

// Builds the view matrix of the camera mirrored about the plane in unk84/unk90
inline void TMirrorCamera::makeMirrorViewMtx()
{
	MMPlane pl;
	MMInitPlane(pl, unk84, unk90);

	MMReflect(unk98, pl, gpCamera->unk124);
	MMVec target;
	MMReflect(target, pl, gpCamera->unk148);
	MMVec up;
	MMReflect(up, pl, gpCamera->mUp);

	C_MTXLookAt(unk30, &unk98, &up, &target);
}

// Same as above for the direct call in TMirrorModelManager::perform, where the
// original keeps TVec3::set and dot out of line

static inline void MMInitPlaneA(MMPlane& pl, const MMVec& n, f32 d)
{
	f32 negD = -d;
	MMSet1(pl.n, n.x, n.y, n.z);
	pl.d = negD;
}

static inline void MMReflectA(MMVec& out, const MMPlane& pl, const MMVec& v)
{
	f32 f = -2.0f * (MMDot1(pl.n, v) - pl.d);
	out.scaleAdd(f, v, pl.n);
}

static inline void MMMakeMirrorViewMtxA(TMirrorCamera* cam)
{
	MMPlane pl;
	MMInitPlaneA(pl, cam->unk84, cam->unk90);

	MMReflectA(cam->unk98, pl, gpCamera->unk124);
	MMVec target;
	MMReflectA(target, pl, gpCamera->unk148);
	MMVec up;
	MMReflectA(up, pl, gpCamera->mUp);

	C_MTXLookAt(cam->unk30, &cam->unk98, &up, &target);
}

void TMirrorCamera::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (cue & (CUE_CALC_VIEW | CUE_SET_PROJECTION)) {
		Mtx44Ptr proj = graphics->mProjMtx.mMtx;
		f32 far = gpCamera->mFar;
		f32 near = gpCamera->mNear;
		C_MTXPerspective(proj, gpCamera->mFovy * unk80, gpCamera->mAspect, near, far);
		MTXCopy(unk30, graphics->mViewMtx);
		graphics->mNearPlane = gpCamera->mNear;
		graphics->mFarPlane  = gpCamera->mFar;
		if (cue & CUE_SET_PROJECTION)
			GXSetProjection(proj, GX_PERSPECTIVE);
		GXSetAlphaUpdate(GX_TRUE);
	}
}

void TMirrorCamera::drawSetting(MtxPtr param_1)
{
	GXLoadTexObj(&unk60, GX_TEXMAP0);
	Mtx afStack_38;
	C_MTXLightPerspective(afStack_38, unk80 * gpCamera->getFovy(),
	                      gpCamera->getAspect(), 0.5f, -0.5f, 0.5f, 0.5f);

	Mtx afStack_68;
	MTXConcat(getUnk30(), param_1, afStack_68);
	Mtx afStack_98;
	MTXConcat(afStack_38, afStack_68, afStack_98);
	GXLoadTexMtxImm(afStack_98, 0x1E, GX_MTX3x4);
}

void TMirrorCamera::calcEffectMtx(MtxPtr) { }

TMirrorCamera::TMirrorCamera(const char* name)
    : JDrama::TCamera(10.0f, 300000.0f, name)
    , unk80(1.3f)
    , unk84(0.0f, 0.0f, 0.0f)
    , unk90(0.0f)
    , unk94(nullptr)
{
	unk94 = (ResTIMG*)new (0x20)
	    u8[GXGetTexBufferSize(0x100, 0x100, 5, 0, 0) + sizeof(ResTIMG)];
	memset(unk94, 0, sizeof(ResTIMG));
	unk94->format          = GX_TF_RGB5A3;
	unk94->alphaEnabled    = true;
	unk94->width           = 0x100;
	unk94->height          = 0x100;
	unk94->minFilter       = 1;
	unk94->magFilter       = 1;
	unk94->mipmapCount     = 1;
	unk94->imageDataOffset = 0x20;

	u32 imgOff   = unk94->imageDataOffset;
	u8 imgFmt    = unk94->format;
	u16 imgH     = unk94->height;
	u16 imgW     = unk94->width;
	GXInitTexObj(&unk60, (u8*)unk94 + imgOff, imgW, imgH, (GXTexFmt)imgFmt,
	             GX_REPEAT, GX_REPEAT, 0);

	GXInitTexObjLOD(&unk60, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE,
	                GX_FALSE, GX_ANISO_1);
	Vec local_20 = (Vec) { 10000.0f, 10000.0f, 10000.0f };
	Vec local_2C = (Vec) { 0.0f, 1.0f, 0.0f };
	Vec local_38 = (Vec) { 20000.0f, 20000.0f, 20000.0f };
	C_MTXLookAt(unk30, &local_20, &local_2C, &local_38);
	unk98.zero();
}

static u8 getVertexFormat(const J3DModelData* model_data, GXAttr attr)
{
	const GXVtxAttrFmtList* list
	    = model_data->getVertexData().getVtxAttrFmtList();
	for (; list->attr != GX_VA_NULL; ++list)
		if (list->attr == attr)
			return list->type;
	return 0xff;
}

void TMirrorModel::setPlane()
{
	MtxPtr mtx = unk4->getModel()->getBaseTRMtx();
	MTXMultVec(mtx, &unkC, &unkC);
	MTXMultVecSR(mtx, &unk18, &unk18);
	VECNormalize(&unk18, &unk18);
	unk24 = -VECDotProduct(&unk18, &unkC);
	unk8->setUnk84AndUnk90(unk18.x, unk18.y, unk18.z, unk24);
}

void TMirrorModel::initPlaneInfo()
{
	u8 posComp = getVertexFormat(unk4->getModel()->getModelData(), GX_VA_POS);

	if (posComp == GX_S16) {
		S16Vec* v = (S16Vec*)unk4->getModel()
		                ->getModelData()
		                ->getVertexData()
		                .getVtxPosArray();
		unkC.x = v->x;
		unkC.y = v->y;
		unkC.z = v->z;
	} else {
		Vec* v = (Vec*)unk4->getModel()
		             ->getModelData()
		             ->getVertexData()
		             .getVtxPosArray();
		unkC.x = v->x;
		unkC.y = v->y;
		unkC.z = v->z;
	}

	u8 normComp = getVertexFormat(unk4->getModel()->getModelData(), GX_VA_NRM);

	if (normComp == GX_S16) {
		S16Vec* v = (S16Vec*)unk4->getModel()
		                ->getModelData()
		                ->getVertexData()
		                .getVtxNormArray();
		// BUG: probably meant to do a float division here?
		unk18.x = v->x / 16384;
		unk18.y = v->y / 16384;
		unk18.z = v->z / 16384;
	} else if (normComp == GX_F32) {
		Vec* v = (Vec*)unk4->getModel()
		             ->getModelData()
		             ->getVertexData()
		             .getVtxNormArray();
		unk18.x = v->x;
		unk18.y = v->y;
		unk18.z = v->z;
	} else {
		unk18.x = 0.0f;
		unk18.y = 1.0f;
		unk18.z = 0.0f;
	}
}

void TMirrorModel::entry()
{
	Mtx lightMtx;
	C_MTXLightPerspective(lightMtx, unk8->unk80 * gpCamera->mFovy,
	                      gpCamera->mAspect, 0.5f, -0.5f, 0.5f, 0.5f);
	Mtx texMtx;
	MTXConcat(lightMtx, unk8->getUnk30(), texMtx);

	J3DMaterial* material
	    = unk4->getModel()->getModelData()->getMaterialNodePointer(0);
	material->change();
	material->getTexGenBlock()->getTexMtx(0)->setEffectMtx(texMtx);
	unk4->entry();
}

// Same as TMirrorCamera::makeMirrorViewMtx, but for the camera of this mirror
void TMirrorModel::calcView()
{
	unk8->makeMirrorViewMtx();
}

void TMirrorModel::getMirrorTexInfo() { }

inline static void identity34(MtxPtr mtx)
{
	mtx[2][3] = 0.0f;
	mtx[1][3] = 0.0f;
	mtx[0][3] = 0.0f;
	mtx[1][2] = 0.0f;
	mtx[0][2] = 0.0f;
	mtx[2][1] = 0.0f;
	mtx[0][1] = 0.0f;
	mtx[2][0] = 0.0f;
	mtx[1][0] = 0.0f;
	mtx[2][2] = 1.0f;
	mtx[1][1] = 1.0f;
	mtx[0][0] = 1.0f;
}

void TMirrorModel::init(const char* name)
{
	unk4 = SMS_MakeMActorWithAnmData(name, gpMirrorModelManager->getUnk20(), 2,
	                                 J3DMLF_MaterialPEFull
	                                     | J3DMLF_UseUniqueMaterials
	                                     | (1 << J3DMLF_TevStageNumShift));

	TPosition3f local_44;
	local_44.identity();
	unk4->getModel()->setBaseTRMtx(local_44);
	unk4->calc();
	unk4->getModel()->getModelData()->getMaterialNodePointer(0)->change();

	if (!gpMirrorModelManager->unk24)
		gpMirrorModelManager->findMirrorCamera();
	unk8 = gpMirrorModelManager->unk24;

	initPlaneInfo();
}

TMirrorModel::TMirrorModel()
    : unk4(nullptr)
    , unk8(0)
    , unk24(0.0f)
{
	unkC.zero();
	unk18.zero();
}

void TMirrorModelObj::setPlane()
{
	MtxPtr mtx = unk4->getModel()->getAnmMtx(0);
	Vec* v     = (Vec*)unk4->getModel()
	             ->getModelData()
	             ->getVertexData()
	             .getVtxPosArray();

	JGeometry::TVec3<f32> local_18;
	local_18.x = v->x;
	local_18.y = v->y;
	local_18.z = v->z;

	unk18.x = mtx[0][1];
	unk18.y = mtx[1][1];
	unk18.z = mtx[2][1];

	MTXMultVec(mtx, &local_18, &local_18);
	unk24 = -VECDotProduct(unk18, local_18);
	unk8->setUnk84AndUnk90(unk18.x, unk18.y, unk18.z, unk24);
}

void TMirrorModelObj::calc()
{
	unk4->getModel()->setAnmMtx(0, unk28->getAnmMtx(0));
}

void TMirrorModelObj::init(const char* name)
{
	TMirrorModel::init(name);
	gpMirrorModelManager->registerObjMirror(this);
}

TMirrorModelManager* gpMirrorModelManager;

bool TMirrorModelManager::isUpperThanMirrorPlane(
    const JGeometry::TVec3<f32>& param_1) const
{
	const JGeometry::TVec3<f32>* normal
	    = unk18 != -1 ? &unk1C[unk18]->getNormalVec() : nullptr;

	f32 d   = unk18 != -1 ? unk1C[unk18]->getD() : 0.0f;
	f32 dot = normal->dot(param_1);

	return dot + d < -50.0f ? false : true;
}

bool TMirrorModelManager::isInMirror(JGeometry::TVec3<f32>& param_1) const
{
	return gpCubeMirror->getDataNo(gpCubeMirror->getInCubeNo(param_1)) != -1
	           ? true
	           : false;
}

// TODO: the original calls TVec3 ctor/set/dot/scaleAdd out of line in here
// (and keeps the plane normal + negated distance as a 16-byte stack object).
// Likely an inline-depth difference; no clean source shape found yet.
void TMirrorModelManager::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (cue & CUE_MOVE) {
		JGeometry::TVec3<f32> local_44 = SMS_GetMarioPos();
		unk18 = gpCubeMirror->getDataNo(gpCubeMirror->getInCubeNo(local_44));
		if (!(unk18 != -1 ? true : false)
		    && !gpMarioGroundPlane[0]->isIllegalData()) {
			const TBGCheckData* plane = gpMarioGroundPlane[0];
			unk24->setUnk84AndUnk90(plane->mNormal.x, plane->mNormal.y,
			                        plane->mNormal.z, plane->mPlaneDistance);
			MMMakeMirrorViewMtxA(unk24);
		}
	}

	if (unk18 != -1 ? true : false) {
		if (cue & CUE_CALC_ANIM)
			unk1C[unk18]->calc();

		if (cue & CUE_CALC_VIEW)
			unk1C[unk18]->unk4->viewCalc();

		if (cue & CUE_ENTRY) {
			unk1C[unk18]->setPlane();
			unk1C[unk18]->calcView();
			unk1C[unk18]->entry();
		}
	}
}

void TMirrorModelManager::findMirrorCamera()
{
	unk24
	    = static_cast<TMirrorCamera*>(JDrama::TNameRefGen::search("鏡カメラ"));
}

void TMirrorModelManager::loadAfter()
{
	if (!unk24)
		findMirrorCamera();

	for (int i = 0; i < unk10; ++i) {
		J3DTexture* texture
		    = unk1C[i]->getUnk4()->getModel()->getModelData()->getTexture();

		ResTIMG& target       = *texture->getResTIMG(0);
		const ResTIMG& source = *unk24->getUnk94();

		target = source;
		target.imageDataOffset
		    = (uintptr_t)&source + source.imageDataOffset - (uintptr_t)&target;
	}
}

void TMirrorModelManager::registerObjMirror(TMirrorModel* model)
{
	unk1C[unk10] = model;
	unk10++;
}

void TMirrorModelManager::load(JSUMemoryInputStream& stream)
{
	s32 local_30;
	JDrama::TViewObj::load(stream);
	s32 local_28;
	s32 local_2C;
	stream >> local_28;
	stream >> local_2C;
	stream >> local_30;
	unk14 = local_28 + local_2C + local_30 * 2;

	if (unk14 != 0) {
		unk20 = new MActorAnmData;
		unk1C = new TMirrorModel*[unk14];
		for (int i = 0; i < local_28; ++i) {
			unk1C[i] = new TMirrorModel;
			char acStack_130[0x100];
			if (gpMarDirector->getCurrentMap() == 7) {
				static const char* table[] = { "205", nullptr };
				snprintf(acStack_130, 0x100, "/scene/map/mirror/mirror%s.bmd",
				         table[i]);
			} else {
				snprintf(acStack_130, 0x100, "/scene/map/mirror/mirror%02d.bmd",
				         i);
			}
			unk1C[i]->init(acStack_130);
			++unk10;
		}
	}
}

TMirrorModelManager::TMirrorModelManager(const char* name)
    : JDrama::TViewObj(name)
    , unk10(0)
    , unk14(0)
    , unk18(-1)
    , unk1C(nullptr)
    , unk24(0)
    , unk28(0)
{
	gpMirrorModelManager = this;
}

void TMirrorMapDrawBuf::perform(u32 cue, JDrama::TGraphics* graphics)
{
	if (!(cue & CUE_DRAW) || (gpMirrorModelManager->unk18 != -1 ? true : false))
		JDrama::TDrawBufObj::perform(cue, graphics);
}
