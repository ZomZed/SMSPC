// Controller input for the Aurora build: GameCube PAD semantics on top of Aurora's PAD
// layer, and a keyboard pad. See port_settings.h. (w9)
//
// What the game expects (JUTGamePad::read = PADRead + PADClamp, TMarioGamePad):
// - Sticks: PADClamp removes a 15-count per-axis dead zone and clamps to the GameCube
//   octagon (main stick 72 on the axes, 40/40 on the diagonals; C-stick 59, 31/31).
//   Aurora's PADClamp scales a full +-127 deflection onto that range, so a modern stick
//   reaches the GameCube maximum at full tilt. Aurora additionally applies its own 24%
//   per-axis dead zone before that (it snaps the stick to the axes within ~14 degrees,
//   twice the GameCube's); it is set to [input] stick_deadzone / cstick_deadzone
//   (default 0: only the GameCube's own dead zone, which PADClamp applies).
// - Analog L/R: PADClamp maps raw 30..180 to 0..150, the range the game reads
//   (mAnalogR / 150, TMario::getLRLevel). On a GameCube pad the analog value reaches its
//   top as the trigger reaches the click (digital L/R). Aurora maps the trigger axis
//   linearly onto 0..255, so the analog value already saturates at 70% of the travel and
//   the last 25% before the click (Aurora clicks at 95%) did nothing. With
//   linear_triggers, the travel up to the click point maps onto 30..180 instead.
// - Buttons: [input] layout = gamecube puts A/B/X/Y where a GameCube pad has them
//   (big button at the bottom = A, left = B, right = X, top = Y), like Aurora already
//   does for Xbox pads. PlayStation: Cross A, Square B, Circle X, Triangle Y, R1 Z,
//   L2/R2 L/R (analog), Options Start, D-pad D-pad.
// - Rumble: the game switches the motor on/off per frame (RumbleMgr); Aurora forwards it
//   to SDL rumble. [input] rumble sets the strength.
#ifdef SMS_PORT_AURORA

#include "port_settings.h"
#include "port_replay.h"
#include "port_mp.h" // w10: port_mp_menu_wants_input

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

PADDefaultMapping gcMapping()
{
	PADDefaultMapping m = {};
	const PADButtonMapping buttons[PAD_BUTTON_COUNT] = {
		{ SDL_GAMEPAD_BUTTON_SOUTH, PAD_BUTTON_A },
		{ SDL_GAMEPAD_BUTTON_WEST, PAD_BUTTON_B },
		{ SDL_GAMEPAD_BUTTON_EAST, PAD_BUTTON_X },
		{ SDL_GAMEPAD_BUTTON_NORTH, PAD_BUTTON_Y },
		{ SDL_GAMEPAD_BUTTON_START, PAD_BUTTON_START },
		{ SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_TRIGGER_Z },
		{ PAD_NATIVE_BUTTON_INVALID, PAD_TRIGGER_L }, // analog trigger axis (emulated click)
		{ PAD_NATIVE_BUTTON_INVALID, PAD_TRIGGER_R },
		{ SDL_GAMEPAD_BUTTON_DPAD_UP, PAD_BUTTON_UP },
		{ SDL_GAMEPAD_BUTTON_DPAD_DOWN, PAD_BUTTON_DOWN },
		{ SDL_GAMEPAD_BUTTON_DPAD_LEFT, PAD_BUTTON_LEFT },
		{ SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PAD_BUTTON_RIGHT },
	};
	// Aurora's default axes (PADSetDefaultMapping replaces them too). SDL's Y axes point
	// down, the GameCube's up.
	const PADAxisMapping axes[PAD_AXIS_COUNT] = {
		{ { SDL_GAMEPAD_AXIS_LEFTX, AXIS_SIGN_POSITIVE }, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_X_POS },
		{ { SDL_GAMEPAD_AXIS_LEFTX, AXIS_SIGN_NEGATIVE }, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_X_NEG },
		{ { SDL_GAMEPAD_AXIS_LEFTY, AXIS_SIGN_NEGATIVE }, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_Y_POS },
		{ { SDL_GAMEPAD_AXIS_LEFTY, AXIS_SIGN_POSITIVE }, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_LEFT_Y_NEG },
		{ { SDL_GAMEPAD_AXIS_RIGHTX, AXIS_SIGN_POSITIVE }, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_POS },
		{ { SDL_GAMEPAD_AXIS_RIGHTX, AXIS_SIGN_NEGATIVE }, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_X_NEG },
		{ { SDL_GAMEPAD_AXIS_RIGHTY, AXIS_SIGN_NEGATIVE }, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_POS },
		{ { SDL_GAMEPAD_AXIS_RIGHTY, AXIS_SIGN_POSITIVE }, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_RIGHT_Y_NEG },
		{ { SDL_GAMEPAD_AXIS_LEFT_TRIGGER, AXIS_SIGN_POSITIVE }, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_TRIGGER_L },
		{ { SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, AXIS_SIGN_POSITIVE }, SDL_GAMEPAD_BUTTON_INVALID, PAD_AXIS_TRIGGER_R },
	};
	for (int i = 0; i < PAD_BUTTON_COUNT; i++)
		m.buttons[i] = buttons[i];
	for (int i = 0; i < PAD_AXIS_COUNT; i++)
		m.axes[i] = axes[i];
	return m;
}

// Raw (pre-PADClamp) trigger value for an analog position v in [0, 1] when `click` is the
// position where the digital press happens: 0 at rest, then 30..180 linearly up to the
// click (PADClamp turns that into 0..150).
u8 linearTrigger(float v, float click)
{
	if (v <= 0.5f / 255.0f)
		return 0;
	float t = click > 0.0f ? v / click : 1.0f;
	if (t > 1.0f)
		t = 1.0f;
	return (u8)std::lround(30.0f + 150.0f * t);
}

struct PortPadState {
	bool configured;
	int deadzone;
	int cdeadzone;
	int rumble;
};
PortPadState sPads[PAD_MAX_CONTROLLERS];
const char* sLastName[PAD_MAX_CONTROLLERS];

// Per controller: dead zones and rumble strength from the settings (Aurora keeps them in
// its controller record, so they are re-applied after a reconnect).
void configureController(u32 port)
{
	PortPadState& st      = sPads[port];
	const PortSettings& s = gPortSettings;
	const char* name      = PADGetName(port);
	if (name != sLastName[port]) { // SDL keeps the name string per controller
		sLastName[port] = name;
		st.configured   = false;
		if (name != nullptr) {
			u32 vid, pid;
			PADGetVidPid(port, &vid, &pid);
			std::fprintf(stderr, "[port] input: pad %u = %s (%04x:%04x, type %d%s)\n", port + 1, name, vid,
			             pid, (int)PADGetControllerType(port), PADIsGCAdapter(port) ? ", GameCube adapter" : "");
		}
	}
	if (name == nullptr || PADIsGCAdapter(port))
		return;
	if (st.configured && st.deadzone == s.stickDeadzone && st.cdeadzone == s.cstickDeadzone
	    && st.rumble == s.rumble)
		return;
	u32 count;
	PADGetButtonMappings(port, &count); // loads Aurora's saved mapping first, if any
	if (PADDeadZones* dz = PADGetDeadZones(port)) {
		dz->useDeadzones     = s.stickDeadzone > 0 || s.cstickDeadzone > 0;
		dz->stickDeadZone    = (u16)(s.stickDeadzone * 32767 / 100);
		dz->substickDeadZone = (u16)(s.cstickDeadzone * 32767 / 100);
	}
	const u16 strength = (u16)(s.rumble * 65535 / 100);
	PADSetRumbleIntensity(port, strength, strength);
	st.configured = true;
	st.deadzone   = s.stickDeadzone;
	st.cdeadzone  = s.cstickDeadzone;
	st.rumble     = s.rumble;
}

// SMS_PORT_KEYSCRIPT=<frame>:<key>[+<key>...][,...] (test knob): holds those keys (SDL key
// names, e.g. Return, Space, W, Left Shift) for 10 frames from <frame> (gPortFrameCount, as
// SMS_PORT_SHOT), whether or not the window has keyboard focus. Drives the keyboard pad
// in unattended runs without sending real key presses to the desktop.
extern "C" volatile unsigned gPortFrameCount; // port_crash.cpp
bool sScriptKeys[SDL_SCANCODE_COUNT];
bool updateKeyScript()
{
	static const char* sScript = std::getenv("SMS_PORT_KEYSCRIPT");
	if (sScript == nullptr)
		return false;
	for (bool& k : sScriptKeys)
		k = false;
	const unsigned frame = gPortFrameCount;
	bool any             = false;
	for (const char* p = sScript; *p != '\0';) {
		char* end;
		unsigned long at = std::strtoul(p, &end, 10);
		if (end == p || *end != ':')
			break;
		p = end + 1;
		while (*p != '\0' && *p != ',') {
			char name[64];
			int n = 0;
			while (*p != '\0' && *p != ',' && *p != '+') {
				if (n < 63)
					name[n++] = *p;
				++p;
			}
			name[n] = '\0';
			if (frame >= at && frame < at + 10) {
				SDL_Scancode sc = SDL_GetScancodeFromName(name);
				if (sc != SDL_SCANCODE_UNKNOWN) {
					sScriptKeys[sc] = true;
					any             = true;
				}
			}
			if (*p == '+')
				++p;
		}
		if (*p == ',')
			++p;
	}
	return any;
}

} // namespace

// SMS_PORT_KEYSCRIPT keys held this frame (port_window.cpp tests its hotkeys with it).
extern "C" bool port_input_script_key(int scancode)
{
	updateKeyScript();
	return scancode > 0 && scancode < SDL_SCANCODE_COUNT && sScriptKeys[scancode];
}

namespace {

bool keyboardFocused()
{
	SDL_Window* focus = SDL_GetKeyboardFocus();
	return focus != nullptr && focus == aurora_get_window();
}

// Keyboard pad (port 0). Returns true if any key of it is held.
bool readKeyboard(PADStatus& kb)
{
	const PortSettings& s = gPortSettings;
	const bool* keys      = SDL_GetKeyboardState(nullptr);
	auto down             = [&](int k) {
        return s.keys[k] > 0 && s.keys[k] < SDL_SCANCODE_COUNT && (keys[s.keys[k]] || sScriptKeys[s.keys[k]]);
	};
	const bool soft       = down(PORT_KEY_SOFT);
	const bool alt        = (SDL_GetModState() & SDL_KMOD_ALT) != 0; // Alt+Enter is fullscreen
	bool any              = false;

	static const struct {
		int key;
		u16 bit;
	} kButtons[] = {
		{ PORT_KEY_A, PAD_BUTTON_A },          { PORT_KEY_B, PAD_BUTTON_B },
		{ PORT_KEY_X, PAD_BUTTON_X },          { PORT_KEY_Y, PAD_BUTTON_Y },
		{ PORT_KEY_Z, PAD_TRIGGER_Z },         { PORT_KEY_START, PAD_BUTTON_START },
		{ PORT_KEY_DPAD_UP, PAD_BUTTON_UP },   { PORT_KEY_DPAD_DOWN, PAD_BUTTON_DOWN },
		{ PORT_KEY_DPAD_LEFT, PAD_BUTTON_LEFT }, { PORT_KEY_DPAD_RIGHT, PAD_BUTTON_RIGHT },
	};
	for (const auto& b : kButtons)
		if (down(b.key) && !(b.key == PORT_KEY_START && alt)) {
			kb.button |= b.bit;
			any = true;
		}
	if (down(PORT_KEY_A))
		kb.analogA = 0xFF;
	if (down(PORT_KEY_B))
		kb.analogB = 0xFF;

	// Triggers: full press = click + full analog; with the soft key a light press
	// (analog only, half way), e.g. FLUDD spraying while walking.
	if (down(PORT_KEY_L)) {
		any = true;
		if (soft)
			kb.triggerLeft = 105;
		else {
			kb.button |= PAD_TRIGGER_L;
			kb.triggerLeft = 180;
		}
	}
	if (down(PORT_KEY_R)) {
		any = true;
		if (soft)
			kb.triggerRight = 105;
		else {
			kb.button |= PAD_TRIGGER_R;
			kb.triggerRight = 180;
		}
	}

	// Sticks: +-127 is a full tilt (PADClamp scales it to the GameCube range); diagonals
	// keep the same length.
	auto stick = [&](int up, int dn, int lf, int rt, float scale, s8& ox, s8& oy) {
		int x = (down(rt) ? 1 : 0) - (down(lf) ? 1 : 0);
		int y = (down(up) ? 1 : 0) - (down(dn) ? 1 : 0);
		if (x == 0 && y == 0)
			return;
		any       = true;
		float len = (x != 0 && y != 0) ? 0.70710678f : 1.0f;
		ox        = (s8)std::lround(127.0f * scale * len * x);
		oy        = (s8)std::lround(127.0f * scale * len * y);
	};
	stick(PORT_KEY_STICK_UP, PORT_KEY_STICK_DOWN, PORT_KEY_STICK_LEFT, PORT_KEY_STICK_RIGHT,
	      soft ? s.softTilt / 100.0f : 1.0f, kb.stickX, kb.stickY);
	stick(PORT_KEY_CSTICK_UP, PORT_KEY_CSTICK_DOWN, PORT_KEY_CSTICK_LEFT, PORT_KEY_CSTICK_RIGHT, 1.0f, kb.substickX,
	      kb.substickY);
	return any;
}

} // namespace

extern "C" void port_mouse_pad(PADStatus* p); // port_mouse.cpp

extern "C" {

void port_input_configure(void)
{
	if (!gPortSettings.gcLayout)
		return;
	// Must run before PADInit (the game calls it from JUTGamePad::init).
	const PADDefaultMapping m = gcMapping();
	PADSetDefaultMapping(&m, PAD_TYPE_PS3);
	PADSetDefaultMapping(&m, PAD_TYPE_PS4);
	PADSetDefaultMapping(&m, PAD_TYPE_PS5);
	PADSetDefaultMapping(&m, PAD_TYPE_STANDARD);
	PADSetDefaultMapping(&m, PAD_TYPE_XBOX360); // Aurora's Xbox defaults are already this layout
	PADSetDefaultMapping(&m, PAD_TYPE_XBOXONE);
}

void port_movetest_pads(PADStatus* status); // port_movetest.cpp

u32 port_input_read(PADStatus* status)
{
	port_vpad_update(); // SMS_PORT_VPAD test controller (port_padfx.cpp)
	const u32 ret         = PADRead(status);
	const PortSettings& s = gPortSettings;

	for (u32 port = 0; port < PAD_MAX_CONTROLLERS; port++) {
		configureController(port);
		PADStatus& st = status[port];
		if (st.err != PAD_ERR_NONE || !s.linearTriggers || PADGetName(port) == nullptr || PADIsGCAdapter(port))
			continue;
		// Aurora: trigger = round(v * 255), or 180 while the digital (emulated click) bit is set.
		const PADDeadZones* dz = PADGetDeadZones(port);
		if (dz == nullptr || !dz->emulateTriggers)
			continue;
		const float clickL = (dz->leftTriggerActivationZone + 1) / 32767.0f;
		const float clickR = (dz->rightTriggerActivationZone + 1) / 32767.0f;
		if (!(st.button & PAD_TRIGGER_L))
			st.triggerLeft = linearTrigger(st.triggerLeft / 255.0f, clickL);
		if (!(st.button & PAD_TRIGGER_R))
			st.triggerRight = linearTrigger(st.triggerRight / 255.0f, clickR);
	}

	if (s.keyboard && !port_replay_active()) { // a replay ignores the keyboard
		PADStatus kb       = {};
		const bool scripted = updateKeyScript();
		if ((keyboardFocused() || scripted) && readKeyboard(kb)) {
			PADStatus& p0 = status[0];
			if (p0.err != PAD_ERR_NONE) {
				p0     = kb;
				p0.err = PAD_ERR_NONE;
			} else {
				p0.button |= kb.button;
				if (kb.stickX != 0 || kb.stickY != 0) {
					p0.stickX = kb.stickX;
					p0.stickY = kb.stickY;
				}
				if (kb.substickX != 0 || kb.substickY != 0) {
					p0.substickX = kb.substickX;
					p0.substickY = kb.substickY;
				}
				if (kb.triggerLeft > p0.triggerLeft)
					p0.triggerLeft = kb.triggerLeft;
				if (kb.triggerRight > p0.triggerRight)
					p0.triggerRight = kb.triggerRight;
				if (kb.analogA > p0.analogA)
					p0.analogA = kb.analogA;
				if (kb.analogB > p0.analogB)
					p0.analogB = kb.analogB;
			}
		} else if (status[0].err == PAD_ERR_NO_CONTROLLER) {
			// No controller: the keyboard (and mouse) is controller 1 (idle).
			status[0]     = PADStatus{};
			status[0].err = PAD_ERR_NONE;
		}
		port_mouse_pad(&status[0]); // the mouse: camera, R, L, Y (port_mouse.cpp)
	}

	// Settings menu (port_menu.cpp): the Back/Create/View button opens/closes it, and while it
	// is open the game sees idle controllers (they stay connected).
	static bool sBackHeld;
	bool back = false;
	for (u32 port = 0; port < PAD_MAX_CONTROLLERS; port++)
		if (status[port].err == PAD_ERR_NONE && (status[port].extButton & PAD_BUTTON_BACK) != 0)
			back = true;
	if (back && !sBackHeld)
		port_menu_toggle();
	sBackHeld = back;
	if (port_menu_is_open() || port_mp_menu_wants_input()) // w10: multiplayer menu too
		for (u32 port = 0; port < PAD_MAX_CONTROLLERS; port++)
			if (status[port].err == PAD_ERR_NONE) {
				status[port]     = PADStatus{};
				status[port].err = PAD_ERR_NONE;
			}

	// Quality-of-life state for this game frame (port_qol.cpp; recorded/replayed before the pads).
	port_qol_frame();

	// Input recording / replay (port_replay.cpp): records what the game gets here, or
	// replaces it with the recording.
	port_replay_pads(status);
	port_movetest_pads(status); // movement tests (port_movetest.cpp; test knobs only)

	// SMS_PORT_PADLOG=1: log controller 1 whenever it changes (before the game's PADClamp,
	// and what PADClamp makes of it).
	static const bool sLog = std::getenv("SMS_PORT_PADLOG") != nullptr;
	static PADStatus sLast;
	if (sLog && std::memcmp(&sLast, &status[0], sizeof(PADStatus)) != 0) {
		sLast            = status[0];
		PADStatus c[4]   = { status[0], {}, {}, {} };
		c[1].err = c[2].err = c[3].err = PAD_ERR_NO_CONTROLLER;
		PADClamp(c);
		std::fprintf(stderr,
		             "[port] pad1 frame %u: err %d buttons %04x stick %d,%d cstick %d,%d L %u R %u"
		             "  -> clamped stick %d,%d cstick %d,%d L %u R %u\n",
		             gPortFrameCount, status[0].err, status[0].button, status[0].stickX, status[0].stickY,
		             status[0].substickX, status[0].substickY, status[0].triggerLeft, status[0].triggerRight,
		             c[0].stickX, c[0].stickY, c[0].substickX, c[0].substickY, c[0].triggerLeft, c[0].triggerRight);
	}

	// DualSense adaptive triggers and lightbar from the game state (port_padfx.cpp).
	port_padfx_frame();
	port_speaker_frame();  // controller speaker: which sounds (port_speaker.cpp)
	port_haptics_frame();  // HD rumble / DualSense haptics (port_haptics.cpp)
	return ret;
}

} // extern "C"

#endif // SMS_PORT_AURORA
