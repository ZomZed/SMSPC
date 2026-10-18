/*
 * port_asan.h - AddressSanitizer manual poisoning for the game's own heaps
 * (worker w10; BOOTLOG "Sanitizers (w10)").
 *
 * The game allocates from JKRExpHeap / JKRSolidHeap inside the fixed 32 MB RAM
 * mapping, which ASan cannot see. In `build_aurora.py --asan` builds
 * (SMS_PORT_ASAN), those heaps poison their free memory and the slack after each
 * allocation, so an overrun from one game allocation into a neighbour or into free
 * memory is reported like a malloc overrun. The two heap units are compiled
 * without ASan instrumentation (they walk their own free-block headers, which stay
 * poisoned), so they call the runtime directly. No-ops in every other build.
 *
 * Granularity: allocations are unpoisoned up to ALIGN_NEXT(size, 4), the rounding
 * the GameCube heaps used; bytes past that would have been the next block header
 * on the GameCube too.
 */
#ifndef PORT_ASAN_H
#define PORT_ASAN_H

#include <stddef.h>

#ifdef SMS_PORT_ASAN
#ifdef __cplusplus
extern "C" {
#endif
void __asan_poison_memory_region(void const volatile* addr, size_t size);
void __asan_unpoison_memory_region(void const volatile* addr, size_t size);
#ifdef __cplusplus
}
#endif
#define PORT_ASAN_POISON(p, n)   __asan_poison_memory_region((const void*)(p), (size_t)(n))
#define PORT_ASAN_UNPOISON(p, n) __asan_unpoison_memory_region((const void*)(p), (size_t)(n))
#else
#define PORT_ASAN_POISON(p, n)   ((void)0)
#define PORT_ASAN_UNPOISON(p, n) ((void)0)
#endif

/* GameCube allocation rounding (4 bytes) */
#define PORT_ASAN_GC_SIZE(n) (((size_t)(n) + 3) & ~(size_t)3)

#endif /* PORT_ASAN_H */
