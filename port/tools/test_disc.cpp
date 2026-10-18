// Host test for the RVZ/WIA/GCZ reader (port/src/port_disc_wia.cpp). Built by test_disc.py.
//
//   test_disc <image.rvz|.wia|.gcz> <game.iso> [full]
//
// Reads every file of the ISO's FST through the reader and compares it byte for byte with
// the ISO; with "full", also compares the whole disc (padding included) in 4 MiB steps,
// and reads a few thousand random small ranges. Exit code 0 = identical.
#include "../src/port_disc.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int main(int argc, char** argv)
{
	if (argc < 3) {
		std::fprintf(stderr, "usage: test_disc <image> <game.iso> [full]\n");
		return 2;
	}
	const bool full = argc > 3 && std::strcmp(argv[3], "full") == 0;
	std::string err;
	std::unique_ptr<PortDiscReader> r(port_disc_open_compressed(argv[1], &err));
	if (!r) {
		std::fprintf(stderr, "open failed: %s\n", err.empty() ? "not RVZ/WIA/GCZ" : err.c_str());
		return 1;
	}
	FILE* iso = std::fopen(argv[2], "rb");
	if (iso == nullptr)
		return 2;
	_fseeki64(iso, 0, SEEK_END);
	const uint64_t isoSize = (uint64_t)_ftelli64(iso);
	auto isoRead = [&](uint64_t off, void* dst, size_t n) {
		_fseeki64(iso, (long long)off, SEEK_SET);
		return std::fread(dst, 1, n, iso) == n;
	};
	std::printf("disc size %llu (iso %llu)\n", (unsigned long long)r->size(), (unsigned long long)isoSize);
	int bad = r->size() == isoSize ? 0 : 1;

	uint8_t h[8];
	isoRead(0x424, h, 8);
	std::vector<uint8_t> fst(be32(h + 4));
	isoRead(be32(h), fst.data(), fst.size());
	const uint32_t count = be32(&fst[8]);
	auto t0 = std::chrono::steady_clock::now();
	uint64_t bytes = 0;
	int files = 0;
	std::vector<uint8_t> a, b;
	for (uint32_t i = 1; i < count; i++) {
		if (fst[i * 12] != 0)
			continue;
		const uint32_t off = be32(&fst[i * 12 + 4]), len = be32(&fst[i * 12 + 8]);
		a.resize(len);
		b.resize(len);
		const int32_t got = r->read(off, a.data(), (int32_t)len);
		isoRead(off, b.data(), len);
		if (got != (int32_t)len || std::memcmp(a.data(), b.data(), len) != 0) {
			std::printf("MISMATCH file %u at 0x%x, %u bytes (read %d)\n", i, off, len, got);
			++bad;
		}
		bytes += len;
		++files;
	}
	double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
	std::printf("files: %d compared, %.1f MB in %.1f s (%.0f MB/s)\n", files, bytes / 1e6, secs, bytes / 1e6 / secs);

	if (full) {
		const size_t step = 4 << 20;
		a.resize(step);
		b.resize(step);
		t0 = std::chrono::steady_clock::now();
		for (uint64_t off = 0; off < isoSize; off += step) {
			const size_t n = (size_t)std::min<uint64_t>(step, isoSize - off);
			const int32_t got = r->read(off, a.data(), (int32_t)n);
			isoRead(off, b.data(), n);
			if (got != (int32_t)n || std::memcmp(a.data(), b.data(), n) != 0) {
				size_t k = 0;
				while (k < n && a[k] == b[k])
					++k;
				std::printf("MISMATCH disc at 0x%llx (+0x%zx), read %d\n", (unsigned long long)off, k, got);
				if (++bad > 20)
					break;
			}
		}
		secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		std::printf("full disc: compared in %.1f s\n", secs);
		std::mt19937_64 rng(12345);
		for (int i = 0; i < 3000; i++) {
			const uint64_t off = rng() % isoSize;
			const size_t n     = (size_t)std::min<uint64_t>(1 + rng() % 70000, isoSize - off);
			r->read(off, a.data(), (int32_t)n);
			isoRead(off, b.data(), n);
			if (std::memcmp(a.data(), b.data(), n) != 0) {
				std::printf("MISMATCH random read at 0x%llx, %zu bytes\n", (unsigned long long)off, n);
				++bad;
			}
		}
		std::printf("random reads: 3000 done\n");
	}
	std::printf("%s\n", bad == 0 ? "IDENTICAL" : "DIFFERENT");
	return bad == 0 ? 0 : 1;
}
