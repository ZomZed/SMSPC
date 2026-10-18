/* Aurora-build shim (port/include_aurora is searched before aurora/include).
 * Aurora's OSContext.h makes the struct opaque (char storage[0x2C8]) under TARGET_PC.
 * The game (JUTException register dump, marerr) and the port's own thread code read
 * the PowerPC fields, so expose the SDK layout; the size is the same 0x2C8 bytes, so
 * this is ABI-compatible with Aurora (which never looks inside). */
#ifndef PORT_AURORA_OSCONTEXT_SHIM_H
#define PORT_AURORA_OSCONTEXT_SHIM_H

#include <dolphin/types.h> /* with TARGET_PC still defined: u32 = uint32_t */

#pragma push_macro("TARGET_PC")
#undef TARGET_PC
#include_next <dolphin/os/OSContext.h>
#pragma pop_macro("TARGET_PC")

#ifdef __cplusplus
static_assert(sizeof(OSContext) == 0x2C8, "OSContext layout");
#endif

#endif
