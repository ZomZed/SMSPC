/*
 * port_endian.h - byte-order helpers for the PC port (see port/ENDIAN.md).
 *
 * All GameCube data files are big-endian; the PC build is little-endian.
 * Included at the end of port_compat.h, so every PC translation unit can use
 * these under `#ifdef SMS_PORT` without an extra #include. Self-contained C,
 * so host tools (port/tools/test_archive.cpp) can include it directly.
 *
 * Naming:
 *   sms_bswap16/32/64      unconditional byte reversal
 *   sms_be16/32(p)         read a big-endian value from unaligned memory
 *   sms_swap16/32_inplace  reverse one value in memory
 *   port_swap_<format>()   one-shot in-place swap pass for a loaded file
 */
#ifndef PORT_ENDIAN_H
#define PORT_ENDIAN_H

#ifdef __cplusplus
extern "C" {
#endif

static inline unsigned short sms_bswap16(unsigned short v)
{
	return (unsigned short)((v >> 8) | (v << 8));
}
static inline unsigned int sms_bswap32(unsigned int v) { return __builtin_bswap32(v); }
static inline unsigned long long sms_bswap64(unsigned long long v) { return __builtin_bswap64(v); }

static inline unsigned short sms_be16(const void* p)
{
	const unsigned char* b = (const unsigned char*)p;
	return (unsigned short)((b[0] << 8) | b[1]);
}
static inline unsigned int sms_be32(const void* p)
{
	const unsigned char* b = (const unsigned char*)p;
	return ((unsigned int)b[0] << 24) | ((unsigned int)b[1] << 16) | ((unsigned int)b[2] << 8)
	       | (unsigned int)b[3];
}

/* Typed big-endian reads (s16 / f32) from unaligned memory; the buffer is not modified. */
static inline short sms_bes16(const void* p) { return (short)sms_be16(p); }
static inline float sms_bef32(const void* p)
{
	unsigned int u = sms_be32(p);
	float f;
	__builtin_memcpy(&f, &u, sizeof f);
	return f;
}

static inline void sms_swap16_inplace(void* p)
{
	unsigned char* b = (unsigned char*)p;
	unsigned char t  = b[0];
	b[0]             = b[1];
	b[1]             = t;
}
static inline void sms_swap32_inplace(void* p)
{
	unsigned char* b = (unsigned char*)p;
	unsigned char t;
	t    = b[0];
	b[0] = b[3];
	b[3] = t;
	t    = b[1];
	b[1] = b[2];
	b[2] = t;
}
static inline void sms_swap64_inplace(void* p)
{
	unsigned char* b = (unsigned char*)p;
	int i;
	for (i = 0; i < 4; i++) {
		unsigned char t = b[i];
		b[i]            = b[7 - i];
		b[7 - i]        = t;
	}
}

/* Swap a value read through a typed stream read (JSUInputStream::readU32 and
 * friends). Size-dispatched so one helper covers every integer/float type. */
static inline void sms_swap_value(void* p, int size)
{
	if (size == 2)
		sms_swap16_inplace(p);
	else if (size == 4)
		sms_swap32_inplace(p);
	else if (size == 8)
		sms_swap64_inplace(p);
}

/* ---- RARC archive (JKRArchive) ----------------------------------------
 *
 * Layout (all big-endian on disc):
 *   0x00 header (0x20 bytes, 8 x u32): 'RARC', file_length, header_length,
 *        file_data_offset, file_data_length, mem part size, aram part size, pad
 *   info block at header_length (normally 0x20), offsets relative to it:
 *        6 x u32 (num_nodes, node_offset, num_file_entries, file_entry_offset,
 *        string_table_length, string_table_offset), u16 nextFreeFileID,
 *        u8 isSyncIDs, 5 pad bytes
 *   node table   (num_nodes x 0x10):   u32 type, u32 name_offset, u16 hash,
 *                                      u16 file_count, u32 first_file_index
 *   file entries (num_files x 0x14):   u16 id, u16 hash, u32 flags<<24|name_offset,
 *                                      u32 data_offset, u32 size, u32 (runtime ptr)
 *   string table: chars, no swap.
 *
 * File *contents* are not touched; each format gets its own pass later.
 */

/* Signature as seen through a native u32 read after / before the swap. */
#define PORT_RARC_MAGIC_NATIVE 0x52415243u /* 'RARC' once swapped */
#define PORT_RARC_MAGIC_RAW    0x43524152u /* "RARC" bytes read little-endian */

/* Swap the 0x20-byte header. Returns 1 if swapped, 0 if it already was in
 * native order (idempotent: the signature doubles as the marker). */
static inline int port_swap_rarc_header(void* header)
{
	unsigned int* w = (unsigned int*)header;
	int i;
	if (w[0] != PORT_RARC_MAGIC_RAW)
		return 0;
	for (i = 0; i < 8; i++)
		w[i] = sms_bswap32(w[i]);
	return 1;
}

/* Swap the info block plus its node table and file entries. `info` points at
 * the info block (archive + header_length). Not idempotent: call exactly once
 * per freshly loaded copy, right after the load. */
static inline void port_swap_rarc_info(void* info)
{
	unsigned char* base = (unsigned char*)info;
	unsigned int* w     = (unsigned int*)info;
	unsigned int i, numNodes, numFiles;
	unsigned char* p;

	for (i = 0; i < 6; i++)
		w[i] = sms_bswap32(w[i]);
	sms_swap16_inplace(base + 0x18);

	numNodes = w[0];
	numFiles = w[2];

	p = base + w[1];
	for (i = 0; i < numNodes; i++, p += 0x10) {
		sms_swap32_inplace(p + 0x0);
		sms_swap32_inplace(p + 0x4);
		sms_swap16_inplace(p + 0x8);
		sms_swap16_inplace(p + 0xA);
		sms_swap32_inplace(p + 0xC);
	}

	p = base + w[3];
	for (i = 0; i < numFiles; i++, p += 0x14) {
		sms_swap16_inplace(p + 0x0);
		sms_swap16_inplace(p + 0x2);
		sms_swap32_inplace(p + 0x4);
		sms_swap32_inplace(p + 0x8);
		sms_swap32_inplace(p + 0xC);
		/* +0x10 is the runtime data pointer, zero on disc */
	}
}

/* Whole archive in one buffer (JKRMemArchive). Idempotent via the header. */
static inline int port_swap_rarc(void* archive)
{
	unsigned int* hdr = (unsigned int*)archive;
	if (!port_swap_rarc_header(archive))
		return 0;
	port_swap_rarc_info((unsigned char*)archive + hdr[2]);
	return 1;
}

/* ---- raw multi-byte reads from a stream ----------------------------------
 *
 * A few loaders use the untyped `stream.read(&x, sizeof(x))` on big-endian file
 * data. Swap the value right after the read; `elem` is the size of one scalar
 * (2 or 4), `size` the total bytes (TVec3<f32> = 12 bytes of 4-byte elements).
 * u8 values (size 1) are left alone. */
static inline void sms_swap_elems(void* p, int size, int elem)
{
	unsigned char* b = (unsigned char*)p;
	int i;
	if (elem != 2 && elem != 4 && elem != 8)
		return;
	for (i = 0; i + elem <= size; i += elem)
		sms_swap_value(b + i, elem);
}

/* TParamT<T>::load: scalar size is 4 for s32/f32 and TVec3<f32>, 2 for s16,
 * 1 (nothing to do) for u8. */
static inline void sms_swap_param_value(void* p, int size)
{
	if (size >= 4 && (size % 4) == 0)
		sms_swap_elems(p, size, 4);
	else if (size == 2)
		sms_swap16_inplace(p);
}

/* ---- ResTIMG (.bti, 0x20-byte header) ---------------------------------
 *
 *   0x00 u8 format, u8 alphaEnabled, u16 width, u16 height, u8 wrapS, wrapT,
 *   0x08 u8 isIndexTexture, u8 colorFormat, u16 numColors, u32 paletteOffset,
 *   0x10 u8 mipmapEnabled, doEdgeLod, biasClamp, maxAnisotropy, minFilter,
 *        magFilter, s8 minLod, maxLod,
 *   0x18 u8 mipmapCount, u8 pad(0x19), s16 lodBias, u32 imageDataOffset
 *
 * Only the header is swapped; texels and palettes stay in GX order.
 * The header has no magic, so the pad byte at 0x19 doubles as the marker
 * (it is 0 in all 374 shipped .bti files; checked in test_archive). A header
 * that is shared by several users (archive resource fetched repeatedly, copied
 * into J3DTexture) is therefore swapped exactly once. Headers built at runtime
 * in native order must set the marker too (see JUTTexture ctor). */
#define PORT_TIMG_SWAPPED 0x5A

/* Swaps unconditionally (then sets the marker). For callers that know the header
 * is fresh from disc because they have their own once-only marker, such as the J3D
 * model pass (file magic). Needed because the pad byte is NOT always 0 inside
 * TEX1: model headers carry tool garbage there (203 distinct values in 34
 * archives), and coronaBoss map/map/water.bmd texture 0 has 0x5A on disc, so the
 * marker test alone left it big-endian (found by the w10 audit). */
static inline void port_swap_timg_force(void* header)
{
	unsigned char* b = (unsigned char*)header;
	sms_swap16_inplace(b + 0x02);
	sms_swap16_inplace(b + 0x04);
	sms_swap16_inplace(b + 0x0A);
	sms_swap32_inplace(b + 0x0C);
	sms_swap16_inplace(b + 0x1A);
	sms_swap32_inplace(b + 0x1C);
	b[0x19] = PORT_TIMG_SWAPPED;
}

static inline int port_swap_timg(void* header)
{
	unsigned char* b = (unsigned char*)header;
	if (b[0x19] == PORT_TIMG_SWAPPED)
		return 0;
	sms_swap16_inplace(b + 0x02);
	sms_swap16_inplace(b + 0x04);
	sms_swap16_inplace(b + 0x0A);
	sms_swap32_inplace(b + 0x0C);
	sms_swap16_inplace(b + 0x1A);
	sms_swap32_inplace(b + 0x1C);
	b[0x19] = PORT_TIMG_SWAPPED;
	return 1;
}

/* J3D TEX1 block: 'TEX1', u32 size, u16 count (+0x8), u32 texOffset (+0xC),
 * u32 nameOffset; texOffset leads to `count` contiguous 0x20-byte ResTIMG
 * headers (J3DTexture::mResources[i]). The block header may still be raw
 * big-endian or already swapped by a future port_swap_j3d: the magic tells. */
static inline int port_swap_tex1_timgs(void* block)
{
	unsigned char* b = (unsigned char*)block;
	unsigned int count, off, i, n = 0;
	if (*(unsigned int*)b == 0x31584554u) { /* "TEX1" bytes read little-endian: still raw */
		count = sms_be16(b + 8);
		off   = sms_be32(b + 0xC);
	} else {
		count = *(unsigned short*)(b + 8);
		off   = *(unsigned int*)(b + 0xC);
	}
	for (i = 0; i < count; i++)
		n += (unsigned int)port_swap_timg(b + off + i * 0x20);
	return (int)n;
}

/* The same for a raw (still big-endian) TEX1 block whose file is known to be
 * swapped only once (port_swap_j3d_model, guarded by the file magic): swaps every
 * header unconditionally (they are `count` contiguous 0x20-byte headers), so a
 * disc pad byte of 0x5A cannot hide one. */
static inline void port_swap_tex1_timgs_force(void* block)
{
	unsigned char* b = (unsigned char*)block;
	unsigned int count = sms_be16(b + 8), off = sms_be32(b + 0xC), i;
	for (i = 0; i < count; i++)
		port_swap_timg_force(b + off + i * 0x20);
}

/* ---- ResFONT (.bfn) ----------------------------------------------------
 *
 *   0x00 'FONT' 'bfn1' u32 filesize, u32 numBlocks, 0x10 pad, blocks at 0x20.
 *   block: u32 magic, u32 size, then
 *     INF1: u16 fontType, ascent, descent, width, leading, defaultCode
 *     WID1: u16 startCode, endCode, then u8 {kerning, width} pairs (no swap)
 *     GLY1: u16 start, end, cellW, cellH; u32 textureSize; u16 format, rows,
 *           cols, texW, texH, pad; then texture pages (GX order, no swap)
 *     MAP1: u16 method, start, end, numEntries, then u16 entries / pairs
 *
 * Idempotent: the 'FONT' signature reads correctly through a native u32 only
 * after the swap. Both 4-byte halves of the u64 magic are swapped separately. */
#define PORT_FONT_MAGIC_RAW 0x544E4F46u /* "FONT" bytes read little-endian */

static inline int port_swap_font(void* font)
{
	unsigned char* b = (unsigned char*)font;
	unsigned int numBlocks, i, j;
	unsigned char* p;
	if (*(unsigned int*)b != PORT_FONT_MAGIC_RAW)
		return 0;
	for (i = 0; i < 4; i++)
		sms_swap32_inplace(b + i * 4);
	numBlocks = *(unsigned int*)(b + 0xC);
	p         = b + 0x20;
	for (i = 0; i < numBlocks; i++) {
		unsigned int size = sms_be32(p + 4); /* not swapped yet */
		unsigned int magic = sms_be32(p);
		sms_swap32_inplace(p);
		sms_swap32_inplace(p + 4);
		if (magic == 0x494E4631u) { /* INF1 */
			for (j = 0; j < 6; j++)
				sms_swap16_inplace(p + 8 + j * 2);
		} else if (magic == 0x57494431u) { /* WID1 */
			sms_swap16_inplace(p + 8);
			sms_swap16_inplace(p + 0xA);
		} else if (magic == 0x474C5931u) { /* GLY1 */
			for (j = 0; j < 4; j++)
				sms_swap16_inplace(p + 8 + j * 2);
			sms_swap32_inplace(p + 0x10);
			for (j = 0; j < 6; j++)
				sms_swap16_inplace(p + 0x14 + j * 2);
		} else if (magic == 0x4D415031u) { /* MAP1: header + every entry */
			for (j = 8; j + 2 <= size; j += 2)
				sms_swap16_inplace(p + j);
		}
		if (size == 0)
			break;
		p += size;
	}
	return 1;
}

/* ---- resource dispatch by file name -----------------------------------
 *
 * JKRArchive::getResource/readResource call this on the data they return, so
 * every consumer (JKRGetResource, J2D 'TIMG' lookups, ...) sees native headers.
 * Each pass is idempotent, so a resource returned repeatedly is swapped once.
 * BMG (.bmg) is deliberately NOT swapped: every reader (SMSGetMessageData,
 * TMessageLoader, the Talk2D2 tag parser) goes through the big-endian stream
 * reads, and DAT1 text carries inline big-endian tag words. See ENDIAN.md. */
static inline int sms_name_has_ext(const char* name, const char* ext)
{
	int n = 0, e = 0, i;
	while (name[n])
		n++;
	while (ext[e])
		e++;
	if (n < e)
		return 0;
	for (i = 0; i < e; i++) {
		char c = name[n - e + i];
		if (c >= 'A' && c <= 'Z')
			c = (char)(c + 32);
		if (c != ext[i])
			return 0;
	}
	return 1;
}

static inline int port_swap_resource(const char* name, void* data, unsigned int size)
{
	if (!data || !name)
		return 0;
	if (sms_name_has_ext(name, ".bti"))
		return size >= 0x20 ? port_swap_timg(data) : 0;
	if (sms_name_has_ext(name, ".bfn"))
		return size >= 0x20 ? port_swap_font(data) : 0;
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* PORT_ENDIAN_H */
