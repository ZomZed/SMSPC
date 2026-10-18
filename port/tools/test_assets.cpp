// Host test for the .pad swap (port_asset_swap.h) and the .bcr BCSV swap in
// Koga::ToolData::Attach, through the real loaders: TMarioInputReplay
// (src/Player/MarioRecord.cpp) and Koga::ToolData (src/MarioUtil/ToolData.cpp).
// Run via test_assets.py.
//
//   test_assets.exe <files.txt>
//
// .pad: init() twice on the same buffer (idempotency, several replays can share
//       one resource), then play() every frame; prints the frame count and an
//       FNV-1a hash over every frame's outputs.
// .bcr: Attach() twice, then prints start_frame / end_frame / type per entry,
//       exactly the fields TMovieRumble::readCurInfo reads.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <Player/MarioRecord.hpp>
#include <MarioUtil/ToolData.hpp>

static unsigned int fnv(unsigned int h, const void* p, int n)
{
	const unsigned char* b = (const unsigned char*)p;
	for (int i = 0; i < n; i++)
		h = (h ^ b[i]) * 16777619u;
	return h;
}

static void testPad(const std::string& path, std::vector<unsigned char>& buf)
{
	TMarioInputReplay r;
	r.init(buf.data());
	r.init(buf.data());
	r.reset();
	r.start();
	unsigned int h = 2166136261u, frames = 0;
	for (;;) {
		f32 mag = 0;
		s16 yaw = 0;
		u32 btn = 0, just = 0;
		u8 p5 = 0, p6 = 0;
		if (!r.play(&mag, &yaw, &btn, &just, &p5, &p6))
			break;
		h = fnv(h, &mag, 4);
		h = fnv(h, &yaw, 2);
		h = fnv(h, &btn, 4);
		h = fnv(h, &just, 4);
		h = fnv(h, &p5, 1);
		h = fnv(h, &p6, 1);
		++frames;
	}
	printf("pad %s %u %08x\n", path.c_str(), frames, h);
}

static void testBcr(const std::string& path, std::vector<unsigned char>& buf)
{
	Koga::ToolData t;
	t.Attach(buf.data());
	t.Attach(buf.data());
	int n = t.mData->mNumEntries;
	printf("bcr %s %d\n", path.c_str(), n);
	for (int i = 0; i < n; i++) {
		s32 start = -1, end = -1;
		const char* type = "";
		BOOL a = t.GetValue(i, "start_frame", start);
		BOOL b = t.GetValue(i, "end_frame", end);
		BOOL c = t.GetValue(i, "type", type);
		printf("  %d %d %d %d %s\n", (int)(a && b && c), start, end, t.isIndexValid(i), type);
	}
}

int main(int argc, char** argv)
{
	if (argc < 2)
		return 2;
	std::ifstream list(argv[1]);
	std::string path;
	while (std::getline(list, path)) {
		if (path.empty())
			continue;
		std::ifstream f(path, std::ios::binary);
		std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
		                               std::istreambuf_iterator<char>());
		buf.resize(buf.size() + 64, 0); // the GC heap would hand out padded blocks
		if (path.size() > 4 && path.compare(path.size() - 4, 4, ".pad") == 0)
			testPad(path, buf);
		else
			testBcr(path, buf);
	}
	return 0;
}
