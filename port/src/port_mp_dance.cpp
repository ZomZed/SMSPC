// Dance emotes: original dance moves posed on Mario's own skeleton (no animation files).
//
// After the body model's normal calc (the idle animation), selected joints are turned by a
// keyframed pose for the dance's current time: each joint rotates about its own pivot in
// Mario's body space (right / up / forward, taken from the model each frame), carrying
// everything below it in the joint tree; then the skin's envelope matrices are rebuilt. The
// hands, cap and FLUDD read the joint matrices afterwards, so they follow.
//   - the local Mario: TMario::calcAnim (MarioDraw.cpp) calls port_mp_dance_pose_local;
//   - other players' puppets: port_mp_puppet.cpp calls port_mp_dance_pose_slot (the dance
//     arrives with the emote broadcast, port_mp_chat.cpp -> port_mp_dance_remote_start).
// Joints (ma_mdl1.bmd): center > jnt_waist > jnt_leg_[RL]1 > jnt_leg_[RL]2 > feet;
// center > chn_chest > jnt_chest > jnt_sldr_[RL] > jnt_arm_[RL]1 > jnt_arm_[RL]2 > jnt_hand_[RL];
// jnt_chest > jnt_head.
//
// Custom emotes: clips animated in Blender on the exported rig (mario_rig/README.txt), turned into
// .smsemote files by mario_rig/glb2emote.py and loaded from <exe dir>\emotes at start-up. They
// are dance ids kCustomBase.. (port_mp_emote.cpp lists them after the built-in emotes).
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <JSystem/J3D/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/JUtility/JUTNameTab.hpp>
#include <dolphin/mtx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

const float kPi = 3.14159265f;

// ---- the dances --------------------------------------------------------------------------------
struct Rot {
	float yaw, pitch, roll; // degrees about Mario's up / right / forward axes
};
struct DancePose {
	Rot center, chest, head;
	Rot armR, elbowR, armL, elbowL; // the left side is given mirrored (as if it were the right)
	Rot legR, kneeR, legL, kneeL;
	float lift; // the whole body raised (units)
};

struct DanceDef {
	const char* name;
	float length; // seconds; 0 = loops until Mario moves
};
// Index = dance id (port_mp_emote.cpp maps emotes to these). Append only.
const DanceDef kDances[] = {
	{ "Cheer", 0.0f },  { "Wave", 0.0f },  { "Clap", 0.0f }, { "Disco", 0.0f },        { "Robot", 0.0f },
	{ "Spin", 0.0f },   { "Groove", 0.0f }, { "Flex", 0.0f }, { "Jumping jacks", 0.0f }, { "Bow", 3.0f },
	// popular viral moves, recreated from scratch (generic move names, our own keyframes)
	{ "Floss", 0.0f },  { "Dab", 2.5f },    { "Windmill", 0.0f }, { "Shuffle", 0.0f },
};
const int kDanceCount = (int)(sizeof(kDances) / sizeof(kDances[0]));

float wave(float t, float hz) { return std::sin(t * 2.0f * kPi * hz); }
float smooth01(float x) { x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); return x * x * (3.0f - 2.0f * x); }

// Arm helpers (the right arm; the left gets the same numbers mirrored):
//   raise = up to the side (0 hanging, 60 straight out, 120 overhead: the arms already hang ~30 out)
//   front = forwards (0 down, 90 straight ahead)
//   twist = turned inwards (towards the other arm)
// Axis signs (Mario faces f, up u, right r = f x u): +roll swings a hanging arm inwards, +pitch
// forwards, +yaw turns a forward-pointing arm inwards; a forearm along r bends down with +roll.
// Heads and chests: +pitch tilts up / leans back.
void arm(Rot& r, float raise, float front, float twist = 0.0f)
{
	r.roll  = -raise;
	r.pitch = front;
	r.yaw   = twist;
}

void dancePose(int id, float t, DancePose& p)
{
	std::memset(&p, 0, sizeof(p));
	switch (id) {
	case 0: { // Cheer: both arms up, hands waving, a little hop on the beat
		const float s = wave(t, 2.0f);
		arm(p.armR, 118.0f + 6.0f * s, 10.0f);
		arm(p.armL, 118.0f - 6.0f * s, 10.0f);
		p.elbowR.roll = 25.0f * s;
		p.elbowL.roll = -25.0f * s;
		p.lift        = 10.0f * std::fabs(wave(t, 1.0f));
		p.head.pitch  = 10.0f; // looking up
		break;
	}
	case 1: { // Wave: the right arm up, the forearm waving
		const float s = wave(t, 2.2f);
		arm(p.armR, 110.0f, 15.0f);
		p.elbowR.roll = 30.0f * s;
		p.head.roll   = 8.0f;
		p.chest.roll  = -4.0f;
		break;
	}
	case 2: { // Clap: arms forward, hands meeting in front, bouncing
		const float s = 0.5f + 0.5f * wave(t, 2.0f);
		arm(p.armR, 15.0f, 75.0f, 30.0f * s);
		arm(p.armL, 15.0f, 75.0f, 30.0f * s);
		p.elbowR.yaw = 20.0f * s;
		p.elbowL.yaw = 20.0f * s;
		p.lift       = 5.0f * s;
		break;
	}
	case 3: { // Disco: the right arm points up high, then down across, hips swaying
		const float beat = std::fmod(t * 1.6f, 2.0f);
		const float k    = smooth01(beat < 1.0f ? beat * 4.0f : (beat - 1.0f) * 4.0f);
		const float up   = beat < 1.0f ? k : 1.0f - k;
		arm(p.armR, 5.0f + 110.0f * up, 20.0f, 0.0f);
		p.elbowR.roll = 10.0f;
		arm(p.armL, 35.0f, 0.0f);
		p.elbowL.roll  = 80.0f; // forearm in: the hand on the hip
		p.center.roll  = 8.0f * wave(t, 0.8f);
		p.chest.roll   = -6.0f * wave(t, 0.8f);
		p.head.roll    = 6.0f * wave(t, 0.8f);
		break;
	}
	case 4: { // Robot: stiff moves that snap between four poses
		static const float kPoses[4][6] = {
			// armR raise, front, elbowR, armL raise, front, elbowL
			{ 60.0f, 0.0f, 90.0f, -5.0f, 60.0f, 0.0f },
			{ -5.0f, 60.0f, 0.0f, 60.0f, 0.0f, 90.0f },
			{ 60.0f, 0.0f, -90.0f, 60.0f, 0.0f, -90.0f },
			{ -5.0f, 90.0f, 0.0f, -5.0f, 90.0f, 0.0f },
		};
		const int i      = (int)(t * 2.0f) & 3;
		const int n      = (i + 1) & 3;
		const float f    = t * 2.0f - std::floor(t * 2.0f);
		const float snap = smooth01((f - 0.8f) * 5.0f); // holds, then a quick snap to the next
		float v[6];
		for (int k = 0; k < 6; k++)
			v[k] = kPoses[i][k] + (kPoses[n][k] - kPoses[i][k]) * snap;
		arm(p.armR, v[0], v[1]);
		p.elbowR.roll = v[2];
		arm(p.armL, v[3], v[4]);
		p.elbowL.roll = v[5];
		p.chest.yaw   = ((i & 1) ? 15.0f : -15.0f);
		p.head.yaw    = ((i & 1) ? -25.0f : 25.0f);
		break;
	}
	case 5: { // Spin: arms out, turning round and round
		p.center.yaw = std::fmod(t * 300.0f, 360.0f);
		arm(p.armR, 60.0f + 5.0f * wave(t, 1.0f), 0.0f);
		arm(p.armL, 60.0f - 5.0f * wave(t, 1.0f), 0.0f);
		p.head.pitch = 8.0f;
		p.lift       = 4.0f;
		break;
	}
	case 6: { // Groove: side to side, arms swinging, nodding on the beat
		const float s = wave(t, 1.0f);
		p.center.roll = 9.0f * s;
		p.chest.roll  = -12.0f * s;
		arm(p.armR, 15.0f, 35.0f * s);
		arm(p.armL, 15.0f, -35.0f * s);
		p.elbowR.pitch = 40.0f;
		p.elbowL.pitch = 40.0f;
		p.head.pitch   = -10.0f * std::fabs(wave(t, 2.0f)); // nodding
		break;
	}
	case 7: { // Flex: arms up at the sides, elbows bent, squeezing in turn
		const float s = 0.5f + 0.5f * wave(t, 1.2f);
		arm(p.armR, 62.0f, 0.0f);
		arm(p.armL, 62.0f, 0.0f);
		p.elbowR.roll = -(95.0f + 20.0f * s); // forearms up
		p.elbowL.roll = -(95.0f + 20.0f * (1.0f - s));
		p.chest.pitch = 8.0f; // chest out
		p.chest.yaw   = 10.0f * (s - 0.5f);
		break;
	}
	case 8: { // Jumping jacks: arms and legs out and in with a hop
		const float s = 0.5f - 0.5f * std::cos(t * 2.0f * kPi * 1.4f);
		arm(p.armR, -5.0f + 122.0f * s, 0.0f);
		arm(p.armL, -5.0f + 122.0f * s, 0.0f);
		p.legR.roll = -22.0f * s; // legs out
		p.legL.roll = -22.0f * s;
		p.lift      = 14.0f * std::sin(t * 2.0f * kPi * 1.4f) * (std::sin(t * 2.0f * kPi * 1.4f) > 0.0f ? 1.0f : 0.0f);
		break;
	}
	case 9: { // Bow: a slow bow with the right arm across the chest
		const float in  = smooth01(t / 0.8f);
		const float out = smooth01((t - 2.2f) / 0.8f);
		const float k   = in * (1.0f - out);
		p.chest.pitch   = -45.0f * k; // leaning forwards
		p.head.pitch    = -20.0f * k;
		arm(p.armR, 20.0f * k, 60.0f * k, 40.0f * k);
		p.elbowR.yaw    = 60.0f * k;
		arm(p.armL, 15.0f * k, -20.0f * k);
		break;
	}
	case 10: { // Floss: straight arms swinging side to side past the hips, one in front, one behind
		const float s = wave(t, 1.6f);                    // which side
		const float c = std::cos(t * 2.0f * kPi * 3.2f);  // front / back, twice as fast
		arm(p.armR, 28.0f * s, 22.0f * c);
		arm(p.armL, -28.0f * s, -22.0f * c);
		p.center.roll = -7.0f * s; // the hips go the other way
		p.chest.roll  = 4.0f * s;
		p.head.roll   = 3.0f * s;
		break;
	}
	case 11: { // Dab: duck the head into the right elbow, the left arm out and up
		const float in  = smooth01(t / 0.25f);
		const float out = smooth01((t - 2.0f) / 0.4f);
		const float k   = in * (1.0f - out);
		arm(p.armR, 55.0f * k, 70.0f * k, 70.0f * k);
		p.elbowR.yaw  = 95.0f * k;
		arm(p.armL, 95.0f * k, 30.0f * k);
		p.head.pitch  = -25.0f * k;
		p.head.yaw    = 30.0f * k;
		p.chest.pitch = -10.0f * k;
		break;
	}
	case 12: { // Windmill: arm circles, one arm after the other, bouncing
		const float a = std::fmod(t * 360.0f * 0.9f, 360.0f);
		p.armR.pitch  = a;
		p.armL.pitch  = std::fmod(a + 180.0f, 360.0f);
		p.armR.roll   = -15.0f;
		p.armL.roll   = -15.0f;
		p.lift        = 6.0f * std::fabs(wave(t, 0.9f));
		p.chest.yaw   = 8.0f * wave(t, 0.9f);
		break;
	}
	case 13: { // Shuffle: stepping in place, legs scissoring, arms pumping
		const float s = wave(t, 1.8f);
		p.legR.pitch  = 28.0f * s;
		p.legL.pitch  = -28.0f * s;
		p.kneeR.pitch = -25.0f * (s < 0.0f ? -s : 0.0f);
		p.kneeL.pitch = -25.0f * (s > 0.0f ? s : 0.0f);
		arm(p.armR, 5.0f, -30.0f * s);
		arm(p.armL, 5.0f, 30.0f * s);
		p.elbowR.pitch = 70.0f;
		p.elbowL.pitch = 70.0f;
		p.lift         = 7.0f * std::fabs(s);
		p.center.yaw   = 6.0f * s;
		break;
	}
	// calibration (SMS_PORT_DANCE_TEST=100..103): one joint, one axis
	case 100: arm(p.armR, 60.0f, 0.0f); break;   // right arm out to the side
	case 101: arm(p.armR, 0.0f, 90.0f); break;   // right arm straight ahead
	case 102: arm(p.armR, 120.0f, 0.0f); break;  // right arm overhead
	case 103: p.chest.pitch = -40.0f; break;     // leaning forwards
	default: break;
	}
}

// ---- posing a model ------------------------------------------------------------------------------
struct Rig {
	J3DModelData* data;
	int center, chest, head, armR, elbowR, armL, elbowL, legR, kneeR, legL, kneeL;
	u8 below[64][64]; // below[j][k]: joint k is j or under it (Mario has 29 joints)
	int num;
};
Rig sRigs[4];

int jointIndex(J3DModelData* d, const char* name)
{
	const int i = d->getJointName()->getIndex(name);
	return i >= 0 && i < d->getJointNum() ? i : -1;
}

void markBelow(Rig& r, int top, J3DNode* n)
{
	for (; n != nullptr; n = n->getYounger()) {
		const int k = ((J3DJoint*)n)->getJntNo();
		if (k < 64)
			r.below[top][k] = 1;
		markBelow(r, top, n->getChild());
	}
}

Rig* rigFor(J3DModelData* d)
{
	for (Rig& r : sRigs)
		if (r.data == d)
			return &r;
	Rig* r = nullptr;
	for (Rig& x : sRigs)
		if (x.data == nullptr) {
			r = &x;
			break;
		}
	if (r == nullptr)
		r = &sRigs[0]; // reuse (models come and go with stages)
	std::memset(r, 0, sizeof(*r));
	r->data = d;
	r->num  = d->getJointNum() < 64 ? d->getJointNum() : 64;
	for (int j = 0; j < r->num; j++) {
		r->below[j][j] = 1;
		markBelow(*r, j, d->getJointNodePointer((u16)j)->getChild());
	}
	r->center = jointIndex(d, "center");
	r->chest  = jointIndex(d, "jnt_chest");
	r->head   = jointIndex(d, "jnt_head");
	r->armR   = jointIndex(d, "jnt_arm_R1");
	r->elbowR = jointIndex(d, "jnt_arm_R2");
	r->armL   = jointIndex(d, "jnt_arm_L1");
	r->elbowL = jointIndex(d, "jnt_arm_L2");
	r->legR   = jointIndex(d, "jnt_leg_R1");
	r->kneeR  = jointIndex(d, "jnt_leg_R2");
	r->legL   = jointIndex(d, "jnt_leg_L1");
	r->kneeL  = jointIndex(d, "jnt_leg_L2");
	return r;
}

void norm(Vec& v)
{
	const float l = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
	if (l > 1e-6f) {
		v.x /= l;
		v.y /= l;
		v.z /= l;
	}
}

// Rotation by `deg` about the unit axis a (world), as a 3x4 with no translation.
void axisAngle(Mtx out, const Vec& a, float deg)
{
	const float r = deg * kPi / 180.0f, c = std::cos(r), s = std::sin(r), t = 1.0f - c;
	out[0][0] = t * a.x * a.x + c;       out[0][1] = t * a.x * a.y - s * a.z; out[0][2] = t * a.x * a.z + s * a.y;
	out[1][0] = t * a.x * a.y + s * a.z; out[1][1] = t * a.y * a.y + c;       out[1][2] = t * a.y * a.z - s * a.x;
	out[2][0] = t * a.x * a.z - s * a.y; out[2][1] = t * a.y * a.z + s * a.x; out[2][2] = t * a.z * a.z + c;
	out[0][3] = out[1][3] = out[2][3] = 0.0f;
}

struct Frame {
	Vec right, up, fwd;
};

// Turns joint j (and everything under it) by rot about its pivot, in body space. `mirror`:
// a left-side joint (yaw and roll mirrored).
void turn(J3DModel* m, const Rig& rig, const Frame& f, int j, Rot rot, bool mirror)
{
	if (j < 0 || (rot.yaw == 0.0f && rot.pitch == 0.0f && rot.roll == 0.0f))
		return;
	if (mirror) {
		rot.yaw  = -rot.yaw;
		rot.roll = -rot.roll;
	}
	Mtx ry, rp, rr, r;
	axisAngle(ry, f.up, rot.yaw);
	axisAngle(rp, f.right, rot.pitch);
	axisAngle(rr, f.fwd, rot.roll);
	MTXConcat(rp, ry, r);
	MTXConcat(rr, r, r);
	// about the pivot: T(p) R T(-p)
	MtxPtr pj = m->getAnmMtx((u16)j);
	const Vec p = { pj[0][3], pj[1][3], pj[2][3] };
	r[0][3] = p.x - (r[0][0] * p.x + r[0][1] * p.y + r[0][2] * p.z);
	r[1][3] = p.y - (r[1][0] * p.x + r[1][1] * p.y + r[1][2] * p.z);
	r[2][3] = p.z - (r[2][0] * p.x + r[2][1] * p.y + r[2][2] * p.z);
	for (int k = 0; k < rig.num; k++)
		if (rig.below[j][k]) {
			MtxPtr a = m->getAnmMtx((u16)k);
			MTXConcat(r, a, a);
		}
}

// ---- custom emotes (Blender clips) ---------------------------------------------------------------
// A .smsemote file, little endian:
//   "SMSE", u32 version (1), char name[32], u32 flags (1 = loops), f32 fps, u32 frames, u32 joints,
//   char joint[joints][16], then frames x joints x { f32 t[3] (game units), q[4] (xyzw), s[3] }:
//   each joint's local transform in the model's own joint frames (what JNT1 / a BCK gives).
// Played by sampling the clip (neighbouring frames blended), chaining the locals down the joint
// tree from the model's root matrix (the game's own, so Mario's position, facing and scale hold),
// fading in over the game's pose (and out at the end of a one-shot clip), then rebuilding the skin.
const int kCustomBase = 200;
const float kFade     = 0.2f; // seconds
struct Custom {
	char name[32];
	bool loop;
	float fps;
	int frames, joints;
	std::vector<std::string> jointNames;
	std::vector<float> data; // frames x joints x 10
	J3DModelData* mapFor;    // the model `map` was made for
	int map[64];             // model joint -> clip joint (-1: keep the game's)
};
std::vector<Custom> sCustom;

float customLength(const Custom& c) { return c.frames > 1 ? (float)(c.frames - 1) / c.fps : 0.0f; }

std::string emotesDir()
{
	if (const char* e = std::getenv("SMS_PORT_EMOTES_DIR"))
		if (*e != '\0')
			return e;
	char buf[MAX_PATH];
	const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	std::string base(buf, n);
	const size_t slash = base.find_last_of("\\/");
	return (slash == std::string::npos ? std::string(".") : base.substr(0, slash)) + "\\emotes";
}

bool loadCustom(const std::string& path, Custom& c)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	std::vector<unsigned char> d;
	unsigned char buf[65536];
	size_t n;
	while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
		d.insert(d.end(), buf, buf + n);
	std::fclose(f);
	auto u32at = [&](size_t o) { u32 v; std::memcpy(&v, &d[o], 4); return v; };
	if (d.size() < 56 || std::memcmp(d.data(), "SMSE", 4) != 0 || u32at(4) != 1)
		return false;
	std::memcpy(c.name, &d[8], 32);
	c.name[31] = '\0';
	c.loop     = (u32at(40) & 1) != 0;
	std::memcpy(&c.fps, &d[44], 4);
	c.frames = (int)u32at(48);
	c.joints = (int)u32at(52);
	const size_t names = 56, body = names + (size_t)c.joints * 16;
	if (c.frames < 1 || c.joints < 1 || c.joints > 64 || !(c.fps > 0.0f)
	    || d.size() < body + (size_t)c.frames * c.joints * 40)
		return false;
	for (int j = 0; j < c.joints; j++) {
		char nm[17] = {};
		std::memcpy(nm, &d[names + j * 16], 16);
		c.jointNames.push_back(nm);
	}
	c.data.resize((size_t)c.frames * c.joints * 10);
	std::memcpy(c.data.data(), &d[body], c.data.size() * 4);
	c.mapFor = nullptr;
	return true;
}

// Every .smsemote in the emotes folder, in file-name order (the emote ids follow it).
struct LoadCustom {
	LoadCustom()
	{
		const std::string dir = emotesDir();
		std::vector<std::string> files;
		WIN32_FIND_DATAA fd;
		HANDLE h = FindFirstFileA((dir + "\\*.smsemote").c_str(), &fd);
		if (h != INVALID_HANDLE_VALUE) {
			do
				files.push_back(fd.cFileName);
			while (FindNextFileA(h, &fd));
			FindClose(h);
		}
		std::sort(files.begin(), files.end(), [](const std::string& a, const std::string& b) {
			return _stricmp(a.c_str(), b.c_str()) < 0;
		});
		for (const std::string& name : files) {
			Custom c;
			if (sCustom.size() < 48 && loadCustom(dir + "\\" + name, c))
				sCustom.push_back(std::move(c));
			else
				std::fprintf(stderr, "[dance] %s: not a custom emote (or too many), skipped\n", name.c_str());
		}
		if (!sCustom.empty())
			std::fprintf(stderr, "[dance] %d custom emote(s) from %s\n", (int)sCustom.size(), dir.c_str());
	}
} sLoadCustom;

struct Quat {
	float x, y, z, w;
};

Quat quatOf(const float m[3][3])
{
	Quat q;
	const float tr = m[0][0] + m[1][1] + m[2][2];
	if (tr > 0.0f) {
		const float k = std::sqrt(tr + 1.0f) * 2.0f;
		q = { (m[2][1] - m[1][2]) / k, (m[0][2] - m[2][0]) / k, (m[1][0] - m[0][1]) / k, 0.25f * k };
	} else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
		const float k = std::sqrt(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f;
		q = { 0.25f * k, (m[0][1] + m[1][0]) / k, (m[0][2] + m[2][0]) / k, (m[2][1] - m[1][2]) / k };
	} else if (m[1][1] > m[2][2]) {
		const float k = std::sqrt(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f;
		q = { (m[0][1] + m[1][0]) / k, 0.25f * k, (m[1][2] + m[2][1]) / k, (m[0][2] - m[2][0]) / k };
	} else {
		const float k = std::sqrt(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f;
		q = { (m[0][2] + m[2][0]) / k, (m[1][2] + m[2][1]) / k, 0.25f * k, (m[1][0] - m[0][1]) / k };
	}
	return q;
}

Quat slerp(Quat a, Quat b, float f)
{
	float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	if (d < 0.0f) {
		b = { -b.x, -b.y, -b.z, -b.w };
		d = -d;
	}
	float ka = 1.0f - f, kb = f;
	if (d < 0.9995f) {
		const float th = std::acos(d), s = std::sin(th);
		ka = std::sin((1.0f - f) * th) / s;
		kb = std::sin(f * th) / s;
	}
	Quat r = { a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb, a.w * ka + b.w * kb };
	const float l = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
	return { r.x / l, r.y / l, r.z / l, r.w / l };
}

// translation, rotation, scale -> 3x4
void trsMtx(Mtx out, const float t[3], Quat q, const float s[3])
{
	const float x = q.x, y = q.y, z = q.z, w = q.w;
	const float r[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
		                    { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
		                    { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
	for (int i = 0; i < 3; i++) {
		out[i][0] = r[i][0] * s[0];
		out[i][1] = r[i][1] * s[1];
		out[i][2] = r[i][2] * s[2];
		out[i][3] = t[i];
	}
}

// 3x4 -> translation, rotation, scale (no shear)
void mtxTrs(const Mtx m, float t[3], Quat& q, float s[3])
{
	float r[3][3];
	for (int c = 0; c < 3; c++) {
		s[c] = std::sqrt(m[0][c] * m[0][c] + m[1][c] * m[1][c] + m[2][c] * m[2][c]);
		const float k = s[c] > 1e-6f ? 1.0f / s[c] : 0.0f;
		for (int i = 0; i < 3; i++)
			r[i][c] = m[i][c] * k;
	}
	for (int i = 0; i < 3; i++)
		t[i] = m[i][3];
	q = quatOf(r);
}

// The clip's local transform of clip joint j at time t.
void sampleLocal(const Custom& c, int j, float t, Mtx out)
{
	const float len = customLength(c);
	if (c.loop && len > 0.0f)
		t = std::fmod(t, len);
	float f = t * c.fps;
	f       = f < 0.0f ? 0.0f : (f > (float)(c.frames - 1) ? (float)(c.frames - 1) : f);
	const int f0 = (int)f, f1 = f0 + 1 < c.frames ? f0 + 1 : f0;
	const float a = f - (float)f0;
	const float* p = &c.data[((size_t)f0 * c.joints + j) * 10];
	const float* n = &c.data[((size_t)f1 * c.joints + j) * 10];
	float tr[3], sc[3];
	for (int i = 0; i < 3; i++) {
		tr[i] = p[i] + (n[i] - p[i]) * a;
		sc[i] = p[7 + i] + (n[7 + i] - p[7 + i]) * a;
	}
	trsMtx(out, tr, slerp({ p[3], p[4], p[5], p[6] }, { n[3], n[4], n[5], n[6] }, a), sc);
}

void chainCustom(J3DModel* m, const Custom& c, J3DNode* n, MtxPtr parentT, int parent, Mtx* orig, Mtx* target,
                 float t)
{
	for (; n != nullptr; n = n->getYounger()) {
		const int k = ((J3DJoint*)n)->getJntNo();
		if (k >= 64)
			continue;
		Mtx local;
		if (c.map[k] >= 0) {
			sampleLocal(c, c.map[k], t, local);
		} else { // not in the clip: as the game posed it, relative to its parent
			Mtx inv;
			MTXInverse(orig[parent], inv);
			MTXConcat(inv, orig[k], local);
		}
		MTXConcat(parentT, local, target[k]);
		chainCustom(m, c, n->getChild(), target[k], k, orig, target, t);
	}
}

void poseCustom(J3DModel* m, int i, float t)
{
	Custom& c       = sCustom[i];
	J3DModelData* d = m->getModelData();
	const int num   = d->getJointNum() < 64 ? d->getJointNum() : 64;
	if (num < 1)
		return;
	if (c.mapFor != d) {
		for (int k = 0; k < 64; k++) {
			c.map[k] = -1;
			if (k < num)
				for (int j = 0; j < c.joints; j++)
					if (c.jointNames[j] == d->getJointName()->getName((u16)k))
						c.map[k] = j;
		}
		c.mapFor = d;
	}
	float w         = smooth01(t / kFade); // fade in
	const float len = customLength(c);
	if (!c.loop)
		w *= smooth01((len - t) / kFade); // and out at the end
	static Mtx orig[64], target[64];
	for (int k = 0; k < num; k++)
		MTXCopy(m->getAnmMtx((u16)k), orig[k]);
	// the root joint stays where the game put it (Mario's place, facing and scale): the clip
	// moves the body through "center" and below
	J3DJoint* root = d->getJointNodePointer(0);
	MTXCopy(orig[0], target[0]);
	chainCustom(m, c, root->getChild(), target[0], 0, orig, target, t);
	for (int k = 0; k < num; k++) {
		MtxPtr a = m->getAnmMtx((u16)k);
		if (w >= 0.999f) {
			MTXCopy(target[k], a);
			continue;
		}
		float t0[3], s0[3], t1[3], s1[3], tr[3], sc[3];
		Quat q0, q1;
		mtxTrs(orig[k], t0, q0, s0);
		mtxTrs(target[k], t1, q1, s1);
		for (int e = 0; e < 3; e++) {
			tr[e] = t0[e] + (t1[e] - t0[e]) * w;
			sc[e] = s0[e] + (s1[e] - s0[e]) * w;
		}
		trsMtx(a, tr, slerp(q0, q1, w), sc);
	}
	m->calcWeightEnvelopeMtx(); // the skin follows the moved joints
}

bool danceValid(int id)
{
	return (id >= 0 && id < kDanceCount) || (id >= kCustomBase && id < kCustomBase + (int)sCustom.size());
}
// seconds; 0 = loops until Mario moves
float danceLength(int id)
{
	if (id >= 0 && id < kDanceCount)
		return kDances[id].length;
	if (id >= kCustomBase && id < kCustomBase + (int)sCustom.size())
		return sCustom[id - kCustomBase].loop ? 0.0f : customLength(sCustom[id - kCustomBase]);
	return 0.0f;
}

void poseModel(J3DModel* m, int dance, float t)
{
	if (m != nullptr && dance >= kCustomBase && dance < kCustomBase + (int)sCustom.size()) {
		poseCustom(m, dance - kCustomBase, t);
		return;
	}
	if (m == nullptr || dance < 0 || (dance >= kDanceCount && dance < 100))
		return;
	J3DModelData* d = m->getModelData();
	Rig* rig        = rigFor(d);
	if (rig->center < 0 || rig->armR < 0 || rig->armL < 0)
		return;
	DancePose p;
	dancePose(dance, t, p);
	// Mario's body axes now: up and forward from the model's base matrix, right from his shoulders
	MtxPtr b = m->getBaseTRMtx();
	Frame f;
	f.up  = { b[0][1], b[1][1], b[2][1] };
	f.fwd = { b[0][2], b[1][2], b[2][2] };
	norm(f.up);
	norm(f.fwd);
	MtxPtr ar = m->getAnmMtx((u16)rig->armR), al = m->getAnmMtx((u16)rig->armL);
	f.right = { ar[0][3] - al[0][3], ar[1][3] - al[1][3], ar[2][3] - al[2][3] };
	const float dU = f.right.x * f.up.x + f.right.y * f.up.y + f.right.z * f.up.z;
	f.right.x -= dU * f.up.x;
	f.right.y -= dU * f.up.y;
	f.right.z -= dU * f.up.z;
	norm(f.right);
	static bool sLogged;
	if (!sLogged && std::getenv("SMS_PORT_DANCE_TEST")) {
		sLogged = true;
		std::fprintf(stderr, "[dance] up %.2f %.2f %.2f fwd %.2f %.2f %.2f right %.2f %.2f %.2f\n", f.up.x, f.up.y, f.up.z,
		             f.fwd.x, f.fwd.y, f.fwd.z, f.right.x, f.right.y, f.right.z);
	}
	// parents before children
	turn(m, *rig, f, rig->center, p.center, false);
	turn(m, *rig, f, rig->chest, p.chest, false);
	turn(m, *rig, f, rig->head, p.head, false);
	turn(m, *rig, f, rig->armR, p.armR, false);
	turn(m, *rig, f, rig->elbowR, p.elbowR, false);
	turn(m, *rig, f, rig->armL, p.armL, true);
	turn(m, *rig, f, rig->elbowL, p.elbowL, true);
	turn(m, *rig, f, rig->legR, p.legR, false);
	turn(m, *rig, f, rig->kneeR, p.kneeR, false);
	turn(m, *rig, f, rig->legL, p.legL, true);
	turn(m, *rig, f, rig->kneeL, p.kneeL, true);
	if (p.lift != 0.0f)
		for (int k = 0; k < rig->num; k++) {
			MtxPtr a = m->getAnmMtx((u16)k);
			a[0][3] += f.up.x * p.lift;
			a[1][3] += f.up.y * p.lift;
			a[2][3] += f.up.z * p.lift;
		}
	m->calcWeightEnvelopeMtx(); // the skin follows the moved joints
}

// ---- who dances --------------------------------------------------------------------------------
int sLocalDance = -1;
double sLocalStart;
int sPreviewDance = -1; // the Customize menu's preview (file select)
double sPreviewStart;
struct Remote {
	int dance = -1;
	double start;
	u16 bck;      // the body animation when it began (another one = he moved on)
	bool haveBck = false;
};
Remote sRemote[PORT_MP_MAX_PLAYERS];

} // namespace

extern "C" {

int port_mp_dance_count(void) { return kDanceCount; }
const char* port_mp_dance_name(int id) { return id >= 0 && id < kDanceCount ? kDances[id].name : "?"; }
float port_mp_dance_length(int id) { return danceLength(id); }
// The custom emotes (Blender clips) for port_mp_emote.cpp's table.
int port_mp_dance_custom_count(void) { return (int)sCustom.size(); }
const char* port_mp_dance_custom_name(int i) { return i >= 0 && i < (int)sCustom.size() ? sCustom[i].name : "?"; }
int port_mp_dance_custom_id(int i) { return kCustomBase + i; }

// port_mp_emote.cpp: the local Mario starts / stops dancing (-1).
void port_mp_dance_local(int id)
{
	sLocalDance = danceValid(id) ? id : -1;
	sLocalStart = port_mp_time_ms();
}
// The Customize menu: preview a dance on the file-select Mario (-1 = stop).
void port_mp_dance_preview(int id)
{
	if (id != sPreviewDance)
		sPreviewStart = port_mp_time_ms();
	sPreviewDance = danceValid(id) ? id : -1;
}
int port_mp_dance_previewing(void) { return sPreviewDance; }

// port_mp_chat.cpp: another player's emote arrived (-1 = not a dance: stop).
void port_mp_dance_remote_start(int slot, int dance)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PLAYERS)
		return;
	sRemote[slot].dance   = danceValid(dance) ? dance : -1;
	sRemote[slot].start   = port_mp_time_ms();
	sRemote[slot].haveBck = false;
}

// TMario::calcAnim, after the body's calc (MarioDraw.cpp).
void port_mp_dance_pose_local(void* model)
{
	// SMS_PORT_DANCE_TEST=<id> (debug): the local Mario always dances that one (screenshots)
	static const char* sTest = std::getenv("SMS_PORT_DANCE_TEST");
	int dance    = sLocalDance;
	double start = sLocalStart;
	if (dance < 0 && sPreviewDance >= 0) {
		dance = sPreviewDance;
		start = sPreviewStart;
	}
	if (dance < 0 && sTest != nullptr) {
		dance = std::atoi(sTest);
		start = 0.0;
	}
	if (dance < 0)
		return;
	float t = (float)((port_mp_time_ms() - start) / 1000.0);
	const float len = danceLength(dance);
	if (len > 0.0f && t > len) {
		if (dance == sPreviewDance) // previews loop
			t = std::fmod(t, len + 0.6f);
		else
			return;
	}
	poseModel((J3DModel*)model, dance, t);
}

// port_mp_puppet.cpp, after a puppet's body calc: that player's dance, if any. `bck`: his body
// animation now (a different one than when the dance began = he moved on).
void port_mp_dance_pose_slot(int slot, void* model, unsigned bck)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PLAYERS)
		return;
	Remote& r = sRemote[slot];
	if (r.dance < 0)
		return;
	if (!r.haveBck) {
		r.bck     = (u16)bck;
		r.haveBck = true;
	} else if (r.bck != (u16)bck) {
		r.dance = -1;
		return;
	}
	const float t = (float)((port_mp_time_ms() - r.start) / 1000.0);
	if (danceLength(r.dance) > 0.0f && t > danceLength(r.dance)) {
		r.dance = -1;
		return;
	}
	poseModel((J3DModel*)model, r.dance, t);
}

} // extern "C"

#endif // SMS_PORT_AURORA
