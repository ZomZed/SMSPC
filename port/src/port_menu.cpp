// In-game settings menu and frame-rate overlay (Dear ImGui through Aurora). (w9)
//
// Esc (or F1), or the controller's Back/Create/View button, opens and closes it. While it is open
// the game keeps running but gets no controller or keyboard input (port_input.cpp), and
// the menu is driven with the mouse, the keyboard (arrows/Tab/Enter/Space/Esc) or the
// controller (D-pad/stick, A = activate, B = back), through ImGui's own navigation.
// Every change goes into gPortSettings and port_settings.ini (a second later), and is
// applied at once, except the button layout, which Aurora reads when the pad layer starts.
//
// Look: a themed window of its own (the rest of the port's ImGui keeps the default style):
// a header band, a sidebar of pages, and pages of section cards whose rows put each setting's
// name and a short explanation on the left and its control on the right (port_menu_ui.h).
// Text is Segoe UI (Windows' own font, baked large at start and scaled to the window, so it stays
// sharp at any resolution) with Segoe's icon font for the sidebar; without them, ImGui's font.
#ifdef SMS_PORT_AURORA

#include "port_settings.h"
#include "port_replay.h"
#include "port_bugreport.h" // w12
#include "port_discord.h"
#include "port_disc.h" // w13: port_build_region, port_behaviour_for_setting, port_restart_request
#include "port_audio.h" // w5: Audio tab, output mode
#include "port_menu_ui.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_video.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" void aurora_set_background_input(bool value);

extern "C" bool gPortQolRebindFf; // port_qol.cpp
extern "C" void port_isle_menu(void); // port_isle.cpp
extern "C" void port_shot_request(unsigned tag); // port_movetest.cpp (tests)

namespace pmui {
ImFont* gFontBody;
ImFont* gFontSmall;
ImFont* gFontBold;
ImFont* gFontTitle;
float gScale = 1.0f;
bool gInTable;
} // namespace pmui

namespace {

using namespace pmui;

bool sOpen;
int sRebind = -1;      // PortKey waiting for a key, or -1
bool sRebindArmed;     // all keys were released after Rebind was clicked
bool sNavWasEnabled;
int sPage;             // the sidebar's selection
bool sIcons;           // the icon font is merged into the body font

const char* const kKeyLabels[PORT_KEY_COUNT] = {
	"Stick up",     "Stick down", "Stick left",  "Stick right", "C-stick up", "C-stick down", "C-stick left",
	"C-stick right", "A",         "B",           "X",           "Y",          "Z",            "L",
	"R",            "Start",      "D-pad up",    "D-pad down",  "D-pad left", "D-pad right",
	"Soft (walk / light L/R)",
};

void changed() { port_settings_dirty(); }

// UTF-8 of a Segoe icon (private use area), or "" without the icon font.
const char* icon(unsigned cp)
{
	static char buf[8][4];
	static int n;
	if (!sIcons)
		return "";
	char* b = buf[n++ & 7];
	b[0]    = (char)(0xE0 | (cp >> 12));
	b[1]    = (char)(0x80 | ((cp >> 6) & 0x3F));
	b[2]    = (char)(0x80 | (cp & 0x3F));
	b[3]    = '\0';
	return b;
}

// ---- pages ------------------------------------------------------------------------------------

void displayPage(PortSettings& s)
{
	pageTitle("Display", "The window, the picture's shape and how often it is drawn.");

	beginSection("Window");
	SDL_Window* w = aurora_get_window();
	bool fs       = w != nullptr && (SDL_GetWindowFlags(w) & SDL_WINDOW_FULLSCREEN) != 0;
	if (row("Fullscreen", "Alt+Enter or F11 switch at any time.") && toggle("##fs", &fs))
		port_window_set_fullscreen(fs); // saved by the window tracking
	static const char* const kAspect[] = { "4:3 (original)", "Fit 640x448 frame", "Stretch to window",
		                                   "16:9 widescreen", "16:10 widescreen", "21:9 widescreen",
		                                   "32:9 widescreen", "Widescreen: match window" }; // w6: widescreen
	if (row("Aspect ratio", "Widescreen widens the 3D view; menus and the HUD stay 4:3, movies are pillarboxed.")
	    && combo("##aspect", &s.aspect, kAspect, PORT_ASPECT_COUNT)) {
		port_window_apply_display();
		changed();
	}
	{ // w11: widescreen HUD position
		static const char* const kHud[] = { "4:3 centered (original)", "16:9 edges", "Match aspect (screen edges)" };
		row("HUD position", s.aspect < PORT_ASPECT_16_9 ? "Only applies with a widescreen aspect ratio."
		                                                  : "Where the counters sit on a wide screen.");
		ImGui::BeginDisabled(s.aspect < PORT_ASPECT_16_9);
		if (combo("##hud", &s.hudPosition, kHud, 3))
			changed(); // applies at once (read every frame by port_widescreen.cpp)
		ImGui::EndDisabled();
	}
	// w6: an ini value not in the list (e.g. 2.41 for 1080 rows) shows as "Custom" instead of
	// "Window resolution". Never below 1x: Aurora keeps the EFB at least 640x448
	// (aurora_efb_floor.patch) so the game's CPU-read screen copies stay GameCube-exact.
	static const char* kScale[]      = { "Window resolution", "1x (640x448)", "2x", "3x", "4x", "6x", "8x", "" };
	static const float kScaleValue[] = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f };
	static char sCustomScale[32];
	int scale = 7, scaleItems = 8;
	for (int i = 0; i < 7; i++)
		if (s.internalScale == kScaleValue[i])
			scale = i, scaleItems = 7;
	std::snprintf(sCustomScale, sizeof(sCustomScale), "Custom (%gx)", s.internalScale < 1.0f ? 1.0f : s.internalScale);
	kScale[7] = sCustomScale;
	if (row("Internal resolution", "Resolution the 3D scene is drawn at. Never below the GameCube's 640x448, "
	                               "so effects that read the screen back stay exact.")
	    && combo("##scale", &scale, kScale, scaleItems) && scale < 7) {
		s.internalScale = kScaleValue[scale];
		port_window_apply_display();
		changed();
	}
	endSection();

	beginSection("Frame rate");
	static const char* const kFps[] = { "30 (original)", "60", "120", "144", "Match display", "Unlimited" };
	static const int kFpsValue[]    = { 30, 60, 120, 144, PORT_FPS_DISPLAY, PORT_FPS_UNLIMITED };
	int fps                         = 0;
	for (int i = 0; i < 6; i++)
		if (s.frameRate == kFpsValue[i])
			fps = i;
	if (row("Frame rate", "The game always runs at 30 Hz; above 30, frames in between are drawn with "
	                      "interpolated camera and character movement.")
	    && combo("##fps", &fps, kFps, 6)) {
		s.frameRate = kFpsValue[fps];
		changed();
	}
	if (port_interp_override())
		status("SMS_PORT_FPS is set: it overrides this for this run.");
	// w6's VSync belongs with the frame rate (w9): it caps the presented frames at the
	// monitor's refresh rate; without it, frames are paced by the game and this setting.
	if (row("VSync", "No tearing: waits for the monitor's refresh. \"Match display\" fits it best.")
	    && toggle("##vsync", &s.vsync)) {
		port_window_apply_display();
		changed();
	}
	{
		const int r     = port_interp_rate();
		const double hz = port_interp_display_hz();
		if (s.vsync && r != 30 && (r == PORT_FPS_UNLIMITED || (r > 0 && r > hz + 0.5)))
			status("VSync caps the frame rate at the monitor's %.0f Hz.", hz);
	}
	if (row("Frame-rate counter", "A small counter in the corner (F3).") && toggle("##overlay", &s.overlay))
		changed();
	endSection();
}

void graphicsPage(PortSettings& s)
{
	pageTitle("Graphics", "Optional enhancements. The first choice of each is how the GameCube draws it.");

	beginSection("Textures");
	// w6: optional enhancements ([graphics] in port_settings.ini)
	static const char* const kAniso[] = { "As the game asks (GameCube)", "1x (off)", "2x", "4x", "8x", "16x" };
	static const int kAnisoValue[]    = { 0, 1, 2, 4, 8, 16 };
	int aniso                         = 0;
	for (int i = 0; i < 6; i++)
		if (s.anisotropy == kAnisoValue[i])
			aniso = i;
	if (row("Anisotropic filtering", "Sharper ground and walls seen at a slant.") && combo("##aniso", &aniso, kAniso, 6)) {
		s.anisotropy = kAnisoValue[aniso];
		port_window_apply_display();
		changed();
	}
	if (row("Trilinear mipmaps", "Smooth transitions between texture detail levels.") && toggle("##tri", &s.trilinear)) {
		port_window_apply_display();
		changed();
	}
	endSection();

	beginSection("Anti-aliasing");
	static const char* const kMsaa[] = { "Off (GameCube)", "4x" };
	int msaa                         = s.msaa > 1 ? 1 : 0;
	if (row("MSAA", "Smooth polygon edges. Takes effect the next time the game starts.") && combo("##msaa", &msaa, kMsaa, 2)) {
		s.msaa = msaa ? 4 : 1;
		changed();
	}
	if (s.msaa != port_settings_active_msaa())
		status("Changed: restart the game to apply.");
	if (row("FXAA", "Post-process smoothing over the whole picture.") && toggle("##fxaa", &s.fxaa)) {
		port_window_apply_display();
		changed();
	}
	endSection();

	beginSection("Picture");
	if (row("Brightness") && slider("##bright", &s.brightness, 50, 150, "%d%%")) {
		port_window_apply_display();
		changed();
	}
	if (row("Gamma") && slider("##gamma", &s.gamma, 50, 200, "%d%%")) {
		port_window_apply_display();
		changed();
	}
	bool sharp = !s.copyFilter;
	if (row("Sharper screen effects", "Skips the slight blur the game puts on screen copies (heat haze, sunglasses).")
	    && toggle("##sharp", &sharp)) {
		s.copyFilter = !sharp;
		port_window_apply_display();
		changed();
	}
	endSection();

	beginSection("Renderer");
	// w6: graphics API (port_gfx_backend.cpp), applied at the next start.
	static const char* const kBackend[] = { "Direct3D 12 (default)", "Vulkan", "Direct3D 11" };
	if (row("Graphics API", "Takes effect the next time the game starts; if it cannot start, another one is used.")
	    && combo("##api", &s.backend, kBackend, PORT_BACKEND_COUNT))
		changed();
	status("%s%s", port_gfx_backend_status(),
	       s.backend != port_gfx_backend_active() ? " (the choice applies at the next start)" : "");
	endSection();

	beginSection("Presets");
	row("GameCube defaults", "Every graphics option back to how the GameCube draws, at 30 fps.");
	if (button("Reset graphics")) {
		s.msaa = 1, s.anisotropy = 0, s.trilinear = false, s.fxaa = false;
		s.brightness = 100, s.gamma = 100, s.copyFilter = true;
		s.frameRate = 30; // w9: the GameCube's 30 frames per second
		s.vsync     = false; // w6: the default (frames paced by the game alone)
		port_window_apply_display();
		changed();
	}
	// w6: preset for weak PCs: the cheapest settings that still render exactly like the
	// GameCube: native 640x448 scene (below that the screen readbacks would no longer match,
	// so there is no lower option), no MSAA/FXAA/forced filtering, 30 fps (no extra frames).
	row("Performance", "For weak PCs: native 640x448, no anti-aliasing or extra filtering, 30 fps. "
	                   "Brightness, gamma, aspect and the graphics API are kept.");
	if (button("Apply performance preset")) {
		s.internalScale = 1.0f;
		s.msaa = 1, s.anisotropy = 0, s.trilinear = false, s.fxaa = false;
		s.frameRate = 30;
		port_window_apply_display();
		changed();
	}
	endSection();
}

void controllerPage(PortSettings& s)
{
	pageTitle("Controller", "Buttons, sticks, rumble and the extras of PlayStation controllers.");

	beginSection("Buttons and sticks");
	static const char* const kLayout[] = { "GameCube positions", "Aurora defaults" };
	int layout = s.gcLayout ? 0 : 1;
	if (row("Button layout", "GameCube positions on a PlayStation pad: Cross A, Square B, Circle X, Triangle Y. "
	                         "Takes effect the next time the game starts.")
	    && combo("##layout", &layout, kLayout, 2)) {
		s.gcLayout = layout == 0;
		changed();
	}
	if (row("Stick dead zone", "0 = only the GameCube's own dead zone.") && slider("##dz", &s.stickDeadzone, 0, 50, "%d%%"))
		changed();
	if (row("C-stick dead zone") && slider("##cdz", &s.cstickDeadzone, 0, 50, "%d%%"))
		changed();
	if (row("Linear analog L/R", "The triggers' travel maps evenly up to the click.") && toggle("##lin", &s.linearTriggers))
		changed();
	if (row("Background input", "Read the controller while the window is not focused.")
	    && toggle("##bg", &s.backgroundInput)) {
		aurora_set_background_input(s.backgroundInput);
		changed();
	}
	endSection();

	beginSection("Rumble");
	static const char* const kRumble[] = { "Off", "Original", "HD" };
	if (row("Rumble", "Original: the game's own motor pulses. HD: designed waveforms, plus FLUDD, footsteps, "
	                  "landings and pickups (DualSense HD haptics; Xbox impulse triggers for FLUDD).")
	    && combo("##rumble", &s.rumbleMode, kRumble, PORT_RUMBLE_COUNT))
		changed();
	if (row("Strength") && slider("##rstr", &s.rumble, 0, 100, "%d%%"))
		changed();
	if (s.rumbleMode == PORT_RUMBLE_HD) {
		bool c = false;
		row("HD effects", "Which kinds of HD rumble play.");
		c |= ImGui::Checkbox("FLUDD", &s.hdFludd);
		ImGui::SameLine();
		c |= ImGui::Checkbox("Movement", &s.hdMovement);
		ImGui::SameLine();
		c |= ImGui::Checkbox("World", &s.hdWorld);
		ImGui::SameLine();
		c |= ImGui::Checkbox("Damage", &s.hdDamage);
		if (c)
			changed();
	}
	status("%s", port_haptics_status());
	endSection();

	// DualSense / DualShock 4 (port_padfx.cpp)
	beginSection("DualSense and DualShock 4");
	static const char* const kTriggers[] = { "Off", "GameCube click", "Full FLUDD feedback" };
	if (row("Adaptive triggers", "GameCube click: a firm wall where the digital L/R click is. Full: R also goes "
	                             "slack when the tank is empty, pulses while spraying, and feels each nozzle.")
	    && combo("##trig", &s.adaptiveTriggers, kTriggers, PORT_TRIGGERS_COUNT))
		changed();
	if (row("Trigger strength") && slider("##tstr", &s.triggerStrength, 0, 100, "%d%%"))
		changed();
	static const char* const kLightbar[] = { "Off", "Fluid level", "Static colour" };
	if (row("Lightbar", "Fluid level: the colour of FLUDD's water or Yoshi's juice, darker as it drains.")
	    && combo("##lb", &s.lightbar, kLightbar, PORT_LIGHTBAR_COUNT))
		changed();
	if (row("Lightbar brightness") && slider("##lbb", &s.lightbarBrightness, 0, 100, "%d%%"))
		changed();
	if (s.lightbar == PORT_LIGHTBAR_STATIC) {
		float c[3] = { s.lightbarColor[0] / 255.0f, s.lightbarColor[1] / 255.0f, s.lightbarColor[2] / 255.0f };
		row("Lightbar colour");
		ImGui::SetNextItemWidth(-FLT_MIN);
		if (ImGui::ColorEdit3("##lbc", c)) {
			for (int i = 0; i < 3; i++)
				s.lightbarColor[i] = (unsigned char)(c[i] * 255.0f + 0.5f);
			changed();
		}
	}
	status("%s", port_padfx_status());
	endSection();
}

void keyboardPage(PortSettings& s)
{
	pageTitle("Keyboard & Mouse", "Play without a controller: keys act as controller 1, the mouse turns the camera.");

	beginSection("Keyboard");
	if (row("Keyboard controls", "Keys drive controller 1.") && toggle("##kb", &s.keyboard))
		changed();
	if (row("Soft key tilt", "How far the stick tilts while the soft (walk) key is held.")
	    && slider("##soft", &s.softTilt, 10, 100, "%d%%"))
		changed();
	endSection();

	beginSection("Mouse");
	if (row("Mouse camera and buttons", "In a level the mouse turns the camera; Esc or any menu frees it. "
	                                    "Left: spray + walk   Right: L   Middle: full spray   Side: Y   Wheel: camera up/down")
	    && toggle("##mouse", &s.mouse))
		changed();
	ImGui::BeginDisabled(!s.mouse);
	if (row("Sensitivity") && slider("##sens", &s.mouseSensitivity, 1, 100))
		changed();
	if (row("Invert vertical") && toggle("##invy", &s.mouseInvertY))
		changed();
	ImGui::EndDisabled();
	endSection();

	beginSection("Key bindings", "Click a key, then press the new one (Esc cancels, Backspace clears).");
	if (sRebind >= 0) {
		int numKeys       = 0;
		const bool* state = SDL_GetKeyboardState(&numKeys);
		bool any          = false;
		for (int k = 1; k < numKeys; k++)
			if (state[k]) {
				any = true;
				if (sRebindArmed) {
					if (k != SDL_SCANCODE_ESCAPE)
						s.keys[sRebind] = k == SDL_SCANCODE_BACKSPACE ? SDL_SCANCODE_UNKNOWN : k;
					sRebind = -1;
					changed();
					break;
				}
			}
		if (!any)
			sRebindArmed = true;
	}
	for (int i = 0; i < PORT_KEY_COUNT; i++) {
		row(kKeyLabels[i]);
		const char* name = s.keys[i] > 0 ? SDL_GetScancodeName((SDL_Scancode)s.keys[i]) : "";
		char label[96];
		std::snprintf(label, sizeof(label), "%s##key%d",
		              sRebind == i ? "Press a key..." : (name != nullptr && *name != '\0' ? name : "(none)"), i);
		if (button(label, sRebind == i, -FLT_MIN)) {
			sRebind      = i;
			sRebindArmed = false;
		}
	}
	endSection();
	muted("Hotkeys: Esc menu, F3 frame-rate counter, F9 save a bug report, F11 or Alt+Enter fullscreen.");
}

void audioPage(PortSettings& s)
{
	pageTitle("Audio", "Volume, speakers and the controller's own speaker.");

	beginSection("Volume");
	bool c = false;
	if (row("Music") && slider("##vmus", &s.qolVolMusic, 0, 100, "%d%%"))
		c = true;
	if (row("Sound effects") && slider("##vsfx", &s.qolVolSfx, 0, 100, "%d%%"))
		c = true;
	if (row("Voices") && slider("##vvoi", &s.qolVolVoice, 0, 100, "%d%%"))
		c = true;
	if (c)
		changed();
	endSection();

	// w5: output (port_audio_out.cpp); w9: DualSense speaker (port_speaker.cpp)
	beginSection("Speakers and headphones");
	static const char* const kOut[] = { "Stereo", "Pro Logic II matrix (original)", "5.1 surround", "7.1 surround" };
	if (row("Output", "Original: the game's own Sound option (mono / stereo / surround) decides. "
	                  "5.1 / 7.1: discrete channels, mixed by the game in surround.")
	    && combo("##out", &s.audioOutput, kOut, 4)) {
		port_audio_set_output_mode(s.audioOutput);
		changed();
	}
	status("Now: %s", port_audio_output_status());
	endSection();

	beginSection("Controller speaker (DualSense)");
	static const char* const kSpk[] = { "Off", "FLUDD sounds", "FLUDD + pickups" };
	if (row("Sounds on the controller") && combo("##spk", &s.speakerMode, kSpk, PORT_SPEAKER_COUNT))
		changed();
	static const char* const kMix[] = { "Keep", "Duck", "Remove" };
	if (row("Those sounds in the main mix") && combo("##mix", &s.mainMix, kMix, PORT_MAINMIX_COUNT))
		changed();
	if (row("Controller speaker volume") && slider("##spkv", &s.speakerVolume, 0, 100, "%d%%"))
		changed();
	status("%s", port_speaker_status());
	endSection();
}

// w13 (BOOTLOG "Regional behaviour (w13)"): game behaviour of another release, and the
// European build's video / language settings. Both apply when the game starts, so a change
// offers a restart (port_restart_request, port_behaviour.cpp).
void gamePage(PortSettings& s)
{
	pageTitle("Game", "Game data, regional behaviour and bug-report recordings.");

	beginSection("Game data");
	row("Disc", "Set disc = in port_settings.ini, or pass the disc image on the command line.");
	ImGui::TextWrapped("%s", s.disc[0] != '\0' ? s.disc : "(auto-detected)");
	endSection();

	beginSection("Region");
	static const char* const kBehaviour[] = { "Match disc", "US", "Japan", "Europe (experimental)" };
	if (lockedRow("Game behaviour", "The game-code differences of another release (sound effects, fixes, small "
	                                "gameplay changes) on whatever disc you use; text and menus stay the disc's. "
	                                "Applies when the game restarts.",
	              "Locked during online play: every player needs the same game behaviour (players whose "
	              "games differ are warned).")
	    && combo("##beh", &s.behaviour, kBehaviour, 4))
		changed();
	endLocked();
	static const char* const kNow[] = { "Japan", "Europe", "US" };
	const int active = SMSGetBehaviour();
	const int chosen = port_behaviour_for_setting(s.behaviour);
	bool restart     = chosen != active;
	status("Now: %s behaviour", kNow[active < 0 || active > 2 ? 2 : active]);
	if (port_build_region() == 'P') {
		static const char* const kVideo[] = { "50 Hz", "60 Hz", "Ask at boot" };
		if (lockedRow("PAL video", "The European release runs at 50 or 60 Hz; \"Ask at boot\" shows the game's own "
		                           "question after the Nintendo logo.",
		              "Locked during online play: a 50 Hz game runs slower than the others'.")
		    && combo("##pal", &s.pal60, kVideo, 3))
			changed();
		endLocked();
		static const char* const kLang[] = { "English", "German", "French", "Spanish", "Italian" };
		if (row("PAL language", "The language a new save file starts with (the game's options change it later).")
		    && combo("##lang", &s.palLanguage, kLang, 5))
			changed();
		restart = restart || port_pal_settings_changed();
	}
	if (restart) {
		lockedRow("Restart needed", "Unsaved progress is lost.", "Leave the multiplayer session first.");
		if (button("Restart now", true))
			port_restart_request();
		endLocked();
	}
	endSection();

	beginSection("Discord");
	if (row("Rich Presence", "Shows the level, shines and online party in Discord. Needs the Discord app running "
	                         "and a client_id in port_settings.ini [discord].")
	    && toggle("##dce", &s.discordEnabled))
		changed();
	if (row("Show level", "Off: Discord only says you are playing.") && toggle("##dcs", &s.discordShowStage))
		changed();
	if (row("Allow joining", "Online lobbies get a Join button; friends can ask to join (F7 accept, F8 ignore).")
	    && toggle("##dcj", &s.discordAllowJoin))
		changed();
	status("%s", port_discord_status());
	endSection();

	beginSection("Bug reports");
	// Input recording for bug reports (port_replay.cpp).
	if (row("Record input", "From the next start. A replay (sms_replay_*.bin next to sms_port.exe) lets "
	                        "developers reproduce what happened.")
	    && toggle("##rec", &s.record))
		changed();
	status("%s", port_replay_status());
	if (port_bugreport_enabled()) { // w12
		row("Save a bug report", "Also F9. The last seconds as video.mp4 + log.txt + the replay, in "
		                         "bug_reports\\ next to sms_port.exe.");
		if (button("Save bug report now"))
			port_bugreport_save("menu", nullptr);
	} else if (port_replay_recording()) {
		row("Save the replay", "Also F9.");
		if (button("Save replay now"))
			port_replay_save("menu");
	}
	endSection();
}

void debugPage(PortSettings&)
{
	pageTitle("Debug", "Developer tools.");
	beginSection("Island test");
	cell(0, true);
	if (online())
		warning("Off during online play: the island is a map of its own the other players cannot join.");
	ImGui::BeginDisabled(online());
	port_isle_menu(); // the island test (port_isle.cpp)
	ImGui::EndDisabled();
	endSection();
}

void gameplayPage(PortSettings&) { port_qol_menu(); }

struct Page {
	unsigned icon;
	const char* name;
	void (*draw)(PortSettings&);
};
// Segoe icon font code points (MDL2 / Fluent): monitor, palette, game pad, keyboard, speaker,
// lightning, globe, wrench
const Page kPages[] = {
	{ 0xE7F4, "Display", displayPage },    { 0xE790, "Graphics", graphicsPage },
	{ 0xE7FC, "Controller", controllerPage }, { 0xE765, "Keyboard & Mouse", keyboardPage },
	{ 0xE767, "Audio", audioPage },         { 0xE945, "Gameplay", gameplayPage },
	{ 0xE909, "Game", gamePage },           { 0xE90F, "Debug", debugPage },
};
const int kPageCount = (int)(sizeof(kPages) / sizeof(kPages[0]));

// ---- theme ------------------------------------------------------------------------------------

void applyTheme(ImGuiStyle& st)
{
	const float u    = gScale;
	st.WindowPadding = ImVec2(0.0f, 0.0f);
	st.FramePadding  = ImVec2(12.0f * u, 7.0f * u);
	st.ItemSpacing   = ImVec2(10.0f * u, 8.0f * u);
	st.ItemInnerSpacing = ImVec2(8.0f * u, 6.0f * u);
	st.ScrollbarSize    = 12.0f * u;
	st.GrabMinSize      = 18.0f * u;
	st.WindowRounding   = 16.0f * u;
	st.ChildRounding    = 12.0f * u;
	st.FrameRounding    = 8.0f * u;
	st.PopupRounding    = 10.0f * u;
	st.ScrollbarRounding = 8.0f * u;
	st.GrabRounding     = 8.0f * u;
	st.TabRounding      = 8.0f * u;
	st.WindowBorderSize = 1.0f;
	st.ChildBorderSize  = 0.0f;
	st.FrameBorderSize  = 0.0f;
	st.PopupBorderSize  = 1.0f;
	st.SelectableTextAlign = ImVec2(0.0f, 0.5f);
	ImVec4* c                       = st.Colors;
	c[ImGuiCol_Text]                = colText();
	c[ImGuiCol_TextDisabled]        = colMuted();
	c[ImGuiCol_WindowBg]            = colWindow();
	c[ImGuiCol_ChildBg]             = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_PopupBg]             = rgb(18, 33, 55, 0.99f);
	c[ImGuiCol_Border]              = rgb(52, 84, 126, 0.7f);
	c[ImGuiCol_FrameBg]             = colFrame();
	c[ImGuiCol_FrameBgHovered]      = colFrameHover();
	c[ImGuiCol_FrameBgActive]       = colFrameActive();
	c[ImGuiCol_Button]              = colFrame();
	c[ImGuiCol_ButtonHovered]       = colFrameHover();
	c[ImGuiCol_ButtonActive]        = colFrameActive();
	c[ImGuiCol_Header]              = colFrameHover();
	c[ImGuiCol_HeaderHovered]       = colFrameHover();
	c[ImGuiCol_HeaderActive]        = colFrameActive();
	c[ImGuiCol_CheckMark]           = colAccent();
	c[ImGuiCol_SliderGrab]          = colAccent();
	c[ImGuiCol_SliderGrabActive]    = colAccentHover();
	c[ImGuiCol_ScrollbarBg]         = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_ScrollbarGrab]       = rgb(52, 84, 126);
	c[ImGuiCol_ScrollbarGrabHovered] = rgb(70, 108, 158);
	c[ImGuiCol_ScrollbarGrabActive] = rgb(90, 130, 180);
	c[ImGuiCol_Separator]           = rgb(52, 84, 126, 0.6f);
	c[ImGuiCol_NavHighlight]        = colWater();
	c[ImGuiCol_TableRowBg]          = ImVec4(0, 0, 0, 0);
	c[ImGuiCol_TableRowBgAlt]       = ImVec4(1, 1, 1, 0.03f);
	c[ImGuiCol_ModalWindowDimBg]    = ImVec4(0, 0, 0, 0.5f);
}

// Body text size for this window: ~21 px at 1080 rows, never tiny, never huge.
const float kBakedBody = 34.0f; // the size the fonts are baked at (port_menu_init_fonts)

void scaleFonts(float bodyPx)
{
	const float k = bodyPx / kBakedBody;
	for (ImFont* f : { gFontBody, gFontSmall, gFontBold, gFontTitle })
		if (f != nullptr)
			f->Scale = k;
}

// ---- the window ---------------------------------------------------------------------------------

void header(float width)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float h  = px(78.0f);
	// a band from sea blue to the window colour, with a sunshine line under it
	// (rounded like the window's top corners: a rounded shape whose vertices get the gradient)
	const int v0 = dl->VtxBuffer.Size;
	dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), IM_COL32_WHITE, px(16.0f), ImDrawFlags_RoundCornersTop);
	ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, p, ImVec2(p.x + width, p.y),
	                                              col32(rgb(18, 84, 128)), col32(rgb(14, 36, 64)));
	dl->AddRectFilled(ImVec2(p.x, p.y + h - px(3.0f)), ImVec2(p.x + width, p.y + h), col32(colAccent()));
	ImGui::SetCursorScreenPos(ImVec2(p.x + px(28.0f), p.y + px(12.0f)));
	{
		FontScope f(gFontTitle);
		ImGui::PushStyleColor(ImGuiCol_Text, colAccent());
		ImGui::TextUnformatted("Settings");
		ImGui::PopStyleColor();
	}
	ImGui::SetCursorScreenPos(ImVec2(p.x + px(30.0f), ImGui::GetCursorScreenPos().y - px(4.0f)));
	{
		FontScope f(gFontSmall);
		ImGui::PushStyleColor(ImGuiCol_Text, rgb(190, 214, 238));
		ImGui::TextUnformatted("Super Mario Sunshine  -  PC port");
		ImGui::PopStyleColor();
	}
	// close button, top right
	const float bs = px(40.0f);
	ImGui::SetCursorScreenPos(ImVec2(p.x + width - bs - px(18.0f), p.y + (h - bs) * 0.5f));
	ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1, 1, 1, 0.06f));
	ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rgb(220, 70, 70, 0.9f));
	ImGui::PushStyleColor(ImGuiCol_ButtonActive, rgb(190, 50, 50, 1.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, bs * 0.5f);
	if (ImGui::Button(sIcons ? (std::string(icon(0xE8BB)) + "##close").c_str() : "X##close", ImVec2(bs, bs)))
		port_menu_toggle();
	ImGui::PopStyleVar();
	ImGui::PopStyleColor(3);
	ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

void sidebar(float height)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, colSidebar());
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(14.0f), px(18.0f)));
	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(6.0f), px(6.0f)));
	ImGui::BeginChild("##nav", ImVec2(px(250.0f), height), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
	for (int i = 0; i < kPageCount; i++) {
		const bool sel = sPage == i;
		ImGui::PushStyleColor(ImGuiCol_Header, sel ? colAccent() : colFrame());
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, sel ? colAccentHover() : colFrame());
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, sel ? colAccentHover() : colFrameActive());
		ImGui::PushStyleColor(ImGuiCol_Text, sel ? colAccentText() : colText());
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, px(10.0f));
		char label[96];
		if (sIcons)
			std::snprintf(label, sizeof(label), "   %s    %s##page%d", icon(kPages[i].icon), kPages[i].name, i);
		else
			std::snprintf(label, sizeof(label), "   %s##page%d", kPages[i].name, i);
		if (ImGui::Selectable(label, sel, 0, ImVec2(0.0f, px(44.0f))))
			sPage = i;
		// D-pad / arrows on the sidebar switch pages as the focus moves
		if (ImGui::IsItemFocused() && !sel && ImGui::GetIO().NavActive)
			sPage = i;
		ImGui::PopStyleVar();
		ImGui::PopStyleColor(4);
	}
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
}

void footer(float width)
{
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const float h  = px(62.0f);
	dl->AddRectFilled(p, ImVec2(p.x + width, p.y + h), col32(colSidebar()), px(16.0f), ImDrawFlags_RoundCornersBottom);
	dl->AddLine(p, ImVec2(p.x + width, p.y), col32(rgb(52, 84, 126, 0.7f)));
	ImGui::SetCursorScreenPos(ImVec2(p.x + px(28.0f), p.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
	{
		FontScope f(gFontSmall);
		ImGui::PushStyleColor(ImGuiCol_Text, colMuted());
		ImGui::TextUnformatted("Changes apply at once and are saved to port_settings.ini.");
		ImGui::PopStyleColor();
	}
	const float bw = px(150.0f);
	ImGui::SetCursorScreenPos(ImVec2(p.x + width - bw - px(24.0f), p.y + (h - ImGui::GetFrameHeight()) * 0.5f));
	if (button("Close   (Esc)", true, bw))
		port_menu_toggle();
}

void drawMenu()
{
	PortSettings& s = gPortSettings;
	ImGuiIO& io     = ImGui::GetIO();
	// size everything from the window height (designed at 1080 rows)
	gScale = io.DisplaySize.y / 1080.0f;
	if (gScale < 0.62f)
		gScale = 0.62f;
	if (gScale > 2.2f)
		gScale = 2.2f;
	scaleFonts(22.0f * gScale);

	ImGuiStyle& style = ImGui::GetStyle();
	const ImGuiStyle saved = style; // the theme is this window's only
	applyTheme(style);
	FontScope body(gFontBody);
	if (gFontBody == nullptr) // ImGui's 13 px font: scale it instead
		ImGui::GetFont()->Scale = (22.0f * gScale) / 13.0f;

	// dim the game behind the menu
	ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(4, 10, 20, 140));

	float w = io.DisplaySize.x * 0.72f, h = io.DisplaySize.y * 0.84f;
	if (w > px(1360.0f))
		w = px(1360.0f);
	if (w < px(900.0f))
		w = io.DisplaySize.x * 0.96f;
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
	bool open = true;
	if (ImGui::Begin("Settings##port", &open,
	                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove
	                     | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar
	                     | ImGuiWindowFlags_NoScrollWithMouse)) {
		if (ImGui::IsWindowAppearing()) {
			ImGui::SetWindowFocus();
			std::fprintf(stderr, "[port] menu window at %.0f,%.0f (display %.0fx%.0f)\n", ImGui::GetWindowPos().x,
			             ImGui::GetWindowPos().y, io.DisplaySize.x, io.DisplaySize.y);
		}
		const float footerH = px(62.0f);
		header(w);
		const float bodyH = h - ImGui::GetCursorPosY() - footerH;
		sidebar(bodyH);
		ImGui::SameLine(0.0f, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(30.0f), px(24.0f)));
		ImGui::BeginChild("##page", ImVec2(0.0f, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleVar();
		if (sPage < 0 || sPage >= kPageCount)
			sPage = 0;
		kPages[sPage].draw(s);
		ImGui::EndChild();
		footer(w);
	}
	ImGui::End();
	if (gFontBody == nullptr)
		ImGui::GetFont()->Scale = 1.0f;
	style = saved;
	if (!open)
		port_menu_toggle();
}

void drawOverlay(const char* text)
{
	ImDrawList* dl   = ImGui::GetForegroundDrawList();
	const ImVec2 sz  = ImGui::CalcTextSize(text);
	const ImVec2 pos = ImVec2(10.0f, 8.0f);
	dl->AddRectFilled(ImVec2(pos.x - 4.0f, pos.y - 3.0f), ImVec2(pos.x + sz.x + 4.0f, pos.y + sz.y + 3.0f),
	                  IM_COL32(0, 0, 0, 160), 4.0f);
	dl->AddText(pos, IM_COL32(96, 255, 96, 255), text);
}

} // namespace

extern "C" {

// port_main.cpp, right after Aurora created ImGui (before the first frame builds the font atlas):
// ImGui's own font stays the default (the multiplayer windows use it); the menu's Segoe UI faces
// and the icon font are added next to it. Missing files: the menu uses ImGui's font.
void port_menu_init_fonts(void)
{
	if (ImGui::GetCurrentContext() == nullptr)
		return;
	ImFontAtlas* atlas = ImGui::GetIO().Fonts;
	if (atlas->Fonts.Size == 0)
		atlas->AddFontDefault();
	const char* winDir = std::getenv("WINDIR");
	char dir[260];
	std::snprintf(dir, sizeof(dir), "%s\\Fonts\\", winDir != nullptr ? winDir : "C:\\Windows");
	auto path = [&](const char* file) {
		static char p[4][300];
		static int n;
		char* b = p[n++ & 3];
		std::snprintf(b, 300, "%s%s", dir, file);
		FILE* f = std::fopen(b, "rb");
		if (f == nullptr)
			return (const char*)nullptr;
		std::fclose(f);
		return (const char*)b;
	};
	const char* regular = path("segoeui.ttf");
	const char* bold    = path("segoeuib.ttf");
	if (regular == nullptr) {
		std::fprintf(stderr, "[port] menu: Segoe UI not found, using ImGui's font\n");
		return;
	}
	ImFontConfig cfg;
	cfg.OversampleH = 2;
	cfg.OversampleV = 1;
	gFontBody       = atlas->AddFontFromFileTTF(regular, kBakedBody, &cfg);
	// Segoe's icons merged into the body font (Windows 11: Segoe Fluent Icons, 10: MDL2 Assets)
	const char* icons = path("SegoeIcons.ttf");
	if (icons == nullptr)
		icons = path("segmdl2.ttf");
	if (icons != nullptr && gFontBody != nullptr) {
		static const ImWchar kIconRange[] = { 0xE700, 0xE9FF, 0 };
		ImFontConfig ic;
		ic.MergeMode        = true;
		ic.OversampleH      = 2;
		ic.GlyphOffset      = ImVec2(0.0f, kBakedBody * 0.12f);
		ic.GlyphMinAdvanceX = kBakedBody;
		sIcons = atlas->AddFontFromFileTTF(icons, kBakedBody * 0.9f, &ic, kIconRange) != nullptr;
	}
	gFontSmall = atlas->AddFontFromFileTTF(regular, kBakedBody * 0.88f, &cfg);
	gFontBold  = atlas->AddFontFromFileTTF(bold != nullptr ? bold : regular, kBakedBody, &cfg);
	gFontTitle = atlas->AddFontFromFileTTF(bold != nullptr ? bold : regular, kBakedBody * 1.45f, &cfg);
	std::fprintf(stderr, "[port] menu fonts: Segoe UI%s%s\n", bold != nullptr ? " + bold" : "", sIcons ? " + icons" : "");
}

void port_menu_toggle(void)
{
	sOpen   = !sOpen;
	sRebind = -1;
	ImGuiIO& io = ImGui::GetIO();
	if (sOpen) {
		sNavWasEnabled = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	} else if (!sNavWasEnabled) {
		io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
	}
	std::fprintf(stderr, "[port] menu %s\n", sOpen ? "opened" : "closed");
}

bool port_menu_is_open(void) { return sOpen; }
bool port_menu_key_capture(void) { return sRebind >= 0 || gPortQolRebindFf; }
bool port_menu_text_input(void) { return ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().WantTextInput; }

void port_menu_draw(const char* overlayText)
{
	if (ImGui::GetCurrentContext() == nullptr)
		return;
	// SMS_PORT_MENU_TEST=<page>@<frame> (tests): open the menu on that page at that frame;
	// all@<frame>: then every page in turn, a screenshot of each (shot_7000<page>.bmp)
	static const char* sTest = std::getenv("SMS_PORT_MENU_TEST");
	static int sTestFrames;
	if (sTest != nullptr) {
		const char* at  = std::strchr(sTest, '@');
		const int start = at != nullptr ? std::atoi(at + 1) : 120;
		const bool all  = std::strncmp(sTest, "all", 3) == 0;
		++sTestFrames;
		if (sTestFrames == start && !sOpen) {
			sPage = all ? 0 : std::atoi(sTest);
			port_menu_toggle();
		}
		const int t = sTestFrames - start;
		if (!all && sOpen && t == 30)
			port_shot_request(700000 + sPage);
		if (all && sOpen && t > 0 && t / 40 < kPageCount) {
			if (t % 40 == 0)
				sPage = t / 40; // the next page (shown for 40 frames)
			if (t % 40 == 30) {
				port_shot_request(700000 + sPage);
				std::fprintf(stderr, "[port] menu test: page %d (%s)\n", sPage, kPages[sPage].name);
			}
		}
	}
	port_qol_draw(); // w9: trackers, fast-forward badge, toasts (port_qol.cpp)
	if (sOpen)
		drawMenu();
	if (overlayText != nullptr && overlayText[0] != '\0')
		drawOverlay(overlayText);
}

} // extern "C"

#endif // SMS_PORT_AURORA
