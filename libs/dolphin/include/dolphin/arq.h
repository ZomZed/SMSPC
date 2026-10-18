#ifndef _DOLPHIN_ARQ_H_
#define _DOLPHIN_ARQ_H_

#include <dolphin/ar.h>

#define ARQ_DMA_ALIGNMENT 32

#define ARQ_TYPE_MRAM_TO_ARAM ARAM_DIR_MRAM_TO_ARAM
#define ARQ_TYPE_ARAM_TO_MRAM ARAM_DIR_ARAM_TO_MRAM

#define ARQ_PRIORITY_LOW  0
#define ARQ_PRIORITY_HIGH 1

#ifdef SMS_PORT
// PC (64-bit): the callback argument is the ARQRequest pointer, the owner is
// sometimes a pointer, and source/dest are main-memory pointers on one side of
// every transfer, so all of them are pointer-sized here. On the GameCube
// uintptr_t is u32, so the two declarations are the same there.
#include <stdint.h>
typedef uintptr_t ARQAddr;
#else
typedef u32 ARQAddr;
#endif

typedef void (*ARQCallback)(ARQAddr pointerToARQRequest);

struct ARQRequest {
	/* 0x00 */ struct ARQRequest* next;
	/* 0x04 */ ARQAddr owner;
	/* 0x08 */ u32 type;
	/* 0x0C */ u32 priority;
	/* 0x10 */ ARQAddr source;
	/* 0x14 */ ARQAddr dest;
	/* 0x18 */ u32 length;
	/* 0x1C */ ARQCallback callback;
};

typedef struct ARQRequest ARQRequest;

#ifdef __cplusplus
extern "C" {
#endif

void ARQInit(void);
void ARQReset(void);
void ARQPostRequest(struct ARQRequest* request, ARQAddr owner, u32 type,
                    u32 priority, ARQAddr source, ARQAddr dest, u32 length,
                    ARQCallback callback);
void ARQRemoveRequest(struct ARQRequest* request);
void ARQRemoveOwnerRequest(ARQAddr owner);
void ARQFlushQueue(void);
void ARQSetChunkSize(u32 size);
u32 ARQGetChunkSize(void);

#ifdef __cplusplus
}
#endif

#endif
