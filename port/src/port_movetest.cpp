// Movement tests (test knobs; nothing happens unless one of these is set).
//
// For matching an area of the seamless island (port_isle.cpp) against the original stage: a short
// session of running, jumping and diving is played in the original stage and recorded, then the same
// inputs are replayed on the island (usually at 2x speed, the fast-forward option); the screenshots
// and Mario's path are compared. Everything counts game frames from "control start": the first
// frame the director is in normal play (TMarDirector state 4) with Mario standing idle, so stages
// with different intros line up.
//
//   SMS_PORT_MOVE_SCRIPT=<from>-<to>:<input>[;...]   inputs by frame from control start:
//       a stick direction N, S, E, W, NE, NW, SE, SW (full tilt; N = away from the camera) and/or
//       buttons A, B, X, Y, Z, R, L joined with '+', e.g. "0-60:N;60-64:N+A;64-90:NE;90-94:NE+B"
//   SMS_PORT_MOVE_REC=<file>     records controller 1 from control start (with a script, the
//                                scripted frames; without, whatever was played) to <file>
//   SMS_PORT_MOVE_PLAY=<file>    plays <file> on controller 1 from control start
//   SMS_PORT_MOVE_FRAMES=<n>     how many frames to record / follow (default: the script's end + 60)
//   SMS_PORT_MOVE_SHOTS=<f>[,..] screenshots at these frames from control start, saved as
//                                shot_9<frame, 5 digits>.bmp / present_9....bmp (SMS_PORT_SHOT's)
//   SMS_PORT_MOVE_EXIT=1         quit 30 frames after the last recorded / played frame
//   SMS_PORT_MOVE_NOZZLE=hover|rocket|turbo   that nozzle (full tank) from control start
//   SMS_PORT_SKIP_STAGE_INTRO=1  skip the stage's opening camera (also on with any of the above):
//                                presses Start while it plays, as a player may
// The log gets "[move] start", Mario's position every 30 frames and "[move] end <frame> pos x y z
// angle a" (the angle in degrees, 0 = facing +Z).
#ifdef SMS_PORT_AURORA

#include <Camera/Camera.hpp>
#include <Map/MapData.hpp>
#include <MarioUtil/MathUtil.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioStatus.hpp>
#include <Player/Watergun.hpp>
#include <System/MarDirector.hpp>
#include <dolphin/pad.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" bool port_replay_save(const char* why); // port_replay.cpp (w12: SMS_PORT_MOVE_SAVE_REPLAY)

namespace {

struct Frame {
	u16 button;
	s8 stickX, stickY, substickX, substickY;
	u8 triggerLeft, triggerRight;
	float camYaw; // the camera heading Mario's controls used when recorded (degrees)
	u32 ignored;  // the stage ignored this frame's input (Mario still idle at the start): replayed as none
};

struct Span {
	int from, to;
	Frame f;
};

// SMS_PORT_MOVE_YAW=<degrees>: the stage is turned by this much where it is played back (the island).
// The stick is relative to the camera and stages start with their own camera, so a played-back stick
// is turned by (recorded camera heading + this) - the camera's heading now: Mario then runs the same
// way in the stage's own frame. SMS_PORT_MOVE_YAWSIGN=-1 flips the turn (diagnostics).
bool sYawSet;
float sYawOffset, sYawSign = 1.0f;
bool sMovedYet; // recording: Mario has left his idle stance since control start
bool sCamYawDue; // this frame's camera heading is the recorded one (CPolarSubCamera::calcExternalData_)
s16 sCamYaw;

// The camera heading Mario's controls use (TMario: intended yaw = stick angle + camera unk258).
float cameraYaw()
{
	if (gpCamera == nullptr)
		return 0.0f;
	return gpCamera->getUnk258() * (360.0f / 65536.0f);
}

// The direction a raw stick gives Mario, as the game computes it: PADClamp (JUTGamePad::read),
// JUTGamePad::CStick::update (/54, onto the unit circle), TMario::checkController (x128, the dead zone
// of 6) and matan(-V, H) (s16 angle) plus the processed length.
void stickDir(s8 rx, s8 ry, s16& angle, float& dist)
{
	PADStatus st[PAD_MAX_CONTROLLERS] = {};
	st[0].stickX = rx;
	st[0].stickY = ry;
	for (int i = 1; i < PAD_MAX_CONTROLLERS; ++i)
		st[i].err = PAD_ERR_NO_CONTROLLER;
	PADClamp(st);
	float px = st[0].stickX / 54.0f, py = st[0].stickY / 54.0f;
	const float m = std::sqrt(px * px + py * py);
	if (m > 1.0f) {
		px /= m;
		py /= m;
	}
	const int hs = (s16)(128.0f * px), vs = (s16)(128.0f * py);
	const float h = hs < -7 ? hs + 6.0f : (hs > 7 ? hs - 6.0f : 0.0f);
	const float v = vs < -7 ? vs + 6.0f : (vs > 7 ? vs - 6.0f : 0.0f);
	angle = matan(-v, h);
	dist  = std::sqrt(h * h + v * v);
}

// The raw stick near (rx, ry) whose game direction is `angle` with about the length `dist`.
void stickFor(s16 angle, float dist, s8 rx, s8 ry, s8& ox, s8& oy)
{
	int best = 1 << 30;
	ox = rx;
	oy = ry;
	for (int dx = -24; dx <= 24; ++dx)
		for (int dy = -24; dy <= 24; ++dy) {
			const int x = rx + dx, y = ry + dy;
			if (x < -127 || x > 127 || y < -127 || y > 127)
				continue;
			s16 a;
			float d;
			stickDir((s8)x, (s8)y, a, d);
			// the direction first; never a weaker stick than recorded (Mario's speed follows its length)
			const int err = std::abs((s16)(a - angle)) * 4 + (int)(d < dist ? (dist - d) * 64.0f : 0.0f)
			              + std::abs(dx) + std::abs(dy);
			if (err < best) {
				best = err;
				ox   = (s8)x;
				oy   = (s8)y;
			}
		}
}

bool sInit;
bool sActive;      // any movement-test knob set
bool sSkipIntro;
std::vector<Span> sScript;
std::string sRecPath, sPlayPath;
std::vector<Frame> sRec, sPlay;
int sFrames = -1;  // frames to record / follow
std::vector<int> sShots;
bool sExit;
int sRel   = -1;   // frame from control start (-1: not started)
int sShotDue;      // 900000 + frame when a screenshot is due
bool sDone;
int sSkipTick;

Frame parseInput(const char* s, const char* e)
{
	Frame f = {};
	std::string tok;
	auto apply = [&](const std::string& t) {
		static const struct { const char* name; int x, y; } kDirs[] = {
			{ "N", 0, 72 },   { "S", 0, -72 },  { "E", 72, 0 },   { "W", -72, 0 },
			{ "NE", 51, 51 }, { "NW", -51, 51 }, { "SE", 51, -51 }, { "SW", -51, -51 },
		};
		for (const auto& d : kDirs)
			if (t == d.name) {
				f.stickX = (s8)d.x;
				f.stickY = (s8)d.y;
				return;
			}
		if (t == "A") f.button |= PAD_BUTTON_A;
		else if (t == "B") f.button |= PAD_BUTTON_B;
		else if (t == "X") f.button |= PAD_BUTTON_X;
		else if (t == "Y") f.button |= PAD_BUTTON_Y;
		else if (t == "Z") f.button |= PAD_TRIGGER_Z;
		else if (t == "R") { f.button |= PAD_TRIGGER_R; f.triggerRight = 255; }
		else if (t == "L") { f.button |= PAD_TRIGGER_L; f.triggerLeft = 255; }
		else if (!t.empty()) std::fprintf(stderr, "[move] unknown input '%s'\n", t.c_str());
	};
	for (const char* p = s; p < e; ++p) {
		if (*p == '+') {
			apply(tok);
			tok.clear();
		} else if (*p != ' ') {
			tok += *p;
		}
	}
	apply(tok);
	return f;
}

void loadFile(const std::string& path, std::vector<Frame>& out)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (f == nullptr) {
		std::fprintf(stderr, "[move] cannot read %s\n", path.c_str());
		return;
	}
	Frame fr;
	while (std::fread(&fr, sizeof(fr), 1, f) == 1)
		out.push_back(fr);
	std::fclose(f);
}

void init()
{
	sInit = true;
	if (const char* s = std::getenv("SMS_PORT_MOVE_SCRIPT")) {
		for (const char* p = s; *p != '\0';) {
			char* end;
			const long a = std::strtol(p, &end, 10);
			if (end == p || *end != '-')
				break;
			const long b = std::strtol(end + 1, &end, 10);
			if (*end != ':')
				break;
			const char* in = end + 1;
			const char* ie = std::strchr(in, ';');
			if (ie == nullptr)
				ie = in + std::strlen(in);
			sScript.push_back({ (int)a, (int)b, parseInput(in, ie) });
			p = *ie == ';' ? ie + 1 : ie;
		}
		sActive = true;
	}
	if (const char* s = std::getenv("SMS_PORT_MOVE_REC")) {
		sRecPath = s;
		sActive  = true;
	}
	if (const char* s = std::getenv("SMS_PORT_MOVE_PLAY")) {
		sPlayPath = s;
		loadFile(sPlayPath, sPlay);
		sActive = true;
	}
	if (const char* s = std::getenv("SMS_PORT_MOVE_SHOTS"))
		for (const char* p = s; *p != '\0';) {
			char* end;
			const long v = std::strtol(p, &end, 10);
			if (end == p)
				break;
			sShots.push_back((int)v);
			p = *end == ',' ? end + 1 : end;
		}
	int scriptEnd = 0;
	for (const Span& sp : sScript)
		scriptEnd = sp.to > scriptEnd ? sp.to : scriptEnd;
	if (const char* s = std::getenv("SMS_PORT_MOVE_FRAMES"))
		sFrames = std::atoi(s);
	else if (!sPlay.empty())
		sFrames = (int)sPlay.size();
	else
		sFrames = scriptEnd + 60;
	sExit      = std::getenv("SMS_PORT_MOVE_EXIT") != nullptr;
	if (const char* y = std::getenv("SMS_PORT_MOVE_YAW")) {
		sYawSet    = true;
		sYawOffset = (float)std::atof(y);
	}
	if (const char* y = std::getenv("SMS_PORT_MOVE_YAWSIGN"))
		sYawSign = (float)std::atof(y);
	sSkipIntro = sActive || std::getenv("SMS_PORT_SKIP_STAGE_INTRO") != nullptr;
	if (sActive)
		std::fprintf(stderr, "[move] movement test: %zu script spans, %s%s, %d frames\n", sScript.size(),
		             sRecPath.empty() ? "" : "recording", sPlay.empty() ? "" : "playing", sFrames);
}

void logPos(const char* what)
{
	TMario* m = gpMarioOriginal;
	if (m == nullptr)
		return;
	const float deg = m->mFaceAngle.y * (360.0f / 65536.0f);
	std::fprintf(stderr, "[move] %s %d pos %.1f %.1f %.1f angle %.1f status %08x cam %.1f", what, sRel, m->mPosition.x,
	             m->mPosition.y, m->mPosition.z, deg, (unsigned)m->getStatus(), cameraYaw());
	// SMS_PORT_MOVE_DETAIL=1: what Mario stands on and touches (floor height and types)
	static const bool sDetail = std::getenv("SMS_PORT_MOVE_DETAIL") != nullptr;
	if (sDetail) {
		auto type = [](const TBGCheckData* d) { return d != nullptr ? (int)d->mBGType : -1; };
		std::fprintf(stderr, " floor %.1f type %#x wall %#x roof %#x water %#x", m->mFloorPosition.y,
		             type(m->mGroundPlane), type(m->mWallPlane), type(m->mRoofPlane), type(m->mWaterFloor));
		if (m->mWallPlane != nullptr)
			std::fprintf(stderr, " wallN %.2f %.2f %.2f", m->mWallPlane->mNormal.x, m->mWallPlane->mNormal.y,
			             m->mWallPlane->mNormal.z);
	}
	std::fprintf(stderr, "\n");
}

void saveRec()
{
	if (sRecPath.empty())
		return;
	FILE* f = std::fopen(sRecPath.c_str(), "wb");
	if (f == nullptr) {
		std::fprintf(stderr, "[move] cannot write %s\n", sRecPath.c_str());
		return;
	}
	std::fwrite(sRec.data(), sizeof(Frame), sRec.size(), f);
	std::fclose(f);
	std::fprintf(stderr, "[move] recorded %zu frames to %s\n", sRec.size(), sRecPath.c_str());
}

} // namespace

extern "C" {

// port_input_read, once per game frame, after the keyboard, menus and replay: controller 1.
void port_movetest_pads(PADStatus* status)
{
	if (!sInit)
		init();
	if (!sSkipIntro || gpMarDirector == nullptr)
		return;
	PADStatus& p0 = status[0];
	const int state = gpMarDirector->mState;
	if (sRel < 0) {
		// the stage's opening camera (state 1): Start skips it (TMarDirector::changeState)
		if (state == TMarDirector::STATE_UNK1) {
			if (p0.err != PAD_ERR_NONE)
				p0 = PADStatus{};
			p0.err = PAD_ERR_NONE;
			if ((++sSkipTick & 7) == 0)
				p0.button |= PAD_BUTTON_START;
			return;
		}
		// control start: normal play and Mario standing idle (his "GO!" pose over), which lines up
		// stages whose director reaches normal play at different points of that pose
		if (!sActive || state != TMarDirector::STATE_UNK4 || gpMarioOriginal == nullptr
		    || gpMarioOriginal->getStatus() != MARIO_STATUS_WAIT)
			return;
		sRel = 0;
		logPos("start");
		// SMS_PORT_MOVE_NOZZLE=hover|rocket|turbo (tests): that nozzle from control start, full tank
		if (const char* nz = std::getenv("SMS_PORT_MOVE_NOZZLE"))
			if (gpMarioOriginal->mWaterGun != nullptr) {
				const TWaterGun::TNozzleType t = std::strcmp(nz, "rocket") == 0  ? TWaterGun::Rocket
				                                 : std::strcmp(nz, "turbo") == 0 ? TWaterGun::Turbo
				                                                                 : TWaterGun::Hover;
				gpMarioOriginal->mWaterGun->changeNozzle(t, true);
				gpMarioOriginal->mWaterGun->resetWaterToFull();
				std::fprintf(stderr, "[move] nozzle: %s\n", nz);
			}
	}
	if (sDone) {
		if (sExit && sRel >= sFrames + 30) {
			// w12 (perf A/B runs): SMS_PORT_MOVE_SAVE_REPLAY=1 saves the input recording
			// (port_replay.cpp) next to the exe before quitting, so the run can be replayed with
			// SMS_PORT_REPLAY=<file> under another build and compared frame by frame.
			if (std::getenv("SMS_PORT_MOVE_SAVE_REPLAY") != nullptr)
				port_replay_save("move");
			std::fflush(stderr);
			ExitProcess(0);
		}
		++sRel;
		return;
	}
	// this frame's input
	Frame f      = {};
	bool driven  = false;
	if (!sPlay.empty()) {
		if (sRel < (int)sPlay.size())
			f = sPlay[sRel];
		if (f.ignored)
			f = Frame{};
		if (sYawSet && std::getenv("SMS_PORT_MOVE_STICKTURN") == nullptr) {
			// Mario's intended yaw = stick direction + camera heading: the camera's heading is the
			// original's (turned with the stage) for this frame (port_movetest_cam_yaw), so the recorded
			// stick sends Mario the same way
			sCamYawDue = true;
			sCamYaw    = (s16)std::lround((f.camYaw + sYawOffset) * (65536.0f / 360.0f));
			if (gpCamera != nullptr)
				gpCamera->portSetUnk258(sCamYaw);
		} else if (sYawSet && (f.stickX != 0 || f.stickY != 0)) {
			// (SMS_PORT_MOVE_STICKTURN=1: turn the stick instead)
			s16 a;
			float d;
			stickDir(f.stickX, f.stickY, a, d);
			if (d > 0.0f) {
				const float turn = sYawSign * (f.camYaw + sYawOffset - cameraYaw());
				const s16 want   = (s16)(a + (s16)std::lround(turn * (65536.0f / 360.0f)));
				const float c = std::cos(turn * 3.14159265f / 180.0f), sn = std::sin(turn * 3.14159265f / 180.0f);
				const float gx = f.stickX * c - f.stickY * sn, gy = f.stickX * sn + f.stickY * c; // a first guess
				stickFor(want, d, (s8)std::lround(gx < -127.f ? -127.f : (gx > 127.f ? 127.f : gx)),
				         (s8)std::lround(gy < -127.f ? -127.f : (gy > 127.f ? 127.f : gy)), f.stickX, f.stickY);
			}
		}
		driven = true;
	} else if (!sScript.empty()) {
		for (const Span& sp : sScript)
			if (sRel >= sp.from && sRel < sp.to) {
				f.button |= sp.f.button;
				if (sp.f.stickX || sp.f.stickY) {
					f.stickX = sp.f.stickX;
					f.stickY = sp.f.stickY;
				}
				f.triggerLeft  = f.triggerLeft > sp.f.triggerLeft ? f.triggerLeft : sp.f.triggerLeft;
				f.triggerRight = f.triggerRight > sp.f.triggerRight ? f.triggerRight : sp.f.triggerRight;
			}
		driven = true;
	}
	if (driven) {
		p0              = PADStatus{};
		p0.err          = PAD_ERR_NONE;
		p0.button       = f.button;
		p0.stickX       = f.stickX;
		p0.stickY       = f.stickY;
		p0.substickX    = f.substickX;
		p0.substickY    = f.substickY;
		p0.triggerLeft  = f.triggerLeft;
		p0.triggerRight = f.triggerRight;
	} else {
		f = { p0.button, p0.stickX, p0.stickY, p0.substickX, p0.substickY, p0.triggerLeft, p0.triggerRight, 0.0f, 0 };
	}
	f.camYaw = cameraYaw();
	if (!sRecPath.empty() && sPlay.empty()) {
		// some stages ignore input for a while after Mario stands idle (the plaza): those frames are
		// marked, so a replay elsewhere starts moving on the same frame
		const bool idle = gpMarioOriginal != nullptr && gpMarioOriginal->getStatus() == MARIO_STATUS_WAIT;
		if (!idle && sRel > 0 && !sMovedYet) {
			sMovedYet = true;
			if (!sRec.empty())
				sRec.back().ignored = 0; // (the status shows the previous frame's input: that one counted)
		}
		f.ignored = !sMovedYet && (f.stickX != 0 || f.stickY != 0 || f.button != 0) ? 1 : 0;
	}
	if (!sRecPath.empty())
		sRec.push_back(f);
	for (int s : sShots)
		if (s == sRel)
			sShotDue = 900000 + s;
	static const bool sEvery = std::getenv("SMS_PORT_MOVE_DETAIL") != nullptr;
	if (sEvery || sRel % 10 == 0)
		logPos("at");
	if (++sRel >= sFrames) {
		logPos("end");
		saveRec();
		sDone = true;
	}
}

// CPolarSubCamera::calcExternalData_ (cameragc.cpp): while a replay plays, the camera's heading (the
// one Mario's controls add the stick to) is the recorded one.
int port_movetest_cam_yaw(s16* yaw)
{
	if (!sCamYawDue || sDone)
		return 0;
	*yaw = sCamYaw;
	return 1;
}

// port_main's screenshot step: 900000 + frame when a movement-test screenshot is due (once).
// Other test knobs: a screenshot at the next present, named after `tag` (shot_<tag>.bmp).
void port_shot_request(unsigned tag) { sShotDue = (int)tag; }

unsigned port_movetest_shot_due(void)
{
	const unsigned d = (unsigned)sShotDue;
	sShotDue         = 0;
	return d;
}

} // extern "C"

#endif // SMS_PORT_AURORA
