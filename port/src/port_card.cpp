// Memory card for the PC port (Aurora build).
//
// The CARD API is Aurora's (lib/dolphin/card.cpp): a "GCI folder" card, one .gci
// file per save, under <user dir>/USA/Card A/ (user dir = Aurora's userPath, by
// default SDL's pref path %APPDATA%\Super Mario Sunshine\; SMS_PORT_USER_DIR
// overrides it, see port_main.cpp). The game's save is
// 01-GMSE-super_mario_sunshine.gci: a 0x40-byte GCI directory entry followed by
// the 0xE000-byte file (7 sectors of 0x2000: options/header, then 3 save slots
// x 2 alternating copies), stored exactly as the game writes it.
//
// SMS_PORT_CARDTEST=1 makes port_main.cpp run port_card_test() instead of the
// game: it drives the game's own TCardManager (worker thread, mount/probe,
// create, option block, save-slot blocks with the alternating write counts and
// checksums) against the real card, and checks that what was written reads back,
// also across runs (each run reports the previous run's marker).

#ifdef SMS_PORT_AURORA

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <aurora/card.h>
#include <dolphin/card.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JSupport/JSUMemoryInputStream.hpp>
#include <JSystem/JSupport/JSUMemoryOutputStream.hpp>
#include <System/CardManager.hpp>
#include <System/FlagManager.hpp>

namespace {

const s32 kBusy = CARD_RESULT_BUSY;

s32 waitCard(TCardManager* cm, const char* what)
{
	s32 r;
	int spins = 0;
	while ((r = cm->getLastStatus()) == kBusy) {
		VIWaitForRetrace(); // sleeps this game thread; the card worker runs
		if (++spins > 600) {
			std::fprintf(stderr, "[cardtest] %s: timed out\n", what);
			return kBusy;
		}
	}
	std::fprintf(stderr, "[cardtest] %-22s -> %d\n", what, r);
	return r;
}

const char kMarker[] = "SMSPORT-CARDTEST";
const u32 kMarkerOfs = 0x1F00; // past the flag data TFlagManager::save writes

} // namespace

// TCardManager::unmount_ calls this instead of CARDUnmount (CardManager.cpp,
// SMS_PORT_AURORA). Aurora's CARDUnmount is a stub that returns NOCARD; the SDK
// returns READY when the card was mounted, and TCardLoad reads that status back
// (getLastStatus after unmount), so a NOCARD here would look like a pulled card.
extern "C" s32 port_CARDUnmount(s32 chan)
{
	s32 r = CARDUnmount(chan);
	if (r == CARD_RESULT_NOCARD && (chan == 0 || chan == 1) && CARDProbe(chan))
		r = CARD_RESULT_READY;
	return r;
}

int port_card_test()
{
	std::fprintf(stderr, "[cardtest] start\n");
	JKRExpHeap::createRoot(1, false);
	JKRExpHeap::create(0x20000, JKRGetRootHeap(), false)->becomeSystemHeap();
	JKRGetRootHeap()->becomeCurrentHeap();
	TFlagManager::start(JKRGetRootHeap());

	CARDInit();

	static u8 sector[0x2000] __attribute__((aligned(32)));
	static u8 workArea[0xA000] __attribute__((aligned(32)));
	static u8 stack[0x4000] __attribute__((aligned(32)));
	static u8 icons[0xA00], banner[0xE00];
	TCardManager* cm = new TCardManager(sector, workArea, 0, 14, stack, sizeof(stack));
	cm->mIcons  = icons;
	cm->mBanner = banner;

	int fails = 0;
	s32 r = cm->probe();
	std::fprintf(stderr, "[cardtest] %-22s -> %d\n", "probe", r);
	if (r != CARD_RESULT_READY)
		++fails;

	cm->readOptionBlock();
	r = waitCard(cm, "readOptionBlock");
	if (r == CARD_RESULT_NOFILE) {
		cm->createFile();
		r = waitCard(cm, "createFile");
		if (r != CARD_RESULT_READY)
			++fails;
		cm->readOptionBlock();
		r = waitCard(cm, "readOptionBlock (new)");
	}
	if (r != CARD_RESULT_READY)
		++fails;

	TCardBookmarkInfo marks[3];
	std::memset(marks, 0, sizeof(marks));
	cm->getBookmarkInfos(marks);
	if (waitCard(cm, "getBookmarkInfos") != CARD_RESULT_READY)
		++fails;
	for (int i = 0; i < 3; ++i)
		std::fprintf(stderr, "[cardtest]   slot %d: state %d, shines %u, last saved %llu\n", i, marks[i].unk0,
		             marks[i].unk4, (unsigned long long)marks[i].unk8);

	// The previous run's marker, if any (slot 2).
	cm->readBlock(2);
	if (waitCard(cm, "readBlock(2)") == CARD_RESULT_READY) {
		JSUMemoryInputStream in;
		cm->getReadStream(&in);
		const char* data = (const char*)in.mBuffer;
		if (std::memcmp(data + kMarkerOfs, kMarker, sizeof(kMarker) - 1) == 0)
			std::fprintf(stderr, "[cardtest]   previous run's marker: %.48s\n", data + kMarkerOfs);
		else
			std::fprintf(stderr, "[cardtest]   no previous marker in slot 2\n");
	}

	// Write slot 2 twice (the two alternating copies), with a timestamped marker.
	static u8 written[0x1FF8];
	for (int pass = 0; pass < 2; ++pass) {
		JSUMemoryOutputStream out;
		cm->getWriteStream(&out);
		TFlagManager::getInstance()->save(out);
		char* data = (char*)out.mBuffer;
		char stamp[64];
		std::snprintf(stamp, sizeof(stamp), "%s %lld pass %d", kMarker, (long long)std::time(nullptr), pass);
		std::memcpy(data + kMarkerOfs, stamp, std::strlen(stamp) + 1);
		std::memcpy(written, data, sizeof(written));
		cm->writeBlock(2);
		if (waitCard(cm, pass ? "writeBlock(2) #2" : "writeBlock(2) #1") != CARD_RESULT_READY)
			++fails;
	}

	// Remount from scratch and read it back.
	std::fprintf(stderr, "[cardtest] %-22s -> %d\n", "unmount", cm->unmount());
	cm->readBlock(2);
	if (waitCard(cm, "readBlock(2) back") != CARD_RESULT_READY) {
		++fails;
	} else {
		JSUMemoryInputStream in;
		cm->getReadStream(&in);
		bool same = std::memcmp(in.mBuffer, written, sizeof(written)) == 0;
		std::fprintf(stderr, "[cardtest]   read-back %s: %.48s\n", same ? "matches" : "DIFFERS",
		             (const char*)in.mBuffer + kMarkerOfs);
		if (!same)
			++fails;
		TFlagManager::getInstance()->load(in);
	}

	// Option block round trip.
	{
		JSUMemoryOutputStream out;
		cm->getOptionWriteStream(&out);
		TFlagManager::getInstance()->saveOption(out);
		cm->writeOptionBlock();
		if (waitCard(cm, "writeOptionBlock") != CARD_RESULT_READY)
			++fails;
		cm->readOptionBlock();
		if (waitCard(cm, "readOptionBlock back") != CARD_RESULT_READY)
			++fails;
	}

	cm->getBookmarkInfos(marks);
	waitCard(cm, "getBookmarkInfos");
	for (int i = 0; i < 3; ++i)
		std::fprintf(stderr, "[cardtest]   slot %d: state %d, shines %u\n", i, marks[i].unk0, marks[i].unk4);

	char path[512];
	if (aurora_card_get_path("GMSE", AURORA_CARD_GCI_DIRECTORY, 0, path, sizeof(path)))
		std::fprintf(stderr, "[cardtest] card folder: %s\n", path);
	std::fprintf(stderr, "[cardtest] %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
	return fails;
}

#endif // SMS_PORT_AURORA
