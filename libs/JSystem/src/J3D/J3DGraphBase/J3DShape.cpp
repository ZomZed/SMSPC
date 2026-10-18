#include <JSystem/J3D/J3DGraphBase/J3DShape.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTevs.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTransform.hpp>
#include <JSystem/JRenderer.hpp>
#include <dolphin/gx.h>
#include <dolphin/gd.h>
#ifdef SMS_PORT
#include <port_gx.h>
#endif

J3DShapeMtx::LoadPipeline J3DShapeMtx::mtxLoadPipeline[4] = {
	&J3DShapeMtx::loadMtxIndx_PNGP,
	&J3DShapeMtx::loadMtxIndx_PCPU,
	&J3DShapeMtx::loadMtxIndx_NCPU,
	&J3DShapeMtx::loadMtxIndx_PNCPU,
};
u32 J3DShapeMtx::currentPipeline;

void J3DShapeMtx::loadMtxIndx_PNGP(int id, u16 mtx_index) const
{
	j3dSys.loadPosMtxIndx(id, mtx_index);
	j3dSys.loadNrmMtxIndx(id, mtx_index);
}

void J3DShapeMtx::loadMtxIndx_PCPU(int id, u16 mtx_index) const
{
	GXLoadPosMtxImm(j3dSys.mViewMtx, id * 3);
	j3dSys.loadNrmMtxIndx(id, mtx_index);
}

void J3DShapeMtx::loadMtxIndx_NCPU(int id, u16 mtx_index) const
{
	j3dSys.loadPosMtxIndx(id, mtx_index);
	GXLoadNrmMtxImm(j3dSys.mViewMtx, id * 3);
}

void J3DShapeMtx::loadMtxIndx_PNCPU(int id, u16) const
{
	GXLoadPosMtxImm(j3dSys.mViewMtx, id * 3);
	GXLoadNrmMtxImm(j3dSys.mViewMtx, id * 3);
}

void J3DShapeMtx::load() const
{
	LoadPipeline ld = mtxLoadPipeline[currentPipeline];
	(this->*ld)(0, mUseMtxIndex);
}

void J3DShapeMtx::calcNBTScale(const Vec& param_1, float (*param_2)[3][3],
                               float (*param_3)[3][3])
{
	J3DPSMtx33Copy(param_2[mUseMtxIndex], param_3[mUseMtxIndex]);
	J3DScaleNrmMtx33(param_3[mUseMtxIndex], param_1);
}

J3DShapeMtxDL::J3DShapeMtxDL(u16 useMtxIndex)
    : J3DShapeMtx(useMtxIndex)
{
}

void J3DShapeMtxDL::load() const { GXCallDisplayList(mDisplayList, 0x20); }

void J3DShapeMtxMulti::load() const
{
	LoadPipeline ld = mtxLoadPipeline[currentPipeline];
	for (int i = 0; i < mUseMtxNum; ++i) {
		if (mUseMtxIndexTable[i] == 0xffff)
			continue;
		(this->*ld)(i, mUseMtxIndexTable[i]);
	}
}

void J3DShapeMtxMulti::calcNBTScale(const Vec& param_1, float (*param_2)[3][3],
                                    float (*param_3)[3][3])
{
	for (int i = 0; i < mUseMtxNum; ++i) {
		if (mUseMtxIndexTable[i] == 0xffff)
			continue;
		J3DPSMtx33Copy(param_2[mUseMtxIndexTable[i]],
		               param_3[mUseMtxIndexTable[i]]);
		J3DScaleNrmMtx33(param_3[mUseMtxIndexTable[i]], param_1);
	}
}

J3DShapeDraw::J3DShapeDraw(const u8* list, u32 size)
{
	mDisplayList     = list;
	mDisplayListSize = size;
}

void J3DShapeDraw::draw() const
{
	GXCallDisplayList((void*)mDisplayList, mDisplayListSize);
}

void J3DShape::initialize()
{
	mMaterial       = nullptr;
	mIndex          = -1;
	mMtxGroupNum    = 0;
	mFlags          = 0;
	mRadius         = 0;
	mMin.x          = 0.0f;
	mMin.y          = 0.0f;
	mMin.z          = 0.0f;
	mMax.x          = 0.0f;
	mMax.y          = 0.0f;
	mMax.z          = 0.0f;
	mVtxDesc        = nullptr;
	mMatrices       = nullptr;
	mDraws          = nullptr;
	mVertexData     = nullptr;
	mDrawMtxData    = nullptr;
	mScaleFlagArray = nullptr;
	mDrawMatrices   = nullptr;
	mNormMatrices   = nullptr;
	mCurrentViewNo  = &j3dDefaultViewNo;
	mHasNBT         = false;
}

J3DShape::~J3DShape() { }

void J3DShape::calcNBTScale(const Vec& param_1, float (*param_2)[3][3],
                            float (*param_3)[3][3])
{
	for (u16 i = 0; i < mMtxGroupNum; ++i)
		mMatrices[i]->calcNBTScale(param_1, param_2, param_3);
}

int J3DShape::countBumpMtxNum() const
{
	int result = 0;
	for (u16 i = 0; i < mMtxGroupNum; ++i)
		result += mMatrices[i]->getUseMtxNum();
	return result;
}

void J3DShape::makeVtxArrayCmd()
{
	const GXVtxAttrFmtList* vtxAttr = mVertexData->getVtxAttrFmtList();
	u8 stride[12];
	void* array[12];

	for (int i = 0; i < 12; i++) {
		stride[i] = 0;
		array[i]  = 0;
	}

	for (; vtxAttr->attr != GX_VA_NULL; vtxAttr++) {
		switch (vtxAttr->attr) {
		case GX_VA_POS: {
			if (vtxAttr->type == GX_F32)
				stride[vtxAttr->attr - GX_VA_POS] = 12;
			else
				stride[vtxAttr->attr - GX_VA_POS] = 6;
			array[vtxAttr->attr - GX_VA_POS] = mVertexData->getVtxPosArray();
		} break;
		case GX_VA_NRM: {
			if (vtxAttr->type == GX_F32)
				stride[vtxAttr->attr - GX_VA_POS] = 12;
			else
				stride[vtxAttr->attr - GX_VA_POS] = 6;
			array[vtxAttr->attr - GX_VA_POS] = mVertexData->getVtxNormArray();
		} break;
		case GX_VA_CLR0:
		case GX_VA_CLR1: {
			stride[vtxAttr->attr - GX_VA_POS] = 4;
			array[vtxAttr->attr - GX_VA_POS]
			    = mVertexData->getVtxColorArray(vtxAttr->attr - GX_VA_CLR0);
		} break;
		case GX_VA_TEX0:
		case GX_VA_TEX1:
		case GX_VA_TEX2:
		case GX_VA_TEX3:
		case GX_VA_TEX4:
		case GX_VA_TEX5:
		case GX_VA_TEX6:
		case GX_VA_TEX7: {
			if (vtxAttr->type == GX_F32)
				stride[vtxAttr->attr - GX_VA_POS] = 8;
			else
				stride[vtxAttr->attr - GX_VA_POS] = 4;
			array[vtxAttr->attr - GX_VA_POS]
			    = mVertexData->getVtxTexCoordArray(vtxAttr->attr - GX_VA_TEX0);
		} break;
		default:
			break;
		}
	}
	for (GXVtxDescList* it = mVtxDesc; it->attr != GX_VA_NULL; ++it) {
		if ((it->attr == GX_VA_NBT) && (it->type != GX_NONE)) {
			mHasNBT = true;
			stride[1] *= 3;
			array[1] = mVertexData->getVtxNBTArray();
		}
	}

	for (s32 i = 0; i < 0x0C; i++) {
#ifdef SMS_PORT
		// PC port: the array size goes into the display list too (port_gx.h).
		if (array[i] != 0) {
			int sizeIdx = (i == 1 && mHasNBT) ? J3DVertexData::PORT_ARRAY_NBT : i;
			PORTGDSetArray((GXAttr)(i + GX_VA_POS), array[i],
			               mVertexData->getPortArraySize(sizeIdx, array[i], stride[i]),
			               stride[i]);
		} else {
			PORTGDSetArrayNone((GXAttr)(i + GX_VA_POS), stride[i]);
		}
#else
		if (array[i] != 0)
			GDSetArray((GXAttr)(i + GX_VA_POS), array[i], stride[i]);
		else
			GDSetArrayRaw((GXAttr)(i + GX_VA_POS), 0, stride[i]);
#endif
	}
}

#ifdef SMS_PORT
// PC port: stride of a VTX1 array the way makeVtxArrayCmd computes it.
static u8 portVtxArrayStride(const J3DVertexData* data, GXAttr attr)
{
	for (const GXVtxAttrFmtList* it = data->getVtxAttrFmtList();
	     it->attr != GX_VA_NULL; it++) {
		if (it->attr == attr)
			return attr == GX_VA_CLR0 ? 4 : it->type == GX_F32 ? 12 : 6;
	}
	return attr == GX_VA_CLR0 ? 4 : 12;
}
#endif

void J3DShape::makeVcdVatCmd()
{
	GDLObj list;

	GDInitGDLObj(&list, mVcdVatCmd, kVcdVatDLSize);
	GDSetCurrent(&list);
	GDSetVtxDescv(mVtxDesc);
	makeVtxArrayCmd();
	J3DSetVtxAttrFmtv(GX_VTXFMT0, mVertexData->getVtxAttrFmtList(), mHasNBT);
	GDPadCurr32();
#ifdef SMS_PORT
	// PC port: draw() calls the list with the full kVcdVatDLSize. On the GameCube the
	// commands fill the 0xC0 buffer (every array writes both CP registers), but the PC
	// list is shorter (no base/stride pair for unused arrays) and the rest of the buffer
	// held stale heap bytes, which Aurora executed (e.g. a bogus indexed XF load on
	// Mario at the airstrip). Pad the tail with NOPs (GX_NOP = 0).
	while (GDGetCurrOffset() < kVcdVatDLSize)
		GDWrite_u8(0);
#endif
	GDFlushCurrToMem();
	GDSetCurrent(nullptr);
}

void J3DShape::loadVtxArray() const
{
#ifdef SMS_PORT
	// PC port: the GPU needs the array sizes (port_gx.h).
	void* pos = j3dSys.getVtxPos();
	J3DLoadArrayBasePtr(
	    GX_VA_POS, pos,
	    mVertexData->getPortArraySize(
	        0, pos, portVtxArrayStride(mVertexData, GX_VA_POS)));
	if (!mHasNBT) {
		void* nrm = j3dSys.getVtxNrm();
		J3DLoadArrayBasePtr(
		    GX_VA_NRM, nrm,
		    mVertexData->getPortArraySize(
		        1, nrm, portVtxArrayStride(mVertexData, GX_VA_NRM)));
	}

	void* col = j3dSys.getVtxCol();
	J3DLoadArrayBasePtr(GX_VA_CLR0, col,
	                    mVertexData->getPortArraySize(2, col, 4));
#else
	J3DLoadArrayBasePtr(GX_VA_POS, j3dSys.getVtxPos());
	if (!mHasNBT)
		J3DLoadArrayBasePtr(GX_VA_NRM, j3dSys.getVtxNrm());

	J3DLoadArrayBasePtr(GX_VA_CLR0, j3dSys.getVtxCol());
#endif
}

void J3DShape::draw() const
{
	GXCallDisplayList(mVcdVatCmd, J3DShape::kVcdVatDLSize);

	J3DShapeMtx::currentPipeline = mFlags >> 2 & 3;
	loadVtxArray();

#ifdef SMS_PORT
	// PC port: matrix array sizes for the indexed matrix loads (port_gx.h).
	u32 drawMtxNum = mDrawMtxData != nullptr ? mDrawMtxData->mEntryNum : 0;
	j3dSys.setModelDrawMtx(mDrawMatrices[*mCurrentViewNo], drawMtxNum);
	j3dSys.setModelNrmMtx(mNormMatrices[*mCurrentViewNo], drawMtxNum);
#else
	j3dSys.setModelDrawMtx(mDrawMatrices[*mCurrentViewNo]);
	j3dSys.setModelNrmMtx(mNormMatrices[*mCurrentViewNo]);
#endif

	JRNLoadCurrentMtx(GX_PNMTX0, mCurrentTexMtx[0], mCurrentTexMtx[1],
	                  mCurrentTexMtx[2], mCurrentTexMtx[3], mCurrentTexMtx[4],
	                  mCurrentTexMtx[5], mCurrentTexMtx[6], mCurrentTexMtx[7]);

	for (u16 i = 0; i < mMtxGroupNum; ++i) {
		if (mMatrices[i])
			mMatrices[i]->load();
		if (mDraws[i])
			mDraws[i]->draw();
	}
}
