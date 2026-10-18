#ifndef JPA_DATA_BLOCK_HPP
#define JPA_DATA_BLOCK_HPP

#include <JSystem/JKernel/JKRHeap.hpp>

// .jpa files are big-endian. The shape blocks (BSP1/ESP1/SSP1/ETX1) are parsed
// straight from the loaded file, so the PC build reads those fields through
// byte-order-aware accessors (the file buffer is never modified, which keeps
// the stream-read blocks BEM1/FLD1 and shared resources intact).
#ifdef SMS_PORT
#define JPA_RD_S16(p) sms_bes16(p)
#define JPA_RD_F32(p) sms_bef32(p)
#define JPA_RD_U32(p) sms_be32(p)
#else
#define JPA_RD_S16(p) (*(s16*)(p))
#define JPA_RD_F32(p) (*(f32*)(p))
#define JPA_RD_U32(p) (*(u32*)(p))
#endif

class JPADataBlock {
public:
	JPADataBlock() { }
	JPADataBlock(const u8*, JKRHeap*);
	virtual ~JPADataBlock() { }

	// fabricated
	u8* getRawData() { return (u8*)mRawData; }

public:
	/* 0x4 */ void* mRawData;
};

#endif
