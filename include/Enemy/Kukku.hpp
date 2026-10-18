#ifndef ENEMY_KUKKU_HPP
#define ENEMY_KUKKU_HPP

#include <Enemy/SmallEnemy.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/Spine.hpp>
#include <JSystem/JGeometry/JGVec3.hpp>

class MActor;
class TMapObjBase;

class TKukkuParams : public TSmallEnemyParams {
public:
	TKukkuParams(const char* path);

	/* 0x2D4 */ TParamRT<f32> mMarchSpeed;
	/* 0x2E8 */ TParamRT<f32> mTurnSpeed;
	/* 0x2FC */ TParamRT<f32> mWaterPowerY;
	/* 0x310 */ TParamRT<f32> mShootSpeed;
	/* 0x324 */ TParamRT<s32> mShootInterval;
	/* 0x338 */ TParamRT<f32> mSearchRange;
	/* 0x34C */ TParamRT<s32> mHabatakiTimer;
	/* 0x360 */ TParamRT<f32> mAirFric;
	/* 0x374 */ TParamRT<f32> mUpperVelocityY;
	/* 0x388 */ TParamRT<f32> mDropSpeed;
	/* 0x39C */ TParamRT<f32> mDropAngleX;
};

// Projectile dropped by a TKukku onto Mario. Pollutes the ground on impact.
class TKukkuBall : public THitActor {
public:
	TKukkuBall(MActor* mactor);

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual void init();

	bool isDead() const;
	void kill();
	void startToMove(const JGeometry::TVec3<f32>& position,
	                 const JGeometry::TVec3<f32>& velocity);
	void checkHitActors();
	void bind();
	void moveObject();

	enum {
		KUKKU_BALL_FLAG_DEAD   = 0x1,
		KUKKU_BALL_FLAG_HIDDEN = 0x4,
	};

public:
	/* 0x68 */ MActor* mMActor;
	/* 0x6C */ u32 mBallFlags;
	/* 0x70 */ JGeometry::TVec3<f32> mVelocity;
	/* 0x7C */ u32 unk7C;
};

// Entirely deadstripped helper class, only known from the map.
class TEnemyCoinUnit {
public:
	TEnemyCoinUnit(int num);

	void init();
	TMapObjBase* getUnusedItem();
};

class TKukku : public TSmallEnemy {
public:
	TKukku(const char* name = "クック");

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	virtual void init(TLiveManager* manager);
	virtual void calcRootMatrix();
	virtual void control();
	virtual void bind();
	virtual const char** getBasNameTable() const;
	virtual void reset();
	virtual void behaveToWater(THitActor* actor);
	virtual void setDeadAnm();
	virtual void setAfterDeadEffect();

	void initParticle();
	void initCollision();
	void initBalls();
	void updateEffect();
	void behaveHitTrample();
	void doFlyToCurPathNode();
	void updateRotation();
	JGeometry::TVec3<f32> calcMomentum(f32 speed);
	void doRecoverToCurPathNode();
	void doHabataki();
	void decideFlyingAnm();
	void shotBall();
	void dropCoins();
	bool isFalling() const;
	bool isRecoveringGraph() const;
	bool isDying() const;
	bool isFindOutMario() const;
	void changeBck(const char* name);
	f32 getWaterDamageRate() const;
	f32 getWaterPowerY() const;
	TKukkuBall* getUnusedBall();

	// fabricated
	TKukkuParams* getParams() const { return (TKukkuParams*)getSaveParam(); }

	// fabricated: const reads keep the ROM's reload before the decrements
	int getUnk1A4() const { return unk1A4; }
	int getShootTimer() const { return mShootTimer; }

public:
	/* 0x194 */ TKukkuBall* mBalls[3];
	/* 0x1A0 */ TMapObjBase* m1upMushroom;
	/* 0x1A4 */ int unk1A4;
	/* 0x1A8 */ int mCenterJointIndex;
	/* 0x1AC */ int mShootTimer;
	/* 0x1B0 */ int mDroppedCoinNum;
};

class TKukkuManager : public TSmallEnemyManager {
public:
	TKukkuManager(const char* name = "クックマネージャー");

	virtual void load(JSUMemoryInputStream& stream);
	virtual void createModelData();

	void initCoins();

public:
	/* 0x60 */ u32 unk60;
};

DECLARE_NERVE(TNerveKukkuRecoverGraph, TLiveActor);
DECLARE_NERVE(TNerveKukkuPostFall, TLiveActor);
DECLARE_NERVE(TNerveKukkuFall, TLiveActor);
DECLARE_NERVE(TNerveKukkuHit, TLiveActor);
DECLARE_NERVE(TNerveKukkuGraphWander, TLiveActor);

#endif
