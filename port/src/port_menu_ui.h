// Look and building blocks of the settings menu (port_menu.cpp, port_qol.cpp).
//
// A page is a column of section cards; a card is a list of rows; a row is a label with a short
// explanation on the left and its control on the right, so every setting reads the same way:
//
//   pmui::beginSection("Window");
//   if (pmui::row("Fullscreen", "Alt+Enter or F11 also switch.") && pmui::toggle("##fs", &fs)) ...
//   pmui::endSection();
//
// Controls drawn after row() fill the right column (their ImGui labels are hidden: "##id").
// The menu's theme is set by port_menu.cpp around its window only; these helpers read it.
#pragma once
#ifdef SMS_PORT_AURORA

#define IMGUI_USER_CONFIG "aurora/imgui_config.h" // as Aurora builds imgui
#include <imgui.h>
#include <imgui_internal.h>

#include <cstdarg>
#include <cstdio>

namespace pmui {

// fonts (port_menu.cpp; null = ImGui's own font)
extern ImFont* gFontBody;
extern ImFont* gFontSmall;
extern ImFont* gFontBold;
extern ImFont* gFontTitle;
extern float gScale; // UI scale of this frame (1 = designed for 1080 rows)
extern bool gInTable; // the current card's table began (ImGui skips it while the card is scrolled out of view)

// palette: Delfino at dusk (deep sea, sunshine yellow, water cyan)
inline ImVec4 rgb(int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); }
inline ImVec4 colWindow() { return rgb(14, 27, 46, 0.97f); }
inline ImVec4 colSidebar() { return rgb(10, 21, 36); }
inline ImVec4 colCard() { return rgb(22, 39, 64); }
inline ImVec4 colFrame() { return rgb(31, 54, 86); }
inline ImVec4 colFrameHover() { return rgb(40, 69, 108); }
inline ImVec4 colFrameActive() { return rgb(48, 82, 128); }
inline ImVec4 colAccent() { return rgb(255, 201, 60); }
inline ImVec4 colAccentHover() { return rgb(255, 218, 110); }
inline ImVec4 colAccentText() { return rgb(20, 28, 48); } // text on the accent
inline ImVec4 colWater() { return rgb(63, 208, 224); }
inline ImVec4 colText() { return rgb(238, 243, 250); }
inline ImVec4 colMuted() { return rgb(165, 185, 211); }
inline ImVec4 colWarn() { return rgb(255, 170, 80); }
inline ImU32 col32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

inline float px(float v) { return v * gScale; }

struct FontScope {
	bool pushed;
	explicit FontScope(ImFont* f)
	    : pushed(f != nullptr)
	{
		if (pushed)
			ImGui::PushFont(f);
	}
	~FontScope()
	{
		if (pushed)
			ImGui::PopFont();
	}
};

// Small grey text, wrapped to the available width.
inline void muted(const char* fmt, ...)
{
	FontScope f(gFontSmall);
	ImGui::PushStyleColor(ImGuiCol_Text, colMuted());
	va_list ap;
	va_start(ap, fmt);
	ImGui::TextWrappedV(fmt, ap);
	va_end(ap);
	ImGui::PopStyleColor();
}

inline void warning(const char* fmt, ...)
{
	FontScope f(gFontSmall);
	ImGui::PushStyleColor(ImGuiCol_Text, colWarn());
	va_list ap;
	va_start(ap, fmt);
	ImGui::TextWrappedV(fmt, ap);
	va_end(ap);
	ImGui::PopStyleColor();
}

// The page's heading and what it is about.
inline void pageTitle(const char* title, const char* about)
{
	{
		FontScope f(gFontTitle);
		ImGui::TextUnformatted(title);
	}
	if (about != nullptr && about[0] != '\0')
		muted("%s", about);
	ImGui::Dummy(ImVec2(0.0f, px(6.0f)));
}

// A card of rows. Every beginSection needs its endSection.
inline void beginSection(const char* title, const char* about = nullptr)
{
	ImGui::PushStyleColor(ImGuiCol_ChildBg, colCard());
	ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, px(12.0f));
	ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(20.0f), px(16.0f)));
	ImGui::BeginChild(title, ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
	                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
	{
		FontScope f(gFontBold);
		ImGui::PushStyleColor(ImGuiCol_Text, colAccent());
		ImGui::TextUnformatted(title);
		ImGui::PopStyleColor();
	}
	if (about != nullptr)
		muted("%s", about);
	ImGui::Dummy(ImVec2(0.0f, px(2.0f)));
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(px(8.0f), px(9.0f)));
	gInTable = ImGui::BeginTable("rows", 2, ImGuiTableFlags_SizingStretchProp);
	if (gInTable) {
		ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthStretch, 1.25f);
		ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
	}
}

// Moves to a row's column (nothing while the card's table is skipped: its widgets draw nothing then).
inline void cell(int column, bool newRow)
{
	if (!gInTable)
		return;
	if (newRow)
		ImGui::TableNextRow();
	ImGui::TableSetColumnIndex(column);
}

inline void endSection()
{
	if (gInTable)
		ImGui::EndTable();
	gInTable = false;
	ImGui::PopStyleVar(); // CellPadding
	ImGui::EndChild();
	ImGui::PopStyleVar(2);
	ImGui::PopStyleColor();
	ImGui::Dummy(ImVec2(0.0f, px(10.0f)));
}

// A setting: its name and a short explanation; then the caller draws the control (right column).
// Always returns true (so it can lead an `if` with the control).
inline bool row(const char* label, const char* help = nullptr)
{
	cell(0, true);
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(label);
	if (help != nullptr && help[0] != '\0')
		muted("%s", help);
	cell(1, false);
	ImGui::SetNextItemWidth(-FLT_MIN);
	return true;
}

// Online play: settings that would make this game behave differently from the other players'
// (and so desync the session) are greyed out while a multiplayer session runs:
//   if (lockedRow("Fast-forward", "While the key is held.") && toggle("##ff", &s.ff)) ...;
//   endLocked();
} // namespace pmui
extern "C" int port_mp_active(void); // port_mp_net.cpp
namespace pmui {
inline bool online() { return port_mp_active() != 0; }
inline bool lockedRow(const char* label, const char* help,
                      const char* why = "Off during online play: it would make your game run differently from the others'.")
{
	const bool on = online();
	row(label, on ? why : help);
	ImGui::BeginDisabled(on);
	return true;
}
inline void endLocked() { ImGui::EndDisabled(); }

// A line of information under the controls (right column), e.g. a device's status.
inline void status(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	cell(0, true);
	cell(1, false);
	muted("%s", buf);
}

// A note across the whole card.
inline void note(const char* fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	std::vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	cell(0, true);
	muted("%s", buf);
}

// An on/off switch (Space/Enter/A or a click flips it).
inline bool toggle(const char* id, bool* v)
{
	const float h = ImGui::GetFrameHeight();
	const float w = h * 1.85f;
	const ImVec2 p = ImGui::GetCursorScreenPos();
	bool changed   = false;
	if (ImGui::InvisibleButton(id, ImVec2(w, h))) {
		*v      = !*v;
		changed = true;
	}
	const bool hovered = ImGui::IsItemHovered();
	const bool focused = ImGui::IsItemFocused();
	// the knob slides (a little animation kept per switch)
	ImGuiStorage* st = ImGui::GetStateStorage();
	const ImGuiID key = ImGui::GetItemID();
	float t           = st->GetFloat(key, *v ? 1.0f : 0.0f);
	const float goal  = *v ? 1.0f : 0.0f;
	t += (goal - t) * (ImGui::GetIO().DeltaTime > 0.0f ? ImMin(1.0f, ImGui::GetIO().DeltaTime * 14.0f) : 1.0f);
	st->SetFloat(key, t);
	ImDrawList* dl = ImGui::GetWindowDrawList();
	const ImVec4 off = hovered ? colFrameHover() : colFrame();
	const ImVec4 on  = hovered ? colAccentHover() : colAccent();
	const ImVec4 track(off.x + (on.x - off.x) * t, off.y + (on.y - off.y) * t, off.z + (on.z - off.z) * t, 1.0f);
	dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col32(track), h * 0.5f);
	if (focused)
		dl->AddRect(ImVec2(p.x - px(3.0f), p.y - px(3.0f)), ImVec2(p.x + w + px(3.0f), p.y + h + px(3.0f)), col32(colWater()),
		            h * 0.5f + px(3.0f), 0, px(2.0f));
	const float r  = h * 0.5f - px(3.0f);
	const float cx = p.x + h * 0.5f + (w - h) * t;
	dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f + px(1.0f)), r, IM_COL32(0, 0, 0, 60));
	dl->AddCircleFilled(ImVec2(cx, p.y + h * 0.5f), r, *v ? IM_COL32(255, 255, 255, 255) : col32(colMuted()));
	ImGui::SameLine(0.0f, px(12.0f));
	ImGui::AlignTextToFramePadding();
	ImGui::PushStyleColor(ImGuiCol_Text, *v ? colText() : colMuted());
	ImGui::TextUnformatted(*v ? "On" : "Off");
	ImGui::PopStyleColor();
	return changed;
}

// Full-width controls for the right column.
inline bool combo(const char* id, int* v, const char* const items[], int count)
{
	ImGui::SetNextItemWidth(-FLT_MIN);
	return ImGui::Combo(id, v, items, count);
}

// The value is written beside the bar (inside, the grab would cover it).
inline bool slider(const char* id, int* v, int lo, int hi, const char* fmt = "%d")
{
	char text[32];
	std::snprintf(text, sizeof(text), fmt, *v);
	const float valueW = ImGui::CalcTextSize("0000%").x;
	ImGui::SetNextItemWidth(ImMax(px(60.0f), ImGui::GetContentRegionAvail().x - valueW - ImGui::GetStyle().ItemSpacing.x));
	const bool r = ImGui::SliderInt(id, v, lo, hi, "");
	ImGui::SameLine();
	ImGui::AlignTextToFramePadding();
	std::snprintf(text, sizeof(text), fmt, *v);
	ImGui::TextUnformatted(text);
	return r;
}

// primary = the sunshine-yellow button
inline bool button(const char* label, bool primary = false, float width = 0.0f)
{
	if (primary) {
		ImGui::PushStyleColor(ImGuiCol_Button, colAccent());
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colAccentHover());
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, colAccentHover());
		ImGui::PushStyleColor(ImGuiCol_Text, colAccentText());
	}
	const bool r = ImGui::Button(label, ImVec2(width, 0.0f));
	if (primary)
		ImGui::PopStyleColor(4);
	return r;
}

// a hover explanation for the last control (longer than a row's help)
inline void tip(const char* text)
{
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
		FontScope f(gFontSmall);
		ImGui::SetTooltip("%s", text);
	}
}

} // namespace pmui

#endif // SMS_PORT_AURORA
