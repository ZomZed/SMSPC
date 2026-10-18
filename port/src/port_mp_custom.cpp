// Character customisation menu (task board P4, w15): presets, per-part colours, saved
// palettes, live preview on the local Mario. Palette model: port_mp_palette.cpp.
//
// Where it appears:
//  - F4 anywhere (single-player too, file select or a stage), or SMS_PORT_KEYSCRIPT=...:F4;
//  - (not in the F2 lobby panel: the title screen's Customize block opens the editor, or F4);
//  - w10's "Character" page can embed the editor with one call, port_mp_custom_draw().
// While the window is open the game gets idle pads (port_mp_request_input) and ImGui's
// keyboard/controller navigation is on, like w9's settings menu.
//
// Every edit previews at once on the local Mario (port_mp_palette_preview); "Apply" makes
// it the player's palette (port_mp_palette_set_local: published with port_mp_set_character
// in a session) and saves it; closing without Apply puts the applied palette back.
// port_characters.ini (next to port_settings.ini, or SMS_PORT_CHAR_INI) keeps the applied
// palette and the saved ones. Default: Classic, i.e. the game's own colours untouched.
#ifdef SMS_PORT_AURORA

#include "port_mp_puppet.h"
#include "port_mp.h"
#include "port_settings.h"

#include <SDL3/SDL_scancode.h>

#include "port_mp_ui.h" // Sunshine-looking widgets (port_mp_ui.cpp); includes imgui
extern "C" void port_mp_ini_save(void); // port_mp_game.cpp (the username)
extern "C" int port_mp_emote_count(void);           // port_mp_emote.cpp
extern "C" const char* port_mp_emote_label(int id);
extern "C" int port_mp_emote_dance(int id);
extern "C" void port_mp_dance_preview(int id);       // port_mp_dance.cpp
extern "C" int port_mp_dance_previewing(void);
#include <cmath>
extern "C" int port_mp_block_preview_active(void);       // port_mp_block.cpp (3D preview)
extern "C" void port_mp_block_preview_rotate(float deg);

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" unsigned port_frame_count(void); // port_main.cpp

// w10's hooks (port_mp_menu.cpp); fallbacks keep this file linkable on its own.
extern "C" {
void port_mp_w15_no_lobby_section(const char*, PortMpLobbyFn) { }
void port_mp_w15_no_request_input(int) { }
}
#pragma comment(linker, "/alternatename:port_mp_register_lobby_section=port_mp_w15_no_lobby_section")
#pragma comment(linker, "/alternatename:port_mp_request_input=port_mp_w15_no_request_input")

namespace {

const char* const kPartNames[PORT_MP_PAL_SLOTS] = { "Cap",   "Shirt", "Overalls",          "Gloves",
	                                                "Shoes", "Skin",  "Hair and moustache" };
// What "Original" looks like, for the swatch diagram only.
const unsigned char kOriginal[PORT_MP_PAL_SLOTS][3] = {
	{ 225, 30, 30 }, { 225, 30, 30 }, { 40, 60, 200 }, { 245, 245, 245 },
	{ 130, 75, 30 }, { 250, 200, 160 }, { 70, 40, 20 },
};

uint8_t* part(PortMpCharacter& c, int s) { return c.colors[s]; } // colors[6] = hair
const uint8_t* part(const PortMpCharacter& c, int s) { return c.colors[s]; }
bool partSet(const PortMpCharacter& c, int s)
{
	const uint8_t* p = part(c, s);
	return (p[0] | p[1] | p[2]) != 0;
}

struct Saved {
	char name[32];
	PortMpCharacter c;
};

bool sInit;
bool sOpen;         // standalone window
bool sEmbedded;     // drawn inside w10's page this frame
bool sNavWasOn;
PortMpCharacter sEdit;   // what the editor shows / previews
PortMpCharacter sApplied;
uint32_t sTagFromIni; // tag=RRGGBB from port_characters.ini (| 1 << 24 when present)
std::vector<Saved> sSaved;
char sSaveName[32] = "My Mario";
int sPreset        = 0;

std::string iniPath()
{
	if (const char* e = getenv("SMS_PORT_CHAR_INI"))
		if (*e != '\0')
			return e;
	std::string base;
	if (const char* s = getenv("SMS_PORT_SETTINGS"))
		base = s;
	if (base.empty()) {
		char buf[MAX_PATH];
		DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
		base.assign(buf, n);
	}
	size_t slash    = base.find_last_of("\\/");
	std::string dir = slash == std::string::npos ? std::string(".") : base.substr(0, slash);
	return dir + "\\port_characters.ini";
}

// 7 parts x RRGGBB (000000 = original colour)
void toHex(const PortMpCharacter& c, char out[64])
{
	char* p = out;
	for (int s = 0; s < PORT_MP_PAL_SLOTS; ++s) {
		const uint8_t* v = part(c, s);
		p += snprintf(p, 8, "%02x%02x%02x", v[0], v[1], v[2]);
	}
	*p = '\0';
}

bool fromHex(const char* h, PortMpCharacter& c)
{
	memset(&c, 0, sizeof(c));
	for (int s = 0; s < PORT_MP_PAL_SLOTS; ++s)
		for (int k = 0; k < 3; ++k) {
			unsigned v;
			if (sscanf(h + s * 6 + k * 2, "%2x", &v) != 1)
				return false;
			part(c, s)[k] = (uint8_t)v;
		}
	return true;
}

// The emote locker: which emotes the emote wheel (G) shows and which four sit on the D-pad.
// Kept in port_characters.ini (emote_wheel = a bit per emote id, emote_dpad = up,right,down,left).
// Every emote is unlocked for now; emoteUnlocked is where a later shop / unlock check goes.
unsigned long long sEmoteWheel = ~0ull; // all of them
int sEmoteDpad[4]              = { 0, 4, 3, 2 }; // Shine pose, Pianta pose, Sleep, Aww
bool emoteUnlocked(int /*id*/) { return true; }

// The name-tag colour (PortMpCharacter.reserved[0..3], see port_mp.h).
bool tagSet(const PortMpCharacter& c) { return c.reserved[0] == 1; }
void setTag(PortMpCharacter& c, uint32_t rgb)
{
	c.reserved[0] = 1;
	c.reserved[1] = (uint8_t)(rgb >> 16);
	c.reserved[2] = (uint8_t)(rgb >> 8);
	c.reserved[3] = (uint8_t)rgb;
}
uint32_t tagOf(const PortMpCharacter& c) { return (uint32_t)c.reserved[1] << 16 | (uint32_t)c.reserved[2] << 8 | c.reserved[3]; }

// A bright random colour (a random hue), for a new player's name tag.
uint32_t randomTag()
{
	static unsigned sSeed = (unsigned)GetTickCount() * 2654435761u;
	sSeed            = sSeed * 1103515245u + 12345u;
	const float h    = (sSeed >> 8) % 360 / 60.0f;
	const float x    = 1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f);
	float r = 0, g = 0, b = 0;
	switch ((int)h) {
	case 0: r = 1; g = x; break;
	case 1: r = x; g = 1; break;
	case 2: g = 1; b = x; break;
	case 3: g = x; b = 1; break;
	case 4: r = x; b = 1; break;
	default: r = 1; b = x; break;
	}
	auto ch = [](float v) { return (uint32_t)(80 + v * 175); }; // never too dark to read
	return ch(r) << 16 | ch(g) << 8 | ch(b);
}

void saveIni()
{
	FILE* f = fopen(iniPath().c_str(), "wb");
	if (f == nullptr)
		return;
	char hx[64];
	fprintf(f, "# port_characters.ini: Mario colour palettes (character menu, F4).\n");
	fprintf(f, "# Parts: cap shirt overalls gloves shoes skin hair, RRGGBB each, 000000 = original.\n");
	toHex(sApplied, hx);
	fprintf(f, "current=%s\n", hx);
	if (tagSet(sApplied))
		fprintf(f, "tag=%06x\n", (unsigned)tagOf(sApplied)); // the name-tag colour
	fprintf(f, "emote_wheel=%llx\nemote_dpad=%d,%d,%d,%d\n", sEmoteWheel, sEmoteDpad[0], sEmoteDpad[1], sEmoteDpad[2],
	        sEmoteDpad[3]); // the emote locker
	for (size_t i = 0; i < sSaved.size(); ++i) {
		toHex(sSaved[i].c, hx);
		fprintf(f, "saved=%s:%s\n", hx, sSaved[i].name);
	}
	fclose(f);
}

void loadIni()
{
	FILE* f = fopen(iniPath().c_str(), "rb");
	if (f == nullptr)
		return;
	char line[256];
	while (fgets(line, sizeof(line), f) != nullptr) {
		line[strcspn(line, "\r\n")] = '\0';
		PortMpCharacter c;
		unsigned tag = 0;
		unsigned long long wheel = 0;
		int dp[4];
		if (strncmp(line, "emote_wheel=", 12) == 0 && sscanf(line + 12, "%llx", &wheel) == 1) {
			sEmoteWheel = wheel;
		} else if (strncmp(line, "emote_dpad=", 11) == 0
		           && sscanf(line + 11, "%d,%d,%d,%d", &dp[0], &dp[1], &dp[2], &dp[3]) == 4) {
			for (int k = 0; k < 4; k++)
				sEmoteDpad[k] = dp[k];
		} else if (strncmp(line, "tag=", 4) == 0 && sscanf(line + 4, "%6x", &tag) == 1) {
			sTagFromIni = tag | 0x1000000u;
		} else if (strncmp(line, "current=", 8) == 0 && fromHex(line + 8, c)) {
			sApplied = c;
		} else if (strncmp(line, "saved=", 6) == 0 && strlen(line) > 6 + 42 && line[6 + 42] == ':'
		           && fromHex(line + 6, c)) {
			Saved s;
			s.c = c;
			snprintf(s.name, sizeof(s.name), "%s", line + 6 + 43);
			sSaved.push_back(s);
		}
	}
	fclose(f);
}

void init()
{
	if (sInit)
		return;
	sInit = true;
	loadIni();
	// the name-tag colour: the saved one, or a random one for a new player (then saved)
	if (sTagFromIni != 0) {
		setTag(sApplied, sTagFromIni & 0xFFFFFF);
	} else {
		setTag(sApplied, randomTag());
		saveIni();
		std::fprintf(stderr, "[mp] character: new player, random name-tag colour %06x\n", (unsigned)tagOf(sApplied));
	}
	// SMS_PORT_PALETTE (port_mp_palette.cpp) wins over the file, for tests.
	const char* env = getenv("SMS_PORT_PALETTE");
	if (env == nullptr || env[0] == '\0') {
		{ // always: the name-tag colour rides along even with the classic palette
			port_mp_palette_set_local(&sApplied);
			std::fprintf(stderr, "[mp] character: palette from %s\n", iniPath().c_str());
		}
	} else {
		port_mp_palette_get_local(&sApplied);
	}
	sEdit = sApplied;
}

void apply()
{
	sApplied = sEdit;
	port_mp_palette_set_local(&sApplied);
	if (port_mp_active())
		port_mp_set_character(&sApplied);
	saveIni();
	std::fprintf(stderr, "[mp] character: palette applied%s\n", port_mp_active() ? " and sent" : "");
}

// Simple front view of Mario from the part colours (the live preview is the real Mario
// in the game behind the window).
void swatch(const PortMpCharacter& c, float w, float h)
{
	ImVec2 o = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##swatch", ImVec2(w, h));
	ImDrawList* dl = ImGui::GetWindowDrawList();
	auto col = [&](int s) {
		const uint8_t* v = partSet(c, s) ? part(c, s) : kOriginal[s];
		return IM_COL32(v[0], v[1], v[2], 255);
	};
	const float u = w / 10.0f, v = h / 16.0f;
	auto rect = [&](float x0, float y0, float x1, float y1, ImU32 cl, float r = 0.0f) {
		dl->AddRectFilled(ImVec2(o.x + x0 * u, o.y + y0 * v), ImVec2(o.x + x1 * u, o.y + y1 * v), cl, r);
	};
	dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), IM_COL32(20, 30, 50, 255), 6.0f);
	rect(3.0f, 0.6f, 7.0f, 2.2f, col(PORT_MP_PAL_CAP), 8.0f);    // cap
	rect(2.6f, 1.9f, 7.6f, 2.5f, col(PORT_MP_PAL_CAP), 3.0f);    // brim
	rect(3.3f, 2.5f, 6.7f, 5.2f, col(PORT_MP_PAL_SKIN), 6.0f);   // face
	rect(3.0f, 2.5f, 3.5f, 4.2f, col(PORT_MP_PAL_HAIR));          // sideburns
	rect(6.5f, 2.5f, 7.0f, 4.2f, col(PORT_MP_PAL_HAIR));
	rect(3.9f, 4.0f, 6.1f, 4.6f, col(PORT_MP_PAL_HAIR), 3.0f);   // moustache
	rect(2.6f, 5.4f, 7.4f, 8.6f, col(PORT_MP_PAL_SHIRT), 4.0f);  // shirt
	rect(1.2f, 5.6f, 2.6f, 9.4f, col(PORT_MP_PAL_SHIRT), 3.0f);  // sleeves
	rect(7.4f, 5.6f, 8.8f, 9.4f, col(PORT_MP_PAL_SHIRT), 3.0f);
	rect(0.9f, 9.3f, 2.8f, 10.9f, col(PORT_MP_PAL_GLOVES), 8.0f); // gloves
	rect(7.2f, 9.3f, 9.1f, 10.9f, col(PORT_MP_PAL_GLOVES), 8.0f);
	rect(3.0f, 7.0f, 7.0f, 13.2f, col(PORT_MP_PAL_OVERALLS), 3.0f); // overalls
	rect(3.4f, 5.4f, 4.1f, 7.2f, col(PORT_MP_PAL_OVERALLS));         // straps
	rect(5.9f, 5.4f, 6.6f, 7.2f, col(PORT_MP_PAL_OVERALLS));
	dl->AddCircleFilled(ImVec2(o.x + 3.75f * u, o.y + 7.4f * v), u * 0.3f, IM_COL32(250, 220, 40, 255));
	dl->AddCircleFilled(ImVec2(o.x + 6.25f * u, o.y + 7.4f * v), u * 0.3f, IM_COL32(250, 220, 40, 255));
	rect(2.4f, 13.1f, 4.9f, 15.0f, col(PORT_MP_PAL_SHOES), 6.0f); // shoes
	rect(5.1f, 13.1f, 7.6f, 15.0f, col(PORT_MP_PAL_SHOES), 6.0f);
}

// Your name (as the others see it) and your name tag's colour, with a preview of the tag.
// Both apply at once (and are saved), unlike the Mario colours below (Apply).
void playerSection()
{
	static char sName[PORT_MP_NAME_MAX];
	static bool sNameInit;
	if (!sNameInit) {
		std::snprintf(sName, sizeof(sName), "%s", port_mp_get_name());
		sNameInit = true;
	}
	const float s = mpui_scale();
	mpui_heading("Player");
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
	if (ImGui::InputText("Name", sName, sizeof(sName), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::IsItemDeactivatedAfterEdit()) {
		port_mp_set_name(sName);
		std::snprintf(sName, sizeof(sName), "%s", port_mp_get_name());
		port_mp_ini_save();
	}
	ImGui::SameLine();
	uint32_t tag = tagOf(sApplied);
	float f[3]   = { (tag >> 16 & 0xFF) / 255.0f, (tag >> 8 & 0xFF) / 255.0f, (tag & 0xFF) / 255.0f };
	bool tagChanged = ImGui::ColorEdit3("Tag", f, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Uint8);
	if (tagChanged)
		tag = (uint32_t)(f[0] * 255.0f + 0.5f) << 16 | (uint32_t)(f[1] * 255.0f + 0.5f) << 8 | (uint32_t)(f[2] * 255.0f + 0.5f);
	ImGui::SameLine();
	if (ImGui::SmallButton("Random##tag")) {
		tag        = randomTag();
		tagChanged = true;
	}
	if (tagChanged) {
		setTag(sApplied, tag);
		setTag(sEdit, tag);
		port_mp_palette_set_local(&sApplied); // published with the character (port_mp_palette.cpp)
		saveIni();
	}
	// the tag as the others see it: your name in the game's font, in your colour
	ImGui::SameLine(0.0f, 14.0f * s);
	const ImVec2 p = ImGui::GetCursorScreenPos();
	const ImU32 c  = IM_COL32(tag >> 16 & 0xFF, tag >> 8 & 0xFF, tag & 0xFF, 255);
	const char* nm = sName[0] ? sName : "Player";
	mpui_text(ImGui::GetWindowDrawList(), ImVec2(p.x, p.y + 1.0f * s), 15.0f * s, c, nm);
	ImGui::Dummy(ImVec2(mpui_text_size(nm, 15.0f * s).x, 15.0f * s));
}

// The emote locker (the Customize menu's Emotes tab).
void emoteLocker()
{
	const int n = port_mp_emote_count();
	mpui_heading("Emote wheel");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Ticked emotes show on the emote wheel (G). Dances can be previewed on Mario.");
	const ImVec2 box(0.0f, ImGui::GetFrameHeightWithSpacing() * 4.3f); // scrolls: the list is long
	if (ImGui::BeginTable("##locker", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
	                      box)) {
		ImGui::TableSetupColumn("Emote", ImGuiTableColumnFlags_WidthStretch, 3.0f);
		ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthStretch, 1.4f);
		ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.6f);
		for (int i = 0; i < n && i < 64; i++) {
			ImGui::PushID(i);
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			const bool unlocked = emoteUnlocked(i);
			bool on             = unlocked && (sEmoteWheel >> i & 1ull) != 0;
			ImGui::BeginDisabled(!unlocked);
			if (ImGui::Checkbox(port_mp_emote_label(i), &on)) {
				sEmoteWheel = on ? (sEmoteWheel | 1ull << i) : (sEmoteWheel & ~(1ull << i));
				saveIni();
			}
			ImGui::EndDisabled();
			ImGui::TableNextColumn();
			const int dance = port_mp_emote_dance(i);
			ImGui::TextDisabled(!unlocked ? "Locked" : dance >= 0 ? "Dance" : "Pose");
			ImGui::TableNextColumn();
			if (dance >= 0 && unlocked) {
				const bool playing = port_mp_dance_previewing() == dance;
				if (ImGui::SmallButton(playing ? "Stop" : "Preview"))
					port_mp_dance_preview(playing ? -1 : dance);
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
	mpui_heading("D-pad");
	static const char* const kDir[4] = { "Up", "Right", "Down", "Left" };
	for (int d = 0; d < 4; d++) {
		ImGui::PushID(100 + d);
		if (d & 1)
			ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
		const int cur = sEmoteDpad[d] >= 0 && sEmoteDpad[d] < n ? sEmoteDpad[d] : 0;
		if (ImGui::BeginCombo(kDir[d], port_mp_emote_label(cur))) {
			for (int i = 0; i < n; i++)
				if (emoteUnlocked(i) && ImGui::Selectable(port_mp_emote_label(i), i == cur)) {
					sEmoteDpad[d] = i;
					saveIni();
				}
			ImGui::EndCombo();
		}
		ImGui::PopID();
	}
}

// The editor (shared by the standalone window and w10's page). Returns 1 on Back/Close.
int editor()
{
	init();
	int close        = 0;
	bool changed     = false;
	if (sEmbedded) // w10's page / the customization block: the menus' title plate
		mpui_title("Customize");
	playerSection();
	// two tabs: Mario's colours and the emote locker
	bool coloursTab = false;
	if (ImGui::BeginTabBar("##customtabs")) {
		if (ImGui::BeginTabItem("Colours")) {
			coloursTab = true;
			ImGui::EndTabItem();
		}
		// SMS_PORT_CUSTOM_TAB=emotes (debug, screenshots): open on the Emotes tab once
		static bool sTabEnv = std::getenv("SMS_PORT_CUSTOM_TAB") != nullptr;
		if (ImGui::BeginTabItem("Emotes", nullptr, sTabEnv ? ImGuiTabItemFlags_SetSelected : 0)) {
			sTabEnv = false;
			emoteLocker();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	if (!coloursTab && port_mp_dance_previewing() < 0)
		port_mp_palette_preview(&sEdit);
	if (coloursTab) {
	if (port_mp_dance_previewing() >= 0)
		port_mp_dance_preview(-1); // the colours tab: Mario stands still again
	const float avail = ImGui::GetContentRegionAvail().x;
	ImGui::BeginGroup();
	const bool compact = port_mp_block_preview_active() != 0; // beside the 3D preview
	mpui_heading("Presets", compact ? avail * 0.36f : avail * 0.42f);
	const float leftW = port_mp_block_preview_active() ? avail * 0.36f : avail * 0.42f;
	if (ImGui::BeginListBox("##presets", ImVec2(leftW, ImGui::GetTextLineHeightWithSpacing() * 4.5f))) {
		for (int i = 0; i < port_mp_palette_preset_count(); ++i)
			if (ImGui::Selectable(port_mp_palette_preset_name(i), sPreset == i)) {
				sPreset = i;
				port_mp_palette_preset(i, &sEdit);
				changed = true;
			}
		for (size_t i = 0; i < sSaved.size(); ++i) {
			char lbl[48];
			snprintf(lbl, sizeof(lbl), "* %s##saved%d", sSaved[i].name, (int)i);
			if (ImGui::Selectable(lbl, false)) {
				sEdit   = sSaved[i].c;
				sPreset = -1;
				changed = true;
			}
		}
		ImGui::EndListBox();
	}
	if (port_mp_block_preview_active()) {
		// the real Mario, in 3D, left of the menu: hold the arrows to turn him
		mpui_heading("Turn Mario", avail * 0.36f);
		const float bw = avail * 0.2f;
		mpui_button("##turnl", MPUI_ICON_BACK, bw);
		if (ImGui::IsItemActive())
			port_mp_block_preview_rotate(-120.0f * ImGui::GetIO().DeltaTime); // 120 degrees a second
		ImGui::SameLine();
		mpui_button("##turnr", MPUI_ICON_PLAY, bw);
		if (ImGui::IsItemActive())
			port_mp_block_preview_rotate(120.0f * ImGui::GetIO().DeltaTime);
		ImGui::TextDisabled("Or drag Mario / triggers");
	} else {
		ImGui::Dummy(ImVec2(1, 4.0f * mpui_scale()));
		swatch(sEdit, avail * 0.42f, avail * 0.27f);
	}
	ImGui::EndGroup();
	ImGui::SameLine();
	ImGui::BeginGroup();
	mpui_heading("Colours");
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Untick a part to keep its original colour.");
	if (!compact)
		ImGui::TextDisabled("Untick: keep the original.");
	for (int s = 0; s < PORT_MP_PAL_SLOTS; ++s) {
		ImGui::PushID(s);
		bool on = partSet(sEdit, s);
		if (ImGui::Checkbox("##on", &on)) {
			uint8_t* v = part(sEdit, s);
			if (on) {
				memcpy(v, kOriginal[s], 3);
			} else {
				v[0] = v[1] = v[2] = 0;
			}
			changed = true;
		}
		ImGui::SameLine();
		uint8_t* v = part(sEdit, s);
		float f[3] = { v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f };
		if (!on)
			for (int k = 0; k < 3; ++k)
				f[k] = kOriginal[s][k] / 255.0f;
		ImGui::BeginDisabled(!on);
		ImGui::SetNextItemWidth(avail * 0.31f);
		// beside the 3D preview the menu is narrower: swatch and name only (a click opens the picker)
		const ImGuiColorEditFlags cf = ImGuiColorEditFlags_Uint8 | ImGuiColorEditFlags_DisplayRGB
		                               | (port_mp_block_preview_active() ? ImGuiColorEditFlags_NoInputs : 0);
		if (ImGui::ColorEdit3(kPartNames[s], f, cf)) {
			for (int k = 0; k < 3; ++k) {
				int x = (int)(f[k] * 255.0f + 0.5f);
				v[k]  = (uint8_t)(x < 0 ? 0 : x > 255 ? 255 : x);
			}
			if ((v[0] | v[1] | v[2]) == 0)
				v[2] = 1; // pure black would read as "original"
			changed = true;
		}
		ImGui::EndDisabled();
		ImGui::PopID();
	}
	ImGui::Spacing();
	ImGui::SetNextItemWidth(avail * (port_mp_block_preview_active() ? 0.2f : 0.3f));
	ImGui::InputText("##name", sSaveName, sizeof(sSaveName));
	ImGui::SameLine();
	if (ImGui::Button("Save as")) {
		for (char* p = sSaveName; *p; ++p)
			if (*p == ':' || *p == '\n' || *p == '\r')
				*p = ' ';
		bool found = false;
		for (size_t i = 0; i < sSaved.size(); ++i)
			if (strcmp(sSaved[i].name, sSaveName) == 0) {
				sSaved[i].c = sEdit;
				found       = true;
			}
		if (!found && sSaved.size() < 32) {
			Saved sv;
			snprintf(sv.name, sizeof(sv.name), "%s", sSaveName);
			sv.c = sEdit;
			sSaved.push_back(sv);
		}
		saveIni();
	}
	ImGui::SameLine();
	if (ImGui::Button("Delete")) {
		for (size_t i = 0; i < sSaved.size(); ++i)
			if (strcmp(sSaved[i].name, sSaveName) == 0) {
				sSaved.erase(sSaved.begin() + i);
				break;
			}
		saveIni();
	}
	ImGui::EndGroup();
	} // the colours tab
	const float avail = ImGui::GetContentRegionAvail().x;
	ImGui::Dummy(ImVec2(1, 6.0f * mpui_scale()));
	if (mpui_button("Apply", MPUI_ICON_SHINE, avail * 0.3f, 1))
		apply();
	ImGui::SameLine();
	if (mpui_button("Revert", MPUI_ICON_REFRESH, avail * 0.3f)) {
		sEdit   = sApplied;
		changed = true;
	}
	ImGui::SameLine();
	if (mpui_button("Back", MPUI_ICON_BACK, avail * 0.3f))
		close = 1;
	if (!port_mp_block_preview_active())
		ImGui::TextDisabled(port_mp_active() ? "Apply: you and the other players see it."
		                                     : "Apply: your Mario keeps these colours.");
	ImGui::Dummy(ImVec2(1, 14.0f * mpui_scale())); // room for the water
	(void)changed;
	port_mp_palette_preview(&sEdit); // live on the local Mario while the editor is shown
	return close;
}

void setNav(bool on)
{
	ImGuiIO& io = ImGui::GetIO();
	if (on) {
		sNavWasOn = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	} else if (!sNavWasOn) {
		io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
	}
}


// SMS_PORT_CHAR_SCRIPT=<frame>:<preset>[,...] (test knob): applies a preset at a presented
// frame as if picked and applied in the menu. SMS_PORT_CHAR_OPEN=<frame>: opens the window.
void testScript()
{
	static const char* sScript = getenv("SMS_PORT_CHAR_SCRIPT");
	static const char* sOpenAt = getenv("SMS_PORT_CHAR_OPEN");
	const unsigned f           = port_frame_count();
	if (sOpenAt != nullptr && f >= (unsigned)atoi(sOpenAt)) {
		sOpenAt = nullptr;
		port_mp_custom_open(1);
	}
	while (sScript != nullptr && *sScript != '\0') {
		unsigned at = 0;
		int preset  = 0;
		if (sscanf(sScript, "%u:%d", &at, &preset) != 2) {
			sScript = nullptr;
			break;
		}
		if (f < at)
			break;
		port_mp_palette_preset(preset, &sEdit);
		sPreset = preset;
		apply();
		std::fprintf(stderr, "[mp] character: SMS_PORT_CHAR_SCRIPT preset %d at frame %u\n", preset, f);
		const char* c = strchr(sScript, ',');
		sScript       = c != nullptr ? c + 1 : nullptr;
	}
}

} // namespace

extern "C" void port_mp_custom_open(int open)
{
	if ((open != 0) == sOpen)
		return;
	init();
	sOpen = open != 0;
	port_mp_request_input(sOpen ? 1 : 0);
	setNav(sOpen);
	if (sOpen) {
		sEdit = sApplied;
	} else {
		port_mp_palette_preview(nullptr); // back to the applied palette
	}
	std::fprintf(stderr, "[mp] character menu %s\n", sOpen ? "opened" : "closed");
}

extern "C" int port_mp_custom_is_open(void) { return sOpen ? 1 : 0; }

extern "C" int port_mp_custom_draw(void)
{
	sEmbedded = true;
	const int close = editor();
	if (close)
		port_mp_palette_preview(nullptr);
	return close;
}

// Every presented frame (port_mp_overlay_draw, inside the ImGui frame).
extern "C" void port_mp_custom_frame(void)
{
	if (ImGui::GetCurrentContext() == nullptr)
		return;
	init();
	testScript();
	static bool sScriptF4;
	const bool f4s = port_input_script_key(SDL_SCANCODE_F4);
	if ((ImGui::IsKeyPressed(ImGuiKey_F4, false) || (f4s && !sScriptF4)) && !port_menu_is_open())
		port_mp_custom_open(!sOpen);
	sScriptF4 = f4s;
	const bool wasEmbedded = sEmbedded;
	sEmbedded              = false;
	if (!sOpen) {
		// w10's page stopped drawing the editor: drop its preview.
		static bool sPrevEmbedded;
		if (sPrevEmbedded && !wasEmbedded)
			port_mp_palette_preview(nullptr);
		sPrevEmbedded = wasEmbedded;
		return;
	}
	if (port_menu_is_open())
		return;
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f, 10.0f), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
	ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.42f, 0.0f), ImGuiCond_Always);
	bool open = true;
	if (ImGui::Begin("Character (F4)###mpchar", &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize)) {
		const float scale = io.DisplaySize.y / 760.0f;
		ImGui::SetWindowFontScale(scale < 1.0f ? 1.0f : scale > 2.5f ? 2.5f : scale);
		if (ImGui::IsWindowAppearing())
			ImGui::SetWindowFocus();
		if (editor())
			open = false;
	}
	ImGui::End();
	if (!open || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
		port_mp_custom_open(0);
}

// The emote locker, read by port_mp_emote.cpp (the D-pad) and port_mp_chat.cpp (the wheel).
extern "C" int port_mp_emote_in_wheel(int id)
{
	init();
	return id >= 0 && id < 64 && emoteUnlocked(id) && (sEmoteWheel >> id & 1ull) ? 1 : 0;
}
extern "C" int port_mp_emote_dpad_get(int dir)
{
	init();
	return dir >= 0 && dir < 4 ? sEmoteDpad[dir] : -1;
}

#endif // SMS_PORT_AURORA
