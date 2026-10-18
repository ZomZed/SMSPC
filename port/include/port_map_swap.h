/*
 * port_map_swap.h - big-endian -> native swap passes for map-side files
 * (.col collision, .ral rail/graph). See port/ENDIAN.md and POINTERS.md.
 *
 *   port_swap_col(file)   collision data (TMapCollisionBase::init)
 *   port_swap_ral(file)   rail graph    (TGraphWeb::load / TRailNode)
 *
 * Both are idempotent and self-contained C so host tools can include them.
 *
 * .col layout (all big-endian on disc):
 *   0x00 u32 vertexNum, u32 vertexOffset, u32 groupNum, u32 groupOffset
 *   groups (groupNum x 0x18):
 *        u16 bgType, s16 triangleNum, u16 flags, u16 pad (0xFFFF),
 *        u32 indicesOff, soundMaterialsOff, unk10Off, additionalDataOff (0 = none)
 *   vertices:  vertexNum x 3 f32
 *   per group: s16 indices[triangleNum * 3], u8 soundMaterials[triangleNum],
 *              u8 unk10[triangleNum], s16 additionalData[triangleNum] (flags & 1)
 * The original loader patched the four offsets of every group into pointers in
 * place (WAS_PATCHED, flag 0x8000). That cannot work with 64-bit pointers, so
 * the PC loader keeps the offsets here and builds native group structs.
 * In all 126 shipped files the groups sit at 0x10, directly after the header,
 * and the arrays do not overlap.
 *
 * Marker: groupOffset. It is a small value (0x10 in every file), so read
 * through a native u32 it is >= 0x01000000 while the file is still big-endian.
 */
#ifndef PORT_MAP_SWAP_H
#define PORT_MAP_SWAP_H

#include "port_endian.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The on-disc group (offsets, not pointers). */
typedef struct {
	unsigned short bgType;
	short triangleNum;
	unsigned short flags;
	unsigned short pad;
	unsigned int indicesOff;
	unsigned int soundMaterialsOff;
	unsigned int unk10Off;
	unsigned int additionalDatasOff;
} PortColGroupFile;

static inline int port_col_is_swapped(const void* file)
{
	return ((const unsigned int*)file)[3] < 0x01000000u;
}

/* Returns 1 if it swapped, 0 if the file already was native. */
static inline int port_swap_col(void* file)
{
	unsigned char* b = (unsigned char*)file;
	unsigned int* h  = (unsigned int*)file;
	unsigned int vertexNum, vertexOff, groupNum, groupOff, i, j;
	if (port_col_is_swapped(file))
		return 0;
	for (i = 0; i < 4; i++)
		sms_swap32_inplace(&h[i]);
	vertexNum = h[0];
	vertexOff = h[1];
	groupNum  = h[2];
	groupOff  = h[3];

	for (i = 0; i < vertexNum * 3; i++)
		sms_swap32_inplace(b + vertexOff + i * 4);

	for (i = 0; i < groupNum; i++) {
		unsigned char* g = b + groupOff + i * sizeof(PortColGroupFile);
		PortColGroupFile* gf = (PortColGroupFile*)g;
		unsigned int tris, flags;
		sms_swap16_inplace(g + 0);
		sms_swap16_inplace(g + 2);
		sms_swap16_inplace(g + 4);
		sms_swap16_inplace(g + 6);
		for (j = 0; j < 4; j++)
			sms_swap32_inplace(g + 8 + j * 4);
		tris  = (unsigned int)(unsigned short)gf->triangleNum;
		flags = gf->flags;
		for (j = 0; j < tris * 3; j++)
			sms_swap16_inplace(b + gf->indicesOff + j * 2);
		if (gf->additionalDatasOff) {
			for (j = 0; j < tris; j++)
				sms_swap16_inplace(b + gf->additionalDatasOff + j * 2);
		}
		(void)flags;
	}
	return 1;
}

/*
 * .ral rail graph file (scene.ral, TGraphGroup / TGraphWeb), big-endian on disc:
 *   graph descriptors, 12 bytes each, ended by an all-zero one:
 *        s32 nodeNum, u32 nameOffset (C string), u32 railNodesOffset
 *   names (bytes)
 *   rail nodes (nodeNum x 0x44, struct TRailNode):
 *        s16 x, y, z; s16 connectionNum; u32 flags; u16 pitch, yaw, roll, speed;
 *        u16 connections[8]; f32 periods[8]
 * The game reads all of it in place (TGraphNode keeps a pointer to each rail
 * node and TGraphWeb::translateNodes edits them), so the whole file is swapped
 * once. Nothing in the file is a pointer; offsets are added to the file base at
 * use, so 64-bit needs no further change.
 *
 * Marker: the first descriptor's nodeNum is a small positive number (< 0x10000);
 * read through a native u32 it is >= 0x10000 while the file is still big-endian
 * (an empty file, all zero, needs no swap).
 */
static inline int port_swap_ral(void* file)
{
	unsigned char* b = (unsigned char*)file;
	unsigned int off = 0, i, j;
	if (*(unsigned int*)b < 0x10000u)
		return 0;
	for (;; off += 12) {
		unsigned int nodeNum, railOff;
		sms_swap32_inplace(b + off + 0);
		sms_swap32_inplace(b + off + 4);
		sms_swap32_inplace(b + off + 8);
		nodeNum = *(unsigned int*)(b + off);
		railOff = *(unsigned int*)(b + off + 8);
		if (nodeNum == 0)
			break;
		for (i = 0; i < nodeNum; i++) {
			unsigned char* n = b + railOff + i * 0x44;
			for (j = 0; j < 4; j++)
				sms_swap16_inplace(n + j * 2); /* x y z connectionNum */
			sms_swap32_inplace(n + 0x8);
			for (j = 0; j < 4 + 8; j++)
				sms_swap16_inplace(n + 0xC + j * 2); /* pitch yaw roll speed, connections */
			for (j = 0; j < 8; j++)
				sms_swap32_inplace(n + 0x24 + j * 4);
		}
	}
	return 1;
}

/*
 * .ymp pollution map (scene/map/ymap.ymp, TPollutionManager::initPollutionInfo),
 * big-endian on disc:
 *   0x0 u16 layerCount, u16 pad, u32 layerInfosOffset (from the file start)
 *   layer infos (layerCount x 0x2C, struct TPollutionLayerInfo):
 *        u16 pollutionType, flags, planeType, pad; f32 verticalOffset, texelSize,
 *        left, top, right, bottom; u16 log2Width, log2Height; u32 unk24;
 *        u32 heightMapOffset (from the file start)
 *   height maps: (1 << log2Width) * (1 << log2Height) bytes, no swap
 * The original patched both offsets into pointers in place (`+ (u32)info`),
 * which cannot work with 64-bit pointers (the pointer member also moves the
 * struct stride from 0x2C to 0x30). The PC loader swaps the header and layer
 * infos here, then builds a native TPollutionLayerInfo array that points into
 * the (never otherwise modified) file's height maps.
 *
 * Marker: layerCount is small (<= 20), so a native read of it is >= 0x100
 * while the file is still big-endian (count 0 needs no swap).
 */
typedef struct {
	unsigned short layerCount;
	unsigned short pad;
	unsigned int layerInfosOffset;
} PortYmpHeader;

typedef struct {
	unsigned short pollutionType, flags, planeType, pad;
	float verticalOffset, texelSize, left, top, right, bottom;
	unsigned short log2Width, log2Height;
	unsigned int unk24;
	unsigned int heightMapOffset;
} PortYmpLayer;

static inline int port_swap_ymp(void* file)
{
	unsigned char* b = (unsigned char*)file;
	unsigned int n, off, i, j;
	if (*(unsigned short*)b < 0x100u)
		return 0;
	sms_swap16_inplace(b + 0);
	sms_swap16_inplace(b + 2);
	sms_swap32_inplace(b + 4);
	n   = ((PortYmpHeader*)b)->layerCount;
	off = ((PortYmpHeader*)b)->layerInfosOffset;
	for (i = 0; i < n; i++) {
		unsigned char* l = b + off + i * sizeof(PortYmpLayer);
		for (j = 0; j < 4; j++)
			sms_swap16_inplace(l + j * 2);
		for (j = 0; j < 6; j++)
			sms_swap32_inplace(l + 8 + j * 4);
		sms_swap16_inplace(l + 0x20);
		sms_swap16_inplace(l + 0x22);
		sms_swap32_inplace(l + 0x24);
		sms_swap32_inplace(l + 0x28);
	}
	return 1;
}

#ifdef __cplusplus
}
#endif

#endif /* PORT_MAP_SWAP_H */
