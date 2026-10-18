// SDK pieces the game uses that Aurora does not implement (or implements with a
// different signature). Declarations: port/include_aurora/port_aurora.h.
// Only compiled into the Aurora build (SMS_PORT_AURORA); empty for build_port.py.
#ifdef SMS_PORT_AURORA

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include <aurora/gfx.hpp>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXCommandList.h>
#include <dolphin/gd.h>

#include "port_gx.h"

extern "C" int port_efb_presentation_only(void); // port_efb.cpp (interpolated frames)
extern "C" int port_efb_host_begin(void);         // port_efb.cpp (EFB reads: the audio thread may run)
extern "C" void port_preemptible_end(int began);   // sdk_os_thread.cpp

namespace {
void logOnce(bool& seen, const char* what)
{
	if (!seen) {
		seen = true;
		std::fprintf(stderr, "[port] unimplemented on Aurora: %s\n", what);
	}
}
} // namespace

// ---- Vertex / matrix / light arrays ------------------------------------------
// Aurora copies `size` bytes of an array to a GPU buffer and reads it in the byte
// order given by `le`. Every array the game draws from is native order on PC: J3D
// VTX1 arrays after the J3D swap pass (port/ENDIAN.md) and the arrays the game builds
// on the CPU (matrices, lights, deformed vertices). The call sites that know the size
// use the PORTGX*/PORTGD* helpers (port/include/port_gx.h).
//
// The SDK 3-argument GXSetArray has no size: fallback for call sites not converted
// yet. It uses the readable span from `data`, at most kFallbackArraySize, and logs the
// attribute once so the call site can be found.
namespace {
const size_t kFallbackArraySize = 0x10000;

u32 readableSpan(const void* data, size_t limit)
{
	if (data == nullptr)
		return 0;
	MEMORY_BASIC_INFORMATION mbi;
	size_t size = 0;
	if (VirtualQuery(data, &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT)
		size = (size_t)((const char*)mbi.BaseAddress + mbi.RegionSize - (const char*)data);
	return (u32)(size > limit ? limit : size);
}

u32 cpArrayIndex(GXAttr attr)
{
	return attr == GX_VA_NBT ? 1u : (u32)(attr - GX_VA_POS);
}
} // namespace

void GXSetArray(GXAttr attr, const void* data, u8 stride)
{
	static bool seen[32];
	u32 idx = cpArrayIndex(attr) & 31;
	if (!seen[idx]) {
		seen[idx] = true;
		std::fprintf(stderr, "[port] GXSetArray(attr %d) without a size: using the readable span "
		             "(max 0x%zx)\n", (int)attr, kFallbackArraySize);
	}
	GXSetArray(attr, data, readableSpan(data, kFallbackArraySize), stride, true);
}

u32 PORTGXFallbackArraySize(const void* data)
{
	return readableSpan(data, kFallbackArraySize);
}

void PORTGXSetArray(GXAttr attr, const void* data, u32 size, u8 stride)
{
	GXSetArray(attr, data, size, stride, true);
}

void PORTGXSetArrayBE(GXAttr attr, const void* data, u32 size, u8 stride)
{
	GXSetArray(attr, data, size, stride, false);
}

// Like a CP array-base register write (GX_CMD_LOAD_CP_REG 0xA0 + attr): base only,
// the stride register keeps its value. Aurora's command carries a 64-bit pointer and
// a size; it goes through GXCmd* so it lands in a display list when one is open.
void PORTGXSetArrayBase(GXAttr attr, const void* data, u32 size)
{
	GXCmd1u8(GX_AURORA);
	GXCmd1u16((u16)(GX_AURORA_LOAD_ARRAYBASE | cpArrayIndex(attr)));
	GXCmd1u64((u64)(uintptr_t)data);
	GXCmd1u32(size);
	GXCmd1u8(1); // native order
}

void PORTGDSetArray(GXAttr attr, void* data, u32 size, u8 stride)
{
	GDSetArraySized(attr, data, size, stride, true);
}

// GDSetArrayRaw(attr, 0, stride) on the GameCube: no array, only the stride.
void PORTGDSetArrayNone(GXAttr attr, u8 stride)
{
	GDWriteCPCmd((u8)(0xB0 + cpArrayIndex(attr)), stride); // CP_REG_ARRAYSTRIDE_ID
}

// ---- Textures in J3D material display lists --------------------------------------
// J3D's loadTexNo writes BP image/TLUT registers with physical addresses. Aurora keeps
// the mode registers from those writes but takes the image itself only from its
// LOAD_TEXOBJ command, so the port appends one after the BP writes (texture object id
// 0: no object cache, Aurora caches by content).
void PORTGDLoadTexImage(GXTexMapID id, const void* image, u16 width, u16 height,
                        GXTexFmt format, GXBool mipmap, u32 tlut)
{
	GDWriteAuroraCmd(GX_AURORA_LOAD_TEXOBJ);
	GDWrite_u8((u8)id);
	GDWrite_u64((u64)(uintptr_t)image);
	GDWrite_u32(width);
	GDWrite_u32(height);
	GDWrite_u32((u32)format);
	GDWrite_u32(tlut);
	GDWrite_u8(mipmap ? 1 : 0);
	GDWrite_u32(0); // texObjId
	GDWrite_u32(0); // texDataVersion
}

void PORTGDLoadTlut(u32 tlut, const void* data, GXTlutFmt format, u16 entries)
{
	GDWriteAuroraCmd(GX_AURORA_LOAD_TLUT);
	GDWrite_u8((u8)tlut);
	GDWrite_u64((u64)(uintptr_t)data);
	GDWrite_u32((u32)format);
	GDWrite_u16(entries);
	GDWrite_u32(0); // tlutObjId
	GDWrite_u32(0); // tlutDataVersion
}

// ---- EFB readback (GXPeekARGB, GPU screenshots) ------------------------------------
// Aurora records GX work on its FIFO thread and renders it on the render worker, so the
// EFB is never in CPU memory. A readback snapshots the EFB at the current point of the
// command stream (aurora::gfx::resolve_pass: drains the FIFO, copies the colour target
// into a pooled texture and continues on a new pass that loads the EFB), then an encoder
// task copies the wanted rectangle into a mappable buffer; the result arrives after the
// frame is submitted (1-3 frames later).
//
// GXPeekARGB: the GameCube reads the EFB synchronously. The game peeks from its draw
// sync callback (TMario::drawSyncCallback, the occlusion probe: alpha 0x10 under Mario
// means visible), i.e. at a fixed point of every frame, so each peek requests a fresh
// one-pixel readback at that point and returns the newest completed value for that
// pixel (or for the most recent pixel when the position moved): the answer lags a couple
// of frames behind, like Aurora's own GXPeekZ.
// w9 (input replay, port_replay.cpp): while input is recorded or replayed, peek N returns the
// result of peek N - kDetPeekLatency (the game peeks once per frame), waiting for the GPU if
// needed, instead of the newest finished one. Mario's occlusion probe feeds the camera, so a
// GPU-timing-dependent answer made replays drift.
extern "C" bool port_replay_deterministic(void);
namespace aurora::webgpu {
extern wgpu::Instance g_instance; // lib/webgpu/gpu.cpp
}

namespace {
GXAlphaReadMode sAlphaReadMode = GX_READ_FF; // GXInit default; the game sets GX_READ_NONE

struct DetPeek { // w9
	std::atomic<bool> done{ false };
	u32 value = 0;
};
const size_t kDetPeekLatency = 3;
std::deque<DetPeek*> sDetPeeks;
u32 sDetLastValue; // the last value handed out (interpolated frames repeat it)

struct EfbReadRequest {
	wgpu::Texture texture;    // pass snapshot (valid for the frame it was taken in)
	wgpu::TextureFormat format;
	wgpu::Buffer buffer;      // MapRead | CopyDst
	u32 x, y, w, h;           // rectangle in render-target pixels
	u32 bytesPerRow;
	bool screenshot;
	bool wholeEfb;            // screenshot of the whole EFB, not only the presented part
	u16 peekX, peekY;         // logical EFB coordinates (peeks)
	DetPeek* det;             // w9: deterministic peek result slot (or null)
	char path[96];            // screenshot file
};

aurora::gfx::EncoderTaskId sEfbReadTask = aurora::gfx::InvalidEncoderTask;

std::mutex sPeekMutex;
u32 sPeekLastValue;          // newest completed peek, any position
u16 sPeekLastX = 0xFFFF, sPeekLastY = 0xFFFF;
std::map<u32, u32> sPeekValues; // (x << 16 | y) -> newest completed value
int sPeeksInFlight;

// Logical EFB size, as configured by the game (VIConfigure, renamed to port_VIConfigure
// by build_aurora.py). Aurora maps it to the render target with its viewport policy
// per axis, without an offset (gx::map_logical_viewport / map_logical_scissor: the EFB
// render target is the logical frame buffer scaled; aspect fitting happens at present).
// With port/patches/aurora_efb_size.patch the render target is the whole EFB (at least
// 640x528, vi::efb_size) and the configured frame buffer (sDisplayW x sDisplayH) is its
// top-left part, the part that is presented (and captured by screenshots).
u32 sLogicalW = 640, sLogicalH = 528;
u32 sDisplayW = 640, sDisplayH = 480;

void mapLogical(u32 lx, u32 ly, u32 targetW, u32 targetH, u32& tx, u32& ty)
{
	float fx = ((float)lx + 0.5f) * (float)targetW / (float)sLogicalW;
	float fy = ((float)ly + 0.5f) * (float)targetH / (float)sLogicalH;
	tx = fx < 0 ? 0 : (u32)fx;
	ty = fy < 0 ? 0 : (u32)fy;
	if (tx >= targetW)
		tx = targetW - 1;
	if (ty >= targetH)
		ty = targetH - 1;
}

// Converts one texel of the snapshot format to GX's 0xAARRGGBB.
u32 texelToARGB(const u8* p, wgpu::TextureFormat format)
{
	switch (format) {
	case wgpu::TextureFormat::BGRA8Unorm:
	case wgpu::TextureFormat::BGRA8UnormSrgb:
		return ((u32)p[3] << 24) | ((u32)p[2] << 16) | ((u32)p[1] << 8) | p[0];
	case wgpu::TextureFormat::RGBA8Unorm:
	case wgpu::TextureFormat::RGBA8UnormSrgb:
		return ((u32)p[3] << 24) | ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
	default:
		return 0;
	}
}

void writeBmp(const EfbReadRequest& req, const u8* data, const char* what = "EFB readback")
{
	FILE* f = std::fopen(req.path, "wb");
	if (f == nullptr) {
		std::fprintf(stderr, "[port] screenshot %s: cannot open\n", req.path);
		return;
	}
	const u32 rowBytes = req.w * 3;
	const u32 pad      = (4 - rowBytes % 4) % 4;
	const u32 imgSize  = (rowBytes + pad) * req.h;
	u8 hdr[54] = { 'B', 'M' };
	auto put32 = [&](int off, u32 v) {
		hdr[off] = (u8)v, hdr[off + 1] = (u8)(v >> 8), hdr[off + 2] = (u8)(v >> 16), hdr[off + 3] = (u8)(v >> 24);
	};
	put32(2, 54 + imgSize);
	put32(10, 54);
	put32(14, 40);
	put32(18, req.w);
	put32(22, req.h); // bottom-up
	hdr[26] = 1;
	hdr[28] = 24;
	put32(34, imgSize);
	std::fwrite(hdr, 1, sizeof(hdr), f);
	u8* row = (u8*)std::malloc(rowBytes + pad + 4);
	for (u32 y = 0; y < req.h; ++y) {
		const u8* src = data + (size_t)(req.h - 1 - y) * req.bytesPerRow;
		for (u32 x = 0; x < req.w; ++x) {
			const u32 c    = texelToARGB(src + x * 4, req.format);
			row[x * 3 + 0] = (u8)c;
			row[x * 3 + 1] = (u8)(c >> 8);
			row[x * 3 + 2] = (u8)(c >> 16);
		}
		for (u32 i = 0; i < pad; ++i)
			row[rowBytes + i] = 0;
		std::fwrite(row, 1, rowBytes + pad, f);
	}
	std::free(row);
	std::fclose(f);
	std::fprintf(stderr, "[port] screenshot %s: saved (%ux%u, %s)\n", req.path, req.w, req.h, what);
}

void efbReadComplete(EfbReadRequest* req, wgpu::MapAsyncStatus status)
{
	if (status == wgpu::MapAsyncStatus::Success) {
		const u8* data = (const u8*)req->buffer.GetConstMappedRange(0, (size_t)req->bytesPerRow * req->h);
		if (data != nullptr) {
			if (req->screenshot) {
				writeBmp(*req, data);
			} else {
				const u32 v = texelToARGB(data, req->format);
				if (req->det != nullptr) // w9
					req->det->value = v;
				std::lock_guard<std::mutex> lock(sPeekMutex);
				sPeekLastValue = v;
				sPeekLastX     = req->peekX;
				sPeekLastY     = req->peekY;
				if (sPeekValues.size() > 4096)
					sPeekValues.clear();
				sPeekValues[((u32)req->peekX << 16) | req->peekY] = v;
			}
		}
		req->buffer.Unmap();
	} else if (req->screenshot) {
		std::fprintf(stderr, "[port] screenshot %s: readback failed (%d)\n", req->path, (int)status);
	}
	if (!req->screenshot) {
		std::lock_guard<std::mutex> lock(sPeekMutex);
		--sPeeksInFlight;
	}
	if (req->det != nullptr)
		req->det->done.store(true); // w9
	delete req;
}

// Render worker, between two passes: copy the rectangle into the readback buffer.
void efbReadEncode(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd,
                   const void* payload, size_t, void*)
{
	EfbReadRequest* req = *(EfbReadRequest* const*)payload;
	const wgpu::BufferDescriptor bd { .label = "port EFB readback",
		                              .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
		                              .size  = (u64)req->bytesPerRow * req->h };
	req->buffer = ctx.device.CreateBuffer(&bd);
	const wgpu::TexelCopyTextureInfo src { .texture = req->texture, .origin = { req->x, req->y, 0 } };
	const wgpu::TexelCopyBufferInfo dst { .layout = { .offset = 0, .bytesPerRow = req->bytesPerRow, .rowsPerImage = req->h },
		                                  .buffer = req->buffer };
	const wgpu::Extent3D size { req->w, req->h, 1 };
	cmd.CopyTextureToBuffer(&src, &dst, &size);
}

// Render worker, after the frame's command buffer was submitted: map the buffer.
void efbReadSubmitted(const aurora::gfx::EncoderTaskCompletionContext&, const void* payload, size_t, void*)
{
	EfbReadRequest* req = *(EfbReadRequest* const*)payload;
	req->texture        = nullptr; // the pooled snapshot is recycled with its frame slot
	if (!req->buffer) {
		efbReadComplete(req, wgpu::MapAsyncStatus::Error);
		return;
	}
	req->buffer.MapAsync(wgpu::MapMode::Read, 0, (size_t)req->bytesPerRow * req->h,
	                     wgpu::CallbackMode::AllowSpontaneous,
	                     [req](wgpu::MapAsyncStatus status, wgpu::StringView) { efbReadComplete(req, status); });
}

// Snapshots the EFB here and queues a readback of a rectangle of it (logical
// coordinates for peeks, the whole target for screenshots).
bool efbRequestRead(EfbReadRequest* req, u16 lx, u16 ly)
{
	if (sEfbReadTask == aurora::gfx::InvalidEncoderTask) {
		const aurora::gfx::EncoderTaskDescriptor desc { .label       = "port EFB readback",
			                                            .callback    = efbReadEncode,
			                                            .userdata    = nullptr,
			                                            .afterSubmit = efbReadSubmitted };
		sEfbReadTask = aurora::gfx::register_encoder_task_type(desc);
	}
	// SMS_PORT_NULLGFX: Dawn's Null device cannot allocate readback buffers (see port_efb.cpp).
	static const bool sNullGfx = std::getenv("SMS_PORT_NULLGFX") != nullptr;
	if (sNullGfx || aurora::gfx::is_offscreen())
		return false;
	// Interpolated (presentation-only) frames: peeks return the game frames' values
	// (port_efb.cpp, port_efb_set_presentation_only). Screenshots are still taken.
	if (!req->screenshot && port_efb_presentation_only() != 0)
		return false;
	aurora::gfx::ResolvedTargets targets;
	// w8 (E24): with MSAA a peek reads sample 0 of the pixel, as the GameCube's one sample per
	// pixel (Mario's occlusion probe tests alpha == 0x10 exactly; a resolved edge pixel would
	// average it away). Screenshots keep the resolve (what the window shows).
	const aurora::gfx::ResolveDesc desc { .color = true, .colorSample0 = !req->screenshot };
	if (!aurora::gfx::resolve_pass(desc, targets) || !targets.colorTexture)
		return false;
	req->texture = targets.colorTexture;
	req->format  = targets.colorFormat;
	if (req->screenshot) {
		// The presented part of the EFB (or all of it for debug captures).
		req->x = req->y = 0;
		req->w          = targets.width;
		req->h          = targets.height;
		if (!req->wholeEfb) {
			// Rounded down: Aurora sizes the EFB target rounded up (aurora_efb_size.patch), so
			// this is the presented size (e.g. 1698 rows * 448 / 528 = 1440.6 -> 1440).
			req->w = (u32)(((u64)targets.width * sDisplayW) / sLogicalW);
			req->h = (u32)(((u64)targets.height * sDisplayH) / sLogicalH);
			req->w = req->w < 1 ? 1 : req->w > targets.width ? targets.width : req->w;
			req->h = req->h < 1 ? 1 : req->h > targets.height ? targets.height : req->h;
		}
	} else {
		mapLogical(lx, ly, targets.width, targets.height, req->x, req->y);
		req->w     = 1;
		req->h     = 1;
		req->peekX = lx;
		req->peekY = ly;
	}
	req->bytesPerRow = (req->w * 4 + 255) & ~255u;
	return aurora::gfx::push_encoder_task(sEfbReadTask, &req, sizeof(req));
}

// w9: see DetPeek.
u32 detPeekARGB(u16 x, u16 y)
{
	if (port_efb_presentation_only() != 0)
		return sDetLastValue; // interpolated frame: no peek of its own
	DetPeek* slot       = new DetPeek;
	EfbReadRequest* req = new EfbReadRequest {};
	req->det            = slot;
	{
		std::lock_guard<std::mutex> lock(sPeekMutex);
		++sPeeksInFlight;
	}
	if (!efbRequestRead(req, x, y)) {
		delete req;
		std::lock_guard<std::mutex> lock(sPeekMutex);
		--sPeeksInFlight;
		slot->done.store(true); // no snapshot possible: answers 0, the same in every run
	}
	sDetPeeks.push_back(slot);
	u32 v = 0;
	if (sDetPeeks.size() > kDetPeekLatency) {
		DetPeek* d = sDetPeeks.front();
		sDetPeeks.pop_front();
		for (int i = 0; i < 2000 && !d->done.load(); i++) {
			aurora::webgpu::g_instance.ProcessEvents();
			if (!d->done.load())
				std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
		if (d->done.load()) {
			v = d->value;
			delete d;
		} else {
			std::fprintf(stderr, "[port] GXPeekARGB result late: replays may drift (w9)\n");
		}
	}
	sDetLastValue = v;
	return v;
}

u32 efbPeekARGB(u16 x, u16 y)
{
	if (port_replay_deterministic()) { // w9
		const u32 v = detPeekARGB(x, y);
		return sAlphaReadMode == GX_READ_00 ? (v & 0x00FFFFFF) : sAlphaReadMode == GX_READ_FF ? (v | 0xFF000000) : v;
	}
	// Queue a fresh read at this point of the frame (at most a few in flight).
	bool request;
	{
		std::lock_guard<std::mutex> lock(sPeekMutex);
		request = sPeeksInFlight < 8;
		if (request)
			++sPeeksInFlight;
	}
	if (request) {
		EfbReadRequest* req = new EfbReadRequest {};
		if (!efbRequestRead(req, x, y)) {
			delete req;
			std::lock_guard<std::mutex> lock(sPeekMutex);
			--sPeeksInFlight;
		}
	}
	u32 v;
	{
		std::lock_guard<std::mutex> lock(sPeekMutex);
		auto it = sPeekValues.find(((u32)x << 16) | y);
		v       = it != sPeekValues.end() ? it->second : sPeekLastValue;
	}
	switch (sAlphaReadMode) {
	case GX_READ_00:
		return v & 0x00FFFFFF;
	case GX_READ_FF:
		return v | 0xFF000000;
	default:
		return v;
	}
}
} // namespace

// GPU screenshot of the EFB as it is now (port_main.cpp calls this right before
// ending a frame listed in SMS_PORT_SHOT). The BMP is written asynchronously.
bool port_efb_screenshot_ex(const char* path, bool whole_efb);
bool port_efb_screenshot(const char* path) { return port_efb_screenshot_ex(path, false); }

// What the window shows (SMS_PORT_SHOT_PRESENT): the swapchain texture after Aurora's present
// pass (viewport/letterbox, resample, post-processing, ImGui), copied before it is presented
// (aurora_present_capture.patch). Works with the window covered or off-screen, like the EFB
// shots; nothing with --nullgfx. Armed here, written when the copy completes.
namespace {
void presentShotDone(void* userdata, const uint8_t* pixels, uint32_t width, uint32_t height,
                     uint32_t bytesPerRow, wgpu::TextureFormat format)
{
	EfbReadRequest* req = (EfbReadRequest*)userdata;
	req->w              = width;
	req->h              = height;
	req->bytesPerRow    = bytesPerRow;
	req->format         = format;
	writeBmp(*req, pixels, "presented swapchain image");
	delete req;
}
} // namespace

bool port_present_screenshot(const char* path)
{
	static const bool sNullGfx = std::getenv("SMS_PORT_NULLGFX") != nullptr;
	if (sNullGfx)
		return false;
	EfbReadRequest* req = new EfbReadRequest {};
	req->screenshot     = true;
	std::snprintf(req->path, sizeof(req->path), "%s", path);
	if (!aurora::gfx::request_present_capture(presentShotDone, req)) {
		delete req;
		return false;
	}
	return true; // (if that frame is not presented the request is dropped with it)
}

// w12: bug reports (port_bugreport.cpp): the presented picture, once per capture period,
// through the same present capture; the readback completes asynchronously.
extern "C" void port_bugreport_push_frame(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t stride, bool bgra,
                                          long long presentedQpc);
extern "C" void port_bugreport_capture_dropped(void);
namespace {
void bugreportCaptureDone(void*, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t bytesPerRow,
                          wgpu::TextureFormat format)
{
	const bool bgra = format == wgpu::TextureFormat::BGRA8Unorm || format == wgpu::TextureFormat::BGRA8UnormSrgb;
	port_bugreport_push_frame(pixels, width, height, bytesPerRow, bgra, 0);
}
} // namespace
extern "C" void port_bugreport_arm_capture(void)
{
	if (!aurora::gfx::request_present_capture(bugreportCaptureDone, nullptr))
		port_bugreport_capture_dropped();
}

// whole_efb: capture all of the EFB render target (port_efb.cpp debug captures), not only
// the presented part.
bool port_efb_screenshot_ex(const char* path, bool whole_efb)
{
	EfbReadRequest* req = new EfbReadRequest {};
	req->screenshot     = true;
	req->wholeEfb       = whole_efb;
	std::snprintf(req->path, sizeof(req->path), "%s", path);
	if (!efbRequestRead(req, 0, 0)) {
		delete req;
		return false;
	}
	return true;
}

extern "C" {

void port_vi_note_tv_mode(unsigned long viTVmode); // port_behaviour.cpp (w13)

// Logical EFB size for the readbacks above; forwards to Aurora's VIConfigure.
void port_VIConfigure(const GXRenderModeObj* rm)
{
	if (rm != nullptr && rm->fbWidth != 0 && rm->efbHeight != 0) {
		sDisplayW = rm->fbWidth;
		sDisplayH = rm->efbHeight;
		// the whole EFB, as Aurora's vi::efb_size (aurora_efb_size.patch)
		sLogicalW = rm->fbWidth > 640 ? rm->fbWidth : 640;
		sLogicalH = rm->efbHeight > 528 ? rm->efbHeight : 528;
	}
	if (rm != nullptr)
		port_vi_note_tv_mode((unsigned long)rm->viTVmode); // w13: PAL 50/60 Hz (port_behaviour.cpp)
	VIConfigure(rm);
}

// ---- OSFatal --------------------------------------------------------------------
// Aurora leaves the OSReport family to the application (its OSReport.cpp is #if 0).
// OSReport/OSVReport/OSPanic: port/src/sdk_os_misc.cpp (shared with build_port.py).
void OSFatal(GXColor fg, GXColor bg, const char* msg)
{
	(void)fg;
	(void)bg;
	std::fprintf(stderr, "[port] OSFatal: %s\n", msg);
	std::fflush(stderr);
	std::abort();
}

// (GXTex*Ids register tables: defined by JSystem's J3DTevs.cpp.)

// Aurora renders progressive frames only.
void GXSetDispCopyFrame2Field(GXCopyMode mode) { (void)mode; }

// ---- Draw sync ----------------------------------------------------------------
// Aurora records GX commands and executes them at frame end, so a token is
// "reached" as soon as it is issued.
static u16 sDrawSyncToken;
static GXDrawSyncCallback sDrawSyncCB;

void GXSetDrawSync(u16 token)
{
	GXFlush();
	sDrawSyncToken = token;
	// w9: not in an interpolated frame of the frame-rate option (port_interp.cpp): the
	// game's callbacks read the EFB for game logic (Mario's occlusion probe, the sun,
	// pollution counters) and keep state, once per game frame.
	if (sDrawSyncCB != nullptr && port_efb_presentation_only() == 0)
		sDrawSyncCB(token);
}

u16 GXReadDrawSync(void) { return sDrawSyncToken; }

GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb)
{
	GXDrawSyncCallback old = sDrawSyncCB;
	sDrawSyncCB = cb;
	return old;
}

void GXWaitDrawDone(void) { GXDrawDone(); }

// ---- Breakpoints: no equivalent ------------------------------------------------
// (Pixel metrics GXClearPixMetric/GXReadPixMetric, GXPeekZ and the GXCopyTex
// wrappers: port_efb.cpp, w8.)
void GXEnableBreakPt(void* break_pt) { (void)break_pt; }
void GXDisableBreakPt(void) { }

// ---- Transform ------------------------------------------------------------------
// Aurora has no GXLoadNrmMtxIndx3x3 but its command processor executes indexed XF
// loads (GX_LOAD_INDX_B from GX_NRM_MTX_ARRAY into the normal matrix memory at
// 0x400 + id * 3, 9 words), so write the command like the SDK does.
void GXLoadNrmMtxIndx3x3(u16 mtx_indx, u32 id)
{
	GXCmd1u8(0x28); // GX_LOAD_INDX_B
	GXCmd1u16(mtx_indx);
	GXCmd1u16((u16)((8u << 12) | (0x400 + id * 3)));
}

// ---- Declared by Aurora's headers but not implemented ----------------------------
// Display copy scaling: Aurora presents the EFB directly, so the XFB is the EFB size
// scaled by yScale (exact for the game's 1:1 NTSC modes; the SDK's register-level
// rounding is not reproduced).
f32 GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight)
{
	return efbHeight != 0 ? (f32)xfbHeight / (f32)efbHeight : 1.0f;
}
u16 GXGetNumXfbLines(u16 efbHeight, f32 yScale) { return (u16)((f32)efbHeight * yScale + 0.5f); }
void GXSetCopyClamp(GXFBClamp clamp) { (void)clamp; }
void GXSetMisc(GXMiscToken token, u32 val)
{
	(void)token;
	(void)val;
}
// Aurora manages its own texture cache; TMEM regions have no meaning.
void GXInitTexCacheRegion(GXTexRegion* region, GXBool is_32b_mipmap, u32 tmem_even,
                          GXTexCacheSize size_even, u32 tmem_odd, GXTexCacheSize size_odd)
{
	(void)region, (void)is_32b_mipmap, (void)tmem_even, (void)size_even, (void)tmem_odd,
	    (void)size_odd;
}
// EFB colour peeks: see "EFB readback" below.
void GXPokeAlphaRead(GXAlphaReadMode mode) { sAlphaReadMode = mode; }
void GXPeekARGB(u16 x, u16 y, u32* color)
{
	// EFB reads are host-only work that can take tens of ms (the frame so far resolved, a
	// GPU result waited for); the audio thread may take the CPU meanwhile (port_efb_host_begin).
	const int pre = port_efb_host_begin();
	*color        = efbPeekARGB(x, y);
	port_preemptible_end(pre);
}

// ---- Texture object getters ------------------------------------------------------
void GXGetTexObjAll(const GXTexObj* obj, void** image_ptr, u16* width, u16* height,
                    GXTexFmt* format, GXTexWrapMode* wrap_s, GXTexWrapMode* wrap_t,
                    u8* mipmap)
{
	*image_ptr = GXGetTexObjData(obj);
	*width     = GXGetTexObjWidth(obj);
	*height    = GXGetTexObjHeight(obj);
	*format    = GXGetTexObjFmt(obj);
	*wrap_s    = GXGetTexObjWrapS(obj);
	*wrap_t    = GXGetTexObjWrapT(obj);
	*mipmap    = GXGetTexObjMipMap(obj);
}

} // extern "C"

#endif // SMS_PORT_AURORA
