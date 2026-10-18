/*
 * port_mwcc.h - CodeWarrior runtime semantics that x86 clang does not give for
 * free (worker w10; BOOTLOG "Sanitizers (w10)").
 *
 * sms_cvt_fp2unsigned(d): float/double -> u32 the way MWCC compiles it, through
 * the runtime helper __cvt_fp2unsigned (PowerPC_EABI_Support/Runtime/runtime.c):
 * negative -> 0, >= 2^32 -> 0xFFFFFFFF, otherwise truncation toward zero.
 * A plain (u32) cast of a negative float is undefined in C++; x86 clang
 * computes it through a 64-bit cvttss2si and keeps the low 32 bits, so -66.4
 * becomes 0xFFFFFFBE instead of the GameCube's 0. The GameCube code calls the
 * helper at 60 sites in game/JSystem code (`bl __cvt_fp2unsigned` in the asm);
 * use this at any of them that can see a negative or huge value.
 *
 * (Narrower conversions are fine as they are: MWCC uses fctiwz and keeps the low
 * 8/16 bits, and clang's x86 code for (s16)/(u8) does the same for any value
 * that fits in an int.)
 */
#ifndef PORT_MWCC_H
#define PORT_MWCC_H

static inline unsigned int sms_cvt_fp2unsigned(double d)
{
	if (d < 0.0)
		return 0;
	if (d >= 4294967296.0)
		return 0xFFFFFFFFu;
	if (d != d) /* NaN: fctiwz gives 0x80000000, then the helper subtracts 0x80000000 */
		return 0;
	return (unsigned int)d;
}

/* sms_cvt_fp2int(d): float/double -> s32 as PowerPC fctiwz does it: saturating
 * (>= 2^31 -> 0x7FFFFFFF, < -2^31 -> 0x80000000, NaN -> 0x80000000). x86
 * cvttss2si returns 0x80000000 for every out-of-range input, so the two differ
 * only for values >= 2^31 (and for (s16)/(u8) casts of such values, which take
 * the low bits of that 32-bit result: GC 0x7FFFFFFF -> -1, x86 -> 0). No game
 * site has been seen with such a value (the UBSan sweeps peak around 1.5e5);
 * use this where one could. */
static inline int sms_cvt_fp2int(double d)
{
	if (d != d)
		return (int)0x80000000u;
	if (d >= 2147483648.0)
		return 0x7FFFFFFF;
	if (d < -2147483648.0)
		return (int)0x80000000u;
	return (int)d;
}

#endif /* PORT_MWCC_H */
