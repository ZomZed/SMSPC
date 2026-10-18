// Lobby-panel section for the goop cleaning sync (w6; game side: port_mp_goop.cpp).
// Host: PortMpSessionSettings.reserved[5] = 0 On (default; w15),
// 1 On, 2 Off. Everyone: whether it is active in their stage, and the stamp counts.
#ifdef SMS_PORT_AURORA

#include "port_mp.h"

#define IMGUI_USER_CONFIG "aurora/imgui_config.h" // as Aurora builds imgui
#include <imgui.h>

extern "C" void port_mp_goop_status(int* active, int* layers, unsigned* sent, unsigned* recv, unsigned* snapCells);

namespace {

void section(int isHost)
{
	PortMpSessionSettings s;
	port_mp_get_settings(&s);
	static const char* const kModes[] = { "On (default)", "On", "Off" };
	int mode = s.reserved[5] <= 2 ? s.reserved[5] : 0;
	if (isHost) {
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
		if (ImGui::Combo("Sync goop cleaning", &mode, kModes, 3)) {
			s.reserved[5] = (uint8_t)mode;
			port_mp_host_set_settings(&s);
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Goop one player sprays away is cleaned for everyone in that stage,\n"
			                  "so the stage's \"all clean\" event happens for everyone.");
	} else {
		ImGui::Text("Sync goop cleaning: %s", kModes[mode]);
	}
	int active, layers;
	unsigned sent, recv, snap;
	port_mp_goop_status(&active, &layers, &sent, &recv, &snap);
	if (layers == 0)
		ImGui::TextDisabled("  (no goop in this stage)");
	else
		ImGui::TextDisabled("  %s here: %d goop layers, %u cleaning stamps sent, %u received%s", active ? "Active" : "Off",
		                    layers, sent, recv, snap ? ", joined with a snapshot" : "");
}

struct Register {
	Register() { port_mp_register_lobby_section("Goop cleaning", section); }
} sRegister;

} // namespace

#endif // SMS_PORT_AURORA
