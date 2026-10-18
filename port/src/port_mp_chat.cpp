// Multiplayer text chat, emote menu and their lobby-panel section (w6, MULTIPLAYER.md task
// board P3). Game side of the emotes: port_mp_emote.cpp. Plugs in only through port_mp.h
// (app channels PORT_MP_CH_CHAT / PORT_MP_CH_EMOTE, an overlay, a lobby section,
// port_mp_request_input); nothing runs outside a multiplayer session.
//
// Chat
//   Open: T or Enter (each only when [keyboard] does not bind it to a pad button), the right-stick
//   click on a controller (opens the on-screen keyboard), or "Chat" in the lobby panel (F2 /
//   Z in the pause menu; GameCube pads). Enter / Done sends, Esc / B cancels. The game gets
//   idle pads while the box is open.
//   Messages: up to kMaxText bytes of printable UTF-8, at most 5 per 10 s (and 0.5 s apart)
//   from us; receivers also drop more than 8 per 10 s from one player. They carry the
//   sender's name and colour (the cap colour of their character, else a colour per slot); the
//   name shown is the one in the session's player table. Recent lines stay 10 s and fade;
//   while the box is open the last kShowOpen lines stay. Joins, leaves and our own errors
//   are system lines. Commands: /<emote> (e.g. /shine), /emotes, /me <text>, /help.
//   Mute: per player in the lobby panel (until they leave).
// Emotes
//   D-pad = quick emotes (port_mp_emote.cpp); G or the left-stick click opens the emote menu
//   (number keys / D-pad + A), also in the lobby panel. Others see a dim "* name: emote" line.
// Host options (PortMpSessionSettings.reserved[1]): bit 0 = chat off, bit 1 = emotes off.
//
// Wire (version 1):
//   CHAT  [u8 1][u8 flags: bit0 = action][u8 r,g,b][u8 n][n bytes name][text]  reliable, to all
//   EMOTE [u8 1][u8 emote id]                                                  reliable, to all
#ifdef SMS_PORT_AURORA

#include "port_mp.h"
#include "port_settings.h"

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_scancode.h>

#define IMGUI_USER_CONFIG "aurora/imgui_config.h" // as Aurora builds imgui
#include <imgui.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>

extern "C" {
unsigned port_frame_count(void); // port_crash.cpp
// port_mp_emote.cpp
int port_mp_emote_count(void);
const char* port_mp_emote_label(int id);
const char* port_mp_emote_command(int id);
int port_mp_emote_dpad(int id);
int port_mp_emote_dance(int id);                      // port_mp_emote.cpp
int port_mp_emote_in_wheel(int id);                   // port_mp_custom.cpp (emote locker)
void port_mp_dance_remote_start(int slot, int dance); // port_mp_dance.cpp
void port_mp_emote_request(int id);
}

namespace {

const int kMaxText     = 120;   // bytes of UTF-8 per message
const int kMaxLog      = 60;
const int kShowClosed  = 8;     // lines shown while the box is closed (if recent)
const int kShowOpen    = 12;
const double kLineMs   = 10000; // a line stays this long, then fades over kFadeMs
const double kFadeMs   = 2000;
const int kVersion     = 1;
// Bottom of the chat area as a share of the picture height: above the plaza's news ticker
// (about 0.86) and around w10's toasts (bottom-left, from 0.80 upward).
const float kInputBottom = 0.77f;

enum LineKind { LINE_CHAT, LINE_ACTION, LINE_SYSTEM, LINE_EMOTE };

struct Line {
	int kind;
	unsigned rgb;
	char name[PORT_MP_NAME_MAX];
	char text[kMaxText + 1];
	double at;
};

std::mutex sMu; // the log (system lines may come from the game thread)
std::deque<Line> sLog;

// Session bookkeeping (main thread).
struct Seen {
	bool used;
	unsigned serial;
	char name[PORT_MP_NAME_MAX];
};
Seen sSeen[PORT_MP_MAX_PLAYERS];
bool sMuted[PORT_MP_MAX_PLAYERS]; // by slot, cleared when the slot's serial changes
std::deque<double> sRecvTimes[PORT_MP_MAX_PLAYERS];
std::deque<double> sEmoteTimes[PORT_MP_MAX_PLAYERS];
std::deque<double> sSendTimes;
bool sWasActive;

// Input box / emote menu (main thread).
bool sOpen, sFocus, sOsk, sOskShift, sEmoteMenu, sEmoteFocus;
char sBuf[kMaxText + 1];
std::atomic<int> sInputRequests{ 0 }; // our port_mp_request_input(1) calls not yet undone
std::atomic<bool> sWatchdog{ false };
bool sNavSaved, sNavWas;
unsigned sLocalSerial = 0xFFFFFFFFu; // the local player's serial: a new value = a new session
float sBaseFont = 13.0f;
float sUiScale  = 1.0f; // our windows: at least the UI font, bigger in big windows

double nowMs() { return port_mp_time_ms(); }

void addLine(int kind, unsigned rgb, const char* name, const char* text)
{
	Line l;
	l.kind = kind;
	l.rgb  = rgb;
	std::snprintf(l.name, sizeof(l.name), "%s", name != nullptr ? name : "");
	std::snprintf(l.text, sizeof(l.text), "%s", text != nullptr ? text : "");
	l.at = nowMs();
	std::fprintf(stderr, "[mp] chat%s: %s%s%s\n",
	             kind == LINE_SYSTEM ? " (system)" : kind == LINE_EMOTE ? " (emote)" : "", l.name,
	             l.name[0] != '\0' ? ": " : "", l.text);
	std::lock_guard<std::mutex> lk(sMu);
	sLog.push_back(l);
	while ((int)sLog.size() > kMaxLog)
		sLog.pop_front();
}

// w15 (speech bubbles over puppets, port_mp_overlay.cpp): each player's latest chat line or
// emote, read through port_mp_chat_last_said below.
struct Said {
	char text[kMaxText + 3];
	int emote; // emote id, or -1 for a chat line
	double at; // 0 = nothing
};
Said sSaid[PORT_MP_MAX_PLAYERS];

void noteSaid(int slot, const char* text, int emote, bool action)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PLAYERS)
		return;
	std::lock_guard<std::mutex> lk(sMu);
	Said& s = sSaid[slot];
	std::snprintf(s.text, sizeof(s.text), "%s%s", action ? "* " : "", text != nullptr ? text : "");
	s.emote = emote;
	s.at    = nowMs();
}

void sysLine(const char* fmt, ...)
{
	char t[160];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(t, sizeof(t), fmt, ap);
	va_end(ap);
	addLine(LINE_SYSTEM, 0xC8C8C8, "", t);
}

bool chatAllowed()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	return (s.reserved[1] & 1) == 0;
}

// Printable UTF-8 only: control characters and malformed sequences are dropped; trimmed.
void cleanText(const char* in, int len, char* out, int cap)
{
	int o = 0;
	for (int i = 0; i < len && in[i] != '\0';) {
		const unsigned char c = (unsigned char)in[i];
		int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
		bool ok = n > 0 && i + n <= len;
		for (int k = 1; ok && k < n; k++)
			ok = ((unsigned char)in[i + k] & 0xC0) == 0x80;
		if (ok && n == 1 && (c < 0x20 || c == 0x7F))
			ok = false;
		if (!ok) {
			i++;
			continue;
		}
		if (o + n >= cap)
			break;
		std::memcpy(out + o, in + i, n);
		o += n;
		i += n;
	}
	out[o] = '\0';
	// trim spaces
	int s = 0;
	while (out[s] == ' ')
		s++;
	if (s > 0)
		std::memmove(out, out + s, std::strlen(out + s) + 1);
	int e = (int)std::strlen(out);
	while (e > 0 && out[e - 1] == ' ')
		out[--e] = '\0';
}

unsigned playerColor(int slot, const PortMpPlayerInfo& p)
{
	const unsigned char* c = p.character.colors[0]; // cap
	if (c[0] != 0 || c[1] != 0 || c[2] != 0) {
		unsigned r = c[0], g = c[1], b = c[2];
		// keep it readable on the dark box
		while (r + g + b < 260) {
			r = r + (255 - r) / 3;
			g = g + (255 - g) / 3;
			b = b + (255 - b) / 3;
		}
		return (r << 16) | (g << 8) | b;
	}
	static const unsigned kSlot[8] = { 0xFF6A5A, 0x6AC8FF, 0x7CE07C, 0xFFD45A,
		                               0xD08CFF, 0xFF9A3C, 0x5AE6D0, 0xFF8CC8 };
	return kSlot[(slot < 0 ? 0 : slot) % 8];
}

bool playerName(int slot, char* out, int cap, unsigned* rgb)
{
	PortMpPlayerInfo p;
	if (slot < 0 || !port_mp_get_player(slot, &p))
		return false;
	std::snprintf(out, cap, "%s", p.name);
	if (rgb != nullptr)
		*rgb = playerColor(slot, p);
	return true;
}

bool rateOk(std::deque<double>& q, int max, double windowMs, double now)
{
	while (!q.empty() && now - q.front() > windowMs)
		q.pop_front();
	if ((int)q.size() >= max)
		return false;
	q.push_back(now);
	return true;
}

void listEmotes()
{
	std::string t = "Emotes:";
	static const char* const kDir[4] = { "Up", "Right", "Down", "Left" };
	for (int i = 0; i < port_mp_emote_count(); i++) {
		char b[64];
		const int d = port_mp_emote_dpad(i);
		std::snprintf(b, sizeof(b), " /%s%s%s%s", port_mp_emote_command(i), d >= 0 ? " (D-pad " : "",
		              d >= 0 ? kDir[d] : "", d >= 0 ? ")" : "");
		t += b;
	}
	sysLine("%s", t.c_str());
}

// Sends what is in `text` (already typed by the player). Returns false if refused.
bool sendChat(const char* text)
{
	char clean[kMaxText + 1];
	cleanText(text, (int)std::strlen(text), clean, sizeof(clean));
	if (clean[0] == '\0')
		return true;
	if (clean[0] == '/') { // commands
		const char* cmd = clean + 1;
		if (std::strcmp(cmd, "help") == 0 || std::strcmp(cmd, "?") == 0) {
			sysLine("T: chat   G: emote menu   D-pad: quick emotes   /emotes: list   /me <text>: action");
			sysLine("/mute <name>, /unmute <name>: hide or show a player's chat");
			return true;
		}
		const bool mute = std::strncmp(cmd, "mute ", 5) == 0, unmute = std::strncmp(cmd, "unmute ", 7) == 0;
		if (mute || unmute) {
			const char* who = cmd + (mute ? 5 : 7);
			for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
				PortMpPlayerInfo p;
				if (port_mp_get_player(i, &p) && !p.isLocal && _stricmp(p.name, who) == 0) {
					sMuted[i] = mute;
					sysLine(mute ? "Muted %s." : "Unmuted %s.", p.name);
					return true;
				}
			}
			sysLine("No player called \"%s\".", who);
			return true;
		}
		if (std::strcmp(cmd, "emotes") == 0) {
			listEmotes();
			return true;
		}
		for (int i = 0; i < port_mp_emote_count(); i++)
			if (_stricmp(cmd, port_mp_emote_command(i)) == 0) {
				port_mp_emote_request(i);
				return true;
			}
		if (std::strncmp(cmd, "me ", 3) != 0) {
			sysLine("Unknown command \"%s\" (try /help).", clean);
			return false;
		}
	}
	if (!chatAllowed()) {
		sysLine("The host turned chat off.");
		return false;
	}
	const double now = nowMs();
	if (!sSendTimes.empty() && now - sSendTimes.back() < 500.0) {
		sysLine("Slow down a little.");
		return false;
	}
	if (!rateOk(sSendTimes, 5, 10000.0, now)) {
		sysLine("Slow down: at most 5 messages in 10 seconds.");
		return false;
	}
	const bool action = std::strncmp(clean, "/me ", 4) == 0;
	const char* body  = action ? clean + 4 : clean;
	const int slot    = port_mp_local_slot();
	char name[PORT_MP_NAME_MAX];
	unsigned rgb = 0xFFFFFF;
	if (!playerName(slot, name, sizeof(name), &rgb))
		std::snprintf(name, sizeof(name), "%s", port_mp_get_name());
	unsigned char pkt[6 + PORT_MP_NAME_MAX + kMaxText];
	const int n = (int)std::strlen(name);
	const int t = (int)std::strlen(body);
	pkt[0]      = kVersion;
	pkt[1]      = action ? 1 : 0;
	pkt[2]      = (unsigned char)(rgb >> 16);
	pkt[3]      = (unsigned char)(rgb >> 8);
	pkt[4]      = (unsigned char)rgb;
	pkt[5]      = (unsigned char)n;
	std::memcpy(pkt + 6, name, n);
	std::memcpy(pkt + 6 + n, body, t);
	port_mp_send(PORT_MP_CH_CHAT, pkt, 6 + n + t, 1, -1);
	addLine(action ? LINE_ACTION : LINE_CHAT, rgb, name, body);
	noteSaid(slot, body, -1, action); // w15: speech bubble
	return true;
}

void receive()
{
	unsigned char buf[PORT_MP_APP_MAX];
	int from;
	int n;
	const double now = nowMs();
	while ((n = port_mp_recv(PORT_MP_CH_CHAT, buf, sizeof(buf), &from)) != 0) {
		if (n < 6 || buf[0] != kVersion || from < 0 || from >= PORT_MP_MAX_PLAYERS)
			continue;
		const int nl = buf[5];
		if (6 + nl > n || !chatAllowed() || sMuted[from])
			continue;
		if (!rateOk(sRecvTimes[from], 8, 10000.0, now))
			continue; // flooding
		char text[kMaxText + 1];
		cleanText((const char*)buf + 6 + nl, n - 6 - nl, text, sizeof(text));
		if (text[0] == '\0')
			continue;
		char name[PORT_MP_NAME_MAX];
		unsigned rgb = ((unsigned)buf[2] << 16) | ((unsigned)buf[3] << 8) | buf[4];
		unsigned tableRgb;
		if (!playerName(from, name, sizeof(name), &tableRgb)) {
			cleanText((const char*)buf + 6, nl, name, sizeof(name));
			tableRgb = rgb;
		}
		if (buf[2] + buf[3] + buf[4] < 120) // unreadable: use ours
			rgb = tableRgb;
		addLine((buf[1] & 1) ? LINE_ACTION : LINE_CHAT, rgb, name, text);
		noteSaid(from, text, -1, (buf[1] & 1) != 0); // w15: speech bubble
	}
	while ((n = port_mp_recv(PORT_MP_CH_EMOTE, buf, sizeof(buf), &from)) != 0) {
		if (n < 2 || buf[0] != kVersion || from < 0 || from >= PORT_MP_MAX_PLAYERS)
			continue;
		port_mp_dance_remote_start(from, buf[1] < port_mp_emote_count() ? port_mp_emote_dance(buf[1]) : -1); // his puppet
		if (sMuted[from])
			continue;
		if (buf[1] >= port_mp_emote_count() || !rateOk(sEmoteTimes[from], 6, 10000.0, now))
			continue; // unknown, or emote spam (the puppet still shows it; only the log line goes)
		char name[PORT_MP_NAME_MAX];
		unsigned rgb = 0xFFFFFF;
		if (!playerName(from, name, sizeof(name), &rgb))
			continue;
		addLine(LINE_EMOTE, rgb, name, port_mp_emote_label(buf[1]));
		noteSaid(from, port_mp_emote_label(buf[1]), buf[1], false); // w15: speech bubble
	}
}

// Joins and leaves as system lines (from the player table, polled each frame).
void resetSession();

void trackPlayers()
{
	PortMpPlayerInfo me;
	const int local = port_mp_local_slot();
	if (local >= 0 && port_mp_get_player(local, &me) && me.serial != sLocalSerial) {
		resetSession(); // a new session (our overlay does not run between sessions)
		sLocalSerial = me.serial;
		sWasActive   = false;
	}
	if (!sWasActive)
		sysLine("Connected. T: chat, G: emotes, D-pad: quick emotes, /help.");
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		PortMpPlayerInfo p;
		const bool used = port_mp_get_player(i, &p) != 0;
		Seen& s         = sSeen[i];
		if (used && (!s.used || s.serial != p.serial)) {
			if (s.used)
				sysLine("%s left.", s.name);
			sMuted[i] = false;
			sRecvTimes[i].clear();
			if (!p.isLocal && sWasActive)
				sysLine("%s joined.", p.name);
			s.used   = true;
			s.serial = p.serial;
			std::snprintf(s.name, sizeof(s.name), "%s", p.name);
		} else if (!used && s.used) {
			sysLine("%s left.", s.name);
			s.used    = false;
			sMuted[i] = false;
		} else if (used) {
			std::snprintf(s.name, sizeof(s.name), "%s", p.name); // renames
		}
	}
	sWasActive = true;
}

void closeChat();
void closeEmoteMenu();

void resetSession()
{
	closeChat();
	closeEmoteMenu();
	for (auto& s : sSeen)
		s.used = false;
	for (bool& m : sMuted)
		m = false;
	sSendTimes.clear();
	std::lock_guard<std::mutex> lk(sMu);
	sLog.clear();
	for (Said& s : sSaid) // w15: speech bubbles
		s.at = 0.0;
}

// ImGui navigation (keyboard/gamepad) while one of our windows is open, as w10's menus do.
void navOn()
{
	if (sNavSaved)
		return;
	ImGuiIO& io = ImGui::GetIO();
	sNavWas     = (io.ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) != 0;
	sNavSaved   = true;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
}

void navOff()
{
	if (!sNavSaved)
		return;
	sNavSaved = false;
	if (!sNavWas && !port_menu_is_open() && !port_mp_menu_wants_input())
		ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
}

void takeInput(bool on)
{
	if (on && sInputRequests.exchange(1) == 0) {
		port_mp_request_input(1);
		// Our overlay only runs during a session: if it ends (host left, timeout) while the
		// box is open, this small watchdog hands the pads back to the game.
		if (!sWatchdog.exchange(true))
			std::thread([] {
				while (sInputRequests.load() > 0) {
					if (!port_mp_active()) {
						if (sInputRequests.exchange(0) > 0)
							port_mp_request_input(0);
						break;
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(200));
				}
				sWatchdog = false;
			}).detach();
	} else if (!on && sInputRequests.exchange(0) > 0) {
		port_mp_request_input(0);
	}
}

void openChat(bool osk, const char* prefill = "")
{
	if (!chatAllowed()) {
		sysLine("The host turned chat off.");
		return;
	}
	sOpen      = true;
	sFocus     = true;
	sOsk       = osk;
	sOskShift  = false;
	sEmoteMenu = false;
	std::snprintf(sBuf, sizeof(sBuf), "%s", prefill);
	takeInput(true);
	if (osk)
		navOn();
}

void closeChat()
{
	sOpen = false;
	sOsk  = false;
	if (!sEmoteMenu) {
		takeInput(false);
		navOff();
	}
}

void openEmoteMenu()
{
	sEmoteMenu  = true;
	sEmoteFocus = true;
	sOpen       = false;
	takeInput(true);
	navOn();
}

void closeEmoteMenu()
{
	sEmoteMenu = false;
	if (!sOpen) {
		takeInput(false);
		navOff();
	}
}

// The game picture inside the window (w6's display aspect; 0 = stretched), as the name tags.
void contentBox(float winW, float winH, float& x, float& y, float& w, float& h)
{
	const float aspect = port_get_display_aspect();
	x = 0.0f, y = 0.0f, w = winW, h = winH;
	if (aspect <= 0.0f || winW <= 0.0f || winH <= 0.0f)
		return;
	if (winW / winH > aspect) {
		w = winH * aspect;
		x = (winW - w) * 0.5f;
	} else {
		h = winW / aspect;
		y = (winH - h) * 0.5f;
	}
}

ImU32 col(unsigned rgb, int a) { return IM_COL32((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, a); }

// The chat lines, bottom-left of the picture, newest at the bottom (above the input line).
void drawLog(float bx, float by, float bw, float bh, float fs, float bottom)
{
	ImDrawList* dl = ImGui::GetBackgroundDrawList(); // over the picture, under ImGui windows
	ImFont* font   = ImGui::GetFont();
	const double now = nowMs();
	const float x0   = bx + bh * 0.02f;
	const float wrap = bw * 0.55f;
	float y          = bottom;
	std::lock_guard<std::mutex> lk(sMu);
	int shown = 0;
	for (auto it = sLog.rbegin(); it != sLog.rend() && shown < (sOpen ? kShowOpen : kShowClosed); ++it) {
		const double age = now - it->at;
		float alpha      = 1.0f;
		if (!sOpen) {
			if (age > kLineMs + kFadeMs)
				break;
			if (age > kLineMs)
				alpha = (float)(1.0 - (age - kLineMs) / kFadeMs);
		}
		char prefix[PORT_MP_NAME_MAX + 8];
		if (it->kind == LINE_CHAT)
			std::snprintf(prefix, sizeof(prefix), "%s: ", it->name);
		else if (it->kind == LINE_ACTION || it->kind == LINE_EMOTE)
			std::snprintf(prefix, sizeof(prefix), "* %s%s", it->name, it->kind == LINE_EMOTE ? ": " : " ");
		else
			prefix[0] = '\0';
		const ImVec2 ps = font->CalcTextSizeA(fs, 1e9f, 0.0f, prefix);
		const ImVec2 ts = font->CalcTextSizeA(fs, 1e9f, wrap - ps.x, it->text);
		const float h   = ts.y > ps.y ? ts.y : ps.y;
		y -= h + fs * 0.18f;
		if (y < by + bh * 0.35f)
			break;
		const int a  = (int)(alpha * 255.0f);
		const float w = ps.x + ts.x;
		dl->AddRectFilled(ImVec2(x0 - fs * 0.3f, y - fs * 0.08f), ImVec2(x0 + w + fs * 0.3f, y + h + fs * 0.08f),
		                  IM_COL32(0, 0, 0, (int)(alpha * 130.0f)), fs * 0.25f);
		const unsigned textRgb = it->kind == LINE_SYSTEM ? 0xC8C8C8 : it->kind == LINE_EMOTE ? 0xB4B4B4 : 0xFFFFFF;
		if (prefix[0] != '\0') {
			dl->AddText(font, fs, ImVec2(x0 + 1, y + 1), IM_COL32(0, 0, 0, a), prefix);
			dl->AddText(font, fs, ImVec2(x0, y), col(it->rgb, a), prefix);
		}
		dl->AddText(font, fs, ImVec2(x0 + ps.x + 1, y + 1), IM_COL32(0, 0, 0, a), it->text, nullptr, wrap - ps.x);
		dl->AddText(font, fs, ImVec2(x0 + ps.x, y), col(textRgb, a), it->text, nullptr, wrap - ps.x);
		shown++;
	}
}

void appendChar(char ch)
{
	const int n = (int)std::strlen(sBuf);
	if (n < kMaxText) {
		sBuf[n]     = ch;
		sBuf[n + 1] = '\0';
	}
}

// On-screen keyboard for controller users (the same layout as w10's lobby menus).
// Returns 1 = send, -1 = cancel, 0 = still open.
int drawOsk(float bx, float by, float bw, float bh)
{
	int result = 0;
	ImGui::SetNextWindowPos(ImVec2(bx + bw * 0.5f, by + bh * 0.62f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowBgAlpha(0.92f);
	if (ImGui::Begin("Chat###mpchatosk", nullptr,
	                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::SetWindowFontScale(sUiScale);
		ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.4f, 1.0f), "%s_", sBuf);
		static const char* const kRows[] = { "1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.?", "!-:;()/@#&" };
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
				if (r == 1 && c == 0 && sFocus) {
					ImGui::SetKeyboardFocusHere();
					sFocus = false;
				}
				if (ImGui::Button(lab, ImVec2(kw, 0.0f)))
					appendChar(ch);
				ImGui::PopID();
			}
		}
		if (ImGui::Button(sOskShift ? "abc" : "ABC", ImVec2(kw * 2.0f, 0.0f)))
			sOskShift = !sOskShift;
		ImGui::SameLine();
		if (ImGui::Button("Space", ImVec2(kw * 3.0f, 0.0f)))
			appendChar(' ');
		ImGui::SameLine();
		if (ImGui::Button("Delete", ImVec2(kw * 1.8f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {
			const int n = (int)std::strlen(sBuf);
			if (n > 0)
				sBuf[n - 1] = '\0';
		}
		ImGui::SameLine();
		if (ImGui::Button("Send", ImVec2(kw * 1.8f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_GamepadStart, false))
			result = 1;
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(kw * 1.8f, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)
		    || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			result = -1;
		ImGui::TextDisabled("A: type   X: delete   Start: send   B: cancel");
	}
	ImGui::End();
	return result;
}

// Keyboard input line under the chat lines. Returns 1 = send, -1 = cancel, 0 = still open.
int drawInputLine(float bx, float by, float bw, float bh, float fs)
{
	int result = 0;
	const float x0 = bx + bh * 0.02f;
	ImGui::SetNextWindowPos(ImVec2(x0 - fs * 0.3f, by + bh * kInputBottom), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
	ImGui::SetNextWindowSize(ImVec2(bw * 0.55f + fs * 0.6f, 0.0f), ImGuiCond_Always);
	ImGui::SetNextWindowBgAlpha(0.75f);
	if (ImGui::Begin("###mpchatline", nullptr,
	                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove
	                     | ImGuiWindowFlags_AlwaysAutoResize)) {
		ImGui::SetWindowFontScale(fs / sBaseFont);
		ImGui::TextUnformatted("Say:");
		ImGui::SameLine();
		ImGui::SetNextItemWidth(-1.0f);
		if (sFocus) {
			ImGui::SetKeyboardFocusHere();
			sFocus = false;
		}
		if (ImGui::InputText("##say", sBuf, sizeof(sBuf), ImGuiInputTextFlags_EnterReturnsTrue))
			result = 1;
		if (ImGui::IsKeyPressed(ImGuiKey_Escape, false))
			result = -1;
		else if (result == 0 && !ImGui::IsItemActive() && !ImGui::IsItemFocused() && !sFocus
		         && ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered())
			result = -1; // clicked elsewhere
	}
	ImGui::End();
	return result;
}

void drawEmoteMenu(float bx, float by, float bw, float bh)
{
	ImGui::SetNextWindowPos(ImVec2(bx + bw * 0.5f, by + bh * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::SetNextWindowBgAlpha(0.9f);
	bool close = false;
	if (ImGui::Begin("Emotes###mpemotes", nullptr,
	                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
		ImGui::SetWindowFontScale(sUiScale);
		static const char* const kDir[4] = { "D-pad up", "D-pad right", "D-pad down", "D-pad left" };
		const float w = ImGui::GetFontSize() * 13.0f;
		int shown = 0;
		for (int i = 0; i < port_mp_emote_count(); i++) {
			if (!port_mp_emote_in_wheel(i)) // the emote locker (Customize) picks them
				continue;
			char lab[64];
			std::snprintf(lab, sizeof(lab), "%d  %s", shown + 1, port_mp_emote_label(i));
			if (shown++ == 0 && sEmoteFocus) {
				ImGui::SetKeyboardFocusHere();
				sEmoteFocus = false;
			}
			const bool hit = ImGui::Button(lab, ImVec2(w, 0.0f))
			                 || (shown <= 9 && ImGui::IsKeyPressed((ImGuiKey)(ImGuiKey_1 + shown - 1), false));
			const int d    = port_mp_emote_dpad(i);
			if (d >= 0) {
				ImGui::SameLine();
				ImGui::TextDisabled("%s", kDir[d]);
			}
			if (hit) {
				port_mp_emote_request(i);
				close = true;
			}
		}
		ImGui::TextDisabled("Mario must stand still. Moving ends an emote.");
		if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)
		    || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_G, false))
			close = true;
	}
	ImGui::End();
	if (close)
		closeEmoteMenu();
}

// Controller buttons the game never uses (stick clicks), read from SDL directly: ImGui only
// sees gamepads while a menu enables gamepad navigation.
bool padPressed(SDL_GamepadButton b, bool& last)
{
	bool down = false;
	int n     = 0;
	if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
		for (int i = 0; i < n && !down; i++)
			if (SDL_Gamepad* g = SDL_GetGamepadFromID(ids[i]))
				down = SDL_GetGamepadButton(g, b);
		SDL_free(ids);
	}
	const bool hit = down && !last;
	last           = down;
	return hit;
}

// A key opens our boxes only when [keyboard] does not bind it to a pad button.
bool keyFree(int scancode)
{
	for (int i = 0; i < PORT_KEY_COUNT; i++)
		if (gPortSettings.keys[i] == scancode)
			return false;
	return true;
}

// SMS_PORT_MP_CHAT_AUTO=<slot>@<frame>:<text>[|...] (debug, mp_test.py): at that presented
// frame the instance with that local slot sends <text> as if typed (commands too). Special
// texts: "!open:<text>" opens the input line with <text>, "!osk:<text>" the on-screen
// keyboard, "!emotes" the emote menu, "!close" closes them, "!chatoff" (host) turns chat off.
void autoChat()
{
	static const char* sEnv = std::getenv("SMS_PORT_MP_CHAT_AUTO");
	static unsigned long long sDone;
	if (sEnv == nullptr)
		return;
	const int slot     = port_mp_local_slot();
	const unsigned now = port_frame_count();
	int idx            = 0;
	for (const char* p = sEnv; *p != '\0' && idx < 64; ++idx) {
		char* end;
		const long s = std::strtol(p, &end, 10);
		if (*end != '@')
			break;
		const unsigned long f = std::strtoul(end + 1, &end, 10);
		if (*end != ':')
			break;
		const char* text = end + 1;
		const char* bar  = std::strchr(text, '|');
		const size_t len = bar != nullptr ? (size_t)(bar - text) : std::strlen(text);
		if (s == slot && now >= f && (sDone & (1ull << idx)) == 0) {
			sDone |= 1ull << idx;
			std::string t(text, len);
			if (t.rfind("!open:", 0) == 0)
				openChat(false, t.c_str() + 6);
			else if (t.rfind("!osk:", 0) == 0)
				openChat(true, t.c_str() + 5);
			else if (t == "!emotes")
				openEmoteMenu();
			else if (t == "!chatoff" && port_mp_is_host()) { // the host's lobby checkbox
				PortMpSessionSettings ss;
				port_mp_get_settings(&ss);
				ss.reserved[1] |= 1;
				port_mp_host_set_settings(&ss);
				sysLine("Chat turned off for the session.");
			}
			else if (t == "!close") {
				closeChat();
				closeEmoteMenu();
			} else
				sendChat(t.c_str());
		}
		p = bar != nullptr ? bar + 1 : text + len;
	}
}

void overlay()
{
	if (ImGui::GetCurrentContext() == nullptr)
		return;
	trackPlayers();
	receive();
	autoChat();

	// Shortcuts, only when no other menu or text field has the keyboard.
	static bool sR3, sL3, sScriptT, sScriptG;
	const bool r3 = padPressed(SDL_GAMEPAD_BUTTON_RIGHT_STICK, sR3);
	const bool l3 = padPressed(SDL_GAMEPAD_BUTTON_LEFT_STICK, sL3);
	const bool st = port_input_script_key(SDL_SCANCODE_T), sg = port_input_script_key(SDL_SCANCODE_G);
	const bool tKey = (st && !sScriptT), gKey = (sg && !sScriptG);
	sScriptT = st, sScriptG = sg;
	const bool othersBusy = port_menu_is_open() || (port_mp_menu_wants_input() && sInputRequests == 0);
	if (!sOpen && !sEmoteMenu && !othersBusy && !ImGui::GetIO().WantTextInput) {
		if ((ImGui::IsKeyPressed(ImGuiKey_T, false) && keyFree(SDL_SCANCODE_T)) || tKey
		    || (ImGui::IsKeyPressed(ImGuiKey_Enter, false) && keyFree(SDL_SCANCODE_RETURN))
		    || (ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) && keyFree(SDL_SCANCODE_KP_ENTER)))
			openChat(false);
		else if (ImGui::IsKeyPressed(ImGuiKey_Slash, false) && keyFree(SDL_SCANCODE_SLASH))
			openChat(false, "/");
		else if (r3)
			openChat(true);
		else if ((ImGui::IsKeyPressed(ImGuiKey_G, false) && keyFree(SDL_SCANCODE_G)) || gKey || l3)
			openEmoteMenu();
	}

	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	float bx, by, bw, bh;
	contentBox(disp.x, disp.y, bx, by, bw, bh);
	const float base = ImGui::GetFontSize();
	const float fs   = bh * 0.026f > base ? bh * 0.026f : base;
	sBaseFont        = base;
	sUiScale         = bh / 720.0f > 1.0f ? bh / 720.0f : 1.0f;

	if (sOpen && port_menu_is_open()) // F1 on top: put the box away
		closeChat();
	if (sOpen) {
		const int r = sOsk ? drawOsk(bx, by, bw, bh) : drawInputLine(bx, by, bw, bh, fs);
		if (r == 1) {
			if (sendChat(sBuf))
				sBuf[0] = '\0';
			closeChat();
		} else if (r == -1) {
			closeChat();
		}
	}
	if (sEmoteMenu)
		drawEmoteMenu(bx, by, bw, bh);
	drawLog(bx, by, bw, bh, fs, by + bh * kInputBottom - (sOpen && !sOsk ? fs * 2.0f : 0.0f));
}

struct Register {
	Register()
	{
		port_mp_register_overlay(overlay); // (chat and emote switches: the title menu's Host a Lobby page)
	}
} sRegister;

} // namespace

extern "C" {

// System line from any thread (port_mp_emote.cpp).
void port_mp_chat_system(const char* text) { addLine(LINE_SYSTEM, 0xC8C8C8, "", text); }

// w15 (speech bubbles, port_mp_overlay.cpp): the latest chat line or emote of `slot`.
// Returns 1 and fills text / emote id (-1 = chat line) / time (port_mp_time_ms) if there
// is one this session, else 0.
int port_mp_chat_last_said(int slot, char* text, int cap, int* emote, double* atMs)
{
	if (slot < 0 || slot >= PORT_MP_MAX_PLAYERS)
		return 0;
	std::lock_guard<std::mutex> lk(sMu);
	const Said& s = sSaid[slot];
	if (s.at <= 0.0)
		return 0;
	if (text != nullptr && cap > 0)
		std::snprintf(text, cap, "%s", s.text);
	if (emote != nullptr)
		*emote = s.emote;
	if (atMs != nullptr)
		*atMs = s.at;
	return 1;
}

// w15: the local player's own emote (port_mp_emote.cpp announce), for its bubble.
void port_mp_chat_note_emote(int id)
{
	if (id >= 0 && id < port_mp_emote_count())
		noteSaid(port_mp_local_slot(), port_mp_emote_label(id), id, false);
}

} // extern "C"

#endif // SMS_PORT_AURORA
