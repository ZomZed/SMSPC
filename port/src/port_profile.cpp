// SMS_PORT_PROFILE=1: per-frame CPU profile and stage load times (w14).
// See port/include/port_profile.h for the categories and port/BOOTLOG.md
// "Performance (w14)" for how to read the output.
//
// Knobs:
//   SMS_PORT_PROFILE=1      enable (otherwise every hook is a single branch)
//   SMS_PORT_PROFILE=2      also print a backtrace where a thread ends a >4 ms stretch
//                           without a scheduling point (sdk_os_thread.cpp checkPreempt)
//                           and where it began; every 15 s, the audio thread's waits for the CPU
//                           by the call chain that ended them
//   SMS_PORT_PROFILE=3      instead: dump the main thread's stack when no frame has been
//                           presented for 150 ms (where a hitch is spent)
//   SMS_PORT_PROFILE_N=<n>  presented frames per summary (default 300 = 10 s at 30 fps)
//
// Output (stderr): "[prof] ..." summary blocks, "[prof] load ..." lines.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "port_profile.h"
#include "port_dvd_cache.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C" int gPortProfile = 0;
extern "C" void port_dump_thread(unsigned long tid); // port_crash.cpp
extern "C" unsigned long port_sched_running_tid(void); // sdk_os_thread.cpp
extern "C" void port_sched_dump_all(void);             // sdk_os_thread.cpp
extern "C" void port_arq_stats(long long* delivered, long long* held); // sdk_audio.cpp
extern "C" void port_sampler_start(void); // port_sampler.cpp (w12: SMS_PORT_PROFILE_SAMPLE)

namespace {

const char* const kNames[PP_NCATS] = {
	"direct",   "  move",   "  gxgen",  "setupObj", "viwait",  "wakelat", "fifo",  "endframe", "beginframe",
	"audio",    "handoff",  "intrwait", "irqlate",  "preempt", "dvd",     "yaz0",  "swap",     "heap",
};

LARGE_INTEGER gFreq;
double gMsPerTick;

std::atomic<long long> gAcc[PP_NCATS];
std::atomic<long long> gCnt[PP_NCATS];
std::atomic<long long> gLoadAcc[PP_NCATS];
std::atomic<long long> gLoadCnt[PP_NCATS];
std::atomic<long long> gLoadBytes[PP_NCATS];
std::atomic<long long> gLastRetrace;

int gN = 300;
// Per-frame samples for the current summary window: [frame][column].
// Columns: PP_NCATS categories, then interval, then main busy.
enum { COL_INTERVAL = PP_NCATS, COL_BUSY, NCOLS };
// Plain pointers: Init (init_seg(lib)) runs before ordinary static constructors.
double* gSamples;
double* gCounts; // per frame event counts per category
int gFrames;
unsigned gFrameTotal;
long long gLastPresent;
long long gWindowStart;

// SMS_PORT_PROFILE=3 hitch sampler: a host thread that dumps the main thread's stack
// once when no frame has been presented for kHitchMs (port_crash.cpp port_dump_thread).
std::atomic<unsigned long> gMainTid;
std::atomic<long long> gLastPresentAtomic;
double kHitchMs = 150.0; // SMS_PORT_PROFILE_HITCH_MS overrides it

// port_dump_thread leaves the thread suspended (its callers exit next); resume it.
void dumpAndResume(unsigned long tid)
{
	port_dump_thread(tid);
	if (HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, tid)) {
		ResumeThread(th);
		CloseHandle(th);
	}
}

DWORD WINAPI hitchSampler(LPVOID)
{
	long long dumpedFor = -1;
	for (;;) {
		Sleep(10);
		long long last = gLastPresentAtomic.load();
		unsigned long tid = gMainTid.load();
		if (last == 0 || tid == 0 || last == dumpedFor)
			continue;
		LARGE_INTEGER c;
		QueryPerformanceCounter(&c);
		double ms = (double)(c.QuadPart - last) * gMsPerTick;
		static int sDumps;
		if (ms > kHitchMs && sDumps < 40) {
			++sDumps;
			dumpedFor = last;
			std::fprintf(stderr, "[prof] hitch: no frame presented for %.0f ms; main thread is at:\n", ms);
			dumpAndResume(tid);
			if (std::getenv("SMS_PORT_PROFILE_ALL") != nullptr) // every game thread
				port_sched_dump_all();
			else if (unsigned long run = port_sched_running_tid(); run != 0 && run != tid) {
				std::fprintf(stderr, "[prof] hitch: the game thread holding the CPU is at:\n");
				dumpAndResume(run);
			}
		}
	}
}

// Stage load tracking (main thread only).
bool gLoadPending, gLoadReady;
long long gLoadT0, gLoadReadyT;
int gLoadState, gLoadArea, gLoadScenario;
PortDvdCacheStats gLoadCache0; // read-ahead cache counters at the state change
long long gLoadArq0, gLoadArqHeld0; // ARQ completion counters at the state change

struct Init {
	Init()
	{
		const char* e = std::getenv("SMS_PORT_PROFILE");
		if (e == nullptr || e[0] == '\0' || e[0] == '0')
			return;
		QueryPerformanceFrequency(&gFreq);
		gMsPerTick = 1000.0 / (double)gFreq.QuadPart;
		if (const char* n = std::getenv("SMS_PORT_PROFILE_N"))
			gN = std::max(10, std::atoi(n));
		gSamples = (double*)std::calloc((size_t)gN * NCOLS, sizeof(double));
		gCounts  = (double*)std::calloc((size_t)gN * PP_NCATS, sizeof(double));
		gPortProfile = std::max(1, std::atoi(e)); // 2: also log long preemption delays with a backtrace
		std::fprintf(stderr, "[prof] SMS_PORT_PROFILE on: summary every %d presented frames\n", gN);
	}
};
#pragma init_seg(lib)
Init sInit;

double pct(std::vector<double>& v, double p)
{
	if (v.empty())
		return 0.0;
	std::sort(v.begin(), v.end());
	size_t i = (size_t)std::floor(p * (double)(v.size() - 1));
	return v[i];
}

void printSummary(long long now)
{
	const int n = gFrames;
	const double secs = (double)(now - gWindowStart) * gMsPerTick / 1000.0;
	std::vector<double> col(n);
	// Frame pacing first.
	double sum = 0, sq = 0, mn = 1e9, mx = 0;
	int late = 0, early = 0;
	for (int f = 0; f < n; ++f) {
		double v = gSamples[(size_t)f * NCOLS + COL_INTERVAL];
		col[f] = v;
		sum += v;
		sq += v * v;
		mn = std::min(mn, v);
		mx = std::max(mx, v);
	}
	// Off by more than 2 ms from the nominal interval: two retraces (33.37 ms) at the
	// original 30 fps, otherwise (w9's frame-rate option) the window's median interval.
	const double med   = pct(col, 0.5);
	const double nomin = std::fabs(med - 33.367) < 4.0 ? 33.367 : med;
	for (int f = 0; f < n; ++f) {
		const double v = gSamples[(size_t)f * NCOLS + COL_INTERVAL];
		if (v > nomin + 2.0)
			++late;
		if (v < nomin - 2.0)
			++early;
	}
	// Whole-process CPU time over the window (all threads: game, Aurora workers, interrupt
	// thread spin, audio output), in % of one core.
	static unsigned long long sLastCpu;
	FILETIME ftC, ftE, ftK, ftU;
	GetProcessTimes(GetCurrentProcess(), &ftC, &ftE, &ftK, &ftU);
	const unsigned long long cpu = ((unsigned long long)ftK.dwHighDateTime << 32 | ftK.dwLowDateTime)
	                               + ((unsigned long long)ftU.dwHighDateTime << 32 | ftU.dwLowDateTime);
	const double cpuPct = sLastCpu != 0 && secs > 0 ? (double)(cpu - sLastCpu) / 1e7 / secs * 100.0 : 0.0;
	sLastCpu            = cpu;
	const double avg = sum / n, sd = std::sqrt(std::max(0.0, sq / n - avg * avg));
	const double p99 = pct(col, 0.99), p01 = pct(col, 0.01);
	std::fprintf(stderr,
	             "[prof] frames %u-%u: %.2f fps | present interval avg %.2f sd %.2f min %.2f p1 %.2f p99 %.2f max "
	             "%.2f ms | off by >2 ms: %d late, %d early | process CPU %.0f%% of a core\n",
	             gFrameTotal - n + 1, gFrameTotal, n / (secs > 0 ? secs : 1), avg, sd, mn, p01, p99, mx, late, early,
	             cpuPct);
	std::fprintf(stderr, "[prof]   %-11s %8s %8s %8s %9s\n", "ms/frame", "avg", "p99", "max", "calls/fr");
	for (int c = 0; c <= COL_BUSY; ++c) {
		if (c == COL_INTERVAL)
			continue;
		double s = 0, m = 0, cnt = 0;
		for (int f = 0; f < n; ++f) {
			double v = gSamples[(size_t)f * NCOLS + c];
			col[f] = v;
			s += v;
			m = std::max(m, v);
			if (c < PP_NCATS)
				cnt += gCounts[(size_t)f * PP_NCATS + c];
		}
		if (s == 0 && cnt == 0)
			continue;
		const char* name = c == COL_BUSY ? "main busy" : kNames[c];
		double p = pct(col, 0.99);
		if (c < PP_NCATS)
			std::fprintf(stderr, "[prof]   %-11s %8.3f %8.3f %8.3f %9.1f\n", name, s / n, p, m, cnt / n);
		else
			std::fprintf(stderr, "[prof]   %-11s %8.3f %8.3f %8.3f\n", name, s / n, p, m);
	}
}

} // namespace

extern "C" {

long long port_prof_now(void)
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return c.QuadPart;
}

void port_prof_add(int cat, long long ticks)
{
	gAcc[cat].fetch_add(ticks, std::memory_order_relaxed);
	gCnt[cat].fetch_add(1, std::memory_order_relaxed);
	gLoadAcc[cat].fetch_add(ticks, std::memory_order_relaxed);
	gLoadCnt[cat].fetch_add(1, std::memory_order_relaxed);
}

void port_prof_add_bytes(int cat, long long ticks, long long bytes)
{
	port_prof_add(cat, ticks);
	gLoadBytes[cat].fetch_add(bytes, std::memory_order_relaxed);
}

void port_prof_retrace(void)
{
	if (gPortProfile)
		gLastRetrace.store(port_prof_now(), std::memory_order_relaxed);
}

void port_prof_main_woke(void)
{
	if (!gPortProfile)
		return;
	long long r = gLastRetrace.load(std::memory_order_relaxed);
	if (r != 0)
		port_prof_add(PP_WAKELAT, port_prof_now() - r);
}

void port_prof_frame(void)
{
	if (!gPortProfile)
		return;
	const long long now = port_prof_now();
	if (gLastPresent == 0) {
		gLastPresent = gWindowStart = now;
		for (int c = 0; c < PP_NCATS; ++c) {
			gAcc[c].store(0);
			gCnt[c].store(0);
		}
		return;
	}
	double* row = &gSamples[(size_t)gFrames * NCOLS];
	double* cnt = &gCounts[(size_t)gFrames * PP_NCATS];
	for (int c = 0; c < PP_NCATS; ++c) {
		row[c] = (double)gAcc[c].exchange(0, std::memory_order_relaxed) * gMsPerTick;
		cnt[c] = (double)gCnt[c].exchange(0, std::memory_order_relaxed);
	}
	{
		static bool sSamplerStarted; // w12: SMS_PORT_PROFILE_SAMPLE (port_sampler.cpp)
		if (!sSamplerStarted) {
			sSamplerStarted = true;
			port_sampler_start();
		}
	}
	if (gPortProfile == 3) {
		static bool sStarted;
		if (!sStarted) {
			sStarted = true;
			gMainTid.store(GetCurrentThreadId());
			if (const char* h = std::getenv("SMS_PORT_PROFILE_HITCH_MS"))
				kHitchMs = std::max(20.0, std::atof(h));
			CloseHandle(CreateThread(nullptr, 1 << 16, hitchSampler, nullptr, 0, nullptr));
		}
		gLastPresentAtomic.store(now);
	}
	row[COL_INTERVAL] = (double)(now - gLastPresent) * gMsPerTick;
	row[COL_BUSY]     = row[COL_INTERVAL] - row[PP_VIWAIT];
	gLastPresent      = now;
	++gFrames;
	++gFrameTotal;

	if (gLoadPending && gLoadReady) {
		gLoadPending = false;
		auto ms = [](int c) { return (double)gLoadAcc[c].load() * gMsPerTick; };
		std::fprintf(stderr,
		             "[prof] load: state %d area %d/%d: first stage frame presented %.1f ms after the state change "
		             "(stage ready at %.1f ms)\n",
		             gLoadState, gLoadArea, gLoadScenario, (double)(now - gLoadT0) * gMsPerTick,
		             (double)(gLoadReadyT - gLoadT0) * gMsPerTick);
		std::fprintf(stderr,
		             "[prof]   dvd %.1f ms (%lld reads, %.1f MB) | yaz0 %.1f ms (%lld files, incl. their reads) | "
		             "swap %.1f ms (%lld) | heap %.1f ms (%lld calls) | setupObjects %.1f ms | audio %.1f ms | main "
		             "asleep %.1f ms\n",
		             ms(PP_DVD), gLoadCnt[PP_DVD].load(), (double)gLoadBytes[PP_DVD].load() / 1048576.0, ms(PP_YAZ0),
		             gLoadCnt[PP_YAZ0].load(), ms(PP_SWAP), gLoadCnt[PP_SWAP].load(), ms(PP_HEAP),
		             gLoadCnt[PP_HEAP].load(), ms(PP_SETUP), ms(PP_AUDIO), ms(PP_VIWAIT));
		PortDvdCacheStats cs;
		port_dvd_cache_stats(&cs);
		std::fprintf(stderr, "[prof]   read-ahead: %lld hits, %lld misses, %lld waited for a prefetch, %lld chunks prefetched, %lld direct reads\n",
		             cs.hits - gLoadCache0.hits, cs.misses - gLoadCache0.misses, cs.waited - gLoadCache0.waited,
		             cs.prefetched - gLoadCache0.prefetched, cs.direct - gLoadCache0.direct);
		long long arq, held;
		port_arq_stats(&arq, &held);
		std::fprintf(stderr, "[prof]   ARQ: %lld completions, held for their poster %lld times (50 us re-checks)\n",
		             arq - gLoadArq0, held - gLoadArqHeld0);
	}

	if (gFrames == gN) {
		printSummary(now);
		gFrames      = 0;
		gWindowStart = now;
	}
}

void port_prof_stage(int state, int area, int scenario)
{
	if (!gPortProfile)
		return;
	gLoadPending  = true;
	gLoadReady    = false;
	gLoadT0       = port_prof_now();
	gLoadState    = state;
	gLoadArea     = area;
	gLoadScenario = scenario;
	port_dvd_cache_stats(&gLoadCache0);
	port_arq_stats(&gLoadArq0, &gLoadArqHeld0);
	for (int c = 0; c < PP_NCATS; ++c) {
		gLoadAcc[c].store(0);
		gLoadCnt[c].store(0);
		gLoadBytes[c].store(0);
	}
}

void port_prof_stage_ready(void)
{
	if (!gPortProfile || !gLoadPending || gLoadReady)
		return;
	gLoadReady  = true;
	gLoadReadyT = port_prof_now();
}

} // extern "C"
