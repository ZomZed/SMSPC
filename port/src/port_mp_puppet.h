/*
 * port_mp_puppet.h - multiplayer remote players ("puppets"), w15.
 *
 * Local snapshot capture (read-only view of the local TMario each logic frame),
 * remote puppet drawing (an extra Mario model per remote player, posed from
 * interpolated snapshots), name tags, and the ghost test harness
 * (SMS_PORT_MP_GHOST / SMS_PORT_MP_GHOST_PLAY). Network side: w10's
 * port/include/port_mp.h (PortMpSnapshot is the 128-byte wire form; the full
 * PortMpPuppetState here is packed into it). See port/MULTIPLAYER.md and BOOTLOG
 * "Multiplayer puppets (w15)".
 *
 * Everything here is PC-only and inactive unless a multiplayer session or a ghost
 * env var is on; single-player never calls into it beyond two cheap checks.
 */
#ifndef PORT_MP_PUPPET_H
#define PORT_MP_PUPPET_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_MP_PUPPET_STATE_VERSION 1
#define PORT_MP_MAX_PUPPETS 16 /* indexed like w10's player slots (PORT_MP_MAX_PLAYERS) */

/* PortMpPuppetState.flags */
enum {
	PMS_VISIBLE   = 1u << 0,  /* the Mario is drawn this frame */
	PMS_HAS_FLUDD = 1u << 1,  /* MARIO_FLAG_HAS_FLUDD: FLUDD on the back */
	PMS_SPRAYING  = 1u << 2,  /* TWaterGun::isEmitting() (with FLUDD) */
	PMS_ON_YOSHI  = 1u << 3,  /* riding Yoshi */
	PMS_CAP       = 1u << 4,  /* wearing the cap (TMarioCap hat model) */
	PMS_HELMET    = 1u << 5,  /* diver helmet */
	PMS_GLASSES   = 1u << 6,  /* sunglasses */
	PMS_SHIRT     = 1u << 7,  /* body shape 10 hidden (MARIO_FLAG_HAS_SHIRT) */
	PMS_HAND4     = 1u << 8,  /* the 4th right hand (shine demo) */
	PMS_IN_DEMO   = 1u << 9,  /* the director is in a demo / talk / pause state */
	PMS_HOLDING   = 1u << 10, /* carrying something */
	PMS_IN_WATER  = 1u << 11, /* MARIO_FLAG_IN_ANY_WATER */
	PMS_FX_OFF    = 1u << 12, /* joint springs off (TMario::addCallBack: first 0x4B0 move ticks of
	                             the stage, or pumping): arms, cap and FLUDD follow stiffly */
};

/*
 * One logic frame of a Mario, everything a puppet needs to look the same.
 * The animation part mirrors TMario's M3UModelMario exactly (slot 0 = whole
 * body, slot 1 = the upper-body pump/hold animation on jnt_chest), so a
 * puppet built from the same mario.bmd and BCK table reproduces the pose.
 * Ghost files store this struct; the network sends it packed into w10's
 * PortMpSnapshot (port_mp_pack_wire / port_mp_unpack_wire).
 */
typedef struct PortMpPuppetState {
	uint16_t version;        /* PORT_MP_PUPPET_STATE_VERSION */
	uint16_t size;           /* sizeof(PortMpPuppetState) */
	uint32_t seq;            /* sender's logic-frame counter (monotonic) */
	uint8_t stage;           /* TMarDirector::mMap (area) */
	uint8_t scenario;        /* TMarDirector::unk7D (episode) */
	uint8_t stageSeq;        /* +1 on every stage load: puppets snap, never blend across */
	uint8_t warpSeq;         /* +1 on teleports (warps, respawns, big jumps): snap */
	uint32_t flags;          /* PMS_* */
	uint32_t status;         /* TMario::mStatus */
	uint32_t statusArg;      /* TMario::mStatusArg */
	uint32_t marioFlags;     /* TMario::mFlag */
	float pos[3];            /* TMario::mPosition */
	float vel[3];            /* TMario::mVel */
	float fwdVel;            /* TMario::mForwardVel */
	int16_t faceAngle[3];    /* TMario::mFaceAngle */
	int16_t modelFaceAngle;  /* TMario::mModelFaceAngle */
	float baseMtx[3][4];     /* body base TR matrix (calcBaseMtx result, world space) */
	/* body animation */
	uint16_t animId;         /* TMario::mAnimationId (gMarioAnimeData index) */
	uint16_t bck[2][2];      /* [slot][new, old] BCK index into ma_*.bck (marioAnimeFiles) */
	uint8_t calc[2];         /* [slot] blend-calc index, 0xFF = slot off */
	uint8_t hand;            /* hand models: 0 body fists, 1 hnd2, 2 hnd3 */
	uint8_t btp;             /* eye texture pattern (ma_*_tx.btp index), 0xFF = none */
	float frame[3];          /* frame ctrl 0 (body), 1 (upper), 2 (eyes) */
	float rate[3];
	float blend[2];          /* [calc] motion blend ratio (run/walk mix) */
	int16_t headRot[4];      /* extra head rotation MarioHeadCtrl applied: RPH(a,b,c) * RPH(0,0,d) */
	int16_t waistRot[3];     /* extra waist rotation MarioWaistCtrl applied: RPH(a,b,c) */
	int16_t health;          /* TMario::mHealth */
	/* FLUDD */
	uint8_t nozzle;          /* TWaterGun::mCurrentNozzle (TNozzleType) */
	uint8_t secondNozzle;    /* TWaterGun::mSecondNozzle */
	uint8_t pressure;        /* TWaterGun::mCurrentPressure */
	uint8_t pad0;
	int16_t fluddBck;        /* FLUDD body MActor BCK index, -1 = none */
	int16_t nozzleBck;       /* current nozzle MActor BCK index, -1 = none */
	float fluddFrame;
	float nozzleFrame;
	int32_t water;           /* TWaterGun::mCurrentWater */
	float emitPos[3];        /* first emitter position (TWaterGun::mEmitPos[0]) */
	float emitDir[3];        /* first emitter direction (nozzle emit matrix column 0) */
	/* Yoshi (drawn while mounted; its base matrix is Mario's, TYoshi::calcAnim) */
	uint8_t yoshiState;      /* TYoshi::mState (8 = mounted) */
	uint8_t yoshiColor;      /* TYoshi::mType (TYoshi::Color) */
	int16_t yoshiBck;        /* Yoshi MActor BCK index, -1 = none */
	float yoshiFrame;
	/* held object */
	uint32_t heldActorType;  /* getActorType() of TMario::mHeldObject, 0 = none */
} PortMpPuppetState;

struct PortMpSnapshot; /* port_mp.h (w10): the wire form */

/* ---- capture (local player) -------------------------------------------- */

/* Fills *out from the local Mario (gpMarioOriginal). Read-only: no game state
 * is written. Returns 0 (and leaves *out untouched) outside gameplay. */
int port_mp_capture(PortMpPuppetState* out);

/* The last state captured by the per-frame hook (port_mp_logic_frame).
 * Returns 0 if none this stage. */
int port_mp_last_local_state(PortMpPuppetState* out);

/* PortMpPuppetState <-> w10's 128-byte PortMpSnapshot (standard fields plus the
 * w15 block in `extra`, `animAux`, `nozzleAngle`, `pad0`; layout in
 * MULTIPLAYER.md "Puppet block"). unpack returns 0 for an invalid snapshot. */
void port_mp_pack_wire(const PortMpPuppetState* in, struct PortMpSnapshot* out);
int port_mp_unpack_wire(const struct PortMpSnapshot* in, PortMpPuppetState* out);

/* ---- remote puppets ------------------------------------------------------ */

/* Feed a state for puppet `slot` (0..PORT_MP_MAX_PUPPETS-1), stamped with tMs on
 * the caller's monotonic millisecond clock (the same clock for every call to
 * that slot). States are buffered ~100 ms and interpolated; out-of-order or
 * duplicate ones (by seq) are dropped. Activates the slot. While a w10 session
 * runs, the puppet code pulls w10's snapshots itself; this is for tests/tools. */
void port_mp_puppet_push(int slot, const PortMpPuppetState* st, double tMs);
void port_mp_puppet_set_name(int slot, const char* name);
/* Name tag colour (0xRRGGBB). */
void port_mp_puppet_set_color(int slot, uint32_t rgb);
void port_mp_puppet_remove(int slot);
void port_mp_puppet_remove_all(void);
int port_mp_puppet_count(void); /* active slots */

/* Turns the puppet view object on for the next stage loads even before any slot
 * is active (a running w10 session does this implicitly). */
void port_mp_puppets_enable(int on);

/* ---- hooks (shared game code, SMS_PORT only) ---------------------------- */

/* TMarDirector::direct, last simulation tick, before the entry list: capture,
 * send, receive, ghost record/playback, puppet poses. */
void port_mp_logic_frame(void);
/* TMarDirector::preEntry: returns the puppet view object to push after
 * "コンダクター" (CUE_CALC_VIEW | CUE_ENTRY), or null when puppets are off. */
void* port_mp_preentry_viewobj(void);

/* ---- remote water (port_mp_water.cpp) --------------------------------------- */
/* All game thread, from port_mp_logic_frame. The local Mario's emits since the last call
 * go out with snapshot `seq` (send) and/or into the ghost recording's .wtr (record). */
void port_mp_water_local_frame(int captured, uint32_t seq, const float pos[3], int send, int record);
void port_mp_water_receive(void);
void port_mp_water_ghost_feed(uint32_t srcSeq, uint32_t seq, int slot);
/* Plays slot's queued water up to its playback tick at the puppet's position. */
void port_mp_water_replay(int slot, int havePlay, uint32_t playSeq, const float pos[3], int show);
void port_mp_water_clear(int slot);

/* ---- colour palettes (port_mp_palette.cpp) -------------------------------- */

/* A palette is w10's PortMpCharacter: colors[0..6] = the slots below (colors[6] =
 * hair). (0,0,0) = keep the original colour of that part; all zero = classic Mario,
 * untouched. */
enum {
	PORT_MP_PAL_CAP,
	PORT_MP_PAL_SHIRT,
	PORT_MP_PAL_OVERALLS,
	PORT_MP_PAL_GLOVES,
	PORT_MP_PAL_SHOES,
	PORT_MP_PAL_SKIN,
	PORT_MP_PAL_HAIR,
	PORT_MP_PAL_SLOTS
};
struct PortMpCharacter;
int port_mp_palette_preset_count(void);
const char* port_mp_palette_preset_name(int i);
void port_mp_palette_preset(int i, struct PortMpCharacter* out);
int port_mp_palette_is_default(const struct PortMpCharacter* c);
/* The local player's palette (single-player and multiplayer; in a session it is also
 * published with port_mp_set_character). Applied to the local Mario on the next logic
 * frame; null = classic. SMS_PORT_PALETTE=<preset name prefix or index> sets it at
 * start; the character menu (port_mp_custom.cpp) loads/saves it in port_characters.ini. */
void port_mp_palette_set_local(const struct PortMpCharacter* c);
void port_mp_palette_get_local(struct PortMpCharacter* out);
/* Live preview on the local Mario while the character menu edits; null = stop. */
void port_mp_palette_preview(const struct PortMpCharacter* c);
/* port_mp_logic_frame: keeps the local Mario's atlas in step with the above. */
void port_mp_palette_local_frame(void);

/* ---- character menu (port_mp_custom.cpp, ImGui) ---------------------------- */
/* Editor contents for w10's "Character" page: call inside its window; returns 1 when
 * the player pressed Back/Close. */
int port_mp_custom_draw(void);
/* Standalone window (F4 anywhere, or the lobby panel's "Character" section); drawn
 * from port_mp_overlay_draw every presented frame. */
void port_mp_custom_open(int open);
int port_mp_custom_is_open(void);
void port_mp_custom_frame(void);

/* ---- name tags (port_mp_overlay.cpp, ImGui) ------------------------------ */

typedef struct PortMpNameTag {
	float x, y;     /* 0..1 in the game picture (0,0 = top left) */
	float alpha;    /* 0..1 (distance fade) */
	uint32_t rgb;
	char name[32];
	int slot;       /* player slot (w10's), or the puppet slot for ghosts */
	int local;      /* 1 = the local Mario (no name tag drawn; anchors his speech bubble) */
} PortMpNameTag;

/* Where a player's name tag is this frame, in window pixels (the point just above the
 * head), for overlays such as chat bubbles. Returns 1 and sets *visible = 1 when the
 * player is drawn on screen; 0 when unknown. Includes the local Mario in a session. */
int port_mp_overlay_get_tag(int slot, float* x, float* y, int* visible);

/* Called by port_window.cpp after port_menu_draw (ImGui frame open). */
void port_mp_overlay_draw(void);
/* Game thread -> overlay: the tags of the last drawn game frame. */
void port_mp_overlay_set_tags(const PortMpNameTag* tags, int count);

#ifdef __cplusplus
}

class J3DModelData;
/* port_mp_palette.cpp, for the puppet builder: own texture headers, recoloured atlas. */
void portMpPaletteOwnHeaders(J3DModelData* d);
void* portMpPaletteApplyModel(J3DModelData* d, const struct PortMpCharacter* c); /* own atlas */
void portMpPaletteRecolorInto(void* atlas, const struct PortMpCharacter* c);     /* in place */
uint32_t portMpPaletteHash(const struct PortMpCharacter* c); /* 0 = classic */
#endif

#endif
