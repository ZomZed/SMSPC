// Shared objectives follow-ups (P1b, w8): the host option in the lobby panel. Game side:
// port_mp_objectives_extra.cpp. Kept apart so ImGui and the game headers never meet in one unit.
//   reserved[7] remote Shines: 0 = toast only (default), 1 = also the "Shine Get" banner for the
//               players in the stage where it was collected
#include "port_mp.h"
#include <imgui.h>

namespace {

void lobbySection(int isHost)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	static const char* const kModes[] = { "Toast only", "Toast + \"Shine Get\" banner in that stage" };
	int mode = s.reserved[7] == 1 ? 1 : 0;
	if (!s.sharedObjectives) {
		ImGui::TextDisabled("Shared objectives are off: every player keeps his own coins and Shines.");
		return;
	}
	if (!isHost) {
		ImGui::Text("Someone else's Shine: %s", kModes[mode]);
	} else {
		ImGui::SetNextItemWidth(320.0f);
		if (ImGui::Combo("Someone else's Shine", &mode, kModes, 2)) {
			s.reserved[7] = (uint8_t)mode;
			port_mp_host_set_settings(&s);
		}
	}
	ImGui::TextDisabled("Red-coin switches start the challenge for everyone in the stage;\n"
	                    "coins anyone collects count for everyone (the 100-coin Shine too).");
}

struct Register {
	Register() { port_mp_register_lobby_section("Shared objectives", lobbySection); }
} sRegister;

} // namespace
