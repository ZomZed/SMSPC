// Sunshine-looking pieces for the multiplayer menus (Dear ImGui): the game's own font
// (gpSystemFont's glyph pages, drawn as textured quads), the blue file-select style panels,
// header plates, big tile buttons with drawn icons, status chips and a colour theme.
// port_mp_ui.cpp; used by port_mp_menu.cpp and port_mp_modes.cpp. Render thread only.
#pragma once

#define IMGUI_USER_CONFIG "aurora/imgui_config.h"
#include <imgui.h>

enum MpUiIcon {
	MPUI_ICON_NONE = 0,
	MPUI_ICON_SHINE,  // a Shine Sprite
	MPUI_ICON_FLAG,   // host (a goal flag)
	MPUI_ICON_SIGNAL, // join / online (signal arcs, like the file-select block)
	MPUI_ICON_CAP,    // character (Mario's cap)
	MPUI_ICON_BACK,   // an arrow back
	MPUI_ICON_CROWN,  // the host in a player list
	MPUI_ICON_LOCK,   // a password lobby
	MPUI_ICON_PLAY,   // resume
	MPUI_ICON_EXIT,   // leave
	MPUI_ICON_REFRESH,
	MPUI_ICON_SUN,  // the file select's loading sun (turning)
	MPUI_ICON_COIN,
};

// Text in the game's font. `px` is the line height in pixels.
ImVec2 mpui_text_size(const char* text, float px);
void mpui_text(ImDrawList* dl, ImVec2 pos, float px, ImU32 col, const char* text, bool shadow = true);
bool mpui_font_ready(void);
// Reads the menu icons from the game's own textures (game thread, once; port_mp_block.cpp).
void mpui_load_game_icons(void);

// The menu's scale (display height / 480, at least 1).
float mpui_scale(void);

// A blue file-select box (rounded, white inner line, corner rivets) from a to b.
void mpui_panel(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 fill, bool rivets = true);
// Draws one of the icons centred at c, `r` = radius.
void mpui_icon(ImDrawList* dl, int icon, ImVec2 c, float r, ImU32 col);

// Inside a window: the page title on a header plate, centred.
void mpui_title(const char* text);
// A section heading (game font, yellow) with a thin line.
void mpui_heading(const char* text, float width = 0.0f); // width 0: to the window's edge
// A big tile button: icon on the left, label in the game font, optional sub-line.
bool mpui_tile(const char* id, const char* label, const char* sub, int icon, ImVec2 size, ImU32 tint = 0);
// A wide game-font button (Resume, Leave, Create!...). `accent`: 0 blue, 1 green, 2 red, 3 gold.
bool mpui_button(const char* label, int icon, float width, int accent = 0);
// A coloured status chip with a dot.
void mpui_chip(const char* text, ImU32 dot);
// Two-way segmented toggle; returns true when changed.
bool mpui_toggle(const char* id, int* value, const char* a, const char* b);
// Signal bars for a ping (ms), drawn at the cursor.
void mpui_ping_bars(unsigned pingMs, bool lagging);

// The colour theme for the multiplayer windows (push before Begin, pop after End).
void mpui_push_theme(void);
void mpui_pop_theme(void);
// Background of the current window as a Sunshine panel, plus the animated water strip.
void mpui_window_background(void);

// A toast box (bottom left) in the game style.
void mpui_toast(ImDrawList* dl, ImVec2 pos, const char* text, float alpha);
