#ifndef ENEMY_TIN_KOOPA_HPP
#define ENEMY_TIN_KOOPA_HPP

#include <Enemy/Enemy.hpp>
#include <Enemy/EnemyManager.hpp>
#include <M3DUtil/M3UJoint.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/Spine.hpp>
#include <Strategic/LiveActor.hpp>

class TTinKoopa;
class TCoasterKiller;
class TCoasterKillerManager;
class TGraphWeb;
class TMapCollisionMove;
class MActor;

class TTinKoopaParams : public TSpineEnemyParams {
public:
	TTinKoopaParams(const char* path);

	/* 0xA8 */ TParamRT<s32> mSLPartsHP;
	/* 0xBC */ TParamRT<s32> mSLFlameHP;
	/* 0xD0 */ TParamRT<s32> mSLFlameRevivalTime;
	/* 0xE4 */ TParamRT<f32> mSLFlameDamageRadius0;
	/* 0xF8 */ TParamRT<f32> mSLFlameDamageHeight0;
	/* 0x10C */ TParamRT<f32> mSLFlameDamageRadius1;
	/* 0x120 */ TParamRT<f32> mSLFlameDamageHeight1;
	/* 0x134 */ TParamRT<f32> mSLDamageRadius;
	/* 0x148 */ TParamRT<f32> mSLDamageHeight0;
	/* 0x15C */ TParamRT<f32> mSLDamageHeight1;
	/* 0x170 */ TParamRT<s32> mSLKillerInterval;
	/* 0x184 */ TParamRT<s32> mSLDefeatWaitTime;
	/* 0x198 */ TParamRT<f32> mSLKillerApproachingDistance;
};

class TTinKoopaFlame : public THitActor {
public:
	TTinKoopaFlame(const char* name, TTinKoopa* owner);
	virtual ~TTinKoopaFlame() { }

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);

	void emitFlameEffects();
	bool isHighPosition();
	void checkMario();
	void hitWater();
	void resetTinKoopaFlame();
	void makeHitCollision();

public:
	/* 0x68 */ TTinKoopa* mOwner;
	/* 0x6C */ f32 mFlameScale;
	/* 0x70 */ s16 mFlameHP;
	/* 0x72 */ u8 mHitByWater;
};

class TTinKoopaLaunchOrder {
public:
	TTinKoopaLaunchOrder(TTinKoopa* owner);

	void checkOrder();
	void makeOrder(s8 lap, long frame, s8 num, s8 dir);

public:
	/* 0x0 */ TTinKoopa* mOwner;
	/* 0x4 */ s8 mLap;
	/* 0x8 */ long mFrame;
	/* 0xC */ s8 mNum;
	/* 0xD */ u8 mDir;
};

class TTinKoopaLaunchSchedule {
public:
	TTinKoopaLaunchSchedule(u8 num, TTinKoopa* owner);

	void checkOrder();

public:
	/* 0x0 */ u8 mOrderNum;
	/* 0x4 */ TTinKoopa* mOwner;
	/* 0x8 */ TTinKoopaLaunchOrder** mOrders;
};

class TTinKoopaPartsBase : public TLiveActor {
public:
	TTinKoopaPartsBase(const char* name, int index, TTinKoopa* owner);
	virtual ~TTinKoopaPartsBase() { }

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	virtual void reset();

	void initTinKoopaPartsBase();
	void resetTinKoopaPartsBase();
	void startBreaking();
	void emitPartsTrackEffects();
	void emitPartsTrackEffects(const char** joint_names, int num);
	void emitPartsDisappearEffects();
	void emitPartsDisappearEffects(const char** joint_names, int num,
	                               f32 scale);

public:
	/* 0xF4 */ TMapCollisionMove* mCollision;
	/* 0xF8 */ u8 mIsBreaking;
	/* 0xFC */ int mPartsIndex;
	/* 0x100 */ TTinKoopa* mOwner;
	/* 0x104 */ MActor* mBreakMActor;
	/* 0x108 */ JGeometry::TVec3<f32> mEffectPos[6];
};

class TTinKoopaMtxCalc : public M3UMtxCalcSIAnmBlendQuat {
public:
	TTinKoopaMtxCalc(TTinKoopa* owner);

	virtual void calc(u16);

	void joinAnm(int);

public:
	/* 0x64 */ TTinKoopa* mOwner;
};

class TTinKoopa : public TSpineEnemy {
public:
	TTinKoopa(const char* name);
	virtual ~TTinKoopa() { }

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	virtual void init(TLiveManager* manager);
	virtual BOOL hasMapCollision() const { return true; }
	virtual const char** getBasNameTable() const;
	virtual void reset();

	void emitTinKoopaEffects();
	void startTinKoopaMessage(u32 message);
	void checkTinKoopaFirstFlameMessage();
	void checkTinKoopaFirstRocketMessage();
	void checkTinKoopaKillerApproachingMessage();
	void checkTinKoopaMessage();
	void checkKillerLaunch();
	void launchKiller(int dir);
	void startBreakingParts();
	void hitParts();
	void changeBck(int index);
	void updateTimers();
	void makeEyeBeamEffect();
	bool checkTruckAnimationPass(int frame);
	void checkLap();
	void makeKillerQueue(int num, s8 dir);
	void makeHitCollision();
	void resetTinKoopa();
	bool checkKillerApproachingFromBack(TCoasterKiller* killer,
	                                    JGeometry::TVec3<f32> pos,
	                                    f32 distance);
	f32 calcCoasterDistanceInOrder(int from, int to);
	f32 calcCoasterDistance(int from, int to);
	void makeCoasterDistanceTable();
	void makeLaunchSchedule();

	// fabricated
	TTinKoopaParams* getParams() const
	{
		return (TTinKoopaParams*)getSaveParam();
	}

public:
	/* 0x150 */ int mBreakStage;
	/* 0x154 */ int unk154;
	/* 0x158 */ int unk158;
	/* 0x15C */ int mLap;
	/* 0x160 */ TTinKoopaFlame* mFlame;
	/* 0x164 */ MActor* mKoopaRail;
	/* 0x168 */ u8 mFirstFlameMessageShown;
	/* 0x169 */ s8 mKillerQueue[4];
	/* 0x170 */ int mKillerQueueNum;
	/* 0x174 */ int mKillerQueueIndex;
	/* 0x178 */ int mKillerTimer;
	/* 0x17C */ int mFlameRevivalTimer;
	/* 0x180 */ int mDefeatTimer;
	/* 0x184 */ JGeometry::TVec3<f32> mHeadPos;
	/* 0x190 */ JGeometry::TVec3<f32> mBreastPos;
	/* 0x19C */ JGeometry::TVec3<f32> mRArmPos;
	/* 0x1A8 */ JGeometry::TVec3<f32> mLArmPos;
	/* 0x1B4 */ f32 unk1B4;
	/* 0x1B8 */ f32 unk1B8;
	/* 0x1BC */ f32 unk1BC;
	/* 0x1C0 */ f32 unk1C0;
	/* 0x1C4 */ TTinKoopaMtxCalc* unk1C4;
	/* 0x1C8 */ int mPartsHP;
	/* 0x1CC */ TTinKoopaPartsBase* mParts[6];
	/* 0x1E4 */ TTinKoopaPartsBase* mBreakingParts;
	/* 0x1E8 */ f32* mCoasterDistanceTable;
	/* 0x1EC */ TGraphWeb* mKillerGraph;
	/* 0x1F0 */ TCoasterKillerManager* mKillerManager;
	/* 0x1F4 */ TTinKoopaLaunchSchedule* mLaunchSchedule;
	/* 0x1F8 */ int unk1F8;
	// TODO: the object ends at 0x1FC (US new size); `&unk1F8 + 1` is used as an
	// emitter id in the original
};

class TTinKoopaManager : public TEnemyManager {
public:
	TTinKoopaManager(const char* name);
	virtual ~TTinKoopaManager() { }

	virtual void load(JSUMemoryInputStream&);
	virtual void loadAfter();
	virtual void createModelData();
	virtual BOOL hasMapCollision() const { return true; }
	virtual TSpineEnemy* createEnemyInstance();
};

DECLARE_NERVE(TNerveTinKoopaWait, TLiveActor);
DECLARE_NERVE(TNerveTinKoopaDamage, TLiveActor);
DECLARE_NERVE(TNerveTinKoopaBreak, TLiveActor);

#endif
