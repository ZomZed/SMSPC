// Host-side runtime pieces of the PC port that are not part of any SDK
// replacement yet: the missing-symbol logger used by stubs_auto.cpp.
#include "port_stubs.h"

#include <cstdio>
#include <cstring>

namespace {
const int kMaxSeen = 4096;
const char* sSeen[kMaxSeen];
int sSeenNum;
} // namespace

extern "C" int port_missing_stub(const char* name)
{
	for (int i = 0; i < sSeenNum; ++i)
		if (sSeen[i] == name)
			return 0;
	if (sSeenNum < kMaxSeen)
		sSeen[sSeenNum++] = name;
	std::fprintf(stderr, "[port] missing: %s\n", name);
	return 0;
}
