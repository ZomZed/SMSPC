// Sampling profiler for the PC port (w12, debug only): SMS_PORT_PROFILE_SAMPLE=<hz>
// (with SMS_PORT_PROFILE on). A host thread suspends the game's main thread and
// Aurora's "FIFO processor" / "render worker" threads <hz> times a second (default 1000),
// unwinds each with RtlVirtualUnwind (no dbghelp while the thread is stopped) and
// resumes it. Every SMS_PORT_PROFILE_SAMPLE_S seconds (default 10) it prints, per thread,
// the functions with the most samples where they were running ("self") and the
// functions most often anywhere on the stack ("incl"), symbolized through the PDB
// (port_symname.cpp). Off unless the variable is set: nothing here runs otherwise.
#ifdef SMS_PORT_AURORA

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" int port_sym_name(const void* addr, char* out, size_t n); // port_symname.cpp

namespace {

struct Counts {
	uint64_t self = 0, incl = 0;
};

struct Target {
	const char* label;
	HANDLE th = nullptr;
	DWORD tid = 0;
	uint64_t samples = 0;
	std::unordered_map<uint64_t, Counts> fn; // function entry (or pc) -> counts
};

constexpr int kMaxDepth = 48;
Target sTargets[3] = { { "main" }, { "Aurora FIFO processor" }, { "Aurora render worker" } };
int sHz = 1000;
double sSummaryS = 10.0;
DWORD sMainTid;

// Unwind one suspended thread: function entry addresses from the leaf outwards.
int unwind(HANDLE th, uint64_t* out, int max)
{
	CONTEXT ctx;
	std::memset(&ctx, 0, sizeof(ctx));
	ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
	if (!GetThreadContext(th, &ctx))
		return 0;
	int n = 0;
	for (int i = 0; i < max; ++i) {
		const DWORD64 pc = ctx.Rip;
		if (pc == 0)
			break;
		DWORD64 base = 0;
		RUNTIME_FUNCTION* f = RtlLookupFunctionEntry(pc, &base, nullptr);
		out[n++] = f != nullptr ? base + f->BeginAddress : pc;
		if (f == nullptr) {
			// leaf function without unwind data: the return address is at [rsp]
			DWORD64 ret = 0;
			SIZE_T got  = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), (LPCVOID)ctx.Rsp, &ret, sizeof(ret), &got) || got != sizeof(ret))
				break;
			ctx.Rip = ret;
			ctx.Rsp += 8;
		} else {
			void* handler  = nullptr;
			DWORD64 estab  = 0;
			RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, pc, f, &ctx, &handler, &estab, nullptr);
		}
	}
	return n;
}

void sampleTarget(Target& t)
{
	if (t.th == nullptr)
		return;
	uint64_t frames[kMaxDepth];
	if (SuspendThread(t.th) == (DWORD)-1)
		return;
	const int n = unwind(t.th, frames, kMaxDepth);
	ResumeThread(t.th);
	if (n == 0)
		return;
	++t.samples;
	t.fn[frames[0]].self++;
	for (int i = 0; i < n; ++i) {
		bool dup = false;
		for (int j = 0; j < i; ++j)
			if (frames[j] == frames[i]) {
				dup = true;
				break;
			}
		if (!dup)
			t.fn[frames[i]].incl++;
	}
}

// Finds Aurora's worker threads by their descriptions (set by aurora::thread::Thread).
void refreshTargets()
{
	typedef HRESULT(WINAPI * GetDescFn)(HANDLE, PWSTR*);
	static GetDescFn sGetDesc = (GetDescFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
	if (sTargets[0].th == nullptr && sMainTid != 0) {
		sTargets[0].tid = sMainTid;
		sTargets[0].th  = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, sMainTid);
	}
	if (sGetDesc == nullptr || (sTargets[1].th != nullptr && sTargets[2].th != nullptr))
		return;
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return;
	THREADENTRY32 te = { sizeof(te) };
	for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
		if (te.th32OwnerProcessID != GetCurrentProcessId())
			continue;
		HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
		if (th == nullptr)
			continue;
		PWSTR desc = nullptr;
		bool used  = false;
		if (SUCCEEDED(sGetDesc(th, &desc)) && desc != nullptr) {
			char name[64];
			std::snprintf(name, sizeof(name), "%ls", desc);
			LocalFree(desc);
			for (int i = 1; i < 3; ++i)
				if (sTargets[i].th == nullptr && std::strcmp(name, sTargets[i].label) == 0) {
					sTargets[i].th  = th;
					sTargets[i].tid = te.th32ThreadID;
					used            = true;
				}
		}
		if (!used)
			CloseHandle(th);
	}
	CloseHandle(snap);
}

const char* symName(uint64_t addr)
{
	static std::unordered_map<uint64_t, std::string> sCache;
	auto it = sCache.find(addr);
	if (it != sCache.end())
		return it->second.c_str();
	char buf[256];
	port_sym_name((const void*)addr, buf, sizeof(buf));
	return sCache.emplace(addr, buf).first->second.c_str();
}

void summary()
{
	for (Target& t : sTargets) {
		if (t.samples == 0)
			continue;
		std::vector<std::pair<uint64_t, Counts>> v(t.fn.begin(), t.fn.end());
		std::fprintf(stderr, "[samp] ---- %s (tid %lu): %llu samples at %d Hz\n", t.label, t.tid,
		             (unsigned long long)t.samples, sHz);
		std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.self > b.second.self; });
		std::fprintf(stderr, "[samp]  self%%  function\n");
		for (size_t i = 0; i < v.size() && i < 40; ++i) {
			if (v[i].second.self * 200 < t.samples) // below 0.5%
				break;
			std::fprintf(stderr, "[samp]  %5.1f  %s\n", 100.0 * (double)v[i].second.self / (double)t.samples,
			             symName(v[i].first));
		}
		std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second.incl > b.second.incl; });
		std::fprintf(stderr, "[samp]  incl%%  function\n");
		for (size_t i = 0; i < v.size() && i < 70; ++i) {
			if (v[i].second.incl * 100 < t.samples) // below 1%
				break;
			std::fprintf(stderr, "[samp]  %5.1f  %s\n", 100.0 * (double)v[i].second.incl / (double)t.samples,
			             symName(v[i].first));
		}
		t.samples = 0;
		t.fn.clear();
	}
	std::fflush(stderr);
}

DWORD WINAPI samplerMain(LPVOID)
{
	LARGE_INTEGER f, t0, t;
	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	const long long period = f.QuadPart / sHz;
	long long next = t0.QuadPart + period, nextSummary = t0.QuadPart + (long long)(sSummaryS * (double)f.QuadPart);
	long long nextRefresh = t0.QuadPart;
	// a high-resolution timer for the sampling period (Sleep granularity is too coarse)
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	for (;;) {
		QueryPerformanceCounter(&t);
		if (t.QuadPart >= nextRefresh) {
			refreshTargets();
			nextRefresh = t.QuadPart + 2 * f.QuadPart;
		}
		for (Target& tg : sTargets)
			sampleTarget(tg);
		if (t.QuadPart >= nextSummary) {
			summary();
			nextSummary = t.QuadPart + (long long)(sSummaryS * (double)f.QuadPart);
		}
		next += period;
		QueryPerformanceCounter(&t);
		if (next > t.QuadPart) {
			LARGE_INTEGER due;
			due.QuadPart = -((next - t.QuadPart) * 10000000LL / f.QuadPart);
			if (timer != nullptr && due.QuadPart < 0) {
				SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
				WaitForSingleObject(timer, 5);
			} else {
				Sleep(1);
			}
		} else {
			next = t.QuadPart; // fell behind (summary): do not catch up in a burst
		}
	}
	return 0;
}

} // namespace

// Called once from the main game thread (port_profile.cpp, first presented frame).
extern "C" void port_sampler_start(void)
{
	const char* e = std::getenv("SMS_PORT_PROFILE_SAMPLE");
	if (e == nullptr || e[0] == '\0' || e[0] == '0')
		return;
	sHz = std::clamp(std::atoi(e), 50, 4000);
	if (const char* s = std::getenv("SMS_PORT_PROFILE_SAMPLE_S"))
		sSummaryS = std::max(1.0, std::atof(s));
	sMainTid = GetCurrentThreadId();
	std::fprintf(stderr, "[samp] sampling main / FIFO / render threads at %d Hz, summary every %.0f s\n", sHz, sSummaryS);
	CloseHandle(CreateThread(nullptr, 1 << 18, samplerMain, nullptr, 0, nullptr));
}

#endif // SMS_PORT_AURORA
