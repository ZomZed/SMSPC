// Quality-of-life options for the Aurora build (w9): F1 > Quality of life, [qol] in
// port_settings.ini. Every option is OFF by default, so the game behaves as the original.
//
// Game-side hooks (all under SMS_PORT, GameCube build unchanged) ask this file through the
// small C API below. The options that change what the game does (skip intro/cutscenes, text
// speed, auto-save, continue after a Shine, camera, cheats, fast-forward) are combined once
// per game frame into gQolState; recordings store it (port_replay.cpp, event E_QOL) and a
// replay applies the recorded values, so replays stay exact whatever the player's settings.
// In a multiplayer session (w10) fast-forward, pause-on-focus-loss, cheats and continue-after-
// Shine are off.
#ifdef SMS_PORT_AURORA

#include "port_settings.h"
#include "port_replay.h"
#include "port_menu_ui.h" // the settings menu's rows (Gameplay page)

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

#include <GC2D/MessageUtil.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <Player/Mario.hpp>
#include <Player/WaterGun.hpp>
#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/StageUtil.hpp>

#define IMGUI_USER_CONFIG "aurora/imgui_config.h" // as Aurora builds imgui
#include <imgui.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" int port_mp_active(void);            // port_mp_net.cpp (w10)
extern "C" volatile unsigned gPortFrameCount;   // port_crash.cpp

namespace {

// Effective game-affecting state (16 bytes, recorded/replayed as E_QOL).
enum : uint8_t {
	Q_SKIP_INTRO     = 0x01,
	Q_SKIP_CUTSCENES = 0x02,
	Q_AUTOSAVE       = 0x04,
	Q_CONTINUE_SHINE = 0x08,
	Q_CAM_INV_X      = 0x10,
	Q_CAM_INV_Y      = 0x20,
	Q_LIVES          = 0x40,
	Q_WATER          = 0x80,
};
struct QolState {
	uint8_t flags;
	uint8_t textSpeed; // 0 normal, 1 fast, 2 instant
	uint8_t camSpeed;  // percent / 2
	uint8_t ffSpeed;   // 1 = off, 2, 4 (active this frame)
	uint8_t pad[12];
};
static_assert(sizeof(QolState) == 16, "E_QOL payload");
QolState sState = { 0, 0, 50, 1, {} };
bool sStateInit;

bool mpActive() { return port_mp_active() != 0; }

QolState liveState()
{
	const PortSettings& s = gPortSettings;
	QolState q            = {};
	const bool mp         = mpActive();
	q.flags |= s.qolSkipIntro ? Q_SKIP_INTRO : 0;
	q.flags |= s.qolSkipCutscenes ? Q_SKIP_CUTSCENES : 0;
	q.flags |= s.qolAutoSave && !mp ? Q_AUTOSAVE : 0;
	q.flags |= s.qolContinueShine && !mp ? Q_CONTINUE_SHINE : 0;
	q.flags |= s.qolCamInvertX ? Q_CAM_INV_X : 0;
	q.flags |= s.qolCamInvertY ? Q_CAM_INV_Y : 0;
	q.flags |= s.qolCheatLives && !mp ? Q_LIVES : 0;
	q.flags |= s.qolCheatWater && !mp ? Q_WATER : 0;
	q.textSpeed = (uint8_t)s.qolTextSpeed;
	q.camSpeed  = (uint8_t)(s.qolCamSpeed / 2);
	q.ffSpeed   = 1;
	if (s.qolFastForward && !mp && !port_menu_is_open()) {
		const bool* keys = SDL_GetKeyboardState(nullptr);
		SDL_Window* w    = aurora_get_window();
		const bool focus = w != nullptr && SDL_GetKeyboardFocus() == w;
		const int k      = s.qolFfKey;
		if (k > 0 && k < SDL_SCANCODE_COUNT && ((focus && keys[k]) || port_input_script_key(k)))
			q.ffSpeed = (uint8_t)(s.qolFfSpeed >= 4 ? 4 : 2);
	}
	// SMS_PORT_FF=2|4 (test knob, port_movetest.cpp's replays): fast-forward the whole run
	static const int sFfEnv = std::getenv("SMS_PORT_FF") ? std::atoi(std::getenv("SMS_PORT_FF")) : 0;
	if (sFfEnv >= 2 && !mp)
		q.ffSpeed = (uint8_t)(sFfEnv >= 4 ? 4 : 2);
	return q;
}

// ---- toasts ---------------------------------------------------------------------------------
char sToast[96];
unsigned sToastUntil;

// ---- trackers -------------------------------------------------------------------------------
const char* message(const char* bmgPath, u32 index, char* buf, size_t n)
{
	buf[0] = '\0';
	void* bmg = JKRFileLoader::getGlbResource(bmgPath);
	if (bmg == nullptr)
		return buf;
	const char* m = SMSGetMessageData(bmg, index);
	if (m == nullptr)
		return buf;
	size_t o = 0;
	for (size_t i = 0; m[i] != '\0' && o + 1 < n; i++) {
		const unsigned char c = (unsigned char)m[i];
		if (c == 0x1A) { // BMG control sequence: 0x1A, length, ...
			const unsigned char len = (unsigned char)m[i + 1];
			if (len < 2)
				break;
			i += len - 1;
			continue;
		}
		if (c == '\n' || c >= 0x20)
			buf[o++] = c == '\n' ? ' ' : (char)c;
	}
	buf[o] = '\0';
	return buf;
}

// Blue coins per guide index (TGuide's table: area of the blue-coin flags) and their totals
// (the game stores only which coins were found; the totals are the game's coin counts).
const u8 kBlueArea[10]  = { 0, 1, 2, 3, 4, 0xd, 6, 8, 9, 0xa };
const int kBlueTotal[10] = { 1, 19, 30, 30, 30, 30, 30, 30, 30, 10 };

int blueCoins(int idx)
{
	int n = 0;
	for (u8 c = 0; c < 50; c++)
		if (TFlagManager::getInstance()->getBlueCoinFlag(kBlueArea[idx], c))
			n++;
	return n;
}

bool inStage()
{
	TApplication* app = &gpApplication;
	return gpMarDirector != nullptr && app->mDirector == (JDrama::TDirector*)gpMarDirector && gpMarDirector->mSetupDone;
}

bool sTrackerKeyOpen;

void drawTrackers(bool paused)
{
	const PortSettings& s = gPortSettings;
	const bool show = (paused && (s.qolBlueCoinTracker || s.qolShineChecklist)) || sTrackerKeyOpen;
	if (!show || !inStage() || TFlagManager::getInstance() == nullptr)
		return;
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 12.0f, 12.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.78f);
	if (ImGui::Begin("##qoltrackers", nullptr,
	                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs
	                     | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
		const float scale = io.DisplaySize.y / 600.0f;
		ImGui::SetWindowFontScale(scale < 1.0f ? 1.0f : scale > 2.5f ? 2.5f : scale);
		char name[96], ep[128];
		const int cur = SMS_getShineStage(gpMarDirector->mMap);
		if (s.qolBlueCoinTracker || sTrackerKeyOpen) {
			ImGui::TextColored(ImVec4(0.45f, 0.65f, 1.0f, 1.0f), "Blue coins");
			int all = 0;
			for (int i = 0; i < 10; i++) {
				const int n = blueCoins(i);
				all += n;
				message("/common/2d/stagename.bmg", (u32)i, name, sizeof(name));
				const ImVec4 col = n >= kBlueTotal[i] ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f)
				                 : i == cur          ? ImVec4(1.0f, 1.0f, 0.6f, 1.0f)
				                                     : ImVec4(0.9f, 0.9f, 0.9f, 1.0f);
				ImGui::TextColored(col, "%-22s %2d / %d", name[0] ? name : "?", n, kBlueTotal[i]);
			}
			ImGui::Text("%-22s %3d / 240", "Total", all);
		}
		if (s.qolShineChecklist || sTrackerKeyOpen) {
			if (s.qolBlueCoinTracker || sTrackerKeyOpen)
				ImGui::Separator();
			const int stage = cur >= 2 && cur <= 8 ? cur : -1;
			if (stage < 0) {
				ImGui::TextDisabled("Shines: %d collected", TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT));
			} else {
				message("/common/2d/stagename.bmg", (u32)stage, name, sizeof(name));
				ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Shines - %s", name);
				for (u32 e = 0; e < 8; e++) {
					const s16 id = SMS_getShineID((u32)stage, e, false);
					if (id < 0)
						continue;
					const bool got = TFlagManager::getInstance()->getShineFlag((u8)id);
					message("/common/2d/scenarioname.bmg", SMS_getNormalStage((u32)id), ep, sizeof(ep));
					ImGui::TextColored(got ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
					                   "%s %u. %s", got ? "[x]" : "[ ]", e + 1, ep);
				}
				int etc = 0, etcGot = 0;
				for (u32 e = 0; e < 3; e++) {
					if (SMS_getShineID((u32)stage, e, true) < 0)
						continue;
					etc++;
					etcGot += SMS_isGetShine((u32)stage, e, true) ? 1 : 0;
				}
				ImGui::Text("Extra Shines: %d / %d", etcGot, etc);
			}
		}
	}
	ImGui::End();
}

} // namespace

extern "C" {

// ---- per game frame (port_input_read, main thread) -------------------------------------------
void port_qol_frame(void)
{
	QolState live = liveState();
	uint8_t st[16];
	std::memcpy(st, &live, 16);
	port_replay_qol(st); // records it, or replaces it with the recorded state
	std::memcpy(&sState, st, 16);
	sStateInit = true;

	// cheats (game state writes, by request; recorded with the rest of the state)
	if ((sState.flags & (Q_LIVES | Q_WATER)) && inStage() && TFlagManager::getInstance() != nullptr) {
		if ((sState.flags & Q_LIVES) && TFlagManager::getInstance()->getFlag(MSF_LIFE_COUNT) < 3)
			TFlagManager::getInstance()->setFlag(MSF_LIFE_COUNT, 3);
		TMario* m = gpMarioOriginal;
		if ((sState.flags & Q_WATER) && m != nullptr && m->mWaterGun != nullptr
		    && m->checkFlag(MARIO_FLAG_HAS_FLUDD)) {
			TWaterGun* w = m->mWaterGun;
			const s32 mx = ((const TWaterGun*)w)->getMaxWater();
			if (w->mCurrentWater < mx)
				w->mCurrentWater = mx;
		}
	}
}

// Before the first game frame (boot logo): the start-up state, from the replay if one plays.
static void ensureState()
{
	if (sStateInit)
		return;
	QolState live = liveState();
	uint8_t st[16];
	std::memcpy(st, &live, 16);
	port_replay_qol_initial(st);
	std::memcpy(&sState, st, 16);
}

int port_qol_skip_intro(void)
{
	ensureState();
	return (sState.flags & Q_SKIP_INTRO) != 0;
}
int port_qol_skip_cutscenes(void) { ensureState(); return (sState.flags & Q_SKIP_CUTSCENES) != 0; }
int port_qol_text_speed(void) { ensureState(); return sState.textSpeed; }
int port_qol_autosave(void) { ensureState(); return (sState.flags & Q_AUTOSAVE) != 0; }
int port_qol_continue_shine(void) { ensureState(); return (sState.flags & Q_CONTINUE_SHINE) != 0; }
int port_qol_ff_speed(void) { return sStateInit ? sState.ffSpeed : 1; }

// TMarioGamePad::updateMeaning: the C-stick values the camera reads.
void port_qol_camera_stick(float* x, float* y)
{
	ensureState();
	if (sState.flags & Q_CAM_INV_X)
		*x = -*x;
	if (sState.flags & Q_CAM_INV_Y)
		*y = -*y;
	if (sState.camSpeed != 50) {
		const float k = sState.camSpeed / 50.0f;
		*x *= k;
		*y *= k;
	}
}

// Pause on focus loss (port_main.cpp VIWaitForRetrace, sdk_audio.cpp aiTick).
static volatile LONG sPaused;
int port_qol_should_pause(void)
{
	if (!gPortSettings.qolPauseOnFocusLoss || mpActive() || port_replay_active())
		return 0;
	// SMS_PORT_FOCUSSCRIPT=<frame>:<ms> (test knob): the window counts as focused, except
	// for <ms> milliseconds from game frame <frame> (unattended runs never have the focus).
	static const char* sFocusScript = std::getenv("SMS_PORT_FOCUSSCRIPT");
	if (sFocusScript != nullptr) {
		static unsigned sAt = (unsigned)std::strtoul(sFocusScript, nullptr, 10);
		static unsigned sMs = std::strchr(sFocusScript, ':') ? (unsigned)std::strtoul(std::strchr(sFocusScript, ':') + 1, nullptr, 10) : 0;
		static ULONGLONG sStart;
		if (sStart == 0 && gPortFrameCount >= sAt)
			sStart = GetTickCount64();
		return sStart != 0 && GetTickCount64() - sStart < sMs ? 1 : 0;
	}
	SDL_Window* w = aurora_get_window();
	if (w == nullptr)
		return 0;
	const Uint64 f = SDL_GetWindowFlags(w);
	return (f & SDL_WINDOW_INPUT_FOCUS) == 0 ? 1 : 0;
}
void port_qol_set_paused(int on)
{
	if (InterlockedExchange(&sPaused, on ? 1 : 0) != (on ? 1 : 0))
		std::fprintf(stderr, "[qol] %s (window focus)\n", on ? "paused" : "resumed");
}
int port_qol_audio_paused(void) { return sPaused != 0; }

// Volume of a DSP voice (port_audio_dsp.cpp via port_speaker.cpp's tags): sound id of the SE
// it plays, 0 for music (sequences, streams).
int port_qol_volumes_active(void)
{
	const PortSettings& s = gPortSettings;
	return s.qolVolMusic != 100 || s.qolVolSfx != 100 || s.qolVolVoice != 100;
}
float port_qol_voice_gain(u32 seId)
{
	const PortSettings& s = gPortSettings;
	if (seId == 0)
		return s.qolVolMusic / 100.0f;
	const u32 id = seId & 0xFFFF;
	if ((id & 0xF800) == 0x7800 || (id & 0xF800) == 0x8800) // Mario/Yoshi voices, NPC voices
		return s.qolVolVoice / 100.0f;
	return s.qolVolSfx / 100.0f;
}

void port_qol_toast(const char* text)
{
	std::snprintf(sToast, sizeof(sToast), "%s", text);
	sToastUntil = gPortFrameCount + 150;
	std::fprintf(stderr, "[qol] %s\n", text);
}

// From port_menu_draw (main thread, ImGui frame).
void port_qol_draw(void)
{
	const PortSettings& s = gPortSettings;
	static bool sKeyHeld;
	const bool* keys = SDL_GetKeyboardState(nullptr);
	const bool key   = s.qolTrackerKey > 0 && s.qolTrackerKey < SDL_SCANCODE_COUNT
	               && (keys[s.qolTrackerKey] || port_input_script_key(s.qolTrackerKey));
	if (key && !sKeyHeld)
		sTrackerKeyOpen = !sTrackerKeyOpen;
	sKeyHeld = key;
	const bool paused = inStage() && gpMarDirector->mState == TMarDirector::STATE_PAUSE_MENU;
	drawTrackers(paused);
	// badge and toast: text sized with the window (as the tracker window)
	const ImVec2 ds  = ImGui::GetIO().DisplaySize;
	const float sc   = ds.y / 600.0f < 1.0f ? 1.0f : ds.y / 600.0f > 2.5f ? 2.5f : ds.y / 600.0f;
	ImFont* font     = ImGui::GetFont();
	const float size = ImGui::GetFontSize() * sc;
	if (sStateInit && sState.ffSpeed > 1) {
		ImDrawList* dl = ImGui::GetForegroundDrawList();
		char t[32];
		std::snprintf(t, sizeof(t), ">> %dx", sState.ffSpeed);
		const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, t);
		const ImVec2 p(ds.x - sz.x - 16.0f * sc, ds.y - sz.y - 12.0f * sc);
		dl->AddRectFilled(ImVec2(p.x - 6 * sc, p.y - 3 * sc), ImVec2(p.x + sz.x + 6 * sc, p.y + sz.y + 3 * sc),
		                  IM_COL32(0, 0, 0, 150), 4 * sc);
		dl->AddText(font, size, p, IM_COL32(255, 230, 90, 255), t);
	}
	if (sToast[0] != '\0' && gPortFrameCount < sToastUntil) {
		ImDrawList* dl  = ImGui::GetForegroundDrawList();
		const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, sToast);
		const ImVec2 p((ds.x - sz.x) * 0.5f, ds.y * 0.82f);
		dl->AddRectFilled(ImVec2(p.x - 8 * sc, p.y - 4 * sc), ImVec2(p.x + sz.x + 8 * sc, p.y + sz.y + 4 * sc),
		                  IM_COL32(0, 0, 0, 170), 5 * sc);
		dl->AddText(font, size, p, IM_COL32(255, 255, 255, 255), sToast);
	}
}

bool gPortQolRebindFf; // the fast-forward key's Rebind waits for a key (port_menu_key_capture)
// The settings menu's "Quality of life" tab.
// The settings menu's Gameplay page (port_menu.cpp; rows from port_menu_ui.h). Volume is on the
// Audio page.
void port_qol_menu(void)
{
	using namespace pmui;
	PortSettings& s = gPortSettings;
	bool c          = false;
	pageTitle("Gameplay", "Conveniences on top of the original game. Everything here is off by default.");
	if (mpActive())
		warning("Online play: the settings that would make your game run differently from the other players' "
		        "are greyed out (and off) until you leave the session.");
	if (port_replay_active())
		warning("A replay plays: the recorded settings apply.");

	beginSection("Skipping");
	if (row("Skip boot logos and intro movie", "Straight to the title screen.") && toggle("##qsi", &s.qolSkipIntro))
		c = true;
	if (row("Start skips cutscenes", "Any cutscene or movie.") && toggle("##qsc", &s.qolSkipCutscenes))
		c = true;
	static const char* const kText[] = { "Normal", "Fast", "Instant" };
	if (row("Dialogue text speed") && combo("##qts", &s.qolTextSpeed, kText, 3))
		c = true;
	if (lockedRow("Auto-save after each Shine", "No save prompt.") && toggle("##qas", &s.qolAutoSave))
		c = true;
	endLocked();
	if (lockedRow("Stay in the level after a Shine", "The Shine is counted and saved; you keep playing instead of going "
	                                                 "back to Delfino Plaza. Secret courses, the airstrip, Corona Mountain "
	                                                 "and scripted Shines still return as usual.")
	    && toggle("##qcs", &s.qolContinueShine))
		c = true;
	endLocked();
	endSection();

	beginSection("Speed and focus");
	if (lockedRow("Fast-forward", "While the key below is held.") && toggle("##qff", &s.qolFastForward))
		c = true;
	endLocked();
	{
		lockedRow("Fast-forward key and speed", nullptr, "");
		const char* kn = s.qolFfKey > 0 ? SDL_GetScancodeName((SDL_Scancode)s.qolFfKey) : "";
		char label[64];
		std::snprintf(label, sizeof(label), "%s##qffkey", gPortQolRebindFf ? "Press a key..." : (kn && *kn ? kn : "(none)"));
		const float half = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
		if (button(label, gPortQolRebindFf, half))
			gPortQolRebindFf = true;
		ImGui::SameLine();
		int sp = s.qolFfSpeed >= 4 ? 1 : 0;
		static const char* const kSp[] = { "2x", "4x" };
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::Combo("##ffspeed", &sp, kSp, 2)) {
			s.qolFfSpeed = sp ? 4 : 2;
			c            = true;
		}
		endLocked();
	}
	if (gPortQolRebindFf) {
		int n             = 0;
		const bool* state = SDL_GetKeyboardState(&n);
		static bool armed;
		bool any = false;
		for (int k = 1; k < n; k++)
			if (state[k]) {
				any = true;
				if (armed && k != SDL_SCANCODE_RETURN && k != SDL_SCANCODE_SPACE) {
					if (k != SDL_SCANCODE_ESCAPE)
						s.qolFfKey = k;
					gPortQolRebindFf = false;
					armed            = false;
					c                = true;
					break;
				}
			}
		if (!any)
			armed = true;
	}
	if (lockedRow("Pause when the window loses focus", nullptr) && toggle("##qpf", &s.qolPauseOnFocusLoss))
		c = true;
	endLocked();
	endSection();

	beginSection("Camera");
	if (row("Free camera", "The camera stays where you turn it instead of swinging back behind Mario, looks much "
	                       "further up and down, and the mouse wheel zooms. L still puts it behind him.")
	    && toggle("##qfc", &s.qolFreeCamera))
		c = true;
	if (s.qolFreeCamera) {
		static const char* const kZoom[] = { "Close", "Normal", "Far", "Farthest" };
		int zi = s.qolFreeCamZoom <= 87 ? 0 : s.qolFreeCamZoom <= 112 ? 1 : s.qolFreeCamZoom <= 137 ? 2 : 3;
		if (row("Free camera distance") && combo("##qzoom", &zi, kZoom, 4)) {
			s.qolFreeCamZoom = 75 + zi * 25;
			c                = true;
		}
	}
	if (row("Invert horizontal") && toggle("##qix", &s.qolCamInvertX))
		c = true;
	if (row("Invert vertical") && toggle("##qiy", &s.qolCamInvertY))
		c = true;
	if (row("Camera speed") && slider("##qcsp", &s.qolCamSpeed, 50, 200, "%d%%"))
		c = true;
	endSection();

	beginSection("Trackers");
	const char* tk = s.qolTrackerKey > 0 ? SDL_GetScancodeName((SDL_Scancode)s.qolTrackerKey) : "none";
	char help[96];
	std::snprintf(help, sizeof(help), "%s shows both at any time.", tk && *tk ? tk : "No key");
	if (row("Blue coin tracker", "In the pause screen.") && toggle("##qbt", &s.qolBlueCoinTracker))
		c = true;
	if (row("Shine checklist", help) && toggle("##qsl", &s.qolShineChecklist))
		c = true;
	endSection();

	beginSection("Cheats");
	if (lockedRow("Infinite lives", nullptr) && toggle("##qil", &s.qolCheatLives))
		c = true;
	endLocked();
	if (lockedRow("Infinite FLUDD water", nullptr) && toggle("##qiw", &s.qolCheatWater))
		c = true;
	endLocked();
	endSection();
	if (c)
		port_settings_dirty();
}

// A key held in the game window (or pressed by a test's key script); any thread.
int port_key_held(int scancode)
{
	if (scancode <= 0 || scancode >= SDL_SCANCODE_COUNT)
		return 0;
	const bool* keys = SDL_GetKeyboardState(nullptr);
	SDL_Window* w    = aurora_get_window();
	return ((w != nullptr && SDL_GetKeyboardFocus() == w && keys[scancode]) || port_input_script_key(scancode)) ? 1 : 0;
}

} // extern "C"

#endif // SMS_PORT_AURORA
