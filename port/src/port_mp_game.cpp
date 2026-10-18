// Multiplayer game glue (w10): the file-select "run left" entry, free-mode session start
// on an in-memory save, stage presence, leaving, the debug auto-start, and the
// port_multiplayer.ini preferences. Called from tiny SMS_PORT hooks in CardLoad.cpp,
// Application.cpp and CardManager.cpp. See port/MULTIPLAYER.md.
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <System/Application.hpp>
#include <System/CardManager.hpp>
#include <System/FlagManager.hpp>
#include <System/MarDirector.hpp>
#include <System/MarioGamePad.hpp>
#include <Player/Mario.hpp>
#include <Player/MarioAccess.hpp>
#include <MoveBG/Item.hpp>
#include <MoveBG/ItemManager.hpp>
#include <Strategic/ActorTypes.hpp>
#include <System/StageUtil.hpp>
#include <JSystem/JDrama/JDRNameRefGen.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

extern "C" void port_mp_net_set_local_area(int stage, int scenario); // port_mp_net.cpp
extern "C" void port_mp_net_test_snapshot(const PortMpSnapshot* snap); // port_mp_net.cpp
extern "C" void port_apply_flag_items(const char* items);           // Application.cpp
extern "C" u32 port_frame_count(void);                               // port_main.cpp
extern "C" void port_mp_toast(const char* text);                     // port_mp_menu.cpp
extern "C" void port_mp_home_area(const PortMpSessionSettings* s, int* stage, int* scenario); // port_mp_arena.cpp
extern "C" int port_mp_arena_exit_lock_left(void);
extern "C" void port_mp_manhunt_shine_got(int shineId); // port_mp_manhunt.cpp
extern "C" void port_mp_shinerun_shine_got(int shineId); // port_mp_shinerun.cpp
extern "C" void port_mp_cnr_shine_got(int shineId);      // port_mp_cnr.cpp
extern "C" void port_mp_cnr_coin_taken(void* item);
extern "C" int port_mp_arena_route(int from, int fromScenario, int* nextState, unsigned char* stage,
                                   unsigned char* scenario);
extern "C" void port_mp_savecard_reset(void);                        // port_mp_savecard.cpp

namespace {

// ---------------------------------------------------------------------------------
// port_multiplayer.ini ([multiplayer]): name, port, enabled, menu defaults.
struct Prefs {
	char name[PORT_MP_NAME_MAX] = "Player";
	int port                     = PORT_MP_DEFAULT_PORT;
	int enabled                  = 1;
	char lobbyName[PORT_MP_LOBBY_NAME_MAX] = "Delfino Lobby";
	int maxPlayers               = 8;
	int sharedObjectives         = 0;
	int chat                     = 1; // lobbies we host: chat allowed
	int emotes                   = 1; // ... emotes allowed
	char lastJoin[64]            = "127.0.0.1";
	int lastJoinPort             = PORT_MP_DEFAULT_PORT;
	// lobby server (port/server): the project's own Oracle Cloud Always Free VM (DEPLOY.md)
	char server[128]             = "137.131.46.75:7700";
	int connType                 = PORT_MP_CONN_SERVER;
	char lastLobby[PORT_MP_LOBBY_NAME_MAX] = "";
};
Prefs sPrefs;
bool sPrefsLoaded;

std::string iniPath()
{
	if (const char* e = std::getenv("SMS_PORT_MP_INI"))
		if (*e != '\0')
			return e;
	std::string base;
	if (const char* s = std::getenv("SMS_PORT_SETTINGS"))
		base = s;
	if (base.empty()) {
		char buf[MAX_PATH];
		DWORD n = GetModuleFileNameA(nullptr, buf, MAX_PATH);
		base.assign(buf, n);
	}
	size_t slash = base.find_last_of("\\/");
	std::string dir = slash == std::string::npos ? std::string(".") : base.substr(0, slash);
	return dir + "\\port_multiplayer.ini";
}

void loadPrefs()
{
	if (sPrefsLoaded)
		return;
	sPrefsLoaded = true;
	FILE* f      = std::fopen(iniPath().c_str(), "rb");
	if (f == nullptr)
		return;
	char line[256];
	while (std::fgets(line, sizeof(line), f) != nullptr) {
		char* eq = std::strchr(line, '=');
		if (eq == nullptr || line[0] == '[' || line[0] == ';' || line[0] == '#')
			continue;
		*eq       = '\0';
		char* key = line;
		char* val = eq + 1;
		auto trim = [](char* s) {
			while (*s == ' ' || *s == '\t')
				s++;
			char* e = s + std::strlen(s);
			while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
				*--e = '\0';
			return s;
		};
		key = trim(key);
		val = trim(val);
		if (std::strcmp(key, "name") == 0)
			std::snprintf(sPrefs.name, sizeof(sPrefs.name), "%s", val);
		else if (std::strcmp(key, "port") == 0)
			sPrefs.port = std::atoi(val);
		else if (std::strcmp(key, "enabled") == 0)
			sPrefs.enabled = std::atoi(val) != 0;
		else if (std::strcmp(key, "lobby_name") == 0)
			std::snprintf(sPrefs.lobbyName, sizeof(sPrefs.lobbyName), "%s", val);
		else if (std::strcmp(key, "max_players") == 0)
			sPrefs.maxPlayers = std::atoi(val);
		else if (std::strcmp(key, "shared_objectives") == 0)
			sPrefs.sharedObjectives = std::atoi(val) != 0;
		else if (std::strcmp(key, "chat") == 0)
			sPrefs.chat = std::atoi(val) != 0;
		else if (std::strcmp(key, "emotes") == 0)
			sPrefs.emotes = std::atoi(val) != 0;
		else if (std::strcmp(key, "last_join") == 0)
			std::snprintf(sPrefs.lastJoin, sizeof(sPrefs.lastJoin), "%s", val);
		else if (std::strcmp(key, "last_join_port") == 0)
			sPrefs.lastJoinPort = std::atoi(val);
		else if (std::strcmp(key, "server") == 0) {
			// older builds saved the placeholder address; keep the built-in server instead
			if (val[0] != '\0' && std::strstr(val, "example.invalid") == nullptr)
				std::snprintf(sPrefs.server, sizeof(sPrefs.server), "%s", val);
		}
		else if (std::strcmp(key, "connection") == 0)
			sPrefs.connType = std::strcmp(val, "direct") == 0 ? PORT_MP_CONN_DIRECT : PORT_MP_CONN_SERVER;
		else if (std::strcmp(key, "last_lobby") == 0)
			std::snprintf(sPrefs.lastLobby, sizeof(sPrefs.lastLobby), "%s", val);
	}
	std::fclose(f);
	if (sPrefs.port <= 0 || sPrefs.port > 65535)
		sPrefs.port = PORT_MP_DEFAULT_PORT;
	if (sPrefs.lastJoinPort <= 0 || sPrefs.lastJoinPort > 65535)
		sPrefs.lastJoinPort = PORT_MP_DEFAULT_PORT;
	if (sPrefs.maxPlayers < 2 || sPrefs.maxPlayers > PORT_MP_MAX_PLAYERS)
		sPrefs.maxPlayers = 8;
}

// ---------------------------------------------------------------------------------
// Session glue state (game thread)
const int kMaxGameFrameFns = 32;
PortMpFrameFn sGameFrameFns[kMaxGameFrameFns]; // port_mp_register_game_frame (static-init registration)
int sGameFrameFnCount;
bool sMpSave;         // the flags in memory are the multiplayer save; card writes blocked
bool sReturnPending;  // left the session in a stage: going back to the file select
bool sHint;           // Mario has control on the file select (menu draws the hint)
int sLeftFrames;      // consecutive frames pushing left at the left boundary
bool sNeedRelease;    // after the menu closed: wait for the stick to come back
int sCurStage = -1, sCurScenario = -1, sCurAppState = -1;

// Left boundary of the save-block screen (area 15): Mario stops at x = 846 there
// (option stage collision); measured with SMS_PORT_TRACE.
const float kFileSelectLeftX = 870.0f;
const int kLeftFramesToOpen  = 30; // presented frames (60 Hz): half a second of running into it

// ---------------------------------------------------------------------------------
// Shared objectives (MULTIPLAYER.md; stage 4). Game thread only.
// TCoin::taken's names, in Shift-JIS like the game's (this file is UTF-8 and not mirrored):
// "シャイン（１００枚コイン用）" and "...カメラ".
const char k100CoinShine[] = "\x83\x56\x83\x83\x83\x43\x83\x93\x81\x69\x82\x50\x82\x4F\x82\x4F\x96\x87\x83\x52\x83"
                             "\x43\x83\x93\x97\x70\x81\x6A";
const char k100CoinShineCamera[] = "\x83\x56\x83\x83\x83\x43\x83\x93\x81\x69\x82\x50\x82\x4F\x82\x4F\x96\x87\x83\x52"
                                   "\x83\x43\x83\x93\x97\x70\x81\x6A\x83\x4A\x83\x81\x83\x89";
int sStaticItems = -1;     // gpItemManager objects loaded from the scene (before the pools)
bool sObjLatched;          // the shared setting has been read for this stage
bool sSharedStage;         // shared objectives are on for this stage visit
std::vector<u32> sObjDone; // ids applied or collected locally in this stage visit (+ persistent)
std::vector<TItem*> sPendingKill; // collected elsewhere while not out yet (red coins)

bool objDone(u32 id)
{
	for (u32 x : sObjDone)
		if (x == id)
			return true;
	return false;
}

int itemIndex(const THitActor* a)
{
	if (gpItemManager == nullptr)
		return -1;
	const int n = gpItemManager->getObjNum();
	for (int i = 0; i < n && i < sStaticItems; i++)
		if (gpItemManager->getObj(i) == a)
			return i;
	return -1; // a pool or run-time object: never shared
}

TItem* itemAt(int index)
{
	if (gpItemManager == nullptr || index < 0 || index >= sStaticItems || index >= gpItemManager->getObjNum())
		return nullptr;
	return (TItem*)gpItemManager->getObj(index);
}

const char* playerName(int slot)
{
	static PortMpPlayerInfo p;
	if (slot < PORT_MP_MAX_PLAYERS && port_mp_get_player(slot, &p))
		return p.name;
	return "Someone";
}

// Removes a collected item the way TItem::taken does, without the touch path (no Mario
// message, no sound, no reward for the local Mario).
void removeItem(TItem* it)
{
	it->kill();
	if (it->checkMapObjFlag(TMapObjBase::MAP_OBJ_FLAG_RESPAWNING)) {
		it->makeObjDefault();
		it->appear();
	}
}

void applyObjective(u32 id, int slot)
{
	std::fprintf(stderr, "[mp] objective %08x from slot %d: applying (stage %d/%d)\n", id, slot, sCurStage, sCurScenario);
	const u32 kind  = id >> 29;
	const int stage = (id >> 21) & 0xFF;
	const int scen  = (id >> 13) & 0xFF;
	const int index = id & 0x1FFF;
	TFlagManager* f = TFlagManager::getInstance();
	if (kind == PORT_MP_OBJ_SHINE) {
		const bool had = f->getShineFlag((u8)index);
		f->setShineFlag((u8)index);
		if (gpItemManager != nullptr && sStaticItems >= 0)
			for (int i = 0; i < gpItemManager->getObjNum(); i++) {
				THitActor* a = gpItemManager->getObj(i);
				if (a != nullptr && a->isActorType(ACTOR_TYPE_SHINE) && ((TShine*)a)->getEventId() == index
				    && !((TShine*)a)->checkLiveFlag(LIVE_FLAG_DEAD))
					((TShine*)a)->kill();
			}
		if (!had && slot != 0xFF) {
			char t[96];
			std::snprintf(t, sizeof(t), "%s got a Shine Sprite!", playerName(slot));
			port_mp_toast(t);
		}
		return;
	}
	if (kind == PORT_MP_OBJ_BLUE) {
		f->setBlueCoinFlag((u8)stage, (u8)index);
		if (sCurStage >= 0 && SMS_getShineStage((u8)sCurStage) == SMS_getShineStage((u8)stage) && gpItemManager != nullptr
		    && sStaticItems >= 0)
			for (int i = 0; i < gpItemManager->getObjNum(); i++) {
				THitActor* a = gpItemManager->getObj(i);
				if (a != nullptr && a->isActorType(ACTOR_TYPE_COIN_BLUE) && ((TCoin*)a)->getEventId() == index
				    && !((TCoin*)a)->checkLiveFlag(LIVE_FLAG_DEAD)) {
					TCoin* c = (TCoin*)a;
					if (c->mContainer != nullptr)
						c->mContainer->receiveMessage(c, HIT_MESSAGE_UNK8);
					removeItem(c);
				}
			}
		return;
	}
	// yellow / red coin of this stage visit
	if (stage != sCurStage || scen != sCurScenario || !sSharedStage || objDone(id))
		return;
	sObjDone.push_back(id);
	TItem* it = itemAt(index);
	if (it == nullptr)
		return;
	const bool red = kind == PORT_MP_OBJ_RED;
	if (red ? !it->isActorType(ACTOR_TYPE_COIN_RED) : !it->isActorType(ACTOR_TYPE_COIN))
		return; // not the same object list (should not happen on the same stage/scenario)
	if (red)
		f->incFlag(MSF_RED_COIN_COUNT, 1);
	else
		f->incGoldCoinFlag(SMS_getShineStage((u8)sCurStage), 1);
	if (it->mContainer != nullptr)
		it->mContainer->receiveMessage(it, HIT_MESSAGE_UNK8);
	if (it->checkLiveFlag(LIVE_FLAG_DEAD))
		sPendingKill.push_back(it); // not out yet: remove it when it appears
	else
		removeItem(it);
	if (!red && f->getFlag(MSF_GOLD_COIN_COUNT) == 100) // as TCoin::taken
		gpItemManager->makeShineAppearWithDemo(k100CoinShine, k100CoinShineCamera, it->mPosition.x, it->mPosition.y,
		                                       it->mPosition.z);
}

void objectivesFrame()
{
	if (sStaticItems >= 0 && !sObjLatched && port_mp_active()) {
		PortMpSessionSettings s;
		port_mp_get_settings(&s);
		sSharedStage = s.sharedObjectives != 0;
		sObjLatched  = true;
		std::fprintf(stderr, "[mp] stage %d/%d: shared objectives %s (%d scene items)\n", sCurStage, sCurScenario,
		             sSharedStage ? "on" : "off", sStaticItems);
	}
	for (size_t i = 0; i < sPendingKill.size();) {
		if (!sPendingKill[i]->checkLiveFlag(LIVE_FLAG_DEAD)) {
			removeItem(sPendingKill[i]);
			sPendingKill.erase(sPendingKill.begin() + i);
		} else {
			i++;
		}
	}
	if (!sObjLatched)
		return; // stage not loaded yet: keep the events queued
	u32 ids[64];
	u8 slots[64];
	int n;
	while ((n = port_mp_objective_poll(ids, slots, 64)) > 0)
		for (int i = 0; i < n; i++)
			applyObjective(ids[i], slots[i]);
}

void prepareFreeSave()
{
	TFlagManager* f = TFlagManager::getInstance();
	f->firstStart();
	// A finished game (w13's story2 preset, Application.cpp): every shine and nozzle box,
	// all plaza events and FMVs seen, Corona visited -> plaza scenario 2, everything open.
	port_apply_flag_items("story2");
	f->saveSuccess(); // a game over restores this state, not a fresh file
	// SMS_PORT_MP_FLAGS=<items> (debug, SMS_PORT_WARP_FLAGS syntax): applied after the saved copy,
	// e.g. 20001=0 (no spare lives) to test a game over in a session.
	if (const char* fl = std::getenv("SMS_PORT_MP_FLAGS"))
		port_apply_flag_items(fl);
	SMSGetApplication()->mSaveFile = 0;
	// From here on TCardManager talks to an empty in-memory card (port_mp_savecard.cpp): unmount
	// the real one first so nothing cached from it is reused.
	if (gpCardManager != nullptr)
		gpCardManager->unmount();
	port_mp_savecard_reset();
	sMpSave        = true;
	sReturnPending = false;
	std::fprintf(stderr, "[mp] free mode: in-memory save ready (shines %d), in-memory card\n",
	             (int)f->getFlag(MSF_SHINE_COUNT));
}

void startFreeModeFromFileSelect()
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	prepareFreeSave();
	int stage, scen;
	port_mp_home_area(&s, &stage, &scen); // the plaza, or a single-level match's level
	std::fprintf(stderr, "[mp] free mode: loading stage %d scenario %d\n", stage, scen);
	gpMarDirector->setNextStage((u16)(((stage + 1) << 8) | scen), nullptr);
	gpCardManager->unmount();
}

} // namespace

extern "C" {

void port_mp_ini_load(char* name, int nameSize, int* port, int* enabled)
{
	loadPrefs();
	std::snprintf(name, nameSize, "%s", sPrefs.name);
	*port    = sPrefs.port;
	*enabled = sPrefs.enabled;
}

void port_mp_ini_save(void)
{
	loadPrefs();
	std::snprintf(sPrefs.name, sizeof(sPrefs.name), "%s", port_mp_get_name());
	FILE* f = std::fopen(iniPath().c_str(), "wb");
	if (f == nullptr)
		return;
	std::fprintf(f,
	             "; Super Mario Sunshine PC port: multiplayer settings\n"
	             "[multiplayer]\n"
	             "enabled = %d\n"
	             "name = %s\n"
	             "port = %d\n"
	             "lobby_name = %s\n"
	             "max_players = %d\n"
	             "shared_objectives = %d\n"
	             "; lobbies you host: chat and emotes allowed\n"
	             "chat = %d\n"
	             "emotes = %d\n"
	             "last_join = %s\n"
	             "last_join_port = %d\n"
	             "; lobby server for joining by lobby name: host[:port] (default port 7700)\n"
	             "server = %s\n"
	             "; server (join by lobby name) or direct (IP address and port)\n"
	             "connection = %s\n"
	             "last_lobby = %s\n",
	             sPrefs.enabled, sPrefs.name, sPrefs.port, sPrefs.lobbyName, sPrefs.maxPlayers,
	             sPrefs.sharedObjectives, sPrefs.chat, sPrefs.emotes, sPrefs.lastJoin, sPrefs.lastJoinPort, sPrefs.server,
	             sPrefs.connType == PORT_MP_CONN_DIRECT ? "direct" : "server", sPrefs.lastLobby);
	std::fclose(f);
}

// Menu access to the preferences (port_mp_menu.cpp).
char* port_mp_pref_lobby_name(void) { loadPrefs(); return sPrefs.lobbyName; }
int* port_mp_pref_max_players(void) { loadPrefs(); return &sPrefs.maxPlayers; }
int* port_mp_pref_shared(void) { loadPrefs(); return &sPrefs.sharedObjectives; }
int* port_mp_pref_chat(void) { loadPrefs(); return &sPrefs.chat; }
int* port_mp_pref_emotes(void) { loadPrefs(); return &sPrefs.emotes; }
int* port_mp_pref_port(void) { loadPrefs(); return &sPrefs.port; }
char* port_mp_pref_last_join(void) { loadPrefs(); return sPrefs.lastJoin; }
int* port_mp_pref_last_join_port(void) { loadPrefs(); return &sPrefs.lastJoinPort; }
char* port_mp_pref_server(void) { loadPrefs(); return sPrefs.server; }
int* port_mp_pref_conn_type(void) { loadPrefs(); return &sPrefs.connType; }
char* port_mp_pref_last_lobby(void) { loadPrefs(); return sPrefs.lastLobby; }
void port_mp_ini_server(char* spec, int size) { loadPrefs(); std::snprintf(spec, size, "%s", sPrefs.server); }

int port_mp_file_select_hint(void) { return sHint; }
void port_mp_menu_closed(void) { sNeedRelease = true; sLeftFrames = 0; }

// Application.cpp, every TApplication state change (game thread).
void port_mp_note_area(int appState, int stage, int scenario)
{
	sCurAppState = appState;
	sHint        = false;
	// a new stage visit: shared-objective bookkeeping starts over
	sStaticItems = -1;
	sObjLatched  = false;
	sSharedStage = false;
	sObjDone.clear();
	sPendingKill.clear();
	if (appState == TApplication::APP_STATE_GAMEPLAY) {
		sCurStage    = stage;
		sCurScenario = scenario;
		port_mp_net_set_local_area(stage, scenario);
	} else {
		sCurStage = sCurScenario = -1;
		port_mp_net_set_local_area(0xFF, 0xFF);
	}
	// Safety net: never "in a lobby" on the title screen / file select (the redirect above
	// should have prevented it). Not APP_STATE_TITLE: that is the episode select a level
	// entrance goes through (TSelectDir), and leaving there dropped the whole lobby whenever
	// the host entered a level.
	if (port_mp_active()
	    && (appState == TApplication::APP_STATE_MENU
	        || (appState == TApplication::APP_STATE_GAMEPLAY && stage == 15 && sMpSave))) {
		port_mp_leave();
		port_mp_toast("Left the lobby: back at the title screen.");
		std::fprintf(stderr, "[mp] reached the title screen in a session: left the lobby\n");
	}
	// Back on the file select (or title) outside a session: the card is the player's again.
	if (sMpSave && !port_mp_active() && (stage == 15 || appState != TApplication::APP_STATE_GAMEPLAY)) {
		if (gpCardManager != nullptr)
			gpCardManager->unmount(); // the in-memory card; the real one mounts fresh next time
		sMpSave        = false;
		sReturnPending = false;
		std::fprintf(stderr, "[mp] multiplayer save released (the real memory card is used again)\n");
	}
}

int port_mp_block_card_writes(void) { return sMpSave; }

// Application.cpp, at every TApplication state change (game thread): a session never lands
// on the title screen / file select. A reset (the GameCube's reset combo) leaves the lobby
// cleanly; the ending (movie 9 -> title), a game over, or anything else heading for the
// title is sent back to the session's Delfino Plaza, keeping the session. Returns 1 when
// it changed the next state / area.
static int redirectTitle(int appState, int reset, int* nextState, unsigned char* stage, unsigned char* scenario)
{
	if (reset) {
		port_mp_leave();
		port_mp_toast("Left the lobby (reset).");
		std::fprintf(stderr, "[mp] reset while in a session: left the lobby\n");
		return 0;
	}
	const int ns       = *nextState;
	// Game mode None (port_mp_modes.cpp): the lobby stays in the plaza; Hide and Seek and Freeze
	// Tag stay in their level or roam all levels (port_mp_arena.cpp). Whatever entrance a player
	// still manages to take (portal, pipe, warp floor, cannon, ...) is let through and sent where
	// it may go here, at the one place every stage change passes (this hook), so no entrance is
	// cut off half-way.
	{
		PortMpSessionSettings ms;
		port_mp_get_settings(&ms);
		if (ms.gameMode != PORT_MP_MODE_FREE && port_mp_arena_route(sCurStage, sCurScenario, nextState, stage, scenario))
			return 1;
	}
	// APP_STATE_TITLE is not the title screen: it sets up TSelectDir, the episode select a
	// level entrance (portal, painting, pipe) goes through. Only the title / file menu
	// (APP_STATE_MENU, TMenuDirector) and the end of the game leave the session's world.
	const bool toTitle = ns == TApplication::APP_STATE_DONE || ns == TApplication::APP_STATE_MENU
	                     || (ns == TApplication::APP_STATE_GAMEPLAY && *stage == 15); // movies (the ending) still play
	if (!toTitle || ns == TApplication::APP_STATE_QUIT)
		return 0;
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	std::fprintf(stderr, "[mp] app state %d -> %d (area %d/%d) would leave for the title: back to the plaza\n", appState,
	             ns, *stage, *scenario);
	int home, homeScen;
	port_mp_home_area(&s, &home, &homeScen); // the plaza, or a single-level match's level
	if (port_mp_arena_exit_lock_left() > 0 && sCurStage >= 0) { // a locked runner stays in his level
		home     = sCurStage;
		homeScen = sCurScenario;
	}
	*nextState = TApplication::APP_STATE_GAMEPLAY;
	*stage     = (u8)home;
	*scenario  = (u8)homeScen;
	if (TFlagManager::getInstance()->getFlag(MSF_LIFE_COUNT) < 1) // after a game over
		TFlagManager::getInstance()->setFlag(MSF_LIFE_COUNT, 3);
	port_mp_toast(home == 1 ? "Back to Delfino Plaza (still in the lobby)." : "Back to the start of the level (still in the lobby).");
	return 1;
}

void port_mp_mode_flags_for_load(int stage); // port_mp_modes.cpp

// Bumped at every stage load (port_mp.h): the sync modules start a new visit on a change, even when
// the new TMarDirector happens to sit at the old one's address (their object lists would be stale).
unsigned gPortMpStageSerial = 0;

int port_mp_redirect_title(int appState, int reset, int* nextState, unsigned char* stage, unsigned char* scenario)
{
	if (!port_mp_active()) {
		port_mp_mode_flags_for_load(*stage); // a session that ended: the real save again
		return 0;
	}
	const int r = redirectTitle(appState, reset, nextState, stage, scenario);
	// game mode None plays the plaza on a locked copy of the save (port_mp_modes.cpp)
	if (*nextState == TApplication::APP_STATE_GAMEPLAY) {
		port_mp_mode_flags_for_load(*stage);
		gPortMpStageSerial++;
	}
	return r;
}
void port_mp_register_game_frame(PortMpFrameFn fn)
{
	if (fn == nullptr)
		return;
	if (sGameFrameFnCount < kMaxGameFrameFns)
		sGameFrameFns[sGameFrameFnCount++] = fn;
	else // (a module would silently never run: raise kMaxGameFrameFns)
		std::fprintf(stderr, "[mp] ERROR: too many game-frame hooks (%d): one is not registered\n", kMaxGameFrameFns);
}

// ItemManager.cpp, TItemManager::loadAfter, before it creates the coin/bottle pools:
// the scene-placed items are [0, count).
void port_mp_items_loaded(int count) { sStaticItems = count; }

// Item.cpp, TCoin/TCoinRed/TCoinBlue::taken (the local Mario or Yoshi's tongue took it).
void port_mp_objective_taken(void* item)
{
	port_mp_cnr_coin_taken(item); // Cops and Robbers: the robbers' loot (port_mp_cnr.cpp)
	if (!sSharedStage || !port_mp_active() || item == nullptr || sCurStage < 0)
		return;
	TCoin* c = (TCoin*)item;
	u32 id   = 0;
	if (c->isActorType(ACTOR_TYPE_COIN_BLUE)) {
		id = PORT_MP_OBJ_ID(PORT_MP_OBJ_BLUE, sCurStage, 0xFF, c->getEventId());
	} else {
		const bool red  = c->isActorType(ACTOR_TYPE_COIN_RED);
		if (!red && !c->isActorType(ACTOR_TYPE_COIN))
			return;
		const int index = itemIndex(c);
		if (index < 0)
			return; // dropped by an enemy / pool coin: local only
		id = PORT_MP_OBJ_ID(red ? PORT_MP_OBJ_RED : PORT_MP_OBJ_COIN, sCurStage, sCurScenario, index);
		if (objDone(id))
			return;
		sObjDone.push_back(id);
	}
	std::fprintf(stderr, "[mp] objective %08x collected here\n", id);
	port_mp_objective_send(id);
}

// MarDirectorDirect.cpp, where the shine-get demo sets the shine's flag.
void port_mp_shine_got(int shineId)
{
	port_mp_manhunt_shine_got(shineId); // Manhunt: the runner's count (port_mp_manhunt.cpp)
	port_mp_shinerun_shine_got(shineId); // Shine Runners (port_mp_shinerun.cpp)
	port_mp_cnr_shine_got(shineId);      // Cops and Robbers: the robbers' loot (port_mp_cnr.cpp)
	if (!sSharedStage || !port_mp_active())
		return;
	port_mp_objective_send(PORT_MP_OBJ_ID(PORT_MP_OBJ_SHINE, 0, 0xFF, shineId));
}

// CardLoad.cpp, TCardLoad::perform (CUE_MOVE), every frame of the save-block screen.
// `marioHasControl`: the save-block screen is up and Mario can walk (no prompt, not the
// title or the options).
extern "C" int port_mp_block_active(void); // port_mp_block.cpp

void port_mp_cardload_perform(int marioHasControl)
{
	sHint = marioHasControl && port_mp_enabled();
	if (!port_mp_enabled())
		return;
	// A session is up (created or joined from the menu): start free mode.
	if (port_mp_active() && !sMpSave) {
		startFreeModeFromFileSelect();
		return;
	}
	// the multiplayer block (port_mp_block.cpp) replaces running off the left edge
	if (!marioHasControl || port_mp_menu_wants_input() || gpMarioOriginal == nullptr || port_mp_block_active()) {
		sLeftFrames = 0;
		return;
	}
	TMario* m          = gpMarioOriginal;
	const float stickX = m->mGamePad != nullptr ? m->mGamePad->mCompSPos[0] : 0.0f;
	if (sNeedRelease) {
		if (stickX > -0.3f)
			sNeedRelease = false;
		return;
	}
	// TCardLoad::perform can see CUE_MOVE more than once per game frame: count presented
	// frames (port_frame_count, 60 Hz) instead of calls.
	static u32 sLastFrame;
	const u32 frame = port_frame_count();
	if (stickX < -0.5f && m->mPosition.x < kFileSelectLeftX) {
		if (frame != sLastFrame)
			sLeftFrames += sLeftFrames == 0 ? 1 : (int)(frame - sLastFrame);
	} else {
		sLeftFrames = 0;
	}
	sLastFrame = frame;
	if (sLeftFrames >= kLeftFramesToOpen) {
		sLeftFrames = 0;
		std::fprintf(stderr, "[mp] Mario ran off the left of the file select: multiplayer menu\n");
		port_mp_menu_open(0);
	}
}

// Application.cpp gameLoop, every frame while a TMarDirector runs (game thread).
// Players whose games differ (game behaviour, 50 / 60 Hz: PortMpCharacter.reserved[4..5], stamped by
// port_mp_net.cpp) play different game code or speed and desync: everyone is told, once per player.
static void versionCheck()
{
	static double sNext;
	static u32 sWarned[PORT_MP_MAX_PLAYERS];
	if (!port_mp_active() || port_mp_time_ms() < sNext)
		return;
	sNext = port_mp_time_ms() + 1000.0;
	PortMpPlayerInfo mine, p;
	const int me = port_mp_local_slot();
	if (me < 0 || !port_mp_get_player(me, &mine))
		return;
	static const char* const kBeh[] = { "?", "Japan", "Europe", "US" };
	for (int i = 0; i < PORT_MP_MAX_PLAYERS; i++) {
		if (i == me || !port_mp_get_player(i, &p) || !p.used || sWarned[i] == p.serial + 1)
			continue;
		const u8 b = p.character.reserved[4], mb = mine.character.reserved[4];
		const u8 v = p.character.reserved[5], mv = mine.character.reserved[5];
		char t[200];
		if (b != 0 && mb != 0 && b != mb && b <= 3 && mb <= 3)
			std::snprintf(t, sizeof(t), "%s plays with %s game behaviour, you with %s: you may desync. Match it in "
			              "Settings > Game (needs a restart).", p.name, kBeh[b], kBeh[mb]);
		else if (v != 0 && mv != 0 && v != mv)
			std::snprintf(t, sizeof(t), "%s runs at %s, you at %s: you may desync. Match PAL video in Settings > Game.",
			              p.name, v == 2 ? "50 Hz" : "60 Hz", mv == 2 ? "50 Hz" : "60 Hz");
		else
			continue;
		sWarned[i] = p.serial + 1;
		port_mp_toast(t);
		std::fprintf(stderr, "[mp] version mismatch: %s\n", t);
	}
}

void port_mp_game_frame(void)
{
	versionCheck();
	if (gpMarDirector == nullptr)
		return;
	// SMS_PORT_MP_LEAVE_AT=<frame> (debug): leave the session at that presented frame.
	static const char* sLeaveAt = std::getenv("SMS_PORT_MP_LEAVE_AT");
	if (sLeaveAt != nullptr && port_mp_active() && port_frame_count() >= (u32)std::atoi(sLeaveAt)) {
		std::fprintf(stderr, "[mp] SMS_PORT_MP_LEAVE_AT: leaving\n");
		sLeaveAt = nullptr;
		port_mp_leave();
	}
	// Left the session (menu, host closed, timeout) while in a multiplayer stage: back to
	// the file select, whose normal loading replaces the multiplayer flags.
	if (sMpSave && !port_mp_active() && !sReturnPending && sCurStage != 15 && sCurStage >= 0) {
		sReturnPending = true;
		std::fprintf(stderr, "[mp] session ended: returning to the file select\n");
		gpMarDirector->setNextStage(15, nullptr);
	}
	if (port_mp_active()) {
		objectivesFrame();
		for (int i = 0; i < sGameFrameFnCount; i++) // registered features (port_mp.h)
			sGameFrameFns[i]();
	}
	// SMS_PORT_MP_TESTMSG=1 (debug, mp_test.py): an app message on PORT_MP_CH_TEST every 2 s
	// (reliable, to everyone), and a log line for each one received.
	static const bool sTestMsg = std::getenv("SMS_PORT_MP_TESTMSG") != nullptr;
	if (sTestMsg && port_mp_active()) {
		static u32 sNext, sCount;
		if (port_frame_count() >= sNext) {
			sNext = port_frame_count() + 120;
			char msg[64];
			int n = std::snprintf(msg, sizeof(msg), "hello %u from %s", ++sCount, port_mp_get_name());
			port_mp_send(PORT_MP_CH_TEST, msg, n, 1, -1);
		}
		char buf[PORT_MP_APP_MAX + 1];
		int from, n;
		while ((n = port_mp_recv(PORT_MP_CH_TEST, buf, PORT_MP_APP_MAX, &from)) != 0) {
			buf[n > 0 ? n : 0] = '\0';
			std::fprintf(stderr, "[mp] testmsg from %d: %s\n", from, buf);
		}
	}
	// SMS_PORT_MP_DEBUG_COLLECT=<frame>[,<frame>...][:coin|blue|red] (debug, shared-objective
	// tests): at each frame, put Mario on the next scene-placed item of that kind that is out,
	// so the game's own touch -> taken path runs.
	static const char* sCollect = std::getenv("SMS_PORT_MP_DEBUG_COLLECT");
	if (sCollect != nullptr && sObjLatched && gpMarioOriginal != nullptr && gpItemManager != nullptr) {
		static unsigned sLastFrame;
		const char* kindStr = std::strchr(sCollect, ':');
		int want            = ACTOR_TYPE_COIN;
		if (kindStr != nullptr && std::strcmp(kindStr + 1, "blue") == 0)
			want = ACTOR_TYPE_COIN_BLUE;
		if (kindStr != nullptr && std::strcmp(kindStr + 1, "red") == 0)
			want = ACTOR_TYPE_COIN_RED;
		const unsigned now = port_frame_count();
		for (const char* p = sCollect; *p != '\0' && *p != ':';) {
			char* e;
			unsigned f = (unsigned)std::strtoul(p, &e, 10);
			if (e == p)
				break;
			if (f > sLastFrame && now >= f) {
				sLastFrame = f;
				for (int i = 0; i < sStaticItems && i < gpItemManager->getObjNum(); i++) {
					TItem* it = (TItem*)gpItemManager->getObj(i);
					if (it != nullptr && it->isActorType(want) && !it->checkLiveFlag(LIVE_FLAG_DEAD)
					    && !objDone(PORT_MP_OBJ_ID(want == ACTOR_TYPE_COIN_RED ? PORT_MP_OBJ_RED : PORT_MP_OBJ_COIN,
					                               sCurStage, sCurScenario, i))) {
						std::fprintf(stderr, "[mp] DEBUG_COLLECT: Mario to item %d (type %x) at %.0f %.0f %.0f\n", i,
						             want, it->mPosition.x, it->mPosition.y, it->mPosition.z);
						gpMarioOriginal->mPosition = it->mPosition;
						break;
					}
				}
			}
			p = *e == ',' ? e + 1 : e;
		}
	}
	// SMS_PORT_MP_TESTSNAP=1 (debug, w10's transport tests before/without w15's capture):
	// a minimal snapshot of the local Mario (position, angles, action, stage).
	static const bool sTestSnap = std::getenv("SMS_PORT_MP_TESTSNAP") != nullptr;
	if (sTestSnap && port_mp_active() && gpMarioOriginal != nullptr && sCurStage >= 0) {
		static u32 sTick;
		TMario* m = gpMarioOriginal;
		PortMpSnapshot s;
		std::memset(&s, 0, sizeof(s));
		s.tick     = ++sTick;
		s.stage    = (u8)sCurStage;
		s.scenario = (u8)sCurScenario;
		s.flags    = PORT_MP_SNAP_VALID;
		s.pos[0] = m->mPosition.x, s.pos[1] = m->mPosition.y, s.pos[2] = m->mPosition.z;
		s.vel[0] = m->mVel.x, s.vel[1] = m->mVel.y, s.vel[2] = m->mVel.z;
		s.forwardVel = m->mForwardVel;
		s.faceAngle[0] = m->mFaceAngle.x, s.faceAngle[1] = m->mFaceAngle.y, s.faceAngle[2] = m->mFaceAngle.z;
		s.action      = m->mStatus;
		s.marioFlags  = m->mFlag;
		s.animId      = m->mAnimationId;
		s.health      = (u8)m->mHealth;
		port_mp_net_test_snapshot(&s);
	}
	// Pause menu: Z opens the multiplayer panel. (The pad's trigger bits read 0 here, so the
	// press is detected from the held buttons.)
	static bool sZHeld = true;
	TMarioGamePad* pad = SMSGetApplication()->mGamePads[0];
	const bool z       = pad != nullptr && (pad->mButton.mButton & PAD_TRIGGER_Z) != 0;
	if (z && !sZHeld && port_mp_active() && gpMarDirector->mState == TMarDirector::STATE_PAUSE_MENU) {
		std::fprintf(stderr, "[mp] pause menu: Z, lobby panel\n");
		port_mp_menu_open(1);
	}
	sZHeld = z;
}

int port_mp_in_pause_menu(void)
{
	return gpMarDirector != nullptr && sCurAppState == TApplication::APP_STATE_GAMEPLAY
	       && gpMarDirector->mState == TMarDirector::STATE_PAUSE_MENU;
}

// Application.cpp, next to SMS_PORT_WARP (after the Nintendo logo).
// SMS_PORT_MP=host[:port] | join:<host>[:port]: host or join, then start free mode right
// away. Returns 1 with the stage to load; 0 to boot normally (unset, or the join failed).
int port_mp_debug_autostart(unsigned char* area, unsigned char* scenario)
{
	const char* e = std::getenv("SMS_PORT_MP");
	if (e == nullptr || *e == '\0')
		return 0;
	if (std::strncmp(e, "hostname:", 9) == 0) { // server mode: lobby name
		PortMpSessionSettings s;
		port_mp_get_settings(&s);
		if (const char* so = std::getenv("SMS_PORT_MP_SHARED"))
			s.sharedObjectives = std::atoi(so) != 0;
		if (const char* ev = std::getenv("SMS_PORT_MP_EVENTS")) // world events (port_mp_events.cpp)
			s.reserved[6] = std::atoi(ev) != 0 ? 0 : 1; // 0 = on (default)
		if (!port_mp_host2(e + 9, 0, &s, PORT_MP_CONN_SERVER, std::getenv("SMS_PORT_MP_PASSWORD"))) {
			std::fprintf(stderr, "[mp] SMS_PORT_MP=hostname failed: %s\n", port_mp_status_text());
			return 0;
		}
	} else if (std::strncmp(e, "joinname:", 9) == 0) {
		port_mp_join_name(e + 9, std::getenv("SMS_PORT_MP_PASSWORD"));
		for (int i = 0; i < 1400 && port_mp_state() == PORT_MP_CONNECTING; i++)
			Sleep(10);
		if (!port_mp_active()) {
			std::fprintf(stderr, "[mp] SMS_PORT_MP=joinname failed: %s\n", port_mp_status_text());
			return 0;
		}
	} else if (std::strncmp(e, "host", 4) == 0) {
		int port = e[4] == ':' ? std::atoi(e + 5) : 0;
		PortMpSessionSettings s;
		port_mp_get_settings(&s);
		if (const char* so = std::getenv("SMS_PORT_MP_SHARED"))
			s.sharedObjectives = std::atoi(so) != 0;
		if (const char* ev = std::getenv("SMS_PORT_MP_EVENTS")) // world events (port_mp_events.cpp)
			s.reserved[6] = std::atoi(ev) != 0 ? 0 : 1; // 0 = on (default)
		if (!port_mp_host(std::getenv("SMS_PORT_MP_LOBBY"), port, &s)) {
			std::fprintf(stderr, "[mp] SMS_PORT_MP=host failed: %s\n", port_mp_status_text());
			return 0;
		}
	} else if (std::strncmp(e, "join:", 5) == 0) {
		char host[128];
		std::snprintf(host, sizeof(host), "%s", e + 5);
		int port    = PORT_MP_DEFAULT_PORT;
		char* colon = std::strrchr(host, ':');
		if (colon != nullptr) {
			*colon = '\0';
			port   = std::atoi(colon + 1);
		}
		port_mp_join(host, port);
		// wait for the handshake (boot time; at most the connect timeout)
		for (int i = 0; i < 800 && port_mp_state() == PORT_MP_CONNECTING; i++)
			Sleep(10);
		if (!port_mp_active()) {
			std::fprintf(stderr, "[mp] SMS_PORT_MP=join failed: %s\n", port_mp_status_text());
			return 0;
		}
	} else {
		std::fprintf(stderr, "[mp] SMS_PORT_MP: unknown value \"%s\" (host[:port] or join:<ip>[:port])\n", e);
		return 0;
	}
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	prepareFreeSave();
	*area     = s.startStage ? s.startStage : 1;
	*scenario = (u8)port_mp_plaza_scenario(&s);
	// SMS_PORT_MP_START_AREA=<area>,<scenario> (debug): start this player somewhere else (e.g.
	// 60,0 with SMS_PORT_DEBUG_CALL=bathdemo@... to test the ending in a session).
	if (const char* sa = std::getenv("SMS_PORT_MP_START_AREA")) {
		int a = 1, c = 0;
		if (std::sscanf(sa, "%d,%d", &a, &c) >= 1) {
			*area     = (unsigned char)a;
			*scenario = (unsigned char)c;
		}
	}
	std::fprintf(stderr, "[mp] SMS_PORT_MP: free mode, stage %d scenario %d\n", *area, *scenario);
	return 1;
}

} // extern "C"

#endif // SMS_PORT_AURORA
