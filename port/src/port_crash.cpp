// Crash reporter for the PC port: on an unhandled exception, prints the
// exception, the faulting address, the registers and a symbolized stack trace
// (from the PDB via dbghelp) to stderr, then exits.
// Also: port_print_backtrace() (used by OSPanic), and SMS_PORT_STACKS=<sec>,
// which dumps every thread's stack after that many seconds (hang diagnosis;
// this toolchain has no working debugger).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>

#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#pragma comment(lib, "dbghelp.lib")

// Crash/backtrace text goes to stderr and is also appended to sms_port_crash.log
// next to the exe, so a double-click run leaves a record.
static char sCrashText[32768]; // w12: everything crashOut printed (bug report)
static size_t sCrashTextLen;
static void crashOut(const char* fmt, ...)
{
	static FILE* sLog;
	static bool sTried;
	if (!sTried) {
		sTried = true;
		char path[MAX_PATH];
		DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
		if (n > 0 && n < MAX_PATH) {
			char* slash = std::strrchr(path, '\\');
			if (slash != nullptr && (size_t)(slash - path) + 20 < MAX_PATH) {
				std::strcpy(slash + 1, "sms_port_crash.log");
				sLog = std::fopen(path, "a");
			}
		}
	}
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (sLog != nullptr) {
		va_start(ap, fmt);
		std::vfprintf(sLog, fmt, ap);
		va_end(ap);
		std::fflush(sLog);
	}
	// w12: the same text for the bug report's log.txt (port_bugreport.cpp)
	va_start(ap, fmt);
	if (sCrashTextLen < sizeof(sCrashText) - 1) {
		const int n = std::vsnprintf(sCrashText + sCrashTextLen, sizeof(sCrashText) - sCrashTextLen, fmt, ap);
		if (n > 0)
			sCrashTextLen += (size_t)n < sizeof(sCrashText) - 1 - sCrashTextLen ? (size_t)n : sizeof(sCrashText) - 1 - sCrashTextLen;
	}
	va_end(ap);
}

#ifdef SMS_PORT_AURORA
extern "C" void port_bugreport_crash(const char* crashText); // port_bugreport.cpp (w12)
static void bugReportOnCrash() { port_bugreport_crash(sCrashText); }
#else
static void bugReportOnCrash() {}
#endif

extern "C" void port_crash_log(const char* fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	crashOut("%s", buf);
}

namespace {

void printFrame(HANDLE proc, DWORD64 addr, int i)
{
	alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
	SYMBOL_INFO* sym  = (SYMBOL_INFO*)buf;
	sym->SizeOfStruct = sizeof(SYMBOL_INFO);
	sym->MaxNameLen   = 255;
	DWORD64 disp      = 0;
	IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
	DWORD ldisp          = 0;
	const char* name     = SymFromAddr(proc, addr, &disp, sym) ? sym->Name : "?";
	if (SymGetLineFromAddr64(proc, addr, &ldisp, &line))
		crashOut("  #%02d %08llx %s+0x%llx  %s:%lu\n", i, (unsigned long long)addr,
		             name, (unsigned long long)disp, line.FileName, line.LineNumber);
	else
		crashOut("  #%02d %08llx %s+0x%llx\n", i, (unsigned long long)addr, name,
		             (unsigned long long)disp);
}

LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep)
{
	static volatile LONG once;
	if (InterlockedExchange(&once, 1))
		ExitProcess(0xC0000005);
	EXCEPTION_RECORD* er = ep->ExceptionRecord;
	std::fflush(stdout);
	crashOut("\n[port] CRASH: exception 0x%08lx at %p (thread %lu)",
	             er->ExceptionCode, er->ExceptionAddress, GetCurrentThreadId());
	if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
		crashOut(", %s address %p", er->ExceptionInformation[0] ? "writing" : "reading",
		             (void*)er->ExceptionInformation[1]);
	crashOut("\n");

	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(proc, nullptr, TRUE);
	CONTEXT ctx = *ep->ContextRecord;
	STACKFRAME64 sf = {};
#if defined(_M_X64) || defined(__x86_64__)
	// Registers, so the faulting instruction (llvm-objdump on the exe) can be
	// tied to a variable without a debugger.
	{
		HMODULE mod = GetModuleHandleA(nullptr);
		crashOut("  image base %p, rip = base+0x%llx\n", (void*)mod,
		             (unsigned long long)(ctx.Rip - (DWORD64)mod));
		const DWORD64 r[16] = { ctx.Rax, ctx.Rbx, ctx.Rcx, ctx.Rdx, ctx.Rsi, ctx.Rdi,
			                    ctx.Rbp, ctx.Rsp, ctx.R8,  ctx.R9,  ctx.R10, ctx.R11,
			                    ctx.R12, ctx.R13, ctx.R14, ctx.R15 };
		static const char* const n[16] = { "rax", "rbx", "rcx", "rdx", "rsi", "rdi",
			                               "rbp", "rsp", "r8",  "r9",  "r10", "r11",
			                               "r12", "r13", "r14", "r15" };
		for (int i = 0; i < 16; i++)
			crashOut("  %-3s %016llx%s", n[i], (unsigned long long)r[i],
			             (i % 4 == 3) ? "\n" : "");
	}
	DWORD machine       = IMAGE_FILE_MACHINE_AMD64;
	sf.AddrPC.Offset    = ctx.Rip;
	sf.AddrFrame.Offset = ctx.Rbp;
	sf.AddrStack.Offset = ctx.Rsp;
#else
	DWORD machine       = IMAGE_FILE_MACHINE_I386;
	sf.AddrPC.Offset    = ctx.Eip;
	sf.AddrFrame.Offset = ctx.Ebp;
	sf.AddrStack.Offset = ctx.Esp;
#endif
	sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
	for (int i = 0; i < 40; i++) {
		if (!StackWalk64(machine, proc, GetCurrentThread(), &sf, &ctx, nullptr,
		                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
			break;
		if (!sf.AddrPC.Offset)
			break;
		printFrame(proc, sf.AddrPC.Offset, i);
	}
	// w12: the bug report (video + log + replay) now, with the backtrace above; the stack scan
	// below reads past the walked frames and has been seen to end the process on its own.
	bugReportOnCrash();
	// A jump to a bad address (data, null) leaves StackWalk with a single
	// frame; scan the top of the stack for plausible return addresses too.
	{
		HMODULE mod = GetModuleHandleA(nullptr);
		DWORD64 lo  = (DWORD64)mod;
		DWORD64 hi  = lo + ((PIMAGE_NT_HEADERS)((char*)mod + ((PIMAGE_DOS_HEADER)mod)->e_lfanew))
		                       ->OptionalHeader.SizeOfImage;
#if defined(_M_X64) || defined(__x86_64__)
		DWORD64* sp = (DWORD64*)ctx.Rsp;
#else
		DWORD* sp = (DWORD*)ctx.Esp;
#endif
		crashOut("  stack scan:\n");
		int shown = 0;
		__try {
			for (int i = 0; i < 256 && shown < 12; i++) {
				DWORD64 v = sp[i];
				if (v >= lo && v < hi) {
					printFrame(proc, v, i);
					shown++;
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	std::fflush(stderr);
	ExitProcess(er->ExceptionCode);
	return EXCEPTION_EXECUTE_HANDLER;
}

#if defined(_M_X64) || defined(__x86_64__)
// Hang diagnosis without a debugger: with SMS_PORT_STACKS=<seconds> set, a
// host thread waits that long, then suspends every other thread of the
// process, prints each one's symbolized stack and exits.
void dumpThread(HANDLE proc, HANDLE th, DWORD tid)
{
	CONTEXT ctx      = {};
	ctx.ContextFlags = CONTEXT_FULL;
	if (!GetThreadContext(th, &ctx))
		return;
	crashOut("[port] thread %lu:\n", tid);
	STACKFRAME64 sf    = {};
	sf.AddrPC.Offset    = ctx.Rip;
	sf.AddrFrame.Offset = ctx.Rbp;
	sf.AddrStack.Offset = ctx.Rsp;
	sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
	for (int i = 0; i < 24; i++) {
		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &ctx, nullptr,
		                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr)
		    || !sf.AddrPC.Offset)
			break;
		printFrame(proc, sf.AddrPC.Offset, i);
	}
}

DWORD WINAPI stackDumper(LPVOID p)
{
	Sleep((DWORD)(uintptr_t)p * 1000);
	HANDLE proc = GetCurrentProcess();
	DWORD self  = GetCurrentThreadId();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(proc, nullptr, TRUE);
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	THREADENTRY32 te = { sizeof(te) };
	HANDLE ths[128];
	DWORD ids[128];
	int n = 0;
	for (BOOL ok = Thread32First(snap, &te); ok && n < 128; ok = Thread32Next(snap, &te)) {
		if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == self)
			continue;
		HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
		if (!th)
			continue;
		SuspendThread(th);
		ths[n]  = th;
		ids[n++] = te.th32ThreadID;
	}
	CloseHandle(snap);
	std::fflush(stdout);
	crashOut("\n[port] SMS_PORT_STACKS: %d threads\n", n);
	for (int i = 0; i < n; i++)
		dumpThread(proc, ths[i], ids[i]);
	std::fflush(stderr);
	ExitProcess(3);
	return 0;
}
#endif

} // namespace

#if defined(_M_X64) || defined(__x86_64__)
// Suspends thread `tid` and prints its stack (e.g. the game thread when an Aurora
// worker thread hits a fatal error). The thread stays suspended: callers exit next.
extern "C" void port_dump_thread(unsigned long tid)
{
	if (tid == GetCurrentThreadId())
		return;
	HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
	if (!th)
		return;
	SuspendThread(th);
	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(proc, nullptr, TRUE);
	dumpThread(proc, th, tid);
	std::fflush(stderr);
}
#endif

extern "C" void port_print_backtrace(void)
{
	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(proc, nullptr, TRUE);
	void* frames[32];
	USHORT n = CaptureStackBackTrace(1, 32, frames, nullptr);
	crashOut("[port] backtrace:\n");
	for (USHORT i = 0; i < n; i++)
		printFrame(proc, (DWORD64)frames[i], i);
	std::fflush(stderr);
}

// SMS_PORT_PROFILE=2 (sdk_os_thread.cpp): frames captured earlier (where a long stretch began).
extern "C" void port_print_frames(void* const* frames, int n)
{
	HANDLE proc = GetCurrentProcess();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(proc, nullptr, TRUE);
	for (int i = 0; i < n; i++)
		printFrame(proc, (DWORD64)frames[i], i);
	std::fflush(stderr);
}

// Stack-smash finder: with SMS_PORT_GS_DEBUG=1 set while running build_aurora.py,
// game/JSystem units are built with -fstack-protector-all and their
// __security_check_cookie calls are renamed to this, which names the function
// whose frame was overrun (the CRT's version fast-fails without a trace).
// clang passes the cookie XORed with the caller's rsp (MSVC scheme).
extern "C" uintptr_t __security_cookie;
extern "C" void sms_gs_check(uintptr_t cookie)
{
	uintptr_t callerSp = (uintptr_t)_AddressOfReturnAddress() + 8;
	if ((cookie ^ callerSp) == __security_cookie || cookie == __security_cookie)
		return;
	std::fflush(stdout);
	crashOut("\n[port] STACK COOKIE SMASHED (got %016llx), frame #00 is the victim:\n",
	             (unsigned long long)cookie);
	port_print_backtrace();
	ExitProcess(0xC0000409);
}

#if defined(_M_X64) || defined(__x86_64__)
// Memory-corruption finder: SMS_PORT_WATCH=<hex address> puts a hardware write
// watchpoint (DR0, 4 bytes) on that address in every thread of the process (a helper
// thread re-arms new threads every 50 ms) and prints a backtrace for the first hits.
// Frames presented so far; the Aurora build's port_main.cpp counts them (stays 0 in
// build_port.py).
// port_frame_count() is also the clock Application.cpp's SMS_PORT_TRACE prints.
extern "C" volatile unsigned gPortFrameCount = 0;
extern "C" unsigned port_frame_count(void) { return gPortFrameCount; }
// SMS_PORT_PADSCRIPT "G<x>;<z>" autopilot: port_main.cpp sets the target (active, x, z),
// Application.cpp's portAutopilot computes the stick. Here so both port builds link.
extern "C" int gPortGoto[3]      = {};
extern "C" int gPortGotoStick[2] = {};
// Frame at which the current stage first gave Mario control (TMarDirector state 4);
// 0 while none has. Set by Application.cpp, read by PADSCRIPT "+<frame>" entries.
extern "C" unsigned gPortStageFrame = 0;
// How many stages have given Mario control so far in this run (PADSCRIPT "+<n>@").
extern "C" int gPortStageCount = 0;
// Frame the latest demo/talk ended, and how many have (PADSCRIPT "+d<frame>").
extern "C" unsigned gPortDemoEndFrame = 0;
extern "C" int gPortDemoEndCount      = 0;
// Game-state bits for PADSCRIPT "?<bit>"/"!<bit>" conditions, published by small SMS_PORT
// hooks in game code (gatekeeper.cpp): bit 0 = a gatekeeper (goop Piranha) head is
// vulnerable, bit 1 = a gatekeeper is asleep in its goop, bit 2 = it is defeated,
// bits 8-15 = its hit points. Cleared on every app state change (Application.cpp).
extern "C" volatile unsigned gPortCond = 0;
namespace {
DWORD64 sWatchAddr;
volatile LONG sWatchHits;
bool sWatchNeedsBuffer; // SMS_PORT_WATCH_GD=1: wait until a GD display list is built over the address
volatile bool sWatchGo;
} // namespace

// Called by port_main.cpp's GDInitGDLObj wrapper: arms the watch (SMS_PORT_WATCH_GD=1)
// once a display list buffer covering the watched address is initialised.
extern "C" void port_watch_note_buffer(const void* start, unsigned long length)
{
	if (sWatchAddr != 0 && (DWORD64)(uintptr_t)start <= sWatchAddr
	    && sWatchAddr < (DWORD64)(uintptr_t)start + length)
		sWatchGo = true;
}

namespace {

LONG WINAPI watchHandler(EXCEPTION_POINTERS* ep)
{
	if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1))
		return EXCEPTION_CONTINUE_SEARCH;
	ep->ContextRecord->Dr6 = 0;
	if (InterlockedIncrement(&sWatchHits) <= 24) {
		unsigned v = *(volatile unsigned*)(uintptr_t)sWatchAddr;
		crashOut("\n[port] WATCH: write to %p (now %08x) by thread %lu at %p\n", (void*)sWatchAddr, v,
		         GetCurrentThreadId(), (void*)ep->ContextRecord->Rip);
		HANDLE proc = GetCurrentProcess();
		SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
		SymInitialize(proc, nullptr, TRUE);
		CONTEXT ctx     = *ep->ContextRecord;
		STACKFRAME64 sf = {};
		sf.AddrPC.Offset    = ctx.Rip;
		sf.AddrFrame.Offset = ctx.Rbp;
		sf.AddrStack.Offset = ctx.Rsp;
		sf.AddrPC.Mode = sf.AddrFrame.Mode = sf.AddrStack.Mode = AddrModeFlat;
		for (int i = 0; i < 16; i++) {
			if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, GetCurrentThread(), &sf, &ctx, nullptr,
			                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr)
			    || !sf.AddrPC.Offset)
				break;
			printFrame(proc, sf.AddrPC.Offset, i);
		}
		std::fflush(stderr);
	}
	return EXCEPTION_CONTINUE_EXECUTION;
}

DWORD WINAPI watchArmer(LPVOID)
{
	DWORD armed[256];
	int numArmed = 0;
	// SMS_PORT_WATCH_FRAME=<n>: arm only from frame n (port_main.cpp's frame counter),
	// to skip earlier, legitimate users of the address.
	unsigned fromFrame = 0;
	if (const char* f = std::getenv("SMS_PORT_WATCH_FRAME"))
		fromFrame = (unsigned)std::strtoul(f, nullptr, 10);
	while (port_frame_count() < fromFrame || (sWatchNeedsBuffer && !sWatchGo))
		Sleep(1);
	crashOut("[port] WATCH armed on %p at frame %u\n", (void*)sWatchAddr, port_frame_count());
	for (;;) {
		HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
		THREADENTRY32 te = { sizeof(te) };
		for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
			if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId())
				continue;
			bool done = false;
			for (int i = 0; i < numArmed; i++)
				if (armed[i] == te.th32ThreadID)
					done = true;
			if (done || numArmed >= 256)
				continue;
			HANDLE th = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
			if (!th)
				continue;
			SuspendThread(th);
			CONTEXT c      = {};
			c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
			if (GetThreadContext(th, &c)) {
				c.Dr0 = sWatchAddr;
				c.Dr7 = (c.Dr7 & ~0xF0003ull) | 1ull | (1ull << 16) | (3ull << 18); // L0, write, 4 bytes
				SetThreadContext(th, &c);
				armed[numArmed++] = te.th32ThreadID;
			}
			ResumeThread(th);
			CloseHandle(th);
		}
		CloseHandle(snap);
		Sleep(50);
	}
}
} // namespace
#endif

namespace {

// abort(), std::terminate, CRT invalid-parameter and pure-virtual-call failures end
// in __fastfail (exit code 0xC0000409) without going through the unhandled-exception
// filter, so they left no trace (seen once in the smoke suite, w10). Print the same
// "[port] CRASH" line and a backtrace first. All CRT users in the exe (game, port,
// Aurora/Dawn static libs) share the UCRT DLL, so one registration covers them.
[[noreturn]] void fatalTrace(const char* what)
{
	std::fflush(stdout);
	crashOut("\n[port] CRASH: %s (thread %lu)\n", what, GetCurrentThreadId());
	port_print_backtrace();
	std::fflush(stderr);
	bugReportOnCrash(); // w12
	ExitProcess(0xC0000409);
}
void onAbortSignal(int) { fatalTrace("abort() / SIGABRT"); }
void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
{
	fatalTrace("CRT invalid parameter");
}
void onPureCall() { fatalTrace("pure virtual function call"); }

#pragma init_seg(lib)
struct CrashInstall {
	CrashInstall()
	{
		SetUnhandledExceptionFilter(crashFilter);
		std::signal(SIGABRT, onAbortSignal);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
		_set_invalid_parameter_handler(onInvalidParameter);
		_set_purecall_handler(onPureCall);
#if defined(_M_X64) || defined(__x86_64__)
		if (const char* w = std::getenv("SMS_PORT_WATCH")) {
			sWatchAddr = std::strtoull(w, nullptr, 16) & ~3ull;
			sWatchNeedsBuffer = std::getenv("SMS_PORT_WATCH_GD") != nullptr;
			if (sWatchAddr != 0) {
				AddVectoredExceptionHandler(1, watchHandler);
				CloseHandle(CreateThread(nullptr, 0, watchArmer, nullptr, 0, nullptr));
			}
		}
		if (const char* s = std::getenv("SMS_PORT_STACKS"))
			if (int sec = std::atoi(s))
				CloseHandle(CreateThread(nullptr, 0, stackDumper, (LPVOID)(uintptr_t)sec, 0,
				                         nullptr));
#endif
	}
} sCrashInstall;

} // namespace
