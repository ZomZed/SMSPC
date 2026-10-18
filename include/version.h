#ifndef VERSION_H
#define VERSION_H

#ifdef VERSION_GMSJ01
#define GMSJ01(value) value
#else
#define GMSJ01(value)
#endif

#ifdef VERSION_GMSP01
#define GMSP01(value) value
#else
#define GMSP01(value)
#endif

#ifdef VERSION_GMSE01
#define GMSE01(value) value
#else
#define GMSE01(value)
#endif

// PAL-only values: the US build is compiled as part of the PAL family
// (VERSION_GMSP01 is defined for it too), so use this for PAL values the US
// release doesn't share.
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
#define GMSP01_ONLY(value) value
#else
#define GMSP01_ONLY(value)
#endif

// The common 2D resource folder: /common/2d/ on the JP and US discs; the PAL disc
// has one cmn2d_<language>.arc per language, mounted as /cmn2d/.
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
#define SMS_COMMON_2D_DIR "/cmn2d/"
#else
#define SMS_COMMON_2D_DIR "/common/2d/"
#endif

// Game-code behaviour (BOOTLOG "Regional behaviour (w13)"). The PC port builds one exe per
// disc region and selects the version's pure code differences at run time; such sites keep
// their GameCube #if text and add a `#ifdef SMS_PORT` branch testing SMS_BEHAVIOUR_IS(US),
// SMS_BEHAVIOUR_IS(JP) or SMS_BEHAVIOUR_IS(EU) (EU: the PAL release). In the GameCube build
// SMS_BEHAVIOUR_IS is the build's own region at compile time.
#define SMS_REGION_JP 0
#define SMS_REGION_EU 1
#define SMS_REGION_US 2
#ifdef SMS_PORT
#ifdef __cplusplus
extern "C" int SMSGetBehaviour(void); // port/src/port_behaviour.cpp
#else
int SMSGetBehaviour(void);
#endif
#define SMS_BEHAVIOUR_IS(region) (SMSGetBehaviour() == SMS_REGION_##region)
#elif defined(VERSION_GMSE01)
#define SMS_BEHAVIOUR_IS(region) (SMS_REGION_US == SMS_REGION_##region)
#elif defined(VERSION_GMSP01)
#define SMS_BEHAVIOUR_IS(region) (SMS_REGION_EU == SMS_REGION_##region)
#else
#define SMS_BEHAVIOUR_IS(region) (SMS_REGION_JP == SMS_REGION_##region)
#endif
// US or PAL ("PAL family": the code `#ifdef VERSION_GMSP01` selects at compile time).
#define SMS_BEHAVIOUR_PAL_FAMILY() (!SMS_BEHAVIOUR_IS(JP))

#define VERSION_SELECT_JOIN(a, b, c, d, e, f, g, h, ...) a b c d e f g h

#define VERSION_SELECT(...) (VERSION_SELECT_JOIN(__VA_ARGS__, , , , , , , ))

#endif
