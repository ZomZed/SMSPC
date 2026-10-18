#ifndef ENEMY_BOSS_HANACHAN_SUB_HPP
#define ENEMY_BOSS_HANACHAN_SUB_HPP

#include <JSystem/JGeometry.hpp>

// A chain of spheres following a head point, used for the body segments of
// the boss. Very similar to TRope from MarioUtil/MtxUtil.

class TSpherePoint {
public:
	TSpherePoint() { }

public:
	/* 0x00 */ JGeometry::TVec3<f32> unk0; // previous position
	/* 0x0C */ JGeometry::TVec3<f32> unkC; // position
	/* 0x18 */ JGeometry::TVec3<f32> unk18; // velocity
	/* 0x24 */ f32 unk24;                  // distance to the previous point
	/* 0x28 */ f32 unk28;                  // rotation around Z
};

class TSphereLink {
public:
	TSphereLink(u16, const JGeometry::TVec3<f32>&, f32, f32, f32, f32, f32,
	            f32);

	BOOL setDegreeZAndRevisionPosXZ(int, f32);
	void moveHead(const JGeometry::TVec3<f32>&);
	void execMapCollision_(JGeometry::TVec3<f32>*);

	// fabricated
	TSpherePoint& getPoint(int i) { return unk4[i]; }

public:
	/* 0x00 */ u16 unk0;
	/* 0x04 */ TSpherePoint* unk4;
	/* 0x08 */ f32 unk8;
	/* 0x0C */ f32 unkC;
	/* 0x10 */ f32 unk10;
	/* 0x14 */ f32 unk14;
	/* 0x18 */ f32 unk18;
};

void BHSCalcRevisionDistXZByRotateZ(f32, f32, f32, f32*, f32*);
f32 BHSCalcCentrifugalForce(const JGeometry::TVec3<f32>&,
                            const JGeometry::TVec3<f32>&,
                            const JGeometry::TVec3<f32>&, f32);

#endif
