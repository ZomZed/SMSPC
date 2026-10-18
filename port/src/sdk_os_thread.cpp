// Host implementation of the Dolphin SDK OS threading layer for the PC port:
// threads and the scheduler, interrupts, message queues, mutexes, condition
// variables and alarms. (Aurora provides the rest of OS but not this.)
//
// Machine model
// -------------
// * CPU: the GameCube has one core with a strict-priority scheduler. Every
//   OSThread is a host thread, but only the thread in gRunning executes game
//   code; all others wait on their own condition variable ("one CPU").
//   Switches happen exactly where the SDK would reschedule: blocking calls,
//   OSYieldThread, resume/wakeup of a higher-priority thread, and on return
//   from interrupts (OSRestoreInterrupts/OSEnableInterrupts re-enabling, and
//   port_os_poll()).
// * Interrupts: handlers (alarms, VI retrace, AI DMA, DSP mail, ...) run on a
//   host "interrupt thread" while holding gIntr. OSDisableInterrupts takes
//   gIntr for the calling thread, so a critical section excludes handlers
//   exactly like MSR[EE]=0. The interrupt state is per thread and is dropped
//   while the thread is switched out, as the PowerPC MSR is. A handler that
//   readies a higher-priority thread requests a switch that the running thread
//   takes at its next interrupt-enabled scheduling point (we cannot preempt a
//   host thread at an arbitrary instruction).
// * Time: alarms are kept on the host performance counter; OSTime arguments
//   are converted relative to OSGetTime(), so any OSGetTime implementation
//   with the GameCube timebase rate (bus clock / 4) works.
//
// Lock order: gIntr -> gSched, gIntr -> gAlarmLock. Never the reverse.
// Pointer-size clean (builds for i686 and x86_64).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmsystem.h>

#include "port_sdk.h"
#include "port_profile.h" // w14: SMS_PORT_PROFILE hooks (handoff / interrupt-lock waits)

#include <dolphin/os.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" void OSSetThreadPriority(OSThread* thread, OSPriority priority);

namespace {

const long long kTimerClock = PORT_BUS_CLOCK / 4; // GameCube timebase, 40.5 MHz

// ---------------------------------------------------------------------------
// Scheduler state
// ---------------------------------------------------------------------------
struct HostThread {
	OSThread* t;
	CONDITION_VARIABLE cv;
	bool intrDisabled; // this thread's MSR[EE]==0 (it holds gIntr)
	volatile bool cancelled;
	void* (*func)(void*);
	void* param;
	unsigned long long seq; // FIFO order among equal priorities
	bool isMain;
	long long dispatchT; // w14 SMS_PORT_PROFILE: when dispatch() handed this thread the CPU
	long long readyT;    // w14 SMS_PORT_PROFILE: when makeReady() made it runnable
	long long runSince;  // w14 SMS_PORT_PROFILE: when it last got the CPU
	bool inPlace;        // w14: in port_preemptible_begin/end (host work, preemptible at once)
	unsigned long hostTid; // w14: host thread id (SMS_PORT_PROFILE=3 hitch sampler)
	volatile unsigned schedEpoch; // w14: scheduling points reached (port_thread_sched_epoch)
};

SRWLOCK gSched            = SRWLOCK_INIT;
SRWLOCK gIntr             = SRWLOCK_INIT;
SRWLOCK gAlarmLock        = SRWLOCK_INIT;

// w14 SMS_PORT_PROFILE: gIntr acquisition that records the wait when contended.
inline void acquireIntr()
{
	if (!gPortProfile) {
		AcquireSRWLockExclusive(&gIntr);
		return;
	}
	if (TryAcquireSRWLockExclusive(&gIntr))
		return;
	long long t0 = port_prof_now();
	AcquireSRWLockExclusive(&gIntr);
	port_prof_add(PP_INTRWAIT, port_prof_now() - t0);
}

OSThread* gRunning;
// w14: highest (numerically) priority allowed to preempt a port_preemptible_begin
// window: JUTException 0, JASystem audio 2 and DVD 3, JKRAram/JKRAramStream/JKRDecomp 8.
// None of them issues GX commands.
const s32 kInPlaceMaxPrio = 9;
volatile LONG gPreemptPending;
int gSchedDisable;
const int kMaxThreads = 64;
OSThread* gThreads[kMaxThreads]; // all live (not moribund) threads
int gNumThreads;
unsigned long long gSeq;

__declspec(thread) OSThread* tSelf;
__declspec(thread) int tIrq;

OSThread sMainThread;
HostThread sMainRec;
bool gInit;

LARGE_INTEGER gQpcFreq;

// The host record is stashed in the (otherwise unused) saved-register area
// of the OSThread's context, split over two words so it fits on 64-bit.
inline HostThread* REC(OSThread* t)
{
	uintptr_t v = (uintptr_t)t->context.gpr[0];
#if UINTPTR_MAX > 0xFFFFFFFFu
	v |= (uintptr_t)t->context.gpr[1] << 32;
#endif
	return (HostThread*)v;
}
inline void setRec(OSThread* t, HostThread* r)
{
	uintptr_t v       = (uintptr_t)r;
	t->context.gpr[0] = (u32)(v & 0xFFFFFFFFu);
#if UINTPTR_MAX > 0xFFFFFFFFu
	t->context.gpr[1] = (u32)(v >> 32);
#else
	t->context.gpr[1] = 0;
#endif
}

void onIdle(); // deferred device completions (below, port_retrace_defer)

inline void schedLock() { AcquireSRWLockExclusive(&gSched); }
inline void schedUnlock() { ReleaseSRWLockExclusive(&gSched); }

// Host clock in timebase ticks (arbitrary epoch).
long long hostTicks()
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	long long f = gQpcFreq.QuadPart;
	return (c.QuadPart / f) * kTimerClock + ((c.QuadPart % f) * kTimerClock) / f;
}

inline bool runnable(OSThread* t)
{
	return (t->state == OS_THREAD_STATE_READY || t->state == OS_THREAD_STATE_RUNNING)
	       && t->suspend <= 0;
}

void addThread(OSThread* t)
{
	for (int i = 0; i < gNumThreads; i++)
		if (gThreads[i] == t)
			return;
	if (gNumThreads == kMaxThreads) {
		std::fprintf(stderr, "[port] too many threads\n");
		std::abort();
	}
	gThreads[gNumThreads++] = t;
}

void removeThread(OSThread* t)
{
	for (int i = 0; i < gNumThreads; i++)
		if (gThreads[i] == t) {
			gThreads[i] = gThreads[--gNumThreads];
			return;
		}
}

OSThread* pickNext()
{
	OSThread* best = nullptr;
	for (int i = 0; i < gNumThreads; i++) {
		OSThread* t = gThreads[i];
		if (!runnable(t))
			continue;
		if (!best || t->priority < best->priority
		    || (t->priority == best->priority && REC(t)->seq < REC(best)->seq))
			best = t;
	}
	return best;
}

void makeReady(OSThread* t)
{
	t->state    = OS_THREAD_STATE_READY;
	REC(t)->seq = ++gSeq;
	if (gPortProfile) // w14: start of its wait for the CPU
		REC(t)->readyT = port_prof_now();
}

// w14 SMS_PORT_PROFILE=2: set by dispatch() on the thread giving up the CPU when a
// higher-priority thread had to wait > 4 ms for it; checkPreempt prints it.
__declspec(thread) long long tPreemptLog;
// SMS_PORT_PROFILE=2: every wait of an audio/loader thread (priority <= 9) for the CPU, by the
// call chain that ended it; checkPreempt prints the biggest every 15 s.
struct WaitKey {
	void* fr[4];
	bool self; // the thread giving up the CPU ended it itself (else another thread dispatched)
	int prio;  // the waiting thread
};
struct WaitBin {
	WaitKey k;
	long long t;
	unsigned n;
	bool used;
};
WaitBin gWaitBins[64];
long long gWaitTotal;
void waitTally(const WaitKey& k, long long t) // gSched held
{
	gWaitTotal += t;
	for (WaitBin& b : gWaitBins) {
		if (b.used && b.k.self == k.self && b.k.prio == k.prio && std::memcmp(b.k.fr, k.fr, sizeof(k.fr)) == 0) {
			b.t += t;
			b.n++;
			return;
		}
		if (!b.used) {
			b.used = true;
			b.k    = k;
			b.t    = t;
			b.n    = 1;
			return;
		}
	}
}

void dispatch(OSThread* next)
{
	OSThread* prev = gRunning;
	gRunning       = next;
	if (next) {
		next->state = OS_THREAD_STATE_RUNNING;
		if (gPortProfile) {
			// w14 SMS_PORT_PROFILE: "preempt" = how long a readied thread waited for a
			// LOWER-priority thread to reach a scheduling point (the GameCube switches at
			// the interrupt); "handoff" (waitForCpu) = the host thread switch itself.
			HostThread* nr = REC(next);
			long long now  = port_prof_now();
			nr->dispatchT  = now;
			if (prev != nullptr && prev != next && next->priority < prev->priority && nr->readyT != 0) {
				// only the part of the wait during prev's current turn on the CPU
				long long from = nr->readyT > REC(prev)->runSince ? nr->readyT : REC(prev)->runSince;
				port_prof_add(PP_PREEMPT, now - from);
				if (gPortProfile == 2 && (now - from) * 1000 > 4 * gQpcFreq.QuadPart && prev == tSelf)
					tPreemptLog = now - from;
				if (gPortProfile == 2 && next->priority <= kInPlaceMaxPrio) { // every audio/loader wait, by the call chain that ended it (printed by checkPreempt)
					void* fr[5];
					const int got = CaptureStackBackTrace(1, 5, fr, nullptr);
					WaitKey k     = { { got > 0 ? fr[0] : nullptr, got > 1 ? fr[1] : nullptr, got > 2 ? fr[2] : nullptr,
					                    got > 3 ? fr[3] : nullptr },
					                  prev == tSelf, (int)next->priority };
					waitTally(k, now - from);
				}
			}
			nr->readyT   = 0;
			nr->runSince = now;
		}
		WakeConditionVariable(&REC(next)->cv);
	}
}

// gSched held. Waits until this thread owns the CPU again.
void waitForCpu(HostThread* r)
{
	bool waited = false;
	while (gRunning != r->t && !r->cancelled) {
		SleepConditionVariableSRW(&r->cv, &gSched, INFINITE, 0);
		waited = true;
	}
	if (waited && r->dispatchT != 0) { // w14 SMS_PORT_PROFILE: dispatch -> running latency
		port_prof_add(PP_HANDOFF, port_prof_now() - r->dispatchT);
		r->dispatchT = 0;
	}
	if (r->cancelled) {
		schedUnlock();
		ExitThread(0);
	}
}

// gSched held; self->state already updated (READY = still runnable unless
// suspended, WAITING = blocked). Gives the CPU to the best runnable thread.
void switchOut(OSThread* self)
{
	OSThread* next = pickNext();
	if (next == self) {
		self->state = OS_THREAD_STATE_RUNNING;
		return;
	}
	HostThread* r = REC(self);
	dispatch(next);
	if (next == nullptr)
		onIdle();
	bool had = r->intrDisabled;
	if (had)
		ReleaseSRWLockExclusive(&gIntr);
	waitForCpu(r);
	if (had) {
		schedUnlock();
		acquireIntr();
		schedLock();
	}
}

// gSched held, game context.
void reschedule(OSThread* self)
{
	if (gSchedDisable > 0 || !self)
		return;
	if (!runnable(self)) {
		switchOut(self);
		return;
	}
	OSThread* n = pickNext();
	if (n && n != self && n->priority < self->priority) {
		makeReady(self);
		switchOut(self);
	}
}

// gSched held. Call after threads may have become ready.
void afterReady()
{
	if (tIrq || !tSelf) {
		if (!gRunning) {
			dispatch(pickNext());
		} else {
			OSThread* n = pickNext();
			if (n && n != gRunning && n->priority < gRunning->priority && n->priority <= kInPlaceMaxPrio
			    && REC(gRunning)->inPlace) {
				// w14: the running thread is in host-only work (port_preemptible_begin):
				// switch at once, as the GameCube's interrupt return would.
				makeReady(gRunning);
				dispatch(n);
			} else if (n && n != gRunning
			           && (n->priority < gRunning->priority || !runnable(gRunning))) {
				InterlockedExchange(&gPreemptPending, 1);
			}
		}
		return;
	}
	reschedule(tSelf);
}

extern "C" void port_print_frames(void* const* frames, int n); // port_crash.cpp
// SMS_PORT_PROFILE=2: the last scheduling point passed while nothing was waiting (where a
// long stretch that a higher-priority thread had to wait out began, roughly)
__declspec(thread) void* tLastPoint[12];
__declspec(thread) int tLastPointN;

// Game context, interrupts enabled: take a pending "interrupt return" switch.
void checkPreempt()
{
	if (tSelf && !tIrq)
		++REC(tSelf)->schedEpoch; // w14: see port_thread_sched_epoch
	if (gPortProfile == 2 && !gPreemptPending && tSelf && !tIrq)
		tLastPointN = CaptureStackBackTrace(1, 12, tLastPoint, nullptr);
	if (!gPreemptPending || tIrq || !tSelf)
		return;
	if (REC(tSelf)->intrDisabled)
		return;
	schedLock();
	InterlockedExchange(&gPreemptPending, 0);
	tPreemptLog = 0;
	reschedule(tSelf);
	schedUnlock();
	if (gPortProfile == 2) { // the wait tally (dispatch), every 15 s
		static long long sLastPrint;
		const long long now = port_prof_now();
		if (sLastPrint == 0)
			sLastPrint = now;
		if (now - sLastPrint > 15 * gQpcFreq.QuadPart) {
			sLastPrint = now;
			WaitBin bins[64];
			long long total;
			schedLock();
			std::memcpy(bins, gWaitBins, sizeof(bins));
			total = gWaitTotal;
			std::memset(gWaitBins, 0, sizeof(gWaitBins));
			gWaitTotal = 0;
			schedUnlock();
			std::fprintf(stderr, "[prof] preempt waits by the call chain that ended them (15 s, total %.0f ms):\n",
			             (double)total * 1000.0 / (double)gQpcFreq.QuadPart);
			for (int k = 0; k < 6; k++) {
				int best = -1;
				for (int j2 = 0; j2 < 64; j2++)
					if (bins[j2].used && bins[j2].t > 0 && (best < 0 || bins[j2].t > bins[best].t))
						best = j2;
				if (best < 0)
					break;
				std::fprintf(stderr, "[prof]   %.0f ms in %u waits of a priority %d thread (%s):\n", (double)bins[best].t * 1000.0 / (double)gQpcFreq.QuadPart,
				             bins[best].n, bins[best].k.prio, bins[best].k.self ? "the running thread switched" : "dispatched by another thread");
				port_print_frames(bins[best].k.fr, 4);
				bins[best].t = 0;
			}
		}
	}
	static int sLogged;
	if (tPreemptLog != 0 && sLogged < 20) { // w14 SMS_PORT_PROFILE=2: where long stretches end
		++sLogged;
		std::fprintf(stderr, "[prof] preemption delayed %.1f ms: thread prio %d ran on until here:\n",
		             (double)tPreemptLog * 1000.0 / (double)gQpcFreq.QuadPart, (int)tSelf->priority);
		port_print_backtrace();
		std::fprintf(stderr, "[prof] ... the stretch began after this scheduling point:\n");
		port_print_frames(tLastPoint, tLastPointN);
	}
}

// ---- thread queues (priority ordered, like the SDK's AddPrio) -------------
void queueRemove(OSThreadQueue* q, OSThread* t)
{
	OSThread* next = t->link.next;
	OSThread* prev = t->link.prev;
	if (next)
		next->link.prev = prev;
	else
		q->tail = prev;
	if (prev)
		prev->link.next = next;
	else
		q->head = next;
	t->link.next = t->link.prev = nullptr;
}

void queueAddPrio(OSThreadQueue* q, OSThread* t)
{
	OSThread* next = q->head;
	while (next && next->priority <= t->priority)
		next = next->link.next;
	if (!next) {
		OSThread* prev = q->tail;
		if (prev)
			prev->link.next = t;
		else
			q->head = t;
		t->link.prev = prev;
		t->link.next = nullptr;
		q->tail      = t;
	} else {
		OSThread* prev  = next->link.prev;
		t->link.next    = next;
		next->link.prev = t;
		t->link.prev    = prev;
		if (prev)
			prev->link.next = t;
		else
			q->head = t;
	}
}

// gSched held.
void sleepOn(OSThreadQueue* q)
{
	OSThread* self = tSelf;
	if (tIrq || !self) {
		std::fprintf(stderr, "[port] blocking call from interrupt context ignored\n");
		return;
	}
	self->state = OS_THREAD_STATE_WAITING;
	self->queue = q;
	queueAddPrio(q, self);
	switchOut(self);
}

// gSched held.
void wakeQueue(OSThreadQueue* q)
{
	OSThread* t = q->head;
	while (t) {
		OSThread* next = t->link.next;
		t->link.next = t->link.prev = nullptr;
		t->queue                    = nullptr;
		if (t->state == OS_THREAD_STATE_WAITING)
			makeReady(t);
		t = next;
	}
	q->head = q->tail = nullptr;
}

// w12 (performance on 2-4 core PCs): the host threads that carry game threads run above
// normal priority. Only one of them executes at a time (gRunning), so this never takes more
// than one core from Aurora's FIFO/render workers, Dawn's and the driver's threads; but a
// hand-off (the running game thread wakes the next one) is then taken by Windows at once
// instead of after the quantum of whichever worker occupies the other core. Measured with
// 2-core affinity: `handoff` 6-8 ms per frame -> see BOOTLOG "Performance (w12)".
// SMS_PORT_GAME_PRIO=0 keeps normal priority (A/B runs); =2 uses THREAD_PRIORITY_HIGHEST.
int gameHostPriority()
{
	static int sPrio = -2;
	if (sPrio == -2) {
		const char* e = std::getenv("SMS_PORT_GAME_PRIO");
		const int v   = e != nullptr ? std::atoi(e) : 1;
		sPrio         = v <= 0 ? THREAD_PRIORITY_NORMAL : v == 1 ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_HIGHEST;
	}
	return sPrio;
}

DWORD WINAPI threadEntry(LPVOID p)
{
	HostThread* r = (HostThread*)p;
	tSelf         = r->t;
	r->hostTid    = GetCurrentThreadId(); // w14
	if (gameHostPriority() != THREAD_PRIORITY_NORMAL) // w12
		SetThreadPriority(GetCurrentThread(), gameHostPriority());
	schedLock();
	waitForCpu(r);
	schedUnlock();
	void* ret = r->func(r->param);
	OSExitThread((OSThread*)ret); // the header declares the value as OSThread*
	return 0;
}

// ---------------------------------------------------------------------------
// Interrupt thread: alarms, one-shot and periodic device interrupts.
// Alarm "fire" fields hold host ticks (see hostTicks()).
// ---------------------------------------------------------------------------
struct Periodic {
	void (*fn)(void);
	long long period; // ticks
	long long next;
	bool enabled;
};
const int kMaxPeriodic = 8;
Periodic gPeriodic[kMaxPeriodic];
int gNumPeriodic;
OSAlarm* gAlarmHead;
OSContext gIrqContext;

// w14: the interrupt thread sleeps on a high-resolution waitable timer (Windows 10
// 1803+, else a normal waitable timer) plus a wake event, instead of a condition
// variable with a millisecond timeout. The millisecond timeout let the VI retrace
// fire up to ~1.5 ms late (SMS_PORT_PROFILE "irqlate"), which showed up directly as
// present-to-present jitter. The last kIrqSpinTicks before a due time are still
// spun (SwitchToThread), as the old code did for the last millisecond.
HANDLE gIrqTimer;
HANDLE gIrqEvent;           // auto-reset: something earlier than the current wait was queued
long long gIrqWaitUntil;    // host ticks the interrupt thread sleeps until, 0 = awake (gAlarmLock)
const long long kIrqSpinTicks = kTimerClock / 2000; // 0.5 ms
// w14: SMS_PORT_W14_LEGACY=1 restores the scheduler timing of before the w14 changes
// (millisecond interrupt waits, no extra scheduling points, no CPU release around
// host blocking work), for A/B measurements with SMS_PORT_PROFILE.
bool gW14Legacy;
// gAlarmLock held: an interrupt is now due at fire.
inline void wakeIrq(long long fire)
{
	if (fire < gIrqWaitUntil) {
		gIrqWaitUntil = 0;
		SetEvent(gIrqEvent);
	}
}

// gAlarmLock held.
void insertAlarm(OSAlarm* a)
{
	OSAlarm* prev = nullptr;
	OSAlarm* next = gAlarmHead;
	while (next && next->fire <= a->fire) {
		prev = next;
		next = next->next;
	}
	a->prev = prev;
	a->next = next;
	if (next)
		next->prev = a;
	if (prev)
		prev->next = a;
	else
		gAlarmHead = a;
	wakeIrq(a->fire); // w14
}

// gAlarmLock held.
void removeAlarm(OSAlarm* a)
{
	if (a->prev)
		a->prev->next = a->next;
	else if (gAlarmHead == a)
		gAlarmHead = a->next;
	else
		return; // not queued
	if (a->next)
		a->next->prev = a->prev;
	a->next = a->prev = nullptr;
}

DWORD WINAPI irqThread(LPVOID)
{
	tIrq = 1;
	AcquireSRWLockExclusive(&gAlarmLock);
	for (;;) {
		long long now  = hostTicks();
		long long next = now + kTimerClock / 10;
		for (int i = 0; i < gNumPeriodic; i++)
			if (gPeriodic[i].enabled && gPeriodic[i].next < next)
				next = gPeriodic[i].next;
		if (gAlarmHead && gAlarmHead->fire < next)
			next = gAlarmHead->fire;
		if (next > now) {
			if (gW14Legacy && (next - now) * 1000 / kTimerClock > 1) {
				// before w14 (w9's version): millisecond timeout, 1 ms short, then spin
				gIrqWaitUntil = next;
				ReleaseSRWLockExclusive(&gAlarmLock);
				WaitForSingleObject(gIrqEvent, (DWORD)((next - now) * 1000 / kTimerClock - 1));
				AcquireSRWLockExclusive(&gAlarmLock);
				gIrqWaitUntil = 0;
			} else if (!gW14Legacy && next - now > kIrqSpinTicks) {
				// w14: high-resolution timer, see gIrqTimer (replaces w9's "sleep 1 ms short
				// and spin the rest": same accuracy, a quarter of the spinning)
				LARGE_INTEGER due;
				due.QuadPart = -((next - now - kIrqSpinTicks) * 10000000ll / kTimerClock); // relative, 100 ns
				if (due.QuadPart == 0)
					due.QuadPart = -1;
				SetWaitableTimer(gIrqTimer, &due, 0, nullptr, nullptr, FALSE);
				gIrqWaitUntil = next;
				ReleaseSRWLockExclusive(&gAlarmLock);
				HANDLE waits[2] = { gIrqEvent, gIrqTimer };
				WaitForMultipleObjects(2, waits, FALSE, INFINITE);
				AcquireSRWLockExclusive(&gAlarmLock);
				gIrqWaitUntil = 0;
			} else {
				ReleaseSRWLockExclusive(&gAlarmLock);
				SwitchToThread();
				AcquireSRWLockExclusive(&gAlarmLock);
			}
			continue;
		}

		// Something is due: enter interrupt context.
		void (*due[kMaxPeriodic])(void);
		int numDue = 0;
		for (int i = 0; i < gNumPeriodic; i++) {
			Periodic& p = gPeriodic[i];
			if (p.enabled && p.next <= now) {
				if (gPortProfile && i == 0) // w14: how late the VI retrace (slot 0, port_main.cpp) fired
					port_prof_add(PP_IRQLATE, (now - p.next) * gQpcFreq.QuadPart / kTimerClock);
				p.next += p.period;
				if (p.next <= now - 4 * p.period) // fell far behind: resync
					p.next = now + p.period;
				due[numDue++] = p.fn;
			}
		}
		ReleaseSRWLockExclusive(&gAlarmLock);

		if (gPortProfile && !TryAcquireSRWLockExclusive(&gIntr)) {
			// w14: a game thread has interrupts disabled; the delivery waits for it.
			long long t0 = port_prof_now();
			AcquireSRWLockExclusive(&gIntr);
			port_prof_add(PP_INTRWAIT, port_prof_now() - t0);
		} else if (!gPortProfile) {
			AcquireSRWLockExclusive(&gIntr);
		}
		for (int i = 0; i < numDue; i++)
			due[i]();
		for (;;) {
			AcquireSRWLockExclusive(&gAlarmLock);
			OSAlarm* a = gAlarmHead;
			if (!a || a->fire > now) {
				ReleaseSRWLockExclusive(&gAlarmLock);
				break;
			}
			removeAlarm(a);
			OSAlarmHandler h = a->handler;
			if (a->period > 0) {
				while (a->fire <= now)
					a->fire += a->period;
				insertAlarm(a);
			} else {
				a->handler = nullptr;
			}
			ReleaseSRWLockExclusive(&gAlarmLock);
			if (h)
				h(a, &gIrqContext);
		}
		ReleaseSRWLockExclusive(&gIntr);
		AcquireSRWLockExclusive(&gAlarmLock);
	}
	return 0;
}

// One-shot device interrupts, built on the alarm queue.
struct OneShot {
	OSAlarm alarm;
	void (*fn)(void*);
	void* arg;
};
const int kNumOneShots = 256;
OneShot gOneShots[kNumOneShots];
int gOneShotNext;
void oneShotHandler(OSAlarm* a, OSContext*)
{
	OneShot* o = (OneShot*)a;
	o->fn(o->arg);
}

} // namespace

// ---------------------------------------------------------------------------
// Port-internal API (port_sdk.h)
// ---------------------------------------------------------------------------
extern "C" {

void port_thread_init(void)
{
	if (gInit)
		return;
	gInit = true;
	QueryPerformanceFrequency(&gQpcFreq);
	timeBeginPeriod(1);
	// w14: interrupt-thread timer and wake event (see gIrqTimer)
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
	gIrqTimer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
	                                   TIMER_ALL_ACCESS);
	if (gIrqTimer == nullptr)
		gIrqTimer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
	gIrqEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	gW14Legacy = std::getenv("SMS_PORT_W14_LEGACY") != nullptr;

	// The calling (main) thread becomes the default thread, priority 16.
	sMainRec.t      = &sMainThread;
	sMainRec.isMain = true;
	sMainRec.hostTid = GetCurrentThreadId(); // w14
	InitializeConditionVariable(&sMainRec.cv);
	setRec(&sMainThread, &sMainRec);
	sMainThread.state    = OS_THREAD_STATE_RUNNING;
	sMainThread.priority = sMainThread.base = 16;
	sMainThread.attr                        = OS_THREAD_ATTR_DETACH;
	gRunning                                = &sMainThread;
	tSelf                                   = &sMainThread;
	addThread(&sMainThread);
	if (gameHostPriority() != THREAD_PRIORITY_NORMAL) // w12: see threadEntry
		SetThreadPriority(GetCurrentThread(), gameHostPriority());

	HANDLE h = CreateThread(nullptr, 1 << 20, irqThread, nullptr,
	                        STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
	SetThreadPriority(h, THREAD_PRIORITY_TIME_CRITICAL);
	CloseHandle(h);
}

void port_os_poll(void) { checkPreempt(); }

// w14: the scheduling points added by w14 (GXCallDisplayList, JKRHeap::alloc, DVDReadPrio).
// w14 (SMS_PORT_PROFILE=3): host thread id of the game thread holding the CPU, 0 if none.
unsigned long port_sched_running_tid(void)
{
	OSThread* t = gRunning; // racy read: diagnostics only
	return t ? REC(t)->hostTid : 0;
}

// w14 (SMS_PORT_PROFILE=3): prints every game thread's priority/state and stack.
void port_sched_dump_all(void)
{
	OSThread* list[kMaxThreads];
	int n;
	schedLock();
	n = gNumThreads;
	for (int i = 0; i < n; i++)
		list[i] = gThreads[i];
	OSThread* run = gRunning;
	schedUnlock();
	for (int i = 0; i < n; i++) {
		HostThread* r = REC(list[i]);
		std::fprintf(stderr, "[prof] game thread prio %d state %d suspend %d%s (host %lu):\n", (int)list[i]->priority,
		             (int)list[i]->state, (int)list[i]->suspend, list[i] == run ? " RUNNING" : "", r->hostTid);
		if (r->hostTid != 0 && r->hostTid != GetCurrentThreadId()) {
			port_dump_thread(r->hostTid); // leaves it suspended: resume it
			if (HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, r->hostTid)) {
				ResumeThread(th);
				CloseHandle(th);
			}
		}
	}
}

// w14: whether t is the game thread holding the CPU (racy read; sdk_audio.cpp's ARQ
// completion queue only uses it to see that a poster has moved on).
int port_thread_is_running(OSThread* t) { return gRunning == t; }

// w14: a count that changes whenever game thread t passes an interrupt-enabled
// scheduling point (checkPreempt: OSRestoreInterrupts/OSEnableInterrupts re-enabling,
// port_os_poll, port_sched_point). Read from other threads (racy; equality test only).
unsigned port_thread_sched_epoch(OSThread* t)
{
	HostThread* r = t ? REC(t) : nullptr;
	return r ? r->schedEpoch : 0;
}

void port_sched_point(void)
{
	if (!gW14Legacy)
		checkPreempt();
}

// w14: host blocking work (a big DVD read) done without holding the CPU, the way a
// GameCube thread sleeps while the drive DMAs: other game threads, lower-priority
// ones included, run meanwhile. The caller must touch no game state between the two
// calls other than the buffer it is filling. Returns 0 (and does nothing) outside a
// game thread.
int port_blocking_begin(void)
{
	OSThread* self = tSelf;
	if (tIrq || !self || gW14Legacy)
		return 0;
	HostThread* r = REC(self);
	schedLock();
	if (gRunning != self || gSchedDisable > 0) { // keep the CPU when the scheduler is off
		schedUnlock();
		return 0;
	}
	self->state = OS_THREAD_STATE_WAITING; // on no queue: only port_blocking_end readies it
	OSThread* next = pickNext();
	dispatch(next);
	if (next == nullptr)
		onIdle(); // as in switchOut: deferred device completions run now
	if (r->intrDisabled)
		ReleaseSRWLockExclusive(&gIntr);
	schedUnlock();
	return 1;
}

void port_blocking_end(int began)
{
	if (!began)
		return;
	OSThread* self = tSelf;
	HostThread* r  = REC(self);
	schedLock();
	makeReady(self);
	// as when an interrupt readies a thread (afterReady, interrupt side)
	if (!gRunning) {
		dispatch(pickNext());
	} else {
		OSThread* n = pickNext();
		if (n && n != gRunning && (n->priority < gRunning->priority || !runnable(gRunning)))
			InterlockedExchange(&gPreemptPending, 1);
	}
	waitForCpu(r);
	if (r->intrDisabled) {
		schedUnlock();
		acquireIntr();
		schedLock();
	}
	schedUnlock();
}

// w14: host-only work during which the calling thread keeps the CPU against
// lower-priority threads but a higher-priority one takes it at once (the interrupt
// thread dispatches it directly instead of waiting for a scheduling point). Used
// around the Aurora present (end/begin frame can wait for the GPU for 100+ ms) so the
// audio thread is not starved. Only threads of priority <= kInPlaceMaxPrio may take
// over: Aurora is not reentrant, and game threads that issue GX commands must not run
// while the caller is inside it (the setup threads, 17, are lower priority anyway; the
// draw-sync thread, 15, peeks the EFB for the lens flare, which drains Aurora's FIFO).
// Requires interrupts enabled; returns 0 (and end does nothing) otherwise.
int port_preemptible_begin(void)
{
	OSThread* self = tSelf;
	if (tIrq || !self || gW14Legacy)
		return 0;
	HostThread* r = REC(self);
	if (r->intrDisabled)
		return 0;
	schedLock();
	if (gRunning != self || gSchedDisable > 0) {
		schedUnlock();
		return 0;
	}
	r->inPlace = true;
	OSThread* n = pickNext(); // one may be waiting already (pending preemption)
	if (n && n != self && n->priority < self->priority && n->priority <= kInPlaceMaxPrio) {
		makeReady(self);
		dispatch(n);
	}
	schedUnlock();
	return 1;
}

void port_preemptible_end(int began)
{
	if (!began)
		return;
	HostThread* r = REC(tSelf);
	schedLock();
	r->inPlace = false;
	waitForCpu(r); // returns at once unless a higher-priority thread took the CPU
	schedUnlock();
}

void port_run_as_interrupt(void (*fn)(void*), void* arg)
{
	if (tIrq) {
		fn(arg);
		return;
	}
	bool held = tSelf && REC(tSelf)->intrDisabled;
	if (!held)
		AcquireSRWLockExclusive(&gIntr);
	tIrq = 1;
	fn(arg);
	tIrq = 0;
	if (!held) {
		ReleaseSRWLockExclusive(&gIntr);
		checkPreempt();
	}
}

int port_irq_add_periodic(void (*fn)(void), long long period_ns)
{
	port_thread_init();
	AcquireSRWLockExclusive(&gAlarmLock);
	int slot    = gNumPeriodic++;
	Periodic& p = gPeriodic[slot];
	p.fn        = fn;
	p.period    = period_ns * kTimerClock / 1000000000ll;
	if (p.period < 1)
		p.period = 1;
	p.next    = hostTicks() + p.period;
	p.enabled = true;
	wakeIrq(p.next); // w14
	ReleaseSRWLockExclusive(&gAlarmLock);
	return slot;
}

void port_irq_set_period(int slot, long long period_ns)
{
	AcquireSRWLockExclusive(&gAlarmLock);
	Periodic& p  = gPeriodic[slot];
	long long np = period_ns * kTimerClock / 1000000000ll;
	if (np < 1)
		np = 1;
	if (np != p.period) {
		p.period = np;
		p.next   = hostTicks() + np;
	}
	wakeIrq(p.next); // w14
	ReleaseSRWLockExclusive(&gAlarmLock);
}

void port_irq_enable(int slot, int enable)
{
	AcquireSRWLockExclusive(&gAlarmLock);
	Periodic& p = gPeriodic[slot];
	if (enable && !p.enabled)
		p.next = hostTicks() + p.period;
	p.enabled = enable != 0;
	wakeIrq(p.next); // w14
	ReleaseSRWLockExclusive(&gAlarmLock);
}

void port_irq_raise(void (*fn)(void*), void* arg, long long delay_ns)
{
	port_thread_init();
	AcquireSRWLockExclusive(&gAlarmLock);
	OneShot* o = nullptr;
	for (int i = 0; i < kNumOneShots; i++) {
		OneShot* c = &gOneShots[(gOneShotNext + i) % kNumOneShots];
		if (!c->alarm.handler) {
			o            = c;
			gOneShotNext = (gOneShotNext + i + 1) % kNumOneShots;
			break;
		}
	}
	if (!o) {
		ReleaseSRWLockExclusive(&gAlarmLock);
		std::fprintf(stderr, "[port] one-shot interrupt pool exhausted\n");
		return;
	}
	o->fn            = fn;
	o->arg           = arg;
	o->alarm.handler = oneShotHandler;
	o->alarm.period  = 0;
	o->alarm.prev = o->alarm.next = nullptr;
	o->alarm.fire                 = hostTicks() + delay_ns * kTimerClock / 1000000000ll;
	insertAlarm(&o->alarm);
	ReleaseSRWLockExclusive(&gAlarmLock);
}

// Deferred device completions (port_sdk.h): drained at the next VI retrace or as
// soon as no game thread is runnable, whichever comes first. The FIFO is only
// changed with the interrupt lock held (interrupt context, or interrupts
// disabled), so it needs no lock of its own.
namespace {
struct Deferred {
	void (*fn)(void*);
	void* arg;
};
const int kNumDeferred = 1024;
Deferred gDeferred[kNumDeferred];
int gDeferHead, gDeferCount;
volatile bool gRetraceDriven; // a VI retrace has called port_retrace_run
} // namespace

void port_retrace_defer(void (*fn)(void*), void* arg)
{
	if (!gRetraceDriven) { // no VI drives the queue (build_port.py): a one-shot interrupt
		port_irq_raise(fn, arg, 1000000);
		return;
	}
	// port_run_as_interrupt takes the interrupt lock from any thread (OSDisableInterrupts
	// does nothing on a host thread that is not a game thread).
	Deferred d = { fn, arg };
	port_run_as_interrupt(
	    [](void* p) {
		    if (gDeferCount == kNumDeferred) {
			    std::fprintf(stderr, "[port] deferred completion queue full\n");
			    return;
		    }
		    gDeferred[(gDeferHead + gDeferCount++) % kNumDeferred] = *(Deferred*)p;
	    },
	    &d);
}

// Interrupt context. Only what was queued before this call; anything a handler
// defers waits for the next drain.
static void drainDeferred()
{
	for (int n = gDeferCount; n > 0; --n) {
		Deferred d = gDeferred[gDeferHead];
		gDeferHead = (gDeferHead + 1) % kNumDeferred;
		--gDeferCount;
		port_run_as_interrupt(d.fn, d.arg);
	}
}

void port_retrace_run(void)
{
	gRetraceDriven = true;
	drainDeferred();
}

int port_cannot_block(void) { return tIrq || !tSelf; }

void port_defer_kick(long long delay_ns)
{
	port_irq_raise([](void*) { drainDeferred(); }, nullptr, delay_ns);
}

namespace {
volatile LONG gIdleRaised;
void idleDrain(void*)
{
	InterlockedExchange(&gIdleRaised, 0);
	drainDeferred();
}

// gSched held, from switchOut when no game thread can run: device work queued by
// the threads that just went to sleep completes now (from the interrupt thread,
// since handlers take gSched) instead of at the next retrace.
void onIdle()
{
	if (gDeferCount > 0 && InterlockedExchange(&gIdleRaised, 1) == 0)
		port_irq_raise(idleDrain, nullptr, 0);
}
} // namespace

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
BOOL OSDisableInterrupts(void)
{
	if (tIrq || !tSelf)
		return FALSE;
	HostThread* r = REC(tSelf);
	if (r->intrDisabled)
		return FALSE;
	acquireIntr();
	r->intrDisabled = true;
	return TRUE;
}

BOOL OSEnableInterrupts(void)
{
	if (tIrq || !tSelf)
		return FALSE;
	HostThread* r = REC(tSelf);
	if (!r->intrDisabled)
		return TRUE;
	r->intrDisabled = false;
	ReleaseSRWLockExclusive(&gIntr);
	checkPreempt();
	return FALSE;
}

BOOL OSRestoreInterrupts(BOOL level)
{
	if (tIrq || !tSelf)
		return FALSE;
	HostThread* r = REC(tSelf);
	BOOL prev     = !r->intrDisabled;
	if (level) {
		if (r->intrDisabled) {
			r->intrDisabled = false;
			ReleaseSRWLockExclusive(&gIntr);
			checkPreempt();
		}
	} else if (!r->intrDisabled) {
		acquireIntr();
		r->intrDisabled = true;
	}
	return prev;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
void OSInitThreadQueue(OSThreadQueue* queue) { queue->head = queue->tail = nullptr; }

OSThread* OSGetCurrentThread(void) { return tSelf ? tSelf : gRunning; }

BOOL OSIsThreadTerminated(OSThread* t)
{
	return t->state == OS_THREAD_STATE_MORIBUND || t->state == 0;
}

s32 OSDisableScheduler(void)
{
	schedLock();
	s32 prev = gSchedDisable++;
	schedUnlock();
	return prev;
}

s32 OSEnableScheduler(void)
{
	schedLock();
	s32 prev = gSchedDisable--;
	if (gSchedDisable == 0)
		afterReady();
	schedUnlock();
	return prev;
}

int OSCreateThread(OSThread* t, void* (*func)(void*), void* param, void* stack, u32 stackSize,
                   s32 priority, u16 attr)
{
	if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX)
		return FALSE;
	port_thread_init();
	t->state     = OS_THREAD_STATE_READY;
	t->attr      = attr & OS_THREAD_ATTR_DETACH;
	t->base      = priority;
	t->priority  = priority;
	t->suspend   = 1;
	t->val       = (void*)-1;
	t->mutex     = nullptr;
	t->queue     = nullptr;
	t->link.next = t->link.prev = nullptr;
	t->linkActive.next = t->linkActive.prev = nullptr;
	OSInitThreadQueue(&t->queueJoin);
	t->queueMutex.head = t->queueMutex.tail = nullptr;
	t->stackBase                            = (u8*)stack;
	t->stackEnd                             = (u32*)((u8*)stack - stackSize);
	if (stack && stackSize)
		*t->stackEnd = OS_THREAD_STACK_MAGIC;

	// The game's stack buffer is not used: game code runs on a host stack.
	port_host_alloc_push(); // host record: not in (or freed with) a game heap
	HostThread* r = new HostThread();
	port_host_alloc_pop();
	r->t          = t;
	r->func       = func;
	r->param      = param;
	InitializeConditionVariable(&r->cv);
	setRec(t, r);

	schedLock();
	addThread(t);
	schedUnlock();

	HANDLE h = CreateThread(nullptr, 1 << 20, threadEntry, r, STACK_SIZE_PARAM_IS_A_RESERVATION,
	                        nullptr);
	if (!h) {
		schedLock();
		removeThread(t);
		schedUnlock();
		return FALSE;
	}
	CloseHandle(h);
	return TRUE;
}

// SDK semantics: OSExitThread(void* val). The decomp header spells the
// parameter OSThread*; it is the thread's return value either way.
void OSExitThread(OSThread* valArg)
{
	void* val = (void*)valArg;
	OSThread* self = tSelf;
	if (!self || REC(self)->isMain) {
		std::fflush(stdout);
		ExitProcess(0);
	}
	HostThread* r = REC(self);
	schedLock();
	self->val   = val;
	self->state = OS_THREAD_STATE_MORIBUND;
	removeThread(self);
	wakeQueue(&self->queueJoin);
	if (r->intrDisabled) {
		r->intrDisabled = false;
		ReleaseSRWLockExclusive(&gIntr);
	}
	dispatch(pickNext());
	schedUnlock();
	ExitThread(0);
}

void OSCancelThread(OSThread* t)
{
	if (t == tSelf) {
		OSExitThread(nullptr);
		return;
	}
	schedLock();
	if (t->state == OS_THREAD_STATE_WAITING && t->queue)
		queueRemove(t->queue, t);
	t->queue = nullptr;
	t->state = OS_THREAD_STATE_MORIBUND;
	removeThread(t);
	wakeQueue(&t->queueJoin);
	if (HostThread* r = REC(t)) {
		r->cancelled = true;
		WakeConditionVariable(&r->cv);
	}
	afterReady();
	schedUnlock();
}

int OSJoinThread(OSThread* t, void** val)
{
	schedLock();
	if ((t->attr & OS_THREAD_ATTR_DETACH) && t->state != OS_THREAD_STATE_MORIBUND) {
		schedUnlock();
		return FALSE;
	}
	while (t->state != OS_THREAD_STATE_MORIBUND && t->state != 0)
		sleepOn(&t->queueJoin);
	if (val)
		*val = t->val;
	schedUnlock();
	return TRUE;
}

void OSDetachThread(OSThread* t) { t->attr |= OS_THREAD_ATTR_DETACH; }

long OSResumeThread(OSThread* t)
{
	schedLock();
	long prev = t->suspend;
	if (prev > 0) {
		t->suspend--;
		if (t->suspend == 0 && t->state == OS_THREAD_STATE_READY) {
			makeReady(t);
			afterReady();
		}
	}
	schedUnlock();
	return prev;
}

s32 OSSuspendThread(OSThread* t)
{
	schedLock();
	s32 prev = t->suspend++;
	if (prev == 0) {
		if (t == tSelf && !tIrq) {
			t->state = OS_THREAD_STATE_READY;
			switchOut(t);
		} else if (t == gRunning) {
			InterlockedExchange(&gPreemptPending, 1);
		}
	}
	schedUnlock();
	return prev;
}

void OSSleepThread(OSThreadQueue* queue)
{
	schedLock();
	sleepOn(queue);
	schedUnlock();
}

void OSWakeupThread(OSThreadQueue* queue)
{
	schedLock();
	wakeQueue(queue);
	afterReady();
	schedUnlock();
}

void OSYieldThread(void)
{
	if (tIrq || !tSelf)
		return;
	schedLock();
	makeReady(tSelf);
	switchOut(tSelf);
	schedUnlock();
}

void OSSetThreadPriority(OSThread* t, OSPriority priority)
{
	if (priority < OS_PRIORITY_MIN || priority > OS_PRIORITY_MAX)
		return;
	schedLock();
	t->base = t->priority = priority;
	if (t->state == OS_THREAD_STATE_WAITING && t->queue) {
		OSThreadQueue* q = t->queue;
		queueRemove(q, t);
		queueAddPrio(q, t);
	}
	afterReady();
	schedUnlock();
}

long OSGetThreadPriority(OSThread* t) { return t->base; }

// ---------------------------------------------------------------------------
// Message queues (OSMessage.c semantics)
// ---------------------------------------------------------------------------
void OSInitMessageQueue(OSMessageQueue* mq, void* msgArray, long msgCount)
{
	OSInitThreadQueue(&mq->queueSend);
	OSInitThreadQueue(&mq->queueReceive);
	mq->msgArray   = msgArray;
	mq->msgCount   = msgCount;
	mq->firstIndex = 0;
	mq->usedCount  = 0;
}

int OSSendMessage(OSMessageQueue* mq, void* msg, long flags)
{
	schedLock();
	while (mq->msgCount <= mq->usedCount) {
		if (!(flags & OS_MESSAGE_BLOCK) || tIrq || !tSelf) {
			schedUnlock();
			return FALSE;
		}
		sleepOn(&mq->queueSend);
	}
	long last                        = (mq->firstIndex + mq->usedCount) % mq->msgCount;
	((OSMessage*)mq->msgArray)[last] = msg;
	mq->usedCount++;
	wakeQueue(&mq->queueReceive);
	afterReady();
	schedUnlock();
	return TRUE;
}

int OSReceiveMessage(OSMessageQueue* mq, void* msg, long flags)
{
	schedLock();
	while (mq->usedCount == 0) {
		if (!(flags & OS_MESSAGE_BLOCK) || tIrq || !tSelf) {
			schedUnlock();
			return FALSE;
		}
		sleepOn(&mq->queueReceive);
	}
	if (msg)
		*(OSMessage*)msg = ((OSMessage*)mq->msgArray)[mq->firstIndex];
	mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
	mq->usedCount--;
	wakeQueue(&mq->queueSend);
	afterReady();
	schedUnlock();
	return TRUE;
}

int OSJamMessage(OSMessageQueue* mq, void* msg, long flags)
{
	schedLock();
	while (mq->msgCount <= mq->usedCount) {
		if (!(flags & OS_MESSAGE_BLOCK) || tIrq || !tSelf) {
			schedUnlock();
			return FALSE;
		}
		sleepOn(&mq->queueSend);
	}
	mq->firstIndex = (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
	((OSMessage*)mq->msgArray)[mq->firstIndex] = msg;
	mq->usedCount++;
	wakeQueue(&mq->queueReceive);
	afterReady();
	schedUnlock();
	return TRUE;
}

// ---------------------------------------------------------------------------
// Mutexes and condition variables
// ---------------------------------------------------------------------------
void OSInitMutex(OSMutex* m)
{
	OSInitThreadQueue(&m->queue);
	m->thread    = nullptr;
	m->count     = 0;
	m->link.next = m->link.prev = nullptr;
}

static void lockMutexLocked(OSMutex* m)
{
	OSThread* self = OSGetCurrentThread();
	for (;;) {
		if (!m->thread) {
			m->thread = self;
			m->count++;
			return;
		}
		if (m->thread == self) {
			m->count++;
			return;
		}
		if (tIrq || !tSelf) {
			std::fprintf(stderr, "[port] OSLockMutex contention in interrupt context\n");
			return;
		}
		self->mutex = m;
		sleepOn(&m->queue);
		self->mutex = nullptr;
	}
}

void OSLockMutex(OSMutex* m)
{
	schedLock();
	lockMutexLocked(m);
	schedUnlock();
}

void OSUnlockMutex(OSMutex* m)
{
	schedLock();
	if (m->thread == OSGetCurrentThread() && --m->count == 0) {
		m->thread = nullptr;
		wakeQueue(&m->queue);
		afterReady();
	}
	schedUnlock();
}

BOOL OSTryLockMutex(OSMutex* m)
{
	OSThread* self = OSGetCurrentThread();
	BOOL ok        = FALSE;
	schedLock();
	if (!m->thread) {
		m->thread = self;
		m->count++;
		ok = TRUE;
	} else if (m->thread == self) {
		m->count++;
		ok = TRUE;
	}
	schedUnlock();
	return ok;
}

void OSInitCond(OSCond* cond) { OSInitThreadQueue(&cond->queue); }

void OSWaitCond(OSCond* cond, OSMutex* m)
{
	schedLock();
	if (m->thread == OSGetCurrentThread()) {
		s32 count = m->count;
		m->thread = nullptr;
		m->count  = 0;
		wakeQueue(&m->queue);
		sleepOn(&cond->queue);
		lockMutexLocked(m);
		m->count = count;
	}
	schedUnlock();
}

void OSSignalCond(OSCond* cond) { OSWakeupThread(&cond->queue); }

// ---------------------------------------------------------------------------
// Alarms
// ---------------------------------------------------------------------------
void OSInitAlarm(void) { }
BOOL OSCheckAlarmQueue(void) { return TRUE; }

void OSCreateAlarm(OSAlarm* alarm)
{
	alarm->handler = nullptr;
	alarm->tag     = 0;
	alarm->next = alarm->prev = nullptr;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
{
	port_thread_init();
	AcquireSRWLockExclusive(&gAlarmLock);
	removeAlarm(alarm);
	alarm->handler = handler;
	alarm->period  = 0;
	alarm->fire    = hostTicks() + tick;
	insertAlarm(alarm);
	ReleaseSRWLockExclusive(&gAlarmLock);
}

void OSSetAbsAlarm(OSAlarm* alarm, long long time, OSAlarmHandler handler)
{
	port_thread_init();
	long long rel = time - OSGetTime();
	AcquireSRWLockExclusive(&gAlarmLock);
	removeAlarm(alarm);
	alarm->handler = handler;
	alarm->period  = 0;
	alarm->fire    = hostTicks() + rel;
	insertAlarm(alarm);
	ReleaseSRWLockExclusive(&gAlarmLock);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler)
{
	port_thread_init();
	long long osNow = OSGetTime();
	AcquireSRWLockExclusive(&gAlarmLock);
	removeAlarm(alarm);
	long long now  = hostTicks();
	long long hs   = now + (start - osNow); // start on the host clock
	alarm->handler = handler;
	alarm->period  = period;
	alarm->start   = start;
	if (hs > now || period <= 0)
		alarm->fire = hs;
	else
		alarm->fire = hs + ((now - hs) / period + 1) * period;
	insertAlarm(alarm);
	ReleaseSRWLockExclusive(&gAlarmLock);
}

void OSCancelAlarm(OSAlarm* alarm)
{
	AcquireSRWLockExclusive(&gAlarmLock);
	removeAlarm(alarm);
	alarm->handler = nullptr;
	ReleaseSRWLockExclusive(&gAlarmLock);
}

} // extern "C"

// Create the default thread before any game static constructor runs.
#pragma init_seg(lib)
namespace {
struct PortThreadBoot {
	PortThreadBoot() { port_thread_init(); }
} sPortThreadBoot;
} // namespace
