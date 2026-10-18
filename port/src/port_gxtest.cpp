// GX smoke test for the Aurora build: SMS_PORT_GXTEST=1 makes port_main.cpp call
// port_gx_test() instead of the game's main().
//
// It sets up what TApplication::initialize would (JKR root/system heaps, DVD, GX),
// loads a real model from the disc through the real loaders
// (SMSLoadArchive-style /data/mario.szs -> JKRMemArchive -> J3DModelLoaderDataBase),
// and draws it through the real J3D path (J3DModel::calc/entry/viewCalc into two
// J3DDrawBuffers, J3DDrawBuffer::draw) with a fixed camera and one light, turning,
// for a few seconds. Halfway it saves a screenshot of the window
// (SMS_PORT_GXTEST_SHOT, default gxtest.bmp in the working directory).
//
// Optional: SMS_PORT_GXTEST_MODEL=<archive path>:<resource path>
// (default /data/mario.szs:/mario/bmd/ma_mdl1.bmd), SMS_PORT_GXTEST_FRAMES=<n>,
// SMS_PORT_GXTEST_CAM=<eye y>,<eye z>,<target y>, SMS_PORT_GXTEST_DUMP=1 (hex dump
// of the material display lists).
//
// Only compiled into the Aurora build (SMS_PORT_AURORA).
#ifdef SMS_PORT_AURORA

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include <dolphin/dvd.h>
#include <dolphin/gx.h>
#include <dolphin/mtx.h>
#include <dolphin/os.h>
#include <dolphin/vi.h>

#include <JSystem/J3D/J3DGraphAnimator/J3DModel.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DDrawBuffer.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DSys.hpp>
#include <JSystem/J3D/J3DGraphBase/J3DTexture.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoader.hpp>
#include <JSystem/J3D/J3DGraphLoader/J3DModelLoaderFlags.hpp>
#include <JSystem/JKernel/JKRDvdRipper.hpp>
#include <JSystem/JKernel/JKRExpHeap.hpp>
#include <JSystem/JKernel/JKRFileLoader.hpp>
#include <JSystem/JKernel/JKRMemArchive.hpp>
#include <JSystem/JMath.hpp>

namespace {

// Objects created here live in JKR heaps like the game's (this file's own
// operator new is the CRT's; see AURORA.md, category 14).
template <typename T, typename... A> T* jkrNew(A... args)
{
	void* mem = JKRHeap::alloc(sizeof(T), 0x20, nullptr);
	return mem != nullptr ? new (mem) T(args...) : nullptr;
}

// Saves the client area of `hwnd` as a 24-bit BMP. Tries PrintWindow with
// PW_RENDERFULLCONTENT (works for flip-model swap chains through DWM) and falls
// back to copying the screen area under the window.
bool saveWindowBmp(HWND hwnd, const char* path)
{
	RECT rc;
	if (!GetClientRect(hwnd, &rc))
		return false;
	int w = rc.right - rc.left, h = rc.bottom - rc.top;
	if (w <= 0 || h <= 0)
		return false;

	HDC screen = GetDC(nullptr);
	HDC mem = CreateCompatibleDC(screen);
	BITMAPINFO bi = {};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = h; // bottom-up, as BMP files store it
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 24;
	bi.bmiHeader.biCompression = BI_RGB;
	void* bits = nullptr;
	HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	HGDIOBJ old = SelectObject(mem, bmp);

	const int stride = (w * 3 + 3) & ~3;
	const size_t imageSize = (size_t)stride * h;
	const UINT kPwClientOnly = 0x1, kPwRenderFullContent = 0x2;
	bool ok = PrintWindow(hwnd, mem, kPwClientOnly | kPwRenderFullContent) != 0;
	GdiFlush();
	bool blank = true;
	for (size_t i = 0; ok && i < imageSize; i++) {
		if (((const u8*)bits)[i] != 0) {
			blank = false;
			break;
		}
	}
	if (!ok || blank) {
		POINT p = { 0, 0 };
		ClientToScreen(hwnd, &p);
		BitBlt(mem, 0, 0, w, h, screen, p.x, p.y, SRCCOPY);
		GdiFlush();
		std::fprintf(stderr, "[gxtest] PrintWindow %s; used a screen copy\n",
		             ok ? "returned a black image" : "failed");
	}

	bool written = false;
	if (FILE* f = std::fopen(path, "wb")) {
		BITMAPFILEHEADER fh = {};
		fh.bfType = 0x4D42;
		fh.bfOffBits = sizeof(fh) + sizeof(bi.bmiHeader);
		fh.bfSize = (DWORD)(fh.bfOffBits + imageSize);
		std::fwrite(&fh, sizeof(fh), 1, f);
		std::fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
		std::fwrite(bits, 1, imageSize, f);
		std::fclose(f);
		written = true;
	}

	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
	ReleaseDC(nullptr, screen);
	return written;
}

void* loadArchive(const char* path, JKRHeap* heap)
{
	// Like SMSLoadArchive: compressed (.szs) archives are decompressed while read.
	JKRExpandSwitch expand = std::strstr(path, ".szs") != nullptr ? EXPAND_SWITCH_DECOMPRESS
	                                                               : EXPAND_SWITCH_DEFAULT;
	return JKRDvdToMainRam(path, nullptr, expand, 0, heap,
	                       JKRDvdRipper::ALLOC_DIRECTION_FORWARD, 0, nullptr);
}

void setupLights(MtxPtr view)
{
	// One white light up-front-right of the model, in view space like JDrama's lights.
	Vec pos = { 3000.0f, 5000.0f, 8000.0f };
	MTXMultVec(view, &pos, &pos);
	GXLightObj light;
	GXInitLightPos(&light, pos.x, pos.y, pos.z);
	GXInitLightDir(&light, 0.0f, 0.0f, -1.0f);
	GXInitLightColor(&light, GXColor{ 0xB0, 0xB0, 0xB0, 0xFF });
	GXInitLightAttn(&light, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f);
	GXLoadLightObjImm(&light, GX_LIGHT0);
	// Materials whose colour block has no ambient colour use the register as is.
	GXSetChanAmbColor(GX_COLOR0A0, GXColor{ 0x40, 0x40, 0x40, 0xFF });
	GXSetChanAmbColor(GX_COLOR1A1, GXColor{ 0x40, 0x40, 0x40, 0xFF });
}

} // namespace

// Also used by port_main.cpp (SMS_PORT_SHOT).
bool port_save_window_bmp(HWND hwnd, const char* path) { return saveWindowBmp(hwnd, path); }

int port_gx_test()
{
	std::fprintf(stderr, "[gxtest] start\n");

	// TApplication::initialize: heaps, DVD, GX.
	JKRDvdRipper::setSzpBufferSize(0x1000);
	JKRExpHeap::createRoot(1, false);
	JKRExpHeap::create(0x20000, JKRGetRootHeap(), false)->becomeSystemHeap();
	JKRGetRootHeap()->becomeCurrentHeap();
	DVDInit();
	GXInit(JKRAllocFromHeap(nullptr, 0x80000, 0x20), 0x80000);
	JMANewSinTable(0xC); // as TApplication does before any J3D calc

	const char* modelSpec = std::getenv("SMS_PORT_GXTEST_MODEL");
	char arcPath[256] = "/data/mario.szs";
	char resPath[256] = "/mario/bmd/ma_mdl1.bmd";
	if (modelSpec != nullptr) {
		const char* colon = std::strchr(modelSpec, ':');
		if (colon != nullptr && colon - modelSpec < (int)sizeof(arcPath)) {
			std::snprintf(arcPath, sizeof(arcPath), "%.*s", (int)(colon - modelSpec), modelSpec);
			std::snprintf(resPath, sizeof(resPath), "%s", colon + 1);
		}
	}

	void* arcBuf = loadArchive(arcPath, JKRGetRootHeap());
	if (arcBuf == nullptr) {
		std::fprintf(stderr, "[gxtest] could not load %s\n", arcPath);
		return 1;
	}
	JKRMemArchive* arc = jkrNew<JKRMemArchive>(arcBuf, (u32)0, MBF_0);
	(void)arc;
	void* bmd = JKRFileLoader::getGlbResource(resPath);
	if (bmd == nullptr) {
		std::fprintf(stderr, "[gxtest] %s not found in %s\n", resPath, arcPath);
		return 1;
	}

	J3DModelData* data = J3DModelLoaderDataBase::load(
	    bmd, J3DMLF_MaterialPEFull | (16 << J3DMLF_TevStageNumShift));
	if (data == nullptr) {
		std::fprintf(stderr, "[gxtest] J3DModelLoaderDataBase::load failed\n");
		return 1;
	}
	J3DModel* model = jkrNew<J3DModel>(data, (u32)0, (u32)1);
	J3DDrawBuffer* opa = jkrNew<J3DDrawBuffer>((u32)0x100);
	J3DDrawBuffer* xlu = jkrNew<J3DDrawBuffer>((u32)0x100);
	std::fprintf(stderr,
	             "[gxtest] %s: %u joints, %u materials, %u shapes, %u vertices, %u textures\n",
	             resPath, (u32)data->getJointNum(), (u32)data->getMaterialNum(),
	             (u32)data->getShapeNum(), data->getVertexData().getVtxNum(),
	             data->getTexture() != nullptr ? (u32)data->getTexture()->getNum() : 0u);

	// Camera: 4:3 perspective, looking at the model's middle from the front.
	Mtx44 proj;
	MTXPerspective(proj, 45.0f, 4.0f / 3.0f, 10.0f, 100000.0f);
	Vec eye = { 0.0f, 90.0f, 320.0f };
	Vec up = { 0.0f, 1.0f, 0.0f };
	Vec at = { 0.0f, 80.0f, 0.0f };
	if (const char* cam = std::getenv("SMS_PORT_GXTEST_CAM")) // "eyeY,eyeZ,atY"
		std::sscanf(cam, "%f,%f,%f", &eye.y, &eye.z, &at.y);
	Mtx view;
	MTXLookAt(view, &eye, &up, &at);

	const char* framesEnv = std::getenv("SMS_PORT_GXTEST_FRAMES");
	int frames = framesEnv != nullptr ? std::atoi(framesEnv) : 300;
	const char* shot = std::getenv("SMS_PORT_GXTEST_SHOT");
	if (shot == nullptr)
		shot = "gxtest.bmp";

	for (int frame = 0; frame < frames; frame++) {
		GXSetCopyClear(GXColor{ 0x30, 0x50, 0x80, 0xFF }, 0xFFFFFF);
		GXSetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
		GXSetScissor(0, 0, 640, 480);
		GXSetProjection(proj, GX_PERSPECTIVE);

		Mtx base;
		MTXRotRad(base, 'y', frame * 0.02f);
		model->setBaseTRMtx(base);

		j3dSys.setViewMtx(view);
		j3dSys.drawInit();
		setupLights(view);
		opa->frameInit();
		xlu->frameInit();
		j3dSys.setDrawBuffer(opa, 0);
		j3dSys.setDrawBuffer(xlu, 1);

		model->calc();
		model->entry();
		model->viewCalc();

		j3dSys.setUnk4C(3);
		opa->draw();
		j3dSys.setUnk4C(4);
		xlu->draw();

		VIWaitForRetrace(); // ends and presents this frame, begins the next

		if (frame == 0 && std::getenv("SMS_PORT_GXTEST_DUMP") != nullptr) {
			// SMS_PORT_GXTEST_DUMP: hex dump of every material display list.
			for (u16 m = 0; m < data->getMaterialNum(); m++) {
				J3DDisplayListObj* dl = model->getMatPacket(m)->getDisplayListObj();
				const u8* p = dl->getDisplayList(0);
				std::fprintf(stderr, "[gxtest] material %u DL (%u bytes, capacity %u):", m,
				             dl->getDisplayListSize(), dl->mCapacity);
				for (u32 i = 0; i < dl->getDisplayListSize(); i++)
					std::fprintf(stderr, "%s%02x", i % 32 == 0 ? "\n  " : " ", p[i]);
				std::fprintf(stderr, "\n");
			}
		}

		if (frame == frames / 2) {
			HWND hwnd = FindWindowA(nullptr, "Super Mario Sunshine");
			bool saved = hwnd != nullptr && saveWindowBmp(hwnd, shot);
			std::fprintf(stderr, "[gxtest] frame %d: screenshot %s %s\n", frame, shot,
			             saved ? "saved" : "FAILED");
		}
	}

	std::fprintf(stderr, "[gxtest] done (%d frames)\n", frames);
	return 0;
}

#endif // SMS_PORT_AURORA
