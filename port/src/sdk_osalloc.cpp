// OSAlloc heap API (same first-fit algorithm and 32-byte cell header as the
// SDK's OSAlloc.c) plus the IPL ROM font queries (no ROM font on the PC: an
// empty ANSI font with plausible metrics is returned).
#include "port_sdk.h"

#include <dolphin/os.h>

#include <cstdio>
#include <cstring>

namespace {

struct Cell {
	Cell* prev;
	Cell* next;
	long size;
};

struct HeapDesc {
	long size;
	Cell* free;
	Cell* allocated;
};

HeapDesc* HeapArray;
int NumHeaps;
void* ArenaStart;
void* ArenaEnd;

Cell* DLAddFront(Cell* list, Cell* cell)
{
	cell->next = list;
	cell->prev = nullptr;
	if (list)
		list->prev = cell;
	return cell;
}

Cell* DLLookup(Cell* list, Cell* cell)
{
	for (; list; list = list->next)
		if (list == cell)
			return list;
	return nullptr;
}

Cell* DLExtract(Cell* list, Cell* cell)
{
	if (cell->next)
		cell->next->prev = cell->prev;
	if (!cell->prev)
		return cell->next;
	cell->prev->next = cell->next;
	return list;
}

Cell* DLInsert(Cell* list, Cell* cell)
{
	Cell* prev = nullptr;
	Cell* next = list;
	for (; next; prev = next, next = next->next)
		if (cell <= next)
			break;

	cell->next = next;
	cell->prev = prev;
	if (next) {
		next->prev = cell;
		if ((u8*)cell + cell->size == (u8*)next) {
			cell->size += next->size;
			next       = next->next;
			cell->next = next;
			if (next)
				next->prev = cell;
		}
	}
	if (prev) {
		prev->next = cell;
		if ((u8*)prev + prev->size == (u8*)cell) {
			prev->size += cell->size;
			prev->next = next;
			if (next)
				next->prev = prev;
		}
		return list;
	}
	return cell;
}

inline bool inRange(void* p, void* s, void* e)
{
	return (uintptr_t)s <= (uintptr_t)p && (uintptr_t)p < (uintptr_t)e;
}

} // namespace

extern "C" {

void* OSAllocFromHeap(int heap, unsigned long size)
{
	if (!HeapArray || heap < 0 || heap >= NumHeaps || HeapArray[heap].size < 0
	    || (long)size <= 0)
		return nullptr;
	HeapDesc* hd = &HeapArray[heap];
	size         = (size + 0x20 + 0x1F) & ~0x1Fu;

	Cell* cell;
	for (cell = hd->free; cell; cell = cell->next)
		if ((long)size <= cell->size)
			break;
	if (!cell)
		return nullptr;

	long leftover = cell->size - (long)size;
	if (leftover < 0x40) {
		hd->free = DLExtract(hd->free, cell);
	} else {
		cell->size    = size;
		Cell* newCell = (Cell*)((u8*)cell + size);
		newCell->size = leftover;
		newCell->prev = cell->prev;
		newCell->next = cell->next;
		if (newCell->next)
			newCell->next->prev = newCell;
		if (newCell->prev)
			newCell->prev->next = newCell;
		else
			hd->free = newCell;
	}
	hd->allocated = DLAddFront(hd->allocated, cell);
	return (u8*)cell + 0x20;
}

void OSFreeToHeap(int heap, void* ptr)
{
	if (!HeapArray || heap < 0 || heap >= NumHeaps || !ptr)
		return;
	Cell* cell    = (Cell*)((u8*)ptr - 0x20);
	HeapDesc* hd  = &HeapArray[heap];
	hd->allocated = DLExtract(hd->allocated, cell);
	hd->free      = DLInsert(hd->free, cell);
}

int OSSetCurrentHeap(int heap)
{
	int prev     = __OSCurrHeap;
	__OSCurrHeap = heap;
	return prev;
}

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps)
{
	HeapArray = (HeapDesc*)arenaStart;
	NumHeaps  = maxHeaps;
	for (int i = 0; i < NumHeaps; i++) {
		HeapArray[i].size = -1;
		HeapArray[i].free = HeapArray[i].allocated = nullptr;
	}
	__OSCurrHeap = -1;
	uintptr_t s  = (uintptr_t)(HeapArray + maxHeaps);
	s            = (s + 0x1F) & ~(uintptr_t)0x1F;
	ArenaStart   = (void*)s;
	ArenaEnd     = (void*)((uintptr_t)arenaEnd & ~(uintptr_t)0x1F);
	return ArenaStart;
}

int OSCreateHeap(void* start, void* end)
{
	if (!HeapArray)
		return -1;
	start = (void*)(((uintptr_t)start + 0x1F) & ~(uintptr_t)0x1F);
	end   = (void*)((uintptr_t)end & ~(uintptr_t)0x1F);
	for (int heap = 0; heap < NumHeaps; heap++) {
		HeapDesc* hd = &HeapArray[heap];
		if (hd->size < 0) {
			hd->size      = (long)((uintptr_t)end - (uintptr_t)start);
			Cell* cell    = (Cell*)start;
			cell->prev    = nullptr;
			cell->next    = nullptr;
			cell->size    = hd->size;
			hd->free      = cell;
			hd->allocated = nullptr;
			return heap;
		}
	}
	return -1;
}

void OSDestroyHeap(int heap)
{
	if (HeapArray && heap >= 0 && heap < NumHeaps)
		HeapArray[heap].size = -1;
}

void OSAddToHeap(int heap, void* start, void* end)
{
	HeapDesc* hd = &HeapArray[heap];
	start        = (void*)(((uintptr_t)start + 0x1F) & ~(uintptr_t)0x1F);
	end          = (void*)((uintptr_t)end & ~(uintptr_t)0x1F);
	Cell* cell   = (Cell*)start;
	cell->size   = (long)((uintptr_t)end - (uintptr_t)start);
	hd->size += cell->size;
	hd->free = DLInsert(hd->free, cell);
}

#define ASSERTREPORT(line, cond)                                                                   \
	if (!(cond)) {                                                                                 \
		OSReport("OSCheckHeap: Failed " #cond " in %d", line);                                     \
		return -1;                                                                                 \
	}

long OSCheckHeap(int heap)
{
	long total = 0;
	long free  = 0;
	ASSERTREPORT(0x37D, HeapArray);
	ASSERTREPORT(0x37E, 0 <= heap && heap < NumHeaps);
	HeapDesc* hd = &HeapArray[heap];
	ASSERTREPORT(0x381, 0 <= hd->size);
	ASSERTREPORT(0x383, hd->allocated == nullptr || hd->allocated->prev == nullptr);
	for (Cell* cell = hd->allocated; cell; cell = cell->next) {
		ASSERTREPORT(0x386, inRange(cell, ArenaStart, ArenaEnd));
		ASSERTREPORT(0x387, ((uintptr_t)cell & 31) == 0);
		ASSERTREPORT(0x388, cell->next == nullptr || cell->next->prev == cell);
		ASSERTREPORT(0x389, 64 <= cell->size);
		ASSERTREPORT(0x38A, (cell->size & 31) == 0);
		total += cell->size;
		ASSERTREPORT(0x38D, 0 < total && total <= hd->size);
	}
	ASSERTREPORT(0x395, hd->free == nullptr || hd->free->prev == nullptr);
	for (Cell* cell = hd->free; cell; cell = cell->next) {
		ASSERTREPORT(0x398, inRange(cell, ArenaStart, ArenaEnd));
		ASSERTREPORT(0x399, ((uintptr_t)cell & 31) == 0);
		ASSERTREPORT(0x39A, cell->next == nullptr || cell->next->prev == cell);
		ASSERTREPORT(0x39B, 64 <= cell->size);
		ASSERTREPORT(0x39C, (cell->size & 31) == 0);
		ASSERTREPORT(0x39D, cell->next == nullptr || (char*)cell + cell->size < (char*)cell->next);
		total += cell->size;
		free += cell->size - 32;
		ASSERTREPORT(0x3A1, 0 < total && total <= hd->size);
	}
	ASSERTREPORT(0x3A8, total == hd->size);
	return free;
}

unsigned long OSReferentSize(void* ptr)
{
	Cell* cell = (Cell*)((u8*)ptr - 32);
	return (unsigned long)(cell->size - 32);
}

void OSDumpHeap(int heap)
{
	OSReport("\nOSDumpHeap(%d):\n", heap);
	HeapDesc* hd = &HeapArray[heap];
	if (hd->size < 0) {
		OSReport("--------Inactive\n");
		return;
	}
	OSReport("addr\tsize\t\tend\tprev\tnext\n--------Allocated\n");
	for (Cell* cell = hd->allocated; cell; cell = cell->next)
		OSReport("%x\t%d\t%x\t%x\t%x\n", cell, cell->size, (char*)cell + cell->size, cell->prev,
		         cell->next);
	OSReport("--------Free\n");
	for (Cell* cell = hd->free; cell; cell = cell->next)
		OSReport("%x\t%d\t%x\t%x\t%x\n", cell, cell->size, (char*)cell + cell->size, cell->prev,
		         cell->next);
}

void OSVisitAllocated(void (*visitor)(void*, unsigned long))
{
	for (int i = 0; i < NumHeaps; i++)
		if (HeapArray[i].size >= 0)
			for (Cell* c = HeapArray[i].allocated; c; c = c->next)
				visitor((u8*)c + 32, (unsigned long)c->size);
}

// ---- ROM font ----------------------------------------------------------------
// The IPL ROM font is not available; provide an ANSI header with empty glyphs so
// JUTRomFont initializes. Text drawn with it renders blank.
static const int kGlyphW = 12;

BOOL OSInitFont(OSFontHeader* font)
{
	std::memset(font, 0, sizeof(OSFontHeader));
	font->fontType    = 0;
	font->firstChar   = 0x20;
	font->lastChar    = 0xFF;
	font->invalChar   = 0x20;
	font->ascent      = 18;
	font->descent     = 6;
	font->width       = kGlyphW;
	font->leading     = 24;
	font->cellWidth   = 24;
	font->cellHeight  = 24;
	font->sheetFormat = 0; // I4
	font->sheetColumn = 21;
	font->sheetRow    = 21;
	font->sheetWidth  = 512;
	font->sheetHeight = 512;
	font->sheetSize   = 0;
	font->sheetImage  = 0;
	return TRUE;
}

u32 OSLoadFont(OSFontHeader* font, void*)
{
	OSInitFont(font);
	return 0;
}

char* OSGetFontWidth(char* string, s32* width)
{
	if (width)
		*width = kGlyphW;
	return *string ? string + 1 : string;
}

char* OSGetFontTexture(char* string, void** image, s32* x, s32* y, s32* width)
{
	static u8 sBlank[512 * 512 / 2] __attribute__((aligned(32)));
	if (image)
		*image = sBlank;
	if (x)
		*x = 0;
	if (y)
		*y = 0;
	if (width)
		*width = kGlyphW;
	return *string ? string + 1 : string;
}

} // extern "C"
