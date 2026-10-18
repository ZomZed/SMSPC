// Host THPPlayer for the PC port (replaces libs/THPPlayer, which the census
// skips): same API, same ActivePlayer struct and the same state machine as the
// decompiled THPPlayer.c.
//
// What it does:
//  - Open/Prepare read and validate the big-endian THP header, component info,
//    video and audio info exactly like the SDK (byte-swapped on the way in).
//  - Prepare starts a host decoder thread (standing in for the SDK's read and
//    video decode threads) and waits for the first decoded frame, like the
//    SDK's PrepareReady. The decoder walks the frame chain like THPRead.c
//    (each frame starts with the size of the next one; looping movies go back
//    to the first frame), decodes the video component with
//    port_thp_video_decode (port_thp_decode.cpp) into a free texture set
//    carved from the game's buffer, and queues it.
//    The texture sets hold real GX_TF_I8 Y/U/V textures in THPVideoDecode's
//    tiled layout, so TModelGate, which binds them as J3D textures, works too.
//  - Prepare installs PlayControl as the VI post-retrace callback; once
//    playing, the next decoded set becomes current at the movie's frame rate
//    (the SDK's non-interlaced timing rule), the replaced set is "used" until
//    THPPlayerDrawDone / THPPlayerPostDrawDone hands it back to the decoder
//    (the SDK's used-set queue), and the state goes to 3 (played) after the
//    last frame, or the movie wraps when the loop flag is set.
//  - THPPlayerDrawCurrentFrame draws the current set like THPDraw.c's
//    THPGXYuv2RgbSetup / THPGXYuv2RgbDraw / THPGXRestore (three I8 textures,
//    four TEV stages doing YUV to RGB), through the game's GX calls, so Aurora
//    renders it. The headless build (no SMS_PORT_AURORA) draws nothing.
//  - Movie audio (w5), as THPAudioDecode.c / THPPlayer.c do it: the decoder thread
//    decodes each frame's audio component with THPAudioDecode (Aurora's, the SDK
//    API; GameCube AI order right/left) into the SDK's three audio buffers
//    (free/decoded queues), THPPlayerInit registers audioCallbackWithMSound as
//    JAudio's mode-3 mix callback, and MixAudio (the SDK's, with VolumeTable and
//    the volume ramp) feeds JASystem::Kernel::vframeWork, so movie sound is mixed
//    into the DAC buffer on the AI clock. PlayControl keeps the SDK's rule that a
//    new video frame is shown only while curVideoNumber - curAudioNumber <= 1,
//    and a non-looping movie ends when the last audio frame has been played.
//    With SMS_PORT_THP_SPEED > 1 (frames skipped) audio is off and the video runs
//    on the frame-rate clock as before. Headless build (no SMS_PORT_AURORA): no audio.

#include "port_sdk.h"
#include "port_profile.h" // w14
#include "port_thp_decode.h"

#ifdef SMS_PORT_AURORA
#include <JSystem/JAudio/JASystem/JASAiCtrl.hpp>
#include <dolphin/thp.h>
#define PORT_THP_AUDIO 1
#endif

#include <THPPlayer/THPPlayer.h>
#include <dolphin/dvd.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <port_widescreen.h> // w6: movie pillarbox in widescreen

#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

extern "C" void GXDrawDone(void);

THPPlayer ActivePlayer;

namespace {

BOOL sInitialized;
VIRetraceCallback sOldVIPostCallback;

inline u32 align32(u32 v) { return (v + 31) & ~31u; }

inline u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
inline f32 bef32(const u8* p)
{
	u32 v = be32(p);
	f32 f;
	std::memcpy(&f, &v, 4);
	return f;
}

bool readAt(void* dst, s32 len, s32 offset)
{
	return DVDReadPrio(&ActivePlayer.fileInfo, dst, len, offset, 2) >= 0;
}

// Development aid: SMS_PORT_THP_SPEED=<n> plays movies n times faster
// (skipping frames) so boot testing does not sit through the 94 s intro.
// Unset (the default) means normal speed.
s32 speed()
{
	static s32 s = -1;
	if (s < 0) {
		const char* e = std::getenv("SMS_PORT_THP_SPEED");
		s             = e ? std::atoi(e) : 1;
		if (s < 1)
			s = 1;
	}
	return s;
}

// ---------------------------------------------------------------------------
// Texture set queues (the SDK's FreeTextureSetQueue, DecodedTextureSetQueue and
// UsedTextureSetQueue), shared by the decoder thread, PlayControl (interrupt
// context) and the draw thread.

struct SetQueue {
	THPTextureSet* e[THP_TEXTURE_SET_COUNT];
	int head;
	int count;

	void clear() { head = count = 0; }
	void push(THPTextureSet* s)
	{
		if (count < THP_TEXTURE_SET_COUNT) {
			e[(head + count) % THP_TEXTURE_SET_COUNT] = s;
			count++;
		}
	}
	THPTextureSet* pop()
	{
		if (count == 0)
			return nullptr;
		THPTextureSet* s = e[head];
		head             = (head + 1) % THP_TEXTURE_SET_COUNT;
		count--;
		return s;
	}
};

// The SDK's FreeAudioBufferQueue / DecodedAudioBufferQueue (THPAudioDecode.c).
struct AudioQueue {
	THPAudioBuffer* e[THP_AUDIO_BUFFER_COUNT];
	int head;
	int count;

	void clear() { head = count = 0; }
	void push(THPAudioBuffer* b)
	{
		if (count < THP_AUDIO_BUFFER_COUNT) {
			e[(head + count) % THP_AUDIO_BUFFER_COUNT] = b;
			count++;
		}
	}
	THPAudioBuffer* pop()
	{
		if (count == 0)
			return nullptr;
		THPAudioBuffer* b = e[head];
		head              = (head + 1) % THP_AUDIO_BUFFER_COUNT;
		count--;
		return b;
	}
};

std::mutex sMutex;
std::condition_variable sCond;
SetQueue sFree, sDecoded, sUsed;
AudioQueue sAudioFree, sAudioDecoded;
bool sAudioOn; // this movie's audio is decoded and mixed (audioExist, normal speed)
// A movie still decoding at process exit: a joinable std::thread destroyed by the exit
// handlers calls std::terminate, so the holder detaches it instead.
struct DecoderThread : std::thread {
	using std::thread::operator=;
	~DecoderThread()
	{
		if (joinable())
			detach();
	}
};
DecoderThread sDecoder;
bool sDecoderRunning;
bool sStopDecoder;
bool sFirstReady;      // first frame decoded (or failed): Prepare may return
bool sFirstOk;

// Reads the frame at `offset` (`size` bytes) and returns a pointer to it.
const u8* frameData(DVDFileInfo* fi, s32 offset, s32 size)
{
	if (ActivePlayer.onMemory)
		return ActivePlayer.movieData + (offset - (s32)ActivePlayer.header.movieDataOffsets);
	if (size <= 0 || (u32)size > align32(ActivePlayer.header.bufsize))
		return nullptr;
	u8* buf = ActivePlayer.readBuffer[0].ptr; // the SDK's read buffers are unused otherwise
	if (DVDReadPrio(fi, buf, size, offset, 2) != size)
		return nullptr;
	return buf;
}

// First word of a frame: the size of the next frame.
bool nextFrameSize(DVDFileInfo* fi, s32 offset, s32* out)
{
	if (ActivePlayer.onMemory) {
		*out = (s32)be32(ActivePlayer.movieData
		                 + (offset - (s32)ActivePlayer.header.movieDataOffsets));
		return true;
	}
	alignas(32) u8 w[32];
	if (DVDReadPrio(fi, w, 32, offset, 2) != 32)
		return false;
	*out = (s32)be32(w);
	return true;
}

// Returns false if the decoder was asked to stop while waiting for an audio buffer.
bool decodeFrame(const u8* data, THPTextureSet* set)
{
	const u32 numComp = ActivePlayer.compInfo.numComponents;
	const u8* sizes   = data + 8;
	const u8* comp    = data + 8 + numComp * 4;
	for (u32 i = 0; i < numComp; i++) {
		u32 size = be32(sizes + i * 4);
#ifdef PORT_THP_AUDIO
		if (ActivePlayer.compInfo.frameComp[i] == 1 && sAudioOn) {
			// THPAudioDecode.c AudioDecode: wait for a free buffer (the SDK blocks
			// in PopFreeAudioBuffer), decode this track, queue it.
			THPAudioBuffer* buf;
			{
				std::unique_lock<std::mutex> lk(sMutex);
				sCond.wait(lk, [] { return sStopDecoder || sAudioFree.count != 0; });
				if (sStopDecoder)
					return false;
				buf = sAudioFree.pop();
			}
			u32 n = THPAudioDecode(buf->buffer, comp + size * ActivePlayer.curAudioTrack, 0);
			if (n > ActivePlayer.header.audioMaxSamples)
				n = ActivePlayer.header.audioMaxSamples;
			{
				std::lock_guard<std::mutex> lk(sMutex);
				buf->validSample = n;
				buf->curPtr      = buf->buffer;
				sAudioDecoded.push(buf);
			}
		}
#endif
		if (ActivePlayer.compInfo.frameComp[i] == 0) {
			int err = port_thp_video_decode(comp, size, set->ytexture, set->utexture,
			                                set->vtexture, ActivePlayer.videoInfo.xSize,
			                                ActivePlayer.videoInfo.ySize);
			// The SDK sets videoError here (state 5). The port keeps playing so a
			// decoder problem shows as a stale picture instead of a stuck director.
			static int sLogged;
			if (err != 0 && sLogged++ < 5)
				std::printf("[port] THP video decode error %d\n", err);
		}
		comp += size;
	}
	return true;
}

void decoderMain()
{
	DVDFileInfo fi    = ActivePlayer.fileInfo; // private copy: DVDReadPrio writes its state
	s32 frame         = 0;                     // relative frame number, as in the SDK
	s32 offset        = ActivePlayer.initOffset;
	s32 size          = ActivePlayer.initReadSize;
	const s32 numFrames = (s32)ActivePlayer.header.numFrames;
	const bool loop   = (ActivePlayer.playFlag & 1) != 0;
	bool more         = true;

	while (more) {
		THPTextureSet* set;
		{
			std::unique_lock<std::mutex> lk(sMutex);
			sCond.wait(lk, [] { return sStopDecoder || sFree.count != 0; });
			if (sStopDecoder)
				return;
			set = sFree.pop();
		}

		const u8* data = frameData(&fi, offset, size);
		if (data == nullptr) {
			std::printf("[port] THP read failed: frame %d, offset 0x%x, size 0x%x\n", frame,
			            offset, size);
			ActivePlayer.dvdError = -1;
			std::lock_guard<std::mutex> lk(sMutex);
			sFirstReady = true;
			sCond.notify_all();
			break;
		}
		if (!decodeFrame(data, set))
			return; // stop requested
		set->frameNumber = frame;
		s32 nextSize     = (s32)be32(data);
		{
			std::lock_guard<std::mutex> lk(sMutex);
			sDecoded.push(set);
			if (!sFirstReady) {
				sFirstReady = true;
				sFirstOk    = true;
			}
			sCond.notify_all();
		}

		// Advance like THPRead.c's Reader; SMS_PORT_THP_SPEED skips frames but
		// always lands on the last one of a non-looping movie.
		s32 steps = speed();
		if (!loop && frame + ActivePlayer.initReadFrame + steps > numFrames - 1)
			steps = numFrames - 1 - (frame + ActivePlayer.initReadFrame);
		if (steps < 1)
			steps = 1;
		for (s32 i = 0; i < steps; i++) {
			if (i > 0 && !nextFrameSize(&fi, offset, &nextSize)) {
				ActivePlayer.dvdError = -1;
				more                  = false;
				break;
			}
			s32 cur = (frame + ActivePlayer.initReadFrame) % numFrames;
			offset += size;
			size = nextSize;
			if (cur == numFrames - 1) {
				if (!loop) {
					more = false; // the SDK suspends its threads here
					break;
				}
				offset = (s32)ActivePlayer.header.movieDataOffsets;
			}
			frame++;
		}
	}

	std::unique_lock<std::mutex> lk(sMutex);
	sCond.wait(lk, [] { return sStopDecoder; });
}

void stopDecoder()
{
	if (!sDecoderRunning)
		return;
	{
		std::lock_guard<std::mutex> lk(sMutex);
		sStopDecoder = true;
	}
	sCond.notify_all();
	sDecoder.join();
	sDecoderRunning = false;
}

// Starts the decoder and waits for the first frame (THPPlayerPrepare).
bool startDecoder()
{
	stopDecoder();
	{
		std::lock_guard<std::mutex> lk(sMutex);
		sFree.clear();
		sDecoded.clear();
		sUsed.clear();
		for (int i = 0; i < THP_TEXTURE_SET_COUNT; i++)
			sFree.push(&ActivePlayer.textureSet[i]);
		sAudioFree.clear();
		sAudioDecoded.clear();
#ifdef PORT_THP_AUDIO
		sAudioOn = ActivePlayer.audioExist && speed() == 1;
#else
		sAudioOn = false;
#endif
		if (sAudioOn)
			for (int i = 0; i < THP_AUDIO_BUFFER_COUNT; i++) {
				ActivePlayer.audioBuffer[i].validSample = 0;
				sAudioFree.push(&ActivePlayer.audioBuffer[i]); // THPPlayerSetBuffer's PushFreeAudioBuffer
			}
		sStopDecoder = false;
		sFirstReady  = false;
		sFirstOk     = false;
	}
	port_host_alloc_push(); // std::thread's state is host data, not a game heap block
	sDecoder = std::thread(decoderMain);
	port_host_alloc_pop();
	sDecoderRunning = true;

	// w14: wait without holding the emulated CPU (the SDK's Prepare sleeps in its DVD
	// reads too). On a cold file cache the first frame took 220 ms, which froze the main
	// thread (and the frame on screen) for as long.
	const int began = port_blocking_begin();
	bool ok;
	{
		std::unique_lock<std::mutex> lk(sMutex);
		sCond.wait(lk, [] { return sFirstReady; });
		ok = sFirstOk;
	}
	port_blocking_end(began);
	return ok;
}

THPTextureSet* popDecoded()
{
	std::lock_guard<std::mutex> lk(sMutex);
	return sDecoded.pop();
}

void pushUsed(THPTextureSet* set)
{
	std::lock_guard<std::mutex> lk(sMutex);
	sUsed.push(set);
}

void releaseUsed()
{
	{
		std::lock_guard<std::mutex> lk(sMutex);
		while (THPTextureSet* s = sUsed.pop())
			sFree.push(s);
	}
	sCond.notify_all();
}

// ---------------------------------------------------------------------------

bool properTimingForStart()
{
	if (ActivePlayer.videoInfo.videoType & 1)
		return VIGetNextField() == 0;
	if (ActivePlayer.videoInfo.videoType & 2)
		return VIGetNextField() == 1;
	return true;
}

bool properTimingForGettingNextFrame()
{
	if (ActivePlayer.videoInfo.videoType & 1)
		return VIGetNextField() == 0;
	if (ActivePlayer.videoInfo.videoType & 2)
		return VIGetNextField() == 1;
	s32 frameRate = (s32)(ActivePlayer.header.frameRate * 100.0f);
	if (VIGetTvFormat() == VI_PAL)
		ActivePlayer.curCount = (s32)(ActivePlayer.retaceCount * frameRate / 5000);
	else
		ActivePlayer.curCount = (s32)(ActivePlayer.retaceCount * frameRate / 5994);
	if (ActivePlayer.prevCount != ActivePlayer.curCount) {
		ActivePlayer.prevCount = ActivePlayer.curCount;
		return true;
	}
	return false;
}

void playControl(u32 retraceCnt)
{
	if (sOldVIPostCallback != nullptr)
		sOldVIPostCallback(retraceCnt);

	if (!ActivePlayer.open || ActivePlayer.state != 2)
		return;
	if (ActivePlayer.dvdError || ActivePlayer.videoError) {
		ActivePlayer.internalState = 5;
		ActivePlayer.state         = 5;
		return;
	}

	THPTextureSet* decoded = (THPTextureSet*)-1;
	++ActivePlayer.retaceCount;
	if (sAudioOn) {
		// THPPlayer.c PlayControl with audio: the picture only advances while it
		// is at most one frame ahead of the audio MixAudio has started playing.
		if (ActivePlayer.retaceCount == 0) {
			if (properTimingForStart()) {
				if (ActivePlayer.curVideoNumber - ActivePlayer.curAudioNumber <= 1) {
					decoded = popDecoded();
					ActivePlayer.videoDecodeCount--;
					ActivePlayer.curVideoNumber++;
				} else {
					ActivePlayer.internalState = 2;
				}
			} else {
				ActivePlayer.retaceCount = -1;
			}
		} else {
			if (ActivePlayer.retaceCount == 1)
				ActivePlayer.internalState = 2;
			if (properTimingForGettingNextFrame()
			    && ActivePlayer.curVideoNumber - ActivePlayer.curAudioNumber <= 1) {
				decoded = popDecoded();
				ActivePlayer.videoDecodeCount--;
				ActivePlayer.curVideoNumber++;
			}
		}
	} else if (ActivePlayer.retaceCount == 0) {
		if (properTimingForStart())
			decoded = popDecoded();
		else
			ActivePlayer.retaceCount = -1;
	} else {
		if (ActivePlayer.retaceCount == 1)
			ActivePlayer.internalState = 2;
		if (properTimingForGettingNextFrame())
			decoded = popDecoded();
	}

	if (decoded != nullptr && decoded != (THPTextureSet*)-1) {
		if (ActivePlayer.dispTextureSet != nullptr)
			pushUsed(ActivePlayer.dispTextureSet);
		ActivePlayer.dispTextureSet = decoded;
		if (!sAudioOn) {
			ActivePlayer.curVideoNumber++;
			ActivePlayer.curAudioNumber = ActivePlayer.curVideoNumber; // no audio: lockstep
		}
	}

	if ((ActivePlayer.playFlag & 1) == 0 && sAudioOn) {
		std::lock_guard<std::mutex> lk(sMutex);
		s32 audioFrame = ActivePlayer.curAudioNumber + ActivePlayer.initReadFrame;
		if (audioFrame == (s32)ActivePlayer.header.numFrames && ActivePlayer.playAudioBuffer == nullptr) {
			ActivePlayer.internalState = 3;
			ActivePlayer.state         = 3;
		}
	} else if ((ActivePlayer.playFlag & 1) == 0) {
		s32 curFrame = ActivePlayer.dispTextureSet != nullptr
		                   ? ActivePlayer.dispTextureSet->frameNumber + ActivePlayer.initReadFrame
		                   : ActivePlayer.initReadFrame - 1;
		if (curFrame == (s32)ActivePlayer.header.numFrames - 1 && decoded == nullptr) {
			ActivePlayer.internalState = 3;
			ActivePlayer.state         = 3;
		}
	}
}

s16 sSoundBuffer[2][0x800 * 2]; // the SDK's are [2][1120] (560 frames); JAudio asks for getDacSize()/2
s32 sSoundBufferIndex;

#ifdef PORT_THP_AUDIO
// THPPlayer.c's movie audio mixer, fed by JASystem::Kernel::vframeWork (mode 3,
// mixInterleaveTrack adds the returned interleaved samples to the DAC buffer).
const u16 kVolumeTable[128] = {
	0,     2,     8,     18,    32,    50,    73,    99,    130,   164,   203,   245,   292,   343,   398,   457,
	520,   587,   658,   733,   812,   895,   983,   1074,  1170,  1269,  1373,  1481,  1592,  1708,  1828,  1952,
	2080,  2212,  2348,  2488,  2632,  2781,  2933,  3090,  3250,  3415,  3583,  3756,  3933,  4114,  4298,  4487,
	4680,  4877,  5079,  5284,  5493,  5706,  5924,  6145,  6371,  6600,  6834,  7072,  7313,  7559,  7809,  8063,
	8321,  8583,  8849,  9119,  9394,  9672,  9954,  10241, 10531, 10826, 11125, 11427, 11734, 12045, 12360, 12679,
	13002, 13329, 13660, 13995, 14335, 14678, 15025, 15377, 15732, 16092, 16456, 16823, 17195, 17571, 17951, 18335,
	18723, 19115, 19511, 19911, 20316, 20724, 21136, 21553, 21974, 22398, 22827, 23260, 23696, 24137, 24582, 25031,
	25484, 25941, 26402, 26868, 27337, 27810, 28288, 28769, 29255, 29744, 30238, 30736, 31238, 31744, 32254, 32768,
};
void mixAudio(s16* destination, u32 sample)
{
	if (!(ActivePlayer.open && ActivePlayer.internalState == 2 && ActivePlayer.audioExist && sAudioOn)) {
		std::memset(destination, 0, sample * 4);
		return;
	}
	u32 requestSample = sample;
	s16* dst          = destination;
	std::lock_guard<std::mutex> lk(sMutex);
	for (;;) {
		u32 sampleNum;
		do {
			if (ActivePlayer.playAudioBuffer == nullptr) {
				ActivePlayer.playAudioBuffer = sAudioDecoded.pop();
				if (ActivePlayer.playAudioBuffer == nullptr) {
					std::memset(dst, 0, requestSample * 4);
					return;
				}
				ActivePlayer.curAudioNumber++;
			}
			sampleNum = ActivePlayer.playAudioBuffer->validSample;
			if (sampleNum == 0) { // empty buffer: hand it back (the SDK would spin on it)
				sAudioFree.push(ActivePlayer.playAudioBuffer);
				ActivePlayer.playAudioBuffer = nullptr;
			}
		} while (sampleNum == 0);

		if (sampleNum > requestSample)
			sampleNum = requestSample;
		s16* curPtr = ActivePlayer.playAudioBuffer->curPtr;
		for (u32 i = 0; i < sampleNum; i++) {
			if (ActivePlayer.rampCount != 0) {
				ActivePlayer.rampCount--;
				ActivePlayer.curVolume += ActivePlayer.deltaVolume;
			} else {
				ActivePlayer.curVolume = ActivePlayer.targetVolume;
			}
			s32 vi = (s32)ActivePlayer.curVolume;
			if (vi < 0)
				vi = 0;
			if (vi > 127)
				vi = 127;
			u16 attenuation = kVolumeTable[vi];
			s32 l_mix       = attenuation * curPtr[0] >> 15;
			s32 r_mix       = attenuation * curPtr[1] >> 15;
			dst[0]          = (s16)(l_mix < -32768 ? -32768 : l_mix > 32767 ? 32767 : l_mix);
			dst[1]          = (s16)(r_mix < -32768 ? -32768 : r_mix > 32767 ? 32767 : r_mix);
			dst += 2;
			curPtr += 2;
		}
		requestSample -= sampleNum;
		ActivePlayer.playAudioBuffer->validSample -= sampleNum;
		ActivePlayer.playAudioBuffer->curPtr = curPtr;
		if (ActivePlayer.playAudioBuffer->validSample == 0) {
			sAudioFree.push(ActivePlayer.playAudioBuffer);
			ActivePlayer.playAudioBuffer = nullptr;
			sCond.notify_all(); // the decoder may be waiting for a free buffer
		}
		if (requestSample == 0)
			return;
	}
}

s16* audioCallbackWithMSound(s32 samples)
{
	if (!ActivePlayer.open || ActivePlayer.internalState != 2 || !ActivePlayer.audioExist || !sAudioOn)
		return nullptr;
	if (samples > 0x800)
		samples = 0x800;
	sSoundBufferIndex ^= 1;
	mixAudio(sSoundBuffer[sSoundBufferIndex], (u32)samples);
	return sSoundBuffer[sSoundBufferIndex];
}

void initAudio() { JASystem::Kernel::registerMixCallback(&audioCallbackWithMSound, 3); }
void quitAudio() { JASystem::Kernel::registerMixCallback(nullptr, 0); }
#else
void initAudio() { }
void quitAudio() { }
#endif

#ifdef SMS_PORT_AURORA
// THPDraw.c, on Aurora's GX.
void gxYuv2RgbSetup(GXRenderModeObj* rmode)
{
	if (rmode) {
		s32 w = rmode->fbWidth;
		s32 h = rmode->efbHeight;
		Mtx44 m;
		Mtx e_m;

		GXSetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
		C_MTXOrtho(m, 0.0f, (f32)h, 0.0f, (f32)w, 0.0f, -1.0f);
		GXSetProjection(m, GX_ORTHOGRAPHIC);
		GXSetViewport(0.0f, 0.0f, (f32)w, (f32)h, 0.0f, 1.0f);
		GXSetScissor(0, 0, (u32)w, (u32)h);
		PSMTXIdentity(e_m);
		GXLoadPosMtxImm(e_m, GX_PNMTX0);
		GXSetCurrentMtx(GX_PNMTX0);
	}

	GXSetZMode(GX_TRUE, GX_ALWAYS, GX_FALSE);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
	GXSetColorUpdate(GX_TRUE);
	GXSetAlphaUpdate(GX_FALSE);
	if (rmode)
		GXSetDispCopyGamma(GX_GM_1_0);
	GXSetNumChans(0);

	GXSetNumTexGens(2);
	GXSetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GXSetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
	GXInvalidateTexAll();

	GXClearVtxDesc();
	GXSetVtxDesc(GX_VA_POS, GX_DIRECT);
	GXSetVtxDesc(GX_VA_TEX0, GX_DIRECT);
	// The SDK passes GX_CLR_RGBA / GX_RGBA4 and GX_CLR_RGBA / GX_RGBX8: the same
	// values as GX_POS_XYZ / GX_S16 and GX_TEX_ST / GX_U16.
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_S16, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_U16, 0);

	GXSetNumTevStages(4);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR_NULL);
	GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_C0);
	GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
	GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_A0);
	GXSetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
	GXSetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K0);
	GXSetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
	GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);

	GXSetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP2, GX_COLOR_NULL);
	GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, GX_CC_CPREV);
	GXSetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_2, GX_FALSE, GX_TEVPREV);
	GXSetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_TEXA, GX_CA_KONST, GX_CA_APREV);
	GXSetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
	GXSetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);
	GXSetTevKAlphaSel(GX_TEVSTAGE1, GX_TEV_KASEL_K1_A);
	GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);

	GXSetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
	GXSetTevColorIn(GX_TEVSTAGE2, GX_CC_ZERO, GX_CC_TEXC, GX_CC_ONE, GX_CC_CPREV);
	GXSetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GXSetTevAlphaIn(GX_TEVSTAGE2, GX_CA_TEXA, GX_CA_ZERO, GX_CA_ZERO, GX_CA_APREV);
	GXSetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GXSetTevSwapMode(GX_TEVSTAGE2, GX_TEV_SWAP0, GX_TEV_SWAP0);

	GXSetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL);
	GXSetTevColorIn(GX_TEVSTAGE3, GX_CC_APREV, GX_CC_CPREV, GX_CC_KONST, GX_CC_ZERO);
	GXSetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GXSetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
	GXSetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
	GXSetTevSwapMode(GX_TEVSTAGE3, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GXSetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K2);

	GXColorS10 c0 = { -90, 0, -114, 135 };
	GXColor k0    = { 0, 0, 226, 88 };
	GXColor k1    = { 179, 0, 0, 182 };
	GXColor k2    = { 255, 0, 255, 128 };
	GXSetTevColorS10(GX_TEVREG0, c0);
	GXSetTevKColor(GX_KCOLOR0, k0);
	GXSetTevKColor(GX_KCOLOR1, k1);
	GXSetTevKColor(GX_KCOLOR2, k2);
	GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
}

void gxYuv2RgbDraw(u8* y_data, u8* u_data, u8* v_data, s16 x, s16 y, s16 textureWidth,
                   s16 textureHeight, s16 polygonWidth, s16 polygonHeight)
{
	GXTexObj tobj0, tobj1, tobj2;

	GXInitTexObj(&tobj0, y_data, (u16)textureWidth, (u16)textureHeight, GX_TF_I8, GX_CLAMP,
	             GX_CLAMP, GX_FALSE);
	GXInitTexObjLOD(&tobj0, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GXLoadTexObj(&tobj0, GX_TEXMAP0);
	GXInitTexObj(&tobj1, u_data, (u16)(textureWidth >> 1), (u16)(textureHeight >> 1), GX_TF_I8,
	             GX_CLAMP, GX_CLAMP, GX_FALSE);
	GXInitTexObjLOD(&tobj1, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GXLoadTexObj(&tobj1, GX_TEXMAP1);
	GXInitTexObj(&tobj2, v_data, (u16)(textureWidth >> 1), (u16)(textureHeight >> 1), GX_TF_I8,
	             GX_CLAMP, GX_CLAMP, GX_FALSE);
	GXInitTexObjLOD(&tobj2, GX_NEAR, GX_NEAR, 0, 0, 0, GX_FALSE, GX_FALSE, GX_ANISO_1);
	GXLoadTexObj(&tobj2, GX_TEXMAP2);

	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	GXPosition3s16(x, y, 0);
	GXTexCoord2u16(0, 0);
	GXPosition3s16((s16)(x + polygonWidth), y, 0);
	GXTexCoord2u16(1, 0);
	GXPosition3s16((s16)(x + polygonWidth), (s16)(y + polygonHeight), 0);
	GXTexCoord2u16(1, 1);
	GXPosition3s16(x, (s16)(y + polygonHeight), 0);
	GXTexCoord2u16(0, 1);
	GXEnd();
}

void gxRestore()
{
	GXSetZMode(GX_TRUE, GX_ALWAYS, GX_FALSE);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_SET);
	GXSetNumTexGens(1);
	GXSetNumChans(0);
	GXSetNumTevStages(1);
	GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR_NULL);
	GXSetTevOp(GX_TEVSTAGE0, GX_REPLACE);
	GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GXSetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GXSetTevSwapMode(GX_TEVSTAGE2, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GXSetTevSwapMode(GX_TEVSTAGE3, GX_TEV_SWAP0, GX_TEV_SWAP0);
	GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
	GXSetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
	GXSetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
	GXSetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
}
#endif // SMS_PORT_AURORA

} // namespace

extern "C" {

BOOL THPPlayerInit(s32)
{
	stopDecoder();
	std::memset(&ActivePlayer, 0, sizeof(ActivePlayer));
	BOOL en           = OSDisableInterrupts();
	sSoundBufferIndex = 0;
	initAudio(); // THPPlayer.c: JAudio mix callback, mode 3
	OSRestoreInterrupts(en);
	std::memset(sSoundBuffer, 0, sizeof(sSoundBuffer));
	sInitialized = TRUE;
	return TRUE;
}

void THPPlayerQuit()
{
	stopDecoder();
	quitAudio();
	sInitialized = FALSE;
}

BOOL THPPlayerOpen(const char* fileName, BOOL onMemory)
{
	if (!sInitialized || ActivePlayer.open)
		return FALSE;

	std::memset(&ActivePlayer.videoInfo, 0, sizeof(THPVideoInfo));
	std::memset(&ActivePlayer.audioInfo, 0, sizeof(THPAudioInfo));

	if (!DVDOpen((char*)fileName, &ActivePlayer.fileInfo))
		return FALSE;

	alignas(32) u8 work[64];
	if (!readAt(work, 64, 0)) {
		DVDClose(&ActivePlayer.fileInfo);
		return FALSE;
	}
	THPHeader& h = ActivePlayer.header;
	std::memcpy(h.magic, work, 4);
	h.version               = be32(work + 0x04);
	h.bufsize               = be32(work + 0x08);
	h.audioMaxSamples       = be32(work + 0x0C);
	h.frameRate             = bef32(work + 0x10);
	h.numFrames             = be32(work + 0x14);
	h.firstFrameSize        = be32(work + 0x18);
	h.movieDataSize         = be32(work + 0x1C);
	h.compInfoDataOffsets   = be32(work + 0x20);
	h.offsetDataOffsets     = be32(work + 0x24);
	h.movieDataOffsets      = be32(work + 0x28);
	h.finalFrameDataOffsets = be32(work + 0x2C);

	if (std::strcmp(h.magic, "THP") != 0 || h.version != 0x11000) {
		DVDClose(&ActivePlayer.fileInfo);
		return FALSE;
	}

	s32 offset = (s32)h.compInfoDataOffsets;
	if (!readAt(work, 32, offset)) {
		DVDClose(&ActivePlayer.fileInfo);
		return FALSE;
	}
	ActivePlayer.compInfo.numComponents = be32(work);
	std::memcpy(ActivePlayer.compInfo.frameComp, work + 4, 16);
	offset += sizeof(THPFrameCompInfo);
	ActivePlayer.audioExist = 0;

	for (u32 i = 0; i < ActivePlayer.compInfo.numComponents; i++) {
		switch (ActivePlayer.compInfo.frameComp[i]) {
		case 0:
			if (!readAt(work, 32, offset)) {
				DVDClose(&ActivePlayer.fileInfo);
				return FALSE;
			}
			ActivePlayer.videoInfo.xSize     = be32(work + 0);
			ActivePlayer.videoInfo.ySize     = be32(work + 4);
			ActivePlayer.videoInfo.videoType = be32(work + 8);
			offset += sizeof(THPVideoInfo);
			break;
		case 1:
			if (!readAt(work, 32, offset)) {
				DVDClose(&ActivePlayer.fileInfo);
				return FALSE;
			}
			ActivePlayer.audioInfo.sndChannels   = be32(work + 0);
			ActivePlayer.audioInfo.sndFrequency  = be32(work + 4);
			ActivePlayer.audioInfo.sndNumSamples = be32(work + 8);
			ActivePlayer.audioInfo.sndNumTracks  = be32(work + 12);
			offset += sizeof(THPAudioInfo);
			ActivePlayer.audioExist = 1;
			break;
		default:
			return FALSE;
		}
	}

	std::printf("[port] THP open %s: %ux%u, %u frames at %.2f fps", fileName, ActivePlayer.videoInfo.xSize,
	            ActivePlayer.videoInfo.ySize, h.numFrames, h.frameRate);
	if (ActivePlayer.audioExist)
		std::printf(", audio %u ch %u Hz, %u track(s)%s", ActivePlayer.audioInfo.sndChannels,
		            ActivePlayer.audioInfo.sndFrequency, ActivePlayer.audioInfo.sndNumTracks,
		            speed() == 1 ? "" : " (off: SMS_PORT_THP_SPEED)");
	std::printf("\n");

	ActivePlayer.internalState = 0;
	ActivePlayer.state         = 0;
	ActivePlayer.playFlag      = 0;
	ActivePlayer.onMemory      = onMemory;
	ActivePlayer.open          = TRUE;
	ActivePlayer.curVolume     = 127.0f;
	ActivePlayer.targetVolume  = ActivePlayer.curVolume;
	ActivePlayer.rampCount     = 0;
	return TRUE;
}

BOOL THPPlayerClose()
{
	if (ActivePlayer.open && ActivePlayer.state == 0) {
		ActivePlayer.open = FALSE;
		DVDClose(&ActivePlayer.fileInfo);
		return TRUE;
	}
	return FALSE;
}

u32 THPPlayerCalcNeedMemory()
{
	if (!ActivePlayer.open)
		return 0;
	u32 xy   = ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize;
	u32 size = ActivePlayer.onMemory ? align32(ActivePlayer.header.movieDataSize)
	                                 : align32(ActivePlayer.header.bufsize) * 10;
	size += align32(xy) * 3;
	size += align32(xy / 4) * 3;
	size += align32(xy / 4) * 3;
	if (ActivePlayer.audioExist)
		size += align32(ActivePlayer.header.audioMaxSamples * 4) * THP_AUDIO_BUFFER_COUNT;
	return size + 0x1000;
}

BOOL THPPlayerSetBuffer(u8* buffer)
{
	if (!ActivePlayer.open || ActivePlayer.state != 0)
		return FALSE;
	u8* ptr = buffer;
	if (ActivePlayer.onMemory) {
		ActivePlayer.movieData = buffer;
		ptr += ActivePlayer.header.movieDataSize;
	} else {
		for (int i = 0; i < THP_READ_BUFFER_COUNT; i++) {
			ActivePlayer.readBuffer[i].ptr = ptr;
			ptr += align32(ActivePlayer.header.bufsize);
		}
	}
	u32 xy     = ActivePlayer.videoInfo.xSize * ActivePlayer.videoInfo.ySize;
	u32 ysize  = align32(xy);
	u32 uvsize = align32(xy / 4);
	for (int i = 0; i < THP_TEXTURE_SET_COUNT; i++) {
		ActivePlayer.textureSet[i].ytexture = ptr;
		ptr += ysize;
		ActivePlayer.textureSet[i].utexture = ptr;
		ptr += uvsize;
		ActivePlayer.textureSet[i].vtexture = ptr;
		ptr += uvsize;
	}
	if (ActivePlayer.audioExist) {
		for (int i = 0; i < THP_AUDIO_BUFFER_COUNT; i++) {
			ActivePlayer.audioBuffer[i].buffer      = (s16*)ptr;
			ActivePlayer.audioBuffer[i].curPtr      = (s16*)ptr;
			ActivePlayer.audioBuffer[i].validSample = 0;
			ptr += align32(ActivePlayer.header.audioMaxSamples * 4);
		}
	}
	ActivePlayer.thpWork = ptr;
	return TRUE;
}

BOOL THPPlayerPrepare(s32 frame, u8 flag, s32 audioTrack)
{
	if (!ActivePlayer.open || ActivePlayer.state != 0)
		return FALSE;

	if (frame > 0) {
		if (ActivePlayer.header.offsetDataOffsets == 0
		    || (s32)ActivePlayer.header.numFrames <= frame)
			return FALSE;
		alignas(32) u8 work[32];
		if (!readAt(work, 32, ActivePlayer.header.offsetDataOffsets + (frame - 1) * 4))
			return FALSE;
		ActivePlayer.initOffset    = ActivePlayer.header.movieDataOffsets + be32(work);
		ActivePlayer.initReadFrame = frame;
		ActivePlayer.initReadSize  = be32(work + 4) - be32(work);
	} else {
		ActivePlayer.initOffset    = ActivePlayer.header.movieDataOffsets;
		ActivePlayer.initReadSize  = ActivePlayer.header.firstFrameSize;
		ActivePlayer.initReadFrame = frame;
	}

	if (ActivePlayer.audioExist) {
		if (audioTrack < 0 || audioTrack >= (s32)ActivePlayer.audioInfo.sndNumTracks)
			return FALSE;
		ActivePlayer.curAudioTrack = audioTrack;
	}

	ActivePlayer.playFlag         = flag & 1;
	ActivePlayer.videoDecodeCount = 0;

	if (ActivePlayer.onMemory
	    && !readAt(ActivePlayer.movieData, ActivePlayer.header.movieDataSize,
	               ActivePlayer.header.movieDataOffsets))
		return FALSE;

	ActivePlayer.dvdError   = 0;
	ActivePlayer.videoError = 0;
	// The SDK starts its read/decode threads here and waits for the first
	// decoded frame (PrepareReady); a read failure makes Prepare fail.
	if (!startDecoder()) {
		stopDecoder();
		return FALSE;
	}

	ActivePlayer.state           = 1;
	ActivePlayer.internalState   = 0;
	ActivePlayer.dispTextureSet  = nullptr;
	ActivePlayer.playAudioBuffer = nullptr;
	ActivePlayer.curVideoNumber  = 0;
	ActivePlayer.curAudioNumber  = 0;

	sOldVIPostCallback = VISetPostRetraceCallback(playControl);
	return TRUE;
}

BOOL THPPlayerPlay()
{
	if (ActivePlayer.open && (ActivePlayer.state == 1 || ActivePlayer.state == 4)) {
		ActivePlayer.state       = 2;
		ActivePlayer.prevCount   = 0;
		ActivePlayer.curCount    = 0;
		ActivePlayer.retaceCount = -1;
		return TRUE;
	}
	return FALSE;
}

void THPPlayerStop()
{
	if (ActivePlayer.open && ActivePlayer.state != 0) {
		std::printf("[port] THP stop: state %d, frame %d of %u\n", ActivePlayer.state,
		            ActivePlayer.dispTextureSet != nullptr
		                ? ActivePlayer.dispTextureSet->frameNumber + ActivePlayer.initReadFrame
		                : -1,
		            ActivePlayer.header.numFrames);
		ActivePlayer.internalState = 0;
		ActivePlayer.state         = 0;
		VISetPostRetraceCallback(sOldVIPostCallback);
		stopDecoder(); // the SDK cancels its read/decode threads here
		if (ActivePlayer.audioExist)
			quitAudio(); // as the SDK does (THPPlayerInit registers it again)
		{
			std::lock_guard<std::mutex> lk(sMutex);
			sUsed.clear();
			sAudioDecoded.clear();
			ActivePlayer.playAudioBuffer = nullptr;
			sAudioOn                     = false;
		}
		ActivePlayer.curVolume  = ActivePlayer.targetVolume;
		ActivePlayer.rampCount  = 0;
		ActivePlayer.dvdError   = 0;
		ActivePlayer.videoError = 0;
	}
}

BOOL THPPlayerPause()
{
	if (ActivePlayer.open && ActivePlayer.state == 2) {
		ActivePlayer.internalState = 4;
		ActivePlayer.state         = 4;
		return TRUE;
	}
	return FALSE;
}

s32 THPPlayerDrawCurrentFrame(GXRenderModeObj* rmode, u32 x, u32 y, u32 polygonW, u32 polygonH)
{
	if (ActivePlayer.open && ActivePlayer.state != 0 && ActivePlayer.dispTextureSet != nullptr) {
#ifdef SMS_PORT_AURORA
		// w6: widescreen keeps movies 4:3, pillarboxed: the quad is squeezed by 1/scale around
		// the centre of the 640-wide frame it is drawn in (the movie director's full-frame
		// ortho, or gxYuv2RgbSetup's). Exact no-op at 4:3.
		float qx = (float)x, qw = (float)polygonW;
		PORTWideSqueezeRectX(&qx, &qw);
		gxYuv2RgbSetup(rmode);
		gxYuv2RgbDraw(ActivePlayer.dispTextureSet->ytexture, ActivePlayer.dispTextureSet->utexture,
		              ActivePlayer.dispTextureSet->vtexture, (s16)std::lround(qx), (s16)y,
		              (s16)ActivePlayer.videoInfo.xSize, (s16)ActivePlayer.videoInfo.ySize,
		              (s16)std::lround(qw), (s16)polygonH);
		gxRestore();
#else
		(void)rmode, (void)x, (void)y, (void)polygonW, (void)polygonH;
#endif
		return (ActivePlayer.dispTextureSet->frameNumber + ActivePlayer.initReadFrame)
		       % (s32)ActivePlayer.header.numFrames;
	}
	return -1;
}

BOOL THPPlayerGetVideoInfo(THPVideoInfo* videoInfo)
{
	if (!ActivePlayer.open)
		return FALSE;
	std::memcpy(videoInfo, &ActivePlayer.videoInfo, sizeof(THPVideoInfo));
	return TRUE;
}

BOOL THPPlayerGetAudioInfo(THPAudioInfo* audioInfo)
{
	if (!ActivePlayer.open)
		return FALSE;
	std::memcpy(audioInfo, &ActivePlayer.audioInfo, sizeof(THPAudioInfo));
	return TRUE;
}

u32 THPPlayerGetTotalFrame() { return ActivePlayer.open ? ActivePlayer.header.numFrames : 0; }

s32 THPPlayerGetState() { return ActivePlayer.state; }

void THPPlayerDrawDone()
{
	// w14: the game calls this every frame (TApplication::gameLoop), so the main thread
	// waits here for Aurora's FIFO worker each frame (SMS_PORT_PROFILE "fifo"); a
	// higher-priority thread may run meanwhile (port_preemptible_begin).
	const int preemptible = port_preemptible_begin();
	{
		PORT_PROF_SCOPE(PP_FIFO);
		GXDrawDone(); // on Aurora this drains the FIFO, so used textures were consumed
	}
	port_preemptible_end(preemptible);
	if (sInitialized)
		releaseUsed();
}

void THPPlayerPostDrawDone()
{
	if (sInitialized)
		releaseUsed();
}

BOOL THPPlayerSetVolume(s32 vol, s32 duration)
{
	if (!ActivePlayer.open || !ActivePlayer.audioExist)
		return FALSE;
	if (vol > 127)
		vol = 127;
	if (vol < 0)
		vol = 0;
	if (duration > 60000)
		duration = 60000;
	if (duration < 0)
		duration = 0;
	BOOL en                   = OSDisableInterrupts();
	ActivePlayer.targetVolume = (f32)vol;
	if (duration != 0) {
		ActivePlayer.rampCount   = 32 * duration; // AIGetDSPSampleRate() == 0 (32 kHz)
		ActivePlayer.deltaVolume = (ActivePlayer.targetVolume - ActivePlayer.curVolume)
		                           / ActivePlayer.rampCount;
	} else {
		ActivePlayer.rampCount = 0;
		ActivePlayer.curVolume = ActivePlayer.targetVolume;
	}
	OSRestoreInterrupts(en);
	return TRUE;
}

} // extern "C"
