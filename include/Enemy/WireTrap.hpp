#ifndef ENEMY_WIRE_TRAP_HPP
#define ENEMY_WIRE_TRAP_HPP

#include <Enemy/Enemy.hpp>
#include <Enemy/EnemyManager.hpp>
#include <Enemy/WireBinder.hpp>
#include <JSystem/JGeometry.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/Spine.hpp>
#include <dolphin/gx.h>
#include <dolphin/types.h>

class JSUMemoryInputStream;
class THitActor;
class TLiveManager;

DECLARE_NERVE(TNerveWireTrapGoWait, TLiveActor);
DECLARE_NERVE(TNerveWireTrapWait, TLiveActor);
DECLARE_NERVE(TNerveWireTrapSearch, TLiveActor);
DECLARE_NERVE(TNerveWireTrapOnewayMoveEnd, TLiveActor);
DECLARE_NERVE(TNerveWireTrapOnewayMove, TLiveActor);
DECLARE_NERVE(TNerveWireTrapOnewayMoveStart, TLiveActor);
DECLARE_NERVE(TNerveWireTrapReturnMove, TLiveActor);

class TWireTrapParams : public TSpineEnemyParams {
public:
	TWireTrapParams(const char* path);

	/* 0xA8 */ TParamRT<f32> mInWaterPowerRate;
	/* 0xBC */ TParamRT<s32> mScaleTimerMax;
	/* 0xD0 */ TParamRT<s32> mGoTimerMax;
};

class TWireTrap : public TSpineEnemy {
public:
	// Movement modes, selected per-instance from the scene file
	enum {
		WIRE_TRAP_MODE_RETURN = 0,
		WIRE_TRAP_MODE_ONEWAY = 1,
		WIRE_TRAP_MODE_SEARCH = 2,
	};

	TWireTrap(const char* name);
	virtual ~TWireTrap() { }

	virtual void load(JSUMemoryInputStream&);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	virtual void init(TLiveManager* manager);
	virtual void calcRootMatrix();
	virtual void moveObject();
	virtual void kill();

	bool isEndWire() const;
	void isStartWire() const;
	TWireBinder* getWireBinder() const;
	TWireBinder* getWireBinder();
	void getRangePosInWire() const;
	static const TNerveBase<TLiveActor>* getNerveFromMode(int mode);
	void isReflect() const;
	const JGeometry::TVec3<f32>& getWireDir() const;
	JGeometry::TVec3<f32> getDirAtWirePos() const;
	void setMoveMode(int mode);
	void emitEffects();
	void updateCollision();
	void checkHitActors();
	void doResetToEdge();
	bool doScaleDown();
	bool doScaleUp();
	bool doSearchMove();
	bool doOnewayMove();
	bool doReturnMove();
	f32 getWaterPow() const;
	JGeometry::TVec3<f32> calcMomentum();
	void behaveHitWireTrap(TWireTrap* other,
	                       const JGeometry::TVec3<f32>& my_momentum,
	                       const JGeometry::TVec3<f32>& other_momentum);
	void behaveHitWater(THitActor* sender);
	void initThisColor(const GXColorS10* color);
	void initWire();
	void initParticle();
	void initCollision();

	// fabricated
	TWireTrapParams* getParams() const
	{
		return (TWireTrapParams*)getSaveParam();
	}

	// fabricated: const reads keep the ROM's reload before the decrements
	int getWaterHitTimer() const { return mWaterHitTimer; }
	int getHitTimer() const { return mHitTimer; }

public:
	/* 0x150 */ u32 unk150[4]; // TODO: unknown, never touched by this TU
	/* 0x160 */ int unk160;
	/* 0x164 */ int mWaterHitTimer;
	/* 0x168 */ f32 mWaterPower;
	/* 0x16C */ int mHitTimer;
	/* 0x170 */ f32 mDirection; // +1 or -1 along the wire
	/* 0x174 */ int mWaitTime;
	/* 0x178 */ f32 mScale;
	/* 0x17C */ f32 mSpeed;
	/* 0x180 */ s16 mMode;
};

class TWireTrapManager : public TEnemyManager {
public:
	TWireTrapManager(const char* name);
	virtual ~TWireTrapManager() { }

	virtual void load(JSUMemoryInputStream&);
	virtual void createModelData();
};

#endif
