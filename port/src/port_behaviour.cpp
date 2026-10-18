// Game-code behaviour selection for the region builds (w13, BOOTLOG "Regional behaviour (w13)").
//
// Every exe is compiled for one disc region (sms_port.exe US, sms_port_jp.exe JP,
// sms_port_eu.exe EU), so everything that depends on the disc's files is right for the disc.
// The pure game-code differences between the releases (class B sites in the BOOTLOG table)
// are chosen at run time instead: game code tests SMS_BEHAVIOUR_IS(US|JP|EU)
// (include/version.h), which calls SMSGetBehaviour() here in the port. The GameCube build
// keeps its compile-time #if blocks.
//
// Choice: %SMS_PORT_BEHAVIOUR% (disc, us, jp/japan, eu/europe; tests and replays), else
// port_settings.ini [game] behaviour (F1 menu, Game tab), else the disc's own region. It is
// read once, so a change applies at the next start; the menu offers a restart
// (port_restart_request), which relaunches the exe with the same arguments.
//
// The European build's SRAM settings (sdk_os_misc.cpp OSGetEuRgb60Mode / OSGetLanguage) come
// from [game] pal_video and pal_language here too.
#include "../../include/version.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef SMS_PORT_AURORA
#include "port_disc.h"
#include "port_settings.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

int buildRegion()
{
#if defined(VERSION_GMSE01)
	return SMS_REGION_US;
#elif defined(VERSION_GMSJ01)
	return SMS_REGION_JP;
#else
	return SMS_REGION_EU;
#endif
}

const char* regionName(int r) { return r == SMS_REGION_US ? "US" : r == SMS_REGION_JP ? "Japan" : "Europe"; }

int sBehaviour = -1;

// PortSettings::behaviour: 0 disc, 1 US, 2 Japan, 3 Europe.
int regionFor(int setting)
{
	return setting == 1 ? SMS_REGION_US : setting == 2 ? SMS_REGION_JP : setting == 3 ? SMS_REGION_EU : buildRegion();
}

int envSetting(const char* e)
{
	if (_stricmp(e, "us") == 0)
		return 1;
	if (_stricmp(e, "jp") == 0 || _stricmp(e, "japan") == 0)
		return 2;
	if (_stricmp(e, "eu") == 0 || _stricmp(e, "europe") == 0 || _stricmp(e, "pal") == 0)
		return 3;
	return 0;
}

// pal_video / pal_language as this run uses them (read once, like the GameCube's SRAM).
int sPalVideo    = -1;
int sPalLanguage = -1;

} // namespace

extern "C" int SMSGetBehaviour(void)
{
	if (sBehaviour >= 0)
		return sBehaviour;
	const char* from = "disc";
	int setting      = 0;
#ifdef SMS_PORT_AURORA
	setting = gPortSettings.behaviour;
	from    = "settings";
#endif
	if (const char* e = std::getenv("SMS_PORT_BEHAVIOUR")) {
		from    = "SMS_PORT_BEHAVIOUR";
		setting = envSetting(e);
	}
	sBehaviour = regionFor(setting);
	std::fprintf(stderr, "[port] game behaviour: %s (%s; disc build %s)\n", regionName(sBehaviour),
	             setting == 0 ? "match disc" : from, regionName(buildRegion()));
	return sBehaviour;
}

int port_behaviour_for_setting(int setting)
{
	if (const char* e = std::getenv("SMS_PORT_BEHAVIOUR")) // the variable overrides the setting
		return regionFor(envSetting(e));
	return regionFor(setting);
}

// European build: [game] pal_video (0 50 Hz, 1 60 Hz, 2 ask at boot; SMS_PORT_PAL60=0/1/2
// overrides it) and pal_language (0-4; SMS_PORT_PAL_LANGUAGE overrides it).
extern "C" int port_settings_pal60(void)
{
	if (sPalVideo < 0) {
#ifdef SMS_PORT_AURORA
		sPalVideo = gPortSettings.pal60;
#else
		sPalVideo = 1;
#endif
		if (const char* e = std::getenv("SMS_PORT_PAL60"))
			sPalVideo = std::atoi(e);
		if (sPalVideo < 0 || sPalVideo > 2)
			sPalVideo = 1;
	}
	return sPalVideo;
}

extern "C" int port_settings_pal_language(void)
{
	if (sPalLanguage < 0) {
#ifdef SMS_PORT_AURORA
		sPalLanguage = gPortSettings.palLanguage;
#else
		sPalLanguage = 0;
#endif
		if (const char* e = std::getenv("SMS_PORT_PAL_LANGUAGE"))
			sPalLanguage = std::atoi(e);
		if (sPalLanguage < 0 || sPalLanguage > 4)
			sPalLanguage = 0;
	}
	return sPalLanguage;
}

#ifdef SMS_PORT_AURORA
// ---- TV format (European build) ---------------------------------------------------------
// The game's VIGetTvFormat calls come here (build_aurora.py renames them; Aurora's own
// returns NTSC). US and Japanese builds: always NTSC. European build: like a PAL GameCube,
// PAL (50 Hz; the Nintendo logo always is) until the game configures an EURGB60 render
// mode, which it does after the logo when 60 Hz is set (pal_video 60, or "Yes" to the boot
// question with pal_video ask; GCLogoDir.cpp). The game derives its timing from the format
// (SMSGetVSyncTimesPerSec), and the port's retrace interrupt follows it
// (port_vi_set_retrace_ns, port_main.cpp): 50 or 59.94 fields per second.
extern "C" void port_vi_set_retrace_ns(long long period_ns); // port_main.cpp

namespace {
enum { kViNtsc = 0, kViPal = 1, kViEurgb60 = 5 };
int sTvFormat = -1;

void setTvFormat(int format)
{
	if (format == sTvFormat)
		return;
	sTvFormat = format;
	port_vi_set_retrace_ns(format == kViPal ? 20000000ll : 16683350ll);
	std::fprintf(stderr, "[port] video: %s\n", format == kViPal ? "PAL 50 Hz" : "PAL 60 Hz (EURGB60)");
}
} // namespace

extern "C" unsigned long port_VIGetTvFormat(void)
{
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	if (sTvFormat < 0)
		setTvFormat(kViPal);
	return (unsigned long)sTvFormat;
#else
	return kViNtsc;
#endif
}

// port_VIConfigure (port_aurora_sdk.cpp): the render mode's TV format.
extern "C" void port_vi_note_tv_mode(unsigned long viTVmode)
{
#if defined(VERSION_GMSP01) && !defined(VERSION_GMSE01)
	const int format = (int)(viTVmode >> 2);
	if (format == kViPal || format == kViEurgb60)
		setTvFormat(format);
#else
	(void)viTVmode;
#endif
}

bool port_pal_settings_changed(void)
{
	if (std::getenv("SMS_PORT_PAL60") == nullptr && gPortSettings.pal60 != port_settings_pal60())
		return true;
	return std::getenv("SMS_PORT_PAL_LANGUAGE") == nullptr && gPortSettings.palLanguage != port_settings_pal_language();
}

// ---- restart (settings menu) ----------------------------------------------------------
namespace {
volatile bool sRestart;
}

void port_restart_request(void) { sRestart = true; }

// SMS_PORT_RESTART_AT=<frame>[:<behaviour>] (test knob): at that retrace, set the Game
// behaviour setting (disc, us, japan, europe) and restart, as the menu does. The restarted
// game has SMS_PORT_RESTARTED=1 and ignores the knob.
extern "C" volatile unsigned gPortFrameCount; // port_crash.cpp
bool port_restart_pending(void)
{
	static const char* sAt = std::getenv("SMS_PORT_RESTARTED") == nullptr ? std::getenv("SMS_PORT_RESTART_AT") : nullptr;
	if (sAt != nullptr && !sRestart && gPortFrameCount >= (unsigned)std::atoi(sAt)) {
		if (const char* b = std::strchr(sAt, ':'))
			gPortSettings.behaviour = envSetting(b + 1);
		sRestart = true;
	}
	return sRestart;
}

// Called by port_main.cpp's pumpEvents, outside an Aurora frame. Shuts the game down like a
// window close, then starts this exe again with the same command line and environment (the
// disc chosen in a file dialog is saved in the settings; a handed-over disc is in
// %SMS_PORT_DISC%). A hand-off child exits with PORT_EXIT_RESTART instead and its parent
// starts it again (port_disc.cpp handOff), so the parent keeps waiting for the game.
void port_restart_now(void)
{
	std::fprintf(stderr, "[port] restarting (settings menu)\n");
	port_settings_save();
	std::fflush(stderr);
	std::fflush(stdout);
	port_padfx_shutdown();
	aurora_shutdown();
	SetEnvironmentVariableA("SMS_PORT_RESTARTED", "1");
	if (std::getenv("SMS_PORT_HANDOFF") != nullptr)
		TerminateProcess(GetCurrentProcess(), PORT_EXIT_RESTART);
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	STARTUPINFOW si = {};
	si.cb           = sizeof(si);
	si.dwFlags      = STARTF_USESTDHANDLES;
	si.hStdInput    = GetStdHandle(STD_INPUT_HANDLE);
	si.hStdOutput   = GetStdHandle(STD_OUTPUT_HANDLE);
	si.hStdError    = GetStdHandle(STD_ERROR_HANDLE);
	PROCESS_INFORMATION pi = {};
	const wchar_t* cl = GetCommandLineW();
	size_t n          = wcslen(cl);
	wchar_t* cmd      = (wchar_t*)std::malloc((n + 1) * sizeof(wchar_t)); // CreateProcessW may write to it
	std::memcpy(cmd, cl, (n + 1) * sizeof(wchar_t));
	if (CreateProcessW(exe, cmd, nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		std::fprintf(stderr, "[port] restart: could not start the game again (error %lu)\n", GetLastError());
	}
	// No static destructors: game threads (audio, loaders) are still running.
	TerminateProcess(GetCurrentProcess(), 0);
}
#endif
