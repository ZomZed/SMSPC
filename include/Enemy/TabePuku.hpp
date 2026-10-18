#ifndef ENEMY_TABEPUKU_HPP
#define ENEMY_TABEPUKU_HPP

#include <Enemy/SmallEnemy.hpp>
#include <JSystem/JDrama/JDRGraphics.hpp>
#include <JSystem/JGeometry.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/Spine.hpp>
#include <dolphin/types.h>

class JSUMemoryInputStream;
class TBGCheckData;
class THitActor;
class TLiveManager;
class TTabePuku;

DECLARE_NERVE(TNerveTabePukuGraphWander, TLiveActor);
DECLARE_NERVE(TNerveTabePukuFound, TLiveActor);
DECLARE_NERVE(TNerveTabePukuRecoverGraph, TLiveActor);
DECLARE_NERVE(TNerveTabePukuAttack, TLiveActor);
DECLARE_NERVE(TNerveTabePukuBite, TLiveActor);
DECLARE_NERVE(TNerveTabePukuDive, TLiveActor);
DECLARE_NERVE(TNerveTabePukuDrag, TLiveActor);

class TTabePukuParams : public TSmallEnemyParams {
public:
	TTabePukuParams(const char* path);

	/* 0x2D4 */ TParamRT<f32> mMarchSpeed;
	/* 0x2E8 */ TParamRT<f32> mAttackSpeed;
	/* 0x2FC */ TParamRT<f32> mDiveSpeed;
	/* 0x310 */ TParamRT<f32> mWaterFric;
	/* 0x324 */ TParamRT<f32> mTurnSlerpRate;
	/* 0x338 */ TParamRT<f32> mApartHeight;
	/* 0x34C */ TParamRT<f32> mCorrectY;
	/* 0x360 */ TParamRT<f32> mCorrectZ;
	/* 0x374 */ TParamRT<f32> mTerritoryRange;
	/* 0x388 */ TParamRT<f32> mDragLength;
};

// Separate collision actor that represents the mouth of the TabePuku and
// does the terrain & wall collision checks for it.
class TTPHitActor : public THitActor {
public:
	TTPHitActor(TTabePuku& owner);

	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	virtual void init();

	void checkHitActors();
	void bind();
	void updateTerrainCollsion();
	void updateObjCollision();

	// fabricated
	BOOL isAirborne() const { return unk88; }

public:
	/* 0x68 */ TTabePuku* mOwner;
	/* 0x6C */ JGeometry::TVec3<f32> unk6C;
	/* 0x78 */ f32 unk78;
	/* 0x7C */ f32 unk7C;
	/* 0x80 */ f32 unk80;
	/* 0x84 */ const TBGCheckData* unk84;
	/* 0x88 */ bool unk88; // not touching the ground
	/* 0x89 */ u8 unk89;
};

class TTabePuku : public TSmallEnemy {
public:
	TTabePuku(const char* name);

	virtual void perform(u32, JDrama::TGraphics*);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	virtual MtxPtr getTakingMtx();
	virtual void init(TLiveManager*);
	virtual void calcRootMatrix();
	virtual void control();
	virtual void bind();
	virtual const char** getBasNameTable() const;
	virtual void reset();
	virtual void behaveToWater(THitActor*);
	virtual void attackToMario();
	virtual void forceKill();
	virtual bool doKeepDistance();
	virtual bool isFindMario(f32);

	void detach();
	void setMomentumFromQuat();
	void swimTo(const JGeometry::TVec3<f32>&);
	bool doDrag();
	void decideDragDir();
	void prepareDrag();
	bool doDive();
	void prepareDive();
	void doBite();
	void swimToCurPathNode(const JGeometry::TVec3<f32>&);
	bool isBiting() const;
	bool isAttacking() const;
	bool isGraphWander() const;
	bool isTouchedPlane() const;
	bool isMissMario() const;
	void emitEffects();
	void updateSound();
	void initParams();
	void initCollision();

	// fabricated
	BOOL isCatching() const { return mHeldObject != nullptr ? TRUE : FALSE; }

	// fabricated
	TTabePukuParams* getParams() const
	{
		return (TTabePukuParams*)getSaveParam();
	}

public:
	/* 0x194 */ TTPHitActor* mHitActor;
	/* 0x198 */ JGeometry::TQuat4<f32> mQuat;
	/* 0x1A8 */ TPosition3f mTakingMtx;
	/* 0x1D8 */ int mMouthJointIdx;
	/* 0x1DC */ u8 unk1DC; // touched a wall
	/* 0x1E0 */ f32 mDiveStartY;
	/* 0x1E4 */ JGeometry::TVec3<f32> mDragDir;
};

class TTabePukuManager : public TSmallEnemyManager {
public:
	TTabePukuManager(const char* name);

	virtual void load(JSUMemoryInputStream&);
	virtual void createModelData();
};

#endif
