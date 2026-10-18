// Multiplayer menus (Dear ImGui through Aurora), w10. See port/MULTIPLAYER.md.
//
//   Main page    (file select: Mario runs off the left edge, or F2 there): username,
//                Create lobby, Join lobby (LAN list + direct IP), Character (stub).
//   Lobby panel  (in a session: F2, or Z in the pause menu): players, ping, stage,
//                game mode list and shared objectives (host), Leave.
// While a page is open the game gets idle pads (port_input.cpp asks
// port_mp_menu_wants_input) and ImGui's keyboard/gamepad navigation drives the menu
// (A = activate, B = back, D-pad/stick = move), like w9's settings menu.
#ifdef SMS_PORT_AURORA

#include "port_mp.h"
#include "port_settings.h"

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_scancode.h>

#include "port_mp_ui.h" // Sunshine-looking widgets (port_mp_ui.cpp); includes imgui

#include <atomic>
#include <deque>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

extern "C" int port_mp_custom_draw(void); // w15, port_mp_custom.cpp (character editor)
extern "C" void port_mp_modes_panel(int host); // port_mp_modes.cpp (game modes, queue, votes)
extern "C" int port_mp_block_active(void);    // port_mp_block.cpp (the file-select block)
extern "C" void port_mp_block_preview(int on); // ... the customization page's 3D preview
extern "C" void port_mp_block_preview_rotate(float deg);

// port_mp_game.cpp
extern "C" {
void port_mp_ini_save(void);
char* port_mp_pref_lobby_name(void);
int* port_mp_pref_max_players(void);
int* port_mp_pref_shared(void);
int* port_mp_pref_chat(void);
int* port_mp_pref_emotes(void);
int* port_mp_pref_port(void);
char* port_mp_pref_last_join(void);
int* port_mp_pref_last_join_port(void);
int* port_mp_pref_conn_type(void);
char* port_mp_pref_last_lobby(void);
char* port_mp_pref_server(void);
void port_mp_toast(const char* text);
int port_mp_file_select_hint(void);
void port_mp_menu_closed(void);
int port_mp_in_pause_menu(void);
const char* port_mp_lobby_name(void);
void port_mp_menu_feature_lobby(int isHost);
void port_mp_menu_feature_overlays(void);
}

namespace {

// PAGE_CHARACTER_ONLY: the character editor opened from the customization block (closing it
// goes back to the file select)
enum Page { PAGE_CLOSED = -1, PAGE_MAIN, PAGE_LOBBY, PAGE_CREATE, PAGE_JOIN, PAGE_CHARACTER, PAGE_CHARACTER_ONLY };

std::atomic<int> sOpenRequest{ -2 }; // page to open (from any thread), -2 = none
std::atomic<bool> sCloseRequest{ false }; // port_mp_menu_close
std::atomic<bool> sWantsInput{ false };
int sPage = PAGE_CLOSED;
bool sNavWasEnabled;
char sNameEdit[PORT_MP_NAME_MAX];
bool sFocusFirst;
double sLastRefresh;
bool sAutoLan; // SMS_PORT_MP_MENU_AUTO=lan

const char* const kModeNames[PORT_MP_MODE_COUNT] = { "None (Delfino Plaza only)", "Free (Delfino Plaza, every level open)", "Hide and Seek (one level or all levels)", "Freeze Tag (one level or all levels)", "Prop Hunt (one level or all levels)", "Manhunt (every level open)", "Shine Runners (every level open)", "Cops and Robbers (every level open)" };

const char* stageName(int stage)
{
	switch (stage) {
	case 0xFF: return "(loading / menu)";
	case 0: return "Delfino Airstrip";
	case 1: return "Delfino Plaza";
	case 2: return "Bianco Hills";
	case 3: return "Ricco Harbor";
	case 4: return "Gelato Beach";
	case 5: return "Pinna Park";
	case 6: return "Sirena Beach";
	case 7: return "Hotel Delfino";
	case 8: return "Pianta Village";
	case 9: return "Noki Bay";
	case 13: return "Pinna Park (inside)";
	case 15: return "File select";
	case 52: return "Corona Mountain";
	case 60: return "Bowser";
	default: return stage >= 20 && stage < 60 ? "Secret course" : "?";
	}
}

void setNav(bool open)
{
	ImGuiIO& io = ImGui::GetIO();
	if (open) {
		sNavWasEnabled = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
	} else if (!sNavWasEnabled && !port_menu_is_open()) {
		io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
	}
}

void openPage(int page)
{
	if (sPage == PAGE_CLOSED)
		setNav(true);
	sPage       = page;
	sFocusFirst = true;
	sWantsInput = true;
	std::snprintf(sNameEdit, sizeof(sNameEdit), "%s", port_mp_get_name());
	if (page == PAGE_JOIN) {
		port_mp_lan_refresh();
		sLastRefresh = port_mp_time_ms();
	}
	std::fprintf(stderr, "[mp] menu page %d\n", page);
}

void closeMenu()
{
	if (sPage == PAGE_CLOSED)
		return;
	sPage       = PAGE_CLOSED;
	sWantsInput = false;
	setNav(false);
	port_mp_menu_closed();
	std::fprintf(stderr, "[mp] menu closed\n");
}

bool backPressed()
{
	return !ImGui::IsAnyItemActive()
	       && (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false));
}

void focusFirst()
{
	if (sFocusFirst) {
		ImGui::SetKeyboardFocusHere();
		sFocusFirst = false;
	}
}

void statusLine()
{
	const int st = port_mp_state();
	ImVec4 col   = st == PORT_MP_FAILED ? ImVec4(1.0f, 0.45f, 0.4f, 1.0f) : ImVec4(0.7f, 0.85f, 1.0f, 1.0f);
	ImGui::TextColored(col, "%s", port_mp_status_text());
}

// ---------------------------------------------------------------------------------
// On-screen keyboard for controller users: every text field gets a small "Keys" button
// that opens a key grid (D-pad/stick + A), with Shift, Space, Delete and Done.
char* sOskBuf;
int sOskSize;
bool sOskOpen, sOskShift;
const char* sOskTitle = "";

bool textField(const char* label, char* buf, int size, ImGuiInputTextFlags flags = 0)
{
	ImGui::PushID(label);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
	bool changed = ImGui::InputText(label, buf, size, flags);
	ImGui::SameLine();
	if (ImGui::SmallButton("Keys")) {
		sOskBuf   = buf;
		sOskSize  = size;
		sOskOpen  = true;
		sOskShift = false;
		sOskTitle = label;
	}
	ImGui::PopID();
	return changed;
}

// Returns true when Done was pressed on the buffer `buf` (so the caller can apply it).
bool oskDraw()
{
	if (!sOskOpen)
		return false;
	bool done = false;
	ImGui::OpenPopup("Keyboard###osk");
	if (ImGui::BeginPopupModal("Keyboard###osk", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::Text("%s:", sOskTitle);
		ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "%s_", sOskBuf);
		static const char* const kRows[] = { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm-_.", "!?&#()+*@/" };
		const float kw = ImGui::GetFontSize() * 1.8f;
		for (int r = 0; r < 5; r++) {
			for (int c = 0; kRows[r][c] != '\0'; c++) {
				char ch = kRows[r][c];
				if (sOskShift && ch >= 'a' && ch <= 'z')
					ch = (char)(ch - 'a' + 'A');
				char lab[8] = { ch, '\0' };
				if (c > 0)
					ImGui::SameLine();
				ImGui::PushID(r * 16 + c);
				if (r == 0 && c == 0 && ImGui::IsWindowAppearing())
					ImGui::SetKeyboardFocusHere();
				if (ImGui::Button(lab, ImVec2(kw, 0.0f))) {
					const int n = (int)std::strlen(sOskBuf);
					if (n < sOskSize - 1) {
						sOskBuf[n]     = ch;
						sOskBuf[n + 1] = '\0';
					}
				}
				ImGui::PopID();
			}
		}
		if (ImGui::Button(sOskShift ? "abc" : "ABC", ImVec2(kw * 2.0f, 0.0f)))
			sOskShift = !sOskShift;
		ImGui::SameLine();
		if (ImGui::Button("Space", ImVec2(kw * 4.0f, 0.0f))) {
			const int n = (int)std::strlen(sOskBuf);
			if (n < sOskSize - 1) {
				sOskBuf[n]     = ' ';
				sOskBuf[n + 1] = '\0';
			}
		}
		ImGui::SameLine();
		if (ImGui::Button("Delete", ImVec2(kw * 2.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {
			const int n = (int)std::strlen(sOskBuf);
			if (n > 0)
				sOskBuf[n - 1] = '\0';
		}
		ImGui::SameLine();
		if (ImGui::Button("Done", ImVec2(kw * 2.0f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false)
		    || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
			sOskOpen = false;
			done     = true;
			ImGui::CloseCurrentPopup();
		}
		ImGui::TextDisabled("A: type   X: delete   B / Start: done");
		ImGui::EndPopup();
	}
	return done;
}

// Copy-to-clipboard button with a short "Copied!" confirmation.
double sCopiedAt = -1e9;
bool sCopyTestNow; // SMS_PORT_MP_MENU_AUTO=copytest: press the first copy button once
char sCopiedWhat[128];

void copyButton(const char* id, const char* text)
{
	ImGui::SameLine();
	ImGui::PushID(id);
	const bool justCopied = port_mp_time_ms() - sCopiedAt < 1500.0 && std::strcmp(sCopiedWhat, text) == 0;
	if (ImGui::SmallButton(justCopied ? "Copied!" : "Copy") || sCopyTestNow) {
		sCopyTestNow = false;
		SDL_SetClipboardText(text);
		std::snprintf(sCopiedWhat, sizeof(sCopiedWhat), "%s", text);
		sCopiedAt = port_mp_time_ms();
		std::fprintf(stderr, "[mp] copied to the clipboard: %s\n", text);
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Copy to the clipboard");
	ImGui::PopID();
}

void serverStatusLine()
{
	static double sAsked = -1e9;
	if (port_mp_time_ms() - sAsked > 3000.0) {
		sAsked = port_mp_time_ms();
		port_mp_server_refresh();
	}
	PortMpServerStatus st;
	port_mp_server_status(&st);
	char t[96];
	switch (st.state) {
	case PORT_MP_SERVER_ONLINE:
		std::snprintf(t, sizeof(t), "Lobby server online  -  %d lobb%s open", st.lobbies, st.lobbies == 1 ? "y" : "ies");
		mpui_chip(t, IM_COL32(90, 230, 100, 255));
		break;
	case PORT_MP_SERVER_OFFLINE:
		mpui_chip("Lobby server offline  -  this network only", IM_COL32(255, 140, 60, 255));
		break;
	case PORT_MP_SERVER_UNSET:
		mpui_chip("No lobby server set up  -  this network only", IM_COL32(255, 210, 60, 255));
		break;
	default:
		mpui_chip("Checking the lobby server...", IM_COL32(160, 180, 235, 255));
		break;
	}
}

// Lobby server (join by name) or direct IP, as a two-way switch.
void connTypeChooser()
{
	int* ct = port_mp_pref_conn_type();
	int v   = *ct == PORT_MP_CONN_DIRECT ? 1 : 0;
	if (mpui_toggle("conntype", &v, "By lobby name", "By IP address")) {
		*ct = v == 1 ? PORT_MP_CONN_DIRECT : PORT_MP_CONN_SERVER;
		port_mp_ini_save();
	}
}

void nameField()
{
	if (textField("Username", sNameEdit, sizeof(sNameEdit), ImGuiInputTextFlags_EnterReturnsTrue)
	    || ImGui::IsItemDeactivatedAfterEdit()) {
		port_mp_set_name(sNameEdit);
		std::snprintf(sNameEdit, sizeof(sNameEdit), "%s", port_mp_get_name());
		port_mp_ini_save();
	}
	ImGui::SameLine();
	if (ImGui::SmallButton("Random")) {
		static const char* const kNames[] = { "Mario", "Luigi", "Pianta", "Noki", "Il Piantissimo", "Shine",
			                                  "Blooper", "Cataquack", "Petey", "Gooble", "Strollin Stu", "Yoshi" };
		static unsigned sIdx;
		sIdx = (sIdx + 1 + (unsigned)port_mp_time_ms()) % (sizeof(kNames) / sizeof(kNames[0]));
		char n[PORT_MP_NAME_MAX];
		std::snprintf(n, sizeof(n), "%s %u", kNames[sIdx], (unsigned)port_mp_time_ms() % 100);
		port_mp_set_name(n);
		std::snprintf(sNameEdit, sizeof(sNameEdit), "%s", port_mp_get_name());
		port_mp_ini_save();
	}
}

// A player's colour (the same as the top-right player list, port_mp_modes.cpp slotColour).
extern "C" uint32_t port_mp_tag_rgb(int slot); // port_mp_palette.cpp
ImU32 slotColour(int slot)
{
	const uint32_t rgb = port_mp_tag_rgb(slot); // the player's name-tag colour (Customize)
	return IM_COL32(rgb >> 16 & 0xFF, rgb >> 8 & 0xFF, rgb & 0xFF, 255);
}

// One lobby in a browser list, as a card: name (game font), host, player pips, lock.
// Returns true when it was picked and can be joined.
bool lobbyCard(int id, const PortMpLobbyInfo& l, bool showAddress)
{
	const float s  = mpui_scale();
	const float fs = ImGui::GetFontSize();
	const float w  = ImGui::GetContentRegionAvail().x;
	const float h  = fs * 3.5f;
	ImGui::PushID(id);
	const bool pressed = ImGui::InvisibleButton("card", ImVec2(w, h));
	const bool hot     = ImGui::IsItemHovered() || ImGui::IsItemFocused();
	ImGui::PopID();
	const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const bool full = l.players >= l.maxPlayers, ok = l.protocolOk && !full;
	mpui_panel(dl, a, b, !ok ? IM_COL32(60, 70, 120, 255) : hot ? IM_COL32(70, 104, 255, 255) : IM_COL32(36, 68, 238, 255), false);
	if (hot && ok)
		dl->AddRect(ImVec2(a.x - 3 * s, a.y - 3 * s), ImVec2(b.x + 3 * s, b.y + 3 * s), IM_COL32(255, 222, 60, 255), 12 * s, 0, 3 * s);
	const float px = 16.0f * s;
	float x        = a.x + 12 * s;
	if (l.hasPassword) {
		mpui_icon(dl, MPUI_ICON_LOCK, ImVec2(x + 7 * s, a.y + h * 0.36f), 7 * s, IM_COL32(255, 222, 60, 255));
		x += 20 * s;
	}
	mpui_text(dl, ImVec2(x, a.y + 8 * s), px, hot && ok ? IM_COL32(255, 222, 60, 255) : IM_COL32(255, 255, 255, 255), l.name);
	char sub[96];
	if (showAddress)
		std::snprintf(sub, sizeof(sub), "Host: %s   (%s:%u)", l.hostName, l.address, l.port);
	else
		std::snprintf(sub, sizeof(sub), "Host: %s", l.hostName);
	dl->AddText(ImVec2(a.x + 14 * s, a.y + 11 * s + px), IM_COL32(205, 220, 255, 255), sub);
	// player pips (right)
	const float pr = fs * 0.28f;
	float px0      = b.x - 14 * s - l.maxPlayers * pr * 2.6f;
	for (int i = 0; i < l.maxPlayers; i++) {
		const ImVec2 c(px0 + i * pr * 2.6f + pr, a.y + h * 0.36f);
		if (i < l.players)
			dl->AddCircleFilled(c, pr, slotColour(i));
		else
			dl->AddCircle(c, pr, IM_COL32(255, 255, 255, 120), 0, 1.2f * s);
	}
	const char* state = !l.protocolOk ? "Other version" : full ? "Full" : "Join!";
	const ImVec2 ss   = ImGui::CalcTextSize(state);
	dl->AddText(ImVec2(b.x - 14 * s - ss.x, a.y + h * 0.62f), ok ? IM_COL32(255, 222, 60, 255) : IM_COL32(255, 150, 120, 255),
	            state);
	return pressed && ok;
}

// SMS_PORT_MP_MENU_AUTO=host[:port] | join:<ip>[:port] (debug, unattended tests): two
// seconds after the main page opens, press "Create and start" / "Join" with these values.
void menuAuto()
{
	static const char* sAuto = std::getenv("SMS_PORT_MP_MENU_AUTO");
	static double sAt;
	static bool sDone;
	if (sAuto == nullptr || sDone)
		return;
	if (sAt == 0.0)
		sAt = port_mp_time_ms() + 2000.0;
	if (port_mp_time_ms() < sAt)
		return;
	sDone = true;
	std::fprintf(stderr, "[mp] SMS_PORT_MP_MENU_AUTO: %s\n", sAuto);
	if (std::strncmp(sAuto, "host", 4) == 0) {
		PortMpSessionSettings s;
		port_mp_get_settings(&s);
		s.maxPlayers = (uint8_t)*port_mp_pref_max_players();
		int port     = sAuto[4] == ':' ? std::atoi(sAuto + 5) : *port_mp_pref_port();
		if (port_mp_host(port_mp_pref_lobby_name(), port, &s))
			closeMenu();
	} else if (std::strncmp(sAuto, "join:", 5) == 0) {
		char host[96];
		std::snprintf(host, sizeof(host), "%s", sAuto + 5);
		int port    = PORT_MP_DEFAULT_PORT;
		char* colon = std::strrchr(host, ':');
		if (colon != nullptr) {
			*colon = '\0';
			port   = std::atoi(colon + 1);
		}
		port_mp_join(host, port);
		sPage = PAGE_JOIN; // shows "Connecting..." and closes once accepted
	} else if (std::strcmp(sAuto, "joinpage") == 0 || std::strcmp(sAuto, "osk") == 0) {
		openPage(PAGE_JOIN); // screenshots of the join page / the on-screen keyboard
		if (sAuto[0] == 'o') {
			sOskBuf   = port_mp_pref_last_lobby();
			sOskSize  = PORT_MP_LOBBY_NAME_MAX;
			sOskTitle = "Lobby name";
			sOskOpen  = true;
		}
	} else if (std::strcmp(sAuto, "lan") == 0) {
		sAutoLan = true; // the join page joins the first LAN lobby it lists
		openPage(PAGE_JOIN);
	}
}

void pageMain()
{
	menuAuto();
	const float s = mpui_scale();
	mpui_title("Multiplayer");
	mpui_heading("Play"); // (your name and colours: the Customize block)
	focusFirst();
	const float W  = ImGui::GetContentRegionAvail().x;
	const float sp = ImGui::GetStyle().ItemSpacing.x;
	const ImVec2 big((W - sp) * 0.5f, 70.0f * s);
	if (mpui_tile("host", "Host a Lobby", "Start a game for friends", MPUI_ICON_FLAG, big))
		openPage(PAGE_CREATE);
	ImGui::SameLine();
	if (mpui_tile("join", "Join a Lobby", "Find a friend's game", MPUI_ICON_SIGNAL, big))
		openPage(PAGE_JOIN);
	ImGui::Dummy(ImVec2(1, 4 * s));
	serverStatusLine();
	if (ImGui::TreeNode("Advanced: lobby server address")) {
		static char sServer[128];
		if (sServer[0] == '\0')
			std::snprintf(sServer, sizeof(sServer), "%s", port_mp_pref_server());
		textField("host[:port]", sServer, sizeof(sServer));
		if (ImGui::SmallButton("Apply")) {
			std::snprintf(port_mp_pref_server(), 128, "%s", sServer);
			port_mp_set_server(sServer);
			port_mp_ini_save();
		}
		ImGui::TextDisabled("See port/server/DEPLOY.md to run your own (free) lobby server.");
		ImGui::TreePop();
	}
	statusLine();
	ImGui::Dummy(ImVec2(1, 2 * s));
	if (mpui_button("Back", MPUI_ICON_BACK, 150.0f * s) || (backPressed() && !sOskOpen))
		closeMenu();
	ImGui::Dummy(ImVec2(1, 14 * s)); // room for the water
	oskDraw();
}

void pageCreate()
{
	const float s = mpui_scale();
	char* lobby   = port_mp_pref_lobby_name();
	int* maxp     = port_mp_pref_max_players();
	int* shared   = port_mp_pref_shared();
	int* port     = port_mp_pref_port();
	static char sPassword[32];
	mpui_title("Host a Lobby");
	mpui_heading("How friends find it");
	focusFirst();
	connTypeChooser();
	const bool server = *port_mp_pref_conn_type() == PORT_MP_CONN_SERVER;
	ImGui::TextDisabled(server ? "Friends pick it from the lobby list, or type its name."
	                           : "Friends type this PC's address (shown in the lobby panel).");
	mpui_heading("Lobby");
	textField("Lobby name", lobby, PORT_MP_LOBBY_NAME_MAX);
	if (ImGui::IsItemDeactivatedAfterEdit())
		port_mp_ini_save();
	if (server)
		textField("Password (optional)", sPassword, sizeof(sPassword), ImGuiInputTextFlags_Password);
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
	if (ImGui::SliderInt("Max players", maxp, 2, PORT_MP_MAX_PLAYERS))
		port_mp_ini_save();
	bool sh = *shared != 0;
	if (ImGui::Checkbox("Shared Shines and coins", &sh)) {
		*shared = sh;
		port_mp_ini_save();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Off: every Mario keeps his own shines and coins.\n"
		                  "On: a shine or coin collected by one player is collected for everyone.");
	bool chatOn = *port_mp_pref_chat() != 0, emotesOn = *port_mp_pref_emotes() != 0;
	if (ImGui::Checkbox("Chat", &chatOn)) {
		*port_mp_pref_chat() = chatOn;
		port_mp_ini_save();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Players press T (or click the right stick) to chat.");
	ImGui::SameLine();
	if (ImGui::Checkbox("Emotes", &emotesOn)) {
		*port_mp_pref_emotes() = emotesOn;
		port_mp_ini_save();
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("G or left-stick click: emote menu.  D-pad: quick emotes.");
	if (!server) {
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
		if (ImGui::InputInt("UDP port", port, 1, 10)) {
			if (*port < 1024)
				*port = 1024;
			if (*port > 65535)
				*port = 65535;
			port_mp_ini_save();
		}
	}
	ImGui::Dummy(ImVec2(1, 6 * s));
	const float W = ImGui::GetContentRegionAvail().x;
	if (mpui_button("Create Lobby!", MPUI_ICON_FLAG, W * 0.62f, 1)) {
		PortMpSessionSettings st;
		port_mp_get_settings(&st);
		st.gameMode         = PORT_MP_MODE_NONE;
		st.reserved[8]      = 0; // game-mode start counter (port_mp_modes.cpp)
		st.maxPlayers       = (uint8_t)*maxp;
		st.sharedObjectives = (uint8_t)(*shared != 0);
		st.reserved[1]      = (uint8_t)((st.reserved[1] & ~3) | (*port_mp_pref_chat() ? 0 : 1)
		                                | (*port_mp_pref_emotes() ? 0 : 2)); // port_mp_chat.cpp
		st.startStage       = 1;
		st.startScenario    = 2;
		port_mp_set_name(sNameEdit);
		if (port_mp_host2(lobby, *port, &st, server ? PORT_MP_CONN_SERVER : PORT_MP_CONN_DIRECT, sPassword)) {
			char t[128];
			if (server)
				std::snprintf(t, sizeof(t), "Lobby \"%s\" is open: friends join by its name (F2: lobby)", lobby);
			else
				std::snprintf(t, sizeof(t), "Lobby open on UDP port %d (F2 shows the address to share)", port_mp_host_port());
			port_mp_toast(t);
			closeMenu(); // port_mp_game.cpp starts free mode on the next file-select frame
		}
	}
	ImGui::SameLine();
	if (mpui_button("Back", MPUI_ICON_BACK, 0.0f) || (backPressed() && !sOskOpen))
		openPage(PAGE_MAIN);
	ImGui::Dummy(ImVec2(1, 2 * s));
	if (server)
		serverStatusLine();
	statusLine();
	ImGui::Dummy(ImVec2(1, 14 * s));
	oskDraw();
}

// Server mode: every open lobby on the lobby server, joined by name (no addresses).
void serverList()
{
	static double sAsked = -1e9;
	if (port_mp_time_ms() - sAsked > 3000.0) { // keeps the status ping (and the list) coming
		sAsked = port_mp_time_ms();
		port_mp_server_refresh();
	}
	PortMpLobbyInfo list[24];
	const int n = port_mp_server_lobbies(list, 24);
	mpui_heading("Open lobbies");
	if (n == 0) {
		ImGui::TextDisabled("No open lobbies right now. Host one, or join by name below.");
		return;
	}
	for (int i = 0; i < n; i++)
		if (lobbyCard(1000 + i, list[i], false)) {
			std::snprintf(port_mp_pref_last_lobby(), PORT_MP_LOBBY_NAME_MAX, "%s", list[i].name);
			if (!list[i].hasPassword) { // a password lobby: the name is filled in, type the password below
				port_mp_ini_save();
				port_mp_set_name(sNameEdit);
				port_mp_join_name(list[i].name, "");
			}
		}
}

void lanList(bool server)
{
	PortMpLobbyInfo list[32];
	const int n = port_mp_lan_lobbies(list, 32);
	if (sAutoLan && n > 0 && list[0].protocolOk) {
		sAutoLan = false;
		std::fprintf(stderr, "[mp] SMS_PORT_MP_MENU_AUTO=lan: joining \"%s\" at %s:%u (%u/%u players, %u ms)\n",
		             list[0].name, list[0].address, list[0].port, list[0].players, list[0].maxPlayers, list[0].pingMs);
		port_mp_join(list[0].address, list[0].port);
	}
	if (port_mp_time_ms() - sLastRefresh > 3000.0) {
		port_mp_lan_refresh();
		sLastRefresh = port_mp_time_ms();
	}
	mpui_heading("On this network");
	for (int i = 0; i < n; i++)
		if (lobbyCard(i, list[i], !server)) {
			port_mp_set_name(sNameEdit);
			if (list[i].hasPassword) // the name field takes the password
				std::snprintf(port_mp_pref_last_lobby(), PORT_MP_LOBBY_NAME_MAX, "%s", list[i].name);
			else
				port_mp_join(list[i].address, list[i].port);
		}
	if (n == 0)
		ImGui::TextDisabled("None found yet (looking every few seconds).");
}

void pageJoin()
{
	const float s = mpui_scale();
	const int st  = port_mp_state();
	if (st == PORT_MP_CONNECTING) {
		mpui_title("Joining...");
		// a Shine spinning while we wait
		ImDrawList* dl = ImGui::GetWindowDrawList();
		const ImVec2 c(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x * 0.5f,
		               ImGui::GetCursorScreenPos().y + 34 * s);
		mpui_icon(dl, MPUI_ICON_SUN, c, 30 * s, IM_COL32(255, 222, 60, 255)); // the game's loading sun
		ImGui::Dummy(ImVec2(1, 72 * s));
		statusLine();
		if (mpui_button("Cancel", MPUI_ICON_BACK, 0.0f, 2) || backPressed())
			port_mp_leave();
		ImGui::Dummy(ImVec2(1, 14 * s));
		return;
	}
	if (st == PORT_MP_CONNECTED) {
		closeMenu(); // free mode starts from the file select
		return;
	}
	mpui_title("Join a Lobby");
	focusFirst();
	connTypeChooser();
	const bool server = *port_mp_pref_conn_type() == PORT_MP_CONN_SERVER;
	if (server) {
		serverList();
		mpui_heading("Join by name");
		static char sPassword[32];
		char* name = port_mp_pref_last_lobby();
		bool go    = textField("Lobby name", name, PORT_MP_LOBBY_NAME_MAX, ImGuiInputTextFlags_EnterReturnsTrue);
		go |= textField("Password (if any)", sPassword, sizeof(sPassword),
		                ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
		if (mpui_button("Join", MPUI_ICON_PLAY, 0.0f, 1) || go) {
			port_mp_ini_save();
			port_mp_set_name(sNameEdit);
			port_mp_join_name(name, sPassword);
		}
	} else {
		mpui_heading("Join by address");
		char* addr = port_mp_pref_last_join();
		int* port  = port_mp_pref_last_join_port();
		textField("Host / IP", addr, 64);
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
		ImGui::InputInt("Port", port, 1, 10);
		if (*port < 1)
			*port = 1;
		if (*port > 65535)
			*port = 65535;
		if (mpui_button("Join", MPUI_ICON_PLAY, 0.0f, 1)) {
			port_mp_ini_save();
			port_mp_set_name(sNameEdit);
			port_mp_join(addr, *port);
		}
	}
	lanList(server);
	ImGui::Dummy(ImVec2(1, 4 * s));
	if (mpui_button("Refresh", MPUI_ICON_REFRESH, 0.0f)) {
		port_mp_lan_refresh();
		port_mp_server_refresh();
		sLastRefresh = port_mp_time_ms();
	}
	ImGui::SameLine();
	if (mpui_button("Back", MPUI_ICON_BACK, 0.0f) || (backPressed() && !sOskOpen)) {
		if (st == PORT_MP_FAILED)
			port_mp_leave();
		openPage(PAGE_MAIN);
	}
	if (server)
		serverStatusLine();
	statusLine();
	ImGui::Dummy(ImVec2(1, 14 * s));
	oskDraw();
}

// Lobby panel: how others join this lobby (with copy buttons).
void joinInfo(bool host)
{
	if (port_mp_conn_type() == PORT_MP_CONN_SERVER) {
		const char* name = port_mp_lobby_name();
		ImGui::Text("Friends join by picking or typing:  %s", name);
		copyButton("lobbyname", name);
		if (host)
			serverStatusLine();
		if (port_mp_relayed())
			ImGui::TextDisabled("(connected through the lobby server's relay)");
		return;
	}
	if (!host)
		return;
	static double sAsked = -1e9; // our lobby server tells us our public address (if set up)
	if (port_mp_time_ms() - sAsked > 4000.0) {
		sAsked = port_mp_time_ms();
		port_mp_server_refresh();
	}
	const int port = port_mp_host_port();
	ImGui::TextUnformatted("Friends join with one of these addresses:");
	PortMpLocalAddr a[8];
	const int n = port_mp_local_addresses(a, 8);
	for (int i = 0; i < n; i++) {
		char t[64];
		std::snprintf(t, sizeof(t), "%s:%d", a[i].ip, port);
		ImGui::Text("  %s: %s", a[i].kind ? "Virtual LAN" : "Same network", t);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("%s", a[i].adapter);
		char id[16];
		std::snprintf(id, sizeof(id), "ip%d", i);
		copyButton(id, t);
		ImGui::SameLine();
		ImGui::TextDisabled("(%s)", a[i].adapter);
	}
	// The public address only from our own lobby server's observation (never a third-party site).
	PortMpServerStatus st;
	port_mp_server_status(&st);
	unsigned o1 = 0, o2 = 0;
	std::sscanf(st.publicAddr, "%u.%u", &o1, &o2);
	const bool privateAddr = o1 == 127 || o1 == 10 || (o1 == 192 && o2 == 168) || (o1 == 172 && o2 >= 16 && o2 < 32)
	                         || (o1 == 100 && o2 >= 64 && o2 < 128) || (o1 == 169 && o2 == 254);
	if (st.state == PORT_MP_SERVER_ONLINE && st.publicAddr[0] != '\0' && !privateAddr) {
		char ip[48];
		std::snprintf(ip, sizeof(ip), "%s", st.publicAddr);
		if (char* c = std::strrchr(ip, ':'))
			*c = '\0';
		char t[64];
		std::snprintf(t, sizeof(t), "%s:%d", ip, port);
		ImGui::Text("  Internet: %s", t);
		copyButton("pub", t);
		ImGui::TextDisabled("    (needs UDP port %d forwarded to this PC on the router; see README)", port);
	} else {
		ImGui::TextDisabled("  Internet: needs port forwarding of UDP %d; see README", port);
	}
}

void pageCharacter()
{
	// w15's character editor (port_mp_custom.cpp); returns nonzero when it is closed.
	if (port_mp_custom_draw())
		openPage(PAGE_MAIN);
}

void pageCharacterOnly()
{
	if (port_mp_custom_draw()) // closed: back to the file select
		closeMenu();
}

// One player in the lobby panel: colour dot, name (game font), host crown, ping bars.
void playerRow(int slot, const PortMpPlayerInfo& p, bool host)
{
	const float s  = mpui_scale();
	const float fs = ImGui::GetFontSize();
	const float w  = ImGui::GetContentRegionAvail().x;
	const float h  = fs * 1.9f;
	const ImVec2 a = ImGui::GetCursorScreenPos(), b(a.x + w, a.y + h);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	dl->AddRectFilled(a, b, p.isLocal ? IM_COL32(50, 86, 245, 200) : IM_COL32(20, 40, 160, 170), 8 * s);
	const ImU32 col = slotColour(slot);
	dl->AddCircleFilled(ImVec2(a.x + 14 * s, (a.y + b.y) * 0.5f), 6 * s, col);
	dl->AddCircle(ImVec2(a.x + 14 * s, (a.y + b.y) * 0.5f), 6 * s, IM_COL32(255, 255, 255, 200), 0, 1.5f * s);
	const float px = 15.0f * s;
	float x        = a.x + 28 * s;
	if (p.isHost) {
		mpui_icon(dl, MPUI_ICON_CROWN, ImVec2(x + 8 * s, (a.y + b.y) * 0.5f), 7 * s, 0);
		x += 22 * s;
	}
	char name[64];
	std::snprintf(name, sizeof(name), "%s%s", p.name, p.isLocal ? " (you)" : "");
	mpui_text(dl, ImVec2(x, a.y + (h - px) * 0.5f), px, col, name);
	// ping on the right
	char ping[32];
	if (p.lagging)
		std::snprintf(ping, sizeof(ping), "reconnecting...");
	else if (p.isLocal && host)
		std::snprintf(ping, sizeof(ping), "host");
	else
		std::snprintf(ping, sizeof(ping), "%u ms", p.pingMs);
	const ImVec2 ts = ImGui::CalcTextSize(ping);
	dl->AddText(ImVec2(b.x - 12 * s - fs * 1.5f - ts.x, a.y + (h - ts.y) * 0.5f), IM_COL32(220, 230, 255, 255), ping);
	if (!(p.isLocal && host)) {
		ImGui::SetCursorScreenPos(ImVec2(b.x - 10 * s - fs * 1.35f, a.y + (h - fs * 1.1f) * 0.5f));
		mpui_ping_bars(p.pingMs, p.lagging != 0);
	}
	ImGui::SetCursorScreenPos(a);
	ImGui::Dummy(ImVec2(w, h));
}

void pageLobby()
{
	const float s = mpui_scale();
	const int st  = port_mp_state();
	if (st != PORT_MP_HOSTING && st != PORT_MP_CONNECTED) {
		mpui_title("Lobby");
		ImGui::TextUnformatted("You are not in a lobby.");
		statusLine();
		focusFirst();
		if (mpui_button("Close", MPUI_ICON_BACK, 0.0f) || backPressed())
			closeMenu();
		ImGui::Dummy(ImVec2(1, 14 * s));
		return;
	}
	const bool host = st == PORT_MP_HOSTING;
	mpui_title(port_mp_lobby_name());
	PortMpSessionSettings set;
	port_mp_get_settings(&set);
	int count = 0;
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		PortMpPlayerInfo p;
		count += port_mp_get_player(i, &p) ? 1 : 0;
	}
	char chip[64];
	std::snprintf(chip, sizeof(chip), "%s  -  %d / %d players", host ? "You are the host" : "You are a guest", count,
	              set.maxPlayers);
	mpui_chip(chip, host ? IM_COL32(255, 222, 60, 255) : IM_COL32(90, 200, 255, 255));
	joinInfo(host);
	mpui_heading("Players");
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		PortMpPlayerInfo p;
		if (port_mp_get_player(i, &p))
			playerRow(i, p, host);
	}
	// Game mode (w15, port_mp_modes.cpp): the host picks one and starts it for everyone.
	mpui_heading("Game mode");
	focusFirst();
	port_mp_modes_panel(host ? 1 : 0); // countdown, queue, votes, AutoStart, mode settings
	mpui_heading("Options");
	if (host) {
		bool sh = set.sharedObjectives != 0;
		if (ImGui::Checkbox("Shared Shines and coins", &sh)) {
			set.sharedObjectives = sh;
			port_mp_host_set_settings(&set);
			*port_mp_pref_shared() = sh;
			port_mp_ini_save();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Off: every Mario keeps his own shines and coins.\n"
			                  "On: collected for everyone. A change applies at each player's next stage load.");
	} else {
		ImGui::Text("Shared Shines and coins: %s", set.sharedObjectives ? "On" : "Off");
	}
	port_mp_menu_feature_lobby(host ? 1 : 0); // other features' sections (port_mp.h)
	mpui_heading("Your profile");
	nameField();
	ImGui::Dummy(ImVec2(1, 6 * s));
	const float W = ImGui::GetContentRegionAvail().x;
	if (mpui_button("Resume", MPUI_ICON_PLAY, W * 0.48f, 1) || (backPressed() && !sOskOpen))
		closeMenu();
	ImGui::SameLine();
	if (mpui_button(host ? "Close Lobby" : "Leave Lobby", MPUI_ICON_EXIT, W * 0.48f, 2)) {
		port_mp_leave();
		closeMenu();
		return;
	}
	statusLine();
	ImGui::Dummy(ImVec2(1, 14 * s));
	if (oskDraw() && std::strcmp(sOskTitle, "Username") == 0)
		port_mp_set_name(sNameEdit);
}

// Toasts (joins, leaves, shared Shines): bottom-left, 5 s each, any thread may add.
struct Toast {
	char text[128];
	double at;
};
std::mutex sToastMu;
std::deque<Toast> sToasts;

void drawToasts(ImDrawList* dl, float sc)
{
	std::lock_guard<std::mutex> lk(sToastMu);
	const double now = port_mp_time_ms();
	while (!sToasts.empty() && now - sToasts.front().at > 5000.0)
		sToasts.pop_front();
	ImGuiIO& io = ImGui::GetIO();
	float y     = io.DisplaySize.y * 0.80f;
	for (auto it = sToasts.rbegin(); it != sToasts.rend(); ++it) {
		const double age = now - it->at;
		const float a    = age > 4000.0 ? (float)((5000.0 - age) / 1000.0) : 1.0f;
		// slides in from the left during its first quarter second
		const float in = age < 250.0 ? (float)(age / 250.0) : 1.0f;
		mpui_toast(dl, ImVec2(10.0f * sc - (1.0f - in) * 120.0f * sc, y), it->text, a);
		y -= 34.0f * sc;
	}
}

void drawHints()
{
	ImDrawList* dl = ImGui::GetForegroundDrawList();
	ImGuiIO& io    = ImGui::GetIO();
	const float sc = mpui_scale();
	drawToasts(dl, sc);
	if (port_mp_file_select_hint() && port_mp_enabled() && !port_mp_block_active()) {
		const char* t = "<< Multiplayer";
		mpui_toast(dl, ImVec2(10.0f * sc, io.DisplaySize.y * 0.66f), t, 0.9f);
	}
	if (port_mp_active() && sPage == PAGE_CLOSED) {
		int n = 0;
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
			PortMpPlayerInfo p;
			n += port_mp_get_player(i, &p);
		}
		char t[96];
		std::snprintf(t, sizeof(t), "%d player%s  (F2%s)", n, n == 1 ? "" : "s",
		              port_mp_in_pause_menu() ? " / Z: lobby" : ": lobby");
		const float fs  = ImGui::GetFontSize() * sc;
		const ImVec2 sz = ImGui::GetFont()->CalcTextSizeA(fs, 1e9f, 0.0f, t);
		const ImVec2 p(io.DisplaySize.x - sz.x - 10.0f * sc, 8.0f * sc);
		dl->AddRectFilled(ImVec2(p.x - 6 * sc, p.y - 3 * sc), ImVec2(p.x + sz.x + 6 * sc, p.y + sz.y + 3 * sc),
		                  IM_COL32(16, 30, 120, 170), 6.0f * sc);
		dl->AddText(ImGui::GetFont(), fs, p, IM_COL32(255, 222, 60, 255), t);
	}
}

} // namespace

extern "C" {

void port_mp_menu_close(void) { sCloseRequest = true; }

void port_mp_menu_open(int page)
{
	sOpenRequest = page == 1 ? PAGE_LOBBY : page == 2 ? PAGE_CHARACTER_ONLY : PAGE_MAIN;
	sWantsInput  = true; // the game sees idle pads from now on
}

// Feature hooks (port_mp.h): overlays and lobby-panel sections registered by other files.
static PortMpFrameFn sOverlayFns[16];
static int sOverlayCount;
static struct {
	const char* title;
	PortMpLobbyFn fn;
} sLobbyFns[16];
static int sLobbyCount;
static std::atomic<int> sInputRequests{ 0 };

void port_mp_register_overlay(PortMpFrameFn fn)
{
	if (fn != nullptr && sOverlayCount < 16)
		sOverlayFns[sOverlayCount++] = fn;
}

void port_mp_register_lobby_section(const char* title, PortMpLobbyFn fn)
{
	if (fn != nullptr && sLobbyCount < 16) {
		sLobbyFns[sLobbyCount].title = title != nullptr ? title : "Feature";
		sLobbyFns[sLobbyCount].fn    = fn;
		sLobbyCount++;
	}
}

void port_mp_request_input(int on)
{
	if (on)
		sInputRequests++;
	else if (sInputRequests.load() > 0)
		sInputRequests--;
}

int port_mp_menu_wants_input(void) { return (sWantsInput.load() || sInputRequests.load() > 0) ? 1 : 0; }

void port_mp_menu_feature_lobby(int isHost)
{
	for (int i = 0; i < sLobbyCount; i++)
		if (ImGui::CollapsingHeader(sLobbyFns[i].title, ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::PushID(i);
			sLobbyFns[i].fn(isHost);
			ImGui::PopID();
		}
}

void port_mp_menu_feature_overlays(void)
{
	if (!port_mp_active())
		return;
	for (int i = 0; i < sOverlayCount; i++)
		sOverlayFns[i]();
}

void port_mp_toast(const char* text)
{
	if (text == nullptr)
		return;
	std::fprintf(stderr, "[mp] toast: %s\n", text);
	std::lock_guard<std::mutex> lk(sToastMu);
	Toast t;
	std::snprintf(t.text, sizeof(t.text), "%s", text);
	t.at = port_mp_time_ms();
	sToasts.push_back(t);
	while (sToasts.size() > 6)
		sToasts.pop_front();
}

void port_mp_menu_draw(void)
{
	if (ImGui::GetCurrentContext() == nullptr)
		return;
	port_mp_menu_feature_overlays(); // registered feature overlays (port_mp.h)
	// SMS_PORT_MP_MENU_AUTO=copytest (debug): in a session, open the lobby panel, press the first
	// Copy button, read the clipboard back, log both and restore what was there before.
	{
		static const char* sAuto = std::getenv("SMS_PORT_MP_MENU_AUTO");
		static double sT0;
		static int sStep;
		static char* sOld;
		if (sAuto != nullptr && std::strcmp(sAuto, "copytest") == 0 && port_mp_active() && sStep < 3) {
			if (sT0 == 0.0)
				sT0 = port_mp_time_ms();
			const double el = port_mp_time_ms() - sT0;
			if (sStep == 0 && el > 4000.0) {
				sStep = 1;
				port_mp_menu_open(1);
			} else if (sStep == 1 && el > 6000.0) {
				sStep        = 2;
				sOld         = SDL_GetClipboardText();
				sCopyTestNow = true; // the next drawn Copy button copies
			} else if (sStep == 2 && el > 6500.0 && !sCopyTestNow) {
				sStep    = 3;
				char* now = SDL_GetClipboardText();
				std::fprintf(stderr, "[mp] clipboard test: copied \"%s\", clipboard reads \"%s\" (%s)\n", sCopiedWhat,
				             now ? now : "", now && std::strcmp(now, sCopiedWhat) == 0 ? "match" : "MISMATCH");
				SDL_free(now);
				SDL_SetClipboardText(sOld ? sOld : ""); // put the user's clipboard back
				SDL_free(sOld);
			}
		}
	}
	if (sCloseRequest.exchange(false))
		closeMenu();
	int req = sOpenRequest.exchange(-2);
	// F2 (or SMS_PORT_KEYSCRIPT): lobby panel in a session, the main page on the file select.
	static bool sScriptF2;
	const bool f2s = port_input_script_key(SDL_SCANCODE_F2);
	bool f2        = ImGui::IsKeyPressed(ImGuiKey_F2, false) || (f2s && !sScriptF2);
	sScriptF2      = f2s;
	if (f2 && !port_menu_is_open()) {
		if (sPage != PAGE_CLOSED)
			closeMenu();
		else if (port_mp_active())
			req = PAGE_LOBBY;
		else if (port_mp_file_select_hint())
			req = PAGE_MAIN;
	}
	if (req != -2 && sPage == PAGE_CLOSED)
		openPage(req);
	else if (req == -2 && sPage == PAGE_CLOSED)
		sWantsInput = false;
	// the character pages on the file select: the camera shows the real Mario, turnable
	const bool preview = (sPage == PAGE_CHARACTER || sPage == PAGE_CHARACTER_ONLY) && port_mp_block_active()
	                     && !port_mp_active() && !port_menu_is_open();
	port_mp_block_preview(preview ? 1 : 0);
	if (sPage == PAGE_CLOSED) {
		drawHints();
		return;
	}
	if (port_menu_is_open()) // w9's settings menu on top: wait
		return;

	ImGuiIO& io       = ImGui::GetIO();
	const float scale = mpui_scale();
	float width = (sPage == PAGE_LOBBY ? 600.0f : sPage >= PAGE_CHARACTER ? 620.0f : 540.0f) * scale;
	if (preview) {
		// Mario in 3D on the left, the menu on the right; drag (outside the menu) or L / R turns him
		width = io.DisplaySize.x * 0.63f;
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 10.0f * scale, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
		                        ImVec2(1.0f, 0.5f));
		if (!io.WantCaptureMouse && ImGui::IsMouseDown(ImGuiMouseButton_Left))
			port_mp_block_preview_rotate(io.MouseDelta.x * 0.6f);
		// the analog triggers turn him, faster the further they are pressed (and the right stick);
		// degrees per second, so the frame rate does not change the speed
		auto analog = [&io](ImGuiKey k) { return io.KeysData[k - ImGuiKey_NamedKey_BEGIN].AnalogValue; };
		const float lt = analog(ImGuiKey_GamepadL2), rt = analog(ImGuiKey_GamepadR2);
		const float stick = analog(ImGuiKey_GamepadRStickRight) - analog(ImGuiKey_GamepadRStickLeft);
		const float speed = (rt * rt - lt * lt) * 270.0f + stick * 150.0f; // squared: light presses turn slowly
		if (speed != 0.0f)
			port_mp_block_preview_rotate(speed * io.DeltaTime);
	} else {
		// the game dims behind the menu, a little blue (like the pause menu)
		ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 12, 60, 110));
		ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
		                        ImVec2(0.5f, 0.5f));
	}
	ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width, io.DisplaySize.y * 0.94f));
	mpui_push_theme();
	bool open = true;
	if (ImGui::Begin("Multiplayer###mp", nullptr,
	                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
		mpui_window_background();
		ImGui::SetWindowFontScale(scale);
		if (ImGui::IsWindowAppearing())
			ImGui::SetWindowFocus();
		switch (sPage) {
		case PAGE_MAIN: pageMain(); break;
		case PAGE_CREATE: pageCreate(); break;
		case PAGE_JOIN: pageJoin(); break;
		case PAGE_CHARACTER: pageCharacter(); break;
		case PAGE_CHARACTER_ONLY: pageCharacterOnly(); break;
		case PAGE_LOBBY: pageLobby(); break;
		}
	}
	ImGui::End();
	mpui_pop_theme();
	if (!open)
		closeMenu();
}

} // extern "C"

#endif // SMS_PORT_AURORA
