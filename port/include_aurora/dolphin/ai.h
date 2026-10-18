/* Aurora-build shim. Our ai.h (SMS_PORT) returns the DMA start address as uintptr_t;
 * Aurora's returns u32. AI is implemented by port/src/sdk_audio.cpp (Aurora has no AI),
 * so use our header everywhere. */
#include "../../../libs/dolphin/include/dolphin/ai.h"
