// port_settings.ini next to sms_port.exe (or %SMS_PORT_SETTINGS%): window, input and
// debug settings of the Aurora build. See port_settings.h. (w9)
#ifdef SMS_PORT_AURORA

#include "port_settings.h"
#include "port_replay.h"
#include "port_bugreport.h" // w12
#include "port_audio.h" // w5: [audio] output

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_scancode.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

PortSettings gPortSettings;

namespace {

struct KeyName {
	const char* ini;
	SDL_Scancode def;
};
const KeyName kKeys[PORT_KEY_COUNT] = {
	{ "stick_up", SDL_SCANCODE_W },       { "stick_down", SDL_SCANCODE_S },
	{ "stick_left", SDL_SCANCODE_A },     { "stick_right", SDL_SCANCODE_D },
	{ "cstick_up", SDL_SCANCODE_UP },     { "cstick_down", SDL_SCANCODE_DOWN },
	{ "cstick_left", SDL_SCANCODE_LEFT }, { "cstick_right", SDL_SCANCODE_RIGHT },
	{ "a", SDL_SCANCODE_SPACE },          { "b", SDL_SCANCODE_E },
	{ "x", SDL_SCANCODE_Q },              { "y", SDL_SCANCODE_F },
	{ "z", SDL_SCANCODE_Z },              { "l", SDL_SCANCODE_C },
	{ "r", SDL_SCANCODE_R },              { "start", SDL_SCANCODE_RETURN },
	{ "dpad_up", SDL_SCANCODE_I },        { "dpad_down", SDL_SCANCODE_K },
	{ "dpad_left", SDL_SCANCODE_J },      { "dpad_right", SDL_SCANCODE_L },
	{ "soft", SDL_SCANCODE_LSHIFT },
};

const char* const kAspectNames[] = { "4:3", "fit", "stretch",
	                                 "16:9", "16:10", "21:9", "32:9", "window" }; // w6: widescreen
static_assert(sizeof(kAspectNames) / sizeof(kAspectNames[0]) == PORT_ASPECT_COUNT, "aspect names");
const char* const kTriggerNames[]  = { "off", "gamecube", "fludd" };
const char* const kLightbarNames[] = { "off", "fluid", "static" };
const char* const kRumbleNames[]   = { "off", "original", "hd" };
const char* const kSpeakerNames[]  = { "off", "fludd", "fludd_pickups" };
const char* const kMainMixNames[]  = { "keep", "duck", "remove" };
const char* const kAudioOutNames[] = { "stereo", "original", "5.1", "7.1" }; // w5: PORT_AUDIO_*
static_assert(sizeof(kTriggerNames) / sizeof(kTriggerNames[0]) == PORT_TRIGGERS_COUNT, "trigger names");
static_assert(sizeof(kLightbarNames) / sizeof(kLightbarNames[0]) == PORT_LIGHTBAR_COUNT, "lightbar names");

std::string sPath;
bool sLoaded;
int sActiveMsaa = 1; // w6

void setDefaults(PortSettings& s)
{
	s.disc[0]        = '\0';
	s.behaviour      = 0; // w13: match the disc
	s.pal60          = 1; // w13: 60 Hz
	s.palLanguage    = 0; // w13: English
	s.width          = 1280;
	s.height         = 960;
	s.hasPos         = false;
	s.x              = 0;
	s.y              = 0;
	s.fullscreen     = false;
	s.maximized      = false;
	s.aspect         = PORT_ASPECT_4_3;
	s.internalScale  = 0.0f;
	s.frameRate      = 30;
	s.hudPosition    = 0; // w11
	s.msaa           = 1;     // w6
	s.anisotropy     = 0;     // w6
	s.trilinear      = false; // w6
	s.fxaa           = false; // w6
	s.brightness     = 100;   // w6
	s.gamma          = 100;   // w6
	s.copyFilter     = true;  // w6
	s.vsync          = false; // w6
	s.backend        = PORT_BACKEND_D3D12; // w6
	s.gcLayout       = true;
	s.stickDeadzone  = 0;
	s.cstickDeadzone = 0;
	s.linearTriggers = true;
	s.rumble         = 50;
	s.keyboard       = true;
	s.softTilt       = 45;
	s.mouse            = true;
	s.mouseSensitivity = 40;
	s.mouseInvertY     = false;
	s.backgroundInput = false;
	s.adaptiveTriggers   = PORT_TRIGGERS_GAMECUBE;
	s.triggerStrength    = 60;
	s.lightbar           = PORT_LIGHTBAR_FLUID;
	s.lightbarBrightness = 100;
	s.lightbarColor[0]   = 51; // the FLUDD water colour as port_padfx.cpp shows it
	s.lightbarColor[1]   = 85;
	s.lightbarColor[2]   = 255;
	s.rumbleMode         = PORT_RUMBLE_ORIGINAL;
	s.hdFludd = s.hdMovement = s.hdWorld = s.hdDamage = true;
	s.speakerMode        = PORT_SPEAKER_OFF;
	s.mainMix            = PORT_MAINMIX_KEEP;
	s.speakerVolume      = 80;
	s.qolSkipIntro = s.qolSkipCutscenes = false;
	s.qolTextSpeed   = 0;
	s.qolFastForward = false;
	s.qolFfKey       = SDL_SCANCODE_TAB;
	s.qolFfSpeed     = 2;
	s.qolAutoSave = s.qolContinueShine = s.qolPauseOnFocusLoss = false;
	s.qolCamInvertX = s.qolCamInvertY = false;
	s.qolFreeCamera = false;
	s.qolFreeCamZoom = 100;
	s.qolCamSpeed = 100;
	s.qolVolMusic = s.qolVolSfx = s.qolVolVoice = 100;
	s.qolBlueCoinTracker = s.qolShineChecklist = false;
	s.qolTrackerKey = SDL_SCANCODE_F4;
	s.qolCheatLives = s.qolCheatWater = false;
	s.audioOutput        = 1; // w5: PORT_AUDIO_ORIGINAL, the game's own Sound option (matrix-encoded stereo)
	for (int i = 0; i < PORT_KEY_COUNT; i++)
		s.keys[i] = kKeys[i].def;
	s.overlay    = false;
	s.frameStats = false;
	s.record     = true;
	s.bugReport        = true; // w12
	s.bugReportSeconds = 20;
	s.bugReportWidth   = 640;
	s.bugReportHeight  = 480;
	s.bugReportFps     = 30;
	s.discordEnabled   = true; // port_discord.cpp
	s.discordClientId[0] = '\0';
	s.discordShowStage = true;
	s.discordAllowJoin = true;
}

std::string exeDir()
{
	char buf[MAX_PATH];
	DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
	std::string p(buf, n);
	size_t slash = p.find_last_of("\\/");
	return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}

std::string trim(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos)
		return std::string();
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

bool parseBool(const std::string& v, bool def)
{
	if (v == "1" || _stricmp(v.c_str(), "true") == 0 || _stricmp(v.c_str(), "yes") == 0
	    || _stricmp(v.c_str(), "on") == 0)
		return true;
	if (v == "0" || _stricmp(v.c_str(), "false") == 0 || _stricmp(v.c_str(), "no") == 0
	    || _stricmp(v.c_str(), "off") == 0)
		return false;
	return def;
}

int parseInt(const std::string& v, int def, int lo, int hi)
{
	char* end;
	long n = std::strtol(v.c_str(), &end, 10);
	if (end == v.c_str())
		return def;
	return n < lo ? lo : n > hi ? hi : (int)n;
}

void apply(PortSettings& s, const std::string& section, const std::string& key, const std::string& v)
{
	if (section == "game") {
		if (key == "disc")
			std::snprintf(s.disc, sizeof(s.disc), "%s", v.c_str());
		else if (key == "behaviour") { // w13 (BOOTLOG "Regional behaviour (w13)")
			static const char* const names[] = { "disc", "us", "japan", "europe" };
			s.behaviour = parseInt(v, s.behaviour, 0, 3);
			for (int i = 0; i < 4; i++)
				if (_stricmp(v.c_str(), names[i]) == 0)
					s.behaviour = i;
		} else if (key == "pal_video") { // w13
			static const char* const names[] = { "50", "60", "ask" };
			s.pal60 = parseInt(v, s.pal60, 0, 2);
			for (int i = 0; i < 3; i++)
				if (_stricmp(v.c_str(), names[i]) == 0)
					s.pal60 = i;
		} else if (key == "pal_language") // w13
			s.palLanguage = parseInt(v, s.palLanguage, 0, 4);
	} else if (section == "window") {
		if (key == "width")
			s.width = parseInt(v, s.width, 640, 16384);
		else if (key == "height")
			s.height = parseInt(v, s.height, 480, 16384);
		else if (key == "x" || key == "y") {
			if (v.empty() || v == "center")
				s.hasPos = false;
			else {
				(key == "x" ? s.x : s.y) = parseInt(v, 0, -100000, 100000);
				s.hasPos = true;
			}
		} else if (key == "fullscreen")
			s.fullscreen = parseBool(v, s.fullscreen);
		else if (key == "maximized")
			s.maximized = parseBool(v, s.maximized);
		else if (key == "aspect") {
			for (int i = 0; i < PORT_ASPECT_COUNT; i++) // w6: was 3
				if (_stricmp(v.c_str(), kAspectNames[i]) == 0)
					s.aspect = i;
		} else if (key == "hud_position") { // w11
			s.hudPosition = _stricmp(v.c_str(), "16:9") == 0 ? 1 : _stricmp(v.c_str(), "match") == 0 ? 2 : 0;
		} else if (key == "internal_scale") {
			float f = (float)std::atof(v.c_str());
			s.internalScale = f < 0.0f ? 0.0f : f > 8.0f ? 8.0f : f;
		} else if (key == "frame_rate") {
			if (_stricmp(v.c_str(), "display") == 0 || _stricmp(v.c_str(), "match") == 0)
				s.frameRate = PORT_FPS_DISPLAY;
			else if (_stricmp(v.c_str(), "unlimited") == 0)
				s.frameRate = PORT_FPS_UNLIMITED;
			else
				s.frameRate = parseInt(v, s.frameRate, 30, 1000);
		}
	} else if (section == "graphics") { // w6
		if (key == "msaa")
			s.msaa = parseInt(v, s.msaa, 0, 16) > 1 ? 4 : 1; // WebGPU render targets: 1 or 4 samples
		else if (key == "anisotropy") {
			int a = parseInt(v, s.anisotropy, 0, 16);
			s.anisotropy = a >= 16 ? 16 : a >= 8 ? 8 : a >= 4 ? 4 : a >= 2 ? 2 : a; // 0, 1, 2, 4, 8, 16
		} else if (key == "trilinear")
			s.trilinear = parseBool(v, s.trilinear);
		else if (key == "fxaa")
			s.fxaa = parseBool(v, s.fxaa);
		else if (key == "brightness")
			s.brightness = parseInt(v, s.brightness, 50, 150);
		else if (key == "gamma")
			s.gamma = parseInt(v, s.gamma, 50, 200);
		else if (key == "copy_filter")
			s.copyFilter = parseBool(v, s.copyFilter);
		else if (key == "vsync")
			s.vsync = parseBool(v, s.vsync);
		else if (key == "backend") { // w6
			const int b = port_gfx_backend_parse(v.c_str());
			if (b >= 0)
				s.backend = b;
		}
	} else if (section == "input") {
		if (key == "layout")
			s.gcLayout = _stricmp(v.c_str(), "aurora") != 0;
		else if (key == "stick_deadzone")
			s.stickDeadzone = parseInt(v, s.stickDeadzone, 0, 90);
		else if (key == "cstick_deadzone")
			s.cstickDeadzone = parseInt(v, s.cstickDeadzone, 0, 90);
		else if (key == "linear_triggers")
			s.linearTriggers = parseBool(v, s.linearTriggers);
		else if (key == "rumble")
			s.rumble = parseInt(v, s.rumble, 0, 100);
		else if (key == "keyboard")
			s.keyboard = parseBool(v, s.keyboard);
		else if (key == "soft_tilt")
			s.softTilt = parseInt(v, s.softTilt, 1, 100);
		else if (key == "mouse")
			s.mouse = parseBool(v, s.mouse);
		else if (key == "mouse_sensitivity")
			s.mouseSensitivity = parseInt(v, s.mouseSensitivity, 1, 100);
		else if (key == "mouse_invert_y")
			s.mouseInvertY = parseBool(v, s.mouseInvertY);
		else if (key == "background_input")
			s.backgroundInput = parseBool(v, s.backgroundInput);
		else if (key == "adaptive_triggers") {
			for (int i = 0; i < PORT_TRIGGERS_COUNT; i++)
				if (_stricmp(v.c_str(), kTriggerNames[i]) == 0)
					s.adaptiveTriggers = i;
		} else if (key == "trigger_strength")
			s.triggerStrength = parseInt(v, s.triggerStrength, 0, 100);
		else if (key == "lightbar") {
			for (int i = 0; i < PORT_LIGHTBAR_COUNT; i++)
				if (_stricmp(v.c_str(), kLightbarNames[i]) == 0)
					s.lightbar = i;
		} else if (key == "lightbar_brightness")
			s.lightbarBrightness = parseInt(v, s.lightbarBrightness, 0, 100);
		else if (key == "rumble_mode") {
			for (int i = 0; i < PORT_RUMBLE_COUNT; i++)
				if (_stricmp(v.c_str(), kRumbleNames[i]) == 0)
					s.rumbleMode = i;
		} else if (key == "hd_fludd")
			s.hdFludd = parseBool(v, s.hdFludd);
		else if (key == "hd_movement")
			s.hdMovement = parseBool(v, s.hdMovement);
		else if (key == "hd_world")
			s.hdWorld = parseBool(v, s.hdWorld);
		else if (key == "hd_damage")
			s.hdDamage = parseBool(v, s.hdDamage);
		else if (key == "lightbar_color") {
			int r, g, b;
			if (std::sscanf(v.c_str(), "%d , %d , %d", &r, &g, &b) == 3)
				for (int i = 0; i < 3; i++) {
					const int c        = i == 0 ? r : i == 1 ? g : b;
					s.lightbarColor[i] = (unsigned char)(c < 0 ? 0 : c > 255 ? 255 : c);
				}
		}
	} else if (section == "audio") {
		if (key == "output") { // w5
			for (int i = 0; i < 4; i++)
				if (_stricmp(v.c_str(), kAudioOutNames[i]) == 0)
					s.audioOutput = i;
		} else if (key == "controller_speaker") {
			for (int i = 0; i < PORT_SPEAKER_COUNT; i++)
				if (_stricmp(v.c_str(), kSpeakerNames[i]) == 0)
					s.speakerMode = i;
		} else if (key == "main_mix") {
			for (int i = 0; i < PORT_MAINMIX_COUNT; i++)
				if (_stricmp(v.c_str(), kMainMixNames[i]) == 0)
					s.mainMix = i;
		} else if (key == "speaker_volume")
			s.speakerVolume = parseInt(v, s.speakerVolume, 0, 100);
	} else if (section == "keyboard") {
		for (int i = 0; i < PORT_KEY_COUNT; i++)
			if (key == kKeys[i].ini) {
				SDL_Scancode sc = SDL_GetScancodeFromName(v.c_str());
				if (v.empty() || _stricmp(v.c_str(), "none") == 0)
					s.keys[i] = SDL_SCANCODE_UNKNOWN;
				else if (sc != SDL_SCANCODE_UNKNOWN)
					s.keys[i] = sc;
				else
					std::fprintf(stderr, "[port] settings: unknown key name '%s' for %s\n", v.c_str(),
					             kKeys[i].ini);
			}
	} else if (section == "qol") {
		auto key2 = [&](int def) {
			SDL_Scancode sc = SDL_GetScancodeFromName(v.c_str());
			return v.empty() || _stricmp(v.c_str(), "none") == 0 ? 0 : sc != SDL_SCANCODE_UNKNOWN ? (int)sc : def;
		};
		if (key == "skip_intro")
			s.qolSkipIntro = parseBool(v, s.qolSkipIntro);
		else if (key == "skip_cutscenes")
			s.qolSkipCutscenes = parseBool(v, s.qolSkipCutscenes);
		else if (key == "text_speed")
			s.qolTextSpeed = _stricmp(v.c_str(), "instant") == 0 ? 2 : _stricmp(v.c_str(), "fast") == 0 ? 1 : 0;
		else if (key == "fast_forward")
			s.qolFastForward = parseBool(v, s.qolFastForward);
		else if (key == "fast_forward_key")
			s.qolFfKey = key2(s.qolFfKey);
		else if (key == "fast_forward_speed")
			s.qolFfSpeed = parseInt(v, s.qolFfSpeed, 2, 4) >= 4 ? 4 : 2;
		else if (key == "auto_save")
			s.qolAutoSave = parseBool(v, s.qolAutoSave);
		else if (key == "continue_after_shine")
			s.qolContinueShine = parseBool(v, s.qolContinueShine);
		else if (key == "pause_on_focus_loss")
			s.qolPauseOnFocusLoss = parseBool(v, s.qolPauseOnFocusLoss);
		else if (key == "camera_invert_x")
			s.qolCamInvertX = parseBool(v, s.qolCamInvertX);
		else if (key == "free_camera")
			s.qolFreeCamera = parseBool(v, s.qolFreeCamera);
		else if (key == "free_camera_zoom")
			s.qolFreeCamZoom = parseInt(v, s.qolFreeCamZoom, 75, 150);
		else if (key == "camera_invert_y")
			s.qolCamInvertY = parseBool(v, s.qolCamInvertY);
		else if (key == "camera_speed")
			s.qolCamSpeed = parseInt(v, s.qolCamSpeed, 50, 200);
		else if (key == "volume_music")
			s.qolVolMusic = parseInt(v, s.qolVolMusic, 0, 100);
		else if (key == "volume_sfx")
			s.qolVolSfx = parseInt(v, s.qolVolSfx, 0, 100);
		else if (key == "volume_voices")
			s.qolVolVoice = parseInt(v, s.qolVolVoice, 0, 100);
		else if (key == "blue_coin_tracker")
			s.qolBlueCoinTracker = parseBool(v, s.qolBlueCoinTracker);
		else if (key == "shine_checklist")
			s.qolShineChecklist = parseBool(v, s.qolShineChecklist);
		else if (key == "tracker_key")
			s.qolTrackerKey = key2(s.qolTrackerKey);
		else if (key == "infinite_lives")
			s.qolCheatLives = parseBool(v, s.qolCheatLives);
		else if (key == "infinite_water")
			s.qolCheatWater = parseBool(v, s.qolCheatWater);
	} else if (section == "debug") {
		if (key == "overlay")
			s.overlay = parseBool(v, s.overlay);
		else if (key == "frame_stats")
			s.frameStats = parseBool(v, s.frameStats);
		else if (key == "record")
			s.record = parseBool(v, s.record);
	} else if (section == "bugreport") { // w12
		if (key == "enabled")
			s.bugReport = parseBool(v, s.bugReport);
		else if (key == "seconds")
			s.bugReportSeconds = parseInt(v, s.bugReportSeconds, 5, 60);
		else if (key == "video_width")
			s.bugReportWidth = parseInt(v, s.bugReportWidth, 160, 1920) & ~1;
		else if (key == "video_height")
			s.bugReportHeight = parseInt(v, s.bugReportHeight, 0, 1080) & ~1;
		else if (key == "fps")
			s.bugReportFps = parseInt(v, s.bugReportFps, 10, 60);
	} else if (section == "discord") {
		if (key == "enabled")
			s.discordEnabled = parseBool(v, s.discordEnabled);
		else if (key == "client_id") {
			std::string d;
			for (char c : v)
				if (c >= '0' && c <= '9' && d.size() < sizeof(s.discordClientId) - 1)
					d += c;
			std::snprintf(s.discordClientId, sizeof(s.discordClientId), "%s", d.c_str());
		} else if (key == "show_stage")
			s.discordShowStage = parseBool(v, s.discordShowStage);
		else if (key == "allow_join")
			s.discordAllowJoin = parseBool(v, s.discordAllowJoin);
	}
}

bool load(PortSettings& s, const std::string& path)
{
	FILE* f = std::fopen(path.c_str(), "rb");
	if (f == nullptr)
		return false;
	std::string section;
	char line[512];
	while (std::fgets(line, sizeof(line), f) != nullptr) {
		std::string l = trim(line);
		if (l.empty() || l[0] == ';' || l[0] == '#')
			continue;
		if (l[0] == '[') {
			size_t e = l.find(']');
			section  = trim(l.substr(1, e == std::string::npos ? std::string::npos : e - 1));
			continue;
		}
		size_t eq = l.find('=');
		if (eq == std::string::npos)
			continue;
		std::string v = trim(l.substr(eq + 1));
		size_t c      = v.find(" ;"); // trailing comment
		if (c != std::string::npos)
			v = trim(v.substr(0, c));
		apply(s, section, trim(l.substr(0, eq)), v);
	}
	std::fclose(f);
	return true;
}

std::string keyName(int sc)
{
	if (sc <= SDL_SCANCODE_UNKNOWN || sc >= SDL_SCANCODE_COUNT)
		return "none";
	const char* n = SDL_GetScancodeName((SDL_Scancode)sc);
	return n != nullptr && *n != '\0' ? n : "none";
}

} // namespace

extern "C" {

const char* port_settings_key_id(int key) { return key >= 0 && key < PORT_KEY_COUNT ? kKeys[key].ini : ""; }

int port_settings_active_msaa(void) { return sActiveMsaa; } // w6

void port_settings_save(void)
{
	if (!sLoaded || port_replay_active()) // a replay never changes the player's settings
		return;
	const PortSettings& s = gPortSettings;
	std::string tmp       = sPath + ".tmp";
	FILE* f               = std::fopen(tmp.c_str(), "wb");
	if (f == nullptr) {
		std::fprintf(stderr, "[port] settings: cannot write %s\n", tmp.c_str());
		return;
	}
	std::fprintf(f,
	             "; Super Mario Sunshine PC port settings. The game rewrites this file when the window\n"
	             "; changes; edit it while the game is closed. Delete it to get the defaults back.\n\n");
	std::fprintf(f, "[game]\n");
	std::fprintf(f, "; The disc image (.iso, .gcm, .ciso, .rvz, ...) or extracted disc folder: US GMSE01 or\n"
	                "; Japan GMSJ01. Empty: look next to sms_port.exe (and ask if nothing is found).\n");
	std::fprintf(f, "disc = %s\n", s.disc);
	// w13 (BOOTLOG "Regional behaviour (w13)")
	static const char* const kBehaviourNames[] = { "disc", "us", "japan", "europe" };
	static const char* const kPalVideoNames[]  = { "50", "60", "ask" };
	std::fprintf(f, "; Game behaviour: disc = as the disc's own release (default), or us / japan / europe:\n"
	                "; that release's game-code differences on any disc (text, menus and other disc data\n"
	                "; stay the disc's). europe is experimental. Takes effect at the next start.\n");
	std::fprintf(f, "behaviour = %s\n", kBehaviourNames[s.behaviour < 0 || s.behaviour > 3 ? 0 : s.behaviour]);
	std::fprintf(f, "; European disc only: picture 60 (60 Hz), 50 (50 Hz) or ask (the game asks at boot),\n"
	                "; and the language (0 English, 1 German, 2 French, 3 Spanish, 4 Italian).\n");
	std::fprintf(f, "pal_video = %s\npal_language = %d\n\n", kPalVideoNames[s.pal60 < 0 || s.pal60 > 2 ? 1 : s.pal60],
	             s.palLanguage);
	std::fprintf(f, "[window]\n");
	std::fprintf(f, "; Windowed size and position (x/y = center: centred on the main display).\n");
	std::fprintf(f, "width = %d\nheight = %d\n", s.width, s.height);
	if (s.hasPos)
		std::fprintf(f, "x = %d\ny = %d\n", s.x, s.y);
	else
		std::fprintf(f, "x = center\ny = center\n");
	std::fprintf(f, "; Alt+Enter or F11 toggles fullscreen.\nfullscreen = %d\nmaximized = %d\n",
	             s.fullscreen ? 1 : 0, s.maximized ? 1 : 0);
	std::fprintf(f, "; 4:3 = window kept at 4:3, black bars when it cannot be (fullscreen, maximized);\n"
	                "; fit = always letterboxed to the 640x448 frame; stretch = fill the window.\n"
	                "; 16:9, 16:10, 21:9, 32:9 = true widescreen (wider view, not stretched);\n"
	                "; window = true widescreen in the window's shape.\n"); // w6
	std::fprintf(f, "aspect = %s\n", kAspectNames[s.aspect]);
	std::fprintf(f, "; Internal resolution: 0 = window resolution, 1 = native 640x448, 2 = 2x, ...\n");
	std::fprintf(f, "internal_scale = %g\n", s.internalScale);
	std::fprintf(f, "; Widescreen HUD: centered = 4:3 in the middle (original), 16:9 = at the edges of a\n"
	                "; 16:9 area, match = at the screen edges (follows the aspect)\n"); // w11
	std::fprintf(f, "hud_position = %s\n",
	             s.hudPosition == 1 ? "16:9" : s.hudPosition == 2 ? "match" : "centered"); // w11
	std::fprintf(f, "; Display frame rate: 30 (original), 60, 120, 144, display (the monitor's refresh\n"
	                "; rate) or unlimited. The game itself always runs at 30 Hz; above 30, frames in\n"
	                "; between are drawn with interpolated camera and models.\n");
	if (s.frameRate == PORT_FPS_DISPLAY)
		std::fprintf(f, "frame_rate = display\n\n");
	else if (s.frameRate == PORT_FPS_UNLIMITED)
		std::fprintf(f, "frame_rate = unlimited\n\n");
	else
		std::fprintf(f, "frame_rate = %d\n\n", s.frameRate);

	// w6: optional enhancements; the defaults render like the GameCube.
	std::fprintf(f, "[graphics]\n");
	std::fprintf(f, "; Multisample anti-aliasing: 1 = off (GameCube), 4 = 4x. Takes effect at the next start.\n");
	std::fprintf(f, "msaa = %d\n", s.msaa);
	std::fprintf(f, "; Anisotropic filtering: 0 = as the game asks per texture (GameCube), 1/2/4/8/16 = that\n"
	                "; maximum for every smoothly filtered, mipmapped texture.\n");
	std::fprintf(f, "anisotropy = %d\n", s.anisotropy);
	std::fprintf(f, "; Smooth (trilinear) blending between mipmap levels for every mipmapped texture.\n");
	std::fprintf(f, "trilinear = %d\n", s.trilinear ? 1 : 0);
	std::fprintf(f, "; Post-process anti-aliasing (FXAA): cheap, slightly soft. Works with or without msaa.\n");
	std::fprintf(f, "fxaa = %d\n", s.fxaa ? 1 : 0);
	std::fprintf(f, "; Brightness and gamma in percent (100 = original).\n");
	std::fprintf(f, "brightness = %d\ngamma = %d\n", s.brightness, s.gamma);
	std::fprintf(f, "; 0 = skip the game's vertical copy filter on screen effects (sharper, not original).\n");
	std::fprintf(f, "copy_filter = %d\n", s.copyFilter ? 1 : 0);
	std::fprintf(f, "; Present with vsync (no tearing; frames still paced to the GameCube's 59.94 Hz).\n"
	                "; With [window] frame_rate above 30, vsync caps it at the monitor's refresh rate.\n");
	std::fprintf(f, "vsync = %d\n", s.vsync ? 1 : 0);
	std::fprintf(f, "; Graphics API: d3d12 (default), vulkan or d3d11. Takes effect at the next start. If it\n"
	                "; cannot start, the game falls back to another one by itself.\n");
	std::fprintf(f, "backend = %s\n\n", port_gfx_backend_ini(s.backend)); // w6

	std::fprintf(f, "[input]\n");
	std::fprintf(f, "; gamecube = buttons where a GameCube pad has them (PlayStation: Cross A, Square B,\n"
	                "; Circle X, Triangle Y; Xbox: A A, X B, B X, Y Y); aurora = Aurora's defaults.\n");
	std::fprintf(f, "layout = %s\n", s.gcLayout ? "gamecube" : "aurora");
	std::fprintf(f, "; Extra stick dead zones in percent. 0 = only the GameCube's own (PADClamp).\n");
	std::fprintf(f, "stick_deadzone = %d\ncstick_deadzone = %d\n", s.stickDeadzone, s.cstickDeadzone);
	std::fprintf(f, "; Analog L/R rise linearly up to the click point (full press = digital L/R).\n");
	std::fprintf(f, "linear_triggers = %d\n", s.linearTriggers ? 1 : 0);
	std::fprintf(f, "; Rumble strength in percent.\nrumble = %d\n", s.rumble);
	std::fprintf(f, "; Keyboard drives controller 1 too (see [keyboard]).\nkeyboard = %d\n",
	             s.keyboard ? 1 : 0);
	std::fprintf(f, "; Stick tilt in percent while the soft key is held (walk); it also makes L/R a light press.\n");
	std::fprintf(f, "soft_tilt = %d\n", s.softTilt);
	std::fprintf(f, "; Mouse (with the keyboard), in a level: turns the camera; left click sprays while walking\n"
	                "; (light R), right click is L, middle click sprays at full power (full R), side button Y,\n"
	                "; the wheel moves the camera up / down. Esc or any menu frees it.\n");
	std::fprintf(f, "mouse = %d\nmouse_sensitivity = %d\nmouse_invert_y = %d\n", s.mouse ? 1 : 0, s.mouseSensitivity,
	             s.mouseInvertY ? 1 : 0);
	std::fprintf(f, "; Read the controller while the window is in the background.\n");
	std::fprintf(f, "background_input = %d\n", s.backgroundInput ? 1 : 0);
	std::fprintf(f, "; DualSense adaptive triggers: off; gamecube = light resistance and a firm wall at the\n"
	                "; click (full press = digital L/R); fludd = that, plus FLUDD feedback on R (slack when the\n"
	                "; tank is empty, a pulse while spraying, hover/turbo/rocket patterns).\n");
	std::fprintf(f, "adaptive_triggers = %s\n", kTriggerNames[s.adaptiveTriggers]);
	std::fprintf(f, "; Strength of the trigger effects in percent.\ntrigger_strength = %d\n", s.triggerStrength);
	std::fprintf(f, "; DualSense / DualShock 4 lightbar: off (SDL's player colour); fluid = the colour of the\n"
	                "; water in FLUDD or of Yoshi's juice, darker as it drains; static = lightbar_color.\n");
	std::fprintf(f, "lightbar = %s\n", kLightbarNames[s.lightbar]);
	std::fprintf(f, "lightbar_brightness = %d\n", s.lightbarBrightness);
	std::fprintf(f, "lightbar_color = %d,%d,%d\n", s.lightbarColor[0], s.lightbarColor[1], s.lightbarColor[2]);
	std::fprintf(f, "; Rumble: off; original = the game's own motor pulses; hd = designed waveforms for the\n"
	                "; game's rumble types plus effects for FLUDD, footsteps, landings, pickups (DualSense: HD\n"
	                "; haptics over USB or Bluetooth; Xbox: impulse triggers for FLUDD). Strength: rumble.\n");
	std::fprintf(f, "rumble_mode = %s\n", kRumbleNames[s.rumbleMode]);
	std::fprintf(f, "; HD rumble categories.\nhd_fludd = %d\nhd_movement = %d\nhd_world = %d\nhd_damage = %d\n\n",
	             s.hdFludd ? 1 : 0, s.hdMovement ? 1 : 0, s.hdWorld ? 1 : 0, s.hdDamage ? 1 : 0);
	std::fprintf(f, "[audio]\n"); // w5: output
	std::fprintf(f, "; original = the game's Sound option decides (mono / stereo / surround = Pro Logic II matrix-encoded\n"
	                "; stereo, as on the GameCube); stereo = always plain stereo; 5.1 / 7.1 = discrete channels (the\n"
	                "; game mixes in surround; the saved Sound option is not changed; stereo if the device has fewer\n"
	                "; channels).\noutput = %s\n",
	             kAudioOutNames[s.audioOutput < 0 || s.audioOutput > 3 ? 1 : s.audioOutput]);
	std::fprintf(f, "; DualSense speaker: off, fludd (FLUDD's sounds), fludd_pickups (also coins, 1-ups, Shines).\n");
	std::fprintf(f, "controller_speaker = %s\n", kSpeakerNames[s.speakerMode]);
	std::fprintf(f, "; Those sounds on the PC speakers meanwhile: keep, duck or remove.\nmain_mix = %s\n", kMainMixNames[s.mainMix]);
	std::fprintf(f, "speaker_volume = %d\n\n", s.speakerVolume);
	std::fprintf(f, "[keyboard]\n; SDL key names (\"Left Shift\", \"Space\", \"Keypad 8\", ...), or none.\n");
	for (int i = 0; i < PORT_KEY_COUNT; i++)
		std::fprintf(f, "%s = %s\n", kKeys[i].ini, keyName(s.keys[i]).c_str());
	std::fprintf(f, "[qol]\n; Quality-of-life options, all off by default (0 = the original game).\n");
	std::fprintf(f, "skip_intro = %d\nskip_cutscenes = %d\n", s.qolSkipIntro ? 1 : 0, s.qolSkipCutscenes ? 1 : 0);
	std::fprintf(f, "; normal, fast or instant\ntext_speed = %s\n", s.qolTextSpeed == 2 ? "instant" : s.qolTextSpeed == 1 ? "fast" : "normal");
	std::fprintf(f, "; hold fast_forward_key for 2x or 4x game speed (not in multiplayer)\nfast_forward = %d\n", s.qolFastForward ? 1 : 0);
	std::fprintf(f, "fast_forward_key = %s\nfast_forward_speed = %d\n", keyName(s.qolFfKey).c_str(), s.qolFfSpeed);
	std::fprintf(f, "auto_save = %d\ncontinue_after_shine = %d\npause_on_focus_loss = %d\n", s.qolAutoSave ? 1 : 0,
	             s.qolContinueShine ? 1 : 0, s.qolPauseOnFocusLoss ? 1 : 0);
	std::fprintf(f, "; Free camera: the camera stays where you turn it (it never swings behind Mario by itself).\n"
	                "free_camera = %d\n", s.qolFreeCamera ? 1 : 0);
	std::fprintf(f, "; Free camera distance in percent: 75, 100, 125 or 150 (the mouse wheel steps through them).\n"
	                "free_camera_zoom = %d\n", s.qolFreeCamZoom);
	std::fprintf(f, "camera_invert_x = %d\ncamera_invert_y = %d\n; percent\ncamera_speed = %d\n", s.qolCamInvertX ? 1 : 0,
	             s.qolCamInvertY ? 1 : 0, s.qolCamSpeed);
	std::fprintf(f, "; percent\nvolume_music = %d\nvolume_sfx = %d\nvolume_voices = %d\n", s.qolVolMusic, s.qolVolSfx,
	             s.qolVolVoice);
	std::fprintf(f, "blue_coin_tracker = %d\nshine_checklist = %d\ntracker_key = %s\n", s.qolBlueCoinTracker ? 1 : 0,
	             s.qolShineChecklist ? 1 : 0, keyName(s.qolTrackerKey).c_str());
	std::fprintf(f, "infinite_lives = %d\ninfinite_water = %d\n", s.qolCheatLives ? 1 : 0, s.qolCheatWater ? 1 : 0);
	std::fprintf(f, "\n[debug]\n; F3 toggles the frame-rate overlay.\noverlay = %d\n", s.overlay ? 1 : 0);	std::fprintf(f, "; Print frame-pacing statistics to the console every 10 seconds.\nframe_stats = %d\n",
	             s.frameStats ? 1 : 0);
	std::fprintf(f, "; Record the controller input from boot, so F9 (or a crash) can save a replay\n"
	                "; (sms_replay_*.bin next to sms_port.exe) that reproduces the session for a bug report.\n");
	std::fprintf(f, "record = %d\n", s.record ? 1 : 0);
	std::fprintf(f, "\n[discord]\n; Discord Rich Presence and invites (no SDK; talks to the running Discord app).\n"
	                "; client_id: leave empty to use the game's own Discord app (normal). Only set it to use a different Application ID.\n"
	                "enabled = %d\nclient_id = %s\n; show the level name and shine count; 0 = only \"Playing\"\n"
	                "show_stage = %d\n; publish a join secret in online lobbies and accept Join / ask-to-join\n"
	                "allow_join = %d\n", s.discordEnabled ? 1 : 0, s.discordClientId, s.discordShowStage ? 1 : 0,
	             s.discordAllowJoin ? 1 : 0);
	bool ok = std::fclose(f) == 0;
	if (!ok || !MoveFileExA(tmp.c_str(), sPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
		std::fprintf(stderr, "[port] settings: cannot replace %s\n", sPath.c_str());
		DeleteFileA(tmp.c_str());
	}
}

void port_settings_configure(AuroraConfig* config)
{
	PortSettings& s = gPortSettings;
	setDefaults(s);
	const char* env = std::getenv("SMS_PORT_SETTINGS");
	sPath           = env != nullptr && *env != '\0' ? std::string(env) : exeDir() + "\\port_settings.ini";
	bool existed    = load(s, sPath);
	sLoaded         = true;
	std::fprintf(stderr, "[port] settings: %s %s\n", existed ? "loaded" : "created", sPath.c_str());
	if (!existed)
		port_settings_save();
	if (std::getenv("SMS_PORT_FRAMESTATS") != nullptr)
		s.frameStats = true;
	{
		// w5: SMS_PORT_AUDIO_OUTPUT=stereo|original|5.1|7.1 overrides [audio] output for this run (not saved).
		PortSettings t = s;
		if (const char* ao = std::getenv("SMS_PORT_AUDIO_OUTPUT"))
			apply(t, "audio", "output", ao);
		port_audio_set_output_mode(t.audioOutput);
	}
	port_replay_init(config); // w9: recording / SMS_PORT_REPLAY (may set SMS_PORT_NO_PADS)
	port_bugreport_init();    // w12: the rolling bug-report capture ([bugreport] enabled)
	// SMS_PORT_NO_PADS=1 (test runs, build_aurora.py --run without --pads): SDL never opens a
	// physical controller, so test instances neither read nor rumble the player's pad.
	// Keyboard and SMS_PORT_KEYSCRIPT/PADSCRIPT still work. SDL reads hints from the environment.
	if (const char* np = std::getenv("SMS_PORT_NO_PADS"))
		if (*np != '\0' && *np != '0') {
			static const char* const kOff[] = { "SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_WGI",
				                                "SDL_JOYSTICK_DIRECTINPUT", "SDL_XINPUT_ENABLED",
				                                "SDL_JOYSTICK_GAMEINPUT" };
			for (const char* h : kOff)
				SetEnvironmentVariableA(h, "0");
			SetEnvironmentVariableA("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", "0x0000/0x0000");
			std::fprintf(stderr, "[port] input: SMS_PORT_NO_PADS: physical controllers ignored\n");
		}
	// SMS_PORT_VPAD=ps5|ps4 (test knob, port_padfx.cpp): a virtual controller. Every physical
	// backend stays off as with SMS_PORT_NO_PADS (the virtual driver is not one of them), so the
	// player's own controller can never be opened; only the virtual pad's VID/PID is let through.
	unsigned vvid, vpid;
	if (port_vpad_requested(&vvid, &vpid)) {
		static const char* const kOff[] = { "SDL_JOYSTICK_HIDAPI", "SDL_JOYSTICK_RAWINPUT", "SDL_JOYSTICK_WGI",
			                                "SDL_JOYSTICK_DIRECTINPUT", "SDL_XINPUT_ENABLED",
			                                "SDL_JOYSTICK_GAMEINPUT" };
		for (const char* h : kOff)
			SetEnvironmentVariableA(h, "0");
		char except[32];
		std::snprintf(except, sizeof(except), "0x%04x/0x%04x", vvid, vpid);
		SetEnvironmentVariableA("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT", except);
		std::fprintf(stderr, "[port] input: SMS_PORT_VPAD: physical controllers ignored, virtual %s only\n", except);
	}

	config->windowWidth     = (uint32_t)s.width;
	config->windowHeight    = (uint32_t)s.height;
	config->startFullscreen = s.fullscreen;
	config->allowJoystickBackgroundEvents = s.backgroundInput;
	config->msaa                          = (uint32_t)s.msaa; // w6 (maxTextureAnisotropy: Aurora default 16)
	config->vsync                         = s.vsync;          // w6 (port_main sets false before; this wins)
	sActiveMsaa                           = s.msaa;
	port_gfx_backend_configure(config, sPath.c_str()); // w6: [graphics] backend, start guard
	// Aurora only takes non-negative positions; others (a display left of or above the
	// main one) are applied after the window exists (port_window.cpp). A position on
	// no connected display is ignored.
	if (s.hasPos) {
		RECT r = { s.x, s.y, s.x + s.width, s.y + s.height };
		if (MonitorFromRect(&r, MONITOR_DEFAULTTONULL) == nullptr)
			s.hasPos = false;
		else if (s.x >= 0 && s.y >= 0) {
			config->windowPosX = s.x;
			config->windowPosY = s.y;
		}
	}
	if (!s.hasPos) {
		config->windowPosX = -1;
		config->windowPosY = -1;
	}
	port_input_configure();
	std::atexit(port_settings_save);
}

} // extern "C"

#endif // SMS_PORT_AURORA
