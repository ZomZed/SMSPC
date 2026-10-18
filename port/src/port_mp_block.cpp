// Multiplayer entry on the save-block screen (the file select, stage 15): a fourth block left
// of block A, with an "online play" symbol on its face. Walking left past block A pans the
// camera over to it (the way walking right pans to the options); hitting it from below opens
// the multiplayer menu (port_mp_menu_open). Only while multiplayer is enabled
// (port_multiplayer.ini enabled=1).
//
// The block is a real TFileLoadBlock spawned with TMapObjBase::initAndRegister("FileLoadBlockA")
// (model, collision, bump animation and particles of the save blocks), with unk138 = 3 so
// TCardLoad::setSelected (CardLoad.cpp) hands the hit to port_mp_block_hit instead of picking a
// save file. Its model data is loaded privately so the face texture can be replaced without
// touching block A.
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <GC2D/CardLoad.hpp>
#include <M3DUtil/MActor.hpp>
#include <MoveBG/MapObjOption.hpp>
#include <Player/Mario.hpp>
#include <System/Application.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoaderFlags.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/ResTIMG.hpp>
#include <JSystem/J2D/J2DPane.hpp>
#include <JSystem/J2D/J2DTextBox.hpp>
#include <JSystem/J2D/J2DScreen.hpp>
#include <JSystem/JKernel/JKRArchive.hpp>
#include <dolphin/os.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void port_mp_menu_open(int page);                // port_mp_menu.cpp
void portMpPaletteOwnHeaders(J3DModelData* d);              // port_mp_palette.cpp
void mpui_load_game_icons(void);                            // port_mp_ui.cpp

namespace {

const float kBlockOffsetX  = -380.0f; // the online block, from block A
const float kCustomOffsetX = -650.0f; // the customization block (270 further left: room for both labels)
const float kPanStartX     = 700.0f;  // Mario left of this: the camera pans over to the blocks
const float kPanBackX      = 760.0f;  // ... and back right of this
const float kPanX          = -825.0f; // camera (position and target) moved by this: the two blocks centred,
                                      // only the edge of block A showing
const float kRoomPastBlock = 60.0f;  // Mario's left limit (the invisible wall): this far past the block

TFileLoadBlock* sBlock;  // online play (unk138 3)
TFileLoadBlock* sCustom; // customization (unk138 4)
float sBlockX;           // Mario's left limit comes from the leftmost block
bool sPanned;
float sBasePos[3], sBaseAt[3];
// A hit opens its menu after the block's bump has played (a real-time delay).
const double kHitDelayMs = 250.0; // the bump shows first
int sHitPending; // 3 / 4 (unk138 of the block), 0 = none
double sHitAt;
// The customization menu's 3D preview: the camera closes in on Mario (left of the screen, the
// menu on the right) and Mario turns to the angle the menu asks for. The menu (render thread)
// sets the wish; port_mp_option_camera (game thread) applies it.
std::atomic<int> sPreviewWant{ 0 };
std::atomic<float> sPreviewTurn{ 0.0f }; // degrees, added to facing the camera
bool sPreview;
float sPrevPos[3], sPrevAt[3];
s16 sFaceCamera;

bool enabled() { return port_mp_enabled() != 0; }

// GX RGBA8 (4x4 tiles: 32 bytes of A,R pairs, then 32 of G,B) from 0xAARRGGBB pixels.
void encodeRGBA8(const u32* px, int w, int h, u8* out)
{
	u8* o = out;
	for (int ty = 0; ty < h; ty += 4)
		for (int tx = 0; tx < w; tx += 4) {
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++) {
					const u32 c = px[(ty + y) * w + tx + x];
					*o++        = (u8)(c >> 24);
					*o++        = (u8)(c >> 16);
				}
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++) {
					const u32 c = px[(ty + y) * w + tx + x];
					*o++        = (u8)(c >> 8);
					*o++        = (u8)c;
				}
		}
}

// SMS_PORT_MP_BLOCK_DUMP=1: lists the block model's textures (to pick the face).
void dumpTextures(J3DModelData* d)
{
	for (u16 i = 0; i < d->getTexture()->getNum(); ++i) {
		ResTIMG* t = d->getTexture()->getResTIMG(i);
		std::fprintf(stderr, "[mp-block] tex %d %s fmt %d %dx%d mips %d\n", i, d->getTextureName()->getName(i),
		             t->format, t->width, t->height, t->mipmapCount);
		char path[48];
		std::snprintf(path, sizeof(path), "mpblock_tex%d.bin", i); // raw image, working directory
		if (FILE* f = std::fopen(path, "wb")) {
			std::fwrite((u8*)t + t->imageDataOffset, (u32)t->width * t->height / 2, 1, f); // CMPR: 4 bpp
			std::fclose(f);
		}
	}
}

// GX CMPR (8x8 tiles of four 4x4 DXT1 blocks, big-endian RGB565) to 0xAARRGGBB.
void decodeCMPR(const u8* src, int w, int h, u32* out)
{
	for (int ty = 0; ty < h; ty += 8)
		for (int tx = 0; tx < w; tx += 8)
			for (int sb = 0; sb < 4; sb++, src += 8) {
				const int sx = tx + (sb & 1) * 4, sy = ty + (sb >> 1) * 4;
				const u16 c0 = (u16)(src[0] << 8 | src[1]), c1 = (u16)(src[2] << 8 | src[3]);
				u32 pal[4];
				int rgb[2][3];
				for (int k = 0; k < 2; k++) {
					const u16 c = k ? c1 : c0;
					rgb[k][0]   = (c >> 11 & 31) * 255 / 31;
					rgb[k][1]   = (c >> 5 & 63) * 255 / 63;
					rgb[k][2]   = (c & 31) * 255 / 31;
				}
				auto mix = [&](int a, int b, int d) {
					return 0xFF000000u | (u32)((rgb[0][0] * a + rgb[1][0] * b) / d) << 16
					       | (u32)((rgb[0][1] * a + rgb[1][1] * b) / d) << 8 | (u32)((rgb[0][2] * a + rgb[1][2] * b) / d);
				};
				pal[0] = mix(1, 0, 1);
				pal[1] = mix(0, 1, 1);
				if (c0 > c1) {
					pal[2] = mix(2, 1, 3);
					pal[3] = mix(1, 2, 3);
				} else {
					pal[2] = mix(1, 1, 2);
					pal[3] = 0;
				}
				for (int y = 0; y < 4; y++)
					for (int x = 0; x < 4; x++)
						out[(sy + y) * w + sx + x] = pal[(src[4 + y] >> (6 - 2 * x)) & 3];
			}
}

// The online symbol: signal arcs over a dot, in a purple gradient with the letters' dark
// outline (A red, B green, C blue). 0 = outside, 1 = outline, 2 = fill.
int signalAt(float x, float y)
{
	const float cx = 32.0f, cy = 47.0f;
	const float dx = x - cx, dy = cy - y; // dy up
	const float r  = std::sqrt(dx * dx + dy * dy);
	const float ang = std::atan2(std::fabs(dx), dy) * 57.29578f; // 0 = straight up
	const float kOutline = 1.8f;
	int best = 0;
	// the dot
	if (r < 5.5f)
		return 2;
	if (r < 5.5f + kOutline)
		best = 1;
	// three arcs, 6 wide, +-44 degrees (round ends)
	static const float kRadius[3] = { 13.0f, 22.0f, 31.0f };
	for (int i = 0; i < 3; i++) {
		const float half = 3.0f;
		float d;
		if (dy > 0.0f && ang <= 44.0f) {
			d = std::fabs(r - kRadius[i]);
		} else { // distance to the arc's end caps
			const float ea = 44.0f / 57.29578f;
			const float ex = cx + kRadius[i] * std::sin(ea), ey = cy - kRadius[i] * std::cos(ea);
			const float ax = std::fabs(dx) + cx; // mirror to the right side
			d = std::sqrt((ax - ex) * (ax - ex) + (y - ey) * (y - ey));
		}
		if (d < half)
			return 2;
		if (d < half + kOutline)
			best = 1;
	}
	return best;
}

// The customization symbol: a painter's palette with four paint blobs. 0 = outside,
// 1 = outline, 2 = the wood, 3..6 = paints (red, green, blue, yellow).
int paletteAt(float x, float y)
{
	const float kOutline = 1.8f;
	// the body: an ellipse, with a thumb hole and a bite out of the lower right
	const float ex = (x - 32.0f) / 25.0f, ey = (y - 33.0f) / 19.0f;
	const float body = std::sqrt(ex * ex + ey * ey); // 1 = the rim
	const float hx = x - 41.0f, hy = y - 41.0f;
	const float hole = std::sqrt(hx * hx + hy * hy) - 4.5f; // < 0 inside the thumb hole
	const float bx = x - 51.0f, by = y - 46.0f;
	const float bite = std::sqrt(bx * bx + by * by) - 8.0f;
	const float rim = (body - 1.0f) * 21.0f; // roughly pixels outside the rim
	const float inside = std::fmax(rim, std::fmax(-hole, -bite));
	if (inside > kOutline)
		return 0;
	if (inside > 0.0f)
		return 1;
	static const float kBlob[4][2] = { { 20.0f, 27.0f }, { 30.0f, 22.0f }, { 41.0f, 24.0f }, { 22.0f, 39.0f } };
	for (int i = 0; i < 4; i++) {
		const float dx = x - kBlob[i][0], dy = y - kBlob[i][1];
		const float d  = std::sqrt(dx * dx + dy * dy);
		if (d < 4.6f)
			return 3 + i;
		if (d < 4.6f + 1.2f)
			return 1;
	}
	return 2;
}

int symbolAt(int kind, float x, float y) { return kind == 1 ? paletteAt(x, y) : signalAt(x, y); }

u32 blend(u32 under, u32 over, float a)
{
	u32 out = 0xFF000000u;
	for (int s = 0; s < 24; s += 8) {
		const float u = (float)(under >> s & 0xFF), o = (float)(over >> s & 0xFF);
		out |= (u32)(u + (o - u) * a + 0.5f) << s;
	}
	return out;
}

void drawSymbol(u32* px, int w, int h, int kind)
{
	static const u32 kPaint[4] = { 0xFFE83A2Eu, 0xFF3CB44Au, 0xFF2F6BE6u, 0xFFFFD23Cu };
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int count[7] = { 0, 0, 0, 0, 0, 0, 0 };
			for (int sy = 0; sy < 4; sy++) // 4x4 supersampling
				for (int sx = 0; sx < 4; sx++)
					count[symbolAt(kind, x + (sx + 0.5f) / 4.0f, y + (sy + 0.5f) / 4.0f)]++;
			const int cover = 16 - count[0];
			if (cover == 0)
				continue;
			u32 c = blend(px[y * w + x], 0xFF292829u, cover / 16.0f); // outline colour of the letters
			const float g  = (y - 12.0f) / 42.0f;
			const float gc = g < 0.0f ? 0.0f : (g > 1.0f ? 1.0f : g);
			if (kind == 0) {
				// purple, dark at the top to light at the bottom, like the letters' gradient
				const u32 fc = 0xFF000000u | (u32)(110 + 110 * gc) << 16 | (u32)(30 + 120 * gc) << 8 | (u32)(160 + 80 * gc);
				if (count[2])
					c = blend(c, fc, count[2] / 16.0f);
			} else {
				// warm wood, lighter at the top, and the paints
				const u32 wood = 0xFF000000u | (u32)(240 - 40 * gc) << 16 | (u32)(200 - 50 * gc) << 8 | (u32)(130 - 40 * gc);
				if (count[2])
					c = blend(c, wood, count[2] / 16.0f);
				for (int k = 0; k < 4; k++)
					if (count[3 + k])
						c = blend(c, kPaint[k], count[3 + k] / 16.0f);
			}
			px[y * w + x] = c;
		}
}

// The block's own model data (so block A keeps its letter), its face replaced with the symbol.
void giveOwnFace(TFileLoadBlock* b, int kind)
{
	MActor* a = b->getMActor();
	if (a == nullptr)
		return;
	void* res = JKRFileLoader::getGlbResource("/scene/mapObj/FileLoadBlockA.bmd");
	if (res == nullptr) {
		std::fprintf(stderr, "[mp-block] FileLoadBlockA.bmd not found: the block keeps the A\n");
		return;
	}
	J3DModelData* d = J3DModelLoaderDataBase::load(
	    res, J3DMLF_MaterialPEFull | J3DMLF_UseUniqueMaterials | (2 << J3DMLF_TevStageNumShift));
	if (d == nullptr)
		return;
	portMpPaletteOwnHeaders(d);
	J3DTexture* tex = d->getTexture();
	int faceIdx = -1, blankIdx = -1;
	for (u16 i = 0; i < tex->getNum(); ++i) {
		const char* n = d->getTextureName()->getName(i);
		if (std::strcmp(n, "F_block_01_3") == 0 && blankIdx < 0)
			blankIdx = i;
	}
	ResTIMG* blank = blankIdx >= 0 ? tex->getResTIMG(blankIdx) : nullptr;
	if (blank == nullptr || blank->format != GX_TF_CMPR || blank->width != 64 || blank->height != 64)
		return;
	static u32 sPx[64 * 64];
	decodeCMPR((const u8*)blank + blank->imageDataOffset, 64, 64, sPx);
	drawSymbol(sPx, 64, 64, kind);
	u8* img = (u8*)JKRHeap::alloc(64 * 64 * 4, 0x20, nullptr);
	if (img == nullptr)
		return;
	encodeRGBA8(sPx, 64, 64, img);
	DCFlushRange(img, 64 * 64 * 4);
	for (u16 i = 0; i < tex->getNum(); ++i) {
		if (std::strcmp(d->getTextureName()->getName(i), "F_block_A") != 0)
			continue;
		ResTIMG* t         = tex->getResTIMG(i);
		t->format          = GX_TF_RGBA8;
		t->mipmapEnabled   = 0;
		t->mipmapCount     = 1;
		t->imageDataOffset = (s32)((uintptr_t)img - (uintptr_t)t);
		faceIdx            = i;
	}
	if (faceIdx < 0)
		return;
	void* mem = JKRHeap::alloc(sizeof(J3DModel), 0x10, nullptr);
	if (mem == nullptr)
		return;
	a->setModel(new (mem) J3DModel(d, 0, 1), 0);
	std::fprintf(stderr, "[mp-block] own face: %s symbol\n", kind == 1 ? "palette" : "online");
}

// The multiplayer section's blue boxes: a private copy of the file select's load.blo (so the
// normal one is untouched) with the top window reading "Multiplayer" and one file window moved
// above the block reading "Online Play". CardLoad.cpp cross-fades it with the normal overlay.
J2DSetScreen* sScreen;
J2DPane* sScreenRoot;
J2DPane* sScreenText[6];
int sScreenAlpha;

J2DPane* windowWith(J2DPane* root, u32 tag)
{
	for (JSUTree<J2DPane>* c = root->getFirstChild(); c != root->getEndChild(); c = c->getNextChild())
		if (c->getObject()->search(tag) != nullptr)
			return c->getObject();
	return nullptr;
}

void makeScreen()
{
	sScreen = nullptr;
	JKRArchive* vol = (JKRArchive*)JKRFileLoader::getVolume("option");
	if (vol == nullptr)
		return;
	J2DSetScreen* s = new J2DSetScreen("load.blo", vol);
	s->setCullBack(GX_CULL_BACK);
	J2DPane* root = s->search('ROOT');
	if (root == nullptr)
		return;
	for (JSUTree<J2DPane>* c = root->getFirstChild(); c != root->getEndChild(); c = c->getNextChild())
		c->getObject()->hide();
	J2DPane* top    = windowWith(root, 'm_0a');
	J2DPane* online = windowWith(root, 'm_1c'); // file B's window: "Online Play"
	J2DPane* custom = windowWith(root, 'm_1a'); // file A's window: "Customize"
	if (top == nullptr || online == nullptr || custom == nullptr)
		return;
	top->show();
	// the file windows: only their text (not the shine / coin counts or the crown), wider than a
	// file window ("Online Play" is longer than "New"), centred above their blocks
	struct Box {
		J2DPane* win;
		u32 text, shadow;
		int centreX; // in the 600-wide screen (the camera centres the pair)
	} boxes[2] = { { online, 'm_1c', 'm_1d', 392 }, { custom, 'm_1a', 'm_1b', 208 } };
	const int kWiden = 25;
	for (int k = 0; k < 2; k++) {
		J2DPane* w = boxes[k].win;
		w->show();
		for (JSUTree<J2DPane>* c = w->getFirstChild(); c != w->getEndChild(); c = c->getNextChild())
			if (c->getObject()->search(boxes[k].text) == nullptr)
				c->getObject()->hide();
		w->resize(w->getWidth() + kWiden, w->getHeight());
		w->move(boxes[k].centreX - w->getWidth() / 2, w->getBounds().y1);
		for (int i = 0; i < 2; i++) {
			J2DPane* tb = s->search(i == 0 ? boxes[k].shadow : boxes[k].text);
			if (tb != nullptr)
				tb->resize(tb->getWidth() + kWiden, tb->getHeight());
		}
	}
	static const u32 kTags[6] = { 'm_0a', 'm_0b', 'm_1c', 'm_1d', 'm_1a', 'm_1b' };
	static const char* const kText[6] = { "Multiplayer", "Multiplayer", "Online Play", "Online Play", "Customize", "Customize" };
	for (int i = 0; i < 6; i++) {
		J2DPane* p = s->search(kTags[i]);
		if (p == nullptr || p->getKind() != 'TBX1')
			return;
		((J2DTextBox*)p)->setString(kText[i]);
		((J2DTextBox*)p)->setFont((JUTFont*)gpSystemFont); // as TCardLoad::makeBuffer does
		if (i >= 2) { // the block labels, a little smaller so both fit side by side
			J2DTextBox* tb = (J2DTextBox*)p;
			tb->setFontSize(tb->mFontSizeX * 85 / 100, tb->mFontSizeY * 85 / 100);
		}
		sScreenText[i] = p;
	}
	sScreen     = s;
	sScreenRoot = root;
}

TFileLoadBlock* spawnOne(TFileLoadBlock* a, const char* name, float offsetX, u8 id, int face)
{
	TFileLoadBlock* b = new TFileLoadBlock(name);
	b->mPosition      = a->mInitialPosition;
	b->mPosition.x += offsetX;
	b->mRotation = a->mInitialRotation;
	b->mScaling  = a->mInitialScaling;
	b->initAndRegister("FileLoadBlockA");
	b->offMapObjFlag(TMapObjBase::MAP_OBJ_FLAG_DISAPPEARING);
	b->makeObjAppeared();
	b->loadAfter();
	b->unk138 = id; // TCardLoad::setSelected: 3 online play, 4 customization
	giveOwnFace(b, face);
	if (std::getenv("SMS_PORT_MP_BLOCK_DUMP") && b->getModel() != nullptr)
		dumpTextures(b->getModel()->getModelData());
	std::fprintf(stderr, "[mp-block] %s spawned at %.0f %.0f %.0f\n", name, b->mPosition.x, b->mPosition.y,
	             b->mPosition.z);
	return b;
}

void spawn(TFileLoadBlock* a)
{
	mpui_load_game_icons(); // the menus' icons, from the game's own textures
	sBlock  = spawnOne(a, "MultiplayerBlock", kBlockOffsetX, 3, 0);
	sCustom = spawnOne(a, "CustomizeBlock", kCustomOffsetX, 4, 1);
	sBlockX = sCustom->mPosition.x;
	makeScreen();
}

} // namespace

extern "C" {

// CardLoad.cpp, TCardLoad::loadAfter: the save blocks exist now.
void port_mp_block_spawn(void* blockA)
{
	sBlock       = nullptr;
	sCustom      = nullptr;
	sHitPending  = 0;
	sPanned      = false;
	sPreview     = false;
	sPreviewWant = 0;
	sScreen      = nullptr;
	sScreenRoot  = nullptr;
	sScreenAlpha = 0;
	if (!enabled() || blockA == nullptr)
		return;
	spawn((TFileLoadBlock*)blockA);
}

// CardLoad.cpp, TCardLoad::setSelected(3 or 4): Mario hit one of the blocks.
void port_mp_block_hit(int which)
{
	if (sHitPending != 0)
		return;
	std::fprintf(stderr, "[mp-block] hit: %s\n", which == 4 ? "customization menu" : "multiplayer menu");
	sHitPending = which;
	sHitAt      = port_mp_time_ms() + kHitDelayMs; // port_mp_option_camera opens the menu then
}

// MarioMove.cpp (option map): Mario's left limit (846 = under block A), moved out past the
// multiplayer block while it exists.
float port_mp_option_xmin(float xmin)
{
	return sBlock != nullptr ? sBlockX - kRoomPastBlock : xmin;
}

// CameraOption.cpp, CPolarSubCamera::ctrlOptionCamera_: 1 = the camera's goal changed (pans
// to the block when Mario walks past block A, back when he returns).
int port_mp_option_camera(float* pos, float* at)
{
	if (sBlock == nullptr || gpMarioOriginal == nullptr)
		return 0;
	TMario* m = gpMarioOriginal;
	if (sHitPending != 0 && port_mp_time_ms() >= sHitAt) {
		port_mp_menu_open(sHitPending == 4 ? 2 : 0); // 2: the character page on its own (port_mp_menu.cpp)
		sHitPending = 0;
	}
	// the customization menu's 3D preview
	const bool want = sPreviewWant.load() != 0;
	if (want && !sPreview) {
		for (int i = 0; i < 3; i++) {
			sPrevPos[i] = pos[i];
			sPrevAt[i]  = at[i];
		}
		float d[3] = { pos[0] - at[0], pos[1] - at[1], pos[2] - at[2] };
		const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		for (int i = 0; i < 3; i++)
			d[i] = len > 1.0f ? d[i] / len : (i == 2 ? 1.0f : 0.0f);
		// Mario a quarter of the way in from the left (the menu covers the right)
		at[0] = m->mPosition.x + 175.0f; // centred in the third left of the menu
		at[1] = m->mPosition.y + 70.0f;
		at[2] = m->mPosition.z;
		for (int i = 0; i < 3; i++)
			pos[i] = at[i] + d[i] * 560.0f; // his whole body in view
		sFaceCamera  = (s16)(std::atan2(d[0], d[2]) * (32768.0f / 3.14159265f));
		sPreviewTurn = 0.0f;
		sPreview     = true;
		return 1;
	}
	if (!want && sPreview) {
		for (int i = 0; i < 3; i++) {
			pos[i] = sPrevPos[i];
			at[i]  = sPrevAt[i];
		}
		sPreview = false;
		return 1;
	}
	if (sPreview) {
		// SMS_PORT_MP_PREVIEW_SPIN=<deg per frame> (debug): turns Mario by itself, for screenshots
		static const char* sSpin = std::getenv("SMS_PORT_MP_PREVIEW_SPIN");
		if (sSpin != nullptr)
			sPreviewTurn = sPreviewTurn.load() + (float)std::atof(sSpin);
		const s16 yaw     = (s16)(sFaceCamera + (int)(sPreviewTurn.load() * (65536.0f / 360.0f)));
		m->mFaceAngle.y   = yaw;
		m->mModelFaceAngle = yaw; // what the model is drawn with
		return 0;
	}
	const float x = m->mPosition.x;
	if (!sPanned && x < kPanStartX) {
		for (int i = 0; i < 3; i++) {
			sBasePos[i] = pos[i];
			sBaseAt[i]  = at[i];
		}
		pos[0] += kPanX;
		at[0] += kPanX;
		sPanned = true;
		return 1;
	}
	if (sPanned && x > kPanBackX) {
		for (int i = 0; i < 3; i++) {
			pos[i] = sBasePos[i];
			at[i]  = sBaseAt[i];
		}
		sPanned = false;
		return 1;
	}
	return 0;
}

int port_mp_block_active(void) { return sBlock != nullptr; }

// CardLoad.cpp: the multiplayer boxes' opacity (255 - the normal overlay's while it fades).
void port_mp_block_set_2d(int alpha)
{
	sScreenAlpha = alpha < 0 ? 0 : (alpha > 255 ? 255 : alpha);
	if (sScreenRoot != nullptr)
		sScreenRoot->setAlpha((u8)sScreenAlpha);
}

// CardLoad.cpp, CUE_DRAW of the save-block screen, after the normal overlay.
void port_mp_block_draw2d(const void* graph)
{
	if (sBlock == nullptr || sScreen == nullptr || sScreenAlpha == 0 || sPreview)
		return;
	// text boxes keep their own alpha: they appear once the windows are half in
	for (int i = 0; i < 6; i++)
		if (sScreenText[i] != nullptr) {
			if (sScreenAlpha > 128)
				sScreenText[i]->show();
			else
				sScreenText[i]->hide();
		}
	sScreen->draw(0, 0, (const J2DGrafContext*)graph);
}

// SMS_PORT_MP_BLOCK_DUMP=1 (debug): the file select's 2D panes (tag, kind, bounds, text).
static void dumpPane(J2DPane* p, int depth)
{
	const u32 tag = (u32)p->mUserInfoTag, kind = p->getKind();
	const JUTRect& b = p->getBounds();
	char t[5] = { (char)(tag >> 24), (char)(tag >> 16), (char)(tag >> 8), (char)tag, 0 };
	char k[5] = { (char)(kind >> 24), (char)(kind >> 16), (char)(kind >> 8), (char)kind, 0 };
	std::fprintf(stderr, "[mp-block] %*s%s %s %d,%d-%d,%d vis %d a %d %s\n", depth * 2, "", t, k, b.x1, b.y1, b.x2,
	             b.y2, p->isVisible(), p->getAlpha(), kind == 'TBX1' ? ((J2DTextBox*)p)->getStringPtr() : "");
	for (JSUTree<J2DPane>* c = p->getFirstChild(); c != p->getEndChild(); c = c->getNextChild())
		dumpPane(c->getObject(), depth + 1);
}
void port_mp_block_dump_screen(void* root)
{
	static bool sDone;
	if (sDone || !std::getenv("SMS_PORT_MP_BLOCK_DUMP") || root == nullptr)
		return;
	sDone = true;
	dumpPane((J2DPane*)root, 0);
}

// CardLoad.cpp: 1 = the camera is over the block (the 2D file select fades out).
int port_mp_block_panned(void) { return sBlock != nullptr && (sPanned || sPreview); }

// port_mp_menu.cpp: the customization page wants the 3D preview (every frame, 0 = off).
void port_mp_block_preview(int on) { sPreviewWant = on && sBlock != nullptr ? 1 : 0; }
// ... turns Mario by `deg` (drag, L / R, the arrow buttons).
void port_mp_block_preview_rotate(float deg)
{
	float t = sPreviewTurn.load() + deg;
	while (t > 180.0f)
		t -= 360.0f;
	while (t < -180.0f)
		t += 360.0f;
	sPreviewTurn = t;
}
int port_mp_block_preview_active(void) { return sPreviewWant.load() != 0; }

} // extern "C"

#endif // SMS_PORT_AURORA
