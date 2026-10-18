#include <Map/PollutionEvent.hpp>

// rogue includes needed for matching sinit & bss
#include <MSound/MSSetSound.hpp>
#include <MSound/MSoundBGM.hpp>

// The retail object emits loadAfter BEFORE the weak vtable functions (dtor, perform),
// which only happens without deferred codegen; nothing else in this TU survives the link.
#pragma defer_codegen off

void TPollutionTest::registerEvent(JDrama::TViewObj*) { }

void TPollutionTest::loadAfter() { JDrama::TViewObj::loadAfter(); }

void TPollutionEventModelStampMario::perform(u32 cue,
                                             JDrama::TGraphics* graphics)
{
}

void TPollutionEventModelStamp::perform(u32 cue, JDrama::TGraphics* graphics) {
}

void TPollutionEventModelStamp::init(const char*) { }

TPollutionEventModelStamp::TPollutionEventModelStamp(const char*) { }

void TPollutionEventMaze::perform(u32 cue, JDrama::TGraphics* graphics) { }

void TPollutionEventMaze::init() { }

TPollutionEventMaze::TPollutionEventMaze(const char*) { }

void TPollutionEventAreaObj::perform(u32 cue, JDrama::TGraphics* graphics) { }

void TPollutionEventAreaObj::init(const char*) { }

TPollutionEventAreaObj::TPollutionEventAreaObj(const char*) { }
