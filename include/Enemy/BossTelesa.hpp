#ifndef ENEMY_BOSS_TELESA_HPP
#define ENEMY_BOSS_TELESA_HPP

#include <Enemy/Enemy.hpp>
#include <Enemy/EnemyManager.hpp>
#include <Enemy/WalkerEnemy.hpp>
#include <MoveBG/MapObjSirena.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/Spine.hpp>

class TBossTelesa;
class TSharedParts;
class TTelesaManager;
class TMapCollisionMove;

DECLARE_NERVE(TNerveBossTelesaFallDemo, TLiveActor);
DECLARE_NERVE(TNerveBossTelesaFreeze, TLiveActor);
DECLARE_NERVE(TNerveBossTelesaPrepareSlot, TLiveActor);
DECLARE_NERVE(TNerveBossTelesaSpitSlotItem, TLiveActor);
DECLARE_NERVE(TNerveBossTelesaSlotStart, TLiveActor);
DECLARE_NERVE(TNerveBossTelesaAppear, TLiveActor);
DECLARE_NERVE(TNerveBossTelesaHideWait, TLiveActor);
DECLARE_NERVE(TNerveBossTelesaHide, TLiveActor);
DECLARE_NERVE(TNerveBossTelesaSpit, TLiveActor);
DECLARE_NERVE(TNerveBossTelesaDie, TLiveActor);

DECLARE_NERVE(TNerveBubbleSplit, TLiveActor);
DECLARE_NERVE(TNerveBubbleLive, TLiveActor);

class TBossTelesaSaveLoadParams : public TSpineEnemyParams {
public:
	TBossTelesaSaveLoadParams(const char* path);

	/* 0x0A8 */ TParamRT<s32> mSLDamageRadius;
	/* 0x0BC */ TParamRT<s32> mSLDamageHeight;
	/* 0x0D0 */ TParamRT<s32> mSLAttackRadius;
	/* 0x0E4 */ TParamRT<s32> mSLAttackHeight;
	/* 0x0F8 */ TParamRT<s32> mSLGenAttackerTime;
	/* 0x10C */ TParamRT<s32> mSLGenBubbleTime;
	/* 0x120 */ TParamRT<f32> mSLHitAngle;
	/* 0x134 */ TParamRT<s32> mSLNumGenBubble;
	/* 0x148 */ TParamRT<f32> mSL1stBubbleSp;
	/* 0x15C */ TParamRT<f32> mSLHideAreaRadius;
	/* 0x170 */ TParamRT<s32> mSLSlotItemNum;
	/* 0x184 */ TParamRT<s32> mSLSlotFruitNum;
	/* 0x198 */ TParamRT<f32> mSLSlotFirstHitCollectRate;
	/* 0x1AC */ TParamRT<f32> mSLSlotHitCollectRate;
	/* 0x1C0 */ TParamRT<f32> mSLTransYOffset;
	/* 0x1D4 */ TParamRT<s32> mSLStopSlotTime0;
	/* 0x1E8 */ TParamRT<s32> mSLStopSlotTime1;
	/* 0x1FC */ TParamRT<s32> mSLStopSlotTime2;
	/* 0x210 */ TParamRT<s32> mSLSpicyTime;
};

class TBossTelesaManager : public TEnemyManager {
public:
	TBossTelesaManager(const char* name);

	virtual void load(JSUMemoryInputStream&);
	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual void createModelData();
	virtual TSpineEnemy* createEnemyInstance();
	virtual void clipEnemies(JDrama::TGraphics*) { }
};

// The slot machine the boss carries in its mouth.
class TTelesaSlot : public TSlotDrum {
public:
	// TODO: the original ctor is emitted out of line (weak) in MarNameRefGen_MapObj
	TTelesaSlot(const char* name)
	    : TSlotDrum(name)
	    , unk19B(false)
	    , unk19C(true)
	    , unk1A4(2)
	    , unk1E0(0)
	{
		unk1A8[0] = false;
		unk1A8[1] = false;
		unk1A8[2] = false;
	}

	virtual void calcRootMatrix();
	virtual void moveObject();
	virtual void initMapObj();
	virtual u32 touchWater(THitActor*);
	virtual void initNeonMatColor() { }

	void entryObjCollision();
	void calcObjCollision();
	int getResultFromAng(f32);
	int getForcastResult(int);
	int getDrumResult(int);
	int getSlotResult();
	// fabricated: TNerveBossTelesaPrepareSlot keeps getSlotResult() out of
	// line behind a bool
	bool isSlotResult(int result) { return getSlotResult() == result; }
	bool isRollDrum();
	void forceStopSlot(int);
	void moveStart();
	void randomReset();

public:
	/* 0x198 */ bool unk198[3]; // drum is still being forced to roll
	/* 0x19B */ bool unk19B;
	/* 0x19C */ bool unk19C;
	/* 0x1A0 */ TBossTelesa* mOwner;
	/* 0x1A4 */ int unk1A4; // result the slot is being forced towards
	/* 0x1A8 */ bool unk1A8[3];
	/* 0x1AC */ JGeometry::TVec3<f32> unk1AC[4];
	/* 0x1DC */ TMapCollisionMove* unk1DC;
	/* 0x1E0 */ u8 unk1E0;
	/* 0x1E4 */ f32 unk1E4[3]; // roulette roll speeds
};

class TBossTelesaKillSmallEnemy : public THitActor {
public:
	TBossTelesaKillSmallEnemy(const char* name)
	    : THitActor(name)
	    , mOwner(nullptr)
	    , unk6C(0)
	{
	}

	void checkHit();

public:
	/* 0x68 */ TBossTelesa* mOwner;
	/* 0x6C */ u8 unk6C;
};

class TBossTelesaTongue : public THitActor {
public:
	TBossTelesaTongue(const char* name)
	    : THitActor(name)
	{
	}

	virtual BOOL receiveMessage(THitActor*, u32);

	void checkHit();

public:
	/* 0x68 */ TBossTelesa* mOwner;
};

class TBossTelesaBody : public THitActor {
public:
	TBossTelesaBody(const char* name)
	    : THitActor(name)
	    , unk6C(0)
	{
	}

	virtual BOOL receiveMessage(THitActor*, u32);

	void checkHit();

public:
	/* 0x68 */ TBossTelesa* mOwner;
	/* 0x6C */ u8 unk6C;
};

class TBossTelesa : public TSpineEnemy {
public:
	TBossTelesa(const char* name = "ボステレサ");

	virtual void loadAfter();
	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual BOOL receiveMessage(THitActor*, u32);
	virtual MtxPtr getTakingMtx();
	virtual void init(TLiveManager*);
	virtual void calcRootMatrix();
	virtual void moveObject();
	virtual void kill();
	virtual const char** getBasNameTable() const;
	virtual void reset();

	void fanfale();
	void forceHide();
	bool isForceRestart();
	void rollRouletteCircle();
	void forceAllItemKill();
	bool checkAllItemDead();
	void checkSlot();
	void fruitCollisionOn();
	void generateSlotItem();
	bool checkSlotResult();
	void slotStop();
	void slotStart();
	void rouletteStart();
	bool isInDamage();
	void setBckAnm(int);
	void genAttacker();
	void offAllCollision();
	void onAllCollision();
	void flashItem(int);
	void openWaterPlace();
	bool slotFall();
	bool rouletteFall();
	void tongueHitWater();
	void damageRecover();
	void setSpicy(TLiveActor*);
	void checkHitObject(THitActor*);
	BOOL checkMessage(THitActor*, u32);
	void prepareGenerate();

	TBossTelesaSaveLoadParams* getParams() const { return mParams; }

	// fabricated
	u8 getHitPointMax() const
	{
		return getSaveParam() ? getSaveParam()->mSLHitPointMax.get() : 1;
	}

	static f32 mEnemyGenRate;
	static f32 mItemGenRate;
	static u8 mNormalAlpha;
	static f32 mBaseHoseiPosY;
	static f32 mRouletteUpRate;
	static int mTelesaGenerateInterval;
	static f32 mCameraMoveLimit;
	static f32 mCameraMoveSp;
	static bool mGenerateTelesa;
	static bool mGenerateBubble;
	static int mEnemyTestType;

public:
	/* 0x150 */ bool unk150;
	/* 0x154 */ int unk154;
	/* 0x158 */ int unk158;
	/* 0x15C */ TBossTelesaSaveLoadParams* mParams;
	/* 0x160 */ int mCurBckAnm;
	/* 0x164 */ int mPrevBckAnm;
	/* 0x168 */ f32 mBlendRatio;
	/* 0x16C */ TBossTelesaBody* mBody;
	/* 0x170 */ TBossTelesaTongue* mTongue;
	/* 0x174 */ TBossTelesaKillSmallEnemy* mKillSmallEnemy;
	/* 0x178 */ TRoulette* mRoulette[3];
	/* 0x184 */ TTelesaSlot* mSlot;
	/* 0x188 */ TSharedParts* mSlotFrame;
	/* 0x18C */ bool unk18C; // slot is rolling
	/* 0x190 */ char unk190[0xC];
	/* 0x19C */ JGeometry::TVec3<f32> mRouletteSoundPos;
	/* 0x1A8 */ int mSlotResult;
	/* 0x1AC */ TLiveActor* mItems[50];
	/* 0x274 */ int mItemNum;
	/* 0x278 */ TMtx34f mTakingMtx;
	/* 0x2A8 */ TMapObjBase* mFruits[20];
	/* 0x2F8 */ TMapObjBase* mChilis[10];
	/* 0x320 */ TMapObjBase* mCoins[10];
	/* 0x348 */ GXColor unk348;
	/* 0x34C */ GXColor unk34C;
	/* 0x350 */ bool mIsSpicy;
	/* 0x354 */ TTelesaManager* mTelesaManager;
	/* 0x358 */ s16 mMarioHP;
	/* 0x35A */ bool unk35A;
	/* 0x35B */ bool unk35B;
	/* 0x35C */ int unk35C;
	/* 0x360 */ f32 mCameraOffsetY;
	/* 0x364 */ f32 unk364;
	/* 0x368 */ int unk368;
	/* 0x36C */ int unk36C;
	/* 0x370 */ u8 unk370;
	/* 0x374 */ JGeometry::TVec3<f32> mEffectPos;
	/* 0x380 */ int unk380;
	/* 0x384 */ bool mBGMStarted;
	/* 0x388 */ int unk388;
};

class TBubbleSaveLoadParams : public TWalkerEnemyParams {
public:
	TBubbleSaveLoadParams(const char* path);

	/* 0x32C */ TParamRT<s32> mSLLiveTime;
	/* 0x340 */ TParamRT<s32> mSLNumDivision;
	/* 0x354 */ TParamRT<f32> mSLMaxScale;
	/* 0x368 */ TParamRT<f32> mSLAddPosBase;
	/* 0x37C */ TParamRT<f32> mSLRateExpand;
	/* 0x390 */ TParamRT<f32> mSLDeadHeight;
};

class TBubbleManager : public TSmallEnemyManager {
public:
	TBubbleManager(const char* name);

	virtual void load(JSUMemoryInputStream&);
	virtual void createModelData();
	virtual TSpineEnemy* createEnemyInstance();
};

class TBubble : public TWalkerEnemy {
public:
	TBubble(const char* name = "バブル");

	virtual MtxPtr getTakingMtx();
	virtual void init(TLiveManager*);
	virtual void calcRootMatrix();
	virtual void kill();
	virtual f32 getGravityY() const;
	virtual const char** getBasNameTable() const;
	virtual void reset();
	virtual void behaveToWater(THitActor*);
	virtual void setDeadAnm();
	virtual void attackToMario();
	virtual void setAfterDeadEffect() { }

	void split();
	void appendItem();
	void appendEnemy();

public:
	/* 0x194 */ TBubbleSaveLoadParams* mParams;
	/* 0x198 */ TLiveActor* mCarried;
	/* 0x19C */ TPosition3f unk19C;
	/* 0x1CC */ f32 mPosOffsetY;
	/* 0x1D0 */ bool mIsSplit;
	/* 0x1D1 */ bool unk1D1;
	/* 0x1D2 */ bool unk1D2;
};

#endif
