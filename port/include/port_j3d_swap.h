/*
 * port_j3d_swap.h - big-endian -> native swap passes for J3D files
 * (.bmd/.bdl/.bmt models, .bck/.btk/.brk/.bpk/.btp/.bva/.blk/.bca/.bpa/.bla/
 * .bxk/.bxa animations). See port/ENDIAN.md.
 *
 *   port_swap_j3d_model(file)  J3DModelLoaderDataBase::load / loadMaterialTable
 *   port_swap_j3d_anm(file)    J3DAnmLoaderDataBase::load
 *
 * Both are idempotent: the file magic ('J3D2' / 'J3D1') is the marker. It
 * reads correctly through a native u32 only after the swap, and it is swapped
 * last, so a file is converted exactly once however often it is loaded.
 *
 * How a block is swapped:
 *  - the block header (magic, size, counts) by a fixed list of fields;
 *  - each table the header points to, by element layout. A table has no
 *    stored length, so it runs from its offset to the next larger offset in
 *    the same header (or the block end), like the readers do (readVertex,
 *    bmd2obj.py). Trailing padding is swapped too, which is harmless.
 *    Two header slots sharing one offset (an empty table placed at the start
 *    of the next one) are swapped once, with the layout of the later slot;
 *    `conflicts` counts the cases where the two layouts differ. The MAT3
 *    indirect slot follows J3DMaterialFactory's own presence test instead.
 *
 * Left big-endian on purpose:
 *  - SHP1 display lists (GX command stream; Aurora parses GameCube-format DLs);
 *  - texel / palette data (GX order). (The ResTIMG headers in TEX1 are swapped
 *    here by port_swap_tex1_timgs_force, which also sets their 0x19 marker.)
 *  - MDL3 (not read by J3DModelLoader).
 * VTX1 vertex arrays ARE swapped to native order (per component type from the
 * VTX1 format list): the CPU reads and writes them (J3DSkinDeform, J3DCluster
 * deformers, DrawUtil, MapMirror, PollutionCount), and Aurora reads arrays in
 * either order (GXSetArray(..., le) -> shader bswap), so native order with
 * le = true keeps CPU-made and file-loaded arrays the same format.
 *
 * Self-contained C so host tools (port/tools/test_j3d.cpp) can include it.
 */
#ifndef PORT_J3D_SWAP_H
#define PORT_J3D_SWAP_H

#include "port_endian.h"
#include "port_profile.h" /* w14: swap-pass timing under SMS_PORT_PROFILE */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- element layouts ---------------------------------------------------- */

typedef struct {
	unsigned short off;   /* byte offset inside the element */
	unsigned char width;  /* 2, 3 (byte reversal of 24 bits) or 4 */
	unsigned char count;  /* consecutive values */
} pj_run;

typedef struct {
	unsigned short size; /* element size; 0 = special (see pj_swap_table) */
	unsigned char nruns;
	pj_run runs[6];
} pj_layout;

enum {
	PJ_BYTES = 0, /* u8 / GXColor / byte structs: nothing to swap */
	PJ_U16,       /* u16 / s16 arrays (also GXColorS10, key tables, J3DModelHierarchy) */
	PJ_U32,       /* u32 / s32 / f32 arrays (also Mtx, GXVtxDescList, GXCullMode) */
	PJ_RGB24,     /* GX_RGBA6 vertex colours: 3-byte big-endian values */
	PJ_NTAB,      /* ResNTAB (special) */
	PJ_TIMG,      /* TEX1 ResTIMG headers: port_swap_tex1_timgs (special) */
	PJ_VTXFMT,    /* GXVtxAttrFmtList: u32 attr, cnt, type; u8 frac; pad */
	PJ_SHPINIT,   /* J3DShapeInitData (0x28) */
	PJ_SHPMTX,    /* J3DShapeMtxInitData: u16, u16, u32 */
	PJ_JNTINIT,   /* J3DJointInitData (0x40) */
	PJ_MATINIT,   /* J3DMaterialInitData (0x14C) */
	PJ_MATINIT21, /* J3DMaterialInitData_v21 (0x138) */
	PJ_INDINIT,   /* J3DIndInitData (0x138): 3 x J3DIndTexMtxInfo f32[2][3] */
	PJ_LIGHT,     /* J3DLightInfo (0x34) */
	PJ_TEXMTX,    /* J3DTexMtxInfo (0x64) */
	PJ_FOG,       /* J3DFogInfo (0x2C) */
	PJ_NBT,       /* J3DNBTScaleInfo (0x10) */
	PJ_REGTAB,    /* J3DAnmCRegKeyTable / KReg (0x1C): 12 u16 + u8 colour id */
	PJ_TPTTAB,    /* J3DAnmTexPatternFullTable (8): u16, u16, u8, pad, u16 */
	PJ_VCIDX,     /* J3DAnmVtxColorIndexData in the file: u16 num, pad, u32 index */
	PJ_ABSENT,    /* slot the loader ignores (MAT3 indirect data, see pj_swap_mat3) */
	PJ_NKINDS
};

static const pj_layout pj_layouts[PJ_NKINDS] = {
	/* BYTES     */ { 1, 0, { { 0, 0, 0 } } },
	/* U16       */ { 2, 1, { { 0, 2, 1 } } },
	/* U32       */ { 4, 1, { { 0, 4, 1 } } },
	/* RGB24     */ { 3, 1, { { 0, 3, 1 } } },
	/* NTAB      */ { 0, 0, { { 0, 0, 0 } } },
	/* TIMG      */ { 0, 0, { { 0, 0, 0 } } },
	/* VTXFMT    */ { 0x10, 1, { { 0, 4, 3 } } },
	/* SHPINIT   */ { 0x28, 2, { { 0x02, 2, 5 }, { 0x0C, 4, 7 } } },
	/* SHPMTX    */ { 0x08, 2, { { 0x00, 2, 2 }, { 0x04, 4, 1 } } },
	/* JNTINIT   */ { 0x40, 4, { { 0x00, 2, 1 }, { 0x04, 4, 3 }, { 0x10, 2, 4 }, { 0x18, 4, 10 } } },
	/* MATINIT   */ { 0x14C, 2, { { 0x08, 2, (0x9C - 0x08) / 2 }, { 0xBC, 2, (0x14C - 0xBC) / 2 } } },
	/* MATINIT21 */ { 0x138, 2, { { 0x08, 2, (0x88 - 0x08) / 2 }, { 0xA8, 2, (0x138 - 0xA8) / 2 } } },
	/* INDINIT   */ { 0x138, 3, { { 0x14, 4, 6 }, { 0x30, 4, 6 }, { 0x4C, 4, 6 } } },
	/* LIGHT     */ { 0x34, 2, { { 0x00, 4, 6 }, { 0x1C, 4, 6 } } },
	/* TEXMTX    */ { 0x64, 3, { { 0x04, 4, 5 }, { 0x18, 2, 1 }, { 0x1C, 4, 18 } } },
	/* FOG       */ { 0x2C, 3, { { 0x02, 2, 1 }, { 0x04, 4, 4 }, { 0x18, 2, 10 } } },
	/* NBT       */ { 0x10, 1, { { 0x04, 4, 3 } } },
	/* REGTAB    */ { 0x1C, 1, { { 0x00, 2, 12 } } },
	/* TPTTAB    */ { 0x08, 2, { { 0x00, 2, 2 }, { 0x06, 2, 1 } } },
	/* VCIDX     */ { 0x08, 2, { { 0x00, 2, 1 }, { 0x04, 4, 1 } } },
	/* ABSENT    */ { 0, 0, { { 0, 0, 0 } } },
};

static inline void pj_swap_run(unsigned char* p, int width, int count)
{
	int i;
	for (i = 0; i < count; i++, p += width) {
		if (width == 2) {
			sms_swap16_inplace(p);
		} else if (width == 4) {
			sms_swap32_inplace(p);
		} else if (width == 3) {
			unsigned char t = p[0];
			p[0]            = p[2];
			p[2]            = t;
		}
	}
}

/* ResNTAB: u16 count, u16 pad, count x { u16 key, u16 offset }, strings. */
static inline void pj_swap_ntab(unsigned char* p, unsigned int bytes)
{
	unsigned int n = sms_be16(p), i;
	if (bytes < 4)
		return;
	if (4 + n * 4 > bytes)
		n = (bytes - 4) / 4;
	pj_swap_run(p, 2, 2);
	for (i = 0; i < n; i++)
		pj_swap_run(p + 4 + i * 4, 2, 2);
}

/* Swap one table of `bytes` bytes laid out as consecutive `kind` elements. */
static inline void pj_swap_table(unsigned char* p, unsigned int bytes, int kind)
{
	const pj_layout* l = &pj_layouts[kind];
	unsigned int e, r;
	if (kind == PJ_NTAB) {
		pj_swap_ntab(p, bytes);
		return;
	}
	if (l->size == 0 || l->nruns == 0)
		return;
	for (e = 0; e + l->size <= bytes; e += l->size)
		for (r = 0; r < l->nruns; r++)
			pj_swap_run(p + e + l->runs[r].off, l->runs[r].width, l->runs[r].count);
}

/* ---- block walk --------------------------------------------------------- */

typedef struct {
	unsigned short field; /* byte offset of the u32 table offset in the block header */
	unsigned char kind;
} pj_sect;

/* Swap every table of a block, then the offset words themselves. The offsets
 * are read big-endian, so this must run before the header is swapped. */
static inline void pj_swap_sections(unsigned char* block, unsigned int blockSize, const pj_sect* s,
                                    int n, int* conflicts)
{
	unsigned int offs[32];
	int i, j;
	if (n > 32)
		n = 32;
	for (i = 0; i < n; i++)
		offs[i] = s[i].kind == PJ_ABSENT ? 0 : sms_be32(block + s[i].field);
	for (i = 0; i < n; i++) {
		unsigned int start = offs[i], end = blockSize;
		int owner = i, seen = 0;
		if (start == 0 || start >= blockSize)
			continue;
		for (j = 0; j < i; j++)
			if (offs[j] == start)
				seen = 1;
		if (seen)
			continue; /* handled with the first slot of the group */
		/* Slots sharing a start: the writer puts an empty table where the
		 * next one begins, so the table belongs to the LAST slot in header
		 * order (e.g. MAT3 light 0x30 / texGenNum 0x34 in most models,
		 * texCoord 0x38 / texNo 0x48 / tevOrder 0x4C in a few). Differing
		 * layouts are counted for the test: high half when one of them is
		 * plain bytes, low half otherwise. */
		for (j = i + 1; j < n; j++) {
			if (offs[j] != start)
				continue;
			if (conflicts && s[j].kind != s[owner].kind)
				*conflicts += (s[j].kind == PJ_BYTES || s[owner].kind == PJ_BYTES) ? 0x10000 : 1;
			owner = j;
		}
		for (j = 0; j < n; j++)
			if (offs[j] > start && offs[j] < end)
				end = offs[j];
		pj_swap_table(block + start, end - start, s[owner].kind);
	}
	for (i = 0; i < n; i++)
		sms_swap32_inplace(block + s[i].field);
}

/* Header scalars: `runs` relative to the block start (magic and size are
 * always swapped by the caller). */
static inline void pj_swap_header(unsigned char* block, const pj_run* runs, int n)
{
	int i;
	for (i = 0; i < n; i++)
		pj_swap_run(block + runs[i].off, runs[i].width, runs[i].count);
}

#define PJ_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define PJ_FOURCC(a, b, c, d)                                                                     \
	(((unsigned int)(a) << 24) | ((unsigned int)(b) << 16) | ((unsigned int)(c) << 8)          \
	 | (unsigned int)(d))

/* ---- model blocks (J3DModelLoader) ------------------------------------- */

/* GXAttr / GXCompType / GXCompCnt values from the VTX1 format list. */
#define PJ_GX_VA_POS 9
#define PJ_GX_VA_NRM 10
#define PJ_GX_VA_CLR0 11
#define PJ_GX_VA_CLR1 12
#define PJ_GX_VA_TEX0 13
#define PJ_GX_VA_NBT 25
#define PJ_GX_VA_NULL 0xFF

static inline int pj_vtx_kind(unsigned int attr, unsigned int type)
{
	if (attr == PJ_GX_VA_CLR0 || attr == PJ_GX_VA_CLR1) {
		switch (type) {
		case 0: /* GX_RGB565 */
		case 3: /* GX_RGBA4 */
			return PJ_U16;
		case 4: /* GX_RGBA6 */
			return PJ_RGB24;
		default: /* GX_RGB8, GX_RGBX8, GX_RGBA8: bytes */
			return PJ_BYTES;
		}
	}
	switch (type) {
	case 2: /* GX_U16 */
	case 3: /* GX_S16 */
		return PJ_U16;
	case 4: /* GX_F32 */
		return PJ_U32;
	default: /* GX_U8, GX_S8 */
		return PJ_BYTES;
	}
}

static inline void pj_swap_vtx1(unsigned char* b, unsigned int size, int* conflicts)
{
	pj_sect s[14];
	unsigned int fmt = sms_be32(b + 0x08);
	int i;
	for (i = 0; i < 14; i++) {
		s[i].field = (unsigned short)(0x08 + i * 4);
		s[i].kind  = PJ_BYTES; /* an array with no format entry stays big-endian */
	}
	s[0].kind = PJ_VTXFMT;
	if (fmt != 0) {
		unsigned char* f = b + fmt;
		while (f + 0x10 <= b + size) {
			unsigned int attr = sms_be32(f), type = sms_be32(f + 8);
			int slot = -1;
			if (attr == PJ_GX_VA_NULL)
				break;
			if (attr == PJ_GX_VA_POS)
				slot = 1;
			else if (attr == PJ_GX_VA_NRM)
				slot = 2;
			else if (attr == PJ_GX_VA_NBT)
				slot = 3;
			else if (attr == PJ_GX_VA_CLR0 || attr == PJ_GX_VA_CLR1)
				slot = 4 + (int)(attr - PJ_GX_VA_CLR0);
			else if (attr >= PJ_GX_VA_TEX0 && attr < PJ_GX_VA_TEX0 + 8)
				slot = 6 + (int)(attr - PJ_GX_VA_TEX0);
			if (slot >= 0)
				s[slot].kind = (unsigned char)pj_vtx_kind(attr, type);
			f += 0x10;
		}
	}
	pj_swap_sections(b, size, s, 14, conflicts);
}

static const pj_run pj_hdr_count16[] = { { 0x08, 2, 1 } };
static const pj_run pj_hdr_inf1[]    = { { 0x08, 2, 1 }, { 0x0C, 4, 2 } };

static const pj_sect pj_sect_inf1[] = { { 0x14, PJ_U16 } }; /* J3DModelHierarchy {u16, u16} */
static const pj_sect pj_sect_evp1[] = { { 0x0C, PJ_BYTES }, { 0x10, PJ_U16 }, { 0x14, PJ_U32 },
	                                    { 0x18, PJ_U32 } };
static const pj_sect pj_sect_drw1[] = { { 0x0C, PJ_BYTES }, { 0x10, PJ_U16 } };
static const pj_sect pj_sect_jnt1[] = { { 0x0C, PJ_JNTINIT }, { 0x10, PJ_U16 }, { 0x14, PJ_NTAB } };
static const pj_sect pj_sect_shp1[] = { { 0x0C, PJ_SHPINIT }, { 0x10, PJ_U16 },  { 0x14, PJ_NTAB },
	                                    { 0x18, PJ_U32 },     { 0x1C, PJ_U16 },  { 0x20, PJ_BYTES },
	                                    { 0x24, PJ_SHPMTX },  { 0x28, PJ_U32 } };
static const pj_sect pj_sect_tex1[] = { { 0x0C, PJ_TIMG }, { 0x10, PJ_NTAB } };
/* J3DMaterialBlock (MAT3), in header order */
static const pj_sect pj_sect_mat3[] = {
	{ 0x0C, PJ_MATINIT }, { 0x10, PJ_U16 },   { 0x14, PJ_NTAB },   { 0x18, PJ_INDINIT },
	{ 0x1C, PJ_U32 },     { 0x20, PJ_BYTES }, { 0x24, PJ_BYTES },  { 0x28, PJ_BYTES },
	{ 0x2C, PJ_BYTES },   { 0x30, PJ_LIGHT }, { 0x34, PJ_BYTES },  { 0x38, PJ_BYTES },
	{ 0x3C, PJ_BYTES },   { 0x40, PJ_TEXMTX }, { 0x44, PJ_TEXMTX }, { 0x48, PJ_U16 },
	{ 0x4C, PJ_BYTES },   { 0x50, PJ_U16 },   { 0x54, PJ_BYTES },  { 0x58, PJ_BYTES },
	{ 0x5C, PJ_BYTES },   { 0x60, PJ_BYTES }, { 0x64, PJ_BYTES },  { 0x68, PJ_FOG },
	{ 0x6C, PJ_BYTES },   { 0x70, PJ_BYTES }, { 0x74, PJ_BYTES },  { 0x78, PJ_BYTES },
	{ 0x7C, PJ_BYTES },   { 0x80, PJ_NBT },
};
/* J3DMaterialBlock_v21 (MAT2), in header order */
static const pj_sect pj_sect_mat2[] = {
	{ 0x0C, PJ_MATINIT21 }, { 0x10, PJ_U16 },    { 0x14, PJ_NTAB },   { 0x18, PJ_U32 },
	{ 0x1C, PJ_BYTES },     { 0x20, PJ_BYTES },  { 0x24, PJ_BYTES },  { 0x28, PJ_BYTES },
	{ 0x2C, PJ_BYTES },     { 0x30, PJ_BYTES },  { 0x34, PJ_TEXMTX }, { 0x38, PJ_TEXMTX },
	{ 0x3C, PJ_U16 },       { 0x40, PJ_BYTES },  { 0x44, PJ_U16 },    { 0x48, PJ_BYTES },
	{ 0x4C, PJ_BYTES },     { 0x50, PJ_BYTES },  { 0x54, PJ_BYTES },  { 0x58, PJ_BYTES },
	{ 0x5C, PJ_FOG },       { 0x60, PJ_BYTES },  { 0x64, PJ_BYTES },  { 0x68, PJ_BYTES },
	{ 0x6C, PJ_BYTES },     { 0x70, PJ_BYTES },  { 0x74, PJ_NBT },
};

/* MAT3: J3DMaterialFactory only uses the indirect table when
 * (u32)(indOffset - nameOffset) > 4; otherwise the slot usually holds the
 * name table's offset, so it must not claim that table. */
static inline void pj_swap_mat3(unsigned char* b, unsigned int size, int* conflicts)
{
	pj_sect s[30];
	unsigned int ind = sms_be32(b + 0x18), name = sms_be32(b + 0x14);
	int i;
	for (i = 0; i < 30; i++)
		s[i] = pj_sect_mat3[i];
	if (ind == 0 || (unsigned int)(ind - name) <= 4)
		s[3].kind = PJ_ABSENT;
	pj_swap_sections(b, size, s, 30, conflicts); /* also swaps every offset word */
	pj_swap_run(b + 8, 2, 1);
}

#define PJ_SWAP_BLOCK(b, size, hdr, sect, conflicts)                                              \
	do {                                                                                           \
		pj_swap_sections((b), (size), (sect), PJ_COUNT(sect), (conflicts));                       \
		pj_swap_header((b), (hdr), PJ_COUNT(hdr));                                                 \
	} while (0)

/* JUTDataFileHeader: u32 magic, type, size, blockNum; 0x10..0x1C tool
 * signature bytes; u32 at 0x1C (mSeAnmOffset in some anims). */
#define PORT_J3D2_MAGIC_RAW 0x3244334Au /* "J3D2" bytes read little-endian */
#define PORT_J3D1_MAGIC_RAW 0x3144334Au /* "J3D1" */

/* Collect the blocks of a raw file. Each block's size is clamped to the file
 * size from the header, so a pass never writes past the file: some shipped
 * .bmt files declare more blocks than they contain (the loader walks past
 * the end, reading but never writing; the pass stops instead). */
static inline unsigned int pj_file_blocks(unsigned char* file, unsigned char** blocks,
                                          unsigned int* sizes, unsigned int max)
{
	unsigned int num = sms_be32(file + 0x0C), size = sms_be32(file + 0x08), i, off = 0x20;
	for (i = 0; i < num && i < max; i++) {
		unsigned int bs;
		if (off + 8 > size)
			break;
		blocks[i] = file + off;
		bs        = sms_be32(file + off + 4);
		sizes[i]  = bs < 8 || bs > size - off ? size - off : bs;
		if (bs < 8 || bs > size - off)
			return i + 1;
		off += bs;
	}
	return i;
}

static inline void pj_swap_file_header(unsigned char* file)
{
	pj_swap_run(file, 4, 4);
	pj_swap_run(file + 0x1C, 4, 1);
}

static inline int port_swap_j3d_model_ex(void* data, int* conflicts)
{
	unsigned char* file = (unsigned char*)data;
	unsigned char* blocks[64];
	unsigned int sizes[64];
	unsigned int n, i;
	if (!file || *(unsigned int*)file != PORT_J3D2_MAGIC_RAW)
		return 0;
	n = pj_file_blocks(file, blocks, sizes, 64);
	for (i = 0; i < n; i++) {
		unsigned char* b  = blocks[i];
		unsigned int tag  = sms_be32(b);
		unsigned int size = sizes[i];
		switch (tag) {
		case PJ_FOURCC('I', 'N', 'F', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_inf1, pj_sect_inf1, conflicts);
			break;
		case PJ_FOURCC('V', 'T', 'X', '1'):
			pj_swap_vtx1(b, size, conflicts);
			break;
		case PJ_FOURCC('E', 'V', 'P', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_count16, pj_sect_evp1, conflicts);
			break;
		case PJ_FOURCC('D', 'R', 'W', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_count16, pj_sect_drw1, conflicts);
			break;
		case PJ_FOURCC('J', 'N', 'T', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_count16, pj_sect_jnt1, conflicts);
			break;
		case PJ_FOURCC('M', 'A', 'T', '3'):
			pj_swap_mat3(b, size, conflicts);
			break;
		case PJ_FOURCC('M', 'A', 'T', '2'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_count16, pj_sect_mat2, conflicts);
			break;
		case PJ_FOURCC('S', 'H', 'P', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_count16, pj_sect_shp1, conflicts);
			break;
		case PJ_FOURCC('T', 'E', 'X', '1'):
			/* ResTIMG headers. Unconditional: this pass runs once per file (the
			 * magic), and the disc pad byte at 0x19 is not reliably 0 in TEX1
			 * (coronaBoss water.bmd has 0x5A). Sets the marker for later calls. */
			port_swap_tex1_timgs_force(b);
			PJ_SWAP_BLOCK(b, size, pj_hdr_count16, pj_sect_tex1, conflicts);
			break;
		default: /* MDL3 and unknown blocks: only the block header */
			break;
		}
		pj_swap_run(b, 4, 2); /* magic, size */
	}
	pj_swap_file_header(file); /* last: the magic is the "already swapped" marker */
	return 1;
}

static inline int port_swap_j3d_model(void* data)
{
	int r;
	PORT_PROF_BEGIN(t0); /* w14: SMS_PORT_PROFILE (no-op outside the Aurora game build) */
	r = port_swap_j3d_model_ex(data, 0);
	if (r)
		PORT_PROF_END(PP_SWAP, t0);
	return r;
}

/* ---- animation blocks (J3DAnmLoader) ------------------------------------ */

/* ANK1 / ANF1: u8 attr, u8 rotFrac, s16 frameMax, u16 jointNum, u16 scale/rot/trans counts */
static const pj_run pj_hdr_ank1[]  = { { 0x0A, 2, 5 } };
static const pj_sect pj_sect_ank1[] = { { 0x14, PJ_U16 }, { 0x18, PJ_U32 }, { 0x1C, PJ_U16 },
	                                    { 0x20, PJ_U32 } };
/* PAK1 / PAF1: frameMax at 0x0C, material num + 4 value counts */
static const pj_run pj_hdr_pak1[]  = { { 0x0C, 2, 6 } };
static const pj_sect pj_sect_pak1[] = { { 0x18, PJ_U16 }, { 0x1C, PJ_U16 }, { 0x20, PJ_NTAB },
	                                    { 0x24, PJ_U16 }, { 0x28, PJ_U16 }, { 0x2C, PJ_U16 },
	                                    { 0x30, PJ_U16 } };
static const pj_sect pj_sect_paf1[] = { { 0x18, PJ_U16 },   { 0x1C, PJ_U16 },   { 0x20, PJ_NTAB },
	                                    { 0x24, PJ_BYTES }, { 0x28, PJ_BYTES }, { 0x2C, PJ_BYTES },
	                                    { 0x30, PJ_BYTES } };
/* TTK1: counts at 0x0A..0x12 and 0x34..0x3A; tables, then the "post" set */
static const pj_run pj_hdr_ttk1[]  = { { 0x0A, 2, 5 }, { 0x34, 2, 4 } };
static const pj_sect pj_sect_ttk1[] = {
	{ 0x14, PJ_U16 }, { 0x18, PJ_U16 }, { 0x1C, PJ_NTAB }, { 0x20, PJ_BYTES },
	{ 0x24, PJ_U32 }, { 0x28, PJ_U32 }, { 0x2C, PJ_U16 },  { 0x30, PJ_U32 },
	{ 0x3C, PJ_U16 }, { 0x40, PJ_U16 }, { 0x44, PJ_NTAB }, { 0x48, PJ_BYTES },
	{ 0x4C, PJ_U32 }, { 0x50, PJ_U32 }, { 0x54, PJ_U16 },  { 0x58, PJ_U32 },
};
/* TRK1: frameMax, C/K material nums, 8 value counts */
static const pj_run pj_hdr_trk1[]  = { { 0x0A, 2, 11 } };
static const pj_sect pj_sect_trk1[] = {
	{ 0x20, PJ_REGTAB }, { 0x24, PJ_REGTAB }, { 0x28, PJ_U16 }, { 0x2C, PJ_U16 }, { 0x30, PJ_NTAB },
	{ 0x34, PJ_NTAB },   { 0x38, PJ_U16 },    { 0x3C, PJ_U16 }, { 0x40, PJ_U16 }, { 0x44, PJ_U16 },
	{ 0x48, PJ_U16 },    { 0x4C, PJ_U16 },    { 0x50, PJ_U16 }, { 0x54, PJ_U16 },
};
/* TPT1 */
static const pj_run pj_hdr_tpt1[]  = { { 0x0A, 2, 3 } };
static const pj_sect pj_sect_tpt1[] = { { 0x10, PJ_TPTTAB }, { 0x14, PJ_U16 }, { 0x18, PJ_U16 },
	                                    { 0x1C, PJ_NTAB } };
/* VAF1 */
static const pj_run pj_hdr_vaf1[]  = { { 0x0A, 2, 3 } };
static const pj_sect pj_sect_vaf1[] = { { 0x10, PJ_U16 }, { 0x14, PJ_BYTES } };
/* CLK1 / CLF1: frameMax, s32 count */
static const pj_run pj_hdr_clk1[]  = { { 0x0A, 2, 1 }, { 0x0C, 4, 1 } };
static const pj_sect pj_sect_clk1[] = { { 0x10, PJ_U16 }, { 0x14, PJ_U32 } };
/* VCK1: frameMax, table nums u16[2], index nums u32[2] */
static const pj_run pj_hdr_vck1[]  = { { 0x0A, 2, 3 }, { 0x10, 4, 2 } };
/* VCF1: frameMax, table nums u16[2], index nums u16[2], u32 */
static const pj_run pj_hdr_vcf1[]  = { { 0x0A, 2, 5 }, { 0x14, 4, 1 } };
static const pj_sect pj_sect_vck1[] = { { 0x18, PJ_U16 },   { 0x1C, PJ_U16 }, { 0x20, PJ_VCIDX },
	                                    { 0x24, PJ_VCIDX }, { 0x28, PJ_U16 }, { 0x2C, PJ_U16 },
	                                    { 0x30, PJ_U16 },   { 0x34, PJ_U16 }, { 0x38, PJ_U16 },
	                                    { 0x3C, PJ_U16 } };
static const pj_sect pj_sect_vcf1[] = { { 0x18, PJ_U16 },   { 0x1C, PJ_U16 },   { 0x20, PJ_VCIDX },
	                                    { 0x24, PJ_VCIDX }, { 0x28, PJ_U16 },   { 0x2C, PJ_U16 },
	                                    { 0x30, PJ_BYTES }, { 0x34, PJ_BYTES }, { 0x38, PJ_BYTES },
	                                    { 0x3C, PJ_BYTES } };

static inline int port_swap_j3d_anm_ex(void* data, int* conflicts)
{
	unsigned char* file = (unsigned char*)data;
	unsigned char* blocks[16];
	unsigned int sizes[16];
	unsigned int n, i;
	if (!file || *(unsigned int*)file != PORT_J3D1_MAGIC_RAW)
		return 0;
	n = pj_file_blocks(file, blocks, sizes, 16);
	for (i = 0; i < n; i++) {
		unsigned char* b  = blocks[i];
		unsigned int tag  = sms_be32(b);
		unsigned int size = sizes[i];
		switch (tag) {
		case PJ_FOURCC('A', 'N', 'K', '1'):
		case PJ_FOURCC('A', 'N', 'F', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_ank1, pj_sect_ank1, conflicts);
			break;
		case PJ_FOURCC('P', 'A', 'K', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_pak1, pj_sect_pak1, conflicts);
			break;
		case PJ_FOURCC('P', 'A', 'F', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_pak1, pj_sect_paf1, conflicts);
			break;
		case PJ_FOURCC('T', 'T', 'K', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_ttk1, pj_sect_ttk1, conflicts);
			break;
		case PJ_FOURCC('T', 'R', 'K', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_trk1, pj_sect_trk1, conflicts);
			break;
		case PJ_FOURCC('T', 'P', 'T', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_tpt1, pj_sect_tpt1, conflicts);
			break;
		case PJ_FOURCC('V', 'A', 'F', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_vaf1, pj_sect_vaf1, conflicts);
			break;
		case PJ_FOURCC('C', 'L', 'K', '1'):
		case PJ_FOURCC('C', 'L', 'F', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_clk1, pj_sect_clk1, conflicts);
			break;
		case PJ_FOURCC('V', 'C', 'K', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_vck1, pj_sect_vck1, conflicts);
			break;
		case PJ_FOURCC('V', 'C', 'F', '1'):
			PJ_SWAP_BLOCK(b, size, pj_hdr_vcf1, pj_sect_vcf1, conflicts);
			break;
		default:
			break;
		}
		pj_swap_run(b, 4, 2);
	}
	pj_swap_file_header(file);
	return 1;
}

static inline int port_swap_j3d_anm(void* data)
{
	int r;
	PORT_PROF_BEGIN(t0); /* w14: SMS_PORT_PROFILE */
	r = port_swap_j3d_anm_ex(data, 0);
	if (r)
		PORT_PROF_END(PP_SWAP, t0);
	return r;
}

#ifdef __cplusplus
}
#endif

#endif /* PORT_J3D_SWAP_H */
