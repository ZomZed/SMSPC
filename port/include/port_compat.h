/*
 * port_compat.h - force-included first in every translation unit of the PC
 * build (port/). It papers over Metrowerks CodeWarrior / PowerPC specifics so
 * the decompiled source compiles with clang on a PC. Nothing here is used by
 * the GameCube build (configure.py / ninja).
 *
 * Rules: keep this header self-contained (no includes of game headers) and
 * keep every shim behaviour-identical to the PowerPC original.
 */
#ifndef PORT_COMPAT_H
#define PORT_COMPAT_H

#define SMS_PORT 1 /* use `#ifdef SMS_PORT` in game/JSystem code for PC-only paths */

/* ---- CodeWarrior keywords / extensions -------------------------------- */
/* Note: __declspec is NOT stubbed out; the host MSVC headers rely on it. */
#define __attribute_weak__ __attribute__((weak))

/* CodeWarrior builtin (the GameCube build maps alignof onto it; C++11 has a
 * real alignof keyword, so only the builtin name is shimmed here). */
#define __builtin_align __alignof__

/* ---- PowerPC intrinsics (behaviour-identical C versions) ---------------- */
#ifdef __cplusplus
extern "C" {
#endif

static inline unsigned int __cntlzw(unsigned int x) { return x ? (unsigned int)__builtin_clz(x) : 32u; }
static inline double __fabs(double x) { return __builtin_fabs(x); }
static inline float __fabsf(float x) { return __builtin_fabsf(x); }
static inline double __fnabs(double x) { return -__builtin_fabs(x); }
static inline float __fnabsf(float x) { return -__builtin_fabsf(x); }
/* frsqrte/fres are estimates on hardware; the full-precision value is what the
 * game relies on after its Newton-Raphson steps. */
static inline double __frsqrte(double x) { return 1.0 / __builtin_sqrt(x); }
static inline float __fres(float x) { return 1.0f / x; }
static inline double __fsel(double a, double b, double c) { return a >= 0.0 ? b : c; }
static inline float __fsels(float a, float b, float c) { return a >= 0.0f ? b : c; }
static inline int __abs(int x) { return x < 0 ? -x : x; }
static inline long __labs(long x) { return x < 0 ? -x : x; }

static inline unsigned int __rlwinm(unsigned int v, int sh, int mb, int me)
{
	unsigned int r = (v << (sh & 31)) | (v >> ((32 - sh) & 31));
	unsigned int m = (mb <= me) ? ((0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)))
	                            : ((0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31 - me)));
	return r & m;
}
static inline unsigned int __rlwimi(unsigned int a, unsigned int v, int sh, int mb, int me)
{
	unsigned int r = (v << (sh & 31)) | (v >> ((32 - sh) & 31));
	unsigned int m = (mb <= me) ? ((0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)))
	                            : ((0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31 - me)));
	return (r & m) | (a & ~m);
}

/* Byte-reversed loads/stores: the GameCube is big-endian, so these swap on a
 * big-endian machine and are plain accesses of the *opposite* order here. */
static inline unsigned short __lhbrx(const void* p, int off)
{
	const unsigned char* b = (const unsigned char*)p + off;
	return (unsigned short)(b[0] | (b[1] << 8));
}
static inline unsigned int __lwbrx(const void* p, int off)
{
	const unsigned char* b = (const unsigned char*)p + off;
	return (unsigned int)b[0] | ((unsigned int)b[1] << 8) | ((unsigned int)b[2] << 16)
	       | ((unsigned int)b[3] << 24);
}
static inline void __sthbrx(unsigned short v, void* p, int off)
{
	unsigned char* b = (unsigned char*)p + off;
	b[0] = (unsigned char)v;
	b[1] = (unsigned char)(v >> 8);
}
static inline void __stwbrx(unsigned int v, void* p, int off)
{
	unsigned char* b = (unsigned char*)p + off;
	b[0] = (unsigned char)v;
	b[1] = (unsigned char)(v >> 8);
	b[2] = (unsigned char)(v >> 16);
	b[3] = (unsigned char)(v >> 24);
}

/* Cache / sync instructions have no meaning on a PC. */
static inline void __dcbf(void* p, int off) { (void)p; (void)off; }
static inline void __dcbi(void* p, int off) { (void)p; (void)off; }
static inline void __dcbst(void* p, int off) { (void)p; (void)off; }
static inline void __dcbt(void* p, int off) { (void)p; (void)off; }
static inline void __dcbz(void* p, int off)
{
	unsigned char* b = (unsigned char*)p + off;
	int i;
	for (i = 0; i < 32; i++)
		b[i] = 0;
}
static inline void __sync(void) { }
static inline void __isync(void) { }
static inline void __eieio(void) { }

#ifdef __cplusplus
}
#endif

/* MSL's <math.h>/<ctype.h>/<stdlib.h> declare their functions in namespace std
 * and define M_PI; the host UCRT does neither unless told to. */
#ifdef __cplusplus
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES 1
#endif
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cstdio>
#include <cstddef>

/* gnu++ mode makes `typeof` a keyword; the game uses it as a member name
 * (TSpcSlice::typeof). Must stay after the host includes above. */
#define typeof sms_typeof

/* MSL exposed these as std::sqrtf/fabsf/powf. */
namespace std {
using ::fabsf;
using ::powf;
using ::sqrtf;
}
#endif

/* MSL's math.h defines M_PI as a *float* literal; keep that (host M_PI is a
 * double and would change float rounding in the sin tables). */
#undef M_PI
#define M_PI 3.14159265358979323846f

/* The GameCube build passes -Dnullptr=0 (configure.py); C sources need it too. */
#ifndef __cplusplus
#ifndef nullptr
#define nullptr 0
#endif
#endif

/* Paired-single psq_st quantization (GQR scale 0): float -> integer truncates
 * toward zero and saturates. */
static inline int __sms_f32_to_s16(float f)
{
	if (f >= 32767.0f)
		return 32767;
	if (f <= -32768.0f)
		return -32768;
	return (int)f;
}
static inline int __sms_f32_to_s8(float f)
{
	if (f >= 127.0f)
		return 127;
	if (f <= -128.0f)
		return -128;
	return (int)f;
}

/* MSL's _tolower is a full tolower (JKRFileLoader::fetchVolumeName lowers
 * volume names with it); the UCRT macro only subtracts 'A' - 'a', which turns
 * "/common/..." into garbage and breaks every JKRGetResource volume lookup. */
#undef _tolower
#define _tolower(c) tolower(c)

/* Big-endian data helpers (byte swaps, RARC swap pass); see port/ENDIAN.md. */
#include "port_endian.h"
#ifdef __cplusplus
#include "port_jaudio_swap.h" /* JAudio bank / wave system / table swap passes */
#include "port_map_swap.h" /* .col collision / .ral rail swap passes */
#include "port_asset_swap.h" /* .pad input replay swap pass */
#endif
#include "port_mwcc.h" /* CodeWarrior runtime semantics (float -> u32); C and C++ */
/* w13: SMS_BEHAVIOUR_IS() for the run-time game behaviour (BOOTLOG "Regional behaviour (w13)") */
#include "../../include/version.h"

#endif /* PORT_COMPAT_H */
