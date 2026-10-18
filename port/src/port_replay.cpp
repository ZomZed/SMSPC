// Input recording and replay for bug reports (w9).
//
// Recording (default on: [debug] record = 1 in port_settings.ini, SMS_PORT_RECORD=0/1
// overrides): from boot, everything the game logic reads from outside itself that can
// differ between two runs on a PC:
//   - the pad status of all four ports, as PADRead returns it to the game (JUTGamePad::read
//     applies PADClamp to it afterwards, so the clamped values follow from it),
//   - the srand() seed (Application.cpp seeds with OSGetTick() at boot),
//   - every OSIsThreadTerminated() answer on the main thread (the game polls its loader
//     threads with it once per frame, so how many frames a load screen lasts depends on
//     the PC's speed),
//   - every THPPlayerGetState() answer (the movie director's end-of-movie check; movies
//     advance with the VI retrace, so the number of game frames per movie varies),
// plus the memory card files as they were at boot, the disc identity and the port version.
// Each event carries the game frame (the count of PADRead calls) it happened at. Pad events
// are stored only when the pads change, so the stream is small (an idle hour costs almost
// nothing); it is capped at 64 MB, enough for many hours of play.
//
// F9 (or the settings menu) saves it as sms_replay_<date>_<time>.bin next to sms_port.exe,
// and a crash (unhandled exception, abort, Aurora FATAL) saves it automatically next to
// sms_port_crash.log. The file is zstd-compressed.
//
// Replay (SMS_PORT_REPLAY=<file>): the memory card files are restored into a fresh folder
// (the player's own card is never touched), physical controllers and the keyboard are
// ignored, and the game gets the recorded pads, seed, loader-thread and movie answers in
// the same order. A recorded "the loader finished" answer that comes earlier than the
// loader really finishes is still correct: every caller then joins the thread
// (OSJoinThread), which waits for it.
// Checkpoints: every 120 game frames the recording stores Mario's position, action and the
// rand() state; a replay compares them and reports the first divergence
// ("[port] replay: checkpoint ...").
//
// EFB readbacks (pollution maps, goop collision, pixel counters, port_efb.cpp) reach the
// game a fixed number of frames after they were requested while recording or replaying
// (port_replay_deterministic()); before, their GPU-timing-dependent arrival made a replay of
// Bianco Hills drift by a few units within a minute.
//
// rand()/srand(): the game's calls are routed here (build_aurora.py renames them in game
// objects) and implemented like the GameCube's MSL (one global LCG, x = x * 0x41C64E6D +
// 12345, result bits 16..30). The host CRT's rand is a different generator with
// per-thread state, so before this the random sequences differed from the GameCube's and
// between threads.
#ifdef SMS_PORT_AURORA

#include "port_replay.h"
#include "port_bugreport.h" // w12
#include "port_sdk.h" // port_print_backtrace
#include "port_settings.h"

#include <Camera/Camera.hpp>
#include <Enemy/Conductor.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>
#include <Player/Mario.hpp>
#include <Strategic/LiveActor.hpp>
#include <Strategic/LiveManager.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>

#include <SDL3/SDL_filesystem.h>
#include <zstd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" volatile unsigned gPortFrameCount; // port_crash.cpp: main-thread retraces
extern "C" BOOL OSIsThreadTerminated(OSThread* thread);
extern "C" s32 THPPlayerGetState(void);
const char* port_disc_identity(void); // port_disc.cpp
char port_build_region(void);         // port_disc.cpp (w13)
extern "C" int port_settings_pal60(void);        // port_behaviour.cpp (w13)
extern "C" int port_settings_pal_language(void); // port_behaviour.cpp (w13)

namespace {

// Debug settings that change what the game does; recorded and re-applied by a replay.
const char* const kEnvKeys[] = { "SMS_PORT_WARP", "SMS_PORT_THP_SPEED", "SMS_PORT_NODRAW" };
std::string sEnv;

const char kMagic[8]      = { 'S', 'M', 'S', 'R', 'P', 'L', 'Y', '1' };
const size_t kMaxEvents   = 64u << 20;
const uint32_t kCheckEach = 120;

enum EventType : uint8_t { E_PAD = 1, E_SEED = 2, E_TERM = 3, E_THP = 4, E_CHK = 5, E_CHK2 = 6, E_QOL = 7 };
const size_t kQolBytes = 16; // E_QOL: the game-affecting quality-of-life state (port_qol.cpp)
const size_t kChk2Bytes = 28; // E_CHK2: Mario, rand, actor hash, camera hash
const size_t kPadBytes = 4 * 12;

enum Mode { M_OFF, M_RECORD, M_REPLAY };
Mode sMode;
DWORD sMainThread;
uint32_t sIter; // PADRead calls so far (= game frames)
CRITICAL_SECTION sLock;
char sStatus[160];

// ---- recording ----------------------------------------------------------------------
std::vector<uint8_t> sEvents;
std::vector<uint8_t> sCard; // card snapshot chunk
std::string sInfo;
bool sFull;
uint8_t sLastPads[kPadBytes];
bool sHavePads;

void put32(std::vector<uint8_t>& v, uint32_t x)
{
	for (int i = 0; i < 4; i++)
		v.push_back((uint8_t)(x >> (8 * i)));
}
uint32_t get32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void event(uint8_t type, const void* payload, size_t n)
{
	if (sMode != M_RECORD || sFull)
		return;
	EnterCriticalSection(&sLock);
	if (sEvents.size() + n + 5 > kMaxEvents) {
		sFull = true;
		std::fprintf(stderr, "[port] replay: recording reached %zu MB; stopped (saved replays end here)\n",
		             kMaxEvents >> 20);
	} else {
		sEvents.push_back(type);
		put32(sEvents, sIter);
		const uint8_t* p = (const uint8_t*)payload;
		sEvents.insert(sEvents.end(), p, p + n);
	}
	LeaveCriticalSection(&sLock);
}

void packPads(const PADStatus* s, uint8_t* out)
{
	for (int i = 0; i < 4; i++, out += 12) {
		out[0]  = (uint8_t)s[i].button;
		out[1]  = (uint8_t)(s[i].button >> 8);
		out[2]  = (uint8_t)s[i].stickX;
		out[3]  = (uint8_t)s[i].stickY;
		out[4]  = (uint8_t)s[i].substickX;
		out[5]  = (uint8_t)s[i].substickY;
		out[6]  = s[i].triggerLeft;
		out[7]  = s[i].triggerRight;
		out[8]  = s[i].analogA;
		out[9]  = s[i].analogB;
		out[10] = (uint8_t)s[i].err;
		out[11] = 0;
	}
}

void unpackPads(const uint8_t* in, PADStatus* s)
{
	for (int i = 0; i < 4; i++, in += 12) {
		s[i]              = PADStatus{};
		s[i].button       = (u16)(in[0] | in[1] << 8);
		s[i].stickX       = (s8)in[2];
		s[i].stickY       = (s8)in[3];
		s[i].substickX    = (s8)in[4];
		s[i].substickY    = (s8)in[5];
		s[i].triggerLeft  = in[6];
		s[i].triggerRight = in[7];
		s[i].analogA      = in[8];
		s[i].analogB      = in[9];
		s[i].err          = (s8)in[10];
	}
}

// ---- files ------------------------------------------------------------------------------
std::string exeDir()
{
	char buf[MAX_PATH];
	DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	std::string p(buf, n);
	size_t slash = p.find_last_of("\\/");
	return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::string userDir()
{
	if (const char* u = std::getenv("SMS_PORT_USER_DIR"))
		if (*u != '\0')
			return u;
	char* p = SDL_GetPrefPath(nullptr, "Super Mario Sunshine");
	std::string s = p != nullptr ? p : "";
	SDL_free(p);
	while (!s.empty() && (s.back() == '\\' || s.back() == '/'))
		s.pop_back();
	return s;
}

std::string cardDir(const std::string& user) { return user + "\\USA\\Card A"; }

bool readFile(const std::string& path, std::vector<uint8_t>* out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	std::fseek(f, 0, SEEK_END);
	long n = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	out->resize(n > 0 ? (size_t)n : 0);
	bool ok = n <= 0 || std::fread(out->data(), 1, (size_t)n, f) == (size_t)n;
	std::fclose(f);
	return ok;
}

// Card snapshot chunk: u32 count, then per file u32 name length, name, u32 size, bytes.
void snapshotCard(const std::string& dir)
{
	sCard.clear();
	std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
	WIN32_FIND_DATAA fd;
	HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				continue;
			std::vector<uint8_t> data;
			if (fd.nFileSizeHigh == 0 && fd.nFileSizeLow < (16u << 20) && readFile(dir + "\\" + fd.cFileName, &data))
				files.push_back({ fd.cFileName, std::move(data) });
		} while (FindNextFileA(h, &fd));
		FindClose(h);
	}
	put32(sCard, (uint32_t)files.size());
	for (auto& f : files) {
		put32(sCard, (uint32_t)f.first.size());
		sCard.insert(sCard.end(), f.first.begin(), f.first.end());
		put32(sCard, (uint32_t)f.second.size());
		sCard.insert(sCard.end(), f.second.begin(), f.second.end());
	}
	std::fprintf(stderr, "[port] replay: recording from boot (card snapshot: %zu file(s) from %s)\n", files.size(),
	             dir.c_str());
}

// ---- replay ---------------------------------------------------------------------------
struct Ev {
	uint8_t type;
	uint32_t iter;
	const uint8_t* data;
};
std::vector<uint8_t> sBlob; // decompressed replay file
std::vector<Ev> sPads, sSeeds, sTerms, sThps, sChks, sQols;
size_t sPadPos, sSeedPos, sTermPos, sThpPos, sChkPos, sQolPos;
uint8_t sLastQol[kQolBytes];
bool sHaveQol;
uint32_t sLastIter;
bool sEnded;
int sMismatches;
int sCompared;
bool sWarned[8];

void divergence(int kind, const char* what, uint32_t recIter)
{
	if (!sWarned[kind]) {
		sWarned[kind] = true;
		std::fprintf(stderr, "[port] replay: %s at game frame %u was recorded at frame %u (diverging)\n", what, sIter,
		             recIter);
	}
}

// The replay's private user folder (card copy), removed at exit.
std::string sReplayUser;
void removeReplayUser()
{
	const std::string dirs[] = { cardDir(sReplayUser), sReplayUser + "\\USA", sReplayUser };
	WIN32_FIND_DATAA fd;
	for (const std::string& d : dirs) {
		HANDLE h = FindFirstFileA((d + "\\*").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do
				if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
					DeleteFileA((d + "\\" + fd.cFileName).c_str());
			while (FindNextFileA(h, &fd));
			FindClose(h);
		}
		RemoveDirectoryA(d.c_str());
	}
}

bool loadReplay(const char* path, AuroraConfig* config)
{
	std::vector<uint8_t> file;
	if (!readFile(path, &file) || file.size() < 16 || std::memcmp(file.data(), kMagic, 8) != 0) {
		std::fprintf(stderr, "[port] replay: %s is not a replay file\n", path);
		return false;
	}
	const unsigned long long raw = ZSTD_getFrameContentSize(file.data() + 8, file.size() - 8);
	if (raw == ZSTD_CONTENTSIZE_ERROR || raw == ZSTD_CONTENTSIZE_UNKNOWN || raw > (1ull << 31)) {
		std::fprintf(stderr, "[port] replay: %s is damaged\n", path);
		return false;
	}
	sBlob.resize((size_t)raw);
	if (ZSTD_isError(ZSTD_decompress(sBlob.data(), sBlob.size(), file.data() + 8, file.size() - 8)))
		return false;
	// Chunks: u32 tag, u32 size, data.
	const uint8_t* p   = sBlob.data();
	const uint8_t* end = p + sBlob.size();
	const uint8_t *card = nullptr, *ev = nullptr;
	size_t cardLen = 0, evLen = 0;
	// w13: replays recorded before the behaviour setting ran with the disc's own behaviour, not
	// with this player's setting (the ENV chunk below overrides it for newer files)
	_putenv_s("SMS_PORT_BEHAVIOUR", "disc");
	while (end - p >= 8) {
		const uint32_t tag = get32(p), n = get32(p + 4);
		p += 8;
		if ((size_t)(end - p) < n)
			break;
		if (tag == 0x4F464E49) // "INFO"
			std::fprintf(stderr, "[port] replay: %.*s\n", (int)n, (const char*)p);
		else if (tag == 0x20564E45) { // "ENV ": key=value lines
			std::string env((const char*)p, n);
			size_t s = 0;
			while (s < env.size()) {
				size_t e = env.find('\n', s);
				if (e == std::string::npos)
					e = env.size();
				const std::string kv = env.substr(s, e - s);
				const size_t eq      = kv.find('=');
				if (eq != std::string::npos) {
					_putenv_s(kv.substr(0, eq).c_str(), kv.substr(eq + 1).c_str());
					std::fprintf(stderr, "[port] replay: %s\n", kv.c_str());
				}
				s = e + 1;
			}
		} else if (tag == 0x44524143) { // "CARD"
			card    = p;
			cardLen = n;
		} else if (tag == 0x544E5645) { // "EVNT"
			ev    = p;
			evLen = n;
		}
		p += n;
	}
	if (ev == nullptr)
		return false;
	for (size_t i = 0; i + 5 <= evLen;) {
		Ev e = { ev[i], get32(ev + i + 1), ev + i + 5 };
		size_t n = 0;
		switch (e.type) {
		case E_PAD: n = kPadBytes; sPads.push_back(e); break;
		case E_SEED: n = 4; sSeeds.push_back(e); break;
		case E_TERM: n = 1; sTerms.push_back(e); break;
		case E_THP: n = 4; sThps.push_back(e); break;
		case E_CHK: n = 20; sChks.push_back(e); break;
		case E_CHK2: n = kChk2Bytes; sChks.push_back(e); break;
		case E_QOL: n = kQolBytes; sQols.push_back(e); break;
		default: i = evLen; continue;
		}
		if (e.iter > sLastIter)
			sLastIter = e.iter;
		i += 5 + n;
	}
	// The recorded memory card goes into a fresh user folder.
	char tmp[MAX_PATH];
	GetTempPathA(MAX_PATH, tmp);
	std::string user = std::string(tmp) + "sms_replay_" + std::to_string(GetCurrentProcessId());
	const std::string dir = cardDir(user);
	CreateDirectoryA(user.c_str(), nullptr);
	CreateDirectoryA((user + "\\USA").c_str(), nullptr);
	CreateDirectoryA(dir.c_str(), nullptr);
	int files = 0;
	if (card != nullptr && cardLen >= 4) {
		uint32_t count = get32(card);
		size_t o       = 4;
		for (uint32_t i = 0; i < count && o + 4 <= cardLen; i++) {
			const uint32_t nl = get32(card + o);
			o += 4;
			if (o + nl + 4 > cardLen)
				break;
			std::string name((const char*)card + o, nl);
			o += nl;
			const uint32_t sz = get32(card + o);
			o += 4;
			if (o + sz > cardLen || name.find_first_of("\\/:") != std::string::npos)
				break;
			if (FILE* f = std::fopen((dir + "\\" + name).c_str(), "wb")) {
				std::fwrite(card + o, 1, sz, f);
				std::fclose(f);
				++files;
			}
			o += sz;
		}
	}
	_putenv_s("SMS_PORT_USER_DIR", user.c_str()); // port_main.cpp hands it to Aurora
	sReplayUser = user;
	std::atexit(removeReplayUser);
	(void)config;
	_putenv_s("SMS_PORT_NO_PADS", "1");
	std::fprintf(stderr,
	             "[port] replay: %s: %zu pad changes, %zu loader answers, %zu movie answers, %zu checkpoints, "
	             "%u frames; card (%d file(s)) restored to %s\n",
	             path, sPads.size(), sTerms.size(), sThps.size(), sChks.size(), sLastIter, files, dir.c_str());
	return true;
}

// ---- checkpoints --------------------------------------------------------------------
uint32_t sRandState = 1;

bool marioState(uint8_t* out20)
{
	TApplication* app = &gpApplication;
	TMario* m         = gpMarioOriginal;
	if (gpMarDirector == nullptr || app->mDirector != (JDrama::TDirector*)gpMarDirector || m == nullptr)
		return false;
	const float pos[3] = { m->mPosition.x, m->mPosition.y, m->mPosition.z };
	std::memcpy(out20, pos, 12);
	const uint32_t st = m->mStatus;
	std::memcpy(out20 + 12, &st, 4);
	std::memcpy(out20 + 16, &sRandState, 4);
	return true;
}

// Hash of the state of every live actor (position, live flags) and of the camera
// (position, target, view and projection matrices): checkpoints compare these too.
uint32_t fnv(uint32_t h, const void* p, size_t n)
{
	const uint8_t* b = (const uint8_t*)p;
	for (size_t i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

uint32_t worldHash()
{
	uint32_t h = 2166136261u;
	if (gpConductor == nullptr)
		return h;
	// gpConductor is never cleared: between a stage's director and its own conductor (a stage
	// load, a stage without one) it still points at the previous stage's freed one. Use it only
	// if this scene's name tree has it (as port_mp_objects.cpp / port_mp_npcs.cpp do).
	{
		JDrama::TNameRefGen* gen = JDrama::TNameRefGen::getInstance();
		JDrama::TNameRef* found  = gen != nullptr && gen->getRootNameRef() != nullptr
		                               ? gen->getRootNameRef()->search("\x83\x52\x83\x93\x83\x5F\x83\x4E\x83\x5E\x81\x5B")
		                               : nullptr;
		if (found != (JDrama::TNameRef*)gpConductor)
			return h;
	}
	for (JGadget::TList<TLiveManager*>::iterator it = gpConductor->unk10.begin(); it != gpConductor->unk10.end(); ++it) {
		TLiveManager* mgr = *it;
		const int n       = mgr->getObjNum();
		h                 = fnv(h, &n, 4);
		for (int i = 0; i < n; i++) {
			const TLiveActor* a = mgr->getObj(i);
			h                   = fnv(h, &a->mPosition, 12);
			h                   = fnv(h, &a->mLiveFlag, 4);
		}
	}
	return h;
}

uint32_t cameraHash()
{
	uint32_t h = 2166136261u;
	if (gpCamera == nullptr)
		return h;
	h = fnv(h, &gpCamera->getUnk124(), 12);
	h = fnv(h, gpCamera->getUnk1EC(), sizeof(Mtx));
	h = fnv(h, gpCamera->getUnk16C(), sizeof(Mtx44));
	return h;
}

void checkpoint()
{
	if (sIter % kCheckEach != 0)
		return;
	uint8_t cur[kChk2Bytes];
	if (!marioState(cur))
		return;
	const uint32_t wh = worldHash(), ch = cameraHash();
	std::memcpy(cur + 20, &wh, 4);
	std::memcpy(cur + 24, &ch, 4);
	if (sMode == M_RECORD) {
		event(E_CHK2, cur, sizeof(cur));
		return;
	}
	while (sChkPos < sChks.size() && sChks[sChkPos].iter < sIter)
		++sChkPos;
	if (sChkPos >= sChks.size() || sChks[sChkPos].iter != sIter)
		return;
	const Ev& e        = sChks[sChkPos++];
	const uint8_t* rec = e.data;
	const size_t n     = e.type == E_CHK2 ? kChk2Bytes : 20; // older files: Mario and rand only
	float a[3], b[3];
	std::memcpy(a, rec, 12);
	std::memcpy(b, cur, 12);
	const bool same = std::memcmp(rec, cur, n) == 0;
	++sCompared;
	if (!same && ++sMismatches <= 5)
		std::fprintf(stderr,
		             "[port] replay: checkpoint frame %u DIFFERS: recorded mario %.1f %.1f %.1f act %08x rand %08x "
		             "actors %08x camera %08x, now %.1f %.1f %.1f act %08x rand %08x actors %08x camera %08x\n",
		             sIter, a[0], a[1], a[2], get32(rec + 12), get32(rec + 16), n > 20 ? get32(rec + 20) : 0,
		             n > 20 ? get32(rec + 24) : 0, b[0], b[1], b[2], get32(cur + 12), get32(cur + 16), wh, ch);
	else if (same && (sChkPos % 10) == 0)
		std::fprintf(stderr, "[port] replay: checkpoint frame %u matches (mario %.1f %.1f %.1f, actors %08x)\n", sIter,
		             b[0], b[1], b[2], wh);
}
// ---- saving -------------------------------------------------------------------------------
void addChunk(std::vector<uint8_t>& out, const char* tag, const void* data, size_t n)
{
	out.insert(out.end(), tag, tag + 4);
	put32(out, (uint32_t)n);
	const uint8_t* p = (const uint8_t*)data;
	out.insert(out.end(), p, p + n);
}

LPTOP_LEVEL_EXCEPTION_FILTER sPrevFilter;
LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep)
{
	static volatile LONG once;
	if (!InterlockedExchange(&once, 1)) {
		std::fprintf(stderr, "[port] replay: exception 0x%08lx at %p\n", ep->ExceptionRecord->ExceptionCode,
		             ep->ExceptionRecord->ExceptionAddress);
		__try {
			if (!port_bugreport_enabled()) // w12: else port_crash.cpp's filter saves the bug report (with the replay)
				port_replay_save("crash");
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}
	return sPrevFilter != nullptr ? sPrevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

void onAbort(int)
{
	static volatile LONG once;
	if (!InterlockedExchange(&once, 1)) {
		std::fprintf(stderr, "[port] abort() called (thread %lu)\n", GetCurrentThreadId());
		port_print_backtrace();
		if (port_bugreport_enabled()) // w12
			port_bugreport_crash("abort() called");
		else
			port_replay_save("abort");
	}
}

} // namespace

// ---- game hooks (renamed in game objects by build_aurora.py) --------------------------
static volatile bool sRandScratchOn; // port_rand_scratch
static uint32_t sRandScratch;

extern "C" int port_rand(void)
{
	EnterCriticalSection(&sLock);
	uint32_t& state = sRandScratchOn && GetCurrentThreadId() == sMainThread ? sRandScratch : sRandState;
	state       = state * 0x41C64E6Du + 12345u;
	const int r = (int)((state >> 16) & 0x7FFF);
	LeaveCriticalSection(&sLock);
	return r;
}

// port_interp.cpp: while an extra display frame is drawn, rand() on the main thread draws
// from a copy of the state, so a draw path that calls rand() cannot change the game's
// sequence (other threads keep using the real one).
extern "C" void port_rand_scratch(bool on)
{
	EnterCriticalSection(&sLock);
	sRandScratch   = sRandState;
	sRandScratchOn = on;
	LeaveCriticalSection(&sLock);
}

extern "C" void port_srand(unsigned int seed)
{
	if (sMode == M_REPLAY && sSeedPos < sSeeds.size())
		seed = get32(sSeeds[sSeedPos++].data);
	else if (sMode == M_RECORD)
		event(E_SEED, &seed, 4);
	EnterCriticalSection(&sLock);
	sRandState = seed;
	LeaveCriticalSection(&sLock);
}

extern "C" BOOL port_OSIsThreadTerminated(OSThread* thread)
{
	BOOL real = OSIsThreadTerminated(thread);
	if (GetCurrentThreadId() != sMainThread || sMode == M_OFF)
		return real;
	if (sMode == M_RECORD) {
		uint8_t b = real ? 1 : 0;
		event(E_TERM, &b, 1);
		return real;
	}
	if (sTermPos >= sTerms.size())
		return real;
	const Ev& e = sTerms[sTermPos++];
	if (e.iter != sIter)
		divergence(E_TERM, "a loader-thread check", e.iter);
	return e.data[0] ? TRUE : FALSE; // TRUE early: the caller's OSJoinThread waits for the loader
}

extern "C" s32 port_THPPlayerGetState(void)
{
	s32 real = THPPlayerGetState();
	if (GetCurrentThreadId() != sMainThread || sMode == M_OFF)
		return real;
	if (sMode == M_RECORD) {
		event(E_THP, &real, 4);
		return real;
	}
	if (sThpPos >= sThps.size())
		return real;
	const Ev& e = sThps[sThpPos++];
	if (e.iter != sIter)
		divergence(E_THP, "a movie-state check", e.iter);
	s32 v;
	std::memcpy(&v, e.data, 4);
	return v;
}

extern "C" {

void port_replay_init(AuroraConfig* config)
{
	InitializeCriticalSection(&sLock);
	sMainThread = GetCurrentThreadId();
	if (const char* rp = std::getenv("SMS_PORT_REPLAY"))
		if (*rp != '\0') {
			if (loadReplay(rp, config)) {
				sMode = M_REPLAY;
				std::snprintf(sStatus, sizeof(sStatus), "Replaying %s", rp);
			} else {
				std::snprintf(sStatus, sizeof(sStatus), "Replay file could not be read");
			}
			return;
		}
	bool record = gPortSettings.record;
	if (const char* r = std::getenv("SMS_PORT_RECORD"))
		record = *r != '\0' && *r != '0';
	if (!record) {
		std::snprintf(sStatus, sizeof(sStatus), "Recording off");
		return;
	}
	sMode = M_RECORD;
	snapshotCard(cardDir(userDir()));
	char date[32];
	time_t now = time(nullptr);
	strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&now));
	sInfo = std::string("recorded ") + date + ", port build " __DATE__ " " __TIME__;
	sEvents.reserve(1 << 20);
	for (const char* k : kEnvKeys)
		if (const char* v = std::getenv(k))
			if (*v != '\0')
				sEnv += std::string(k) + "=" + v + "\n";
	sPrevFilter = SetUnhandledExceptionFilter(crashFilter);
	std::signal(SIGABRT, onAbort);
	std::snprintf(sStatus, sizeof(sStatus), "Recording since boot (F9 saves a replay)");
}

void port_replay_pads(PADStatus* status)
{
	if (sMode == M_OFF)
		return;
	if (sMode == M_RECORD) {
		uint8_t packed[kPadBytes];
		packPads(status, packed);
		if (!sHavePads || std::memcmp(packed, sLastPads, kPadBytes) != 0) {
			event(E_PAD, packed, kPadBytes);
			std::memcpy(sLastPads, packed, kPadBytes);
			sHavePads = true;
		}
	} else {
		while (sPadPos + 1 < sPads.size() && sPads[sPadPos + 1].iter <= sIter)
			++sPadPos;
		if (!sPads.empty() && sPads[sPadPos].iter <= sIter)
			unpackPads(sPads[sPadPos].data, status);
		if (!sEnded && sIter >= sLastIter) {
			sEnded = true;
			std::fprintf(stderr, "[port] replay: end of the recording at game frame %u (retrace %u); %d of %d "
			                     "checkpoints differ\n",
			             sIter, gPortFrameCount, sMismatches, sCompared);
			if (const char* ex = std::getenv("SMS_PORT_REPLAY_EXIT"))
				if (*ex == '1') {
					std::fflush(stderr);
					std::fflush(stdout);
					// End the process at once: std::exit ran static destructors while other
					// threads still worked (Aurora's joinable pipeline-cache writer thread ->
					// abort; after aurora_shutdown the AI interrupt thread used the destroyed
					// audio stream). A replay saves nothing, so only the card copy is removed.
					removeReplayUser();
					TerminateProcess(GetCurrentProcess(), sMismatches ? 1u : 0u);
				}
		}
	}
	checkpoint();
	++sIter;
}

bool port_replay_active(void) { return sMode == M_REPLAY; }

// Quality-of-life state (port_qol.cpp), once per game frame before port_replay_pads: a recording
// stores every change, a replay replaces the live state with the recorded one (recordings made
// before E_QOL existed replay with every option off).
static void qolDefault(uint8_t* st)
{
	std::memset(st, 0, kQolBytes);
	st[2] = 50; // camera speed 100 %
	st[3] = 1;  // no fast-forward
}

void port_replay_qol(uint8_t* st)
{
	if (sMode == M_RECORD) {
		if (!sHaveQol || std::memcmp(st, sLastQol, kQolBytes) != 0) {
			event(E_QOL, st, kQolBytes);
			std::memcpy(sLastQol, st, kQolBytes);
			sHaveQol = true;
		}
	} else if (sMode == M_REPLAY) {
		while (sQolPos < sQols.size() && sQols[sQolPos].iter <= sIter) {
			std::memcpy(sLastQol, sQols[sQolPos].data, kQolBytes);
			sHaveQol = true;
			++sQolPos;
		}
		if (sHaveQol)
			std::memcpy(st, sLastQol, kQolBytes);
		else
			qolDefault(st);
	}
}

// Before the first game frame (boot): the recorded start-up state.
void port_replay_qol_initial(uint8_t* st)
{
	if (sMode != M_REPLAY)
		return;
	if (!sQols.empty() && sQols[0].iter == 0)
		std::memcpy(st, sQols[0].data, kQolBytes);
	else
		qolDefault(st);
}
bool port_replay_deterministic(void) { return sMode != M_OFF; }
bool port_replay_recording(void) { return sMode == M_RECORD; }
const char* port_replay_status(void) { return sStatus; }

bool port_replay_save(const char* why) { return port_replay_save_dir(why, nullptr); }

// w12: dir = the bug report's folder (null: next to the exe).
bool port_replay_save_dir(const char* why, const char* dir)
{
	if (sMode != M_RECORD)
		return false;
	std::vector<uint8_t> events;
	EnterCriticalSection(&sLock);
	events = sEvents;
	const uint32_t frames = sIter;
	LeaveCriticalSection(&sLock);
	const char* disc = port_disc_identity();
	std::string info = sInfo + "\nsaved because: " + why + "\ngame frames: " + std::to_string(frames)
	                   + "\ndisc: " + (disc != nullptr ? disc : "?");
	std::vector<uint8_t> blob;
	addChunk(blob, "INFO", info.data(), info.size());
	std::string env = sEnv;
	{ // w13: the game behaviour and (European build) the PAL SRAM settings this run used (read once)
		static const char* const kBehaviour[] = { "jp", "eu", "us" }; // SMS_REGION_*
		const int b = SMSGetBehaviour();
		env += std::string("SMS_PORT_BEHAVIOUR=") + kBehaviour[b < 0 || b > 2 ? 2 : b] + "\n";
		if (port_build_region() == 'P')
			env += "SMS_PORT_PAL60=" + std::to_string(port_settings_pal60()) + "\nSMS_PORT_PAL_LANGUAGE="
			       + std::to_string(port_settings_pal_language()) + "\n";
	}
	addChunk(blob, "ENV ", env.data(), env.size());
	addChunk(blob, "CARD", sCard.data(), sCard.size());
	addChunk(blob, "EVNT", events.data(), events.size());
	std::vector<uint8_t> out(8 + ZSTD_compressBound(blob.size()));
	std::memcpy(out.data(), kMagic, 8);
	const size_t n = ZSTD_compress(out.data() + 8, out.size() - 8, blob.data(), blob.size(), 9);
	if (ZSTD_isError(n))
		return false;
	out.resize(8 + n);
	char name[64];
	time_t now = time(nullptr);
	strftime(name, sizeof(name), "sms_replay_%Y%m%d_%H%M%S.bin", localtime(&now));
	const std::string path = (dir != nullptr ? std::string(dir) : exeDir()) + "\\" + name;
	FILE* f = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return false;
	const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
	std::fclose(f);
	std::fprintf(stderr, "[port] replay: %s %s (%u game frames, %zu KB)\n", ok ? "saved" : "FAILED to save",
	             path.c_str(), frames, out.size() >> 10);
	if (ok)
		std::snprintf(sStatus, sizeof(sStatus), "Saved %s", name);
	return ok;
}

} // extern "C"

#endif // SMS_PORT_AURORA
