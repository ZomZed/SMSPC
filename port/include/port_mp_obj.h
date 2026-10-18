/*
 * port_mp_obj.h - multiplayer enemy / object sync hooks for game code (w11, task board P8;
 * port/src/port_mp_objects.cpp, design in port/MULTIPLAYER.md "Enemy and object sync").
 * Aurora (PC) build only; every hook is one load + branch while no actor is driven by
 * another player (gPortMpObjPuppets == 0), i.e. always in single player.
 */
#ifndef PORT_MP_OBJ_H
#define PORT_MP_OBJ_H

#ifdef SMS_PORT_AURORA

#ifdef __cplusplus
extern "C" {
#endif

/* Number of actors on this client that another player runs (puppets, bosses following the
 * stage leader). */
extern int gPortMpObjPuppets;

/* Nonzero while map-object reactions are forwarded (port_mp_mapreact.cpp: in a session with
 * other players in the stage). */
extern int gPortMpMapReact;

/* A hit from the local Mario (stomp, hip drop, punch) or his water is about to reach
 * `target`: if another player runs it, the hit is also sent to that player. `senderPos` is
 * the attacker's position (Mario, or the water particle). */
void port_mp_obj_hit(void* target, const float* senderPos, unsigned message, int water);

/* Top of a boss's own damage function: returns 1 when another player runs this boss; the
 * damage was sent there and must not be applied here. */
int port_mp_obj_boss_damage(void* boss, int kind);

#ifdef __cplusplus
}
#endif

#define PORT_MP_OBJ_HIT(target, senderPos, message, water)                                      \
	do {                                                                                        \
		if (gPortMpObjPuppets || gPortMpMapReact)                                                                \
			port_mp_obj_hit((target), (senderPos), (message), (water));                         \
	} while (0)
#define PORT_MP_OBJ_BOSS_DAMAGE(boss, kind) (gPortMpObjPuppets && port_mp_obj_boss_damage((boss), (kind)))

/* Boss damage kinds (port_mp_obj_boss_damage). */
enum {
	PORT_MP_BOSS_GESSO_TENTACLE = 1,
	PORT_MP_BOSS_GESSO_EYE      = 2,
	PORT_MP_BOSS_GESSO_BEAK     = 3,
	PORT_MP_BOSS_PAKKUN_HIPDROP = 4,
	PORT_MP_BOSS_PAKKUN_WATER   = 5,
	PORT_MP_BOSS_PAKKUN_FLYING  = 6,
};

#endif /* SMS_PORT_AURORA */

#endif /* PORT_MP_OBJ_H */
