// Multiplayer: local-only stops don't stop the world (like sm64coopdx). On its own the game stops
// every actor while its one player looks at the pause menu, the guide map (Z) or the save
// prompt (TMarDirector::direct leaves CUE_MOVE / CUE_CALC_ANIM out of the movement pass), and
// stops enemies and map objects while Mario talks to an NPC (their perform() checks
// isTalkModeNow()). In a session the other players keep playing in the same stage, so that
// froze the world for one instance only and desynced enemies, NPCs and objects.
//
// In a session (port_mp_active()):
//   - pause menu, guide map, save prompt: the movement and animation passes keep running; only
//     the local Mario (with his FLUDD, cap and Yoshi, which his perform() drives) stands still,
//     as in the original pause (TMario::perform skips CUE_MOVE). Remote puppets and the sync
//     modules run from port_mp_logic_frame as before.
//   - NPC talk: enemies, enemy attachments, map objects and items keep moving. Mario is
//     invincible while talking (TMario::isInvincible checks the talk mode), so nothing new can
//     hurt him.
// Single player (no session): every check here is false, the original behaviour.
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#include <System/MarDirector.hpp>
#include <dolphin/os.h>

// The world keeps running through local-only stops (NPC talk).
extern "C" int port_mp_world_runs(void)
{
	return port_mp_active() ? 1 : 0;
}

// The movement pass runs behind this director state (pause menu, guide map, save prompt).
extern "C" int port_mp_world_runs_behind(int state)
{
	if (!port_mp_active())
		return 0;
	return state == TMarDirector::STATE_PAUSE_MENU || state == TMarDirector::STATE_GUIDE
	       || state == TMarDirector::STATE_CARD_SAVE;
}

// The local Mario stands still while the world runs behind a menu.
extern "C" int port_mp_local_mario_frozen(void)
{
	const bool frozen = gpMarDirector != nullptr && port_mp_world_runs_behind(gpMarDirector->mState);
	static bool sWas;
	if (frozen != sWas) {
		sWas = frozen;
		if (frozen)
			OSReport("[mp] menu open (director state %d): the world keeps running, Mario waits\n",
			         (int)gpMarDirector->mState);
		else
			OSReport("[mp] menu closed\n");
	}
	return frozen ? 1 : 0;
}

#endif // SMS_PORT_AURORA
