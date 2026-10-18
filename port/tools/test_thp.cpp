// Standalone test for port/src/port_thp_decode.cpp (driven by test_thp.py).
//
//   test_thp <file.thp> <out_dir> [all] <frame> [<frame> ...]
//
// Walks the THP frame chain like THPRead.c, decodes the requested frames into
// GX I8 tiled Y/U/V planes, untiles them and converts to RGB with the THP TEV
// setup's constants (THPGXYuv2RgbSetup), so the PPMs show what the GX draw path
// produces. "all" also decodes every frame and reports errors and timing.

#include "../src/port_thp_decode.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static uint32_t be32(const uint8_t* p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint8_t texel(const std::vector<uint8_t>& plane, uint32_t w, uint32_t x, uint32_t y)
{
	uint32_t tilesPerRow = (w + 7) / 8;
	return plane[((y >> 2) * tilesPerRow + (x >> 3)) * 32 + (y & 3) * 8 + (x & 7)];
}

static float clamp255(float v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// The four TEV stages of THPGXYuv2RgbSetup, evaluated in float (K colours /255).
static void tevYuvToRgb(float Y, float U, float V, uint8_t* rgb)
{
	// stage 0: C0 + K0 * U ; alpha A0 - K0a * U    (C0 = {-90, 0, -114, 135})
	float r = -90.0f, b = -114.0f + 226.0f / 255.0f * U;
	float a = 135.0f - 88.0f / 255.0f * U;
	// stage 1: (prev + K1 * V) * 2 ; alpha prev - K1a * V   (K1 = {179, 0, 0, 182})
	r = (r + 179.0f / 255.0f * V) * 2.0f;
	b = b * 2.0f;
	a = a - 182.0f / 255.0f * V;
	// stage 2: prev + Y, clamped
	r = clamp255(r + Y);
	b = clamp255(b + Y);
	a = clamp255(a + Y);
	// stage 3: K2 = {255, 0, 255} selects R and B from colour, G from alpha
	rgb[0] = (uint8_t)(r + 0.5f);
	rgb[1] = (uint8_t)(a + 0.5f);
	rgb[2] = (uint8_t)(b + 0.5f);
}

int main(int argc, char** argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "usage: test_thp <file.thp> <out_dir> [all] <frame>...\n");
		return 2;
	}
	FILE* f = std::fopen(argv[1], "rb");
	if (!f) {
		std::fprintf(stderr, "cannot open %s\n", argv[1]);
		return 2;
	}
	std::fseek(f, 0, SEEK_END);
	long fileSize = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::vector<uint8_t> file((size_t)fileSize);
	if (std::fread(file.data(), 1, file.size(), f) != file.size()) {
		std::fprintf(stderr, "read failed\n");
		return 2;
	}
	std::fclose(f);

	const uint8_t* h = file.data();
	if (std::memcmp(h, "THP\0", 4) != 0) {
		std::fprintf(stderr, "not a THP file\n");
		return 2;
	}
	uint32_t version = be32(h + 4), numFrames = be32(h + 0x14), firstFrameSize = be32(h + 0x18);
	uint32_t compInfoOffset = be32(h + 0x20), movieDataOffset = be32(h + 0x28);
	float frameRate;
	{
		uint32_t v = be32(h + 0x10);
		std::memcpy(&frameRate, &v, 4);
	}
	const uint8_t* ci = h + compInfoOffset;
	uint32_t numComp = be32(ci);
	uint32_t width = 0, height = 0, videoType = 0;
	const uint8_t* info = ci + 4 + 16;
	for (uint32_t i = 0; i < numComp; i++) {
		if (ci[4 + i] == 0) {
			width     = be32(info);
			height    = be32(info + 4);
			videoType = be32(info + 8);
			info += 12;
		} else if (ci[4 + i] == 1) {
			info += 16;
		}
	}
	std::printf("version %x frames %u rate %.3f comps %u video %ux%u type %u\n", version,
	            numFrames, frameRate, numComp, width, height, videoType);

	bool all = false;
	std::vector<int> want;
	for (int i = 3; i < argc; i++) {
		if (std::strcmp(argv[i], "all") == 0)
			all = true;
		else
			want.push_back(std::atoi(argv[i]));
	}

	std::vector<uint8_t> Y(((width + 7) / 8) * ((height + 3) / 4) * 32);
	std::vector<uint8_t> U(((width / 2 + 7) / 8) * ((height / 2 + 3) / 4) * 32);
	std::vector<uint8_t> V(U.size());

	uint32_t offset = movieDataOffset, size = firstFrameSize;
	int errors = 0;
	double totalMs = 0;
	int decoded = 0;
	for (uint32_t frame = 0; frame < numFrames; frame++) {
		if ((size_t)offset + size > file.size()) {
			std::printf("frame %u: past end of file\n", frame);
			return 1;
		}
		const uint8_t* fr = file.data() + offset;
		bool wanted = false;
		for (int w : want)
			wanted |= (uint32_t)w == frame;
		if (all || wanted) {
			const uint8_t* comp = fr + 8 + numComp * 4;
			for (uint32_t i = 0; i < numComp; i++) {
				uint32_t csize = be32(fr + 8 + i * 4);
				if (ci[4 + i] == 0) {
					auto t0 = std::chrono::steady_clock::now();
					int err = port_thp_video_decode(comp, csize, Y.data(), U.data(), V.data(),
					                                width, height);
					auto t1 = std::chrono::steady_clock::now();
					totalMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
					decoded++;
					if (err) {
						if (errors++ < 10)
							std::printf("frame %u: decode error %d\n", frame, err);
					}
					if (wanted && !err) {
						std::string path = std::string(argv[2]) + "/frame_" + std::to_string(frame)
						                   + ".ppm";
						FILE* o = std::fopen(path.c_str(), "wb");
						std::fprintf(o, "P6\n%u %u\n255\n", width, height);
						std::vector<uint8_t> row(width * 3);
						for (uint32_t y = 0; y < height; y++) {
							for (uint32_t x = 0; x < width; x++)
								tevYuvToRgb(texel(Y, width, x, y), texel(U, width / 2, x / 2, y / 2),
								            texel(V, width / 2, x / 2, y / 2), &row[x * 3]);
							std::fwrite(row.data(), 1, row.size(), o);
						}
						std::fclose(o);
						std::printf("wrote %s\n", path.c_str());
					}
				}
				comp += csize;
			}
		}
		uint32_t next = be32(fr);
		offset += size;
		size = next;
	}
	std::printf("SUMMARY decoded %d frames, %d errors, %.2f ms/frame\n", decoded, errors,
	            decoded ? totalMs / decoded : 0.0);
	return errors ? 1 : 0;
}
