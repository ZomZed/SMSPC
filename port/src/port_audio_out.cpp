// Audio output for the PC port: the AI DMA buffers go to an SDL3 audio stream
// (Aurora links SDL3), optionally also to a WAV file, plus a second PCM stream
// for host-decoded audio (port_audio_submit_stream, for THP movie sound).
//
// Knobs:
//   SMS_PORT_AUDIO=0       no audio device (the game still runs its audio frame loop)
//   SMS_PORT_WAV=<path>    also write everything the AI plays to <path> (16-bit WAV at the
//                          AI rate; 6 / 8 channels when 5.1 / 7.1 output is chosen)
//   SMS_PORT_AUDIO_FORCE_MC=1  open a 6 / 8-channel stream even if the device reports fewer
//                          channels (SDL downmixes; for testing the multichannel path)
//   SMS_PORT_AUDIO_LOG=1   periodic queue/underrun statistics
//   SMS_PORT_AUDIO_MUTE=1  open the device at zero gain (tests: real queue timing, no sound)
//
// Clock: the AI DMA interrupt (sdk_audio.cpp) stays the game's audio clock. Each
// interrupt pushes one buffer here; port_audio_out_next_period() trims the next
// interrupt period by up to +-2 % so the device queue stays around 3 buffers,
// absorbing the drift between the host timer and the sound card.

#include "port_audio.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef SMS_PORT_AURORA
#include <SDL3/SDL.h>
extern "C" void port_bugreport_audio(const int16_t* lr, uint32_t frames, uint32_t rate); // port_bugreport.cpp (w12)
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

SRWLOCK gLock = SRWLOCK_INIT;
bool gInitDone;
bool gLog;

// ---- WAV capture ------------------------------------------------------------------
FILE* gWav;
unsigned long long gWavBytes;
unsigned gWavRate;
unsigned gWavPatchCounter;
unsigned gWavCh = 2;

void wavPatchHeader()
{
	if (!gWav)
		return;
	long pos   = std::ftell(gWav);
	unsigned d = (unsigned)gWavBytes;
	unsigned r = d + 36;
	std::fseek(gWav, 4, SEEK_SET);
	std::fwrite(&r, 4, 1, gWav);
	std::fseek(gWav, 40, SEEK_SET);
	std::fwrite(&d, 4, 1, gWav);
	std::fseek(gWav, pos, SEEK_SET);
	std::fflush(gWav);
}

void wavOpen(const char* path, unsigned rate, unsigned ch)
{
	gWav = std::fopen(path, "wb");
	if (!gWav) {
		std::fprintf(stderr, "[port] audio: cannot write %s\n", path);
		return;
	}
	gWavRate = rate;
	gWavCh              = ch;
	unsigned char h[44] = { 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
		                    16, 0, 0, 0, 1, 0, (unsigned char)ch, 0 };
	unsigned byteRate = rate * 2 * ch;
	std::memcpy(h + 24, &rate, 4);
	std::memcpy(h + 28, &byteRate, 4);
	h[32] = (unsigned char)(2 * ch);
	h[34] = 16;
	std::memcpy(h + 36, "data", 4);
	std::fwrite(h, 1, 44, gWav);
	std::fprintf(stderr, "[port] audio: recording to %s (%u channels)\n", path, ch);
	std::atexit([] {
		AcquireSRWLockExclusive(&gLock);
		wavPatchHeader();
		if (gWav)
			std::fclose(gWav);
		gWav = nullptr;
		ReleaseSRWLockExclusive(&gLock);
	});
}

// ---- Output mode ------------------------------------------------------------------
volatile int gMode   = PORT_AUDIO_ORIGINAL; // PORT_AUDIO_*
volatile int gWantCh = 2;                   // 6 / 8 for 5.1 / 7.1
int gOutCh           = 2;                   // channels of the open device stream
bool gReopen;                               // the mode changed: reopen at the next buffer
char gStatus[160]    = "Original: the game's Sound option (stereo / Pro Logic II matrix)";

// Discrete channels paired with AI buffers (port_audio_mc_bind), 6 per frame:
// FL, FR, FC, LFE, RL, RR.
const int kBound = 4;
struct McBound {
	const int16_t* dac;
	uint32_t frames;
	int16_t data[4096 * 6];
};
McBound gBoundSlots[kBound];
int gBoundNext;
SRWLOCK gBoundLock = SRWLOCK_INIT;

inline int16_t sat16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

// One 6-channel frame -> the device / WAV layout (SDL order: FL FR FC LFE BL BR [SL SR]).
// 7.1: the rear pair is split equally (-3 dB each) between back and side.
void layoutFrame(const int16_t* s6, int ch, int16_t* out)
{
	if (ch == 2) {
		out[0] = s6[0];
		out[1] = s6[1];
		return;
	}
	out[0] = s6[0];
	out[1] = s6[1];
	out[2] = s6[2];
	out[3] = s6[3];
	if (ch == 6) {
		out[4] = s6[4];
		out[5] = s6[5];
	} else {
		int16_t l = (int16_t)(s6[4] * 23170 / 32768), r = (int16_t)(s6[5] * 23170 / 32768);
		out[4] = l;
		out[5] = r;
		out[6] = l;
		out[7] = r;
	}
}

// ---- SDL --------------------------------------------------------------------------
#ifdef SMS_PORT_AURORA
SDL_AudioStream* gMain;
SDL_AudioStream* gExtra;
int gExtraRate;
SDL_AudioDeviceID gDev;
bool gSdlOk;
#endif
unsigned gRate = 32000;
unsigned long long gBuffers;
unsigned gUnderruns;
unsigned gDrops;
int gQueuedFrames = -1; // last measurement

#ifdef SMS_PORT_AURORA
void openStream()
{
	if (gExtra) {
		SDL_DestroyAudioStream(gExtra);
		gExtra     = nullptr;
		gExtraRate = 0;
	}
	if (gMain) {
		SDL_DestroyAudioStream(gMain);
		gMain = nullptr;
		gDev  = 0;
	}
	int want = gWantCh;
	SDL_AudioSpec dev = {};
	int frames        = 0;
	SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, &frames);
	const char* force = std::getenv("SMS_PORT_AUDIO_FORCE_MC");
	const bool forced = force && *force && *force != '0';
	gOutCh            = 2;
	if (want > 2) {
		if (dev.channels >= want || forced) {
			gOutCh = want;
			std::snprintf(gStatus, sizeof(gStatus), "%s: %d discrete channels%s", want == 6 ? "5.1" : "7.1", want,
			              dev.channels >= want ? "" : " (forced; the device has fewer, SDL downmixes)");
		} else {
			std::snprintf(gStatus, sizeof(gStatus),
			              "%s chosen, but the output device reports %d channels: stereo fallback (the WAV capture "
			              "keeps %d channels)",
			              want == 6 ? "5.1" : "7.1", dev.channels, want);
		}
	} else if (gMode == PORT_AUDIO_STEREO) {
		std::snprintf(gStatus, sizeof(gStatus), "Stereo (the game mixes in stereo)");
	} else {
		std::snprintf(gStatus, sizeof(gStatus), "Original: the game's Sound option (stereo / Pro Logic II matrix)");
	}
	SDL_AudioSpec spec = { SDL_AUDIO_S16, gOutCh, (int)gRate };
	gMain              = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
	if (!gMain) {
		std::fprintf(stderr, "[port] audio: no audio device: %s\n", SDL_GetError());
		std::snprintf(gStatus, sizeof(gStatus), "No audio device: %s", SDL_GetError());
		return;
	}
	gDev = SDL_GetAudioStreamDevice(gMain);
	if (std::getenv("SMS_PORT_AUDIO_MUTE") != nullptr) // tests: the device runs (queue statistics) but plays silence
		SDL_SetAudioStreamGain(gMain, 0.0f);
	SDL_ResumeAudioStreamDevice(gMain);
	SDL_GetAudioDeviceFormat(gDev, &dev, &frames);
	std::fprintf(stderr, "[port] audio: SDL output %s, device %d Hz %d ch, %d-frame buffer; game rate %u Hz, stream %d ch\n",
	             SDL_GetCurrentAudioDriver(), dev.freq, dev.channels, frames, gRate, gOutCh);
	std::fprintf(stderr, "[port] audio: output mode: %s\n", gStatus);
}
#endif

void initOnce(unsigned rate)
{
	if (gInitDone)
		return;
	gInitDone = true;
	gRate     = rate;
	gLog      = std::getenv("SMS_PORT_AUDIO_LOG") != nullptr;
	if (const char* w = std::getenv("SMS_PORT_WAV"))
		if (*w)
			wavOpen(w, rate, (unsigned)gWantCh);
#ifdef SMS_PORT_AURORA
	const char* en = std::getenv("SMS_PORT_AUDIO");
	if (en && en[0] == '0') {
		std::fprintf(stderr, "[port] audio: output disabled (SMS_PORT_AUDIO=0)\n");
		return;
	}
	if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
		std::fprintf(stderr, "[port] audio: SDL audio init failed: %s\n", SDL_GetError());
		return;
	}
	gSdlOk  = true;
	gReopen = false;
	openStream();
#endif
}

// Extra-stream frames (stereo) -> gOutCh channels, FL/FR only.
void upmixStereo(const int16_t* lr, int frames, int ch, int16_t* out)
{
	for (int i = 0; i < frames; ++i) {
		int16_t* o = out + i * ch;
		for (int c = 0; c < ch; ++c)
			o[c] = 0;
		o[0] = lr[2 * i];
		o[1] = lr[2 * i + 1];
	}
}

} // namespace

// JAIGlobalParameter.cpp (SMS_PORT): re-applies the game's last Sound option through
// port_audio_game_output_mode when the output mode changes.
extern "C" void port_jai_reapply_output_mode(void);

extern "C" {

void port_audio_set_output_mode(int mode)
{
	if (mode < PORT_AUDIO_STEREO || mode > PORT_AUDIO_71)
		mode = PORT_AUDIO_ORIGINAL;
	AcquireSRWLockExclusive(&gLock);
	const bool changed = gMode != mode || !gInitDone;
	gMode              = mode;
	gWantCh            = mode == PORT_AUDIO_51 ? 6 : mode == PORT_AUDIO_71 ? 8 : 2;
	if (gInitDone && changed)
		gReopen = true;
	if (!gInitDone)
		std::snprintf(gStatus, sizeof(gStatus), "%s (starts with the game)",
		              mode == PORT_AUDIO_STEREO ? "Stereo" : mode == PORT_AUDIO_51 ? "5.1" : mode == PORT_AUDIO_71 ? "7.1" : "Original");
	ReleaseSRWLockExclusive(&gLock);
	if (changed)
		port_jai_reapply_output_mode();
}

const char* port_audio_output_status(void) { return gStatus; }

int port_audio_mc_channels(void) { return gWantCh; }

int port_audio_game_output_mode(int gameMode)
{
	switch (gMode) {
	case PORT_AUDIO_STEREO:
		return gameMode == 0 ? 0 : 1; // mono stays mono
	case PORT_AUDIO_51:
	case PORT_AUDIO_71:
		return 2; // the game must feed the back buses
	default:
		return gameMode;
	}
}

void port_audio_mc_bind(const int16_t* dspL, const int16_t* dspR, const int16_t* dac, uint32_t frames)
{
	if (gWantCh <= 2 || dac == nullptr || frames == 0 || frames > 4096)
		return;
	uint32_t have        = 0;
	const int16_t* s6    = port_dsp_mc_shadow(dspL, &have);
	AcquireSRWLockExclusive(&gBoundLock);
	// JAudio rotates three DAC buffers. Update an existing binding in place:
	// adding another slot for the same address lets playback find an old copy.
	McBound* pick = nullptr;
	for (McBound& slot : gBoundSlots)
		if (slot.dac == dac) {
			pick = &slot;
			break;
		}
	if (!pick) {
		pick = &gBoundSlots[gBoundNext];
		gBoundNext = (gBoundNext + 1) % kBound;
	}
	McBound& b = *pick;
	b.dac      = dac;
	b.frames   = frames;
	for (uint32_t i = 0; i < frames; ++i) {
		int16_t* o = b.data + i * 6;
		if (s6 != nullptr && i < have) {
			for (int c = 0; c < 6; ++c)
				o[c] = s6[i * 6 + c];
		} else {
			for (int c = 0; c < 6; ++c)
				o[c] = 0;
			o[0] = dspL ? dspL[i] : 0;
			o[1] = dspR ? dspR[i] : 0;
		}
		// The extra mix (THP movie sound) the AI buffer got on top of the DSP output
		// goes to the fronts. AI order is (R, L).
		if (dspL && dspR) {
			o[0] = sat16(o[0] + (dac[2 * i + 1] - dspL[i]));
			o[1] = sat16(o[1] + (dac[2 * i] - dspR[i]));
		}
	}
	ReleaseSRWLockExclusive(&gBoundLock);
}

double port_audio_out_wav_seconds(void)
{
	return gWavRate ? (double)(gWavBytes / (2ull * gWavCh)) / gWavRate : 0.0;
}

void port_audio_out_ai(const int16_t* rl, uint32_t frames, uint32_t rate)
{
	if (frames == 0 || frames > 4096 || rl == nullptr)
		return;
	AcquireSRWLockExclusive(&gLock);
	initOnce(rate);
#ifdef SMS_PORT_AURORA
	if (gReopen && gSdlOk) {
		gReopen = false;
		openStream();
	}
#endif
	// AI order is (right, left); SDL and WAV want (left, right).
	static int16_t lr[4096 * 2];
	for (uint32_t i = 0; i < frames; ++i) {
		lr[2 * i]     = rl[2 * i + 1];
		lr[2 * i + 1] = rl[2 * i];
	}
#ifdef SMS_PORT_AURORA
	port_bugreport_audio(lr, frames, rate); // w12: the bug-report capture's sound ring (also without a device)
#endif
	// 6-channel frames for this buffer (5.1 / 7.1), else the stereo in FL/FR.
	static int16_t s6[4096 * 6];
	const bool mc = gWantCh > 2;
	if (mc) {
		bool found = false;
		AcquireSRWLockShared(&gBoundLock);
		for (McBound& b : gBoundSlots)
			if (b.dac == rl && b.frames >= frames) {
				std::memcpy(s6, b.data, frames * 6 * sizeof(int16_t));
				found = true;
				break;
			}
		ReleaseSRWLockShared(&gBoundLock);
		if (!found)
			for (uint32_t i = 0; i < frames; ++i) {
				int16_t* o = s6 + i * 6;
				o[0]       = lr[2 * i];
				o[1]       = lr[2 * i + 1];
				o[2] = o[3] = o[4] = o[5] = 0;
			}
	}
	static int16_t out[4096 * 8];
	if (gWav) {
		if (gWavCh == 2 || !mc) {
			if (gWavCh == 2)
				std::fwrite(lr, 4, frames, gWav);
			else {
				upmixStereo(lr, (int)frames, (int)gWavCh, out);
				std::fwrite(out, 2 * gWavCh, frames, gWav);
			}
		} else {
			for (uint32_t i = 0; i < frames; ++i)
				layoutFrame(s6 + i * 6, (int)gWavCh, out + i * gWavCh);
			std::fwrite(out, 2 * gWavCh, frames, gWav);
		}
		gWavBytes += frames * 2ull * gWavCh;
		if (++gWavPatchCounter % 64 == 0)
			wavPatchHeader();
	}
	++gBuffers;
#ifdef SMS_PORT_AURORA
	if (gMain) {
		const int fb = 2 * gOutCh;
		int queued   = SDL_GetAudioStreamQueued(gMain) / fb;
		if (queued == 0 && gBuffers > 16)
			++gUnderruns;
		// Far behind (the game stalled, e.g. a long load): drop the backlog so the
		// latency does not stay high.
		if (queued > (int)(gRate / 4)) {
			SDL_ClearAudioStream(gMain);
			++gDrops;
			queued = 0;
		}
		if (gOutCh == 2) {
			SDL_PutAudioStreamData(gMain, lr, (int)(frames * 4));
		} else {
			for (uint32_t i = 0; i < frames; ++i)
				layoutFrame(s6 + i * 6, gOutCh, out + i * gOutCh);
			SDL_PutAudioStreamData(gMain, out, (int)(frames * fb));
		}
		gQueuedFrames = queued + (int)frames;
		if (gLog && gBuffers % 600 == 0)
			std::fprintf(stderr, "[port] audio: %llu buffers, queue %d frames, %u underruns, %u drops (%d ch)\n",
			             gBuffers, gQueuedFrames, gUnderruns, gDrops, gOutCh);
	}
#endif
	ReleaseSRWLockExclusive(&gLock);
}

long long port_audio_out_next_period(long long nominalNs)
{
	if (gQueuedFrames < 0)
		return nominalNs;
	const int target = (int)(gRate * 52 / 1000); // ~3 DMA buffers
	double err       = (double)(gQueuedFrames - target) / target;
	if (err > 1.0)
		err = 1.0;
	if (err < -1.0)
		err = -1.0;
	// Quantised to 0.5 % steps: the interrupt slot is only reprogrammed (which
	// restarts its phase) when the step changes, not on every buffer.
	int step = (int)(err * 4.0 + (err < 0 ? -0.5 : 0.5));
	return nominalNs + nominalNs * step / 200;
}

void port_audio_submit_stream(const int16_t* samples, int frames, int rate)
{
#ifdef SMS_PORT_AURORA
	if (samples == nullptr || frames <= 0 || rate <= 0)
		return;
	AcquireSRWLockExclusive(&gLock);
	initOnce(32000);
	if (gDev != 0) {
		// Stereo goes to FL/FR whatever the channel count.
		if (!gExtra || gExtraRate != rate) {
			SDL_AudioSpec src = { SDL_AUDIO_S16, gOutCh, rate };
			if (gExtra) {
				SDL_ClearAudioStream(gExtra);
				SDL_SetAudioStreamFormat(gExtra, &src, nullptr);
			} else {
				SDL_AudioSpec dst = {};
				SDL_GetAudioDeviceFormat(gDev, &dst, nullptr);
				gExtra = SDL_CreateAudioStream(&src, &dst);
				if (gExtra && !SDL_BindAudioStream(gDev, gExtra)) {
					SDL_DestroyAudioStream(gExtra);
					gExtra = nullptr;
				}
			}
			gExtraRate = rate;
		}
		if (gExtra) {
			if (gOutCh == 2) {
				SDL_PutAudioStreamData(gExtra, samples, frames * 4);
			} else {
				static int16_t up[4096 * 8];
				for (int done = 0; done < frames; done += 4096) {
					int n = frames - done < 4096 ? frames - done : 4096;
					upmixStereo(samples + done * 2, n, gOutCh, up);
					SDL_PutAudioStreamData(gExtra, up, n * 2 * gOutCh);
				}
			}
		}
	}
	ReleaseSRWLockExclusive(&gLock);
#else
	(void)samples;
	(void)frames;
	(void)rate;
#endif
}

void port_audio_clear_stream(void)
{
#ifdef SMS_PORT_AURORA
	AcquireSRWLockExclusive(&gLock);
	if (gExtra)
		SDL_ClearAudioStream(gExtra);
	ReleaseSRWLockExclusive(&gLock);
#endif
}

} // extern "C"