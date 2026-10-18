// Frame-rate option for the Aurora build (w9): display frames between the game's 30 Hz
// logic frames, with interpolated camera and J3D model matrices.
//
// The game logic is untouched: TMarDirector::direct() still runs two 60 Hz simulation
// ticks per call, once every two 59.94 Hz VI retraces, and each logic frame's own picture
// is presented at its retrace as before. In between, port_main.cpp's VIWaitForRetrace asks
// for extra display frames when they are due (setting [window] frame_rate: 60, 120, 144,
// the monitor's refresh rate, or unlimited; SMS_PORT_FPS overrides it for a run).
//
// How the stage is drawn: the last tick of direct() ends with the perform list unk34
// (CUE_CALC_VIEW | CUE_ENTRY: camera matrices into the graphics state, J3D viewCalc of
// every model, draw-buffer entries) for the new state S_E; the next direct() call starts by
// drawing those buffers (the "logic draw"), whose picture the VI shows from the next retrace
// P on. Each unk34 pass is snapshotted here (camera matrices, every J3D model's joint and
// envelope world matrices; port_interp_entry_*, J3DModel::viewCalc hook), three states
// deep. An extra display frame at time t, while the picture of state S_s is up (since P_s),
// shows lerp(S_s, S_s+1, (t - P_s) / tick) (port_mardirector_redraw,
// src/System/MarDirectorDirect.cpp): the view half of unk34 (CUE_CALC_VIEW |
// CUE_SET_PROJECTION; the draw buffers keep the logic frame's entries, whose J3D packets
// read their model's current draw matrices when drawn) with the interpolated camera and
// model matrices, the draw lists, then the view half once more with the live values (that
// leaves every model and object as the logic frame's unk34 left it, and each model's
// double-buffered draw matrices on the same buffer), then the screen fader
// (port_app_redraw, src/System/Application.cpp). The display keeps the original latency.
//
// What a display frame must not do, and how it is prevented:
//  - change game state: camera/model matrices are restored, the restoring view pass
//    recomputes everything unk34's view half derives from the state, the shadow requests the next logic
//    draw consumes are restored and sound effects gated (MarDirectorDirect.cpp), rand()
//    draws from a scratch copy (port_replay.cpp), draw-sync callbacks (Mario's occlusion
//    probe, the sun's lens-flare peeks, the pollution pixel counters) are not called
//    (port_aurora_sdk.cpp), EFB copies are not read back into game memory (port_efb.cpp);
//  - repeat once-per-logic-frame GPU work the game reads: the pollution layer and graffiti
//    lists are skipped, port_efb_end_frame() runs only for frames holding a logic draw.
// Aurora reads indexed matrices and vertex arrays from memory on its FIFO thread, so the
// FIFO is drained (GXDrawDone) before memory a queued draw still reads is rewritten.
// Also interpolated (W9-21..23): J3D joints in their parent's space (slerp; envelopes recomputed),
// instanced models per viewCalc occurrence, texture matrices baked in material display lists
// (camera/joint-dependent maps, SRT scrolls), shadow requests. Not interpolated (30 Hz steps,
// drawn at the newer state): particles, other non-J3D effects, 2D. Camera cuts and teleports
// snap. Hooks for other systems: BOOTLOG "Interpolation: ownership and hooks".
#ifdef SMS_PORT_AURORA

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <SDL3/SDL_video.h>

#include <aurora/aurora.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/types.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

#include "port_settings.h"

extern "C" volatile unsigned gPortFrameCount; // port_crash.cpp (frames as in SMS_PORT_SHOT)
extern "C" int port_app_redraw(void);       // src/System/Application.cpp (SMS_PORT)
extern "C" void port_rand_scratch(bool on); // port_replay.cpp
extern "C" void port_efb_clear_all(void);   // port_efb.cpp (w9 addition)
extern "C" void port_efb_set_presentation_only(int on); // port_efb.cpp (w8)
// libs/JSystem J3D (SMS_PORT hooks, w9)
extern "C" int port_j3d_joint_parents(void* model, s16* out, int max);
extern "C" void port_j3d_texmtx_recalc(void* texMtx, const f32* anm, const f32* view, int viewBranch,
                                       const void* srtA, const void* srtB, f32 t, f32* out);

namespace {

// SMS_PORT_INTERP_JOINTS=world: the old per-matrix world lerp (for comparison runs).
const int sJointMode = [] {
	const char* e = std::getenv("SMS_PORT_INTERP_JOINTS");
	return e != nullptr && _stricmp(e, "world") == 0 ? 0 : 1;
}();
// SMS_PORT_INTERP_TEXMTX=0 / SMS_PORT_INTERP_SHADOWS=0: leave those at the newer state (comparison).
const bool sTexMtxOn = [] {
	const char* e = std::getenv("SMS_PORT_INTERP_TEXMTX");
	return e == nullptr || *e != '0';
}();
const bool sShadowsOn = [] {
	const char* e = std::getenv("SMS_PORT_INTERP_SHADOWS");
	return e == nullptr || *e != '0';
}();

long long qpcFreq()
{
	static long long f = [] {
		LARGE_INTEGER v;
		QueryPerformanceFrequency(&v);
		return (long long)v.QuadPart;
	}();
	return f;
}

long long qpc()
{
	LARGE_INTEGER v;
	QueryPerformanceCounter(&v);
	return v.QuadPart;
}

double toMs(long long d) { return (double)d * 1000.0 / (double)qpcFreq(); }
long long fromMs(double ms) { return (long long)(ms * (double)qpcFreq() / 1000.0); }

const double kRetraceMs = 16.68335;
const int kGens         = 3; // states kept

// ---- states and segments ---------------------------------------------------------------
bool sEntry;          // inside a logic unk34 pass (snapshots are taken)
bool sRedraw;         // inside an extra display frame
bool sInterp;         // inside its interpolated unk34 pass / draw lists
u32 sEntryTick;       // unk34 passes so far = id of the newest state
u32 sDrawState;       // state the newest logic draw rendered
u32 sLogicDraws;      // logic draws so far (overlay)
long long sDrawQpc;   // time of the newest logic draw
bool sSegValid;
u32 sSegState;        // state whose picture is up (the display moves from it to the next)
long long sSegStart;  // its retrace
double sTickMs = 2 * kRetraceMs;

// the current display frame
float sAlpha;
bool sClearFirst; // the open Aurora frame already holds a logic draw: clear the EFB first
bool sCamCut;

// ---- camera ------------------------------------------------------------------------------
struct CamSnap {
	u32 tick = ~0u;
	float view[12];
	float proj[16];
};
CamSnap sCam[kGens];
float sLiveView[12], sLiveProj[16];
bool sCamApplied;

// ---- models ------------------------------------------------------------------------------
struct ModelSnap {
	u32 fastState = ~0u; // the state pair sSegState the flag below was worked out for
	bool fast     = false; // its joints turn too far between the two states to blend (see below)
	u32 tick[kGens];
	u32 nodes[kGens];
	u32 envs[kGens];
	std::vector<float> m[kGens];
	u32 seen = 0;
	std::vector<s16> parents; // parent joint of each joint (-1: root), W9-21
	u32 parentsNodes = ~0u;    // joint count the table was made for
	bool parentsOk   = false;  // every parent comes before its child
	ModelSnap()
	{
		for (int i = 0; i < kGens; i++)
			tick[i] = ~0u, nodes[i] = envs[i] = 0;
	}
};
// Keyed by model and occurrence: a J3DModel viewCalc'd several times in one pass (instanced:
// one model drawn at several places) has one snapshot per occurrence, matched by order in the
// display frame's view pass (and only used when the live matrices equal the newer snapshot).
struct ModelKey {
	void* model;
	u32 occ;
	bool operator==(const ModelKey& o) const { return model == o.model && occ == o.occ; }
};
struct ModelKeyHash {
	size_t operator()(const ModelKey& k) const { return std::hash<void*>()(k.model) ^ (size_t)k.occ * 0x9E3779B9u; }
};
std::unordered_map<ModelKey, ModelSnap, ModelKeyHash> sModels;
std::unordered_map<void*, u32> sOcc;          // viewCalc calls per model in the current pass
std::unordered_map<void*, u32> sInstancedTick; // model -> logic frame it was viewCalc'd more than once
std::vector<float> sLive; // the live matrices of the model being drawn, restored by _end
u32 sInterpModels;
u32 sOldSnaps;  // SMS_PORT_INTERP_LOG: joints the old per-matrix lerp would have snapped
float sMaxTurn; // and the largest joint turn between the two states (degrees)
long long sT[4]; // SMS_PORT_INTERP_LOG timing: drain 1 start/end, drain 2 start/end

// SMS_PORT_FPS=<rate>|display|unlimited overrides the setting for one run (tests).
int sEnvRate = -2;
int envRate()
{
	if (sEnvRate == -2) {
		const char* e = std::getenv("SMS_PORT_FPS");
		if (e == nullptr || *e == '\0')
			sEnvRate = -3;
		else if (_stricmp(e, "display") == 0)
			sEnvRate = PORT_FPS_DISPLAY;
		else if (_stricmp(e, "unlimited") == 0)
			sEnvRate = PORT_FPS_UNLIMITED;
		else {
			const int r = std::atoi(e);
			sEnvRate    = r < 30 ? 30 : r > 1000 ? 1000 : r;
		}
	}
	return sEnvRate;
}

// Refresh rate of the window's monitor (re-read every 2 s; 60 if unknown).
double displayHz()
{
	static double sHz;
	static ULONGLONG sAt;
	const ULONGLONG nowMs = GetTickCount64();
	if (sHz == 0.0 || nowMs - sAt > 2000) {
		sAt = nowMs;
		sHz = 60.0;
		if (SDL_Window* w = aurora_get_window())
			if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)))
				if (m->refresh_rate >= 30.0f)
					sHz = m->refresh_rate;
	}
	return sHz;
}

// Display-frame period in ms; 0 = unlimited.
double periodMs()
{
	const int r = port_interp_rate();
	double p;
	if (r == PORT_FPS_UNLIMITED)
		p = 0.0;
	else if (r == PORT_FPS_DISPLAY)
		p = 1000.0 / displayHz();
	else if (r == 60 || r == 120 || r == 240)
		p = kRetraceMs * 60.0 / r; // locked to the 59.94 Hz VI: frames land on retraces
	else
		p = 1000.0 / (double)r;
	// w6's VSync: a present waits for the monitor's refresh, so frames above the refresh
	// rate would only block; the rate is capped at the refresh rate instead (one setting).
	if (gPortSettings.vsync) {
		const double minP = 1000.0 / displayHz();
		if (p < minP)
			p = minP;
	}
	return p;
}

// ---- matrix helpers ------------------------------------------------------------------------
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// One 3x4 world matrix (rotation*scale | translation). Columns are lerped and rescaled to
// the lerped column length; large jumps (teleports, flips) snap to b.
void lerpMtx(const float* a, const float* b, float t, float* out)
{
	const float dx = b[3] - a[3], dy = b[7] - a[7], dz = b[11] - a[11];
	if (dx * dx + dy * dy + dz * dz > 500.0f * 500.0f) {
		std::memcpy(out, b, 12 * sizeof(float));
		return;
	}
	float la[3], lb[3];
	for (int c = 0; c < 3; c++) {
		const float ax = a[c], ay = a[4 + c], az = a[8 + c];
		const float bx = b[c], by = b[4 + c], bz = b[8 + c];
		la[c] = std::sqrt(ax * ax + ay * ay + az * az);
		lb[c] = std::sqrt(bx * bx + by * by + bz * bz);
		if (la[c] < 1e-6f || lb[c] < 1e-6f || (ax * bx + ay * by + az * bz) < 0.5f * la[c] * lb[c]) {
			std::memcpy(out, b, 12 * sizeof(float)); // > 60 degrees in one tick, or degenerate
			return;
		}
	}
	for (int c = 0; c < 3; c++) {
		const float x = lerpf(a[c], b[c], t), y = lerpf(a[4 + c], b[4 + c], t), z = lerpf(a[8 + c], b[8 + c], t);
		const float l = std::sqrt(x * x + y * y + z * z);
		const float s = l > 1e-6f ? lerpf(la[c], lb[c], t) / l : 1.0f;
		out[c] = x * s, out[4 + c] = y * s, out[8 + c] = z * s;
	}
	out[3]  = lerpf(a[3], b[3], t);
	out[7]  = lerpf(a[7], b[7], t);
	out[11] = lerpf(a[11], b[11], t);
}

// ---- joint interpolation in the parent's space (W9-21) --------------------------------------
// A joint's world matrix W = P * L (P: its parent's world matrix). The local L of both states
// is split into a rotation R and the rest U (L3x3 = R * U, QR decomposition: U holds scale and
// shear), R is slerped (quaternions, shortest way), U and the translation lerped, and the
// world matrix rebuilt from the parent's interpolated one. Both endpoints are reproduced
// (R * U = L), limbs keep their length whatever the poses, and fast turns (spin jump, flips)
// follow the arc instead of snapping.
struct Quat {
	float w, x, y, z;
};

// 3x4 row-major [M | t] inverse; false if singular (a joint scaled to 0, e.g. hidden parts).
bool invAffine(const float* m, float* o)
{
	const float a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9], i = m[10];
	const float A = e * i - f * h, B = -(d * i - f * g), C = d * h - e * g;
	const float det = a * A + b * B + c * C;
	if (std::fabs(det) < 1e-12f)
		return false;
	const float k = 1.0f / det;
	o[0] = A * k, o[1] = -(b * i - c * h) * k, o[2] = (b * f - c * e) * k;
	o[4] = B * k, o[5] = (a * i - c * g) * k, o[6] = -(a * f - c * d) * k;
	o[8] = C * k, o[9] = -(a * h - b * g) * k, o[10] = (a * e - b * d) * k;
	for (int r = 0; r < 3; r++)
		o[r * 4 + 3] = -(o[r * 4 + 0] * m[3] + o[r * 4 + 1] * m[7] + o[r * 4 + 2] * m[11]);
	return true;
}

void mulAffine(const float* p, const float* l, float* o) // o = p * l (o may not alias)
{
	for (int r = 0; r < 3; r++) {
		for (int c = 0; c < 4; c++)
			o[r * 4 + c] = p[r * 4 + 0] * l[0 * 4 + c] + p[r * 4 + 1] * l[1 * 4 + c] + p[r * 4 + 2] * l[2 * 4 + c];
		o[r * 4 + 3] += p[r * 4 + 3];
	}
}

// M (3x3 of a 3x4) = Q * U, Q a proper rotation (columns q0..q2), U upper triangular.
bool qrSplit(const float* m, float q[3][3], float u[3][3])
{
	float col[3][3];
	for (int c = 0; c < 3; c++)
		for (int r = 0; r < 3; r++)
			col[c][r] = m[r * 4 + c];
	std::memset(u, 0, 9 * sizeof(float));
	for (int c = 0; c < 3; c++) {
		float v[3] = { col[c][0], col[c][1], col[c][2] };
		for (int k = 0; k < c; k++) {
			const float d = q[k][0] * col[c][0] + q[k][1] * col[c][1] + q[k][2] * col[c][2];
			u[k][c]       = d;
			v[0] -= d * q[k][0], v[1] -= d * q[k][1], v[2] -= d * q[k][2];
		}
		const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
		if (l < 1e-7f)
			return false;
		u[c][c] = l;
		q[c][0] = v[0] / l, q[c][1] = v[1] / l, q[c][2] = v[2] / l;
	}
	const float det = q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) - q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0])
	                + q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
	if (det < 0.0f) { // mirrored: the sign goes into U
		q[2][0] = -q[2][0], q[2][1] = -q[2][1], q[2][2] = -q[2][2];
		u[2][2] = -u[2][2];
	}
	return true;
}

Quat quatFromCols(const float q[3][3]) // rotation matrix with columns q[c]
{
	// R[r][c] = q[c][r]
	const float m00 = q[0][0], m01 = q[1][0], m02 = q[2][0];
	const float m10 = q[0][1], m11 = q[1][1], m12 = q[2][1];
	const float m20 = q[0][2], m21 = q[1][2], m22 = q[2][2];
	Quat o;
	const float tr = m00 + m11 + m22;
	if (tr > 0.0f) {
		const float s = std::sqrt(tr + 1.0f) * 2.0f;
		o.w = 0.25f * s, o.x = (m21 - m12) / s, o.y = (m02 - m20) / s, o.z = (m10 - m01) / s;
	} else if (m00 > m11 && m00 > m22) {
		const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
		o.w = (m21 - m12) / s, o.x = 0.25f * s, o.y = (m01 + m10) / s, o.z = (m02 + m20) / s;
	} else if (m11 > m22) {
		const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
		o.w = (m02 - m20) / s, o.x = (m01 + m10) / s, o.y = 0.25f * s, o.z = (m12 + m21) / s;
	} else {
		const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
		o.w = (m10 - m01) / s, o.x = (m02 + m20) / s, o.y = (m12 + m21) / s, o.z = 0.25f * s;
	}
	return o;
}

Quat slerp(Quat a, Quat b, float t)
{
	float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
	if (d < 0.0f)
		d = -d, b.w = -b.w, b.x = -b.x, b.y = -b.y, b.z = -b.z;
	float ka = 1.0f - t, kb = t;
	if (d < 0.9995f) {
		const float th = std::acos(d), s = std::sin(th);
		ka = std::sin((1.0f - t) * th) / s;
		kb = std::sin(t * th) / s;
	}
	Quat o = { a.w * ka + b.w * kb, a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb };
	const float l = std::sqrt(o.w * o.w + o.x * o.x + o.y * o.y + o.z * o.z);
	o.w /= l, o.x /= l, o.y /= l, o.z /= l;
	return o;
}

float turnDeg(const float* a, const float* b);

// The largest joint turn between two states of a model, in each joint's parent space.
float maxLocalTurn(void* model, const float* a, const float* b, u32 nodeNum)
{
	static std::vector<s16> parents;
	parents.assign(nodeNum, -1);
	const bool ok = port_j3d_joint_parents(model, parents.data(), (int)nodeNum) == (int)nodeNum;
	float worst = 0.0f, ia[12], ib[12], la[12], lb[12];
	for (u32 i = 0; i < nodeNum; i++) {
		const int p = ok ? parents[i] : -1;
		if (p < 0 || p >= (int)i || !invAffine(a + p * 12, ia) || !invAffine(b + p * 12, ib))
			continue;
		mulAffine(ia, a + i * 12, la);
		mulAffine(ib, b + i * 12, lb);
		const float t = turnDeg(la, lb);
		worst         = t > worst ? t : worst;
	}
	return worst;
}

// The rotation between two affine 3x4 transforms, in degrees (0 if one is degenerate).
float turnDeg(const float* a, const float* b)
{
	float qa[3][3], ua[3][3], qb[3][3], ub[3][3];
	if (!qrSplit(a, qa, ua) || !qrSplit(b, qb, ub))
		return 0.0f;
	const Quat x = quatFromCols(qa), y = quatFromCols(qb);
	float d = std::fabs(x.w * y.w + x.x * y.x + x.y * y.y + x.z * y.z);
	if (d > 1.0f)
		d = 1.0f;
	return 2.0f * std::acos(d) * 57.29578f;
}

// Interpolated affine transform between a and b (both 3x4).
void interpAffine(const float* a, const float* b, float t, float* out)
{
	float qa[3][3], ua[3][3], qb[3][3], ub[3][3];
	if (!qrSplit(a, qa, ua) || !qrSplit(b, qb, ub)) { // degenerate (scaled to 0): plain lerp
		for (int k = 0; k < 12; k++)
			out[k] = lerpf(a[k], b[k], t);
		return;
	}
	const Quat q = slerp(quatFromCols(qa), quatFromCols(qb), t);
	const float w = q.w, x = q.x, y = q.y, z = q.z;
	const float r[3][3] = { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
		                    { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
		                    { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
	float u[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			u[i][j] = lerpf(ua[i][j], ub[i][j], t);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			out[i * 4 + j] = r[i][0] * u[0][j] + r[i][1] * u[1][j] + r[i][2] * u[2][j];
	out[3]  = lerpf(a[3], b[3], t);
	out[7]  = lerpf(a[7], b[7], t);
	out[11] = lerpf(a[11], b[11], t);
}

void camPos(const float* v, float* p) // world position of a view matrix [R | t]: -R^T t
{
	for (int i = 0; i < 3; i++)
		p[i] = -(v[i] * v[3] + v[4 + i] * v[7] + v[8 + i] * v[11]);
}

void normalize3(float* v)
{
	const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 1e-9f)
		v[0] /= l, v[1] /= l, v[2] /= l;
}

// View: rotation rows lerped and re-orthonormalised, camera position lerped; projection
// lerped. False for a cut (the camera jumped or changed lens): the caller uses b.
bool lerpCamera(const CamSnap& a, const CamSnap& b, float t, float* view, float* proj)
{
	float pa[3], pb[3];
	camPos(a.view, pa);
	camPos(b.view, pb);
	const float d2 = (pb[0] - pa[0]) * (pb[0] - pa[0]) + (pb[1] - pa[1]) * (pb[1] - pa[1])
	               + (pb[2] - pa[2]) * (pb[2] - pa[2]);
	const float fwd  = a.view[8] * b.view[8] + a.view[9] * b.view[9] + a.view[10] * b.view[10];
	const bool persp = a.proj[15] == 0.0f && b.proj[15] == 0.0f;
	const bool fov   = persp && std::fabs(a.proj[0] - b.proj[0]) > 0.3f * std::fabs(b.proj[0]);
	if (d2 > 1000.0f * 1000.0f || fwd < 0.7f || a.proj[15] != b.proj[15] || fov)
		return false;
	float r[3][3];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r[i][j] = lerpf(a.view[i * 4 + j], b.view[i * 4 + j], t);
	normalize3(r[2]); // z (backwards)
	const float d = r[0][0] * r[2][0] + r[0][1] * r[2][1] + r[0][2] * r[2][2];
	for (int j = 0; j < 3; j++)
		r[0][j] -= d * r[2][j];
	normalize3(r[0]);                                // x (right)
	r[1][0] = r[2][1] * r[0][2] - r[2][2] * r[0][1]; // y = z cross x
	r[1][1] = r[2][2] * r[0][0] - r[2][0] * r[0][2];
	r[1][2] = r[2][0] * r[0][1] - r[2][1] * r[0][0];
	float p[3];
	for (int i = 0; i < 3; i++)
		p[i] = lerpf(pa[i], pb[i], t);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++)
			view[i * 4 + j] = r[i][j];
		view[i * 4 + 3] = -(r[i][0] * p[0] + r[i][1] * p[1] + r[i][2] * p[2]);
	}
	for (int i = 0; i < 16; i++)
		proj[i] = lerpf(a.proj[i], b.proj[i], t);
	return true;
}

// ---- pacing state ---------------------------------------------------------------------------
long long sNextDue;   // next display frame (QPC)
double sCostMs  = 4.0; // display frame cost (render + present), running average
double sCostMax = 8.0; // recent maximum (decays), for the "finished before the retrace" test

// SMS_PORT_INTERP_LOG=1: a line per presented frame (frame-time logs).
const bool sLog = std::getenv("SMS_PORT_INTERP_LOG") != nullptr;

} // namespace

namespace {
int lagNow(); // W12 (defined with the point sets)
}
extern "C" {

int port_interp_rate(void)
{
	const int e = envRate();
	return e != -3 ? e : gPortSettings.frameRate;
}

bool port_interp_override(void) { return envRate() != -3; }

double port_interp_display_hz(void) { return displayHz(); }

// Display frames per second actually aimed for (0 = unlimited), after the VSync cap.
double port_interp_effective_fps(void)
{
	const double p = periodMs();
	return p > 0.0 ? 1000.0 / p : 0.0;
}

unsigned port_interp_logic_frames(void) { return sLogicDraws; }

bool port_interp_drawing(void) { return sRedraw; }

// For systems plugging into display frames (BOOTLOG "Interpolation: ownership and hooks"):
// true while a display frame's interpolated view pass and draw lists run, and the position
// between the two logic states shown (0 = the older, 1 = the newer).
bool port_interp_interpolating(void) { return sInterp; }
float port_interp_alpha(void) { return sAlpha; }
// W12: 0 normally; 1 while the display frame lies between the states sEntryTick - 2 and sEntryTick - 1
// (the retrace that shows the newest logic draw has not come yet): systems that keep their own
// last-two-states copies (TModelWaterManager) then blend their two older copies.
int port_interp_lag(void) { return lagNow(); }

// ---- game hooks -------------------------------------------------------------------------------
// MarDirectorDirect.cpp, around the logic unk34 pass, with the camera's matrices.
void port_interp_entry_begin(const f32* view, const f32* proj)
{
	++sEntryTick;
	sEntry     = port_interp_rate() != 30;
	CamSnap& c = sCam[sEntryTick % kGens];
	if (view != nullptr && proj != nullptr) {
		std::memcpy(c.view, view, sizeof(c.view));
		std::memcpy(c.proj, proj, sizeof(c.proj));
		c.tick = sEntryTick;
	} else {
		c.tick = ~0u;
	}
	sOcc.clear();
	if ((sEntryTick & 63) == 0) { // forget models not drawn lately (they may be freed)
		for (auto it = sModels.begin(); it != sModels.end();)
			it = sEntryTick - it->second.seen > 6 ? sModels.erase(it) : std::next(it);
		for (auto it = sInstancedTick.begin(); it != sInstancedTick.end();)
			it = sEntryTick - it->second > 6 ? sInstancedTick.erase(it) : std::next(it);
	}
}

void port_interp_entry_end(void) { sEntry = false; }

// The draw half of direct() starts: it renders the newest state.
void port_interp_logic_draw_begin(void)
{
	sDrawState = sEntryTick;
	sDrawQpc   = qpc();
	++sLogicDraws;
}

void port_interp_logic_draw_end(void) {}


// J3DModel::viewCalc.
int port_interp_model_begin(void* model, Mtx* node, u32 nodeNum, Mtx* env, u32 envNum)
{
	if (sEntry) {
		const u32 occ = sOcc[model]++;
		if (occ != 0)
			sInstancedTick[model] = sEntryTick;
		ModelSnap& e   = sModels[ModelKey { model, occ }];
		const int slot = sEntryTick % kGens;
		e.m[slot].resize((size_t)(nodeNum + envNum) * 12);
		if (nodeNum != 0)
			std::memcpy(e.m[slot].data(), node, nodeNum * sizeof(Mtx));
		if (envNum != 0)
			std::memcpy(e.m[slot].data() + nodeNum * 12, env, envNum * sizeof(Mtx));
		e.nodes[slot] = nodeNum;
		e.envs[slot]  = envNum;
		e.tick[slot]  = sEntryTick;
		e.seen        = sEntryTick;
		// SMS_PORT_INTERP_LOG: how far this model's joints turn between two logic frames (in their
		// parents' space), whether or not display frames are drawn in between
		const int prev = (sEntryTick + kGens - 1) % kGens;
		if (sLog && nodeNum > 8 && e.tick[prev] == sEntryTick - 1 && e.nodes[prev] == nodeNum) {
			const float t = maxLocalTurn(model, e.m[prev].data(), e.m[slot].data(), nodeNum);
			if (t > 45.0f)
				std::fprintf(stderr, "[interp] state %u: model %p (%u joints) turns %.0f deg\n", sEntryTick, model, nodeNum, t);
		}
		return 0;
	}
	if (!sInterp)
		return 0;
	static const bool sModelsOff = [] { // SMS_PORT_INTERP_MODELS=0: models step at 30 (comparison)
		const char* e = std::getenv("SMS_PORT_INTERP_MODELS");
		return e != nullptr && *e == '0';
	}();
	if (sModelsOff)
		return 0;
	const u32 occ = sOcc[model]++;
	auto it       = sModels.find(ModelKey { model, occ });
	if (it == sModels.end())
		return 0;
	ModelSnap& e = it->second;
	const int sa = sSegState % kGens, sb = (sSegState + 1) % kGens;
	if (e.tick[sa] != sSegState || e.nodes[sa] != nodeNum || e.envs[sa] != envNum)
		return 0;
	if (e.tick[sb] != sSegState + 1 || e.nodes[sb] != nodeNum || e.envs[sb] != envNum)
		return 0;
	const u32 count = nodeNum + envNum;
	if (count == 0)
		return 0;
	// An instanced model (several viewCalcs per pass) is matched by order: the newer state must
	// be what this viewCalc has now, or the matrices are left alone. (Single models may have
	// their matrices recomputed in the view pass, e.g. the sky following the camera.)
	const auto inst = sInstancedTick.find(model);
	if (inst != sInstancedTick.end() && inst->second == sSegState + 1 && nodeNum != 0
	    && std::memcmp(e.m[sb].data(), node, nodeNum * sizeof(Mtx)) != 0)
		return 0;
	sLive.resize((size_t)count * 12);
	if (nodeNum != 0)
		std::memcpy(sLive.data(), node, nodeNum * sizeof(Mtx));
	if (envNum != 0)
		std::memcpy(sLive.data() + nodeNum * 12, env, envNum * sizeof(Mtx));
	const float* a = e.m[sa].data();
	const float* b = e.m[sb].data();
	if (sJointMode == 0) { // SMS_PORT_INTERP_JOINTS=world: the old per-matrix lerp (comparison)
		for (u32 i = 0; i < count; i++) {
			float* dst = i < nodeNum ? &node[i][0][0] : &env[i - nodeNum][0][0];
			lerpMtx(a + i * 12, b + i * 12, sAlpha, dst);
		}
		++sInterpModels;
		return 1;
	}
	if (nodeNum == 0)
		return 0;
	if (e.parentsNodes != nodeNum) {
		e.parents.assign(nodeNum, -1);
		e.parentsOk    = port_j3d_joint_parents(model, e.parents.data(), (int)nodeNum) == (int)nodeNum;
		e.parentsNodes = nodeNum;
		for (u32 i = 0; i < nodeNum && e.parentsOk; i++) {
			if (e.parents[i] == -2) // not in the tree: interpolated on its own
				e.parents[i] = -1;
			if (e.parents[i] >= (s16)i)
				e.parentsOk = false;
		}
	}
	// A teleport (the model's root moved > 500 units in one frame) snaps.
	const float dx = b[3] - a[3], dy = b[7] - a[7], dz = b[11] - a[11];
	if (dx * dx + dy * dy + dz * dz > 500.0f * 500.0f) {
		std::memcpy(node, b, nodeNum * sizeof(Mtx));
		++sInterpModels;
		return 2;
	}
	// Animations played very fast (the turbo nozzle's dash) move a limb a third of its cycle or more
	// between two logic frames; blending the two poses then shows poses the animation never has
	// (legs swinging backwards, flailing). Such a model's joints show the nearer state (the game's
	// own 30 Hz steps, as on the GameCube) while its root still moves smoothly. Normal animations
	// stay well under the limit (SMS_PORT_INTERP_FAST_DEG, default 45 degrees per logic frame).
	static const float sFastDeg = [] {
		const char* v = std::getenv("SMS_PORT_INTERP_FAST_DEG");
		return v != nullptr ? (float)std::atof(v) : 45.0f;
	}();
	if (e.fastState != sSegState) {
		e.fastState = sSegState;
		e.fast      = sFastDeg > 0.0f && e.parentsOk && maxLocalTurn(model, a, b, nodeNum) > sFastDeg;
		if (sLog && e.fast)
			std::fprintf(stderr, "[interp] states %u-%u: model %p (%u joints) too fast to blend: stepped\n", sSegState,
			             sSegState + 1, model, nodeNum);
	}
	float ia[12], ib[12], la[12], lb[12], l[12];
	float modelTurn = 0.0f; // SMS_PORT_INTERP_LOG: the largest joint turn in its parent's space
	for (u32 i = 0; i < nodeNum; i++) {
		const float* ai = a + i * 12;
		const float* bi = b + i * 12;
		float* dst      = &node[i][0][0];
		const int p     = e.parentsOk ? e.parents[i] : -1;
		if (sLog) { // the old per-matrix lerp snapped a joint turning > 60 degrees in one frame
			float worst = 1.0f;
			for (int c = 0; c < 3; c++) {
				const float ax = ai[c], ay = ai[4 + c], az = ai[8 + c], bx = bi[c], by = bi[4 + c], bz = bi[8 + c];
				const float la = std::sqrt(ax * ax + ay * ay + az * az), lb = std::sqrt(bx * bx + by * by + bz * bz);
				if (la > 1e-6f && lb > 1e-6f) {
					const float cs = (ax * bx + ay * by + az * bz) / (la * lb);
					worst          = cs < worst ? cs : worst;
				}
			}
			if (worst < 0.5f)
				++sOldSnaps;
			const float deg = std::acos(worst < -1.0f ? -1.0f : worst) * 57.29578f;
			if (deg > sMaxTurn)
				sMaxTurn = deg;
		}
		if (p >= 0 && invAffine(a + p * 12, ia) && invAffine(b + p * 12, ib)) {
			mulAffine(ia, ai, la);
			mulAffine(ib, bi, lb);
			if (sLog) {
				const float t = turnDeg(la, lb);
				modelTurn     = t > modelTurn ? t : modelTurn;
			}
			if (e.fast)
				std::memcpy(l, sAlpha < 0.5f ? la : lb, sizeof(l)); // the nearer state's pose
			else
				interpAffine(la, lb, sAlpha, l);
			mulAffine(&node[p][0][0], l, dst); // the parent is already interpolated
		} else {
			interpAffine(ai, bi, sAlpha, dst);
		}
	}
	if (sLog && modelTurn > 45.0f)
		std::fprintf(stderr, "[interp] model %p (%u joints): a joint turns %.0f deg in its parent's space\n", model,
		             nodeNum, modelTurn);
	++sInterpModels;
	return 2; // J3DModel::viewCalc recomputes the envelope matrices from these
}

void port_interp_model_end(void* model, Mtx* node, u32 nodeNum, Mtx* env, u32 envNum)
{
	(void)model;
	if (sLive.size() < (size_t)(nodeNum + envNum) * 12)
		return;
	if (nodeNum != 0)
		std::memcpy(node, sLive.data(), nodeNum * sizeof(Mtx));
	if (envNum != 0)
		std::memcpy(env, sLive.data() + nodeNum * 12, envNum * sizeof(Mtx));
}

} // extern "C"

namespace {

// ---- texture matrices in material display lists (W9-22) -------------------------------------
// J3DJoint::entryIn computes each material's texture matrices (camera-dependent ones from the
// logic frame's camera and joint, scrolling ones from the SRT animation) and bakes them into the
// model's packet display list. Display frames reuse those lists, so environment/projection maps
// kept the newer state's camera while the geometry moved with the interpolated one: the water's
// shimmer and reflections jumped every logic frame. Each texture matrix written by
// J3DMaterial::makeDisplayList during the logic unk34 is recorded (where its 8/12 words are);
// a display frame recomputes it with the interpolated camera, joint and SRT
// (port_j3d_texmtx_recalc) and writes it into the list; the original words are put back
// before the next logic draw (port_interp_redraw_draw_end).
struct TexRec {
	void* model;
	void* texMtx;
	u8* dl;
	u8 len;
	u8 viewBranch;
	const float* anmPtr; // the matrix passed to J3DMaterial::calc (a joint's: &mNodeMatrices[j])
	float anm[12];       // its value then
	u8 srt[20];
};
struct SrtHist {
	u8 cur[20], prev[20], prev2[20]; // W12: three states (a display frame can lag the newest by one)
	u32 curTick = ~0u, prevTick = ~0u, prev2Tick = ~0u;
};
struct PtrPairHash {
	size_t operator()(const std::pair<void*, void*>& p) const
	{
		return std::hash<void*>()(p.first) * 31u ^ std::hash<void*>()(p.second);
	}
};
std::vector<TexRec> sTexRecs;
u32 sTexTick = ~0u;
std::unordered_map<std::pair<void*, void*>, SrtHist, PtrPairHash> sSrt;
std::unordered_map<void*, std::pair<u32, u32>> sTexByModel; // model -> [first, end) in sTexRecs
u32 sTexByModelTick = ~0u;
const float* sNoteCalcMtx; // the joint matrix of the last J3DMaterial::calc
bool sInMakeDL;
struct TexUndo {
	u8* dl;
	u8 len;
	u8 bytes[48];
};
std::vector<TexUndo> sTexUndo;
u32 sTexPatched, sTexRejected;

void putBE(u8* p, float f)
{
	u32 u;
	std::memcpy(&u, &f, 4);
	p[0] = (u8)(u >> 24), p[1] = (u8)(u >> 16), p[2] = (u8)(u >> 8), p[3] = (u8)u;
}

void texUndoAll()
{
	for (auto it = sTexUndo.rbegin(); it != sTexUndo.rend(); ++it)
		std::memcpy(it->dl, it->bytes, it->len * 4u);
	sTexUndo.clear();
}

// ---- shadow requests (W9-23) -----------------------------------------------------------------
// TMBindShadowManager builds the shadows from requests (position, radii) that the objects
// make during the logic ticks and unk34. A display frame drew them where the newer state has
// them while the models were interpolated, so Mario's shadow ran ahead of him and snapped back
// every logic frame. The requests calcVtx saw on each logic frame are kept; in a display frame
// each request is moved between its match in the previous logic frame (same index, or nearby
// with the same kind) and its current place.
std::vector<u8> sShadow[2];
u32 sShadowTick[2] = { ~0u, ~0u };
int sShadowStride;

// W12: how many logic states the newest one is ahead of the state after the one on screen:
// 0 = the display frame lies between the states sSegState and sEntryTick (the live data is
// its newer end); 1 = the retrace that puts sSegState + 1 up has not come yet (the game thread
// has already run the next logic frame while it waits for it; at 144 fps 38 % of the display
// frames), so the newer end must come from the copies kept at the logic draws, not from the live data.
int lagNow()
{
	static const bool sLagOn = [] { // SMS_PORT_INTERP_LAG=0: the old behaviour (comparison runs)
		const char* e = std::getenv("SMS_PORT_INTERP_LAG");
		return e == nullptr || *e != '0';
	}();
	const int lag = (int)(sEntryTick - sSegState) - 1;
	return lag == 1 && !sLagOn ? 99 : lag;
}

} // namespace

extern "C" {

// J3DMaterial::calc: the joint matrix the material's texture matrices are computed from.
void port_interp_note_texmtx_calc(const f32* anmMtx)
{
	if (sEntry)
		sNoteCalcMtx = anmMtx;
}

void port_interp_note_makedl(int on) { sInMakeDL = on != 0 && sEntry && sTexMtxOn; }

// J3DTexMtx::load inside J3DMaterial::makeDisplayList of a logic unk34.
void port_interp_note_texmtx_load(void* model, void* texMtx, u8* dl, int len, int viewBranch, const void* srt,
                                  const f32* total, const f32* view)
{
	if (!sInMakeDL || dl == nullptr || model == nullptr || sNoteCalcMtx == nullptr || view == nullptr)
		return;
	if (sTexTick != sEntryTick) {
		sTexRecs.clear();
		sTexTick = sEntryTick;
		if (sSrt.size() > 20000)
			sSrt.clear();
	}
	TexRec r;
	r.model      = model;
	r.texMtx     = texMtx;
	r.dl         = dl;
	r.len        = (u8)len;
	r.viewBranch = (u8)(viewBranch != 0);
	r.anmPtr     = sNoteCalcMtx;
	std::memcpy(r.anm, sNoteCalcMtx, sizeof(r.anm));
	std::memcpy(r.srt, srt, sizeof(r.srt));
	// Exactness check: recomputing with the live inputs must give exactly what the game
	// bakes, or this matrix is never touched (other texgen paths, effect matrices, ...).
	float chk[12];
	port_j3d_texmtx_recalc(texMtx, r.anm, view, r.viewBranch, nullptr, r.srt, 1.0f, chk);
	if (std::memcmp(chk, total, r.len * sizeof(float)) != 0) {
		++sTexRejected;
		return;
	}
	sTexRecs.push_back(r);
	SrtHist& h = sSrt[std::make_pair(model, texMtx)];
	if (h.curTick != sEntryTick) {
		std::memcpy(h.prev2, h.prev, sizeof(h.prev));
		h.prev2Tick = h.prevTick;
		std::memcpy(h.prev, h.cur, sizeof(h.cur));
		h.prevTick = h.curTick;
		std::memcpy(h.cur, srt, sizeof(h.cur));
		h.curTick = sEntryTick;
	}
}

// J3DModel::viewCalc of an interpolated display frame (node: the model's joint matrices as
// this viewCalc uses them, interpolated or live; view: the interpolated camera).
void port_interp_model_texmtx(void* model, Mtx* node, u32 nodeNum, const f32* view)
{
	const int lag = lagNow();
	if (!sInterp || !sTexMtxOn || (lag != 0 && lag != 1) || sTexTick != sEntryTick)
		return;
	auto mi = sInstancedTick.find(model);
	if (mi != sInstancedTick.end() && mi->second == sTexTick)
		return; // instanced: its packet lists belong to its last entry only
	if (sTexByModelTick != sTexTick) {
		sTexByModel.clear();
		for (u32 i = 0; i < sTexRecs.size();) {
			u32 j = i;
			while (j < sTexRecs.size() && sTexRecs[j].model == sTexRecs[i].model)
				++j;
			if (sTexByModel.find(sTexRecs[i].model) == sTexByModel.end())
				sTexByModel[sTexRecs[i].model] = std::make_pair(i, j);
			i = j;
		}
		sTexByModelTick = sTexTick;
	}
	auto range = sTexByModel.find(model);
	if (range == sTexByModel.end())
		return;
	const float* nodeBase = nodeNum != 0 ? &node[0][0][0] : nullptr;
	for (u32 i = range->second.first; i < range->second.second; i++) {
		const TexRec& r = sTexRecs[i];
		// the joint matrix: this model's (interpolated) one if it was a joint, else as recorded
		const float* anm = r.anm;
		if (nodeBase != nullptr && r.anmPtr >= nodeBase && r.anmPtr < nodeBase + nodeNum * 12
		    && (r.anmPtr - nodeBase) % 12 == 0)
			anm = nodeBase + (r.anmPtr - nodeBase);
		const auto h      = sSrt.find(std::make_pair(r.model, r.texMtx));
		const void* srtA = nullptr;
		const void* srtB = r.srt;
		if (lag == 0) {
			srtA = h != sSrt.end() && h->second.prevTick == sTexTick - 1 ? h->second.prev : nullptr;
		} else { // between the states sTexTick - 2 and sTexTick - 1
			if (h == sSrt.end() || h->second.prevTick != sTexTick - 1 || h->second.prev2Tick != sTexTick - 2)
				continue;
			srtA = h->second.prev2;
			srtB = h->second.prev;
		}
		float out[12];
		port_j3d_texmtx_recalc(r.texMtx, anm, view, r.viewBranch, srtA, srtB, sAlpha, out);
		TexUndo u;
		u.dl  = r.dl;
		u.len = r.len;
		std::memcpy(u.bytes, r.dl, r.len * 4u);
		sTexUndo.push_back(u);
		for (int k = 0; k < r.len; k++)
			putBE(r.dl + k * 4, out[k]);
		++sTexPatched;
	}
}

// ---- point sets (W9-25): particles and other position arrays ---------------------------------
// port_interp_points_begin(key, pos, n, vel, ticks): call before drawing or computing vertices
// from `n` xyz positions (`pos`, 3 floats each). On a logic frame it keeps a copy (state
// sEntryTick); in an interpolated display frame it writes lerp(previous copy, live, alpha) into
// `pos` and returns 1, and port_interp_points_end(key, pos) must put the live values back after
// the draw/vertex work. A slot is only interpolated when it moved as its velocity says (`vel`
// per tick, `ticks` ticks per logic frame; vel may be null: then a plain distance bound), so
// reused or compacted slots are left at their live place.
struct PointSet {
	std::vector<float> gen[2], live; // compact xyz copies of the last two logic states / live
	std::vector<u8> valid[2];        // per slot: still the same object (port_interp_points_move/invalidate)
	u32 tick[2] = { ~0u, ~0u };
	int cur     = 0;
	int stride  = 3; // floats between two positions in the game's array
};
std::unordered_map<const void*, PointSet> sPointSets;

static void gatherXyz(std::vector<float>& out, const float* pos, int n, int stride)
{
	out.resize((size_t)n * 3);
	for (int i = 0; i < n; i++)
		std::memcpy(&out[(size_t)i * 3], pos + (size_t)i * stride, 3 * sizeof(float));
}

u32 sPointsLerped, sPointsSeen; // SMS_PORT_INTERP_LOG

// As port_interp_points_begin, for positions `stride` floats apart (a field of a struct array).
int port_interp_points_begin_s(const void* key, float* pos, int n, int stride, const float* vel, float ticks)
{
	static const bool sOn = [] { // SMS_PORT_INTERP_POINTS=0: comparison runs
		const char* e = std::getenv("SMS_PORT_INTERP_POINTS");
		return e == nullptr || *e != '0';
	}();
	if (!sOn || pos == nullptr || n <= 0 || stride < 3 || port_interp_rate() == 30)
		return 0;
	PointSet& ps = sPointSets[key];
	ps.stride    = stride;
	if (!sRedraw) { // logic frame (unk34 or logic draw): the positions of state sEntryTick
		if (ps.tick[ps.cur] != sEntryTick)
			ps.cur ^= 1;
		gatherXyz(ps.gen[ps.cur], pos, n, stride);
		ps.valid[ps.cur].assign((size_t)n, 1);
		ps.tick[ps.cur] = sEntryTick;
		return 0;
	}
	const int lag = lagNow();
	if (!sInterp || (lag != 0 && lag != 1))
		return 0;
	const int g = ps.tick[0] == sSegState ? 0 : ps.tick[1] == sSegState ? 1 : -1;
	if (g < 0)
		return 0;
	// lag 1 (W12): the newer end is the copy of state sSegState + 1, not the live data
	const int g2 = lag == 0 ? -1 : ps.tick[0] == sSegState + 1 ? 0 : ps.tick[1] == sSegState + 1 ? 1 : -2;
	if (g2 == -2)
		return 0;
	const std::vector<float>& prev = ps.gen[g];
	gatherXyz(ps.live, pos, n, stride);
	const int pn  = (int)(prev.size() / 3);
	const int nn  = g2 >= 0 ? (int)(ps.gen[g2].size() / 3) : n;
	const int m01 = n < pn ? n : pn;
	const int m   = m01 < nn ? m01 : nn;
	for (int i = 0; i < m; i++) {
		if (!ps.valid[g][i] || (g2 >= 0 && !ps.valid[g2][i])) {
			if (g2 >= 0 && ps.valid[g2][i]) // new in the newer state: at its place there
				std::memcpy(pos + (size_t)i * stride, &ps.gen[g2][(size_t)i * 3], 3 * sizeof(float));
			continue;
		}
		const float* a = &prev[(size_t)i * 3];
		float* b       = pos + (size_t)i * stride;
		if (g2 >= 0)
			std::memcpy(b, &ps.gen[g2][(size_t)i * 3], 3 * sizeof(float));
		const float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
		bool same;
		if (vel != nullptr) {
			// moved no further than its speed allows (drag, gravity and sliding change the
			// velocity within a frame) and not against it
			const float* v  = vel + (size_t)i * 3;
			const float vl  = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) * ticks;
			const float dl  = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
			const float dot = d[0] * v[0] + d[1] * v[1] + d[2] * v[2];
			same = dl <= 2.5f * vl + 20.0f && (dot >= 0.0f || dl < 20.0f);
		} else {
			same = d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 300.0f * 300.0f;
		}
		if (same) {
			for (int k = 0; k < 3; k++)
				b[k] = lerpf(a[k], b[k], sAlpha);
			++sPointsLerped;
		}
	}
	sPointsSeen += (u32)n;
	return 1;
}

int port_interp_points_begin(const void* key, float* pos, int n, const float* vel, float ticks)
{
	return port_interp_points_begin_s(key, pos, n, 3, vel, ticks);
}

// ---- scalar sets (W9-27): animation phases, scroll offsets, amplitudes -----------------------
// port_interp_values(key, v, n, periods): as the point sets, for `n` floats; periods[i] > 0: the
// value wraps at that period (a phase or a scroll offset), interpolated the short way.
struct ValueSet {
	std::vector<float> gen[2], live;
	u32 tick[2] = { ~0u, ~0u };
	int cur     = 0;
};
std::unordered_map<const void*, ValueSet> sValueSets;

int port_interp_values(const void* key, float* v, int n, const float* periods)
{
	static const bool sOn = [] { // SMS_PORT_INTERP_VALUES=0: comparison runs
		const char* e = std::getenv("SMS_PORT_INTERP_VALUES");
		return e == nullptr || *e != '0';
	}();
	if (!sOn || v == nullptr || n <= 0 || port_interp_rate() == 30)
		return 0;
	ValueSet& vs = sValueSets[key];
	if (!sRedraw) {
		if (vs.tick[vs.cur] != sEntryTick)
			vs.cur ^= 1;
		vs.gen[vs.cur].assign(v, v + n);
		vs.tick[vs.cur] = sEntryTick;
		return 0;
	}
	const int lag = lagNow();
	if (!sInterp || (lag != 0 && lag != 1))
		return 0;
	const int g = vs.tick[0] == sSegState ? 0 : vs.tick[1] == sSegState ? 1 : -1;
	if (g < 0 || (int)vs.gen[g].size() != n)
		return 0;
	const int g2 = lag == 0 ? -1 : vs.tick[0] == sSegState + 1 ? 0 : vs.tick[1] == sSegState + 1 ? 1 : -2; // W12
	if (g2 == -2 || (g2 >= 0 && (int)vs.gen[g2].size() != n))
		return 0;
	vs.live.assign(v, v + n);
	for (int i = 0; i < n; i++) {
		const float a  = vs.gen[g][i];
		const float nv = g2 >= 0 ? vs.gen[g2][i] : v[i];
		float d        = nv - a;
		const float p = periods != nullptr ? periods[i] : 0.0f;
		if (p > 0.0f) {
			if (d > 0.5f * p)
				d -= p;
			else if (d < -0.5f * p)
				d += p;
		}
		v[i] = a + d * sAlpha;
	}
	sPointsLerped += (u32)n; // (SMS_PORT_INTERP_LOG "points")
	return 1;
}

void port_interp_values_end(const void* key, float* v)
{
	auto it = sValueSets.find(key);
	if (it == sValueSets.end() || it->second.live.empty() || v == nullptr)
		return;
	std::memcpy(v, it->second.live.data(), it->second.live.size() * sizeof(float));
	it->second.live.clear();
}

// The game moved slot `from` to `to` (compaction) / filled `index` with a new object, after
// the newest logic copy was taken: the copy follows, so slots keep their identity.
void port_interp_points_move(const void* key, int from, int to)
{
	auto it = sPointSets.find(key);
	if (it == sPointSets.end() || sRedraw)
		return;
	PointSet& ps = it->second;
	const int g = ps.cur, n = (int)ps.valid[g].size();
	if (from < 0 || to < 0 || from >= n || to >= n)
		return;
	std::memcpy(&ps.gen[g][(size_t)to * 3], &ps.gen[g][(size_t)from * 3], 3 * sizeof(float));
	ps.valid[g][to] = ps.valid[g][from];
}

void port_interp_points_invalidate(const void* key, int index)
{
	auto it = sPointSets.find(key);
	if (it == sPointSets.end() || sRedraw)
		return;
	PointSet& ps = it->second;
	if (index >= 0 && index < (int)ps.valid[ps.cur].size())
		ps.valid[ps.cur][index] = 0;
}

void port_interp_points_end(const void* key, float* pos)
{
	auto it = sPointSets.find(key);
	if (it == sPointSets.end() || it->second.live.empty() || pos == nullptr)
		return;
	PointSet& ps = it->second;
	const int n  = (int)(ps.live.size() / 3);
	for (int i = 0; i < n; i++)
		std::memcpy(pos + (size_t)i * ps.stride, &ps.live[(size_t)i * 3], 3 * sizeof(float));
	ps.live.clear();
}

// ---- tracked points (W9-26): objects with a stable address and an age, e.g. JPA particles ----
// port_interp_track(id, pos, age) before drawing one: on a logic draw it remembers the place
// (state sEntryTick); in an interpolated display frame, if the same object (same id, older age
// remembered, at most 8 frames older) was seen on the previous logic frame, it writes the place
// between the two into pos. port_interp_track_end() puts every changed place back.
struct TrackRec {
	float pos[3];
	float age;
	u32 tick;
	float pos2[3]; // W12: the state before (tick2)
	float age2;
	u32 tick2 = ~0u;
};
std::unordered_map<const void*, TrackRec> sTrack;
struct TrackUndo {
	float* pos;
	float live[3];
};
std::vector<TrackUndo> sTrackUndo;

void port_interp_track(const void* id, float* pos, float age)
{
	static const bool sOn = [] { // SMS_PORT_INTERP_PARTICLES=0: comparison runs
		const char* e = std::getenv("SMS_PORT_INTERP_PARTICLES");
		return e == nullptr || *e != '0';
	}();
	if (!sOn || pos == nullptr || port_interp_rate() == 30)
		return;
	if (!sRedraw) {
		TrackRec& r = sTrack[id];
		if (r.tick != sEntryTick && r.tick != ~0u && r.tick + 1 == sEntryTick) {
			std::memcpy(r.pos2, r.pos, sizeof(r.pos2));
			r.age2  = r.age;
			r.tick2 = r.tick;
		}
		std::memcpy(r.pos, pos, sizeof(r.pos));
		r.age  = age;
		r.tick = sEntryTick;
		if ((sEntryTick & 63) == 0 && sTrack.size() > 4096) // forget old ones now and then
			for (auto it = sTrack.begin(); it != sTrack.end();)
				it = sEntryTick - it->second.tick > 2 ? sTrack.erase(it) : std::next(it);
		return;
	}
	const int lag = lagNow();
	if (!sInterp || (lag != 0 && lag != 1))
		return;
	auto it = sTrack.find(id);
	if (it == sTrack.end())
		return;
	const TrackRec& tr = it->second;
	const float* from;
	const float* to;
	if (lag == 0) {
		if (tr.tick != sSegState || !(age > tr.age) || age > tr.age + 8.0f)
			return;
		from = tr.pos;
		to   = pos;
	} else { // W12: between the copies of the states sSegState and sSegState + 1
		if (tr.tick != sSegState + 1 || tr.tick2 != sSegState || !(tr.age > tr.age2) || tr.age > tr.age2 + 8.0f)
			return;
		from = tr.pos2;
		to   = tr.pos;
	}
	TrackUndo u;
	u.pos = pos;
	std::memcpy(u.live, pos, sizeof(u.live));
	sTrackUndo.push_back(u);
	float res[3];
	for (int k = 0; k < 3; k++)
		res[k] = lerpf(from[k], to[k], sAlpha);
	std::memcpy(pos, res, sizeof(res));
	++sPointsLerped;
}

void port_interp_track_end(void)
{
	for (auto it = sTrackUndo.rbegin(); it != sTrackUndo.rend(); ++it)
		std::memcpy(it->pos, it->live, sizeof(it->live));
	sTrackUndo.clear();
}

// TMBindShadowManager::perform, right before calcVtx.
void port_interp_shadow_requests(void* requests, int num, int stride)
{
	if (!sShadowsOn || requests == nullptr || num < 0 || stride < 0x24 || port_interp_rate() == 30)
		return;
	// The shadow manager runs in the GX draw list: on a logic frame its calcVtx is part of the
	// logic draw of state sDrawState (the requests of that state; the draw then clears them).
	if (!sRedraw) {
		const int slot = sDrawState & 1;
		sShadow[slot].assign((const u8*)requests, (const u8*)requests + (size_t)num * stride);
		sShadowTick[slot] = sDrawState;
		sShadowStride     = stride;
		return;
	}
	// A display frame between sSegState and sSegState + 1: the live list holds the requests of
	// sSegState + 1 (made since that logic draw), the snapshot those of sSegState.
	const int lag = lagNow();
	if (!sInterp || stride != sShadowStride || (lag != 0 && lag != 1))
		return;
	const int sa = sSegState & 1, sb = (sSegState + 1) & 1;
	if (sShadowTick[sa] != sSegState)
		return;
	if (lag == 1) { // W12: the live list is a later state's: use the copy of state sSegState + 1
		if (sShadowTick[sb] != sSegState + 1)
			return;
		const int cnt = (int)(sShadow[sb].size() / stride);
		const int c   = cnt < num ? cnt : num;
		std::memcpy(requests, sShadow[sb].data(), (size_t)c * stride);
	}
	const u8* prev    = sShadow[sa].data();
	const int prevNum = (int)(sShadow[sa].size() / stride);
	u8* cur           = (u8*)requests;
	static const bool sShLog = std::getenv("SMS_PORT_INTERP_SHADOWLOG") != nullptr;
	if (sShLog) {
		std::fprintf(stderr, "[interp] shadows frame %u alpha %.2f: %d requests (logic frame had %d, previous %d)\n",
		             gPortFrameCount, sAlpha, num, (int)(sShadow[sb].size() / stride), prevNum);
		for (int i = 0; i < num && i < 4; i++) {
			float c[6], b[6] = {}, a[6] = {};
			std::memcpy(c, cur + (size_t)i * stride, sizeof(c));
			if (i < (int)(sShadow[sb].size() / stride))
				std::memcpy(b, sShadow[sb].data() + (size_t)i * stride, sizeof(b));
			if (i < prevNum)
				std::memcpy(a, prev + (size_t)i * stride, sizeof(a));
			std::fprintf(stderr, "[interp]   #%d type %u/%u now %.0f %.0f %.0f r %.0f | logic %.0f %.0f %.0f | prev %.0f %.0f %.0f\n", i,
			             *(const u32*)(cur + (size_t)i * stride + 0x20), cur[(size_t)i * stride + 0x1C], c[0], c[1], c[2],
			             c[3], b[0], b[1], b[2], a[0], a[1], a[2]);
		}
	}
	for (int i = 0; i < num; i++) {
		u8* rq = cur + (size_t)i * stride;
		float p[6];
		std::memcpy(p, rq, sizeof(p)); // position, radius x/z, rotation y
		u32 type;
		std::memcpy(&type, rq + 0x20, 4);
		const u8 shadowType = rq[0x1C];
		// its match in the previous logic frame: same index, else the nearest of the same kind
		auto dist2 = [&](int j) { // squared distance to prev[j], or -1 if another kind
			const u8* pq = prev + (size_t)j * stride;
			u32 pt;
			std::memcpy(&pt, pq + 0x20, 4);
			if (pt != type || pq[0x1C] != shadowType)
				return -1.0f;
			float pp[3];
			std::memcpy(pp, pq, sizeof(pp));
			return (pp[0] - p[0]) * (pp[0] - p[0]) + (pp[1] - p[1]) * (pp[1] - p[1]) + (pp[2] - p[2]) * (pp[2] - p[2]);
		};
		const float kMax2 = 200.0f * 200.0f;
		const u8* best    = nullptr;
		if (i < prevNum) {
			const float d = dist2(i);
			if (d >= 0.0f && d < kMax2)
				best = prev + (size_t)i * stride;
		}
		if (best == nullptr) {
			float bestD2 = kMax2;
			for (int j = i - 16 < 0 ? 0 : i - 16; j < prevNum && j <= i + 16; j++) {
				const float d = dist2(j);
				if (d >= 0.0f && d < bestD2)
					bestD2 = d, best = prev + (size_t)j * stride;
			}
		}
		if (best == nullptr)
			continue;
		float q[6];
		std::memcpy(q, best, sizeof(q));
		for (int k = 0; k < 5; k++)
			p[k] = lerpf(q[k], p[k], sAlpha);
		if (std::fabs(p[5] - q[5]) < 45.0f)
			p[5] = lerpf(q[5], p[5], sAlpha);
		std::memcpy(rq, p, sizeof(p));
	}
}

// ---- port_mardirector_redraw, with the camera's live matrices (overwritten with the
// interpolated ones until port_interp_redraw_draw_end). 0: no display frame now.
int port_interp_redraw_begin(f32* view, f32* proj)
{
	if (!sRedraw || !sSegValid)
		return 0;
	const CamSnap& a = sCam[sSegState % kGens];
	const CamSnap& b = sCam[(sSegState + 1) % kGens];
	if (a.tick != sSegState || b.tick != sSegState + 1)
		return 0;
	// Queued draws (a logic draw in this Aurora frame) may still read matrices and vertex
	// arrays the unk34 pass is about to rewrite.
	sT[0] = qpc();
	GXDrawDone();
	sT[1] = qpc();
	sOcc.clear(); // occurrences counted again in this frame's interpolated view pass
	texUndoAll(); // (nothing pending normally)
	if (sClearFirst) {
		port_efb_clear_all();
		sClearFirst = false;
	}
	std::memcpy(sLiveView, view, sizeof(sLiveView));
	std::memcpy(sLiveProj, proj, sizeof(sLiveProj));
	float v[12], p[16];
	// SMS_PORT_INTERP_CAMERA=0: the camera steps at 30 (the newer state), the rest interpolates (comparison)
	static const bool sCamOff = [] {
		const char* e = std::getenv("SMS_PORT_INTERP_CAMERA");
		return e != nullptr && *e == '0';
	}();
	sCamCut = sCamOff || !lerpCamera(a, b, sAlpha, v, p);
	if (sCamCut) {
		std::memcpy(v, b.view, sizeof(v));
		std::memcpy(p, b.proj, sizeof(p));
	}
	std::memcpy(view, v, sizeof(v));
	std::memcpy(proj, p, sizeof(p));
	sCamApplied = true;
	sInterp     = true;
	return 1;
}

// The display frame's draw lists are recorded: live camera and models again (the restoring
// unk34 pass follows).
int port_interp_redraw_draw_end(f32* view, f32* proj)
{
	sInterp = false;
	sT[2] = qpc();
	GXDrawDone(); // the draws just queued read the interpolated matrices and arrays
	sT[3] = qpc();
	texUndoAll(); // the material lists as the logic unk34 built them (the next logic draw uses them)
	if (sCamApplied) {
		std::memcpy(view, sLiveView, sizeof(sLiveView));
		std::memcpy(proj, sLiveProj, sizeof(sLiveProj));
		sCamApplied = false;
	}
	static const bool sNoRestore = std::getenv("SMS_PORT_INTERP_NORESTORE") != nullptr; // debug
	return sNoRestore ? 0 : 1;
}

// ---- port_main.cpp: pacing ---------------------------------------------------------------
// Extra display frames are drawn: frame rate above 30, during gameplay (a stage draw in
// the last 0.2 s).
bool port_interp_active(void)
{
	if (port_interp_rate() == 30 || !sSegValid || port_qol_ff_speed() > 1) // not while fast-forwarding
		return false;
	return toMs(qpc() - sDrawQpc) < 200.0;
}

// Main thread, after each VI retrace it waited for (retraceQpc: when it fired). The first
// retrace after a logic draw puts that draw's picture up.
void port_interp_retrace(long long retraceQpc)
{
	if (sLogicDraws == 0 || (sSegValid && sSegState == sDrawState))
		return;
	if (sSegValid && sDrawState == sSegState + 1) {
		const double d = toMs(retraceQpc - sSegStart);
		if (d > 10.0 && d < 120.0)
			sTickMs += (d - sTickMs) * 0.25;
	}
	sSegState = sDrawState;
	sSegStart = retraceQpc;
	sSegValid = true;
}

// QPC time the next display frame is due.
long long port_interp_due(void) { return sNextDue; }

// Whether the frame due at `due` is closer to `t` than half a display period.
bool port_interp_near(long long due, long long t)
{
	const double p = periodMs();
	const long long d = due > t ? due - t : t - due;
	return d < fromMs(p * 0.5);
}

// How far ahead of its due time a display frame is started (its recent maximum cost and a margin).
// w14 (brought in by w9): +2 ms while Aurora holds finished display frames until their slot
// (port_main.cpp setPresentSlot): drawing early costs no timing any more, and the frame is then
// ready before the retrace its slot often falls on (60/120). SMS_PORT_INTERP_NOHOLD=1: the old lead.
long long port_interp_lead(void)
{
	static const bool sNoHold = std::getenv("SMS_PORT_INTERP_NOHOLD") != nullptr;
	return fromMs(sCostMax + 1.2 + (sNoHold || periodMs() <= 0.0 ? 0.0 : 2.0));
}

// Whether a display frame started now would be presented before `deadline`.
bool port_interp_fits(long long now, long long deadline) { return now + fromMs(sCostMax + 1.0) < deadline; }

// A due frame that could not be drawn: try again a period later.
void port_interp_skip(long long now)
{
	const double p = periodMs();
	sNextDue       = now + fromMs(p > 1.0 ? p : 1.0);
}

// A due frame that would not be finished before the retrace at `retrace`: drawn right
// after it instead (if that retrace has no logic frame to present).
void port_interp_defer(long long retrace) { sNextDue = retrace; }

// A logic frame's picture was just presented at the retrace that fired at `retrace`:
// display frames continue on a grid from that retrace.
void port_interp_logic_presented(long long retrace)
{
	const double p = periodMs();
	sNextDue       = retrace + fromMs(p > 1.0 ? p : 1.0);
	if (sLog)
		std::fprintf(stderr, "[interp] %.3f logic frame (state %u)\n", toMs(qpc()), sDrawState);
}

// Draws one display frame into the open Aurora frame (logicPending: it already holds a
// logic draw, which this replaces). False if nothing was drawn.
bool port_interp_render(long long now, bool logicPending)
{
	static const bool sNoRedraw = [] { // SMS_PORT_INTERP_REDRAW=0: no extra frames (comparison)
		const char* e = std::getenv("SMS_PORT_INTERP_REDRAW");
		return e != nullptr && *e == '0';
	}();
	if (sNoRedraw || !sSegValid || sEntryTick < sSegState + 1)
		return false;
	const double a = toMs(now - sSegStart) / sTickMs;
	sAlpha         = a < 0.0 ? 0.0f : a > 1.0 ? 1.0f : (float)a;
	sClearFirst    = logicPending;
	sRedraw        = true;
	sInterpModels  = 0;
	sCamCut        = false;
	port_rand_scratch(true);
	port_efb_set_presentation_only(1); // port_efb.cpp: GPU-only copies, game-frame peeks, no readbacks
	const long long tStart = qpc();
	const int ok = port_app_redraw();
	const long long tEnd = qpc();
	port_efb_set_presentation_only(0);
	port_rand_scratch(false);
	sRedraw     = false;
	sInterp     = false;
	sClearFirst = false;
	if (sLog && ok) {
		float pa[3], pb[3];
		camPos(sCam[sSegState % kGens].view, pa);
		camPos(sCam[(sSegState + 1) % kGens].view, pb);
		const float move = std::sqrt((pb[0] - pa[0]) * (pb[0] - pa[0]) + (pb[1] - pa[1]) * (pb[1] - pa[1])
		                             + (pb[2] - pa[2]) * (pb[2] - pa[2]));
		std::fprintf(stderr,
		             "[interp] %.3f display frame alpha %.3f (states %u-%u, frame %u)%s%s, %u models, camera moved %.0f"
		             " (ms: drain %.2f, view+draw %.2f, drain %.2f, restore+fader %.2f), texmtx %u patched / %zu"
		             " recorded / %u rejected, joints: %u would snap (old), max turn %.0f deg, points %u/%u\n",
		             toMs(now), sAlpha, sSegState, sSegState + 1, gPortFrameCount, logicPending ? " over logic" : "",
		             sCamCut ? " CUT" : "", sInterpModels, move, toMs(sT[1] - sT[0]), toMs(sT[2] - sT[1]),
		             toMs(sT[3] - sT[2]), toMs(tEnd - sT[3]), sTexPatched, sTexRecs.size(), sTexRejected, sOldSnaps,
		             sMaxTurn, sPointsLerped, sPointsSeen);
	}
	if (sLog) {
		sPointsLerped = sPointsSeen = 0;
		sTexPatched = 0;
		sOldSnaps   = 0;
		sMaxTurn    = 0.0f;
	}
	return ok != 0;
}

// After a display frame was presented (start: when port_interp_render was called).
void port_interp_presented(long long start, long long end)
{
	const double cost = toMs(end - start);
	sCostMs += (cost - sCostMs) * 0.2;
	if (sCostMs > 30.0)
		sCostMs = 30.0;
	sCostMax = cost > sCostMax * 0.97 ? (cost < 30.0 ? cost : 30.0) : sCostMax * 0.97;
	const double p = periodMs();
	if (p <= 0.0) {
		sNextDue = end + fromMs(1.0); // unlimited: still yield 1 ms per frame to the other game threads
	} else {
		sNextDue += fromMs(p);
		if (sNextDue < end) // fell behind: restart the grid
			sNextDue = end + fromMs(p);
	}
	if (sLog)
		std::fprintf(stderr, "[interp] %.3f presented, %.2f ms\n", toMs(end), cost);
}

} // extern "C"

#endif // SMS_PORT_AURORA
