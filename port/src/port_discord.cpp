// Discord Rich Presence + invites through Discord's local RPC IPC pipe. No Discord SDK.
//
// Protocol (documented RPC over IPC): connect to \\.\pipe\discord-ipc-0..9; every frame is
// [u32 opcode][u32 length][UTF-8 JSON], little-endian. Opcodes: 0 HANDSHAKE {"v":1,"client_id"},
// 1 FRAME, 2 CLOSE, 3 PING, 4 PONG. We send SET_ACTIVITY, SUBSCRIBE (ACTIVITY_JOIN,
// ACTIVITY_JOIN_REQUEST), SEND_ACTIVITY_JOIN_INVITE and CLOSE_ACTIVITY_REQUEST.
//
// Threading: a worker thread owns the pipe (connect/reconnect with backoff, reads, writes). The
// game thread only calls port_discord_frame(): once a second it copies a few game values into a
// snapshot (read-only), takes finished events (join, ask-to-join) off a queue, and draws the
// ask-to-join prompt. Nothing here can block the game; with Discord closed the cost is one
// failed CreateFile sweep every 5..30 s on the worker thread.
//
// Join secret: "sms1:<lobby name>" for lobbies on the lobby server without a password (the same
// thing the in-game "Join by name" flow needs). Direct (IP) sessions and password lobbies never
// publish a secret, so nothing beyond the lobby name leaves the game.
#ifdef SMS_PORT_AURORA

#include "port_discord.h"
#include "port_mp.h"
#include "port_settings.h"

#include <System/Application.hpp>
#include <System/FlagManager.hpp>
#include <System/FlagManagerFlags.hpp>

#include <imgui.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" void port_mp_toast(const char* text);                                     // port_mp_menu.cpp
extern "C" const char* port_mp_lobby_name(void);                                     // port_mp_net.cpp
extern "C" int port_mp_lobby_has_password(void);                                     // port_mp_net.cpp
extern "C" void port_mp_stage_label(int stage, int scenario, char* out, size_t cap); // port_mp_presence.cpp

namespace {

// ---- tiny JSON ----------------------------------------------------------------------------
struct J {
	char t = 0; // 0 null, 's' string, 'n' number, 'b' bool, 'o' object, 'a' array
	std::string s;
	double n = 0;
	std::vector<std::string> keys;
	std::vector<J> vals; // object values (parallel to keys) or array items

	const J* get(const char* k) const
	{
		if (t == 'o')
			for (size_t i = 0; i < keys.size(); ++i)
				if (keys[i] == k)
					return &vals[i];
		return nullptr;
	}
	std::string str(const char* k) const
	{
		const J* v = get(k);
		return v && v->t == 's' ? v->s : std::string();
	}
};

struct Parser {
	const char* p;
	const char* e;
	int depth = 0;
	void ws()
	{
		while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n'))
			++p;
	}
	static void utf8(std::string& o, unsigned c)
	{
		if (c < 0x80) o += (char)c;
		else if (c < 0x800) { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 63)); }
		else if (c < 0x10000) { o += (char)(0xE0 | c >> 12); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
		else { o += (char)(0xF0 | c >> 18); o += (char)(0x80 | ((c >> 12) & 63)); o += (char)(0x80 | ((c >> 6) & 63)); o += (char)(0x80 | (c & 63)); }
	}
	bool hex4(unsigned& v)
	{
		if (e - p < 4) return false;
		v = 0;
		for (int i = 0; i < 4; ++i, ++p) {
			char c = *p;
			v <<= 4;
			if (c >= '0' && c <= '9') v |= c - '0';
			else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
			else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
			else return false;
		}
		return true;
	}
	bool str(std::string& o)
	{
		if (p >= e || *p != '"') return false;
		++p;
		while (p < e && *p != '"') {
			if (*p != '\\') { o += *p++; continue; }
			if (++p >= e) return false;
			char c = *p++;
			switch (c) {
			case 'b': o += '\b'; break;
			case 'f': o += '\f'; break;
			case 'n': o += '\n'; break;
			case 'r': o += '\r'; break;
			case 't': o += '\t'; break;
			case 'u': {
				unsigned v;
				if (!hex4(v)) return false;
				if (v >= 0xD800 && v < 0xDC00 && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
					p += 2;
					unsigned lo;
					if (!hex4(lo)) return false;
					v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
				}
				utf8(o, v);
				break;
			}
			default: o += c;
			}
		}
		if (p >= e) return false;
		++p;
		return true;
	}
	bool value(J& v)
	{
		if (++depth > 16) return false;
		struct D { int& d; ~D() { --d; } } guard{ depth };
		ws();
		if (p >= e) return false;
		if (*p == '"') { v.t = 's'; return str(v.s); }
		if (*p == '{') {
			v.t = 'o';
			++p; ws();
			if (p < e && *p == '}') { ++p; return true; }
			for (;;) {
				ws();
				std::string k;
				if (!str(k)) return false;
				ws();
				if (p >= e || *p++ != ':') return false;
				J c;
				if (!value(c)) return false;
				v.keys.push_back(std::move(k));
				v.vals.push_back(std::move(c));
				ws();
				if (p >= e) return false;
				if (*p == ',') { ++p; continue; }
				if (*p == '}') { ++p; return true; }
				return false;
			}
		}
		if (*p == '[') {
			v.t = 'a';
			++p; ws();
			if (p < e && *p == ']') { ++p; return true; }
			for (;;) {
				J c;
				if (!value(c)) return false;
				v.vals.push_back(std::move(c));
				ws();
				if (p >= e) return false;
				if (*p == ',') { ++p; continue; }
				if (*p == ']') { ++p; return true; }
				return false;
			}
		}
		if (e - p >= 4 && !std::memcmp(p, "true", 4)) { v.t = 'b'; v.n = 1; p += 4; return true; }
		if (e - p >= 5 && !std::memcmp(p, "false", 5)) { v.t = 'b'; p += 5; return true; }
		if (e - p >= 4 && !std::memcmp(p, "null", 4)) { p += 4; return true; }
		char* end = nullptr;
		v.n = std::strtod(p, &end);
		if (end == p) return false;
		v.t = 'n';
		p = end;
		return true;
	}
};

bool parseJson(const std::string& text, J& out)
{
	Parser ps{ text.data(), text.data() + text.size() };
	return ps.value(out);
}

std::string q(const std::string& v) // JSON string literal
{
	std::string o = "\"";
	for (unsigned char c : v) {
		switch (c) {
		case '\\': o += "\\\\"; break;
		case '"': o += "\\\""; break;
		case '\n': o += "\\n"; break;
		case '\r': o += "\\r"; break;
		case '\t': o += "\\t"; break;
		default:
			if (c < 0x20) {
				char b[8];
				std::snprintf(b, sizeof(b), "\\u%04x", c);
				o += b;
			} else {
				o += (char)c;
			}
		}
	}
	return o + '"';
}

// ---- state ---------------------------------------------------------------------------------
enum Status { ST_IDLE, ST_DISABLED, ST_NO_CLIENT_ID, ST_SEARCHING, ST_CONNECTING, ST_CONNECTED };

struct Snap {
	int appState = 0, stage = 0xFF, scenario = 0, shines = -1, blueCoins = -1;
	bool mpActive = false, mpServer = false, hasPassword = false;
	int players = 1, maxPlayers = 1;
	std::string lobby;
	bool showStage = true, allowJoin = true, valid = false;
};

struct JoinRequest {
	std::string userId, name;
	double at = 0; // seconds (steady clock)
};

struct Cmd { // worker -> pipe
	std::string cmd, args;
};

struct State {
	std::atomic<int> status{ ST_IDLE };
	std::atomic<bool> enabled{ false }, stop{ false }, done{ false }, sampleWanted{ false };
	std::string clientId, pipeBase;
	long long started = 0;
	std::thread th;
	std::mutex mu; // guards everything below
	Snap snap;
	std::string user;                 // connected Discord user name
	std::deque<std::string> joins;    // lobby names received via ACTIVITY_JOIN
	std::deque<JoinRequest> requests; // ask-to-join
	std::deque<Cmd> out;              // replies to send
	char statusText[160] = "";
};

State& S = *new State; // leaked on purpose: the worker may outlive static destruction at exit
bool sInit;
bool sAtexit;

double nowSec()
{
	return GetTickCount64() / 1000.0;
}

// ---- pipe ----------------------------------------------------------------------------------
struct Pipe {
	HANDLE h = INVALID_HANDLE_VALUE;
	bool open() const { return h != INVALID_HANDLE_VALUE; }
	void close()
	{
		if (open())
			CloseHandle(h);
		h = INVALID_HANDLE_VALUE;
	}
	bool send(unsigned op, const std::string& payload)
	{
		if (!open())
			return false;
		std::string buf(8 + payload.size(), '\0');
		unsigned hdr[2] = { op, (unsigned)payload.size() };
		std::memcpy(&buf[0], hdr, 8);
		std::memcpy(&buf[8], payload.data(), payload.size());
		DWORD w = 0;
		if (!WriteFile(h, buf.data(), (DWORD)buf.size(), &w, nullptr) || w != buf.size()) {
			close();
			return false;
		}
		return true;
	}
	// 1 = frame read, 0 = nothing yet, -1 = pipe broken
	int recv(unsigned& op, std::string& payload)
	{
		DWORD avail = 0;
		if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr))
			return -1;
		if (avail < 8)
			return 0;
		unsigned hdr[2];
		DWORD r = 0;
		if (!ReadFile(h, hdr, 8, &r, nullptr) || r != 8 || hdr[1] > (1u << 20))
			return -1;
		op = hdr[0];
		payload.assign(hdr[1], '\0');
		DWORD got = 0;
		ULONGLONG until = GetTickCount64() + 3000;
		while (got < hdr[1]) {
			DWORD a = 0;
			if (!PeekNamedPipe(h, nullptr, 0, nullptr, &a, nullptr))
				return -1;
			if (a == 0) {
				if (GetTickCount64() > until)
					return -1;
				Sleep(5);
				continue;
			}
			DWORD want = a < hdr[1] - got ? a : hdr[1] - got;
			if (!ReadFile(h, &payload[got], want, &r, nullptr) || r == 0)
				return -1;
			got += r;
		}
		return 1;
	}
};

void setStatus(int st, const char* text)
{
	S.status = st;
	std::lock_guard<std::mutex> lk(S.mu);
	std::snprintf(S.statusText, sizeof(S.statusText), "%s", text);
}

// ---- presence content ----------------------------------------------------------------------
const char* worldKey(int stage)
{
	switch (stage) {
	case 0: return "airport";
	case 1: case 56: return "delfino";
	case 2: case 55: return "bianco";
	case 3: case 59: return "ricco";
	case 4: return "gelato";
	case 5: case 13: case 58: return "pinna";
	case 6: case 7: case 14: return "sirena";
	case 8: return "monte";
	case 9: case 16: case 57: return "noki";
	case 60: return "corona";
	}
	if (stage >= 20 && stage <= 52) { // secret courses, see port_mp_presence.cpp kExNames
		const int i = stage - 20;
		if (i < 8) return "delfino";
		if (i < 10) return "bianco";
		if (i < 12) return "ricco";
		if (i < 14) return "gelato";
		if (i < 20) return "pinna";
		if (i < 22) return "sirena";
		if (i < 24) return "monte";
		if (i < 26) return "noki";
		return "corona";
	}
	return nullptr;
}

std::string makeActivity(const Snap& s, const std::string& nonce, bool& joinable)
{
	std::string details, state;
	const char* key = nullptr;
	std::string place;
	bool inGame = s.appState == TApplication::APP_STATE_GAMEPLAY;
	switch (s.appState) {
	case TApplication::APP_STATE_GAMEPLAY: break;
	case TApplication::APP_STATE_MOVIE: details = "Watching a cutscene"; break;
	case TApplication::APP_STATE_TITLE: details = "At the title screen"; break;
	case TApplication::APP_STATE_MENU: details = "Choosing a save file"; break;
	case TApplication::APP_STATE_BOOT:
	case TApplication::APP_STATE_NLOGO: details = "Starting the game"; break;
	default: details = "In the menus"; break;
	}
	if (inGame) {
		if (!s.showStage) {
			details = "Playing Super Mario Sunshine";
		} else {
			char label[160];
			port_mp_stage_label(s.stage, s.scenario, label, sizeof(label));
			details = label;
			std::string episode;
			const size_t c = details.find(", episode ");
			if (c != std::string::npos) {
				episode = "Episode " + details.substr(c + 10);
				details.resize(c);
			}
			place = details;
			key = worldKey(s.stage);
			if (s.stage == 1) state = "Exploring";
			else if (!episode.empty()) state = episode;
			if (s.shines >= 0) {
				char b[64];
				std::snprintf(b, sizeof(b), "%d Shine%s", s.shines, s.shines == 1 ? "" : "s");
				state += (state.empty() ? "" : " - ") + std::string(b);
				if (s.blueCoins > 0) {
					std::snprintf(b, sizeof(b), ", %d Blue Coin%s", s.blueCoins, s.blueCoins == 1 ? "" : "s");
					state += b;
				}
			}
		}
	}
	if (s.mpActive) {
		state = std::string("Co-op") + (state.empty() ? "" : " - " + state);
		if (!s.lobby.empty() && s.showStage && !inGame)
			state += " - " + s.lobby;
	}
	if (state.size() > 120) state.resize(120);
	if (details.size() > 120) details.resize(120);

	joinable = s.mpActive && s.mpServer && !s.hasPassword && s.allowJoin && !s.lobby.empty()
	           && s.lobby.size() < 100;

	std::string a = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId())
	                + ",\"activity\":{\"details\":" + q(details);
	if (!state.empty())
		a += ",\"state\":" + q(state);
	a += ",\"timestamps\":{\"start\":" + std::to_string(S.started) + "}";
	a += ",\"assets\":{\"large_image\":\"logo\",\"large_text\":\"Super Mario Sunshine (PC port)\"";
	if (key && !place.empty())
		a += std::string(",\"small_image\":") + q(key) + ",\"small_text\":" + q(place);
	a += "}";
	if (s.mpActive) {
		const std::string id = s.lobby.empty() ? "sms-port" : "sms-" + s.lobby;
		a += ",\"party\":{\"id\":" + q(id) + ",\"size\":[" + std::to_string(s.players) + ","
		     + std::to_string(s.maxPlayers < s.players ? s.players : s.maxPlayers) + "]}";
		if (joinable)
			a += ",\"secrets\":{\"join\":" + q("sms1:" + s.lobby) + "}";
	}
	a += ",\"instance\":" + std::string(s.mpActive ? "true" : "false") + "}},\"nonce\":" + q(nonce) + "}";
	return a;
}

// ---- worker ----------------------------------------------------------------------------------
void worker()
{
	Pipe pipe;
	bool ready = false;
	ULONGLONG nextTry = 0, lastSend = 0, handshakeAt = 0, lastPing = 0;
	DWORD backoff = 5000;
	unsigned long long nonce = 0;
	std::string lastActivity;
	bool joinSub = false;
	auto nextNonce = [&] { return std::to_string(++nonce); };

	auto drop = [&](const char* why, DWORD retry) {
		if (pipe.open() || ready)
			std::fprintf(stderr, "[discord] disconnected (%s)\n", why);
		pipe.close();
		ready = false;
		lastActivity.clear();
		S.sampleWanted = false;
		nextTry = GetTickCount64() + retry;
		setStatus(S.enabled ? ST_SEARCHING : ST_DISABLED,
		          S.enabled ? "Discord is not running (will keep looking)" : "Off");
	};

	while (!S.stop) {
		const ULONGLONG now = GetTickCount64();
		if (!S.enabled) {
			if (pipe.open() && ready)
				pipe.send(1, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId())
				                 + "},\"nonce\":" + q(nextNonce()) + "}");
			if (pipe.open() || ready) {
				pipe.close();
				ready = false;
				std::fprintf(stderr, "[discord] disconnected (switched off)\n");
			}
			setStatus(ST_DISABLED, "Off");
			Sleep(250);
			continue;
		}
		if (!pipe.open()) {
			if (now < nextTry) {
				Sleep(100);
				continue;
			}
			for (int i = 0; i < 10 && !pipe.open(); ++i) {
				std::string path = S.pipeBase + std::to_string(i);
				pipe.h = CreateFileA(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
			}
			if (!pipe.open()) {
				nextTry = GetTickCount64() + backoff;
				backoff = backoff < 30000 ? backoff * 2 : 30000;
				setStatus(ST_SEARCHING, "Discord is not running (will keep looking)");
				continue;
			}
			if (!pipe.send(0, "{\"v\":1,\"client_id\":" + q(S.clientId) + "}")) {
				drop("handshake write failed", 5000);
				continue;
			}
			handshakeAt = now;
			setStatus(ST_CONNECTING, "Connecting to Discord...");
		}

		// inbound
		for (int guard = 0; guard < 32 && pipe.open(); ++guard) {
			unsigned op = 0;
			std::string payload;
			int r = pipe.recv(op, payload);
			if (r < 0) {
				drop("pipe closed", 5000);
				break;
			}
			if (r == 0)
				break;
			if (op == 3) { // PING
				pipe.send(4, payload);
				continue;
			}
			J j;
			const bool ok = parseJson(payload, j);
			if (op == 2) {
				const J* code = ok ? j.get("code") : nullptr;
				std::fprintf(stderr, "[discord] Discord closed the connection: %s (code %d)\n",
				             ok ? j.str("message").c_str() : "?", code ? (int)code->n : 0);
				const bool badId = code && ((int)code->n == 4000);
				drop("closed by Discord", badId ? 120000 : 10000);
				if (badId)
					setStatus(ST_NO_CLIENT_ID, "Discord rejected the client_id (check the Application ID)");
				break;
			}
			if (op != 1 || !ok)
				continue;
			const std::string evt = j.str("evt");
			const J* data = j.get("data");
			if (evt == "READY") {
				ready = true;
				backoff = 5000;
				std::string user;
				if (data)
					if (const J* u = data->get("user"))
						user = u->str("username");
				{
					std::lock_guard<std::mutex> lk(S.mu);
					S.user = user;
				}
				std::fprintf(stderr, "[discord] connected as %s\n", user.empty() ? "?" : user.c_str());
				char t[128];
				std::snprintf(t, sizeof(t), "Connected%s%s", user.empty() ? "" : " as ", user.c_str());
				setStatus(ST_CONNECTED, t);
				S.sampleWanted = true;
				joinSub = false;
			} else if (evt == "ACTIVITY_JOIN" && data) {
				std::string secret = data->str("secret");
				std::lock_guard<std::mutex> lk(S.mu);
				if (S.joins.size() < 4)
					S.joins.push_back(secret);
			} else if (evt == "ACTIVITY_JOIN_REQUEST" && data) {
				JoinRequest rq;
				if (const J* u = data->get("user")) {
					rq.userId = u->str("id");
					rq.name = u->str("global_name");
					if (rq.name.empty()) rq.name = u->str("username");
				}
				rq.at = nowSec();
				if (!rq.userId.empty()) {
					std::lock_guard<std::mutex> lk(S.mu);
					if (S.requests.size() < 8)
						S.requests.push_back(rq);
				}
			} else if (evt == "ERROR") {
				std::fprintf(stderr, "[discord] error reply: %s\n", payload.substr(0, 300).c_str());
			}
		}
		if (!pipe.open())
			continue;
		if (!ready) {
			if (now - handshakeAt > 10000)
				drop("handshake timeout", 10000);
			Sleep(50);
			continue;
		}

		// subscriptions (once per connection)
		if (!joinSub) {
			joinSub = true;
			pipe.send(1, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN\",\"nonce\":" + q(nextNonce()) + "}");
			pipe.send(1, "{\"cmd\":\"SUBSCRIBE\",\"evt\":\"ACTIVITY_JOIN_REQUEST\",\"nonce\":" + q(nextNonce()) + "}");
		}

		// replies to ask-to-join prompts
		for (;;) {
			Cmd c;
			{
				std::lock_guard<std::mutex> lk(S.mu);
				if (S.out.empty())
					break;
				c = S.out.front();
				S.out.pop_front();
			}
			pipe.send(1, "{\"cmd\":" + q(c.cmd) + ",\"args\":" + c.args + ",\"nonce\":" + q(nextNonce()) + "}");
		}

		// presence (Discord allows about 5 updates per 20 s)
		if (pipe.open() && now - lastSend >= 4000) {
			Snap snap;
			{
				std::lock_guard<std::mutex> lk(S.mu);
				snap = S.snap;
			}
			if (snap.valid) {
				bool joinable = false;
				const std::string act = makeActivity(snap, "0", joinable);
				if (act != lastActivity) {
					if (pipe.send(1, makeActivity(snap, nextNonce(), joinable))) {
						lastActivity = act;
						lastSend = now;
						std::fprintf(stderr, "[discord] presence updated%s\n", joinable ? " (joinable)" : "");
					}
				}
			}
		}
		(void)lastPing;
		Sleep(100);
	}
	if (pipe.open() && ready)
		pipe.send(1, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId())
		                 + "},\"nonce\":" + q(nextNonce()) + "}");
	pipe.close();
	S.done = true;
}

// ---- game thread -----------------------------------------------------------------------------
void sample()
{
	Snap s;
	TApplication* app = &gpApplication;
	s.appState = app->mAppState;
	s.stage = app->mCurrArea.getStage();
	s.scenario = app->mCurrArea.getScenario();
	if (s.appState == TApplication::APP_STATE_GAMEPLAY && TFlagManager::getInstance()) {
		s.shines = TFlagManager::getInstance()->getFlag(MSF_SHINE_COUNT);
		s.blueCoins = TFlagManager::getInstance()->getFlag(MSF_BLUE_COIN_COUNT);
		if (s.shines < 0 || s.shines > 999) s.shines = -1;
		if (s.blueCoins < 0 || s.blueCoins > 999) s.blueCoins = -1;
	}
	s.showStage = gPortSettings.discordShowStage;
	s.allowJoin = gPortSettings.discordAllowJoin;
	if (port_mp_active()) {
		s.mpActive = true;
		s.mpServer = port_mp_conn_type() == PORT_MP_CONN_SERVER;
		s.hasPassword = port_mp_lobby_has_password() != 0;
		const char* lobby = port_mp_lobby_name();
		s.lobby = lobby ? lobby : "";
		PortMpSessionSettings ms;
		port_mp_get_settings(&ms);
		s.maxPlayers = ms.maxPlayers;
		s.players = 0;
		PortMpPlayerInfo pi;
		for (int i = 0; i < PORT_MP_MAX_PLAYERS; ++i)
			s.players += port_mp_get_player(i, &pi) != 0;
		if (s.players < 1) s.players = 1;
	}
	s.valid = true;
	std::lock_guard<std::mutex> lk(S.mu);
	S.snap = s;
}

void startThread()
{
	S.started = (long long)std::time(nullptr);
	S.enabled = true;
	setStatus(ST_SEARCHING, "Discord is not running (will keep looking)");
	std::fprintf(stderr, "[discord] started: looking for Discord (%s0..9)\n", S.pipeBase.c_str());
	S.th = std::thread(worker);
	if (!sAtexit) {
		sAtexit = true;
		std::atexit([] { port_discord_shutdown(); });
	}
}

void startup()
{
	sInit = true;
	// The port's own Discord application; players never set anything. [discord] client_id in
	// port_settings.ini only overrides it (e.g. a fork with its own app).
	static const char kBuiltInClientId[] = "1560555505439477850";
	std::string id = gPortSettings.discordClientId[0] != 0 ? gPortSettings.discordClientId : kBuiltInClientId;
	if (const char* env = std::getenv("SMS_PORT_DISCORD_APP_ID")) // overrides the ini for this run
		if (*env)
			id = env;
	const char* base = std::getenv("SMS_PORT_DISCORD_PIPE"); // test hook: fake IPC server
	S.pipeBase = base && *base ? base : "\\\\.\\pipe\\discord-ipc-";
	S.clientId = id;
	if (id.empty()) {
		setStatus(ST_NO_CLIENT_ID, "No client_id: set it under [discord] in port_settings.ini");
		if (gPortSettings.discordEnabled)
			std::fprintf(stderr, "[discord] idle: create an application at https://discord.com/developers/applications "
			                     "and put its Application ID in port_settings.ini [discord] client_id\n");
	} else if (!gPortSettings.discordEnabled) {
		setStatus(ST_DISABLED, "Off");
	}
}

void drawPrompt()
{
	if (ImGui::GetCurrentContext() == nullptr)
		return;
	JoinRequest rq;
	{
		std::lock_guard<std::mutex> lk(S.mu);
		if (S.requests.empty())
			return;
		rq = S.requests.front();
	}
	auto answer = [&](bool accept) {
		std::lock_guard<std::mutex> lk(S.mu);
		if (!S.requests.empty())
			S.requests.pop_front();
		S.out.push_back({ accept ? "SEND_ACTIVITY_JOIN_INVITE" : "CLOSE_ACTIVITY_REQUEST",
		                  "{\"user_id\":" + q(rq.userId) + "}" });
	};
	if (!gPortSettings.discordAllowJoin || nowSec() - rq.at > 30.0) {
		answer(false);
		return;
	}
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.04f), ImGuiCond_Always,
	                        ImVec2(0.5f, 0.0f));
	ImGui::SetNextWindowBgAlpha(0.9f);
	const ImGuiWindowFlags fl = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize
	                            | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing
	                            | ImGuiWindowFlags_NoSavedSettings;
	bool accept = false, ignore = false;
	if (ImGui::Begin("##discordjoin", nullptr, fl)) {
		ImGui::Text("Discord: %s wants to join your game", rq.name.empty() ? "Someone" : rq.name.c_str());
		if (ImGui::Button("Accept (F7)"))
			accept = true;
		ImGui::SameLine();
		if (ImGui::Button("Ignore (F8)"))
			ignore = true;
	}
	ImGui::End();
	static const char* sAuto = std::getenv("SMS_PORT_DISCORD_AUTOREPLY"); // test hook: "accept" / "ignore"
	if (sAuto && *sAuto) { accept = !std::strcmp(sAuto, "accept"); ignore = !accept; }
	if (ImGui::IsKeyPressed(ImGuiKey_F7, false)) accept = true;
	if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) ignore = true;
	if (accept) answer(true);
	else if (ignore) answer(false);
}

void handleJoins()
{
	std::string secret;
	{
		std::lock_guard<std::mutex> lk(S.mu);
		if (S.joins.empty())
			return;
		secret = S.joins.front();
		S.joins.pop_front();
	}
	if (!gPortSettings.discordAllowJoin || !port_mp_enabled()) {
		port_mp_toast("Discord join ignored (joining is off).");
		return;
	}
	std::string name;
	if (secret.compare(0, 5, "sms1:") == 0)
		name = secret.substr(5);
	bool ok = !name.empty() && name.size() < PORT_MP_LOBBY_NAME_MAX;
	for (unsigned char c : name)
		ok = ok && c >= 0x20 && c != 0x7F;
	if (!ok) {
		std::fprintf(stderr, "[discord] join: unrecognised secret\n");
		port_mp_toast("Discord invite not recognised (different game version?).");
		return;
	}
	std::fprintf(stderr, "[discord] join request for lobby \"%s\"\n", name.c_str());
	if (port_mp_active() && name == port_mp_lobby_name())
		return; // already there
	char t[160];
	std::snprintf(t, sizeof(t), "Joining \"%s\" from Discord...", name.c_str());
	port_mp_toast(t);
	port_mp_join_name(name.c_str(), "");
}

} // namespace

extern "C" void port_discord_frame(void)
{
	if (!sInit)
		startup();
	const bool want = gPortSettings.discordEnabled;
	if (!S.th.joinable()) {
		if (!want || S.clientId.empty())
			return; // idle: zero cost
		startThread();
	}
	if (S.enabled.load() != want) {
		S.enabled = want;
		if (!want)
			return;
	}
	if (S.status.load() != ST_CONNECTED)
		return; // Discord not running: nothing to feed
	static ULONGLONG sNext;
	const ULONGLONG now = GetTickCount64();
	if (S.sampleWanted.exchange(false) || now >= sNext) {
		sNext = now + 1000;
		sample();
	}
	handleJoins();
	drawPrompt();
}

extern "C" const char* port_discord_status(void)
{
	static char buf[160];
	std::lock_guard<std::mutex> lk(S.mu);
	if (!sInit)
		return "Not started yet";
	std::snprintf(buf, sizeof(buf), "%s", S.statusText[0] ? S.statusText : "Off");
	return buf;
}

extern "C" void port_discord_shutdown(void)
{
	if (!S.th.joinable())
		return;
	S.stop = true;
	for (int i = 0; i < 60 && !S.done; ++i)
		Sleep(10);
	if (S.done)
		S.th.join();
	else
		S.th.detach();
}

#endif
