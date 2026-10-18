#ifndef STRATEGIC_TAKE_ACTOR_HPP
#define STRATEGIC_TAKE_ACTOR_HPP

#include <Strategic/HitActor.hpp>

class TTakeActor : public THitActor {
public:
	TTakeActor(const char* name)
	    : THitActor(name)
	    , mHolder(nullptr)
	    , mHeldObject(nullptr)
	{
	}
	virtual ~TTakeActor() { }
	virtual MtxPtr getTakingMtx() = 0;
	virtual void ensureTakeSituation()
	{
		if (mHeldObject != nullptr && mHeldObject->mHolder != this)
			mHeldObject = nullptr;

		if (mHolder != nullptr && mHolder->mHeldObject != this)
			mHolder = nullptr;
	}
	virtual BOOL moveRequest(const JGeometry::TVec3<f32>& where_to)
	{
		mPosition = where_to;
		return true;
	}
#ifdef LINK_ALL_SOURCE
	virtual f32 getRadiusAtY(f32) const { return mDamageRadius; } // weak inline in the original
#else
	virtual f32 getRadiusAtY(f32) const;
#endif

	BOOL isTaken() const
	{
		if (mHolder != nullptr)
			return TRUE;
		return FALSE;
	}
	bool isHolding() const { return mHeldObject != nullptr ? true : false; }
	TTakeActor* getHolder() { return mHolder; }
	TTakeActor* getHeldObject() { return mHeldObject; }

public:
	/* 0x68 */ TTakeActor* mHolder;
	/* 0x6C */ TTakeActor* mHeldObject;
};

#endif
