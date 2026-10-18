// Host test for the collision (.col) swap pass and the 64-bit group loader
// (port/include/port_map_swap.h, TMapCollisionBase::init). Run through test_col.py.
//
//   test_col.exe <files.txt>
//
// files.txt lists "path-on-disk". For each file the test:
//   - registers the raw big-endian bytes as the resource "/scene/test.col"
//     (JKRFileLoader::getGlbResource is replaced by a lookup in the test);
//   - builds a real TMapCollisionData (grid, check-data pool, lists) and loads the
//     file twice through the real TMapCollisionStatic::init -> TMapCollisionBase::init
//     (swap + native groups) -> initCheckData -> initAllCheckData -> setCheckData
//     -> TBGCheckData::setVertex -> addCheckDataToGrid, exactly as in the game;
//   - prints one "== <name>" section: counts, then one line per triangle
//     (bgType data sound unk7 p1 p2 p3 minY maxY) read from the TBGCheckData the
//     real code built. test_col.py compares it with values read big-endian in Python.
// The second load reuses the already-swapped buffer (the shared-resource case)
// and must give the same triangles.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define private public
#define protected public
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JSupport/JSUMemoryInputStream.hpp>
#include <Map/MapCollisionData.hpp>
#include <Map/MapCollisionEntry.hpp>
#include <Map/MapData.hpp>
#undef private
#undef protected

// Host math the units above call (the dolphin mtx library is not part of this test).
extern "C" void PSMTXIdentity(Mtx m)
{
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 4; j++)
			m[i][j] = (i == j) ? 1.0f : 0.0f;
}
extern "C" void PSMTXCopy(Mtx s, Mtx d) { memcpy(d, s, sizeof(Mtx)); }
extern "C" void PSMTXMultVecArray(Mtx m, Vec* s, Vec* d, u32 n)
{
	for (u32 i = 0; i < n; i++) {
		Vec v = s[i];
		d[i].x = m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3];
		d[i].y = m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3];
		d[i].z = m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3];
	}
}
void MsVECNormalize(Vec* src, Vec* dst)
{
	float l = sqrtf(src->x * src->x + src->y * src->y + src->z * src->z);
	if (l > 0.0f) {
		dst->x = src->x / l;
		dst->y = src->y / l;
		dst->z = src->z / l;
	}
}

static std::vector<unsigned char> gFile;
static int gFailures = 0;

void* JKRFileLoader::getGlbResource(const char* name)
{
	return strcmp(name, "/scene/test.col") == 0 ? gFile.data() : nullptr;
}

static void be32(std::vector<unsigned char>& v, unsigned x)
{
	v.push_back(x >> 24);
	v.push_back(x >> 16);
	v.push_back(x >> 8);
	v.push_back(x);
}

static void dump(const TMapCollisionBase& c, std::vector<std::string>& out)
{
	char buf[512];
	for (u32 i = 0; i < c.mCheckDataNum; i++) {
		const TBGCheckData& d = c.mCheckDatas[i];
		snprintf(buf, sizeof buf, "t %u %d %u %u %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g",
		         d.mBGType, d.mData, d.mSoundMaterial, d.unk7, d.mPoint1.x, d.mPoint1.y,
		         d.mPoint1.z, d.mPoint2.x, d.mPoint2.y, d.mPoint2.z, d.mPoint3.x, d.mPoint3.y,
		         d.mPoint3.z, d.mMinY, d.mMaxY);
		out.push_back(buf);
	}
}

static std::vector<std::string> load(int pass)
{
	// grid 64 x 64 cells (+-32768), generous pools: every triangle may touch many cells
	std::vector<unsigned char> hdr;
	be32(hdr, 64);
	be32(hdr, 64);
	be32(hdr, 60000);   // check data
	be32(hdr, 600000);  // list nodes
	be32(hdr, 1000);     // warp nodes
	JSUMemoryInputStream stream(hdr.data(), (s32)hdr.size());
	TMapCollisionData* data = new TMapCollisionData;
	data->init(stream);

	TMapCollisionStatic* col = new TMapCollisionStatic;
	col->init("/scene/test.col", 4, nullptr); // 4: use the vertices as they are
	std::vector<std::string> out;
	char buf[128];
	snprintf(buf, sizeof buf, "counts %u %u", col->mVertexNum, col->mCollisionGroupNum);
	out.push_back(buf);
	snprintf(buf, sizeof buf, "pool %u", data->unk34);
	out.push_back(buf);
	dump(*col, out);
	if (col->mCheckDataNum != data->unk34) {
		printf("FAIL pass %d: check data %u vs pool %u\n", pass, col->mCheckDataNum, data->unk34);
		gFailures++;
	}
	// sanity of the native structs themselves
	for (u32 i = 0; i < col->mCollisionGroupNum; i++) {
		auto& g = col->mCollisionGroups[i];
		unsigned char* lo = gFile.data();
		unsigned char* hi = lo + gFile.size();
		if ((unsigned char*)g.mIndices < lo || (unsigned char*)(g.mIndices + g.mTriangleNum * 3) > hi
		    || g.mSoundMaterials < lo || g.mSoundMaterials + g.mTriangleNum > hi || g.unk10 < lo
		    || g.unk10 + g.mTriangleNum > hi) {
			printf("FAIL group %u points outside the file\n", i);
			gFailures++;
		}
	}
	delete[] col->mCollisionGroups;
	delete col;
	delete[] data->unk14;
	delete[] data->unk18;
	delete[] data->unk28;
	delete[] data->unk2C;
	delete[] data->unk30;
	delete data;
	return out;
}

int main(int argc, char** argv)
{
	std::ifstream list(argv[1]);
	std::string path;
	int files = 0;
	while (std::getline(list, path)) {
		if (path.empty())
			continue;
		std::ifstream f(path, std::ios::binary);
		gFile.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
		std::string rel = path;
		printf("== %s\n", rel.c_str());
		std::vector<std::string> a = load(0);
		std::vector<std::string> b = load(1); // shared, already swapped buffer
		if (a != b) {
			printf("FAIL second load differs: %s\n", path.c_str());
			gFailures++;
		}
		for (auto& s : a)
			printf("%s\n", s.c_str());
		files++;
	}
	printf("SUMMARY files %d failures %d\n", files, gFailures);
	return gFailures ? 1 : 0;
}



