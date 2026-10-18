#include <MarioUtil/ToolData.hpp>
#include <macros.h>
#ifdef SMS_PORT
#include <string.h>
#endif

namespace Koga {
ToolData::ToolData() { mData = nullptr; }
ToolData::~ToolData() { }

#ifdef SMS_PORT
// The BCSV (.bcr) file is big-endian and read in place. Swap it to native order
// once: header, field table, and the numeric cells of every entry (strings are
// left alone; each cell offset is swapped once even if several fields share it).
// Idempotent: a field count of 0x10000 or more means it is still big-endian.
static u32 toolDataSwap32(u32 v)
{
	return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24);
}

static void toolDataSwapToNative(void* jmapData)
{
	u32* header = (u32*)jmapData;
	if (header[1] < 0x10000)
		return;
	for (int i = 0; i < 4; ++i)
		header[i] = toolDataSwap32(header[i]);
	ToolData::JMapItem* items
	    = (ToolData::JMapItem*)((u8*)jmapData + 4 * sizeof(u32));
	const s32 numFields = (s32)header[1];
	for (s32 i = 0; i < numFields; ++i) {
		items[i].mHash     = toolDataSwap32(items[i].mHash);
		items[i].mMask     = toolDataSwap32(items[i].mMask);
		items[i].mOffsData = (u16)((items[i].mOffsData >> 8) | (items[i].mOffsData << 8));
	}
	const s32 numEntries = (s32)header[0];
	u8* data             = (u8*)jmapData + (s32)header[2];
	for (s32 e = 0; e < numEntries; ++e) {
		u8* entry = data + e * header[3];
		for (s32 i = 0; i < numFields; ++i) {
			bool seen = false;
			for (s32 j = 0; j < i; ++j)
				if (items[j].mOffsData == items[i].mOffsData)
					seen = true;
			if (seen)
				continue;
			u8* cell = entry + items[i].mOffsData;
			switch (items[i].mType) {
			case JMAP_VALUE_TYPE_LONG:
			case JMAP_VALUE_TYPE_FLOAT:
			case JMAP_VALUE_TYPE_LONG_2:
			case JMAP_VALUE_TYPE_STRING_PTR: {
				u32 v;
				memcpy(&v, cell, 4);
				v = toolDataSwap32(v);
				memcpy(cell, &v, 4);
			} break;
			case JMAP_VALUE_TYPE_SHORT: {
				u8 t    = cell[0];
				cell[0] = cell[1];
				cell[1] = t;
			} break;
			}
		}
	}
}
#endif

BOOL ToolData::Attach(const void* jmapData)
{
	if (!jmapData)
		return FALSE;
#ifdef SMS_PORT
	toolDataSwapToNative((void*)jmapData);
#endif
	this->mData = (JMapData*)jmapData;
	return TRUE;
}

BOOL ToolData::GetValue(int entryIndex, const char* key, s32& pValueOut) const
{
	s32 itemIndex = searchItemInfo(key);
	if (itemIndex < 0) {
		return FALSE;
	}
	return getValue(entryIndex, itemIndex, &pValueOut);
}

BOOL ToolData::GetValue(int entryIndex, const char* key,
                        const char*& pValueOut) const
{
	s32 itemIndex = searchItemInfo(key);
	if (itemIndex < 0) {
		return FALSE;
	}
	return getValue(entryIndex, itemIndex, &pValueOut);
}
} // namespace Koga
