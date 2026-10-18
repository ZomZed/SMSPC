#ifndef JUT_COLOR_HPP
#define JUT_COLOR_HPP

#include "dolphin/gx/GXStruct.h"

namespace JUtility {

struct TColor : public GXColor {
	TColor(u8 r, u8 g, u8 b, u8 a) { set(r, g, b, a); }
	TColor() { set(0xffffffff); }
	TColor(u32 u32Color) { set(u32Color); }
	TColor(GXColor color) { set(color); }

	// TColor(const TColor& other) { *(GXColor*)this = *(GXColor*)&other; }

	operator u32() const { return toUInt32(); }
#ifdef SMS_PORT
	// PC port: the u32 form is 0xRRGGBBAA (what GX and the game's constants
	// use); reading the bytes as a native u32 would reverse it on little-endian.
	u32 toUInt32() const
	{
		return (u32)r << 24 | (u32)g << 16 | (u32)b << 8 | (u32)a;
	}
#else
	u32 toUInt32() const { return *(u32*)&r; }
#endif

	void set(u8 cR, u8 cG, u8 cB, u8 cA)
	{
		r = cR;
		g = cG;
		b = cB;
		a = cA;
	}

#ifdef SMS_PORT
	void set(u32 u32Color)
	{
		set((u8)(u32Color >> 24), (u8)(u32Color >> 16), (u8)(u32Color >> 8),
		    (u8)u32Color);
	}
#else
	void set(u32 u32Color) { *(u32*)&r = u32Color; }
#endif
	void set(GXColor gxColor) { *(GXColor*)&r = gxColor; }
	GXColor get() const { return *this; }
};

} // namespace JUtility

#endif
