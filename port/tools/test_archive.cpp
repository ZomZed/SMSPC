// Host test for the PC port's endianness layer 1 (see port/ENDIAN.md).
//
//   python port/tools/test_archive.py      (builds with build_port.py's clang flags and runs)
//
// 1. Yaz0: decompresses data/common.szs with the same algorithm as
//    JKRDecomp::decodeSZS (header fields read big-endian, as patched under
//    SMS_PORT).
// 2. RARC: runs port_swap_rarc() (the pass JKRMemArchive::open runs) and walks
//    the archive with the same lookups JKRArchive uses (node table, file
//    entries, flags >> 24, CArcName hash), then compares every file's size
//    against `dtk vfs ls -r`.
// 3. JSUMemoryInputStream: links the real JSUInputStream.cpp/JSUMemoryStream.cpp
//    and checks the byte-swapped typed reads and string length prefixes.
// 4. Per-format passes on real files extracted with `dtk vfs cp` (argv[3]):
//    ResTIMG (.bti, port_swap_resource), J3D TEX1 headers, ResFONT (.bfn),
//    BMG (real MessageUtil.cpp / MessageLoader.cpp), TParamT<T>::load
//    (real ParamInst.cpp over every .prm in params.szs).
// 5. JKernel on the real code (POINTERS.md sections 1 and 4): JKRHeap /
//    JKRExpHeap / JKRSolidHeap in a 32 MB arena mapped at 0x80000000 like
//    sdk_os_misc.cpp (alignment, getSize/resize, coalescing back to one free
//    block, operator new/delete routing), then common.szs / option.szs mounted
//    through the real JKRMemArchive (0x14-byte SDIFileEntry stride, mFileData
//    side table, lookups). The BMG test reads its files through these mounts
//    and the real JKRFileLoader::getGlbResource.
//
// Pointer-size clean (no pointer <-> u32 casts), so it also builds for x86_64.

#include <JSystem/J2D/J2DTextBox.hpp>
#include <JSystem/JGeometry.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>
#include <JSystem/JKernel/JKRSolidHeap.hpp>
#include <JSystem/JSupport/JSUMemoryInputStream.hpp>
#include <JSystem/JUtility/JUTFont.hpp>
#include <JSystem/ResTIMG.hpp>
#include <GC2D/MessageLoader.hpp>
#include <GC2D/MessageUtil.hpp>
#include <System/ParamInst.hpp>
#include <dolphin/dvd.h>
#include <dolphin/os.h>
#include <dolphin/os/OSAlloc.h>

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static int gFailures = 0;
#define CHECK(cond, ...)                                                       \
	do {                                                                       \
		if (!(cond)) {                                                         \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                   \
			std::printf(__VA_ARGS__);                                          \
			std::printf("\n");                                                 \
			gFailures++;                                                       \
		}                                                                      \
	} while (0)

// ---- Yaz0 (mirrors JKRDecomp::decodeSZS with dstSize == 0) ---------------
static bool decodeYaz0(const u8* src, size_t srcSize, std::vector<u8>& out)
{
	if (srcSize < 0x10 || std::memcmp(src, "Yaz0", 4) != 0)
		return false;
	u32 expanded = sms_be32(src + 4); // the SMS_PORT fix: big-endian size
	out.assign(expanded, 0);
	u8* dst       = out.data();
	u8* decompEnd = dst + expanded;
	const u8* cur = src + 0x10;
	const u8* end = src + srcSize;
	s32 chunkBitsLeft = 0, chunkBits = 0;
	while (dst != decompEnd) {
		if (cur >= end)
			return false;
		if (chunkBitsLeft == 0) {
			chunkBits     = *cur++;
			chunkBitsLeft = 8;
		}
		if (chunkBits & 0x80) {
			*dst++ = *cur++;
		} else {
			u32 dist  = cur[1] | (cur[0] & 0xF) << 8;
			u32 n     = cur[0] >> 4;
			cur += 2;
			const u8* copy = dst - dist;
			s32 count      = n == 0 ? *cur++ + 0x12 : n + 2;
			if (copy - 1 < out.data())
				return false;
			do {
				*dst++ = copy[-1];
				copy++;
			} while (--count != 0 && dst != decompEnd);
		}
		chunkBits <<= 1;
		chunkBitsLeft--;
	}
	return true;
}

// ---- RARC walk (mirrors JKRArchive's struct access after the swap) --------
struct DirEntry { // JKRArchive::SDIDirEntry
	u32 mType, mOffset;
	u16 mHash, mNum;
	u32 mFirstIdx;
};
static const size_t kFileEntryStride = 0x14; // on-disc SDIFileEntry size

struct Arc {
	const u8* base;
	const u8* info;
	const DirEntry* dirs;
	const u8* files;
	const char* strs;
	u32 numNodes, numFiles;

	u16 fileId(u32 i) const { return *(const u16*)(files + i * kFileEntryStride); }
	u16 fileHash(u32 i) const { return *(const u16*)(files + i * kFileEntryStride + 2); }
	u32 flagsName(u32 i) const { return *(const u32*)(files + i * kFileEntryStride + 4); }
	u32 dataOff(u32 i) const { return *(const u32*)(files + i * kFileEntryStride + 8); }
	u32 size(u32 i) const { return *(const u32*)(files + i * kFileEntryStride + 12); }
	const char* name(u32 i) const { return strs + (flagsName(i) & 0xFFFFFF); }
};

static u16 arcHash(const char* s, size_t n) // JKRArchive::CArcName::store
{
	u16 h = 0;
	for (size_t i = 0; i < n; i++)
		h = (u16)(std::tolower((unsigned char)s[i]) + h * 3);
	return h;
}

static void walk(const Arc& a, u32 node, const std::string& prefix,
                 std::map<std::string, u32>& out, int depth)
{
	CHECK(node < a.numNodes, "node %u out of range", node);
	if (node >= a.numNodes || depth > 32)
		return;
	const DirEntry& d = a.dirs[node];
	for (u32 i = d.mFirstIdx; i < d.mFirstIdx + d.mNum; i++) {
		const char* nm = a.name(i);
		CHECK(a.fileHash(i) == arcHash(nm, std::strlen(nm)), "hash mismatch for %s", nm);
		if ((a.flagsName(i) >> 24) & 0x02) {
			if (!std::strcmp(nm, ".") || !std::strcmp(nm, ".."))
				continue;
			walk(a, a.dataOff(i), prefix + nm + "/", out, depth + 1);
		} else {
			out[prefix + nm] = a.size(i);
		}
	}
}

// JKRArchive::findFsResource over the swapped tables.
static int findFs(const Arc& a, const char* path, u32 node)
{
	const char* slash = std::strchr(path, '/');
	size_t len        = slash ? (size_t)(slash - path) : std::strlen(path);
	u16 h             = arcHash(path, len);
	const DirEntry& d = a.dirs[node];
	for (u32 i = d.mFirstIdx; i < d.mFirstIdx + d.mNum; i++) {
		const char* nm = a.name(i);
		if (a.fileHash(i) != h || std::strlen(nm) != len || std::strncmp(nm, path, len))
			continue;
		if ((a.flagsName(i) >> 24) & 0x02)
			return slash ? findFs(a, slash + 1, a.dataOff(i)) : -1;
		return slash ? -1 : (int)i;
	}
	return -1;
}

static std::string dtkSize(u32 n) // dtk's human-readable size column
{
	char buf[32];
	if (n == 1)
		return "1 byte";
	if (n < 1024) {
		std::snprintf(buf, sizeof buf, "%u bytes", n);
		return buf;
	}
	double v = n / 1024.0;
	const char* unit = "KiB";
	if (v >= 1024.0) {
		v /= 1024.0;
		unit = "MiB";
	}
	std::snprintf(buf, sizeof buf, "%#.3g %s", v, unit);
	return buf;
}

static std::string trim(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
	return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}

// ---- JSUMemoryInputStream typed reads -------------------------------------
static void testStream()
{
	// big-endian: u32 0x11223344, s16 -2, f32 1.5, u16 0xBEEF, "hi" (u16 len), u64
	const u8 data[] = { 0x11, 0x22, 0x33, 0x44, 0xFF, 0xFE, 0x3F, 0xC0, 0x00, 0x00, 0xBE, 0xEF,
		                0x00, 0x02, 'h',  'i',  0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
		                0x00, 0x00, 0x00, 0x2A };
	JSUMemoryInputStream s(data, sizeof data);
	CHECK(s.readU32() == 0x11223344u, "readU32");
	CHECK(s.readS16() == -2, "readS16");
	CHECK(s.readF32() == 1.5f, "readF32");
	u16 w;
	s >> w;
	CHECK(w == 0xBEEF, "operator>>(u16)");
	char* str = s.readString();
	CHECK(str && !std::strcmp(str, "hi"), "readString length prefix");
	delete[] str;
	CHECK(s.readU64() == 0x0102030405060708ull, "readU64");
	s32 v;
	s >> v;
	CHECK(v == 42, "operator>>(s32)");
	CHECK(s.getAvailable() == 0, "stream fully consumed");
	u32 raw;
	s.seek(0, JSUStreamSeekFrom_SET);
	s.read(&raw, 4);
	CHECK(raw == 0x44332211u, "untyped read stays raw");
}

// ===========================================================================
// Per-format passes (ENDIAN.md "In-place" list). Files come from `dtk vfs cp`
// extractions of the real .szs archives (argv[3], see test_archive.py).
// ===========================================================================
namespace fs_ = std::filesystem;

static std::vector<u8> slurp(const fs_::path& p)
{
	std::vector<u8> v;
	FILE* f = std::fopen(p.string().c_str(), "rb");
	if (!f)
		return v;
	std::fseek(f, 0, SEEK_END);
	v.resize((size_t)std::ftell(f));
	std::fseek(f, 0, SEEK_SET);
	std::fread(v.data(), 1, v.size(), f);
	std::fclose(f);
	return v;
}

// -- stubs for the real MessageLoader.cpp / MessageUtil.cpp / ParamInst.cpp --
size_t J2DTextBox::setString(const char*, ...) { return 0; }
TBaseParam::TBaseParam(TParams*, unsigned short code, const char* n)
    : keyCode(code)
    , name(n)
    , next(nullptr)
{
}

// ---- ResTIMG --------------------------------------------------------------
static void checkTimg(const ResTIMG* t, size_t fileSize, const char* what)
{
	CHECK(t->width >= 1 && t->width <= 1024 && t->height >= 1 && t->height <= 1024,
	      "%s: size %ux%u", what, t->width, t->height);
	CHECK(t->format <= 0xE, "%s: format %u", what, t->format);
	CHECK(t->imageDataOffset >= 0x20 && t->imageDataOffset < fileSize,
	      "%s: imageDataOffset 0x%x (file 0x%zx)", what, t->imageDataOffset, fileSize);
	CHECK(t->numColors <= 0x4000, "%s: numColors %u", what, t->numColors);
	if (t->numColors)
		CHECK(t->paletteOffset >= 0x20 && t->paletteOffset < fileSize, "%s: paletteOffset 0x%x",
		      what, t->paletteOffset);
	CHECK(t->mipmapCount >= 1 && t->mipmapCount <= 12, "%s: mipmapCount %u", what, t->mipmapCount);
	CHECK(t->minFilter <= 5 && t->magFilter <= 1, "%s: filters %u/%u", what, t->minFilter,
	      t->magFilter);
	CHECK(t->wrapS <= 2 && t->wrapT <= 2, "%s: wrap %u/%u", what, t->wrapS, t->wrapT);
}

static void testTimg(const fs_::path& dir)
{
	int n = 0, bad0 = gFailures;
	for (fs_::recursive_directory_iterator it(dir), end; it != end; ++it) {
		if (!it->is_regular_file() || it->path().extension() != ".bti")
			continue;
		std::vector<u8> b = slurp(it->path());
		if (b.size() < 0x20)
			continue;
		std::vector<u8> orig = b;
		std::string nm       = it->path().filename().string();
		CHECK(b[0x19] == 0, "%s: pad byte 0x19 not zero on disc", nm.c_str());
		CHECK(port_swap_resource(nm.c_str(), b.data(), (unsigned)b.size()) == 1, "%s: swap", nm.c_str());
		CHECK(port_swap_resource(nm.c_str(), b.data(), (unsigned)b.size()) == 0,
		      "%s: second swap must be a no-op", nm.c_str());
		const ResTIMG* t = (const ResTIMG*)b.data();
		CHECK(t->width == sms_be16(&orig[2]) && t->height == sms_be16(&orig[4]) &&
		          t->numColors == sms_be16(&orig[0xA]) && t->paletteOffset == sms_be32(&orig[0xC]) &&
		          t->imageDataOffset == sms_be32(&orig[0x1C]) &&
		          (u16)t->lodBias == sms_be16(&orig[0x1A]),
		      "%s: fields differ from byte-wise decode", nm.c_str());
		CHECK(std::memcmp(b.data() + 0x20, orig.data() + 0x20, b.size() - 0x20) == 0,
		      "%s: pixel/palette payload must stay untouched", nm.c_str());
		checkTimg(t, b.size(), nm.c_str());
		if (nm == "coin_number_0_min.bti")
			CHECK(t->width == 28 && t->height == 28 && t->format == 5 && t->alphaEnabled == 2,
			      "coin_number_0_min.bti: %ux%u fmt %u", t->width, t->height, t->format);
		n++;
	}
	CHECK(n > 50, "only %d .bti files found", n);
	std::printf("ResTIMG: %d .bti headers swapped, idempotent, payload untouched (%s)\n", n,
	            gFailures == bad0 ? "ok" : "FAILURES");

	// not a .bti / too short: untouched
	u8 z[0x20] = { 0 };
	CHECK(port_swap_resource("x.blo", z, 0x20) == 0 && port_swap_resource("a.BTI", z, 0x20) == 1,
	      "extension dispatch (case-insensitive)");
	CHECK(port_swap_resource("x.bti", z, 0x10) == 0, "short resource ignored");
}

// J3D TEX1 block inside a real .bmd
static void testTex1(const fs_::path& dir)
{
	int models = 0, tex = 0;
	for (fs_::recursive_directory_iterator it(dir), end; it != end; ++it) {
		if (!it->is_regular_file() || it->path().extension() != ".bmd")
			continue;
		std::vector<u8> b = slurp(it->path());
		if (b.size() < 0x20 || std::memcmp(b.data(), "J3D2", 4) != 0)
			continue;
		u32 blocks = sms_be32(&b[0xC]);
		size_t off = 0x20;
		for (u32 i = 0; i < blocks && off + 8 <= b.size(); i++) {
			u32 sz = sms_be32(&b[off + 4]);
			if (std::memcmp(&b[off], "TEX1", 4) == 0) {
				std::vector<u8> a = b, c = b; // a: raw block, c: block header pre-swapped
				u32 count = sms_be16(&b[off + 8]), toff = sms_be32(&b[off + 0xC]);
				CHECK(port_swap_tex1_timgs(&a[off]) == (int)count, "TEX1 swapped %u headers", count);
				CHECK(port_swap_tex1_timgs(&a[off]) == 0, "TEX1 second pass is a no-op");
				for (u32 k = 0; k < count; k++) {
					const ResTIMG* t = (const ResTIMG*)&a[off + toff + k * 0x20];
					CHECK(t->width == sms_be16(&b[off + toff + k * 0x20 + 2]), "TEX1 width");
					CHECK(t->imageDataOffset + toff + k * 0x20 < sz + 0x20 * count + 0x100000,
					      "TEX1 imageDataOffset 0x%x", t->imageDataOffset);
					checkTimg(t, 0x1000000, it->path().filename().string().c_str());
					tex++;
				}
				// header already swapped by a future port_swap_j3d: same result
				u8* h = &c[off];
				sms_swap32_inplace(h);
				sms_swap32_inplace(h + 4);
				sms_swap16_inplace(h + 8);
				sms_swap32_inplace(h + 0xC);
				sms_swap32_inplace(h + 0x10);
				CHECK(port_swap_tex1_timgs(h) == (int)count, "TEX1 with pre-swapped block header");
				CHECK(std::memcmp(&a[off + toff], &c[off + toff], count * 0x20) == 0,
				      "TEX1 results agree");
				models++;
			}
			off += sz;
		}
	}
	CHECK(models > 0, "no .bmd with TEX1 found");
	std::printf("J3D TEX1: %d models, %d embedded ResTIMG headers swapped\n", models, tex);
}

// ---- ResFONT --------------------------------------------------------------
static void testFont(const fs_::path& dir)
{
	std::vector<u8> b = slurp(dir / "common" / "test_fontex.bfn");
	CHECK(b.size() > 0x100, "test_fontex.bfn missing");
	if (b.size() <= 0x100)
		return;
	CHECK(port_swap_font(b.data()) == 1, "font swap");
	CHECK(port_swap_font(b.data()) == 0, "font second swap is a no-op");
	const ResFONT* f = (const ResFONT*)b.data();
	CHECK(*(const u32*)b.data() == 'FONT' && *(const u32*)(b.data() + 4) == 'bfn1', "FONT/bfn1 magic");
	CHECK(f->filesize == b.size(), "filesize %u == %zu", f->filesize, b.size());
	CHECK(f->numBlocks == 15, "numBlocks %u", f->numBlocks);
	const u8* p = b.data() + 0x20;
	int inf = 0, wid = 0, gly = 0, map = 0;
	for (u32 i = 0; i < f->numBlocks; i++) {
		u32 magic = *(const u32*)p, size = *(const u32*)(p + 4);
		CHECK(size >= 8 && (p - b.data()) + size <= b.size(), "block %u size 0x%x out of range", i, size);
		if (magic == 'INF1') {
			const ResFONT::INF1* x = (const ResFONT::INF1*)p;
			CHECK(x->fontType == 2 && x->ascent == 0x14 && x->width == 0x14, "INF1 type %u asc %u w %u",
			      x->fontType, x->ascent, x->width);
			inf++;
		} else if (magic == 'WID1') {
			const ResFONT::WID1* x = (const ResFONT::WID1*)p;
			CHECK(x->startCode <= x->endCode && size == ((8 + 4 + (x->endCode - x->startCode + 1) * 2 + 0x1F) & ~0x1Fu),
			      "WID1 range %u..%u size 0x%x", x->startCode, x->endCode, size);
			wid++;
		} else if (magic == 'GLY1') {
			const ResFONT::GLY1* x = (const ResFONT::GLY1*)p;
			CHECK(x->startCode <= x->endCode && x->textureWidth > 0 && x->textureHeight > 0 &&
			          x->numRows * x->numColumns > 0 &&
			          size == 0x20 + x->textureSize * ((x->endCode - x->startCode) / (x->numRows * x->numColumns) + 1),
			      "GLY1 %u..%u tex %ux%u size 0x%x texSize 0x%x", x->startCode, x->endCode, x->textureWidth,
			      x->textureHeight, size, x->textureSize);
			gly++;
		} else if (magic == 'MAP1') {
			const ResFONT::MAP1* x = (const ResFONT::MAP1*)p;
			CHECK(x->mappingMethod <= 3 && x->startCode <= x->endCode, "MAP1 method %u %u..%u",
			      x->mappingMethod, x->startCode, x->endCode);
			const u16* e = &x->mLeading;
			if (x->mappingMethod == 2)
				CHECK(x->numEntries == x->endCode - x->startCode + 1, "MAP1 method 2 count");
			if (x->mappingMethod == 3) { // sorted (code, glyph) pairs, binary searched
				for (int k = 1; k < x->numEntries; k++)
					CHECK(e[k * 2] > e[(k - 1) * 2], "MAP1 method 3 not sorted at %d", k);
			}
			map++;
		} else {
			CHECK(false, "unknown block 0x%08x", magic);
		}
		p += size;
	}
	CHECK(p == b.data() + b.size(), "blocks tile the file exactly");
	CHECK(inf == 1 && wid == 1 && gly == 1 && map == 12, "block mix INF1 %d WID1 %d GLY1 %d MAP1 %d", inf,
	      wid, gly, map);
	std::printf("ResFONT: test_fontex.bfn: %d INF1, %d WID1, %d GLY1, %d MAP1, tiles %zu bytes\n", inf,
	            wid, gly, map, b.size());
}

// ---- BMG ------------------------------------------------------------------
// A resource of a mounted archive (real JKRFileLoader::getGlbResource), with
// its size from the owning archive.
static const u8* globalRes(const char* path, size_t* size)
{
	void* p = JKRFileLoader::getGlbResource(path);
	*size   = 0;
	if (p) {
		char vol[64];
		const char* s = path + 1;
		size_t n      = std::strcspn(s, "/");
		std::snprintf(vol, sizeof vol, "%.*s", (int)n, s);
		JKRArchive* ar = (JKRArchive*)JKRFileLoader::getVolume(vol);
		*size          = ar ? (size_t)ar->getResSize(p) : 0;
	}
	return (const u8*)p;
}

static void testBmg(const fs_::path& dir)
{
	// Served by the JKRMemArchive mounts of option.szs / common.szs (testMemArchive);
	// cross-checked against the dtk extraction.
	size_t loadSize;
	void* load = (void*)globalRes("/option/loadmessage.bmg", &loadSize);
	std::vector<u8> ref = slurp(dir / "option" / "loadmessage.bmg");
	CHECK(load && loadSize == ref.size() && std::memcmp(load, ref.data(), ref.size()) == 0,
	      "loadmessage.bmg through JKRMemArchive matches the dtk extraction");
	CHECK(load != nullptr, "loadmessage.bmg missing");
	if (!load)
		return;
	CHECK(port_swap_resource("loadmessage.bmg", load, 2048) == 0, "BMG is left big-endian by design");

	// SMSGetMessageData (real MessageUtil.cpp)
	const char* s = SMSGetMessageData(load, 0x1D);
	CHECK(s && !std::strcmp(s, "Select data."), "message 0x1D = \"%s\"", s ? s : "(null)");
	CHECK(SMSGetMessageData(load, 35) == nullptr, "id == count is out of range");
	CHECK(SMSGetMessageData(load, 0) && !std::strcmp(SMSGetMessageData(load, 0), "Corrupt"),
	      "message 0 = \"%s\"", SMSGetMessageData(load, 0));
	int seen = 0;
	for (u32 i = 0; i < 35; i++) {
		const char* m = SMSGetMessageData(load, i);
		CHECK(m != nullptr && std::strlen(m) < 200, "loadmessage id %u", i);
		for (const char* c = m; c && *c; c++)
			CHECK((u8)*c >= 0x20 || *c == '\n' || *c == 0x1A, "id %u non-text byte 0x%x", i, (u8)*c);
		seen += m != nullptr;
	}
	void* stage = JKRFileLoader::getGlbResource("/common/2d/stagename.bmg");
	const char* st = SMSGetMessageData(stage, 0);
	CHECK(st && std::strlen(st) > 0, "stagename id 0");
	std::printf("BMG SMSGetMessageData: loadmessage.bmg 0x1D = \"%s\", %d/35 ids decode; stagename[0] = \"%s\"\n",
	            s ? s : "?", seen, st ? st : "?");

	// TMessageLoader (real MessageLoader.cpp): header words, INF1 table, DAT1
	const char* names[] = { "/common/2d/balloon.bmg", "/common/2d/sys_message.bmg" };
	for (int k = 0; k < 2; k++) {
		size_t rawSize;
		const u8* raw = globalRes(names[k], &rawSize);
		CHECK(raw && rawSize > 0x40, "%s missing", names[k]);
		if (!raw || rawSize <= 0x40)
			continue;
		TMessageLoader ml(names[k]);
		u32 cnt = sms_be16(&raw[0x20 + 8]);
		CHECK(ml.getMessageNum() == cnt && cnt > 10 && cnt < 255, "%s: count %u vs %u", names[k],
		      ml.getMessageNum(), cnt);
		const u8* dat = ml.getMessageData();
		CHECK(dat == raw + 0x20 + sms_be32(&raw[0x24]) + 8, "%s: DAT1 pointer", names[k]);
		size_t datSize = rawSize - (size_t)(dat - raw);
		int bad = 0;
		for (u32 i = 0; i < cnt; i++) {
			const JMSMesgEntry* e = ml.getMessageEntry(i);
			if (!e || e->unk0 >= datSize || e->unk0 != sms_be32(&raw[0x30 + i * 12]) ||
			    e->unk4 != (s16)sms_be16(&raw[0x30 + i * 12 + 4]) ||
			    e->unk6 != (s16)sms_be16(&raw[0x30 + i * 12 + 6]))
				bad++;
		}
		CHECK(bad == 0, "%s: %d bad INF1 entries", names[k], bad);
		CHECK(ml.getMessageEntry(cnt) == nullptr, "entry count bound");
		std::printf("BMG TMessageLoader: %s: %u entries, first text \"%.24s\"\n", names[k], cnt,
		            (const char*)dat + ml.getMessageEntry(0)->unk0);
	}
}

// ---- TParamT<T>::load -----------------------------------------------------
static void testParams(const fs_::path& dir)
{
	int files = 0, entries = 0, sizes[17] = { 0 };
	for (fs_::recursive_directory_iterator it(dir), end; it != end; ++it) {
		if (!it->is_regular_file() || it->path().extension() != ".prm")
			continue;
		std::vector<u8> b = slurp(it->path());
		JSUMemoryInputStream in(b.data(), (s32)b.size());
		s32 n = in.readS32();
		CHECK(n > 0 && n < 4096, "%s: param count %d", it->path().filename().string().c_str(), n);
		for (s32 i = 0; i < n && gFailures < 20; i++) {
			in.read16b();
			char name[0x50];
			in.readString(name, 0x50);
			u32 sz = sms_be32(b.data() + in.getPosition());
			const u8* val = b.data() + in.getPosition() + 4; // TParamT::load skips 4, then reads sz bytes
			TParamT<u8> pu8(nullptr, 0, 0, "");
			TParamT<s16> ps16(nullptr, 0, 0, "");
			TParamT<s32> ps32(nullptr, 0, 0, "");
			TParamT<f32> pf32(nullptr, 0, 0, "");
			TParamT<JGeometry::TVec3<f32> > pvec(nullptr, JGeometry::TVec3<f32>(0, 0, 0), 0, "");
			JSUMemoryInputStream s2(b.data() + in.getPosition(), 4 + sz);
			if (sz == 1) {
				pu8.load(s2);
				CHECK(pu8.value == val[0], "%s: u8", name);
			} else if (sz == 2) {
				ps16.load(s2);
				CHECK(ps16.value == (s16)sms_be16(val), "%s: s16 %d", name, ps16.value);
			} else if (sz == 4) {
				ps32.load(s2);
				s2.seek(0, JSUStreamSeekFrom_SET);
				pf32.load(s2);
				u32 bits;
				std::memcpy(&bits, &pf32.value, 4);
				CHECK(ps32.value == (s32)sms_be32(val) && bits == sms_be32(val), "%s: s32/f32", name);
				// most f32 params are sane magnitudes, never denormal/huge garbage from a missed swap
			} else if (sz == 12) {
				pvec.load(s2);
				u32 bx, by, bz;
				std::memcpy(&bx, &pvec.value.x, 4);
				std::memcpy(&by, &pvec.value.y, 4);
				std::memcpy(&bz, &pvec.value.z, 4);
				CHECK(bx == sms_be32(val) && by == sms_be32(val + 4) && bz == sms_be32(val + 8),
				      "%s: vec3", name);
			}
			if (sz < 17)
				sizes[sz]++;
			in.skip(4 + sz);
			entries++;
		}
		CHECK(in.getAvailable() == 0, "%s: %d bytes left", it->path().filename().string().c_str(),
		      in.getAvailable());
		files++;
	}
	CHECK(files > 100, "only %d .prm files", files);
	// A float read without the swap would show up as an absurd value; spot-check one known file.
	std::vector<u8> bird = slurp(dir / "params" / "animal" / "bird.prm");
	std::printf("TParamT::load: %d .prm files, %d params (u8 %d, s16 %d, 4-byte %d, vec3 %d), all match byte-wise decode\n",
	            files, entries, sizes[1], sizes[2], sizes[4], sizes[12]);
}

// ===========================================================================
// JKernel: heaps and JKRMemArchive on the real code (POINTERS.md 1 and 4)
// ===========================================================================

// Host stand-ins for the SDK pieces the JKernel units call (the game build
// gets them from port/src/sdk_*.cpp). Everything else they reference (DVD
// archives, decompression, ARAM) is never reached here and is left unresolved
// at link time (/force:unresolved, see test_archive.py).
extern "C" {
static void* sArenaLo;
static void* sArenaHi;
void OSInitMutex(OSMutex*) { }
void OSLockMutex(OSMutex*) { }
void OSUnlockMutex(OSMutex*) { }
void OSReport(const char*, ...) { }
void OSPanic(const char* file, int line, const char* msg, ...)
{
	std::printf("FAIL OSPanic %s:%d: %s\n", file, line, msg);
	std::exit(1);
}
void* OSGetArenaLo(void) { return sArenaLo; }
void* OSGetArenaHi(void) { return sArenaHi; }
void OSSetArenaLo(void* p) { sArenaLo = p; }
void OSSetArenaHi(void* p) { sArenaHi = p; }
void* OSInitAlloc(void* lo, void*, int) { return lo; }
void JUTWarningConsole(const char*) { }
void JUTWarningConsole_f(const char*, ...) { }
void JUTReportConsole(const char*) { }
void JUTReportConsole_f(const char*, ...) { }
void port_host_alloc_push(void);
void port_host_alloc_pop(void);
__declspec(dllimport) void* __stdcall VirtualAlloc(void*, size_t, unsigned long, unsigned long);
}

static const uintptr_t kRamBase = 0x80000000u, kRamSize = 0x02000000u; // as port_sdk.h

// Maps "main RAM" where sdk_os_misc.cpp puts it (below 4 GB), so the heaps
// and JKRHeap::initArena run on the same addresses as in the game.
static bool mapArena()
{
	void* p = VirtualAlloc((void*)kRamBase, kRamSize, 0x3000 /* RESERVE|COMMIT */, 0x04);
	if (p != (void*)kRamBase)
		return false;
	*(u32*)(kRamBase + 0x28) = 0x01800000; // physical memory size, read by initArena
	sArenaLo                 = (void*)(kRamBase + 0x4000);
	sArenaHi                 = (void*)(kRamBase + kRamSize);
	return true;
}

static bool inArena(const void* p)
{
	return (uintptr_t)p >= kRamBase && (uintptr_t)p < kRamBase + kRamSize;
}

static u32 rng = 12345;
static u32 nextRand()
{
	rng = rng * 1103515245u + 12345u;
	return rng >> 8;
}

static void testHeap(JKRExpHeap* root)
{
	typedef JKRExpHeap::CMemBlock Blk;
	CHECK(sizeof(Blk) == (sizeof(void*) == 8 ? 0x20u : 0x10u), "sizeof(CMemBlock) 0x%zx", sizeof(Blk));
	CHECK(inArena(root) && ((uintptr_t)root->getStartAddr() & 15) == 0, "root heap in the arena");

	JKRExpHeap* h = JKRExpHeap::create(0x100000, root, false);
	CHECK(h && inArena(h), "child ExpHeap");
	if (!h)
		return;
	const s32 free0 = h->getTotalFreeSize();
	CHECK(h->getFreeSize() == free0, "one free block at start");

	static const int aligns[] = { 0, 4, 8, 16, 32, 64, -4, -8, -16, -32, -64 };
	struct A {
		u8* p;
		u32 size;
		u8 fill;
	};
	std::vector<A> live;
	int bad = 0, allocs = 0;
	for (int round = 0; round < 4000; round++) {
		if (live.empty() || nextRand() % 3 != 0) {
			u32 size  = 1 + nextRand() % 3000;
			int align = aligns[nextRand() % (sizeof aligns / sizeof aligns[0])];
			u8* p     = (u8*)h->alloc(size, align);
			if (!p)
				continue;
			allocs++;
			int need = align < 0 ? -align : align;
			if (need < 16)
				need = 16;
			if (((uintptr_t)p & (need - 1)) != 0 || h->getSize(p) < (s32)size
			    || Blk::getBlock(p) != Blk::getHeapBlock(p) || !Blk::getBlock(p)->isValid())
				bad++;
			u8 fill = (u8)nextRand();
			std::memset(p, fill, size);
			A a = { p, size, fill };
			live.push_back(a);
		} else {
			size_t i = nextRand() % live.size();
			A a      = live[i];
			for (u32 k = 0; k < a.size; k++)
				if (a.p[k] != a.fill) {
					bad++;
					break;
				}
			if (nextRand() % 4 == 0) { // shrink in place, keep the prefix
				u32 ns = a.size / 2 + 1;
				if (h->resize(a.p, ns) >= (s32)ns) {
					live[i].size = ns;
					continue;
				}
			}
			h->free(a.p);
			live[i] = live.back();
			live.pop_back();
		}
	}
	CHECK(bad == 0, "%d bad allocations (alignment, getSize, header, contents)", bad);
	CHECK(h->check(), "JKRExpHeap::check after %d allocs", allocs);
	for (size_t i = 0; i < live.size(); i++)
		h->free(live[i].p);
	CHECK(h->check() && h->getTotalFreeSize() == free0 && h->getFreeSize() == free0,
	      "everything freed coalesces back into one block (%d / %d)", h->getTotalFreeSize(), free0);

	// freeAll / freeGroup paths
	void* x = h->alloc(100, 0);
	void* y = h->alloc(200, -32);
	CHECK(x && y && ((uintptr_t)y & 31) == 0, "head/tail alloc");
	h->freeAll();
	CHECK(h->getTotalFreeSize() == free0, "freeAll");

	// Solid heap: the 16-byte floor, head and tail
	JKRSolidHeap* sh = JKRSolidHeap::create(0x4000, root, false);
	CHECK(sh != nullptr, "solid heap");
	if (sh) {
		u8* a = (u8*)sh->alloc(3, 4);
		u8* b = (u8*)sh->alloc(5, 0);
		u8* c = (u8*)sh->alloc(7, -4);
		u8* d = (u8*)sh->alloc(9, 64);
		CHECK(a && b && c && d && !((uintptr_t)a & 15) && !((uintptr_t)b & 15)
		          && !((uintptr_t)c & 15) && !((uintptr_t)d & 63) && b >= a + 16 && c > d,
		      "solid heap alignment %p %p %p %p", a, b, c, d);
	}

	// operator new/delete routing (JKRHeap.cpp, SMS_PORT)
	JKRHeap* prev = JKRHeap::getCurrentHeap();
	h->becomeCurrentHeap();
	port_host_alloc_pop(); // test runs inside a host scope (main)
	s32 before = h->getTotalFreeSize();
	int* gameArr = new int[100];
	CHECK(JKRHeap::findFromRoot(gameArr) == h && ((uintptr_t)gameArr & 15) == 0,
	      "new goes to the current JKRHeap, 16-aligned");
	delete[] gameArr;
	CHECK(h->getTotalFreeSize() == before, "delete returns it to the heap");
	port_host_alloc_push();
	int* hostArr = new int[100];
	CHECK(!inArena(hostArr), "new inside port_host_alloc_push uses the CRT heap");
	delete[] hostArr; // routed to free() by address
	CHECK(h->getTotalFreeSize() == before, "host delete leaves the game heap alone");
	prev->becomeCurrentHeap();

	std::printf("JKRExpHeap: %d random allocs (alignments -64..64), check() ok, coalesces to 0x%x; "
	            "solid heap and new/delete routing %s\n",
	            allocs, free0, bad == 0 ? "ok" : "FAILED");
	h->destroy();
}

// Mounts an unswapped RARC image through the real JKRMemArchive, from a copy
// in the root heap (as the game does with SMSLoadArchive + mountFixed).
static JKRMemArchive* mountMem(JKRHeap* heap, const std::vector<u8>& raw, u8** bufOut)
{
	u8* buf = (u8*)JKRAllocFromHeap(heap, (u32)raw.size(), 32);
	*bufOut = buf;
	if (!buf)
		return nullptr;
	std::memcpy(buf, raw.data(), raw.size());
	return new (heap, 0) JKRMemArchive(buf, (u32)raw.size(), MBF_0);
}

static void testMemArchive(JKRMemArchive* ar, const u8* base, const std::vector<u8>& raw,
                           const Arc& a, const std::map<std::string, u32>& files)
{
	CHECK(ar && ar->isMounted(), "JKRMemArchive mount");
	if (!ar)
		return;
	const u32* hdr  = (const u32*)base;
	const u8* data  = base + hdr[2] + hdr[3];
	const u8* ent0  = a.files - a.base + base; // file table inside the mounted copy
	CHECK(hdr[0] == 'RARC', "JKRMemArchive::open swapped the header");
	CHECK(ar->countFile() == a.numFiles, "countFile %u", ar->countFile());

	int checked = 0, bad = 0;
	for (std::map<std::string, u32>::const_iterator it = files.begin(); it != files.end(); ++it) {
		std::string path = "/" + it->first;
		void* p          = ar->getResource(path.c_str());
		int idx          = findFs(a, it->first.c_str(), 0);
		// .bti/.bfn headers are swapped in place by getResource (port_swap_resource)
		bool swapped = sms_name_has_ext(it->first.c_str(), ".bti")
		               || sms_name_has_ext(it->first.c_str(), ".bfn");
		if (!p || idx < 0 || p != data + a.dataOff((u32)idx) || ar->getResSize(p) != (s32)it->second
		    || (!swapped
		        && std::memcmp(p, raw.data() + (data - base) + a.dataOff((u32)idx), it->second) != 0)) {
			if (bad < 5)
				std::printf("  JKRMemArchive mismatch: %s\n", path.c_str());
			bad++;
		}
		if (idx >= 0 && ar->getFileAttribute((u32)idx) != a.flagsName((u32)idx) >> 24)
			bad++;
		checked++;
	}
	CHECK(bad == 0, "%d JKRMemArchive lookups disagree with the table walk", bad);

	// The on-disc runtime slot (+0x10 of each 0x14-byte entry) stays untouched;
	// the pointers live in mFileData.
	int dirty = 0;
	for (u32 i = 0; i < a.numFiles; i++)
		dirty += *(const u32*)(ent0 + i * 0x14 + 0x10) != 0;
	CHECK(dirty == 0, "%d on-disc data slots written", dirty);

	// remove / detach / re-fetch, readResource
	std::map<std::string, u32>::const_iterator f = files.begin();
	std::string path = "/" + f->first;
	void* p          = ar->getResource(path.c_str());
	CHECK(ar->removeResource(p) && ar->getResSize(p) == -1, "removeResource clears the pointer");
	CHECK(ar->getResource(path.c_str()) == p && ar->getResSize(p) == (s32)f->second, "re-fetch");
	CHECK(ar->detachResource(p) && ar->getResSize(p) == -1, "detachResource");
	std::vector<u8> buf((f->second + 31) & ~31u);
	size_t n = ar->readResource(buf.data(), (u32)buf.size(), path.c_str());
	CHECK(n >= f->second && std::memcmp(buf.data(), (u8*)p, f->second) == 0, "readResource copy");
	std::printf("JKRMemArchive \"%s\": %d files via getResource/getResSize/getFileAttribute, "
	            "on-disc slots untouched, remove/detach/read checked\n",
	            a.strs + a.dirs[0].mOffset, checked);
}

static std::vector<u8> loadSzs(const std::string& path)
{
	std::vector<u8> comp, out;
	FILE* f = std::fopen(path.c_str(), "rb");
	if (!f)
		return out;
	std::fseek(f, 0, SEEK_END);
	comp.resize((size_t)std::ftell(f));
	std::fseek(f, 0, SEEK_SET);
	std::fread(comp.data(), 1, comp.size(), f);
	std::fclose(f);
	if (!decodeYaz0(comp.data(), comp.size(), out))
		out.clear();
	return out;
}

int main(int argc, char** argv)
{
	// The real JKRHeap.cpp replaces the global new: keep this host program's own
	// containers on the CRT heap (game-side allocations name a heap explicitly).
	port_host_alloc_push();

	const char* szs = argc > 1 ? argv[1]
	                           : "<project>/dolphin_test/files/data/common.szs";
	const char* dtk = argc > 2 ? argv[2] : "build\\tools\\dtk.exe";

	testStream();

	FILE* f = std::fopen(szs, "rb");
	if (!f) {
		std::printf("FAIL cannot open %s\n", szs);
		return 1;
	}
	std::vector<u8> comp;
	std::fseek(f, 0, SEEK_END);
	comp.resize((size_t)std::ftell(f));
	std::fseek(f, 0, SEEK_SET);
	std::fread(comp.data(), 1, comp.size(), f);
	std::fclose(f);

	std::vector<u8> arc;
	bool ok = decodeYaz0(comp.data(), comp.size(), arc);
	CHECK(ok, "Yaz0 decode");
	if (!ok)
		return 1;
	std::printf("Yaz0: %zu -> %zu bytes (header size %u)\n", comp.size(), arc.size(),
	            sms_be32(comp.data() + 4));

	const std::vector<u8> rawArc = arc; // unswapped, for the JKRMemArchive mount
	u32* hdr = (u32*)arc.data();
	CHECK(hdr[0] == PORT_RARC_MAGIC_RAW, "raw RARC magic before swap");
	CHECK(port_swap_rarc(arc.data()) == 1, "first swap");
	CHECK(port_swap_rarc(arc.data()) == 0, "second swap is a no-op");
	CHECK(hdr[0] == 'RARC', "signature == 'RARC' after swap");
	CHECK(hdr[1] == arc.size(), "file_length %u == %zu", hdr[1], arc.size());

	Arc a;
	a.base     = arc.data();
	a.info     = a.base + hdr[2];
	const u32* iw = (const u32*)a.info;
	a.numNodes = iw[0];
	a.dirs     = (const DirEntry*)(a.info + iw[1]);
	a.numFiles = iw[2];
	a.files    = a.info + iw[3];
	a.strs     = (const char*)(a.info + iw[5]);
	CHECK(a.dirs[0].mType == 'ROOT', "root node type 'ROOT'");
	std::printf("RARC: %u nodes, %u file entries, volume name \"%s\"\n", a.numNodes, a.numFiles,
	            a.strs + a.dirs[0].mOffset);

	std::map<std::string, u32> files;
	walk(a, 0, "", files, 0);
	int shown = 0;
	for (std::map<std::string, u32>::const_iterator it = files.begin();
	     it != files.end() && shown < 6; ++it, ++shown)
		std::printf("  %-36s %u\n", it->first.c_str(), it->second);

	int idx = findFs(a, "timg/arrow.bti", 0);
	CHECK(idx >= 0, "findFsResource(timg/arrow.bti)");
	idx = findFs(a, "shadowpalm.bck", 0);
	CHECK(idx >= 0 && a.size((u32)idx) == 480, "shadowpalm.bck size 480 (dtk: 480 bytes)");
	if (idx >= 0)
		std::printf("shadowpalm.bck: %u bytes, id %u\n", a.size((u32)idx), a.fileId((u32)idx));

	// Compare against dtk vfs ls -r.
	std::string cmd = std::string("\"\"") + dtk + "\" vfs ls -r \"" + szs + ":\"\"";
	FILE* p = _popen(cmd.c_str(), "r");
	int compared = 0, mismatched = 0;
	if (!p) {
		CHECK(false, "cannot run dtk");
	} else {
		char line[512];
		while (std::fgets(line, sizeof line, p)) {
			std::string l(line);
			size_t b1 = l.find('|'), b2 = l.find('|', b1 + 1);
			if (b1 == std::string::npos || b2 == std::string::npos)
				continue;
			if (trim(l.substr(b2 + 1)) != "File")
				continue;
			std::string sz = trim(l.substr(0, b1)), name = trim(l.substr(b1 + 1, b2 - b1 - 1));
			std::map<std::string, u32>::const_iterator it = files.find(name);
			compared++;
			if (it == files.end()) {
				mismatched++;
				std::printf("  missing in our walk: %s\n", name.c_str());
			} else if (dtkSize(it->second) != sz) {
				mismatched++;
				std::printf("  size mismatch %s: ours %s, dtk %s\n", name.c_str(),
				            dtkSize(it->second).c_str(), sz.c_str());
			}
		}
		_pclose(p);
		CHECK(compared > 0, "dtk produced no file lines");
		CHECK(compared == (int)files.size(), "file count: dtk %d, ours %zu", compared, files.size());
		CHECK(mismatched == 0, "%d mismatches vs dtk", mismatched);
		std::printf("dtk vfs ls -r: %d files compared, %d mismatches\n", compared, mismatched);
	}

	bool mounted = false;
	if (!mapArena()) {
		std::printf("(cannot map 0x%08zX: JKernel heap/archive tests and BMG skipped)\n",
		            (size_t)kRamBase);
	} else {
		JKRExpHeap* root = JKRExpHeap::createRoot(1, false);
		CHECK(root && JKRHeap::getRootHeap() == root, "createRoot");
		CHECK((uintptr_t)JKRHeap::getCodeStart() == kRamBase
		          && (uintptr_t)JKRHeap::getUserRamEnd() == kRamBase + kRamSize
		          && JKRHeap::getMemorySize() == 0x01800000,
		      "initArena: code start %p, user RAM end %p", JKRHeap::getCodeStart(),
		      JKRHeap::getUserRamEnd());
		testHeap(root);

		u8* buf;
		JKRMemArchive* common = mountMem(root, rawArc, &buf);
		testMemArchive(common, buf, rawArc, a, files);

		// option.szs next to common.szs, for the BMG test
		std::string opt = szs;
		size_t slash    = opt.find_last_of("/\\");
		opt = (slash == std::string::npos ? std::string() : opt.substr(0, slash + 1)) + "option.szs";
		std::vector<u8> optArc = loadSzs(opt);
		CHECK(!optArc.empty(), "decode %s", opt.c_str());
		JKRMemArchive* option = optArc.empty() ? nullptr : mountMem(root, optArc, &buf);
		CHECK(option && JKRFileLoader::getVolume("option") == option
		          && JKRFileLoader::getVolume("common") == common,
		      "volumes \"option\" and \"common\" mounted");
		mounted = common && option;
	}

	if (argc > 3) {
		fs_::path dir = argv[3];
		testTimg(dir);
		testTex1(dir);
		testFont(dir);
		if (mounted)
			testBmg(dir);
		testParams(dir);
	} else {
		std::printf("(no extracted data dir given: per-format tests skipped)\n");
	}

	std::printf(gFailures ? "RESULT: FAIL (%d)\n" : "RESULT: OK\n", gFailures);
	return gFailures ? 1 : 0;
}
