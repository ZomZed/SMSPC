// Graphics backend choice and start-up fallback (w6).
//
// [graphics] backend = d3d12 (default) | vulkan | d3d11, or the F1 Display tab. Takes effect
// at the next start. Aurora's prebuilt Dawn has D3D12, Vulkan, D3D11 (and Null) compiled in.
//
// Two fallbacks:
// - Aurora's own: when the chosen API cannot create an adapter/device, aurora_initialize tries
//   D3D12, Vulkan, D3D11 in turn (then Null). The menu then names the API actually running.
// - A start guard for crashes: right before aurora_initialize a small file
//   (<settings file>.gpustart) records the API being started and the process id; it is removed
//   once 120 frames have been presented, or at a normal exit. If the next start finds it still
//   naming the configured API and that process is gone, the API crashed or hung while
//   starting, so the setting is switched
//   (D3D12 -> Vulkan, others -> D3D12) and saved, and the menu says why.
// SMS_PORT_BACKEND=d3d12|vulkan|d3d11 (test knob) overrides the setting for one run, without
// the guard. SMS_PORT_NULLGFX keeps its Null backend (port_main.cpp sets it after this).
#ifdef SMS_PORT_AURORA

#include "port_settings.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

const char* const kIni[PORT_BACKEND_COUNT]   = { "d3d12", "vulkan", "d3d11" };
const char* const kNames[PORT_BACKEND_COUNT] = { "Direct3D 12", "Vulkan", "Direct3D 11" };

std::string sGuard;      // guard file path, empty when the guard is not used this run
bool sArmed;             // guard file written for this start
int sRequested = -1;     // PortBackend asked of Aurora this run
int sCrashed   = -1;     // PortBackend that did not get through its last start (switched away)
unsigned sPresented;
char sStatus[160];

AuroraBackend toAurora(int b)
{
	switch (b) {
	case PORT_BACKEND_VULKAN: return BACKEND_VULKAN;
	case PORT_BACKEND_D3D11: return BACKEND_D3D11;
	default: return BACKEND_D3D12;
	}
}

int fromAurora(AuroraBackend b)
{
	switch (b) {
	case BACKEND_D3D12: return PORT_BACKEND_D3D12;
	case BACKEND_VULKAN: return PORT_BACKEND_VULKAN;
	case BACKEND_D3D11: return PORT_BACKEND_D3D11;
	default: return -1;
	}
}

void clearGuard()
{
	if (sArmed) {
		DeleteFileA(sGuard.c_str());
		sArmed = false;
	}
}

} // namespace

extern "C" {

int port_gfx_backend_parse(const char* v)
{
	for (int i = 0; i < PORT_BACKEND_COUNT; i++)
		if (_stricmp(v, kIni[i]) == 0)
			return i;
	if (_stricmp(v, "d3d") == 0 || _stricmp(v, "dx12") == 0)
		return PORT_BACKEND_D3D12;
	if (_stricmp(v, "dx11") == 0)
		return PORT_BACKEND_D3D11;
	if (_stricmp(v, "vk") == 0)
		return PORT_BACKEND_VULKAN;
	return -1;
}

const char* port_gfx_backend_ini(int b) { return b >= 0 && b < PORT_BACKEND_COUNT ? kIni[b] : kIni[0]; }
const char* port_gfx_backend_label(int b) { return b >= 0 && b < PORT_BACKEND_COUNT ? kNames[b] : "?"; }

// From port_settings_configure, after the ini is loaded.
void port_gfx_backend_configure(AuroraConfig* config, const char* settingsPath)
{
	PortSettings& s = gPortSettings;
	int want        = s.backend;
	if (const char* e = std::getenv("SMS_PORT_BACKEND"); e != nullptr && *e != '\0') {
		const int b = port_gfx_backend_parse(e);
		if (b >= 0)
			want = b;
		std::fprintf(stderr, "[port] gfx backend: SMS_PORT_BACKEND=%s\n", e);
	} else if (std::getenv("SMS_PORT_NULLGFX") == nullptr) {
		sGuard = std::string(settingsPath) + ".gpustart";
		if (FILE* f = std::fopen(sGuard.c_str(), "rb")) {
			char buf[32]    = {};
			unsigned long pid = 0;
			std::fgets(buf, sizeof(buf), f);
			std::fclose(f);
			if (const char* sp = std::strchr(buf, ' '))
				pid = std::strtoul(sp + 1, nullptr, 10);
			buf[std::strcspn(buf, "\r\n ")] = '\0';
			// Another copy of the game started from the same settings file and still starting
			// up: not a crash, and its guard is left alone.
			bool alive = false;
			if (pid != 0 && pid != GetCurrentProcessId())
				if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
					DWORD code = 0;
					alive      = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
					CloseHandle(h);
				}
			if (alive) {
				sGuard.clear(); // no guard for this run either
			} else {
				if (port_gfx_backend_parse(buf) == want) {
				sCrashed = want;
				want     = want == PORT_BACKEND_D3D12 ? PORT_BACKEND_VULKAN : PORT_BACKEND_D3D12;
				std::fprintf(stderr,
				             "[port] gfx backend: %s did not finish starting last time (%s left behind); "
				             "switching to %s\n",
				             kNames[sCrashed], sGuard.c_str(), kNames[want]);
					s.backend = want;
					port_settings_save();
				}
				DeleteFileA(sGuard.c_str());
			}
		}
	}
	sRequested             = want;
	config->desiredBackend = toAurora(want);
}

// From port_main.cpp, right before aurora_initialize.
void port_gfx_backend_arm(void)
{
	if (sGuard.empty() || sRequested < 0)
		return;
	if (FILE* f = std::fopen(sGuard.c_str(), "wb")) {
		std::fprintf(f, "%s %lu\n", kIni[sRequested], (unsigned long)GetCurrentProcessId());
		std::fclose(f);
		sArmed = true;
		std::atexit(clearGuard);
	}
}

// From port_window_frame on every presented frame.
void port_gfx_backend_frame(void)
{
	if (sArmed && ++sPresented >= 120)
		clearGuard();
}

// PortBackend actually running (-1: Null or unknown).
int port_gfx_backend_active(void) { return fromAurora(aurora_get_backend()); }

// One line for the menu: what is running and why, if it is not the setting.
const char* port_gfx_backend_status(void)
{
	const AuroraBackend ab = aurora_get_backend();
	const int active       = fromAurora(ab);
	const char* running    = active >= 0 ? kNames[active] : ab == BACKEND_NULL ? "Null (no GPU)" : "?";
	if (sCrashed >= 0 && active != sCrashed)
		std::snprintf(sStatus, sizeof(sStatus), "Running on %s (%s did not finish starting last time)", running,
		              kNames[sCrashed]);
	else if (sRequested >= 0 && active != sRequested)
		std::snprintf(sStatus, sizeof(sStatus), "Running on %s (%s could not start)", running, kNames[sRequested]);
	else
		std::snprintf(sStatus, sizeof(sStatus), "Running on %s", running);
	return sStatus;
}

} // extern "C"

#endif // SMS_PORT_AURORA
