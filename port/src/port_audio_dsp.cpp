// Host replacement for JAudio's DSP microcode (the voice renderer).
//
// JAudio programs voices by writing JASystem::DSPInterface::DSPBuffer blocks (one
// per DSP channel, 64 of them) and FXBuffer blocks (4 effect lines) in main RAM,
// then drives the DSP over the mailbox (see sdk_audio.cpp):
//   0x81 DsetupTable(64, voices, resampling filter, ADPCM coefficients, fx lines)
//   0x82 DsyncFrame(subframes, left buffer, right buffer) + output level (4.12)
//   DSPReleaseHalt(map)  -> render one 80-sample sub-frame of the mapped voices
// Every rendered sub-frame lands at its slot of the left/right frame buffers, which
// JASystem::Kernel::vframeWork interleaves into the AI DMA buffer.
//
// The blocks are written by the (natively compiled) game code, so they are read
// here through the game's own struct definitions: native byte order, x64 layout.
// Wave data in ARAM is the raw big-endian .aw content.
//
// What is rendered per voice and sub-frame:
//   * source: AFC ADPCM (9-byte frames of 16 samples, 4-bit; 5-byte frames, 2-bit)
//     with the coefficient table JAudio passes in DsetupTable; PCM8/PCM16 from ARAM;
//     PCM16 from main RAM in a circular buffer (JAI streams, source type 0x21);
//     64-sample oscillator waveforms (sequence "osc" notes);
//   * looping with the stored ADPCM history at the loop start;
//   * pitch: 4.12 step per output sample, resampled with JAudio's own 4-tap
//     polyphase table (DSPRES_FILTER, 64 phases);
//   * distance low-pass (lowPassCoeff), volume ramps per output bus over the
//     sub-frame (or the "auto mixer" pan/volume form), pause, end requests;
//   * four FX lines (delay buffer in main RAM + 8-tap FIR feedback, two sends).
// Written from the data layout and JAudio's usage; no microcode was translated.

#include "port_audio.h"

#include <JSystem/JAudio/JASystem/JASDSPInterface.hpp>
#include <JSystem/JAudio/JASystem/JASDriverTables.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using JASystem::DSPInterface::DSPBuffer;
using JASystem::DSPInterface::FXBuffer;

namespace {

const int kSub      = 80; // samples per sub-frame
const int kVoices   = 64;
const int kFxLines  = 4;
const int kBuses    = 13;

// DSP DRAM addresses JAudio uses as mix-bus ids (setBusConnect / SEND_TABLE),
// plus 0x0C00/0x0C50, the auto mixer's back-left/back-right FX sends.
const u16 kBusIds[kBuses] = { 0x0D00, 0x0D60, 0x0DC0, 0x0E20, 0x0E80, 0x0EE0, 0x0CA0,
	                          0x0F40, 0x0FA0, 0x0B00, 0x09A0, 0x0C00, 0x0C50 };
enum {
	BUS_L      = 0,
	BUS_R      = 1,
	BUS_FX0    = 2,
	BUS_FX2    = 4,
	BUS_FX3    = 5,
	BUS_BACK_R = 6,
	BUS_BACK_L = 7,
	BUS_FA0    = 8,
	BUS_CENTER = 10,
	BUS_C00    = 11,
	BUS_C50    = 12
};
s32 gC00Prev[80];  // bus 0x0C00 of the previous sub-frame
s32 gC50Tail[40];  // last 40 samples of bus 0x0C50 of the previous sub-frame
s32 gCa0Delay[40]; // last 40 samples of bus 0x0CA0 (the microcode mixes it 40 samples late)
s32 gF40Prev[80];  // bus 0x0F40 of the previous sub-frame (mixed one sub-frame late)

int busIndex(u16 id)
{
	for (int i = 0; i < kBuses; ++i)
		if (kBusIds[i] == id)
			return i;
	return -1;
}

struct Voice {
	u32 pos;          // next source sample to fetch
	u32 frac;         // resampler phase, 16 bits
	s16 hist[4];      // resampler input history (oldest first)
	s16 yn1, yn2;     // ADPCM history
	int frame;        // decoded ADPCM frame index, -1 = none
	s16 frameBuf[16];
	bool ended;       // source ran out (non-looping)
	s32 lp;           // low-pass state
	u32 oscPhase;     // oscillator phase, 16.16 over 64 samples
	s16 mixHist[255]; // last voice output samples, for the mixer channel delays
};

DSPBuffer* gVoices;
FXBuffer* gFx;
s16 gRes[64][4];      // resampling filter taps, Q15 (DSPRES_FILTER s16 0-255: 64 phases x 4)
s16 gOsc[4][64];      // oscillator tables (DSPRES_FILTER s16 256-511)
s16 gPanSin[128];     // quarter sine, 0..0x7FFD (DSPRES_FILTER s16 512-639): auto-mixer pan law
s16 gCoef[16][2];     // ADPCM predictor coefficients, Q11
bool gSetup;
Voice gState[kVoices];
u32 gFxPos[kFxLines];
// Each FX line's DSP buffer (bus 0x0DC0 + 0x60 * line): words 0-79 are the bus
// the voices send into, 80-87 hold the raw tail of the last block read from the
// delay line, which becomes the FIR history (words 0-7) of the next sub-frame.
s32 gFxTail[kFxLines][8];
u64 gFxInAbs[kFxLines], gFxOutAbs[kFxLines]; // debug (SMS_PORT_AUDIO_LOG): delay-line input / output level

s16* gOutL;
s16* gOutR;
u32 gSubframes;
u32 gSubIndex;
u32 gLevel = 0x1000;

s32 gBus[kBuses][kSub];

// 5.1 / 7.1: the buses before the Pro Logic II downmix, per output buffer
// (port_audio_mc_bind in port_audio_out.cpp pairs them with the AI buffer):
// FL, FR, FC, LFE, RL, RR (interleaved, already at the output level).
const int kMcSlots     = 4;
const u32 kMcMaxFrames = 80 * 16;
struct McShadow {
	const s16* key;
	u32 frames;
	s16 data[kMcMaxFrames * 6];
};
McShadow gMcSlots[kMcSlots];
int gMcNext;
McShadow* gMc;
// LFE: 2nd-order Butterworth low-pass at 120 Hz (32 kHz), direct form I.
struct Biquad {
	f32 b0, b1, b2, a1, a2;
	f32 x1, x2, y1, y2;
	f32 run(f32 x)
	{
		f32 y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		x2    = x1;
		x1    = x;
		y2    = y1;
		y1    = y;
		return y;
	}
};
Biquad gLfe = [] {
	const f32 fc = 120.0f, fs = 32000.0f, q = 0.70710678f;
	const f32 w0 = 6.28318531f * fc / fs, c = std::cos(w0), al = std::sin(w0) / (2.0f * q);
	const f32 a0 = 1.0f + al;
	Biquad b     = {};
	b.b0         = (1.0f - c) * 0.5f / a0;
	b.b1         = (1.0f - c) / a0;
	b.b2         = (1.0f - c) * 0.5f / a0;
	b.a1         = -2.0f * c / a0;
	b.a2         = (1.0f - al) / a0;
	return b;
}();

bool gLog = std::getenv("SMS_PORT_AUDIO_LOG") != nullptr;

inline s16 clamp16(s32 v)
{
	if (v > 32767)
		return 32767;
	if (v < -32768)
		return -32768;
	return (s16)v;
}

inline s16 f2s16(f32 v)
{
	if (!(v < 32767.0f))
		return v != v ? 0 : 32767;
	if (v < -32768.0f)
		return -32768;
	return (s16)std::lrintf(v);
}

template <class T> T* ramPtr(u32 addr) { return (T*)(uintptr_t)addr; }

// ---- source decoding -------------------------------------------------------------

void decodeAdpcmFrame(Voice& v, const DSPBuffer& b, int frame)
{
	u32 base   = (u32)(uintptr_t)b.baseAddress;
	bool hq    = b.samplesSourceType == 9;
	u32 fbytes = hq ? 9 : 5;
	const u8* p = port_aram_ptr(base + (u32)frame * fbytes);
	int shift   = p[0] >> 4;
	int ci      = p[0] & 0xF;
	s32 c1 = gCoef[ci][0], c2 = gCoef[ci][1];
	for (int i = 0; i < 16; ++i) {
		s32 d;
		if (hq) {
			int n = (p[1 + i / 2] >> ((i & 1) ? 0 : 4)) & 0xF;
			d     = (n ^ 8) - 8;          // signed 4-bit
			d   <<= shift;
		} else {
			int n = (p[1 + i / 4] >> (6 - 2 * (i & 3))) & 3;
			d     = (n ^ 2) - 2;          // signed 2-bit
			d   <<= shift + 2;
		}
		s32 s = d + ((c1 * v.yn1 + c2 * v.yn2) >> 11);
		s16 o = clamp16(s);
		v.yn2 = v.yn1;
		v.yn1 = o;
		v.frameBuf[i] = o;
	}
	v.frame = frame;
}

// Next source sample (one input sample at the source rate).
s16 fetch(Voice& v, DSPBuffer& b)
{
	if (v.ended)
		return 0;

	const u16 type = b.samplesSourceType;

	if (type == 0x21) { // PCM16 in main RAM, circular (JAI streams)
		if (b.remainingLength == 0) {
			v.ended = true;
			return 0;
		}
		u32 loopLen = b.endPosition >> 16;
		if (loopLen != 0 && v.pos >= loopLen)
			v.pos = 0;
		s16 s = b.baseAddress ? b.baseAddress[v.pos] : 0;
		++v.pos;
		if (b.remainingLength != 0xFFFFFFFFu)
			--b.remainingLength;
		return s;
	}

	if (v.pos >= b.endPosition) {
		if (b.isLooping) {
			v.pos = (u32)(uintptr_t)b.loopAddress;
			v.yn1 = b.loopYN1;
			v.yn2 = b.loopYN2;
			v.frame = -1;
			if ((v.pos & 15) != 0 && (type == 9 || type == 5)) {
				// Loop start inside a frame: decode that frame from the stored history.
				decodeAdpcmFrame(v, b, (int)(v.pos >> 4));
			}
		} else {
			v.ended = true;
			return 0;
		}
	}

	u32 base = (u32)(uintptr_t)b.baseAddress;
	s16 s    = 0;
	switch (type) {
	case 9:
	case 5: {
		int f = (int)(v.pos >> 4);
		if (f != v.frame)
			decodeAdpcmFrame(v, b, f);
		s = v.frameBuf[v.pos & 15];
		break;
	}
	case 8:
		s = (s16)((s8)*port_aram_ptr(base + v.pos) << 8);
		break;
	case 0x10: {
		const u8* p = port_aram_ptr(base + v.pos * 2);
		s           = (s16)((p[0] << 8) | p[1]);
		break;
	}
	default:
		s = 0;
		break;
	}
	++v.pos;
	return s;
}

// 64-sample oscillator waveforms (noteOnOsc voices, source types 0-3): the four
// 64-entry tables JAudio hands the DSP after the resampling taps in DSPRES_FILTER
// (sine, triangle and two single-pulse tables).
s16 oscSample(u32 type, u32 idx) { return gOsc[type & 3][idx & 63]; }

void resetVoice(Voice& v, DSPBuffer& b)
{
	std::memset(&v, 0, sizeof(v));
	v.frame = -1;
	v.pos   = b.samplesSourceType == 0x21 ? (u32)b.currentPosition >> 16 : 0;
	v.frac  = (u16)b.currentPosFrac;
	b.resetVpb   = 0;
	b.endReached = 0;
	// Prime the resampler so the first output sample is the first source sample.
	if (b.samplesSourceType >= 4) {
		v.hist[1] = fetch(v, b);
		v.hist[2] = fetch(v, b);
		v.hist[3] = fetch(v, b);
	}
}

// Renders one sub-frame of voice i into `out` (source rate -> 32 kHz, before volume).
// Returns false if the voice produced nothing worth mixing.
bool renderSource(int i, DSPBuffer& b, s32* out)
{
	Voice& v = gState[i];
	if (b.resetVpb) {
		static int sLogged;
		if (gLog && sLogged < 400) {
			++sLogged;
			std::fprintf(stderr,
			             "[port] audio: v%02d start src %02x base %08x pitch %04x loop %u ls %u end %u n %d "
			             "dolby %u vol [%04x>%04x@%04x %04x>%04x@%04x %04x>%04x@%04x %04x>%04x@%04x] lp %d fm %02x\n",
			             i, b.samplesSourceType, (u32)(uintptr_t)b.baseAddress, b.resamplingRatio, b.isLooping,
			             (u32)(uintptr_t)b.loopAddress, b.endPosition, b.sampleCount, b.useDolbyVolume,
			             b.mixChannels[0].currentVolume, b.mixChannels[0].targetVolume, b.mixChannels[0].id,
			             b.mixChannels[1].currentVolume, b.mixChannels[1].targetVolume, b.mixChannels[1].id,
			             b.mixChannels[2].currentVolume, b.mixChannels[2].targetVolume, b.mixChannels[2].id,
			             b.mixChannels[3].currentVolume, b.mixChannels[3].targetVolume, b.mixChannels[3].id,
			             b.lowPassCoeff, b.filterMode);
			if (b.samplesSourceType == 0x10 || b.samplesSourceType == 8) {
				// Smoothness of the first samples read big-endian vs little-endian
				// (mean |x[n]-x[n-1]| / mean |x|: real audio is well below 1).
				u32 base = (u32)(uintptr_t)b.baseAddress;
				double sa[2] = {}, sd[2] = {};
				int cnt = b.sampleCount < 4000 ? b.sampleCount : 4000;
				s32 prev[2] = {};
				for (int k = 0; k < cnt; ++k) {
					const u8* p = port_aram_ptr(base + (b.samplesSourceType == 8 ? k : k * 2));
					s32 x[2];
					if (b.samplesSourceType == 8) {
						x[0] = x[1] = (s8)p[0];
					} else {
						x[0] = (s16)((p[0] << 8) | p[1]);
						x[1] = (s16)((p[1] << 8) | p[0]);
					}
					for (int e = 0; e < 2; ++e) {
						sa[e] += x[e] < 0 ? -x[e] : x[e];
						sd[e] += x[e] - prev[e] < 0 ? prev[e] - x[e] : x[e] - prev[e];
						prev[e] = x[e];
					}
				}
				std::fprintf(stderr, "    v%02d pcm smoothness BE %.2f LE %.2f (mean |x| %.0f)\n", i,
				             sa[0] ? sd[0] / sa[0] : 0.0, sa[1] ? sd[1] / sa[1] : 0.0, cnt ? sa[0] / cnt : 0.0);
			}
		}
		resetVoice(v, b);
	}

	if (b.useConstantSample) { // paused
		for (int n = 0; n < kSub; ++n)
			out[n] = b.constantSample;
		return b.constantSample != 0;
	}

	u32 step = (u32)b.resamplingRatio << 4; // 4.12 -> 16.16
	const u16 type = b.samplesSourceType;

	if (type < 4) {
		for (int n = 0; n < kSub; ++n) {
			out[n] = oscSample(type, v.oscPhase >> 16);
			v.oscPhase += step;
		}
		return true;
	}

	for (int n = 0; n < kSub; ++n) {
		const s16* h = gRes[(v.frac >> 10) & 63];
		s32 acc      = h[0] * v.hist[0] + h[1] * v.hist[1] + h[2] * v.hist[2] + h[3] * v.hist[3];
		out[n]       = clamp16(acc >> 15);
		v.frac += step;
		while (v.frac >= 0x10000) {
			v.frac -= 0x10000;
			v.hist[0] = v.hist[1];
			v.hist[1] = v.hist[2];
			v.hist[2] = v.hist[3];
			v.hist[3] = fetch(v, b);
		}
	}
	b.currentPosFrac = (s16)(u16)v.frac;

	if (type == 0x21) {
		u32 loopLen         = b.endPosition >> 16;
		b.currentPosition   = (int)(v.pos << 16);
		b.samplesBeforeLoop = loopLen > v.pos ? (loopLen - v.pos) << 16 : 0;
	}

	// Per-voice filters (setFilterMode): bits 0-4 = FIR tap count (JAudio uses 8,
	// the game-side "outer" FIR, e.g. muffling), bit 5 = IIR. Coefficients Q15. The
	// IIR form (b0 x[n] + b1 x[n-1] + a1 y[n-1] + a2 y[n-2]) is a reconstruction;
	// JAudio's default coefficients {0x7FFF, 0, 0, 0} make it a pass-through,
	// which is how the game sets it unless a sequence animates them.
	int taps = b.filterMode & 0x1F;
	if (taps > 8)
		taps = 8;
	if (taps > 0) {
		static int sFirLog;
		if (gLog && sFirLog < 5) {
			++sFirLog;
			std::fprintf(stderr, "[port] audio: v%02d FIR %d taps: %d %d %d %d %d %d %d %d\n", i, taps,
			             b.variableFirCoeffs[0], b.variableFirCoeffs[1], b.variableFirCoeffs[2],
			             b.variableFirCoeffs[3], b.variableFirCoeffs[4], b.variableFirCoeffs[5],
			             b.variableFirCoeffs[6], b.variableFirCoeffs[7]);
		}
		// Tap order of the microcode (jdsp words 0x0BF6-0x0C10, called from
		// 0x0300-0x0328): the history (VPB 0x0440, the last N inputs, oldest
		// first) is copied in front of the sub-frame, and output j is
		// sum c[k] * x[j - N + k]: c[0] on the oldest sample, c[N-1] on x[j-1],
		// so the current input is not in its own output (one-sample latency).
		// Afterwards the last N inputs go back to the history.
		s16* fh = (s16*)b.variableFirHistory;
		s16 x[8 + kSub];
		for (int k = 0; k < taps; ++k)
			x[k] = fh[k];
		for (int n = 0; n < kSub; ++n)
			x[taps + n] = clamp16(out[n]);
		for (int n = 0; n < kSub; ++n) {
			s32 acc = 0;
			for (int k = 0; k < taps; ++k)
				acc += b.variableFirCoeffs[k] * x[n + k];
			out[n] = clamp16((acc + 0x4000) >> 15);
		}
		for (int k = 0; k < taps; ++k)
			fh[k] = x[kSub + k];
	}
	if (b.filterMode & 0x20) {
		const s16* c = b.biquadFilterCoeffs;
		if (c[0] != 0x7FFF || c[1] != 0 || c[2] != 0 || c[3] != 0) {
			static int sIirLog;
			if (gLog && sIirLog < 5) {
				++sIirLog;
				std::fprintf(stderr, "[port] audio: v%02d IIR %d %d %d %d\n", i, c[0], c[1], c[2], c[3]);
			}
			s16* h = b.biquadHistory; // x1, x2, y1, y2
			for (int n = 0; n < kSub; ++n) {
				s32 x  = clamp16(out[n]);
				s32 y  = clamp16((c[0] * x + c[1] * h[0] + c[2] * h[2] + c[3] * h[3]) >> 15);
				h[1]   = h[0];
				h[0]   = (s16)x;
				h[3]   = h[2];
				h[2]   = (s16)y;
				out[n] = y;
			}
		}
	}

	// Distance filter: one-pole low-pass, coefficient Q15 (0 = off).
	if (b.lowPassCoeff != 0) {
		s32 c = b.lowPassCoeff;
		if (c < 0)
			c = -c;
		for (int n = 0; n < kSub; ++n) {
			v.lp   = out[n] + (((v.lp - out[n]) * c) >> 15);
			out[n] = v.lp;
		}
	}
	return true;
}

// The microcode's voice mixer (jdsp 0x0D0A-0x0D21). The DSP runs with M0 (no
// product doubling) here, so a gain is Q16: bus += (gain * sample) >> 16, stored
// with 16-bit saturation (40-bit mode). The first 64 samples use `gain`; then the
// 16.16 `step` is added once and the last 16 samples use the new gain, which is
// returned. Nothing happens when both are zero.
s32 mixDsp(s32* bus, const s32* src, s32 gain, s32 step)
{
	if (gain == 0 && step == 0)
		return gain;
	for (int n = 0; n < 64; ++n)
		bus[n] = clamp16(bus[n] + ((gain * src[n]) >> 16));
	s32 g2 = (s32)((((long long)gain << 16) + step) >> 16);
	for (int n = 64; n < kSub; ++n)
		bus[n] = clamp16(bus[n] + ((g2 * src[n]) >> 16));
	return g2;
}

// Step for a gain change: ASR 5 of (delta << 16), i.e. 1/32 of the change
// lands in this sub-frame (0x0396-0x039A, 0x0DA6-0x0DAA).
inline s32 rampStep(s32 delta) { return delta * 2048; }

const u16 kAutoBuses[8] = { 0x0D00, 0x0D60, 0x0F40, 0x0CA0, 0x0E80, 0x0EE0, 0x0C00, 0x0C50 };

void mixVoice(int vi, DSPBuffer& b, const s32* src)
{
	if (b.useDolbyVolume) {
		// "Auto mixer" (jdsp 0x0D32-0x0DC0, bus table at 0x0DD6).
		// End request: each sub-frame the target becomes current / 2 (logical
		// shift) and the voice is done once the current volume is 0.
		if (b.endRequested) {
			b.dolbyVolumeTarget = (u16)(b.dolbyVolumeCurrent >> 1);
			if (b.dolbyVolumeCurrent == 0)
				b.done = 1;
		}
		// Position word: high byte left/right (JAudio pan x 127.5), low 7 bits
		// front/back ("dolby" x 127.5). Gains from the quarter-sine table at DSP
		// 0x0200 (DSPRES_FILTER s16 512-639), products Q16 (M0).
		int pan = b.dolbyVoicePosition >> 8;
		if (pan > 127)
			pan = 127; // the table has 128 entries; JAudio never goes past 127
		int fb       = b.dolbyVoicePosition & 0x7F;
		s32 sR       = gPanSin[pan], sL = gPanSin[pan ^ 0x7F];
		s32 sB       = gPanSin[fb], sF = gPanSin[fb ^ 0x7F];
		s32 p[4]     = { (sL * sF) >> 16, (sR * sF) >> 16, (sL * sB) >> 16, (sR * sB) >> 16 };
		s32 vCur     = (s16)b.dolbyVolumeCurrent;
		s32 fx       = (s16)b.dolbyReverbFactor;
		s32 fxv      = (fx * vCur) >> 15; // MULC, ASL 1, take the middle word
		s32 delta    = (s32)(s16)b.dolbyVolumeTarget - vCur;
		if (delta > 32767)
			delta = 32767; // MRR from ac0.m saturates in 40-bit mode
		else if (delta < -32768)
			delta = -32768;
		b.dolbyVolumeCurrent = b.dolbyVolumeTarget;
		s32 gain[8], dstep[8];
		for (int k = 0; k < 4; ++k) {
			gain[k]      = (p[k] * vCur) >> 16;
			gain[4 + k]  = (gain[k] * fxv) >> 16;
			dstep[k]     = (p[k] * delta) >> 16;
			dstep[4 + k] = (dstep[k] * fx) >> 16;
		}
		for (int k = 0; k < 8; ++k) {
			int bi = busIndex(kAutoBuses[k]);
			if (bi >= 0)
				mixDsp(gBus[bi], src, gain[k], rampStep(dstep[k]));
		}
		return;
	}

	// Normal mixer channels (jdsp 0x0370-0x03B6).
	// End request: every channel's target becomes current / 2, and the voice is
	// done once all six current volumes are 0 (0x0370-0x0385).
	if (b.endRequested) {
		bool zero = true;
		for (int c = 0; c < 6; ++c) {
			b.mixChannels[c].targetVolume = (u16)(b.mixChannels[c].currentVolume >> 1);
			if (b.mixChannels[c].currentVolume != 0)
				zero = false;
		}
		if (zero)
			b.done = 1;
	}
	// Per-channel delay: the level word's low byte is the current delay in
	// samples and moves one step per sub-frame toward the high byte; the mix
	// reads the voice output that many samples back.
	Voice& v = gState[vi];
	s32 ext[255 + kSub];
	for (int n = 0; n < 255; ++n)
		ext[n] = v.mixHist[n];
	for (int n = 0; n < kSub; ++n)
		ext[255 + n] = src[n];
	for (int n = 0; n < 255; ++n)
		v.mixHist[n] = (s16)ext[kSub + n];
	for (int c = 0; c < 6; ++c) {
		DSPBuffer::Channel& ch = b.mixChannels[c];
		s32 cur   = (s16)ch.currentVolume;
		s32 tgt   = (s16)ch.targetVolume;
		int delay = ch.level & 0xFF;
		int want  = ch.level >> 8;
		if (want > delay)
			ch.level = (u16)(ch.level + 1);
		else if (want < delay)
			ch.level = (u16)(ch.level - 1);
		if (ch.id == 0)
			continue; // not mixed; current volume unchanged
		int bi = busIndex(ch.id);
		s32 g  = cur;
		if (bi >= 0)
			g = mixDsp(gBus[bi], ext + 255 - delay, cur, rampStep(tgt - cur));
		else if (cur != 0 || tgt != cur)
			g = (s32)((((long long)cur << 16) + rampStep(tgt - cur)) >> 16);
		ch.currentVolume = (u16)g;
	}
}

// FX (reverb/echo) lines, as the microcode runs them (jdsp words 0x0172-0x01A5
// at the start of each sub-frame, before the voices; write-back 0x0205-0x0230
// at the end). FXBuffer is copied to DSP 0x0390: unk0 mode (0 = line off),
// unk2 delay length in sub-frame blocks, unk4 the delay line in main RAM,
// unk8/unkA and unkC/unkE two output buses with Q15 volumes, unk10 an 8-tap FIR.
bool fxLineActive(const FXBuffer& fx)
{
	return fx.unk0 != 0 && fx.unk4 != nullptr && fx.unk2 != 0;
}

// The FX FIR (jdsp 0x0BD7-0x0BF5): in place over the 88-word line buffer,
// word j = sum c[k] * word[j + k] for j = 0..79, so c[0] takes the oldest of
// the eight samples before the one at j + 8 and c[7] the newest. Rounded
// product (MOVPZ), stored with 16-bit saturation.
void fxFir(s32* buf, const s16* c)
{
	for (int j = 0; j < kSub; ++j) {
		s32 acc = 0;
		for (int k = 0; k < 8; ++k)
			acc += c[k] * buf[j + k];
		buf[j] = clamp16((acc + 0x4000) >> 15);
	}
}

void fxLinesRead()
{
	if (!gFx)
		return;
	for (int i = 0; i < kFxLines; ++i) {
		FXBuffer& fx = gFx[i];
		if (gLog) {
			static u32 sLast[kFxLines][4];
			u32 now[4] = { (u32)fx.unk0 | ((u32)fx.unk2 << 16), ((u32)fx.unk8 << 16) | (u16)fx.unkA,
				           ((u32)fx.unkC << 16) | (u16)fx.unkE, fx.unk4 ? 1u : 0u };
			if (std::memcmp(now, sLast[i], sizeof(now)) != 0) {
				std::memcpy(sLast[i], now, sizeof(now));
				std::fprintf(stderr,
				             "[port] audio: fx%d on %d buf %s %d blocks, out %04x vol %d, out %04x vol %d, fir %d %d %d %d "
				             "%d %d %d %d\n",
				             i, fx.unk0, fx.unk4 ? "yes" : "no", fx.unk2, fx.unk8, fx.unkA, fx.unkC, fx.unkE, fx.unk10[0],
				             fx.unk10[1], fx.unk10[2], fx.unk10[3], fx.unk10[4], fx.unk10[5], fx.unk10[6], fx.unk10[7]);
			}
		}
		if (!fxLineActive(fx))
			continue;
		if (gFxPos[i] >= (u32)fx.unk2)
			gFxPos[i] = 0;
		// History: words 80-87 of the last sub-frame move to 0-7 (0x018C-0x0192),
		// then the block at the current position (written `blocks` sub-frames
		// ago) is DMAd to words 8-87 (0x012D-0x013E).
		s32 buf[8 + kSub];
		for (int k = 0; k < 8; ++k)
			buf[k] = gFxTail[i][k];
		const s16* blk = fx.unk4 + gFxPos[i] * kSub;
		for (int n = 0; n < kSub; ++n)
			buf[8 + n] = blk[n];
		for (int k = 0; k < 8; ++k)
			gFxTail[i][k] = buf[kSub + k];
		// Mode bit 0 clear: FIR before the sends (0x013F-0x014A).
		if (!(fx.unk0 & 1))
			fxFir(buf, fx.unk10);
		// The two outputs take words 0-79 (0x014B-0x0164, mixer 0x00D8).
		int d0 = fx.unk8 ? busIndex(fx.unk8) : -1, d1 = fx.unkC ? busIndex(fx.unkC) : -1;
		for (int n = 0; n < kSub; ++n) {
			if (d0 >= 0)
				gBus[d0][n] += (buf[n] * fx.unkA) >> 15;
			if (d1 >= 0)
				gBus[d1][n] += (buf[n] * fx.unkE) >> 15;
		}
		if (gLog)
			for (int n = 0; n < kSub; ++n)
				gFxOutAbs[i] += (u64)std::abs((buf[n] * fx.unkA) >> 15) + (u64)std::abs((buf[n] * fx.unkE) >> 15);
		// Mode bit 1 clear: FIR again, in place, after the sends (0x0165-0x0171).
		if (!(fx.unk0 & 2))
			fxFir(buf, fx.unk10);
		// Words 0-79 are now the line's bus: whatever an earlier line sent into it
		// is overwritten; the voices (and later lines) add on top, and the sum is
		// written back to the delay line after the voices (fxLinesWrite).
		for (int n = 0; n < kSub; ++n)
			gBus[BUS_FX0 + i][n] = buf[n];
	}
	// Then the auto mixer's back FX sends of the previous sub-frame go into
	// lines 2 and 3 with the same spread as the main back buses (0x01A6-0x01C9):
	// 0x0C00 -> 0x0E80 x1.0, 0x0EE0 x-0.5615; 0x0C50 samples 40-79 -> the first
	// halves, 0x0E80 x-0.5615, 0x0EE0 x1.0 (its samples 0-39 go in at the end
	// of the sub-frame, fxLinesWrite).
	for (int n = 0; n < kSub; ++n) {
		gBus[BUS_FX2][n] += (gC00Prev[n] * 0x7FFF) >> 15;
		gBus[BUS_FX3][n] += (gC00Prev[n] * -0x47E0) >> 15;
	}
	for (int n = 0; n < 40; ++n) {
		gBus[BUS_FX2][n] += (gC50Tail[n] * -0x47E0) >> 15;
		gBus[BUS_FX3][n] += (gC50Tail[n] * 0x7FFF) >> 15;
	}
}

// ---- 5.1 / 7.1: the back FX sends as rear reverb ------------------------------------
// The auto mixer's back FX sends 0x0C00 / 0x0C50 are matrix-encoded into FX lines 2-3
// (x1.0 / x-0.5615, fxLinesRead / fxLinesWrite), so on the GameCube the reverb of a
// source behind the camera returns through the buses lines 2-3 send to and the Pro Logic
// decoder steers it back. For discrete channels the same sends also run through two
// "rear" lines with line 2's and line 3's settings (0x0C00 -> left rear, 0x0C50 -> right
// rear, not encoded), and two "fold" lines fed with nothing but the encoded fold remove
// its share from the buses the real lines 2-3 return to. The lines are linear (up to
// the 16-bit stores), so the discrete fronts keep everything else of the original mix
// and the back reverb moves to the rears. The matrix-encoded stereo output is untouched.
const u32 kShadowMaxBlocks = 512;
struct ShadowLine {
	s16 delay[kShadowMaxBlocks * kSub];
	u32 blocks; // 0 = idle
	u32 pos;
	s32 tail[8];
	s32 bus[kSub]; // this sub-frame's words 0-79
};
ShadowLine gShadow[4];    // 0, 1: encoded fold of lines 2, 3; 2, 3: rear lines (0x0C00, 0x0C50)
s32 gMcAdj[kBuses][kSub]; // per-bus correction of the discrete channels, this sub-frame
s32 gRearRev[2][kSub];    // rear reverb (left, right), this sub-frame
bool gShadowOn;           // the lines ran this sub-frame

void shadowLinesRead()
{
	std::memset(gMcAdj, 0, sizeof(gMcAdj));
	std::memset(gRearRev, 0, sizeof(gRearRev));
	gShadowOn = gFx != nullptr && port_audio_mc_channels() > 2;
	if (!gShadowOn) {
		for (ShadowLine& sh : gShadow)
			sh.blocks = 0;
		return;
	}
	for (int s = 0; s < 4; ++s) {
		const FXBuffer& fx = gFx[2 + (s & 1)];
		ShadowLine& sh     = gShadow[s];
		if (!fxLineActive(fx) || (u32)fx.unk2 > kShadowMaxBlocks) {
			sh.blocks = 0;
			std::memset(sh.bus, 0, sizeof(sh.bus));
			continue;
		}
		if (sh.blocks != (u32)fx.unk2) { // (re)start silent
			sh.blocks = fx.unk2;
			sh.pos    = 0;
			std::memset(sh.delay, 0, sizeof(sh.delay));
			std::memset(sh.tail, 0, sizeof(sh.tail));
		}
		s32 buf[8 + kSub];
		for (int k = 0; k < 8; ++k)
			buf[k] = sh.tail[k];
		const s16* blk = sh.delay + sh.pos * kSub;
		for (int n = 0; n < kSub; ++n)
			buf[8 + n] = blk[n];
		for (int k = 0; k < 8; ++k)
			sh.tail[k] = buf[kSub + k];
		if (!(fx.unk0 & 1))
			fxFir(buf, fx.unk10);
		int d0 = fx.unk8 ? busIndex(fx.unk8) : -1, d1 = fx.unkC ? busIndex(fx.unkC) : -1;
		// A return into another FX line's bus would feed that line; SMS returns to the
		// output buses only, so such a return is not corrected.
		if (d0 >= BUS_FX0 && d0 <= BUS_FX3)
			d0 = -1;
		if (d1 >= BUS_FX0 && d1 <= BUS_FX3)
			d1 = -1;
		for (int n = 0; n < kSub; ++n) {
			const s32 a = (buf[n] * fx.unkA) >> 15, e = (buf[n] * fx.unkE) >> 15;
			if (s < 2) {
				if (d0 >= 0)
					gMcAdj[d0][n] -= a;
				if (d1 >= 0)
					gMcAdj[d1][n] -= e;
			} else
				gRearRev[s - 2][n] += a + e; // the line's whole return, on its rear
		}
		if (!(fx.unk0 & 2))
			fxFir(buf, fx.unk10);
		for (int n = 0; n < kSub; ++n)
			sh.bus[n] = buf[n];
	}
	// The fold at the start of the sub-frame, timed as in fxLinesRead.
	for (int n = 0; n < kSub; ++n) {
		gShadow[0].bus[n] += (gC00Prev[n] * 0x7FFF) >> 15;
		gShadow[1].bus[n] += (gC00Prev[n] * -0x47E0) >> 15;
		gShadow[2].bus[n] += (gC00Prev[n] * 0x7FFF) >> 15;
	}
	for (int n = 0; n < 40; ++n) {
		gShadow[0].bus[n] += (gC50Tail[n] * -0x47E0) >> 15;
		gShadow[1].bus[n] += (gC50Tail[n] * 0x7FFF) >> 15;
		gShadow[3].bus[n] += (gC50Tail[n] * 0x7FFF) >> 15;
	}
}

void shadowLinesWrite()
{
	if (!gShadowOn)
		return;
	for (int n = 0; n < 40; ++n) {
		gShadow[0].bus[40 + n] += (gBus[BUS_C50][n] * -0x47E0) >> 15;
		gShadow[1].bus[40 + n] += (gBus[BUS_C50][n] * 0x7FFF) >> 15;
		gShadow[3].bus[40 + n] += (gBus[BUS_C50][n] * 0x7FFF) >> 15;
	}
	for (ShadowLine& sh : gShadow) {
		if (sh.blocks == 0)
			continue;
		s16* blk = sh.delay + sh.pos * kSub;
		for (int n = 0; n < kSub; ++n)
			blk[n] = clamp16(sh.bus[n]);
		sh.pos = (sh.pos + 1) % sh.blocks;
	}
}

// Phantom centre (5.1 / 7.1), deliberately conservative: only what the two fronts have
// strongly in common (smoothed correlation above 0.3, over ~50 ms) is steered, at most
// half of the mid signal (-6 dB), and half of what goes to the centre is taken out of
// each front, so a centred mono sound keeps its level and image.
struct PhantomCentre {
	f32 ll = 0.0f, rr = 0.0f, lr = 0.0f, g = 0.0f;
	f32 run(f32& fl, f32& fr)
	{
		const f32 a = 1.0f / 1600.0f; // 50 ms at 32 kHz
		ll += a * (fl * fl - ll);
		rr += a * (fr * fr - rr);
		lr += a * (fl * fr - lr);
		f32 rho = lr / std::sqrt(ll * rr + 1.0f);
		f32 want = rho <= 0.3f ? 0.0f : 0.5f * (rho - 0.3f) / 0.7f;
		g += (1.0f / 320.0f) * (want - g); // ~10 ms glide, no zipper noise
		const f32 c = g * 0.5f * (fl + fr);
		fl -= 0.5f * c;
		fr -= 0.5f * c;
		return c;
	}
};
PhantomCentre gPhantom;

// End of the sub-frame (0x0205-0x0230, per line 0x01D6): words 0-79 of each
// active line go back to the delay line at the current position, which then
// advances, wrapping at `blocks`.
void fxLinesWrite()
{
	// 0x01F0-0x0204: 0x0C50 samples 0-39 -> second halves of lines 2 and 3.
	for (int n = 0; n < 40; ++n) {
		gBus[BUS_FX2][40 + n] += (gBus[BUS_C50][n] * -0x47E0) >> 15;
		gBus[BUS_FX3][40 + n] += (gBus[BUS_C50][n] * 0x7FFF) >> 15;
	}
	shadowLinesWrite(); // 5.1 / 7.1 (needs this sub-frame's 0x0C50)
	std::memcpy(gC00Prev, gBus[BUS_C00], sizeof(gC00Prev));
	std::memcpy(gC50Tail, gBus[BUS_C50] + 40, sizeof(gC50Tail));
	if (!gFx)
		return;
	for (int i = 0; i < kFxLines; ++i) {
		FXBuffer& fx = gFx[i];
		if (!fxLineActive(fx))
			continue;
		s16* blk = fx.unk4 + gFxPos[i] * kSub;
		for (int n = 0; n < kSub; ++n)
			blk[n] = clamp16(gBus[BUS_FX0 + i][n]);
		if (gLog)
			for (int n = 0; n < kSub; ++n)
				gFxInAbs[i] += (u64)std::abs(blk[n]);
		gFxPos[i] = (gFxPos[i] + 1) % (u32)fx.unk2;
	}
}

} // namespace

extern "C" {

// w9: DualSense speaker tap (port_speaker.cpp)
u32 port_speaker_voice_tag(int voice);
float port_speaker_main_gain(void);
void port_speaker_push(const int* l, const int* r, int n);
void port_speaker_subframe_end(int n);
bool port_speaker_wants(u32 id);
int port_qol_volumes_active(void);   // w9: port_qol.cpp
float port_qol_voice_gain(u32 seId); // w9: port_qol.cpp

void port_dsp_setup(uint32_t numVoices, uint32_t voices, uint32_t resFilter, uint32_t adpcmFilter,
                    uint32_t fxLines)
{
	// The voice and FX blocks live in a JKR heap in emulated main RAM, so their
	// 32-bit mail addresses are valid pointers. The two tables are static arrays in
	// the exe image (above 4 GB on x64), so their mail addresses are truncated:
	// take them from the symbols instead.
	(void)resFilter;
	(void)adpcmFilter;
	gVoices = ramPtr<DSPBuffer>(voices);
	gFx     = ramPtr<FXBuffer>(fxLines);
	if (gVoices != JASystem::DSPInterface::getDSPHandle(0) || gFx != JASystem::DSPInterface::getFXHandle(0)) {
		std::fprintf(stderr, "[port] audio: DsetupTable addresses do not match the JAudio buffers; using the symbols\n");
		gVoices = JASystem::DSPInterface::getDSPHandle(0);
		gFx     = JASystem::DSPInterface::getFXHandle(0);
	}
	// The tables are u32 arrays holding pairs of s16 in GameCube (big-endian) word
	// order: the first s16 is the high half of each native u32.
	// DSPRES_FILTER (640 s16): 0-255 resampling taps (64 phases x 4; phase 0 is
	// {0x0C39, 0x66AD, 0x0D46, -0x21}, phase 63 its mirror), 256-511 four 64-sample
	// oscillator tables, 512-639 a quarter sine.
	const u32* rw = JASystem::DSPInterface::DSPRES_FILTER;
	auto res16    = [rw](u32 k) { return (s16)((k & 1) ? (rw[k / 2] & 0xFFFF) : (rw[k / 2] >> 16)); };
	for (int p = 0; p < 64; ++p)
		for (int t = 0; t < 4; ++t)
			gRes[p][t] = res16((u32)p * 4 + t);
	for (int o = 0; o < 4; ++o)
		for (int i = 0; i < 64; ++i)
			gOsc[o][i] = res16(256 + (u32)o * 64 + i);
	for (int i = 0; i < 128; ++i)
		gPanSin[i] = res16(512 + (u32)i);
	const u32* aw = JASystem::DSPInterface::DSPADPCM_FILTER;
	for (int i = 0; i < 16; ++i) {
		gCoef[i][0] = (s16)(aw[i] >> 16);
		gCoef[i][1] = (s16)(aw[i] & 0xFFFF);
	}
	std::memset(gState, 0, sizeof(gState));
	std::memset(gFxPos, 0, sizeof(gFxPos));
	std::memset(gFxTail, 0, sizeof(gFxTail));
	std::memset(gC00Prev, 0, sizeof(gC00Prev));
	std::memset(gC50Tail, 0, sizeof(gC50Tail));
	gSetup = gVoices != nullptr;
	std::fprintf(stderr, "[port] audio: DSP setup, %u voices at %08x, fx at %08x (voice block %zu bytes)\n",
	             numVoices, voices, fxLines, sizeof(DSPBuffer));
}

void port_dsp_sync_frame(uint32_t subframes, uint32_t left, uint32_t right, uint32_t mixerLevel)
{
	gOutL      = ramPtr<s16>(left);
	gOutR      = ramPtr<s16>(right);
	gSubframes = subframes;
	gSubIndex  = 0;
	gLevel     = mixerLevel;
	// Discrete-channel shadow of this output buffer (5.1 / 7.1 output).
	gMc = nullptr;
	if (port_audio_mc_channels() > 2 && gOutL != nullptr) {
		McShadow* pick = &gMcSlots[gMcNext];
		for (McShadow& m : gMcSlots)
			if (m.key == gOutL)
				pick = &m;
		if (pick == &gMcSlots[gMcNext])
			gMcNext = (gMcNext + 1) % kMcSlots;
		pick->key    = gOutL;
		pick->frames = subframes * kSub > kMcMaxFrames ? kMcMaxFrames : subframes * kSub;
		std::memset(pick->data, 0, sizeof(pick->data));
		gMc = pick;
	}
}

const int16_t* port_dsp_mc_shadow(const int16_t* dspL, uint32_t* frames)
{
	for (McShadow& m : gMcSlots)
		if (m.key == dspL) {
			*frames = m.frames;
			return m.data;
		}
	return nullptr;
}

void port_dsp_render_subframe(uint64_t map)
{
	if (!gSetup)
		return;
	std::memset(gBus, 0, sizeof(gBus));

	fxLinesRead();
	shadowLinesRead(); // 5.1 / 7.1: rear reverb lines (idle otherwise)

	static u32 sFrames;
	int active = 0;
	for (int i = 0; i < kVoices; ++i) {
		DSPBuffer& b = gVoices[i];
		if (!((map >> (63 - i)) & 1) || !b.enabled || b.done)
			continue;
		s32 src[kSub];
		renderSource(i, b, src);
		// Debug: SMS_PORT_AUDIO_SE_ONLY=1 silences the normal-mixer voices (the
		// sequence music) so auto-mixed sound effects and voices stand alone in a
		// capture. Volumes and end fades still run.
		static const bool sSeOnly = std::getenv("SMS_PORT_AUDIO_SE_ONLY") != nullptr;
		if (sSeOnly && !b.useDolbyVolume)
			std::memset(src, 0, sizeof(src));
		// w9: DualSense speaker tap (port_speaker.cpp). A voice of a selected sound effect
		// (FLUDD, pickups) is captured from its front-bus contribution, with the game's volume,
		// pan and fades, and kept, ducked or removed in the main mix.
		// w9 quality of life: music / sound effect / voice volumes (port_qol.cpp); the sound
		// id comes from the same SE track tags (0 = not a sound effect: music).
		const u32 seTag = port_speaker_voice_tag(i);
		if (port_qol_volumes_active()) {
			const float vg = port_qol_voice_gain(seTag);
			if (vg != 1.0f)
				for (int n = 0; n < kSub; ++n)
					src[n] = (s32)((float)src[n] * vg);
		}
		if (seTag != 0 && port_speaker_wants(seTag)) {
			static s32 sSave[kBuses][kSub];
			std::memcpy(sSave, gBus, sizeof(gBus));
			mixVoice(i, b, src);
			s32 dl[kSub], dr[kSub];
			for (int n = 0; n < kSub; ++n) {
				dl[n] = gBus[BUS_L][n] - sSave[BUS_L][n];
				dr[n] = gBus[BUS_R][n] - sSave[BUS_R][n];
			}
			port_speaker_push(dl, dr, kSub);
			const float g = port_speaker_main_gain();
			if (g < 1.0f)
				for (int k = 0; k < kBuses; ++k)
					for (int n = 0; n < kSub; ++n)
						gBus[k][n] = sSave[k][n] + (s32)((float)(gBus[k][n] - sSave[k][n]) * g);
		} else
			mixVoice(i, b, src); // also runs the end-request fade, which sets done
		++active;
		if (gState[i].ended && b.samplesSourceType >= 4) {
			if (!b.endRequested)
				b.endReached = 1;
			b.done = 1;
		}
	}
	port_speaker_subframe_end(kSub); // w9: DualSense speaker stream, one sub-frame
	fxLinesWrite();

	static const char* sTrace = std::getenv("SMS_PORT_AUDIO_TRACE"); // voice number
	if (sTrace) {
		DSPBuffer& b = gVoices[std::atoi(sTrace) & 63];
		if (b.enabled)
			std::fprintf(stderr, "T %u src %02x pitch %04x vol %04x %04x auto %u %04x pos %u\n", sFrames,
			             b.samplesSourceType, b.resamplingRatio, b.mixChannels[0].currentVolume,
			             b.mixChannels[1].currentVolume, b.useDolbyVolume, b.dolbyVolumeCurrent,
			             gState[std::atoi(sTrace) & 63].pos);
	}

	if (gOutL && gOutR && gSubIndex < gSubframes) {
		s16* l = gOutL + gSubIndex * kSub;
		s16* r = gOutR + gSubIndex * kSub;
		// Output level, 4.12 exactly as JAudio writes it (DsetMixerLevel(x) stores
		// x * 4096): jdsp 0x0D22-0x0D31 multiplies with M0 and shifts left by 4.
		// The voice mixers are Q16 (M0), so a channel volume at JAudio's cap
		// (inputGainDown 0.5 x 16384) is x0.125, and so is an auto-mixed voice at
		// full volume, centre (sin x sin >> 16, x volume >> 16). SMS's level 5.0
		// makes a full-volume music voice x0.625 at the output (BOOTLOG A-11).
		s32 lvl = (s32)gLevel;
		for (int n = 0; n < kSub; ++n) {
			// Downmix of the other buses into the front pair, with the gains and
			// timing of the microcode (jdsp words 0x00EF-0x0114, run at the start of
			// a sub-frame before the buses are cleared, and 0x03F4-0x0434, run after
			// the voices): 0x0F40 (previous sub-frame) -> L x1.0, R x-0.5615;
			// 0x0CA0 (40 samples late) -> L x-0.5615, R x1.0; 0x09A0 -> L, R
			// x0.7071; 0x0FA0 -> R x-1.0.
			s32 back40 = n < 40 ? gCa0Delay[n] : gBus[BUS_BACK_R][n - 40];
			s32 f40    = gF40Prev[n];
			s32 L      = gBus[BUS_L][n] + f40 + ((back40 * -0x47E0) >> 15) + ((gBus[BUS_CENTER][n] * 0x5A82) >> 15);
			s32 R      = gBus[BUS_R][n] + ((f40 * -0x47E0) >> 15) + back40 + ((gBus[BUS_CENTER][n] * 0x5A82) >> 15)
			      - gBus[BUS_FA0][n];
			l[n]  = clamp16((s32)(((long long)L * lvl) >> 12));
			r[n]  = clamp16((s32)(((long long)R * lvl) >> 12));
			if (gMc != nullptr && gSubIndex * kSub + n < gMc->frames) {
				// Discrete channels from the same buses, before the matrix: fronts
				// (0x0FA0 stays on the right as in the downmix), the centre bus 0x09A0
				// plus a conservative phantom centre, the back buses 0x0F40 / 0x0CA0 as
				// the rears (no matrix delay) with the back FX sends' reverb (shadow
				// lines above), LFE a 120 Hz low-pass of the sum at -12 dB.
				f32 fl = (f32)(gBus[BUS_L][n] + gMcAdj[BUS_L][n]);
				f32 fr = (f32)(gBus[BUS_R][n] + gMcAdj[BUS_R][n] - gBus[BUS_FA0][n] - gMcAdj[BUS_FA0][n]);
				f32 fc = (f32)(gBus[BUS_CENTER][n] + gMcAdj[BUS_CENTER][n]);
				f32 rl = (f32)(gBus[BUS_BACK_L][n] + gMcAdj[BUS_BACK_L][n] + gRearRev[0][n]);
				f32 rr = (f32)(gBus[BUS_BACK_R][n] + gMcAdj[BUS_BACK_R][n] + gRearRev[1][n]);
				f32 lfe = gLfe.run(0.25f * (fl + fr + fc + rl + rr));
				fc += gPhantom.run(fl, fr);
				const f32 k = (f32)lvl * (1.0f / 4096.0f);
				s16* o      = gMc->data + (gSubIndex * kSub + n) * 6;
				o[0]        = f2s16(fl * k);
				o[1]        = f2s16(fr * k);
				o[2]        = f2s16(fc * k);
				o[3]        = f2s16(lfe * k);
				o[4]        = f2s16(rl * k);
				o[5]        = f2s16(rr * k);
			}
		}
	}
	for (int n = 0; n < 40; ++n)
		gCa0Delay[n] = gBus[BUS_BACK_R][n + 40];
	std::memcpy(gF40Prev, gBus[BUS_BACK_L], sizeof(gF40Prev));
	++gSubIndex;

	if (gLog) {
		// Filter usage statistics (voice-sub-frames with each filter active).
		static u32 sIir, sFir, sLp, sAll;
		for (int i = 0; i < kVoices; ++i) {
			DSPBuffer& b = gVoices[i];
			if (!b.enabled)
				continue;
			++sAll;
			const s16* c = b.biquadFilterCoeffs;
			if ((b.filterMode & 0x20) && (c[0] != 0x7FFF || c[1] || c[2] || c[3]))
				++sIir;
			if (b.filterMode & 0x1F)
				++sFir;
			if (b.lowPassCoeff)
				++sLp;
		}
		static u32 sDelay, sEnding;
		for (int i = 0; i < kVoices; ++i) {
			DSPBuffer& b = gVoices[i];
			if (!b.enabled || b.done)
				continue;
			if (b.endRequested)
				++sEnding;
			if (!b.useDolbyVolume)
				for (int c = 0; c < 6; ++c)
					if (b.mixChannels[c].id && b.mixChannels[c].level)
						++sDelay;
		}
		if ((sFrames + 1) % 2000 == 0) {
			std::fprintf(stderr,
			             "[port] audio: filters over %u voice-sub-frames: IIR %u, FIR %u, low-pass %u; channel delays %u, "
			             "ending %u\n",
			             sAll, sIir, sFir, sLp, sDelay, sEnding);
			std::fprintf(stderr, "[port] audio: fx mean |in| %llu %llu %llu %llu, |out| %llu %llu %llu %llu\n",
			             gFxInAbs[0] / 160000, gFxInAbs[1] / 160000, gFxInAbs[2] / 160000, gFxInAbs[3] / 160000,
			             gFxOutAbs[0] / 160000, gFxOutAbs[1] / 160000, gFxOutAbs[2] / 160000, gFxOutAbs[3] / 160000);
			std::memset(gFxInAbs, 0, sizeof(gFxInAbs));
			std::memset(gFxOutAbs, 0, sizeof(gFxOutAbs));
		}
	}
	if (gLog && (++sFrames % 400) == 0) {
		std::fprintf(stderr, "[port] audio: sub-frame %u, %d voices, level %04x\n", sFrames, active, gLevel);
		for (int i = 0; i < kVoices; ++i) {
			DSPBuffer& b = gVoices[i];
			if (!b.enabled)
				continue;
			if (b.useDolbyVolume)
				std::fprintf(stderr, "    v%02d src %02x auto vol %04x>%04x pos %04x fx %04x\n", i, b.samplesSourceType,
				             b.dolbyVolumeCurrent, b.dolbyVolumeTarget, b.dolbyVoicePosition, b.dolbyReverbFactor);
			else
				std::fprintf(stderr, "    v%02d src %02x vol %04x %04x %04x %04x lp %d fm %02x iir %d %d %d %d\n", i,
				             b.samplesSourceType, b.mixChannels[0].targetVolume, b.mixChannels[1].targetVolume,
				             b.mixChannels[2].targetVolume, b.mixChannels[3].targetVolume, b.lowPassCoeff,
				             b.filterMode, b.biquadFilterCoeffs[0], b.biquadFilterCoeffs[1],
				             b.biquadFilterCoeffs[2], b.biquadFilterCoeffs[3]);
		}
	}
}

} // extern "C"
