// Multiplayer emotes, game side (w6, MULTIPLAYER.md task board P3). The chat/overlay side
// is port_mp_chat.cpp (ImGui, no game headers).
//
// An emote plays one of Mario's own animations on the local Mario through the game's normal
// paths, so w15's snapshot capture (body BCK, frame, hand, eyes) carries it to every puppet
// with no extra protocol:
//   - "animation" emotes put Mario in MARIO_STATUS_NOMOTION (the demo status whose handler
//     only calls stopProcess and leaves the animation alone) and set the animation each logic
//     frame with TMario::setAnimation (same id = keeps playing, as the game's own states do);
//   - "Sleep" is the game's own idle chain: MARIO_STATUS_SLEEPY (belt, yawn, sit, then
//     sleeping with its zzz effect; any input wakes him the game's way);
//   - "Pianta pose" is TMario::changeMontemanWaitingAnim (the Pianta Village wait).
// Only from a plain idle stance (MARIO_STATUS_WAIT, upper body idle, on the ground, not
// holding anything, not on Yoshi, not in water, no talk/demo). Moving the stick, any action
// button, damage or a game state change ends it (back to MARIO_STATUS_WAIT).
// Input: D-pad (unused by Mario in the game) = the four quick emotes; the emote menu
// (port_mp_chat.cpp: G / left-stick click / lobby panel) and chat commands (/shine) call
// port_mp_emote_request. Only while a multiplayer session runs; single player never gets here
// (registered game-frame callbacks run only while port_mp_active()).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <Player/Mario.hpp>
#include <Player/MarioFlags.hpp>
#include <Player/MarioStatus.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void port_mp_chat_system(const char* text); // port_mp_chat.cpp
void port_mp_chat_note_emote(int id);       // port_mp_chat.cpp (w15: speech bubbles)
u32 port_frame_count(void);                 // port_crash.cpp
}

namespace {

enum Kind {
	KIND_ANIM_LOOP, // NOMOTION + animation until input
	KIND_ANIM_ONCE, // NOMOTION + animation, held at its end for `hold` logic frames, then WAIT
	KIND_SLEEP,     // MARIO_STATUS_SLEEPY: the game's own sleep chain
	KIND_MONTEMAN,  // the Pianta Village wait animation inside MARIO_STATUS_WAIT
	KIND_DANCE,     // NOMOTION + the idle animation, posed by port_mp_dance.cpp (anim = dance id)
};

struct Emote {
	const char* label;
	const char* command; // chat: "/<command>"
	int kind;
	int anim;
	int hold;    // KIND_ANIM_ONCE: logic frames held at the end
	int maxTime; // logic frames before it ends by itself (0 = until input)
};

// Index = emote id on the wire (PORT_MP_CH_EMOTE). Append only.
const Emote kEmotes[] = {
	// The Shine-get pose (arm raised, as after catching a Shine), held ~5 s.
	{ "Shine pose", "shine", KIND_ANIM_ONCE, TMario::ANIM_DEMO_SHINE_GET, 60, 150 },
	// The hot-weather "phew" (bent over, wiping sweat) the game plays after a hard brake.
	{ "Phew", "phew", KIND_ANIM_ONCE, TMario::ANIM_HOT_WAIT, 1, 300 },
	// The slump of a missed Shine.
	{ "Aww", "aww", KIND_ANIM_ONCE, TMario::ANIM_GET_FAIL, 30, 300 },
	{ "Sleep", "sleep", KIND_SLEEP, 0, 0, 0 },
	// Hands on hips, the Pianta Village wait.
	{ "Pianta pose", "pianta", KIND_MONTEMAN, 0, 0, 0 },
	{ "Yawn and stretch", "yawn", KIND_ANIM_ONCE, TMario::ANIM_YAWN, 1, 300 },
	{ "Sit", "sit", KIND_ANIM_LOOP, TMario::ANIM_SIT_WAIT, 0, 0 },
	// Dances (port_mp_dance.cpp): original moves posed on Mario's skeleton.
	{ "Cheer", "cheer", KIND_DANCE, 0, 0, 0 },
	{ "Wave", "wave", KIND_DANCE, 1, 0, 0 },
	{ "Clap", "clap", KIND_DANCE, 2, 0, 0 },
	{ "Disco", "disco", KIND_DANCE, 3, 0, 0 },
	{ "Robot", "robot", KIND_DANCE, 4, 0, 0 },
	{ "Spin", "spin", KIND_DANCE, 5, 0, 0 },
	{ "Groove", "groove", KIND_DANCE, 6, 0, 0 },
	{ "Flex", "flex", KIND_DANCE, 7, 0, 0 },
	{ "Jumping jacks", "jacks", KIND_DANCE, 8, 0, 0 },
	{ "Bow", "bow", KIND_DANCE, 9, 0, 90 },
	{ "Floss", "floss", KIND_DANCE, 10, 0, 0 },
	{ "Dab", "dab", KIND_DANCE, 11, 0, 75 },
	{ "Windmill", "windmill", KIND_DANCE, 12, 0, 0 },
	{ "Shuffle", "shuffle", KIND_DANCE, 13, 0, 0 },
	// Not used: ANIM_DEMO_GATE_OUT_APPEAR_GET (Mario is scaled to nothing, the pipe "appear").
};
const int kBuiltinCount = (int)(sizeof(kEmotes) / sizeof(kEmotes[0]));

// Custom emotes (port_mp_dance.cpp loads SMS_PC/emotes/*.smsemote, made in Blender) follow the
// built-in ones: ids kBuiltinCount.. in file-name order, so players with the same emotes folder
// (it ships with the build) agree on the ids. The emote locker keeps 64 bits, hence the cap.
extern "C" int port_mp_dance_custom_count(void);
extern "C" const char* port_mp_dance_custom_name(int i);
extern "C" float port_mp_dance_length(int id);
extern "C" int port_mp_dance_custom_id(int i);
const int kMaxEmotes = 64;
Emote sTable[kMaxEmotes];
char sCustomCommand[kMaxEmotes][32];
int sTableCount;

int emoteCount()
{
	if (sTableCount == 0) {
		for (int i = 0; i < kBuiltinCount; i++)
			sTable[sTableCount++] = kEmotes[i];
		for (int i = 0; i < port_mp_dance_custom_count() && sTableCount < kMaxEmotes; i++) {
			const char* name = port_mp_dance_custom_name(i);
			char* cmd        = sCustomCommand[sTableCount];
			int n            = 0;
			for (const char* c = name; *c != '\0' && n < 31; c++)
				if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9'))
					cmd[n++] = *c;
				else if (*c >= 'A' && *c <= 'Z')
					cmd[n++] = (char)(*c - 'A' + 'a');
			cmd[n]           = '\0';
			const int dance  = port_mp_dance_custom_id(i);
			const float len  = port_mp_dance_length(dance);
			// one-shot clips end with the clip (+2 frames of its fade out); loops wait for input
			sTable[sTableCount++] = { name, cmd, KIND_DANCE, dance, 0, len > 0.0f ? (int)(len * 30.0f + 0.99f) + 2 : 0 };
		}
	}
	return sTableCount;
}
const Emote& emoteAt(int id) { emoteCount(); return sTable[id]; }
const int kSitEmote   = 6;

// D-pad quick emotes (up, right, down, left): chosen in the Customize menu's emote locker.
extern "C" int port_mp_emote_dpad_get(int dir); // port_mp_custom.cpp
extern "C" void port_mp_dance_local(int id);     // port_mp_dance.cpp

std::atomic<int> sRequest{ -1 };
int sActive = -1;   // KIND_ANIM_* emote this module is running, or -1
int sTime, sHeld;
bool sSitStarted;   // "Sit": the sit-down animation first, then the loop

bool emotesAllowed()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return (s.reserved[1] & 2) == 0; // reserved[1] bit 1: host turned emotes off
}

bool directorNormal(TMarDirector* d)
{
	return d->mState == TMarDirector::STATE_UNK4 && !d->isTalkModeNow() && !d->isDemoModeNow();
}

// Plain idle stance (or one of our own emotes, so another can replace it).
bool idle(TMario* m)
{
	if (m->mHeldObject != nullptr || m->onYoshi() || m->checkFlag(MARIO_FLAG_IN_ANY_WATER))
		return false;
	if (sActive >= 0 && m->mStatus == MARIO_STATUS_NOMOTION)
		return true;
	if (m->mStatus == MARIO_STATUS_SLEEPY || m->mStatus == MARIO_STATUS_SLEEP)
		return true; // dozing off by himself (the game's idle timer): still standing still
	return m->mStatus == MARIO_STATUS_WAIT && m->isUpperState(TMario::UPPER_STATE_IDLE);
}

// Leaving the game's own sleep states as wakeup() does (its zzz effect).
void leaveSleep(TMario* m)
{
	if (m->mStatus == MARIO_STATUS_SLEEP)
		m->sleepingEffectKill();
	if (m->mStatus == MARIO_STATUS_SLEEPY || m->mStatus == MARIO_STATUS_SLEEP)
		m->changePlayerStatus(MARIO_STATUS_WAIT, 0, false);
}

// The player wants to move on: stick, action buttons or analog triggers.
bool cancelInput(TMarioGamePad* p)
{
	if (p == nullptr)
		return false;
	const u32 kButtons = JUTGamePad::A | JUTGamePad::B | JUTGamePad::X | JUTGamePad::Y | JUTGamePad::Z
	                     | JUTGamePad::R | JUTGamePad::L;
	// Newly pressed buttons from the held bits (mTrigger is consumed before we run).
	static u32 sLastHeld;
	const u32 held    = p->mButton.mButton & kButtons;
	const u32 pressed = held & ~sLastHeld;
	sLastHeld         = held;
	return pressed != 0 || p->mMainStick.mValue > 0.25f || p->mButton.mAnalogRf > 0.3f || p->mButton.mAnalogLf > 0.3f;
}

// NOMOTION carries MARIO_STATUS_FLAG_UNK1000, which refuses unforced status changes, so
// leaving it needs force (as readBillboard does).
void endEmote(TMario* m, bool toWait)
{
	if (toWait && m->mStatus == MARIO_STATUS_NOMOTION)
		m->changePlayerStatus(MARIO_STATUS_WAIT, 0, true);
	sActive = -1;
	port_mp_dance_local(-1);
}

s16 sStartHealth, sLastInvincible;
f32 sLastY;

void announce(int id)
{
	const unsigned char msg[2] = { 1, (unsigned char)id }; // [version][emote id]
	port_mp_send(PORT_MP_CH_EMOTE, msg, 2, 1, -1);
	port_mp_chat_note_emote(id); // w15: the local player's speech bubble
	std::fprintf(stderr, "[mp] emote %d (%s) at frame %u\n", id, emoteAt(id).label, port_frame_count());
}

void start(TMario* m, TMarDirector* d, int id)
{
	if (id < 0 || id >= emoteCount())
		return;
	if (!emotesAllowed()) {
		port_mp_chat_system("The host turned emotes off.");
		return;
	}
	if (!directorNormal(d) || !idle(m)) {
		std::fprintf(stderr, "[mp] emote %d refused: director state %d/%d, status 0x%x, upper %u, held %d, yoshi %d\n",
		             id, (int)d->mState, (int)d->unk124, (unsigned)m->mStatus, (unsigned)m->mUpperState,
		             m->mHeldObject != nullptr, (int)m->onYoshi());
		port_mp_chat_system("Emotes work while Mario stands still.");
		return;
	}
	const Emote& e = emoteAt(id);
	switch (e.kind) {
	case KIND_SLEEP:
		if (sActive >= 0)
			endEmote(m, true);
		if (m->mStatus != MARIO_STATUS_SLEEPY && m->mStatus != MARIO_STATUS_SLEEP)
			m->changePlayerStatus(MARIO_STATUS_SLEEPY, 0, false);
		break;
	case KIND_MONTEMAN:
		if (sActive >= 0)
			endEmote(m, true);
		leaveSleep(m);
		if (m->mStatus == MARIO_STATUS_WAIT)
			m->changeMontemanWaitingAnim();
		break;
	default:
		leaveSleep(m);
		if (m->mStatus != MARIO_STATUS_NOMOTION)
			m->changePlayerStatus(MARIO_STATUS_NOMOTION, 0, false);
		sActive     = id;
		sTime       = 0;
		sHeld       = 0;
		sSitStarted = false;
		sStartHealth    = m->mHealth;
		sLastInvincible = m->mInvincibilityFrames;
		sLastY          = m->mPosition.y;
		m->setAnimation(id == kSitEmote ? TMario::ANIM_SIT : (e.kind == KIND_DANCE ? (int)TMario::ANIM_WAIT : e.anim), 1.0f);
		port_mp_dance_local(e.kind == KIND_DANCE ? e.anim : -1);
		break;
	}
	announce(id);
}

void run(TMario* m, TMarDirector* d, bool cancel)
{
	if (sActive < 0)
		return;
	if (m->mStatus != MARIO_STATUS_NOMOTION) { // the game took over (damage, warp, ...)
		std::fprintf(stderr, "[mp] emote %d ends: status 0x%x\n", sActive, (unsigned)m->mStatus);
		sActive = -1;
		port_mp_dance_local(-1);
		return;
	}
	// Anything the idle stance would react to ends it at once and gives Mario back to the game:
	// input, a talk/demo, damage (NOMOTION refuses the knockback status, so do not linger), the
	// floor dropping away or moving (stopProcess only snaps to the floor below).
	const bool hurt  = m->mHealth < sStartHealth || m->mInvincibilityFrames > sLastInvincible;
	sLastInvincible  = m->mInvincibilityFrames;
	const bool moved = std::fabs(m->mPosition.y - sLastY) > 8.0f;
	sLastY           = m->mPosition.y;
	if (!directorNormal(d) || cancel || m->mHeldObject != nullptr || hurt || moved) {
		std::fprintf(stderr, "[mp] emote %d ends: director %d cancel %d held %d hurt %d moved %d\n", sActive,
		             (int)directorNormal(d), (int)cancel, m->mHeldObject != nullptr, (int)hurt, (int)moved);
		endEmote(m, true);
		return;
	}
	const Emote& e = emoteAt(sActive);
	++sTime;
	if (sActive == kSitEmote && !sSitStarted) { // Sit: the sit-down animation, then sit_wait (loop)
		m->setAnimation(TMario::ANIM_SIT, 1.0f);
		if (m->isLast1AnimeFrame())
			sSitStarted = true;
		return;
	}
	m->setAnimation(e.kind == KIND_DANCE ? (int)TMario::ANIM_WAIT : e.anim, 1.0f);
	if (e.kind == KIND_ANIM_ONCE && m->isLast1AnimeFrame() && ++sHeld >= e.hold) {
		endEmote(m, true);
		return;
	}
	if (e.maxTime > 0 && sTime >= e.maxTime)
		endEmote(m, true);
}

// SMS_PORT_MP_EMOTE_AUTO=<slot>@<frame>:<emote id>[|...] (debug): request that emote at
// that presented frame on the instance with that local slot (mp_test.py runs).
void autoEmotes()
{
	static const char* sEnv = std::getenv("SMS_PORT_MP_EMOTE_AUTO");
	static unsigned sDone; // bit per entry
	if (sEnv == nullptr)
		return;
	const int slot     = port_mp_local_slot();
	const unsigned now = port_frame_count();
	int idx            = 0;
	for (const char* p = sEnv; *p != '\0' && idx < 32; ++idx) {
		char* end;
		const long s = std::strtol(p, &end, 10);
		if (*end != '@')
			break;
		const unsigned long f = std::strtoul(end + 1, &end, 10);
		if (*end != ':')
			break;
		const long id = std::strtol(end + 1, &end, 10);
		if (s == slot && now >= f && (sDone & (1u << idx)) == 0) {
			sDone |= 1u << idx;
			sRequest = (int)id;
		}
		p = *end == '|' ? end + 1 : end;
	}
}

void emoteFrame()
{
	TMario* m       = gpMarioOriginal;
	TMarDirector* d = gpMarDirector;
	if (m == nullptr || d == nullptr || m->mModel == nullptr || !d->mSetupDone) {
		sActive = -1;
		return;
	}
	autoEmotes();
	const bool cancel = cancelInput(m->mGamePad); // every frame: it tracks button edges
	int req = sRequest.exchange(-1);
	// D-pad quick emotes (never while a menu or the chat box has the pads: they read idle).
	if (req < 0 && m->mGamePad != nullptr && !port_mp_menu_wants_input()) {
		// Edges of the held D-pad bits: the pad's own trigger bits are already consumed by the
		// time registered game-frame callbacks run (two app frames per logic frame).
		static u32 sLastDpad;
		const u32 held = m->mGamePad->mButton.mButton & 0xF;
		const u32 t    = held & ~sLastDpad;
		sLastDpad      = held;
		if (t & JUTGamePad::DPAD_UP)
			req = port_mp_emote_dpad_get(0);
		else if (t & JUTGamePad::DPAD_RIGHT)
			req = port_mp_emote_dpad_get(1);
		else if (t & JUTGamePad::DPAD_DOWN)
			req = port_mp_emote_dpad_get(2);
		else if (t & JUTGamePad::DPAD_LEFT)
			req = port_mp_emote_dpad_get(3);
	}
	run(m, d, cancel);
	if (req >= 0)
		start(m, d, req);
}

struct Register {
	Register() { port_mp_register_game_frame(emoteFrame); }
} sRegister;

} // namespace

extern "C" {

int port_mp_emote_count(void) { return emoteCount(); }
const char* port_mp_emote_label(int id) { return id >= 0 && id < emoteCount() ? emoteAt(id).label : "?"; }
const char* port_mp_emote_command(int id) { return id >= 0 && id < emoteCount() ? emoteAt(id).command : ""; }
// D-pad direction (0 up, 1 right, 2 down, 3 left) of an emote, or -1.
int port_mp_emote_dpad(int id)
{
	for (int i = 0; i < 4; i++)
		if (port_mp_emote_dpad_get(i) == id)
			return i;
	return -1;
}
// The dance an emote plays (port_mp_dance.cpp), or -1.
int port_mp_emote_dance(int id) { return id >= 0 && id < emoteCount() && emoteAt(id).kind == KIND_DANCE ? emoteAt(id).anim : -1; }
// Any thread; applied on the next game logic frame.
void port_mp_emote_request(int id) { sRequest = id; }

} // extern "C"

#endif // SMS_PORT_AURORA
