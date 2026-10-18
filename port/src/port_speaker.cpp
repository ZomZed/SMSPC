// DualSense speaker for the Aurora build (w9): selected game sounds on the controller's own
// speaker. Settings ([audio] / F1 Audio tab): controller_speaker = off | fludd | fludd_pickups,
// main_mix = keep | duck | remove (what happens to those sounds on the PC speakers),
// speaker_volume (%).
//
// Which sounds: a sound effect plays on a track of JAudio's SE sequence; JAI keeps the
// playing sound of every SE track slot (JAIData::mSeTrack[category][slot].mSound, its track
// number JAISound::mTrack, set in JAIBasic::sendPlayingSeCommand). Once per game frame the
// game thread maps each playing SE track (JAISystemInterface::trackToSeqp, and its child
// tracks) to its sound id, keyed by the track's channel manager. In w5's DSP mixer
// (port_audio_dsp.cpp, a marked tap) every voice is traced back to its owner: TDSPChannel
// (DSPCH[i]) -> mSign = the TChannel that allocated it -> its TChannelMgr -> sound id. Voices
// of the chosen sounds (FLUDD: the PO_* sounds 0x0000-0x0825 incl. spray, hover, rocket
// trigger, nozzle switch, empty sputter, refill, pump; SY_PO_* air pressure / water full;
// NEWP_AIR_TAME rocket charge; pickups: coins, red/blue coins, 1-up, Shine, nozzle/pump get)
// are captured from their front-bus contribution (so the game's own volume, pan and fades
// apply), and are kept, ducked (x0.35) or removed in the main mix.
//
// Output (the stream port_haptics.cpp opens for a DualSense):
//  - USB: channels 1-2 of the controller's 4-channel audio device (48 kHz);
//  - Bluetooth: HID report 0x36 (398 bytes, one per 10.667 ms): byte 1 = sequence << 4;
//    sub-packets 0x11 config (0x91, 7: FE 40 40 40 40 40, counter), 0x10 controller state
//    (0x90, 63: the effect state - our trigger effects, lightbar and rumble, the audio
//    fields as the reference stream has them), 0x12 haptics (0x92, 64 samples), 0x13
//    speaker (0x93, 200: one Opus frame), zero padding, CRC-32 over 0xA2 + bytes 0-393 at
//    394. Opus: 48 kHz stereo, 10 ms frames (480 samples), CBR 160 kbit/s = 200 bytes,
//    application AUDIO; each packet takes 512 samples of the 48 kHz stream resampled to 480,
//    as the reference implementation does (the controller drains 480 samples per 10.667 ms).
//    Before the first 0x36, the speaker is switched on with an effect state (SDL_SendGamepadEffect:
//    enable bits 0xA0 / 0x80, speaker volume 0x64, audio control 0x30, preamp 0x02; SDL wraps it
//    in its 0x31 report with the CRC). The same is sent over USB.
//  Protocol facts: LinuxAudio4Dualsense5 (github.com/GeorgLegato/LinuxAudio4Dualsense5,
//  AGPL-3.0; facts only, no code taken).
// While the 0x36 stream runs, its state sub-packet restates the lightbar and trigger effects
// port_padfx.cpp has set through SDL, so SDL's 0x31 reports and ours agree.
// Test runs never open a device; SMS_PORT_HAPTICS_DUMP records the reports
// (<prefix>_bt.bin) and the speaker stream (<prefix>_speaker.wav).
#ifdef SMS_PORT_AURORA

#include "port_settings.h"

#include <SDL3/SDL_gamepad.h>

#include <JSystem/JAudio/JAInterface/JAIBasic.hpp>
#include <JSystem/JAudio/JAInterface/JAIData.hpp>
#include <JSystem/JAudio/JAInterface/JAIGlobalParameter.hpp>
#include <JSystem/JAudio/JAInterface/JAISound.hpp>
#include <JSystem/JAudio/JAInterface/JAISystemInterface.hpp>
#include <JSystem/JAudio/JASystem/JASChannel.hpp>
#include <JSystem/JAudio/JASystem/JASDSPChannel.hpp>
#include <JSystem/JAudio/JASystem/JASTrack.hpp>

#ifdef PORT_HAVE_OPUS
#include <opus.h>
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

extern "C" volatile unsigned gPortFrameCount;
extern "C" bool port_haptics_stream_running(void);
extern "C" void port_padfx_ds5_state(unsigned char* l11, unsigned char* r11, int* rgb, bool* ledOn);
extern "C" int port_haptics_original_motor(void);
extern "C" int port_qol_volumes_active(void); // port_qol.cpp: music / SFX / voice volumes // 0..255, the game's motor in Original mode
uint32_t port_ds5_crc(const uint8_t* report, size_t n); // port_haptics.cpp

#ifdef PORT_HAVE_OPUS
// The encoder (repacketizer/extensions) calls this packet parser, which libopus defines in
// opus_decoder.c; only the encoder is compiled, so here it is (RFC 6716 section 3.1: TOC
// code 0 = one frame, 1/2 = two, 3 = the count in the second byte's low 6 bits).
extern "C" int opus_packet_get_nb_frames(const unsigned char packet[], opus_int32 len)
{
	if (len < 1)
		return OPUS_BAD_ARG;
	const int code = packet[0] & 0x3;
	if (code == 0)
		return 1;
	if (code != 3)
		return 2;
	if (len < 2)
		return OPUS_INVALID_PACKET;
	return packet[1] & 0x3F;
}
#endif
namespace {

const bool kLog = std::getenv("SMS_PORT_SPEAKER_LOG") != nullptr;
unsigned sTagHits, sTagTable; // SMS_PORT_SPEAKER_LOG statistics
float sPushPeak;

bool isFludd(u32 id)
{
	const u32 s = id & 0xFFFF;
	if ((s & 0xF000) == 0 && (s & 0x07FF) <= 0x25)
		return true; // PO_*: water, spray, hover, triggers, nozzle switch, empty, pump, suck
	return s == 0x4006 || s == 0x4807 || s == 0x4808 || s == 0x4809 || s == 0x4022;
}

bool isPickup(u32 id)
{
	switch (id & 0xFFFF) {
	case 0x4811: case 0x7915: case 0x4845: case 0x4846: case 0x4841: case 0x4814:
	case 0x484E: case 0x484F: case 0x4850:
		return true;
	default:
		return false;
	}
}

// ---- SE track -> sound id (game thread builds, audio thread reads) ---------------------
std::mutex sTagMutex;
std::vector<std::pair<const void*, u32>> sTags; // channel manager -> selected sound id

void addTrack(std::vector<std::pair<const void*, u32>>& out, JASystem::TTrack* t, u32 id, int depth)
{
	if (t == nullptr || depth > 3)
		return;
	out.emplace_back((const void*)&t->mChannelUpdater, id);
	for (int i = 0; i < 16; i++)
		addTrack(out, t->getChild(i), id, depth + 1);
}

void buildTags(int mode)
{
	std::vector<std::pair<const void*, u32>> tags;
	JAIBasic* b = JAIBasic::getInterface();
	const bool all = port_qol_volumes_active() != 0; // w9 QoL volumes: every SE is tagged
	if ((mode != PORT_SPEAKER_OFF || all) && b != nullptr && b->unk0 != nullptr && b->unk0->mSeTrack != nullptr
	    && b->mSeSequence != nullptr && b->mSeSequence->getSeqParameter() != nullptr) {
		u8 trackId = 0;
		for (u8 cat = 0; cat < JAIGlobalParameter::getParamSeCategoryMax(); ++cat) {
			const u8 maxPlaying = b->unk0->mCategoryInfoTable[b->mSoundScene][cat].mMaxPlaying;
			for (u8 j = 0; j < maxPlaying; ++j, ++trackId) {
				JAISound* s = b->unk0->mSeTrack[cat][j].mSound;
				if (s == nullptr)
					continue;
				const u32 id = s->mSoundID;
				if (!all && !(isFludd(id) || (mode == PORT_SPEAKER_FLUDD_PICKUPS && isPickup(id))))
					continue;
				addTrack(tags, JAISystemInterface::trackToSeqp(b->mSeSequence, s->mTrack), id, 0);
			}
		}
	}
	std::lock_guard<std::mutex> l(sTagMutex);
	sTags.swap(tags);
	sTagTable = (unsigned)sTags.size();
	static unsigned sFrames;
	if (kLog && mode != PORT_SPEAKER_OFF && (++sFrames % 60) == 0) {
		std::fprintf(stderr, "[speaker] frame %u: %u tracks tagged, %u tagged voice sub-frames, peak %.3f\n", gPortFrameCount,
		             sTagTable, sTagHits, sPushPeak);
		sTagHits  = 0;
		sPushPeak = 0.0f;
	}
}

// ---- speaker stream: DSP rate (32 kHz) ring, read at 48 kHz --------------------------------
const int kRing = 32000; // 1 s
float sRing[kRing];
int sWrite, sRead;   // positions (mod kRing)
double sFrac;        // resampler phase
std::mutex sRingMutex;
std::vector<float> sDumpSpk;
bool sActive; // a sink consumes the speaker stream (main-mix duck/remove only then)

int ringFill() { return (sWrite - sRead + kRing) % kRing; }

void ringPush(const float* x, int n)
{
	std::lock_guard<std::mutex> l(sRingMutex);
	for (int i = 0; i < n; i++) {
		sRing[sWrite] = x[i];
		sWrite        = (sWrite + 1) % kRing;
		if (sWrite == sRead)
			sRead = (sRead + 1) % kRing; // overrun: drop the oldest
	}
}

// n samples at 48 kHz (mono), with the fill kept near 40 ms (drift between the DSP clock
// and the controller's).
void ringPull48(float* out, int n)
{
	std::lock_guard<std::mutex> l(sRingMutex);
	const double step = 32000.0 / 48000.0;
	int fill          = ringFill();
	if (fill > 3200) { // > 100 ms behind: skip ahead
		sRead = (sWrite - 1280 + kRing) % kRing;
		fill  = 1280;
	}
	const double adj = fill > 1600 ? 1.002 : fill < 800 ? 0.998 : 1.0;
	for (int i = 0; i < n; i++) {
		if (ringFill() < 2) {
			out[i] = 0.0f;
			continue;
		}
		const float a = sRing[sRead], b = sRing[(sRead + 1) % kRing];
		out[i]        = a + (b - a) * (float)sFrac;
		sFrac += step * adj;
		while (sFrac >= 1.0) {
			sFrac -= 1.0;
			sRead = (sRead + 1) % kRing;
		}
	}
}

// ---- Bluetooth 0x36 ------------------------------------------------------------------------
#ifdef PORT_HAVE_OPUS
OpusEncoder* sOpus;
#endif

// The reference stream's state snapshot (63 bytes; DS5 effect-state layout), see the header.
void stateSnapshot(uint8_t* st)
{
	std::memset(st, 0, 63);
	st[0]  = 0xFD; // valid flags 0: compatible vibration, triggers R/L, volumes, audio control
	st[1]  = 0xE3; // valid flags 1
	st[4]  = 0x7F; // headphone volume
	st[5]  = 0x64; // speaker volume
	st[7]  = 0x09; // audio control
	st[9]  = 0x10; // power save: mic muted
	st[37] = 0x0A;
	st[38] = 0x04; // valid flags 2: improved vibration
	st[42] = 0x01;
	st[46] = 0xFF;
	// ours: rumble, trigger effects, lightbar
	const int motor = port_haptics_original_motor();
	st[2] = st[3] = (uint8_t)motor;
	unsigned char l[11], r[11];
	int rgb[3];
	bool led = false;
	port_padfx_ds5_state(l, r, rgb, &led);
	std::memcpy(st + 10, r, 11);
	std::memcpy(st + 21, l, 11);
	if (led) {
		st[1] |= 0x04; // lightbar colour
		st[44] = (uint8_t)rgb[0], st[45] = (uint8_t)rgb[1], st[46] = (uint8_t)rgb[2];
	}
}

void resample512to480(const float* in, float* out)
{
	for (int i = 0; i < 480; i++) {
		const double p = i * (511.0 / 479.0);
		const int k    = (int)p;
		const float f  = (float)(p - k);
		out[i]         = k + 1 < 512 ? in[k] + (in[k + 1] - in[k]) * f : in[511];
	}
}

} // namespace

extern "C" {

bool port_speaker_wanted(void) { return gPortSettings.speakerMode != PORT_SPEAKER_OFF; }

// Game thread, once per game frame.
void port_speaker_frame(void)
{
	buildTags(gPortSettings.speakerMode);
	sActive = gPortSettings.speakerMode != PORT_SPEAKER_OFF && port_haptics_stream_running();
}

// USB sink just opened / Bluetooth stream starting: switch the speaker on (SDL effect state).
void port_speaker_enable(SDL_Gamepad* gp)
{
	if (gp == nullptr || gPortSettings.speakerMode == PORT_SPEAKER_OFF)
		return;
	uint8_t st[47] = {};
	st[0]          = 0x80 | 0x20; // audio control, speaker volume
	st[1]          = 0x80;        // audio control 2
	st[5]          = 0x64;        // speaker volume
	st[7]          = 0x30;        // audio control: speaker path
	st[37]         = 0x02;        // preamp gain
	const bool ok  = SDL_SendGamepadEffect(gp, st, sizeof(st));
	std::fprintf(stderr, "[speaker] speaker enable state sent: %s\n", ok ? "ok" : SDL_GetError());
}

void port_speaker_set_active(bool on) { sActive = on && gPortSettings.speakerMode != PORT_SPEAKER_OFF; }

// port_audio_dsp.cpp tap: the selected sound id of DSP voice i, or 0.
u32 port_speaker_voice_tag(int i)
{
	if ((gPortSettings.speakerMode == PORT_SPEAKER_OFF && !port_qol_volumes_active())
	    || JASystem::TDSPChannel::DSPCH == nullptr)
		return 0;
	const JASystem::TDSPChannel& d = JASystem::TDSPChannel::DSPCH[i];
	const JASystem::TChannel* c    = (const JASystem::TChannel*)d.mSign;
	if (c == nullptr)
		return 0;
	const void* mgr = c->unk9C != nullptr ? (const void*)c->unk9C : (const void*)c->unk4;
	std::unique_lock<std::mutex> l(sTagMutex, std::try_to_lock);
	if (!l.owns_lock())
		return 0;
	for (const auto& t : sTags)
		if (t.first == mgr) {
			++sTagHits;
			return t.second;
		}
	return 0;
}

// Whether the controller speaker plays this sound id.
bool port_speaker_wants(u32 id)
{
	const int mode = gPortSettings.speakerMode;
	return mode != PORT_SPEAKER_OFF && (isFludd(id) || (mode == PORT_SPEAKER_FLUDD_PICKUPS && isPickup(id)));
}

// Main-mix gain for a tagged voice (keep 1, duck 0.35, remove 0), only while a sink plays it.
float port_speaker_main_gain(void)
{
	if (!sActive)
		return 1.0f;
	return gPortSettings.mainMix == PORT_MAINMIX_REMOVE ? 0.0f : gPortSettings.mainMix == PORT_MAINMIX_DUCK ? 0.35f : 1.0f;
}

// The tapped voices' front-bus contribution of one DSP sub-frame (32 kHz, s32 bus scale):
// summed here, pushed once per sub-frame by port_speaker_subframe_end.
static float sAcc[256];
static int sAccN;
void port_speaker_push(const int* l, const int* r, int n)
{
	if (gPortSettings.speakerMode == PORT_SPEAKER_OFF)
		return;
	const float g = gPortSettings.speakerVolume / 100.0f * (1.0f / 32768.0f) * 4.0f; // voices sit well below full scale
	for (int i = 0; i < n && i < 256; i++)
		sAcc[i] += 0.5f * (float)(l[i] + r[i]) * g;
	if (n > sAccN)
		sAccN = n < 256 ? n : 256;
}

// End of a DSP sub-frame (port_audio_dsp.cpp): the speaker stream advances by n samples,
// silence when no selected sound played.
void port_speaker_subframe_end(int n)
{
	if (gPortSettings.speakerMode == PORT_SPEAKER_OFF || !sActive) {
		sAccN = 0;
		std::memset(sAcc, 0, sizeof(sAcc));
		return;
	}
	n = n < 256 ? n : 256;
	for (int i = 0; i < n; i++) {
		const float a = sAcc[i] < 0 ? -sAcc[i] : sAcc[i];
		if (a > sPushPeak)
			sPushPeak = a;
	}
	ringPush(sAcc, n);
	std::memset(sAcc, 0, sizeof(sAcc));
	sAccN = 0;
}

void port_speaker_fill_usb(float* out4, int frames)
{
	if (gPortSettings.speakerMode == PORT_SPEAKER_OFF)
		return;
	static std::vector<float> m;
	m.resize((size_t)frames);
	ringPull48(m.data(), frames);
	for (int i = 0; i < frames; i++) {
		const float v = m[(size_t)i] > 1.0f ? 1.0f : m[(size_t)i] < -1.0f ? -1.0f : m[(size_t)i];
		out4[(size_t)i * 4]     = v;
		out4[(size_t)i * 4 + 1] = v;
	}
}

// Bluetooth thread: the 0x36 report (speaker on), or false (port_haptics sends 0x32).
bool port_speaker_bt_report(uint8_t* r, int8_t* haptics64, uint8_t seq, uint8_t counter, int* size)
{
#ifdef PORT_HAVE_OPUS
	if (gPortSettings.speakerMode == PORT_SPEAKER_OFF)
		return false;
	if (sOpus == nullptr) {
		int err = 0;
		sOpus   = opus_encoder_create(48000, 2, OPUS_APPLICATION_AUDIO, &err);
		if (sOpus == nullptr) {
			std::fprintf(stderr, "[speaker] opus_encoder_create failed: %d\n", err);
			return false;
		}
		opus_encoder_ctl(sOpus, OPUS_SET_BITRATE(160000));
		opus_encoder_ctl(sOpus, OPUS_SET_VBR(0));
		opus_encoder_ctl(sOpus, OPUS_SET_COMPLEXITY(3));
		opus_encoder_ctl(sOpus, OPUS_SET_EXPERT_FRAME_DURATION(OPUS_FRAMESIZE_10_MS));
	}
	float in[512], m480[480], st480[960];
	ringPull48(in, 512);
	resample512to480(in, m480);
	for (int i = 0; i < 480; i++) {
		const float v = m480[i] > 1.0f ? 1.0f : m480[i] < -1.0f ? -1.0f : m480[i];
		st480[2 * i] = st480[2 * i + 1] = v;
	}
	if (std::getenv("SMS_PORT_HAPTICS_DUMP") != nullptr)
		sDumpSpk.insert(sDumpSpk.end(), in, in + 512);
	unsigned char opus[400];
	int bytes = opus_encode_float(sOpus, st480, 480, opus, (opus_int32)sizeof(opus));
	if (bytes < 0)
		bytes = 0;
	if (bytes > 200)
		bytes = 200;
	std::memset(r, 0, 398);
	r[0] = 0x36;
	r[1] = (uint8_t)(seq << 4);
	r[2] = 0x91, r[3] = 7, r[4] = 0xFE;
	for (int i = 5; i <= 9; i++)
		r[i] = 0x40;
	r[10] = counter;
	r[11] = 0x90, r[12] = 63;
	stateSnapshot(r + 13);
	r[76] = 0x92, r[77] = 64;
	std::memcpy(r + 78, haptics64, 64);
	r[142] = 0x93, r[143] = 200;
	std::memcpy(r + 144, opus, (size_t)bytes);
	const uint32_t crc = port_ds5_crc(r, 394);
	r[394] = (uint8_t)crc, r[395] = (uint8_t)(crc >> 8), r[396] = (uint8_t)(crc >> 16), r[397] = (uint8_t)(crc >> 24);
	*size = 398;
	static unsigned sN;
	if (kLog && (sN++ % 470) == 0)
		std::fprintf(stderr, "[speaker] 0x36 report %u: opus %d bytes, ring %d samples\n", sN, bytes, ringFill());
	return true;
#else
	(void)r, (void)haptics64, (void)seq, (void)counter, (void)size;
	return false;
#endif
}

// Writes the dumped speaker stream (48 kHz mono as stereo WAV) next to the haptics dump.
void port_speaker_dump_close(const char* prefix)
{
	if (sDumpSpk.empty())
		return;
	std::string path = std::string(prefix) + "_speaker.wav";
	FILE* f          = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return;
	const uint32_t frames = (uint32_t)sDumpSpk.size(), data = frames * 4, rate = 48000;
	auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f), u32(36 + data), std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16), u16(1), u16(2), u32(rate), u32(rate * 4), u16(4), u16(16);
	std::fwrite("data", 1, 4, f), u32(data);
	for (float x : sDumpSpk) {
		x               = x > 1.0f ? 1.0f : x < -1.0f ? -1.0f : x;
		const int16_t s = (int16_t)std::lround(x * 32767.0f);
		std::fwrite(&s, 2, 1, f), std::fwrite(&s, 2, 1, f);
	}
	std::fclose(f);
	std::fprintf(stderr, "[speaker] dump: speaker stream (%u samples at 48 kHz) in %s\n", frames, path.c_str());
}

const char* port_speaker_status(void)
{
	static char buf[160];
#ifndef PORT_HAVE_OPUS
	std::snprintf(buf, sizeof(buf), "Controller speaker: built without Opus (USB only).");
#else
	std::snprintf(buf, sizeof(buf), "Controller speaker: %s.",
	              gPortSettings.speakerMode == PORT_SPEAKER_OFF ? "off" : sActive ? "playing" : "no DualSense stream");
#endif
	return buf;
}

} // extern "C"

#endif // SMS_PORT_AURORA
