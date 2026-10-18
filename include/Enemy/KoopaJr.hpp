#ifndef ENEMY_KOOPAJR_HPP
#define ENEMY_KOOPAJR_HPP

#include <Enemy/Enemy.hpp>
#include <Enemy/EnemyManager.hpp>
#include <JSystem/JDrama/JDRGraphics.hpp>
#include <JSystem/JGeometry/JGVec3.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/Spine.hpp>
#include <System/BaseParam.hpp>
#include <dolphin/types.h>

class JSUMemoryInputStream;
class TLiveManager;
class TBathtub;
class TBathtubBinder;
class TBathtubKiller;
class TBathtubKillerManager;
class TKoopa;
class TKoopaJr;

class TDirectionCalc {
public:
	TDirectionCalc(JGeometry::TVec3<f32>);
	TDirectionCalc(f32);
	TDirectionCalc();

	static f32 r2d(f32);
	static f32 d2r(f32);
	f32 absDirection(f32);
	JGeometry::TVec3<f32> calcDirectionVector();
	void makeDirection(JGeometry::TVec3<f32>);
	f32 calcTurnDirection(f32, f32);
	f32 sub(f32);
	f32 calcNearerDirection(f32);
	void normalize();

public:
	/* 0x0 */ f32 unk0;
};

class TCallbackHitActor : public THitActor {
public:
	TCallbackHitActor(const char* name, u32 actor_type, f32 damage_radius,
	                  f32 damage_height, THitActor* owner);

	virtual BOOL receiveMessage(THitActor* sender, u32 message);

public:
	/* 0x68 */ THitActor* mOwner;
};

class TKoopaJrSubmarineManager : public TEnemyManager {
public:
	TKoopaJrSubmarineManager(const char* name
	                         = "クッパジュニアサブマリンマネージャー");

	virtual void load(JSUMemoryInputStream&);
	virtual void loadAfter();
	virtual void createModelData();
	virtual TSpineEnemy* createEnemyInstance();
};

DECLARE_NERVE(TNerveKoopaJrSubmarineWait, TLiveActor);
DECLARE_NERVE(TNerveKoopaJrSubmarineCannonOpenClose, TLiveActor);
DECLARE_NERVE(TNerveKoopaJrSubmarineLaunchKiller, TLiveActor);

class TKoopaJrSubmarineParams : public TSpineEnemyParams {
public:
	TKoopaJrSubmarineParams(const char*);

public:
	/* 0x0A8 */ TParamRT<f32> killerTargetDistanceMin;
	/* 0x0BC */ TParamRT<f32> killerTargetDistance;
	/* 0x0D0 */ TParamRT<f32> bottomHeight;
	/* 0x0E4 */ TParamRT<f32> centerZ;
	/* 0x0F8 */ TParamRT<f32> aboidKoopaFlameAngle;
	/* 0x10C */ TParamRT<f32> traceMarioAngle;
	/* 0x120 */ TParamRT<f32> mSLWavePhaseVelocity;
	/* 0x134 */ TParamRT<f32> mSLWaveAmplitudeMin;
	/* 0x148 */ TParamRT<f32> mSLWaveAmplitudeMaxLaunch;
	/* 0x15C */ TParamRT<f32> mSLWaveAmplitudeMax;
	/* 0x170 */ TParamRT<f32> mSLSwingPhaseVelocity;
	/* 0x184 */ TParamRT<f32> mSLSwingAmplitudeMin;
	/* 0x198 */ TParamRT<f32> mSLSwingAmplitudeMax;
	/* 0x1AC */ TParamRT<f32> mSLRoundAngleVelocity;
	/* 0x1C0 */ TParamRT<f32> mSLRoundDistance;
	/* 0x1D4 */ TParamRT<f32> mSLAcceleration;
	/* 0x1E8 */ TParamRT<f32> mSLRotationSpeed;
	/* 0x1FC */ TParamRT<f32> mSLSpeedMax;
	/* 0x210 */ TParamRT<f32> mSLKoopaJrSubmarineScale;
	/* 0x224 */ TParamRT<f32> mSLDamageRadius;
	/* 0x238 */ TParamRT<f32> mSLDamageHeight;
	/* 0x24C */ TParamRT<f32> shineKillerProbability0;
	/* 0x260 */ TParamRT<f32> shineKillerProbability1;
	/* 0x274 */ TParamRT<s32> mSLKillerIntervalFast;
	/* 0x288 */ TParamRT<s32> mSLKillerInterval;
};

class TKoopaJrSubmarine : public TSpineEnemy {
public:
	TKoopaJrSubmarine(const char* name = "クッパジュニアサブマリン");

	virtual void perform(u32, JDrama::TGraphics*);
	virtual BOOL receiveMessage(THitActor*, u32);
	virtual void init(TLiveManager*);
	virtual void calcRootMatrix();
	virtual void bind();
	virtual const char** getBasNameTable() const;
	virtual void reset();

	void checkNerve();
	void makeDirection();
	void makeRoundVelocity();
	void makeRelativeAngle();
	void setKoopaJr(TKoopaJr*);
	void updateTimers();
	void emitKoopaJrSubmarineEffects();
	void makeKillerVelocity(TBathtubKiller*, JGeometry::TVec3<f32>);
	void launchKiller();
	void checkKillerLaunch();
	bool appearShineKiller(int);
	void prepareKillerLaunchFast(int);
	void prepareKillerLaunch(int);
	void setAnimationIndex(int);
	void damageKoopaJrSubmarine();
	f32 getWaveAngle();
	f32 getSwingAngle();
	void moveSwing();
	void makeCollisionPositions();
	void resetKoopaJrSubmarine();

	// fabricated: see TKoopa::pushNerve
	void pushNerve(const TNerveBase<TLiveActor>* nerve);

	// fabricated
	TKoopaJrSubmarineParams* getParams() const
	{
		return (TKoopaJrSubmarineParams*)getSaveParam();
	}

public:
	/* 0x150 */ s32 unk150[1]; // timers: killer launch interval
	/* 0x154 */ f32 unk154;
	/* 0x158 */ f32 unk158;
	/* 0x15C */ f32 unk15C;
	/* 0x160 */ f32 unk160;
	/* 0x164 */ TDirectionCalc unk164; // angle around the bathtub
	/* 0x168 */ f32 unk168;            // distance from the bathtub
	/* 0x16C */ TDirectionCalc unk16C; // facing direction
	/* 0x170 */ u8 unk170;
	/* 0x174 */ TBathtubBinder* mBathtubBinder;
	/* 0x178 */ u8 unk178[8]; // killer types to launch
	/* 0x180 */ s32 unk180;   // index of the next killer to launch
	/* 0x184 */ s32 unk184;   // number of killers to launch
	/* 0x188 */ f32 unk188;   // cannon animation rate
	/* 0x18C */ u8 unk18C;    // hit by water
	/* 0x190 */ f32 unk190;   // swing amplitude
	/* 0x194 */ f32 unk194;   // swing phase
	/* 0x198 */ f32 unk198;   // wave amplitude
	/* 0x19C */ f32 unk19C;   // wave phase
	/* 0x1A0 */ TKoopaJr* mKoopaJr;
	/* 0x1A4 */ TCallbackHitActor* mRearBody;
	/* 0x1A8 */ TCallbackHitActor* mFrontBody;
};

class TKoopaJrManager : public TEnemyManager {
public:
	TKoopaJrManager(const char* name = "クッパジュニアマネージャー");

	virtual void load(JSUMemoryInputStream&);
	virtual void loadAfter();
	virtual void createModelData();
	virtual TSpineEnemy* createEnemyInstance();
};

DECLARE_NERVE(TNerveKoopaJrWait, TLiveActor);
DECLARE_NERVE(TNerveKoopaJrDamage, TLiveActor);
DECLARE_NERVE(TNerveKoopaJrDemo, TLiveActor);
DECLARE_NERVE(TNerveKoopaJrLaunch, TLiveActor);
DECLARE_NERVE(TNerveKoopaJrYahoo, TLiveActor);

class TKoopaJrParams : public TSpineEnemyParams {
public:
	TKoopaJrParams(const char*);

public:
	/* 0x0A8 */ TParamRT<f32> mSLLaunchKillerLimit;
	/* 0x0BC */ TParamRT<f32> mSLDamageRadius;
	/* 0x0D0 */ TParamRT<f32> mSLDamageHeight;
	/* 0x0E4 */ TParamRT<f32> mSLKoopaJrScale;
	/* 0x0F8 */ TParamRT<f32> mSLFastLaunchDistance;
	/* 0x10C */ TParamRT<s32> mSLDamagePeriod;
	/* 0x120 */ TParamRT<s32> mSLLaunchKillerPeriod;
	/* 0x134 */ TParamRT<s32> mSLLaunchKillerPeriodFast;
};

class TKoopaJr : public TSpineEnemy {
public:
	TKoopaJr(const char* name = "クッパジュニア");

	virtual void perform(u32, JDrama::TGraphics*);
	virtual BOOL receiveMessage(THitActor*, u32);
	virtual void init(TLiveManager*);
	virtual void calcRootMatrix();
	virtual const char** getBasNameTable() const;
	virtual void reset();

	f32 getBathtubY();
	void checkNerveKillerHit();
	void checkNerveKillerLaunchFast();
	void checkNerveKillerLaunchNormal();
	void checkNerve();
	void startDamageNerve();
	void checkSubmarineSwing();
	void damageKoopaJr();
	void updateTimers();
	void setAnimationIndex(int);
	void emitKoopaJrEffects();
	void startKoopaJrMessage(u32);
	void resetKoopaJr();

	// fabricated: see TKoopa::pushNerve
	void pushNerve(const TNerveBase<TLiveActor>* nerve);

	// fabricated
	TKoopaJrParams* getParams() const
	{
		return (TKoopaJrParams*)getSaveParam();
	}

public:
	/* 0x150 */ s32 unk150[3]; // timers: damage, killer launch, fast launch
	/* 0x15C */ TBathtub* mBathtub;
	/* 0x160 */ TKoopa* mKoopa;
	/* 0x164 */ TKoopaJrSubmarine* mSubmarine;
	/* 0x168 */ TKoopaJrSubmarineManager* mSubmarineManager;
	/* 0x16C */ TBathtubKillerManager* mKillerManager;
};

#endif
