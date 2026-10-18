// Multiplayer overlay (w15): puppet name tags and chat speech bubbles, drawn with Dear
// ImGui over the presented picture at the screen positions port_mp_puppet.cpp projected
// with the game's own projection and viewport for the last logic frame. Also runs the
// character menu (port_mp_custom.cpp).
//
// Speech bubbles: each player's latest chat line (or emote) from w6's chat
// (port_mp_chat_last_said in port_mp_chat.cpp) above his puppet, or above the local Mario
// for our own lines; 5 s, fading out over the last second; wrapped at a max width and
// kept inside the picture. The chat log at the bottom left is w6's and stays as it is.
#ifdef SMS_PORT_AURORA

#include "port_mp_puppet.h"
#include "port_mp.h"
#include "port_settings.h"

#include <SDL3/SDL_video.h>

#include "port_mp_ui.h" // the game font (mpui_text); includes imgui

#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>

// w6's chat (port_mp_chat.cpp); the fallback keeps this file linkable without it.
extern "C" {
int port_mp_chat_last_said(int slot, char* text, int cap, int* emote, double* atMs);
int port_mp_w15_no_last_said(int, char*, int, int*, double*) { return 0; }
}
#pragma comment(linker, "/alternatename:port_mp_chat_last_said=port_mp_w15_no_last_said")

namespace {

std::mutex sMutex;
PortMpNameTag sTags[PORT_MP_MAX_PUPPETS + 1];
int sCount;
std::chrono::steady_clock::time_point sSetAt; // tags older than kStale are not drawn
const std::chrono::milliseconds kStale(500);   // (left the stage, a movie, a load)

const double kBubbleMs = 5000.0; // a bubble shows this long
const double kFadeMs   = 1000.0; // the last part fades

// The game picture inside the window (w6's display aspect; 0 = stretched).
void contentBox(float winW, float winH, float& x, float& y, float& w, float& h)
{
	const float aspect = port_get_display_aspect();
	x = 0.0f, y = 0.0f, w = winW, h = winH;
	if (aspect <= 0.0f || winW <= 0.0f || winH <= 0.0f)
		return;
	if (winW / winH > aspect) {
		w = winH * aspect;
		x = (winW - w) * 0.5f;
	} else {
		h = winW / aspect;
		y = (winH - h) * 0.5f;
	}
}

// A five-pointed star (the emote mark).
void star(ImDrawList* dl, ImVec2 c, float r, ImU32 col)
{
	ImVec2 pts[10];
	for (int i = 0; i < 10; ++i) {
		const float a  = -1.5707963f + i * 0.6283185f;
		const float rr = (i & 1) ? r * 0.45f : r;
		pts[i]         = ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr);
	}
	for (int i = 0; i < 10; ++i) // fan from the centre (the outline is concave)
		dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 10], col);
}

void drawBubble(ImDrawList* dl, ImFont* font, float size, float bx, float by, float bw, float bh, float ax,
                float ay, float lift, const char* text, int emote, float alpha)
{
	const float pad   = size * 0.45f;
	const float maxW  = bw * 0.30f;
	const float icon  = emote >= 0 ? size * 1.1f : 0.0f;
	ImVec2 ts         = font->CalcTextSizeA(size, 1e9f, maxW - icon, text);
	const float w     = ts.x + icon + pad * 2.0f;
	const float h     = (ts.y > icon ? ts.y : icon) + pad * 2.0f;
	const float tail  = size * 0.5f;
	float x0          = ax - w * 0.5f;
	float y0          = ay - lift - tail - h;
	// Inside the picture (the tail still points at the player).
	if (x0 < bx + 4.0f)
		x0 = bx + 4.0f;
	if (x0 + w > bx + bw - 4.0f)
		x0 = bx + bw - 4.0f - w;
	if (y0 < by + 4.0f)
		y0 = by + 4.0f;
	if (y0 + h > by + bh - 4.0f)
		y0 = by + bh - 4.0f - h;
	const int a    = (int)(alpha * 235.0f);
	const ImU32 bg = IM_COL32(255, 255, 255, a), fg = IM_COL32(20, 20, 30, (int)(alpha * 255.0f));
	const ImU32 ol = IM_COL32(0, 0, 0, (int)(alpha * 120.0f));
	float tx       = ax < x0 + tail ? x0 + tail : (ax > x0 + w - tail ? x0 + w - tail : ax);
	dl->AddRectFilled(ImVec2(x0 - 1, y0 - 1), ImVec2(x0 + w + 1, y0 + h + 1), ol, pad + 1);
	dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + w, y0 + h), bg, pad);
	if (y0 + h < ay - lift) // tail, when the bubble is above the player
		dl->AddTriangleFilled(ImVec2(tx - tail * 0.6f, y0 + h - 1), ImVec2(tx + tail * 0.6f, y0 + h - 1),
		                      ImVec2(tx, y0 + h + tail), bg);
	float x = x0 + pad;
	if (emote >= 0) {
		star(dl, ImVec2(x + icon * 0.5f, y0 + h * 0.5f), icon * 0.5f, IM_COL32(250, 200, 30, a));
		x += icon;
	}
	dl->AddText(font, size, ImVec2(x, y0 + pad), fg, text, nullptr, maxW - icon);
}

} // namespace

// A world-space cage outline projected by a game mode (Cops and Robbers' prison): points in the
// game picture (0..1), a ring on the ground, one overhead and bars between them; drawn while fresh.
namespace {
const int kCageMax = 32;
struct Cage {
	int n;
	float bottom[kCageMax][2], top[kCageMax][2];
	unsigned char valid[kCageMax];
	float label[2];
	int labelValid;
	uint32_t rgb;
	char text[24];
	std::chrono::steady_clock::time_point at;
} sCage;
} // namespace

extern "C" void port_mp_overlay_set_cage(const float* bottom, const float* top, const unsigned char* valid, int n,
                                         const float* label, int labelValid, uint32_t rgb, const char* text)
{
	std::lock_guard<std::mutex> lock(sMutex);
	sCage.n = n > kCageMax ? kCageMax : (n < 0 ? 0 : n);
	if (sCage.n > 0) {
		std::memcpy(sCage.bottom, bottom, sizeof(float) * 2 * sCage.n);
		std::memcpy(sCage.top, top, sizeof(float) * 2 * sCage.n);
		std::memcpy(sCage.valid, valid, sCage.n);
	}
	sCage.label[0]   = label[0];
	sCage.label[1]   = label[1];
	sCage.labelValid = labelValid;
	sCage.rgb        = rgb;
	std::snprintf(sCage.text, sizeof(sCage.text), "%s", text != nullptr ? text : "");
	sCage.at = std::chrono::steady_clock::now();
}

static void drawCage(ImDrawList* dl, float bx, float by, float bw, float bh)
{
	Cage c;
	{
		std::lock_guard<std::mutex> lock(sMutex);
		if ((sCage.n == 0 && !sCage.labelValid) || std::chrono::steady_clock::now() - sCage.at > kStale)
			return;
		c = sCage;
	}
	const unsigned r = (c.rgb >> 16) & 0xFF, g = (c.rgb >> 8) & 0xFF, b = c.rgb & 0xFF;
	const ImU32 col = IM_COL32(r, g, b, 210), shadow = IM_COL32(0, 0, 0, 120);
	const float thick = bh * 0.004f > 2.0f ? bh * 0.004f : 2.0f;
	auto P = [&](const float* p) { return ImVec2(bx + p[0] * bw, by + p[1] * bh); };
	for (int i = 0; i < c.n; i++) {
		const int j = (i + 1) % c.n;
		if (!c.valid[i])
			continue;
		if (c.valid[j]) {
			for (int k = 0; k < 2; k++) { // the rings (a dark outline under the colour)
				const float* a  = k == 0 ? c.bottom[i] : c.top[i];
				const float* a2 = k == 0 ? c.bottom[j] : c.top[j];
				dl->AddLine(P(a), P(a2), shadow, thick + 2.0f);
				dl->AddLine(P(a), P(a2), col, thick);
			}
		}
		dl->AddLine(P(c.bottom[i]), P(c.top[i]), shadow, thick + 1.0f); // the bars
		dl->AddLine(P(c.bottom[i]), P(c.top[i]), col, thick * 0.8f);
	}
	if (c.labelValid && c.text[0] != '\0') {
		ImFont* font     = ImGui::GetFont();
		const float size = bh * 0.03f > ImGui::GetFontSize() ? bh * 0.03f : ImGui::GetFontSize();
		const ImVec2 sz  = font->CalcTextSizeA(size, 1e9f, 0.0f, c.text);
		const ImVec2 p   = ImVec2(bx + c.label[0] * bw - sz.x * 0.5f, by + c.label[1] * bh - sz.y);
		dl->AddRectFilled(ImVec2(p.x - 6.0f, p.y - 2.0f), ImVec2(p.x + sz.x + 6.0f, p.y + sz.y + 2.0f), IM_COL32(0, 0, 0, 150), 4.0f);
		dl->AddText(font, size, p, col, c.text);
	}
}

extern "C" void port_mp_overlay_set_tags(const PortMpNameTag* tags, int count)
{
	std::lock_guard<std::mutex> lock(sMutex);
	if (count > PORT_MP_MAX_PUPPETS + 1)
		count = PORT_MP_MAX_PUPPETS + 1;
	if (count > 0)
		memcpy(sTags, tags, sizeof(PortMpNameTag) * count);
	sCount = count < 0 ? 0 : count;
	sSetAt = std::chrono::steady_clock::now();
}

extern "C" int port_mp_overlay_get_tag(int slot, float* x, float* y, int* visible)
{
	if (visible != nullptr)
		*visible = 0;
	if (ImGui::GetCurrentContext() == nullptr)
		return 0;
	std::lock_guard<std::mutex> lock(sMutex);
	if (std::chrono::steady_clock::now() - sSetAt > kStale)
		return 0;
	for (int i = 0; i < sCount; ++i)
		if (sTags[i].slot == slot) {
			const ImVec2 disp = ImGui::GetIO().DisplaySize;
			float bx, by, bw, bh;
			contentBox(disp.x, disp.y, bx, by, bw, bh);
			if (x != nullptr)
				*x = bx + sTags[i].x * bw;
			if (y != nullptr)
				*y = by + sTags[i].y * bh;
			if (visible != nullptr)
				*visible = sTags[i].x >= 0.0f && sTags[i].x <= 1.0f && sTags[i].y >= 0.0f && sTags[i].y <= 1.0f;
			return 1;
		}
	return 0;
}

extern "C" void port_mp_overlay_draw(void)
{
	if (ImGui::GetCurrentContext() == nullptr)
		return;
	port_mp_custom_frame(); // character menu (F4), port_mp_custom.cpp
	PortMpNameTag tags[PORT_MP_MAX_PUPPETS + 1];
	int n;
	{
		std::lock_guard<std::mutex> lock(sMutex);
		n = std::chrono::steady_clock::now() - sSetAt > kStale ? 0 : sCount;
		memcpy(tags, sTags, sizeof(PortMpNameTag) * n);
	}
	const ImVec2 disp = ImGui::GetIO().DisplaySize;
	float bx, by, bw, bh;
	contentBox(disp.x, disp.y, bx, by, bw, bh);
	ImDrawList* dl   = ImGui::GetBackgroundDrawList();
	drawCage(dl, bx, by, bw, bh); // a game mode's cage marker (Cops and Robbers' prison)
	if (n == 0)
		return;
	ImFont* font     = ImGui::GetFont();
	const float base = ImGui::GetFontSize();
	// Readable at any window size: about 2.2% of the picture height, never below the UI font.
	const float size = bh * 0.022f > base ? bh * 0.022f : base;
	const double now = port_mp_time_ms();
	for (int i = 0; i < n; ++i) {
		const PortMpNameTag& t = tags[i];
		const float cx         = bx + t.x * bw;
		const float cy         = by + t.y * bh;
		float tagH             = 0.0f;
		if (!t.local) {
			const bool game = mpui_font_ready(); // the game's own font, else ImGui's
			const float px  = size * 1.25f;
			const ImVec2 sz = game ? mpui_text_size(t.name, px) : font->CalcTextSizeA(size, 1e9f, 0.0f, t.name);
			const ImVec2 p(cx - sz.x * 0.5f, cy - sz.y);
			const int a  = (int)(t.alpha * 255.0f);
			const int ab = (int)(t.alpha * 150.0f);
			dl->AddRectFilled(ImVec2(p.x - 5.0f, p.y - 2.0f), ImVec2(p.x + sz.x + 5.0f, p.y + sz.y + 2.0f),
			                  IM_COL32(0, 0, 0, ab), 4.0f);
			const unsigned r = (t.rgb >> 16) & 0xFF, g = (t.rgb >> 8) & 0xFF, b = t.rgb & 0xFF;
			if (game) {
				mpui_text(dl, p, px, IM_COL32(r, g, b, a), t.name);
			} else {
				dl->AddText(font, size, ImVec2(p.x + 1.0f, p.y + 1.0f), IM_COL32(0, 0, 0, a), t.name);
				dl->AddText(font, size, p, IM_COL32(r, g, b, a), t.name);
			}
			tagH = sz.y + 4.0f;
		}
		// Speech bubble above the tag (above the head for the local Mario).
		char said[128];
		int emote = -1;
		double at = 0.0;
		if (t.slot < 0 || !port_mp_chat_last_said(t.slot, said, sizeof(said), &emote, &at))
			continue;
		const double age = now - at;
		if (age < 0.0 || age > kBubbleMs)
			continue;
		float alpha = age > kBubbleMs - kFadeMs ? (float)((kBubbleMs - age) / kFadeMs) : 1.0f;
		if (!t.local)
			alpha *= t.alpha < 0.35f ? 0.35f : t.alpha; // far away: fainter, still readable
		drawBubble(dl, font, size, bx, by, bw, bh, cx, cy, tagH, said, emote, alpha);
	}
}

#endif // SMS_PORT_AURORA
