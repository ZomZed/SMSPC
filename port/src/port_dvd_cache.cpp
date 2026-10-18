// Host-side read-ahead cache below the DVD layer (w14). sdk_dvd.cpp (w7) reads every
// disc file through here: extracted folders (one host file per disc file) and disc
// images (.iso/.gcm/.ciso and w9's RVZ/WIA/GCZ readers, port_disc.cpp).
//
// - Reads smaller than a chunk are served from 256 KB chunks aligned in the file (folder)
//   or on the disc (image), kept in a 16-chunk LRU (4 MB). The game's stage loads read
//   archives in 1 KB pieces (JKRDvdRipper's Yaz0 stream) and audio in 32 KB pieces, so
//   ~1450 host reads per stage become a few dozen.
// - A read that continues where the previous read of the same source ended starts a
//   background prefetch of the next chunk (one host thread; never more than 2 chunks in
//   flight). A later read of a chunk being fetched waits for it instead of reading twice.
// - Reads of a chunk or more go straight to the destination (whole-archive reads).
// - Only the host copy gets faster. When a read completes for the game is decided by
//   sdk_dvd.cpp as before (its request queue / port_retrace_defer), and the bytes are
//   the same, so game behaviour and replays are unchanged.
// SMS_PORT_DVD_READAHEAD=0 turns it off (direct reads, as before) for A/B runs.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "port_dvd_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef SMS_PORT_AURORA
#include "port_disc.h" // port_disc_read (w9)
#endif

namespace {

const uint32_t kChunk  = 256 * 1024;
const int kSlots       = 16;
const int kMaxInFlight = 2;

struct Slot {
	uint64_t key;    // source id (0 = image, else the host HANDLE)
	uint64_t base;   // chunk start (file offset, or disc offset for the image)
	int32_t valid;   // bytes held (short at the end of the file / disc)
	bool used;
	bool loading;    // being fetched (by the prefetch thread or a reader)
	unsigned long long stamp;
	uint8_t* data;
};

SRWLOCK gLock             = SRWLOCK_INIT;
CONDITION_VARIABLE gLoaded = CONDITION_VARIABLE_INIT; // a slot finished loading
CONDITION_VARIABLE gWork   = CONDITION_VARIABLE_INIT; // prefetch queue not empty
Slot gSlot[kSlots];
unsigned long long gStamp;
int gInFlight;

// Prefetch requests (gLock held).
struct Pref {
	uint64_t key, base, end; // end: where the source ends (no prefetch past it)
	HANDLE h;
};
Pref gQueue[8];
int gQueueCount;

// Sequential detection: the last read end per source (gLock held). A few sources at once.
struct Stream {
	uint64_t key, end;
};
Stream gStreams[8];
int gStreamNext;

int gMode = -1; // -1 unknown, 0 off, 1 on
PortDvdCacheStats gStats;

bool enabled()
{
	if (gMode < 0) {
		const char* e = std::getenv("SMS_PORT_DVD_READAHEAD");
		gMode         = (e != nullptr && e[0] == '0') ? 0 : (e != nullptr && e[0] == 'v') ? 2 : 1;
		if (gMode)
			for (Slot& s : gSlot)
				s.data = (uint8_t*)std::malloc(kChunk);
	}
	return gMode >= 1;
}

// Reads straight from the source.
int32_t fetch(uint64_t key, HANDLE h, uint64_t offset, void* dst, int32_t length)
{
	if (length <= 0)
		return 0;
	if (key == 0) {
#ifdef SMS_PORT_AURORA
		return port_disc_read((uint32_t)offset, dst, length);
#else
		return -1;
#endif
	}
	OVERLAPPED ov = {};
	ov.Offset     = (DWORD)offset;
	ov.OffsetHigh = (DWORD)(offset >> 32);
	DWORD got     = 0;
	if (!ReadFile(h, dst, (DWORD)length, &got, &ov) && GetLastError() != ERROR_HANDLE_EOF)
		return -1;
	return (int32_t)got;
}

// gLock held.
Slot* find(uint64_t key, uint64_t base)
{
	for (Slot& s : gSlot)
		if (s.used && s.key == key && s.base == base)
			return &s;
	return nullptr;
}

// gLock held: least recently used slot that is not loading, or null.
Slot* victim()
{
	Slot* best = nullptr;
	for (Slot& s : gSlot) {
		if (s.loading)
			continue;
		if (!s.used)
			return &s;
		if (!best || s.stamp < best->stamp)
			best = &s;
	}
	return best;
}

// gLock held on entry and exit; fills a claimed slot without holding the lock.
bool load(Slot* s, HANDLE h, uint64_t end)
{
	s->loading = true;
	const uint64_t key = s->key, base = s->base;
	int32_t want       = (int32_t)(end > base ? (end - base < kChunk ? end - base : kChunk) : 0);
	ReleaseSRWLockExclusive(&gLock);
	int32_t got = fetch(key, h, base, s->data, want);
	AcquireSRWLockExclusive(&gLock);
	s->loading = false;
	s->valid   = got < 0 ? 0 : got;
	s->stamp   = ++gStamp;
	if (got < 0)
		s->used = false;
	WakeAllConditionVariable(&gLoaded);
	return got >= 0;
}

DWORD WINAPI prefetchThread(LPVOID)
{
	AcquireSRWLockExclusive(&gLock);
	for (;;) {
		while (gQueueCount == 0)
			SleepConditionVariableSRW(&gWork, &gLock, INFINITE, 0);
		Pref p = gQueue[0];
		std::memmove(gQueue, gQueue + 1, sizeof(Pref) * (size_t)--gQueueCount);
		if (find(p.key, p.base) == nullptr) {
			if (Slot* s = victim()) {
				s->used  = true;
				s->key   = p.key;
				s->base  = p.base;
				s->valid = 0;
				s->stamp = ++gStamp;
				load(s, p.h, p.end);
				++gStats.prefetched;
			}
		}
		--gInFlight;
	}
}

// gLock held.
// Prefetch the chunk at base if it starts before limit; it is loaded up to dataEnd.
void queuePrefetch(uint64_t key, HANDLE h, uint64_t base, uint64_t limit, uint64_t dataEnd)
{
	static bool sThread;
	if (base >= limit || gInFlight >= kMaxInFlight || gQueueCount == 8 || find(key, base) != nullptr)
		return;
	for (int i = 0; i < gQueueCount; i++)
		if (gQueue[i].key == key && gQueue[i].base == base)
			return;
	if (!sThread) {
		sThread = true;
		HANDLE t = CreateThread(nullptr, 1 << 16, prefetchThread, nullptr, 0, nullptr);
		SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
		CloseHandle(t);
	}
	gQueue[gQueueCount++] = { key, base, dataEnd, h };
	++gInFlight;
	WakeConditionVariable(&gWork);
}

// end: where the source's data ends (reads are clipped there); prefEnd: no prefetch at or
// past it (the end of the disc file being read).
int32_t cachedRead(uint64_t key, HANDLE h, uint64_t offset, uint64_t end, uint64_t prefEnd, void* dst, int32_t length)
{
	if (length <= 0)
		return 0;
	if (offset >= end)
		return 0;
	if ((uint64_t)length > end - offset)
		length = (int32_t)(end - offset);
	if (!enabled() || (uint32_t)length >= kChunk) {
		AcquireSRWLockExclusive(&gLock);
		++gStats.direct;
		ReleaseSRWLockExclusive(&gLock);
		return fetch(key, h, offset, dst, length);
	}
	uint8_t* out = (uint8_t*)dst;
	int32_t done = 0;
	AcquireSRWLockExclusive(&gLock);
	// sequential stream? (this read starts where the last read of this source ended)
	bool sequential = false;
	for (Stream& st : gStreams)
		if (st.key == key && st.end != 0 && offset >= st.end && offset - st.end < kChunk) {
			sequential = true;
			break;
		}
	while (done < length) {
		const uint64_t pos  = offset + done;
		const uint64_t base = pos - pos % kChunk;
		Slot* s             = find(key, base);
		if (s != nullptr && s->loading) { // being prefetched: wait instead of reading twice
			while (s->loading && s->key == key && s->base == base)
				SleepConditionVariableSRW(&gLoaded, &gLock, INFINITE, 0);
			s = find(key, base);
			if (s != nullptr)
				++gStats.waited;
		}
		if (s != nullptr && !s->loading) {
			++gStats.hits;
		} else {
			s = victim();
			if (s == nullptr) { // every slot is loading: read directly
				ReleaseSRWLockExclusive(&gLock);
				int32_t r = fetch(key, h, pos, out + done, length - done);
				return r < 0 ? -1 : done + r;
			}
			s->used  = true;
			s->key   = key;
			s->base  = base;
			s->valid = 0;
			++gStats.misses;
			if (!load(s, h, end)) {
				ReleaseSRWLockExclusive(&gLock);
				return -1;
			}
		}
		s->stamp          = ++gStamp;
		const uint32_t in = (uint32_t)(pos - base);
		if ((int32_t)in >= s->valid)
			break; // past what the source has
		int32_t n = s->valid - (int32_t)in;
		if (n > length - done)
			n = length - done;
		std::memcpy(out + done, s->data + in, (size_t)n);
		done += n;
	}
	// remember where this source's read ended; prefetch the next chunk of a stream
	const uint64_t readEnd = offset + done;
	Stream* st             = nullptr;
	for (Stream& x : gStreams)
		if (x.key == key)
			st = &x;
	if (st == nullptr) {
		st = &gStreams[gStreamNext];
		gStreamNext = (gStreamNext + 1) % 8;
		st->key     = key;
	}
	st->end = readEnd;
	if (sequential) {
		const uint64_t next = readEnd - readEnd % kChunk + kChunk;
		queuePrefetch(key, h, next, prefEnd < end ? prefEnd : end, end);
	}
	ReleaseSRWLockExclusive(&gLock);
	return done;
}

// SMS_PORT_DVD_READAHEAD=verify: every cached read is compared with a direct read.
int32_t verified(int32_t r, uint64_t key, HANDLE h, uint64_t offset, uint64_t end, void* dst, int32_t length)
{
	if (gMode != 2)
		return r;
	if (offset < end && (uint64_t)length > end - offset)
		length = (int32_t)(end - offset);
	uint8_t* ref = (uint8_t*)std::malloc(length > 0 ? (size_t)length : 1);
	int32_t r2   = offset < end ? fetch(key, h, offset, ref, length) : 0;
	static long long sChecked, sBad;
	++sChecked;
	if (r2 != r || (r > 0 && std::memcmp(ref, dst, (size_t)r) != 0)) {
		++sBad;
		std::fprintf(stderr, "[port] read-ahead VERIFY MISMATCH: key %llx offset 0x%llx length %d: %d vs %d\n",
		             (unsigned long long)key, (unsigned long long)offset, length, r, r2);
	}
	if ((sChecked & 1023) == 0)
		std::fprintf(stderr, "[port] read-ahead verify: %lld reads checked, %lld mismatches\n", sChecked, sBad);
	std::free(ref);
	return r;
}

} // namespace

extern "C" {

int32_t port_dvd_cache_read_file(void* h, uint32_t fileSize, uint32_t offset, void* dst, int32_t length)
{
	int32_t r = cachedRead((uint64_t)(uintptr_t)h, (HANDLE)h, offset, fileSize, fileSize, dst, length);
	return verified(r, (uint64_t)(uintptr_t)h, (HANDLE)h, offset, fileSize, dst, length);
}

int32_t port_dvd_cache_read_image(uint32_t discOffset, uint32_t fileEnd, void* dst, int32_t length)
{
	// Chunks are aligned on the disc. Reads past the file's end see the following disc
	// bytes as before (port_disc_read stops at the end of the disc); prefetch stays in the file.
	int32_t r = cachedRead(0, INVALID_HANDLE_VALUE, discOffset, 0x100000000ull, fileEnd, dst, length);
	return verified(r, 0, INVALID_HANDLE_VALUE, discOffset, 0x100000000ull, dst, length);
}

void port_dvd_cache_stats(PortDvdCacheStats* out)
{
	AcquireSRWLockExclusive(&gLock);
	*out = gStats;
	ReleaseSRWLockExclusive(&gLock);
}

} // extern "C"
