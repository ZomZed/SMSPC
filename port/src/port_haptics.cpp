// HD rumble and DualSense haptics for the Aurora build (w9). Settings: [input] rumble_mode
// (off / original / hd), rumble (strength %), hd_fludd / hd_movement / hd_world / hd_damage.
//
// Original (default): the game's RumbleMgr switches the motor on and off (PADControlMotor,
// a PWM of its power curves) and Aurora forwards that to SDL rumble, as before.
// HD: port_PADControlMotor (build_aurora.py renames the game's PADControlMotor calls) drops
// the game's on/off pulses on controller 1, and this engine plays designed waveforms instead,
// from game state that is only READ:
//  - the game's own rumble channels (RumbleMgr: each active channel's type index and its
//    power curve value this frame) -> one voice per rumble type with its own timbre (damage
//    = sharp hit, hip drop = heavy thud, MAPOBJ_APPEAR / boss types = rolling rumble, ...);
//    the game's curve is the envelope;
//  - sound-effect starts (JAIBasic::startSoundBasic, a marked SMS_PORT hook): footsteps by
//    ground type (the SE id encodes the material), wall bumps, water entry/exit, slides,
//    coins / red / blue coins, 1-up, Shine get, Yoshi gulp and juice;
//  - Mario's state: landings scaled by the fall speed, wall kicks, dives, swimming in/out;
//  - FLUDD (TWaterGun): a spray buzz scaled by the trigger pressure that thins as the tank
//    empties, rocket charge and launch, hover hum, turbo rattle.
// Every voice belongs to a category (FLUDD / movement / world / damage) the settings can mute.
//
// Outputs:
//  - two motors (SDL_RumbleGamepad, low/high frequency mix per voice), for any pad;
//  - Xbox impulse triggers (SDL_RumbleGamepadTriggers) carry the FLUDD voices;
//  - DualSense HD haptics, a PCM stream to the two voice-coil actuators:
//    * USB: the controller is also a 4-channel USB audio device; channels 3-4 drive the
//      actuators (SDL audio stream, 48 kHz);
//    * Bluetooth: HID output report 0x32 written with SDL_hid_write on the device SDL's
//      DualSense driver uses (SDL_GetGamepadPath): 141 bytes after the report id; byte 1 =
//      sequence number in the high nibble; sub-packet 0x11 (header 0x91, length 7: FE 00 00
//      00 00 FF, packet counter), sub-packet 0x12 (header 0x92, length 64: 64 signed 8-bit
//      samples, 32 stereo frames at 3 kHz, left/right actuator), zero padding, CRC-32 over
//      0xA2 + the report bytes in the last 4 bytes (little endian); one report every
//      32/3000 s = 10.667 ms. That report carries no controller-state sub-packet, so SDL's
//      own 0x31 reports (lightbar, adaptive triggers) interleave with it untouched.
//      Protocol facts from SAxense (github.com/egormanga/SAxense, MPL-2.0) and
//      LinuxAudio4Dualsense5 (github.com/GeorgLegato/LinuxAudio4Dualsense5, AGPL-3.0); no
//      code was taken from either, this is a fresh implementation of the described layout.
//      The 64 bytes are read as stereo 3 kHz by SAxense and as mono 6 kHz by
//      LinuxAudio4Dualsense5; SMS_PORT_HAPTICS_BT_LAYOUT=mono6k switches to the latter.
//    With HD haptics running, the DualSense gets no SDL rumble: SDL's rumble emulation
//    report switches the controller's audio haptics off.
// Test runs (SMS_PORT_NO_PADS / SMS_PORT_VPAD / replays) never open a device:
// SMS_PORT_HAPTICS_DUMP=<prefix> records instead what would be sent: <prefix>_bt.bin (the
// 0x32 reports, 142 bytes each, at their real cadence), <prefix>_hd.wav (the actuator
// stream, 3 kHz stereo) and the motor values in the log (SMS_PORT_HAPTICS_LOG=1).
#ifdef SMS_PORT_AURORA

#include "port_settings.h"
#include "port_replay.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_hidapi.h>
#include <SDL3/SDL_init.h>

#include <MarioUtil/RumbleMgr.hpp>
#include <Player/Mario.hpp>
#include <Player/WaterGun.hpp>
#include <Player/Yoshi.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// after the game headers (windows.h defines near/far, which JDrama's camera uses as names)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" volatile unsigned gPortFrameCount; // port_crash.cpp
// port_speaker.cpp (controller speaker, shares this module's DualSense stream)
extern "C" void port_speaker_fill_usb(float* out4, int frames);
extern "C" bool port_speaker_bt_report(uint8_t* r, int8_t* haptics64, uint8_t seq, uint8_t counter, int* size);
extern "C" bool port_speaker_wanted(void);
extern "C" void port_speaker_enable(SDL_Gamepad* gp);
extern "C" void port_speaker_dump_close(const char* prefix);

namespace {

const int kLog = [] {
	const char* e = std::getenv("SMS_PORT_HAPTICS_LOG");
	return e != nullptr ? std::atoi(e) : 0;
}();

double nowSec()
{
	static const double inv = [] {
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		return 1.0 / (double)f.QuadPart;
	}();
	LARGE_INTEGER c;
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * inv;
}

enum Cat { CAT_FLUDD, CAT_MOVE, CAT_WORLD, CAT_DAMAGE, CAT_COUNT };
const char* const kCatNames[] = { "fludd", "movement", "world", "damage" };

// A timbre: carrier frequency (actuators), noise share, motor mix.
struct Tone {
	float freq, freqEnd; // Hz (start -> end over the voice's length)
	float noise;         // 0..1
	float low, high;     // two-motor weights
};

// One-shot voice: linear attack, hold, exponential decay (tau), on a tone.
struct Voice {
	const char* name;
	Cat cat;
	Tone tone;
	float amp;
	float attack, hold, decay; // seconds
	float pan;                 // -1 left .. +1 right actuator
	double start;
	// renderer state
	float phase  = 0.0f;
	uint32_t rng = 0x1234567u;
	float length() const { return attack + hold + 5.0f * decay; }
	float env(float t) const
	{
		if (t < 0.0f)
			return 0.0f;
		if (t < attack)
			return attack > 0.0f ? t / attack : 1.0f;
		t -= attack;
		if (t < hold)
			return 1.0f;
		t -= hold;
		return decay > 0.0f ? std::exp(-t / decay) : 0.0f;
	}
	float freqAt(float t) const
	{
		const float L = attack + hold + decay;
		const float k = L > 0.0f ? (t / L > 1.0f ? 1.0f : t / L) : 1.0f;
		return tone.freq + (tone.freqEnd - tone.freq) * k;
	}
};

// Continuous voice whose level the game state sets each game frame.
struct Drone {
	const char* name;
	Cat cat;
	Tone tone;
	float pan;
	float target = 0.0f; // set per game frame
	float level  = 0.0f; // smoothed (renderer and motor read)
	float am     = 0.0f; // amplitude modulation rate (Hz), 0 = none
	float phase = 0.0f, amPhase = 0.0f;
	uint32_t rng = 0x9E3779B9u;
};

enum DroneId {
	D_SPRAY,
	D_HOVER,
	D_TURBO,
	D_ROCKET_CHARGE,
	D_SLIDE,
	D_GAME0, // + rumble type index (25 types)
	D_COUNT = D_GAME0 + 25
};

struct Engine {
	std::mutex m;
	std::vector<Voice> voices;
	Drone drones[D_COUNT];
	bool muted = true; // nothing plays (menus, not HD, ...)
	float strength = 0.5f;
	bool catOn[CAT_COUNT] = { true, true, true, true };
};
Engine sE;

// Game rumble types (RumbleType.cpp names; channelNum = 25) -> timbre and category.
struct GameType {
	Cat cat;
	Tone tone;
	float pan;
};
GameType gameType(int i)
{
	switch (i) {
	case 0: return { CAT_MOVE, { 38, 30, 0.15f, 1.0f, 0.35f }, 0 };   // MARIO_HIPDROP: heavy thud
	case 1: return { CAT_DAMAGE, { 150, 90, 0.45f, 0.5f, 1.0f }, 0 }; // MARIO_DAMAGE: sharp hit
	case 2: return { CAT_MOVE, { 55, 45, 0.1f, 0.9f, 0.25f }, 0 };   // MARIO_LANDING
	case 3: return { CAT_WORLD, { 70, 70, 0.1f, 0.8f, 0.8f }, 0 };   // MARIO_0_1SEC_FULL
	case 4: return { CAT_MOVE, { 34, 28, 0.2f, 1.0f, 0.4f }, 0 };    // MARIO_HIP_ATTACK2
	case 5: return { CAT_DAMAGE, { 170, 100, 0.5f, 0.6f, 1.0f }, 0 }; // MARIO_DAMAGE2
	case 8:
	case 9: return { CAT_WORLD, { 30, 30, 0.05f, 1.0f, 0.2f }, 0 };  // MARIO_1SEC_SIN(_HALF): swell
	case 19: return { CAT_WORLD, { 26, 22, 0.3f, 1.0f, 0.3f }, 0 };  // MAPOBJ_APPEAR: rolling
	case 20: return { CAT_WORLD, { 95, 95, 0.1f, 0.3f, 0.6f }, 0 };  // MARIO_WEAK
	case 21: return { CAT_WORLD, { 60, 50, 0.15f, 0.9f, 0.6f }, 0 }; // MARIO_HARD
	case 22: return { CAT_DAMAGE, { 190, 190, 0.6f, 0.3f, 1.0f }, 0 }; // MARIO_ELECTRIC_DAMAGE: buzz
	default: return { CAT_WORLD, { 28, 24, 0.35f, 1.0f, 0.5f }, 0 }; // RUMBLE_xx (bosses, events): rolling
	}
}

void initDrones()
{
	Drone* d = sE.drones;
	d[D_SPRAY]         = Drone { "fludd spray", CAT_FLUDD, { 150, 150, 0.55f, 0.2f, 0.9f }, 0.3f };
	d[D_HOVER]         = Drone { "fludd hover", CAT_FLUDD, { 55, 55, 0.1f, 0.7f, 0.3f }, 0.0f };
	d[D_HOVER].am      = 7.0f;
	d[D_TURBO]         = Drone { "fludd turbo", CAT_FLUDD, { 75, 75, 0.7f, 0.8f, 0.6f }, 0.0f };
	d[D_TURBO].am      = 22.0f;
	d[D_ROCKET_CHARGE] = Drone { "rocket charge", CAT_FLUDD, { 40, 40, 0.2f, 0.6f, 0.4f }, 0.0f };
	d[D_SLIDE]         = Drone { "slide", CAT_MOVE, { 45, 45, 0.6f, 0.6f, 0.3f }, 0.0f };
	static const char* const kNames[25] = { "rumble MARIO_HIPDROP", "rumble MARIO_DAMAGE", "rumble MARIO_LANDING",
		                                    "rumble MARIO_0_1SEC_FULL", "rumble MARIO_HIP_ATTACK2", "rumble MARIO_DAMAGE2",
		                                    "rumble 06", "rumble 07", "rumble MARIO_1SEC_SIN", "rumble MARIO_1SEC_SIN_HALF",
		                                    "rumble 10", "rumble 11", "rumble 12", "rumble 13", "rumble 14", "rumble 15",
		                                    "rumble 16", "rumble 17", "rumble 18", "rumble MAPOBJ_APPEAR",
		                                    "rumble MARIO_WEAK", "rumble MARIO_HARD", "rumble MARIO_ELECTRIC_DAMAGE",
		                                    "rumble 23", "rumble 24" };
	for (int i = 0; i < 25; i++) {
		const GameType g = gameType(i);
		d[D_GAME0 + i]   = Drone { kNames[i], g.cat, g.tone, g.pan };
	}
}

void addVoice(const char* name, Cat cat, Tone tone, float amp, float attack, float hold, float decay, float pan = 0.0f,
              double delay = 0.0)
{
	std::lock_guard<std::mutex> l(sE.m);
	if (sE.muted || !sE.catOn[cat])
		return;
	if (sE.voices.size() >= 48)
		sE.voices.erase(sE.voices.begin());
	Voice v;
	v.name = name, v.cat = cat, v.tone = tone, v.amp = amp;
	v.attack = attack, v.hold = hold, v.decay = decay, v.pan = pan;
	v.start  = nowSec() + delay;
	v.rng    = 0x2545F491u ^ (uint32_t)(sE.voices.size() * 7919u);
	sE.voices.push_back(v);
	if (kLog)
		std::fprintf(stderr, "[haptics] frame %u: %s (%s) amp %.2f\n", gPortFrameCount, name, kCatNames[cat], amp);
}

// ---- events from sound effects --------------------------------------------------------
std::mutex sSeMutex;
std::vector<uint32_t> sSeQueue;

struct Material {
	const char* name;
	Tone tone;
	float amp, decay;
};
// Mario's footstep sound ids (MSD_SE_MA_WALK_*, 0x1820-0x1933) by ground type.
const Material* footMaterial(uint32_t id)
{
	static const Material kHard   = { "step stone", { 170, 140, 0.3f, 0.2f, 0.8f }, 0.22f, 0.025f };
	static const Material kSoil   = { "step soil", { 70, 60, 0.25f, 0.6f, 0.3f }, 0.18f, 0.035f };
	static const Material kSand   = { "step sand", { 90, 70, 0.85f, 0.5f, 0.5f }, 0.16f, 0.05f };
	static const Material kWood   = { "step wood", { 115, 100, 0.2f, 0.5f, 0.6f }, 0.22f, 0.04f };
	static const Material kMetal  = { "step metal", { 230, 220, 0.1f, 0.2f, 0.9f }, 0.24f, 0.09f };
	static const Material kGrass  = { "step grass", { 60, 50, 0.9f, 0.4f, 0.2f }, 0.12f, 0.04f };
	static const Material kSoft   = { "step soft", { 60, 50, 0.3f, 0.5f, 0.2f }, 0.12f, 0.04f };
	static const Material kWater  = { "step water", { 50, 40, 0.7f, 0.5f, 0.4f }, 0.16f, 0.08f };
	static const Material kGoop   = { "step goop", { 35, 28, 0.35f, 0.8f, 0.2f }, 0.2f, 0.12f };
	static const Material kYoshi  = { "yoshi step", { 45, 40, 0.2f, 1.0f, 0.3f }, 0.32f, 0.06f };
	if (id >= 0x1820 && id < 0x1838)
		return &kHard; // stone, stone with sand, marble
	if (id >= 0x1838 && id < 0x1848)
		return &kSoil;
	if (id >= 0x1848 && id < 0x1858)
		return &kSand; // sand, gravel
	if (id >= 0x1858 && id < 0x1878)
		return &kWood; // woodboard, thin wood, wood, squeaky wood
	if (id >= 0x1878 && id < 0x1890)
		return &kMetal; // metal net, metal (vc/sl)
	if (id >= 0x1890 && id < 0x18B0)
		return &kGrass; // branch, tall grass, lawn, straw
	if (id >= 0x18B0 && id < 0x18D0)
		return &kMetal; // roof tile, tin roof, hood, wire
	if (id >= 0x18D0 && id < 0x18F8)
		return &kSoft; // table, bed, carpet, chair
	if (id >= 0x18F8 && id < 0x1900)
		return &kHard; // glass
	if (id >= 0x1910 && id < 0x1920)
		return &kSoft; // mushroom, carpet 2
	if (id >= 0x1924 && id < 0x192C)
		return &kWater;
	if (id >= 0x192C && id < 0x1934)
		return &kGoop; // mud
	if (id == 0x1942 || id == 0x1943)
		return &kYoshi;
	return nullptr;
}

double sSlideUntil;

void soundEvent(uint32_t full)
{
	const uint32_t id = full & 0xFFFF;
	if (const Material* mt = footMaterial(id)) {
		const float pan = (id & 4) ? 0.6f : -0.6f; // the L/R foot variants differ in bit 2
		addVoice(mt->name, CAT_MOVE, mt->tone, mt->amp, 0.003f, 0.0f, mt->decay, pan);
		return;
	}
	switch (id) {
	case 0x1802: case 0x1949: case 0x194A: case 0x194B: // wall bumps (soft)
		addVoice("wall bump", CAT_MOVE, { 90, 70, 0.2f, 0.6f, 0.5f }, 0.3f, 0.002f, 0.0f, 0.05f);
		break;
	case 0x1803: case 0x1948: case 0x1965: // wall bumps (hard), head
		addVoice("wall bump hard", CAT_MOVE, { 100, 70, 0.25f, 0.8f, 0.7f }, 0.5f, 0.002f, 0.0f, 0.06f);
		break;
	case 0x1805: case 0x1806: case 0x1938: // falls into water: dive swell
		addVoice("water entry", CAT_MOVE, { 45, 30, 0.8f, 0.8f, 0.4f }, 0.5f, 0.03f, 0.05f, 0.25f);
		break;
	case 0x181D: case 0x181E: case 0x1939: // jumps out of water: surface swell
		addVoice("water exit", CAT_MOVE, { 40, 90, 0.5f, 0.6f, 0.4f }, 0.35f, 0.06f, 0.0f, 0.15f);
		break;
	case 0x1009: case 0x1001: case 0x1141: case 0x113F: case 0x1140: // slips / goop slide / wall, tree slide
		sSlideUntil = nowSec() + 0.15;
		break;
	case 0x4811: case 0x7915: // coin
		addVoice("coin", CAT_WORLD, { 260, 300, 0.05f, 0.0f, 0.7f }, 0.3f, 0.002f, 0.0f, 0.03f);
		break;
	case 0x4846: case 0x4845: // red / blue coin: a double tick
		addVoice("red/blue coin", CAT_WORLD, { 300, 340, 0.05f, 0.1f, 0.8f }, 0.4f, 0.002f, 0.0f, 0.035f);
		addVoice("red/blue coin 2", CAT_WORLD, { 340, 380, 0.05f, 0.1f, 0.8f }, 0.4f, 0.002f, 0.0f, 0.035f, 0.0f,
		         0.07);
		break;
	case 0x4841: // 1-up: rising chirp
		addVoice("1-up", CAT_WORLD, { 120, 320, 0.05f, 0.2f, 0.8f }, 0.45f, 0.02f, 0.2f, 0.06f);
		break;
	case 0x4814: case 0x387B: // Shine get: a big swell
		addVoice("shine get", CAT_WORLD, { 60, 180, 0.15f, 0.7f, 0.6f }, 0.7f, 0.3f, 0.6f, 0.6f);
		break;
	case 0x1947: // Yoshi swallows (gulp)
		addVoice("yoshi gulp", CAT_WORLD, { 55, 28, 0.1f, 0.9f, 0.2f }, 0.45f, 0.01f, 0.03f, 0.08f);
		break;
	case 0x4840: case 0x483F: // fruit collected (juice)
		addVoice("yoshi juice", CAT_WORLD, { 80, 110, 0.5f, 0.5f, 0.5f }, 0.35f, 0.02f, 0.08f, 0.1f);
		break;
	default:
		break;
	}
}

// ---- events from Mario's state and FLUDD -----------------------------------------------
struct MarioTrack {
	bool valid      = false;
	u32 status      = 0;
	bool airborne   = false;
	bool swimming   = false;
	float minVy     = 0.0f;
	int trigState   = 0;
	int nozzle      = -1;
};
MarioTrack sMT;

bool gameActive(TMario** out)
{
	TApplication* app = &gpApplication;
	TMario* m         = gpMarioOriginal;
	if (gpMarDirector == nullptr || app->mDirector != (JDrama::TDirector*)gpMarDirector || m == nullptr
	    || !gpMarDirector->mSetupDone)
		return false;
	const u8 st = gpMarDirector->mState;
	if (st == TMarDirector::STATE_PAUSE_MENU || st == TMarDirector::STATE_GUIDE || st == TMarDirector::STATE_CARD_SAVE)
		return false;
	*out = m;
	return true;
}

void marioEvents(TMario* m)
{
	const u32 st       = m->mStatus;
	const bool air     = (st & MARIO_STATUS_FLAG_JUMPING) != 0 || (st & MARIO_STATUS_TYPE_MASK) == MARIO_STATUS_TYPE_JUMPING;
	const bool swim    = (st & MARIO_STATUS_FLAG_SWIMMING) != 0;
	if (!sMT.valid) {
		sMT.valid = true, sMT.status = st, sMT.airborne = air, sMT.swimming = swim, sMT.minVy = 0.0f;
		return;
	}
	if (air)
		sMT.minVy = m->mVel.y < sMT.minVy ? m->mVel.y : sMT.minVy;
	if (st != sMT.status) {
		if (st == (u32)MARIO_STATUS_WALL_JUMP)
			addVoice("wall kick", CAT_MOVE, { 130, 100, 0.2f, 0.5f, 0.9f }, 0.5f, 0.003f, 0.02f, 0.05f);
		else if (st == (u32)MARIO_STATUS_DIVE)
			addVoice("dive", CAT_MOVE, { 70, 40, 0.5f, 0.6f, 0.4f }, 0.35f, 0.04f, 0.05f, 0.12f);
	}
	if (sMT.airborne && !air && !swim) { // landed
		const float vy = -sMT.minVy;
		if (vy > 15.0f) { // a normal jump lands at ~37, a fall from height at 60-80
			float k = (vy - 15.0f) / 60.0f;
			k       = k > 1.0f ? 1.0f : k;
			addVoice("landing", CAT_MOVE, { 50 - 15 * k, 35 - 10 * k, 0.15f, 1.0f, 0.3f }, 0.15f + 0.7f * k, 0.004f,
			         0.01f, 0.06f + 0.08f * k);
		}
	}
	if (!sMT.airborne && air)
		sMT.minVy = 0.0f;
	if (swim != sMT.swimming)
		addVoice(swim ? "dive in" : "surface", CAT_MOVE, swim ? Tone { 45, 30, 0.7f, 0.8f, 0.3f } : Tone { 35, 80, 0.4f, 0.6f, 0.4f },
		         0.35f, 0.05f, 0.05f, 0.2f);
	sMT.status = st, sMT.airborne = air, sMT.swimming = swim;
}

// FLUDD drones from the water gun; rocket launch as a one-shot.
void fluddState(TMario* m, float* spray, float* hover, float* turbo, float* charge)
{
	*spray = *hover = *turbo = *charge = 0.0f;
	if (!m->checkFlag(MARIO_FLAG_HAS_FLUDD) || m->mWaterGun == nullptr)
		return;
	TWaterGun* w       = m->mWaterGun;
	const s32 maxWater = ((const TWaterGun*)w)->getMaxWater();
	const float fill   = maxWater > 0 ? (float)w->mCurrentWater / (float)maxWater : 0.0f;
	// trigger pressure: the analog R the game read (0..150 after PADClamp)
	const float press = m->unk108 != nullptr ? (m->unk108->mAnalogRU8 > 150 ? 1.0f : m->unk108->mAnalogRU8 / 150.0f) : 0.0f;
	const int nozzle   = w->mCurrentNozzle;
	TNozzleBase* nz    = ((const TWaterGun*)w)->getCurrentNozzle();
	int trig           = 0;
	float chg          = 0.0f;
	if (nz != nullptr && nz->getNozzleKind() == 1) {
		const TNozzleTrigger* t = (const TNozzleTrigger*)nz;
		trig                    = t->unk385;
		const f32 mx            = t->mEmitParams.mInsidePressureMax.get();
		chg                     = mx > 0.0f ? t->unk388 / mx : 0.0f;
	}
	const bool emitting = w->canSpray();
	if (nozzle == TWaterGun::Hover && trig == TNozzleTrigger::ACTIVE)
		*hover = 0.45f;
	else if (nozzle == TWaterGun::Turbo && trig == TNozzleTrigger::ACTIVE)
		*turbo = 0.6f;
	else if ((nozzle == TWaterGun::Rocket || nozzle == TWaterGun::Turbo) && trig == TNozzleTrigger::INACTIVE && chg > 0.02f)
		*charge = 0.15f + 0.45f * (chg > 1.0f ? 1.0f : chg);
	else if (emitting && (nozzle == TWaterGun::Spray || nozzle == TWaterGun::Underwater || nozzle == TWaterGun::Yoshi))
		*spray = (0.12f + 0.48f * press) * (0.35f + 0.65f * fill); // thins as the tank empties
	if (nozzle == TWaterGun::Rocket && trig == TNozzleTrigger::ACTIVE && sMT.trigState != TNozzleTrigger::ACTIVE
	    && sMT.nozzle == nozzle)
		addVoice("rocket launch", CAT_FLUDD, { 36, 28, 0.3f, 1.0f, 0.7f }, 1.0f, 0.005f, 0.05f, 0.25f);
	sMT.trigState = trig, sMT.nozzle = nozzle;
	// spray pitch rises a little with the pressure
	sE.drones[D_SPRAY].tone.freq = sE.drones[D_SPRAY].tone.freqEnd = 130.0f + 60.0f * press;
	sE.drones[D_ROCKET_CHARGE].tone.freq = sE.drones[D_ROCKET_CHARGE].tone.freqEnd = 40.0f + 120.0f * chg;
}

// ---- mixing --------------------------------------------------------------------------------
// Motor levels at time t (game thread). Caller holds sE.m.
void motorsAt(double t, float* low, float* high, float* fluddLevel)
{
	float lo = 0.0f, hi = 0.0f, fl = 0.0f;
	for (const Voice& v : sE.voices) {
		const float e = v.env((float)(t - v.start)) * v.amp;
		lo += e * v.tone.low, hi += e * v.tone.high;
		if (v.cat == CAT_FLUDD)
			fl += e;
	}
	for (const Drone& d : sE.drones) {
		if (!sE.catOn[d.cat])
			continue;
		const float e = d.target;
		lo += e * d.tone.low, hi += e * d.tone.high;
		if (d.cat == CAT_FLUDD)
			fl += e;
	}
	*low = lo > 1.0f ? 1.0f : lo, *high = hi > 1.0f ? 1.0f : hi, *fluddLevel = fl > 1.0f ? 1.0f : fl;
}

inline float noise(uint32_t& s)
{
	s ^= s << 13, s ^= s >> 17, s ^= s << 5;
	return (float)(int32_t)s * (1.0f / 2147483648.0f);
}

// Actuator PCM: n stereo frames at `rate`, starting at time t0. Any thread.
void renderPcm(float* lr, int n, double rate, double t0)
{
	std::lock_guard<std::mutex> l(sE.m);
	std::memset(lr, 0, sizeof(float) * 2 * n);
	if (sE.muted)
		return;
	const float dt = (float)(1.0 / rate);
	for (Voice& v : sE.voices) {
		if (!sE.catOn[v.cat])
			continue;
		const float gl = v.pan > 0.0f ? 1.0f - v.pan : 1.0f, gr = v.pan < 0.0f ? 1.0f + v.pan : 1.0f;
		float t        = (float)(t0 - v.start);
		for (int i = 0; i < n; i++, t += dt) {
			const float e = v.env(t);
			if (e <= 0.0f)
				continue;
			v.phase += 6.2831853f * v.freqAt(t) * dt;
			if (v.phase > 6.2831853f)
				v.phase -= 6.2831853f;
			const float s = (std::sin(v.phase) * (1.0f - v.tone.noise) + noise(v.rng) * v.tone.noise) * e * v.amp;
			lr[2 * i] += s * gl, lr[2 * i + 1] += s * gr;
		}
	}
	for (Drone& d : sE.drones) {
		if (!sE.catOn[d.cat] || (d.target <= 0.0f && d.level <= 0.0005f))
			continue;
		const float gl = d.pan > 0.0f ? 1.0f - d.pan : 1.0f, gr = d.pan < 0.0f ? 1.0f + d.pan : 1.0f;
		const float k  = 1.0f - std::exp(-dt / 0.03f); // 30 ms smoothing of the level
		for (int i = 0; i < n; i++) {
			d.level += (d.target - d.level) * k;
			d.phase += 6.2831853f * d.tone.freq * dt;
			if (d.phase > 6.2831853f)
				d.phase -= 6.2831853f;
			float a = d.level;
			if (d.am > 0.0f) {
				d.amPhase += 6.2831853f * d.am * dt;
				if (d.amPhase > 6.2831853f)
					d.amPhase -= 6.2831853f;
				a *= 0.6f + 0.4f * std::sin(d.amPhase);
			}
			const float s = (std::sin(d.phase) * (1.0f - d.tone.noise) + noise(d.rng) * d.tone.noise) * a;
			lr[2 * i] += s * gl, lr[2 * i + 1] += s * gr;
		}
	}
	// The voice-coil actuators need a strong signal: 50% strength (the default) is unity gain;
	// a soft clip keeps loud mixes from turning into a square wave.
	const float g = 2.0f * sE.strength;
	for (int i = 0; i < 2 * n; i++) {
		const float y = std::tanh(lr[i] * g * 1.2f) * (1.0f / 0.8337f); // tanh(1.2) = 0.8337: unity at full scale
		lr[i]         = y > 1.0f ? 1.0f : y < -1.0f ? -1.0f : y;
	}
}

// ---- DualSense Bluetooth report 0x32 ---------------------------------------------------------
uint32_t crc32Update(uint32_t c, const uint8_t* p, size_t n)
{
	static uint32_t table[256];
	static bool init;
	if (!init) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t r = i;
			for (int k = 0; k < 8; k++)
				r = (r >> 1) ^ ((r & 1) ? 0xEDB88320u : 0u);
			table[i] = r;
		}
		init = true;
	}
	for (size_t i = 0; i < n; i++)
		c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
	return c;
}

// CRC-32 of 0xA2 (the Bluetooth HID output header) followed by the report.
uint32_t ds5Crc(const uint8_t* report, size_t n)
{
	const uint8_t hdr = 0xA2;
	uint32_t c        = crc32Update(0xFFFFFFFFu, &hdr, 1);
	return ~crc32Update(c, report, n);
}

const int kBtReportSize = 142; // id + 141
const double kBtPeriod  = 32.0 / 3000.0;

void buildReport32(uint8_t* r, const int8_t* samples64, uint8_t seq, uint8_t counter)
{
	std::memset(r, 0, kBtReportSize);
	r[0]  = 0x32;
	r[1]  = (uint8_t)(seq << 4); // sequence number, tag 0
	r[2]  = 0x80 | 0x11;         // sub-packet 0x11 (sized)
	r[3]  = 7;
	r[4]  = 0xFE;
	r[9]  = 0xFF;
	r[10] = counter;
	r[11] = 0x80 | 0x12; // sub-packet 0x12: haptics
	r[12] = 64;
	std::memcpy(r + 13, samples64, 64);
	const uint32_t crc = ds5Crc(r, kBtReportSize - 4);
	r[138] = (uint8_t)crc, r[139] = (uint8_t)(crc >> 8), r[140] = (uint8_t)(crc >> 16), r[141] = (uint8_t)(crc >> 24);
}

bool sMono6k = [] {
	const char* e = std::getenv("SMS_PORT_HAPTICS_BT_LAYOUT");
	return e != nullptr && _stricmp(e, "mono6k") == 0;
}();

void packSamples(double t0, int8_t* out64, std::vector<float>* wav)
{
	float buf[2 * 64];
	if (sMono6k) {
		renderPcm(buf, 64, 6000.0, t0);
		for (int i = 0; i < 64; i++) {
			const float m = 0.5f * (buf[2 * i] + buf[2 * i + 1]);
			out64[i]      = (int8_t)std::lround(m * 127.0f);
			if (wav)
				wav->push_back(m), wav->push_back(m);
		}
	} else {
		renderPcm(buf, 32, 3000.0, t0);
		for (int i = 0; i < 64; i++)
			out64[i] = (int8_t)std::lround(buf[i] * 127.0f);
		if (wav)
			wav->insert(wav->end(), buf, buf + 64);
	}
}

// ---- sinks -------------------------------------------------------------------------------------
enum SinkKind { SINK_NONE, SINK_MOTORS, SINK_USB_AUDIO, SINK_BT_HID, SINK_DUMP };
const char* const kSinkNames[] = { "none", "two motors", "DualSense haptics over USB audio",
	                               "DualSense haptics over Bluetooth", "dump (test)" };

struct Sinks {
	SinkKind kind = SINK_NONE;
	SDL_Gamepad* gp = nullptr;
	// USB audio
	SDL_AudioStream* usb = nullptr;
	int usbChannels      = 0;
	// Bluetooth / dump thread
	std::thread thread;
	std::atomic<bool> stop { false };
	SDL_hid_device* hid = nullptr;
	std::atomic<unsigned> reports { 0 };
	std::atomic<unsigned> writeErrors { 0 };
	// motors
	Uint16 lastLo = 0xFFFF, lastHi = 0xFFFF, lastTrig = 0xFFFF;
	double lastMotorSend = 0.0;
	std::string status = "Rumble: original.";
	// Still running at process exit (window closed without port_haptics_shutdown): a joinable
	// std::thread destroyed by the exit handlers calls std::terminate. Detach it instead.
	~Sinks()
	{
		if (thread.joinable()) {
			stop = true;
			thread.detach();
		}
	}
};
Sinks sS;

// USB: channels 3-4 of the controller's 4-channel audio device.
void SDLCALL usbCallback(void*, SDL_AudioStream* stream, int additional, int)
{
	const int ch     = 4;
	const int frames = additional / (int)(sizeof(float) * ch);
	if (frames <= 0)
		return;
	static std::vector<float> hap, out;
	hap.resize((size_t)frames * 2);
	out.assign((size_t)frames * ch, 0.0f);
	renderPcm(hap.data(), frames, 48000.0, nowSec());
	for (int i = 0; i < frames; i++) {
		out[(size_t)i * ch + 2] = hap[(size_t)i * 2];
		out[(size_t)i * ch + 3] = hap[(size_t)i * 2 + 1];
	}
	port_speaker_fill_usb(out.data(), frames); // channels 1-2: the controller speaker
	SDL_PutAudioStreamData(stream, out.data(), (int)(out.size() * sizeof(float)));
}

bool openUsbAudio()
{
	if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO))
		return false;
	int n                  = 0;
	SDL_AudioDeviceID* ids = SDL_GetAudioPlaybackDevices(&n);
	SDL_AudioDeviceID dev  = 0;
	for (int i = 0; i < n && ids != nullptr; i++) {
		const char* name = SDL_GetAudioDeviceName(ids[i]);
		if (name != nullptr && (SDL_strcasestr(name, "DualSense") != nullptr || SDL_strcasestr(name, "Wireless Controller") != nullptr)) {
			dev = ids[i];
			break;
		}
	}
	SDL_free(ids);
	if (dev == 0) {
		std::fprintf(stderr, "[haptics] USB: no DualSense audio device found\n");
		return false;
	}
	SDL_AudioSpec have = {};
	int frames         = 0;
	SDL_GetAudioDeviceFormat(dev, &have, &frames);
	if (have.channels < 4) {
		std::fprintf(stderr, "[haptics] USB: '%s' has %d channels (needs 4)\n", SDL_GetAudioDeviceName(dev), have.channels);
		return false;
	}
	SDL_AudioSpec spec = { SDL_AUDIO_F32, 4, 48000 };
	sS.usb             = SDL_OpenAudioDeviceStream(dev, &spec, usbCallback, nullptr);
	if (sS.usb == nullptr) {
		std::fprintf(stderr, "[haptics] USB: open failed: %s\n", SDL_GetError());
		return false;
	}
	SDL_ResumeAudioStreamDevice(sS.usb);
	sS.usbChannels = have.channels;
	std::fprintf(stderr, "[haptics] USB: haptics on '%s' (%d ch, %d Hz)\n", SDL_GetAudioDeviceName(dev), have.channels,
	             have.freq);
	return true;
}

FILE* sDumpBt;
std::vector<float> sDumpWav;
std::string sDumpPrefix;

void writeWav(const std::string& path, const std::vector<float>& lr, int rate)
{
	FILE* f = std::fopen(path.c_str(), "wb");
	if (f == nullptr)
		return;
	const uint32_t frames = (uint32_t)(lr.size() / 2), data = frames * 4;
	auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f), u32(36 + data), std::fwrite("WAVEfmt ", 1, 8, f);
	u32(16), u16(1), u16(2), u32(rate), u32(rate * 4), u16(4), u16(16);
	std::fwrite("data", 1, 4, f), u32(data);
	for (float x : lr) {
		const int16_t s = (int16_t)std::lround(x * 32767.0f);
		std::fwrite(&s, 2, 1, f);
	}
	std::fclose(f);
}

// Bluetooth (device) or dump: one report per 10.667 ms on an absolute schedule.
void btThread(bool dump)
{
	// high-resolution waitable timer (Windows 10 1803+), then a short spin to the deadline
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
	uint8_t seq = 0, counter = 0;
	const double t0 = nowSec();
	uint64_t n      = 0;
	uint8_t r[400];
	while (!sS.stop.load()) {
		const double due = t0 + (double)n * kBtPeriod;
		double now       = nowSec();
		if (now < due) {
			const double wait = due - now;
			if (wait > 0.0015) {
				LARGE_INTEGER rel;
				rel.QuadPart = -(LONGLONG)((wait - 0.001) * 1e7); // 100 ns units, relative
				if (timer != nullptr && SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE))
					WaitForSingleObject(timer, 50);
				else
					Sleep(1);
			}
			while (nowSec() < due)
				SwitchToThread();
		} else if (now - due > 4 * kBtPeriod) { // fell behind (suspend, debugger): resync
			n = (uint64_t)((now - t0) / kBtPeriod);
			continue;
		}
		int8_t hap[64];
		packSamples(due, hap, dump ? &sDumpWav : nullptr);
		int size = kBtReportSize;
		if (!port_speaker_bt_report(r, hap, seq, counter, &size)) // speaker off: the 0x32 report
			buildReport32(r, hap, seq, counter);
		if (dump) {
			if (sDumpBt != nullptr)
				std::fwrite(r, 1, (size_t)size, sDumpBt);
		} else if (SDL_hid_write(sS.hid, r, (size_t)size) < 0) {
			if (sS.writeErrors.fetch_add(1) < 5)
				std::fprintf(stderr, "[haptics] Bluetooth: report 0x%02x write failed: %s\n", r[0], SDL_GetError());
		}
		sS.reports.fetch_add(1);
		seq     = (uint8_t)((seq + 1) & 15);
		counter = (uint8_t)(counter + 1);
		++n;
	}
	if (timer != nullptr)
		CloseHandle(timer);
}

void closeSinks()
{
	if (sS.thread.joinable()) {
		sS.stop = true;
		sS.thread.join();
		sS.stop = false;
	}
	if (sS.hid != nullptr) {
		SDL_hid_close(sS.hid);
		sS.hid = nullptr;
	}
	if (sS.usb != nullptr) {
		SDL_DestroyAudioStream(sS.usb);
		sS.usb = nullptr;
	}
	if (sDumpBt != nullptr) {
		std::fclose(sDumpBt);
		sDumpBt = nullptr;
		writeWav(sDumpPrefix + "_hd.wav", sDumpWav, sMono6k ? 6000 : 3000);
		std::fprintf(stderr, "[haptics] dump: %u reports in %s_bt.bin, actuator stream in %s_hd.wav\n",
		             sS.reports.load(), sDumpPrefix.c_str(), sDumpPrefix.c_str());
		port_speaker_dump_close(sDumpPrefix.c_str());
	}
	if (sS.gp != nullptr && (sS.kind == SINK_MOTORS)) {
		SDL_RumbleGamepad(sS.gp, 0, 0, 0);
		SDL_RumbleGamepadTriggers(sS.gp, 0, 0, 0);
	}
	sS.kind = SINK_NONE;
	sS.gp   = nullptr;
	sS.lastLo = sS.lastHi = sS.lastTrig = 0xFFFF;
}

bool testRun()
{
	const char* np = std::getenv("SMS_PORT_NO_PADS");
	unsigned v, p;
	return (np != nullptr && *np != '\0' && *np != '0') || port_vpad_requested(&v, &p) || port_replay_active();
}

// Chooses and opens the output for controller 1: the DualSense stream (USB audio /
// Bluetooth reports) when HD rumble or the controller speaker wants it, else the motors (HD).
void chooseSink(SDL_Gamepad* gp, bool hd, bool speaker)
{
	const char* dump = std::getenv("SMS_PORT_HAPTICS_DUMP");
	if (dump != nullptr && *dump != '\0' && sS.kind == SINK_DUMP && (hd || speaker)) {
		sS.gp = gp; // one dump for the whole run
		return;
	}
	closeSinks();
	sS.gp = gp;
	if (!hd && !speaker)
		return;
	if (dump != nullptr && *dump != '\0') {
		sDumpPrefix = dump;
		sDumpBt     = std::fopen((sDumpPrefix + "_bt.bin").c_str(), "wb");
		sDumpWav.clear();
		sS.kind   = SINK_DUMP;
		sS.thread = std::thread(btThread, true);
		std::fprintf(stderr, "[haptics] HD: dumping the DualSense stream to %s_*\n", dump);
		return;
	}
	if (gp == nullptr || PADIsGCAdapter(0))
		return; // GameCube adapter: the game's own on/off pulses (port_PADControlMotor)
	const bool ds5 = PADGetControllerType(0) == PAD_TYPE_PS5;
	if (ds5 && !testRun()) {
		const SDL_JoystickConnectionState cs = SDL_GetGamepadConnectionState(gp);
		if (cs == SDL_JOYSTICK_CONNECTION_WIRED && openUsbAudio()) {
			if (hd)
				SDL_RumbleGamepad(gp, 0, 0, 0); // leave the actuators to the audio stream
			port_speaker_enable(gp);
			sS.kind = SINK_USB_AUDIO;
			return;
		}
		if (cs == SDL_JOYSTICK_CONNECTION_WIRELESS) {
			const char* path = SDL_GetGamepadPath(gp);
			sS.hid           = path != nullptr ? SDL_hid_open_path(path) : nullptr;
			if (sS.hid != nullptr) {
				if (hd)
					SDL_RumbleGamepad(gp, 0, 0, 0);
				port_speaker_enable(gp);
				sS.kind   = SINK_BT_HID;
				sS.thread = std::thread(btThread, false);
				std::fprintf(stderr, "[haptics] Bluetooth: haptics reports 0x32 every 10.667 ms on %s\n", path);
				return;
			}
			std::fprintf(stderr, "[haptics] Bluetooth: cannot open the HID device (%s): %s\n", path ? path : "no path",
			             SDL_GetError());
		}
	}
	sS.kind = hd ? SINK_MOTORS : SINK_NONE;
}

void sendMotors(double t)
{
	float lo, hi, fl;
	{
		std::lock_guard<std::mutex> l(sE.m);
		motorsAt(t, &lo, &hi, &fl);
	}
	const float g   = sE.strength;
	const Uint16 L  = (Uint16)std::lround(lo * g * 65535.0f);
	const Uint16 H  = (Uint16)std::lround(hi * g * 65535.0f);
	const Uint16 TR = (Uint16)std::lround(fl * g * 65535.0f);
	const bool refresh = t - sS.lastMotorSend > 0.1;
	if (sS.kind == SINK_MOTORS && sS.gp != nullptr) {
		if (L != sS.lastLo || H != sS.lastHi || (refresh && (L | H))) {
			SDL_RumbleGamepad(sS.gp, L, H, 200);
			if (kLog >= 2 || (kLog && (L != sS.lastLo || H != sS.lastHi)))
				std::fprintf(stderr, "[haptics] frame %u: motors low %5u high %5u\n", gPortFrameCount, L, H);
		}
		const SDL_GamepadType gt = SDL_GetGamepadType(sS.gp);
		if ((gt == SDL_GAMEPAD_TYPE_XBOXONE || gt == SDL_GAMEPAD_TYPE_XBOX360) && (TR != sS.lastTrig || (refresh && TR)))
			SDL_RumbleGamepadTriggers(sS.gp, 0, TR, 200); // impulse triggers: FLUDD on the right
	} else if (kLog && (L != sS.lastLo || H != sS.lastHi)) {
		std::fprintf(stderr, "[haptics] frame %u: motor equivalent low %5u high %5u (%s)\n", gPortFrameCount, L, H,
		             kSinkNames[sS.kind]);
	}
	if (refresh || L != sS.lastLo || H != sS.lastHi)
		sS.lastMotorSend = t;
	sS.lastLo = L, sS.lastHi = H, sS.lastTrig = TR;
}

int sAppliedMode = -1;
SDL_Gamepad* sChosenGp;
bool sChosenHd, sChosenSpk, sChosen;
std::atomic<int> sOrigMotor { 0 }; // the game's motor on controller 1 in Original mode (0..255)

} // namespace

extern "C" {

// Game calls of PADControlMotor (renamed in build_aurora.py).
void port_PADControlMotor(u32 chan, u32 cmd)
{
	const int mode = gPortSettings.rumbleMode;
	if (mode == PORT_RUMBLE_OFF) {
		if (cmd != PAD_MOTOR_STOP_HARD)
			cmd = PAD_MOTOR_STOP_HARD;
	} else if (mode == PORT_RUMBLE_HD && chan == 0 && !PADIsGCAdapter(0)) {
		// the HD engine plays the game's rumble channels instead (a GameCube pad on the
		// adapter keeps the game's pulses: its motor is on/off, what they were made for)
		return;
	}
	if (chan == 0) // for the DualSense Bluetooth stream's state (port_speaker.cpp)
		sOrigMotor = cmd == PAD_MOTOR_RUMBLE ? (int)std::lround(gPortSettings.rumble * 2.55) : 0;
	PADControlMotor(chan, cmd);
}

int port_haptics_original_motor(void) { return gPortSettings.rumbleMode == PORT_RUMBLE_ORIGINAL ? sOrigMotor.load() : 0; }

// JAIBasic::startSoundBasic (SMS_PORT hook): a sound effect starts. Any thread.
void port_haptics_sound(u32 id)
{
	if (gPortSettings.rumbleMode != PORT_RUMBLE_HD)
		return;
	std::lock_guard<std::mutex> l(sSeMutex);
	if (sSeQueue.size() < 256)
		sSeQueue.push_back(id);
}

// Main thread, once per game frame (port_input_read).
void port_haptics_frame(void)
{
	static bool sInit;
	if (!sInit) {
		initDrones();
		sInit = true;
	}
	const PortSettings& s = gPortSettings;
	const s32 index       = PADGetIndexForPort(0);
	SDL_Gamepad* gp       = index >= 0 ? PADGetSDLGamepadForIndex((u32)index) : nullptr;
	const bool hd         = s.rumbleMode == PORT_RUMBLE_HD;
	const bool spk        = port_speaker_wanted();

	if (s.rumbleMode != sAppliedMode) {
		PADControlMotor(0, PAD_MOTOR_STOP_HARD); // no motor left on from the previous mode
		sAppliedMode = s.rumbleMode;
	}
	// SMS_PORT_HAPTICS_DUMP_END=<frame>: finish the dump files at that frame (test runs that
	// are killed afterwards).
	static const unsigned sDumpEnd = [] {
		const char* e = std::getenv("SMS_PORT_HAPTICS_DUMP_END");
		return e != nullptr ? (unsigned)std::strtoul(e, nullptr, 10) : 0u;
	}();
	if (sDumpEnd != 0 && sS.kind == SINK_DUMP && gPortFrameCount >= sDumpEnd)
		closeSinks();
	if (!sChosen || gp != sChosenGp || hd != sChosenHd || spk != sChosenSpk) {
		chooseSink(gp, hd, spk);
		sChosen = true, sChosenGp = gp, sChosenHd = hd, sChosenSpk = spk;
	}
	if (!hd) {
		{
			std::lock_guard<std::mutex> l(sE.m);
			sE.muted = true;
			sE.voices.clear();
		}
		sS.status = s.rumbleMode == PORT_RUMBLE_OFF ? "Rumble off." : "Rumble: original (the game's motor pulses).";
		return;
	}

	TMario* m       = nullptr;
	const bool live = gameActive(&m);
	const double t  = nowSec();
	{
		std::lock_guard<std::mutex> l(sE.m);
		sE.muted    = !live;
		sE.strength = s.rumble / 100.0f;
		sE.catOn[CAT_FLUDD] = s.hdFludd, sE.catOn[CAT_MOVE] = s.hdMovement;
		sE.catOn[CAT_WORLD] = s.hdWorld, sE.catOn[CAT_DAMAGE] = s.hdDamage;
		// drop finished voices
		for (size_t i = 0; i < sE.voices.size();)
			if (t - sE.voices[i].start > sE.voices[i].length())
				sE.voices.erase(sE.voices.begin() + (ptrdiff_t)i);
			else
				++i;
		for (Drone& d : sE.drones)
			d.target = 0.0f;
		if (!live)
			sE.voices.clear();
	}
	std::vector<uint32_t> se;
	{
		std::lock_guard<std::mutex> l(sSeMutex);
		se.swap(sSeQueue);
	}
	if (live) {
		for (uint32_t id : se)
			soundEvent(id);
		marioEvents(m);
		float spray, hover, turbo, charge;
		fluddState(m, &spray, &hover, &turbo, &charge);
		std::lock_guard<std::mutex> l(sE.m);
		sE.drones[D_SPRAY].target         = spray;
		sE.drones[D_HOVER].target         = hover;
		sE.drones[D_TURBO].target         = turbo;
		sE.drones[D_ROCKET_CHARGE].target = charge;
		sE.drones[D_SLIDE].target         = t < sSlideUntil ? 0.3f : 0.0f;
		// The game's own rumble channels: type index and this frame's curve value.
		RumbleMgr* rm = SMSRumbleMgr;
		if (rm != nullptr && rm->mActive && !rm->mPaused && rm->mControllerMgrTbl[0] != nullptr
		    && rm->mControllerMgrTbl[0]->mChannelMgrTbl != nullptr) {
			const RumbleChannelMgr* ch = rm->mControllerMgrTbl[0]->mChannelMgrTbl;
			for (int i = 0; i < RUMBLE_CHANNELS_PER_CONTROLLER; i++) {
				const int type = ch[i].mChannelDataIdx;
				if (type < 0 || type >= 25 || ch[i].mChannelDataTbl == nullptr)
					continue;
				float p = ch[i].mPower;
				p       = p < 0.0f ? 0.0f : p > 1.0f ? 1.0f : p;
				Drone& d = sE.drones[D_GAME0 + type];
				if (p > d.target)
					d.target = p;
			}
			// MARIO_WEAK / MARIO_HARD from a FLUDD trigger nozzle count as FLUDD
			const bool fluddRumble = sMT.trigState != 0 || charge > 0.0f;
			sE.drones[D_GAME0 + 20].cat = fluddRumble ? CAT_FLUDD : CAT_WORLD;
			sE.drones[D_GAME0 + 21].cat = fluddRumble ? CAT_FLUDD : CAT_WORLD;
			if (kLog >= 2)
				for (int k = 0; k < 25; k++)
					if (sE.drones[D_GAME0 + k].target > 0.0f)
						std::fprintf(stderr, "[haptics] frame %u: %s power %.2f\n", gPortFrameCount,
						             sE.drones[D_GAME0 + k].name, sE.drones[D_GAME0 + k].target);
		}
	} else {
		sMT = MarioTrack();
	}
	sendMotors(t);
	char buf[200];
	std::snprintf(buf, sizeof(buf), "Rumble: HD, %s%s.", kSinkNames[sS.kind],
	              sS.kind == SINK_BT_HID || sS.kind == SINK_DUMP ? "" : "");
	sS.status = buf;
	static double sLastStat;
	if (kLog && (sS.kind == SINK_BT_HID || sS.kind == SINK_DUMP) && t - sLastStat > 5.0) {
		sLastStat = t;
		std::fprintf(stderr, "[haptics] %s: %u reports so far, %u write errors\n", kSinkNames[sS.kind],
		             sS.reports.load(), sS.writeErrors.load());
	}
}

const char* port_haptics_status(void) { return sS.status.c_str(); }

// Bluetooth sink owner (port_speaker.cpp needs the HID handle for its 0x31 setup report).
SDL_hid_device* port_haptics_bt_device(void) { return sS.kind == SINK_BT_HID ? sS.hid : nullptr; }
bool port_haptics_bt_running(void) { return sS.kind == SINK_BT_HID || sS.kind == SINK_DUMP; }
bool port_haptics_stream_running(void)
{
	return sS.kind == SINK_BT_HID || sS.kind == SINK_DUMP || sS.kind == SINK_USB_AUDIO;
}

void port_haptics_shutdown(void)
{
	closeSinks();
	sChosen = false;
}

} // extern "C"

// Shared with port_speaker.cpp.
uint32_t port_ds5_crc(const uint8_t* report, size_t n) { return ds5Crc(report, n); }

#endif // SMS_PORT_AURORA
