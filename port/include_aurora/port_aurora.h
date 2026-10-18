/*
 * port_aurora.h - force-included (after port_compat.h) into every game/JSystem unit
 * of the Aurora build (port/tools/build_aurora.py). Never seen by the GameCube build
 * or by build_port.py.
 *
 * The game is compiled against Aurora's SDK headers (aurora/include/dolphin), which
 * follow the real SDK but (a) leave out some functions/macros the game uses, and
 * (b) change a few signatures for 64-bit / PC (TARGET_PC). This header fills (a) with
 * declarations taken from our decomp headers (libs/dolphin/include) and bridges (b)
 * with C++ overloads, so the shared game source stays unchanged. Implementations of
 * the functions Aurora does not provide live in port/src/port_aurora_sdk.cpp.
 *
 * Categories are logged in port/AURORA.md.
 */
#ifndef PORT_AURORA_H
#define PORT_AURORA_H

#include <dolphin/types.h>
#include <dolphin/os.h>
#include <dolphin/gx.h>
#include <dolphin/arq.h>
#include <dolphin/vi.h>
#ifdef __cplusplus
/* Aurora's GDBase.h and card.h are C++-only on TARGET_PC (static_cast, bool). */
#include <dolphin/gd.h>
#include <dolphin/card.h>
#endif

/* ---- types.h: alignment attribute ---------------------------------------
 * Aurora picks __declspec(align(n)) for clang-cl/MSVC (clang targeting msvc does not
 * define __GNUC__), which must precede the declarator; the game writes
 * ATTRIBUTE_ALIGN after it (`u8 buf[N] ATTRIBUTE_ALIGN(32);`). clang accepts the GNU
 * attribute in that position. */
#undef ATTRIBUTE_ALIGN
#define ATTRIBUTE_ALIGN(num) __attribute__((aligned(num)))

#ifdef __cplusplus
extern "C" {
#endif

/* ---- OS ----------------------------------------------------------------- */
/* Our decomp's OSException.h names (Aurora only has the __OS_EXCEPTION_* spelling). */
#ifndef OS_EXCEPTION_DSI
#define OS_EXCEPTION_SYSTEM_RESET        __OS_EXCEPTION_SYSTEM_RESET
#define OS_EXCEPTION_MACHINE_CHECK       __OS_EXCEPTION_MACHINE_CHECK
#define OS_EXCEPTION_DSI                 __OS_EXCEPTION_DSI
#define OS_EXCEPTION_ISI                 __OS_EXCEPTION_ISI
#define OS_EXCEPTION_EXTERNAL_INTERRUPT  __OS_EXCEPTION_EXTERNAL_INTERRUPT
#define OS_EXCEPTION_ALIGNMENT           __OS_EXCEPTION_ALIGNMENT
#define OS_EXCEPTION_PROGRAM             __OS_EXCEPTION_PROGRAM
#define OS_EXCEPTION_FLOATING_POINT      __OS_EXCEPTION_FLOATING_POINT
#define OS_EXCEPTION_DECREMENTER         __OS_EXCEPTION_DECREMENTER
#define OS_EXCEPTION_SYSTEM_CALL         __OS_EXCEPTION_SYSTEM_CALL
#define OS_EXCEPTION_TRACE               __OS_EXCEPTION_TRACE
#define OS_EXCEPTION_PERFORMANCE_MONITOR __OS_EXCEPTION_PERFORMACE_MONITOR
#define OS_EXCEPTION_BREAKPOINT          __OS_EXCEPTION_BREAKPOINT
#define OS_EXCEPTION_SYSTEM_INTERRUPT    __OS_EXCEPTION_SYSTEM_INTERRUPT
#define OS_EXCEPTION_THERMAL_INTERRUPT   __OS_EXCEPTION_THERMAL_INTERRUPT
#define OS_EXCEPTION_MEMORY_PROTECTION   __OS_EXCEPTION_MEMORY_PROTECTION
#define OS_EXCEPTION_MAX                 OS_EXCEPTION_MEMORY_PROTECTION
#endif
#ifndef OS_ERROR_MEMORY_PROTECTION
#define OS_ERROR_MEMORY_PROTECTION OS_ERROR_PROTECTION
#endif

/* ---- DSP: SDK-internal task functions JAudio's DSP driver calls --------- */
#include <dolphin/dsp.h>
DSPTaskInfo* __DSPGetCurrentTask(void);
void __DSP_boot_task(DSPTaskInfo* task);
void __DSP_insert_task(DSPTaskInfo* task);
void __DSP_exec_task(DSPTaskInfo* curr, DSPTaskInfo* next);
void __DSP_remove_task(DSPTaskInfo* task);

/* ---- GX: functions the game uses that Aurora does not declare ----------- */
void GXSetDrawSync(u16 token);
void GXWaitDrawDone(void);
void GXReadPixMetric(u32* top_pixels_in, u32* top_pixels_out, u32* bot_pixels_in,
                     u32* bot_pixels_out, u32* clr_pixels_in, u32* copy_clks);
void GXClearPixMetric(void);
void GXEnableBreakPt(void* break_pt);
void GXDisableBreakPt(void);
void GXLoadNrmMtxIndx3x3(u16 mtx_indx, u32 id);
void GXGetTexObjAll(const GXTexObj* obj, void** image_ptr, u16* width, u16* height,
                    GXTexFmt* format, GXTexWrapMode* wrap_s, GXTexWrapMode* wrap_t,
                    u8* mipmap);

typedef enum _GXCopyMode {
	GX_COPY_PROGRESSIVE = 0,
	GX_COPY_INTLC_EVEN  = 2,
	GX_COPY_INTLC_ODD   = 3,
} GXCopyMode;
void GXSetDispCopyFrame2Field(GXCopyMode mode);

/* Register-id tables (SDK GXTexture.c globals; Aurora keeps them file-local;
 * JSystem's J3DTevs.cpp defines them). */
extern u8 GXTexMode0Ids[8];
extern u8 GXTexMode1Ids[8];
extern u8 GXTexImage0Ids[8];
extern u8 GXTexImage1Ids[8];
extern u8 GXTexImage2Ids[8];
extern u8 GXTexImage3Ids[8];
extern u8 GXTexTlutIds[8];

#ifndef GXInitLightShininess
#define GXInitLightShininess(lt_obj, shininess)                                          \
	GXInitLightAttn(lt_obj, 0.0F, 0.0F, 1.0F, (shininess) / 2.0F, 0.0F,                  \
	                1.0F - (shininess) / 2.0F)
#endif

#ifdef __cplusplus
/* ---- GD: inlines from our GDBase.h / GDGeometry.h / GDVert.h ------------- */
static inline GDLObj* GDGetCurrent(void) { return __GDCurrentDL; }
static inline void GDBegin(GXPrimitive type, GXVtxFmt vtx_fmt, u16 vtx_num)
{
	GDWrite_u8((u8)(vtx_fmt | type));
	GDWrite_u16(vtx_num);
}
static inline void GDEnd(void) { }
static inline void GDWrite_s8(s8 v) { GDWrite_u8((u8)v); }
static inline void GDWrite_s16(s16 v) { GDWrite_u16((u16)v); }
static inline void GDWrite_s32(s32 v) { GDWrite_u32((u32)v); }
#endif

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
/* GDVert.h is ours only (GDPosition3f32, GDNormal1x16, ... as static inlines). */
#include <dolphin/gd/GDVert.h>

/* ---- C++ overloads bridging SDK signatures to Aurora's TARGET_PC ones ---- */

/* Real SDK GXSetArray has no size / endianness; Aurora uploads `size` bytes of the
 * array and byte-swaps them unless `le`. Arrays are native order on PC (port/ENDIAN.md).
 * Call sites that know the size use PORTGXSetArray (port/include/port_gx.h); this
 * fallback uses a bounded readable span (port_aurora_sdk.cpp). */
void GXSetArray(GXAttr attr, const void* data, u8 stride);

/* The game passes const tables; Aurora's prototype is non-const (it only reads). */
/* (Templates so a literal 0 still picks Aurora's own prototype unambiguously.) */
template <typename P, typename V>
static inline void GXSetCopyFilter(GXBool aa, const P* sample_pattern, GXBool vf, const V* vfilter)
{
	GXSetCopyFilter(aa, (u8(*)[2])sample_pattern, vf, (u8*)vfilter);
}

/* SDK OSPhysicalToCached etc. are address macros. Aurora's versions take a u32
 * physical address (offset into its MEM1); the game also feeds them pointers it
 * already owns (DSP ucode, textures), for which the host address is the identity. */
template <typename T> static inline void* OSPhysicalToCached(T* p) { return (void*)p; }
template <typename T> static inline void* OSPhysicalToUncached(T* p) { return (void*)p; }

/* Aurora's CARDInit takes the game code / maker code for the save file names. */
static inline void CARDInit(void) { CARDInit("GMSE", "01"); }

#endif /* __cplusplus */

#endif /* PORT_AURORA_H */
