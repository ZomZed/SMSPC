#ifndef ENEMY_BOSS_HANACHAN_PARTS_HPP
#define ENEMY_BOSS_HANACHAN_PARTS_HPP

#include <Enemy/BossHanachanAnm.hpp>
#include <NPC/NpcInbetween.hpp>
#include <Strategic/LiveActor.hpp>
#include <Player/ModelWaterManager.hpp>

class TBossHanachan;
class TMapCollisionMove;
class TIdxGroupObj;

class TFootHitActor : public TWaterHitActor {
public:
	TFootHitActor(const char* name)
	    : TWaterHitActor(name)
	{
	}

public:
	/* 0x6C */ MtxPtr unk6C;
};

class TBossHanachanPartsBase : public TLiveActor {
public:
	TBossHanachanPartsBase(TBossHanachan*, u32, int, const char*);
	virtual const char** getBasNameTable() const;
	virtual bool setAnm_(EnumBossHanachanAnmKind,
	                     EnumBossHanachanStopMotionBlendOnOff)
	    = 0;

	void considerSetAnm_(EnumBossHanachanNerveAnm);
	bool isReactToTrampleOrHipDrop_() const;
	void calcRotateZWhenGetUp_();
	bool isMarioOn_() const;
	const TLiveActor* getSandActor_() const;
	void copyFrameFromOldAnmToNewAnm_();
	bool isCurBckAlreadyEnd_() const;
	void setDamageFog_(JDrama::TGraphics*);
	void entryCircleShadow_();
	void moveMapCollision_();
	void changeTumbleAnmRate_();
	void restartBck_();
	void setNonstopMotionBlendRatio_(f32 ratio)
	{
		unk110->mForcedBlendRatio = ratio;
	}
	void offNonstopMotionBlend_() { unk110->mForcedBlendRatio = 0.0f; }
	void initMapCollisionAndHitActor_(TIdxGroupObj*);

public:
	/* 0xF4 */ s32 unkF4; // current EnumBossHanachanAnmKind
	/* 0xF8 */ s32 unkF8; // previous EnumBossHanachanAnmKind
	/* 0xFC */ TBossHanachan* unkFC;
	/* 0x100 */ TWaterHitActor* unk100;
	/* 0x104 */ TMapCollisionMove* unk104;
	/* 0x108 */ MtxPtr unk108;
	/* 0x10C */ s32 unk10C;
	/* 0x110 */ TNpcInbetween* unk110;
};

class TBossHanachanPartsHead : public TBossHanachanPartsBase {
public:
	TBossHanachanPartsHead(TBossHanachan*, const char*);
	virtual BOOL receiveMessage(THitActor*, u32);
	virtual bool setAnm_(EnumBossHanachanAnmKind,
	                     EnumBossHanachanStopMotionBlendOnOff);

public:
	/* 0x114 */ MtxPtr unk114; // nose hole L
	/* 0x118 */ MtxPtr unk118; // nose hole R
};

class TBossHanachanPartsBody : public TBossHanachanPartsBase {
public:
	TBossHanachanPartsBody(TBossHanachan*, const char*);
	virtual BOOL receiveMessage(THitActor*, u32);
	virtual bool setAnm_(EnumBossHanachanAnmKind,
	                     EnumBossHanachanStopMotionBlendOnOff);

	void initFootHitActor_(TIdxGroupObj*);

public:
	/* 0x114 */ s32 unk114; // body index
	/* 0x118 */ TFootHitActor* unk118[2];
	/* 0x120 */ f32 unk120;
	/* 0x124 */ JGeometry::TVec3<f32> unk124; // previous position
	/* 0x130 */ JGeometry::TVec3<f32> unk130; // position before previous
	/* 0x13C */ f32 unk13C;                   // previous rotation z
	/* 0x140 */ f32 unk140;                   // rotation z before previous
	/* 0x144 */ f32 unk144;
	/* 0x148 */ f32 unk148;
	/* 0x14C */ MtxPtr unk14C[2]; // leg joints L3/R3
	/* 0x154 */ JGeometry::TVec3<f32> unk154;
};

extern const char* cMapCollisionJointName;
extern const char* cBodyMapCollisionFileName;
extern const char* cHeadMapCollisionFileName;
extern const char* cLegJointName_L3;
extern const char* cLegJointName_R3;
extern const char* cNoseHallJointName_L;
extern const char* cNoseHallJointName_R;

#endif
