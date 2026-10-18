// Host test for the JAudio endianness passes (see port/ENDIAN.md, port_jaudio_swap.h).
//
//   python port/tools/test_jaudio.py      (builds with build_port.py's clang flags and runs)
//
// Loads the real mSound.aaf (extracted from nintendo.szs by the driver) and the
// real AudioRes files from the disc folder, and for each piece:
//   * walks the init-data command list the way JAIBasic::checkInitDataOnMemory
//     (SMS_PORT) does, with sms_be32 reads;
//   * runs the swap pass that the game runs (port_swap_ibnk / wsys / ...) and
//     checks the result with the real parser structs (JASBNKParser.hpp,
//     JASWSParser.hpp) walked exactly like createBasicBank / createBasicWaveBank:
//     counts, offsets inside the blob, finite floats, wave ranges inside the .aw
//     file and archive entries inside sequence.arc;
//   * checks the passes are idempotent where they promise to be.
// Pointer-size clean; builds for x86_64 and i686.

#include <JSystem/JAudio/JASystem/JASBNKParser.hpp>
#include <JSystem/JAudio/JASystem/JASWSParser.hpp>
#include <JSystem/JAudio/JAInterface/JAIData.hpp>
#include <JSystem/JAudio/JASystem/JASDSPInterface.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
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

static std::vector<u8> readFile(const std::string& p)
{
	std::ifstream f(p, std::ios::binary);
	return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static u64 hashBytes(const u8* p, size_t n)
{
	u64 h = 1469598103934665603ull;
	for (size_t i = 0; i < n; i++)
		h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

static bool sane(float f, float lo, float hi) { return std::isfinite(f) && f >= lo && f <= hi; }

using namespace JASystem;

// ---- IBNK -----------------------------------------------------------------
static int gInst, gOsc, gKeymap, gVmap, gPerc, gPmap;

static void checkOscTable(const u8* base, u32 size, BNKParser::TOffset<s16> o)
{
	if (!o.mOffset)
		return;
	CHECK(o.mOffset + 6 <= size, "osc table offset 0x%x outside bank", o.mOffset);
	const s16* t = (const s16*)(base + o.mOffset);
	int n         = 0;
	s16 mode;
	do {
		mode = t[n * 3];
		CHECK(mode >= 0 && mode <= 15, "osc mode %d", mode);
		n++;
	} while (mode <= 0xa && n < 64);
	CHECK(n < 64 && mode >= 0xd, "osc table end mode %d after %d triples", mode, n);
}

static void checkVmap(const u8* base, u32 size, BNKParser::TVmap* v)
{
	CHECK((u8*)v + sizeof(*v) <= base + size, "vmap outside");
	CHECK(sane(v->unk8, 0.0f, 16.0f), "vmap volume %f", v->unk8);
	CHECK(sane(v->unkC, 0.0f, 16.0f), "vmap pitch %f", v->unkC);
	CHECK(v->unk4 < 0x10000, "vmap wave id 0x%x", v->unk4); // u32 whose low half is the wave id
	gVmap++;
}

static void testBank(const u8* raw, u32 size, int idx)
{
	std::vector<u8> bank(raw, raw + size);
	CHECK(port_swap_ibnk(bank.data()) == 1, "bank %d: first swap", idx);
	u64 h = hashBytes(bank.data(), size);
	CHECK(port_swap_ibnk(bank.data()) == 0, "bank %d: second swap must be a no-op", idx);
	CHECK(hashBytes(bank.data(), size) == h, "bank %d: second swap changed data", idx);

	auto* hdr = (BNKParser::THeader*)bank.data();
	u32 magic;
	std::memcpy(&magic, bank.data(), 4);
	CHECK(magic == 'IBNK', "bank %d magic 0x%x", idx, magic);
	CHECK(*((u32*)bank.data() + 1) == size, "bank %d size field", idx);

	int insts = 0;
	for (int i = 0; i < 0x80; i++) {
		BNKParser::TInst* in = hdr->mInstOffsets[i].ptr(hdr);
		if (!in)
			continue;
		insts++;
		gInst++;
		CHECK(hdr->mInstOffsets[i].mOffset + 0x30 <= size, "inst offset");
		CHECK(sane(in->unk8, 0.0f, 16.0f), "bank %d inst %d volume %f", idx, i, in->unk8);
		CHECK(sane(in->unkC, 0.0f, 16.0f), "bank %d inst %d pitch %f", idx, i, in->unkC);
		for (int j = 0; j < 2; j++) {
			BNKParser::TOsc* o = in->mOscOffsets[j].ptr(hdr);
			if (!o)
				continue;
			gOsc++;
			CHECK(sane(o->unk4, 0.0f, 1e6f), "osc rate %f", o->unk4);
			CHECK(sane(o->unk10, -16.0f, 16.0f), "osc width %f", o->unk10);
			CHECK(sane(o->unk14, -16.0f, 16.0f), "osc vertex %f", o->unk14);
			checkOscTable(bank.data(), size, o->mAdsTableOffset);
			checkOscTable(bank.data(), size, o->mRelTableOffset);
		}
		for (int j = 0; j < 2; j++)
			if (auto* r = in->mRandOffsets[j].ptr(hdr)) {
				CHECK(sane(r->unk4, -1e6f, 1e6f) && sane(r->unk8, -1e6f, 1e6f), "rand");
			}
		for (int j = 0; j < 2; j++)
			if (auto* s = in->mSenseOffsets[j].ptr(hdr)) {
				CHECK(sane(s->unk4, -1e6f, 1e6f) && sane(s->unk8, -1e6f, 1e6f), "sense");
			}
		CHECK(in->mKeyRegionCount >= 1 && in->mKeyRegionCount <= 0x80,
		      "bank %d inst %d key regions %u", idx, i, in->mKeyRegionCount);
		for (u32 j = 0; j < in->mKeyRegionCount && j < 0x80; j++) {
			BNKParser::TKeymap* k = in->mKeymapOffsets[j].ptr(hdr);
			CHECK(k != nullptr, "keymap %u null", j);
			if (!k)
				continue;
			gKeymap++;
			CHECK(k->unk4 >= 1 && k->unk4 <= 0x80, "velo regions %u", k->unk4);
			for (u32 v = 0; v < k->unk4 && v < 0x80; v++)
				if (auto* vm = k->mVmapOffsets[v].ptr(hdr))
					checkVmap(bank.data(), size, vm);
		}
	}
	for (int i = 0; i < 12; i++) {
		BNKParser::TPerc* p = hdr->mPercOffsets[i].ptr(hdr);
		if (!p)
			continue;
		gPerc++;
		CHECK(p->mMagic == 'PERC' || p->mMagic == 'PER2', "perc magic 0x%x", p->mMagic);
		for (int j = 0; j < 0x80; j++) {
			BNKParser::TPmap* m = p->mPmapOffsets[j].ptr(hdr);
			if (!m)
				continue;
			gPmap++;
			CHECK(sane(m->unk0, 0.0f, 16.0f) && sane(m->unk4, 0.0f, 16.0f), "pmap vol/pitch");
			CHECK(m->mVeloRegionCount >= 1 && m->mVeloRegionCount <= 0x80, "pmap regions %u",
			      m->mVeloRegionCount);
			for (u32 v = 0; v < m->mVeloRegionCount && v < 0x80; v++)
				if (auto* vm = m->mVeloRegionOffsets[v].ptr(hdr))
					checkVmap(bank.data(), size, vm);
		}
	}
	CHECK(insts > 0, "bank %d has no instruments", idx);
}

// ---- WSYS -----------------------------------------------------------------
static int gWaves, gGroups;

static void testWsys(const u8* raw, u32 size, int idx, const std::string& banksDir)
{
	std::vector<u8> ws(raw, raw + size);
	CHECK(port_swap_wsys(ws.data()) == 1, "wsys %d: first swap", idx);
	u64 h = hashBytes(ws.data(), size);
	CHECK(port_swap_wsys(ws.data()) == 0, "wsys %d: second swap must be a no-op", idx);
	CHECK(hashBytes(ws.data(), size) == h, "wsys %d: second swap changed data", idx);

	auto* hdr = (WSParser::THeader*)ws.data();
	auto* cg  = hdr->mCtrlGroupOffset.ptr(hdr);
	auto* ab  = hdr->mArchiveBankOffset.ptr(hdr);
	CHECK(cg && ab, "wsys %d: missing WBCT/WINF", idx);
	if (!cg || !ab)
		return;
	u32 magic;
	std::memcpy(&magic, ab, 4);
	CHECK(magic == 'WINF', "WINF magic 0x%x", magic);
	std::memcpy(&magic, cg, 4);
	CHECK(magic == 'WBCT', "WBCT magic 0x%x", magic);
	CHECK(cg->mCtrlGroupCount >= 1 && cg->mCtrlGroupCount < 0x100, "wsys %d groups %u", idx,
	      cg->mCtrlGroupCount);
	for (u32 g = 0; g < cg->mCtrlGroupCount; g++) {
		gGroups++;
		auto* scene = cg->mCtrlSceneOffsets[g].ptr(hdr);
		std::memcpy(&magic, scene, 4);
		CHECK(magic == 'SCNE', "SCNE magic 0x%x", magic);
		auto* ctrl = scene->mCtrlOffset.ptr(hdr);
		std::memcpy(&magic, ctrl, 4);
		CHECK(magic == 'C-DF', "C-DF magic 0x%x", magic);
		auto* arc = ab->mArchiveOffsets[g].ptr(hdr);
		// the wave count sits in the last 4 bytes of the 0x74-byte name field
		u32 arcCount;
		std::memcpy(&arcCount, (u8*)arc + 0x70, 4);
		CHECK(arcCount == ctrl->mWaveCount, "wsys %d group %u: archive count %u != ctrl %u", idx,
		      g, arcCount, ctrl->mWaveCount);
		CHECK(arc->mFileName[0] != 0 && std::strstr(arc->mFileName, ".aw"), "arc name '%s'",
		      arc->mFileName);
		std::string awPath = banksDir + "/" + arc->mFileName;
		u64 awSize         = std::filesystem::exists(awPath) ? std::filesystem::file_size(awPath) : 0;
		CHECK(awSize != 0, "wave archive %s not found", awPath.c_str());
		for (u32 w = 0; w < ctrl->mWaveCount; w++) {
			gWaves++;
			WSParser::TWave* wv = arc->mWaveOffsets[w].ptr(hdr);
			CHECK(wv && (u8*)wv + 0x2C <= ws.data() + size, "wave %u offset", w);
			if (!wv)
				continue;
			CHECK(sane(wv->unk4, 1000.0f, 64000.0f), "wave sample rate %f", wv->unk4);
			// mOffset/unkC: offset and length of the ADPCM data in the .aw
			CHECK((u64)wv->mOffset + wv->unkC <= awSize,
			      "wsys %d %s wave %u: offset 0x%x + len 0x%x beyond .aw size 0x%llx", idx,
			      arc->mFileName, w, wv->mOffset, wv->unkC, (unsigned long long)awSize);
			WSParser::TCtrlWave* cw = ctrl->mCtrlWaveOffsets[w].ptr(hdr);
			CHECK(cw != nullptr, "ctrl wave %u null", w);
			if (cw)
				CHECK(JSULoHalf(cw->unk0) < 0x1000, "ctrl wave id 0x%x", cw->unk0);
		}
	}
}

// ---- aaf walk ---------------------------------------------------------------
static u32 W(const std::vector<u8>& f, u32 i) { return sms_be32(f.data() + i * 4); }

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (argc < 4) {
		std::printf("usage: test_jaudio mSound.aaf AudioRes-dir bas-dir\n");
		return 2;
	}
	std::vector<u8> aaf = readFile(argv[1]);
	std::string audio   = argv[2];
	std::string basDir  = argv[3];
	CHECK(aaf.size() > 0x1000, "aaf missing");
	if (aaf.empty())
		return 1;
	std::printf("mSound.aaf: %zu bytes\n", aaf.size());

	u32 i = 0;
	int banks = 0, waveBanks = 0;
	bool done = false;
	while (!done && i * 4 < aaf.size()) {
		u32 cmd = W(aaf, i++);
		switch (cmd) {
		case 0:
			done = true;
			break;
		case 1: { // sound tables
			CHECK(W(aaf, i + 2) == 0, "expected a single SE table");
			u32 off = W(aaf, i), sz = W(aaf, i + 1);
			i += 3;
			CHECK(off + sz <= aaf.size(), "SE table range");
			std::vector<u8> t(aaf.begin() + off, aaf.begin() + off + sz);
			port_swap_jaisoundtable(t.data(), sz);
			u16 total = *(u16*)(t.data() + 4);
			u32 sum = 0, maxCat = 0;
			for (int c = 0; c < 18; c++) {
				u16 cnt   = *(u16*)(t.data() + 6 + 4 * c);
				u16 start = *(u16*)(t.data() + 8 + 4 * c);
				sum += cnt;
				CHECK((u32)start + cnt <= total, "category %d range %u+%u > %u", c, start, cnt,
				      total);
				if (c < 0x10 && cnt)
					maxCat = c + 1;
			}
			CHECK(total == (sz - 0x50) / 0x10, "table total %u vs size %u", total, (sz - 0x50) / 0x10);
			CHECK(t[3] > 0 && t[3] <= 0x20, "category byte %u", t[3]);
			std::printf("SE table: %u bytes, %u sounds, %u in categories, %u categories used, byte3=%u\n",
			            sz, total, sum, maxCat, t[3]);
			const JAISoundInfo* info = (const JAISoundInfo*)(t.data() + 0x50);
			for (u32 s = 0; s < total; s++) {
				CHECK(sane(info[s].mPitch, 0.0f, 16.0f), "sound %u pitch %f", s, info[s].mPitch);
				CHECK(info[s].mVolume <= 0x7F || info[s].mVolume == 0xFF, "sound %u vol %u", s,
				      info[s].mVolume);
			}
			break;
		}
		case 2: { // bank list
			while (W(aaf, i) != 0) {
				u32 off = W(aaf, i), sz = W(aaf, i + 1), wb = W(aaf, i + 2);
				CHECK(off + sz <= aaf.size(), "bank range");
				CHECK(wb < 0x100, "wave bank number %u", wb);
				testBank(aaf.data() + off, sz, banks);
				banks++;
				i += 3;
			}
			i++;
			break;
		}
		case 3: { // wave bank list
			while (W(aaf, i) != 0) {
				u32 off = W(aaf, i), sz = W(aaf, i + 1), timing = W(aaf, i + 2);
				CHECK(off + sz <= aaf.size(), "wsys range");
				CHECK(timing <= 2, "load timing %u", timing);
				testWsys(aaf.data() + off, sz, waveBanks, audio + "/Banks");
				waveBanks++;
				i += 3;
			}
			i++;
			break;
		}
		case 4: { // sequence archive header
			u32 off = W(aaf, i), sz = W(aaf, i + 1);
			i += 3;
			std::vector<u8> t(aaf.begin() + off, aaf.begin() + off + sz);
			port_swap_barc(t.data(), sz);
			u32 count = *(u32*)(t.data() + 0xC);
			CHECK(count == (sz - 0x20) / 0x20, "BARC count %u vs size", count);
			CHECK(std::strcmp((char*)t.data() + 0x10, "sequence.arc") == 0, "BARC file name");
			u64 arcSize = std::filesystem::file_size(audio + "/Seqs/sequence.arc");
			int real    = 0;
			for (u32 e = 0; e < count; e++) {
				u8* ent  = t.data() + 0x20 + 0x20 * e;
				u16 unkE = *(u16*)(ent + 0xE);
				u32 aoff = *(u32*)(ent + 0x18), asz = *(u32*)(ent + 0x1C);
				if (unkE == 0xFFFF) {
					real++;
					CHECK((u64)aoff + asz <= arcSize, "entry %u: 0x%x+0x%x beyond arc 0x%llx", e,
					      aoff, asz, (unsigned long long)arcSize);
					CHECK(ent[0] != 0, "entry %u has no name", e);
				} else {
					CHECK(unkE < count, "entry %u link %u", e, unkE);
				}
			}
			std::printf("BARC: %u entries (%d direct), sequence.arc 0x%llx bytes\n", count, real,
			            (unsigned long long)arcSize);
			break;
		}
		case 5: { // stream list
			u32 off = W(aaf, i), sz = W(aaf, i + 1);
			i += 3;
			std::vector<u8> t(aaf.begin() + off, aaf.begin() + off + sz);
			port_swap_stream_entries(t.data(), sz);
			JAIData::FabricatedStreamEntry* e = (JAIData::FabricatedStreamEntry*)t.data();
			static_assert(sizeof(JAIData::FabricatedStreamEntry) == 0x30, "stream entry size");
			std::printf("stream list: first entry '%s' unk20=0x%x\n", e->mFileName, e->unk20);
			CHECK(std::filesystem::exists(audio + "/Streams/" + e->mFileName), "stream file '%s'",
			      e->mFileName);
			break;
		}
		case 6: { // scene list
			u32 off = W(aaf, i), sz = W(aaf, i + 1);
			i += 3;
			u32 n = sms_be32(aaf.data() + off);
			CHECK(n >= 1 && n < 0x40, "scene count %u", n);
			for (u32 s = 0; s < n; s++) {
				u32 o = sms_be32(aaf.data() + off + 4 + 4 * s);
				CHECK(o >= 4 + 4 * n && o < sz, "scene %u offset 0x%x", s, o);
			}
			std::printf("scene list: %u scenes, %u bytes\n", n, sz);
			break;
		}
		case 7: { // fx scenes
			u32 off = W(aaf, i), sz = W(aaf, i + 1);
			i += 3;
			std::vector<u8> t(aaf.begin() + off, aaf.begin() + off + sz);
			port_swap_fxscene(t.data(), sz);
			auto* tab = (u32*)t.data();
			CHECK(tab[0] >= 1 && tab[0] < 0x10, "fx scenes %u", tab[0]);
			for (u32 s = 0; s < tab[0]; s++) {
				u32 o = tab[5 + s];
				CHECK(o + 4 * sizeof(JASystem::DSPInterface::FxlineConfig_) <= sz, "fx scene offset");
				auto* cfg = (JASystem::DSPInterface::FxlineConfig_*)(t.data() + o);
				static_assert(sizeof(JASystem::DSPInterface::FxlineConfig_) == 0x20, "fxline size");
				for (int l = 0; l < 4; l++)
					CHECK(cfg[l].unk2 < 0x40 && cfg[l].unk6 < 0x40,
					      "fx scene %u line %d send idx %u/%u", s, l, cfg[l].unk2, cfg[l].unk6);
			}
			std::printf("fx scenes: %u, buffer sizes %u/%u/%u/%u\n", tab[0], tab[1], tab[2], tab[3],
			            tab[4]);
			break;
		}
		default:
			CHECK(false, "unknown aaf command %u", cmd);
			done = true;
		}
	}
	std::printf("banks: %d (instruments %d, oscs %d, keymaps %d, vmaps %d, percs %d, pmaps %d)\n",
	            banks, gInst, gOsc, gKeymap, gVmap, gPerc, gPmap);
	std::printf("wave systems: %d (groups %d, waves %d)\n", waveBanks, gGroups, gWaves);
	CHECK(banks == 13, "expected 13 banks, got %d", banks);
	CHECK(waveBanks == 3, "expected 3 wave systems, got %d", waveBanks);

	// .bas animation sound data
	int bas = 0;
	for (auto& e : std::filesystem::directory_iterator(basDir)) {
		if (e.path().extension() != ".bas")
			continue;
		std::vector<u8> d = readFile(e.path().string());
		u16 rawCount      = (u16)((d[0] << 8) | d[1]);
		CHECK(port_swap_bas(d.data()) == 1, "bas first swap");
		u64 h = hashBytes(d.data(), d.size());
		CHECK(port_swap_bas(d.data()) == 0 && hashBytes(d.data(), d.size()) == h,
		      "bas not idempotent");
		u16 count = *(u16*)d.data();
		CHECK(count == rawCount && 8 + 0x20u * count == d.size(), "bas %s count %u size %zu",
		      e.path().filename().string().c_str(), count, d.size());
		for (u16 k = 0; k < count; k++) {
			u8* ent = d.data() + 8 + 0x20 * k;
			CHECK(sane(*(float*)(ent + 4), -1e5f, 1e5f) && sane(*(float*)(ent + 8), -1e5f, 1e5f),
			      "bas frame range");
			CHECK(sane(*(float*)(ent + 0xC), 0.0f, 16.0f), "bas pitch");
			CHECK((*(u32*)ent >> 16) < 0x9000, "bas sound id 0x%x", *(u32*)ent);
		}
		bas++;
	}
	std::printf(".bas files: %d\n", bas);
	CHECK(bas > 100, "too few .bas files");

	// sequence-embedded oscillator table
	{
		u8 raw[18] = { 0, 1, 0, 0x10, 0x7F, 0xFF, 0, 0xF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
		s16* n     = seq_osc_table_native(raw);
		CHECK(n[0] == 1 && n[1] == 0x10 && n[2] == 0x7FFF && n[3] == 0xF, "seq osc table %d %d %d %d",
		      n[0], n[1], n[2], n[3]);
		CHECK(seq_osc_table_native(raw) == n, "seq osc table cache");
		raw[3] = 0x20; // buffer reused by another sequence
		CHECK(seq_osc_table_native(raw)[1] == 0x20, "seq osc table revalidation");
	}

	std::printf(gFailures ? "FAILED: %d checks\n" : "OK\n", gFailures);
	return gFailures ? 1 : 0;
}

