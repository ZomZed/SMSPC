#ifndef ENEMY_BOSS_WANWAN_HPP
#define ENEMY_BOSS_WANWAN_HPP

#include <Enemy/Enemy.hpp>
#include <Enemy/EnemyManager.hpp>
#include <M3DUtil/M3UJoint.hpp>
#include <Strategic/Binder.hpp>
#include <Strategic/Nerve.hpp>
#include <JSystem/JDrama/JDRViewObj.hpp>

class TRope;
class TBossWanwan;
class TBWLeash;

class TBWParams : public TSpineEnemyParams {
public:
	TBWParams(const char*);

	/* 0xA8 */ TParamRT<f32> mSLMarchSpeed;
	/* 0xBC */ TParamRT<f32> mSLTurnSpeed;
	/* 0xD0 */ TParamRT<f32> mSLLeashNodeLen;
	/* 0xE4 */ TParamRT<f32> mSLPicketHeight;
	/* 0xF8 */ TParamRT<f32> mSLPicketRadius;
	/* 0x10C */ TParamRT<f32> mSLChainHitHeight;
	/* 0x120 */ TParamRT<f32> mSLChainHitRadius;
	/* 0x134 */ TParamRT<f32> mSLChainGroundRadius;
	/* 0x148 */ TParamRT<f32> mSLPullLimit;
	/* 0x15C */ TParamRT<f32> mSLAttackSpeed;
	/* 0x170 */ TParamRT<s32> mSLStunTimer;
	/* 0x184 */ TParamRT<f32> mSLSearchLength;
	/* 0x198 */ TParamRT<f32> mSLSearchAngle;
	/* 0x1AC */ TParamRT<u8> mSLBWHitPointMax;
	/* 0x1C0 */ TParamRT<f32> mSLHeadGap;
	/* 0x1D4 */ TParamRT<f32> mSLShakeLengthMax;
	/* 0x1E8 */ TParamRT<f32> mSLShakeLengthMaxHP0;
};

class TBWLeashNode : public THitActor {
public:
	TBWLeashNode(TBWLeash* owner, int index, const char* name);

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);

	void calcMatrix();
	void calcTemperature();

	f32 getTemperature() const { return mTemperature; }

public:
	/* 0x68 */ TBWLeash* mOwner;
	/* 0x6C */ MActor* mMActor;
	/* 0x70 */ f32 mTemperature;
	/* 0x74 */ int mIndex;
};

class TBWLeash : public JDrama::TViewObj {
public:
	TBWLeash(TBossWanwan* owner, int node_num, const char* name);

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);

	void pullTail(const JGeometry::TVec3<f32>&);
	void invalidateAllCollision();

	// fabricated
	TBWLeashNode* getNode(int i) { return mNodes[i]; }

public:
	/* 0x10 */ TBossWanwan* mOwner;
	/* 0x14 */ TRope* mRope;
	/* 0x18 */ TBWLeashNode** mNodes;
};

class TBWPicket : public TTakeActor {
public:
	TBWPicket(TBossWanwan* owner, const char* name);

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	virtual MtxPtr getTakingMtx();
	virtual BOOL moveRequest(const JGeometry::TVec3<f32>& where_to);

public:
	/* 0x70 */ TBossWanwan* mOwner;
	/* 0x74 */ TMtx34f mTakingMtx;
	/* 0xA4 */ MActor* mMActor;
};

class TBWHit : public THitActor {
public:
	TBWHit(TBossWanwan* owner, int joint_index, const char* name);

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);

public:
	/* 0x68 */ TBossWanwan* mOwner;
	/* 0x6C */ int mJointIndex;
};

class TBWBinder : public TBinder {
public:
	TBWBinder();
	virtual void bind(TLiveActor*);
};

class TBossWanwanMtxCalc : public M3UMtxCalcSIAnmBlendQuat {
public:
	TBossWanwanMtxCalc(TBossWanwan* owner);

	virtual void calc(u16);

	void joinAnm(int);

public:
	/* 0x64 */ TBossWanwan* mOwner;
};

class TBossWanwan : public TSpineEnemy {
public:
	TBossWanwan(const char* name);

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	virtual void init(TLiveManager*);
	virtual void calcRootMatrix();
	virtual void control();
	virtual void kill() { }

	void showMessage(u32);
	void emitEffects();
	void slideToCurPathNode(f32, f32);
	void rollNextGraphNode();
	void reverseNextGraphNode();
	BOOL isHeadPulled();
	bool isMarioInSight();
	bool isTailBurning();
	bool isBurning();
	void startGoldBrk();
	void changeBck(int);
	void releasePicket();
	void takeBath();
	void shakeCamera(int);

	// fabricated
	TBWParams* getParams() const { return (TBWParams*)getSaveParam(); }
	// fabricated
	f32 getHitPointRate() const
	{
		return (f32)mHitPoints / (f32)getParams()->mSLBWHitPointMax.get();
	}

public:
	/* 0x150 */ TBossWanwanMtxCalc* mMtxCalc;
	/* 0x154 */ TBWLeash* mLeash;
	/* 0x158 */ TBWPicket* mPicket;
	/* 0x15C */ JGeometry::TVec3<f32> mPullVec;
	/* 0x168 */ f32 mRollAngle;
	/* 0x16C */ int mIsRolling;
	/* 0x170 */ TBWHit* mHits[2];
	/* 0x178 */ f32 mBlendRate;
	/* 0x17C */ int mIsTied;
	/* 0x180 */ int unk180;
	/* 0x184 */ int unk184;
	/* 0x188 */ int unk188;
	/* 0x18C */ s8 mIsDead;
	/* 0x18D */ s8 mShineAppeared;
	/* 0x190 */ int unk190;
	/* 0x194 */ s8 unk194;
	/* 0x195 */ s8 mDived;
	/* 0x198 */ u32 mMessageFlags;
	/* 0x19C */ int unk19C;
	/* 0x1A0 */ s8 unk1A0;
	/* 0x1A4 */ JGeometry::TVec3<f32> mEffectPos;
	/* 0x1B0 */ int unk1B0;
	/* 0x1B4 */ s16 unk1B4;
};

class TBossWanwanManager : public TEnemyManager {
public:
	TBossWanwanManager(const char* name);

	virtual void load(JSUMemoryInputStream&);
	virtual void createModelData();
	virtual TSpineEnemy* createEnemyInstance();

	void initJParticle();
};

DECLARE_NERVE(TNerveBWGraphWander, TLiveActor);
DECLARE_NERVE(TNerveBWRoll, TLiveActor);
DECLARE_NERVE(TNerveBWBark, TLiveActor);
DECLARE_NERVE(TNerveBWJump, TLiveActor);
DECLARE_NERVE(TNerveBWStun, TLiveActor);
DECLARE_NERVE(TNerveBWWakeup, TLiveActor);
DECLARE_NERVE(TNerveBWJumpToBath, TLiveActor);
DECLARE_NERVE(TNerveBWDie, TLiveActor);
DECLARE_NERVE(TNerveBWJumpAway, TLiveActor);
DECLARE_NERVE(TNerveBWShake, TLiveActor);
DECLARE_NERVE(TNerveBWFall, TLiveActor);

#endif
