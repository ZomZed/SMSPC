// PC-port settings, input and window glue for the Aurora build (w9).
//
//   port_settings.cpp  port_settings.ini next to the exe: load, defaults, save
//   port_input.cpp     PAD post-processing (GameCube stick/trigger semantics), keyboard pad
//   port_window.cpp    window size/position/fullscreen, 4:3 letterboxing, internal resolution,
//                      hotkeys (Alt+Enter, F11, F3), frame-time statistics and the F3 overlay
//
// Hooks in port_main.cpp (one line each): port_settings_configure() before
// aurora_initialize(), port_window_frame() in VIWaitForRetrace and port_input_read()
// in port_PADRead.
#ifndef PORT_SETTINGS_H
#define PORT_SETTINGS_H

#include <dolphin/pad.h>

#include <aurora/aurora.h>

enum PortAspect {
	PORT_ASPECT_4_3,     // windowed: client area locked to 4:3, picture fills it; otherwise letterboxed
	PORT_ASPECT_FIT,     // always letterboxed to the EFB shape (640x448)
	PORT_ASPECT_STRETCH, // picture stretched to the window
	// w6: true widescreen (Hor+). The logical EFB stays 640x528 GameCube pixels; the render
	// target and the display get the wide shape (anamorphic: wider target pixels), and the
	// game widens its projection by port_get_aspect_scale() (game side: w11).
	PORT_ASPECT_16_9,
	PORT_ASPECT_16_10,
	PORT_ASPECT_21_9,
	PORT_ASPECT_32_9,
	PORT_ASPECT_WINDOW, // widescreen matching the window's shape
	PORT_ASPECT_COUNT
};

// gPortSettings.frameRate besides 30/60/120/144
enum {
	PORT_FPS_DISPLAY   = -1, // the monitor's refresh rate
	PORT_FPS_UNLIMITED = 0,
};

// w6: [graphics] backend (port_gfx_backend.cpp)
enum PortBackend { PORT_BACKEND_D3D12, PORT_BACKEND_VULKAN, PORT_BACKEND_D3D11, PORT_BACKEND_COUNT };

// [input] adaptive_triggers (port_padfx.cpp)
enum PortAdaptiveTriggers {
	PORT_TRIGGERS_OFF,
	PORT_TRIGGERS_GAMECUBE, // light resistance, a firm wall at the digital click
	PORT_TRIGGERS_FLUDD,    // GameCube click plus FLUDD feedback on R
	PORT_TRIGGERS_COUNT
};

// [input] lightbar (port_padfx.cpp)
enum PortLightbar {
	PORT_LIGHTBAR_OFF,    // left to SDL (player colour)
	PORT_LIGHTBAR_FLUID,  // FLUDD water / Yoshi juice colour, dimmed as it drains
	PORT_LIGHTBAR_STATIC, // lightbar_color
	PORT_LIGHTBAR_COUNT
};

// [input] rumble_mode (port_haptics.cpp)
enum PortRumbleMode {
	PORT_RUMBLE_OFF,
	PORT_RUMBLE_ORIGINAL, // the game's motor on/off pulses (default)
	PORT_RUMBLE_HD,       // designed waveforms, motors / impulse triggers / DualSense haptics
	PORT_RUMBLE_COUNT
};

// [audio] controller_speaker / main_mix (port_speaker.cpp)
enum PortSpeaker { PORT_SPEAKER_OFF, PORT_SPEAKER_FLUDD, PORT_SPEAKER_FLUDD_PICKUPS, PORT_SPEAKER_COUNT };
enum PortMainMix { PORT_MAINMIX_KEEP, PORT_MAINMIX_DUCK, PORT_MAINMIX_REMOVE, PORT_MAINMIX_COUNT };

enum PortKey {
	PORT_KEY_STICK_UP,
	PORT_KEY_STICK_DOWN,
	PORT_KEY_STICK_LEFT,
	PORT_KEY_STICK_RIGHT,
	PORT_KEY_CSTICK_UP,
	PORT_KEY_CSTICK_DOWN,
	PORT_KEY_CSTICK_LEFT,
	PORT_KEY_CSTICK_RIGHT,
	PORT_KEY_A,
	PORT_KEY_B,
	PORT_KEY_X,
	PORT_KEY_Y,
	PORT_KEY_Z,
	PORT_KEY_L,
	PORT_KEY_R,
	PORT_KEY_START,
	PORT_KEY_DPAD_UP,
	PORT_KEY_DPAD_DOWN,
	PORT_KEY_DPAD_LEFT,
	PORT_KEY_DPAD_RIGHT,
	PORT_KEY_SOFT, // held: half stick tilt, light (analog only) L/R
	PORT_KEY_COUNT
};

struct PortSettings {
	// [game]
	char disc[1024]; // disc image or extracted folder (UTF-8); empty = auto-detect (port_disc.cpp)
	// w13 (BOOTLOG "Regional behaviour (w13)"): game-code behaviour, port_behaviour.cpp.
	int behaviour;   // 0 match disc, 1 US, 2 Japan, 3 Europe. Applied at the next start.
	int pal60;       // w13: European build: 0 50 Hz, 1 60 Hz (EURGB60), 2 ask at boot (the game's prompt)
	int palLanguage; // w13: European build: 0 English, 1 German, 2 French, 3 Spanish, 4 Italian
	// [window]
	int width;
	int height;
	bool hasPos;
	int x;
	int y;
	bool fullscreen;
	bool maximized;
	int aspect;          // PortAspect
	float internalScale; // 0 = window resolution, otherwise a multiple of 640x448
	int frameRate;       // display frames per second: 30 (original), 60, 120, 144,
	                     // PORT_FPS_DISPLAY or PORT_FPS_UNLIMITED (port_interp.cpp)
	int hudPosition;     // w11: widescreen HUD: 0 = 4:3 centred (original), 1 = edges of a 16:9
	                     //      area, 2 = edges of the screen (match the aspect)
	// [graphics] (w6: optional enhancements, defaults = GameCube-accurate rendering)
	int msaa;            // w6: 1 = off; 4 = 4x MSAA (WebGPU: 1 or 4 only). Next start.
	int anisotropy;      // w6: 0 = per texture (GX request); 1..16 = forced maximum
	bool trilinear;      // w6: blend between mip levels (linear mip filter) for every mipmapped texture
	bool fxaa;           // w6: post-process anti-aliasing in the present pass
	int brightness;      // w6: percent, 100 = original
	int gamma;           // w6: percent, 100 = original (output = input^(100/gamma))
	bool copyFilter;     // w6: GXSetCopyFilter's vertical filter on texture copies (true = GameCube)
	bool vsync;          // w6: present with vsync (false = as before: VI pacing only)
	int backend;         // w6: PortBackend (D3D12 default). Next start.
	// [input]
	bool gcLayout;       // PlayStation/generic pads: Cross=A, Square=B, Circle=X, Triangle=Y
	int stickDeadzone;   // percent, extra radial-per-axis dead zone before PADClamp (0 = GC only)
	int cstickDeadzone;  // percent
	bool linearTriggers; // analog L/R rise linearly up to the digital click point
	int rumble;          // percent, 0..100
	bool keyboard;       // keyboard drives pad 1 (in addition to a controller)
	int softTilt;        // percent of full stick tilt while the soft key is held
	bool mouse;          // mouse: camera (C-stick), left R, right L, middle Y (port_mouse.cpp)
	int mouseSensitivity; // 1..100
	bool mouseInvertY;
	bool backgroundInput;
	// DualSense / DualShock 4 extras (port_padfx.cpp)
	int adaptiveTriggers;   // PortAdaptiveTriggers (DualSense only)
	int triggerStrength;    // percent, 0..100
	int lightbar;           // PortLightbar (DualSense, DualShock 4)
	int lightbarBrightness; // percent, 0..100
	unsigned char lightbarColor[3]; // the static colour
	int rumbleMode;         // PortRumbleMode (strength: rumble)
	bool hdFludd, hdMovement, hdWorld, hdDamage; // HD rumble categories
	// [audio] controller speaker (port_speaker.cpp)
	int speakerMode;   // PortSpeaker
	int mainMix;       // PortMainMix
	int speakerVolume; // percent
	// [qol] quality-of-life options (port_qol.cpp), all off by default
	bool qolSkipIntro, qolSkipCutscenes;
	int qolTextSpeed; // 0 normal, 1 fast, 2 instant
	bool qolFastForward;
	int qolFfKey, qolFfSpeed; // SDL scancode (Tab), 2 or 4
	bool qolAutoSave, qolContinueShine, qolPauseOnFocusLoss;
	bool qolCamInvertX, qolCamInvertY;
	bool qolFreeCamera; // the camera stays where the player turns it (no auto turn behind Mario)
	int qolFreeCamZoom; // free camera: distance in percent of the game's (mouse wheel), 50..250
	int qolCamSpeed; // percent
	int qolVolMusic, qolVolSfx, qolVolVoice; // percent
	bool qolBlueCoinTracker, qolShineChecklist;
	int qolTrackerKey; // SDL scancode (F4)
	bool qolCheatLives, qolCheatWater;
	// [audio] output (w5): PORT_AUDIO_* (port_audio.h): 0 stereo, 1 original (Pro Logic II matrix), 2 5.1, 3 7.1
	int audioOutput;
	// [keyboard] SDL scancodes
	int keys[PORT_KEY_COUNT];
	// [debug]
	bool overlay;
	bool frameStats;
	bool record; // input recording for replays (port_replay.cpp)
	// [bugreport] (w12, port_bugreport.cpp): rolling capture of the last `bugReportSeconds` of
	// picture + sound + log, saved by F9 / the menu / a crash into bug_reports\ next to the exe.
	bool bugReport;
	int bugReportSeconds;               // 5..60
	int bugReportWidth, bugReportHeight; // video size; height 0 = keep the window's aspect
	int bugReportFps;                    // 10..60
	// [discord] (port_discord.cpp): Rich Presence + invites over Discord's local IPC pipe
	bool discordEnabled;
	char discordClientId[32]; // optional override of the built-in Discord Application ID; empty = built-in
	bool discordShowStage;    // false: only "Playing Super Mario Sunshine"
	bool discordAllowJoin;    // publish a join secret and accept Discord joins / ask-to-join
};

extern PortSettings gPortSettings;

extern "C" {
// Loads port_settings.ini (creating it with defaults), applies the window settings to
// `config` and sets up the PAD defaults. Call before aurora_initialize().
void port_settings_configure(AuroraConfig* config);
// Writes port_settings.ini now.
void port_settings_save(void);
// Marks the settings changed; port_window_frame() saves them a moment later.
void port_settings_dirty(void);

// PADRead + GameCube semantics + keyboard pad. Replaces PADRead in port_PADRead.
u32 port_input_read(PADStatus* status);
// PAD defaults that must be set before PADInit (port_settings_configure calls it).
void port_input_configure(void);
// True while SMS_PORT_KEYSCRIPT holds this SDL scancode (test knob, port_input.cpp).
bool port_input_script_key(int scancode);

// Main thread, once per VI retrace, from VIWaitForRetrace. `presented` is true when a
// new Aurora frame was just begun (the overlay is drawn into it).
void port_window_frame(bool presented);
// The same for an extra (interpolated) display frame between retraces: counts a present
// but no retrace (port_interp.cpp).
void port_window_display_frame(void);

// Frame-rate option (port_interp.cpp): display frames between the 30 Hz logic frames.
// Effective display rate setting (SMS_PORT_FPS overrides gPortSettings.frameRate).
int port_interp_rate(void);
bool port_interp_override(void);
double port_interp_display_hz(void);     // the window's monitor refresh rate
double port_interp_effective_fps(void);  // after w6's VSync cap (0 = unlimited)
// Logic (game) frames drawn so far, for the overlay.
unsigned port_interp_logic_frames(void);
// Pacing, from VIWaitForRetrace (main thread; times are QueryPerformanceCounter values).
bool port_interp_active(void);    // extra display frames are being drawn (gameplay, rate > 30)
bool port_interp_drawing(void);   // inside one
bool port_interp_interpolating(void); // inside its interpolated view pass / draw lists (BOOTLOG "Interpolation: ownership and hooks")
float port_interp_alpha(void);        // position between the two logic states shown (0..1)
void port_interp_retrace(long long retraceQpc);
long long port_interp_due(void);
long long port_interp_lead(void);
bool port_interp_near(long long due, long long t);
bool port_interp_fits(long long now, long long deadline);
void port_interp_skip(long long now);
void port_interp_defer(long long retrace);
void port_interp_logic_presented(long long now);
bool port_interp_render(long long now, bool logicPending);
void port_interp_presented(long long start, long long end);
// Re-applies aspect lock / viewport policy / internal resolution from gPortSettings.
void port_window_apply_display(void);
void port_window_set_fullscreen(bool on);
// w6: MSAA sample count Aurora was started with (port_settings_configure); the menu shows
// "restart required" while it differs from gPortSettings.msaa.
int port_settings_active_msaa(void);
// w6: horizontal widening of the 3D view for the aspect setting: display aspect / (4/3).
// 1.0 in the 4:3, fit and stretch modes (GameCube picture). Any thread; cheap. The game side
// (w11) multiplies the camera's projection aspect and its culling by it, and squeezes the
// 4:3 HUD by 1/scale.
float port_get_aspect_scale(void);
// w6: display aspect of the picture (4/3 in the GameCube modes; 0 = stretch to the window).
float port_get_display_aspect(void);
// w6: graphics backend (port_gfx_backend.cpp). configure: from port_settings_configure;
// arm: port_main.cpp right before aurora_initialize; frame: port_window_frame (presented).
int port_gfx_backend_parse(const char* v); // ini name -> PortBackend, -1 if unknown
const char* port_gfx_backend_ini(int backend);
const char* port_gfx_backend_label(int backend);
void port_gfx_backend_configure(AuroraConfig* config, const char* settingsPath);
void port_gfx_backend_arm(void);
void port_gfx_backend_frame(void);
int port_gfx_backend_active(void);         // PortBackend running, -1 = Null/unknown
const char* port_gfx_backend_status(void); // "Running on Vulkan (...)"

// Settings menu (port_menu.cpp, Dear ImGui): Esc (or F1) or the controller's Back/Create/View button.
void port_menu_toggle(void);
void port_menu_init_fonts(void); // port_menu.cpp: the menu's fonts, right after Aurora starts ImGui
bool port_menu_is_open(void);
bool port_menu_key_capture(void); // a Rebind button waits for a key (Esc cancels it)
bool port_menu_text_input(void);  // ImGui has a text field focused (chat, names)
// Draws the menu (if open) and the overlay text (if not null) into the current Aurora frame.
void port_menu_draw(const char* overlayText);

// DualSense / DualShock 4 adaptive triggers and lightbar (port_padfx.cpp). Main thread,
// from port_input_read once per game frame (reads game state only).
void port_padfx_frame(void);
// Puts the triggers back to no effect (before SDL shuts down; port_main.cpp window close).
void port_padfx_shutdown(void);
// Text for the menu: which controller features are active.
const char* port_padfx_status(void);
// HD rumble and DualSense haptics (port_haptics.cpp): main thread, once per game frame.
void port_haptics_frame(void);
void port_haptics_shutdown(void);
const char* port_haptics_status(void);
// Controller speaker (port_speaker.cpp): main thread, once per game frame.
void port_speaker_frame(void);
const char* port_speaker_status(void);
// Quality-of-life options (port_qol.cpp). port_qol_frame: main thread, once per game frame.
void port_qol_frame(void);
void port_qol_menu(void);  // the F1 tab
void port_qol_draw(void);  // trackers, fast-forward badge, toasts (ImGui frame)
int port_qol_ff_speed(void);
int port_qol_should_pause(void);
void port_qol_set_paused(int on);
void port_qol_toast(const char* text);
// SMS_PORT_VPAD test knob: a virtual DualSense / DualShock 4 (port_padfx.cpp).
bool port_vpad_requested(unsigned* vid, unsigned* pid);
void port_vpad_update(void);

// INI name of a PortKey ("stick_up", "a", ...).
const char* port_settings_key_id(int key);
}

#endif
