// Host test for the .ymp pollution map swap pass and the 64-bit layer-info loader
// (port_swap_ymp, TPollutionManager::setDataAddress). Run through test_ymp.py.
//
//   test_ymp.exe <files.txt>
//
// For each file it mirrors TPollutionManager::initPollutionInfo's data steps
// (JKRGetResource -> swap -> layer count -> setDataAddress, the part before it
// needs the director and the joint-model loader) with the real
// TPollutionManager::setDataAddress, twice on the same buffer, and prints the
// native TPollutionLayerInfo array: fields plus the height map offset and a
// checksum of the first and last bytes of the map.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define private public
#define protected public
#include <Map/PollutionManager.hpp>
#undef private
#undef protected

static int gFailures = 0;

static std::vector<std::string> run(std::vector<unsigned char>& buf)
{
	TPollutionManager* mgr = (TPollutionManager*)calloc(1, sizeof(TPollutionManager));
	TPollutionManager::TPollutionInfo* info = (TPollutionManager::TPollutionInfo*)buf.data();
	port_swap_ymp(info);
	mgr->mJointModelNum = info->mLayerCount;
	mgr->setDataAddress(info);
	std::vector<std::string> out;
	char s[512];
	snprintf(s, sizeof s, "layers %d", mgr->mJointModelNum);
	out.push_back(s);
	for (int i = 0; i < mgr->mJointModelNum; i++) {
		const TPollutionLayerInfo& l = mgr->mLayerInfos[i];
		size_t size = (size_t)1 << (l.mLog2Width + l.mLog2Height);
		long off    = (long)(l.mHeightMap - buf.data());
		unsigned first = l.mHeightMap[0], last = l.mHeightMap[size - 1];
		snprintf(s, sizeof s, "layer %u %u %u %.9g %.9g %.9g %.9g %.9g %.9g %u %u %u %ld %zu %u %u",
		         l.mPollutionType, l.mFlags, l.mPlaneType, l.mVerticalOffset, l.mTexelSize, l.mLeft,
		         l.mTop, l.mRight, l.mBottom, l.mLog2Width, l.mLog2Height, l.unk24, off, size, first,
		         last);
		out.push_back(s);
		if (off < 0 || (size_t)off + size > buf.size()) {
			printf("FAIL height map outside the file\n");
			gFailures++;
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
		std::vector<std::string> a = run(buf);
		std::vector<std::string> b = run(buf); // same buffer again: no double swap
		if (a != b) {
			printf("FAIL second load differs: %s\n", path.c_str());
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
