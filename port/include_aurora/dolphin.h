/* Aurora-build shim for our decomp's <dolphin.h>, which includes its siblings with
 * quotes (so they would bypass Aurora's headers and redefine u32 & co). */
#ifndef DOLPHIN_H
#define DOLPHIN_H

#include <dolphin/types.h>
#include <dolphin/db.h>
#include <dolphin/dvd.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>
#include <dolphin/dsp.h>

#ifndef __SDKVER_SYMBOL__
#define __SDKVER_SYMBOL__
#define __SDKVER__ "12Dec2001"
#endif

#endif
