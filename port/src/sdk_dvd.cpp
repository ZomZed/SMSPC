// Interim folder-based DVD for the PC port: the disc's file system is the
// host folder "files/" under the working directory (an extracted disc), or
// %SMS_PORT_DISC% if set. An FST-like entry table is built by walking that
// folder (directories before their children, names sorted case-insensitively)
// so DVDConvertPathToEntrynum / DVDFastOpen / DVDReadDir work as on hardware.
// Path rules follow the SDK's dvdfs.c: case-insensitive names, '/' starts at
// the root, otherwise relative to the DVDChangeDir directory, "." and ".."
// are understood, repeated slashes are ignored.
// Reads go through one queue and complete in interrupt context once no game thread
// can run or at the next VI retrace (see Req below).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "port_sdk.h"
#include "port_profile.h" // w14
#include "port_dvd_cache.h" // w14: host read-ahead cache (SMS_PORT_DVD_READAHEAD)

#include <dolphin/dvd.h>
#include <dolphin/os.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef SMS_PORT_AURORA
#include "port_disc.h" // w9: disc images (.iso/.gcm/.ciso) and the data-location search
#endif

namespace {

struct Entry {
	std::string name;     // as on disc
	std::string hostPath; // full host path
	bool isDir;
	s32 parent;
	s32 next; // dirs: index after the last descendant
	u32 size;
	HANDLE h;
	u32 discOffset; // w9: disc images only
};

std::vector<Entry> gFst;
bool gImage; // w9: entries come from a disc image's FST (port_disc.cpp)
s32 gCurrDir;
bool gBuilt;
DVDDiskID gDiskID = { { 'G', 'M', 'S', 'E' }, { '0', '1' }, 0, 0, 0, 0, {} };

bool lessNoCase(const std::string& a, const std::string& b)
{
	return _stricmp(a.c_str(), b.c_str()) < 0;
}

void walk(const std::string& host, s32 parent)
{
	struct Item {
		std::string name;
		bool dir;
		u32 size;
	};
	std::vector<Item> items;
	WIN32_FIND_DATAA fd;
	HANDLE f = FindFirstFileA((host + "\\*").c_str(), &fd);
	if (f == INVALID_HANDLE_VALUE)
		return;
	do {
		if (!std::strcmp(fd.cFileName, ".") || !std::strcmp(fd.cFileName, ".."))
			continue;
		items.push_back({ fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0,
		                  fd.nFileSizeLow });
	} while (FindNextFileA(f, &fd));
	FindClose(f);
	std::sort(items.begin(), items.end(),
	          [](const Item& a, const Item& b) { return lessNoCase(a.name, b.name); });
	for (const Item& it : items) {
		s32 idx = (s32)gFst.size();
		gFst.push_back({ it.name, host + "\\" + it.name, it.dir, parent, idx + 1, it.size,
		                 INVALID_HANDLE_VALUE });
		if (it.dir) {
			walk(host + "\\" + it.name, idx);
			gFst[idx].next = (s32)gFst.size();
		}
	}
}

void build()
{
	if (gBuilt)
		return;
	gBuilt = true;
	port_host_alloc_push(); // the table is host data, keep it off the game heaps
#ifdef SMS_PORT_AURORA
	// w9: port_disc.cpp picks the game data (command line, SMS_PORT_DISC, port_settings.ini,
	// auto-detection, then a file dialog) and checks it is the US disc. A disc image is read
	// in place with its own FST (disc order); a folder is walked as below.
	{
		std::string folder;
		std::vector<PortDiscEntry> img;
		if (port_disc_locate(&folder, &img)) {
			for (const PortDiscEntry& e : img)
				gFst.push_back({ e.name, std::string(), e.isDir, e.parent, e.next, e.size, INVALID_HANDLE_VALUE,
				                 e.offset });
			gImage = true;
		} else {
			gFst.push_back({ "", folder, true, 0, 1, 0, INVALID_HANDLE_VALUE, 0 });
			walk(folder, 0);
			gFst[0].next = (s32)gFst.size();
		}
		gCurrDir = 0;
		std::printf("[port] DVD: %u entries from %s\n", (unsigned)gFst.size(),
		            gImage ? "the disc image" : folder.c_str());
		port_host_alloc_pop();
		return;
	}
#endif
	char cwd[MAX_PATH];
	const char* env = std::getenv("SMS_PORT_DISC");
	std::string root;
	if (env && *env) {
		root = env;
	} else {
		GetCurrentDirectoryA(MAX_PATH, cwd);
		root = std::string(cwd) + "\\files";
		// Double-click launch: no SMS_PORT_DISC and no ./files. Walk up from the exe's
		// directory looking for dolphin_test\files (the extracted disc next to the
		// workers), then files.
		if (GetFileAttributesA(root.c_str()) == INVALID_FILE_ATTRIBUTES) {
			char exe[MAX_PATH];
			DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
			std::string dir(exe, n < MAX_PATH ? n : 0);
			for (size_t cut = dir.find_last_of("\\/"); cut != std::string::npos;
			     cut = dir.find_last_of("\\/")) {
				dir.resize(cut);
				const char* const kTry[] = { "\\dolphin_test\\files", "\\files" };
				bool found = false;
				for (const char* t : kTry) {
					std::string cand = dir + t;
					DWORD a = GetFileAttributesA(cand.c_str());
					if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
						root  = cand;
						found = true;
						break;
					}
				}
				if (found)
					break;
			}
		}
	}
	gFst.push_back({ "", root, true, 0, 1, 0, INVALID_HANDLE_VALUE });
	walk(root, 0);
	gFst[0].next = (s32)gFst.size();
	gCurrDir     = 0;
	std::printf("[port] DVD: %u entries from %s\n", (unsigned)gFst.size(), root.c_str());
	port_host_alloc_pop();
}

s32 findChild(s32 dir, const char* name, size_t len)
{
	for (s32 i = dir + 1; i < gFst[dir].next; i = gFst[i].isDir ? gFst[i].next : i + 1) {
		const std::string& n = gFst[i].name;
		if (n.size() == len && _strnicmp(n.c_str(), name, len) == 0)
			return i;
	}
	return -1;
}

s32 convert(const char* path)
{
	build();
	s32 dir = gCurrDir;
	if (*path == '/') {
		dir = 0;
		while (*path == '/')
			path++;
	}
	while (*path) {
		const char* end = path;
		while (*end && *end != '/')
			end++;
		size_t len = end - path;
		if (len == 1 && path[0] == '.') {
		} else if (len == 2 && path[0] == '.' && path[1] == '.') {
			dir = gFst[dir].parent;
		} else {
			s32 e = findChild(dir, path, len);
			if (e < 0)
				return -1;
			if (!gFst[e].isDir) {
				while (*end == '/')
					end++;
				return *end ? -1 : e;
			}
			dir = e;
		}
		path = end;
		while (*path == '/')
			path++;
	}
	return dir;
}

s32 doReadImpl(DVDFileInfo* fi, void* addr, s32 length, s32 offset);

// w14: SMS_PORT_PROFILE times every read (port_profile.h, PP_DVD).
s32 doRead(DVDFileInfo* fi, void* addr, s32 length, s32 offset)
{
	PORT_PROF_BEGIN(t0);
	s32 r = doReadImpl(fi, addr, length, offset);
	PORT_PROF_END_BYTES(PP_DVD, t0, length);
	return r;
}

// w14: reads at least this big release the emulated CPU (DVDReadPrio).
const s32 kBlockingReadMin = 64 * 1024;
// w14: opens the host file of fi's entry if needed (folder mode), so the entry table is
// only written while holding the CPU; false if the entry cannot be read.
bool openHandle(DVDFileInfo* fi)
{
	s32 e = (s32)fi->startAddr;
	if (e <= 0 || e >= (s32)gFst.size() || gFst[e].isDir)
		return false;
	if (gImage)
		return true;
	Entry& en = gFst[e];
	if (en.h == INVALID_HANDLE_VALUE)
		en.h = CreateFileA(en.hostPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		                   FILE_ATTRIBUTE_NORMAL, nullptr);
	return en.h != INVALID_HANDLE_VALUE;
}

s32 doReadImpl(DVDFileInfo* fi, void* addr, s32 length, s32 offset)
{
	s32 e = (s32)fi->startAddr;
	if (e <= 0 || e >= (s32)gFst.size() || gFst[e].isDir || length < 0)
		return DVD_RESULT_FATAL_ERROR;
	Entry& en = gFst[e];
#ifdef SMS_PORT_AURORA
	if (gImage) { // w9: read the image; past the file's end come the following disc bytes, as on hardware
		// w14: through the read-ahead cache (port_dvd_cache.cpp; same bytes, thread-safe)
		const s32 r = port_dvd_cache_read_image(en.discOffset + (u32)offset, en.discOffset + en.size, addr, length);
		if (r < 0)
			return DVD_RESULT_FATAL_ERROR;
		if (r < length)
			std::memset((u8*)addr + r, 0, length - r);
		fi->cb.transferredSize = length;
		return length;
	}
#endif
	if (en.h == INVALID_HANDLE_VALUE)
		en.h = CreateFileA(en.hostPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
		                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (en.h == INVALID_HANDLE_VALUE)
		return DVD_RESULT_FATAL_ERROR;
	DWORD got = 0;
	if (offset < (s32)en.size) {
		OVERLAPPED ov = {};
		ov.Offset     = (DWORD)offset;
		DWORD want    = (DWORD)std::min<s64>(length, (s64)en.size - offset);
		// w14: through the read-ahead cache (port_dvd_cache.cpp; same bytes, thread-safe)
		const s32 r = port_dvd_cache_read_file(en.h, en.size, (u32)offset, addr, (s32)want);
		if (r < 0)
			return DVD_RESULT_FATAL_ERROR;
		got = (DWORD)r;
		(void)ov;
	}
	if ((s32)got < length) // reads past the end see the following disc bytes
		std::memset((u8*)addr + got, 0, length - got);
	fi->cb.transferredSize = length;
	return length;
}

// The drive. As on hardware, a request returns at once (async) or blocks only its
// own thread (sync), and the transfer completes later in interrupt context, in issue
// order: here when no game thread is runnable or at the next VI retrace, whichever
// comes first (port_retrace_defer). So a callback never runs before the caller's next
// statement, and other threads run while a sync read is "in flight". Game and
// JSystem code relies on both: it sets "loading" flags after issuing reads, and a
// higher-priority loader thread (JASDvdThread) must not finish before its caller
// continues.
// gReqs and gPending are only touched with interrupts disabled or in interrupt context.
// The host file read itself (W7-J) runs on the drive thread below, started when the
// request is issued, without the interrupt lock; the completion only picks up its
// result. Before, the read ran inside the completion, holding the interrupt lock, so
// a big async read stalled every game thread and the VI retrace that drained it.
struct Req {
	DVDFileInfo* fi;
	void* addr;
	s32 length;
	s32 offset;
	bool stream; // DVDPrepareStreamAsync: no data
	bool sync;   // DVDReadPrio: the caller sleeps on gSyncQueue until done
	bool used;
	bool canceled; // DVDCancel*: already called back
	bool done;
	s32 result;
	// Drive-thread state, under gIoLock: 0 queued, 1 reading, 2 transferred.
	int io;
	s32 ioResult;
	bool redeferred; // the completion ran before the transfer ended and was put back
};
const int kNumReqs = 256;
Req gReqs[kNumReqs];
int gPending; // requests issued and not yet completed
OSThreadQueue gSyncQueue; // DVDReadPrio callers waiting for their request
bool gQueueInit;

// The drive thread: a host thread that does the queued transfers in issue order. Lock
// order: the interrupt lock, then gIoLock; the drive thread never takes the interrupt
// lock while holding gIoLock or gReadLock, so interrupt context may wait on it.
SRWLOCK gIoLock = SRWLOCK_INIT;
CONDITION_VARIABLE gIoCv = CONDITION_VARIABLE_INIT; // a transfer ended / a request was queued
Req* gIoFifo[kNumReqs];
int gIoHead, gIoCount;
// Serialises host reads: the drive thread's and DVDReadPrio's inline ones (disc images
// keep shared decode state; folder mode opens handles lazily).
SRWLOCK gReadLock = SRWLOCK_INIT;

s32 readLocked(DVDFileInfo* fi, void* addr, s32 length, s32 offset)
{
	AcquireSRWLockExclusive(&gReadLock);
	s32 r = doRead(fi, addr, length, offset);
	ReleaseSRWLockExclusive(&gReadLock);
	return r;
}

DWORD WINAPI driveThread(void*)
{
	for (;;) {
		AcquireSRWLockExclusive(&gIoLock);
		while (gIoCount == 0)
			SleepConditionVariableSRW(&gIoCv, &gIoLock, INFINITE, 0);
		Req* r   = gIoFifo[gIoHead];
		gIoHead  = (gIoHead + 1) % kNumReqs;
		--gIoCount;
		// A canceled request transfers nothing (DVDCancel stops the drive).
		const bool skip = r->canceled || r->stream;
		r->io           = 1;
		ReleaseSRWLockExclusive(&gIoLock);
		const s32 res = skip ? 0 : readLocked(r->fi, r->addr, r->length, r->offset);
		AcquireSRWLockExclusive(&gIoLock);
		r->ioResult           = res;
		r->io                 = 2;
		const bool redeferred = r->redeferred;
		WakeAllConditionVariable(&gIoCv);
		ReleaseSRWLockExclusive(&gIoLock);
		if (redeferred) // its completion is waiting for this: drain now
			port_defer_kick(0);
	}
}

void initQueue()
{
	if (!gQueueInit) {
		gQueueInit = true;
		OSInitThreadQueue(&gSyncQueue);
		HANDLE h = CreateThread(nullptr, 1 << 16, driveThread, nullptr,
		                        STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
		SetThreadPriority(h, THREAD_PRIORITY_ABOVE_NORMAL);
		CloseHandle(h);
	}
}

void completeReq(void* p) // interrupt context
{
	Req* r          = (Req*)p;
	DVDFileInfo* fi = r->fi;
	// Completions drain in issue order, like the transfers, so when this one's transfer
	// has not ended yet no later one has either. Instead of holding the interrupt lock
	// for it, the completion goes back on the queue (behind the later ones, which do the
	// same) and the drive thread drains again when it ends. With a warm file cache the
	// transfer is long done by the time the completion runs, so the timing in game terms
	// is unchanged. (Canceled requests wait too: the drive thread still holds the slot.)
	AcquireSRWLockExclusive(&gIoLock);
	const bool transferred = r->io == 2;
	if (!transferred)
		r->redeferred = true;
	ReleaseSRWLockExclusive(&gIoLock);
	if (!transferred) {
		port_retrace_defer(completeReq, r);
		return;
	}
	const bool canceled = r->canceled;
	s32 result          = canceled ? DVD_RESULT_CANCELED : 0;
	if (!canceled) {
		result       = r->ioResult;
		fi->cb.state = result < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
	}
	--gPending;
	if (r->sync) { // the waiting caller frees the slot
		r->result = result;
		r->done   = true;
		OSWakeupThread(&gSyncQueue);
		return;
	}
	r->used = false;
	if (!canceled && fi->callback)
		fi->callback(result, fi);
	if (gPending == 0) // inline sync readers wait for the queue to drain
		OSWakeupThread(&gSyncQueue);
}

Req* queueReq(DVDFileInfo* fi, void* addr, s32 length, s32 offset, bool stream, bool sync,
              DVDCallback callback)
{
	initQueue();
	// Under the interrupt lock from any thread (port_run_as_interrupt), so a host
	// thread's request cannot race the completion handler.
	struct Ctx {
		Req want;
		DVDCallback callback;
		Req* got;
	} c = { { fi, addr, length, offset, stream, sync, true, false, false, 0, 0, 0, false }, callback,
		    nullptr };
	port_run_as_interrupt(
	    [](void* p) {
		    Ctx* c = (Ctx*)p;
		    for (Req& s : gReqs)
			    if (!s.used) {
				    c->got = &s;
				    break;
			    }
		    if (c->got == nullptr)
			    return;
		    *c->got         = c->want;
		    DVDFileInfo* fi = c->want.fi;
		    if (!c->want.sync)
			    fi->callback = c->callback;
		    fi->cb.state = gPending > 0 ? DVD_STATE_WAITING : DVD_STATE_BUSY;
		    ++gPending;
		    AcquireSRWLockExclusive(&gIoLock); // the drive thread starts the transfer now
		    gIoFifo[(gIoHead + gIoCount++) % kNumReqs] = c->got;
		    WakeAllConditionVariable(&gIoCv);
		    ReleaseSRWLockExclusive(&gIoLock);
		    port_retrace_defer(completeReq, c->got);
	    },
	    &c);
	if (c.got == nullptr) {
		std::fprintf(stderr, "[port] DVD: request queue full\n");
		return nullptr;
	}
	// Transfer time bound, like the drive's: 0.2 ms + length at 20 MB/s (faster than
	// the GameCube's ~3 MB/s, so loads stay quick). Without it a read issued while
	// another thread runs a long frame would wait for that thread to sleep.
	port_defer_kick(200000 + (long long)length * 50);
	return c.got;
}

// Cancels the pending request of this block, if any: the block's callback gets
// DVD_RESULT_CANCELED now, its queued completion does nothing. Returns whether one was pending.
bool cancelReq(DVDCommandBlock* block)
{
	struct Ctx {
		DVDCommandBlock* block;
		bool found;
	} c = { block, false };
	port_run_as_interrupt(
	    [](void* p) {
		    Ctx* c = (Ctx*)p;
		    for (Req& r : gReqs)
			    if (r.used && !r.canceled && &r.fi->cb == c->block) {
				    // The drive stops: a transfer in progress ends before this returns,
				    // a queued one never starts (the caller may reuse the buffer).
				    AcquireSRWLockExclusive(&gIoLock);
				    r.canceled = true;
				    while (r.io == 1)
					    SleepConditionVariableSRW(&gIoCv, &gIoLock, INFINITE, 0);
				    ReleaseSRWLockExclusive(&gIoLock);
				    r.fi->cb.state = DVD_STATE_CANCELED;
				    c->found       = true;
				    if (!r.sync && r.fi->callback) // a sync caller gets the result on completion
					    r.fi->callback(DVD_RESULT_CANCELED, r.fi);
				    return;
			    }
	    },
	    &c);
	return c.found;
}

} // namespace

extern "C" {

void DVDInit() { build(); }
s32 DVDConvertPathToEntrynum(char* path) { return convert(path); }

BOOL DVDFastOpen(s32 entrynum, DVDFileInfo* fi)
{
	build();
	if (entrynum <= 0 || entrynum >= (s32)gFst.size() || gFst[entrynum].isDir)
		return FALSE;
	fi->startAddr = (u32)entrynum; // stands in for the disc offset
	fi->length    = gFst[entrynum].size;
	fi->callback  = nullptr;
	fi->cb.state  = DVD_STATE_END;
	return TRUE;
}

BOOL DVDOpen(char* fileName, DVDFileInfo* fi)
{
	s32 e = convert(fileName);
	if (e < 0) {
		std::printf("[port] DVDOpen: %s not found\n", fileName);
		return FALSE;
	}
	return DVDFastOpen(e, fi);
}

BOOL DVDClose(DVDFileInfo*) { return TRUE; }

s32 DVDReadPrio(DVDFileInfo* fi, void* addr, s32 length, s32 offset, s32)
{
	// The hardware behaviour (default): the caller sleeps while its request is in flight,
	// queued behind earlier requests, and other threads run meanwhile. JAudio relies on
	// it (JAIGFrameSequence sets mLoadingFlag after handing the read to the
	// higher-priority JASDvdThread; stage BGM is refused otherwise). The file-select
	// crash it once exposed no longer reproduces after the JAudio table/bitfield fixes
	// (w10: 48 smoke runs incl. ASan/UBSan). SMS_PORT_DVD_SYNC_BLOCK=0: the old inline
	// reads (after waiting for the async requests issued before them), for comparison.
	static const bool sBlock = [] {
		const char* e = std::getenv("SMS_PORT_DVD_SYNC_BLOCK");
		return e == nullptr || std::strcmp(e, "0") != 0;
	}();
	if (!sBlock && !port_cannot_block()) {
		initQueue();
		BOOL en = OSDisableInterrupts();
		while (gPending > 0)
			OSSleepThread(&gSyncQueue);
		OSRestoreInterrupts(en);
	}
	Req* r = (port_cannot_block() || !sBlock) ? nullptr
	                                          : queueReq(fi, addr, length, offset, false, true, nullptr);
	if (r == nullptr) { // interrupt context or a host (non-game) thread, or a full queue: read now
		// w14: a big read (whole archives: 1-20 MB, 50-100 ms on a cold file cache) is done
		// without holding the emulated CPU, as a GameCube thread sleeps while the drive
		// works; otherwise the Nintendo logo and the audio thread stall behind it. The
		// file handle is opened first, so the entry table is only touched holding the CPU.
		int began = 0;
		if (length >= kBlockingReadMin && openHandle(fi))
			began = port_blocking_begin();
		s32 res = readLocked(fi, addr, length, offset); // W7-J: not beside the drive thread
		port_blocking_end(began);
		fi->cb.state = res < 0 ? DVD_STATE_FATAL_ERROR : DVD_STATE_END;
		// w14: a scheduling point, like the DVD interrupt that ends a GameCube read.
		port_sched_point();
		return res;
	}
	BOOL en = OSDisableInterrupts();
	while (!r->done)
		OSSleepThread(&gSyncQueue);
	s32 res = r->result;
	r->used = false;
	OSRestoreInterrupts(en);
	return res;
}

BOOL DVDReadAsyncPrio(DVDFileInfo* fi, void* addr, s32 length, s32 offset, DVDCallback callback,
                      s32)
{
	return queueReq(fi, addr, length, offset, false, false, callback) != nullptr;
}

s32 DVDGetCommandBlockStatus(DVDCommandBlock* block) { return block->state; }
s32 DVDGetFileInfoStatus(DVDFileInfo* fi) { return fi->cb.state; }
s32 DVDGetDriveStatus() { return gPending > 0 ? DVD_STATE_BUSY : DVD_STATE_END; }
BOOL DVDCheckDisk(void) { return TRUE; }
s32 DVDCancel(DVDCommandBlock* block)
{
	cancelReq(block);
	return 0;
}
BOOL DVDCancelAsync(DVDCommandBlock* block, DVDCBCallback cb)
{
	cancelReq(block);
	if (cb)
		cb(0, block);
	return TRUE;
}
DVDDiskID* DVDGetCurrentDiskID() { return &gDiskID; }
void DVDPause() { }
void DVDResume() { }
BOOL DVDSetAutoInvalidation(BOOL) { return TRUE; }

BOOL DVDChangeDir(char* dirName)
{
	s32 e = convert(dirName);
	if (e < 0 || !gFst[e].isDir)
		return FALSE;
	gCurrDir = e;
	return TRUE;
}

BOOL DVDOpenDir(char* dirName, DVDDir* dir)
{
	s32 e = convert(dirName);
	if (e < 0 || !gFst[e].isDir)
		return FALSE;
	dir->entryNum = (u32)e;
	dir->location = (u32)e + 1;
	dir->next     = (u32)gFst[e].next;
	return TRUE;
}

BOOL DVDReadDir(DVDDir* dir, DVDDirEntry* dirent)
{
	u32 loc = dir->location;
	if (loc <= dir->entryNum || loc >= dir->next)
		return FALSE;
	Entry& en        = gFst[loc];
	dirent->entryNum = loc;
	dirent->isDir    = en.isDir;
	dirent->name     = (char*)en.name.c_str();
	dir->location    = en.isDir ? (u32)en.next : loc + 1;
	return TRUE;
}

BOOL DVDCloseDir(DVDDir*) { return TRUE; }

// Streaming audio is not emulated: report success so callers proceed.
BOOL DVDPrepareStreamAsync(DVDFileInfo* fi, u32, u32, DVDCallback callback)
{
	return queueReq(fi, nullptr, 0, 0, true, false, callback) != nullptr;
}

} // extern "C"
