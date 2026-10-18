// port_mp_palette.cpp - Mario colour palettes (character customisation groundwork), w15.
//
// Mario's colours are baked into one CMPR (DXT1) atlas, H_ma_new_main_s3tc (256x256):
// every body material samples it, and the hand and cap models copy its header. So a
// palette is applied to the texture, not to TEV colours: each 4x4 block's two RGB565
// endpoints are classified (atlas region + hue/saturation/value) as cap, shirt,
// overalls, gloves, shoes, skin or hair and replaced by the palette colour scaled by the
// endpoint's brightness, so the painted shading survives. Blocks keep their CMPR mode
// (endpoints are swapped and the indices remapped when the new order would change it).
//
// - Puppets get their own copy of the image and of every texture header of their own
//   model data (body, hands, cap), so nothing is shared with the local Mario.
// - The local Mario is recoloured in place (mario.arc stays loaded for the whole game),
//   always from a pristine copy, on the next logic frame after the palette (or the
//   character menu's preview) changes, then Aurora's texture-object cache is dropped so
//   the locked display lists rebind it. Only when the player picked a palette: default
//   (all colours 0) = untouched GameCube data.
//
// Palette = w10's PortMpCharacter (port_mp.h): colors[0..5] = cap, shirt, overalls,
// gloves, shoes, skin, colors[6] = hair (w10 grew the array to 7: same bytes as the old
// reserved[0..2]). An all-zero colour leaves that part as it is.
#include "port_mp_puppet.h"
#include "port_mp.h"

#include <Player/Mario.hpp>
#include <System/MarDirector.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <dolphin/os.h>
#include <dolphin/gx.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

const char kMainTex[] = "H_ma_new_main_s3tc";

struct Preset {
	const char* name;
	u8 rgb[PORT_MP_PAL_SLOTS][3]; // cap, shirt, overalls, gloves, shoes, skin, hair (0 = keep)
};

// clang-format off
const Preset kPresets[] = {
	{ "Classic",       { { 0 } } },
	{ "Green (Luigi)", { { 40, 170, 50 }, { 40, 170, 50 }, { 40, 50, 150 }, { 0 }, { 110, 60, 25 }, { 0 }, { 0 } } },
	{ "Yellow (Wario)",{ { 245, 205, 30 }, { 245, 205, 30 }, { 120, 40, 160 }, { 255, 255, 255 }, { 40, 130, 50 }, { 0 }, { 0 } } },
	{ "Purple (Waluigi)", { { 110, 40, 170 }, { 110, 40, 170 }, { 30, 30, 70 }, { 0 }, { 230, 130, 30 }, { 0 }, { 0 } } },
	{ "Fire",          { { 245, 245, 245 }, { 245, 245, 245 }, { 220, 35, 35 }, { 0 }, { 0 }, { 0 }, { 0 } } },
	{ "Shadow",        { { 60, 60, 110 }, { 60, 60, 110 }, { 30, 30, 60 }, { 90, 90, 150 }, { 40, 40, 80 }, { 110, 110, 170 }, { 20, 20, 40 } } },
	{ "Pink",          { { 245, 120, 180 }, { 245, 120, 180 }, { 250, 245, 250 }, { 0 }, { 200, 60, 120 }, { 0 }, { 230, 190, 80 } } },
	{ "Aqua",          { { 40, 200, 210 }, { 40, 200, 210 }, { 30, 70, 120 }, { 0 }, { 40, 40, 40 }, { 0 }, { 0 } } },
	{ "Orange",        { { 250, 140, 30 }, { 250, 140, 30 }, { 70, 70, 80 }, { 0 }, { 60, 40, 30 }, { 0 }, { 0 } } },
	{ "Gold",          { { 235, 185, 40 }, { 235, 185, 40 }, { 160, 110, 20 }, { 255, 230, 120 }, { 120, 80, 10 }, { 0 }, { 0 } } },
	{ "Black & white", { { 30, 30, 30 }, { 30, 30, 30 }, { 230, 230, 230 }, { 0 }, { 30, 30, 30 }, { 0 }, { 0 } } },
};
// clang-format on
const int kPresetCount = (int)(sizeof(kPresets) / sizeof(kPresets[0]));

const u8* slotRgb(const PortMpCharacter& c, int slot)
{
	return c.colors[slot < 7 ? slot : 6]; // w10: hair is colors[6] (the same bytes as the old reserved[0..2])
}

bool slotSet(const PortMpCharacter& c, int slot)
{
	const u8* p = slotRgb(c, slot);
	return (p[0] | p[1] | p[2]) != 0;
}

u32 charHash(const PortMpCharacter& c)
{
	u32 h = 2166136261u;
	for (int s = 0; s < PORT_MP_PAL_SLOTS; ++s)
		for (int k = 0; k < 3; ++k)
			h = (h ^ slotRgb(c, s)[k]) * 16777619u;
	return h;
}

// The name-tag colour bytes (PortMpCharacter.reserved[0..3]) folded into a hash, 0 = none.
u32 tagHash(const PortMpCharacter& c)
{
	return c.reserved[0] == 1 ? (0x9E3779B9u ^ ((u32)c.reserved[1] << 16 | (u32)c.reserved[2] << 8 | c.reserved[3])) : 0;
}

// ---- classification -------------------------------------------------------

enum Region { R_CAP, R_GLOVE, R_ARM, R_SHOE, R_OVERALL, R_HEAD, R_FACE, R_LOGO };

// Parts of the atlas (normalised u, v of the block centre); see BOOTLOG.
Region region(f32 u, f32 v)
{
	if (v >= 0.625f)
		return u >= 0.75f ? R_CAP : u >= 0.5f ? R_GLOVE : u >= 0.25f ? R_ARM : R_SHOE;
	if (u < 0.5f)
		return R_OVERALL;
	if (v < 0.375f)
		return R_HEAD;
	return u < 0.75f ? R_FACE : R_LOGO;
}

void rgb565(u16 c, int out[3])
{
	const int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
	out[0] = r << 3 | r >> 2;
	out[1] = g << 2 | g >> 4;
	out[2] = b << 3 | b >> 2;
}

u16 to565(const f32 c[3])
{
	int v[3];
	for (int k = 0; k < 3; ++k) {
		f32 x = c[k] + 0.5f;
		v[k]  = x <= 0.0f ? 0 : (x >= 255.0f ? 255 : (int)x);
	}
	return (u16)((v[0] >> 3) << 11 | (v[1] >> 2) << 5 | (v[2] >> 3));
}

// -1 = leave as is.
int classify(const int rgb[3], Region reg)
{
	const f32 r = rgb[0] / 255.0f, g = rgb[1] / 255.0f, b = rgb[2] / 255.0f;
	const f32 mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	const f32 mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
	const f32 v = mx, s = mx > 0.0f ? (mx - mn) / mx : 0.0f;
	f32 h = 0.0f;
	if (mx > mn) {
		const f32 d = mx - mn;
		if (mx == r)
			h = 60.0f * ((g - b) / d);
		else if (mx == g)
			h = 60.0f * ((b - r) / d + 2.0f);
		else
			h = 60.0f * ((r - g) / d + 4.0f);
		if (h < 0.0f)
			h += 360.0f;
	}
	const bool red   = (h <= 20.0f || h >= 335.0f) && s >= 0.6f && v >= 0.2f;
	const bool blue  = h >= 190.0f && h <= 265.0f && s >= 0.3f && v >= 0.1f;
	const bool skin  = h >= 8.0f && h <= 40.0f && s >= 0.12f && s <= 0.62f && v >= 0.62f;
	const bool brown = (h <= 45.0f || h >= 340.0f) && s >= 0.3f && v < 0.75f;
	const bool grey  = s <= 0.22f && v >= 0.35f;
	if ((reg == R_ARM || reg == R_HEAD || reg == R_FACE) && skin)
		return PORT_MP_PAL_SKIN;
	if (reg == R_CAP && red)
		return PORT_MP_PAL_CAP;
	if ((reg == R_OVERALL || reg == R_ARM) && red)
		return PORT_MP_PAL_SHIRT;
	if (reg == R_OVERALL && blue)
		return PORT_MP_PAL_OVERALLS;
	if (reg == R_GLOVE && grey)
		return PORT_MP_PAL_GLOVES;
	if (reg == R_SHOE && (s >= 0.25f || v < 0.5f))
		return PORT_MP_PAL_SHOES;
	if (reg == R_HEAD && (brown || v < 0.35f))
		return PORT_MP_PAL_HAIR;
	if (reg == R_FACE && v < 0.3f) // moustache
		return PORT_MP_PAL_HAIR;
	return -1;
}

u32 cmprSize(int w, int h, int mips)
{
	u32 n = 0;
	for (int i = 0; i < (mips < 1 ? 1 : mips); ++i) {
		const int tw = ((w > 8 ? w : 8) + 7) & ~7, th = ((h > 8 ? h : 8) + 7) & ~7;
		n += (u32)tw * th / 2;
		w >>= 1, h >>= 1;
	}
	return n;
}

// Recolours src into dst (same size). Data is GX order: 8x8 tiles of four 4x4 DXT1
// blocks, big-endian RGB565 endpoints.
void recolor(const u8* src, u8* dst, int w0, int h0, int mips, const PortMpCharacter& c)
{
	memcpy(dst, src, cmprSize(w0, h0, mips));
	for (int pass = 0; pass < 2; ++pass) {
		static int vref[PORT_MP_PAL_SLOTS];
		if (pass == 0)
			memset(vref, 0, sizeof(vref));
		const u8* s = src;
		u8* d       = dst;
		int w = w0, h = h0;
		for (int lvl = 0; lvl < (mips < 1 ? 1 : mips); ++lvl) {
			const int tw = ((w > 8 ? w : 8) + 7) & ~7, th = ((h > 8 ? h : 8) + 7) & ~7;
			for (int ty = 0; ty < th; ty += 8)
				for (int tx = 0; tx < tw; tx += 8)
					for (int sub = 0; sub < 4; ++sub, s += 8, d += 8) {
						const f32 u = (tx + (sub & 1) * 4 + 2) / (f32)tw;
						const f32 v = (ty + (sub >> 1) * 4 + 2) / (f32)th;
						const Region reg = region(u, v);
						const u16 c0 = (u16)(s[0] << 8 | s[1]), c1 = (u16)(s[2] << 8 | s[3]);
						u16 n[2]      = { c0, c1 };
						for (int e = 0; e < 2; ++e) {
							int rgb[3];
							rgb565(n[e], rgb);
							const int cat = classify(rgb, reg);
							if (cat < 0 || !slotSet(c, cat))
								continue;
							const int mx = rgb[0] > rgb[1] ? (rgb[0] > rgb[2] ? rgb[0] : rgb[2])
							                               : (rgb[1] > rgb[2] ? rgb[1] : rgb[2]);
							if (pass == 0) {
								if (mx > vref[cat])
									vref[cat] = mx;
								continue;
							}
							const f32 shade = mx / (vref[cat] * 0.95f > 1.0f ? vref[cat] * 0.95f : 1.0f);
							const u8* t     = slotRgb(c, cat);
							const f32 o[3]  = { t[0] * shade, t[1] * shade, t[2] * shade };
							n[e]            = to565(o);
						}
						if (pass == 0)
							continue;
						u8 idx[4] = { s[4], s[5], s[6], s[7] };
						if (c0 > c1) { // 4-colour block: keep n0 > n1
							if (n[0] == n[1]) {
								if (n[0] < 0xFFFF)
									++n[0];
								else
									--n[1];
							}
							if (n[0] < n[1]) {
								const u16 t = n[0];
								n[0] = n[1], n[1] = t;
								for (int k = 0; k < 4; ++k)
									idx[k] ^= 0x55; // 0<->1, 2<->3
							}
						} else if (n[0] > n[1]) { // 3-colour + transparent: keep n0 <= n1
							const u16 t = n[0];
							n[0] = n[1], n[1] = t;
							for (int k = 0; k < 4; ++k) {
								u8 o = 0;
								for (int px = 0; px < 4; ++px) {
									u8 m = (idx[k] >> (6 - 2 * px)) & 3;
									if (m < 2)
										m ^= 1;
									o |= (u8)(m << (6 - 2 * px));
								}
								idx[k] = o;
							}
						}
						d[0] = (u8)(n[0] >> 8), d[1] = (u8)n[0], d[2] = (u8)(n[1] >> 8), d[3] = (u8)n[1];
						memcpy(d + 4, idx, 4);
					}
			w >>= 1, h >>= 1;
		}
	}
}

int findMainTex(J3DModelData* d)
{
	if (d == nullptr || d->getTexture() == nullptr || d->getTextureName() == nullptr)
		return -1;
	for (u16 i = 0; i < d->getTexture()->getNum(); ++i)
		if (strcmp(d->getTextureName()->getName(i), kMainTex) == 0)
			return i;
	return -1;
}

u8* imageOf(ResTIMG* t) { return (u8*)t + t->imageDataOffset; }

// The original atlas (host memory), taken before anything recoloured it.
u8* sPristine;
u32 sPristineSize;
int sPristineW, sPristineH, sPristineMips;

bool savePristine(ResTIMG* t)
{
	if (sPristine != nullptr)
		return true;
	if (t->format != GX_TF_CMPR)
		return false;
	sPristineW    = t->width;
	sPristineH    = t->height;
	sPristineMips = t->mipmapCount;
	sPristineSize = cmprSize(t->width, t->height, t->mipmapCount);
	sPristine     = (u8*)malloc(sPristineSize);
	memcpy(sPristine, imageOf(t), sPristineSize);
	return true;
}

// Local player's palette (set by the menu, or SMS_PORT_PALETTE=<preset name or index>).
PortMpCharacter sLocal;
bool sLocalChecked;
u32 sLocalApplied = 0;    // hash of what is in mario.arc now (0 = pristine)

void checkLocalEnv()
{
	if (sLocalChecked)
		return;
	sLocalChecked = true;
	const char* e = getenv("SMS_PORT_PALETTE");
	if (e == nullptr || e[0] == '\0')
		return;
	for (int i = 0; i < kPresetCount; ++i)
		if (strncmp(kPresets[i].name, e, strlen(e)) == 0 || atoi(e) == i && e[0] >= '0' && e[0] <= '9') {
			port_mp_palette_preset(i, &sLocal);
			OSReport("[mp] palette: local preset %d (%s)\n", i, kPresets[i].name);
			return;
		}
}

} // namespace

// ---------------------------------------------------------------------------

extern "C" int port_mp_palette_preset_count(void) { return kPresetCount; }

extern "C" const char* port_mp_palette_preset_name(int i)
{
	return i >= 0 && i < kPresetCount ? kPresets[i].name : "";
}

extern "C" void port_mp_palette_preset(int i, PortMpCharacter* out)
{
	memset(out, 0, sizeof(*out));
	if (i < 0 || i >= kPresetCount)
		return;
	out->preset = (u8)i;
	for (int s = 0; s < PORT_MP_PAL_SLOTS; ++s)
		memcpy((u8*)slotRgb(*out, s), kPresets[i].rgb[s], 3);
}

extern "C" int port_mp_palette_is_default(const PortMpCharacter* c)
{
	if (c == nullptr)
		return 1;
	for (int s = 0; s < PORT_MP_PAL_SLOTS; ++s)
		if (slotSet(*c, s))
			return 0;
	return 1;
}

extern "C" void port_mp_palette_set_local(const PortMpCharacter* c)
{
	checkLocalEnv();
	if (c == nullptr)
		memset(&sLocal, 0, sizeof(sLocal));
	else
		sLocal = *c;
}

// The name-tag colour of a player (0xRRGGBB): the one chosen in Customize (sent with the
// character), else a colour by slot. Used by the player lists, the lobby panel and the tags.
extern "C" uint32_t port_mp_tag_rgb(int slot)
{
	PortMpPlayerInfo p;
	if (slot >= 0 && slot < PORT_MP_MAX_PLAYERS && port_mp_get_player(slot, &p) && p.used) {
		const PortMpCharacter& c = p.isLocal ? sLocal : p.character;
		if (c.reserved[0] == 1)
			return (uint32_t)c.reserved[1] << 16 | (uint32_t)c.reserved[2] << 8 | c.reserved[3];
	}
	static const uint32_t kSlot[PORT_MP_MAX_PLAYERS] = { 0xFF5046, 0x5ADC5A, 0x5AA0FF, 0xFFDC3C,
		                                                 0xE66EFF, 0x46E6E6, 0xFF9632, 0xF0F0F0 };
	return kSlot[(slot < 0 ? 0 : slot) % PORT_MP_MAX_PLAYERS];
}

extern "C" void port_mp_palette_get_local(PortMpCharacter* out)
{
	checkLocalEnv();
	*out = sLocal;
}

// Gives a model data its own copy of every texture header (offsets re-based), so
// later setResTIMG / image changes do not write into the shared file.
void portMpPaletteOwnHeaders(J3DModelData* d)
{
	J3DTexture* tex = d->getTexture();
	if (tex == nullptr || tex->mNum == 0)
		return;
	ResTIMG* src = tex->mResources;
	ResTIMG* dst = (ResTIMG*)JKRHeap::alloc(sizeof(ResTIMG) * tex->mNum, 0x20, nullptr);
	if (dst == nullptr)
		return;
	for (u16 i = 0; i < tex->mNum; ++i) {
		dst[i]                 = src[i];
		dst[i].imageDataOffset = (s32)((uintptr_t)&src[i] + src[i].imageDataOffset - (uintptr_t)&dst[i]);
		dst[i].paletteOffset   = (s32)((uintptr_t)&src[i] + src[i].paletteOffset - (uintptr_t)&dst[i]);
	}
	tex->mResources = dst;
}

// A puppet's body data (already with its own headers): its own copy of the atlas,
// recoloured (or the original for classic), so a later change can be made in place
// (portMpPaletteRecolorInto). Returns the copy, or null.
void* portMpPaletteApplyModel(J3DModelData* d, const PortMpCharacter* c)
{
	const int mi = findMainTex(d);
	if (mi < 0)
		return nullptr;
	ResTIMG* t = d->getTexture()->getResTIMG(mi);
	if (!savePristine(t) || t->width != sPristineW || t->height != sPristineH)
		return nullptr;
	u8* img = (u8*)JKRHeap::alloc(sPristineSize, 0x20, nullptr);
	if (img == nullptr)
		return nullptr;
	if (c == nullptr || port_mp_palette_is_default(c))
		memcpy(img, sPristine, sPristineSize);
	else
		recolor(sPristine, img, sPristineW, sPristineH, sPristineMips, *c);
	DCFlushRange(img, sPristineSize);
	J3DTexture* tex = d->getTexture();
	for (u16 i = 0; i < tex->mNum; ++i)
		if (strcmp(d->getTextureName()->getName(i), kMainTex) == 0)
			tex->mResources[i].imageDataOffset = (s32)((uintptr_t)img - (uintptr_t)&tex->mResources[i]);
	return img;
}

extern "C" void port_mp_w15_no_set_character(const PortMpCharacter*) { }
#pragma comment(linker, "/alternatename:port_mp_set_character=port_mp_w15_no_set_character")

// Aurora (lib/gx): drop the texture-object cache, so textures bound from locked display
// lists are hashed again (a recoloured atlas shows at once; unchanged ones hit Aurora's
// content cache).
namespace aurora { namespace gx { void clear_static_texture_cache() noexcept; } }

namespace {
PortMpCharacter sPreview; // the character menu's live preview
bool sPreviewOn;
} // namespace

extern "C" void port_mp_palette_preview(const PortMpCharacter* c)
{
	sPreviewOn = c != nullptr;
	if (c != nullptr)
		sPreview = *c;
}

// The local Mario, every logic frame: make mario.arc's atlas match the local palette (or
// the menu's preview) whenever that changes - recoloured from the pristine copy, or the
// original put back - and have Aurora rebind it. Nothing happens while the palette stays
// classic, so single-player data is untouched by default.
// In a session, the committed palette is also published as our character (PLAYER_INFO),
// so the others' puppets wear it.
extern "C" void port_mp_palette_local_frame(void)
{
	static u32 sPublished = 0;
	checkLocalEnv();
	if (port_mp_active()) {
		// the palette (when not classic) and the name-tag colour (port_mp_tag_rgb)
		const u32 lh = (port_mp_palette_is_default(&sLocal) ? 0 : charHash(sLocal)) ^ tagHash(sLocal);
		if (lh != sPublished) {
			port_mp_set_character(&sLocal);
			sPublished = lh;
		}
	} else {
		sPublished = 0;
	}
	TMario* m = gpMarioOriginal;
	if (m == nullptr || m->mBodyModelData == nullptr)
		return;
	const PortMpCharacter& want = sPreviewOn ? sPreview : sLocal;
	const u32 h                 = port_mp_palette_is_default(&want) ? 0 : charHash(want);
	if (h == sLocalApplied)
		return;
	const int mi = findMainTex(m->mBodyModelData);
	if (mi < 0)
		return;
	ResTIMG* t = m->mBodyModelData->getTexture()->getResTIMG(mi);
	if (!savePristine(t) || t->width != sPristineW || t->height != sPristineH)
		return;
	u8* img = imageOf(t);
	if (h == 0)
		memcpy(img, sPristine, sPristineSize);
	else
		recolor(sPristine, img, sPristineW, sPristineH, sPristineMips, want);
	DCFlushRange(img, sPristineSize);
	aurora::gx::clear_static_texture_cache();
	sLocalApplied = h;
	if (!sPreviewOn)
		OSReport("[mp] palette: local Mario %s\n", h == 0 ? "back to the original colours" : "recoloured");
}

// A puppet's atlas copy (from portMpPaletteApplyModel) recoloured in place; Aurora
// rebinds it.
void portMpPaletteRecolorInto(void* img, const PortMpCharacter* c)
{
	if (img == nullptr || sPristine == nullptr)
		return;
	if (c == nullptr || port_mp_palette_is_default(c))
		memcpy(img, sPristine, sPristineSize);
	else
		recolor(sPristine, (u8*)img, sPristineW, sPristineH, sPristineMips, *c);
	DCFlushRange(img, sPristineSize);
	aurora::gx::clear_static_texture_cache();
}

u32 portMpPaletteHash(const PortMpCharacter* c)
{
	return c == nullptr || port_mp_palette_is_default(c) ? 0 : charHash(*c);
}
