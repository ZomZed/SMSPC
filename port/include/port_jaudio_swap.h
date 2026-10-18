/*
 * port_jaudio_swap.h - in-place byte-swap passes for JAudio data (see
 * port/ENDIAN.md). C++ header, self-contained (host tests include it directly
 * after port_endian.h; port_compat.h includes it for the game build).
 *
 * Every pass is pointer-size clean: it only touches the file bytes and never
 * stores a pointer into them. Offsets are kept as offsets (the parsers add the
 * base at use through TOffset<T>::ptr / JSUConvertOffsetToPtr).
 *
 * Shared sub-structures are swapped once: each pass keeps a one-flag-per-u16
 * map of what it already swapped, so a node reached twice is not undone.
 *
 *   port_swap_ibnk(data)              IBNK instrument bank   (JASBNKParser)
 *   port_swap_wsys(data)              WSYS wave system       (JASWSParser)
 *   port_swap_jaisoundtable(p, size)  SE / sequence / stream info table
 *   port_swap_fxscene(p, size)        FX scene table (FxlineConfig_ arrays)
 *   port_swap_barc(p, size)           sequence archive header (JASVload)
 *   port_swap_stream_entries(p, size) stream list (0x30-byte entries)
 *
 * ibnk and wsys are idempotent (the magic is the marker). The others must be
 * run exactly once on a freshly loaded copy.
 */
#ifndef PORT_JAUDIO_SWAP_H
#define PORT_JAUDIO_SWAP_H

#include "port_endian.h"

#include <stdlib.h>
#include <string.h>

namespace port_ja {

struct Swapper {
	unsigned char* base;
	unsigned size;
	unsigned char* seen;  /* one flag per 2-byte unit: fields swapped */
	unsigned char* nodes; /* one flag per 2-byte unit: structs visited */

	Swapper(void* p, unsigned sz)
	    : base((unsigned char*)p)
	    , size(sz)
	    , seen((unsigned char*)calloc(sz / 2 + 2, 1))
	    , nodes((unsigned char*)calloc(sz / 2 + 2, 1))
	{
	}
	~Swapper() { free(seen); free(nodes); }

	/* true the first time a struct at off is visited */
	bool node(unsigned off, unsigned len)
	{
		if (!ok(off, len) || nodes[off >> 1])
			return false;
		nodes[off >> 1] = 1;
		return true;
	}

	bool ok(unsigned off, unsigned len) const { return seen && nodes && off <= size && len <= size - off; }

	/* true when [off, off+len) is in range and was not swapped yet; marks it */
	bool claim(unsigned off, unsigned len)
	{
		if (!ok(off, len) || len == 0)
			return false;
		unsigned u0 = off >> 1, u1 = (off + len - 1) >> 1;
		if (seen[u0])
			return false;
		for (unsigned u = u0; u <= u1; u++)
			seen[u] = 1;
		return true;
	}
	bool s16(unsigned off)
	{
		if (!claim(off, 2))
			return false;
		sms_swap16_inplace(base + off);
		return true;
	}
	bool s32(unsigned off)
	{
		if (!claim(off, 4))
			return false;
		sms_swap32_inplace(base + off);
		return true;
	}
	/* native read of a value that has been swapped (or is swapped now) */
	unsigned n32(unsigned off)
	{
		unsigned v = 0;
		if (ok(off, 4))
			memcpy(&v, base + off, 4);
		return v;
	}
	/* swap (once) and read a u32 */
	unsigned w32(unsigned off)
	{
		s32(off);
		return n32(off);
	}
};

#define PORT_JA_MAGIC_LE(a, b, c, d) \
	((unsigned)(a) | ((unsigned)(b) << 8) | ((unsigned)(c) << 16) | ((unsigned)(d) << 24))

/* ---- IBNK bank --------------------------------------------------------- */

/* Oscillator envelope table: (mode, time, value) s16 triples, ending with the
 * first triple whose mode is above 0xA (same rule as getOscTableEndPtr). */
static inline void swap_osc_table(Swapper& s, unsigned off)
{
	if (!s.ok(off, 6) || s.seen[off >> 1])
		return;
	for (;;) {
		if (!s.ok(off, 6))
			return;
		int mode = (short)sms_be16(s.base + off);
		s.s16(off);
		s.s16(off + 2);
		s.s16(off + 4);
		off += 6;
		if (mode > 0xA)
			return;
	}
}

static inline void swap_ibnk_vmap(Swapper& s, unsigned off)
{
	if (!off || !s.node(off, 0x10))
		return;
	s.s32(off + 4);
	s.s32(off + 8);
	s.s32(off + 0xC);
}
static inline void swap_ibnk_rand(Swapper& s, unsigned off) /* TRand */
{
	if (!off || !s.node(off, 0xC))
		return;
	s.s32(off + 4);
	s.s32(off + 8);
}

static inline int port_swap_ibnk_impl(void* data)
{
	unsigned char* p = (unsigned char*)data;
	unsigned magic;
	memcpy(&magic, p, 4);
	if (magic != PORT_JA_MAGIC_LE('I', 'B', 'N', 'K'))
		return 0; /* already native (or not a bank) */
	unsigned size = sms_be32(p + 4);
	if (size < 0x3E4 || size > 0x1000000)
		return 0;
	Swapper s(p, size);

	/* header words (magic, size, vir id, 'BANK' chunk header) */
	for (unsigned o = 4; o < 0x24; o += 4) /* the magic at 0 flips last */
		s.s32(o);
	for (int i = 0; i < 0x80; i++) {
		unsigned io = s.w32(0x24 + 4 * i);
		if (!io || !s.node(io, 0x2C))
			continue;
		/* TInst */
		s.s32(io + 0x8);
		s.s32(io + 0xC);
		for (int j = 0; j < 2; j++) {
			unsigned oo = s.w32(io + 0x10 + 4 * j); /* TOsc */
			if (oo && s.node(oo, 0x18)) {
				s.s32(oo + 4);
				unsigned ads = s.w32(oo + 8);
				unsigned rel = s.w32(oo + 0xC);
				s.s32(oo + 0x10);
				s.s32(oo + 0x14);
				if (ads)
					swap_osc_table(s, ads);
				if (rel)
					swap_osc_table(s, rel);
			}
		}
		for (int j = 0; j < 2; j++)
			swap_ibnk_rand(s, s.w32(io + 0x18 + 4 * j));
		for (int j = 0; j < 2; j++) {
			unsigned so = s.w32(io + 0x20 + 4 * j); /* TSense */
			if (so && s.node(so, 0xC)) {
				s.s32(so + 4);
				s.s32(so + 8);
			}
		}
		unsigned nkey = s.w32(io + 0x28);
		if (nkey > 0x80)
			nkey = 0x80;
		for (unsigned j = 0; j < nkey; j++) {
			unsigned ko = s.w32(io + 0x2C + 4 * j); /* TKeymap */
			if (!ko || !s.node(ko, 8))
				continue;
			unsigned nv = s.w32(ko + 4);
			if (nv > 0x80)
				nv = 0x80;
			for (unsigned k = 0; k < nv; k++)
				swap_ibnk_vmap(s, s.w32(ko + 8 + 4 * k));
		}
	}
	for (int i = 0; i < 12; i++) {
		unsigned po = s.w32(0x3B4 + 4 * i); /* TPerc */
		if (!po || !s.node(po, 0x288))
			continue;
		s.s32(po); /* 'PERC' / 'PER2' becomes the native multi-char value */
		bool per2 = s.n32(po) == (unsigned)(('P' << 24) | ('E' << 16) | ('R' << 8) | '2');
		for (int j = 0; j < 0x80; j++) {
			unsigned mo = s.w32(po + 0x88 + 4 * j); /* TPmap */
			if (!mo || !s.node(mo, 0x14))
				continue;
			s.s32(mo);
			s.s32(mo + 4);
			swap_ibnk_rand(s, s.w32(mo + 8));
			swap_ibnk_rand(s, s.w32(mo + 0xC));
			unsigned nv = s.w32(mo + 0x10);
			if (nv > 0x80)
				nv = 0x80;
			for (unsigned k = 0; k < nv; k++)
				swap_ibnk_vmap(s, s.w32(mo + 0x14 + 4 * k));
		}
		if (per2)
			for (int j = 0; j < 0x80; j++)
				s.s16(po + 0x308 + 2 * j); /* mRelease; mPan is s8 */
	}
	/* flip the marker last: the magic is now native 'IBNK' */
	sms_swap32_inplace(p);
	return 1;
}

/* ---- WSYS wave system ---------------------------------------------------- */

static inline void swap_wsys_ctrl(Swapper& s, unsigned off, bool waves)
{
	if (!off || !s.node(off, 8))
		return;
	s.s32(off); /* 'C-DF' / 'C-EX' / 'C-ST' */
	unsigned n = s.w32(off + 4);
	if (n > 0x10000)
		return;
	for (unsigned i = 0; i < n; i++) {
		unsigned wo = s.w32(off + 8 + 4 * i);
		if (waves && wo)
			s.s32(wo); /* TCtrlWave.unk0 (rest of the entry is unused) */
	}
}

static inline int port_swap_wsys_impl(void* data)
{
	unsigned char* p = (unsigned char*)data;
	unsigned magic;
	memcpy(&magic, p, 4);
	if (magic != PORT_JA_MAGIC_LE('W', 'S', 'Y', 'S'))
		return 0;
	unsigned size = sms_be32(p + 4);
	if (size < 0x18 || size > 0x1000000)
		return 0;
	Swapper s(p, size);
	for (unsigned o = 4; o < 0x18; o += 4)
		s.s32(o);
	unsigned winf = s.n32(0x10), wbct = s.n32(0x14);

	/* WINF: magic, archive count, archive offsets */
	if (winf && s.ok(winf, 8)) {
		s.s32(winf);
		unsigned na = s.w32(winf + 4);
		if (na > 0x400)
			na = 0;
		for (unsigned i = 0; i < na; i++) {
			unsigned ao = s.w32(winf + 8 + 4 * i); /* TWaveArchive */
			if (!ao || !s.node(ao, 0x78))
				continue;
			unsigned nw = s.w32(ao + 0x70); /* wave count sits at the end of the name */
			if (nw > 0x10000)
				continue;
			for (unsigned k = 0; k < nw; k++) {
				unsigned wo = s.w32(ao + 0x74 + 4 * k); /* TWave, 0x2C bytes */
				if (!wo || !s.node(wo, 0x2C))
					continue;
				s.s32(wo + 4);
				for (unsigned f = 8; f <= 0x1C; f += 4)
					s.s32(wo + f);
				s.s16(wo + 0x20);
				s.s16(wo + 0x22);
				s.s32(wo + 0x28);
			}
		}
	}
	/* WBCT: magic, ?, group count, scene offsets; SCNE -> C-DF/C-EX/C-ST */
	if (wbct && s.ok(wbct, 0xC)) {
		s.s32(wbct);
		s.s32(wbct + 4);
		unsigned ng = s.w32(wbct + 8);
		if (ng > 0x400)
			ng = 0;
		for (unsigned g = 0; g < ng; g++) {
			unsigned so = s.w32(wbct + 0xC + 4 * g);
			if (!so || !s.node(so, 0x18))
				continue;
			s.s32(so);
			s.s32(so + 4);
			s.s32(so + 8);
			unsigned cdf = s.w32(so + 0xC), cex = s.w32(so + 0x10), cst = s.w32(so + 0x14);
			swap_wsys_ctrl(s, cdf, true);
			swap_wsys_ctrl(s, cex, false);
			swap_wsys_ctrl(s, cst, false);
		}
	}
	sms_swap32_inplace(p); /* marker last */
	return 1;
}

/* ---- JAI sound info table (SE / sequence / stream) ----------------------- */

/* 0x00: u8 x4 (byte 3 = category count), 0x04: u16 total, 0x06: 18 x (u16 count,
 * u16 start), 0x50: JAISoundInfo[] (0x10 each: u32 swBit, u8, u8, u16, f32, u8, u8) */
static inline void port_swap_jaisoundtable(void* data, unsigned size)
{
	unsigned char* p = (unsigned char*)data;
	if (!p || size < 0x50)
		return;
	for (unsigned o = 4; o < 0x4E; o += 2)
		sms_swap16_inplace(p + o);
	unsigned n = (size - 0x50) / 0x10;
	for (unsigned i = 0; i < n; i++) {
		unsigned char* e = p + 0x50 + 0x10 * i;
		sms_swap32_inplace(e + 0);
		sms_swap16_inplace(e + 6);
		sms_swap32_inplace(e + 8);
	}
}

/* ---- FX scene table ------------------------------------------------------ */

/* u32 sceneMax, u32 bufferMax[4], u32 sceneOffset[sceneMax]; each scene is 4
 * FxlineConfig_ (0x20 bytes: u8, u16@2, s16@4, u16@6, s16@8, int@C, s16[8]@10). */
static inline void port_swap_fxscene(void* data, unsigned size)
{
	unsigned char* p = (unsigned char*)data;
	if (!p || size < 0x14)
		return;
	Swapper s(p, size);
	for (unsigned o = 0; o < 0x14; o += 4)
		s.s32(o);
	unsigned n = s.n32(0);
	if (n > 0x100)
		return;
	for (unsigned i = 0; i < n; i++) {
		unsigned so = s.w32(0x14 + 4 * i);
		for (unsigned l = 0; l < 4; l++) {
			unsigned lo = so + 0x20 * l;
			if (!s.node(lo, 0x20))
				continue;
			s.s16(lo + 2);
			s.s16(lo + 4);
			s.s16(lo + 6);
			s.s16(lo + 8);
			s.s32(lo + 0xC);
			for (unsigned k = 0; k < 8; k++)
				s.s16(lo + 0x10 + 2 * k);
		}
	}
}

/* ---- BARC sequence archive header (JASVload) ----------------------------- */

/* 0x00 'BARC' + '----' (not read), 0x08 u32 (runtime slot), 0x0C u32 entry
 * count, 0x10 char name[0x10]; then count x 0x20-byte entries: char name[0xC],
 * u16 @C, u16 @E (next link), u32 @10, @14, @18 (offset in the archive), @1C (size). */
static inline void port_swap_barc(void* data, unsigned size)
{
	unsigned char* p = (unsigned char*)data;
	if (!p || size < 0x20)
		return;
	sms_swap32_inplace(p + 8);
	sms_swap32_inplace(p + 0xC);
	unsigned n;
	memcpy(&n, p + 0xC, 4);
	if (n > (size - 0x20) / 0x20)
		n = (size - 0x20) / 0x20;
	for (unsigned i = 0; i < n; i++) {
		unsigned char* e = p + 0x20 + 0x20 * i;
		sms_swap16_inplace(e + 0xC);
		sms_swap16_inplace(e + 0xE);
		sms_swap32_inplace(e + 0x10);
		sms_swap32_inplace(e + 0x14);
		sms_swap32_inplace(e + 0x18);
		sms_swap32_inplace(e + 0x1C);
	}
}

/* ---- stream list ---------------------------------------------------------- */

/* 0x30-byte entries: u8 unk0[0x10], char name[0x10], u32 unk20, u32 unk24[3].
 * unk20 was a void* slot in the decomp header (a pointer in a file struct):
 * the port declares it u32 so the entry stays 0x30 bytes on 64-bit. */
static inline void port_swap_stream_entries(void* data, unsigned size)
{
	unsigned char* p = (unsigned char*)data;
	for (unsigned i = 0; p && (i + 1) * 0x30 <= size; i++) {
		unsigned char* e = p + 0x30 * i;
		for (unsigned f = 0x20; f < 0x30; f += 4)
			sms_swap32_inplace(e + f);
	}
}

/* ---- .bas animation sound data (JAIAnimeSoundData) ------------------------ */

/* u16 count, u8 pad[6] (byte 7 is 0 in all 8314 .bas files of the game), then
 * count x 0x20 bytes: u32 soundID, f32 start, f32 end, f32 pitch, u32 flags,
 * u8 x 8+. Idempotent: byte 7 is set to PORT_BAS_SWAPPED after the swap (the
 * data is fetched from the archive on every animation change). */
#define PORT_BAS_SWAPPED 0xA5
static inline int port_swap_bas(void* data)
{
	unsigned char* p = (unsigned char*)data;
	if (!p || p[7] == PORT_BAS_SWAPPED)
		return 0;
	sms_swap16_inplace(p);
	unsigned n = (unsigned)p[0] | ((unsigned)p[1] << 8);
	for (unsigned i = 0; i < n; i++) {
		unsigned char* e = p + 8 + 0x20 * i;
		sms_swap32_inplace(e + 0x0);
		sms_swap32_inplace(e + 0x4);
		sms_swap32_inplace(e + 0x8);
		sms_swap32_inplace(e + 0xC);
		sms_swap32_inplace(e + 0x10);
	}
	p[7] = PORT_BAS_SWAPPED;
	return 1;
}

/* ---- sequence-embedded s16 tables ----------------------------------------- */

/* Oscillator envelope tables inside sequence data are big-endian s16 triples
 * that the oscillator reads natively. Returns a native-order copy (cached by
 * address, revalidated against the raw bytes so a reused sequence buffer gets a
 * fresh copy). Never freed: tables are tiny. */
static inline short* seq_osc_table_native(const void* raw)
{
	enum { kSlots = 256, kMax = 96 };
	struct Ent {
		const void* raw;
		unsigned len;
		unsigned char bytes[kMax * 2];
		short native[kMax];
	};
	static Ent* tab[kSlots];
	const unsigned char* r = (const unsigned char*)raw;
	unsigned n = 0;
	for (;;) { /* length in s16 units, terminator triple included */
		if (n + 3 > kMax)
			break;
		int mode = (short)sms_be16(r + 2 * n);
		n += 3;
		if (mode > 0xA)
			break;
	}
	unsigned h = (unsigned)(((size_t)raw >> 2) * 2654435761u) % kSlots;
	for (unsigned probe = 0; probe < kSlots; probe++) {
		Ent*& e = tab[(h + probe) % kSlots];
		if (!e) {
			e = (Ent*)calloc(1, sizeof(Ent));
		} else if (e->raw != raw) {
			continue;
		}
		if (e->raw != raw || e->len != n || memcmp(e->bytes, r, n * 2) != 0) {
			e->raw = raw;
			e->len = n;
			memcpy(e->bytes, r, n * 2);
			for (unsigned i = 0; i < n; i++)
				e->native[i] = (short)sms_be16(r + 2 * i);
		}
		return e->native;
	}
	return (short*)raw; /* table full: cannot happen with ~100 sequences */
}

} /* namespace port_ja */

static inline int port_swap_ibnk(void* data) { return port_ja::port_swap_ibnk_impl(data); }
static inline int port_swap_wsys(void* data) { return port_ja::port_swap_wsys_impl(data); }
using port_ja::port_swap_barc;
using port_ja::port_swap_bas;
using port_ja::port_swap_fxscene;
using port_ja::port_swap_jaisoundtable;
using port_ja::port_swap_stream_entries;
using port_ja::seq_osc_table_native;

#endif /* PORT_JAUDIO_SWAP_H */
