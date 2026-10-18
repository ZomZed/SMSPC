#ifndef ENEMY_KILLER_HPP
#define ENEMY_KILLER_HPP

#include <Enemy/WalkerEnemy.hpp>
#include <Enemy/SmallEnemy.hpp>
#include <Strategic/Nerve.hpp>
#include <System/ParamInst.hpp>
#include <JSystem/JGeometry.hpp>
#include <dolphin/gx/GXStruct.h>

class TLiveManager;
class THitActor;
class JSUMemoryInputStream;

class TFlyEnemyParams : public TWalkerEnemyParams {
public:
	TFlyEnemyParams(const char* path);

	/* 0x32C */ TParamRT<f32> mSLNormalFlyGravityY;
	/* 0x340 */ TParamRT<f32> mSLNormalFlySpeed;
	/* 0x354 */ TParamRT<f32> mSLChaseFlyGravityY;
	/* 0x368 */ TParamRT<f32> mSLChaseDist;
	/* 0x37C */ TParamRT<f32> mSLForceGravityY;
};

class TKillerSaveLoadParams : public TFlyEnemyParams {
public:
	TKillerSaveLoadParams(const char* path);

	/* 0x390 */ TParamRT<f32> mSLWaterAddGravityY;
	/* 0x3A4 */ TParamRT<s32> mSLChaseTimer;
	/* 0x3B8 */ TParamRT<f32> mSLBombRange;
};

class TFlyEnemy : public TWalkerEnemy {
public:
	TFlyEnemy(const char* name)
	    : TWalkerEnemy(name)
	    , unk198(0)
	    , unk19C(nullptr)
	    , unk1A0(0)
	    , unk1A5(1)
	    , unk1A6(0)
	{
	}

	virtual void init(TLiveManager*);
	virtual void bind();
	virtual f32 getGravityY() const;
	virtual void reset();
	virtual void setAfterDeadEffect() { }
	virtual void flyBehavior() { }
	virtual void setChaseFlyAnm() { }
	virtual void setNormalFlyAnm() { }

	void flyMove();
	void calcChaseParam();
	void fly();

	// fabricated
	TFlyEnemyParams* getFlyParams() const { return unk19C; }

	static f32 mTestSp;
	static int mInvalidTime;
	static f32 mTestMarioSpMax;

public:
	/* 0x194 */ f32 unk194;
	/* 0x198 */ s32 unk198;
	/* 0x19C */ TFlyEnemyParams* unk19C;
	/* 0x1A0 */ s32 unk1A0;
	/* 0x1A4 */ u8 unk1A4;
	/* 0x1A5 */ u8 unk1A5;
	/* 0x1A6 */ u8 unk1A6;
	/* 0x1A8 */ JGeometry::TVec3<f32> unk1A8;
};

class TKillerManager : public TSmallEnemyManager {
public:
	TKillerManager(const char* name);

	virtual void load(JSUMemoryInputStream&);
	virtual void createModelData();
	virtual TSpineEnemy* createEnemyInstance();
};

class TKiller : public TFlyEnemy {
public:
	TKiller(const char* name = "キラー");

	virtual void init(TLiveManager*);
	virtual void calcRootMatrix();
	virtual void bind();
	virtual const char** getBasNameTable() const;
	virtual void reset();
	virtual void genEventCoin();
	virtual void behaveToWater(THitActor*);
	virtual void changeOut();
	virtual void setDeadAnm();
	virtual void attackToMario();
	virtual void forceKill() { }
	virtual void setMActorAndKeeper();
	virtual bool isHitValid(u32);
	virtual bool isCollidMove(THitActor*);
	virtual bool isFindMario(f32);
	virtual void flyBehavior();
	virtual void setChaseFlyAnm();
	virtual void setNormalFlyAnm();

	void setColorType();
	bool isRollFly();

	// fabricated
	TKillerSaveLoadParams* getKillerParams() const
	{
		return (TKillerSaveLoadParams*)getSaveParam();
	}

	static u8 mSerialBomb;
	static u8 mSmokeJntNo;
	static u8 mTrampleDie;
	static u8 mRollSw;

public:
	/* 0x1B4 */ TKillerSaveLoadParams* unk1B4;
	/* 0x1B8 */ f32 unk1B8;
	/* 0x1BC */ TPosition3f unk1BC;
	/* 0x1EC */ GXColorS10 mNoseColor;
	/* 0x1F4 */ GXColorS10 mEyesColor;
	/* 0x1FC */ GXColorS10 mBodyColor;
	/* 0x204 */ GXColorS10 mBaseColor;
	/* 0x20C */ f32 unk20C;
};

DECLARE_NERVE(TNerveFlyEnemyNormalFly, TLiveActor);
DECLARE_NERVE(TNerveFlyEnemyChaseFly, TLiveActor);
DECLARE_NERVE(TNerveKillerExplosion, TLiveActor);

#endif
