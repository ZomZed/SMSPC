// Host AI / AR / ARQ / DSP for the PC port.
//
// * AR/ARQ: ARAM is a 16 MB host buffer; ARAM addresses are offsets into it.
//   Transfers complete immediately (memcpy) and their callbacks run as
//   interrupts.
// * AI: the DMA interrupt ticks at the rate the programmed buffer drains
//   (length / 4 bytes per stereo sample at the DSP sample rate). At each tick the
//   buffer that starts playing goes to port_audio_out.cpp (SDL3 / WAV).
// * DSP: the JAudio microcode's mail protocol. It answers the boot handshake,
//   decodes the commands (0x81 DsetupTable, 0x82 DsyncFrame, DSPReleaseHalt),
//   renders one sub-frame per DSPReleaseHalt with the host microcode
//   (port_audio_dsp.cpp) and then reports the finished sub-frame, so JASystem's
//   audio frame loop runs as on the GameCube. Replies are delivered through
//   JAudio's own __DSPHandler in interrupt context.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "port_sdk.h"
#include "port_audio.h"
#include "port_profile.h" // w14

#include <dolphin/ai.h>
#include <dolphin/ar.h>
#include <dolphin/arq.h>
#include <dolphin/dsp.h>
#include <dolphin/os.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace JASystem {
namespace DSPInterface {
	extern u16 JAS_DSP_PREFIX;
}
} // namespace JASystem

extern "C" void __DSPHandler(int interrupt, OSContext* context);
#ifdef SMS_PORT_AURORA
extern "C" int port_qol_audio_paused(void); // w9: port_qol.cpp
#endif

// Defined by the SDK's dsp_task.c on the GameCube; JAudio's osdsp_task.c
// declares them with C++ linkage.
DSPTaskInfo* __DSP_curr_task;
DSPTaskInfo* __DSP_first_task;

namespace {

// ---- ARAM ------------------------------------------------------------------
const u32 kAramSize = 0x01000000; // 16 MB
const u32 kAramBase = 0x4000;     // the SDK reserves the first 16 KB
u8* gAram;
u32 gAramNext = kAramBase;
u32* gAramStack;
u32 gAramStackCount;
ARCallback gArCallback;

u8* aram(u32 addr)
{
	if (!gAram)
		gAram = (u8*)VirtualAlloc(nullptr, kAramSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	return gAram + (addr & (kAramSize - 1));
}

void aramCopy(u32 type, uintptr_t mram, uintptr_t aramAddr, u32 length)
{
	// ARQ main-memory addresses are host pointers carried in ARQAddr, which is
	// pointer-sized on the PC (arq.h), so no translation table is needed. ARAM
	// addresses stay genuine 32-bit offsets into the 16 MB host ARAM buffer.
	// (The raw ARStartDMA entry keeps the SDK's u32 main-memory argument: the
	// game only reaches ARAM through ARQ.)
	u8* m = (u8*)mram;
	if (type == ARAM_DIR_MRAM_TO_ARAM)
		std::memcpy(aram((u32)aramAddr), m, length);
	else
		std::memcpy(m, aram((u32)aramAddr), length);
}

void callArCallback(void*)
{
	if (gArCallback)
		gArCallback();
}

void callArqCallback(void* p)
{
	ARQRequest* r = (ARQRequest*)p;
	if (r->callback)
		r->callback((ARQAddr)(uintptr_t)r); // the callback argument is the request pointer
}

// ARQ completion timing. The copy itself is done when the request is posted, but
// the callback (the "DMA done" interrupt) is delivered later, in FIFO order, after
// the time the transfer would take on the bus (~80 MB/s ARAM DMA, plus a small
// per-request cost): like on the GameCube, code that posts a request and then sets
// its own "busy" state always gets the completion afterwards.
long long gArqBusyUntilNs;
long long hostNowNs()
{
	static LARGE_INTEGER sFreq;
	if (!sFreq.QuadPart)
		QueryPerformanceFrequency(&sFreq);
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (long long)((double)c.QuadPart * 1e9 / (double)sFreq.QuadPart);
}
// w14: the cost of a transfer. Only the fixed per-request part by default: what the
// completion delay exists for is ordering (see the queue below), and the 12 ns/byte bus
// time made every stage load wait for its ARAM wave uploads at GameCube DMA speed
// (JASystem's DVD thread uploads in 32 KB chunks and waits for each; Bianco setupObjects
// ~200 ms instead of ~45). SMS_PORT_ARQ_BUS_TIME=1 restores the bus-time model.
long long arqCostNs(u32 length)
{
	static const bool sBusTime = std::getenv("SMS_PORT_ARQ_BUS_TIME") != nullptr;
	return 20000 + (sBusTime ? (long long)length * 12 : 0);
}

// w14 (edit in w5's file, see BOOTLOG "Performance (w14)", ARQ race): AR/ARQ
// completions, delivered in FIFO order from one queue. A completion is due after its
// transfer time AND only once the posting game thread has certainly gone past the
// statements that follow ARQPostRequest. JASystem::Dvd::loadToAramDvdTMain posts and only
// then does ++bufferFull, a plain read-modify-write of a volatile; the completion
// (aramDmaFinish: --bufferFull) runs on the host interrupt thread, truly in parallel.
// When the poster's host thread was descheduled inside its ++ for longer than the delay,
// the decrement was lost and the DVD thread spun forever in `while (bufferFull != 0)`
// (3 hangs in ~20 runs with a 20 us delay; possible, rarer, with the bus-time delay). On
// the GameCube the window is a few instructions against a DMA of tens of microseconds.
// "Gone past" (any one of):
//  - the poster reached a scheduling point since posting (port_thread_sched_epoch: the
//    SDK calls that can switch threads, e.g. the next chunk's DVDRead or DVDClose);
//  - the poster is no longer the running game thread (it can only lose the CPU at such
//    a point);
//  - its host thread has used kPostCycles of CPU since posting (for posters that go
//    straight into a busy-wait with no SDK call; a few instructions take far less, and
//    time spent descheduled does not count).
// Posts from interrupt context or host threads only wait for the transfer time.
struct ArqDone {
	void (*fn)(void*);
	void* arg;
	long long dueNs;
	OSThread* poster;
	HANDLE posterThread;
	ULONG64 cycles;
	unsigned epoch;
};
const int kArqQueue      = 256;
const ULONG64 kPostCycles = 200000; // ~50 us of CPU
ArqDone gArqQ[kArqQueue];
int gArqHead, gArqCount;
bool gArqArmed;
SRWLOCK gArqLock = SRWLOCK_INIT;

HANDLE currentThreadHandle()
{
	static thread_local HANDLE sHandle;
	if (sHandle == nullptr)
		DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &sHandle,
		                THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0);
	return sHandle;
}

// Counters for SMS_PORT_PROFILE's load lines (port_arq_stats): completions delivered, and
// how many of them were found due but held back for their poster. SMS_PORT_ARQ_UNORDERED=1
// (debug, to reproduce the hang) delivers on the transfer time alone, as before.
volatile long long gArqDelivered, gArqHeld;

bool arqEligible(const ArqDone& d)
{
	static const bool sUnordered = std::getenv("SMS_PORT_ARQ_UNORDERED") != nullptr;
	if (sUnordered)
		return true;
	if (d.poster == nullptr || !port_thread_is_running(d.poster)
	    || port_thread_sched_epoch(d.poster) != d.epoch)
		return true;
	ULONG64 now = 0;
	if (!QueryThreadCycleTime(d.posterThread, &now))
		return true;
	return now - d.cycles >= kPostCycles;
}

void arqDrain(void*); // interrupt context

// gArqLock held.
void arqArm(long long delayNs)
{
	gArqArmed = true;
	port_irq_raise(arqDrain, nullptr, delayNs < 0 ? 0 : delayNs);
}

void arqDrain(void*)
{
	for (;;) {
		AcquireSRWLockExclusive(&gArqLock);
		gArqArmed = false;
		if (gArqCount == 0) {
			ReleaseSRWLockExclusive(&gArqLock);
			return;
		}
		ArqDone d          = gArqQ[gArqHead];
		const long long now = hostNowNs();
		if (now < d.dueNs) {
			arqArm(d.dueNs - now);
			ReleaseSRWLockExclusive(&gArqLock);
			return;
		}
		if (!arqEligible(d)) {
			++gArqHeld;
			arqArm(50000); // look again in 50 us
			ReleaseSRWLockExclusive(&gArqLock);
			return;
		}
		gArqHead = (gArqHead + 1) % kArqQueue;
		--gArqCount;
		++gArqDelivered;
		ReleaseSRWLockExclusive(&gArqLock);
		d.fn(d.arg); // already in interrupt context
	}
}

void arqComplete(void (*fn)(void*), void* arg, u32 length)
{
	const bool fromGame  = !port_cannot_block();
	OSThread* poster     = fromGame ? OSGetCurrentThread() : nullptr;
	HANDLE posterThread  = fromGame ? currentThreadHandle() : nullptr;
	ULONG64 cycles       = 0;
	unsigned epoch       = 0;
	if (fromGame) {
		QueryThreadCycleTime(posterThread, &cycles);
		epoch = port_thread_sched_epoch(poster);
	}
	AcquireSRWLockExclusive(&gArqLock);
	const long long now = hostNowNs();
	if (gArqBusyUntilNs < now)
		gArqBusyUntilNs = now;
	gArqBusyUntilNs += arqCostNs(length);
	if (gArqCount == kArqQueue) { // cannot happen with the game's request counts
		ReleaseSRWLockExclusive(&gArqLock);
		std::fprintf(stderr, "[port] ARQ completion queue full\n");
		return;
	}
	gArqQ[(gArqHead + gArqCount++) % kArqQueue] = { fn, arg, gArqBusyUntilNs, poster, posterThread, cycles, epoch };
	if (!gArqArmed)
		arqArm(gArqBusyUntilNs - now);
	ReleaseSRWLockExclusive(&gArqLock);
}

// ---- AI ----------------------------------------------------------------------
AIDCallback gAiCallback;
uintptr_t gAiStart;
u32 gAiLength;
BOOL gAiDmaOn;
u32 gAiDspRate = AI_SAMPLERATE_32KHZ;
u32 gAiStreamRate, gAiStreamState, gAiStreamTrigger;
u8 gAiVolL, gAiVolR;
int gAiSlot = -1;

u32 aiRate() { return gAiDspRate == AI_SAMPLERATE_48KHZ ? 48000 : 32000; }

long long aiPeriodNs()
{
	u32 samples = gAiLength ? gAiLength / 4 : 0x280;
	return (long long)samples * 1000000000ll / aiRate();
}

long long gAiPeriodSet; // period currently programmed into the interrupt slot

// AI DMA interrupt: the buffer programmed by the last AIInitDMA starts playing
// (it goes to the host output now); the callback then queues the next one.
void aiTick()
{
	if (!gAiDmaOn)
		return;
#ifdef SMS_PORT_AURORA
	if (port_qol_audio_paused()) // w9: quality-of-life pause on focus loss (port_qol.cpp)
		return;
#endif
	if (gAiStart && gAiLength) {
		// SMS_PORT_AUDIO_LOG: a buffer that starts playing again (the audio thread did not
		// queue the next one in time: a stutter)
		static uintptr_t sLast;
		static unsigned sTicks, sStale;
		static const bool sLog = std::getenv("SMS_PORT_AUDIO_LOG") != nullptr;
		sStale += gAiStart == sLast ? 1 : 0;
		sLast = gAiStart;
		if (sLog && ++sTicks % 600 == 0)
			std::fprintf(stderr, "[port] audio: %u of the last 600 DMA buffers replayed (not queued in time)\n", sStale),
			    sStale = 0;
		port_audio_out_ai((const int16_t*)gAiStart, gAiLength / 4, aiRate());
	}
	if (gAiCallback)
		gAiCallback();
	long long p = port_audio_out_next_period(aiPeriodNs());
	if (gAiSlot >= 0 && p != gAiPeriodSet) {
		gAiPeriodSet = p;
		port_irq_set_period(gAiSlot, p);
	}
}

// ---- fake DSP ------------------------------------------------------------------
SRWLOCK gMailLock = SRWLOCK_INIT;
const int kMailQ  = 256;
u32 gMailFromDsp[kMailQ];
int gMailHead, gMailCount;
int gCmdExpect = -1; // data mails still expected for the current command
int gCmdTotal;
u32 gCmdData[16];
int gCmdLen;

void pushMail(u32 m)
{
	AcquireSRWLockExclusive(&gMailLock);
	if (gMailCount < kMailQ) {
		gMailFromDsp[(gMailHead + gMailCount) % kMailQ] = m;
		gMailCount++;
	}
	ReleaseSRWLockExclusive(&gMailLock);
}

OSContext gDspContext;
void dspInterrupt(void*) { __DSPHandler(__OS_INTERRUPT_DSP_DSP, &gDspContext); }

// DSP -> CPU "request" (0xDCD10004 + payload, handled by the task's req_cb).
void dspRequest(u32 payload, long long delay_ns)
{
	pushMail(0xDCD10004);
	pushMail(((u32)JASystem::DSPInterface::JAS_DSP_PREFIX << 16) | (payload & 0xFFFF));
	port_irq_raise(dspInterrupt, nullptr, delay_ns);
}

void dspCommandDone()
{
	if (gCmdTotal == 0) {
		// DSPReleaseHalt (two mails: the 64-bit voice map): the DSP renders one
		// sub-frame and halts again.
		{
			PORT_PROF_SCOPE(PP_AUDIO); // w14: SMS_PORT_PROFILE
			port_dsp_render_subframe(((u64)gCmdData[0] << 32) | gCmdData[1]);
		}
		dspRequest(0xFF00, 200000);
		return;
	}
	u32 cmd = gCmdData[0] >> 24;
	if (cmd == 0x81 && gCmdLen >= 5) // DsetupTable
		port_dsp_setup(gCmdData[0] & 0xFFFF, gCmdData[1], gCmdData[2], gCmdData[3], gCmdData[4]);
	if (cmd == 0x82) { // DsyncFrame: sub-frames are released one by one
		if (gCmdLen >= 3)
			port_dsp_sync_frame((gCmdData[0] >> 16) & 0xFF, gCmdData[1], gCmdData[2], gCmdData[0] & 0xFFFF);
		return;
	}
	dspRequest(gCmdData[0] >> 16, 100000); // completion for DspStartWork
}

} // namespace

extern "C" {

// ---- AR ----------------------------------------------------------------------
u32 ARInit(u32* stack_index_addr, u32 num_entries)
{
	aram(0);
	gAramStack      = stack_index_addr;
	gAramStackCount = 0;
	(void)num_entries;
	return kAramBase;
}
void ARReset(void) { }
int ARCheckInit(void) { return TRUE; }
u32 ARGetBaseAddress(void) { return kAramBase; }
u32 ARGetSize(void) { return kAramSize; }
void ARSetSize(void) { }
u32 ARGetDMAStatus(void) { return 0; }
ARCallback ARRegisterDMACallback(ARCallback callback)
{
	ARCallback old = gArCallback;
	gArCallback    = callback;
	return old;
}
u32 ARAlloc(u32 length)
{
	u32 a = gAramNext;
	if (gAramStack)
		gAramStack[gAramStackCount++] = length;
	gAramNext += length;
	return a;
}
u32 ARFree(u32* length)
{
	u32 len = gAramStack && gAramStackCount ? gAramStack[--gAramStackCount] : 0;
	gAramNext -= len;
	if (length)
		*length = len;
	return gAramNext;
}
void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length)
{
	aramCopy(type, mainmem_addr, aram_addr, length);
	arqComplete(callArCallback, nullptr, length); // w14: ordered, after the poster moved on
}

// ---- ARQ ---------------------------------------------------------------------
static u32 gArqChunk = 0x1000;
void ARQInit(void) { aram(0); }
void ARQReset(void) { }
void ARQPostRequest(ARQRequest* request, ARQAddr owner, u32 type, u32 priority, ARQAddr source, ARQAddr dest,
                    u32 length, ARQCallback callback)
{
	request->next     = nullptr;
	request->owner    = owner;
	request->type     = type;
	request->priority = priority;
	request->source   = source;
	request->dest     = dest;
	request->length   = length;
	request->callback = callback;
	if (type == ARQ_TYPE_MRAM_TO_ARAM)
		aramCopy(ARAM_DIR_MRAM_TO_ARAM, source, dest, length);
	else
		aramCopy(ARAM_DIR_ARAM_TO_MRAM, dest, source, length);
	// Completion interrupt after the transfer time, never before returning.
	arqComplete(callArqCallback, request, length); // w14: ordered, after the poster moved on
}
void ARQRemoveRequest(ARQRequest*) { }
void ARQRemoveOwnerRequest(ARQAddr) { }
void ARQFlushQueue(void) { }
void ARQSetChunkSize(u32 size) { gArqChunk = size; }
u32 ARQGetChunkSize(void) { return gArqChunk; }

// ---- AI ----------------------------------------------------------------------
void AIInit(u8*) { }
void AIReset(void) { }
BOOL AICheckInit(void) { return TRUE; }
AIDCallback AIRegisterDMACallback(AIDCallback callback)
{
	AIDCallback old = gAiCallback;
	gAiCallback     = callback;
	return old;
}
void AIInitDMA(uintptr_t start_addr, u32 length)
{
	gAiStart  = start_addr;
	gAiLength = length;
	// The period follows the length at the next tick (aiTick); reprogramming the
	// slot here would shift the interrupt phase on every buffer.
}
BOOL AIGetDMAEnableFlag(void) { return gAiDmaOn; }
void AIStartDMA(void)
{
	gAiDmaOn = TRUE;
	if (gAiSlot < 0) {
		gAiPeriodSet = aiPeriodNs();
		gAiSlot      = port_irq_add_periodic(aiTick, gAiPeriodSet);
	} else
		port_irq_enable(gAiSlot, 1);
}
void AIStopDMA(void)
{
	gAiDmaOn = FALSE;
	if (gAiSlot >= 0)
		port_irq_enable(gAiSlot, 0);
}
u32 AIGetDMABytesLeft(void) { return 0; }
uintptr_t AIGetDMAStartAddr(void) { return gAiStart; }
u32 AIGetDMALength(void) { return gAiLength; }
AISCallback AIRegisterStreamCallback(AISCallback) { return nullptr; }
u32 AIGetStreamSampleCount(void) { return 0; }
void AIResetStreamSampleCount(void) { }
void AISetStreamTrigger(u32 trigger) { gAiStreamTrigger = trigger; }
u32 AIGetStreamTrigger(void) { return gAiStreamTrigger; }
void AISetStreamPlayState(u32 state) { gAiStreamState = state; }
u32 AIGetStreamPlayState(void) { return gAiStreamState; }
void AISetDSPSampleRate(u32 rate) { gAiDspRate = rate; }
u32 AIGetDSPSampleRate(void) { return gAiDspRate; }
void AISetStreamSampleRate(u32 rate) { gAiStreamRate = rate; }
u32 AIGetStreamSampleRate(void) { return gAiStreamRate; }
void AISetStreamVolLeft(u8 vol) { gAiVolL = vol; }
u8 AIGetStreamVolLeft(void) { return gAiVolL; }
void AISetStreamVolRight(u8 vol) { gAiVolR = vol; }
u8 AIGetStreamVolRight(void) { return gAiVolR; }

// ---- DSP ---------------------------------------------------------------------
void DSPInit(void) { }
BOOL DSPCheckInit(void) { return TRUE; }
void DSPReset(void) { }
void DSPHalt(void) { }
void DSPUnhalt(void) { }
u32 DSPGetDMAStatus(void) { return 0; }
void DSPAssertInt(void) { }
u32 DSPCheckMailToDSP(void) { return 0; } // the fake DSP consumes mail instantly
u32 DSPReadCPUToDSPMbox(void) { return 0; }

u32 DSPCheckMailFromDSP(void)
{
	AcquireSRWLockExclusive(&gMailLock);
	u32 r = gMailCount ? 1 : 0;
	ReleaseSRWLockExclusive(&gMailLock);
	return r;
}

u32 DSPReadMailFromDSP(void)
{
	u32 m = 0;
	AcquireSRWLockExclusive(&gMailLock);
	if (gMailCount) {
		m         = gMailFromDsp[gMailHead];
		gMailHead = (gMailHead + 1) % kMailQ;
		gMailCount--;
	}
	ReleaseSRWLockExclusive(&gMailLock);
	return m;
}

void DSPSendMailToDSP(u32 mail)
{
	if (gCmdExpect < 0) {
		if ((mail >> 16) == 0xCDD1) // task-switch acknowledgements
			return;
		gCmdTotal  = (int)mail;
		gCmdExpect = mail == 0 ? 2 : (int)mail;
		gCmdLen    = 0;
		return;
	}
	if (gCmdLen < 16)
		gCmdData[gCmdLen++] = mail;
	if (--gCmdExpect == 0) {
		gCmdExpect = -1;
		dspCommandDone();
	}
}

void __DSP_boot_task(DSPTaskInfo* task)
{
	(void)task;
	// The microcode announces itself (0xDCD10000), then sends the handshake
	// mail DspHandShake waits for.
	pushMail(0xDCD10000);
	pushMail(0x8071FEED);
	port_irq_raise(dspInterrupt, nullptr, 1000000);
}
uint8_t* port_aram_ptr(uint32_t addr) { return aram(addr); }
uint32_t port_aram_size(void) { return kAramSize; }
// w14: ARQ completion counters for SMS_PORT_PROFILE (port_profile.cpp).
void port_arq_stats(long long* delivered, long long* held)
{
	*delivered = gArqDelivered;
	*held      = gArqHeld;
}

void __DSP_exec_task(DSPTaskInfo*, DSPTaskInfo*) { }
void __DSP_insert_task(DSPTaskInfo*) { }
void __DSP_remove_task(DSPTaskInfo*) { }

} // extern "C"
