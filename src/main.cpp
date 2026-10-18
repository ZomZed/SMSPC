#include <System/Application.hpp>
#ifdef SMS_PORT_ASAN
#include <JSystem/JKernel/JKRHeap.hpp>
#include <stdlib.h>
#endif

// BSS
TApplication gpApplication;

#ifdef SMS_PORT_ASAN
// build_aurora.py --asan: SMS_PORT_ASAN_SELFTEST=1 checks that the JKR heap
// poisoning works (port/include/port_asan.h): it reads one byte past a 20-byte
// game allocation, which must produce a heap-buffer-overflow / use-after-poison
// report, then frees the block and reads it again (use-after-poison).
static void portAsanSelfTest()
{
	if (!getenv("SMS_PORT_ASAN_SELFTEST"))
		return;
	volatile char* p = (volatile char*)JKRHeap::alloc(20, 0, nullptr);
	OSReport("[port] ASan self-test: reading %p[20] (expect a report)\n", p);
	(void)p[20];
	JKRHeap::free((void*)p, nullptr);
	(void)p[0];
}
#endif

#ifdef SMS_PORT
int main(void)
#else
void main(void)
#endif
{
	SMSGetApplication()->initialize();
#ifdef SMS_PORT_ASAN
	portAsanSelfTest();
#endif
	SMSGetApplication()->proc();
	SMSGetApplication()->finalize();
}
