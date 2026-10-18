#ifndef ENEMY_BATHTUBPEACH_HPP
#define ENEMY_BATHTUBPEACH_HPP

#include <Enemy/BathtubBinder.hpp>
#include <Enemy/Enemy.hpp>
#include <Enemy/EnemyManager.hpp>
#include <JSystem/JGeometry/JGVec3.hpp>
#include <Strategic/HitActor.hpp>
#include <Strategic/LiveActor.hpp>

class TBathtubPeachParams : public TSpineEnemyParams {
public:
	TBathtubPeachParams(const char* path)
	    : TSpineEnemyParams(path)
	    , PARAM_INIT(turnSpeed, 8.0f)
	    , PARAM_INIT(turnSpeed2, 1.0f)
	    , PARAM_INIT(speed, 16.0f)
	    , PARAM_INIT(angle, 72.0f)
	    , PARAM_INIT(range, 100.0f)
	    , PARAM_INIT(radius, 2200.0f)
	{
		TParams::load(mPrmPath);
	}

	/* 0x0A8 */ TParamRT<f32> turnSpeed;
	/* 0x0BC */ TParamRT<f32> turnSpeed2;
	/* 0x0D0 */ TParamRT<f32> speed;
	/* 0x0E4 */ TParamRT<f32> angle;
	/* 0x0F8 */ TParamRT<f32> range;
	/* 0x10C */ TParamRT<f32> radius;
};

class TBathtubPeachManager : public TEnemyManager {
public:
	TBathtubPeachManager(const char* name);

	virtual void load(JSUMemoryInputStream&);
	virtual void createModelData();
	virtual TSpineEnemy* createEnemyInstance();
};

class TBathtubPeach : public TSpineEnemy {
public:
	TBathtubPeach(const char* name);

	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual BOOL receiveMessage(THitActor* sender, u32 message);
	virtual Mtx* getRootJointMtx() const;
	virtual void init(TLiveManager* manager);
	virtual void calcRootMatrix();
	virtual const char** getBasNameTable() const;
	virtual void reset();

	TBathtubPeachParams* getParam() const;
	void changeAnm(int bck, int btp, f32 speed);
	void faceTo(const JGeometry::TVec3<f32>& pos, f32 max_turn);
	void goTo(const JGeometry::TVec3<f32>& pos);

public:
	/* 0x150 */ TBathtubBinder mBathtubBinder;
};

#endif
