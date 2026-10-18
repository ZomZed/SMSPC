#include <JSystem/JKernel/JKRHeap.hpp>
#include <JSystem/JUtility/JUTAssert.hpp>
#include <macros.h>
#include <string.h>
#include <dolphin/os.h>
#ifdef SMS_PORT
#include "port_profile.h" // w14: SMS_PORT_PROFILE=1 profiler
// port/src/sdk_os_thread.cpp: takes a pending switch to a higher-priority thread
// (the GameCube would have preempted at once; see JKRHeap::alloc).
extern "C" void port_sched_point(void);
#endif

JKRHeap* JKRHeap::sSystemHeap;
JKRHeap* JKRHeap::sCurrentHeap;
JKRHeap* JKRHeap::sRootHeap;
JKRHeapErrorHandler* JKRHeap::mErrorHandler;
void* JKRHeap::mCodeStart;
void* JKRHeap::mCodeEnd;
void* JKRHeap::mUserRamStart;
void* JKRHeap::mUserRamEnd;
u32 JKRHeap::mMemorySize;

JKRHeap::JKRHeap(void* data, u32 size, JKRHeap* parent, bool errorFlag)
    : JKRDisposer()
    , mChildTree(this)
    , mDisposerList()
{
	OSInitMutex(&mMutex);
	mSize  = size;
	mStart = (u8*)data;
	mEnd   = ((u8*)data + size);
	if (parent == nullptr) {
		becomeSystemHeap();
		becomeCurrentHeap();
	} else {
		parent->mChildTree.appendChild(&mChildTree);
		if (sSystemHeap == sRootHeap)
			becomeSystemHeap();
		if (sCurrentHeap == sRootHeap)
			becomeCurrentHeap();
	}
	mErrorFlag = errorFlag;
	if (mErrorFlag == true && mErrorHandler == nullptr)
		mErrorHandler = JKRDefaultMemoryErrorRoutine;
}

JKRHeap::~JKRHeap()
{
	mChildTree.getParent()->removeChild(&mChildTree);
	JSUTree<JKRHeap>* nextRootHeap = sRootHeap->mChildTree.getFirstChild();
	if (sCurrentHeap == this)
		sCurrentHeap = !nextRootHeap ? sRootHeap : nextRootHeap->getObject();

	if (sSystemHeap == this)
		sSystemHeap = !nextRootHeap ? sRootHeap : nextRootHeap->getObject();
}

bool JKRHeap::initArena(char** outUserRamStart, u32* outUserRamSize,
                        int numHeaps)
{
	void* arenaLo = OSGetArenaLo();
	void* arenaHi = OSGetArenaHi();
	if (arenaLo == arenaHi) {
		return false;
	}
	void* arenaStart = OSInitAlloc(arenaLo, arenaHi, numHeaps);
	arenaHi          = (u8*)OSRoundDown32B(arenaHi);
	arenaLo          = (u8*)OSRoundUp32B(arenaStart);
	u8* start        = (u8*)OSPhysicalToCached(0);
	mCodeEnd         = (u8*)arenaLo;
	mCodeStart       = (u8*)start;
	mUserRamStart    = (u8*)arenaLo;
	mUserRamEnd      = (u8*)arenaHi;
	mMemorySize      = *(u32*)((start + 0x28));
	OSReport("%x %x %x %x %x\n", mCodeStart, mCodeEnd, mUserRamStart,
	         mUserRamEnd, mMemorySize);
	OSSetArenaLo(arenaHi);
	OSSetArenaHi(arenaHi);
	*outUserRamStart = (char*)arenaLo;
	*outUserRamSize  = (uintptr_t)arenaHi - (uintptr_t)arenaLo;
	OSReport("arenaLo=%x arenaHi=%x \n", arenaLo, arenaHi);
	return true;
}

JKRHeap* JKRHeap::becomeSystemHeap()
{
	JKRHeap* old = sSystemHeap;
	sSystemHeap  = this;
	return old;
}

JKRHeap* JKRHeap::becomeCurrentHeap()
{
	JKRHeap* old = sCurrentHeap;
	sCurrentHeap = this;
	return old;
}

void* JKRHeap::alloc(u32 byteCount, int padding, JKRHeap* heap)
{
#ifdef SMS_PORT
	// w14: a scheduling point. Long stretches of game code (setupObjects runs on the
	// main thread for 30-80 ms) otherwise starve the audio thread, which the
	// GameCube's interrupts would have switched to at once. Every load allocates.
	port_sched_point();
	PORT_PROF_SCOPE(PP_HEAP);
#endif
	void* memory = nullptr;
	if (heap) {
		memory = heap->alloc(byteCount, padding);
	} else if (sCurrentHeap) {
		memory = sCurrentHeap->alloc(byteCount, padding);
	}
	return memory;
}

void JKRHeap::free(void* memory, JKRHeap* heap)
{
#ifdef SMS_PORT
	PORT_PROF_SCOPE(PP_HEAP); // w14
#endif
	if (heap != nullptr || (heap = findFromRoot(memory)) != nullptr)
		heap->free(memory);
}

void JKRHeap::freeAll()
{
	JUT_WARNING_F(!mInitFlag, "freeAll in heap %x", this);
	JSUListIterator<JKRDisposer> iterator(&mDisposerList);
	while (iterator = mDisposerList.getFirst(),
	       iterator != mDisposerList.getEnd()) {
		iterator->~JKRDisposer();
	}
}

s32 JKRHeap::resize(void* ptr, u32 size, JKRHeap* heap)
{
	if (!heap) {
		heap = findFromRoot(ptr);
		if (!heap) {
			return -1;
		}
	}
	return heap->resize(ptr, size);
}

s32 JKRHeap::getSize(void* ptr, JKRHeap* heap)
{
	if (!heap) {
		heap = findFromRoot(ptr);
		if (!heap) {
			return -1;
		}
	}
	return heap->getSize(ptr);
}

JKRHeap* JKRHeap::findFromRoot(void* ptr)
{
	if (sRootHeap != nullptr)
		return sRootHeap->find(ptr);

	return nullptr;
}

JKRHeap* JKRHeap::find(void* memory) const
{
	if ((mStart <= memory) && (memory <= mEnd)) {
		if (mChildTree.getNumChildren() != 0) {
			for (JSUTreeIterator<JKRHeap> iterator(mChildTree.getFirstChild());
			     iterator != mChildTree.getEndChild(); ++iterator) {
				JKRHeap* result = iterator->find(memory);
				if (result) {
					return result;
				}
			}
		}
		return const_cast<JKRHeap*>(this);
	}
	return nullptr;
}

void JKRHeap::dispose_subroutine(uintptr_t begin, uintptr_t end)
{
	JSUListIterator<JKRDisposer> last_iterator;
	JSUListIterator<JKRDisposer> next_iterator;
	JSUListIterator<JKRDisposer> iterator;
	for (iterator = mDisposerList.getFirst();
	     iterator != mDisposerList.getEnd(); iterator = next_iterator) {
		JKRDisposer* disposer = iterator.getObject();

		if ((void*)begin <= disposer && disposer < (void*)end) {
			iterator->~JKRDisposer();
			if (last_iterator
			    == JSUListIterator<JKRDisposer>(
			        (JSULink<JKRDisposer>*)nullptr)) {
				next_iterator = mDisposerList.getFirst();
				continue;
			}

			next_iterator = last_iterator;
			next_iterator++;
			continue;
		}

		last_iterator = iterator;
		next_iterator = iterator;
		next_iterator++;
	}
}

bool JKRHeap::dispose(void* memory, u32 size)
{
	uintptr_t begin = (uintptr_t)memory;
	uintptr_t end   = (uintptr_t)memory + size;
	dispose_subroutine(begin, end);
	return false;
}

void JKRHeap::dispose(void* begin, void* end)
{
	dispose_subroutine((uintptr_t)begin, (uintptr_t)end);
}

void JKRHeap::dispose()
{
	JSUListIterator<JKRDisposer> iterator(&mDisposerList);
	while (iterator = mDisposerList.getFirst(),
	       iterator != mDisposerList.getEnd()) {
		iterator->~JKRDisposer();
	}
}

void JKRHeap::copyMemory(void* dst, void* src, u32 size)
{
	u32 count = (size + 3) / 4;

	u32* dst_32 = (u32*)dst;
	u32* src_32 = (u32*)src;
	while (count-- > 0)
		*dst_32++ = *src_32++;
}

void JKRDefaultMemoryErrorRoutine(void* heap, u32 size, int alignment)
{
#ifdef SMS_PORT
	OSReport("[port] JKRHeap alloc failure: heap %p size 0x%x align %d (free 0x%x, max "
	         "block 0x%x)\n",
	         heap, size, alignment, ((JKRHeap*)heap)->getTotalFreeSize(),
	         ((JKRHeap*)heap)->getFreeSize());
#endif
	OSErrorLine(694, "abort\n");
}

bool JKRHeap::setErrorFlag(bool errorFlag)
{
	bool prev  = mErrorFlag;
	mErrorFlag = errorFlag;
	return prev;
}

JKRHeapErrorHandler* JKRHeap::setErrorHandler(JKRHeapErrorHandler* errorHandler)
{
	JKRHeapErrorHandler* prev = mErrorHandler;
	if (!errorHandler) {
		errorHandler = JKRDefaultMemoryErrorRoutine;
	}
	mErrorHandler = errorHandler;
	return prev;
}

void JKRHeap::fillMemory(u8* dst, u32 size, u8 value)
{
	JUT_ASSERT_F(false, "UNIMPLEMENTED");
}

#ifdef SMS_PORT
// On the PC these replace the global new/delete of the whole executable, so
// host code (the SDK layer, the C++ runtime) goes through them too.
//  - Before the root heap exists, or inside a port_host_alloc_push/pop scope
//    (host-side tables that must not live in, or die with, a game heap),
//    new falls back to the CRT heap (16-aligned on x64).
//  - delete routes by address: game RAM (the arena JKRHeap::initArena took,
//    mapped below 4 GB) goes to the JKRHeap that owns it, exactly as on the
//    GameCube (a pointer no heap owns is ignored); anything else came from the
//    CRT fallback and is returned there.
// The JKR heaps raise every alignment below 16 to 16 themselves.
#include <cstdio>
static thread_local int sPortHostAllocDepth;

extern "C" void port_host_alloc_push(void) { ++sPortHostAllocDepth; }
extern "C" void port_host_alloc_pop(void) { --sPortHostAllocDepth; }

static void* portNew(size_t byteCount, int alignment, JKRHeap* heap)
{
	if (heap == nullptr
	    && (JKRHeap::getCurrentHeap() == nullptr || sPortHostAllocDepth > 0)) {
		return std::malloc(byteCount != 0 ? byteCount : 1);
	}
	void* p = JKRHeap::alloc(byteCount, alignment, heap);
	if (p == nullptr) {
		// The GameCube would carry on with a null object (MWCC checks the
		// result of new); clang does not, so report the failure here.
		JKRHeap* h = heap ? heap : JKRHeap::getCurrentHeap();
		std::fprintf(stderr,
		             "[port] new: %zu bytes (align %d) failed in heap %p "
		             "(free %u, max block %u)\n",
		             byteCount, alignment, (void*)h, h ? (u32)h->getTotalFreeSize() : 0,
		             h ? (u32)h->getFreeSize() : 0);
	}
	return p;
}

static void portDelete(void* memory)
{
	if (memory == nullptr)
		return;
	if (JKRHeap::getCodeStart() <= memory && memory < JKRHeap::getUserRamEnd()) {
		JKRHeap::free(memory, nullptr);
	} else {
		std::free(memory);
	}
}

void* operator new(size_t byteCount) { return portNew(byteCount, 4, nullptr); }
void* operator new(size_t byteCount, int alignment)
{
	return portNew(byteCount, alignment, nullptr);
}
void* operator new(size_t byteCount, JKRHeap* heap, int alignment)
{
	return portNew(byteCount, alignment, heap);
}

void* operator new[](size_t byteCount) { return portNew(byteCount, 4, nullptr); }
void* operator new[](size_t byteCount, int alignment)
{
	return portNew(byteCount, alignment, nullptr);
}
void* operator new[](size_t byteCount, JKRHeap* heap, int alignment)
{
	return portNew(byteCount, alignment, heap);
}

void operator delete(void* memory) { portDelete(memory); }
void operator delete[](void* memory) { portDelete(memory); }
// The sized forms (C++14) would otherwise come from the CRT; route them here
// explicitly instead of relying on the CRT forwarding to the unsized ones.
void operator delete(void* memory, size_t) { portDelete(memory); }
void operator delete[](void* memory, size_t) { portDelete(memory); }
#else
void* operator new(size_t byteCount)
{
	return JKRHeap::alloc(byteCount, 4, nullptr);
}
void* operator new(size_t byteCount, int alignment)
{
	return JKRHeap::alloc(byteCount, alignment, nullptr);
}
void* operator new(size_t byteCount, JKRHeap* heap, int alignment)
{
	return JKRHeap::alloc(byteCount, alignment, heap);
}

void* operator new[](size_t byteCount)
{
	return JKRHeap::alloc(byteCount, 4, nullptr);
}
void* operator new[](size_t byteCount, int alignment)
{
	return JKRHeap::alloc(byteCount, alignment, nullptr);
}
void* operator new[](size_t byteCount, JKRHeap* heap, int alignment)
{
	return JKRHeap::alloc(byteCount, alignment, heap);
}

// this is not needed without the other pragma and asm bs
void operator delete(void* memory) { JKRHeap::free(memory, nullptr); }
void operator delete[](void* memory) { JKRHeap::free(memory, nullptr); }
#endif

// NOTE: all TState related stuff is guesswork mostly.

JKRHeap::TState::TState(const JKRHeap* heap, bool isCompareOnDestructed)
    : mHeap(heap)
    , mId(0xFFFFFFFF)
    , mIsCompareOnDestructed(isCompareOnDestructed)
{
	mHeap->state_register(this, mId);
}

JKRHeap::TState::TState(const JKRHeap* heap, u32 id, bool isCompareOnDestructed)
    : mHeap(heap)
    , mId(id)
    , mIsCompareOnDestructed(isCompareOnDestructed)
{
	mHeap->state_register(this, mId);
}

JKRHeap::TState::~TState() { JUT_ASSERT_F(false, "UNIMPLEMENTED"); }

void JKRHeap::state_register(JKRHeap::TState* p, u32) const
{
	JUT_ASSERT(p != 0);
	JUT_ASSERT(p->getHeap() == this);
}

bool JKRHeap::state_compare(const JKRHeap::TState& r1,
                            const JKRHeap::TState& r2) const
{
	JUT_ASSERT(r1.getHeap() == r2.getHeap());
	return r1.getCheckCode() == r2.getCheckCode();
}

void JKRHeap::state_dumpDifference(const TState& r1, const TState& r2) { }

void JKRHeap::state_dump(const TState& state) const
{
	JUT_LOG_F("check-code : 0x%08x", state.getCheckCode());
	JUT_LOG_F("id         : 0x%08x", state.getId());
	JUT_LOG_F("used size  : %u", state.getUsedSize());
}
