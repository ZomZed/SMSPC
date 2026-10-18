// Island test (debug, settings menu > Debug): the seamless island made from the exported stage
// maps (a Blender layout, converted by level_export/tools/isle_build.py into <exe dir>\isle\isle.arc).
//
// Its own map: loading it mounts isle.arc as the stage archive straight from the host file
// (TApplication::mountStageArchive -> port_isle_stage_archive); the disc is not touched. The archive
// is Bianco Hills episode 1's with the island as its map (isle_build.py), so the game runs it under
// that stage's number; leaving the island turns island mode off, and Bianco Hills is the real one
// again. Its model is split over map.bmd, map1.bmd, ... and its collision over map.col, map1.col, ...
// (MapModel.cpp / Map.cpp load the extra ones, port only). A later step: an option to play the
// island in single player / multiplayer instead of the hub.
// In the island: 1 / D-pad left = Hover nozzle, 2 / D-pad up = Rocket, 3 / D-pad right = Turbo,
// 4 / D-pad down = back to the plain squirt (FLUDD's own button still switches as usual).
// Drawing: the island's models hold one joint per 16384-unit tile; each logic frame the tiles
// farther than the draw distance, or behind the camera, are put to sleep (TJointObj hides their
// shapes), so a frame draws what can be seen instead of all ~2000 shapes.
#ifdef SMS_PORT_AURORA

#include "port_settings.h"

#include <Camera/Camera.hpp>
#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DJoint.hpp>
#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <Map/JointModel.hpp>
#include <Map/Map.hpp>
#include <Map/MapCollisionData.hpp>
#include <Map/MapData.hpp>
#include <Map/MapModel.hpp>
#include <MoveBG/MapObjWave.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <Player/WaterGun.hpp>
#include <System/Application.hpp>
#include <System/CardManager.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>

#define IMGUI_USER_CONFIG "aurora/imgui_config.h"
#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" int port_key_held(int scancode);                                     // port_qol.cpp
extern "C" int port_mp_active(void);                                               // port_mp_net.cpp

namespace {

const int kStage = 2, kScenario = 0; // the stage number the island runs under (its archive is Bianco's)

std::atomic<int> sRequest{ 0 }; // 1 = load the island, 2 = leave it
bool sOn;                       // island mode: the next load of kStage/kScenario is the island
bool sVisited;                  // the island has been loaded since island mode came on
bool sToldKeys;
const float kDrawDistance = 90000.0f; // game units
int sVisibleTiles, sTotalTiles;
const void* sWaveFor; // the wave object given Gelato's settings (once per island load)

std::string islePath()
{
	if (const char* e = std::getenv("SMS_PORT_ISLE"))
		if (*e != '\0')
			return e;
	char buf[MAX_PATH];
	const DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	std::string base(buf, n);
	const size_t slash = base.find_last_of("\\/");
	return (slash == std::string::npos ? std::string(".") : base.substr(0, slash)) + "\\isle\\isle.arc";
}

bool available()
{
	const DWORD a = GetFileAttributesA(islePath().c_str());
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool inIsland()
{
	TApplication* app = SMSGetApplication();
	return sOn && app != nullptr && app->mCurrArea.getStage() == kStage && app->mCurrArea.getScenario() == kScenario;
}

bool directorIdle()
{
	TMarDirector* d = gpMarDirector;
	return d != nullptr
	       && (d->mFlags
	           & (TMarDirector::DIRECTOR_FLAG_SHINE_GET_PENDING | TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING
	              | TMarDirector::DIRECTOR_FLAG_ACTOR_DEMO_STAGE_TRANSITION_PENDING
	              | TMarDirector::DIRECTOR_FLAG_GATE_DEMO_STAGE_TRANSITION_PENDING
	              | TMarDirector::DIRECTOR_FLAG_GAME_OVER_PENDING))
	              == 0;
}

void goTo(int stage, int scenario)
{
	TApplication* app = SMSGetApplication();
	if (app->mCurrArea.getStage() == 15) { // the file select: as the multiplayer free mode starts
		gpMarDirector->setNextStage((u16)(((stage + 1) << 8) | scenario), nullptr);
		gpCardManager->unmount();
	} else {
		app->setNextArea(TGameSequence((u8)stage, (u8)scenario));
		gpMarDirector->onFlag(TMarDirector::DIRECTOR_FLAG_STAGE_TRANSITION_PENDING);
	}
}

// SMS_PORT_ISLE_PROBE=<x>,<z> (diagnostics): once the island is up, logs the collision grid
// cell's floor lists at that point, as TMapCollisionData::checkGround walks them.
void probeCollision()
{
	static bool sDone;
	static const char* sProbe = std::getenv("SMS_PORT_ISLE_PROBE");
	if (sDone || sProbe == nullptr || gpMap == nullptr || gpMap->mCollisionData == nullptr || gpMarioOriginal == nullptr)
		return;
	sDone = true;
	float x = 0, z = 0;
	if (std::sscanf(sProbe, "%f,%f", &x, &z) != 2)
		return;
	TMapCollisionData* cd = gpMap->mCollisionData;
	const int gx = (int)((x + cd->mGridExtentX) * (1.0f / 1024));
	const int gz = (int)((z + cd->mGridExtentY) * (1.0f / 1024));
	std::fprintf(stderr, "[isle] probe %.1f %.1f: cell %d %d of %d x %d\n", x, z, gx, gz, cd->unk8, cd->unkC);
	for (int r = 0; r < 2; ++r) {
		const TBGCheckListRoot& root = r == 0 ? cd->getGridRoot14(gx, gz) : cd->getGridRoot18(gx, gz);
		int n = 0;
		for (const TBGCheckList* l = root.getGroundList(); l != nullptr && n < 200; l = l->getNext(), ++n) {
			const TBGCheckData* d = l->unk8;
			std::fprintf(stderr,
			             "[isle]   %s ground %d: type %#x flags %#x minY %.2f maxY %.2f p (%.1f %.2f %.1f) (%.1f %.2f %.1f) "
			             "(%.1f %.2f %.1f) n (%.3f %.3f %.3f) d %.2f\n",
			             r == 0 ? "static" : "moving", n, d->mBGType, d->mFlags, d->mMinY, d->mMaxY, d->mPoint1.x,
			             d->mPoint1.y, d->mPoint1.z, d->mPoint2.x, d->mPoint2.y, d->mPoint2.z, d->mPoint3.x, d->mPoint3.y,
			             d->mPoint3.z, d->mNormal.x, d->mNormal.y, d->mNormal.z, d->mPlaneDistance);
		}
	}
	for (float y : { 60.0f, 100.0f, 200.0f }) {
		const TBGCheckData* hit = nullptr;
		const float h           = cd->checkGround(x, y, z, 0, &hit);
		std::fprintf(stderr, "[isle]   checkGround(y %.0f) = %.2f type %#x\n", y, h, hit ? hit->mBGType : 0xffff);
	}
}

// Key 5: Mario's position on screen and in the log (for reporting spots on the island).
void positionKey()
{
	static bool sLast;
	const bool k = port_key_held(34) != 0; // SDL_SCANCODE_5
	if (k && !sLast && gpMarioOriginal != nullptr) {
		const JGeometry::TVec3<f32>& p = gpMarioOriginal->mPosition;
		char msg[96];
		std::snprintf(msg, sizeof(msg), "Mario at %.0f, %.0f, %.0f", p.x, p.y, p.z);
		port_qol_toast(msg);
		std::fprintf(stderr, "[isle] %s\n", msg);
	}
	sLast = k;
}

void nozzleKeys()
{
	TMario* m = gpMarioOriginal;
	if (m == nullptr || m->mWaterGun == nullptr || m->mGamePad == nullptr)
		return;
	static bool sKeyLast[4];
	static u32 sDpadLast;
	const u32 dpad = m->mGamePad->mButton.mButton & 0xF;
	const u32 dt   = dpad & ~sDpadLast;
	sDpadLast      = dpad;
	static const int kKeys[4] = { 30, 31, 32, 33 }; // SDL_SCANCODE_1..4
	int pick = -1;
	for (int i = 0; i < 4; ++i) {
		const bool k = port_key_held(kKeys[i]) != 0;
		if (k && !sKeyLast[i])
			pick = i;
		sKeyLast[i] = k;
	}
	if (dt & JUTGamePad::DPAD_LEFT)
		pick = 0;
	else if (dt & JUTGamePad::DPAD_UP)
		pick = 1;
	else if (dt & JUTGamePad::DPAD_RIGHT)
		pick = 2;
	else if (dt & JUTGamePad::DPAD_DOWN)
		pick = 3;
	if (pick < 0)
		return;
	static const TWaterGun::TNozzleType kType[4] = { TWaterGun::Hover, TWaterGun::Rocket, TWaterGun::Turbo,
		                                             TWaterGun::Spray };
	static const char* const kName[4] = { "Hover nozzle", "Rocket nozzle", "Turbo nozzle", "Squirt nozzle" };
	m->mWaterGun->changeNozzle(kType[pick], true);
	port_qol_toast(kName[pick]);
}

// The waves around Mario (TMapObjWave, wave.bti) as at Gelato Beach, whose sea the island uses: Gelato's
// wave heights (TMapObjWave::load, map 4), and the per-frame height and alpha by the water's depth under
// Mario that TMapObjWave::movement only runs for Gelato and Sirena (maps 4 and 6). Without them the
// patch keeps full alpha and open-sea waves at the shore, and the shallows lose their rippling ring.
void gelatoWaves()
{
	TMapObjWave* w = gpMapObjWave;
	if (w == nullptr || w->unk94 == nullptr || gpMarioOriginal == nullptr)
		return;
	if (sWaveFor != w) {
		sWaveFor = w;
		w->unk2C = 40.0f;
		w->unk30 = 30.0f;
		w->unk34 = 5.0f;
		w->unk38 = 0.0f;
		w->unk3C = w->unk2C;
		w->unk40 = w->unk30;
	}
	w->updateHeightAndAlpha();
}

// Hides the island's tiles that cannot be seen (see the top of the file).
void cullTiles()
{
	if (gpMap == nullptr || gpCamera == nullptr || gpMap->getModelManager() == nullptr)
		return;
	Vec eye, at;
	gpCamera->JSGGetViewPosition(&eye);
	gpCamera->JSGGetViewTargetPosition(&at);
	float fx = at.x - eye.x, fz = at.z - eye.z;
	const float fl = std::sqrt(fx * fx + fz * fz);
	if (fl > 1e-3f) {
		fx /= fl;
		fz /= fl;
	}
	TMapModelManager* mm = gpMap->getModelManager();
	int visible = 0, total = 0;
	for (int i = 0; i < mm->getJointModelNum(); ++i) {
		TJointObj* root = (TJointObj*)mm->getJointModel(i);
		for (int k = 0; k < root->getChildrenNum(); ++k) {
			TJointObj* tile = root->getChild(k);
			J3DJoint* j     = tile->getJoint();
			const Vec& mn = j->getMin();
			const Vec& mx = j->getMax();
			// nearest point of the tile's box to the eye (XZ), and the box centre
			const float nx = eye.x < mn.x ? mn.x : (eye.x > mx.x ? mx.x : eye.x);
			const float nz = eye.z < mn.z ? mn.z : (eye.z > mx.z ? mx.z : eye.z);
			const float dx = nx - eye.x, dz = nz - eye.z;
			const float dist = std::sqrt(dx * dx + dz * dz);
			const float cx = (mn.x + mx.x) * 0.5f - eye.x, cz = (mn.z + mx.z) * 0.5f - eye.z;
			const float half = 0.5f * std::sqrt((mx.x - mn.x) * (mx.x - mn.x) + (mx.z - mn.z) * (mx.z - mn.z));
			const bool behind = fl > 1e-3f && (cx * fx + cz * fz) < -half;
			const bool show   = dist < kDrawDistance && (!behind || dist < 4000.0f);
			if (show)
				tile->awake();
			else
				tile->sleep();
			visible += show;
			++total;
		}
	}
	static int sLogged;
	if (sLogged < 3 && total > 0) {
		++sLogged;
		std::fprintf(stderr, "[isle] tiles %d of %d drawn, eye %.0f %.0f %.0f, models %d\n", visible, total, eye.x, eye.y,
		             eye.z, mm->getJointModelNum());
	}
	sVisibleTiles = visible;
	sTotalTiles   = total;
}

} // namespace

extern "C" {

int port_isle_active(void) { return sOn ? 1 : 0; }
// 1 while the island itself is the stage being loaded/played (Map.cpp sea objects).
int port_isle_here(void) { return inIsland() ? 1 : 0; }

// SMS_PORT_ISLE_AUTO=1 (tests): port_warp_target (sdk_os_misc.cpp) turns the island on before the
// boot warp, so SMS_PORT_WARP=2,0 lands in it.
void port_isle_force_on(void)
{
	if (!sOn && available())
		sOn = true;
}

// TApplication::mountStageArchive: in island mode, the stage archive is isle.arc, read from the
// host file into the current heap (as SMSLoadArchive would); nullptr = load from the disc as usual.
void* port_isle_stage_archive(int stage, int scenario)
{
	if (!sOn || stage != kStage || scenario != kScenario)
		return nullptr;
	FILE* f = std::fopen(islePath().c_str(), "rb");
	if (f == nullptr)
		return nullptr;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	void* buf = size > 0 ? JKRHeap::alloc((u32)size, 32, nullptr) : nullptr;
	const bool ok = buf != nullptr && std::fread(buf, 1, (size_t)size, f) == (size_t)size;
	std::fclose(f);
	std::fprintf(stderr, "[isle] stage archive: %s (%ld bytes)%s\n", islePath().c_str(), size, ok ? "" : " FAILED");
	return ok ? buf : nullptr;
}

// port_mp_logic_frame (every game logic frame, single player too).
void port_isle_logic_frame(void)
{
	const int req = sRequest.exchange(0);
	if (req != 0 && directorIdle()) {
		if (req == 1 && available()) {
			sOn       = true;
			sVisited  = false;
			sToldKeys = false;
			std::fprintf(stderr, "[isle] loading the island: %s\n", islePath().c_str());
			goTo(kStage, kScenario);
		} else if (req == 2) {
			sOn = false;
			std::fprintf(stderr, "[isle] left the island\n");
			goTo(15, 0); // the file select
		}
	}
	if (!inIsland()) {
		if (sOn && sVisited && directorIdle()) { // left the island the game's way: island mode ends
			sOn      = false;
			sVisited = false;
			std::fprintf(stderr, "[isle] left the island: island mode off\n");
		}
		return;
	}
	sVisited = true;
	if (!sToldKeys && gpMarioOriginal != nullptr) {
		sToldKeys = true;
		port_qol_toast("Island test: 1 Hover, 2 Rocket, 3 Turbo, 4 Squirt (or the D-pad), 5 position");
	}
	cullTiles();
	gelatoWaves();
	nozzleKeys();
	positionKey();
	probeCollision();
}

// The settings menu's Debug tab.
void port_isle_menu(void)
{
	ImGui::TextUnformatted("Island test");
	ImGui::TextDisabled("The seamless island (level_export/tools/isle_build.py -> isle\\isle.arc).");
	if (!available()) {
		ImGui::TextWrapped("Not built yet: %s is missing.", islePath().c_str());
		return;
	}
	if (!sOn) {
		if (ImGui::Button("Load the island") && !port_mp_active()) // (not in a multiplayer session)
			sRequest = 1;
		ImGui::TextDisabled("Loads the island as its own map (a debug test for now).");
	} else {
		if (ImGui::Button("Leave the island (to the file select)"))
			sRequest = 2;
		ImGui::TextDisabled("Island mode is on until you leave the island.");
	}
	ImGui::TextDisabled("In the island: 1 / D-pad left Hover, 2 / up Rocket, 3 / right Turbo, 4 / down Squirt.");
	if (inIsland())
		ImGui::Text("Tiles drawn: %d of %d (draw distance %.0f)", sVisibleTiles, sTotalTiles, kDrawDistance);
}

} // extern "C"

#endif // SMS_PORT_AURORA
