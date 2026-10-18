#ifndef ENEMY_FRUITS_BOAT_HPP
#define ENEMY_FRUITS_BOAT_HPP

#include <Enemy/Enemy.hpp>
#include <Enemy/EnemyManager.hpp>
#include <Strategic/Nerve.hpp>
#include <Strategic/Spine.hpp>
#include <dolphin/types.h>

class J3DFrameCtrl;
class J3DAnmTransformKey;
class JSUMemoryInputStream;
class THitActor;
class TLiveManager;

DECLARE_NERVE(TNerveFruitsBoatBckTrace, TLiveActor);
DECLARE_NERVE(TNerveFruitsBoatGraphWander, TLiveActor);

class TFruitsBoatParams : public TSpineEnemyParams {
public:
	TFruitsBoatParams(const char* path);

	/* 0xA8 */ TParamRT<f32> mSLMoveSpeed;
	/* 0xBC */ TParamRT<f32> mSLRotSpeed;
	/* 0xD0 */ TParamRT<f32> mSLBckMoveSpeed;
};

class TFruitsBoat : public TSpineEnemy {
public:
	TFruitsBoat(const char* name);
	virtual ~TFruitsBoat() { }

	virtual void load(JSUMemoryInputStream&);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	virtual Mtx* getRootJointMtx() const;
	virtual void init(TLiveManager* manager);
	virtual void calcRootMatrix();
	virtual void setGroundCollision();
	virtual void moveObject();
	virtual void requestShadow();

	int setBckTrack(const char* name);
	void setJumpReaction();
	void traceBckTrack();
	int getBoatType() const;
	void rowToCurPathNode(f32 speed);

	// fabricated
	TFruitsBoatParams* getParams() const
	{
		return (TFruitsBoatParams*)getSaveParam();
	}

public:
	/* 0x150 */ s16 unk150; // toggled by graph node flag 0x400
	/* 0x154 */ f32 mShadowRadiusX;
	/* 0x158 */ f32 mShadowRadiusZ;
	/* 0x15C */ J3DAnmTransformKey* mTrackAnm;
	/* 0x160 */ J3DFrameCtrl* mTrackCtrl;
	/* 0x164 */ JGeometry::TVec3<f32> mTiltAxis;
	/* 0x170 */ f32 mTiltAngle;
	/* 0x174 */ f32 mTiltSpeed;
};

class TFruitsBoatManager : public TEnemyManager {
public:
	TFruitsBoatManager(int boat_type, const char* name);
	virtual ~TFruitsBoatManager() { }

	virtual void load(JSUMemoryInputStream&);
	virtual void createModelData();
	virtual TSpineEnemy* createEnemyInstance();

public:
	/* 0x54 */ int mBoatType;
};

#endif
