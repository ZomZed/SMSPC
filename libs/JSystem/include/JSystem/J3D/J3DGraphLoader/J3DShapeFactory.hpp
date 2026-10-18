#ifndef J3D_SHAPE_FACTORY_HPP
#define J3D_SHAPE_FACTORY_HPP

#include <dolphin/types.h>
#include <dolphin/mtx.h>
#include <dolphin/gx.h>
#include <JSystem/JSupport.hpp>

class J3DShape;
class J3DShapeMtx;
class J3DShapeDraw;
struct ResNTAB;

struct J3DShapeInitData {
	/* 0x00 */ u8 mShapeMtxType;
	/* 0x02 */ u16 mMtxGroupNum;
	/* 0x04 */ u16 mVtxDescListIndex;
	/* 0x06 */ u16 mMtxInitDataIndex;
	/* 0x08 */ u16 mDrawInitDataIndex;
	/* 0x0C */ f32 mRadius;
	/* 0x10 */ Vec mMin;
	/* 0x1C */ Vec mMax;
};

struct J3DShapeMtxInitData {
	/* 0x00 */ u16 mUseMtxIndex;
	/* 0x02 */ u16 mUseMtxCount;
	/* 0x04 */ u32 mFirstUseMtxIndex;
};

struct J3DShapeDrawInitData {
	/* 0x00 */ u32 mDisplayListSize;
	/* 0x04 */ u32 mDisplayListIndex;
};

struct J3DShapeBlock {
	/* 0x00 */ u8 mMagic[4];
	/* 0x04 */ u32 mSize;

	/* 0x08 */ u16 mShapeNum;
	/* 0x0A */ u16 _pad;

	/* 0x0C */ JSU_FILE_PTR(J3DShapeInitData) mpShapeInitData;
	/* 0x10 */ JSU_FILE_PTR(u16) mpIndexTable;
	/* 0x14 */ JSU_FILE_PTR(ResNTAB) mpNameTable;
	/* 0x18 */ JSU_FILE_PTR(GXVtxDescList) mpVtxDescList;
	/* 0x1C */ JSU_FILE_PTR(u16) mpMtxTable;
	/* 0x20 */ JSU_FILE_PTR(u8) mpDisplayListData;
	/* 0x24 */ JSU_FILE_PTR(J3DShapeMtxInitData) mpMtxInitData;
	/* 0x28 */ JSU_FILE_PTR(J3DShapeDrawInitData) mpDrawInitData;
}; // Size: 0x2C

#ifdef SMS_PORT
static_assert(sizeof(J3DShapeInitData) == 0x28, "SHP1 init data");
static_assert(sizeof(J3DShapeMtxInitData) == 0x8, "SHP1 mtx init data");
static_assert(sizeof(J3DShapeDrawInitData) == 0x8, "SHP1 draw init data");
static_assert(sizeof(J3DShapeBlock) == 0x2C, "SHP1 layout");
#endif

enum J3DMdlDataFlag {
	J3DMdlDataFlag_ConcatView   = 0x10,
	J3DMdlDataFlag_NoUseDrawMtx = 0x20,
	J3DMdlDataFlag_NoAnimation  = 0x100,
};

class J3DShapeFactory {
public:
	J3DShapeFactory(const J3DShapeBlock&);
	J3DShape* create(int, J3DMdlDataFlag, GXVtxDescList*);
	J3DShapeMtx* newShapeMtx(int, int) const;
	J3DShapeDraw* newShapeDraw(int, int, J3DMdlDataFlag) const;
	void allocVcdVatCmdBuffer(u32);

	u32 getMtxGroupNum(int no) const
	{
		return mpShapeInitData[mpIndexTable[no]].mMtxGroupNum;
	}
	GXVtxDescList* getVtxDescList(int no) const
	{
		return (GXVtxDescList*)((u8*)mpVtxDescList
		                        + mpShapeInitData[mpIndexTable[no]]
		                              .mVtxDescListIndex);
	}
	f32 getRadius(int no) const
	{
		return mpShapeInitData[mpIndexTable[no]].mRadius;
	}
	Vec& getMin(int no) const { return mpShapeInitData[mpIndexTable[no]].mMin; }
	Vec& getMax(int no) const { return mpShapeInitData[mpIndexTable[no]].mMax; }

private:
	/* 0x00 */ J3DShapeInitData* mpShapeInitData;
	/* 0x04 */ u16* mpIndexTable;
	/* 0x08 */ GXVtxDescList* mpVtxDescList;
	/* 0x0C */ u16* mpMtxTable;
	/* 0x10 */ u8* mpDisplayListData;
	/* 0x14 */ J3DShapeMtxInitData* mpMtxInitData;
	/* 0x18 */ J3DShapeDrawInitData* mpDrawInitData;
	/* 0x1C */ u8* mpVcdVatCmdBuffer;
};

#endif
