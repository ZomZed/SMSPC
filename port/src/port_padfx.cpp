// DualSense / DualShock 4 extras for the Aurora build (w9): adaptive triggers and the
// lightbar, driven by game state that is only READ here (never written).
//
// Called once per game frame from port_input_read (main thread, right after PADRead), so
// the controller is updated at most once per logic frame, and only when something changed.
//
// Adaptive triggers (DualSense only; SDL_SendGamepadEffect with a DS5 effect packet that
// sets only the two trigger enable bits, 0x04 right / 0x08 left of the first byte, so the
// rumble and the lightbar state in the controller are left alone):
//  - GameCube click: light resistance through the analog range (zones 0-8 of the trigger's
//    10 zones), a firm wall in zone 9, where Aurora's emulated digital click sits (95%):
//    pushing through the wall = full press = digital L/R, as on a GameCube pad.
//  - Full FLUDD feedback, on R: slack (no effect) when the tank is empty or a trigger nozzle
//    waits for the release; a gentle pulse while spraying (stronger towards a full pull);
//    resistance that builds while the rocket/turbo nozzle charges; a steady thrum while
//    hovering, a fast buzz while turbo-dashing, a hard kick at rocket launch.
//    Otherwise (water left, not spraying) the GameCube click. L always has the click.
//  Effect packets follow the documented DualSense trigger modes (0x05 off, 0x21 feedback
//  with a 3-bit force per zone, 0x26 vibration with a 3-bit amplitude per zone and the
//  frequency in byte 9).
//
// Lightbar (DualSense and DualShock 4; Aurora's PADSetColor, which calls SDL_SetGamepadLED
// on the next PADRead): the colour of the fluid, from the game's own water colour table
// gModelWaterManagerWaterColor[] (ModelWaterManager.cpp; index 0 = FLUDD water, 1-3 = the
// Yoshi juices, selected by TModelWaterManager::unk5D5F = TYoshi::mType while riding), the
// way the game tints the water it draws. The table colour is scaled to full brightness and
// its saturation raised to at least 80% so an LED shows that hue (FLUDD's {60,70,120} would
// otherwise read as white). Brightness follows the remaining fluid (TWaterGun current / max
// water; Yoshi: TYoshi juice unkC / unk8, as the HUD gauge), through a gamma-2.2 curve so
// the dimming stays visible down to a faint glow at empty. Without FLUDD (secret courses,
// before it is found), in the pause/map/save menus and outside a stage (title, file select,
// movies): a dim neutral white.
//
// SMS_PORT_PADFX_LOG=1 logs every lightbar/trigger change; =2 also Mario's trigger state
// each game frame. SMS_PORT_VPAD=ps5|ps4 (+ SMS_PORT_VPADSCRIPT) attaches a virtual
// controller through SDL's virtual joystick driver and logs what SDL hands it (LED colour,
// effect packets, rumble): the whole path except the controller's own HID report. With it,
// every physical backend stays off (port_settings.cpp), so the player's pad is never used.
#ifdef SMS_PORT_AURORA

#include "port_settings.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <Player/Mario.hpp>
#include <Player/ModelWaterManager.hpp>
#include <Player/WaterGun.hpp>
#include <Player/Yoshi.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

extern GXColor gModelWaterManagerWaterColor[4]; // src/Player/ModelWaterManager.cpp
extern "C" volatile unsigned gPortFrameCount;   // port_crash.cpp (frames as SMS_PORT_SHOT)

namespace {

const int kLogLevel = [] {
	const char* e = std::getenv("SMS_PORT_PADFX_LOG");
	return e != nullptr ? std::atoi(e) : 0;
}();

// ---- what the game is doing (read only) -----------------------------------------------
enum Scene { SCENE_NONE, SCENE_MENU, SCENE_NO_FLUDD, SCENE_FLUDD, SCENE_YOSHI };

struct GameState {
	Scene scene      = SCENE_NONE;
	float fill       = 0.0f; // remaining fluid 0..1
	int colorIdx     = 0;    // gModelWaterManagerWaterColor index
	int nozzle       = -1;   // TWaterGun::TNozzleType
	bool emitting    = false;
	int triggerState = 0;    // TNozzleTrigger::SPRAYSTATE (trigger nozzles)
	float charge     = 0.0f; // trigger nozzles: inside pressure / max
};

GameState readGame()
{
	GameState g;
	TApplication* app = &gpApplication;
	TMario* m         = gpMarioOriginal;
	if (gpMarDirector == nullptr || app->mDirector != (JDrama::TDirector*)gpMarDirector || m == nullptr
	    || !gpMarDirector->mSetupDone)
		return g; // title, file select, movies, loading
	const u8 st = gpMarDirector->mState;
	if (st == TMarDirector::STATE_PAUSE_MENU || st == TMarDirector::STATE_GUIDE || st == TMarDirector::STATE_CARD_SAVE) {
		g.scene = SCENE_MENU;
		return g;
	}
	TYoshi* y = m->mYoshi;
	if (y != nullptr && y->mState == TYoshi::STATE_MOUNTED) {
		g.scene    = SCENE_YOSHI;
		g.fill     = y->unk8 > 0 ? (float)y->unkC / (float)y->unk8 : 0.0f; // as the HUD gauge
		g.colorIdx = gpModelWaterManager != nullptr ? gpModelWaterManager->unk5D5F : 0;
	} else if (!m->checkFlag(MARIO_FLAG_HAS_FLUDD) || m->mWaterGun == nullptr) {
		g.scene = SCENE_NO_FLUDD;
		return g;
	} else {
		g.scene = SCENE_FLUDD;
	}
	TWaterGun* w = m->mWaterGun;
	if (w == nullptr)
		return g;
	const s32 maxWater = ((const TWaterGun*)w)->getMaxWater();
	if (g.scene == SCENE_FLUDD)
		g.fill = maxWater > 0 ? (float)w->mCurrentWater / (float)maxWater : 0.0f;
	g.nozzle   = w->mCurrentNozzle;
	g.emitting = w->canSpray();
	TNozzleBase* nz = ((const TWaterGun*)w)->getCurrentNozzle();
	if (nz != nullptr && nz->getNozzleKind() == 1) {
		const TNozzleTrigger* t = (const TNozzleTrigger*)nz;
		g.triggerState          = t->unk385;
		const f32 mx            = t->mEmitParams.mInsidePressureMax.get();
		g.charge                = mx > 0.0f ? t->unk388 / mx : 0.0f;
	}
	if (g.fill < 0.0f)
		g.fill = 0.0f;
	if (g.fill > 1.0f)
		g.fill = 1.0f;
	if (g.colorIdx < 0 || g.colorIdx > 3)
		g.colorIdx = 0;
	return g;
}

// ---- trigger effects -------------------------------------------------------------------
typedef u8 Effect[11];

void fxOff(Effect e)
{
	std::memset(e, 0, sizeof(Effect));
	e[0] = 0x05;
}

// Per-zone force 0..8 (0 = none in that zone).
void fxFeedback(Effect e, const int force[10])
{
	u32 forces = 0;
	u16 active = 0;
	for (int i = 0; i < 10; i++)
		if (force[i] > 0) {
			forces |= (u32)((force[i] > 8 ? 8 : force[i]) - 1) << (3 * i);
			active |= (u16)(1 << i);
		}
	if (active == 0) {
		fxOff(e);
		return;
	}
	std::memset(e, 0, sizeof(Effect));
	e[0] = 0x21;
	e[1] = (u8)active, e[2] = (u8)(active >> 8);
	e[3] = (u8)forces, e[4] = (u8)(forces >> 8), e[5] = (u8)(forces >> 16), e[6] = (u8)(forces >> 24);
}

// Per-zone amplitude 0..8, frequency in Hz.
void fxVibration(Effect e, int freq, const int amp[10])
{
	u32 amps   = 0;
	u16 active = 0;
	for (int i = 0; i < 10; i++)
		if (amp[i] > 0) {
			amps |= (u32)((amp[i] > 8 ? 8 : amp[i]) - 1) << (3 * i);
			active |= (u16)(1 << i);
		}
	if (active == 0 || freq <= 0) {
		fxOff(e);
		return;
	}
	std::memset(e, 0, sizeof(Effect));
	e[0] = 0x26;
	e[1] = (u8)active, e[2] = (u8)(active >> 8);
	e[3] = (u8)amps, e[4] = (u8)(amps >> 8), e[5] = (u8)(amps >> 16), e[6] = (u8)(amps >> 24);
	e[9] = (u8)(freq > 255 ? 255 : freq);
}

int scaled(float v, float s, int lo) // v (1..8 at full strength) * strength, at least lo
{
	int n = (int)std::lround(v * s);
	return n < lo ? lo : n > 8 ? 8 : n;
}

// GameCube click: light resistance up to the click zone, a firm wall at it.
const char* fxClick(Effect e, float s)
{
	if (s <= 0.0f) {
		fxOff(e);
		return "off";
	}
	const int light = scaled(3.0f, s, 1);
	int wall        = scaled(8.0f, s, 2);
	if (wall <= light)
		wall = light + 1;
	int f[10];
	for (int i = 0; i < 9; i++)
		f[i] = light;
	f[9] = wall;
	fxFeedback(e, f);
	return "click";
}

const char* fxFludd(Effect e, const GameState& g, float s)
{
	if (g.scene != SCENE_FLUDD && g.scene != SCENE_YOSHI)
		return fxClick(e, s);
	if (s <= 0.0f) {
		fxOff(e);
		return "off";
	}
	if (g.fill <= 0.0f) { // empty tank / no juice: the trigger goes slack
		fxOff(e);
		return "slack (empty)";
	}
	int z[10];
	const bool trig = g.nozzle == TWaterGun::Rocket || g.nozzle == TWaterGun::Hover || g.nozzle == TWaterGun::Turbo;
	if (trig) {
		if (g.triggerState == TNozzleTrigger::DEAD) { // fired: waits for the release
			fxOff(e);
			return "slack (release)";
		}
		if (g.triggerState == TNozzleTrigger::ACTIVE) {
			if (g.nozzle == TWaterGun::Hover) {
				for (int i = 0; i < 10; i++)
					z[i] = scaled(4.0f, s, 1);
				fxVibration(e, 15, z);
				return "hover thrum";
			}
			if (g.nozzle == TWaterGun::Turbo) {
				for (int i = 0; i < 10; i++)
					z[i] = scaled(7.0f, s, 1);
				fxVibration(e, 45, z);
				return "turbo buzz";
			}
			for (int i = 0; i < 10; i++)
				z[i] = scaled(8.0f, s, 1);
			fxVibration(e, 30, z);
			return "rocket kick";
		}
		if (g.charge > 0.02f && g.nozzle != TWaterGun::Hover) { // rocket / turbo charging
			for (int i = 0; i < 10; i++)
				z[i] = scaled(1.0f + 7.0f * (g.charge > 1.0f ? 1.0f : g.charge), s, 1);
			fxFeedback(e, z);
			return "charging";
		}
		return fxClick(e, s);
	}
	if (g.emitting) { // spray / underwater / Yoshi juice: a pulse, stronger towards a full pull
		for (int i = 0; i < 10; i++)
			z[i] = i < 2 ? 0 : scaled(1.0f + 3.0f * (float)(i - 2) / 7.0f, s, 1);
		fxVibration(e, 10, z);
		return "spray pulse";
	}
	return fxClick(e, s);
}

// ---- lightbar -------------------------------------------------------------------------
struct Rgb {
	int r, g, b;
	bool operator!=(const Rgb& o) const { return r != o.r || g != o.g || b != o.b; }
};

// The game's water colour at full brightness, saturation raised to >= 80%.
Rgb fluidColor(int idx)
{
	const GXColor c = gModelWaterManagerWaterColor[idx];
	float v[3]      = { (float)c.r, (float)c.g, (float)c.b };
	float mx        = v[0] > v[1] ? (v[0] > v[2] ? v[0] : v[2]) : (v[1] > v[2] ? v[1] : v[2]);
	if (mx <= 0.0f)
		return Rgb { 0, 0, 255 };
	for (float& x : v)
		x = x * 255.0f / mx;
	float mn = v[0] < v[1] ? (v[0] < v[2] ? v[0] : v[2]) : (v[1] < v[2] ? v[1] : v[2]);
	const float kMinFloor = 255.0f * 0.2f; // saturation (255 - min) / 255 >= 0.8
	if (mn > kMinFloor)
		for (float& x : v)
			x = 255.0f - (255.0f - x) * (255.0f - kMinFloor) / (255.0f - mn);
	return Rgb { (int)std::lround(v[0]), (int)std::lround(v[1]), (int)std::lround(v[2]) };
}

Rgb scale(Rgb c, float k)
{
	auto f = [k](int x) {
		const int n = (int)std::lround(x * k);
		return n < 0 ? 0 : n > 255 ? 255 : n;
	};
	return Rgb { f(c.r), f(c.g), f(c.b) };
}

// Perceptual dimming: visible steps all the way down, a faint glow at empty.
float fluidLevel(float fill)
{
	if (fill <= 0.0f)
		return 0.012f;
	return 0.03f + 0.97f * std::pow(fill, 2.2f);
}

const Rgb kNeutral = { 255, 240, 220 }; // warm white
// FLUDD's water at full fill: #00C9FF (chosen by the user; the game's water table gives a
// darker blue, 51,85,255 after the saturation lift). Yoshi's juices keep the game's colours.
const Rgb kFluddWater = { 0, 201, 255 };
const float kNeutralLevel = 0.06f;

Rgb lightbarColor(const GameState& g, const PortSettings& s, const char** why)
{
	const float b = s.lightbarBrightness / 100.0f;
	if (s.lightbar == PORT_LIGHTBAR_STATIC) {
		*why = "static";
		return scale(Rgb { s.lightbarColor[0], s.lightbarColor[1], s.lightbarColor[2] }, b);
	}
	if (g.scene == SCENE_YOSHI) {
		*why = "yoshi juice";
		return scale(fluidColor(g.colorIdx), fluidLevel(g.fill) * b);
	}
	if (g.scene == SCENE_FLUDD) {
		*why = "fludd water";
		return scale(kFluddWater, fluidLevel(g.fill) * b);
	}
	*why = g.scene == SCENE_MENU ? "neutral (menu)" : g.scene == SCENE_NO_FLUDD ? "neutral (no FLUDD)" : "neutral";
	return scale(kNeutral, kNeutralLevel * b);
}

// ---- per-controller output state --------------------------------------------------------
struct Out {
	SDL_Gamepad* gp = nullptr;
	bool ledSet     = false; // we changed the LED (restored on Off / shutdown)
	Rgb led         = { -1, -1, -1 };
	bool fxSet      = false; // we set a trigger effect (cleared on Off / shutdown)
	Effect l, r;
	std::string status = "No DualSense or DualShock 4 on controller 1.";
};
Out sOut;
std::mutex sOutMutex; // sOut.l / r / led are read by the Bluetooth stream thread

void hex(const Effect e, char* buf)
{
	for (int i = 0; i < 11; i++)
		std::snprintf(buf + i * 3, 4, "%02x ", e[i]);
	buf[32] = '\0';
}

bool sendTriggers(SDL_Gamepad* gp, const Effect l, const Effect r)
{
	// SDL's DS5EffectsState_t (47 bytes): only the right/left trigger fields are enabled.
	u8 pkt[47] = {};
	pkt[0]     = 0x04 | 0x08;
	std::memcpy(pkt + 10, r, 11);
	std::memcpy(pkt + 21, l, 11);
	return SDL_SendGamepadEffect(gp, pkt, sizeof(pkt));
}

} // namespace

extern "C" {

void port_padfx_frame(void)
{
	const PortSettings& s = gPortSettings;
	const s32 index       = PADGetIndexForPort(0);
	SDL_Gamepad* gp       = index >= 0 ? PADGetSDLGamepadForIndex((u32)index) : nullptr;
	if (gp != sOut.gp) { // (re)connected: nothing sent to this one yet
		sOut       = Out();
		sOut.gp    = gp;
		if (gp != nullptr)
			std::fprintf(stderr, "[padfx] controller 1: %s, type %d, lightbar %s, adaptive triggers %s\n",
			             SDL_GetGamepadName(gp) ? SDL_GetGamepadName(gp) : "?", (int)PADGetControllerType(0),
			             PADHasLED(0) ? "yes" : "no", PADGetControllerType(0) == PAD_TYPE_PS5 ? "yes" : "no");
	}
	if (gp == nullptr)
		return;
	const bool ds5 = PADGetControllerType(0) == PAD_TYPE_PS5;
	const bool led = PADHasLED(0) != FALSE;
	if (!ds5 && !led) {
		sOut.status = "Controller 1 has no lightbar or adaptive triggers.";
		return;
	}
	const GameState g = readGame();
	const u32 frame   = gPortFrameCount;

	if (kLogLevel >= 2 && gpMarioOriginal != nullptr && gpMarioOriginal->unk108 != nullptr) {
		const TMarioControllerWork* cw = gpMarioOriginal->unk108;
		std::fprintf(stderr,
		             "[padfx] frame %u: analogR u8 %u level %.3f R-button %d | fwd vel %.2f upper %u emitting %d "
		             "water %.3f nozzle %d trig-state %d charge %.2f\n",
		             frame, cw->mAnalogRU8, cw->mAnalogR, (cw->mInput & TMarioControllerWork::R) ? 1 : 0,
		             gpMarioOriginal->mForwardVel, gpMarioOriginal->mUpperState, g.emitting ? 1 : 0, g.fill, g.nozzle,
		             g.triggerState, g.charge);
	}

	// Lightbar
	if (led) {
		if (s.lightbar == PORT_LIGHTBAR_OFF) {
			if (sOut.ledSet) { // give it back to SDL's player-1 colour
				PADSetColor(0, 0x00, 0x00, 0x40);
				sOut.ledSet = false;
				sOut.led    = Rgb { -1, -1, -1 };
				if (kLogLevel)
					std::fprintf(stderr, "[padfx] frame %u: lightbar off (player colour 0,0,64)\n", frame);
			}
		} else {
			const char* why = "";
			const Rgb c     = lightbarColor(g, s, &why);
			if (c != sOut.led) {
				PADSetColor(0, (u8)c.r, (u8)c.g, (u8)c.b);
				std::lock_guard<std::mutex> lk(sOutMutex);
				sOut.led    = c;
				sOut.ledSet = true;
				if (kLogLevel)
					std::fprintf(stderr, "[padfx] frame %u: lightbar %3d,%3d,%3d  %s fill %.3f colour %d\n", frame, c.r,
					             c.g, c.b, why, g.fill, g.colorIdx);
			}
		}
	}

	// Adaptive triggers
	const char* lName = "none";
	const char* rName = "none";
	if (ds5) {
		Effect l, r;
		const float str = s.triggerStrength / 100.0f;
		if (s.adaptiveTriggers == PORT_TRIGGERS_OFF) {
			fxOff(l);
			fxOff(r);
			lName = rName = "off";
		} else {
			lName = fxClick(l, str);
			rName = s.adaptiveTriggers == PORT_TRIGGERS_FLUDD ? fxFludd(r, g, str) : fxClick(r, str);
		}
		const bool want = s.adaptiveTriggers != PORT_TRIGGERS_OFF || sOut.fxSet;
		if (want && (!sOut.fxSet || std::memcmp(l, sOut.l, sizeof(Effect)) != 0 || std::memcmp(r, sOut.r, sizeof(Effect)) != 0)) {
			const bool ok = sendTriggers(gp, l, r);
			{
				std::lock_guard<std::mutex> lk(sOutMutex);
				std::memcpy(sOut.l, l, sizeof(Effect));
				std::memcpy(sOut.r, r, sizeof(Effect));
				sOut.fxSet = s.adaptiveTriggers != PORT_TRIGGERS_OFF;
			}
			if (kLogLevel) {
				char hl[40], hr[40];
				hex(l, hl);
				hex(r, hr);
				std::fprintf(stderr, "[padfx] frame %u: triggers L %-6s [%s] R %-16s [%s]%s\n", frame, lName, hl, rName,
				             hr, ok ? "" : "  (SDL refused)");
			}
		}
	}
	char buf[160];
	std::snprintf(buf, sizeof(buf), "Controller 1: %s%s%s.", ds5 ? "DualSense, adaptive triggers " : "lightbar",
	              ds5 ? (s.adaptiveTriggers == PORT_TRIGGERS_OFF ? "off" : rName) : "",
	              led ? (s.lightbar == PORT_LIGHTBAR_OFF ? ", lightbar off" : ", lightbar on") : "");
	sOut.status = buf;
}

void port_padfx_shutdown(void)
{
	port_haptics_shutdown(); // HD rumble / DualSense audio stream (port_haptics.cpp)
	if (sOut.gp == nullptr)
		return;
	if (sOut.fxSet) {
		Effect off;
		fxOff(off);
		sendTriggers(sOut.gp, off, off);
		sOut.fxSet = false;
	}
	if (sOut.ledSet) {
		SDL_SetGamepadLED(sOut.gp, 0x00, 0x00, 0x40);
		sOut.ledSet = false;
	}
}

const char* port_padfx_status(void) { return sOut.status.c_str(); }

// The effects last sent, for the DualSense Bluetooth audio stream's state sub-packet
// (port_speaker.cpp restates them in every 0x36 report). Off when nothing was set.
void port_padfx_ds5_state(unsigned char* l11, unsigned char* r11, int* rgb, bool* ledOn)
{
	std::lock_guard<std::mutex> lk(sOutMutex);
	if (sOut.fxSet) {
		std::memcpy(l11, sOut.l, 11);
		std::memcpy(r11, sOut.r, 11);
	} else {
		fxOff(l11);
		fxOff(r11);
	}
	*ledOn = sOut.ledSet && sOut.led.r >= 0;
	rgb[0] = sOut.led.r, rgb[1] = sOut.led.g, rgb[2] = sOut.led.b;
}

} // extern "C"

// ---- SMS_PORT_VPAD: virtual controller for tests ------------------------------------------
namespace {

struct VPad {
	bool tried         = false;
	SDL_JoystickID id  = 0;
	SDL_Joystick* js   = nullptr;
	float axes[SDL_GAMEPAD_AXIS_COUNT] = {};
	bool buttons[SDL_GAMEPAD_BUTTON_COUNT] = {};
	Uint16 rumbleLo = 0, rumbleHi = 0;
};
VPad sV;

bool SDLCALL vRumbleTriggers(void*, Uint16 l, Uint16 r)
{
	static Uint16 sL = 0xFFFF, sR = 0xFFFF;
	if (l != sL || r != sR)
		std::fprintf(stderr, "[vpad] frame %u: RumbleTriggers %u,%u\n", gPortFrameCount, l, r);
	sL = l, sR = r;
	return true;
}

bool SDLCALL vSetLED(void*, Uint8 r, Uint8 g, Uint8 b)
{
	std::fprintf(stderr, "[vpad] frame %u: SetLED %u,%u,%u\n", gPortFrameCount, r, g, b);
	return true;
}

bool SDLCALL vSendEffect(void*, const void* data, int size)
{
	const u8* p = (const u8*)data;
	char hr[40], hl[40];
	if (size >= 32) {
		hex(p + 10, hr);
		hex(p + 21, hl);
	} else {
		hr[0] = hl[0] = '\0';
	}
	std::fprintf(stderr, "[vpad] frame %u: SendEffect %d bytes, enable %02x %02x, R [%s] L [%s]\n", gPortFrameCount,
	             size, size > 0 ? p[0] : 0, size > 1 ? p[1] : 0, hr, hl);
	return true;
}

bool SDLCALL vRumble(void*, Uint16 lo, Uint16 hi)
{
	if (lo != sV.rumbleLo || hi != sV.rumbleHi)
		std::fprintf(stderr, "[vpad] frame %u: Rumble %u,%u\n", gPortFrameCount, lo, hi);
	sV.rumbleLo = lo, sV.rumbleHi = hi;
	return true;
}

int buttonByName(const char* n)
{
	static const struct {
		const char* name;
		int b;
	} k[] = {
		{ "SOUTH", SDL_GAMEPAD_BUTTON_SOUTH }, { "CROSS", SDL_GAMEPAD_BUTTON_SOUTH },
		{ "EAST", SDL_GAMEPAD_BUTTON_EAST },   { "CIRCLE", SDL_GAMEPAD_BUTTON_EAST },
		{ "WEST", SDL_GAMEPAD_BUTTON_WEST },   { "SQUARE", SDL_GAMEPAD_BUTTON_WEST },
		{ "NORTH", SDL_GAMEPAD_BUTTON_NORTH }, { "TRIANGLE", SDL_GAMEPAD_BUTTON_NORTH },
		{ "START", SDL_GAMEPAD_BUTTON_START }, { "OPTIONS", SDL_GAMEPAD_BUTTON_START },
		{ "BACK", SDL_GAMEPAD_BUTTON_BACK },   { "CREATE", SDL_GAMEPAD_BUTTON_BACK },
		{ "LB", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER }, { "RB", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
		{ "UP", SDL_GAMEPAD_BUTTON_DPAD_UP },  { "DOWN", SDL_GAMEPAD_BUTTON_DPAD_DOWN },
		{ "LEFT", SDL_GAMEPAD_BUTTON_DPAD_LEFT }, { "RIGHT", SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
	};
	for (const auto& e : k)
		if (_stricmp(n, e.name) == 0)
			return e.b;
	return -1;
}

int axisByName(const char* n)
{
	static const char* const k[] = { "LX", "LY", "RX", "RY", "LT", "RT" };
	for (int i = 0; i < 6; i++)
		if (_stricmp(n, k[i]) == 0)
			return i;
	return -1;
}

// SMS_PORT_VPADSCRIPT=<from>[-<to>]:<ctl>[=<value>][+<ctl>...][,...]: holds those controls
// from frame <from> to <to> (inclusive; default <from>+10), frames as SMS_PORT_KEYSCRIPT.
// Controls: LT RT (0..1), LX LY RX RY (-1..1, Y up), buttons (CROSS, SQUARE, CIRCLE, TRIANGLE,
// OPTIONS, CREATE, LB, RB, UP/DOWN/LEFT/RIGHT, or SOUTH/EAST/WEST/NORTH/START/BACK).
void vScript(unsigned frame)
{
	for (float& a : sV.axes)
		a = 0.0f;
	for (bool& b : sV.buttons)
		b = false;
	const char* sc = std::getenv("SMS_PORT_VPADSCRIPT");
	if (sc == nullptr)
		return;
	for (const char* p = sc; *p != '\0';) {
		char* end;
		const unsigned long from = std::strtoul(p, &end, 10);
		if (end == p)
			break;
		unsigned long to = from + 10;
		p                = end;
		if (*p == '-') {
			to = std::strtoul(p + 1, &end, 10);
			p  = end;
		}
		if (*p != ':')
			break;
		++p;
		while (*p != '\0' && *p != ',') {
			char name[32];
			int n = 0;
			while (*p != '\0' && *p != ',' && *p != '+' && *p != '=') {
				if (n < 31)
					name[n++] = *p;
				++p;
			}
			name[n]     = '\0';
			float value = 1.0f;
			if (*p == '=') {
				value = (float)std::strtod(p + 1, &end);
				p     = end;
			}
			if (frame >= from && frame <= to) {
				const int a = axisByName(name);
				const int b = buttonByName(name);
				if (a >= 0)
					sV.axes[a] = value;
				else if (b >= 0)
					sV.buttons[b] = value >= 0.5f;
			}
			if (*p == '+')
				++p;
		}
		if (*p == ',')
			++p;
	}
}

Sint16 axisValue(int axis, float v)
{
	if (axis >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER) { // triggers: the full joystick range is 0..1
		v = v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
		return (Sint16)std::lround(-32768.0 + v * 65535.0);
	}
	v = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
	if (axis == SDL_GAMEPAD_AXIS_LEFTY || axis == SDL_GAMEPAD_AXIS_RIGHTY)
		v = -v; // SDL's Y points down
	return (Sint16)std::lround(v * 32767.0);
}

} // namespace

extern "C" {

bool port_vpad_requested(unsigned* vid, unsigned* pid)
{
	const char* e = std::getenv("SMS_PORT_VPAD");
	if (e == nullptr || *e == '\0' || *e == '0')
		return false;
	if (_stricmp(e, "xbox") == 0) { // Xbox Series controller (impulse triggers)
		*vid = 0x045E;
		*pid = 0x0B12;
		return true;
	}
	*vid = 0x054C;
	*pid = _stricmp(e, "ps4") == 0 ? 0x09CC : 0x0CE6; // DualShock 4 v2 / DualSense
	return true;
}

void port_vpad_update(void)
{
	unsigned vid, pid;
	if (!port_vpad_requested(&vid, &pid))
		return;
	if (!sV.tried) {
		sV.tried = true;
		SDL_VirtualJoystickDesc d;
		SDL_INIT_INTERFACE(&d);
		d.type        = SDL_JOYSTICK_TYPE_GAMEPAD;
		d.vendor_id   = (Uint16)vid;
		d.product_id  = (Uint16)pid;
		d.naxes       = SDL_GAMEPAD_AXIS_COUNT;
		d.nbuttons    = SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1;
		d.button_mask = (1u << (SDL_GAMEPAD_BUTTON_DPAD_RIGHT + 1)) - 1;
		d.axis_mask   = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
		d.name        = vid == 0x045E ? "SMS test virtual Xbox controller"
		                : pid == 0x09CC ? "SMS test virtual DualShock 4" : "SMS test virtual DualSense";
		d.Rumble      = vRumble;
		if (vid == 0x045E)
			d.RumbleTriggers = vRumbleTriggers;
		else {
			d.SetLED     = vSetLED;
			d.SendEffect = vSendEffect;
		}
		sV.id         = SDL_AttachVirtualJoystick(&d);
		sV.js         = sV.id != 0 ? SDL_OpenJoystick(sV.id) : nullptr;
		std::fprintf(stderr, "[vpad] attached %s (%04x:%04x): %s\n", d.name, vid, pid,
		             sV.js != nullptr ? "ok" : SDL_GetError());
	}
	if (sV.js == nullptr)
		return;
	const unsigned frame = gPortFrameCount;
	static float sLast[SDL_GAMEPAD_AXIS_COUNT] = { -9, -9, -9, -9, -9, -9 };
	vScript(frame);
	for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) {
		SDL_SetJoystickVirtualAxis(sV.js, a, axisValue(a, sV.axes[a]));
		if (a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER && sV.axes[a] != sLast[a]) {
			SDL_Gamepad* g = SDL_GetGamepadFromID(sV.id);
			std::fprintf(stderr, "[vpad] frame %u: %s = %.3f (joystick axis %d; SDL gamepad axis now %d)\n", frame,
			             a == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? "LT" : "RT", sV.axes[a], axisValue(a, sV.axes[a]),
			             g != nullptr ? SDL_GetGamepadAxis(g, (SDL_GamepadAxis)a) : -1);
		}
		sLast[a] = sV.axes[a];
	}
	for (int b = 0; b <= SDL_GAMEPAD_BUTTON_DPAD_RIGHT; b++)
		SDL_SetJoystickVirtualButton(sV.js, b, sV.buttons[b]);
}

} // extern "C"

#endif // SMS_PORT_AURORA
