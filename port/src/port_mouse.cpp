// Mouse controls (with the keyboard pad, controller 1), like sm64coopdx:
//   move        = turns the camera directly (CameraNormal.cpp takes the movement each frame:
//                 yaw by the distance moved, pitch as the camera's height ratio)
//   left click  = light R: spray while walking
//   right click = L (camera behind Mario)
//   middle      = full R: stand and spray at full power, side button = Y, wheel = camera up / down
// In a level with the window focused and no menu open the mouse is captured (hidden); Esc,
// any menu (settings, multiplayer, chat), the title / file select, or leaving the window frees
// it. port_settings.ini [controller]: mouse, mouse_sensitivity, mouse_invert_y.
//
// port_mouse_frame runs on the main thread (SDL) once per presented frame and gathers the
// movement; port_mouse_pad (port_input_read) and port_mouse_take_camera (the camera) run on
// the game thread, so only atomics cross the threads.
#ifdef SMS_PORT_AURORA

#include "port_settings.h"
#include "port_replay.h"
#include "port_mp.h"

#include <aurora/aurora.h>
#include <dolphin/pad.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#define IMGUI_USER_CONFIG "aurora/imgui_config.h"
#include <imgui.h>

#include <System/Application.hpp>
#include <System/MarDirector.hpp>

#include <atomic>
#include <cmath>

extern "C" bool port_menu_is_open(void); // port_menu.cpp (the settings menu)

namespace {

// The free camera's zoom levels, in percent of the game's distance (sm64coopdx-like: a few
// gentle steps, never far away).
const int kZoomLevels[] = { 75, 100, 125, 150 };
const int kZoomLevelCount = (int)(sizeof(kZoomLevels) / sizeof(kZoomLevels[0]));

// The level `dir` steps (-1 closer, +1 further) from `zoom` (snapped to the nearest level first).
int freeCamZoomStep(int zoom, int dir)
{
	int best = 0;
	for (int i = 1; i < kZoomLevelCount; i++)
		if (std::abs(kZoomLevels[i] - zoom) < std::abs(kZoomLevels[best] - zoom))
			best = i;
	best += dir;
	best = best < 0 ? 0 : (best >= kZoomLevelCount ? kZoomLevelCount - 1 : best);
	return kZoomLevels[best];
}

std::atomic<bool> sCaptured{ false };
std::atomic<int> sDX{ 0 }, sDY{ 0 }; // accumulated movement, 1/100 pixel
std::atomic<int> sWheel{ 0 };
std::atomic<unsigned> sButtons{ 0 };

// In a level (not the title, the file select or a movie): the mouse may turn the camera.
bool inLevel()
{
	if (gpMarDirector == nullptr)
		return false;
	const int stage = SMSGetApplication()->mCurrArea.getStage();
	return stage != 15;
}

void release(SDL_Window* w)
{
	if (sCaptured.exchange(false))
		SDL_SetWindowRelativeMouseMode(w, false);
	sButtons = 0;
	sDX      = 0;
	sDY      = 0;
}

} // namespace

extern "C" {

// Main thread, once per presented frame (port_window.cpp), inside the ImGui frame.
void port_mouse_frame(void)
{
	SDL_Window* w = aurora_get_window();
	if (w == nullptr)
		return;
	const PortSettings& s = gPortSettings;
	float dx = 0.0f, dy = 0.0f;
	SDL_MouseButtonFlags b = SDL_GetRelativeMouseState(&dx, &dy); // also restarts the accumulation
	const bool* keys       = SDL_GetKeyboardState(nullptr);
	const bool menus = port_menu_is_open() || port_mp_menu_wants_input() || ImGui::GetIO().WantTextInput;
	const bool allow = s.mouse && s.keyboard && !port_replay_active() && SDL_GetKeyboardFocus() == w && !menus
	                   && inLevel() && !keys[SDL_SCANCODE_ESCAPE];
	if (!allow) {
		release(w);
		return;
	}
	if (!sCaptured.load()) {
		// captured as soon as the level is playable (no click needed, like sm64coopdx); a
		// button still held from a menu click does not count until it is let go
		if (b != 0)
			return;
		SDL_SetWindowRelativeMouseMode(w, true);
		sCaptured = true;
		return;
	}
	sDX += (int)(dx * 100.0f);
	sDY += (int)(dy * 100.0f);
	const float wheel = ImGui::GetIO().MouseWheel;
	if (wheel != 0.0f)
		sWheel += wheel > 0.0f ? 1 : -1;
	sButtons = b;
}

// Game thread (port_freecam.cpp): the movement since the last call in pixels (the free camera
// scales it itself); the wheel steps the free camera's zoom. Returns 0 when not captured.
int port_mouse_take_raw(float* dx, float* dy)
{
	*dx = 0.0f;
	*dy = 0.0f;
	if (!sCaptured.load())
		return 0;
	*dx = sDX.exchange(0) / 100.0f;
	*dy = sDY.exchange(0) / 100.0f;
	const int notches = sWheel.exchange(0);
	if (notches != 0 && gPortSettings.qolFreeCamera) {
		gPortSettings.qolFreeCamZoom = freeCamZoomStep(gPortSettings.qolFreeCamZoom, notches > 0 ? -1 : 1);
		port_settings_dirty();
	}
	return 1;
}

// Game thread (the camera, CameraNormal.cpp): the movement since the last call, as a camera
// turn. Returns 0 when the mouse is not captured.
int port_mouse_take_camera(float* yaw, float* pitch)
{
	*yaw   = 0.0f;
	*pitch = 0.0f;
	if (!sCaptured.load())
		return 0;
	const PortSettings& s = gPortSettings;
	const float px = sDX.exchange(0) / 100.0f, py = sDY.exchange(0) / 100.0f;
	const int notches = sWheel.exchange(0);
	// sensitivity 40: one pixel turns about 0.15 degrees (65536 units a turn)
	const float k = s.mouseSensitivity * 0.7f;
	*yaw          = -px * k * (s.qolCamInvertX ? -1.0f : 1.0f);
	float wheelPitch = notches * -0.06f;
	if (s.qolFreeCamera && notches != 0) { // free camera: the wheel steps through the zoom levels
		gPortSettings.qolFreeCamZoom = freeCamZoomStep(gPortSettings.qolFreeCamZoom, notches > 0 ? -1 : 1);
		port_settings_dirty();
		wheelPitch = 0.0f;
	}
	*pitch        = (py * s.mouseSensitivity * 0.00012f + wheelPitch) * (s.mouseInvertY ? -1.0f : 1.0f);
	return 1;
}

// Game thread (port_input_read): the mouse buttons' part of controller 1.
void port_mouse_pad(PADStatus* p)
{
	if (!sCaptured.load() || p == nullptr)
		return;
	const unsigned b = sButtons.load();
	if (b & SDL_BUTTON_MMASK) { // full R: stand still, full-power spray
		p->button |= PAD_TRIGGER_R;
		if (p->triggerRight < 180)
			p->triggerRight = 180;
	} else if (b & SDL_BUTTON_LMASK) { // light R (analog only): spray while walking
		if (p->triggerRight < 105)
			p->triggerRight = 105;
	}
	if (b & SDL_BUTTON_RMASK) { // the L trigger
		p->button |= PAD_TRIGGER_L;
		if (p->triggerLeft < 180)
			p->triggerLeft = 180;
	}
	if (b & (SDL_BUTTON_X1MASK | SDL_BUTTON_X2MASK))
		p->button |= PAD_BUTTON_Y;
}

int port_mouse_captured(void) { return sCaptured.load() ? 1 : 0; }

// CameraNormal.cpp: the Free camera setting (no auto turn behind Mario).
int port_free_camera(void) { return gPortSettings.qolFreeCamera ? 1 : 0; }
// cameragc.cpp: the free camera's distance factor (1 when off), easing towards the chosen
// zoom level so a wheel notch glides instead of jumping (like sm64coopdx's free camera).
float port_free_camera_zoom(void)
{
	static float sZoom = 1.0f;
	const float want   = gPortSettings.qolFreeCamera ? gPortSettings.qolFreeCamZoom / 100.0f : 1.0f;
	sZoom += (want - sZoom) * 0.18f;
	if (std::fabs(want - sZoom) < 0.002f)
		sZoom = want;
	return sZoom;
}

} // extern "C"

#endif // SMS_PORT_AURORA
