#include <dolphin/types.h>

#pragma cplusplus on

#ifdef SMS_PORT
// PC: in C (gnu99) "extern inline" emits an external definition, which
// collides with the host CRT at link time (the x64 UCRT exports sqrtf), and a
// plain C99 "inline" may call an external floor(double) instead. Keep all four
// file-local so the including file uses exactly these versions.
#define FAKE_TGMATH_EXTERN_INLINE static inline
#define FAKE_TGMATH_INLINE        static inline
#else
#define FAKE_TGMATH_EXTERN_INLINE extern inline
#define FAKE_TGMATH_INLINE        inline
#endif

FAKE_TGMATH_EXTERN_INLINE float sqrtf(float x)
{
	volatile float y;
	if (x > 0.0f) {
		double guess = __frsqrte((double)x); // returns an approximation to
		guess = .5 * guess * (3.0 - guess * guess * x); // now have 12 sig bits
		guess = .5 * guess * (3.0 - guess * guess * x); // now have 24 sig bits
		guess = .5 * guess * (3.0 - guess * guess * x); // now have 32 sig bits
		y     = (float)(x * guess);
		return y;
	}
	return x;
}

FAKE_TGMATH_EXTERN_INLINE float sqrt(float x)
{
	volatile float y;
	if (x > 0.0f) {
		double guess = __frsqrte((double)x); // returns an approximation to
		guess = .5 * guess * (3.0 - guess * guess * x); // now have 12 sig bits
		guess = .5 * guess * (3.0 - guess * guess * x); // now have 24 sig bits
		guess = .5 * guess * (3.0 - guess * guess * x); // now have 32 sig bits
		guess = .5 * guess * (3.0 - guess * guess * x); // now have 32 sig bits

		y = (float)(x * guess);
		return y;
	}
	return x;
}

FAKE_TGMATH_EXTERN_INLINE float fabs(float x)
{
#if __MIPS__
	return fabsf(x);
#else
	(*(int*)&x) &= 0x7fffffff;
	return x;
#endif
}

FAKE_TGMATH_INLINE float floor(float x)
{
	int i   = (int)x;
	float y = x - (float)i;

	if (!y || x > 8388608.0f)
		return x; // x is already an int

	if (x < 0)
		return (float)--i;
	// x < 0 -> int conversion of x above rounded toward zero(so decrement)
	return (float)i;
}

#pragma cplusplus reset
