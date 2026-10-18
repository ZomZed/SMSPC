/*
 * port_profile.h - SMS_PORT_PROFILE=1 frame/load profiler for the PC port (w14).
 * Implementation: port/src/port_profile.cpp. See port/BOOTLOG.md "Performance (w14)".
 *
 * Off by default: every hook is one test of gPortProfile (a plain global read)
 * until SMS_PORT_PROFILE=1 is set, so the hooks can stay in the build.
 *
 * Each scope adds QueryPerformanceCounter ticks to a category. Categories are
 * summed per presented frame (main thread, port_main.cpp) and printed every
 * SMS_PORT_PROFILE_N presented frames (default 300) as avg / p99 / max ms per
 * frame, plus the present-to-present interval (frame pacing). Load categories
 * (DVD, Yaz0, swap, heap, setupObjects) are also summed per stage load and printed
 * when the stage's first frame is presented.
 *
 * C and C++. Outside the Aurora game build (host tools that include the swap
 * headers, build_port.py) the macros compile to nothing.
 */
#ifndef PORT_PROFILE_H
#define PORT_PROFILE_H

#ifdef __cplusplus
extern "C" {
#endif

enum PortProfCat {
	PP_DIRECT,     /* game: TApplication::gameLoop mDirector->direct() (logic + GX command generation) */
	PP_MOVE,       /* game: TMarDirector simulation ticks (movement / calcAnim perform lists) */
	PP_GXGEN,      /* game: TMarDirector draw perform lists (J3D/GX command generation) */
	PP_SETUP,      /* load: TMarDirector::setupObjects (main thread) */
	PP_VIWAIT,     /* main thread asleep in VIWaitForRetrace */
	PP_WAKELAT,    /* retrace interrupt -> main thread running again (scheduler + timer latency) */
	PP_FIFO,       /* waiting for Aurora's FIFO worker: the game's per-frame GXDrawDone
	                  (THPPlayerDrawDone) and the drain before the present */
	PP_ENDFRAME,   /* present: aurora_end_frame (pipelines, hand-off to the render worker, imgui) */
	PP_BEGINFRAME, /* present: event pump + aurora_begin_frame (frame-slot wait = GPU back-pressure) */
	PP_AUDIO,      /* audio: port_dsp_render_subframe (runs on the game thread that releases the DSP) */
	PP_HANDOFF,    /* scheduler: dispatched -> running latency, all game threads */
	PP_INTRWAIT,   /* scheduler: contended OSDisableInterrupts (waiting for the interrupt thread) */
	PP_IRQLATE,    /* interrupt thread: VI retrace interrupt lateness vs. its due time */
	PP_PREEMPT,    /* scheduler: interrupt readied a higher-priority thread -> running thread reached a
	                  scheduling point (the GameCube preempts at once; host threads cannot) */
	PP_DVD,        /* load: DVD reads (host file I/O) */
	PP_YAZ0,       /* load: Yaz0/Yay0 decompression (inclusive of the DVD reads it does) */
	PP_SWAP,       /* load: big-endian swap passes */
	PP_HEAP,       /* JKRHeap::alloc / JKRHeap::free (static entry points) */
	PP_NCATS
};

extern int gPortProfile; /* non-zero when SMS_PORT_PROFILE=1 */

long long port_prof_now(void);                                /* QPC ticks */
void port_prof_add(int cat, long long ticks);                 /* thread-safe */
void port_prof_add_bytes(int cat, long long ticks, long long bytes);
/* Main thread, once per presented frame (port_main.cpp). */
void port_prof_frame(void);
/* TApplication::proc state change (Application.cpp): starts a load measurement. */
void port_prof_stage(int state, int area, int scenario);
/* The stage's own frames start (TMarDirector after setupObjects, any other
 * director on its first direct()): the next presented frame ends the load. */
void port_prof_stage_ready(void);
/* Retrace interrupt timestamp (port_main.cpp viRetrace) and main-thread wake. */
void port_prof_retrace(void);
void port_prof_main_woke(void);

#ifdef __cplusplus
}
#endif

#if defined(SMS_PORT_AURORA) && !defined(PORT_PROFILE_DISABLE)
#define PORT_PROF_BEGIN(v) long long v = gPortProfile ? port_prof_now() : 0
#define PORT_PROF_END(cat, v)                                                                    \
	do {                                                                                         \
		if (gPortProfile)                                                                        \
			port_prof_add((cat), port_prof_now() - (v));                                         \
	} while (0)
#define PORT_PROF_END_BYTES(cat, v, n)                                                           \
	do {                                                                                         \
		if (gPortProfile)                                                                        \
			port_prof_add_bytes((cat), port_prof_now() - (v), (long long)(n));                   \
	} while (0)
#ifdef __cplusplus
struct PortProfScope {
	int cat;
	long long t0;
	explicit PortProfScope(int c)
	    : cat(c)
	    , t0(gPortProfile ? port_prof_now() : 0)
	{
	}
	~PortProfScope()
	{
		if (gPortProfile)
			port_prof_add(cat, port_prof_now() - t0);
	}
};
#define PORT_PROF_CAT2(a, b) a##b
#define PORT_PROF_CAT(a, b) PORT_PROF_CAT2(a, b)
#define PORT_PROF_SCOPE(cat) PortProfScope PORT_PROF_CAT(sPortProfScope, __LINE__)(cat)
#endif
#else
#define PORT_PROF_BEGIN(v)
#define PORT_PROF_END(cat, v) ((void)0)
#define PORT_PROF_END_BYTES(cat, v, n) ((void)0)
#define PORT_PROF_SCOPE(cat)
#endif

#endif /* PORT_PROFILE_H */
