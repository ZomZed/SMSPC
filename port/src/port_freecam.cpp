// The Free camera (settings: Camera > Free camera), built like sm64ex's Puppycam / sm64coopdx's
// free camera instead of bending the game's own camera:
//   - its own yaw, tilt and distance: the mouse turns it directly, the C-stick (or the keyboard's
//     camera keys) at a steady rate, L glides it behind Mario;
//   - it always looks at one point on Mario (about head height), so a low camera looks up at
//     him and never points at the floor;
//   - tilt from just under his head height (looking up a little) to about 55 degrees above;
//   - collision: the nearest surface on the line from that point to the camera puts the camera
//     just in front of it, and the camera stays above the ground / under a roof;
//   - zoom: a few gentle steps (the mouse wheel), gliding (port_free_camera_zoom).
// cameragc.cpp calls port_freecam_frame at the end of the normal / tower camera's update and
// takes back the position, the look-at point and the yaw (which Mario's controls follow).
#ifdef SMS_PORT_AURORA

#include "port_settings.h"

#include <Camera/cameralib.hpp>
#include <Map/Map.hpp>

#include <cmath>

extern "C" int port_mouse_take_raw(float* dx, float* dy); // port_mouse.cpp
extern "C" float port_free_camera_zoom(void);

namespace {

const float kAnchorHeight = 110.0f;    // the look-at point above Mario's feet
const s16 kTiltMin        = -0x0400;   // about -5 degrees: low, looking slightly up at Mario
const s16 kTiltMax        = 0x2700;    // about 55 degrees
const float kGap          = 30.0f;     // kept from walls / floors / roofs
const float kMinDist      = 120.0f;

bool sActive;
f32 sYaw, sTilt; // angle units (65536 a turn), as floats for smooth mouse input
f32 sBaseDist;   // the game camera's distance when the free camera took over
bool sRecentre;

f32 dist3(const Vec& a, const Vec& b)
{
	const f32 dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
	return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The nearest collision on the segment a -> b (TMap::intersectLine returns the first one it
// meets, not the nearest: shorten the segment to each hit until nothing closer is found).
bool nearestHit(const JGeometry::TVec3<f32>& a, JGeometry::TVec3<f32>& b)
{
	bool any = false;
	for (int i = 0; i < 6; i++) {
		JGeometry::TVec3<f32> hit;
		if (gpMap->intersectLine(a, b, false, &hit) == nullptr)
			break;
		any = true;
		// stop a hair before it so the next test does not find the same triangle
		b.x = a.x + (hit.x - a.x) * 0.98f;
		b.y = a.y + (hit.y - a.y) * 0.98f;
		b.z = a.z + (hit.z - a.z) * 0.98f;
	}
	return any;
}

} // namespace

extern "C" {

// The normal / tower camera's update (cameragc.cpp). In: the game's camera (pos, at, yaw and
// pitch), Mario's position and facing, the C-stick (after the invert / speed settings), L just
// pressed, `restart` when the free camera was not in charge last frame. Out: pos, at and yaw.
// Returns 0 when the free camera is off.
int port_freecam_frame(f32* pos, f32* at, s16* yaw, s16 pitch, const f32* mario, s16 marioFaceY, f32 stickX,
                       f32 stickY, int lPressed, int restart)
{
	const PortSettings& s = gPortSettings;
	if (!s.qolFreeCamera || gpMap == nullptr) {
		sActive = false;
		return 0;
	}
	const JGeometry::TVec3<f32> anchor(mario[0], mario[1] + kAnchorHeight, mario[2]);
	if (!sActive || restart) {
		// take over from where the game camera is, so nothing jumps
		const Vec p = { pos[0], pos[1], pos[2] };
		const Vec a = { at[0], at[1], at[2] };
		sYaw      = (f32)*yaw;
		sTilt     = (f32)pitch;
		sBaseDist = dist3(p, a);
		if (sBaseDist < 600.0f || sBaseDist > 2500.0f)
			sBaseDist = 1100.0f;
		sActive   = true;
		sRecentre = false;
	}
	// input: the mouse turns it by how far it moved, the C-stick at a steady rate
	f32 mdx = 0.0f, mdy = 0.0f;
	port_mouse_take_raw(&mdx, &mdy);
	const f32 k = s.mouseSensitivity * 0.7f; // sensitivity 40: ~0.15 degrees a pixel
	sYaw += -mdx * k * (s.qolCamInvertX ? -1.0f : 1.0f);
	sTilt += mdy * k * (s.mouseInvertY ? -1.0f : 1.0f); // mouse up: the camera goes down, looks up
	sYaw += stickX * 0x300;
	sTilt -= stickY * 0x220;
	if (mdx != 0.0f || stickX != 0.0f)
		sRecentre = false;
	// L: glide behind Mario
	if (lPressed)
		sRecentre = true;
	if (sRecentre) {
		const s16 want = (s16)(marioFaceY + 0x8000);
		const s16 diff = (s16)(want - (s16)sYaw);
		sYaw += diff * 0.35f;
		if (diff > -200 && diff < 200)
			sRecentre = false;
	}
	if (sTilt < kTiltMin)
		sTilt = kTiltMin;
	if (sTilt > kTiltMax)
		sTilt = kTiltMax;
	while (sYaw > 32768.0f)
		sYaw -= 65536.0f;
	while (sYaw < -32768.0f)
		sYaw += 65536.0f;

	// where it wants to be
	f32 dist = sBaseDist * port_free_camera_zoom();
	if (dist < kMinDist)
		dist = kMinDist;
	Vec want;
	CLBPolarToCross(anchor, &want, dist, (s16)sTilt, (s16)sYaw);
	JGeometry::TVec3<f32> cam(want.x, want.y, want.z);
	// collision: in front of the nearest wall / floor / roof between Mario and it
	if (nearestHit(anchor, cam)) {
		JGeometry::TVec3<f32> d(cam.x - anchor.x, cam.y - anchor.y, cam.z - anchor.z);
		const f32 len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		const f32 keep = len > kGap + kMinDist * 0.25f ? (len - kGap) / len : 0.25f;
		cam.x = anchor.x + d.x * keep;
		cam.y = anchor.y + d.y * keep;
		cam.z = anchor.z + d.z * keep;
	}
	// never under the ground or above a roof right there
	const TBGCheckData* bg = nullptr;
	const f32 ground = gpMap->checkGround(cam.x, cam.y + 60.0f, cam.z, &bg);
	if (ground > -100000.0f && cam.y < ground + kGap)
		cam.y = ground + kGap;
	const f32 roof = gpMap->checkRoof(cam.x, cam.y - 60.0f, cam.z, &bg);
	if (roof < 100000.0f && roof > ground + 2.0f * kGap && cam.y > roof - kGap)
		cam.y = roof - kGap;

	pos[0] = cam.x;
	pos[1] = cam.y;
	pos[2] = cam.z;
	at[0]  = anchor.x;
	at[1]  = anchor.y;
	at[2]  = anchor.z;
	*yaw   = (s16)sYaw;
	return 1;
}

} // extern "C"

#endif // SMS_PORT_AURORA
