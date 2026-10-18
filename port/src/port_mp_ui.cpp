// Sunshine-looking pieces for the multiplayer menus. See port_mp_ui.h.
//
// The game font: gpSystemFont (Application.cpp loads it at boot) keeps its glyph pages as raw
// GX textures in the font resource (GLY1 blocks; the headers are byte-swapped by
// port_swap_font, the images are not). Each page is decoded once to RGBA, registered with
// aurora_imgui_add_texture, and a character is drawn as a quad of its cell, placed the way
// JUTResFont::drawChar_scale places it (cell origin = pen - width.field_0x0, advance =
// width.field_0x1, the cell grid indexed with numRows as the column count, as the game does).
#ifdef SMS_PORT_AURORA

#include "port_mp_ui.h"

#include <aurora/imgui.h>

#include <JSystem/JUtility/JUTResFont.hpp>
#include <JSystem/JKernel/JKRDvdRipper.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <System/Application.hpp> // SMSLoadArchive

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

extern JUTResFont* gpSystemFont; // System/Application.cpp

namespace {

// ---- the game font --------------------------------------------------------------------------
struct FontPage {
	ImTextureID tex;
};
struct FontBlock {
	std::vector<FontPage> pages;
};
std::vector<FontBlock> sBlocks;
bool sFontTried, sFontOk;

// One GX intensity/alpha texture (I4, I8, IA4, IA8) to RGBA8: white, alpha = coverage, or for
// IA formats the intensity as the colour (outlines stay dark when tinted).
bool decodeFontTexture(const u8* src, int w, int h, int fmt, std::vector<u32>& out)
{
	out.assign((size_t)w * h, 0);
	auto put = [&](int x, int y, u8 i, u8 a) {
		if (x < w && y < h)
			out[(size_t)y * w + x] = (u32)a << 24 | (u32)i << 16 | (u32)i << 8 | i; // ABGR in memory: R=G=B=i
	};
	switch (fmt) {
	case 0: // I4: 8x8 tiles, 4 bpp
		for (int ty = 0; ty < h; ty += 8)
			for (int tx = 0; tx < w; tx += 8)
				for (int y = 0; y < 8; y++)
					for (int x = 0; x < 8; x += 2) {
						const u8 b = *src++;
						const u8 i0 = (u8)((b >> 4) * 17), i1 = (u8)((b & 15) * 17);
						put(tx + x, ty + y, 255, i0);
						put(tx + x + 1, ty + y, 255, i1);
					}
		return true;
	case 1: // I8: 8x4 tiles
		for (int ty = 0; ty < h; ty += 4)
			for (int tx = 0; tx < w; tx += 8)
				for (int y = 0; y < 4; y++)
					for (int x = 0; x < 8; x++) {
						const u8 i = *src++;
						put(tx + x, ty + y, 255, i);
					}
		return true;
	case 2: // IA4: 8x4 tiles, high nibble alpha
		for (int ty = 0; ty < h; ty += 4)
			for (int tx = 0; tx < w; tx += 8)
				for (int y = 0; y < 4; y++)
					for (int x = 0; x < 8; x++) {
						const u8 b = *src++;
						put(tx + x, ty + y, (u8)((b & 15) * 17), (u8)((b >> 4) * 17));
					}
		return true;
	case 3: // IA8: 4x4 tiles, alpha then intensity
		for (int ty = 0; ty < h; ty += 4)
			for (int tx = 0; tx < w; tx += 4)
				for (int y = 0; y < 4; y++)
					for (int x = 0; x < 4; x++) {
						const u8 a = *src++, i = *src++;
						put(tx + x, ty + y, i, a);
					}
		return true;
	default: return false;
	}
}

bool initFont()
{
	if (sFontTried)
		return sFontOk;
	JUTResFont* f = gpSystemFont;
	if (f == nullptr || ImGui::GetCurrentContext() == nullptr)
		return false; // try again next frame
	sFontTried = true;
	sBlocks.resize(f->mGlyphBlockNum);
	std::vector<u32> rgba;
	for (int b = 0; b < f->mGlyphBlockNum; b++) {
		const ResFONT::GLY1* g = f->mpGlyphBlocks[b];
		const int cells = g->numRows * g->numColumns;
		const int pages = cells > 0 ? (g->endCode - g->startCode + cells) / cells : 0;
		std::fprintf(stderr, "[mp-ui] font block %d: codes %u-%u, %dx%d cells, page %ux%u fmt %u, %d pages\n", b,
		             g->startCode, g->endCode, g->cellWidth, g->cellHeight, g->textureWidth, g->textureHeight,
		             g->textureFormat, pages);
		for (int p = 0; p < pages; p++) {
			FontPage pg{};
			if (decodeFontTexture(&g->data[p * g->textureSize], g->textureWidth, g->textureHeight, g->textureFormat,
			                      rgba))
				pg.tex = aurora_imgui_add_texture(g->textureWidth, g->textureHeight, rgba.data());
			sBlocks[b].pages.push_back(pg);
		}
	}
	sFontOk = !sBlocks.empty();
	return sFontOk;
}

// Calls fn(textureOrNull, uv0, uv1, x0, w) for each character; returns the advance in font units.
template <typename F> float forEachGlyph(const char* text, F fn)
{
	JUTResFont* f = gpSystemFont;
	float pen     = 0.0f;
	for (const char* c = text; *c != '\0'; ++c) {
		const int code = f->getFontCode((u8)*c);
		JUTFont::TWidth wd;
		f->getWidthEntry((u8)*c, &wd);
		for (int b = 0; b < f->mGlyphBlockNum; b++) {
			const ResFONT::GLY1* g = f->mpGlyphBlocks[b];
			if (code < g->startCode || code > g->endCode)
				continue;
			const int idx = code - g->startCode, cells = g->numRows * g->numColumns;
			const int page = idx / cells, cell = idx % cells;
			const int row = cell / g->numRows, col = cell - row * g->numRows; // as JUTResFont::loadImage
			const float u0 = (float)(col * g->cellWidth) / g->textureWidth;
			const float v0 = (float)(row * g->cellHeight) / g->textureHeight;
			const float u1 = (float)(col * g->cellWidth + f->getWidth()) / g->textureWidth;
			const float v1 = (float)(row * g->cellHeight + f->getHeight()) / g->textureHeight;
			if (page < (int)sBlocks[b].pages.size())
				fn(sBlocks[b].pages[page].tex, ImVec2(u0, v0), ImVec2(u1, v1), pen - wd.field_0x0);
			break;
		}
		pen += wd.field_0x1;
	}
	return pen;
}

ImU32 withAlpha(ImU32 c, float a)
{
	const int ca = (int)((c >> IM_COL32_A_SHIFT) & 0xFF);
	return (c & ~IM_COL32_A_MASK) | ((ImU32)(ca * a) << IM_COL32_A_SHIFT);
}

double nowSec() { return ImGui::GetTime(); }

// colours
const ImU32 kBlue      = IM_COL32(36, 68, 238, 255);  // the file-select boxes
const ImU32 kBlueLight = IM_COL32(70, 104, 255, 255);
const ImU32 kNavy      = IM_COL32(16, 30, 120, 238);
const ImU32 kNavyEdge  = IM_COL32(8, 16, 70, 255);
const ImU32 kYellow    = IM_COL32(255, 222, 60, 255);
const ImU32 kWhite     = IM_COL32(255, 255, 255, 255);

// ---- icons from the game's own textures ---------------------------------------------------
// Read once on the game thread (mpui_load_game_icons, from the file select's setup): each
// archive is read from the player's disc (JKRDvdToMainRam expands the Yaz0), the BTI files
// are found by name in the RARC, decoded to RGBA and kept; the render thread registers them
// with ImGui on first use. Nothing from the game is shipped with the port.
struct GameIcon {
	const char* arc;  // /data/<arc>.arc
	const char* file; // the file name inside it
	int w, h;
	std::vector<u8> rgba;
	ImTextureID tex;
};
GameIcon sGameIcons[] = {
	{ "game_6", "shine_icon.bti" },      // 0 Shine
	{ "game_6", "mario_icon.bti" },      // 1 Mario's head
	{ "game_6", "monte_icon.bti" },      // 2 a Pianta
	{ "option", "select_arrow.bti" },    // 3 the white arrow (points right)
	{ "option", "load_sun_1.bti" },      // 4 the loading sun
	{ "option", "shine_icon_min.bti" },  // 5 a small Shine
	{ "common", "question.bti" },        // 6 "?"
	{ "game_6", "coin_icon.bti" },       // 7 a coin
	{ "game_6", "water_nozzle_01.bti" }, // 8 FLUDD's hover nozzle
};
const int kGameIconCount = (int)(sizeof(sGameIcons) / sizeof(sGameIcons[0]));
std::atomic<bool> sGameIconsReady{ false };

u16 be16(const u8* p) { return (u16)(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

// A file inside a (decompressed) RARC, by name. Returns its bytes, or null.
const u8* rarcFind(const u8* d, u32 size, const char* name, u32* outSize)
{
	if (size < 0x40 || std::memcmp(d, "RARC", 4) != 0)
		return nullptr;
	const u32 dataOff = be32(d + 0x0C) + 0x20;
	const u8* info    = d + 0x20;
	const u32 nEnt = be32(info + 8), entOff = be32(info + 12) + 0x20, strOff = be32(info + 20) + 0x20;
	for (u32 i = 0; i < nEnt; i++) {
		const u8* e = d + entOff + i * 20;
		if (entOff + i * 20 + 20 > size)
			break;
		const u8 type = e[4];
		if (type & 2) // a directory
			continue;
		const char* n = (const char*)d + strOff + be16(e + 6);
		if (std::strcmp(n, name) == 0) {
			*outSize = be32(e + 12);
			return d + dataOff + be32(e + 8);
		}
	}
	return nullptr;
}

void rgb5a3(u16 c, u8* o)
{
	if (c & 0x8000) {
		o[0] = (u8)((c >> 10 & 31) * 255 / 31);
		o[1] = (u8)((c >> 5 & 31) * 255 / 31);
		o[2] = (u8)((c & 31) * 255 / 31);
		o[3] = 255;
	} else {
		o[0] = (u8)((c >> 8 & 15) * 17);
		o[1] = (u8)((c >> 4 & 15) * 17);
		o[2] = (u8)((c & 15) * 17);
		o[3] = (u8)((c >> 12 & 7) * 255 / 7);
	}
}

// A BTI (header big-endian, as on the disc) to RGBA bytes. I4, I8, IA4, IA8, RGB565, RGB5A3,
// RGBA8 and CMPR (what the menus use).
bool decodeBti(const u8* b, u32 size, int* w, int* h, std::vector<u8>& out)
{
	if (size < 0x20)
		return false;
	const int fmt = b[0];
	const int W = be16(b + 2), H = be16(b + 4);
	const u8* src = b + be32(b + 0x1C);
	static const int kTw[15] = { 8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 0, 0, 0, 0, 8 };
	static const int kTh[15] = { 8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 0, 0, 0, 0, 8 };
	if (fmt > 14 || kTw[fmt] == 0 || fmt == 8 || fmt == 9)
		return false;
	*w = W;
	*h = H;
	out.assign((size_t)W * H * 4, 0);
	auto px = [&](int x, int y) -> u8* { return x < W && y < H ? &out[((size_t)y * W + x) * 4] : nullptr; };
	const int tw = kTw[fmt], th = kTh[fmt];
	for (int ty = 0; ty < H; ty += th)
		for (int tx = 0; tx < W; tx += tw) {
			if (fmt == 14) { // CMPR
				for (int sb = 0; sb < 4; sb++, src += 8) {
					const int sx = tx + (sb & 1) * 4, sy = ty + (sb >> 1) * 4;
					const u16 c0 = be16(src), c1 = be16(src + 2);
					u8 pal[4][4];
					for (int k = 0; k < 2; k++) {
						const u16 c = k ? c1 : c0;
						pal[k][0] = (u8)((c >> 11 & 31) * 255 / 31);
						pal[k][1] = (u8)((c >> 5 & 63) * 255 / 63);
						pal[k][2] = (u8)((c & 31) * 255 / 31);
						pal[k][3] = 255;
					}
					for (int ch = 0; ch < 3; ch++) {
						pal[2][ch] = c0 > c1 ? (u8)((2 * pal[0][ch] + pal[1][ch]) / 3) : (u8)((pal[0][ch] + pal[1][ch]) / 2);
						pal[3][ch] = c0 > c1 ? (u8)((pal[0][ch] + 2 * pal[1][ch]) / 3) : 0;
					}
					pal[2][3] = 255;
					pal[3][3] = c0 > c1 ? 255 : 0;
					for (int y = 0; y < 4; y++)
						for (int x = 0; x < 4; x++)
							if (u8* o = px(sx + x, sy + y))
								std::memcpy(o, pal[(src[4 + y] >> (6 - 2 * x)) & 3], 4);
				}
				continue;
			}
			if (fmt == 6) { // RGBA8: A,R pairs then G,B pairs
				for (int i = 0; i < 16; i++)
					if (u8* o = px(tx + i % 4, ty + i / 4)) {
						o[3] = src[2 * i];
						o[0] = src[2 * i + 1];
						o[1] = src[32 + 2 * i];
						o[2] = src[32 + 2 * i + 1];
					}
				src += 64;
				continue;
			}
			for (int y = 0; y < th; y++)
				for (int x = 0; x < tw; x++) {
					u8* o = px(tx + x, ty + y);
					u8 tmp[4];
					if (o == nullptr)
						o = tmp;
					const int i = y * tw + x;
					switch (fmt) {
					case 0: { const u8 v = (u8)(((i & 1) ? src[i / 2] & 15 : src[i / 2] >> 4) * 17); o[0] = o[1] = o[2] = 255; o[3] = v; break; }
					case 1: o[0] = o[1] = o[2] = 255; o[3] = src[i]; break;
					case 2: o[0] = o[1] = o[2] = (u8)((src[i] & 15) * 17); o[3] = (u8)((src[i] >> 4) * 17); break;
					case 3: o[3] = src[2 * i]; o[0] = o[1] = o[2] = src[2 * i + 1]; break;
					case 4: { const u16 c = be16(src + 2 * i); o[0] = (u8)((c >> 11 & 31) * 255 / 31); o[1] = (u8)((c >> 5 & 63) * 255 / 63); o[2] = (u8)((c & 31) * 255 / 31); o[3] = 255; break; }
					case 5: rgb5a3(be16(src + 2 * i), o); break;
					}
				}
			src += 32;
		}
	return true;
}

// Draws game icon `k` fitted in a square of radius r around c (rotation in radians, mirror).
bool drawGameIcon(ImDrawList* dl, int k, ImVec2 c, float r, ImU32 tint, float rot = 0.0f, bool mirror = false)
{
	if (!sGameIconsReady.load() || k < 0 || k >= kGameIconCount)
		return false;
	GameIcon& g = sGameIcons[k];
	if (g.rgba.empty())
		return false;
	if (g.tex == 0)
		g.tex = aurora_imgui_add_texture(g.w, g.h, g.rgba.data());
	const float s  = 2.0f * r / (float)(g.w > g.h ? g.w : g.h);
	const float hw = g.w * s * 0.5f, hh = g.h * s * 0.5f;
	const float cs = std::cos(rot), sn = std::sin(rot);
	auto pt = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
	const float u0 = mirror ? 1.0f : 0.0f, u1 = mirror ? 0.0f : 1.0f;
	dl->AddImageQuad(g.tex, pt(-hw, -hh), pt(hw, -hh), pt(hw, hh), pt(-hw, hh), ImVec2(u0, 0), ImVec2(u1, 0),
	                 ImVec2(u1, 1), ImVec2(u0, 1), tint);
	return true;
}

} // namespace

bool mpui_font_ready(void) { return initFont(); }

void mpui_load_game_icons(void)
{
	static bool sTried;
	if (sTried)
		return;
	sTried = true;
	int loaded = 0;
	for (int k = 0; k < kGameIconCount; k++) {
		if (!sGameIcons[k].rgba.empty())
			continue;
		char path[48];
		std::snprintf(path, sizeof(path), "/data/%s.arc", sGameIcons[k].arc);
		u8* arc = (u8*)SMSLoadArchive(path, nullptr, 0, nullptr); // the .szs, expanded, on the current heap
		const int size = arc != nullptr ? (int)be32(arc + 4) : 0; // RARC: the file size
		if (arc == nullptr) {
			std::fprintf(stderr, "[mp-ui] icons: %s not readable\n", path);
			continue;
		}
		// every icon from this archive while it is loaded
		for (int j = k; j < kGameIconCount; j++) {
			GameIcon& g = sGameIcons[j];
			if (std::strcmp(g.arc, sGameIcons[k].arc) != 0 || !g.rgba.empty())
				continue;
			u32 fsize     = 0;
			const u8* bti = rarcFind(arc, (u32)size, g.file, &fsize);
			if (bti != nullptr && decodeBti(bti, fsize, &g.w, &g.h, g.rgba))
				loaded++;
			else
				std::fprintf(stderr, "[mp-ui] icons: %s not found in %s\n", g.file, path);
		}
		JKRHeap::free(arc, nullptr);
	}
	std::fprintf(stderr, "[mp-ui] icons: %d of %d game textures loaded\n", loaded, kGameIconCount);
	sGameIconsReady = true;
}

float mpui_scale(void)
{
	const float s = ImGui::GetIO().DisplaySize.y / 480.0f;
	return s < 1.0f ? 1.0f : (s > 3.0f ? 3.0f : s);
}

ImVec2 mpui_text_size(const char* text, float px)
{
	if (!initFont())
		return ImGui::GetFont()->CalcTextSizeA(px, 1e9f, 0.0f, text);
	const float sc = px / (float)gpSystemFont->getHeight();
	const float w  = forEachGlyph(text, [](ImTextureID, ImVec2, ImVec2, float) {});
	return ImVec2(w * sc, px);
}

void mpui_text(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* text, bool shadow)
{
	if (!initFont()) {
		if (shadow)
			dl->AddText(ImGui::GetFont(), px, ImVec2(pos.x + 2, pos.y + 2), IM_COL32(0, 0, 40, 160), text);
		dl->AddText(ImGui::GetFont(), px, pos, col, text);
		return;
	}
	const float sc = px / (float)gpSystemFont->getHeight();
	const float fw = (float)gpSystemFont->getWidth() * sc;
	for (int pass = shadow ? 0 : 1; pass < 2; pass++) {
		const float off = pass == 0 ? px * 0.08f : 0.0f;
		const ImU32 c   = pass == 0 ? withAlpha(IM_COL32(0, 8, 60, 255), ((col >> IM_COL32_A_SHIFT) & 0xFF) / 400.0f) : col;
		forEachGlyph(text, [&](ImTextureID tex, ImVec2 uv0, ImVec2 uv1, float x) {
			if (tex == 0)
				return;
			const ImVec2 a(pos.x + x * sc + off, pos.y + off);
			dl->AddImage(tex, a, ImVec2(a.x + fw, a.y + px), uv0, uv1, c);
		});
	}
}

void mpui_panel(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, bool rivets)
{
	const float s  = mpui_scale();
	const float r  = 9.0f * s;
	const float in = 4.0f * s;
	dl->AddRectFilled(ImVec2(a.x - 2 * s, a.y - 2 * s), ImVec2(b.x + 2 * s, b.y + 2 * s), withAlpha(kNavyEdge, 0.85f),
	                  r + 2 * s);
	dl->AddRectFilled(a, b, fill, r);
	// a soft highlight on the upper half
	dl->AddRectFilledMultiColor(ImVec2(a.x + r * 0.5f, a.y + 2 * s), ImVec2(b.x - r * 0.5f, (a.y + b.y) * 0.5f),
	                            IM_COL32(255, 255, 255, 34), IM_COL32(255, 255, 255, 34), IM_COL32(255, 255, 255, 0),
	                            IM_COL32(255, 255, 255, 0));
	dl->AddRect(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), IM_COL32(255, 255, 255, 210), r - in * 0.6f, 0,
	            1.4f * s);
	if (rivets && b.x - a.x > 40 * s && b.y - a.y > 24 * s) {
		const float d = in + 5.0f * s;
		const ImVec2 pts[4] = { ImVec2(a.x + d, a.y + d), ImVec2(b.x - d, a.y + d), ImVec2(a.x + d, b.y - d),
			                    ImVec2(b.x - d, b.y - d) };
		for (const ImVec2& p : pts)
			dl->AddCircleFilled(p, 2.2f * s, IM_COL32(255, 255, 255, 230));
	}
}

void mpui_icon(ImDrawList* dl, int icon, ImVec2 c, float r, ImU32 col)
{
	const float t = (float)nowSec();
	// the game's own textures (mpui_load_game_icons); the drawn versions below are the fallback
	switch (icon) {
	case MPUI_ICON_SHINE:
		if (drawGameIcon(dl, 0, ImVec2(c.x, c.y + std::sin(t * 3.0f) * r * 0.06f), r * 1.1f, kWhite))
			return;
		break;
	case MPUI_ICON_FLAG: // host a lobby: a Shine to play for
		if (drawGameIcon(dl, 0, c, r * 1.05f, kWhite))
			return;
		break;
	case MPUI_ICON_SIGNAL: // join: friends (a Pianta)
		if (drawGameIcon(dl, 2, c, r * 1.1f, kWhite))
			return;
		break;
	case MPUI_ICON_CAP:
		if (drawGameIcon(dl, 1, c, r * 1.05f, kWhite))
			return;
		break;
	case MPUI_ICON_BACK:
	case MPUI_ICON_EXIT:
		if (drawGameIcon(dl, 3, c, r * 0.8f, col, 0.0f, true))
			return;
		break;
	case MPUI_ICON_PLAY:
		if (drawGameIcon(dl, 3, c, r * 0.8f, col))
			return;
		break;
	case MPUI_ICON_CROWN:
		if (drawGameIcon(dl, 5, c, r * 1.1f, kWhite))
			return;
		break;
	case MPUI_ICON_LOCK:
		if (drawGameIcon(dl, 6, c, r * 0.95f, IM_COL32(255, 222, 60, 255)))
			return;
		break;
	case MPUI_ICON_REFRESH: // the file select's loading sun, turning
		if (drawGameIcon(dl, 4, c, r * 1.05f, col, t * 2.0f))
			return;
		break;
	case MPUI_ICON_SUN:
		if (drawGameIcon(dl, 4, c, r, col, t * 2.5f))
			return;
		break;
	case MPUI_ICON_COIN:
		if (drawGameIcon(dl, 7, c, r, kWhite))
			return;
		break;
	default: break;
	}
	switch (icon) {
	case MPUI_ICON_SHINE: {
		// a Shine Sprite: a fat five-pointed star with eyes, gently bobbing
		const float bob = std::sin(t * 3.0f) * r * 0.06f;
		c.y += bob;
		ImVec2 p[10];
		for (int i = 0; i < 10; i++) {
			const float ang = -1.5708f + i * 0.6283f;
			const float rr  = (i & 1) ? r * 0.55f : r;
			p[i]            = ImVec2(c.x + std::cos(ang) * rr, c.y + std::sin(ang) * rr);
		}
		for (int i = 0; i < 10; i++)
			dl->AddTriangleFilled(c, p[i], p[(i + 1) % 10], (i & 1) ? IM_COL32(255, 170, 20, 255) : IM_COL32(255, 214, 40, 255));
		for (int i = 0; i < 10; i++)
			dl->AddLine(p[i], p[(i + 1) % 10], IM_COL32(150, 70, 0, 255), r * 0.08f);
		dl->AddCircleFilled(ImVec2(c.x - r * 0.18f, c.y - r * 0.02f), r * 0.09f, IM_COL32(40, 20, 0, 255));
		dl->AddCircleFilled(ImVec2(c.x + r * 0.18f, c.y - r * 0.02f), r * 0.09f, IM_COL32(40, 20, 0, 255));
		dl->AddCircleFilled(ImVec2(c.x + r * 0.42f, c.y - r * 0.55f), r * 0.10f, IM_COL32(255, 255, 255, 220));
		break;
	}
	case MPUI_ICON_FLAG:
		dl->AddLine(ImVec2(c.x - r * 0.45f, c.y + r * 0.85f), ImVec2(c.x - r * 0.45f, c.y - r * 0.85f), col, r * 0.14f);
		dl->AddTriangleFilled(ImVec2(c.x - r * 0.38f, c.y - r * 0.85f), ImVec2(c.x + r * 0.75f, c.y - r * 0.45f),
		                      ImVec2(c.x - r * 0.38f, c.y - r * 0.05f), IM_COL32(255, 80, 60, 255));
		dl->AddCircleFilled(ImVec2(c.x - r * 0.45f, c.y + r * 0.85f), r * 0.2f, col);
		break;
	case MPUI_ICON_SIGNAL: {
		const ImVec2 o(c.x, c.y + r * 0.6f);
		dl->AddCircleFilled(o, r * 0.16f, col);
		for (int i = 1; i <= 2; i++) {
			dl->PathArcTo(o, r * (0.3f + 0.4f * i), -2.36f, -0.785f, 16);
			dl->PathStroke(col, 0, r * 0.18f);
		}
		break;
	}
	case MPUI_ICON_CAP:
		dl->PathArcTo(ImVec2(c.x, c.y + r * 0.25f), r * 0.75f, 3.1416f, 6.2832f, 20);
		dl->PathFillConvex(IM_COL32(230, 30, 30, 255));
		dl->AddRectFilled(ImVec2(c.x - r * 0.2f, c.y + r * 0.15f), ImVec2(c.x + r * 0.95f, c.y + r * 0.42f),
		                  IM_COL32(200, 20, 20, 255), r * 0.12f);
		dl->AddCircleFilled(ImVec2(c.x - r * 0.05f, c.y - r * 0.12f), r * 0.28f, kWhite);
		dl->AddText(ImGui::GetFont(), r * 0.42f, ImVec2(c.x - r * 0.16f, c.y - r * 0.33f), IM_COL32(230, 30, 30, 255), "M");
		break;
	case MPUI_ICON_BACK:
		dl->AddLine(ImVec2(c.x + r * 0.7f, c.y), ImVec2(c.x - r * 0.3f, c.y), col, r * 0.22f);
		dl->AddTriangleFilled(ImVec2(c.x - r * 0.75f, c.y), ImVec2(c.x - r * 0.15f, c.y - r * 0.5f),
		                      ImVec2(c.x - r * 0.15f, c.y + r * 0.5f), col);
		break;
	case MPUI_ICON_CROWN: {
		const ImVec2 p[7] = { ImVec2(c.x - r, c.y + r * 0.5f), ImVec2(c.x - r, c.y - r * 0.45f), ImVec2(c.x - r * 0.5f, c.y + r * 0.05f),
			                  ImVec2(c.x, c.y - r * 0.65f), ImVec2(c.x + r * 0.5f, c.y + r * 0.05f), ImVec2(c.x + r, c.y - r * 0.45f),
			                  ImVec2(c.x + r, c.y + r * 0.5f) };
		dl->AddTriangleFilled(p[0], p[1], p[2], kYellow);
		dl->AddTriangleFilled(p[2], p[3], p[4], kYellow);
		dl->AddTriangleFilled(p[4], p[5], p[6], kYellow);
		dl->AddRectFilled(ImVec2(c.x - r, c.y + r * 0.05f), ImVec2(c.x + r, c.y + r * 0.5f), kYellow);
		dl->AddCircleFilled(ImVec2(c.x, c.y + r * 0.28f), r * 0.14f, IM_COL32(230, 40, 40, 255));
		break;
	}
	case MPUI_ICON_LOCK:
		dl->PathArcTo(ImVec2(c.x, c.y - r * 0.1f), r * 0.42f, 3.1416f, 6.2832f, 12);
		dl->PathStroke(col, 0, r * 0.18f);
		dl->AddRectFilled(ImVec2(c.x - r * 0.62f, c.y - r * 0.1f), ImVec2(c.x + r * 0.62f, c.y + r * 0.75f), col, r * 0.15f);
		break;
	case MPUI_ICON_PLAY:
		dl->AddTriangleFilled(ImVec2(c.x - r * 0.45f, c.y - r * 0.65f), ImVec2(c.x + r * 0.7f, c.y),
		                      ImVec2(c.x - r * 0.45f, c.y + r * 0.65f), col);
		break;
	case MPUI_ICON_EXIT:
		dl->AddRect(ImVec2(c.x - r * 0.75f, c.y - r * 0.75f), ImVec2(c.x + r * 0.15f, c.y + r * 0.75f), col, r * 0.1f, 0,
		            r * 0.16f);
		dl->AddLine(ImVec2(c.x - r * 0.25f, c.y), ImVec2(c.x + r * 0.55f, c.y), col, r * 0.2f);
		dl->AddTriangleFilled(ImVec2(c.x + r * 0.9f, c.y), ImVec2(c.x + r * 0.45f, c.y - r * 0.4f),
		                      ImVec2(c.x + r * 0.45f, c.y + r * 0.4f), col);
		break;
	case MPUI_ICON_REFRESH:
		dl->PathArcTo(c, r * 0.6f, 0.5f, 5.5f, 20);
		dl->PathStroke(col, 0, r * 0.18f);
		dl->AddTriangleFilled(ImVec2(c.x + r * 0.95f, c.y - r * 0.15f), ImVec2(c.x + r * 0.35f, c.y - r * 0.15f),
		                      ImVec2(c.x + r * 0.65f, c.y + r * 0.35f), col);
		break;
	default: break;
	}
}

void mpui_title(const char* text)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const float s  = mpui_scale();
	const float px = 24.0f * s;
	const ImVec2 ts = mpui_text_size(text, px);
	const float w   = ts.x + 70.0f * s;
	const float h   = px + 18.0f * s;
	const ImVec2 cur = ImGui::GetCursorScreenPos();
	const float cx   = cur.x + ImGui::GetContentRegionAvail().x * 0.5f;
	const ImVec2 a(cx - w * 0.5f, cur.y), b(cx + w * 0.5f, cur.y + h);
	mpui_panel(dl, a, b, kBlue);
	mpui_icon(dl, MPUI_ICON_SHINE, ImVec2(a.x + 22.0f * s, (a.y + b.y) * 0.5f), 11.0f * s, 0);
	mpui_text(dl, ImVec2(cx - ts.x * 0.5f + 10.0f * s, a.y + (h - px) * 0.5f), px, kWhite, text);
	ImGui::Dummy(ImVec2(ImGui::GetContentRegionAvail().x, h + 6.0f * s));
}

void mpui_heading(const char* text, float width)
{
	ImDrawList* dl   = ImGui::GetWindowDrawList();
	const float s    = mpui_scale();
	const float px   = 15.0f * s;
	ImGui::Dummy(ImVec2(1, 3.0f * s));
	const ImVec2 cur = ImGui::GetCursorScreenPos();
	const ImVec2 ts  = mpui_text_size(text, px);
	mpui_text(dl, cur, px, kYellow, text);
	const float ly = cur.y + px * 0.6f;
	const float w = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
	if (w > ts.x + 16.0f * s)
		dl->AddLine(ImVec2(cur.x + ts.x + 8.0f * s, ly), ImVec2(cur.x + w, ly),
	            IM_COL32(255, 255, 255, 70), 1.0f * s);
	ImGui::Dummy(ImVec2(ts.x, px + 3.0f * s));
}

bool mpui_tile(const char* id, const char* label, const char* sub, int icon, ImVec2 size, ImU32 tint)
{
	const float s = mpui_scale();
	const bool pressed = ImGui::InvisibleButton(id, size);
	const bool hot     = ImGui::IsItemHovered() || ImGui::IsItemFocused();
	const bool held    = ImGui::IsItemActive();
	ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	if (held) {
		a.y += 2 * s;
		b.y += 2 * s;
	}
	ImDrawList* dl = ImGui::GetWindowDrawList();
	mpui_panel(dl, a, b, tint ? tint : (hot ? kBlueLight : kBlue));
	if (hot)
		dl->AddRect(ImVec2(a.x - 3 * s, a.y - 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s), kYellow, 12.0f * s, 0, 3.0f * s);
	const float ir = (b.y - a.y) * 0.28f;
	const ImVec2 ic(a.x + 14.0f * s + ir, (a.y + b.y) * 0.5f);
	dl->AddCircleFilled(ic, ir * 1.25f, IM_COL32(10, 24, 110, 160));
	mpui_icon(dl, icon, ic, ir, kWhite);
	const float px = 19.0f * s;
	const float tx = ic.x + ir * 1.25f + 12.0f * s;
	const float ty = sub ? a.y + (b.y - a.y) * 0.5f - px * 0.95f : a.y + (b.y - a.y - px) * 0.5f;
	mpui_text(dl, ImVec2(tx, ty), px, hot ? kYellow : kWhite, label);
	if (sub)
		dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.9f, ImVec2(tx, ty + px * 1.15f), IM_COL32(210, 225, 255, 255), sub);
	return pressed;
}

bool mpui_button(const char* id, int icon, float width, int accent)
{
	const float s  = mpui_scale();
	const float px = 16.0f * s;
	// "Label##id": only the part before ## is shown (as ImGui does)
	char label[64];
	std::snprintf(label, sizeof(label), "%s", id);
	if (char* hh = std::strstr(label, "##"))
		*hh = '\0';
	const ImVec2 size(width > 0 ? width : mpui_text_size(label, px).x + (icon ? 56.0f : 32.0f) * s, px + 16.0f * s);
	const bool pressed = ImGui::InvisibleButton(id, size);
	const bool hot     = ImGui::IsItemHovered() || ImGui::IsItemFocused();
	const bool held    = ImGui::IsItemActive();
	ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	if (held) {
		a.y += 2 * s;
		b.y += 2 * s;
	}
	static const ImU32 kFill[4][2] = { { IM_COL32(36, 68, 238, 255), IM_COL32(70, 104, 255, 255) },
		                               { IM_COL32(30, 150, 60, 255), IM_COL32(50, 190, 80, 255) },
		                               { IM_COL32(190, 40, 40, 255), IM_COL32(230, 60, 60, 255) },
		                               { IM_COL32(220, 150, 20, 255), IM_COL32(250, 185, 40, 255) } };
	ImDrawList* dl = ImGui::GetWindowDrawList();
	mpui_panel(dl, a, b, kFill[accent & 3][hot ? 1 : 0], false);
	if (hot)
		dl->AddRect(ImVec2(a.x - 3 * s, a.y - 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s), kYellow, 12.0f * s, 0, 3.0f * s);
	const ImVec2 ts = mpui_text_size(label, px);
	float x         = (a.x + b.x - ts.x) * 0.5f;
	if (icon) {
		x += 11.0f * s;
		mpui_icon(dl, icon, ImVec2(x - 14.0f * s, (a.y + b.y) * 0.5f), 8.0f * s, kWhite);
	}
	mpui_text(dl, ImVec2(x, a.y + (b.y - a.y - px) * 0.5f), px, hot ? kYellow : kWhite, label);
	return pressed;
}

void mpui_chip(const char* text, ImU32 dot)
{
	const float s   = mpui_scale();
	const float fs  = ImGui::GetFontSize();
	const ImVec2 ts = ImGui::CalcTextSize(text);
	const ImVec2 a  = ImGui::GetCursorScreenPos();
	const ImVec2 b(a.x + ts.x + fs * 1.6f, a.y + fs * 1.5f);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(a, b, IM_COL32(8, 18, 80, 200), fs * 0.75f);
	const float pulse = 0.75f + 0.25f * std::sin((float)nowSec() * 4.0f);
	dl->AddCircleFilled(ImVec2(a.x + fs * 0.7f, (a.y + b.y) * 0.5f), fs * 0.28f, withAlpha(dot, pulse));
	dl->AddText(ImVec2(a.x + fs * 1.15f, a.y + fs * 0.25f), IM_COL32(230, 238, 255, 255), text);
	ImGui::Dummy(ImVec2(b.x - a.x, b.y - a.y + 2 * s));
}

bool mpui_toggle(const char* id, int* value, const char* a, const char* b)
{
	const float s  = mpui_scale();
	const float fs = ImGui::GetFontSize();
	ImGui::PushID(id);
	bool changed = false;
	for (int i = 0; i < 2; i++) {
		const char* lab = i == 0 ? a : b;
		const ImVec2 sz(ImGui::CalcTextSize(lab).x + fs * 1.6f, fs * 1.7f);
		if (i)
			ImGui::SameLine(0, 0);
		ImGui::PushID(i);
		if (ImGui::InvisibleButton("seg", sz) && *value != i) {
			*value  = i;
			changed = true;
		}
		const bool hot = ImGui::IsItemHovered() || ImGui::IsItemFocused();
		const ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
		ImDrawList* dl  = ImGui::GetWindowDrawList();
		const bool on   = *value == i;
		dl->AddRectFilled(p0, p1, on ? kYellow : (hot ? IM_COL32(40, 70, 210, 255) : IM_COL32(12, 24, 100, 230)), fs * 0.8f,
		                  i == 0 ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersRight);
		if (hot)
			dl->AddRect(p0, p1, kYellow, fs * 0.8f, i == 0 ? ImDrawFlags_RoundCornersLeft : ImDrawFlags_RoundCornersRight, 2.0f * s);
		dl->AddText(ImVec2(p0.x + fs * 0.8f, p0.y + fs * 0.35f), on ? IM_COL32(30, 30, 90, 255) : IM_COL32(230, 238, 255, 255), lab);
		ImGui::PopID();
	}
	ImGui::PopID();
	return changed;
}

void mpui_ping_bars(unsigned pingMs, bool lagging)
{
	const float fs = ImGui::GetFontSize();
	const ImVec2 a = ImGui::GetCursorScreenPos();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const int lit  = lagging ? 0 : pingMs < 60 ? 4 : pingMs < 120 ? 3 : pingMs < 200 ? 2 : 1;
	const ImU32 on = lit >= 3 ? IM_COL32(90, 230, 100, 255) : lit == 2 ? IM_COL32(255, 210, 60, 255) : IM_COL32(255, 90, 60, 255);
	for (int i = 0; i < 4; i++) {
		const float h = fs * (0.3f + 0.22f * i);
		const ImVec2 p0(a.x + i * fs * 0.32f, a.y + fs * 1.05f - h), p1(p0.x + fs * 0.22f, a.y + fs * 1.05f);
		const bool flash = lagging && std::fmod(nowSec(), 0.8) < 0.4;
		dl->AddRectFilled(p0, p1, i < lit ? on : (flash ? IM_COL32(255, 120, 60, 200) : IM_COL32(255, 255, 255, 50)), 1.0f);
	}
	ImGui::Dummy(ImVec2(fs * 1.35f, fs * 1.1f));
}

void mpui_push_theme(void)
{
	const float s = mpui_scale();
	ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_PopupBg, kNavy);
	ImGui::PushStyleColor(ImGuiCol_Border, IM_COL32(255, 255, 255, 60));
	ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 245, 255, 255));
	ImGui::PushStyleColor(ImGuiCol_TextDisabled, IM_COL32(160, 180, 235, 255));
	ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(8, 18, 90, 220));
	ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(30, 56, 190, 230));
	ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(40, 70, 220, 240));
	ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(50, 86, 245, 255));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(85, 120, 255, 255));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(230, 180, 30, 255));
	ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(40, 70, 220, 200));
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(70, 104, 255, 230));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(230, 180, 30, 255));
	ImGui::PushStyleColor(ImGuiCol_CheckMark, kYellow);
	ImGui::PushStyleColor(ImGuiCol_SliderGrab, kYellow);
	ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, IM_COL32(255, 240, 150, 255));
	ImGui::PushStyleColor(ImGuiCol_Separator, IM_COL32(255, 255, 255, 60));
	ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, IM_COL32(10, 22, 100, 230));
	ImGui::PushStyleColor(ImGuiCol_TableRowBg, IM_COL32(255, 255, 255, 0));
	ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, IM_COL32(255, 255, 255, 16));
	ImGui::PushStyleColor(ImGuiCol_TableBorderLight, IM_COL32(255, 255, 255, 30));
	ImGui::PushStyleColor(ImGuiCol_NavHighlight, kYellow);
	ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 40));
	ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, IM_COL32(120, 150, 255, 200));
	ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, IM_COL32(0, 8, 40, 120));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * s);
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 7.0f * s);
	ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 7.0f * s);
	ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10.0f * s);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * s, 14.0f * s));
	ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * s, 4.0f * s));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * s, 6.0f * s));
}

void mpui_pop_theme(void)
{
	ImGui::PopStyleVar(8);
	ImGui::PopStyleColor(26);
}

void mpui_window_background(void)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 a = ImGui::GetWindowPos();
	const ImVec2 b(a.x + ImGui::GetWindowWidth(), a.y + ImGui::GetWindowHeight());
	const float s = mpui_scale();
	dl->PushClipRectFullScreen();
	mpui_panel(dl, a, b, kNavy);
	// water along the bottom (Delfino's sea), gently moving
	const float t  = (float)nowSec();
	const float wy = b.y - 16.0f * s;
	const float in = 6.0f * s;
	for (int layer = 0; layer < 2; layer++) {
		const ImU32 c = layer == 0 ? IM_COL32(40, 170, 255, 70) : IM_COL32(120, 230, 255, 60);
		const float amp = (layer ? 2.5f : 3.5f) * s, ph = layer ? 1.7f : 0.0f;
		const int n = 48;
		for (int i = 0; i < n; i++) {
			const float x0 = a.x + in + (b.x - a.x - 2 * in) * i / n;
			const float x1 = a.x + in + (b.x - a.x - 2 * in) * (i + 1) / n;
			const float y0 = wy + std::sin(x0 * 0.04f / s + t * 1.6f + ph) * amp;
			const float y1 = wy + std::sin(x1 * 0.04f / s + t * 1.6f + ph) * amp;
			dl->AddQuadFilled(ImVec2(x0, y0), ImVec2(x1, y1), ImVec2(x1, b.y - in), ImVec2(x0, b.y - in), c);
		}
	}
	dl->PopClipRect();
}

void mpui_toast(ImDrawList* dl, ImVec2 pos, const char* text, float alpha)
{
	const float s   = mpui_scale();
	const float px  = 14.0f * s;
	const ImVec2 ts = mpui_text_size(text, px);
	const ImVec2 a(pos.x, pos.y - ts.y - 16.0f * s), b(pos.x + ts.x + 26.0f * s, pos.y);
	mpui_panel(dl, a, b, withAlpha(kBlue, alpha * 0.92f), false);
	mpui_text(dl, ImVec2(a.x + 13.0f * s, a.y + 8.0f * s), px, withAlpha(kWhite, alpha), text);
}

#endif // SMS_PORT_AURORA
