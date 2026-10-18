#ifndef J_SUPPORT_HPP
#define J_SUPPORT_HPP

#include <dolphin/types.h>
#include <stdint.h>

template <typename T> T* JSUConvertOffsetToPtr(const void* ptr, u32 offset)
{
	if (offset == 0) {
		return nullptr;
	} else {
		return (T*)((intptr_t)ptr + offset);
	}
}

template <typename T>
T* JSUConvertOffsetToPtr(const void* ptr, const void* offset)
{
	if (offset == nullptr) {
		return nullptr;
	} else {
#ifdef SMS_PORT
		// The "pointer" always carries a 32-bit file offset (callers cast an
		// s32 to void*); take the low 32 bits, sign-extended, as on the GC.
		return (T*)((intptr_t)ptr + (s32)(intptr_t)offset);
#else
		return (T*)((intptr_t)ptr + (s32)offset);
#endif
	}
}

#ifdef SMS_PORT
/*
 * A 32-bit file offset in a struct that is read in place from a file (J3D
 * blocks). On the GameCube these members are declared as pointers (4 bytes,
 * holding the offset); on a 64-bit host a pointer would be 8 bytes and break
 * the file layout, so the PC build stores the raw u32 instead. It converts to
 * u32 implicitly, so `JSUConvertOffsetToPtr<T>(block, block->mpX)` picks the
 * u32 overload and `(u32)block->mpX` keeps working. See port/POINTERS.md.
 */
template <typename T> struct JSUFileOffset {
	u32 mOffset;
	operator u32() const { return mOffset; }
	bool operator==(decltype(nullptr)) const { return mOffset == 0; }
	bool operator!=(decltype(nullptr)) const { return mOffset != 0; }
};
#define JSU_FILE_PTR(T) JSUFileOffset<T>
#else
#define JSU_FILE_PTR(T) T*
#endif

inline u16 JSULoHalf(u32 in) { return in & 0xffff; }
inline u8 JSULoByte(u16 in) { return in & 0xff; }
inline u8 JSUHiByte(u16 in) { return in >> 8; }

#endif
