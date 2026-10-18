#ifndef JPA_EMITTER_LOADER_HPP
#define JPA_EMITTER_LOADER_HPP

#include <JSystem/JParticle/JPADataBlock.hpp>
#include <dolphin/types.h>

class JKRHeap;
class JPATextureResource;

class JPAKeyFrameAnime : public JPADataBlock {
public:
	JPAKeyFrameAnime(const u8* param_1, JKRHeap* param_2)
	    : JPADataBlock(param_1, param_2)
	{
#ifdef SMS_PORT
		// KFA1 key frames (4 x f32 per key from +0x20) are read in place from
		// this block's own copy (JPABaseEmitter::calcKeyFrameAnime), which was
		// just made from the big-endian file: swap it once, here.
		u8* raw = (u8*)mRawData;
		for (u32 i = 0; i < (u32)raw[0x10] * 4; ++i)
			sms_swap32_inplace(raw + 0x20 + i * 4);
#endif
	}
	virtual ~JPAKeyFrameAnime() { }
};

struct JPAEmitterData;

class JPAEmitterLoaderDataBase {
public:
	static JPAEmitterData* load(const u8*, JKRHeap*, JPATextureResource*);
};

struct JPABinaryHeader {
	u32 unk0;
	u32 unk4;
	u32 unk8;
	u32 unkC;
};

class JPAEmitterLoader {
public:
	JPAEmitterLoader(JKRHeap* param_1, const u8* param_2,
	                 const JPABinaryHeader* param_3)
	{
		unk8  = param_2;
		unkC  = param_3;
		unk10 = 0;
		mHeap = param_1;
	}

	virtual ~JPAEmitterLoader() { }

public:
	/* 0x4 */ JKRHeap* mHeap;
	/* 0x8 */ const u8* unk8;
	/* 0xC */ const JPABinaryHeader* unkC;
	/* 0x10 */ JPAEmitterData* unk10;
};

class JPAEmitterLoader_v10 : public JPAEmitterLoader {
public:
	JPAEmitterLoader_v10(JKRHeap*, const u8*, const JPABinaryHeader*);
	virtual ~JPAEmitterLoader_v10() { }

	JPAEmitterData* load(JPATextureResource*);
};

#endif
