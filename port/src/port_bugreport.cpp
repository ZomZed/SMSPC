// Bug reports (w12): a rolling capture of the last [bugreport] seconds (default 20) of what
// the player sees and hears plus every log line, saved on F9, from the settings menu ("Save a
// bug report") or on a crash as bug_reports\<yyyy-mm-dd_hh-mm-ss>_<f9|menu|crash>\ next to
// sms_port.exe:
//   video.mp4   H.264 + AAC, the presented picture downscaled to video_width x video_height at
//               `fps` frames per second, with the game's final stereo mix;
//   log.txt     port build, Windows, GPU, window/internal resolution, settings, stage, Mario,
//               frame rate, multiplayer state, (crash) the backtrace, then the last seconds of
//               stdout/stderr lines, each with wall-clock time and the presented-frame number;
//   sms_replay_<date>_<time>.bin   the input recording (port_replay.cpp).
//
// How it runs (all Windows built-ins, no third-party code):
// - Picture: once per capture period port_bugreport_frame() (main thread, per presented frame)
//   arms Aurora's present capture (aurora_present_capture.patch; port_aurora_sdk.cpp
//   port_bugreport_arm_capture). The readback is asynchronous: when the GPU copy is mapped,
//   port_bugreport_push_frame() shrinks it to NV12 (nearest sampling, ~0.5 ms) and queues it
//   for the encoder thread, which runs Microsoft's H.264 encoder MFT (low latency, a keyframe
//   every second) and keeps the encoded samples in a memory ring of `seconds` + 2 s.
// - Sound: port_audio_out_ai() hands the stereo mix here (port_bugreport_audio), kept as PCM
//   in a ring of the same length.
// - Log: stdout and stderr are routed through pipes (dup2); a thread per pipe forwards the
//   bytes to the original handles (the console or the parent's redirection, unchanged) and
//   keeps the lines, time-stamped, in a ring.
// - Save: the encoder thread muxes the samples from the first keyframe inside the window into
//   the .mp4 with an IMFSinkWriter (video passed through, audio resampled to 48 kHz and AAC
//   encoded by the sink writer); log.txt and the replay are written by the caller. F9 / menu
//   return at once (the mux finishes in the background, a toast names the folder); a crash
//   waits for it at most kCrashWaitMs and never deadlocks (if the crash is on the encoder
//   thread, or the wait runs out, log.txt and the replay are still there).
// Off ([bugreport] enabled = 0, or SMS_PORT_BUGREPORT=0): nothing here runs.
//
// Test knobs: SMS_PORT_TEST_F9=<frame> saves a report at that presented frame (and exits
// 10 s later); SMS_PORT_TEST_CRASH=<frame> crashes (null write) at that frame.
#ifdef SMS_PORT_AURORA

#include "port_bugreport.h"
#include "port_replay.h"
#include "port_settings.h"

#include <SDL3/SDL_video.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fcntl.h>
#include <io.h>

#include <strmif.h> // ICodecAPI
#include <codecapi.h>
#include <dxgi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dxgi.lib")

extern "C" volatile unsigned gPortFrameCount;       // port_crash.cpp: presented frames
extern "C" void port_qol_toast(const char* text);   // port_qol.cpp
extern "C" const char* port_gfx_backend_status(void); // port_gfx_backend.cpp
extern "C" bool port_replay_save_dir(const char* why, const char* dir); // port_replay.cpp
extern "C" void port_bugreport_arm_capture(void);  // port_aurora_sdk.cpp
extern "C" void port_crash_log(const char* fmt, ...);  // port_crash.cpp: stderr + sms_port_crash.log

namespace {

// Microsoft's software H.264 encoder MFT (always present on Windows 8+).
const CLSID kH264Encoder = { 0x6CA50344, 0x051A, 0x4DED, { 0x97, 0x79, 0xA4, 0x33, 0x05, 0x16, 0x5E, 0x35 } };

const int kCrashWaitMs  = 8000;
const int kAudioOutRate = 48000; // the AAC encoder takes 44.1 / 48 kHz only
const size_t kLogMaxBytes = 4u << 20;

bool sEnabled;
int sSeconds = 20, sWidth = 640, sHeight = 480, sFps = 30;
LARGE_INTEGER sQpcFreq;
long long sQpc0; // the capture's time origin

long long qpcNow()
{
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return c.QuadPart;
}
long long to100ns(long long qpc) { return (long long)((qpc - sQpc0) * 10000000.0 / (double)sQpcFreq.QuadPart); }

std::string exeDir()
{
	char buf[MAX_PATH];
	DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	std::string p(buf, n);
	size_t slash = p.find_last_of("\\/");
	return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

// ---- log ring (stdout + stderr tee) -----------------------------------------------------
struct LogLine {
	long long qpc;
	std::string text; // "[hh:mm:ss.mmm f123] line"
};
CRITICAL_SECTION sLogLock;
std::deque<LogLine> sLog;
size_t sLogBytes;

void logPush(const char* s, size_t n)
{
	SYSTEMTIME st;
	GetLocalTime(&st);
	char pre[48];
	const int pn = std::snprintf(pre, sizeof(pre), "[%02d:%02d:%02d.%03d f%u] ", st.wHour, st.wMinute, st.wSecond,
	                             st.wMilliseconds, gPortFrameCount);
	LogLine l;
	l.qpc = qpcNow();
	l.text.reserve((size_t)pn + n);
	l.text.assign(pre, (size_t)pn);
	l.text.append(s, n);
	EnterCriticalSection(&sLogLock);
	sLogBytes += l.text.size();
	sLog.push_back(std::move(l));
	const long long keep = (long long)(sSeconds + 2) * sQpcFreq.QuadPart;
	while (!sLog.empty() && (sLogBytes > kLogMaxBytes || sLog.back().qpc - sLog.front().qpc > keep)) {
		sLogBytes -= sLog.front().text.size();
		sLog.pop_front();
	}
	LeaveCriticalSection(&sLogLock);
}

struct Tee {
	HANDLE rd, orig;
};

DWORD WINAPI teeThread(LPVOID p)
{
	Tee* t = (Tee*)p;
	char buf[8192];
	std::string partial;
	for (;;) {
		DWORD n = 0;
		if (!ReadFile(t->rd, buf, sizeof(buf), &n, nullptr) || n == 0)
			break;
		if (t->orig != nullptr) {
			DWORD w;
			WriteFile(t->orig, buf, n, &w, nullptr);
		}
		size_t s = 0;
		for (size_t i = 0; i < n; i++) {
			if (buf[i] != '\n')
				continue;
			size_t e = i;
			if (partial.empty()) {
				if (e > s && buf[e - 1] == '\r')
					--e;
				logPush(buf + s, e - s);
			} else {
				partial.append(buf + s, e - s);
				if (!partial.empty() && partial.back() == '\r')
					partial.pop_back();
				logPush(partial.data(), partial.size());
				partial.clear();
			}
			s = i + 1;
		}
		if (s < n)
			partial.append(buf + s, n - s);
		if (partial.size() > 65536) { // a line that never ends: keep it anyway
			logPush(partial.data(), partial.size());
			partial.clear();
		}
	}
	return 0;
}

// Routes stdout (fd 1) / stderr (fd 2) through a pipe read by teeThread.
void teeInstall(int fd, DWORD stdId)
{
	// The shipped exe is a GUI-subsystem program: the UCRT leaves stdout/stderr without a descriptor
	// (_fileno == -2), so printf never reaches fd 1/2 and a plain _dup2 onto 1/2 captured nothing (empty
	// "last 20 s of log" in every bug report). Give the stream a real descriptor first, then redirect that.
	FILE* stream = fd == 1 ? stdout : stderr;
	if (_fileno(stream) < 0 && std::freopen("NUL", "w", stream) == nullptr)
		return;
	fd = _fileno(stream);
	if (fd < 0)
		return;
	HANDLE orig = GetStdHandle(stdId);
	if (orig == INVALID_HANDLE_VALUE)
		orig = nullptr;
	HANDLE rd, wr;
	SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, FALSE };
	if (!CreatePipe(&rd, &wr, &sa, 1 << 16))
		return;
	const int wfd = _open_osfhandle((intptr_t)wr, _O_WRONLY | _O_TEXT);
	if (wfd < 0) {
		CloseHandle(rd);
		CloseHandle(wr);
		return;
	}
	if (orig != nullptr) { // keep our own handle to the original (the CRT closes fd's old one)
		HANDLE dup = nullptr;
		DuplicateHandle(GetCurrentProcess(), orig, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
		orig = dup;
	}
	std::fflush(stream);
	if (_dup2(wfd, fd) != 0) {
		_close(wfd);
		CloseHandle(rd);
		return;
	}
	_close(wfd);
	SetStdHandle(stdId, (HANDLE)_get_osfhandle(fd));
	std::setvbuf(stream, nullptr, _IONBF, 0); // freopen reset it; lines must reach the ring at once
	Tee* t = new Tee { rd, orig };
	HANDLE th = CreateThread(nullptr, 0, teeThread, t, 0, nullptr);
	if (th != nullptr)
		CloseHandle(th);
}

// ---- audio ring ------------------------------------------------------------------------
SRWLOCK sAudioLock = SRWLOCK_INIT;
std::vector<int16_t> sAudio; // circular, interleaved L/R
size_t sAudioCap, sAudioPos;  // frames
unsigned long long sAudioTotal; // frames pushed so far
long long sAudioEndQpc;         // when the last push happened
unsigned sAudioRate;

// ---- picture queue ---------------------------------------------------------------------
struct RawFrame {
	std::vector<uint8_t> nv12;
	long long qpc;
};
CRITICAL_SECTION sFrameLock;
std::deque<RawFrame*> sPending; // to the encoder thread
std::vector<RawFrame*> sFree;
HANDLE sWork;         // encoder thread wake-up
HANDLE sEncThread;
DWORD sEncThreadId;
long long sLastArm;
unsigned sDropped;

// ---- encoded ring (encoder thread only) --------------------------------------------------
struct EncSample {
	long long t, dur; // 100 ns
	bool key;
	std::vector<uint8_t> data;
};
std::deque<EncSample> sRing;
size_t sRingBytes;
IMFTransform* sMft;
IMFMediaType* sOutType; // the encoder's current output type
bool sEncOk, sEncTried;
bool sProvidesSamples;
long long sLastT = -1;
unsigned sEncoded, sKeyframes;
char sGpuName[128] = "?";

// ---- save request ----------------------------------------------------------------------
struct SaveReq {
	char path[MAX_PATH];
	HANDLE done;
	bool ok;
	char result[160];
};
SaveReq* volatile sSaveReq;
volatile LONG sSaving;

template <class T> void release(T*& p)
{
	if (p != nullptr)
		p->Release();
	p = nullptr;
}

void gpuName()
{
	IDXGIFactory1* f = nullptr;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&f)) || f == nullptr)
		return;
	IDXGIAdapter1* a = nullptr;
	for (UINT i = 0; f->EnumAdapters1(i, &a) == S_OK; i++) {
		DXGI_ADAPTER_DESC1 d;
		if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
			WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, sGpuName, sizeof(sGpuName), nullptr, nullptr);
			std::snprintf(sGpuName + std::strlen(sGpuName), sizeof(sGpuName) - std::strlen(sGpuName), " (%llu MB)",
			              (unsigned long long)(d.DedicatedVideoMemory >> 20));
			release(a);
			break;
		}
		release(a);
	}
	release(f);
}

bool encoderInit()
{
	sEncTried = true;
	HRESULT hr = CoCreateInstance(kH264Encoder, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IMFTransform), (void**)&sMft);
	if (FAILED(hr) || sMft == nullptr) {
		std::fprintf(stderr, "[port] bugreport: no H.264 encoder (0x%08lx): reports have no video\n", hr);
		return false;
	}
	ICodecAPI* api = nullptr;
	if (SUCCEEDED(sMft->QueryInterface(__uuidof(ICodecAPI), (void**)&api)) && api != nullptr) {
		VARIANT v;
		v.vt   = VT_UI4;
		v.ulVal = (ULONG)sFps; // a keyframe every second
		api->SetValue(&CODECAPI_AVEncMPVGOPSize, &v);
		v.vt    = VT_UI4;
		v.ulVal = 0; // no B-frames
		api->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &v);
		v.vt      = VT_BOOL;
		v.boolVal = VARIANT_TRUE;
		api->SetValue(&CODECAPI_AVLowLatencyMode, &v);
		v.vt    = VT_UI4;
		v.ulVal = eAVEncCommonRateControlMode_CBR;
		api->SetValue(&CODECAPI_AVEncCommonRateControlMode, &v);
		release(api);
	}
	const UINT32 bitrate = (UINT32)((double)sWidth * sHeight * sFps * 0.12); // 640x480x30: ~1.1 Mbit/s... x2 below
	IMFMediaType* ot = nullptr;
	MFCreateMediaType(&ot);
	ot->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	ot->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
	ot->SetUINT32(MF_MT_AVG_BITRATE, bitrate * 2);
	MFSetAttributeSize(ot, MF_MT_FRAME_SIZE, (UINT32)sWidth, (UINT32)sHeight);
	MFSetAttributeRatio(ot, MF_MT_FRAME_RATE, (UINT32)sFps, 1);
	MFSetAttributeRatio(ot, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	ot->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	ot->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
	hr = sMft->SetOutputType(0, ot, 0);
	if (FAILED(hr)) {
		std::fprintf(stderr, "[port] bugreport: H.264 output type refused (0x%08lx)\n", hr);
		release(ot);
		return false;
	}
	sOutType = ot;
	IMFMediaType* it = nullptr;
	MFCreateMediaType(&it);
	it->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	it->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
	MFSetAttributeSize(it, MF_MT_FRAME_SIZE, (UINT32)sWidth, (UINT32)sHeight);
	MFSetAttributeRatio(it, MF_MT_FRAME_RATE, (UINT32)sFps, 1);
	MFSetAttributeRatio(it, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	it->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	hr = sMft->SetInputType(0, it, 0);
	release(it);
	if (FAILED(hr)) {
		std::fprintf(stderr, "[port] bugreport: H.264 input type refused (0x%08lx)\n", hr);
		return false;
	}
	MFT_OUTPUT_STREAM_INFO si = {};
	sMft->GetOutputStreamInfo(0, &si);
	sProvidesSamples = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
	sMft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
	sMft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	std::fprintf(stderr, "[port] bugreport: capturing %dx%d at %d fps, last %d s (H.264 %u kbit/s)\n", sWidth, sHeight,
	             sFps, sSeconds, bitrate * 2 / 1000);
	return true;
}

void ringPush(EncSample&& s)
{
	sRingBytes += s.data.size();
	sRing.push_back(std::move(s));
	const long long keep = (long long)(sSeconds + 2) * 10000000ll;
	while (sRing.size() > 1 && (sRing.back().t - sRing.front().t > keep || sRingBytes > (64u << 20))) {
		sRingBytes -= sRing.front().data.size();
		sRing.pop_front();
	}
}

void encoderDrain()
{
	for (;;) {
		MFT_OUTPUT_DATA_BUFFER ob = {};
		IMFSample* mine           = nullptr;
		if (!sProvidesSamples) {
			MFT_OUTPUT_STREAM_INFO si = {};
			sMft->GetOutputStreamInfo(0, &si);
			IMFMediaBuffer* b = nullptr;
			MFCreateSample(&mine);
			MFCreateMemoryBuffer(si.cbSize > 0 ? si.cbSize : (DWORD)(sWidth * sHeight * 4), &b);
			mine->AddBuffer(b);
			release(b);
			ob.pSample = mine;
		}
		DWORD status = 0;
		HRESULT hr   = sMft->ProcessOutput(0, 1, &ob, &status);
		if (ob.pEvents != nullptr)
			ob.pEvents->Release();
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
			IMFMediaType* t = nullptr;
			if (SUCCEEDED(sMft->GetOutputAvailableType(0, 0, &t)) && t != nullptr) {
				if (SUCCEEDED(sMft->SetOutputType(0, t, 0))) {
					release(sOutType);
					sOutType = t;
				} else
					release(t);
			}
			release(mine);
			continue;
		}
		if (FAILED(hr)) { // MF_E_TRANSFORM_NEED_MORE_INPUT or an error
			release(mine);
			break;
		}
		IMFSample* out = ob.pSample;
		if (out != nullptr) {
			IMFMediaBuffer* cb = nullptr;
			if (SUCCEEDED(out->ConvertToContiguousBuffer(&cb)) && cb != nullptr) {
				BYTE* p   = nullptr;
				DWORD len = 0;
				if (SUCCEEDED(cb->Lock(&p, nullptr, &len)) && len > 0) {
					EncSample s;
					s.data.assign(p, p + len);
					s.key = MFGetAttributeUINT32(out, MFSampleExtension_CleanPoint, 0) != 0;
					if (FAILED(out->GetSampleTime(&s.t)))
						s.t = sLastT;
					if (FAILED(out->GetSampleDuration(&s.dur)) || s.dur <= 0)
						s.dur = 10000000ll / sFps;
					cb->Unlock();
					++sEncoded;
					if (s.key)
						++sKeyframes;
					ringPush(std::move(s));
				}
				release(cb);
			}
			if (out != mine)
				out->Release();
		}
		release(mine);
	}
}

void encodeFrame(const RawFrame& f)
{
	if (!sEncOk)
		return;
	long long t = to100ns(f.qpc);
	if (t <= sLastT)
		t = sLastT + 1;
	sLastT = t;
	IMFMediaBuffer* b = nullptr;
	const DWORD n     = (DWORD)f.nv12.size();
	if (FAILED(MFCreateMemoryBuffer(n, &b)) || b == nullptr)
		return;
	BYTE* p = nullptr;
	if (SUCCEEDED(b->Lock(&p, nullptr, nullptr))) {
		std::memcpy(p, f.nv12.data(), n);
		b->Unlock();
	}
	b->SetCurrentLength(n);
	IMFSample* s = nullptr;
	MFCreateSample(&s);
	s->AddBuffer(b);
	s->SetSampleTime(t);
	s->SetSampleDuration(10000000ll / sFps);
	HRESULT hr = sMft->ProcessInput(0, s, 0);
	release(s);
	release(b);
	if (hr == MF_E_NOTACCEPTING) {
		encoderDrain();
		return;
	}
	encoderDrain();
}

// SPS/PPS of the first keyframe (Annex B), for MF_MT_MPEG_SEQUENCE_HEADER.
std::vector<uint8_t> sequenceHeader(const std::vector<uint8_t>& d)
{
	std::vector<uint8_t> out;
	size_t i = 0;
	while (i + 4 <= d.size()) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			size_t start = i + 3;
			size_t end   = start;
			while (end + 3 <= d.size() && !(d[end] == 0 && d[end + 1] == 0 && (d[end + 2] == 1 || (end + 3 < d.size() && d[end + 2] == 0 && d[end + 3] == 1))))
				++end;
			if (end + 3 > d.size())
				end = d.size();
			const int type = d[start] & 0x1F;
			if (type == 7 || type == 8) {
				const uint8_t sc[4] = { 0, 0, 0, 1 };
				out.insert(out.end(), sc, sc + 4);
				out.insert(out.end(), d.begin() + (long)start, d.begin() + (long)end);
			}
			i = end;
		} else
			++i;
	}
	return out;
}

bool writeSample(IMFSinkWriter* sw, DWORD stream, const uint8_t* data, size_t n, long long t, long long dur, bool key)
{
	IMFMediaBuffer* b = nullptr;
	if (FAILED(MFCreateMemoryBuffer((DWORD)n, &b)) || b == nullptr)
		return false;
	BYTE* p = nullptr;
	if (SUCCEEDED(b->Lock(&p, nullptr, nullptr))) {
		std::memcpy(p, data, n);
		b->Unlock();
	}
	b->SetCurrentLength((DWORD)n);
	IMFSample* s = nullptr;
	MFCreateSample(&s);
	s->AddBuffer(b);
	s->SetSampleTime(t);
	s->SetSampleDuration(dur);
	if (key)
		s->SetUINT32(MFSampleExtension_CleanPoint, 1);
	const HRESULT hr = sw->WriteSample(stream, s);
	release(s);
	release(b);
	return SUCCEEDED(hr);
}

// Encoder thread: writes video.mp4 from the ring and the audio ring.
void mux(SaveReq* req)
{
	req->ok = false;
	if (!sEncOk || sRing.empty()) {
		std::snprintf(req->result, sizeof(req->result), "no video (encoder %s, %zu samples)", sEncOk ? "ok" : "unavailable",
		              sRing.size());
		return;
	}
	const long long endT   = sRing.back().t + sRing.back().dur;
	const long long startT = endT - (long long)sSeconds * 10000000ll;
	size_t first = 0;
	for (size_t i = 0; i < sRing.size(); i++)
		if (sRing[i].key && sRing[i].t >= startT) {
			first = i;
			break;
		}
	if (!sRing[first].key) // no keyframe inside the window: the newest one before it
		for (size_t i = sRing.size(); i-- > 0;)
			if (sRing[i].key) {
				first = i;
				break;
			}
	const long long t0 = sRing[first].t;

	// Audio window: the ring's frame j (0 = oldest kept) is at endQpc - (kept - j) / rate.
	std::vector<int16_t> pcm;
	unsigned rate = 0;
	long long audioStartT = 0;
	{
		AcquireSRWLockShared(&sAudioLock);
		rate = sAudioRate;
		if (rate > 0 && sAudioTotal > 0) {
			const size_t kept   = (size_t)(sAudioTotal < sAudioCap ? sAudioTotal : sAudioCap);
			const long long end = to100ns(sAudioEndQpc);
			// skip frames before t0
			long long skip = (t0 - (end - (long long)kept * 10000000ll / rate)) * (long long)rate / 10000000ll;
			if (skip < 0)
				skip = 0;
			if ((size_t)skip < kept) {
				const size_t n = kept - (size_t)skip;
				pcm.resize(n * 2);
				size_t start = (sAudioPos + sAudioCap - kept + (size_t)skip) % sAudioCap;
				for (size_t i = 0; i < n; i++) {
					const size_t s = (start + i) % sAudioCap;
					pcm[2 * i]     = sAudio[2 * s];
					pcm[2 * i + 1] = sAudio[2 * s + 1];
				}
				audioStartT = end - (long long)n * 10000000ll / rate;
			}
		}
		ReleaseSRWLockShared(&sAudioLock);
	}
	// Resample to 48 kHz (linear) and align at t0.
	std::vector<int16_t> pcm48;
	if (!pcm.empty() && rate > 0) {
		const size_t inN  = pcm.size() / 2;
		const size_t outN = (size_t)((double)inN * kAudioOutRate / rate);
		pcm48.resize(outN * 2);
		for (size_t i = 0; i < outN; i++) {
			const double x = (double)i * rate / kAudioOutRate;
			size_t a       = (size_t)x;
			if (a + 1 >= inN)
				a = inN >= 2 ? inN - 2 : 0;
			const double fr = x - (double)a;
			for (int c = 0; c < 2; c++) {
				const double v   = pcm[2 * a + c] * (1.0 - fr) + pcm[2 * (a + 1 < inN ? a + 1 : a) + c] * fr;
				pcm48[2 * i + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			}
		}
	}

	wchar_t wpath[MAX_PATH];
	MultiByteToWideChar(CP_UTF8, 0, req->path, -1, wpath, MAX_PATH);
	IMFAttributes* attr = nullptr;
	MFCreateAttributes(&attr, 2);
	attr->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
	attr->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
	IMFSinkWriter* sw = nullptr;
	HRESULT hr        = MFCreateSinkWriterFromURL(wpath, nullptr, attr, &sw);
	release(attr);
	if (FAILED(hr) || sw == nullptr) {
		std::snprintf(req->result, sizeof(req->result), "cannot create %s (0x%08lx)", req->path, hr);
		return;
	}
	IMFMediaType* vt = nullptr;
	MFCreateMediaType(&vt);
	sOutType->CopyAllItems(vt);
	{
		const std::vector<uint8_t> sh = sequenceHeader(sRing[first].data);
		if (!sh.empty())
			vt->SetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sh.data(), (UINT32)sh.size());
	}
	DWORD vs = 0, as = 0;
	hr = sw->AddStream(vt, &vs);
	if (SUCCEEDED(hr))
		hr = sw->SetInputMediaType(vs, vt, nullptr);
	release(vt);
	if (FAILED(hr)) {
		std::snprintf(req->result, sizeof(req->result), "video stream refused (0x%08lx)", hr);
		release(sw);
		DeleteFileA(req->path);
		return;
	}
	bool audio = !pcm48.empty();
	if (audio) {
		IMFMediaType *ao = nullptr, *ai = nullptr;
		MFCreateMediaType(&ao);
		ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
		ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
		ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioOutRate);
		ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000); // 128 kbit/s
		ao->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
		ao->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
		MFCreateMediaType(&ai);
		ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
		ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioOutRate);
		ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
		ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioOutRate * 4);
		ai->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
		hr = sw->AddStream(ao, &as);
		if (SUCCEEDED(hr))
			hr = sw->SetInputMediaType(as, ai, nullptr);
		release(ao);
		release(ai);
		if (FAILED(hr)) {
			std::fprintf(stderr, "[port] bugreport: AAC stream refused (0x%08lx): video only\n", hr);
			audio = false;
		}
	}
	hr = sw->BeginWriting();
	if (FAILED(hr)) {
		std::snprintf(req->result, sizeof(req->result), "BeginWriting failed (0x%08lx)", hr);
		release(sw);
		DeleteFileA(req->path);
		return;
	}
	// Interleave: audio in 100 ms chunks up to each video sample's time.
	const size_t chunk = kAudioOutRate / 10;
	size_t aPos        = 0; // frames of pcm48 written
	bool ok            = true;
	unsigned vWritten  = 0;
	for (size_t i = first; i < sRing.size() && ok; i++) {
		const EncSample& s = sRing[i];
		while (audio && aPos < pcm48.size() / 2 && audioStartT + (long long)aPos * 10000000ll / kAudioOutRate < s.t) {
			size_t n = pcm48.size() / 2 - aPos;
			if (n > chunk)
				n = chunk;
			const long long at = audioStartT + (long long)aPos * 10000000ll / kAudioOutRate - t0;
			if (at >= 0)
				ok = writeSample(sw, as, (const uint8_t*)&pcm48[2 * aPos], n * 4, at, (long long)n * 10000000ll / kAudioOutRate,
				                 true);
			aPos += n;
		}
		if (ok) {
			ok = writeSample(sw, vs, s.data.data(), s.data.size(), s.t - t0, s.dur, s.key);
			++vWritten;
		}
	}
	while (ok && audio && aPos < pcm48.size() / 2) {
		size_t n = pcm48.size() / 2 - aPos;
		if (n > chunk)
			n = chunk;
		const long long at = audioStartT + (long long)aPos * 10000000ll / kAudioOutRate - t0;
		if (at > endT - t0)
			break;
		if (at >= 0)
			ok = writeSample(sw, as, (const uint8_t*)&pcm48[2 * aPos], n * 4, at, (long long)n * 10000000ll / kAudioOutRate, true);
		aPos += n;
	}
	hr = sw->Finalize();
	release(sw);
	req->ok = ok && SUCCEEDED(hr);
	std::snprintf(req->result, sizeof(req->result), "%s: %u frames, %.1f s%s%s", req->ok ? "video.mp4 written" : "video.mp4 FAILED",
	              vWritten, (endT - t0) / 1e7, audio ? ", with audio" : ", no audio",
	              req->ok ? "" : " (write/finalize error)");
}

DWORD WINAPI encoderThread(LPVOID)
{
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	MFStartup(MF_VERSION, MFSTARTUP_LITE);
	gpuName();
	for (;;) {
		WaitForSingleObject(sWork, 1000);
		for (;;) {
			RawFrame* f = nullptr;
			EnterCriticalSection(&sFrameLock);
			if (!sPending.empty()) {
				f = sPending.front();
				sPending.pop_front();
			}
			LeaveCriticalSection(&sFrameLock);
			if (f == nullptr)
				break;
			if (!sEncTried)
				sEncOk = encoderInit();
			encodeFrame(*f);
			EnterCriticalSection(&sFrameLock);
			sFree.push_back(f);
			LeaveCriticalSection(&sFrameLock);
		}
		if (SaveReq* req = (SaveReq*)InterlockedExchangePointer((PVOID volatile*)&sSaveReq, nullptr)) {
			mux(req);
			std::fprintf(stderr, "[port] bugreport: %s\n", req->result);
			SetEvent(req->done);
		}
	}
}

// ---- frame-rate window (main thread) ------------------------------------------------------
long long sPresentTimes[512];
unsigned sPresentHead;

double recentFps(double seconds)
{
	const long long now = qpcNow();
	const long long lim = now - (long long)(seconds * sQpcFreq.QuadPart);
	int n               = 0;
	long long oldest    = now;
	for (unsigned i = 0; i < 512; i++) {
		const long long t = sPresentTimes[i];
		if (t != 0 && t >= lim) {
			++n;
			if (t < oldest)
				oldest = t;
		}
	}
	const double span = (now - oldest) / (double)sQpcFreq.QuadPart;
	return span > 0.5 ? n / span : 0.0;
}

void windowsVersion(char* out, size_t cap)
{
	typedef LONG(WINAPI * RtlGetVersionFn)(PRTL_OSVERSIONINFOW);
	RTL_OSVERSIONINFOW v = { sizeof(v) };
	if (HMODULE nt = GetModuleHandleA("ntdll.dll"))
		if (RtlGetVersionFn fn = (RtlGetVersionFn)GetProcAddress(nt, "RtlGetVersion"))
			fn(&v);
	std::snprintf(out, cap, "Windows %lu.%lu build %lu", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
}

bool safeGameState(char* out, size_t cap)
{
	__try {
		port_bugreport_game_state(out, cap);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

// log.txt
void writeLog(const std::string& dir, const char* why, const char* crashText, const char* videoResult)
{
	FILE* f = std::fopen((dir + "\\log.txt").c_str(), "wb");
	if (f == nullptr)
		return;
	char date[64], win[96];
	time_t now = time(nullptr);
	strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&now));
	windowsVersion(win, sizeof(win));
	const PortSettings& s = gPortSettings;
	int ww = 0, wh = 0;
	if (SDL_Window* w = aurora_get_window())
		SDL_GetWindowSize(w, &ww, &wh);
	std::fprintf(f, "Super Mario Sunshine PC port: bug report (%s)\n", why);
	std::fprintf(f, "saved: %s\nport build: %s %s\n%s\n", date, __DATE__, __TIME__, win);
	std::fprintf(f, "GPU: %s; %s\n", sGpuName, port_gfx_backend_status());
	std::fprintf(f, "window: %dx%d%s%s, aspect mode %d, internal scale %.2f (0 = window size), frame rate setting %d\n",
	             ww, wh, s.fullscreen ? " fullscreen" : "", s.maximized ? " maximized" : "", s.aspect, s.internalScale,
	             s.frameRate);
	std::fprintf(f, "settings: msaa %d, anisotropy %d, trilinear %d, fxaa %d, vsync %d, backend %d, behaviour %d, "
	                "audio output %d, keyboard %d, mouse %d, record %d\n",
	             s.msaa, s.anisotropy, s.trilinear, s.fxaa, s.vsync, s.backend, s.behaviour, s.audioOutput, s.keyboard,
	             s.mouse, s.record);
	std::fprintf(f, "frame: %u presented; %.1f fps over the last 5 s, %.1f over the last 20 s\n", gPortFrameCount,
	             recentFps(5.0), recentFps(20.0));
	{
		char gs[1024] = "";
		if (!safeGameState(gs, sizeof(gs)))
			std::snprintf(gs, sizeof(gs), "game state: unreadable (exception)\n");
		std::fputs(gs, f);
	}
	std::fprintf(f, "capture: %dx%d at %d fps, %u frames encoded (%u keyframes), %u dropped; audio %u Hz; %s\n", sWidth,
	             sHeight, sFps, sEncoded, sKeyframes, sDropped, sAudioRate, videoResult);
	if (crashText != nullptr && *crashText != '\0')
		std::fprintf(f, "\n---- crash ----\n%s\n", crashText);
	std::fprintf(f, "\n---- last %d s of log (time, presented frame) ----\n", sSeconds);
	// The crash path may run while a tee thread holds the lock: bounded wait, then give up.
	bool locked = false;
	for (int i = 0; i < 100 && !(locked = TryEnterCriticalSection(&sLogLock) != 0); i++)
		Sleep(5);
	if (locked) {
		for (const LogLine& l : sLog) {
			std::fwrite(l.text.data(), 1, l.text.size(), f);
			std::fputc('\n', f);
		}
		LeaveCriticalSection(&sLogLock);
	} else
		std::fprintf(f, "(log ring busy)\n");
	std::fclose(f);
}

bool save(const char* why, const char* crashText, int waitMs)
{
	if (!sEnabled)
		return false;
	if (InterlockedExchange(&sSaving, 1))
		return false; // one at a time
	char name[64];
	time_t now = time(nullptr);
	strftime(name, sizeof(name), "%Y-%m-%d_%H-%M-%S", localtime(&now));
	const std::string root = exeDir() + "\\bug_reports";
	CreateDirectoryA(root.c_str(), nullptr);
	const std::string dir = root + "\\" + name + "_" + why;
	if (!CreateDirectoryA(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
		std::fprintf(stderr, "[port] bugreport: cannot create %s\n", dir.c_str());
		InterlockedExchange(&sSaving, 0);
		return false;
	}
	std::fprintf(stderr, "[port] bugreport: saving (%s) to %s\n", why, dir.c_str());
	// The video: a job for the encoder thread (not when that thread is the one crashing).
	SaveReq* req     = new SaveReq {};
	bool video       = sEncThread != nullptr && GetCurrentThreadId() != sEncThreadId;
	req->done        = CreateEventA(nullptr, TRUE, FALSE, nullptr);
	std::snprintf(req->path, sizeof(req->path), "%s\\video.mp4", dir.c_str());
	std::snprintf(req->result, sizeof(req->result), "video.mp4 being written in the background");
	if (video) {
		sSaveReq = req;
		SetEvent(sWork);
	} else
		std::snprintf(req->result, sizeof(req->result), "no video (crash on the capture thread)");
	port_replay_save_dir(why, dir.c_str());
	if (video && waitMs > 0) {
		if (WaitForSingleObject(req->done, (DWORD)waitMs) != WAIT_OBJECT_0)
			std::snprintf(req->result, sizeof(req->result), "video.mp4 not finished within %d ms", waitMs);
	}
	writeLog(dir, why, crashText, req->result);
	std::fprintf(stderr, "[port] bugreport: saved %s\\log.txt\n", dir.c_str());
	if (!video || waitMs > 0) { // the request is over
		CloseHandle(req->done);
		delete req;
	} else { // leaks one small struct per report in the rare case the mux never completes
		CloseHandle(req->done);
		req->done = nullptr;
	}
	InterlockedExchange(&sSaving, 0);
	return true;
}

} // namespace

extern "C" {

void port_bugreport_init(void)
{
	bool on = gPortSettings.bugReport;
	if (const char* e = std::getenv("SMS_PORT_BUGREPORT"))
		on = *e != '\0' && *e != '0';
	if (!on || std::getenv("SMS_PORT_NULLGFX") != nullptr)
		return;
	QueryPerformanceFrequency(&sQpcFreq);
	sQpc0    = qpcNow();
	sSeconds = gPortSettings.bugReportSeconds;
	sWidth   = gPortSettings.bugReportWidth & ~1;
	sHeight  = gPortSettings.bugReportHeight & ~1;
	sFps     = gPortSettings.bugReportFps;
	InitializeCriticalSection(&sLogLock);
	InitializeCriticalSection(&sFrameLock);
	sWork      = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	sEncThread = CreateThread(nullptr, 0, encoderThread, nullptr, 0, &sEncThreadId);
	teeInstall(1, STD_OUTPUT_HANDLE);
	teeInstall(2, STD_ERROR_HANDLE);
	sEnabled = true;
	std::fprintf(stderr, "[port] bugreport: on (F9 or the settings menu saves the last %d s; a crash saves them too)\n",
	             sSeconds);
}

bool port_bugreport_enabled(void) { return sEnabled; }

void port_bugreport_frame(void)
{
	if (!sEnabled)
		return;
	const long long now = qpcNow();
	sPresentTimes[sPresentHead++ & 511] = now;
	// test knobs
	static const unsigned sTestF9 = std::getenv("SMS_PORT_TEST_F9") ? (unsigned)std::atoi(std::getenv("SMS_PORT_TEST_F9")) : 0;
	static const unsigned sTestCrash =
	    std::getenv("SMS_PORT_TEST_CRASH") ? (unsigned)std::atoi(std::getenv("SMS_PORT_TEST_CRASH")) : 0;
	static bool sTestF9Done;
	if (sTestF9 != 0 && !sTestF9Done && gPortFrameCount >= sTestF9) {
		sTestF9Done = true;
		port_bugreport_save("f9", nullptr);
	}
	if (sTestF9 != 0 && gPortFrameCount >= sTestF9 + 300) {
		std::fprintf(stderr, "[port] bugreport: SMS_PORT_TEST_F9 done, exiting\n");
		std::fflush(stderr);
		Sleep(300); // the tee threads
		TerminateProcess(GetCurrentProcess(), 0);
	}
	if (sTestCrash != 0 && gPortFrameCount >= sTestCrash) {
		std::fprintf(stderr, "[port] bugreport: SMS_PORT_TEST_CRASH: crashing now\n");
		*(volatile int*)0 = 1;
	}
	// arm the next capture at the configured rate; the readbacks complete asynchronously and
	// may overlap (a capture armed for a frame that is never presented is dropped by Aurora)
	if (now - sLastArm < (long long)(sQpcFreq.QuadPart * 0.9 / sFps))
		return;
	sLastArm = now;
	port_bugreport_arm_capture();
}

void port_bugreport_capture_dropped(void) {}

void port_bugreport_push_frame(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t stride, bool bgra, long long)
{
	if (!sEnabled || w == 0 || h == 0)
		return;
	if (sHeight == 0) { // keep the picture's shape (decided by the first frame)
		int hh = (int)((unsigned long long)sWidth * h / w) & ~1;
		sHeight = hh < 2 ? 2 : hh;
	}
	RawFrame* f = nullptr;
	EnterCriticalSection(&sFrameLock);
	if (sPending.size() >= 3) {
		++sDropped; // the encoder is behind: skip this picture
	} else if (!sFree.empty()) {
		f = sFree.back();
		sFree.pop_back();
	} else
		f = new RawFrame;
	LeaveCriticalSection(&sFrameLock);
	if (f == nullptr)
		return;
	const int dw = sWidth, dh = sHeight;
	f->nv12.resize((size_t)dw * dh * 3 / 2);
	f->qpc = qpcNow();
	uint8_t* Y  = f->nv12.data();
	uint8_t* UV = Y + (size_t)dw * dh;
	static std::vector<uint32_t> sx;
	if (sx.size() != (size_t)dw * 2 || sx[0] != w) { // (x offsets in bytes, cached per source width)
		sx.assign((size_t)dw * 2, 0);
		sx[0] = w;
		for (int x = 0; x < dw; x++)
			sx[(size_t)dw + x] = (uint32_t)((unsigned long long)x * w / dw) * 4;
	}
	const uint32_t* xo = sx.data() + dw;
	const int ri = bgra ? 2 : 0, bi = bgra ? 0 : 2;
	for (int y = 0; y < dh; y++) {
		const uint8_t* row = pixels + (size_t)((unsigned long long)y * h / dh) * stride;
		uint8_t* yo        = Y + (size_t)y * dw;
		uint8_t* uvo       = UV + (size_t)(y / 2) * dw;
		for (int x = 0; x < dw; x++) {
			const uint8_t* p = row + xo[x];
			const int r = p[ri], g = p[1], b = p[bi];
			yo[x] = (uint8_t)(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
			if ((y & 1) == 0 && (x & 1) == 0) {
				uvo[x]     = (uint8_t)(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
				uvo[x + 1] = (uint8_t)(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
			}
		}
	}
	EnterCriticalSection(&sFrameLock);
	sPending.push_back(f);
	LeaveCriticalSection(&sFrameLock);
	SetEvent(sWork);
}

void port_bugreport_audio(const int16_t* lr, uint32_t frames, uint32_t rate)
{
	if (!sEnabled || rate == 0)
		return;
	AcquireSRWLockExclusive(&sAudioLock);
	if (sAudioRate != rate || sAudio.empty()) {
		sAudioRate  = rate;
		sAudioCap   = (size_t)rate * (size_t)(sSeconds + 2);
		sAudio.assign(sAudioCap * 2, 0);
		sAudioPos   = 0;
		sAudioTotal = 0;
	}
	for (uint32_t i = 0; i < frames; i++) {
		sAudio[2 * sAudioPos]     = lr[2 * i];
		sAudio[2 * sAudioPos + 1] = lr[2 * i + 1];
		sAudioPos                 = (sAudioPos + 1) % sAudioCap;
	}
	sAudioTotal += frames;
	sAudioEndQpc = qpcNow();
	ReleaseSRWLockExclusive(&sAudioLock);
}

bool port_bugreport_save(const char* why, const char* crashText)
{
	const bool ok = save(why, crashText, 0);
	if (ok) {
		char msg[200];
		std::snprintf(msg, sizeof(msg), "Bug report saved to bug_reports\\ (%s)", why);
		port_qol_toast(msg);
	}
	return ok;
}

void port_bugreport_crash(const char* crashText)
{
	static volatile LONG once;
	if (!sEnabled || InterlockedExchange(&once, 1))
		return;
	port_crash_log("[port] bugreport: crash: saving a report\n");
	std::fflush(stdout);
	std::fflush(stderr);
	Sleep(150); // let the tee threads store the crash lines
	save("crash", crashText, kCrashWaitMs);
	std::fflush(stderr);
	Sleep(100);
}

} // extern "C"

#endif // SMS_PORT_AURORA
