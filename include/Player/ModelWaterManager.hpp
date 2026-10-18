#ifndef PLAYER_MODEL_WATER_MANAGER_HPP
#define PLAYER_MODEL_WATER_MANAGER_HPP

#include <JSystem/JDrama/JDRViewObj.hpp>
#include <JSystem/JGeometry.hpp>
#include <System/ParamInst.hpp>
#include <System/Params.hpp>
#include <Strategic/HitActor.hpp>

class TBGCheckData;
class TDLTexQuad;
class JUTTexture;
class J3DModelData;

class TWaterEmitInfo : public TParams {
public:
	TWaterEmitInfo(const char*);

public:
	/* 0x8 */ TParamRT<s32> mNum;
	/* 0x1C */ TParamRT<s16> mAlive;
	/* 0x30 */ TParamRT<s16> mAttack;
	/* 0x44 */ TParamVec mDir;
	/* 0x60 */ TParamVec mPos;
	/* 0x7C */ TParamVec mV;
	/* 0x98 */ TParamRT<f32> mDirTremble;
	/* 0xAC */ TParamRT<f32> mPow;
	/* 0xC0 */ TParamRT<f32> mPowTremble;
	/* 0xD4 */ TParamRT<f32> mSize;
	/* 0xE8 */ TParamRT<f32> mSizeTremble;
	/* 0xFC */ TParamRT<f32> mHitRadius;
	/* 0x110 */ TParamRT<f32> mHitHeight;
	/* 0x124 */ TParamRT<s32> mFlag;
	/* 0x138 */ TParamRT<s16> mType;
	/* 0x14C */ TParamRT<s16> __padding;
};

class TWaterParticleType : public TParams {
public:
	TWaterParticleType(const char* path);

public:
	/* 0x8 */ TParamRT<f32> mExtension;
	/* 0x1C */ TParamRT<f32> mMagnify;
	/* 0x30 */ TParamRT<f32> mCleanSize;
	/* 0x44 */ TParamRT<f32> mGravity;
	/* 0x58 */ TParamRT<f32> mAlive;
};

class TWaterHitActor : public THitActor {
public:
	// TODO: TModelWaterManager::mStaticHitActor is constructed without
	// initializing unk68, while BossHanachanParts constructs these with a name
	// and unk68 = 0. Two constructors reproduce both; the original spelling is
	// unknown.
	TWaterHitActor() { }
	TWaterHitActor(const char* name)
	    : THitActor(name)
	    , mHitCounter(0)
	{
	}

	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	void onWaterHitCounter();

public:
	// TODO: TWaterHitActor's own code (BossHanachanSub/Parts/Main) accesses
	// 0x68 as a halfword hit counter, while TModelWaterManager's particle
	// lookups read a word at the same offset through a cast. The latter are
	// probably a different class; the union keeps both compiling as-is.
	union {
		/* 0x68 */ int unk68;
		/* 0x68 */ s16 mHitCounter;
	};
};

class TModelWaterManager;

extern TModelWaterManager* gpModelWaterManager;

#ifdef SMS_PORT
// PC multiplayer (w15, port/src/port_mp_water.cpp): the other players' FLUDD water is
// drawn as real particles of this manager, flagged PORT_REMOTE_FLAG. They are visual
// only (no hit messages, no pollution cleaning, no sounds, their own random numbers) and
// live in slots past SLOT_NUM, so the local game keeps its full SLOT_NUM budget and
// behaves exactly as without them. On the GameCube the arrays are SLOT_NUM long.
#define MODEL_WATER_ARRAY_NUM 512
// Every emit request (local water only reaches the network through this).
extern void (*gPortWaterEmitHook)(const TWaterEmitInfo& info, int emitted);
// The TWaterEmitInfo values makeEmit reads, for TModelWaterManager::portEmitRemote.
struct TPortWaterEmit {
	JGeometry::TVec3<f32> pos, dir, v;
	f32 dirTremble, pow, powTremble, size, sizeTremble;
	s16 alive;
	u16 flag;
	u8 type;
};
#else
#define MODEL_WATER_ARRAY_NUM 256
#endif

class TModelWaterManager : public JDrama::TViewObj {
public:
	TModelWaterManager(const char* name = "<TModelWaterParticle>")
	    : JDrama::TViewObj(name)
	{
	}

	virtual void load(JSUMemoryInputStream&);
	virtual void loadAfter();
	f32 getWPGravity(int) const;
	void getWaterAlpha() const;
	bool askHitWaterParticleOnGround(const JGeometry::TVec3<f32>&);
	void makeEmit(const TWaterEmitInfo&);
	u8 emitRequest(const TWaterEmitInfo&);
	void splashSound(const JGeometry::TVec3<f32>&, f32) const;
	void splashGround(int);
	void touchingExec(int);
	void splashWall(int);
	void splashWallPosSize(const JGeometry::TVec3<f32>&, f32);
	f32 getPlaneFriction(const TBGCheckData*);
	f32 getPlaneFall(const TBGCheckData*);
	f32 getPlaneVanishSpeed(const TBGCheckData*);
	bool askDoWaterHitCheck();
	void wind(const JGeometry::TVec3<f32>&);
	void garbageCollect();
	void move();
	void calcWorldMinMax();
	void calcDrawVtx(MtxPtr);
	void calcVMMtxGround(MtxPtr, f32, const JGeometry::TVec3<f32>&,
	                     const JGeometry::TVec3<f32>&, MtxPtr);
	void calcVMMtxWall(MtxPtr, f32, const JGeometry::TVec3<f32>&,
	                   const JGeometry::TVec3<f32>&, MtxPtr);
	void calcVMAll(MtxPtr);
	void drawTouching();
	void drawTouchingMask();
	void drawSilhouette(MtxPtr);
	void drawWaterVolume(MtxPtr);
	void drawMirror(MtxPtr);
	void drawShineShadowVolume(MtxPtr);
	void drawRefracAndSpec() const;
	virtual void perform(u32 cue, JDrama::TGraphics* graphics);

	static TWaterHitActor mStaticHitActor;

	// fabricated
	BOOL checkParticleFlag(TWaterHitActor* hit, u16 flag)
	{
		return mParticleFlagSOA[hit->unk68] & flag ? TRUE : FALSE;
	}
	u16 getParticleFlag(TWaterHitActor* hit)
	{
		return mParticleFlagSOA[hit->unk68];
	}
	s16 getParticleAttack(TWaterHitActor* hit)
	{
		return mParticleAttackSOA[hit->unk68];
	}
	int getFlagBottom4Bits(int i) const { return mParticleFlagSOA[i] & 0xf; }
	void setFlagBottom4Bits(int i, int flag)
	{
		mParticleFlagSOA[i] = (mParticleFlagSOA[i] & ~0xf) | flag;
	}
	bool checkFlagBottom4Bits(int i, int flag) const
	{
		return (mParticleFlagSOA[i] & 0xf) == flag ? true : false;
	}
	JGeometry::TVec3<f32> vecBetweenParticles(int i, int j) const
	{
		JGeometry::TVec3<f32> d = mParticlePositionSOA[i];
		d -= mParticlePositionSOA[j];
		return d;
	}
	const JGeometry::TVec3<f32>* getParticlePositions() const
	{
		return mParticlePositionSOA;
	}
	THitActor** getParticleUnk2514() { return unk2514; }
	u16 getParticleCount() const { return mParticleCount; }

	enum { SLOT_NUM = 256 };

#ifdef SMS_PORT
	enum {
		PORT_REMOTE_MAX  = MODEL_WATER_ARRAY_NUM - SLOT_NUM, // remote particles at once
		PORT_REMOTE_FLAG = 0x8000, // mParticleFlagSOA: a remote player's (visual only)
	};
	bool portIsRemote(int i) const
	{
		return (mParticleFlagSOA[i] & PORT_REMOTE_FLAG) != 0;
	}
	// Emits `num` visual-only particles like emitRequest (makeEmit with the manager's
	// own random numbers), then moves them `age` simulation ticks ahead. Returns how
	// many fitted (PORT_REMOTE_MAX remote particles at most).
	int portEmitRemote(const TPortWaterEmit& info, int num, int age);
	f32 portRand();
	void portStepFlying(int i);
	void portInterpCalcView(MtxPtr viewMtx);
#endif

public:
	/* 0x10 */ s16 unk10;
	/* 0x12 */ u16 mParticleCount;
	/* 0x14 */ f32 mParticleLifetimeSOA[MODEL_WATER_ARRAY_NUM];
	/* 0x414 */ u16 mParticleFlagSOA[MODEL_WATER_ARRAY_NUM];
	/* 0x614 */ s16 mParticleAttackSOA[MODEL_WATER_ARRAY_NUM];
	/* 0x814 */ JGeometry::TVec3<f32> mParticlePositionSOA[MODEL_WATER_ARRAY_NUM];
	/* 0x1414 */ JGeometry::TVec3<f32> mParticleVelocitySOA[MODEL_WATER_ARRAY_NUM];
	/* 0x2014 */ f32 mParticleSizeSOA[MODEL_WATER_ARRAY_NUM];
	/* 0x2414 */ u8 mParticleTypeSOA[MODEL_WATER_ARRAY_NUM];
	/* 0x2514 */ THitActor* unk2514[MODEL_WATER_ARRAY_NUM];
	/* 0x2914 */ const TBGCheckData* unk2914[MODEL_WATER_ARRAY_NUM];
	/* 0x2D14 */ Mtx unk2D14[MODEL_WATER_ARRAY_NUM];
	/* 0x5D14 */ f32 unk5D14;
	/* 0x5D18 */ f32 unk5D18;
	/* 0x5D1C */ f32 unk5D1C;
	/* 0x5D20 */ GXColor unk5D20;
	/* 0x5D24 */ GXColor unk5D24;
	/* 0x5D28 */ f32 unk5D28;
	/* 0x5D2C */ f32 unk5D2C;
	/* 0x5D30 */ TDLTexQuad* unk5D30;
	/* 0x5D34 */ JUTTexture* unk5D34;
	/* 0x5D38 */ JUTTexture* unk5D38;
	/* 0x5D3C */ JUTTexture* unk5D3C;
	/* 0x5D40 */ JUTTexture* unk5D40;
	/* 0x5D44 */ JUTTexture* unk5D44;
	/* 0x5D48 */ J3DModelData* unk5D48;
	/* 0x5D4C */ J3DModelData* unk5D4C;
	/* 0x5D50 */ J3DModelData* unk5D50;
	/* 0x5D54 */ J3DModelData* unk5D54;
	/* 0x5D58 */ J3DModelData* unk5D58;
	/* 0x5D5C */ u8 unk5D5C;
	/* 0x5D5D */ u8 unk5D5D;
	/* 0x5D5E */ u8 unk5D5E;
	/* 0x5D5F */ u8 unk5D5F;
	/* 0x5D60 */ u16 unk5D60;
	/* 0x5D62 */ u8 unk5D62;
	/* 0x5D63 */ u8 unk5D63;
	/* 0x5D64 */ u8 unk5D64;
	/* 0x5D65 */ u8 unk5D65;
	/* 0x5D68 */ f32 unk5D68;
	/* 0x5D6C */ f32 unk5D6C;
	/* 0x5D70 */ JGeometry::TVec3<f32> unk5D70;
	/* 0x5D7C */ JGeometry::TVec3<f32> unk5D7C;
	/* 0x5D88 */ f32 unk5D88[13];
	/* 0x5DBC */ TWaterParticleType* mWaterParticleTypes[17];
	/* 0x5E00 */ int unk5E00;
	/* 0x5E04 */ u8 unk5E04;
	/* 0x5E08 */ f32 unk5E08;
	/* 0x5E0C */ f32 unk5E0C;
	/* 0x5E10 */ Mtx unk5E10;
	/* 0x5E40 */ f32 unk5E40;
	/* 0x5E44 */ u8 unk5E44;
	/* 0x5E45 */ u8 unk5E45;
	/* 0x5E46 */ u8 unk5E46;
	/* 0x5E47 */ u8 unk5E47;
	/* 0x5E48 */ u8 unk5E48;
	/* 0x5E49 */ u8 unk5E49;
#ifdef SMS_PORT
	// (offsets above are the GameCube's; on the PC the arrays are longer)
	u16 mPortRemoteCount; // remote particles in [0, mParticleCount)
	u32 mPortRandState;   // their random numbers (the game's rand() is not touched)
	// Frame-rate option (port_interp.cpp): each particle's position at the last two
	// logic frames' view passes, for extra display frames in between.
	JGeometry::TVec3<f32> mPortPrevPos[MODEL_WATER_ARRAY_NUM];
	JGeometry::TVec3<f32> mPortLastPos[MODEL_WATER_ARRAY_NUM];
	JGeometry::TVec3<f32> mPortPrev2Pos[MODEL_WATER_ARRAY_NUM]; // W12: the state before mPortPrevPos
#endif
};

#endif
