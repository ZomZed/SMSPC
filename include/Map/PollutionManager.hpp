#ifndef MAP_POLLUTION_MANAGER_HPP
#define MAP_POLLUTION_MANAGER_HPP

#include <Map/JointModelManager.hpp>
#include <Map/PollutionCount.hpp>
#include <Map/PollutionLayer.hpp>

class J3DModel;

class TPollutionManager;

extern TPollutionManager* gpPollution;

class TPollutionLayerInfo;

class TPollutionManager : public TJointModelManager {
public:
	struct TPollutionInfo {
		/* 0x0 */ u16 mLayerCount;
#ifdef SMS_PORT
		// file offset of the layer infos (see port_swap_ymp); the pointer the
		// original patched in here does not fit the 8-byte header on x64
		/* 0x4 */ u32 mLayerInfosOffset;
#else
		/* 0x4 */ TPollutionLayerInfo* mLayerInfos;
#endif
	};

	TPollutionManager(const char* name = "落書き管理");

	virtual void load(JSUMemoryInputStream&);
	virtual void perform(u32 cue, JDrama::TGraphics* graphics);
	virtual TJointModel* newJointModel(int) const;

	void setDataAddress(TPollutionManager::TPollutionInfo* info);
	void initPollutionInfo();

	/// Stamps the top-down ortho projection of the model onto
	/// ground pollution layers
	void stampModel(J3DModel* model);

	/// Stamps the the specified stamp type at the specified position
	/// with the specified size
	void stamp(u16 stamp_type, f32 x, f32 y, f32 z, f32 size);

	/// Cleans the specified area of pollution
	void clean(f32 x, f32 y, f32 z, f32 size);

	/// Stamps the specified stamp type at the specified position
	/// affecting only ground polution layers
	void stampGround(u16 stamp_type, f32 x, f32 y, f32 z, f32 size);

	/// Queries the pollution type of the layer that covers the specified point
	int getPollutionType(f32 x, f32 y, f32 z) const;

	/// Queries the total pollution degree of the entire map
	u32 getPollutionDegree() const;

	void isProhibit(f32 x, f32 y, f32 z) const;

	/// Queries whether the specified position is polluted by any layer
	bool isPolluted(f32 x, f32 y, f32 z) const;

	void subtractFromYMap(f32 x, f32 y, f32 z) const;

	/// Queries whether the entire map is cleared
	bool cleanedAll() const;

	void draw();

	// fabricated
	TPollutionCounterLayer& getCounterLayer() { return unk70; }
	TPollutionCounterObj& getCounterObj() { return unk1EC; }
	TPollutionLayer* getLayer(int i)
	{
		return (TPollutionLayer*)getJointModel(i);
	}
	TPollutionLayer* getLayer(int i) const
	{
		return (TPollutionLayer*)getJointModel(i);
	}
	const TPollutionLayerInfo* getLayerInfo(int i) const
	{
		return &mLayerInfos[i];
	}
	TPollutionLayerInfo* getLayerInfo(int i) { return &mLayerInfos[i]; }
	void offLayer(int i) { unk70.offLayer(i); }
	// fabricated: no matching UNUSED symbol in the map, but the extra inline
	// level is required for loadAfter__23TMapObjRevivalPollutionFv to match
	int registerRevivalTexStamp(int layer_idx, s16 x, s16 y, s16 width,
	                            s16 height, int interval, ResTIMG* tex)
	{
		return getCounterLayer().registerRevivalTexStamp(
		    layer_idx, x, y, width, height, interval, tex);
	}
	void pollute(f32 x, f32 y, f32 z, f32 size) { stamp(1, x, y, z, size); }

	static u8 mEdgeAlpha;
	static int mFlushTime;

public:
	/* 0x6C */ TPollutionLayerInfo* mLayerInfos;
	/* 0x70 */ TPollutionCounterLayer unk70;
	/* 0x1EC */ TPollutionCounterObj unk1EC;
	/* 0x204 */ ResTIMG* mDefaultPolluteStampTex;
	/* 0x208 */ ResTIMG* mDefaultCleanStampTex;
	/* 0x20C */ u16 unk20C;
};

#endif
