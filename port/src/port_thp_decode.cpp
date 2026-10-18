// Host THP video frame decoder for the PC port (written for the port; the SDK's
// libs/dolphin/src/thp/THPDec.c is paired-single asm and is not built).
//
// A THP video frame is a baseline JPEG: SOI, DQT, SOF0 (3 components, Y 2x2
// and U/V 1x1 sampling, i.e. 4:2:0), DHT, optional DRI, SOS, then the entropy
// coded scan. THP differences from JFIF, as THPDec.c reads them:
//  - the scan has no 0xFF00 byte stuffing: it is read as a plain bit stream;
//  - a restart interval only realigns the bit stream to the next byte and
//    resets the DC predictors (there are no RSTn marker bytes to skip).
//
// Output matches THPVideoDecode: GX_TF_I8 textures, 8x4-texel tiles of 32 bytes
// in row order (big-endian/tiled exactly as GX and Aurora's I8 decoder expect).
// Samples are plain JPEG samples (Y 0..255, U/V centred on 128); the THP draw
// path's TEV stages do the YUV to RGB conversion.

#include "port_thp_decode.h"

#include <cmath>
#include <cstring>

namespace {

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

// Natural (row-major) index of the k-th coefficient in zigzag order.
const u8 kZigzag[64 + 16] = {
	0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33,
	40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36,
	29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54,
	47, 55, 62, 63,
	// overrun guard for a corrupt run length (k up to 63 + 15)
	63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63,
};

enum {
	kFastBits = 9,
};

struct HuffTable {
	bool valid;
	u8 fastLen[1 << kFastBits]; // 0: code longer than kFastBits (or invalid)
	u8 fastVal[1 << kFastBits];
	s32 maxCode[18];            // largest code of each length, -1 if none
	s32 valOffset[17];          // vals index of a code of length l = code + valOffset[l]
	u8 vals[256];
};

struct Component {
	u8 quant;
	u8 dcTable;
	u8 acTable;
	s32 pred;
};

struct Decoder {
	const u8* p;
	const u8* end;
	u16 width;
	u16 height;
	u32 restartInterval;
	bool quantValid[4];
	float quant[4][64]; // natural order
	HuffTable huff[4][2]; // [table id][0 = DC, 1 = AC]
	Component comp[3];
};

// MSB-first bit reader over the scan. `buf` holds `bits` valid bits at the top.
struct BitReader {
	const u8* p;
	const u8* end;
	u32 buf;
	s32 bits;

	inline void fill()
	{
		while (bits <= 24) {
			u32 b = p < end ? *p++ : 0;
			buf |= b << (24 - bits);
			bits += 8;
		}
	}
	inline void skip(s32 n)
	{
		buf <<= n;
		bits -= n;
	}
	inline u32 get(s32 n) // 1..16 bits
	{
		fill();
		u32 v = buf >> (32 - n);
		skip(n);
		return v;
	}
	inline void alignByte() { skip(bits & 7); }
};

inline u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

// Cosine basis for the separable float IDCT:
// kBasis[x][u] = C(u) / 2 * cos((2x + 1) u pi / 16), C(0) = 1 / sqrt(2), else 1.
struct Basis {
	float c[8][8];
	Basis()
	{
		const double pi = 3.14159265358979323846;
		for (int x = 0; x < 8; x++)
			for (int u = 0; u < 8; u++) {
				double cu = u == 0 ? std::sqrt(0.5) : 1.0;
				c[x][u]   = (float)(cu / 2.0 * std::cos((2 * x + 1) * u * pi / 16.0));
			}
	}
};
const Basis kBasis;

bool buildHuffman(HuffTable& t, const u8* counts, const u8* syms, int numSyms)
{
	std::memset(&t, 0, sizeof(t));
	if (numSyms > 256)
		return false;
	std::memcpy(t.vals, syms, (size_t)numSyms);
	s32 code = 0;
	s32 k    = 0;
	for (int len = 1; len <= 16; len++) {
		s32 n = counts[len - 1];
		if (n != 0) {
			t.valOffset[len] = k - code;
			// fast table for short codes
			if (len <= kFastBits) {
				for (s32 i = 0; i < n; i++) {
					s32 first = (code + i) << (kFastBits - len);
					s32 count = 1 << (kFastBits - len);
					for (s32 j = 0; j < count; j++) {
						t.fastLen[first + j] = (u8)len;
						t.fastVal[first + j] = syms[k + i];
					}
				}
			}
			code += n;
			k += n;
			t.maxCode[len] = code - 1;
			if (code > (1 << len))
				return false; // over-subscribed
		} else {
			t.maxCode[len]   = -1;
			t.valOffset[len] = 0;
		}
		code <<= 1;
	}
	t.maxCode[17] = 0x7FFFFFFF;
	t.valid       = true;
	return true;
}

inline int decodeHuffman(BitReader& br, const HuffTable& t)
{
	br.fill();
	u32 look = br.buf >> (32 - kFastBits);
	if (t.fastLen[look] != 0) {
		br.skip(t.fastLen[look]);
		return t.fastVal[look];
	}
	for (int len = kFastBits + 1; len <= 16; len++) {
		s32 code = (s32)(br.buf >> (32 - len));
		if (code <= t.maxCode[len]) {
			br.skip(len);
			s32 idx = code + t.valOffset[len];
			return (idx >= 0 && idx < 256) ? t.vals[idx] : -1;
		}
	}
	return -1;
}

// JPEG "receive and extend": s magnitude bits to a signed value.
inline s32 receiveExtend(BitReader& br, int s)
{
	s32 v = (s32)br.get(s);
	if (v < (1 << (s - 1)))
		v += (s32)(~0u << s) + 1;
	return v;
}

// Entropy-decodes one block and dequantises it into natural order.
bool decodeBlock(BitReader& br, Decoder& d, Component& c, float* coef)
{
	std::memset(coef, 0, 64 * sizeof(float));
	const float* q = d.quant[c.quant];

	int t = decodeHuffman(br, d.huff[c.dcTable][0]);
	if (t < 0 || t > 16)
		return false;
	if (t != 0)
		c.pred += receiveExtend(br, t);
	coef[0] = (float)c.pred * q[0];

	const HuffTable& ac = d.huff[c.acTable][1];
	for (int k = 1; k < 64;) {
		int rs = decodeHuffman(br, ac);
		if (rs < 0)
			return false;
		int r = rs >> 4;
		int s = rs & 15;
		if (s == 0) {
			if (r != 15)
				break; // end of block
			k += 16;
			continue;
		}
		k += r;
		if (k > 63)
			return false;
		int n   = kZigzag[k];
		coef[n] = (float)receiveExtend(br, s) * q[n];
		k++;
	}
	return true;
}

// Separable float IDCT, level shift and clamp to 8-bit samples.
void inverseDct(const float* in, u8* out)
{
	float tmp[64];
	// rows: coefficient row v, horizontal frequencies u -> pixel columns x
	for (int v = 0; v < 8; v++) {
		const float* row = in + v * 8;
		float* dst       = tmp + v * 8;
		bool acZero      = row[1] == 0 && row[2] == 0 && row[3] == 0 && row[4] == 0
		              && row[5] == 0 && row[6] == 0 && row[7] == 0;
		if (acZero) {
			float dc = row[0] * kBasis.c[0][0];
			for (int x = 0; x < 8; x++)
				dst[x] = dc;
			continue;
		}
		for (int x = 0; x < 8; x++) {
			const float* b = kBasis.c[x];
			dst[x] = row[0] * b[0] + row[1] * b[1] + row[2] * b[2] + row[3] * b[3]
			         + row[4] * b[4] + row[5] * b[5] + row[6] * b[6] + row[7] * b[7];
		}
	}
	// columns: vertical frequencies v -> pixel rows y
	for (int y = 0; y < 8; y++) {
		const float* b = kBasis.c[y];
		for (int x = 0; x < 8; x++) {
			float s = tmp[x] * b[0] + tmp[8 + x] * b[1] + tmp[16 + x] * b[2]
			          + tmp[24 + x] * b[3] + tmp[32 + x] * b[4] + tmp[40 + x] * b[5]
			          + tmp[48 + x] * b[6] + tmp[56 + x] * b[7];
			s += 128.5f;
			out[y * 8 + x] = s <= 0.0f ? 0 : s >= 255.0f ? 255 : (u8)(s32)s;
		}
	}
}

// Stores an 8x8 block at (bx, by) (multiples of 8) into a GX I8 tiled plane.
void storeBlock(u8* plane, u32 pw, u32 ph, u32 bx, u32 by, const u8* px)
{
	u32 tilesPerRow = (pw + 7) / 8;
	u32 cols        = pw - bx < 8 ? pw - bx : 8;
	for (u32 row = 0; row < 8; row++) {
		u32 y = by + row;
		if (y >= ph)
			break;
		u8* dst = plane + ((y >> 2) * tilesPerRow + (bx >> 3)) * 32 + (y & 3) * 8;
		std::memcpy(dst, px + row * 8, cols);
	}
}

int readQuant(Decoder& d, const u8* seg, u32 len)
{
	u32 pos = 0;
	while (pos < len) {
		u8 pq = seg[pos++];
		u8 id = pq & 15;
		if ((pq >> 4) != 0 || id > 3 || pos + 64 > len)
			return 3; // only 8-bit tables
		for (int i = 0; i < 64; i++)
			d.quant[id][kZigzag[i]] = (float)seg[pos + i];
		d.quantValid[id] = true;
		pos += 64;
	}
	return 0;
}

int readHuffman(Decoder& d, const u8* seg, u32 len)
{
	u32 pos = 0;
	while (pos < len) {
		if (pos + 17 > len)
			return 3;
		u8 tc  = seg[pos] >> 4;
		u8 id  = seg[pos] & 15;
		pos++;
		const u8* counts = seg + pos;
		pos += 16;
		int num = 0;
		for (int i = 0; i < 16; i++)
			num += counts[i];
		if (tc > 1 || id > 3 || pos + (u32)num > len)
			return 3;
		if (!buildHuffman(d.huff[id][tc], counts, seg + pos, num))
			return 3;
		pos += (u32)num;
	}
	return 0;
}

int readFrameHeader(Decoder& d, const u8* seg, u32 len)
{
	if (len < 6 + 9)
		return 3;
	if (seg[0] != 8)
		return 10;
	d.height = be16(seg + 1);
	d.width  = be16(seg + 3);
	if (seg[5] != 3)
		return 12;
	for (int i = 0; i < 3; i++) {
		const u8* c = seg + 6 + i * 3;
		if ((i == 0 && c[1] != 0x22) || (i > 0 && c[1] != 0x11))
			return 19;
		if (c[2] > 3)
			return 3;
		d.comp[i].quant = c[2];
	}
	return 0;
}

int readScanHeader(Decoder& d, const u8* seg, u32 len)
{
	if (len < 1 + 6 + 3)
		return 3;
	if (seg[0] != 3)
		return 12;
	for (int i = 0; i < 3; i++) {
		u8 sel          = seg[2 + i * 2];
		d.comp[i].dcTable = sel >> 4;
		d.comp[i].acTable = sel & 15;
		if (d.comp[i].dcTable > 3 || d.comp[i].acTable > 3
		    || !d.huff[d.comp[i].dcTable][0].valid || !d.huff[d.comp[i].acTable][1].valid)
			return 15;
		if (!d.quantValid[d.comp[i].quant])
			return 3;
		d.comp[i].pred = 0;
	}
	return 0;
}

// Walks the marker segments up to and including SOS; leaves d.p at the scan.
int readHeaders(Decoder& d)
{
	bool haveFrame = false;
	for (;;) {
		if (d.p >= d.end || *d.p++ != 0xFF)
			return 3;
		while (d.p < d.end && *d.p == 0xFF)
			d.p++;
		if (d.p >= d.end)
			return 3;
		u8 marker = *d.p++;
		if (marker == 0xD8) // SOI
			continue;
		if (d.p + 2 > d.end)
			return 3;
		u32 len = be16(d.p);
		if (len < 2 || d.p + len > d.end)
			return 3;
		const u8* seg = d.p + 2;
		u32 segLen    = len - 2;
		d.p += len;
		int err = 0;
		switch (marker) {
		case 0xC4:
			err = readHuffman(d, seg, segLen);
			break;
		case 0xC0:
			err       = readFrameHeader(d, seg, segLen);
			haveFrame = err == 0;
			break;
		case 0xDB:
			err = readQuant(d, seg, segLen);
			break;
		case 0xDD:
			if (segLen < 2)
				return 3;
			d.restartInterval = be16(seg);
			break;
		case 0xDA:
			if (!haveFrame)
				return 3;
			return readScanHeader(d, seg, segLen);
		default:
			if ((marker >= 0xE0 && marker <= 0xEF) || marker == 0xFE)
				break; // APPn / COM: skipped
			return 11;
		}
		if (err != 0)
			return err;
	}
}

} // namespace

extern "C" int port_thp_video_decode(const void* data, uint32_t size, uint8_t* tileY,
                                     uint8_t* tileU, uint8_t* tileV, uint32_t width,
                                     uint32_t height)
{
	if (data == nullptr || tileY == nullptr || tileU == nullptr || tileV == nullptr)
		return 25;

	static thread_local Decoder d;
	std::memset(&d, 0, sizeof(d));
	d.p   = (const u8*)data;
	d.end = d.p + size;

	int err = readHeaders(d);
	if (err != 0)
		return err;
	if ((width != 0 && width != d.width) || (height != 0 && height != d.height))
		return 21;

	BitReader br;
	br.p    = d.p;
	br.end  = d.end;
	br.buf  = 0;
	br.bits = 0;

	const u32 w = d.width, h = d.height;
	const u32 cw = w / 2, ch = h / 2;
	const u32 mcusX = (w + 15) / 16, mcusY = (h + 15) / 16;
	u32 restartLeft = d.restartInterval;

	float coef[64];
	u8 px[64];
	for (u32 my = 0; my < mcusY; my++) {
		for (u32 mx = 0; mx < mcusX; mx++) {
			// four Y blocks (TL, TR, BL, BR), then U, then V
			for (int b = 0; b < 4; b++) {
				if (!decodeBlock(br, d, d.comp[0], coef))
					return 20;
				inverseDct(coef, px);
				storeBlock(tileY, w, h, mx * 16 + (b & 1) * 8, my * 16 + (b >> 1) * 8, px);
			}
			if (!decodeBlock(br, d, d.comp[1], coef))
				return 20;
			inverseDct(coef, px);
			storeBlock(tileU, cw, ch, mx * 8, my * 8, px);
			if (!decodeBlock(br, d, d.comp[2], coef))
				return 20;
			inverseDct(coef, px);
			storeBlock(tileV, cw, ch, mx * 8, my * 8, px);

			if (d.restartInterval != 0 && --restartLeft == 0) {
				restartLeft = d.restartInterval;
				br.alignByte();
				d.comp[0].pred = d.comp[1].pred = d.comp[2].pred = 0;
			}
		}
	}
	return 0;
}
