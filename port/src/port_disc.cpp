// Game data location and disc-image reader for the Aurora build (w9). See port_disc.h.
//
// Disc images: plain GameCube images (.iso/.gcm, 1:1 copies of the 1.4 GB disc) and
// CISO (.ciso, a block map that leaves out unused 2 MB-class blocks). The disc header
// (boot.bin, offset 0) has the game ID at 0x000, the GameCube magic 0xC2339F3D at 0x01C,
// and the FST offset/size at 0x424/0x428. FST entries are 12 bytes, big-endian:
// flags (0 file, 1 dir) and a 24-bit name offset into the string table that follows the
// entries; then the file offset (dirs: parent index) and the file length (dirs: index
// after the last descendant; entry 0, the root: the entry count).
// Dolphin's compressed formats RVZ, WIA and GCZ are read by port_disc_wia.cpp.
#ifdef SMS_PORT_AURORA

#include "port_disc.h"
#include "port_settings.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "shell32.lib") // CommandLineToArgvW

namespace {

// w13 (BOOTLOG "Regional behaviour (w13)"): this exe's region; discs of the other two
// Super Mario Sunshine regions are accepted and handed to the sibling exe (handOff below).
#if defined(VERSION_GMSE01)
const char kGameId[] = "GMSE01";
#elif defined(VERSION_GMSJ01)
const char kGameId[] = "GMSJ01";
#else
const char kGameId[] = "GMSP01";
#endif
bool isSmsId(const std::string& id)
{
	return id.size() == 6 && id.compare(0, 3, "GMS") == 0 && (id[3] == 'E' || id[3] == 'J' || id[3] == 'P')
	       && id.compare(4, 2, "01") == 0;
}
const uint32_t kGcMagic = 0xC2339F3Du;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

// ---- image reader -----------------------------------------------------------------
struct Image {
	HANDLE h = INVALID_HANDLE_VALUE;
	uint64_t size = 0; // logical disc size
	// CISO
	bool ciso = false;
	uint32_t blockSize = 0;
	std::vector<int32_t> blockIndex; // logical block -> stored block, or -1 (zeros)
	// RVZ / WIA / GCZ
	std::unique_ptr<PortDiscReader> comp;
};
Image gImage;
std::string gIdentity; // for replay files (port_disc_identity)

bool rawRead(HANDLE h, uint64_t off, void* dst, uint32_t len, uint32_t* got)
{
	OVERLAPPED ov = {};
	ov.Offset     = (DWORD)off;
	ov.OffsetHigh = (DWORD)(off >> 32);
	DWORD n       = 0;
	BOOL ok       = ReadFile(h, dst, len, &n, &ov);
	if (!ok && GetLastError() != ERROR_HANDLE_EOF)
		return false;
	*got = n;
	return true;
}

int32_t imageRead(const Image& im, uint64_t off, void* dst, int32_t length)
{
	if (length <= 0)
		return 0;
	if (off >= im.size)
		return 0;
	if (off + (uint64_t)length > im.size)
		length = (int32_t)(im.size - off);
	if (im.comp)
		return im.comp->read(off, dst, length);
	uint8_t* out = (uint8_t*)dst;
	if (!im.ciso) {
		uint32_t got;
		return rawRead(im.h, off, out, (uint32_t)length, &got) ? (int32_t)got : -1;
	}
	int32_t done = 0;
	while (done < length) {
		const uint64_t pos   = off + done;
		const uint64_t blk   = pos / im.blockSize;
		const uint32_t inBlk = (uint32_t)(pos % im.blockSize);
		const uint32_t n     = (uint32_t)std::min<uint64_t>(im.blockSize - inBlk, (uint64_t)(length - done));
		const int32_t stored = blk < im.blockIndex.size() ? im.blockIndex[blk] : -1;
		if (stored < 0) {
			std::memset(out + done, 0, n);
		} else {
			uint32_t got;
			const uint64_t src = 0x8000ull + (uint64_t)stored * im.blockSize + inBlk;
			if (!rawRead(im.h, src, out + done, n, &got) || got != n)
				return -1;
		}
		done += (int32_t)n;
	}
	return done;
}

enum class Probe { Ok, NotFound, NotGameCube, WrongGame, Unsupported, Damaged };

std::string lower(std::string s)
{
	for (char& c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

bool endsWith(const std::string& s, const char* ext)
{
	const size_t n = std::strlen(ext);
	return s.size() >= n && lower(s.substr(s.size() - n)) == ext;
}

bool isUnsupportedImage(const std::string& p)
{
	return endsWith(p, ".wbfs"); // Wii only
}

// Opens an image and checks its header. `id` receives the 6-character game ID; *why says
// why a compressed image cannot be read.
Probe openImage(const std::string& path, Image* im, std::string* id, std::string* why)
{
	if (isUnsupportedImage(path))
		return Probe::Unsupported;
	if (PortDiscReader* r = port_disc_open_compressed(path, why)) {
		Image img;
		img.comp.reset(r);
		img.size = r->size();
		uint8_t head[0x20];
		if (imageRead(img, 0, head, sizeof(head)) != (int32_t)sizeof(head))
			return Probe::Damaged;
		if (be32(head + 0x1C) != kGcMagic)
			return Probe::NotGameCube;
		id->assign((const char*)head, 6);
		if (!isSmsId(*id)) // w13: any SMS region
			return Probe::WrongGame;
		*im = std::move(img);
		return Probe::Ok;
	}
	if (!why->empty())
		return Probe::Unsupported;
	std::wstring w(path.size() + 1, L'\0');
	w.resize(MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], (int)w.size()));
	if (!w.empty() && w.back() == L'\0')
		w.pop_back();
	HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
	                       FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return Probe::NotFound;
	Image img;
	img.h = h;
	LARGE_INTEGER sz;
	GetFileSizeEx(h, &sz);
	img.size = (uint64_t)sz.QuadPart;
	uint8_t head[0x20];
	uint32_t got = 0;
	if (!rawRead(h, 0, head, sizeof(head), &got) || got != sizeof(head)) {
		CloseHandle(h);
		return Probe::NotGameCube;
	}
	if (std::memcmp(head, "CISO", 4) == 0) {
		std::vector<uint8_t> map(0x8000 - 8);
		img.ciso      = true;
		img.blockSize = head[4] | head[5] << 8 | head[6] << 16 | (uint32_t)head[7] << 24;
		if (img.blockSize == 0 || !rawRead(h, 8, map.data(), (uint32_t)map.size(), &got) || got != map.size()) {
			CloseHandle(h);
			return Probe::Damaged;
		}
		int32_t stored = 0;
		size_t last    = 0;
		for (size_t i = 0; i < map.size(); i++) {
			img.blockIndex.push_back(map[i] ? stored++ : -1);
			if (map[i])
				last = i + 1;
		}
		img.blockIndex.resize(last);
		img.size = (uint64_t)last * img.blockSize;
		if (imageRead(img, 0, head, sizeof(head)) != (int32_t)sizeof(head)) {
			CloseHandle(h);
			return Probe::Damaged;
		}
	}
	if (be32(head + 0x1C) != kGcMagic) {
		CloseHandle(h);
		return Probe::NotGameCube;
	}
	id->assign((const char*)head, 6);
	if (!isSmsId(*id)) { // w13: any SMS region
		CloseHandle(h);
		return Probe::WrongGame;
	}
	*im = std::move(img);
	return Probe::Ok;
}

bool loadFst(const Image& im, std::vector<PortDiscEntry>* out)
{
	uint8_t hdr[8];
	if (imageRead(im, 0x424, hdr, 8) != 8)
		return false;
	const uint32_t fstOff = be32(hdr), fstSize = be32(hdr + 4);
	if (fstSize < 12 || fstSize > 0x1000000 || (uint64_t)fstOff + fstSize > im.size)
		return false;
	std::vector<uint8_t> fst(fstSize);
	if (imageRead(im, fstOff, fst.data(), (int32_t)fstSize) != (int32_t)fstSize)
		return false;
	const uint32_t count = be32(&fst[8]);
	if (count == 0 || (uint64_t)count * 12 > fstSize)
		return false;
	const char* strings    = (const char*)&fst[count * 12];
	const size_t stringLen = fstSize - count * 12;
	out->clear();
	out->reserve(count);
	// Parents of files are implied by the enclosing directory range.
	std::vector<int32_t> dirStack;
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t* e = &fst[i * 12];
		PortDiscEntry d;
		d.isDir             = e[0] != 0;
		const uint32_t name = (uint32_t)e[1] << 16 | (uint32_t)e[2] << 8 | e[3];
		if (i != 0) {
			if (name >= stringLen)
				return false;
			d.name.assign(strings + name, strnlen(strings + name, stringLen - name));
		}
		const uint32_t a = be32(e + 4), b = be32(e + 8);
		while (!dirStack.empty() && (uint32_t)(*out)[dirStack.back()].next <= i)
			dirStack.pop_back();
		if (d.isDir) {
			d.parent = (int32_t)a;
			d.next   = (int32_t)b;
			d.offset = 0;
			d.size   = 0;
			if (b <= i || b > count)
				return false;
		} else {
			d.parent = dirStack.empty() ? 0 : dirStack.back();
			d.next   = (int32_t)i + 1;
			d.offset = a;
			d.size   = b;
			if ((uint64_t)a + b > im.size)
				return false;
		}
		out->push_back(d);
		if (d.isDir)
			dirStack.push_back((int32_t)i);
	}
	(*out)[0].parent = 0;
	return true;
}

// ---- locating ---------------------------------------------------------------------
std::string exeDir()
{
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
	char u8[MAX_PATH * 3];
	int m = WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, u8, sizeof(u8), nullptr, nullptr);
	std::string p(u8, m > 0 ? m : 0);
	size_t slash = p.find_last_of("\\/");
	return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::wstring wide(const std::string& s)
{
	std::wstring w(s.size() + 1, L'\0');
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], (int)w.size());
	w.resize(n > 0 ? n : 0);
	return w;
}

bool isDirectory(const std::string& p)
{
	DWORD a = GetFileAttributesW(wide(p).c_str());
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool isFile(const std::string& p)
{
	DWORD a = GetFileAttributesW(wide(p).c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// A folder is game data if it is the disc's files/ folder (has data/ and AudioRes/), or a
// folder that contains one (Dolphin's "Extract Entire Disc" output: files/ and sys/).
bool resolveFolder(const std::string& p, std::string* root)
{
	if (isDirectory(p + "\\data") && isDirectory(p + "\\AudioRes")) {
		*root = p;
		return true;
	}
	if (isDirectory(p + "\\files\\data") && isDirectory(p + "\\files\\AudioRes")) {
		*root = p + "\\files";
		return true;
	}
	return false;
}

// Extracted folders: sys/boot.bin next to files/ carries the game ID, when present.
bool folderGameId(const std::string& root, std::string* id)
{
	std::string boot = root + "\\..\\sys\\boot.bin";
	FILE* f          = _wfopen(wide(boot).c_str(), L"rb");
	if (f == nullptr)
		return false;
	char buf[6];
	bool ok = std::fread(buf, 1, 6, f) == 6;
	std::fclose(f);
	if (ok)
		id->assign(buf, 6);
	return ok;
}

const char* regionName(const std::string& id)
{
	if (id.size() >= 4 && id.compare(0, 3, "GMS") == 0) {
		switch (id[3]) {
		case 'E':
			return "Super Mario Sunshine, US (GMSE01)";
		case 'J':
			return "Super Mario Sunshine, Japan (GMSJ01)";
		case 'P':
			return "Super Mario Sunshine, Europe (GMSP01)";
		}
	}
	return "another GameCube game";
}

// w13: the European disc was refused until its PAL-only 2D code (TConsoleStr, the option
// screen, the file select, the 60 Hz question, the talk balloons) was reconstructed; it
// boots to gameplay now (experimental, BOOTLOG "Regional behaviour (w13)"). SMS_PORT_EU_DISC=0
// still refuses it.
bool euDiscBlocked(const std::string& p, const std::string& id, std::string* err)
{
	const char* e = std::getenv("SMS_PORT_EU_DISC");
	if (id.size() < 4 || id[3] != 'P' || e == nullptr || e[0] != '0')
		return false;
	*err = p + "\nis " + regionName(id)
	       + ".\n\nThe European disc is turned off (SMS_PORT_EU_DISC=0). Please use the US (GMSE01) or "
	         "Japanese (GMSJ01) disc, or remove the variable.";
	return true;
}

// w13: runs the sibling exe built for `id`'s region on disc `p` (same arguments, the disc in
// %SMS_PORT_DISC%, std handles inherited, killed together with this process through a job
// object), waits for it and exits with its exit code. Returns only if that is impossible
// (sibling missing, or already a hand-off), with *err saying why.
bool handOff(const std::string& p, const std::string& id, std::string* err)
{
	const char* name = id[3] == 'J' ? "sms_port_jp.exe" : id[3] == 'P' ? "sms_port_eu.exe" : "sms_port.exe";
	std::string exe = exeDir() + "\\" + name;
	if (!isFile(exe)) { // development tree: port/build_aurora{,_jp,_eu}/ side by side
		const char* sub = id[3] == 'J' ? "build_aurora_jp" : id[3] == 'P' ? "build_aurora_eu" : "build_aurora";
		const std::string dev = exeDir() + "\\..\\" + sub + "\\" + name;
		if (isFile(dev))
			exe = dev;
	}
	if (std::getenv("SMS_PORT_HANDOFF") != nullptr) {
		*err = p + "\nis " + regionName(id) + ", but this program is the " + kGameId + " build (hand-off loop).";
		return false;
	}
	if (!isFile(exe)) {
		*err = p + "\nis " + regionName(id) + ".\n\nIt needs " + name + " next to this program, which is missing.";
		return false;
	}
	std::fprintf(stderr, "[port] disc: %s is %.6s; handing over to %s\n", p.c_str(), id.c_str(), name);
	std::fflush(stderr);
	if (SDL_Window* w = aurora_get_window()) // only after the file dialog: early hand-offs have no window
		SDL_HideWindow(w);
	std::fflush(stdout);
	SetEnvironmentVariableW(L"SMS_PORT_DISC", wide(p).c_str());
	SetEnvironmentVariableW(L"SMS_PORT_HANDOFF", L"1");
	// Command line: the sibling's path, then everything after argv[0] of ours.
	std::wstring cmd = L"\"" + wide(exe) + L"\"";
	const wchar_t* rest = GetCommandLineW();
	bool quoted = false;
	for (; *rest != L'\0' && (quoted || (*rest != L' ' && *rest != L'\t')); ++rest)
		if (*rest == L'"')
			quoted = !quoted;
	cmd += rest;
	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim = {};
	lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (job != nullptr)
		SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof(lim));
	STARTUPINFOW si = {};
	si.cb         = sizeof(si);
	si.dwFlags    = STARTF_USESTDHANDLES;
	si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
	DWORD code = 0;
	do { // w13: the child exits with PORT_EXIT_RESTART when the settings menu restarts the game
		PROCESS_INFORMATION pi = {};
		if (!CreateProcessW(wide(exe).c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr,
		                    &si, &pi)) {
			*err = "Could not start " + exe + ".";
			return false;
		}
		if (job != nullptr)
			AssignProcessToJobObject(job, pi.hProcess);
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);
		WaitForSingleObject(pi.hProcess, INFINITE);
		GetExitCodeProcess(pi.hProcess, &code);
		CloseHandle(pi.hProcess);
		if (code == PORT_EXIT_RESTART) {
			std::fprintf(stderr, "[port] disc: %s restarts\n", name);
			SetEnvironmentVariableW(L"SMS_PORT_RESTARTED", L"1");
		}
	} while (code == PORT_EXIT_RESTART);
	std::fflush(stderr);
	TerminateProcess(GetCurrentProcess(), code); // no atexit/Aurora teardown: nothing was started here
	return false;
}

// Tries one candidate. Returns true if usable; *err is set when it is unusable in a way the
// user should hear about (wrong game, unsupported format).
bool tryPath(const std::string& p, std::string* folder, std::vector<PortDiscEntry>* entries, bool* image,
             std::string* err)
{
	if (p.empty())
		return false;
	if (isDirectory(p)) {
		std::string root;
		if (!resolveFolder(p, &root))
			return false;
		std::string id;
		if (folderGameId(root, &id) && !isSmsId(id)) { // w13: any SMS region
			*err = "The extracted game in\n" + p + "\nis " + regionName(id)
			       + ".\n\nThis PC port needs Super Mario Sunshine (GMSE01, GMSJ01 or GMSP01).";
			return false;
		}
		if (euDiscBlocked(p, id, err)) // w13
			return false;
		if (!id.empty() && id != kGameId && !handOff(p, id, err)) // w13: other region: sibling exe
			return false;
		*folder = root;
		gIdentity = "folder" + (id.empty() ? std::string() : " " + id); // no path: replays get sent around
		*image  = false;
		return true;
	}
	if (!isFile(p))
		return false;
	Image im;
	std::string id, why;
	switch (openImage(p, &im, &id, &why)) {
	case Probe::Ok:
		break;
	case Probe::WrongGame:
		*err = p + "\nis " + regionName(id) + ".\n\nThis PC port needs Super Mario Sunshine (GMSE01, GMSJ01 or GMSP01).";
		return false;
	case Probe::Unsupported:
		*err = p + "\ncannot be used: " + (why.empty() ? std::string("a Wii disc image (WBFS)") : why) + ".";
		return false;
	case Probe::Damaged:
		*err = p + "\nis damaged (unreadable disc header).";
		return false;
	case Probe::NotGameCube:
		*err = p + "\nis not a GameCube disc image.";
		return false;
	case Probe::NotFound:
		return false;
	}
	if (euDiscBlocked(p, id, err) || id != kGameId) { // w13: EU refused; another region: the sibling exe
		if (im.h != INVALID_HANDLE_VALUE)
			CloseHandle(im.h);
		if (err->empty())
			handOff(p, id, err);
		return false;
	}
	if (!loadFst(im, entries)) {
		if (im.h != INVALID_HANDLE_VALUE)
			CloseHandle(im.h);
		*err = p + "\nhas a damaged file system table.";
		return false;
	}
	if (gImage.h != INVALID_HANDLE_VALUE)
		CloseHandle(gImage.h);
	gImage = std::move(im);
	{
		// Disc identity for replays: game ID plus a hash of the disc header and FST.
		uint8_t head[0x440];
		uint64_t h = 1469598103934665603ull;
		if (imageRead(gImage, 0, head, sizeof(head)) == (int32_t)sizeof(head))
			for (uint8_t c : head)
				h = (h ^ c) * 1099511628211ull;
		for (const PortDiscEntry& e : *entries)
			for (uint32_t v : { e.offset, e.size, (uint32_t)e.name.size() })
				h = (h ^ v) * 1099511628211ull;
		char buf[64];
		std::snprintf(buf, sizeof(buf), "%.6s %016llx", (const char*)head, (unsigned long long)h);
		const size_t slash = p.find_last_of("\\/");
		gIdentity          = std::string("image ") + buf + " " + (slash == std::string::npos ? p : p.substr(slash + 1));
	}
	*image = true;
	return true;
}

std::vector<std::string> commandLinePaths()
{
	std::vector<std::string> out;
	int argc       = 0;
	LPWSTR* argv   = CommandLineToArgvW(GetCommandLineW(), &argc);
	for (int i = 1; argv != nullptr && i < argc; i++) {
		if (argv[i][0] == L'-')
			continue; // flags (Aurora's own options)
		char u8[MAX_PATH * 3];
		int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, u8, sizeof(u8), nullptr, nullptr);
		if (n > 1)
			out.push_back(u8);
	}
	LocalFree(argv);
	return out;
}

// Disc images in `dir` (*.iso, *.gcm, *.ciso, *.rvz, *.wia, *.gcz).
std::vector<std::string> imagesIn(const std::string& dir)
{
	std::vector<std::string> out;
	WIN32_FIND_DATAW fd;
	HANDLE f = FindFirstFileW(wide(dir + "\\*").c_str(), &fd);
	if (f == INVALID_HANDLE_VALUE)
		return out;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		char u8[MAX_PATH * 3];
		WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, u8, sizeof(u8), nullptr, nullptr);
		std::string n = u8;
		if (endsWith(n, ".iso") || endsWith(n, ".gcm") || endsWith(n, ".ciso") || endsWith(n, ".rvz")
		    || endsWith(n, ".wia") || endsWith(n, ".gcz"))
			out.push_back(dir + "\\" + n);
	} while (FindNextFileW(f, &fd));
	FindClose(f);
	return out;
}

// File-open dialog (SDL3: asynchronous; the callback runs on another thread on Windows).
std::atomic<int> gDialogState; // 0 waiting, 1 chosen, 2 cancelled
std::string gDialogPath;
void SDLCALL dialogDone(void*, const char* const* files, int)
{
	if (files != nullptr && files[0] != nullptr) {
		gDialogPath = files[0];
		gDialogState.store(1);
	} else {
		gDialogState.store(2);
	}
}

// The prompt runs on its own thread with no owner window, so the game window stays enabled
// and accepts a disc image or folder dragged onto it while the prompt is up.
const wchar_t kPromptTitle[] = L"Super Mario Sunshine - game data";
std::atomic<int> gPromptState; // 0 open, 1 OK (browse), 2 Quit
std::wstring gPromptText;
DWORD WINAPI promptThread(void*)
{
	int r = MessageBoxW(nullptr, gPromptText.c_str(), kPromptTitle, MB_OKCANCEL | MB_ICONINFORMATION | MB_SETFOREGROUND);
	gPromptState.store(r == IDOK ? 1 : 2);
	return 0;
}

// Waits for a drop on the game window (returns true with its path) or for the prompt
// (returns false; gPromptState says which button). Quitting the window exits.
bool waitForDropOrPrompt(std::string* dropped)
{
	while (gPromptState.load() == 0) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
				std::exit(0);
			if (e.type == SDL_EVENT_DROP_FILE && e.drop.data != nullptr) {
				*dropped = e.drop.data;
				if (HWND box = FindWindowW(L"#32770", kPromptTitle))
					PostMessageW(box, WM_CLOSE, 0, 0);
				return true;
			}
		}
		SDL_Delay(10);
	}
	return false;
}

std::string askForDisc(const std::string& reason)
{
	SDL_Window* win = aurora_get_window();
	std::fprintf(stderr, "[port] disc: %s (asking for the disc)\n", reason.c_str());
	std::string msg = reason
	                  + "\n\nThis PC port runs Super Mario Sunshine from your own copy of the game (US GMSE01, "
	                    "Japan GMSJ01 or Europe GMSP01) as a disc image (.iso, .rvz, .gcm, .ciso, .wia or .gcz), or a folder extracted with Dolphin "
	                    "(Right-click > Properties > Filesystem > Extract Entire Disc).\n\n"
	                    "Drag and drop the disc image (or folder) onto the game window, or click OK to choose it. "
	                    "You can also put it next to sms_port.exe, pass it on the command line, or set disc = <path> "
	                    "in port_settings.ini.";
	if (win != nullptr) {
		SDL_SetWindowTitle(win, "Super Mario Sunshine - drop your disc image here");
		SDL_ShowWindow(win);
	}
	SDL_SetEventEnabled(SDL_EVENT_DROP_FILE, true);
	gPromptText.assign(msg.size() + 1, L'\0');
	gPromptText.resize((size_t)MultiByteToWideChar(CP_UTF8, 0, msg.c_str(), -1, &gPromptText[0], (int)gPromptText.size()));
	if (!gPromptText.empty() && gPromptText.back() == L'\0')
		gPromptText.pop_back();
	gPromptState.store(0);
	if (HANDLE t = CreateThread(nullptr, 0, promptThread, nullptr, 0, nullptr))
		CloseHandle(t);
	else
		gPromptState.store(1);
	std::string dropped;
	const bool gotDrop = waitForDropOrPrompt(&dropped);
	if (win != nullptr)
		SDL_SetWindowTitle(win, "Super Mario Sunshine");
	if (gotDrop) {
		std::fprintf(stderr, "[port] disc: dropped %s\n", dropped.c_str());
		return dropped;
	}
	if (gPromptState.load() != 1)
		std::exit(0);
	static const SDL_DialogFileFilter filters[] = {
		{ "GameCube disc image (*.iso, *.rvz, *.gcm, *.ciso, *.wia, *.gcz)", "iso;rvz;gcm;ciso;wia;gcz" },
		{ "All files", "*" },
	};
	gDialogState.store(0);
	SDL_ShowOpenFileDialog(dialogDone, nullptr, win, filters, 2, nullptr, false);
	while (gDialogState.load() == 0) {
		SDL_Event e; // keeps the (parent) window responsive while the dialog is open
		while (SDL_PollEvent(&e))
			if (e.type == SDL_EVENT_DROP_FILE && e.drop.data != nullptr)
				return e.drop.data; // dropped on the window instead
		SDL_Delay(10);
	}
	if (gDialogState.load() != 1)
		std::exit(0);
	return gDialogPath;
}

} // namespace

bool port_disc_locate(std::string* folder, std::vector<PortDiscEntry>* entries)
{
	bool image = false;
	std::string err;
	auto attempt = [&](const std::string& p, const char* source) {
		std::string e;
		if (tryPath(p, folder, entries, &image, &e)) {
			std::fprintf(stderr, "[port] disc: %s %s (%s)\n", image ? "image" : "folder", p.c_str(), source);
			return true;
		}
		if (!e.empty() && err.empty())
			err = e;
		return false;
	};

	for (const std::string& p : commandLinePaths())
		if (attempt(p, "command line"))
			return image;
	if (const char* env = std::getenv("SMS_PORT_DISC"))
		if (*env != '\0' && attempt(env, "SMS_PORT_DISC"))
			return image;
	if (gPortSettings.disc[0] != '\0' && attempt(gPortSettings.disc, "port_settings.ini"))
		return image;
	// A disc the player named explicitly but that cannot be used (wrong region, Wii disc, ...):
	// say why instead of silently picking something else.
	const bool explicitError = !err.empty();

	// Auto-detect: next to the exe, then ./files, then walking up from the exe.
	const std::string dir = exeDir();
	if (!explicitError) {
	if (attempt(dir, "next to the exe"))
		return image;
	for (const std::string& p : imagesIn(dir))
		if (attempt(p, "next to the exe"))
			return image;
	char cwd[MAX_PATH];
	GetCurrentDirectoryA(MAX_PATH, cwd);
	if (attempt(std::string(cwd) + "\\files", "working directory"))
		return image;
	for (std::string d = dir;;) {
		size_t cut = d.find_last_of("\\/");
		if (cut == std::string::npos)
			break;
		d.resize(cut);
		if (attempt(d + "\\dolphin_test\\files", "folder walk-up") || attempt(d + "\\files", "folder walk-up"))
			return image;
	}
	}

	// Ask.
	for (;;) {
		std::string p = askForDisc(err.empty() ? "The game data was not found." : err);
		err.clear();
		if (attempt(p, "chosen")) {
			std::snprintf(gPortSettings.disc, sizeof(gPortSettings.disc), "%s", p.c_str());
			port_settings_save();
			return image;
		}
		if (err.empty())
			err = p + "\nis not Super Mario Sunshine game data.";
	}
}

const char* port_disc_identity(void) { return gIdentity.c_str(); }

// w13: region of this build, and the early hand-off (port_main.cpp, before Aurora opens a
// window): the first disc the normal search would find (command line, %SMS_PORT_DISC%,
// settings, auto-detection; no dialog) is checked, and a disc of another region goes to the
// sibling exe right away. Anything else is left to port_disc_locate.
char port_build_region(void) { return kGameId[3]; }
const char* port_build_game_id(void) { return kGameId; }

void port_disc_early_handoff(void)
{
	auto probe = [](const std::string& p) -> bool {
		if (p.empty())
			return false;
		std::string id;
		if (isDirectory(p)) {
			std::string root;
			if (!resolveFolder(p, &root))
				return false;
			folderGameId(root, &id);
		} else {
			if (!isFile(p))
				return false;
			Image im;
			std::string why;
			const Probe r = openImage(p, &im, &id, &why);
			if (im.h != INVALID_HANDLE_VALUE)
				CloseHandle(im.h);
			if (r != Probe::Ok)
				return r != Probe::NotFound; // a named but unusable disc: stop here, locate explains
		}
		std::string err;
		if (isSmsId(id) && id != kGameId && !euDiscBlocked(p, id, &err))
			handOff(p, id, &err); // returns only on failure; locate shows the message later
		return true;
	};
	for (const std::string& p : commandLinePaths())
		if (probe(p))
			return;
	if (const char* env = std::getenv("SMS_PORT_DISC"))
		if (probe(env))
			return;
	if (gPortSettings.disc[0] != '\0' && probe(gPortSettings.disc))
		return;
	const std::string dir = exeDir();
	if (probe(dir))
		return;
	for (const std::string& p : imagesIn(dir))
		if (probe(p))
			return;
	char cwd[MAX_PATH];
	GetCurrentDirectoryA(MAX_PATH, cwd);
	if (probe(std::string(cwd) + "\\files"))
		return;
	for (std::string d = dir;;) {
		size_t cut = d.find_last_of("\\/");
		if (cut == std::string::npos)
			break;
		d.resize(cut);
		if (probe(d + "\\dolphin_test\\files") || probe(d + "\\files"))
			return;
	}
}

int32_t port_disc_read(uint32_t offset, void* dst, int32_t length)
{
	return imageRead(gImage, offset, dst, length);
}

#endif // SMS_PORT_AURORA
