// PC entry point and VI frame pacing for the Aurora build (port/tools/build_aurora.py).
//
// Aurora's aurora_main.lib provides the process main(), which calls aurora_main()
// below. We initialise Aurora (window, WebGPU device, GX), open the first frame and
// then run the game's own main() (src/main.cpp, renamed to sms_game_main by the build).
//
// Frame model: a 59.94 Hz VI retrace interrupt runs on the port interrupt thread
// (port/src/sdk_os_thread.cpp). VIWaitForRetrace() sleeps the calling game thread
// until it fires (so lower-priority game threads such as the setup thread run
// meanwhile); on the main thread it then ends the Aurora frame in progress
// (submitting the GX work recorded since the last retrace and presenting it), pumps
// window/input events and begins the next frame, so GX calls are always recorded
// inside a frame.
//
// Only compiled into the Aurora build (SMS_PORT_AURORA); empty for build_port.py.
#ifdef SMS_PORT_AURORA

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/main.h> // #define main aurora_main
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "port_sdk.h" // port_irq_add_periodic, PORT_MRAM_SIZE
#include "port_profile.h" // w14: SMS_PORT_PROFILE=1 hooks (a branch each when off)
#include "port_settings.h" // w9: settings, input, window (port_settings/input/window.cpp)
#include "port_disc.h"     // w13: port_disc_early_handoff (region builds)

#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gd/GDBase.h>

extern "C" {
// Frames presented by the main thread (SMS_PORT_SHOT/PADSCRIPT/WATCH_FRAME); defined in
// port_crash.cpp.
extern volatile unsigned gPortFrameCount;
extern "C" int gPortGoto[3];      // port_crash.cpp; PADSCRIPT G<x>;<z> target
extern "C" int gPortGotoStick[2]; // stick for it, from Application.cpp
extern "C" unsigned gPortStageFrame; // port_crash.cpp; PADSCRIPT "+<frame>" origin
extern "C" int gPortStageCount;      // port_crash.cpp; PADSCRIPT "+<n>@" stage index
extern "C" unsigned gPortDemoEndFrame; // port_crash.cpp; PADSCRIPT "+d<frame>" origin
extern "C" int gPortDemoEndCount;
extern "C" volatile unsigned gPortCond; // port_crash.cpp; PADSCRIPT "?<bit>"/"!<bit>"
extern "C" int gPortRoute[16][2];       // Application.cpp; PADSCRIPT "W" route (W7-J)
extern "C" int gPortRouteN;
// w5: boss autopilot (port_bosstrace.cpp): PADSCRIPT B<d>;<deg>, FB, AIM<d>;<deg>;<gun>, PULL
extern "C" float gPortBossPos[3];
extern "C" float gPortBossRotY;
extern "C" unsigned gPortBossFrame;
extern "C" int port_boss_bot(int d, int deg, int gun, unsigned now, int* mode, int* gx, int* gz, int* stickY);
extern "C" int port_boss_pull(unsigned now, int* mode, int* gx, int* gz);
extern "C" int gPortBossWant;
extern "C" void port_boss_assist(void);
extern "C" int port_boss_throw(unsigned now, int* mode, int* gx, int* gz);
extern "C" int port_boss_turret(unsigned now, int* stickX, int* fire);
extern "C" int port_boss_rocket(unsigned now, int* mode, int* gx, int* gz);
extern "C" int port_boss_eel(unsigned now, int* gotoOn, int* gx, int* gz, int* creep, int* holdB, int* fire);
#define sFrameCount gPortFrameCount
extern const void* gPortLastDL; // port_GXCallDisplayList (SMS_PORT_GXSYNC)
extern u32 gPortLastDLSize;
}

int sms_game_main(void); // src/main.cpp (int main(void) under SMS_PORT)
int port_gx_test();      // port_gxtest.cpp (SMS_PORT_GXTEST=1)
int port_card_test();    // port_card.cpp (SMS_PORT_CARDTEST=1)
bool port_save_window_bmp(HWND hwnd, const char* path); // port_gxtest.cpp
bool port_efb_screenshot(const char* path);             // port_aurora_sdk.cpp
extern "C" unsigned port_movetest_shot_due(void);     // port_movetest.cpp
bool port_present_screenshot(const char* path);         // port_aurora_sdk.cpp
extern "C" void port_efb_end_frame(void);               // port_efb.cpp

// The crash reporter is port/src/port_crash.cpp (shared with build_port.py).

namespace {
bool sInFrame;
bool sPaused;
DWORD sMainThreadId; // the thread that owns the Aurora window/frame
volatile u32 sRetraceCount;
OSThreadQueue sRetraceQueue;
void* sCurrentFB;
void* sNextFB;
BOOL sBlack = TRUE;
VIRetraceCallback sPreCB;
VIRetraceCallback sPostCB;
// NTSC: 59.94 fields per second.
const long long kRetracePeriodNs = 16683350;
// w13: the European build's PAL 50 Hz mode switches the retrace to 50 fields per second
// (port_vi_set_retrace_ns, called by port_behaviour.cpp when the game's TV format changes).
volatile long long sRetracePeriodNs = kRetracePeriodNs;
int sViSlot = -1;

// The VI retrace interrupt (59.94 Hz, on the port interrupt thread, in interrupt
// context like the GameCube's): count, retrace callbacks, wake VIWaitForRetrace.
volatile long long sRetraceQpc; // w9: when the latest retrace fired (frame-rate option)

void viRetrace()
{
	if (sPreCB != nullptr)
		sPreCB(sRetraceCount + 1);
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	sRetraceQpc = q.QuadPart;
	++sRetraceCount;
	sCurrentFB = sNextFB;
	if (sPostCB != nullptr)
		sPostCB(sRetraceCount);
	// Device completions deferred since the last retrace (async DVD, ...), in order.
	port_retrace_run();
	port_prof_retrace(); // w14
	OSWakeupThread(&sRetraceQueue);
}

void logCallback(AuroraLogLevel level, const char* module, const char* message, unsigned int len)
{
	static const char* const kNames[] = { "DEBUG", "INFO", "WARNING", "ERROR", "FATAL" };
	FILE* out = level >= LOG_ERROR ? stderr : stdout;
	std::fprintf(out, "[aurora %s: %s] %.*s\n", kNames[level], module, (int)len, message);
	if (level == LOG_FATAL) {
		std::fflush(out);
		// SMS_PORT_NODRAW (debug): a shader-compile failure on Aurora's pipeline worker
		// parks that worker instead of aborting. Later pipelines then never compile, so
		// their draws are skipped, and the game logic keeps running.
		static const bool sNoDraw = std::getenv("SMS_PORT_NODRAW") != nullptr;
		if (sNoDraw && GetCurrentThreadId() != sMainThreadId && std::strstr(message, "WGSL") != nullptr) {
			std::fprintf(stderr, "[port] SMS_PORT_NODRAW: pipeline worker parked after a shader error\n");
			std::fflush(stderr);
			for (;;)
				Sleep(INFINITE);
		}
		port_crash_log("[aurora FATAL: %s] %.*s\n", module, (int)len, message);
		port_print_backtrace();
		if (gPortLastDL != nullptr) {
			port_crash_log("[port] display list being processed: %p, %u bytes\n", gPortLastDL, gPortLastDLSize);
			const u8* p = (const u8*)gPortLastDL;
			for (u32 i = 0; i < gPortLastDLSize && i < 0x400; i += 32) {
				char line[3 * 32 + 1];
				int n = 0;
				for (u32 j = i; j < i + 32 && j < gPortLastDLSize; ++j)
					n += std::snprintf(line + n, sizeof(line) - n, "%02x ", p[j]);
				port_crash_log("  %04x: %s\n", i, line);
			}
		}
		// Aurora's GX FIFO and pipeline workers run on their own threads: also show
		// where the game's main thread is.
		port_crash_log("[port] game main thread at the time:\n");
		port_dump_thread(sMainThreadId);
		std::abort();
	}
}

void pumpEvents()
{
	const AuroraEvent* ev = aurora_update();
	while (ev != nullptr && ev->type != AURORA_NONE) {
		switch (ev->type) {
		case AURORA_EXIT:
			std::fprintf(stderr, "[port] window closed, exiting\n");
			std::fflush(stdout);
			port_padfx_shutdown(); // w9: DualSense triggers back to no effect while SDL is up
			aurora_shutdown();
			std::exit(0);
		case AURORA_PAUSED:
			sPaused = true;
			break;
		case AURORA_UNPAUSED:
			sPaused = false;
			break;
		default:
			break;
		}
		++ev;
	}
	if (port_restart_pending()) // w13: settings menu "Restart now" (game behaviour, PAL video)
		port_restart_now();
}

// SMS_PORT_SHOT=<frame>[,<frame>...]: saves each listed frame as shot_<frame>.bmp in the
// working directory.
bool shotListed(u32 frame)
{
	static const char* sList = std::getenv("SMS_PORT_SHOT");
	if (sList == nullptr)
		return false;
	for (const char* p = sList; *p != '\0';) {
		char* end;
		unsigned long f = std::strtoul(p, &end, 10);
		if (end == p)
			break;
		if (f == frame)
			return true;
		p = *end == ',' ? end + 1 : end;
	}
	return false;
}

// Default: the EFB is read back from the GPU right before the frame is presented
// (port_aurora_sdk.cpp; works with the window covered, minimised or off-screen, and
// with several copies running).
void maybeScreenshotEfb(u32 frame)
{
	static const bool sWindow = std::getenv("SMS_PORT_SHOT_WINDOW") != nullptr;
	if (sWindow)
		return;
	char path[64];
	std::snprintf(path, sizeof(path), "shot_%u.bmp", frame);
	if (!port_efb_screenshot(path))
		std::fprintf(stderr, "[port] screenshot %s: EFB readback not possible here\n", path);
	// SMS_PORT_SHOT_PRESENT=1 (w8): also present_<frame>.bmp, the picture exactly as the
	// window shows it (window size, letterbox bars, aspect mode, post-processing, overlay).
	static const bool sPresent = std::getenv("SMS_PORT_SHOT_PRESENT") != nullptr;
	if (sPresent) {
		std::snprintf(path, sizeof(path), "present_%u.bmp", frame);
		if (!port_present_screenshot(path))
			std::fprintf(stderr, "[port] screenshot %s: present capture not possible here\n", path);
	}
}

// SMS_PORT_SHOT_WINDOW=1: the old capture of the window after presenting (PrintWindow,
// see port_gxtest.cpp; PrintWindow is black for the D3D12 swap chain, so this falls back
// to a screen copy, which needs a visible, uncovered window: other windows on top end up
// in the shot, and with --nullgfx / SMS_PORT_OFFSCREEN nothing useful is captured).
void maybeScreenshot(u32 frame)
{
	static const char* sList = std::getenv("SMS_PORT_SHOT");
	if (sList == nullptr || std::getenv("SMS_PORT_SHOT_WINDOW") == nullptr)
		return;
	for (const char* p = sList; *p != '\0';) {
		char* end;
		unsigned long f = std::strtoul(p, &end, 10);
		if (end == p)
			break;
		if (f == frame) {
			char path[64];
			std::snprintf(path, sizeof(path), "shot_%u.bmp", frame);
			// This process's own window (FindWindow by title can pick another running copy).
			HWND hwnd = nullptr;
			if (SDL_Window* w = aurora_get_window())
				hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w),
				                                    SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
			bool ok = hwnd != nullptr && port_save_window_bmp(hwnd, path);
			// The fallback copies the screen, so say when another window covers ours.
			bool covered = false;
			RECT rc;
			if (hwnd != nullptr && GetClientRect(hwnd, &rc)) {
				const POINT probe[5] = { { rc.right / 2, rc.bottom / 2 }, { 4, 4 }, { rc.right - 5, 4 },
					                     { 4, rc.bottom - 5 }, { rc.right - 5, rc.bottom - 5 } };
				for (POINT p : probe) {
					ClientToScreen(hwnd, &p);
					if (GetAncestor(WindowFromPoint(p), GA_ROOT) != hwnd)
						covered = true;
				}
			}
			std::fprintf(stderr, "[port] screenshot %s: %s%s\n", path, ok ? "saved" : "FAILED",
			             covered ? " (window partly covered or off-screen: the shot may show other windows)" : "");
			return;
		}
		p = *end == ',' ? end + 1 : end;
	}
}

void beginFrame()
{
	for (;;) {
		pumpEvents();
		if (!sPaused && aurora_begin_frame())
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	sInFrame = true;
}
} // namespace

extern "C" {

// w13: retrace rate for the game's TV format (port_behaviour.cpp; PAL 50 Hz: 20 ms).
void port_vi_set_retrace_ns(long long period_ns)
{
	sRetracePeriodNs = period_ns;
	if (sViSlot >= 0)
		port_irq_set_period(sViSlot, period_ns);
}

// Sleeps the calling game thread until the next retrace (other game threads run
// meanwhile, as on the GameCube). On the main thread the Aurora frame recorded so far
// is then submitted and presented, and the next one begun, so GX calls always land
// inside a frame. Other threads (JKRDvdRipper waits for retraces while loading) only wait.
//
// Presenting (w9): Aurora presents its EFB, and an Aurora frame with no drawing in it
// presents a cleared (black) EFB. The game draws at 30 Hz and waits two retraces per
// frame, so presenting on every retrace alternated picture/black frames. Like the VI,
// which keeps scanning out the last XFB until the game copies a new one, a frame is now
// presented only once the game has copied one with GXCopyDisp (port_GXCopyDisp below)
// since the last present; otherwise the previous image stays on screen and only window
// events are pumped. Retrace timing is unchanged (the interrupt keeps counting).
// Until the first GXCopyDisp (and in the SMS_PORT_GXTEST smoke test, which calls
// Aurora directly) every retrace presents, as before.
static volatile bool sCopyDispSeen;
static volatile bool sCopyDispPending;
static volatile bool sFlushPending; // w14: VIFlush since the last retrace wait (port_VIFlush)
static u32 sRetracesSincePresent;

// w14: the main thread asleep until a retrace or the frame-rate alarm (SMS_PORT_PROFILE
// "viwait"; display frames drawn during a wait are not counted as asleep).
static void sleepForRetrace()
{
	PORT_PROF_BEGIN(t0);
	OSSleepThread(&sRetraceQueue);
	PORT_PROF_END(PP_VIWAIT, t0);
}

// w9: frame-rate option (port_interp.cpp). Extra display frames between the logic frames
// are drawn by the main thread while it waits for a retrace, when one is due and fits
// before the retrace, and right after a retrace that has no logic frame to present.
static OSAlarm sInterpAlarm;
static void interpAlarm(OSAlarm*, OSContext*) { OSWakeupThread(&sRetraceQueue); }

static long long qpcNow()
{
	LARGE_INTEGER q;
	QueryPerformanceCounter(&q);
	return q.QuadPart;
}

static long long qpcFromMs(double ms)
{
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	return (long long)(ms * f.QuadPart / 1000.0);
}

// SMS_PORT_SHOT_DISPLAY=<frame>:<count>: from frame <frame> (numbered as in SMS_PORT_SHOT)
// on, saves the next <count> presented frames as disp_<n>_L.bmp (logic frame picture) or
// disp_<n>_I.bmp (extra display frame).
// "+<frame>:<count>" (w9): <frame> counted from when the stage gave Mario control (as the
// PADSCRIPT "+" entries), so captures line up with scripted moves across runs.
static int sDispShots;
static void dispShotStart(u32 frame)
{
	static const char* sEnv = std::getenv("SMS_PORT_SHOT_DISPLAY");
	if (sEnv == nullptr)
		return;
	const bool rel = sEnv[0] == '+';
	const u32 at   = (u32)std::atoi(rel ? sEnv + 1 : sEnv);
	if (rel ? (gPortStageFrame != 0 && gPortStageFrame + at == frame) : at == frame) {
		const char* c = std::strchr(sEnv, ':');
		sDispShots    = c != nullptr ? std::atoi(c + 1) : 10;
	}
}
static void dispShot(bool logic)
{
	static u32 sIdx;
	if (sDispShots <= 0)
		return;
	--sDispShots;
	char path[64];
	std::snprintf(path, sizeof(path), "disp_%03u_%c.bmp", sIdx++, logic ? 'L' : 'I');
	port_efb_screenshot(path);
}

// w14 (brought in by w9): present pacing for the frame-rate option (Aurora patch
// aurora_present_pacing.patch). A display frame is started port_interp_lead() (its recent
// maximum cost plus a margin) before its slot; presented as soon as it was drawn, it landed
// 1-10 ms early, and the logic frames (presented when the main thread wakes at the retrace) a
// variable time late, so the interval sd was ~3.5 ms at 60. Now every frame carries the QPC
// time of its slot as Aurora's present tag (logic frames: their retrace; display frames: their
// due time), and Aurora's render worker holds the Present until slot + delay
// (presentNotBefore). Only the presentation moves, nothing the game computes (replays are
// unaffected). Only while display frames are drawn (port_interp_active); at the original
// 30 fps and at an unlimited rate frames present as before. SMS_PORT_INTERP_NOHOLD=1: no tags.
// The delay adapts (render worker): the 90th percentile of the last 128 frames' time from slot
// to reaching the Present + 0.5 ms, between 0.5 ms and min(8 ms, 0.8 display periods); a frame
// later than that is presented at once. SMS_PORT_PRESENT_DELAY_MS=<ms> fixes it.
static void setPresentSlot(long long slot)
{
	static const bool sNoHold = std::getenv("SMS_PORT_INTERP_NOHOLD") != nullptr;
	if (!sNoHold && slot > 0 && port_interp_effective_fps() > 0.0)
		aurora_set_present_tag(slot);
}

// SMS_PORT_PRESENT_LOG=1: a line per Present (time, slot, arrival and present against it).
static const bool sPresentLog = std::getenv("SMS_PORT_PRESENT_LOG") != nullptr;
static int64_t sPresentTag;         // render worker only: the frame's slot (0 = untagged)
static long long sPresentArrive;    // render worker only: when the frame reached presentNotBefore
static long long sPresentStart;     // render worker only: when its Present was issued
static double sPresentNeedMs  = 2.0; // render worker only: 90th percentile of arrival - slot
static double sPresentDelayMs = 2.4;

// Aurora render worker, right before a Present: waits until the frame's slot + delay (QPC).
static void presentNotBefore(int64_t slot)
{
	sPresentTag    = slot;
	sPresentArrive = qpcNow();
	if (slot <= 0)
		return;
	static const double sFixed = std::getenv("SMS_PORT_PRESENT_DELAY_MS") != nullptr
	                                 ? std::atof(std::getenv("SMS_PORT_PRESENT_DELAY_MS"))
	                                 : -1.0;
	// at most 0.8 display periods: a longer hold keeps Aurora's frame slots busy, and the
	// main thread then waits in aurora_begin_frame and misses the next slot
	const double fps   = port_interp_effective_fps();
	const double maxMs = fps > 0.0 ? std::min(8.0, 800.0 / fps) : 8.0;
	sPresentDelayMs    = sFixed >= 0.0 ? sFixed : std::min(maxMs, std::max(0.5, sPresentNeedMs + 0.5));
	const long long tag  = slot + qpcFromMs(sPresentDelayMs);
	const long long left = tag - qpcNow();
	if (left <= 0 || left > qpcFromMs(100.0)) { // late, or a stale tag: present now
		sPresentStart = qpcNow();
		return;
	}
	static const long long kSpin = qpcFromMs(0.4); // the high-resolution timer's lateness
	if (left > kSpin) {
		static HANDLE sTimer = [] {
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
			HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			return h != nullptr ? h : CreateWaitableTimerW(nullptr, FALSE, nullptr);
		}();
		LARGE_INTEGER f, due;
		QueryPerformanceFrequency(&f);
		due.QuadPart = -(long long)((double)(left - kSpin) * 1e7 / (double)f.QuadPart); // relative, 100 ns
		if (due.QuadPart < 0 && SetWaitableTimer(sTimer, &due, 0, nullptr, nullptr, FALSE))
			WaitForSingleObject(sTimer, 200);
	}
	while (qpcNow() < tag)
		YieldProcessor();
	sPresentStart = qpcNow();
}

// Render worker, after a Present: when the frame could have been presented at the earliest
// (Present blocks while the GPU is behind: then its return minus an unblocked Present's cost;
// otherwise the frame's arrival), relative to its slot, into the delay estimate.
static void presentDone()
{
	const long long now = qpcNow();
	if (sPresentTag > 0) {
		const double msPerTick  = 1000.0 / (double)qpcFromMs(1000.0);
		const double kPresentMs = 0.4; // an unblocked Present
		const double took       = (double)(now - sPresentStart) * msPerTick;
		const long long ready   = took > kPresentMs + 0.3 ? now - qpcFromMs(kPresentMs) : sPresentArrive;
		static double sReady[128];
		static unsigned sCount;
		sReady[sCount++ % 128] = (double)(ready - sPresentTag) * msPerTick;
		if (sCount % 16 == 0) { // 90th percentile of the last 128 frames
			const unsigned n = sCount < 128 ? sCount : 128;
			double sorted[128];
			std::memcpy(sorted, sReady, n * sizeof(double));
			std::sort(sorted, sorted + n);
			sPresentNeedMs = sorted[(n * 9) / 10];
		}
	}
	if (sPresentLog) {
		const double ms = 1000.0 / (double)qpcFromMs(1000.0);
		std::fprintf(stderr, "[present] %.3f slot %.3f arrived %+.3f presented %+.3f (delay %.2f)\n", now * ms,
		             sPresentTag > 0 ? sPresentTag * ms : 0.0, sPresentTag > 0 ? (sPresentArrive - sPresentTag) * ms : 0.0,
		             sPresentTag > 0 ? (now - sPresentTag) * ms : 0.0, sPresentDelayMs);
	}
}

// target: when the frame is meant to be on screen (it is drawn slightly ahead).
static void displayFrame(long long target)
{
	const long long t0 = qpcNow();
	const bool logic   = sCopyDispPending; // the open frame holds a logic draw (replaced)
	if (!port_interp_render(target > t0 ? target : t0, logic)) {
		port_interp_skip(t0);
		return;
	}
	if (logic)
		port_efb_end_frame();
	dispShot(false);
	setPresentSlot(target); // w14: Aurora presents it on its slot, not on completion
	// w14: host-only present work; threads of priority <= 9 may run meanwhile (see the
	// logic-frame present in VIWaitForRetrace).
	const int preemptible = port_preemptible_begin();
	PORT_PROF_BEGIN(tEnd);
	aurora_end_frame();
	PORT_PROF_END(PP_ENDFRAME, tEnd);
	sInFrame         = false;
	sCopyDispPending = false;
	port_prof_frame(); // w14: one presented frame
	PORT_PROF_BEGIN(tBegin);
	beginFrame();
	PORT_PROF_END(PP_BEGINFRAME, tBegin);
	port_preemptible_end(preemptible);
	port_window_display_frame();
	port_interp_presented(t0, qpcNow());
}

static BOOL interpWait(BOOL en, u32 c)
{
	static bool sAlarmInit;
	if (!sAlarmInit) {
		OSCreateAlarm(&sInterpAlarm);
		sAlarmInit = true;
	}
	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	const long long retracePeriod = (long long)((double)sRetracePeriodNs * f.QuadPart / 1e9); // w13
	while (c == sRetraceCount && port_interp_active() && sInFrame) {
		const long long now  = qpcNow();
		const long long next = sRetraceQpc + retracePeriod;
		long long due        = port_interp_due();
		const long long lead = port_interp_lead(); // drawn this much ahead, to be shown at `due`
		if (sCopyDispPending && port_interp_near(due, next)) {
			// that slot is the logic frame's own: its picture is presented at the retrace
			port_interp_defer(next);
			break;
		}
		if (due - lead <= now) {
			if (port_interp_fits(now, next)) {
				OSRestoreInterrupts(en);
				displayFrame(due);
				en = OSDisableInterrupts();
				continue;
			}
			break; // does not fit before the retrace: drawn when it can be, after it
		}
		bool armed = false;
		if (due - lead < next) {
			const long long ticks = (long long)((double)(due - lead - now) * (PORT_BUS_CLOCK / 4) / f.QuadPart);
			OSSetAlarm(&sInterpAlarm, ticks > 0 ? ticks : 1, interpAlarm);
			armed = true;
		}
		sleepForRetrace(); // w14: profiled
		if (armed)
			OSCancelAlarm(&sInterpAlarm);
	}
	return en;
}

void VIWaitForRetrace(void)
{
	const bool isMain = GetCurrentThreadId() == sMainThreadId;
	// w9 quality of life (port_qol.cpp, off by default):
	//  - pause while the window has no focus: the game thread waits here and the audio
	//    interrupt stops (sdk_audio.cpp), until the window gets the focus back;
	//  - fast-forward: only every 2nd / 4th wait of the game thread waits for a retrace,
	//    so 2 / 4 game frames run per frame time (the extra frames are still presented).
	bool ffSkip = false;
	if (isMain) {
		if (port_qol_should_pause()) {
			port_qol_set_paused(1);
			while (port_qol_should_pause()) {
				SDL_PumpEvents();
				if (SDL_HasEvent(SDL_EVENT_QUIT) || SDL_HasEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED))
					break;
				Sleep(15);
			}
			port_qol_set_paused(0);
		}
		static u32 sFfCalls;
		const int ff = port_qol_ff_speed();
		ffSkip       = ff > 1 && (++sFfCalls % (u32)ff) != 0;
	}
	BOOL en = OSDisableInterrupts();
	u32 c = sRetraceCount;
	if (isMain && !ffSkip)
		en = interpWait(en, c); // w9
	while (c == sRetraceCount && !ffSkip) {
		if (isMain)
			sleepForRetrace(); // w14: profiled
		else
			OSSleepThread(&sRetraceQueue);
	}
	// fast-forward: a skipped wait counts as a retrace, so TVideo::waitForRetrace's
	// "until the count has advanced" loop does not wait for a real one (4x = 2 game frames per
	// real retrace).
	if (ffSkip)
		InterlockedIncrement((volatile LONG*)&sRetraceCount);
	OSRestoreInterrupts(en);

	if (!isMain)
		return;
	port_prof_main_woke(); // w14
	port_interp_retrace(sRetraceQpc); // w9: segment bookkeeping
	const u32 sFrames = ++sFrameCount;
	if (sFrames == 1 || sFrames % 600 == 0)
		std::fprintf(stderr, "[port] frame %u (retrace %u)\n", sFrames, sRetraceCount);
	static const bool sPresentAll = std::getenv("SMS_PORT_PRESENT_ALL") != nullptr; // old behaviour
	// SMS_PORT_SHOT: GPU readback of the EFB right before it is presented. A listed frame
	// that presents nothing is taken at the next present (same image as on screen).
	static u32 sShotPending;
	if (shotListed(sFrames))
		sShotPending = sFrames;
	if (const unsigned mv = port_movetest_shot_due()) // movement tests (port_movetest.cpp)
		sShotPending = mv;
	dispShotStart(sFrames);
	// w14: present on the retrace the GameCube VI would flip on: the wait that follows
	// TVideo::waitForRetrace's VIFlush (port_VIFlush below). Presenting on the first wait
	// after GXCopyDisp instead moved the present one retrace later whenever a frame's
	// CPU work took longer than one retrace (16.7 ms), so the interval alternated 50/17 ms
	// (Bianco with the EFB readbacks: 2-3% of frames). SMS_PORT_PRESENT_EARLY=1: old rule.
	// With w9's frame-rate option drawing display frames (port_interp_active), w9's pacing
	// places the logic frame's present (interpWait above) and the old rule stays: the
	// interpolation segments start at the first retrace after the logic draw.
	static const bool sPresentEarly = std::getenv("SMS_PORT_PRESENT_EARLY") != nullptr;
	const bool flipNow = sPresentEarly || sFlushPending || port_interp_active();
	sFlushPending      = false;
	if (!sInFrame || !sCopyDispSeen || (sCopyDispPending && flipNow) || sPresentAll) {
		// w14: the present is host-only work that can wait for the GPU for 100+ ms
		// (aurora_begin_frame's frame slot); the audio and loader threads (priority <= 9)
		// may run meanwhile, threads that issue GX may not. See sdk_os_thread.cpp.
		const int preemptible = port_preemptible_begin();
		if (sInFrame) {
			port_efb_end_frame(); // port_efb.cpp: store finished EFB-copy readbacks
			if (sShotPending != 0) {
				maybeScreenshotEfb(sShotPending);
				sShotPending = 0;
			}
			dispShot(true);
			// w14: with SMS_PORT_PROFILE the FIFO drain that aurora_end_frame starts with is
			// done (and timed) first; it is idempotent, so end_frame then finds it empty.
			if (gPortProfile) {
				PORT_PROF_BEGIN(tFifo);
				AuroraGXSync();
				PORT_PROF_END(PP_FIFO, tFifo);
			}
			if (port_interp_active())
				setPresentSlot(sRetraceQpc); // w14: on the grid with the display frames
			PORT_PROF_BEGIN(tEnd);
			aurora_end_frame();
			PORT_PROF_END(PP_ENDFRAME, tEnd);
			sInFrame = false;
		}
		sCopyDispPending = false;
		sRetracesSincePresent = 0;
		maybeScreenshot(sFrames);
		port_prof_frame(); // w14: one presented frame
		PORT_PROF_BEGIN(tBegin);
		beginFrame();
		PORT_PROF_END(PP_BEGINFRAME, tBegin);
		port_preemptible_end(preemptible);
		port_window_frame(true); // w9: hotkeys, aspect, settings, F3 overlay, frame stats
		port_interp_logic_presented(sRetraceQpc); // w9: display frames continue from this retrace
	} else if (port_interp_active() && port_interp_due() <= qpcNow() + qpcFromMs(0.5)) {
		// w9: a display frame already due (no logic frame to present at this retrace)
		displayFrame(port_interp_due()); // w14: tagged with its slot (presented at once if late)
		port_window_frame(false); // retrace statistics
	} else {
		// Nothing new to show: keep the window responsive without touching the open frame.
		SDL_PumpEvents();
		++sRetracesSincePresent;
		if (sRetracesSincePresent > 60
		    && (SDL_HasEvent(SDL_EVENT_QUIT) || SDL_HasEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED))) {
			std::fprintf(stderr, "[port] window closed (no frame for %u retraces), exiting\n",
			             sRetracesSincePresent);
			std::fflush(stdout);
			port_padfx_shutdown(); // w9
			std::exit(0);
		}
		maybeScreenshot(sFrames);
		port_window_frame(false);
	}
}

// w14: game/JSystem calls to VIFlush are renamed to this by build_aurora.py. On the
// GameCube the frame buffer set before VIFlush is scanned out from the next retrace;
// VIWaitForRetrace presents there (see the present rule above).
void port_VIFlush(void)
{
	VIFlush();
	sFlushPending = true;
}

// Game/JSystem calls to GXCopyDisp are renamed to this by build_aurora.py (Aurora's
// GXCopyDisp itself does nothing: the EFB is presented at the end of the Aurora frame).
void port_GXCopyDisp(void* dest, GXBool clear)
{
	GXCopyDisp(dest, clear);
	if (port_interp_drawing()) // w9: an extra display frame's copy is not a logic frame
		return;
	sCopyDispSeen    = true;
	sCopyDispPending = true;
}

// Game/JSystem calls to PADRead are renamed to this by build_aurora.py.
// SMS_PORT_PADSCRIPT=<frame>:<buttons>[,<frame>:<buttons>...] holds <buttons> on
// pad 0 for 10 frames from <frame> (frames as in SMS_PORT_SHOT), on top of the real pad (for driving boot
// through menus in unattended runs). <buttons> is a '+'-separated list of
// START, A, B, X, Y, Z, L, R, UP, DOWN, LEFT, RIGHT, or a hex mask.
extern "C" void port_sweep_pad_apply(PADStatus* status, u32 frame); // port_sweep_input.cpp (SMS_PORT_STICKSCRIPT)
extern "C" void port_route_pad(PADStatus* status); // Application.cpp (w12: SMS_PORT_ROUTE)
u32 port_PADRead(PADStatus* status)
{
	u32 ret = port_input_read(status); // w9: PADRead + GameCube stick/trigger semantics + keyboard
	port_sweep_pad_apply(status, sFrameCount);
	port_route_pad(status); // w12: SMS_PORT_ROUTE (Application.cpp)
	static const char* sScript = std::getenv("SMS_PORT_PADSCRIPT");
	if (sScript == nullptr)
		return ret;
	port_boss_assist(); // w5: SMS_PORT_INFWATER test aid (port_bosstrace.cpp)
	static const struct {
		const char* name;
		u16 mask;
	} kNames[] = { { "START", PAD_BUTTON_START }, { "A", PAD_BUTTON_A },  { "B", PAD_BUTTON_B },
		           { "X", PAD_BUTTON_X },         { "Y", PAD_BUTTON_Y },  { "Z", PAD_TRIGGER_Z },
		           { "L", PAD_TRIGGER_L },        { "R", PAD_TRIGGER_R }, { "UP", PAD_BUTTON_UP },
		           { "DOWN", PAD_BUTTON_DOWN },   { "LEFT", PAD_BUTTON_LEFT },
		           { "RIGHT", PAD_BUTTON_RIGHT } };
	// Entry forms: <frame>:<tokens> (10 frames), <from>-<to>:<tokens> (held, <to> exclusive),
	// <from>-<to>/<period>:<tokens> (pressed for 5 frames every <period> frames).
	// Tokens: button names, a hex button mask, or S<degrees> = main stick fully tilted
	// (0 up, 90 right, 180 down, 270 left).
	// W7-J (BOOTLOG): S<deg>;<pct> partial tilt; G<x>;<z>;<pct> slow walk; GN<x>;<z> walk
	// along SMS_PORT_NAV; G* / F* walk to / turn toward Shadow Mario; F<x>;<z> turn toward a
	// point in the spray stance; W<x>;<z>|... a route; ?<bit> / !<bit> as listed in BOOTLOG.
	const u32 now = sFrameCount;
	u16 add = 0;
	bool stick = false, goTo = false, face = false, route = false, goNav = false;
	int stickX = 0, stickY = 0, goMag = 100;
	bool gotoFace = false, zeroR = false; // w5: boss bots
	int analogR = 0;
	for (const char* p = sScript; *p != '\0';) {
		// "+<frame>...": counted from when the current stage gave Mario control
		// (gPortStageFrame, set by Application.cpp); inactive until then.
		// "+<n>@<frame>...": the same, but only in the n-th stage to give control in this
		// run (0 = the first; gPortStageCount).
		// "+d<frame>...": counted from when the latest demo or talk ended (director mode
		// back to 0; gPortDemoEndFrame), e.g. a boss's appear demo; inactive before any.
		const bool rel = *p == '+';
		if (rel)
			++p;
		const bool relDemo = rel && *p == 'd';
		if (relDemo)
			++p;
		char* end;
		unsigned long at = std::strtoul(p, &end, 10);
		if (end == p)
			break;
		long stageIdx = -1;
		if (rel && *end == '@') {
			stageIdx = (long)at;
			p        = end + 1;
			at       = std::strtoul(p, &end, 10);
			if (end == p)
				break;
		}
		unsigned long to = at + 10, period = 0;
		if (*end == '-')
			to = std::strtoul(end + 1, &end, 10);
		unsigned long onLen = 5; // "/<period>;<on>" (W7-J): held <on> frames of each period
		if (*end == '/')
			period = std::strtoul(end + 1, &end, 10);
		if (period != 0 && *end == ';')
			onLen = std::strtoul(end + 1, &end, 10);
		if (*end != ':')
			break;
		p = end + 1;
		u16 mask = 0;
		bool hasStick = false, hasGoto = false, hasFace = false, condFail = false;
		int sx = 0, sy = 0, gx = 0, gz = 0, gm = 100, ra = 0, rn = 0;
		bool gn = false;
		int rpts[16][2];
		bool faceB = false, hasAim = false, hasPull = false, hasThrow = false, hasRocket = false; // w5: boss bots
		int aimD = 0, aimDeg = 0, aimGun = 0;
		while (*p != '\0' && *p != ',') {
			const char* tok = p;
			while (*p != '\0' && *p != ',' && *p != '+')
				++p;
			size_t len = (size_t)(p - tok);
			bool named = false;
			for (const auto& n : kNames)
				if (std::strlen(n.name) == len && std::strncmp(n.name, tok, len) == 0) {
					mask |= n.mask;
					named = true;
				}
			if (!named && len == 2 && tok[0] == 'R' && tok[1] == 'A') {
				ra    = 0xC0; // w5: bare RA = RA192 (light analog press, FLUDD spray without the click)
				named = true;
			}
			// w5: boss-relative autopilot (port_bosstrace.cpp tracks the boss, SMS_PORT_BOSS_TARGET):
			// B<d>;<deg> walks to the point <d> units from the boss at <deg> from its facing
			// (0 = in front); FB turns/creeps toward the boss (35 % stick); AIM<d>;<deg>;<gun> =
			// spray bot; PULL = grab-and-pull bot.
			if (!named && len == 2 && tok[0] == 'F' && tok[1] == 'B') {
				named = true;
				if (now - gPortBossFrame < 10) {
					gx      = (int)gPortBossPos[0];
					gz      = (int)gPortBossPos[2];
					hasGoto = true;
					faceB   = true;
				}
			}
			if (!named && len == 2 && tok[0] == 'N' && tok[1] == 'B') {
				// NB: the entry applies only while no boss is tracked (none updated for 30
				// frames: beaten, or not there yet), e.g. A presses for the Shine demo / save prompt.
				if (now - gPortBossFrame < 30)
					condFail = true;
				named = true;
			}
			if (!named && len == 4 && std::strncmp(tok, "PULL", 4) == 0) {
				hasPull = true;
				named   = true;
			}
			if (!named && len == 6 && std::strncmp(tok, "TURRET", 6) == 0) {
				// Mecha-Bowser coaster: turn toward the boss and fire (port_boss_turret)
				int tsx = 0, fire = 0;
				if (port_boss_turret(now, &tsx, &fire)) {
					hasStick = true;
					sx       = tsx;
					sy       = 0;
					if (fire)
						mask |= PAD_TRIGGER_R;
				}
				named = true;
			}
			if (!named && len == 3 && std::strncmp(tok, "EEL", 3) == 0) {
				// Eely-Mouth, under water (port_boss_eel)
				int on = 0, ex = 0, ez = 0, creep = 0, holdB = 0, fire = 0;
				if (port_boss_eel(now, &on, &ex, &ez, &creep, &holdB, &fire)) {
					if (on) {
						hasGoto = true;
						faceB   = creep != 0;
						gx      = ex;
						gz      = ez;
					}
					if (holdB)
						mask |= PAD_BUTTON_B;
					if (fire)
						mask |= PAD_TRIGGER_R;
				}
				named = true;
			}
			if (!named && len == 6 && std::strncmp(tok, "ROCKET", 6) == 0) {
				hasRocket = true; // final Bowser: super hip drops on the tub grips (port_boss_rocket)
				named     = true;
			}
			if (!named && len == 5 && std::strncmp(tok, "THROW", 5) == 0) {
				hasThrow = true; // carry-and-throw bot (port_bosstrace.cpp port_boss_throw)
				named    = true;
			}
			if (!named && len > 3 && std::strncmp(tok, "AIM", 3) == 0) {
				char* e2;
				aimD   = (int)std::strtol(tok + 3, &e2, 10);
				aimDeg = *e2 == ';' ? (int)std::strtol(e2 + 1, &e2, 10) : 0;
				aimGun = *e2 == ';' ? (int)std::strtol(e2 + 1, nullptr, 0) : 0x1400;
				hasAim = true;
				named  = true;
			}
			if (!named && len > 1 && tok[0] == 'B' && (tok[1] == '-' || (tok[1] >= '0' && tok[1] <= '9'))) {
				char* e2;
				const double d   = std::strtod(tok + 1, &e2);
				const double deg = *e2 == ';' ? std::strtod(e2 + 1, nullptr) : 0.0;
				named            = true;
				if (now - gPortBossFrame < 10) {
					const double a = (gPortBossRotY + deg) * 3.14159265358979 / 180.0;
					gx             = (int)(gPortBossPos[0] + d * std::sin(a));
					gz             = (int)(gPortBossPos[2] + d * std::cos(a));
					hasGoto        = true;
				}
			}
			if (!named && len > 1 && tok[0] == 'W') {
				// W<x>;<z>|<x>;<z>|...: walk a route point by point (Application.cpp takes
				// the next point within 150 units; W7-J)
				rn = 0;
				for (const char* q = tok + 1; q < p && rn < 16;) {
					char* e4;
					rpts[rn][0] = (int)std::strtol(q, &e4, 10);
					rpts[rn][1] = *e4 == ';' ? (int)std::strtol(e4 + 1, &e4, 10) : 0;
					++rn;
					q = e4;
					while (q < p && *q != '|')
						++q;
					if (q < p)
						++q;
				}
				named = rn > 0;
			}
			if (!named && len >= 2 && (tok[0] == 'G' || tok[0] == 'F') && tok[1] == '*') {
				// G* / F* (W7-J): walk to / turn toward the stage's Shadow Mario
				// (Application.cpp follows him); G*;<percent> as for G
				gx = 0x7FFFFFFF;
				gz = 0;
				if (tok[0] == 'G') {
					hasGoto = true;
					if (len > 3 && tok[2] == ';')
						gm = (int)std::strtol(tok + 3, nullptr, 10);
				} else {
					hasFace = true;
				}
				named = true;
			}
			if (!named && len > 1 && tok[0] == 'F' && (tok[1] == '-' || (tok[1] >= '0' && tok[1] <= '9'))) {
				// F<x>;<z>: turn Mario toward world (x, z) in the FLUDD spray stance (R held),
				// stick X only; an S token in the same entry still sets the aim (W7-J)
				char* e2;
				gx      = (int)std::strtol(tok + 1, &e2, 10);
				gz      = *e2 == ';' ? (int)std::strtol(e2 + 1, nullptr, 10) : 0;
				hasFace = true;
				named   = true;
			}
			if (!named && len > 1 && tok[0] == 'G') {
				// G<x>;<z>[;<percent>]: walk Mario to world (x, z) (Application.cpp
				// portAutopilot), with the stick tilted <percent> of the way (W7-J: a
				// slow walk, e.g. spraying goop clean ahead of him)
				// GN<x>;<z>: the same along SMS_PORT_NAV (Application.cpp portNavStep)
				char* e2;
				const bool viaNav = tok[1] == 'N';
				gx = (int)std::strtol(tok + (viaNav ? 2 : 1), &e2, 10);
				gz = *e2 == ';' ? (int)std::strtol(e2 + 1, &e2, 10) : 0;
				if (*e2 == ';')
					gm = (int)std::strtol(e2 + 1, nullptr, 10);
				gn = viaNav;
				hasGoto = true;
				named   = true;
			}
			if (!named && len > 1 && tok[0] == 'S' && tok[1] >= '0' && tok[1] <= '9') {
				// S<degrees>[;<percent>]: the stick tilted <percent> of the way (W7-J:
				// a partial FLUDD aim in the spray stance)
				char* e3;
				const double rad = std::strtod(tok + 1, &e3) * 3.14159265358979 / 180.0;
				const double pct = *e3 == ';' ? std::strtod(e3 + 1, nullptr) : 100.0;
				sx       = (int)(std::sin(rad) * pct);
				sy       = (int)(std::cos(rad) * pct);
				hasStick = true;
				named    = true;
			}
			if (!named && len > 1 && (tok[0] == '?' || tok[0] == '!') && tok[1] >= '0' && tok[1] <= '9') {
				// ?<bit> / !<bit>: the entry only applies while game-state bit <bit> of
				// gPortCond is set / clear (port_crash.cpp; 0 = boss head vulnerable,
				// 1 = boss asleep, 2 = boss defeated).
				const unsigned bit = 1u << (std::strtoul(tok + 1, nullptr, 10) & 31);
				if (((gPortCond & bit) != 0) != (tok[0] == '?'))
					condFail = true;
				named = true;
			}
			if (!named && len > 2 && tok[0] == 'R' && tok[1] == 'A') {
				// RA<0-255>: analog R only, without the digital R click (FLUDD: a light
				// press sprays while walking; R fully pressed makes Mario stand still)
				ra    = (int)std::strtoul(tok + 2, nullptr, 10);
				named = true;
			}
			if (!named)
				mask |= (u16)std::strtoul(tok, nullptr, 16);
			if (*p == '+')
				++p;
		}
		u32 clock = now;
		bool active = true;
		if (relDemo) {
			active = gPortDemoEndCount > 0;
			clock  = now - gPortDemoEndFrame;
		} else if (rel) {
			active = gPortStageFrame != 0
			         && (stageIdx < 0 || stageIdx == gPortStageCount - 1);
			clock = now - gPortStageFrame;
		}
		active = active && !condFail && clock >= at && clock < to;
		if (active && period != 0)
			active = (clock - at) % period < onLen;
		if (active) {
			add |= mask;
			if (ra > analogR)
				analogR = ra > 255 ? 255 : ra;
			// w5: several bots in one entry: the boss code picks (gPortBossWant: 0 pull, 1 / 2
			// spray / stomp, 3 wait, 4 carry and throw); with no boss tracked, AIM goes for the Shine.
			const bool bossFresh = now - gPortBossFrame < 10;
			if (hasRocket) {
				int mode = 0, bx = 0, bz = 0;
				if (port_boss_rocket(now, &mode, &bx, &bz)) {
					hasAim = hasPull = hasThrow = false;
					if (mode == 1 || mode == 5) {
						hasGoto = true;
						gx      = bx;
						gz      = bz;
						if (mode == 5)
							add |= PAD_BUTTON_A; // jump while running
					} else if (mode == 6)
						add |= PAD_TRIGGER_L;
					else if (mode == 9)
						add |= PAD_TRIGGER_R;
				} else if ((now % 60) < 5) {
					add |= PAD_BUTTON_A; // tub gone (ending demo, prompts): press on
				}
			}
			if (hasThrow) {
				if (bossFresh && gPortBossWant == 4) {
					hasAim = hasPull = false;
					int mode = 0, bx = 0, bz = 0;
					port_boss_throw(now, &mode, &bx, &bz);
					if (mode == 1 || mode == 2) {
						hasGoto = true;
						faceB   = mode == 2;
						gx      = bx;
						gz      = bz;
					} else if (mode == 7)
						add |= PAD_BUTTON_B; // pick up / throw
				}
			}
			if (bossFresh && gPortBossWant == 3 && (hasAim || hasPull || hasThrow))
				hasAim = hasPull = false; // the boss is out of reach: wait
			if (hasPull && hasAim) {
				if (gPortBossWant || now - gPortBossFrame >= 10) // no boss: AIM walks to the Shine
					hasPull = false;
				else
					hasAim = false;
			}
			if (hasPull) {
				int mode = 0, bx = 0, bz = 0;
				port_boss_pull(now, &mode, &bx, &bz);
				if (mode == 1 || mode == 2 || mode == 8) {
					hasGoto = true;
					faceB   = mode == 2;
					gx      = bx;
					gz      = bz;
				}
				if (mode == 7 || mode == 8)
					add |= PAD_BUTTON_B; // grab, and keep the grip while pulling
			}
			if (hasAim) {
				int mode = 0, bx = 0, bz = 0, botY = 0;
				port_boss_bot(aimD, aimDeg, aimGun, now, &mode, &bx, &bz, &botY);
				if (mode == 1 || mode == 2) {
					hasGoto = true;
					faceB   = mode == 2;
					gx      = bx;
					gz      = bz;
				} else if (mode == 5 || mode == 6) {
					add |= mode == 5 ? PAD_BUTTON_A : PAD_TRIGGER_L; // stomp: jump, then ground pound
					if (mode == 5) {
						hasGoto = true; // steer while jumping
						gx      = bx;
						gz      = bz;
					}
				} else if (mode == 3 || mode == 4) {
					add |= PAD_TRIGGER_R; // squat + spray (4: digital R kept, analog released = pump)
					if (mode == 4)
						zeroR = true;
					hasStick = true;
					sx       = 0;
					sy       = botY;
				}
			}
			if (hasStick) {
				stick  = true;
				stickX = sx;
				stickY = sy;
			}
			if (hasGoto) {
				goTo         = true;
				gotoFace     = faceB; // w5
				gPortGoto[1] = gx;
				gPortGoto[2] = gz;
				goMag        = gm;
				goNav        = gn;
			}
			if (hasFace) {
				face         = true;
				gPortGoto[1] = gx;
				gPortGoto[2] = gz;
			}
			if (rn > 0) {
				route       = true;
				gPortRouteN = rn;
				std::memcpy(gPortRoute, rpts, sizeof(int) * 2 * rn);
			}
		}
		if (*p == ',')
			++p;
	}
	// 2: Application.cpp turns him (F); 3: walks the W route. A later G entry wins over W.
	gPortGoto[0] = goTo ? (goNav ? 4 : 1) : route ? 3 : face ? 2 : 0; // 4: G along SMS_PORT_NAV
	if (route && !goTo) {
		goTo = true;
		goMag = 100;
	}
	if (goTo) {
		stick  = true;
		stickX = gPortGotoStick[0] * goMag / 100;
		stickY = gPortGotoStick[1] * goMag / 100;
		if (gotoFace) { // w5: FB / bot facing: creep
			stickX = stickX * 35 / 100;
			stickY = stickY * 35 / 100;
		}
	} else if (face) {
		stick  = true;
		stickX = gPortGotoStick[0];
	}
	static u16 sLastAdd;
	static bool sLastStick;
	static int sLastX, sLastY;
	static bool sLastGoto;
	if (goTo && !sLastGoto)
		std::fprintf(stderr, "[port] PADSCRIPT: frame %u, walking to %d,%d\n", now, gPortGoto[1],
		             gPortGoto[2]);
	sLastGoto = goTo;
	if ((add != sLastAdd && add != 0)
	    || (stick && !goTo && !face && (!sLastStick || stickX != sLastX || stickY != sLastY)))
		std::fprintf(stderr, "[port] PADSCRIPT: frame %u, buttons %04x stick %d,%d (pad err %d)\n",
		             now, add, stickX, stickY, status[0].err);
	sLastAdd   = add;
	sLastStick = stick;
	sLastX     = stickX;
	sLastY     = stickY;
	if (stick) {
		status[0].stickX = (s8)stickX;
		status[0].stickY = (s8)stickY;
		status[0].err    = PAD_ERR_NONE;
	}
	if (add != 0) {
		status[0].button |= add;
		status[0].err = PAD_ERR_NONE;
		if (add & PAD_TRIGGER_L)
			status[0].triggerLeft = 0xFF;
		if (add & PAD_TRIGGER_R)
			status[0].triggerRight = 0xFF;
		if (add & PAD_BUTTON_A)
			status[0].analogA = 0xFF;
		if (add & PAD_BUTTON_B)
			status[0].analogB = 0xFF;
	}
	if (zeroR) // w5: AIM bot pump (digital R held, analog released)
		status[0].triggerRight = 0;
	if (analogR != 0) {
		status[0].triggerRight = (u8)analogR;
		status[0].err          = PAD_ERR_NONE;
	}
	return ret;
}

// Game/JSystem calls to GXCallDisplayList are renamed to this by build_aurora.py.
// SMS_PORT_GXSYNC=1: process the FIFO before and after each display list (slow), so a
// FIFO error is raised while the game thread is still inside the call that queued
// the bad commands; the list is hex-dumped by the fatal-error handler.
const void* gPortLastDL;
u32 gPortLastDLSize;
// SMS_PORT_NODRAW=1 (debug, build_aurora.py --nodraw): display lists are dropped, so
// no J3D model reaches Aurora's pipeline compiler. For driving game logic past
// rendering fatals; nothing the game computes depends on GX output.
void port_GXCallDisplayList(const void* list, u32 nbytes)
{
	// w14: a scheduling point. The GameCube switches to a higher-priority thread the
	// moment an interrupt readies it (the audio thread on every AI/DSP interrupt); host
	// threads only switch at such points, and the main thread can draw for several ms
	// without reaching another one (SMS_PORT_PROFILE "preempt").
	port_sched_point();

	static const bool sNoDraw = std::getenv("SMS_PORT_NODRAW") != nullptr;
	if (sNoDraw)
		return;
	static const bool sSync = std::getenv("SMS_PORT_GXSYNC") != nullptr;
	if (!sSync) {
		GXCallDisplayList(list, nbytes);
		return;
	}
	AuroraGXSync();
	gPortLastDL     = list;
	gPortLastDLSize = nbytes;
	GXCallDisplayList(list, nbytes);
	AuroraGXSync();
	gPortLastDL = nullptr;
}

// Game/JSystem calls to GDInitGDLObj are renamed to this by build_aurora.py
// (SMS_PORT_WATCH_GD, port_crash.cpp).
void port_watch_note_buffer(const void* start, unsigned long length);
void port_GDInitGDLObj(GDLObj* dl, void* start, u32 length)
{
	GDInitGDLObj(dl, start, length);
	port_watch_note_buffer(start, length);
}

u32 VIGetRetraceCount(void) { return sRetraceCount; }
u32 VIGetNextField(void) { return 1; } // progressive / top field
u32 VIGetDTVStatus(void) { return 0; } // no component cable: game keeps 60 Hz NTSC
void* VIGetCurrentFrameBuffer(void) { return sCurrentFB; }
void* VIGetNextFrameBuffer(void) { return sNextFB; }
void VISetNextFrameBuffer(void* fb) { sNextFB = fb; }
void VISetBlack(BOOL black) { sBlack = black; }

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
	BOOL en = OSDisableInterrupts();
	VIRetraceCallback old = sPreCB;
	sPreCB = cb;
	OSRestoreInterrupts(en);
	return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
	BOOL en = OSDisableInterrupts();
	VIRetraceCallback old = sPostCB;
	sPostCB = cb;
	OSRestoreInterrupts(en);
	return old;
}

} // extern "C"

int main(int argc, char* argv[]) // aurora_main via <aurora/main.h>
{
	// Unbuffered, so OSReport (stdout) and port/Aurora logs (stderr) stay in order
	// when both are redirected to one file.
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	AuroraConfig config = {};
	config.appName = "Super Mario Sunshine";
	config.logCallback = &logCallback;
	config.logLevel = LOG_INFO;
	config.vsync = false; // VIWaitForRetrace paces to 59.94 Hz itself
	config.windowWidth = 640 * 2;
	config.windowHeight = 480 * 2;
	port_settings_configure(&config); // w9: port_settings.ini (window, input), PAD defaults
	port_disc_early_handoff(); // w13: a JP/EU/US disc for another region build runs in the sibling exe
	// Default (double-click, build_aurora.py --run): real D3D12, visible window.
	// SMS_PORT_NULLGFX=1 (build_aurora.py --run --nullgfx): no GPU device. Dawn's Null
	// backend still runs Aurora's GX command processing; ImGui then gets an SDL renderer,
	// forced to the software driver so it does not create a D3D device either.
	// SMS_PORT_OFFSCREEN=1 (also implied by SMS_PORT_NULLGFX): the window Aurora always
	// creates is placed far off-screen (Aurora ignores negative positions) and is shown
	// without activation.
	// SMS_PORT_CACHE_DIR: Aurora's pipeline/Dawn cache directory (default: the per-user
	// app data folder shared by every run). build_aurora.py --run points it into
	// port/build_aurora so test runs neither use nor change the cache of hand-started runs.
	if (const char* cache = std::getenv("SMS_PORT_CACHE_DIR"))
		if (*cache != '\0') {
			CreateDirectoryA(cache, nullptr);
			config.cachePath = cache;
		}
	// SMS_PORT_USER_DIR: Aurora's user folder, which holds the memory card (default: the
	// per-user app data folder). build_aurora.py --run points it into port/build_aurora
	// so scripted runs never write a player's save file.
	if (const char* user = std::getenv("SMS_PORT_USER_DIR"))
		if (*user != '\0') {
			CreateDirectoryA(user, nullptr);
			config.userPath = user;
		}
	// Automated test runs (any of the debug/test knobs below set; a player's double-click
	// sets none of them) are silent and off-screen by default, so agent and smoke runs
	// never play sound or pop up on the user's monitors. SMS_PORT_VISIBLE=1 opts out of
	// the off-screen window; an explicit SMS_PORT_AUDIO=1 keeps the sound on.
	{
		static const char* const kTestKnobs[] = {
		    "SMS_PORT_WARP",       "SMS_PORT_PADSCRIPT", "SMS_PORT_KEYSCRIPT", "SMS_PORT_STICKSCRIPT",
		    "SMS_PORT_SHOT",       "SMS_PORT_USER_DIR",  "SMS_PORT_CACHE_DIR", "SMS_PORT_SETTINGS",
		    "SMS_PORT_MP",         "SMS_PORT_MP_GHOST_PLAY", "SMS_PORT_REPLAY", "SMS_PORT_VPAD",
		    "SMS_PORT_NO_PADS",    "SMS_PORT_THP_SPEED", "SMS_PORT_DEBUG_CALL", "SMS_PORT_PROFILE"};
		bool testRun = false;
		for (const char* k : kTestKnobs)
			if (const char* v = std::getenv(k))
				if (*v != '\0')
					testRun = true;
		if (testRun) {
			if (std::getenv("SMS_PORT_AUDIO") == nullptr)
				_putenv_s("SMS_PORT_AUDIO", "0"); // CRT env: later std::getenv calls see it
			if (std::getenv("SMS_PORT_OFFSCREEN") == nullptr && std::getenv("SMS_PORT_VISIBLE") == nullptr)
				_putenv_s("SMS_PORT_OFFSCREEN", "1");
		}
	}
	const bool nullGfx = std::getenv("SMS_PORT_NULLGFX") != nullptr;
	if (nullGfx) {
		config.desiredBackend = BACKEND_NULL;
		SetEnvironmentVariableA("SDL_RENDER_DRIVER", "software");
	}
	if (nullGfx || std::getenv("SMS_PORT_OFFSCREEN") != nullptr) {
		config.windowPosX = 30000;
		config.windowPosY = 30000;
		SetEnvironmentVariableA("SDL_WINDOW_ACTIVATE_WHEN_SHOWN", "0");
	}
	// MEM1 is not allocated by Aurora: port/src/sdk_os_misc.cpp maps main RAM at
	// 0x80000000 and defines Aurora's MEM1Start/OSBaseAddress (see AURORA.md, RAM model).
	config.mem1Size = PORT_MRAM_SIZE;
	config.mem2Size = ARAM_DEFAULT_SIZE; // 16 MiB ARAM
	// --- memory card (w5, port_card.cpp) ---
	// Saves: Aurora's GCI-folder card under <userPath>/USA/Card A/. userPath defaults
	// to SDL's pref path (%APPDATA%\Super Mario Sunshine\); SMS_PORT_USER_DIR moves it
	// (test runs use it so they never touch the player's save).
	if (const char* userDir = std::getenv("SMS_PORT_USER_DIR"))
		if (*userDir != '\0') {
			CreateDirectoryA(userDir, nullptr);
			config.userPath = userDir;
		}
	// --- end memory card ---
	if (!nullGfx)
		port_gfx_backend_arm(); // w6: start guard for [graphics] backend (port_gfx_backend.cpp)
	AuroraInfo info = aurora_initialize(argc, argv, &config);
	if (nullGfx && info.backend != BACKEND_NULL) {
		std::fprintf(stderr, "[port] SMS_PORT_NULLGFX: Aurora did not select the Null backend (got %d); exiting\n",
		             (int)info.backend);
		std::fflush(stderr);
		ExitProcess(4);
	}
	if (!nullGfx)
		port_menu_init_fonts(); // the settings menu's Segoe UI (before the first frame builds the font atlas)
	VISetWindowTitle("Super Mario Sunshine");
	aurora_set_present_hooks(presentNotBefore, presentDone); // w14: present pacing
	std::fprintf(stderr, "[port] Aurora initialised (backend %d, %ux%u)\n", (int)info.backend,
	             info.windowSize.width, info.windowSize.height);

	// On the GameCube the boot code (__start) runs OSInit before main()
	// (port/src/sdk_os_misc.cpp: main RAM, boot info, clocks, arena).
	OSInit();

	// VI retrace interrupt.
	sMainThreadId = GetCurrentThreadId();
	OSInitThreadQueue(&sRetraceQueue);
	sViSlot = port_irq_add_periodic(viRetrace, sRetracePeriodNs); // w13: slot kept for PAL 50 Hz

	beginFrame();

	int ret;
	const char* gxTest = std::getenv("SMS_PORT_GXTEST");
	if (gxTest != nullptr && gxTest[0] != '\0' && gxTest[0] != '0') {
		// GX smoke test (port_gxtest.cpp): draws one real model, no game.
		ret = port_gx_test();
	} else if (const char* ct = std::getenv("SMS_PORT_CARDTEST"); ct != nullptr && ct[0] == '1') {
		ret = port_card_test(); // memory card self-test (port_card.cpp), no game
	} else {
		std::fprintf(stderr, "[port] entering game main()\n");
		std::fflush(stderr);
		ret = sms_game_main();
	}
	std::fprintf(stderr, "[port] game main() returned %d\n", ret);

	if (sInFrame) {
		aurora_end_frame();
		sInFrame = false;
	}
	aurora_shutdown();
	return ret;
}

#endif // SMS_PORT_AURORA
