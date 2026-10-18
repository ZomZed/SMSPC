/*
 * port_asset_swap.h - big-endian -> native swap pass for the .pad input
 * replays, the last unswapped in-place format found by the disc inventory
 * (worker w10, port/tools/disc_inventory.py; see port/ENDIAN.md "Disc format
 * inventory"). (.bcr BCSV tables are swapped by Koga::ToolData::Attach itself,
 * w7's fix in src/MarioUtil/ToolData.cpp.)
 *
 *   port_swap_pad(file)    .pad Mario input replays (TMarioInputReplay::init)
 *
 * Idempotent and self-contained C so host tools can include it.
 * The format stores no pointers, so it is pointer-size clean.
 *
 * .pad layout ("MARIO RECORDv0.2", 381 files in 20 scene archives, read by
 * TEnemyMario: the Shadow Mario chases, the dolpic1 run-away/gate replays):
 *   0x00 char magic[16]
 *   0x10 s32 frame count (replay length)
 *   0x14 s32 offsets[10]: (durations, values) for 5 channels:
 *        stick magnitude f32, stick yaw s16, buttons u16, two u8 channels
 *   Each durations table is u32[n]; its value table holds n entries. The
 *   durations of every channel add up to the frame count (checked on all files),
 *   which is how n is found; n is also clamped to the next table's offset.
 *   Marker: offsets[0] is 0x40 in every file, so it reads 0x40000000 through a
 *   native u32 while the file is still big-endian.
 */
#ifndef PORT_ASSET_SWAP_H
#define PORT_ASSET_SWAP_H

#include "port_endian.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_PAD_FIRST_TABLE 0x40u

static inline int port_pad_is_swapped(const void* file)
{
	return ((const unsigned int*)((const unsigned char*)file + 0x14))[0]
	       == PORT_PAD_FIRST_TABLE;
}

/* Returns 1 if it swapped, 0 if the file already was native (or is null). */
static inline int port_swap_pad(void* file)
{
	static const int valueSize[5] = { 4, 2, 2, 1, 1 };
	unsigned char* b = (unsigned char*)file;
	unsigned int* off;
	unsigned int length, ch, i, k;
	if (!file || port_pad_is_swapped(file))
		return 0;
	for (i = 0; i < 11; ++i)
		sms_swap32_inplace(b + 0x10 + i * 4);
	length = *(unsigned int*)(b + 0x10);
	off    = (unsigned int*)(b + 0x14);
	for (ch = 0; ch < 5; ++ch) {
		unsigned int durOff = off[ch * 2], valOff = off[ch * 2 + 1];
		unsigned int limit = 0xFFFFFFFFu, n = 0, sum = 0;
		/* the table ends where the next one (by offset) starts */
		for (k = 0; k < 10; ++k)
			if (off[k] > durOff && off[k] < limit)
				limit = off[k];
		while (sum < length && (limit == 0xFFFFFFFFu || durOff + n * 4 + 4 <= limit)) {
			unsigned int* d = (unsigned int*)(b + durOff) + n;
			sms_swap32_inplace(d);
			sum += *d;
			++n;
		}
		if (valueSize[ch] > 1)
			for (i = 0; i < n; ++i)
				sms_swap_value(b + valOff + i * valueSize[ch], valueSize[ch]);
	}
	return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* PORT_ASSET_SWAP_H */
