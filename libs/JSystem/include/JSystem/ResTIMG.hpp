#ifndef RESTIMG_HPP
#define RESTIMG_HPP

#include <dolphin/types.h>

// The two offsets are relative to the header and can be negative at run time:
// J3DTexture::setResTIMG, TMapMirror and TModelGate store deltas to another
// header (`&src + src.off - &dst`). On x64 a u32 delta would be zero-extended
// by every `(u8*)img + img->off` reader and land ~4 GB away, so on PC they are
// s32 (same bytes, sign-extended on use). Game memory is one arena below
// 4 GB of 32 MB (port_sdk.h), so every delta fits.
struct ResTIMG {
	/* 0x00 */ u8 format;
	/* 0x01 */ u8 alphaEnabled;
	/* 0x02 */ u16 width;
	/* 0x04 */ u16 height;
	/* 0x06 */ u8 wrapS;
	/* 0x07 */ u8 wrapT;
	/* 0x08 */ u8 isIndexTexture;
	/* 0x09 */ u8 colorFormat;
	/* 0x0A */ u16 numColors;
#ifdef SMS_PORT
	/* 0x0C */ s32 paletteOffset;
#else
	/* 0x0C */ u32 paletteOffset;
#endif
	/* 0x10 */ u8 mipmapEnabled;
	/* 0x11 */ u8 doEdgeLod;
	/* 0x12 */ u8 biasClamp;
	/* 0x13 */ u8 maxAnisotropy;
	/* 0x14 */ u8 minFilter;
	/* 0x15 */ u8 magFilter;
	/* 0x16 */ s8 minLod;
	/* 0x17 */ s8 maxLod;
	/* 0x18 */ u8 mipmapCount;
	/* 0x19 */ char unk19[1];
	/* 0x1A */ s16 lodBias;
#ifdef SMS_PORT
	/* 0x1C */ s32 imageDataOffset;
#else
	/* 0x1C */ u32 imageDataOffset;
#endif
}; // Size: 0x20

#endif
