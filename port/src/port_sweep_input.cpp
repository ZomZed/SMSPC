// Scripted analog input for unattended level tests (w11 level sweep). Aurora build only,
// off unless SMS_PORT_STICKSCRIPT is set. Called from port_main.cpp's port_PADRead after
// the real (clamped) pad, before SMS_PORT_PADSCRIPT; it only changes pad 0. Values are
// written as the game sees them (already in the clamped GameCube range).
// (SMS_PORT_PADSCRIPT has since gained S<degrees> stick tokens; this file adds the
// continuous "auto" walk used by the sweep and can be folded into it.)
//
// SMS_PORT_STICKSCRIPT=<entry>[;<entry>...]
//   <from>-<to>:<x>,<y>[,<buttons>]  hold main stick (x, y in -127..127) and the buttons
//                                    ('+'-separated names: A B X Y Z L R START UP DOWN LEFT
//                                    RIGHT) for frames from <= f < to
//   auto:<from>[-<to>]               from that frame on: walk in a slowly turning circle,
//                                    spray (R) half the time, jump (A) every few seconds
// Frames are the presented-frame counter used by SMS_PORT_SHOT / SMS_PORT_PADSCRIPT.
#ifdef SMS_PORT_AURORA

#include <dolphin/pad.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

u16 parseButtons(const char* p, const char* end)
{
	static const struct {
		const char* name;
		u16 mask;
	} kNames[] = { { "START", PAD_BUTTON_START }, { "A", PAD_BUTTON_A },  { "B", PAD_BUTTON_B },
		           { "X", PAD_BUTTON_X },         { "Y", PAD_BUTTON_Y },  { "Z", PAD_TRIGGER_Z },
		           { "L", PAD_TRIGGER_L },        { "R", PAD_TRIGGER_R }, { "UP", PAD_BUTTON_UP },
		           { "DOWN", PAD_BUTTON_DOWN },   { "LEFT", PAD_BUTTON_LEFT },
		           { "RIGHT", PAD_BUTTON_RIGHT } };
	u16 mask = 0;
	while (p < end) {
		const char* tok = p;
		while (p < end && *p != '+')
			++p;
		const size_t len = (size_t)(p - tok);
		for (const auto& n : kNames)
			if (std::strlen(n.name) == len && std::strncmp(n.name, tok, len) == 0)
				mask |= n.mask;
		if (p < end)
			++p;
	}
	return mask;
}

void apply(PADStatus* s, int x, int y, u16 buttons)
{
	s->stickX = (s8)(x < -127 ? -127 : x > 127 ? 127 : x);
	s->stickY = (s8)(y < -127 ? -127 : y > 127 ? 127 : y);
	s->button |= buttons;
	s->err = PAD_ERR_NONE;
	if (buttons & PAD_TRIGGER_R)
		s->triggerRight = 0xFF;
	if (buttons & PAD_TRIGGER_L)
		s->triggerLeft = 0xFF;
	if (buttons & PAD_BUTTON_A)
		s->analogA = 0xFF;
	if (buttons & PAD_BUTTON_B)
		s->analogB = 0xFF;
}

} // namespace

extern "C" void port_sweep_pad_apply(PADStatus* status, u32 frame)
{
	static const char* sScript = std::getenv("SMS_PORT_STICKSCRIPT");
	if (sScript == nullptr || status == nullptr)
		return;
	for (const char* p = sScript; *p != '\0';) {
		const char* end = std::strchr(p, ';');
		if (end == nullptr)
			end = p + std::strlen(p);
		if (std::strncmp(p, "auto:", 5) == 0) {
			char* q;
			unsigned long from = std::strtoul(p + 5, &q, 10);
			unsigned long to   = 0xFFFFFFFFul;
			if (*q == '-')
				to = std::strtoul(q + 1, nullptr, 10);
			if (frame >= from && frame < to) {
				const u32 t     = frame - (u32)from;
				const float ang = (float)t * (6.2831853f / 600.0f); // one turn per 10 s
				u16 buttons     = 0;
				if ((t / 120) % 2 == 1)
					buttons |= PAD_TRIGGER_R; // spray
				if (t % 240 >= 200 && t % 240 < 206)
					buttons |= PAD_BUTTON_A; // short jump press
				// radius 56: inside the clamped GameCube octagon (port_input.cpp)
				apply(status, (int)(56.0f * std::cos(ang)), (int)(56.0f * std::sin(ang)), buttons);
				return;
			}
		} else {
			char* q;
			unsigned long from = std::strtoul(p, &q, 10);
			unsigned long to   = from;
			if (*q == '-')
				to = std::strtoul(q + 1, &q, 10);
			if (*q == ':') {
				int x = (int)std::strtol(q + 1, &q, 10);
				int y = 0;
				if (*q == ',')
					y = (int)std::strtol(q + 1, &q, 10);
				u16 buttons = 0;
				if (*q == ',')
					buttons = parseButtons(q + 1, end);
				if (frame >= from && frame < to) {
					apply(status, x, y, buttons);
					return;
				}
			}
		}
		p = *end == ';' ? end + 1 : end;
	}
}

#endif // SMS_PORT_AURORA
