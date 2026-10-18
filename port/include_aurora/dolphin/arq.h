/* Aurora-build shim. Aurora's ARQRequest keeps owner/source/dest as u32 (40 bytes);
 * our 64-bit arq.h (SMS_PORT) makes them pointer-sized (ARQAddr, 56 bytes), which
 * JKRAram/JASystem and port/src/sdk_audio.cpp are written for. The ARQ implementation
 * in the Aurora build is sdk_audio.cpp (Aurora's AR.cpp is not linked), so there is no
 * Aurora ABI to match: use our header everywhere. */
#include "../../../libs/dolphin/include/dolphin/arq.h"
