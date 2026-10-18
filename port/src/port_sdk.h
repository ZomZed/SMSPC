/*
 * port_sdk.h - internal glue shared by the host implementations of the
 * Dolphin SDK (port/src/sdk_*.cpp). Not seen by game code.
 *
 * Files:
 *   sdk_os_thread.cpp  threads/scheduler, interrupts, messages, mutexes,
 *                      conds, alarms, interrupt thread  (kept with Aurora)
 *   sdk_audio.cpp      AI / AR / ARQ / DSP (fake JAudio DSP)  (kept with Aurora
 *                      for AI/ARQ/DSP; AR overlaps Aurora)
 *   sdk_os_misc.cpp, sdk_osalloc.cpp, sdk_dvd.cpp, sdk_vi.cpp, sdk_pad.cpp,
 *   sdk_misc.c         interim pieces for the headless boot; Aurora's
 *                      lib/dolphin replaces them.
 */
#ifndef PORT_SDK_H
#define PORT_SDK_H

#include <dolphin/os.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the default thread (the calling thread, priority 16) and the
 * interrupt thread. Idempotent; runs before any game static constructor. */
void port_thread_init(void);

/* Interim machine bring-up (main RAM mapping, low-memory globals, clocks).
 * Idempotent. Replaced by Aurora's OSInit. */
void port_os_init(void);

/* A scheduling point: takes a switch requested by an interrupt handler if
 * interrupts are enabled. Call from host code that game threads poll
 * (e.g. OSGetTime) so busy-waits see interrupt-driven wakeups promptly. */
void port_os_poll(void);
/* w14: the same, at the extra scheduling points w14 added (port_GXCallDisplayList,
 * JKRHeap::alloc, DVDReadPrio); SMS_PORT_W14_LEGACY=1 turns them off for A/B runs. */
void port_sched_point(void);
/* w14: nonzero if t is the game thread holding the CPU (diagnostic-grade, racy). */
int port_thread_is_running(OSThread* t);
/* w14: changes whenever game thread t passes an interrupt-enabled scheduling point
 * (racy read from other threads; compare for equality only). */
unsigned port_thread_sched_epoch(OSThread* t);

/* Runs fn(arg) "as an interrupt handler": interrupts are disabled for the
 * duration and no thread switch happens inside it. Usable from game threads
 * (e.g. to deliver a DVD/ARQ completion callback) and from host threads. */
void port_run_as_interrupt(void (*fn)(void* arg), void* arg);

/* w14: host blocking work (big DVD reads) without holding the emulated CPU, like a
 * GameCube thread asleep during a DMA. Between the two calls the caller may touch no
 * game state except the buffer it fills. begin returns 0 (end then does nothing)
 * outside a game thread or with the scheduler disabled. */
int port_blocking_begin(void);
void port_blocking_end(int began);

/* w14: host-only work (the Aurora present) during which the caller keeps the CPU
 * against lower-priority threads, but a higher-priority thread readied by an
 * interrupt runs at once, as the GameCube would preempt. Interrupts must be enabled
 * (begin returns 0 otherwise). */
int port_preemptible_begin(void);
void port_preemptible_end(int began);

/* Periodic "hardware interrupt" serviced by the interrupt thread; the handler
 * runs in interrupt context. Returns a slot id. */
int port_irq_add_periodic(void (*fn)(void), long long period_ns);
void port_irq_set_period(int slot, long long period_ns);
void port_irq_enable(int slot, int enable);

/* One-shot device interrupt: fn(arg) runs in interrupt context on the
 * interrupt thread after delay_ns (once interrupts are enabled). */
void port_irq_raise(void (*fn)(void* arg), void* arg, long long delay_ns);

/* Device completions (sdk_os_thread.cpp): fn(arg) runs in interrupt context strictly
 * after the caller returned, in the order deferred, at the next VI retrace or as soon
 * as no game thread is runnable, whichever comes first. Like real hardware, other
 * game threads get to run before a transfer completes, and the point is deterministic
 * in game terms. Work deferred from inside such a handler waits for the next drain.
 * Used for DVD (sdk_dvd.cpp); also meant for ARQ/AR DMA completions.
 * port_retrace_run() is called by the VI retrace interrupt (port_main.cpp); a build
 * whose VI never calls it (build_port.py) gets a 1 ms one-shot interrupt instead. */
void port_retrace_defer(void (*fn)(void* arg), void* arg);
void port_retrace_run(void);
/* Also drains the deferred queue after delay_ns of host time (a device's own
 * completion time bound, e.g. a disc transfer). */
void port_defer_kick(long long delay_ns);
/* Nonzero where OSSleepThread cannot block: interrupt context, or a host thread
 * that is not a game thread (THP reader, card worker, ...). */
int port_cannot_block(void);

/* Host allocation scope (JKRHeap.cpp): while pushed on the calling thread,
 * the global operator new uses the CRT heap instead of the current JKRHeap,
 * so host-side tables neither eat game memory nor die with a scene heap.
 * operator delete routes by address, so no scope is needed to free. */
void port_host_alloc_push(void);
void port_host_alloc_pop(void);

/* Prints the calling thread's symbolized stack to stderr (port_crash.cpp);
 * used by OSPanic. */
void port_print_backtrace(void);
/* printf to stderr and to sms_port_crash.log next to the exe (port_crash.cpp). */
void port_crash_log(const char* fmt, ...);
/* Suspends a thread and prints its stack (port_crash.cpp, x64 only). */
void port_dump_thread(unsigned long tid);

/* Interim main RAM layout: mapped at its GameCube address so hardware-address
 * accesses in game code work unchanged. */
#define PORT_MRAM_BASE   0x80000000u
#define PORT_MRAM_MIRROR 0xC0000000u
/* 176 MB: the GameCube's 24 MB and room for PC-only content (the island test stage); the stage heap
 * takes whatever is left (TApplication::initialize), and the uncached mirror at 0xC0000000 must end
 * below the 0xCC000000 register window. */
#define PORT_MRAM_SIZE   0x0B000000u

#define PORT_BUS_CLOCK  162000000u
#define PORT_CORE_CLOCK 486000000u

#ifdef __cplusplus
}
#endif

#endif
