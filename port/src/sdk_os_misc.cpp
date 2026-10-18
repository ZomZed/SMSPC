// Interim host versions of the non-threading Dolphin OS pieces (arena, time,
// report, caches, stopwatch, boot globals) so the game boots headless before
// Aurora's lib/dolphin is wired in. Delete this file when Aurora provides them.
//
// Main RAM is a 32 MB file mapping placed at 0x80000000 and mirrored at
// 0xC0000000, so hardware-address accesses in game code (e.g.
// *(u32*)0x80000028 in JKRHeap::initArena) work. The 32-bit exe is linked
// /largeaddressaware for this.
//
// Aurora build (SMS_PORT_AURORA): this file is also the RAM model there.
// The same mapping at 0x80000000 is Aurora's MEM1: we define the symbols of
// Aurora's OSMemory.cpp (OSBaseAddress, MEM1Start, MEM1End,
// OSGetPhysicalMemSize) so that object is never linked, and Aurora's
// OSAddress/OSCache objects work on this memory. OSInit, the arena, the boot
// info and the clock are ours (Aurora's OSInit.cpp/OSArena.cpp/OSTime.cpp are
// not linked); OSGetTime stays here because it is a scheduling point
// (port_os_poll) for the thread layer. See port/AURORA.md "RAM model".

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "port_sdk.h"

#include <dolphin/os.h>
#include <dolphin/base/PPCArch.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
u32 OSGetResetCode(void);
}

namespace {
const long long kTimerClock = PORT_BUS_CLOCK / 4;
bool gInit;
void* gArenaLo;
void* gArenaHi;
u32 gSoundMode = OS_SOUND_MODE_STEREO;
u32 gProgressive;
LARGE_INTEGER gQpcFreq, gQpcStart;
long long gTimeBase;

long long rawTime()
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	long long d = c.QuadPart - gQpcStart.QuadPart;
	long long f = gQpcFreq.QuadPart;
	return gTimeBase + (d / f) * kTimerClock + ((d % f) * kTimerClock) / f;
}

inline void put32(uintptr_t addr, u32 v) { *(u32*)addr = v; }
} // namespace

#ifdef SMS_PORT_AURORA
// Aurora's lib/dolphin/os/internal.hpp declares these with C++ linkage.
void* MEM1Start;
void* MEM1End;
#endif

extern "C" {

#ifdef SMS_PORT_AURORA
uintptr_t OSBaseAddress; // Aurora: OS_BASE_CACHED, __OSBusClock, OSPhysicalToCached
#endif

volatile int __OSCurrHeap = -1;

void port_os_init(void)
{
	if (gInit)
		return;
	gInit = true;
	std::setvbuf(stdout, nullptr, _IONBF, 0);

	QueryPerformanceFrequency(&gQpcFreq);
	QueryPerformanceCounter(&gQpcStart);
	// The timebase counts from 2000-01-01 00:00 local time (RTC).
	SYSTEMTIME now, y2k = {};
	GetLocalTime(&now);
	y2k.wYear  = 2000;
	y2k.wMonth = 1;
	y2k.wDay   = 1;
	FILETIME fn, fy;
	SystemTimeToFileTime(&now, &fn);
	SystemTimeToFileTime(&y2k, &fy);
	unsigned long long n = ((unsigned long long)fn.dwHighDateTime << 32) | fn.dwLowDateTime;
	unsigned long long y = ((unsigned long long)fy.dwHighDateTime << 32) | fy.dwLowDateTime;
	unsigned long long d = n - y;
	gTimeBase = (long long)(d / 10000000ull) * kTimerClock
	            + (long long)((d % 10000000ull) * kTimerClock / 10000000ull);

	HANDLE h = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
	                              PORT_MRAM_SIZE, nullptr);
	void* a  = h ? MapViewOfFileEx(h, FILE_MAP_ALL_ACCESS, 0, 0, PORT_MRAM_SIZE,
	                               (void*)(uintptr_t)PORT_MRAM_BASE)
	             : nullptr;
	void* b  = h ? MapViewOfFileEx(h, FILE_MAP_ALL_ACCESS, 0, 0, PORT_MRAM_SIZE,
	                               (void*)(uintptr_t)PORT_MRAM_MIRROR)
	             : nullptr;
	if (a != (void*)(uintptr_t)PORT_MRAM_BASE) {
		std::fprintf(stderr, "[port] FATAL: could not map main RAM at 0x%08X (err %lu)\n",
		             PORT_MRAM_BASE, GetLastError());
		std::exit(2);
	}
	if (b != (void*)(uintptr_t)PORT_MRAM_MIRROR)
		std::fprintf(stderr, "[port] warning: no uncached mirror at 0x%08X\n", PORT_MRAM_MIRROR);

	// Hardware register window (0xCC000000): game/JSystem code writes GX
	// commands straight to the write-gather pipe at 0xCC008000. Until a GX
	// backend exists, back it with scratch memory so those writes are dropped.
	if (!VirtualAlloc((void*)(uintptr_t)0xCC000000u, 0x10000, MEM_RESERVE | MEM_COMMIT,
	                  PAGE_READWRITE))
		std::fprintf(stderr, "[port] warning: could not map the 0xCC000000 register window\n");

	uintptr_t lo = PORT_MRAM_BASE;
	gArenaLo     = (void*)(lo + 0x4000); // keep the OS low-memory globals area free
	gArenaHi     = (void*)(lo + PORT_MRAM_SIZE);
	// OSBootInfo / low-memory globals as the IPL leaves them (native endian).
	// w13: this build's disc region (Aurora's card folder USA/JAP/EUR follows it)
#if defined(VERSION_GMSE01)
	std::memcpy((void*)lo, "GMSE01", 6);
#elif defined(VERSION_GMSJ01)
	std::memcpy((void*)lo, "GMSJ01", 6);
#else
	std::memcpy((void*)lo, "GMSP01", 6);
#endif
	put32(lo + 0x20, 0x0D15EA5E);           // boot magic
	put32(lo + 0x24, 1);                    // version
	put32(lo + 0x28, 0x01800000);           // physical memory size
	put32(lo + 0x2C, 0x00000003);           // console type: retail
	put32(lo + 0x30, (u32)(uintptr_t)gArenaLo); // < 4 GB by construction
	put32(lo + 0x34, (u32)(uintptr_t)gArenaHi);
	put32(lo + 0xCC, 0);                    // TV mode: NTSC
	put32(lo + 0xF0, 0x01800000);           // simulated memory size
	put32(lo + 0xF8, PORT_BUS_CLOCK);
	put32(lo + 0xFC, PORT_CORE_CLOCK);
#ifdef SMS_PORT_AURORA
	// Aurora's headers read these fields through OSBaseAddress (no fixed-address globals).
	MEM1Start     = (void*)lo;
	MEM1End       = (void*)(lo + PORT_MRAM_SIZE);
	OSBaseAddress = lo;
#else
	__OSPhysicalMemSize  = 0x01800000;
	__OSSimulatedMemSize = 0x01800000;
	__OSBusClock         = PORT_BUS_CLOCK;
	__OSCoreClock        = PORT_CORE_CLOCK;
	__OSTVMode           = 0;
#endif
}

void OSInit(void) { port_os_init(); }
u32 OSGetConsoleType(void) { return 0x00000003; }
u32 OSGetPhysicalMemSize(void) { return 0x01800000; }
u32 OSGetConsoleSimulatedMemSize(void) { return 0x01800000; }
u32 OSGetResetCode(void) { return 0; }
BOOL OSGetResetSwitchState(void) { return FALSE; }
BOOL OSGetResetButtonState(void) { return FALSE; }
OSResetCallback OSSetResetCallback(OSResetCallback) { return nullptr; }
u32 OSGetSoundMode(void) { return gSoundMode; }
void OSSetSoundMode(u32 mode) { gSoundMode = mode; }
u32 OSGetProgressiveMode(void) { return gProgressive; }
void OSSetProgressiveMode(u32 mode) { gProgressive = mode; }
// w13 (BOOTLOG "Regional behaviour (w13)"): per region build. The JP IPL font is Shift-JIS.
#ifdef VERSION_GMSJ01
u16 OSGetFontEncode(void) { return OS_FONT_ENCODE_SJIS; }
#else
u16 OSGetFontEncode(void) { return OS_FONT_ENCODE_ANSI; }
#endif
// PAL-only SRAM settings the EU build reads: the 60 Hz (EURGB60) choice from the game's
// boot prompt, and the console language (0 English, 1 German, 2 French, 3 Spanish,
// 4 Italian, 5 Dutch). Defaults: 60 Hz, English; SMS_PORT_PAL60=0 and
// SMS_PORT_PAL_LANGUAGE=<0-5> override them (the settings menu sets these, w13 stage 3).
int port_settings_pal60(void);         // port_behaviour.cpp ([game] pal_60hz)
int port_settings_pal_language(void);  // port_behaviour.cpp ([game] pal_language)
static u32 sEuRgb60 = 1;
static bool sEuRgb60Read = false;
u32 OSGetEuRgb60Mode(void)
{
	if (!sEuRgb60Read) {
		sEuRgb60Read = true;
		sEuRgb60     = port_settings_pal60() != 0;
		if (const char* e = std::getenv("SMS_PORT_PAL60"))
			sEuRgb60 = std::atoi(e) != 0;
	}
	return sEuRgb60;
}
void OSSetEuRgb60Mode(u32 on)
{
	sEuRgb60Read = true;
	sEuRgb60     = on != 0;
}
u8 OSGetLanguage(void)
{
	if (const char* e = std::getenv("SMS_PORT_PAL_LANGUAGE")) {
		const int l = std::atoi(e);
		if (l >= 0 && l <= 5)
			return (u8)l;
	}
	return (u8)port_settings_pal_language();
}
void OSProtectRange(u32, void*, u32, u32) { }
OSErrorHandler OSSetErrorHandler(OSError, OSErrorHandler) { return nullptr; }
void OSFillFPUContext(OSContext*) { }
void OSRegisterResetFunction(OSResetFunctionInfo*) { }
void OSUnregisterResetFunction(OSResetFunctionInfo*) { }

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu)
{
	std::printf("[port] OSResetSystem(%d, 0x%x, %d): exiting\n", reset, (unsigned)resetCode,
	            forceMenu);
	std::fflush(stdout);
	ExitProcess(0);
}

void DCInvalidateRange(void*, u32) { }
void DCFlushRange(void*, u32) { }
void DCStoreRange(void*, u32) { }
void DCFlushRangeNoSync(void*, u32) { }
void DCStoreRangeNoSync(void*, u32) { }
void DCZeroRange(void* addr, u32 nBytes)
{
	u8* s = (u8*)((uintptr_t)addr & ~(uintptr_t)31);
	u8* e = (u8*)(((uintptr_t)addr + nBytes + 31) & ~(uintptr_t)31);
	std::memset(s, 0, e - s);
}
void DCTouchRange(void*, u32) { }
void ICInvalidateRange(void*, u32) { }
u32 PPCMfmsr() { return 0x9032; }
void PPCMtmsr(u32) { }
void PPCSync() { }

// SMS_PORT_WARP=<area>,<scenario> (debug, off by default): TApplication::proc goes
// from the Nintendo logo straight into that stage on a fresh save (port_sdk.h).
extern "C" void port_isle_force_on(void); // port_isle.cpp

int port_warp_target(unsigned char* area, unsigned char* scenario)
{
	if (const char* isle = std::getenv("SMS_PORT_ISLE_AUTO")) // the island test (port_isle.cpp)
		if (*isle != '\0' && *isle != '0')
			port_isle_force_on();
	const char* s = std::getenv("SMS_PORT_WARP");
	if (s == nullptr || *s == '\0')
		return 0;
	char* end;
	unsigned long a = std::strtoul(s, &end, 0);
	unsigned long sc = 0;
	if (*end == ',')
		sc = std::strtoul(end + 1, nullptr, 0);
	*area     = (unsigned char)a;
	*scenario = (unsigned char)sc;
	return 1;
}

// The only OSReport/OSVReport/OSPanic in both port builds (Aurora leaves them to
// the application).
void OSVReport(const char* fmt, va_list ap) { std::vprintf(fmt, ap); }

void OSReport(const char* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	std::vprintf(fmt, ap);
	va_end(ap);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
	va_list ap;
	std::printf("[OSPanic] %s:%d: ", file ? file : "?", line);
	va_start(ap, msg);
	std::vprintf(msg, ap);
	va_end(ap);
	std::printf("\n[OSPanic] halting\n");
	std::fflush(stdout);
	{
		// also into sms_port_crash.log (port_crash.cpp), with the backtrace below
		char text[512];
		va_start(ap, msg);
		std::vsnprintf(text, sizeof(text), msg, ap);
		va_end(ap);
		port_crash_log("\n[OSPanic] %s:%d: %s\n", file ? file : "?", line, text);
	}
	port_print_backtrace();
	if (IsDebuggerPresent())
		__debugbreak();
	ExitProcess(3);
}

OSTime OSGetTime(void)
{
	port_os_poll();
	return rawTime();
}

OSTick OSGetTick(void)
{
	port_os_poll();
	return (OSTick)rawTime();
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td)
{
	long long secs = ticks / kTimerClock;
	long long rem  = ticks % kTimerClock;
	if (rem < 0) {
		rem += kTimerClock;
		secs--;
	}
	long long usec = rem * 1000000 / kTimerClock;
	td->msec       = (int)(usec / 1000);
	td->usec       = (int)(usec % 1000);
	long long days = secs / 86400;
	long long s    = secs % 86400;
	if (s < 0) {
		s += 86400;
		days--;
	}
	td->hour  = (int)(s / 3600);
	td->min   = (int)(s / 60 % 60);
	td->sec   = (int)(s % 60);
	td->wday  = (int)(((days + 6) % 7 + 7) % 7); // 2000-01-01 was a Saturday
	int year  = 2000;
	auto leap = [](int yr) { return (yr % 4 == 0 && yr % 100 != 0) || yr % 400 == 0; };
	while (days < 0) {
		year--;
		days += leap(year) ? 366 : 365;
	}
	while (days >= (leap(year) ? 366 : 365)) {
		days -= leap(year) ? 366 : 365;
		year++;
	}
	td->year = year;
	td->yday = (int)days;
	static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	int mon                    = 0;
	for (;;) {
		int md = mdays[mon] + (mon == 1 && leap(year) ? 1 : 0);
		if (days < md)
			break;
		days -= md;
		mon++;
	}
	td->mon  = mon;
	td->mday = (int)days + 1;
}

void OSInitStopwatch(OSStopwatch* sw, char* name)
{
	sw->name  = name;
	sw->total = 0;
	sw->hits  = 0;
	sw->min   = 0x00000000FFFFFFFF;
	sw->max   = 0;
}
void OSStartStopwatch(OSStopwatch* sw)
{
	sw->running = TRUE;
	sw->last    = OSGetTime();
}
void OSStopStopwatch(OSStopwatch* sw)
{
	if (sw->running) {
		OSTime interval = OSGetTime() - sw->last;
		sw->total += interval;
		sw->running = FALSE;
		sw->hits++;
		if (sw->max < interval)
			sw->max = interval;
		if (interval < sw->min)
			sw->min = interval;
	}
}
OSTime OSCheckStopwatch(OSStopwatch* sw)
{
	OSTime t = sw->total;
	if (sw->running)
		t += OSGetTime() - sw->last;
	return t;
}
void OSResetStopwatch(OSStopwatch* sw) { OSInitStopwatch(sw, sw->name); }
void OSDumpStopwatch(OSStopwatch* sw)
{
	std::printf("Stopwatch [%s]: hits %lu total %lld\n", sw->name, (unsigned long)sw->hits,
	            (long long)sw->total);
}

void* OSGetArenaLo(void)
{
	port_os_init();
	return gArenaLo;
}
void* OSGetArenaHi(void)
{
	port_os_init();
	return gArenaHi;
}
void OSSetArenaLo(void* p) { gArenaLo = p; }
void OSSetArenaHi(void* p) { gArenaHi = p; }
void* OSAllocFromArenaLo(u32 size, u32 align)
{
	uintptr_t p = ((uintptr_t)gArenaLo + align - 1) & ~(uintptr_t)(align - 1);
	gArenaLo    = (void*)((p + size + align - 1) & ~(uintptr_t)(align - 1));
	return (void*)p;
}
void* OSAllocFromArenaHi(u32 size, u32 align)
{
	uintptr_t p = ((uintptr_t)gArenaHi - size) & ~(uintptr_t)(align - 1);
	gArenaHi    = (void*)p;
	return (void*)p;
}

} // extern "C"

#pragma init_seg(lib)
namespace {
struct PortOsBoot {
	PortOsBoot() { port_os_init(); }
} sPortOsBoot;
} // namespace
