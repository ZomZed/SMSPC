// Window, letterboxing, internal resolution, hotkeys, frame statistics and the F3
// overlay for the Aurora build. See port_settings.h. (w9)
//
// Aspect ([window] aspect):
// - 4:3 (default): the windowed client area is kept at 4:3 (SDL aspect lock), and the
//   640x448 picture is scaled to fill it, the 4:3 picture a TV shows (the VI scales the
//   448 lines and 640 pixels to most of the 4:3 screen). When the client area is not
//   4:3 (fullscreen on a 16:9 display, maximized), Aurora's FIT policy letterboxes
//   instead. FIT keeps the EFB's own 640:448 shape (about 7% wider than 4:3) because
//   Aurora has no way to set a display aspect (VILockAspectRatio is declared, not
//   implemented); port/patches/aurora_display_aspect.patch implements it. Once Aurora has
//   it (detected at link time, see VILockAspectRatio below), 4:3 mode locks the aspect
//   and always uses FIT: exactly 4:3 in every window shape and in fullscreen.
// - fit: always FIT. stretch: Aurora STRETCH, the picture fills the window.
// Internal resolution: VISetFrameBufferScale ([window] internal_scale, 0 = window size).
//
// Hotkeys (SDL event watch, handled on the next presented frame): Alt+Enter and F11
// toggle fullscreen (borderless desktop), F3 toggles the overlay, F9 saves a replay
// (port_replay.cpp), F1 the settings menu
// (port_menu.cpp).
// Window size/position/maximized/fullscreen are saved to port_settings.ini a second
// after they change and at exit.
#ifdef SMS_PORT_AURORA

#include "port_settings.h"
#include "port_replay.h"
#include "port_bugreport.h" // w12: F9 saves a bug report (video + log + replay)
#include "port_discord.h"
#include "port_mp.h" // w10: port_mp_menu_draw

#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/vi.h>
#include <aurora/gfx.h> // w6: aurora_enable_vsync
#include <aurora/math.hpp>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace aurora::vi {
// Same configured dimensions used by Aurora when the display aspect is unlocked.
Vec2<uint32_t> configured_fb_size() noexcept;
}

extern "C" volatile unsigned gPortFrameCount; // port_crash.cpp

// VILockAspectRatio: declared by Aurora's vi.h, defined only with
// port/patches/aurora_display_aspect.patch applied. Until then the linker resolves it to
// this fallback (COFF weak external), which records that it is missing.
static bool sAspectLockMissing;
extern "C" void port_VILockAspectRatio_missing(int, int) { sAspectLockMissing = true; }
#pragma comment(linker, "/alternatename:VILockAspectRatio=port_VILockAspectRatio_missing")
extern "C" void port_VIUnlockAspectRatio_missing(void) {}
#pragma comment(linker, "/alternatename:VIUnlockAspectRatio=port_VIUnlockAspectRatio_missing")

namespace {

volatile LONG sToggleFullscreen;
volatile LONG sToggleOverlay;
volatile LONG sToggleMenu;
volatile LONG sEscMenu; // Esc: the settings menu, unless another overlay takes the key
volatile LONG sSaveReplay; // F9 (port_replay.cpp)
volatile LONG sGeometryChanged;
bool sInit;
bool sOffscreen; // SMS_PORT_OFFSCREEN / SMS_PORT_NULLGFX test runs: never move or save the window
int sPolicy = -1;
bool sDirty;
unsigned long long sDirtySince;

// ---- frame statistics ------------------------------------------------------
LARGE_INTEGER sQpcFreq;
long long now()
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return c.QuadPart;
}
double ms(long long ticks) { return ticks * 1000.0 / (double)sQpcFreq.QuadPart; }

struct Window {
	long long start = 0;
	u32 retraceStart = 0;
	long long lastRetrace = 0;
	long long lastPresent = 0;
	int retraces = 0;
	int presents = 0;
	double retraceMax = 0, presentMax = 0, presentSum = 0, presentSumSq = 0;
	int lateRetraces = 0; // main-thread retrace gaps > 25 ms (a missed VI field)
	unsigned logicStart = 0; // port_interp_logic_frames() at the window start
	void reset(long long t, u32 rc)
	{
		*this       = Window();
		start       = t;
		retraceStart = rc;
		lastRetrace = t;
		lastPresent = t;
	}
};
Window sStats;   // 10 s window for the log
Window sOverlay; // 0.5 s window for the overlay
char sOverlayText[192];

void statFrame(Window& w, long long t, bool presented, bool retrace)
{
	if (retrace) {
		const double r = ms(t - w.lastRetrace);
		w.lastRetrace  = t;
		w.retraces++;
		if (r > w.retraceMax)
			w.retraceMax = r;
		if (r > 25.0)
			w.lateRetraces++;
	}
	if (presented) {
		const double p = ms(t - w.lastPresent);
		w.lastPresent  = t;
		w.presents++;
		w.presentSum += p;
		w.presentSumSq += p * p;
		if (p > w.presentMax)
			w.presentMax = p;
	}
}

void updateStats(bool presented, bool retrace)
{
	const long long t = now();
	const u32 rc      = VIGetRetraceCount();
	if (sStats.start == 0) {
		sStats.reset(t, rc);
		sOverlay.reset(t, rc);
		return;
	}
	statFrame(sStats, t, presented, retrace);
	statFrame(sOverlay, t, presented, retrace);
	const unsigned logic = port_interp_logic_frames();

	const double overlayMs = ms(t - sOverlay.start);
	if (overlayMs >= 500.0) {
		const double secs = overlayMs / 1000.0;
		const double fps  = sOverlay.presents / secs;
		const double hz   = (rc - sOverlay.retraceStart) / secs;
		const double avg  = sOverlay.presents > 0 ? sOverlay.presentSum / sOverlay.presents : 0.0;
		// display frames, frame time, game (logic) frames per second, VI rate
		std::snprintf(sOverlayText, sizeof(sOverlayText), "%.1f fps  %.1f ms (max %.1f)  logic %.1f Hz  VI %.2f Hz",
		              fps, avg, sOverlay.presentMax, (logic - sOverlay.logicStart) / secs, hz);
		sOverlay.reset(t, rc);
		sOverlay.logicStart = logic;
	}

	// SMS_PORT_FRAMESTATS=<seconds> sets the interval (default 10 s).
	static const double sInterval = [] {
		const char* e = std::getenv("SMS_PORT_FRAMESTATS");
		const double v = e != nullptr ? std::atof(e) : 0.0;
		return v >= 1.0 ? v * 1000.0 : 10000.0;
	}();
	const double statsMs = ms(t - sStats.start);
	if (gPortSettings.frameStats && statsMs >= sInterval) {
		const double secs = statsMs / 1000.0;
		const int n       = sStats.presents;
		const double avg  = n > 0 ? sStats.presentSum / n : 0.0;
		const double sd   = n > 1 ? std::sqrt(std::fmax(0.0, sStats.presentSumSq / n - avg * avg)) : 0.0;
		std::fprintf(stderr,
		             "[port] frames: VI %.3f Hz (%u retraces), main-thread retrace gap max %.2f ms (%d > 25 ms), "
		             "presents %.2f/s, present interval %.2f ms (sd %.2f, max %.2f), logic frames %.2f/s\n",
		             (rc - sStats.retraceStart) / secs, rc - sStats.retraceStart, sStats.retraceMax,
		             sStats.lateRetraces, n / secs, avg, sd, sStats.presentMax, (logic - sStats.logicStart) / secs);
		sStats.reset(t, rc);
		sStats.logicStart = logic;
	}
}

// ---- window -----------------------------------------------------------------
bool SDLCALL eventWatch(void*, SDL_Event* e)
{
	switch (e->type) {
	case SDL_EVENT_KEY_DOWN:
		if (e->key.repeat)
			break;
		if (e->key.scancode == SDL_SCANCODE_F11
		    || ((e->key.scancode == SDL_SCANCODE_RETURN || e->key.scancode == SDL_SCANCODE_KP_ENTER)
		        && (e->key.mod & SDL_KMOD_ALT) != 0))
			InterlockedExchange(&sToggleFullscreen, 1);
		else if (e->key.scancode == SDL_SCANCODE_F3)
			InterlockedExchange(&sToggleOverlay, 1);
		else if (e->key.scancode == SDL_SCANCODE_F9)
			InterlockedExchange(&sSaveReplay, 1);
		else if (e->key.scancode == SDL_SCANCODE_F1) // the old key, still works
			InterlockedExchange(&sToggleMenu, 1);
		else if (e->key.scancode == SDL_SCANCODE_ESCAPE)
			InterlockedExchange(&sEscMenu, 1);
		break;
	case SDL_EVENT_WINDOW_MOVED:
	case SDL_EVENT_WINDOW_RESIZED:
	case SDL_EVENT_WINDOW_MAXIMIZED:
	case SDL_EVENT_WINDOW_RESTORED:
	case SDL_EVENT_WINDOW_ENTER_FULLSCREEN:
	case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN:
		InterlockedExchange(&sGeometryChanged, 1);
		break;
	default:
		break;
	}
	return true;
}

// w6: the fixed display aspects (4:3 and the widescreen modes) as numerator/denominator.
bool fixedAspect(int aspect, int& num, int& den)
{
	switch (aspect) {
	case PORT_ASPECT_4_3: num = 4, den = 3; return true;
	case PORT_ASPECT_16_9: num = 16, den = 9; return true;
	case PORT_ASPECT_16_10: num = 16, den = 10; return true;
	case PORT_ASPECT_21_9: num = 64, den = 27; return true; // "21:9" displays are 64:27 (2560x1080)
	case PORT_ASPECT_32_9: num = 32, den = 9; return true;
	default: return false;
	}
}

volatile float sAspectScale = 1.0f;  // w6: port_get_aspect_scale()
volatile float sDisplayAspect = 4.0f / 3.0f;

// w6: widescreen factor for the current setting and client area.
void updateAspectScale(SDL_Window* w)
{
	int num, den;
	float display = 4.0f / 3.0f;
	if (fixedAspect(gPortSettings.aspect, num, den))
		display = (float)num / (float)den;
	else if (gPortSettings.aspect == PORT_ASPECT_WINDOW) {
		int pw = 0, ph = 0;
		if (w != nullptr && SDL_GetWindowSizeInPixels(w, &pw, &ph) && pw > 0 && ph > 0)
			display = (float)pw / (float)ph;
		else
			return; // minimized: keep the last value
	} else if (gPortSettings.aspect == PORT_ASPECT_STRETCH)
		display = 0.0f;
	const float scale = display > 4.0f / 3.0f ? display / (4.0f / 3.0f) : 1.0f;
	if (scale != sAspectScale || display != sDisplayAspect) {
		sAspectScale   = scale;
		sDisplayAspect = display;
		std::fprintf(stderr, "[port] window: display aspect %.4f, 3D view widened x%.4f\n", display, scale);
	}
}

void applyAspectLock(SDL_Window* w)
{
	int num, den; // w6: every fixed aspect locks the windowed client area (was 4:3 only)
	// w6 test knob: SMS_PORT_NO_ASPECT_LOCK=1 keeps the configured window shape, so a fixed
	// aspect is shown with bars as in fullscreen/maximized (checked off-screen with
	// SMS_PORT_SHOT_PRESENT without covering the desktop).
	static const bool sNoLock = std::getenv("SMS_PORT_NO_ASPECT_LOCK") != nullptr;
	if (!sNoLock && fixedAspect(gPortSettings.aspect, num, den))
		SDL_SetWindowAspectRatio(w, (float)num / (float)den, (float)num / (float)den);
	else
		SDL_SetWindowAspectRatio(w, 0.0f, 0.0f);
}

// Aspect lock and internal resolution from the settings (at start and when the menu changes them).
void applyDisplay(SDL_Window* w)
{
	const PortSettings& s = gPortSettings;
	applyAspectLock(w);
	int num, den;
	if (fixedAspect(s.aspect, num, den)) // w6: widescreen modes lock their aspect too
		VILockAspectRatio(num, den);
	else
		VIUnlockAspectRatio();
	updateAspectScale(w); // w6
	VISetFrameBufferScale(s.internalScale);
	aurora_set_texture_filtering((uint16_t)s.anisotropy, s.trilinear); // w6: [graphics] (0/false = GX)
	aurora_set_post_process(s.gamma / 100.0f, s.brightness / 100.0f, s.fxaa); // w6
	aurora_set_copy_filter_enabled(s.copyFilter);                            // w6
	static int sVsync = -1;                                                  // w6
	if (sVsync != (int)s.vsync) {
		if (sVsync != -1) // Aurora starts with config.vsync (port_settings_configure)
			aurora_enable_vsync(s.vsync);
		sVsync = s.vsync;
	}
	sPolicy = -1;
}

void init(SDL_Window* w)
{
	sInit = true;
	QueryPerformanceFrequency(&sQpcFreq);
	sOffscreen = std::getenv("SMS_PORT_OFFSCREEN") != nullptr || std::getenv("SMS_PORT_NULLGFX") != nullptr;
	const PortSettings& s = gPortSettings;
	SDL_AddEventWatch(eventWatch, nullptr);
	if (!sOffscreen) {
		// Positions Aurora could not take at creation (negative: a display left of/above the main one).
		if (s.hasPos && (s.x < 0 || s.y < 0) && !s.fullscreen)
			SDL_SetWindowPosition(w, s.x, s.y);
		if (s.maximized && !s.fullscreen)
			SDL_MaximizeWindow(w);
	}
	applyDisplay(w);
	std::fprintf(stderr,
	             "[port] window: aspect %s%s, internal scale %g, fullscreen %d (Alt+Enter/F11), overlay %d (F3)\n",
	             s.aspect == PORT_ASPECT_4_3 ? "4:3" : s.aspect == PORT_ASPECT_FIT ? "fit"
	             : s.aspect == PORT_ASPECT_STRETCH                             ? "stretch"
	                                                                           : "widescreen", // w6
	             s.aspect == PORT_ASPECT_4_3 && sAspectLockMissing ? " (no Aurora aspect lock: 640:448 letterbox)" : "",
	             s.internalScale, s.fullscreen ? 1 : 0, s.overlay ? 1 : 0);
	std::fprintf(stderr,
	             "[port] graphics: msaa %d, fxaa %d, anisotropy %d%s, trilinear %d, brightness %d%%, gamma %d%%, "
	             "copy filter %d, vsync %d\n",
	             s.msaa, s.fxaa ? 1 : 0, s.anisotropy, s.anisotropy == 0 ? " (per texture)" : "", s.trilinear ? 1 : 0,
	             s.brightness, s.gamma, s.copyFilter ? 1 : 0, s.vsync ? 1 : 0); // w6
}

// Viewport policy for the current client area (see the top of the file).
void updatePolicy(SDL_Window* w)
{
	int policy;
	switch (gPortSettings.aspect) {
	case PORT_ASPECT_FIT:
		policy = AURORA_VIEWPORT_FIT;
		break;
	case PORT_ASPECT_STRETCH:
	case PORT_ASPECT_WINDOW: // w6: the window's shape; the game widens the view to match
		policy = AURORA_VIEWPORT_STRETCH;
		updateAspectScale(w);
		break;
	default: {
		if (!sAspectLockMissing) { // Aurora letterboxes to the locked 4:3 itself
			policy = AURORA_VIEWPORT_FIT;
			break;
		}
		int pw = 0, ph = 0;
		SDL_GetWindowSizeInPixels(w, &pw, &ph);
		if (pw <= 0 || ph <= 0)
			return; // minimized
		const float a = (float)pw / (float)ph;
		policy        = std::fabs(a - 4.0f / 3.0f) < 0.01f ? AURORA_VIEWPORT_STRETCH : AURORA_VIEWPORT_FIT;
		break;
	}
	}
	if (policy != sPolicy) {
		sPolicy = policy;
		AuroraSetViewportPolicy((AuroraViewportPolicy)policy);
	}
}

void trackGeometry(SDL_Window* w)
{
	if (sOffscreen)
		return;
	PortSettings& s           = gPortSettings;
	const SDL_WindowFlags f   = SDL_GetWindowFlags(w);
	const bool fullscreen     = (f & SDL_WINDOW_FULLSCREEN) != 0;
	const bool maximized      = (f & SDL_WINDOW_MAXIMIZED) != 0;
	const bool minimized      = (f & SDL_WINDOW_MINIMIZED) != 0;
	if (minimized)
		return;
	bool changed = fullscreen != s.fullscreen || (!fullscreen && maximized != s.maximized);
	s.fullscreen = fullscreen;
	if (!fullscreen)
		s.maximized = maximized;
	if (!fullscreen && !maximized) {
		int x, y, cw, ch;
		if (SDL_GetWindowPosition(w, &x, &y) && SDL_GetWindowSize(w, &cw, &ch) && cw > 0 && ch > 0) {
			if (!s.hasPos || x != s.x || y != s.y || cw != s.width || ch != s.height)
				changed = true;
			s.hasPos = true;
			s.x      = x;
			s.y      = y;
			s.width  = cw;
			s.height = ch;
		}
	}
	if (changed)
		port_settings_dirty();
}

} // namespace

extern "C" {

void port_settings_dirty(void)
{
	if (!sDirty)
		sDirtySince = GetTickCount64();
	sDirty = true;
}

float port_get_aspect_scale(void) { return sAspectScale; }    // w6
float port_get_display_aspect(void)
{
	// Fit preserves the configured framebuffer shape, which differs from the TV's
	// 4:3 display aspect (for example, 640x448). Overlay coordinates must use the
	// same picture bounds as presentation, including after a VI mode change.
	if (gPortSettings.aspect == PORT_ASPECT_FIT) {
		const auto size = aurora::vi::configured_fb_size();
		if (size.x > 0 && size.y > 0)
			return (float)size.x / (float)size.y;
	}
	return sDisplayAspect;
}

void port_window_apply_display(void)
{
	if (SDL_Window* w = aurora_get_window())
		applyDisplay(w);
}

void port_window_set_fullscreen(bool on)
{
	if (SDL_Window* w = aurora_get_window())
		SDL_SetWindowFullscreen(w, on);
}

void port_mp_overlay_draw(void); // w15: multiplayer name tags (port_mp_overlay.cpp)

extern "C" void port_mouse_frame(void); // port_mouse.cpp

static void windowFrame(bool presented, bool retrace)
{
	SDL_Window* w = aurora_get_window();
	if (w == nullptr)
		return;
	if (!sInit)
		init(w);
	updateStats(presented, retrace);
	if (!presented)
		return; // window changes and ImGui only between Aurora frames' begin and end
	port_gfx_backend_frame(); // w6: start guard cleared after 120 presented frames

	// SMS_PORT_KEYSCRIPT can press the hotkeys too (unattended tests).
	static bool sScriptF11, sScriptF3, sScriptF1;
	const bool f11 = port_input_script_key(SDL_SCANCODE_F11), f3 = port_input_script_key(SDL_SCANCODE_F3);
	const bool f1  = port_input_script_key(SDL_SCANCODE_F1);
	static bool sScriptF9;
	const bool f9 = port_input_script_key(SDL_SCANCODE_F9);
	if (f9 && !sScriptF9)
		InterlockedExchange(&sSaveReplay, 1);
	sScriptF9 = f9;
	if (f1 && !sScriptF1)
		InterlockedExchange(&sToggleMenu, 1);
	sScriptF1 = f1;
	static bool sScriptEsc;
	const bool esc = port_input_script_key(SDL_SCANCODE_ESCAPE);
	if (esc && !sScriptEsc)
		InterlockedExchange(&sEscMenu, 1);
	sScriptEsc = esc;
	if (f11 && !sScriptF11)
		InterlockedExchange(&sToggleFullscreen, 1);
	if (f3 && !sScriptF3)
		InterlockedExchange(&sToggleOverlay, 1);
	sScriptF11 = f11;
	sScriptF3  = f3;
	port_mouse_frame(); // the mouse's camera / buttons (port_mouse.cpp)

	if (InterlockedExchange(&sToggleFullscreen, 0)) {
		const bool fs = (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) == 0;
		SDL_SetWindowFullscreen(w, fs);
		std::fprintf(stderr, "[port] window: fullscreen %s\n", fs ? "on" : "off");
	}
	if (InterlockedExchange(&sToggleOverlay, 0)) {
		gPortSettings.overlay = !gPortSettings.overlay;
		port_settings_dirty();
	}
	if (InterlockedExchange(&sGeometryChanged, 0))
		trackGeometry(w);
	if (InterlockedExchange(&sSaveReplay, 0)) { // w12: F9 = bug report (video + log + replay), else the replay alone
		if (port_bugreport_enabled())
			port_bugreport_save("f9", nullptr);
		else
			port_replay_save("F9");
	}
	port_bugreport_frame(); // w12: arms the picture capture, counts the frame rate
	if (InterlockedExchange(&sToggleMenu, 0))
		port_menu_toggle();
	// Esc opens / closes the settings menu. While a multiplayer window or the chat has the
	// keyboard, Esc is theirs (they close on it), and a key being rebound takes it as "cancel".
	if (InterlockedExchange(&sEscMenu, 0) && !port_menu_key_capture()
	    && (port_menu_is_open() || (!port_mp_menu_wants_input() && !port_menu_text_input())))
		port_menu_toggle();
	updatePolicy(w);
	port_mp_overlay_draw(); // w15: multiplayer name tags (nothing without puppets)
	port_menu_draw(gPortSettings.overlay ? sOverlayText : nullptr);
	port_mp_menu_draw(); // w10: multiplayer menus and hints (port_mp_menu.cpp)
	port_discord_frame(); // Discord Rich Presence (own thread; this only samples state, draws the ask-to-join prompt)

	if (sDirty && GetTickCount64() - sDirtySince >= 1000) {
		sDirty = false;
		port_settings_save();
	}
}

void port_window_frame(bool presented) { windowFrame(presented, true); }

void port_window_display_frame(void) { windowFrame(true, false); }

} // extern "C"

#endif // SMS_PORT_AURORA
