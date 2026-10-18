#include <JSystem/JDrama/JDRViewObj.hpp>

#ifdef SMS_DRAWHANG_DEBUG
// Diagnostic only (all-source debug build): while gSmsDrawHangDebug is set, log
// every object performed and wait for the GPU after each one, so the last
// "DH>" line in the log names the object whose draw hangs the GPU.
#include <dolphin/os.h>
#include <dolphin/gx.h>
extern int gSmsDrawHangDebug;
#endif

void JDrama::TViewObj::testPerform(u32 cue, JDrama::TGraphics* graphics)
{
	if (((cue & CUE_MOVEMENT_GATE_A) != 0) && unkC.check(CUE_MOVEMENT_GATE_A)) {
		cue = cue & ~CUE_MOVE;
	}
	if (((cue & CUE_MOVEMENT_GATE_B) != 0) && unkC.check(CUE_MOVEMENT_GATE_B)) {
		cue = cue & ~CUE_MOVE;
	}
	cue &= ~unkC.get();
	if (cue) {
#ifdef SMS_DRAWHANG_DEBUG
		if (gSmsDrawHangDebug) {
			const char* n = getName() ? getName() : "(null)";
			OSReport("DH> %p %s cue=%x\n", this, n, cue);
			perform(cue, graphics);
			GXSetDrawDone();
			GXWaitDrawDone();
			OSReport("DH< %p %s\n", this, n);
			return;
		}
#endif
		perform(cue, graphics);
	}
}
