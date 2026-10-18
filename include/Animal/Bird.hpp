#ifndef ANIMAL_BIRD_HPP
#define ANIMAL_BIRD_HPP

#include <Enemy/Enemy.hpp>
#include <Enemy/EnemyManager.hpp>
#include <System/ParamInst.hpp>
#include <dolphin/gx.h>

class TMapObjBase;
class TWireBinder;

class TAnimalBirdParams : public TSpineEnemyParams {
public:
	TAnimalBirdParams(const char*);

	/* 0xA8 */ TParamRT<f32> mMarchSpeed;
	/* 0xBC */ TParamRT<f32> mTurnSpeed;
	/* 0xD0 */ TParamRT<s32> mReturnTimer;
	/* 0xE4 */ TParamRT<f32> mSearchLength;
	/* 0xF8 */ TParamRT<f32> mSearchHeight;
	/* 0x10C */ TParamRT<f32> mSearchAware;
	/* 0x120 */ TParamRT<f32> mSearchAngle;
	/* 0x134 */ TParamRT<s32> mActionTimer;
	/* 0x148 */ TParamRT<s32> mWaterproofTimerMax;
	/* 0x15C */ TParamRT<s32> mFloatingTimerMax;
	/* 0x170 */ TParamRT<f32> mLandingGravityY;
	/* 0x184 */ TParamRT<f32> mLandingTorqueY;
	/* 0x198 */ TParamRT<f32> mWalkingTorqueY;
	/* 0x1AC */ TParamRT<f32> mWalkingSpeed;
	/* 0x1C0 */ TParamRT<s32> mWalkTimer;
	/* 0x1D4 */ TParamRT<f32> mLandingFric;
	/* 0x1E8 */ TParamRT<s32> mActionTimerAdd;
	/* 0x1FC */ TParamRT<f32> mWaterPowerY;
};

class TAnimalBird : public TSpineEnemy {
public:
	TAnimalBird(const char* name);

	virtual void load(JSUMemoryInputStream&);
	virtual void loadAfter();
	virtual BOOL receiveMessage(THitActor*, u32);
	virtual void init(TLiveManager*);
	virtual void calcRootMatrix();
	virtual void bind();
	virtual void moveObject();
	virtual const char** getBasNameTable() const;

	f32 getFootGroundHeight();
	f32 getWaterPowerY() const;
	f32 getWaterDamageRate() const;
	f32 getMyMarchSpeed() const;
	void setBckAnm(int);
	void setParamsOnLanding();
	void setParamsOnFloating();
	void setGoalToComeback();
	void doGotoRandomNextGraphNode();
	bool doLanding(bool);
	void doWalk();
	void doFlyToCurPathNode();
	void doDropCoin();
	bool isFlying() const;
	bool isChanged() const;
	bool isCheckWithWireBinder() const;
	bool isGroundShaken() const;
	bool isChangeToItem() const;
	bool isFindMario() const;
	bool isWantToRest() const;
	bool isWantToAction() const;
	bool isWantToFly() const;
	void updateSound();
	void checkNotAppear(long);
	void checkChangeToItem();
	void checkFalling();
	bool isOnGroundNerve() const;
	void behaveHitWater();
	void initParams();
	void initCollision();
	void initTevColor(const GXColorS10*);

	// fabricated
	TAnimalBirdParams* getBirdParams() const
	{
		return (TAnimalBirdParams*)getSaveParam();
	}

public:
	/* 0x150 */ TMapObjBase* mItem;
	/* 0x154 */ TWireBinder* mWireBinder;
	/* 0x158 */ JGeometry::TVec3<f32> mInitialPos;
	/* 0x164 */ JGeometry::TVec3<f32> mInitialRot;
	/* 0x170 */ f32 mWalkDir;
	/* 0x174 */ f32 mSizeRate;
	/* 0x178 */ s32 mWaterproofTimer;
	/* 0x17C */ s32 mFloatingTimer;
	/* 0x180 */ s32 mColorIdx;
};

class TAnimalBirdManager : public TEnemyManager {
public:
	TAnimalBirdManager(const char* name);

	virtual void load(JSUMemoryInputStream&);
	virtual void loadAfter();
	virtual void createModelData();
};

DECLARE_NERVE(TNerveAnimalBirdWaitOnGround, TLiveActor);
DECLARE_NERVE(TNerveAnimalBirdActionOnGround, TLiveActor);
DECLARE_NERVE(TNerveAnimalBirdWalkOnGround, TLiveActor);
DECLARE_NERVE(TNerveAnimalBirdTakeoff, TLiveActor);
DECLARE_NERVE(TNerveAnimalBirdGraphWander, TLiveActor);
DECLARE_NERVE(TNerveAnimalBirdChangeToCoin, TLiveActor);
DECLARE_NERVE(TNerveAnimalBirdComeback, TLiveActor);
DECLARE_NERVE(TNerveAnimalBirdPreLanding, TLiveActor);
DECLARE_NERVE(TNerveAnimalBirdLanding, TLiveActor);

#endif
