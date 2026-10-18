// Host-side read-ahead cache below the DVD layer (w14, port_dvd_cache.cpp).
// sdk_dvd.cpp reads disc files through it; SMS_PORT_DVD_READAHEAD=0 turns it off,
// SMS_PORT_DVD_READAHEAD=verify compares every cached read with a direct read.
// It only changes how fast the host copy is made, never when a read completes for the
// game or what it returns.
#ifndef PORT_DVD_CACHE_H
#define PORT_DVD_CACHE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Extracted folder: reads [offset, offset + length) of the open host file `h` (a HANDLE)
// of `fileSize` bytes. Returns the bytes read (short at the end of the file) or -1.
int32_t port_dvd_cache_read_file(void* h, uint32_t fileSize, uint32_t offset, void* dst, int32_t length);

// Disc image: reads `length` bytes at disc offset `discOffset` (port_disc_read; past the
// file's end the following disc bytes, as before). `fileEnd` (disc offset of the end of
// the file being read) only bounds the prefetch. Returns bytes read or -1.
int32_t port_dvd_cache_read_image(uint32_t discOffset, uint32_t fileEnd, void* dst, int32_t length);

typedef struct {
	long long hits, misses, waited, prefetched, direct;
} PortDvdCacheStats;
void port_dvd_cache_stats(PortDvdCacheStats* out);

#ifdef __cplusplus
}
#endif

#endif
