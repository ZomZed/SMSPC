#ifndef CAMERA_SUN_MODEL_HPP
#define CAMERA_SUN_MODEL_HPP

#include <JSystem/J3D/J3DGraphBase/Components/J3DGXColorS10.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DAnimation.hpp>
#include <JSystem/JDrama/JDRActor.hpp>

class J3DModel;
class TMapStaticObj;

// NOTE: seems like they wrote something weird for these vars...
#ifdef LINK_ALL_SOURCE
// (link-all-source: defined once, in lensflare.cpp, like the original)
extern const char* cSunVolumeName;
extern const char* cSunsetVolumeName;
#else
// Every unit including this header carries both strings in its string pool,
// but only lensflare.cpp owns the variables: declare them and keep the literals
// alive through an unreferenced const table (the compiler emits the strings
// but drops the table).
extern const char* cSunVolumeName;
extern const char* cSunsetVolumeName;
static const char* const sSunVolumeNameLiterals[] = { "/scene/sun", "/scene/sunset" };
#endif

class TSunModel : public JDrama::TActor {
public:
	TSunModel(bool, const char* name = "<TSunModel>");

	virtual void load(JSUMemoryInputStream&);
	virtual void perform(u32 cue, JDrama::TGraphics* graphics);

	void getZBufValue();

	// fabricated
	f32 calcHiddenRatio()
	{
		int hiddenCount                  = 0;
		const JGeometry::TVec2<s16>* pos = unkB4;
		const bool* visible              = unk180;
		for (int i = 0; i < 17; ++i, ++pos, ++visible) {
			if (pos->x != -1 && pos->y != -1 && *visible == 0)
				++hiddenCount;
		}
		return hiddenCount * (1.0f / 17.0f);
	}

	u8 getUnk191() { return unk191; }
	f32 getUnk194() { return unk194; }
#ifdef SMS_PORT
	const JGeometry::TVec2<f32>& portSunScreenPos() const { return unkF8[0]; } // port: sun-warp trace
#endif

	// TODO: incorrect! Maybe a cameralib inline or even TVec3?
	bool isInBounds(f32 bounds)
	{
		const JGeometry::TVec2<f32>& pos = unkF8[0];
		return -bounds <= pos.x && pos.x <= bounds && -bounds <= pos.y
		               && pos.y <= bounds
		           ? true
		           : false;
	}
	f32 getUnkAC() { return unkAC; }

private:
	void calcOtherFPosFromCenterAndRadius_(JGeometry::TVec2<f32>*,
	                                       const JGeometry::TVec2<f32>&, f32);

	void calcDispRatioAndScreenPos_();

public:
	/* 0x44 */ J3DModelData* unk44;
	/* 0x48 */ J3DModel* unk48;
	/* 0x4C */ J3DAnmTextureSRTKey* unk4C;
	/* 0x50 */ J3DFrameCtrl unk50;
	/* 0x64 */ TMapStaticObj* unk64;
	/* 0x68 */ u8 unk68;
	/* 0x6C */ f32 unk6C;
	/* 0x70 */ f32 unk70;
	/* 0x74 */ u8 unk74;
	/* 0x78 */ f32 unk78;
	/* 0x7C */ f32 unk7C;
	/* 0x80 */ u8 unk80;
	/* 0x84 */ f32 unk84;
	/* 0x88 */ f32 unk88;
	/* 0x8C */ J3DGXColorS10 unk8C;
	/* 0x94 */ J3DGXColorS10 unk94;
	/* 0x9C */ f32 unk9C;
	/* 0xA0 */ f32 unkA0;
	/* 0xA4 */ f32 unkA4;
	/* 0xA8 */ f32 unkA8;
	/* 0xAC */ f32 unkAC;
	/* 0xB0 */ f32 unkB0;
	/* 0xB4 */ JGeometry::TVec2<s16> unkB4[17];
	/* 0xF8 */ JGeometry::TVec2<f32> unkF8[17];
	/* 0x180 */ bool unk180[17];
	/* 0x191 */ u8 unk191;
	/* 0x194 */ f32 unk194;
	/* 0x198 */ JGeometry::TVec3<f32> unk198;
	/* 0x1A4 */ f32 unk1A4;
	/* 0x1A8 */ f32 unk1A8;
	/* 0x1AC */ u8 unk1AC;
};

extern TSunModel* gpSunModel;

#endif
