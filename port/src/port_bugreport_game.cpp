// Bug reports (port_bugreport.cpp, w12): the game-state lines of log.txt. Separate file so the
// game headers stay away from the Media Foundation ones.
#ifdef SMS_PORT_AURORA

#include "port_bugreport.h"

#include <Player/Mario.hpp>
#include <System/Application.hpp>
#include <System/MarDirector.hpp>

#include <cstdio>

extern "C" int port_mp_state(void);                // port_mp_net.cpp (w10)
extern "C" const char* port_mp_status_text(void);  // port_mp_net.cpp (w10)

namespace {

const char* stageName(int stage)
{
	switch (stage) {
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
	case 14: return "Hotel Delfino casino";
	case 15: return "File select";
	case 16: return "Noki Bay (undersea)";
	case 52: return "Corona Mountain";
	case 55: return "Petey Piranha boss";
	case 56: return "delfinoBoss";
	case 57: return "mareBoss";
	case 58: return "Mecha-Bowser";
	case 59: return "ricco8 boss";
	case 60: return "Bowser (coronaBoss)";
	default: return stage >= 20 && stage < 53 ? "secret course" : "?";
	}
}

} // namespace

extern "C" void port_bugreport_game_state(char* out, size_t cap)
{
	size_t n = 0;
	auto add = [&](const char* fmt, auto... args) {
		if (n < cap)
			n += (size_t)std::snprintf(out + n, cap - n, fmt, args...);
	};
	TApplication* app = SMSGetApplication();
	if (app == nullptr) {
		add("game: no application\n");
		return;
	}
	const int stage = app->mCurrArea.getStage(), scenario = app->mCurrArea.getScenario();
	add("stage: %d (%s), scenario %d, app state %d\n", stage, stageName(stage), scenario, (int)app->mAppState);
	TMarDirector* dir = gpMarDirector;
	const bool inStage = dir != nullptr && app->mDirector == (JDrama::TDirector*)dir;
	if (inStage)
		add("director: state %d, map %d, scenario %d\n", (int)dir->mState, (int)dir->mMap, (int)dir->unk7D);
	TMario* m = gpMarioOriginal;
	if (inStage && m != nullptr)
		add("mario: pos %.1f %.1f %.1f, action 0x%08x, health %d\n", m->mPosition.x, m->mPosition.y, m->mPosition.z,
		    (unsigned)m->mStatus, (int)m->getHealth());
	else
		add("mario: not in a stage\n");
	add("multiplayer: state %d, %s\n", port_mp_state(), port_mp_status_text());
}

#endif // SMS_PORT_AURORA
