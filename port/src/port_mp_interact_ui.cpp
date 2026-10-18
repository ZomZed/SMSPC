// Multiplayer player interaction (P2, w8): the host options in the lobby panel. Game side:
// port_mp_interact.cpp. Kept separate so ImGui and the game headers never meet in one unit.
//   reserved[0] interaction mode: 0 = Full (default), 1 = Solid only, 2 = Off
//   reserved[4] FLUDD pushes players: 0 = yes (default), 1 = no
#include "port_mp.h"
#include <imgui.h>

namespace {

void lobbySection(int isHost)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	static const char* const kModes[] = { "Full (knockback)", "Solid only", "Off" };
	int mode   = s.reserved[0] <= 2 ? s.reserved[0] : 0;
	bool fludd = s.reserved[4] == 0;
	if (!isHost) {
		ImGui::Text("Player interaction: %s", kModes[mode]);
		ImGui::Text("FLUDD pushes players: %s", fludd ? "yes" : "no");
		return;
	}
	bool changed = false;
	ImGui::SetNextItemWidth(220.0f);
	if (ImGui::Combo("Player interaction", &mode, kModes, 3)) {
		s.reserved[0] = (uint8_t)mode;
		changed       = true;
	}
	if (ImGui::IsItemHovered())
		ImGui::SetTooltip("Full: players are solid, stomping or ground-pounding someone knocks them back.\n"
		                  "Solid only: players push each other apart, nothing else.\n"
		                  "Off: players pass through each other. Nobody loses health either way.");
	ImGui::BeginDisabled(mode != 0);
	if (ImGui::Checkbox("FLUDD pushes players", &fludd)) {
		s.reserved[4] = fludd ? 0 : 1;
		changed       = true;
	}
	ImGui::EndDisabled();
	if (changed)
		port_mp_host_set_settings(&s);
}

struct Register {
	Register() { port_mp_register_lobby_section("Player interaction", lobbySection); }
} sRegister;

} // namespace
