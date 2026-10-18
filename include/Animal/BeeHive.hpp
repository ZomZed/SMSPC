#ifndef ANIMAL_BEE_HIVE_HPP
#define ANIMAL_BEE_HIVE_HPP

#include <Animal/fishoid.hpp>
#include <Strategic/Nerve.hpp>
#include <JSystem/JGeometry.hpp>

class TBeeHive;
class TMapObjBase;

class TBeeHiveParams : public TSpineEnemyParams {
public:
	TBeeHiveParams(const char* path);

	/* 0xA8 */ TParamRT<s32> mGiveupTimer;
	/* 0xBC */ TParamRT<f32> mGiveupRange;
	/* 0xD0 */ TParamRT<s32> mDecrimentTimer;
	/* 0xE4 */ TParamRT<f32> mRebound;
	/* 0xF8 */ TParamRT<f32> mDecay;
	/* 0x10C */ TParamRT<f32> mAngleMaxAdd;
	/* 0x120 */ TParamRT<f32> mShakePower;
	/* 0x134 */ TParamRT<f32> mFallAngularVel;
	/* 0x148 */ TParamRT<f32> mSearchRange;
};

class TBee : public TRealoidActor {
public:
	TBee(MActor* actor, TBeeHive* hive);

	virtual BOOL receiveMessage(THitActor*, u32);
	virtual void init();

	void behaveToEat();

public:
	/* 0xA8 */ TBeeHive* unkA8;
};

class TBeeHive : public TRealoid {
public:
	TBeeHive(const char* name);

	virtual void load(JSUMemoryInputStream&);
	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual BOOL receiveMessage(THitActor*, u32);
	virtual void init(TLiveManager*);
	virtual void calcRootMatrix();
	virtual void control();
	virtual void bind();
	virtual void reset();
	virtual TRealoidActor* createRealoidActor(MActor*);

	void initCollision();
	void initBoids();
	void setBoidBaseParams();
	void setBoidParamOnAttacking();
	void setBoidParamOnWaiting();
	void setBoidParamOnMarioWaterIn();
	void receiveMessageFromChild(TBee*);
	void loadCoin(JSUMemoryInputStream&);
	void controlCollision();
	void controlSound();
	void prepareWait();
	bool doWait();
	void prepareFall();
	bool doFall();
	void appearBee(int);
	void disappearBee(int);
	void decrimentBee();
	void appearAllBees();
	void dropCoin();
	void prepareBreak();
	void prepareVanish();
	void prepareReset();
	bool doScaling();
	bool isMissMario() const;
	void setShakePower(const JGeometry::TVec3<f32>&);
	JGeometry::TVec3<f32> getCenterOfGravity() const;
	bool isFalling() const;
	bool isWaiting() const;
	// fabricated
	f32 getSwingSpeedC() const { return mSwingSpeed; }
	bool isNotBreakOnceYet() const;

	// fabricated
	TBeeHiveParams* getParams() const
	{
		return (TBeeHiveParams*)getSaveParam();
	}

public:
	/* 0x158 */ JGeometry::TQuat4<f32> mShakeQuat;
	/* 0x168 */ JGeometry::TQuat4<f32> mCurQuat;
	/* 0x178 */ JGeometry::TQuat4<f32> mBaseQuat;
	/* 0x188 */ f32 mSwingAngle;
	/* 0x18C */ f32 mSwingSpeed;
	/* 0x190 */ f32 mSwingLimit;
	/* 0x194 */ JGeometry::TVec3<f32> mInitialPos;
	/* 0x1A0 */ JGeometry::TVec3<f32> mSoundPos;
	/* 0x1AC */ TMapObjBase* mItem;
	/* 0x1B0 */ int mCollisionIdx;
	/* 0x1B4 */ int mBeeNum;
	/* 0x1B8 */ TMapObjBase** mCoins;
	/* 0x1BC */ int mCoinIdx;
};

class TBeeHiveManager : public TEnemyManager {
public:
	TBeeHiveManager(const char* name);

	virtual void load(JSUMemoryInputStream&);
	virtual void createModelData();
};

DECLARE_NERVE(TNerveBeeHiveWait, TLiveActor);
DECLARE_NERVE(TNerveBeeHiveFall, TLiveActor);
DECLARE_NERVE(TNerveBeeHiveBreak, TLiveActor);
DECLARE_NERVE(TNerveBeeHiveAttack, TLiveActor);
DECLARE_NERVE(TNerveBeeHiveMarioWaterIn, TLiveActor);
DECLARE_NERVE(TNerveBeeHiveReset, TLiveActor);

#endif
