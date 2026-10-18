#include <GC2D/MessageLoader.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JSupport/JSUMemoryInputStream.hpp>

TMessageLoader::TMessageLoader()
    : unk0(0)
    , unk4(0)
{
}

TMessageLoader::TMessageLoader(const char* param_1)
    : unk0(0)
    , unk4(0)
{
	u8* res = (u8*)JKRGetResource(param_1);
	if (res) {
		u32 a;
		u32 b;
		readHeader(&a, &b, res);
		unk4 = parseBlock(a, b, res + 0x20);
		// NOTE: assert but in an if?
		if (unk4)
			(void)unk4;
	}
}

u32 TMessageLoader::loadMessageData(const char* param_1)
{
	u8* res = (u8*)JKRGetResource(param_1);
	if (!res)
		return -1;

#ifdef SMS_PORT
	// BMG stays big-endian in memory (stream readers need it): decode the
	// header words explicitly.
	unk4 = parseBlock((int)sms_be32(res + 0x8) * 32, (int)sms_be32(res + 0xC),
	                  res + 0x20);
#else
	unk4 = parseBlock(*(int*)(res + 0x8) * 32, *(int*)(res + 0xC), res + 0x20);
#endif
	if (!unk4)
		return -1;

	return unk2;
}

void TMessageLoader::readHeader(u32* a, u32* b, void* header)
{
#ifdef SMS_PORT
	*a = sms_be32((u8*)header + 8) * 32;
	*b = sms_be32((u8*)header + 12);
#else
	u32* casted = (u32*)header;

	*a = *(casted + 2) * 32;
	*b = *(casted + 3);
#endif
}

void* TMessageLoader::parseBlock(u32 param_1, u32 param_2, void* param_3)
{
	JSUMemoryInputStream local_5c(param_3, param_1);

	void* result;

	for (int i = 0; i < param_2; ++i) {
		int local_74;
#ifdef SMS_PORT
		local_74 = local_5c.readS32(); // typed read: big-endian block magic
#else
		local_5c.read(&local_74, 4);
#endif

		int local_70;

		switch (local_74) {
		case 'INF1': {
			local_70 = readInfoBlock(local_5c.getCurrent());
			local_5c.skip(4);
			break;
		}

		case 'DAT1':
#ifdef SMS_PORT
			local_70 = local_5c.readS32();
#else
			local_5c.read(&local_70, 4);
#endif
			result = local_5c.getCurrent();
			break;

		case 'STR1':
			local_70 = 0;
			break;

		default:
			local_70 = 0;
			break;
		}

		local_5c.skip(local_70 - 8);
	}

	return result;
}

TMessageLoader::EntryInfo* TMessageLoader::getMessageEntry(u32 param_1)
{
	EntryInfo* result;
	if (u16(param_1) >= unk0)
		result = nullptr;
	else
		result = &unk8[param_1];

	return result;
}

int TMessageLoader::readInfoBlock(void* data)
{
#ifdef SMS_PORT
	int length = (int)sms_be32(data);
#else
	int length = *(int*)data;
#endif
	data = (u8*)data + 4;
	JSUMemoryInputStream local_38(data, length - 8);
	local_38.read(&unk0, 2);
#ifdef SMS_PORT
	sms_swap16_inplace(&unk0); // entry count
#endif
	local_38.readU16();
	unk2 = local_38.readU16();
	local_38.skip(2);

	for (int i = 0; i < unk0; ++i) {
		local_38.read(&unk8[i], sizeof(EntryInfo));
#ifdef SMS_PORT
		// INF1 entry: u32 text offset, s16 attributes x2, 4 raw bytes
		sms_swap32_inplace(&unk8[i].unk0);
		sms_swap16_inplace(&unk8[i].unk4);
		sms_swap16_inplace(&unk8[i].unk6);
#endif
	}

	return length;
}
