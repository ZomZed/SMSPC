// Game side of true widescreen (Hor+), w11. See port/include/port_widescreen.h.
#include <port_widescreen.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef SMS_PORT_AURORA
#include "port_settings.h" // gPortSettings.hudPosition (w9's settings)
#include <dolphin/gx.h>    // GXGet/SetProjectionv (PORTWideOrthoProj*)
#endif

// w6's setting (port_window.cpp). Until that is linked in, resolve it to a fallback that
// returns 1 (GameCube picture), the same way port_window.cpp handles VILockAspectRatio.
extern "C" float port_get_aspect_scale(void);
extern "C" float port_get_aspect_scale_missing(void) { return 1.0f; }
#pragma comment(linker, "/alternatename:port_get_aspect_scale=port_get_aspect_scale_missing")

namespace {

const float kFrameCentreX = 320.0f; // centre of the 640-wide logical frame

// SMS_PORT_WIDESCREEN: "16:9", "64:27", "21:9" (= 64:27), or a display aspect like 1.7778.
// <= 0 when unset.
float envDisplayAspect()
{
	static float sAspect = -2.0f;
	if (sAspect > -1.5f)
		return sAspect;
	sAspect          = -1.0f;
	const char* e    = std::getenv("SMS_PORT_WIDESCREEN");
	if (e == nullptr || *e == '\0')
		return sAspect;
	int num = 0, den = 0;
	if (std::sscanf(e, "%d:%d", &num, &den) == 2 && num > 0 && den > 0) {
		if (num == 21 && den == 9) // marketing "21:9" is 64:27 (2560x1080)
			num = 64, den = 27;
		sAspect = (float)num / (float)den;
	} else {
		sAspect = (float)std::atof(e);
	}
	std::fprintf(stderr, "[port] widescreen: SMS_PORT_WIDESCREEN=%s -> display aspect %.4f\n", e,
	             sAspect);
	return sAspect;
}

// Per aspect field: the game's own value and what we last wrote there.
struct AspectSlot {
	float* field;
	float base;
	float applied;
};
AspectSlot sSlots[16];
int sNextSlot;

// SMS_PORT_WIDESCREEN_LOG=1: every 600 hook calls, how many calls changed a value. At 4:3
// "changed" must stay 0 (the hooks never touch the GameCube state there).
unsigned sCalls, sChanged;
void count(bool changed)
{
	static int sLog = -1;
	if (sLog < 0) {
		const char* e = std::getenv("SMS_PORT_WIDESCREEN_LOG");
		sLog          = (e != nullptr && *e != '\0' && *e != '0') ? 1 : 0;
	}
	if (sLog == 0)
		return;
	++sCalls;
	if (changed)
		++sChanged;
	if (sCalls % 600 == 0)
		std::fprintf(stderr, "[port] widescreen: scale %.4f, %u hook calls, %u changed a value\n",
		             PORTWideScale(), sCalls, sChanged);
}

} // namespace

extern "C" float PORTWideScale(void)
{
	const float env = envDisplayAspect();
	float scale;
	if (env > 0.0f)
		scale = env > 4.0f / 3.0f ? env / (4.0f / 3.0f) : 1.0f;
	else
		scale = port_get_aspect_scale();
	if (!(scale > 1.0f) || !(scale < 8.0f)) // 4:3 / fit / stretch, or nonsense
		return 1.0f;
	return scale;
}

extern "C" void PORTWideCameraAspect(float* aspect)
{
	AspectSlot* slot = nullptr;
	for (AspectSlot& s : sSlots)
		if (s.field == aspect) {
			slot = &s;
			break;
		}
	const float scale = PORTWideScale();
	if (slot == nullptr) {
		if (scale == 1.0f) {
			count(false);
			return; // nothing widened yet: leave the field (and the slot table) alone
		}
		slot          = &sSlots[sNextSlot];
		sNextSlot     = (sNextSlot + 1) % (int)(sizeof(sSlots) / sizeof(sSlots[0]));
		slot->field   = aspect;
		slot->base    = *aspect;
		slot->applied = *aspect;
	} else if (*aspect != slot->applied) {
		slot->base = *aspect; // the game (or a cutscene) set a new aspect since last time
	}
	const float before = *aspect;
	slot->applied      = slot->base * scale;
	*aspect            = slot->applied;
	count(*aspect != before);
}

extern "C" void PORTWideSqueezeRectX(float* x, float* width)
{
	const float scale = PORTWideScale();
	if (scale == 1.0f) {
		count(false);
		return;
	}
	const float x1 = kFrameCentreX + (*x - kFrameCentreX) / scale;
	const float x2 = kFrameCentreX + (*x + *width - kFrameCentreX) / scale;
	*x             = x1;
	*width         = x2 - x1;
	count(true);
}

extern "C" void PORTWideSqueezeRectXi(int* x, int* width)
{
	const float scale = PORTWideScale();
	if (scale == 1.0f) {
		count(false);
		return;
	}
	const float x1 = kFrameCentreX + ((float)*x - kFrameCentreX) / scale;
	const float x2 = kFrameCentreX + ((float)(*x + *width) - kFrameCentreX) / scale;
	*x             = (int)std::floor(x1 + 0.5f);
	*width         = (int)std::floor(x2 + 0.5f) - *x;
	count(true);
}

extern "C" void PORTWideOrthoX(float* left, float* right)
{
	const float scale = PORTWideScale();
	if (scale == 1.0f) {
		count(false);
		return;
	}
	const float c    = 0.5f * (*left + *right);
	const float half = 0.5f * (*right - *left) * scale;
	*left            = c - half;
	*right           = c + half;
	count(true);
}

extern "C" int PORTWideOrthoProjBegin(float saved[7])
{
	const float scale = PORTWideScale();
	if (scale == 1.0f) {
		count(false);
		return 0;
	}
#ifndef SMS_PORT_AURORA
	(void)saved;
	return 0;
#else
	GXGetProjectionv(saved);
	if (saved[0] == 0.0f) // perspective
		return 0;
	// x' = p[1] * x + p[2]; dividing both by scale keeps the centre (x' = 0) in place
	float p[7];
	for (int i = 0; i < 7; ++i)
		p[i] = saved[i];
	p[1] /= scale;
	p[2] /= scale;
	GXSetProjectionv(p);
	count(true);
	return 1;
#endif
}

extern "C" void PORTWideOrthoProjEnd(const float saved[7])
{
#ifdef SMS_PORT_AURORA
	GXSetProjectionv(saved);
#else
	(void)saved;
#endif
}

extern "C" float PORTWideSightExtraDeg(float fovy, float aspect)
{
	const float scale = PORTWideScale();
	if (scale == 1.0f)
		return 0.0f;
	const float rad = 3.14159265f / 180.0f;
	const float t   = std::tan(0.5f * fovy * rad);
	const float wide = 2.0f * std::atan(t * aspect);         // aspect is already widened
	const float gc   = 2.0f * std::atan(t * aspect / scale); // GameCube view
	return (wide - gc) / rad;
}

namespace {
// 0 = centred 4:3 (original), 1 = edges of a 16:9 area, 2 = edges of the screen.
int hudPosition()
{
	static int sEnv = -2; // SMS_PORT_HUD_EDGES: unset, or 0 / 1 (= 16:9) / 2 (= match)
	if (sEnv == -2) {
		const char* e = std::getenv("SMS_PORT_HUD_EDGES");
		sEnv          = (e == nullptr || *e == '\0') ? -1 : std::atoi(e);
	}
	if (sEnv >= 0)
		return sEnv;
#ifdef SMS_PORT_AURORA
	return gPortSettings.hudPosition; // w9's settings file, [window] hud_position
#else
	return 0;
#endif
}
} // namespace

extern "C" float PORTWideHudShift(int side, float frameWidth)
{
	const int mode = hudPosition();
	float scale    = PORTWideScale();
	if (scale == 1.0f || side == 0 || mode <= 0)
		return 0.0f;
	if (mode == 1 && scale > (16.0f / 9.0f) / (4.0f / 3.0f))
		scale = (16.0f / 9.0f) / (4.0f / 3.0f); // edges of a 16:9 area on wider screens
	// The wide picture shows c -+ c*scale of the frame (c = its centre): move the 4:3 edge
	// to that edge.
	return (side < 0 ? -1.0f : 1.0f) * 0.5f * frameWidth * (scale - 1.0f);
}

extern "C" int PORTWideHudSide(float centreX, float frameWidth)
{
	const float c    = 0.5f * frameWidth;
	const float band = frameWidth / 8.0f; // the lives counter and the news ticker stay
	if (centreX < c - band)
		return -1;
	if (centreX > c + band)
		return 1;
	return 0;
}