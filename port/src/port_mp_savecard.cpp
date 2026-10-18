// In-memory memory card for multiplayer sessions (w10). See port/MULTIPLAYER.md "Free mode".
//
// TCardManager's CARD calls are renamed to these in CardManager.cpp (SMS_PORT_AURORA). While
// a multiplayer save is active (port_mp_block_card_writes), they emulate a formatted card
// holding at most the game's one file, in memory: create, open, read, write, status and free
// space behave like an empty GameCube card, so every save prompt (shine get, pause menu, the
// ending) completes normally ("File created.", "Saved."), and the player's real memory card is
// never read or written. Outside a session every call goes to the real CARD API unchanged.
// The emulated card is emptied when a session's save starts (port_mp_savecard_reset).
#ifdef SMS_PORT_AURORA

#include <dolphin/card.h>

#include <cstring>
#include <mutex>
#include <vector>

extern "C" int port_mp_block_card_writes(void); // port_mp_game.cpp
extern "C" s32 port_CARDUnmount(s32 chan);      // port_card.cpp (w5)

namespace {

std::mutex sMu;
bool sExists;
char sName[CARD_FILENAME_MAX + 1];
std::vector<u8> sData;
CARDStat sStat;
const s32 kCardBytes = 0x80000 - 5 * 0x2000; // a 59-block card's user area

bool emu() { return port_mp_block_card_writes() != 0; }

bool nameIs(const char* n) { return sExists && std::strncmp(sName, n, CARD_FILENAME_MAX) == 0; }

void fill(s32 chan, CARDFileInfo* fi)
{
	std::memset(fi, 0, sizeof(*fi));
	fi->chan   = chan;
	fi->fileNo = 0;
	fi->offset = 0;
	fi->length = (s32)sData.size();
}

} // namespace

extern "C" {

void port_mp_savecard_reset(void)
{
	std::lock_guard<std::mutex> lk(sMu);
	sExists = false;
	sData.clear();
	std::memset(&sStat, 0, sizeof(sStat));
}

s32 port_mp_CARDProbeEx(s32 chan, s32* memSize, s32* sectorSize)
{
	if (!emu())
		return CARDProbeEx(chan, memSize, sectorSize);
	if (memSize != nullptr)
		*memSize = 4; // Mbit: the standard 59-block card
	if (sectorSize != nullptr)
		*sectorSize = 0x2000;
	return CARD_RESULT_READY;
}

s32 port_mp_CARDMount(s32 chan, void* workArea, CARDCallback detachCallback)
{
	return emu() ? CARD_RESULT_READY : CARDMount(chan, workArea, detachCallback);
}

s32 port_mp_CARDCheck(s32 chan) { return emu() ? CARD_RESULT_READY : CARDCheck(chan); }

s32 port_mp_CARDUnmount(s32 chan) { return emu() ? CARD_RESULT_READY : port_CARDUnmount(chan); }

s32 port_mp_CARDFormat(s32 chan)
{
	if (!emu())
		return CARDFormat(chan);
	port_mp_savecard_reset();
	return CARD_RESULT_READY;
}

s32 port_mp_CARDCreate(s32 chan, const char* fileName, u32 size, CARDFileInfo* fileInfo)
{
	if (!emu())
		return CARDCreate(chan, fileName, size, fileInfo);
	std::lock_guard<std::mutex> lk(sMu);
	if (sExists)
		return nameIs(fileName) ? CARD_RESULT_EXIST : CARD_RESULT_NOENT; // one file only
	if ((s32)size > kCardBytes)
		return CARD_RESULT_INSSPACE;
	sExists = true;
	std::strncpy(sName, fileName, CARD_FILENAME_MAX);
	sName[CARD_FILENAME_MAX] = '\0';
	sData.assign(size, 0);
	std::memset(&sStat, 0, sizeof(sStat));
	std::strncpy(sStat.fileName, fileName, CARD_FILENAME_MAX);
	sStat.length      = size;
	sStat.iconAddr    = 0xFFFFFFFF; // as a fresh file: TCardManager sets its status next
	sStat.commentAddr = 0xFFFFFFFF;
	fill(chan, fileInfo);
	return CARD_RESULT_READY;
}

s32 port_mp_CARDOpen(s32 chan, const char* fileName, CARDFileInfo* fileInfo)
{
	if (!emu())
		return CARDOpen(chan, fileName, fileInfo);
	std::lock_guard<std::mutex> lk(sMu);
	if (!nameIs(fileName))
		return CARD_RESULT_NOFILE;
	fill(chan, fileInfo);
	return CARD_RESULT_READY;
}

s32 port_mp_CARDClose(CARDFileInfo* fileInfo)
{
	if (!emu())
		return CARDClose(fileInfo);
	return CARD_RESULT_READY;
}

s32 port_mp_CARDRead(const CARDFileInfo* fileInfo, void* addr, s32 length, s32 offset)
{
	if (!emu())
		return CARDRead(fileInfo, addr, length, offset);
	std::lock_guard<std::mutex> lk(sMu);
	if (!sExists || offset < 0 || length < 0 || offset + length > (s32)sData.size())
		return CARD_RESULT_LIMIT;
	std::memcpy(addr, sData.data() + offset, length);
	return CARD_RESULT_READY;
}

s32 port_mp_CARDWrite(const CARDFileInfo* fileInfo, const void* addr, s32 length, s32 offset)
{
	if (!emu())
		return CARDWrite(fileInfo, addr, length, offset);
	std::lock_guard<std::mutex> lk(sMu);
	if (!sExists || offset < 0 || length < 0 || offset + length > (s32)sData.size())
		return CARD_RESULT_LIMIT;
	std::memcpy(sData.data() + offset, addr, length);
	return CARD_RESULT_READY;
}

s32 port_mp_CARDGetStatus(s32 chan, s32 fileNo, CARDStat* stat)
{
	if (!emu())
		return CARDGetStatus(chan, fileNo, stat);
	std::lock_guard<std::mutex> lk(sMu);
	if (!sExists || fileNo != 0)
		return CARD_RESULT_NOFILE;
	*stat = sStat;
	return CARD_RESULT_READY;
}

s32 port_mp_CARDSetStatus(s32 chan, s32 fileNo, const CARDStat* stat)
{
	if (!emu())
		return CARDSetStatus(chan, fileNo, stat);
	std::lock_guard<std::mutex> lk(sMu);
	if (!sExists || fileNo != 0)
		return CARD_RESULT_NOFILE;
	sStat = *stat;
	return CARD_RESULT_READY;
}

s32 port_mp_CARDFreeBlocks(s32 chan, s32* byteNotUsed, s32* filesNotUsed)
{
	if (!emu())
		return CARDFreeBlocks(chan, byteNotUsed, filesNotUsed);
	std::lock_guard<std::mutex> lk(sMu);
	if (byteNotUsed != nullptr)
		*byteNotUsed = kCardBytes - (sExists ? (s32)sData.size() : 0);
	if (filesNotUsed != nullptr)
		*filesNotUsed = sExists ? 126 : 127;
	return CARD_RESULT_READY;
}

} // extern "C"

#endif // SMS_PORT_AURORA
