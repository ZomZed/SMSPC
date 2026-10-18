/* Aurora-build shim. Aurora's OSErrorHandler is variadic (OSError, OSContext*, ...);
 * the game and JSystem define 4-argument handlers (error, context, dsisr, dar) and
 * pass them as OSErrorHandler, which is what our decomp header declares. Aurora does
 * not implement OSSetErrorHandler, so there is no ABI to match: use our header. */
#include "../../../../libs/dolphin/include/dolphin/os/OSError.h"
