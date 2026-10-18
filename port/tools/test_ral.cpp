// Host test for the .ral rail graph swap pass (port_swap_ral) through the real
// TGraphGroup / TGraphWeb constructors (src/Enemy/graph.cpp). Run via test_ral.py.
//
//   test_ral.exe <files.txt>
//
// For each file: builds a TGraphGroup from the raw big-endian bytes (twice, to
// check idempotency on a shared resource), then prints per graph its name,
// node count and every rail node as seen after TGraphWeb::translateNodes.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define private public
#define protected public
#include <Enemy/Graph.hpp>
#undef private
#undef protected

static int gFailures = 0;

// host math the rail code calls (the dolphin mtx library is not part of this test)
extern "C" float PSVECMag(Vec* v) { return sqrtf(v->x * v->x + v->y * v->y + v->z * v->z); }

static std::vector<std::string> dump(TGraphGroup* g)
{
	std::vector<std::string> out;
	char buf[512];
	for (int i = 0; i < g->unk4; i++) {
		TGraphWeb* w = g->unk8[i];
		snprintf(buf, sizeof buf, "graph %s %d", w->unkC, w->unk8);
		out.push_back(buf);
		for (int n = 0; n < w->unk8; n++) {
			const TRailNode* r = w->getGraphNode(n).getRailNode();
			std::string s = "node";
			snprintf(buf, sizeof buf, " %d %d %d %d %u %u %u %u %u", r->mPosition.x, r->mPosition.y,
			         r->mPosition.z, r->mConnectionNum, r->mFlags, r->mPitch, r->mYaw, r->mRoll,
			         r->mSpeed);
			s += buf;
			for (int c = 0; c < 8; c++) {
				snprintf(buf, sizeof buf, " %u", r->mConnections[c]);
				s += buf;
			}
			for (int c = 0; c < 8; c++) {
				snprintf(buf, sizeof buf, " %.9g", r->mPeriods[c]);
				s += buf;
			}
			out.push_back(s);
		}
	}
	return out;
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	std::ifstream list(argv[1]);
	std::string path;
	int files = 0;
	while (std::getline(list, path)) {
		if (path.empty())
			continue;
		std::ifstream f(path, std::ios::binary);
		std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
		                               std::istreambuf_iterator<char>());
		std::vector<std::string> a, b;
		{
			TGraphGroup* g = new TGraphGroup(buf.data());
			a = dump(g);
		}
		std::vector<unsigned char> afterFirst = buf;
		{
			TGraphGroup* g = new TGraphGroup(buf.data()); // same buffer again: no double swap
			b = dump(g);
		}
		if (a != b || afterFirst != buf) {
			printf("FAIL second TGraphGroup on the same buffer differs: %s\n", path.c_str());
			gFailures++;
		}
		printf("== %s\n", path.c_str());
		for (auto& s : a)
			printf("%s\n", s.c_str());
		files++;
	}
	printf("SUMMARY files %d failures %d\n", files, gFailures);
	return gFailures ? 1 : 0;
}

