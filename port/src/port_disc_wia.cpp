// RVZ, WIA and GCZ disc images (Dolphin's compressed formats) for the PC port. (w9)
// Written from the public format descriptions; decompression uses the real libraries:
// zstd (Aurora's build), zlib (Aurora's build), bzip2 1.0.8 (port_deps), and liblzma when
// the build has it (PORT_DISC_HAVE_LZMA).
//
// WIA / RVZ (big-endian):
//   file head (0x48): magic "WIA\1" / "RVZ\1", version, compatible version, size of the
//     disc struct, SHA-1, plain disc size (u64), file size (u64), SHA-1.
//   disc struct (0xDC): disc type (1 GameCube, 2 Wii), compression (0 none, 1 purge, 2 bzip2,
//     3 LZMA, 4 LZMA2, 5 zstd), level, chunk size, the disc's first 0x80 bytes, partition
//     table (Wii only), raw data table (count, offset, stored size), group table (count,
//     offset, stored size), and up to 7 bytes of compressor properties (LZMA/LZMA2).
//   The raw data and group tables are stored compressed like the data.
//   raw data entry (0x18): disc offset, size, first group, group count. Its groups cover the
//     disc from the offset rounded down to 0x8000, one chunk each (the last one shorter).
//   group entry: offset / 4 and stored size (WIA: 8 bytes); RVZ adds a third word and uses
//     bit 31 of the size as "compressed" (otherwise stored as is). Size 0 = all zeros.
//     A non-zero third word is the size of RVZ-packed data: the decompressed group is a list
//     of records, u32 size then either that many literal bytes, or (bit 31 set) a 68-byte
//     seed for the disc's padding generator, which recreates (size & 0x7FFFFFFF) bytes.
//   Padding ("junk") generator: Nintendo's lagged Fibonacci generator, x[n] = x[n-521] ^
//     x[n-32] over 32-bit words; the 17 seed words are expanded to 521, each word w is output
//     as the bytes w>>24, w>>18, w>>8, w, and the state is advanced 4 times before the first
//     output. The disc reseeds it at every 0x8000-byte block, so a record starting at disc
//     offset o uses the stream from byte (o % 0x8000) on. (Checked against the real padding
//     of the GMSE01 disc: whole and partial blocks regenerate byte for byte.)
// GCZ (little-endian): magic 0xB10BC001, sub type, compressed data size (u64), disc size
//   (u64), block size, block count; then a u64 offset per block (bit 63 = stored
//   uncompressed) and a u32 checksum per block; then the zlib-compressed blocks.
#if defined(SMS_PORT_AURORA) || defined(PORT_DISC_STANDALONE)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "port_disc.h"

#ifndef PORT_DISC_NO_BZIP2
#define BZ_EXPORT // static library, plain declarations (not DLL function pointers)
#include <bzlib.h>
#endif
#include <zlib.h>
#include <zstd.h>
#ifdef PORT_DISC_HAVE_LZMA
#include <lzma.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace {

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint64_t le64(const uint8_t* p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

struct File {
	HANDLE h = INVALID_HANDLE_VALUE;
	uint64_t size = 0;
	~File()
	{
		if (h != INVALID_HANDLE_VALUE)
			CloseHandle(h);
	}
	bool open(const std::string& path)
	{
		std::wstring w(path.size() + 1, L'\0');
		int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), (int)path.size(), &w[0], (int)w.size());
		w.resize(n > 0 ? n : 0);
		h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
		                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
		if (h == INVALID_HANDLE_VALUE)
			return false;
		LARGE_INTEGER sz;
		GetFileSizeEx(h, &sz);
		size = (uint64_t)sz.QuadPart;
		return true;
	}
	// Positioned read of exactly `len` bytes.
	bool read(uint64_t off, void* dst, size_t len) const
	{
		uint8_t* out = (uint8_t*)dst;
		while (len > 0) {
			OVERLAPPED ov = {};
			ov.Offset     = (DWORD)off;
			ov.OffsetHigh = (DWORD)(off >> 32);
			DWORD want    = (DWORD)std::min<size_t>(len, 0x40000000);
			DWORD got     = 0;
			if (!ReadFile(h, out, want, &got, &ov) || got == 0)
				return false;
			out += got;
			off += got;
			len -= got;
		}
		return true;
	}
};

// ---- padding generator --------------------------------------------------------------
class Junk {
public:
	static const int K = 521, J = 32;

	void seed(const uint8_t* seed68)
	{
		for (int i = 0; i < 17; i++)
			w[i] = be32(seed68 + i * 4);
		for (int i = 17; i < K; i++)
			w[i] = (w[i - 17] << 23) ^ (w[i - 16] >> 9) ^ w[i - 1];
		// Output byte 1 comes from bits 18..25: move them into place once here.
		for (uint32_t& x : w)
			x = (x & 0xFF00FFFFu) | ((x >> 2) & 0x00FF0000u);
		for (int i = 0; i < 4; i++)
			step();
		pos = 0;
	}
	void skip(size_t n)
	{
		pos += n;
		while (pos >= K * 4) {
			step();
			pos -= K * 4;
		}
	}
	void get(uint8_t* out, size_t n)
	{
		while (n > 0) {
			const uint32_t x = w[pos >> 2];
			*out++           = (uint8_t)(x >> (24 - 8 * (pos & 3)));
			--n;
			if (++pos == K * 4) {
				step();
				pos = 0;
			}
		}
	}

private:
	void step()
	{
		for (int i = 0; i < J; i++)
			w[i] ^= w[i + K - J];
		for (int i = J; i < K; i++)
			w[i] ^= w[i - J];
	}
	uint32_t w[K];
	size_t pos = 0;
};

// ---- decompression ----------------------------------------------------------------
enum Method { M_NONE = 0, M_PURGE = 1, M_BZIP2 = 2, M_LZMA = 3, M_LZMA2 = 4, M_ZSTD = 5 };

const char* methodName(uint32_t m)
{
	static const char* const kNames[] = { "none", "purge", "bzip2", "LZMA", "LZMA2", "Zstandard" };
	return m < 6 ? kNames[m] : "unknown";
}

bool methodSupported(uint32_t m)
{
#ifdef PORT_DISC_HAVE_LZMA
	return m <= M_ZSTD;
#else
	return m <= M_ZSTD && m != M_LZMA && m != M_LZMA2
#ifdef PORT_DISC_NO_BZIP2
	       && m != M_BZIP2
#endif
	    ;
#endif
}

// Decompresses `src` into exactly `outLen` bytes.
bool decompress(uint32_t method, const uint8_t* props, uint32_t propsLen, const uint8_t* src, size_t srcLen,
                uint8_t* out, size_t outLen)
{
	switch (method) {
	case M_NONE:
		if (srcLen < outLen)
			return false;
		std::memcpy(out, src, outLen);
		return true;
	case M_PURGE: {
		// Segments (u32 offset, u32 size, data) over a zero-filled buffer, then a SHA-1.
		if (srcLen < 20)
			return false;
		std::memset(out, 0, outLen);
		size_t p = 0, end = srcLen - 20;
		while (p < end) {
			if (end - p < 8)
				return false;
			const uint32_t off = be32(src + p), n = be32(src + p + 4);
			p += 8;
			if (n > end - p || (uint64_t)off + n > outLen)
				return false;
			std::memcpy(out + off, src + p, n);
			p += n;
		}
		return true;
	}
#ifndef PORT_DISC_NO_BZIP2
	case M_BZIP2: {
		unsigned int n = (unsigned int)outLen;
		return BZ2_bzBuffToBuffDecompress((char*)out, &n, (char*)src, (unsigned int)srcLen, 0, 0) == BZ_OK
		       && n == outLen;
	}
#endif
	case M_ZSTD: {
		const size_t n = ZSTD_decompress(out, outLen, src, srcLen);
		return !ZSTD_isError(n) && n == outLen;
	}
#ifdef PORT_DISC_HAVE_LZMA
	case M_LZMA:
	case M_LZMA2: {
		lzma_filter filters[2] = {};
		filters[0].id          = method == M_LZMA ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2;
		filters[1].id          = LZMA_VLI_UNKNOWN;
		if (lzma_properties_decode(&filters[0], nullptr, props, propsLen) != LZMA_OK)
			return false;
		lzma_stream s = LZMA_STREAM_INIT;
		lzma_ret r    = lzma_raw_decoder(&s, filters);
		free(filters[0].options);
		if (r != LZMA_OK)
			return false;
		s.next_in   = src;
		s.avail_in  = srcLen;
		s.next_out  = out;
		s.avail_out = outLen;
		r           = lzma_code(&s, LZMA_FINISH);
		const bool ok = (r == LZMA_OK || r == LZMA_STREAM_END) && s.avail_out == 0;
		lzma_end(&s);
		return ok;
	}
#endif
	default:
		return false;
	}
}

// Small cache of decoded blocks (WIA/RVZ groups, GCZ blocks), most recent first.
struct BlockCache {
	struct Item {
		uint64_t key;
		std::vector<uint8_t> data;
	};
	std::list<Item> items;
	size_t limit = 8;
	std::vector<uint8_t>* find(uint64_t key)
	{
		for (auto it = items.begin(); it != items.end(); ++it)
			if (it->key == key) {
				items.splice(items.begin(), items, it);
				return &items.front().data;
			}
		return nullptr;
	}
	std::vector<uint8_t>& add(uint64_t key)
	{
		if (items.size() >= limit)
			items.pop_back();
		items.push_front(Item{ key, {} });
		return items.front().data;
	}
};

// ---- WIA / RVZ ----------------------------------------------------------------------
class WiaReader : public PortDiscReader {
public:
	bool open(const std::string& path, std::string* err);
	uint64_t size() const override { return discSize; }
	int32_t read(uint64_t offset, void* dst, int32_t length) override;

private:
	struct Raw {
		uint64_t off, end, base; // base: off rounded down to 0x8000
		uint32_t group, groups;
	};
	struct Group {
		uint64_t off;
		uint32_t size;
		bool compressed;
		uint32_t packed;
	};
	bool readTable(uint64_t off, uint32_t stored, size_t outLen, std::vector<uint8_t>* out);
	const std::vector<uint8_t>* group(size_t rawIndex, uint32_t k, uint64_t* start);

	File f;
	bool rvz = false;
	uint64_t discSize = 0;
	uint32_t method = 0, chunk = 0;
	uint8_t props[7] = {};
	uint32_t propsLen = 0;
	uint8_t dhead[0x80] = {};
	std::vector<Raw> raws;
	std::vector<Group> groups;
	std::mutex lock;
	BlockCache cache;
};

bool WiaReader::readTable(uint64_t off, uint32_t stored, size_t outLen, std::vector<uint8_t>* out)
{
	std::vector<uint8_t> src(stored);
	out->resize(outLen);
	return f.read(off, src.data(), stored) && decompress(method, props, propsLen, src.data(), stored, out->data(), outLen);
}

bool WiaReader::open(const std::string& path, std::string* err)
{
	if (!f.open(path))
		return false;
	uint8_t head[0x48];
	if (f.size < sizeof(head) || !f.read(0, head, sizeof(head)))
		return false;
	if (std::memcmp(head, "WIA\x01", 4) == 0)
		rvz = false;
	else if (std::memcmp(head, "RVZ\x01", 4) == 0)
		rvz = true;
	else
		return false; // not WIA/RVZ
	const char* fmt        = rvz ? "RVZ" : "WIA";
	const uint32_t discLen = be32(head + 12);
	discSize               = be64(head + 0x24);
	uint8_t d[0xDC]        = {};
	if (discLen < 0xDC || !f.read(0x48, d, sizeof(d))) {
		*err = std::string("damaged ") + fmt + " file (disc header)";
		return false;
	}
	const uint32_t discType = be32(d);
	method                  = be32(d + 4);
	chunk                   = be32(d + 12);
	std::memcpy(dhead, d + 16, 0x80);
	const uint32_t nPart   = be32(d + 0x90);
	const uint32_t nRaw    = be32(d + 0xB4);
	const uint64_t rawOff  = be64(d + 0xB8);
	const uint32_t rawSize = be32(d + 0xC0);
	const uint32_t nGroup  = be32(d + 0xC4);
	const uint64_t grpOff  = be64(d + 0xC8);
	const uint32_t grpSize = be32(d + 0xD0);
	propsLen               = std::min<uint32_t>(d[0xD4], 7);
	std::memcpy(props, d + 0xD5, propsLen);
	if (discType != 1 || nPart != 0) {
		*err = std::string("a Wii disc (") + fmt + "); this port needs the GameCube game";
		return false;
	}
	if (!methodSupported(method)) {
		*err = std::string(fmt) + " compressed with " + methodName(method)
		       + ", which this build cannot read; convert it again in Dolphin with Zstandard (RVZ) or to ISO";
		return false;
	}
	if (chunk < 0x8000 || chunk % 0x8000 != 0 || discSize == 0) {
		*err = std::string("damaged ") + fmt + " file (chunk size)";
		return false;
	}
	std::vector<uint8_t> t;
	if (!readTable(rawOff, rawSize, (size_t)nRaw * 0x18, &t)) {
		*err = std::string("damaged ") + fmt + " file (raw data table)";
		return false;
	}
	for (uint32_t i = 0; i < nRaw; i++) {
		const uint8_t* e = &t[i * 0x18];
		Raw r;
		r.off    = be64(e);
		r.end    = r.off + be64(e + 8);
		r.base   = r.off & ~0x7FFFull;
		r.group  = be32(e + 16);
		r.groups = be32(e + 20);
		if (r.end > r.off)
			raws.push_back(r);
	}
	std::sort(raws.begin(), raws.end(), [](const Raw& a, const Raw& b) { return a.off < b.off; });
	const size_t gsz = rvz ? 12 : 8;
	if (!readTable(grpOff, grpSize, nGroup * gsz, &t)) {
		*err = std::string("damaged ") + fmt + " file (group table)";
		return false;
	}
	groups.resize(nGroup);
	for (uint32_t i = 0; i < nGroup; i++) {
		const uint8_t* e = &t[i * gsz];
		Group& g         = groups[i];
		g.off            = (uint64_t)be32(e) * 4;
		const uint32_t s = be32(e + 4);
		g.size           = rvz ? (s & 0x7FFFFFFFu) : s;
		g.compressed     = rvz ? (s >> 31) != 0 : method != M_NONE;
		g.packed         = rvz ? be32(e + 8) : 0;
		if (g.size != 0 && g.off + g.size > f.size) {
			*err = std::string("damaged ") + fmt + " file (group outside the file)";
			return false;
		}
	}
	for (const Raw& r : raws)
		if ((uint64_t)r.group + r.groups > nGroup || (r.end - r.base + chunk - 1) / chunk > r.groups) {
			*err = std::string("damaged ") + fmt + " file (raw data entry)";
			return false;
		}
	return true;
}

// Group k of raw data entry `ri`, decoded (lock held). *start = its first disc offset.
const std::vector<uint8_t>* WiaReader::group(size_t ri, uint32_t k, uint64_t* start)
{
	const Raw& r       = raws[ri];
	*start             = r.base + (uint64_t)k * chunk;
	const size_t len   = (size_t)(std::min<uint64_t>(*start + chunk, r.end) - *start);
	const uint32_t gi  = r.group + k;
	if (std::vector<uint8_t>* hit = cache.find(gi))
		return hit;
	const Group& g          = groups[gi];
	std::vector<uint8_t>& out = cache.add(gi);
	if (g.size == 0) {
		out.assign(len, 0);
		return &out;
	}
	std::vector<uint8_t> src(g.size);
	if (!f.read(g.off, src.data(), g.size))
		return nullptr;
	std::vector<uint8_t> plain;
	const size_t plainLen = g.packed != 0 ? g.packed : len;
	if (g.compressed) {
		plain.resize(plainLen);
		if (!decompress(method, props, propsLen, src.data(), src.size(), plain.data(), plainLen))
			return nullptr;
	} else {
		if (src.size() < plainLen)
			return nullptr;
		src.resize(plainLen);
		plain.swap(src);
	}
	if (g.packed == 0) {
		out.swap(plain);
		out.resize(len);
		return &out;
	}
	// RVZ packing: literal and padding records.
	out.resize(len);
	size_t p = 0, o = 0;
	Junk junk;
	while (o < len) {
		if (plain.size() - p < 4)
			return nullptr;
		const uint32_t rec = be32(&plain[p]);
		p += 4;
		const size_t n = std::min<size_t>(rec & 0x7FFFFFFFu, len - o);
		if (rec & 0x80000000u) {
			if (plain.size() - p < 68)
				return nullptr;
			junk.seed(&plain[p]);
			p += 68;
			junk.skip((size_t)((*start + o) % 0x8000));
			junk.get(&out[o], n);
		} else {
			if (plain.size() - p < n)
				return nullptr;
			std::memcpy(&out[o], &plain[p], n);
			p += rec & 0x7FFFFFFFu;
		}
		o += n;
	}
	return &out;
}

int32_t WiaReader::read(uint64_t offset, void* dst, int32_t length)
{
	// SMS_PORT_DISCSTATS=1: time spent in reads (decompression included), every 256 reads.
	static const bool sStats = std::getenv("SMS_PORT_DISCSTATS") != nullptr;
	struct StatsGuard {
		bool on;
		LARGE_INTEGER t0;
		explicit StatsGuard(bool o) : on(o)
		{
			if (on)
				QueryPerformanceCounter(&t0);
		}
		~StatsGuard()
		{
			if (!on)
				return;
			static double sSecs;
			static unsigned sReads;
			LARGE_INTEGER t1, fq;
			QueryPerformanceCounter(&t1);
			QueryPerformanceFrequency(&fq);
			sSecs += (double)(t1.QuadPart - t0.QuadPart) / fq.QuadPart;
			if (++sReads % 256 == 0)
				std::fprintf(stderr, "[port] disc: %u reads, %.3f s in the RVZ/WIA reader\n", sReads, sSecs);
		}
	} statsGuard(sStats);
	if (length <= 0 || offset >= discSize)
		return 0;
	if (offset + (uint64_t)length > discSize)
		length = (int32_t)(discSize - offset);
	std::lock_guard<std::mutex> guard(lock);
	uint8_t* out = (uint8_t*)dst;
	int32_t done = 0;
	while (done < length) {
		const uint64_t pos = offset + done;
		size_t n           = (size_t)(length - done);
		if (pos < 0x80) {
			n = std::min<size_t>(n, (size_t)(0x80 - pos));
			std::memcpy(out + done, dhead + pos, n);
			done += (int32_t)n;
			continue;
		}
		// Raw data entry containing pos (entries are sorted and do not overlap).
		auto it = std::upper_bound(raws.begin(), raws.end(), pos, [](uint64_t v, const Raw& r) { return v < r.off; });
		if (it == raws.begin() || pos >= (it - 1)->end) {
			const uint64_t next = it == raws.end() ? discSize : it->off; // not stored: zeros
			n = (size_t)std::min<uint64_t>(n, next - pos);
			std::memset(out + done, 0, n);
			done += (int32_t)n;
			continue;
		}
		const size_t ri = (size_t)(it - 1 - raws.begin());
		const Raw& r    = raws[ri];
		uint64_t start;
		const std::vector<uint8_t>* g = group(ri, (uint32_t)((pos - r.base) / chunk), &start);
		if (g == nullptr)
			return -1;
		n = (size_t)std::min<uint64_t>(n, std::min<uint64_t>(r.end, start + g->size()) - pos);
		std::memcpy(out + done, g->data() + (pos - start), n);
		done += (int32_t)n;
	}
	return done;
}

// ---- GCZ ------------------------------------------------------------------------------
class GczReader : public PortDiscReader {
public:
	bool open(const std::string& path, std::string* err);
	uint64_t size() const override { return discSize; }
	int32_t read(uint64_t offset, void* dst, int32_t length) override;

private:
	File f;
	uint64_t discSize = 0, dataStart = 0, compSize = 0;
	uint32_t blockSize = 0;
	std::vector<uint64_t> ptrs;
	std::mutex lock;
	BlockCache cache;
};

bool GczReader::open(const std::string& path, std::string* err)
{
	if (!f.open(path))
		return false;
	uint8_t h[32];
	if (f.size < 32 || !f.read(0, h, 32) || le32(h) != 0xB10BC001u)
		return false;
	compSize            = le64(h + 8);
	discSize            = le64(h + 16);
	blockSize           = le32(h + 24);
	const uint32_t nblk = le32(h + 28);
	if (blockSize == 0 || (uint64_t)nblk * blockSize < discSize || nblk > (1u << 26)) {
		*err = "damaged GCZ file (header)";
		return false;
	}
	std::vector<uint8_t> t((size_t)nblk * 8);
	if (!f.read(32, t.data(), t.size())) {
		*err = "damaged GCZ file (block table)";
		return false;
	}
	ptrs.resize(nblk);
	for (uint32_t i = 0; i < nblk; i++)
		ptrs[i] = le64(&t[i * 8]);
	dataStart = 32 + (uint64_t)nblk * 12;
	cache.limit = 16;
	return true;
}

int32_t GczReader::read(uint64_t offset, void* dst, int32_t length)
{
	if (length <= 0 || offset >= discSize)
		return 0;
	if (offset + (uint64_t)length > discSize)
		length = (int32_t)(discSize - offset);
	std::lock_guard<std::mutex> guard(lock);
	uint8_t* out = (uint8_t*)dst;
	int32_t done = 0;
	const uint64_t kStored = 1ull << 63;
	while (done < length) {
		const uint64_t pos = offset + done;
		const uint32_t b   = (uint32_t)(pos / blockSize);
		std::vector<uint8_t>* blk = cache.find(b);
		if (blk == nullptr) {
			const uint64_t a = ptrs[b] & ~kStored;
			const uint64_t e = b + 1 < ptrs.size() ? (ptrs[b + 1] & ~kStored) : compSize;
			if (e < a || e - a > blockSize + 1024)
				return -1;
			std::vector<uint8_t> src((size_t)(e - a));
			if (!f.read(dataStart + a, src.data(), src.size()))
				return -1;
			blk = &cache.add(b);
			blk->resize(blockSize);
			if (ptrs[b] & kStored) {
				std::memcpy(blk->data(), src.data(), std::min<size_t>(src.size(), blockSize));
			} else {
				uLongf n = blockSize;
				if (uncompress(blk->data(), &n, src.data(), (uLong)src.size()) != Z_OK)
					return -1;
			}
		}
		const size_t inBlk = (size_t)(pos % blockSize);
		const size_t n     = std::min<size_t>((size_t)(length - done), blockSize - inBlk);
		std::memcpy(out + done, blk->data() + inBlk, n);
		done += (int32_t)n;
	}
	return done;
}

} // namespace

PortDiscReader* port_disc_open_compressed(const std::string& path, std::string* err)
{
	err->clear();
	{
		std::unique_ptr<WiaReader> r(new WiaReader);
		if (r->open(path, err))
			return r.release();
		if (!err->empty())
			return nullptr;
	}
	{
		std::unique_ptr<GczReader> r(new GczReader);
		if (r->open(path, err))
			return r.release();
	}
	return nullptr;
}

#endif
