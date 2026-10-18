// Interim VI / PAD / CARD for the headless boot (Aurora replaces VI, PAD and
// CARD).
//
// VI: a 59.94 Hz retrace interrupt on the interrupt thread increments the
//     retrace count, runs the pre/post retrace callbacks and wakes threads in
//     VIWaitForRetrace. Always NTSC, no component cable.
// PAD: port 1 reads the keyboard (only while this console window has focus);
//     ports 2-4 report no controller. Keyboard mapping:
//       arrows        main stick          I/J/K/L   C-stick (up/left/down/right)
//       Z             A                   X         B
//       C             X button            V         Y button
//       Enter         Start               Space     Z trigger
//       Left Shift    R (full press)      Left Ctrl L (full press)
//       T/F/G/H       D-pad up/left/down/right
// CARD: no memory card inserted.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "port_sdk.h"

#include <dolphin/card.h>
#include <dolphin/os.h>
#include <dolphin/pad.h>
#include <dolphin/vi.h>

#include <cstring>

namespace {

volatile u32 gRetraceCount;
OSThreadQueue gRetraceQueue;
VIRetraceCallback gPreCB, gPostCB;
int gViSlot = -1;
void* gNextFb;
BOOL gBlack = TRUE;

void viRetrace()
{
	++gRetraceCount;
	if (gPreCB)
		gPreCB(gRetraceCount);
	if (gPostCB)
		gPostCB(gRetraceCount);
	OSWakeupThread(&gRetraceQueue);
}

inline bool key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

} // namespace

extern "C" {

// ---- VI ----------------------------------------------------------------------
void VIInit(void)
{
	if (gViSlot >= 0)
		return;
	OSInitThreadQueue(&gRetraceQueue);
	gViSlot = port_irq_add_periodic(viRetrace, 16683333); // NTSC field rate
}
void VIWaitForRetrace(void)
{
	BOOL en = OSDisableInterrupts();
	u32 c   = gRetraceCount;
	do
		OSSleepThread(&gRetraceQueue);
	while (c == gRetraceCount);
	OSRestoreInterrupts(en);
}
void VIConfigure(GXRenderModeObj*) { }
void VIConfigurePan(u16, u16, u16, u16) { }
void VIFlush(void) { }
void VISetNextFrameBuffer(void* fb) { gNextFb = fb; }
void VISetNextRightFrameBuffer(void*) { }
void VISetBlack(BOOL black) { gBlack = black; }
void VISet3D(BOOL) { }
u32 VIGetRetraceCount(void) { return gRetraceCount; }
u32 VIGetNextField(void) { return gRetraceCount & 1; }
u32 VIGetCurrentLine(void) { return 0; }
u32 VIGetTvFormat(void) { return VI_NTSC; }
u32 VIGetDTVStatus(void) { return 0; }
VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
	BOOL en               = OSDisableInterrupts();
	VIRetraceCallback old = gPreCB;
	gPreCB                = cb;
	OSRestoreInterrupts(en);
	return old;
}
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
	BOOL en               = OSDisableInterrupts();
	VIRetraceCallback old = gPostCB;
	gPostCB               = cb;
	OSRestoreInterrupts(en);
	return old;
}

// ---- PAD ---------------------------------------------------------------------
BOOL PADInit() { return TRUE; }
int PADReset(unsigned long) { return TRUE; }
BOOL PADRecalibrate(u32) { return TRUE; }
void PADSetSpec(u32) { }
void PADSetAnalogMode(u32) { }
void PADControlMotor(s32, u32) { }
void PADSetSamplingRate(unsigned long) { }
BOOL PADSync(void) { return TRUE; }

u32 PADRead(PADStatus* status)
{
	std::memset(status, 0, sizeof(PADStatus) * PAD_MAX_CONTROLLERS);
	for (int i = 1; i < PAD_MAX_CONTROLLERS; i++)
		status[i].err = PAD_ERR_NO_CONTROLLER;
	PADStatus& s = status[0];
	s.err        = PAD_ERR_NONE;
	HWND con     = GetConsoleWindow();
	if (con && GetForegroundWindow() != con)
		return PAD_CHAN0_BIT;
	u16 b = 0;
	if (key('Z')) b |= PAD_BUTTON_A;
	if (key('X')) b |= PAD_BUTTON_B;
	if (key('C')) b |= PAD_BUTTON_X;
	if (key('V')) b |= PAD_BUTTON_Y;
	if (key(VK_RETURN)) b |= PAD_BUTTON_START;
	if (key(VK_SPACE)) b |= PAD_TRIGGER_Z;
	if (key(VK_LSHIFT)) {
		b |= PAD_TRIGGER_R;
		s.triggerRight = 0xFF;
	}
	if (key(VK_LCONTROL)) {
		b |= PAD_TRIGGER_L;
		s.triggerLeft = 0xFF;
	}
	if (key('T')) b |= PAD_BUTTON_UP;
	if (key('G')) b |= PAD_BUTTON_DOWN;
	if (key('F')) b |= PAD_BUTTON_LEFT;
	if (key('H')) b |= PAD_BUTTON_RIGHT;
	s.button    = b;
	s.stickX    = (s8)((key(VK_RIGHT) ? 100 : 0) - (key(VK_LEFT) ? 100 : 0));
	s.stickY    = (s8)((key(VK_UP) ? 100 : 0) - (key(VK_DOWN) ? 100 : 0));
	s.substickX = (s8)((key('L') ? 100 : 0) - (key('J') ? 100 : 0));
	s.substickY = (s8)((key('I') ? 100 : 0) - (key('K') ? 100 : 0));
	return PAD_CHAN0_BIT;
}

// PADClamp as the SDK's Padclamp.c (octagonal stick gate, trigger dead zone).
static void clampStick(s8* px, s8* py, s8 max, s8 xy, s8 min)
{
	int x = *px, y = *py, signX, signY, d;
	if (0 <= x) signX = 1; else { signX = -1; x = -x; }
	if (0 <= y) signY = 1; else { signY = -1; y = -y; }
	if (x <= min) x = 0; else x -= min;
	if (y <= min) y = 0; else y -= min;
	if (x == 0 && y == 0) {
		*px = *py = 0;
		return;
	}
	if (xy * y <= xy * x) {
		d = xy * x + (max - xy) * y;
		if (xy * max < d) {
			x = (s8)(xy * max * x / d);
			y = (s8)(xy * max * y / d);
		}
	} else {
		d = xy * y + (max - xy) * x;
		if (xy * max < d) {
			x = (s8)(xy * max * x / d);
			y = (s8)(xy * max * y / d);
		}
	}
	*px = (s8)(signX * x);
	*py = (s8)(signY * y);
}
static void clampTrigger(u8* t)
{
	if (*t <= 30) *t = 0;
	else if (180 < *t) *t = 150;
	else *t -= 30;
}
void PADClamp(PADStatus* status)
{
	for (int i = 0; i < PAD_MAX_CONTROLLERS; i++, status++) {
		if (status->err != PAD_ERR_NONE)
			continue;
		clampStick(&status->stickX, &status->stickY, 72, 40, 15);
		clampStick(&status->substickX, &status->substickY, 59, 31, 15);
		clampTrigger(&status->triggerLeft);
		clampTrigger(&status->triggerRight);
	}
}

// ---- CARD: no card -------------------------------------------------------------
void CARDInit(void) { }
s32 CARDGetResultCode(s32) { return CARD_RESULT_NOCARD; }
s32 CARDProbeEx(s32, s32* memSize, s32* sectorSize)
{
	if (memSize) *memSize = 0;
	if (sectorSize) *sectorSize = 0;
	return CARD_RESULT_NOCARD;
}
int CARDProbe(long) { return FALSE; }
s32 CARDMount(s32, void*, CARDCallback) { return CARD_RESULT_NOCARD; }
s32 CARDUnmount(s32) { return CARD_RESULT_NOCARD; }
long CARDCheck(long) { return CARD_RESULT_NOCARD; }
long CARDFormat(long) { return CARD_RESULT_NOCARD; }
s32 CARDFreeBlocks(s32, s32* b, s32* f)
{
	if (b) *b = 0;
	if (f) *f = 0;
	return CARD_RESULT_NOCARD;
}
s32 CARDOpen(s32, char*, CARDFileInfo*) { return CARD_RESULT_NOCARD; }
s32 CARDClose(CARDFileInfo*) { return CARD_RESULT_NOCARD; }
long CARDCreate(long, char*, unsigned long, CARDFileInfo*) { return CARD_RESULT_NOCARD; }
long CARDRead(CARDFileInfo*, void*, s32, s32) { return CARD_RESULT_NOCARD; }
long CARDWrite(CARDFileInfo*, void*, long, long) { return CARD_RESULT_NOCARD; }
s32 CARDGetStatus(s32, s32, CARDStat*) { return CARD_RESULT_NOCARD; }
long CARDSetStatus(long, long, CARDStat*) { return CARD_RESULT_NOCARD; }

} // extern "C"
