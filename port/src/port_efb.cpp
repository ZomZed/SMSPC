// EFB features for the Aurora build (w8): GXCopyTex results the CPU reads, GXPeekZ
// and the pixel metrics. (GPU screenshots, GXPeekARGB, GXPokeAlphaRead and
// port_VIConfigure: port_aurora_sdk.cpp, w6. Display copies / presenting:
// port_main.cpp, w9.)
//
// Aurora records GX work on its FIFO thread and renders it on its render worker, so
// the EFB never exists in CPU memory. Everything here snapshots the EFB at the current
// point of the command stream (aurora::gfx::resolve_pass: drains the FIFO, copies the
// targets into pooled textures and continues on a new pass that loads the EFB), then
// an encoder task (between two passes on the frame encoder) copies or reduces what is
// needed into a mappable buffer. Results arrive after the frame is submitted, 1-3
// frames later.
//
//  - GXCopyTex: the copy itself stays Aurora's GPU copy (keyed by the destination
//    address; a later GXLoadTexObj of that address samples it). Copies the CPU reads
//    are also read back into the destination memory: the game's pollution maps
//    (TPollutionLayer: GX_CTF_R8 copies into the layer image that isPolluted(), the
//    goop collision and the cleaning logic read). The copied rectangle is box-filtered
//    to the destination size on the GPU, read back, converted to the GX texture format
//    and tiling, and stored on the game thread at the end of a frame in which the game
//    copied to that address again (so the memory is known to be alive).
//    SMS_PORT_COPY_READBACK=all reads back every copy, =0 none; default: GX_CTF_R8 and the
//    depth formats Z8/Z16/Z24X8 (E28: the Corona bath water's height field is a Z24X8 copy
//    the CPU decodes; stored big-endian and tiled like any GX texture).
//    Coherence: if the CPU rewrites such a destination after a store (e.g. a layer is
//    reloaded), Aurora's GPU copy is dropped (GXDestroyCopyTex) so the next draw samples
//    the memory, as on hardware where the memory is the only copy.
//  - GXPeekZ (replaces Aurora's end-of-frame depth snapshot): the sun's lens-flare test
//    (TSunMgr::drawSyncCallback, 17 samples) reads the depth right after the opaque
//    scene, before the sun itself is drawn. Each peek queues a one-texel readback of a
//    depth snapshot taken at that point (one snapshot per draw-sync callback) and returns
//    the newest completed value for the same (draw-sync token, peek index): the value is
//    from the right point of the frame, 1-3 frames old.
//  - GXClearPixMetric / GXReadPixMetric: the pollution counters (TPollutionCounter*:
//    the "how much goop is left" degree used by events, shine conditions and the
//    sinking buildings) count pixels passing the alpha test between the two calls.
//    WebGPU occlusion queries are boolean on D3D12, so the count is taken from the
//    image: GXClearPixMetric clears the EFB colour to black (a copy-with-clear into a
//    scratch texture; the game's copy clear colour and copy rectangle are restored),
//    GXReadPixMetric counts the non-black pixels of a snapshot. In both counter passes a
//    fragment passes the alpha test exactly when its colour is non-zero (TEV colour and
//    alpha come from the same I8 pollution texel with the same constants), and nothing
//    the passes draw into the EFB is used except through the layer copy (the layer pass
//    draws black over its area first anyway). Pixels are counted once (overdraw inside
//    one counter is not counted twice), in logical EFB pixels. Until the first result
//    for a counter arrives the count reads 0x00FFFFFF ("dirty", never "clean").
//
// Game calls of GXSetTexCopySrc/Dst, GXSetCopyClear and GXCopyTex are renamed to
// port_* by build_aurora.py; these record the state and forward to Aurora.
#ifdef SMS_PORT_AURORA

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <aurora/gfx.hpp>
#include <aurora/math.hpp>
#include <dolphin/gx.h>
#include <dolphin/gx/GXAurora.h>

namespace aurora::vi {
// lib/dolphin/vi/vi_internal.hpp; whole EFB (port/patches/aurora_efb_size.patch)
Vec2<uint32_t> efb_size() noexcept;
}

extern "C" void GXDestroyCopyTex(void* dest);                  // Aurora GXExtra.cpp
extern "C" u16 GXReadDrawSync(void);                           // port_aurora_sdk.cpp
extern "C" int port_preemptible_begin(void);                  // sdk_os_thread.cpp (host work the audio thread may interrupt)
extern "C" void port_preemptible_end(int began);
extern "C" int port_efb_host_begin(void);                     // below (EFB reads: the audio thread may run)
bool port_efb_screenshot_ex(const char* path, bool whole_efb); // port_aurora_sdk.cpp (debug captures)

// w9: deterministic EFB delivery pumps Dawn's events while it waits (detEndFrame).
namespace aurora::webgpu {
extern wgpu::Instance g_instance; // lib/webgpu/gpu.cpp
}

namespace {

// ---- State mirrored from the game's calls ------------------------------------------
struct CopyState {
	u16 srcX = 0, srcY = 0, srcW = 640, srcH = 480;
	u16 dstW = 640, dstH = 480;
	GXTexFmt fmt       = GX_TF_RGBA8;
	GXBool mipmap      = GX_FALSE;
	GXColor clearColor = { 0, 0, 0, 0 };
	u32 clearZ         = 0xFFFFFF;
} sCopy;

const bool sLog = std::getenv("SMS_PORT_COPY_LOG") != nullptr;
// SMS_PORT_EFB_TRACE=1 (tests, E24): one line per value the game reads (pixel counter) and per
// copy readback stored into game memory, with the game frame; the copies of interpolated frames
// (once per destination) and any read attempted in one. Runs at different frame rates or a
// record/replay pair can be compared line by line.
const bool sTrace = std::getenv("SMS_PORT_EFB_TRACE") != nullptr;

// Logical size of the whole EFB (at least 640x528; the configured frame buffer is its
// top-left part); Aurora maps it to its render target per axis
// (gx::map_logical_viewport / map_logical_scissor).
void logicalSize(u32& w, u32& h)
{
	const auto s = aurora::vi::efb_size();
	w            = s.x != 0 ? s.x : 640;
	h            = s.y != 0 ? s.y : 528;
}

// ---- Snapshots ---------------------------------------------------------------------
// Peeks of one draw-sync callback share one snapshot.
struct SnapshotCache {
	u32 frame = ~0u;
	u32 epoch = ~0u;
	aurora::gfx::ResolvedTargets targets;
} sSnap;
u32 sEpoch;               // bumped by everything here that changes the EFB or the pass
// Game frames (one per port_efb_end_frame of a frame the game itself drew). Ages that are
// about the game's behaviour (is a copy destination still in use, was a counter read
// recently, how late was a readback) count these, not Aurora frames: with frame-rate
// interpolation (w9) several presented frames can belong to one game frame.
u32 sGameFrame = 1;
// port_efb_set_presentation_only(): the frame being recorded is an extra presented frame
// (interpolation), not one the game logic produced. Its GX calls still render, but nothing
// is read back and no CPU-visible result (copy memory, peeks, pixel counts) changes.
bool sPresentationOnly;
u16 sPeekToken = 0xFFFF;  // draw-sync token of the current peek series
u32 sPeekFrame = ~0u;
u32 sPeekIndex;

// SMS_PORT_NULLGFX (build_aurora.py --nullgfx; port_main.cpp exits unless Aurora picked
// the Null backend): Dawn's Null device cannot allocate the readback buffers ("Out of
// memory"), so nothing is read back. Copies stay GPU copies, CPU-written memory stays as
// the game wrote it, peeks keep their defaults and pixel metrics keep reading "dirty".
bool gpuReadbacks()
{
	static const bool sOn = std::getenv("SMS_PORT_NULLGFX") == nullptr;
	return sOn;
}

// w6 (round 5): sample0 = with MSAA, the colour snapshot is sample 0 of each pixel (one
// sample per pixel like the GameCube) instead of the resolve, for counters and CPU copies.
bool snapshot(bool wantDepth, aurora::gfx::ResolvedTargets& out, bool shareable, bool sample0 = false)
{
	if (!gpuReadbacks() || aurora::gfx::is_offscreen())
		return false;
	const u32 frame = aurora::gfx::current_frame();
	shareable       = shareable && !sample0; // E24: the shared snapshot is always the resolve
	if (shareable && sSnap.frame == frame && sSnap.epoch == sEpoch && sSnap.targets.colorTexture
	    && (!wantDepth || sSnap.targets.depthTexture)) {
		out = sSnap.targets;
		return true;
	}
	aurora::gfx::ResolvedTargets t;
	const aurora::gfx::ResolveDesc desc { .color = true, .depth = wantDepth, .colorSample0 = sample0 };
	if (!aurora::gfx::resolve_pass(desc, t) || !t.colorTexture || (wantDepth && !t.depthTexture))
		return false;
	++sEpoch; // the pass was broken: an older snapshot is no longer "now"
	if (shareable) {
		sSnap.frame   = frame;
		sSnap.epoch   = sEpoch;
		sSnap.targets = t;
	}
	out = t;
	return true;
}

// ---- Jobs: GPU work queued with an encoder task, read back after submit -------------
enum class JobKind : u8 { PeekDepth, CopyRead, Metric };

// Buffers of one job, pooled: a set goes back to the pool when its readback has been
// mapped and read (the GPU is done with all three), so steady state creates no buffers.
struct JobBuffers {
	wgpu::Buffer readback; // MapRead | CopyDst
	wgpu::Buffer out;      // compute output (Storage | CopySrc | CopyDst), null for peeks
	wgpu::Buffer uniform;  // compute parameters (Uniform | CopyDst), null for peeks
	u32 readBytes = 0, outBytes = 0;
};
std::mutex sPoolMutex;
std::multimap<u64, JobBuffers> sPool; // (readBytes << 32 | outBytes) -> free sets
constexpr size_t kMaxPooled = 64;

JobBuffers acquireBuffers(const wgpu::Device& dev, u32 readBytes, u32 outBytes)
{
	{
		std::lock_guard<std::mutex> lock(sPoolMutex);
		auto it = sPool.find(((u64)readBytes << 32) | outBytes);
		if (it != sPool.end()) {
			JobBuffers b = std::move(it->second);
			sPool.erase(it);
			return b;
		}
	}
	JobBuffers b;
	b.readBytes = readBytes;
	b.outBytes  = outBytes;
	wgpu::BufferDescriptor rd;
	rd.label    = "port EFB readback";
	rd.usage    = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
	rd.size     = readBytes;
	b.readback  = dev.CreateBuffer(&rd);
	if (outBytes != 0) {
		wgpu::BufferDescriptor od;
		od.label  = "port EFB compute output";
		od.usage  = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
		od.size   = outBytes;
		b.out     = dev.CreateBuffer(&od);
		wgpu::BufferDescriptor ud;
		ud.label  = "port EFB params";
		ud.usage  = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
		ud.size   = 64;
		b.uniform = dev.CreateBuffer(&ud);
	}
	return b;
}

void releaseBuffers(JobBuffers&& b)
{
	if (!b.readback)
		return;
	std::lock_guard<std::mutex> lock(sPoolMutex);
	if (sPool.size() < kMaxPooled)
		sPool.emplace(((u64)b.readBytes << 32) | b.outBytes, std::move(b));
}

struct Job {
	JobKind kind;
	wgpu::Texture tex;          // snapshot (valid for the frame it was taken in)
	wgpu::TextureFormat format; // snapshot colour format
	u32 texW = 0, texH = 0;
	JobBuffers bufs;            // from the pool (render worker), back to it when read
	u64 busyKey = 0;            // outstanding-job key (keyBusy)
	u32 readBytes = 0;
	u32 x = 0, y = 0;           // peek texel (target pixels)
	u32 key = 0;                // peek / metric result key
	float toLogical = 1.0f;     // metric: logical pixels per target pixel
	// copy readback
	float ox = 0, oy = 0, sx = 1, sy = 1; // source origin / step per destination texel (target px)
	u32 taps = 1, dstW = 0, dstH = 0;
	GXTexFmt texFmt = GX_TF_RGBA8;
	bool depth      = false;    // copy readback of a depth format: data = z24 per texel (u32)
	void* dest      = nullptr;
	u64 serial      = 0;
	u32 frame       = 0;
	std::vector<u8> data;       // copy readback result (RGBA8 per destination texel)
	// w9 deterministic delivery (see detEndFrame): result kept here until it is applied
	u32 detFrame = 0;
	u32 value    = 0; // peek z24 / metric count
	bool ok      = false;
	std::atomic<bool> done{ false };
};

aurora::gfx::EncoderTaskId sTask = aurora::gfx::InvalidEncoderTask;

std::mutex sMutex;              // everything below written by the map callbacks
std::map<u32, u32> sPeekDepth;  // key -> z24
std::map<u32, u32> sMetric;     // draw-sync token -> logical pixels
std::map<u32, u32> sZeros;      // token -> zero results in a row
std::map<u32, bool> sSeenNonZero; // token -> a non-zero result was seen (this stage)
std::map<u32, u32> sMetricLastRead; // token -> frame of the last GXReadPixMetric (game thread)
std::map<u32, u32> sMetricEpoch;    // token -> frame its counter started over
std::vector<Job*> sCopyDone;    // completed copy readbacks, stored on the game thread
int sInFlight;
// Outstanding jobs per result (copy destination, peek, counter). When the GPU is slow a
// result that already has kMaxPerKey (4) requests in flight is not requested again (no
// snapshot, no pass break, no job): requests coalesce to the GPU's pace instead of piling up.
struct Outstanding {
	int n     = 0;
	u32 frame = 0; // game frame of the newest request
};
std::map<u64, Outstanding> sOutstanding;
constexpr int kMaxPerKey   = 4; // results normally arrive 1-3 frames late
constexpr int kMaxInFlight = 256;

u64 copyBusyKey(const void* dest) { return (u64)(uintptr_t)dest; }
u64 peekBusyKey(u32 key) { return (1ull << 62) | key; }
u64 metricBusyKey(u32 key) { return (2ull << 62) | key; }

// w9 (input replay, port_replay.cpp): while input is recorded or replayed, results reach
// the game a fixed number of game frames after they were requested (kDetLatency game
// frames, waiting for the GPU if it is later), instead of whenever the GPU finishes (1-3
// frames). Otherwise the pollution maps, goop collision and pixel counters the game logic
// reads would depend on the PC's GPU timing, and a replay would drift.
extern "C" bool port_replay_deterministic(void);
const u32 kDetLatency = 3;
std::deque<Job*> sDetQueue; // requested jobs in request order (game thread)

bool keyBusy(u64 key)
{
	// w9: never coalesce while deterministic: whether a request is made must not depend on
	// the GPU's pace (detEndFrame waits for each result instead).
	if (port_replay_deterministic())
		return false;
	std::lock_guard<std::mutex> lock(sMutex);
	if (sInFlight >= kMaxInFlight)
		return true;
	auto it = sOutstanding.find(key);
	if (it == sOutstanding.end())
		return false;
	if (sGameFrame - it->second.frame > 60) { // lost requests (frame never submitted): forget
		sInFlight -= it->second.n;
		sOutstanding.erase(it);
		return false;
	}
	return it->second.n >= kMaxPerKey;
}

// Game-thread bookkeeping for copy readbacks.
struct DestInfo {
	u32 lastCopyFrame = 0; // frame of the game's latest copy to it
	u64 applied       = 0; // serial of the newest data stored
	u32 bytes         = 0; // size of the stored texture
	u64 hash          = 0; // hash of the bytes as stored (CPU-write detection)
	u64 discardUpTo   = 0; // readbacks of copies up to this serial predate a CPU write
};
std::map<void*, DestInfo> sDests;
u64 sCopySerial;

// ---- Compute pipelines (created on the render worker on first use) -----------------
const char* kDownsampleWGSL = R"(
struct P { origin: vec2f, step: vec2f, dst: vec2u, src: vec2u, taps: vec2u, srgb: u32, pad: u32 };
@group(0) @binding(0) var tex: texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> outp: array<u32>;
@group(0) @binding(2) var<uniform> p: P;
fn toSrgb(c: f32) -> f32 {
  if (c <= 0.0031308) { return c * 12.92; }
  return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}
@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  if (id.x >= p.dst.x || id.y >= p.dst.y) { return; }
  var acc = vec4f(0.0);
  for (var j = 0u; j < p.taps.y; j++) {
    for (var i = 0u; i < p.taps.x; i++) {
      let f = (vec2f(f32(i), f32(j)) + vec2f(0.5)) / vec2f(p.taps);
      let s = p.origin + (vec2f(id.xy) + f) * p.step;
      let c = clamp(vec2i(floor(s)), vec2i(0), vec2i(p.src) - vec2i(1));
      var t = textureLoad(tex, c, 0);
      if (p.srgb != 0u) { t = vec4f(toSrgb(t.r), toSrgb(t.g), toSrgb(t.b), t.a); }
      acc += t;
    }
  }
  let v = clamp(acc / f32(p.taps.x * p.taps.y), vec4f(0.0), vec4f(1.0));
  let b = vec4u(round(v * 255.0));
  outp[id.y * p.dst.x + id.x] = b.x | (b.y << 8u) | (b.z << 16u) | (b.w << 24u);
}
)";

// w6 (round 5): counted on the logical EFB grid (one sample per GameCube pixel, at its
// centre) instead of over every render-target pixel scaled by the area ratio, so the count
// does not depend on the internal resolution or the widescreen target shape (the target's
// scale is not uniform: at 1x the 528 logical rows are 566 target rows).
const char* kCountWGSL = R"(
struct P { size: vec2u, src: vec2u };
@group(0) @binding(0) var tex: texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> total: atomic<u32>;
@group(0) @binding(2) var<uniform> p: P;
var<workgroup> local: atomic<u32>;
@compute @workgroup_size(16, 16, 1)
fn cs_main(@builtin(global_invocation_id) id: vec3u, @builtin(local_invocation_index) li: u32) {
  if (li == 0u) { atomicStore(&local, 0u); }
  workgroupBarrier();
  if (id.x < p.size.x && id.y < p.size.y) {
    let s = (vec2f(id.xy) + vec2f(0.5)) * vec2f(p.src) / vec2f(p.size);
    let t = clamp(vec2i(floor(s)), vec2i(0), vec2i(p.src) - vec2i(1));
    let c = textureLoad(tex, t, 0).rgb;
    if (any(c > vec3f(0.5 / 255.0))) { atomicAdd(&local, 1u); }
  }
  workgroupBarrier();
  if (li == 0u) { atomicAdd(&total, atomicLoad(&local)); }
}
)";

// E28: depth copy readback. The depth snapshot is R32Float (Aurora's pass snapshot, sample 0
// with MSAA); each destination texel takes the depth at its GameCube pixel centre as z24.
const char* kDepthWGSL = R"(
struct P { origin: vec2f, step: vec2f, dst: vec2u, src: vec2u, rev: u32, pad: u32, pad2: vec2u };
@group(0) @binding(0) var tex: texture_2d<f32>;
@group(0) @binding(1) var<storage, read_write> outp: array<u32>;
@group(0) @binding(2) var<uniform> p: P;
@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) id: vec3u) {
  if (id.x >= p.dst.x || id.y >= p.dst.y) { return; }
  let s = p.origin + (vec2f(id.xy) + vec2f(0.5)) * p.step;
  let c = clamp(vec2i(floor(s)), vec2i(0), vec2i(p.src) - vec2i(1));
  var d = textureLoad(tex, c, 0).r;
  if (p.rev != 0u) { d = 1.0 - d; }
  outp[id.y * p.dst.x + id.x] = u32(round(clamp(d, 0.0, 1.0) * 16777215.0));
}
)";

wgpu::ComputePipeline sDownsamplePipe, sCountPipe, sDepthPipe;

// R32Float is not filterable: the depth pipeline needs an explicit UnfilterableFloat layout.
wgpu::ComputePipeline makeDepthPipeline(const wgpu::Device& dev)
{
	wgpu::BindGroupLayoutEntry e[3] = {};
	e[0].binding               = 0;
	e[0].visibility            = wgpu::ShaderStage::Compute;
	e[0].texture.sampleType    = wgpu::TextureSampleType::UnfilterableFloat;
	e[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
	e[1].binding               = 1;
	e[1].visibility            = wgpu::ShaderStage::Compute;
	e[1].buffer.type           = wgpu::BufferBindingType::Storage;
	e[2].binding               = 2;
	e[2].visibility            = wgpu::ShaderStage::Compute;
	e[2].buffer.type           = wgpu::BufferBindingType::Uniform;
	wgpu::BindGroupLayoutDescriptor bd;
	bd.entryCount              = 3;
	bd.entries                 = e;
	wgpu::BindGroupLayout bgl  = dev.CreateBindGroupLayout(&bd);
	wgpu::PipelineLayoutDescriptor pl;
	pl.bindGroupLayoutCount = 1;
	pl.bindGroupLayouts     = &bgl;
	wgpu::ShaderSourceWGSL src;
	src.code = kDepthWGSL;
	wgpu::ShaderModuleDescriptor md;
	md.nextInChain = &src;
	md.label       = "port EFB depth readback";
	wgpu::ComputePipelineDescriptor pd;
	pd.label              = "port EFB depth readback";
	pd.layout             = dev.CreatePipelineLayout(&pl);
	pd.compute.module     = dev.CreateShaderModule(&md);
	pd.compute.entryPoint = "cs_main";
	return dev.CreateComputePipeline(&pd);
}

wgpu::ComputePipeline makePipeline(const wgpu::Device& dev, const char* code, const char* label)
{
	wgpu::ShaderSourceWGSL src;
	src.code = code;
	wgpu::ShaderModuleDescriptor md;
	md.nextInChain = &src;
	md.label       = label;
	wgpu::ComputePipelineDescriptor pd;
	pd.label              = label;
	pd.compute.module     = dev.CreateShaderModule(&md);
	pd.compute.entryPoint = "cs_main";
	return dev.CreateComputePipeline(&pd);
}

void dispatch(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd,
              const wgpu::ComputePipeline& pipe, const wgpu::Texture& tex, const void* params,
              size_t paramSize, u32 groupsX, u32 groupsY, const JobBuffers& b)
{
	// The pooled buffers are idle (their previous job's readback was mapped): the queue write
	// lands before this frame's commands run, and the output starts at zero (the counter
	// accumulates into it).
	u8 ubData[64] = {};
	std::memcpy(ubData, params, paramSize < sizeof(ubData) ? paramSize : sizeof(ubData));
	ctx.queue.WriteBuffer(b.uniform, 0, ubData, sizeof(ubData));
	cmd.ClearBuffer(b.out, 0, b.outBytes);
	const u64 outBytes   = b.outBytes;
	wgpu::Buffer out     = b.out;
	wgpu::Buffer ub      = b.uniform;
	wgpu::Buffer readback = b.readback;
	wgpu::TextureView vw = tex.CreateView();
	wgpu::BindGroupEntry e[3];
	e[0].binding     = 0;
	e[0].textureView = vw;
	e[1].binding     = 1;
	e[1].buffer      = out;
	e[1].size        = outBytes;
	e[2].binding     = 2;
	e[2].buffer      = ub;
	e[2].size        = ub.GetSize();
	wgpu::BindGroupDescriptor bgd;
	bgd.layout     = pipe.GetBindGroupLayout(0);
	bgd.entryCount = 3;
	bgd.entries    = e;
	wgpu::BindGroup bg            = ctx.device.CreateBindGroup(&bgd);
	wgpu::ComputePassEncoder pass = cmd.BeginComputePass();
	pass.SetPipeline(pipe);
	pass.SetBindGroup(0, bg);
	pass.DispatchWorkgroups(groupsX, groupsY);
	pass.End();
	cmd.CopyBufferToBuffer(out, 0, readback, 0, outBytes);
}

bool isSrgb(wgpu::TextureFormat f)
{
	return f == wgpu::TextureFormat::BGRA8UnormSrgb || f == wgpu::TextureFormat::RGBA8UnormSrgb;
}

// Render worker, between two passes.
void jobEncode(const aurora::gfx::EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd,
               const void* payload, size_t, void*)
{
	Job* j = *(Job* const*)payload;
	const u32 outBytes = j->kind == JobKind::PeekDepth ? 0 : j->kind == JobKind::Metric ? 16 : j->readBytes;
	j->bufs = acquireBuffers(ctx.device, j->readBytes, outBytes);

	switch (j->kind) {
	case JobKind::PeekDepth: {
		const wgpu::TexelCopyTextureInfo src { .texture = j->tex, .origin = { j->x, j->y, 0 } };
		const wgpu::TexelCopyBufferInfo dst {
			.layout = { .offset = 0, .bytesPerRow = 256, .rowsPerImage = 1 }, .buffer = j->bufs.readback
		};
		const wgpu::Extent3D size { 1, 1, 1 };
		cmd.CopyTextureToBuffer(&src, &dst, &size);
		break;
	}
	case JobKind::CopyRead: {
		if (j->depth) {
			if (!sDepthPipe)
				sDepthPipe = makeDepthPipeline(ctx.device);
			struct {
				float ox, oy, sx, sy;
				u32 dstW, dstH, srcW, srcH, rev, pad, pad2[2];
			} dp = { j->ox, j->oy, j->sx, j->sy, j->dstW, j->dstH, j->texW, j->texH,
				     aurora::gfx::uses_reversed_z() ? 1u : 0u, 0, { 0, 0 } };
			dispatch(ctx, cmd, sDepthPipe, j->tex, &dp, sizeof(dp), (j->dstW + 7) / 8, (j->dstH + 7) / 8, j->bufs);
			break;
		}
		if (!sDownsamplePipe)
			sDownsamplePipe = makePipeline(ctx.device, kDownsampleWGSL, "port EFB copy readback");
		struct {
			float ox, oy, sx, sy;
			u32 dstW, dstH, srcW, srcH, tapsX, tapsY, srgb, pad;
		} params = { j->ox, j->oy, j->sx, j->sy, j->dstW, j->dstH, j->texW, j->texH,
			         j->taps, j->taps, isSrgb(j->format) ? 1u : 0u, 0 };
		dispatch(ctx, cmd, sDownsamplePipe, j->tex, &params, sizeof(params), (j->dstW + 7) / 8,
		         (j->dstH + 7) / 8, j->bufs);
		break;
	}
	case JobKind::Metric: {
		if (!sCountPipe)
			sCountPipe = makePipeline(ctx.device, kCountWGSL, "port pixel metric");
		struct {
			u32 w, h, srcW, srcH;
		} params = { j->dstW, j->dstH, j->texW, j->texH }; // w6: logical grid, target size
		dispatch(ctx, cmd, sCountPipe, j->tex, &params, sizeof(params), (j->dstW + 15) / 16,
		         (j->dstH + 15) / 16, j->bufs);
		break;
	}
	}
}

void jobComplete(Job* j, wgpu::MapAsyncStatus status)
{
	bool keep = false;
	if (status == wgpu::MapAsyncStatus::Success) {
		const u8* data = (const u8*)j->bufs.readback.GetConstMappedRange(0, j->readBytes);
		if (data != nullptr) {
			switch (j->kind) {
			case JobKind::PeekDepth: {
				float d;
				std::memcpy(&d, data, 4);
				if (aurora::gfx::uses_reversed_z())
					d = 1.0f - d;
				d       = d < 0 ? 0 : d > 1 ? 1 : d;
				u32 z24 = (u32)(d * 16777215.0f + 0.5f);
				if (j->detFrame != 0) { // w9: applied by detEndFrame
					j->value = z24 > 0xFFFFFF ? 0xFFFFFF : z24;
					j->ok    = true;
					break;
				}
				std::lock_guard<std::mutex> lock(sMutex);
				sPeekDepth[j->key] = z24 > 0xFFFFFF ? 0xFFFFFF : z24;
				static u32 sLogged;
				if (sLog && (sLogged++ % 500) == 0)
					std::fprintf(stderr, "[port] GXPeekZ key 0x%x at %u,%u (target px): 0x%06x\n", j->key,
					             j->x, j->y, sPeekDepth[j->key]);
				break;
			}
			case JobKind::Metric: {
				u32 n;
				std::memcpy(&n, data, 4);
				if (j->detFrame != 0) { // w9: applied by detEndFrame
					j->value = n;
					j->ok    = true;
					break;
				}
				std::lock_guard<std::mutex> lock(sMutex);
				if (auto ep = sMetricEpoch.find(j->key); ep != sMetricEpoch.end() && j->frame < ep->second)
					break; // read before the counter started over (previous stage)
				// A zero count is believed only after 30 in a row: the first frames of a stage
				// can skip the counter's draw (pipeline still compiling), and a false "clean"
				// would fire the stage's cleaned events.
				// A counter that has read non-zero before (goop being cleaned) needs only 2 zeros
				// in a row, so the stage's "cleaned" event is not delayed noticeably.
				u32& zeros = sZeros[j->key];
				zeros      = n == 0 ? zeros + 1 : 0;
				if (n != 0)
					sSeenNonZero[j->key] = true;
				if (n != 0 || zeros >= (sSeenNonZero[j->key] ? 2u : 30u))
					sMetric[j->key] = (u32)std::lround((double)n * j->toLogical);
				static u32 sLogged;
				if (sLog && (sLogged++ % 300) == 0)
					std::fprintf(stderr, "[port] pixel metric token 0x%x: %u logical pixels (%u zero results in a row)\n",
					             j->key, (u32)std::lround((double)n * j->toLogical), zeros);
				break;
			}
			case JobKind::CopyRead:
				j->data.assign(data, data + j->readBytes);
				j->ok = true; // w9
				keep  = true;
				break;
			}
		}
		j->bufs.readback.Unmap();
		releaseBuffers(std::move(j->bufs)); // GPU done with them (failed maps: dropped)
	}
	j->bufs = {};
	std::lock_guard<std::mutex> lock(sMutex);
	if (sInFlight > 0) // (can already be 0 when keyBusy forgot this request as lost)
		--sInFlight;
	if (auto it = sOutstanding.find(j->busyKey); it != sOutstanding.end() && --it->second.n <= 0)
		sOutstanding.erase(it);
	if (j->detFrame != 0) { // w9: detEndFrame applies and deletes it
		j->done.store(true);
		return;
	}
	if (keep)
		sCopyDone.push_back(j);
	else
		delete j;
}

// Render worker, after the frame's command buffer was submitted.
void jobSubmitted(const aurora::gfx::EncoderTaskCompletionContext&, const void* payload, size_t, void*)
{
	Job* j = *(Job* const*)payload;
	j->tex = nullptr; // the pooled snapshot is recycled with its frame slot
	if (!j->bufs.readback) {
		jobComplete(j, wgpu::MapAsyncStatus::Error);
		return;
	}
	j->bufs.readback.MapAsync(wgpu::MapMode::Read, 0, j->readBytes, wgpu::CallbackMode::AllowSpontaneous,
	                     [j](wgpu::MapAsyncStatus status, wgpu::StringView) { jobComplete(j, status); });
}

bool pushJob(Job* j)
{
	if (!gpuReadbacks())
		return false;
	if (sTask == aurora::gfx::InvalidEncoderTask) {
		const aurora::gfx::EncoderTaskDescriptor desc { .label       = "port EFB readback (w8)",
			                                            .callback    = jobEncode,
			                                            .userdata    = nullptr,
			                                            .afterSubmit = jobSubmitted };
		sTask = aurora::gfx::register_encoder_task_type(desc);
	}
	{
		std::lock_guard<std::mutex> lock(sMutex);
		if (sInFlight >= kMaxInFlight)
			return false; // the GPU is far behind; drop rather than pile up
		++sInFlight;
		Outstanding& o = sOutstanding[j->busyKey];
		++o.n;
		o.frame = sGameFrame;
	}
	if (port_replay_deterministic())
		j->detFrame = sGameFrame; // non-zero marks deterministic delivery (w9)
	if (!aurora::gfx::push_encoder_task(sTask, &j, sizeof(j))) {
		std::lock_guard<std::mutex> lock(sMutex);
		--sInFlight;
		if (auto it = sOutstanding.find(j->busyKey); it != sOutstanding.end() && --it->second.n <= 0)
			sOutstanding.erase(it);
		return false;
	}
	if (j->detFrame != 0)
		sDetQueue.push_back(j);
	++sEpoch;
	return true;
}

// ---- Copy readback ---------------------------------------------------------------
int copyReadbackMode()
{
	static int mode = -1; // 0 none, 1 GX_CTF_R8 (default), 2 all
	if (mode < 0) {
		const char* e = std::getenv("SMS_PORT_COPY_READBACK");
		mode          = e == nullptr ? 1 : std::strcmp(e, "all") == 0 ? 2 : std::atoi(e) != 0 ? 1 : 0;
	}
	return mode;
}

// Depth copy formats read back (E28): Z8, Z16, Z24X8. The bath water's height field
// (TBathWaterMeshRenderer::makeHeightMap) is a Z24X8 copy the CPU decodes every frame. The
// CTF depth formats (Z4, Z8M, Z8L, Z16L) are not used by the game and stay GPU-only.
bool isReadableDepthFmt(GXTexFmt f) { return f == GX_TF_Z8 || f == GX_TF_Z16 || f == GX_TF_Z24X8; }

void queueCopyReadback(void* dest)
{
	const int mode    = copyReadbackMode();
	const bool depth  = (sCopy.fmt & _GX_TF_ZTF) != 0;
	if (depth && !isReadableDepthFmt(sCopy.fmt))
		return;
	if (mode == 0 || (mode == 1 && sCopy.fmt != GX_CTF_R8 && !depth))
		return;
	if (sCopy.dstW == 0 || sCopy.dstH == 0 || sCopy.srcW == 0 || sCopy.srcH == 0)
		return;
	if (keyBusy(copyBusyKey(dest)))
		return; // GPU behind: the newer of the requests in flight will be stored
	aurora::gfx::ResolvedTargets t;
	if (!snapshot(depth, t, false, !depth)) // w6: colour = sample 0 with MSAA; depth: sample 0 anyway
		return;
	u32 lw, lh;
	logicalSize(lw, lh);
	Job* j       = new Job {};
	j->kind      = JobKind::CopyRead;
	j->depth     = depth;
	j->tex       = depth ? t.depthTexture : t.colorTexture;
	j->format    = t.colorFormat;
	j->texW      = t.width;
	j->texH      = t.height;
	j->ox        = sCopy.srcX * (float)t.width / lw;
	j->oy        = sCopy.srcY * (float)t.height / lh;
	j->sx        = sCopy.srcW * (float)t.width / lw / sCopy.dstW;
	j->sy        = sCopy.srcH * (float)t.height / lh / sCopy.dstH;
	const float s = j->sx > j->sy ? j->sx : j->sy;
	j->taps      = s <= 1.0f ? 1 : s >= 8.0f ? 8 : (u32)std::ceil(s);
	// w6 (round 5): a 1:1 copy reads each GameCube pixel at its centre, as the copy unit
	// does, instead of box-filtering its (non-integer, e.g. 4x4.29 at 4x) block of target
	// pixels, which mixed in neighbouring rows: the stored map depended on the internal
	// resolution (pollution maps are drawn with nearest sampling, one block per texel).
	if ((sCopy.srcW == sCopy.dstW && sCopy.srcH == sCopy.dstH) || depth)
		j->taps = 1; // depth: the texel at the GameCube pixel centre, never averaged
	j->dstW      = sCopy.dstW;
	j->dstH      = sCopy.dstH;
	j->texFmt    = sCopy.fmt;
	j->dest      = dest;
	j->busyKey   = copyBusyKey(dest);
	j->serial    = ++sCopySerial;
	j->frame     = sGameFrame;
	j->readBytes = ((j->dstW * j->dstH * 4) + 255) & ~255u;
	if (!pushJob(j))
		delete j;
}

// RGB -> Y, as the copy unit does for the intensity formats.
inline u8 luma(const u8* c)
{
	int y = (66 * c[0] + 129 * c[1] + 25 * c[2] + 128) / 256 + 16;
	return (u8)(y > 255 ? 255 : y);
}

// GX texture size in bytes (tiled).
u32 texBytes(GXTexFmt fmt, u32 w, u32 h)
{
	switch (fmt) {
	case GX_TF_I4:
	case GX_CTF_R4: return ((w + 7) / 8) * ((h + 7) / 8) * 32;
	case GX_TF_I8:
	case GX_TF_IA4:
	case GX_CTF_RA4:
	case GX_CTF_A8:
	case GX_CTF_R8:
	case GX_CTF_G8:
	case GX_CTF_B8:
	case GX_TF_Z8: return ((w + 7) / 8) * ((h + 3) / 4) * 32;
	case GX_TF_RGBA8:
	case GX_TF_Z24X8: return ((w + 3) / 4) * ((h + 3) / 4) * 64;
	default: return ((w + 3) / 4) * ((h + 3) / 4) * 32;
	}
}

// Stores RGBA8 texels as a GX texture (big-endian, tiled) at dest.
void encodeTexture(GXTexFmt fmt, const u8* rgba, u32 w, u32 h, u8* dest)
{
	auto texel = [&](u32 x, u32 y) -> const u8* {
		static const u8 zero[4] = { 0, 0, 0, 0 };
		return (x < w && y < h) ? rgba + ((size_t)y * w + x) * 4 : zero;
	};
	u32 bw, bh, bpp; // block size in texels, bits per texel
	switch (fmt) {
	case GX_TF_I4:
	case GX_CTF_R4: bw = 8, bh = 8, bpp = 4; break;
	case GX_TF_I8:
	case GX_TF_IA4:
	case GX_CTF_RA4:
	case GX_CTF_A8:
	case GX_CTF_R8:
	case GX_CTF_G8:
	case GX_CTF_B8:
	case GX_TF_Z8: bw = 8, bh = 4, bpp = 8; break;
	case GX_TF_RGBA8:
	case GX_TF_Z24X8: bw = 4, bh = 4, bpp = 32; break; // Z24X8: (X, Z hi) plane, (Z mid, Z lo) plane
	default: bw = 4, bh = 4, bpp = 16; break; // IA8, RA8, RG8, GB8, RGB565, RGB5A3
	}
	u8* out = dest;
	for (u32 by = 0; by < h; by += bh) {
		for (u32 bx = 0; bx < w; bx += bw) {
			if (bpp == 32) { // AR plane, then GB plane
				for (u32 y = 0; y < 4; y++)
					for (u32 x = 0; x < 4; x++) {
						const u8* c = texel(bx + x, by + y);
						*out++ = c[3], *out++ = c[0];
					}
				for (u32 y = 0; y < 4; y++)
					for (u32 x = 0; x < 4; x++) {
						const u8* c = texel(bx + x, by + y);
						*out++ = c[1], *out++ = c[2];
					}
				continue;
			}
			for (u32 y = 0; y < bh; y++) {
				for (u32 x = 0; x < bw; x++) {
					const u8* c = texel(bx + x, by + y);
					switch (fmt) {
					case GX_TF_I4:
					case GX_CTF_R4: {
						u8 v = (fmt == GX_TF_I4 ? luma(c) : c[0]) >> 4;
						if ((x & 1) == 0)
							*out = (u8)(v << 4);
						else
							*out++ |= v;
						break;
					}
					case GX_TF_I8: *out++ = luma(c); break;
					case GX_CTF_R8: *out++ = c[0]; break;
					case GX_CTF_G8: *out++ = c[1]; break;
					case GX_CTF_B8: *out++ = c[2]; break;
					case GX_CTF_A8: *out++ = c[3]; break;
					case GX_TF_Z8: *out++ = c[0]; break;                       // Z[23:16]
					case GX_TF_Z16: *out++ = c[0], *out++ = c[1]; break;       // Z[23:8], big-endian
					case GX_TF_IA4: *out++ = (u8)((c[3] & 0xF0) | (luma(c) >> 4)); break;
					case GX_CTF_RA4: *out++ = (u8)((c[3] & 0xF0) | (c[0] >> 4)); break;
					case GX_TF_IA8: *out++ = c[3], *out++ = luma(c); break;
					case GX_CTF_RA8: *out++ = c[3], *out++ = c[0]; break;
					case GX_CTF_RG8: *out++ = c[0], *out++ = c[1]; break;
					case GX_CTF_GB8: *out++ = c[1], *out++ = c[2]; break;
					case GX_TF_RGB565: {
						u16 v  = (u16)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
						*out++ = (u8)(v >> 8), *out++ = (u8)v;
						break;
					}
					case GX_TF_RGB5A3: {
						u16 v;
						if (c[3] >= 0xE0)
							v = (u16)(0x8000 | ((c[0] >> 3) << 10) | ((c[1] >> 3) << 5) | (c[2] >> 3));
						else
							v = (u16)(((c[3] >> 5) << 12) | ((c[0] >> 4) << 8) | ((c[1] >> 4) << 4)
							          | (c[2] >> 4));
						*out++ = (u8)(v >> 8), *out++ = (u8)v;
						break;
					}
					default: *out++ = 0, *out++ = 0; break;
					}
				}
			}
		}
	}
}

u64 hashBytes(const void* p, u32 n) // FNV-1a over 8-byte words
{
	const u8* b = (const u8*)p;
	u64 h       = 1469598103934665603ull;
	u32 i       = 0;
	for (; i + 8 <= n; i += 8) {
		u64 v;
		std::memcpy(&v, b + i, 8);
		h = (h ^ v) * 1099511628211ull;
	}
	for (; i < n; i++)
		h = (h ^ b[i]) * 1099511628211ull;
	return h;
}

// w9: applies the results requested kDetLatency game frames ago, in request order (see
// sDetQueue). Game thread, end of each game frame (before sGameFrame advances).
void detEndFrame()
{
	while (!sDetQueue.empty() && sDetQueue.front()->detFrame + kDetLatency <= sGameFrame) {
		Job* j = sDetQueue.front();
		// The GPU work was submitted frames ago; it normally finished long since. Map
		// callbacks fire from Instance::ProcessEvents, which Aurora runs at frame ends; this
		// frame cannot end while we wait, so pump it here.
		// host-only wait for the GPU: the audio thread may run meanwhile (port_efb_host_begin)
		const int pre = j->done.load() ? 0 : port_efb_host_begin();
		for (int i = 0; i < 2000 && !j->done.load(); i++) {
			aurora::webgpu::g_instance.ProcessEvents();
			if (!j->done.load())
				std::this_thread::sleep_for(std::chrono::microseconds(500));
		}
		port_preemptible_end(pre);
		if (!j->done.load()) {
			std::fprintf(stderr, "[port] EFB result %u frames late: replays may drift (w9)\n", sGameFrame - j->detFrame);
			break; // jobComplete still owns it
		}
		sDetQueue.pop_front();
		if (j->ok && j->kind == JobKind::CopyRead) {
			j->detFrame = 0;
			std::lock_guard<std::mutex> lock(sMutex);
			sCopyDone.push_back(j); // stored by endFrame below, in this order
			continue;
		}
		if (j->ok) {
			std::lock_guard<std::mutex> lock(sMutex);
			if (j->kind == JobKind::PeekDepth) {
				sPeekDepth[j->key] = j->value;
			} else if (auto ep = sMetricEpoch.find(j->key); ep == sMetricEpoch.end() || j->frame >= ep->second) {
				const u32 n = j->value; // as in jobComplete
				u32& zeros  = sZeros[j->key];
				zeros       = n == 0 ? zeros + 1 : 0;
				if (n != 0)
					sSeenNonZero[j->key] = true;
				if (n != 0 || zeros >= (sSeenNonZero[j->key] ? 2u : 30u))
					sMetric[j->key] = (u32)std::lround((double)n * j->toLogical);
			}
		}
		delete j;
	}
}

// Game thread, end of each Aurora frame.
void endFrame()
{
	if (port_replay_deterministic() || !sDetQueue.empty())
		detEndFrame();
	const u32 now = sGameFrame;

	// CPU writes to a destination since our last store: drop Aurora's GPU copy so the
	// next draw samples the memory (hardware has only the memory).
	for (auto& [dest, info] : sDests) {
		if (info.applied != 0 && info.bytes != 0 && hashBytes(dest, info.bytes) != info.hash) {
			if (sLog)
				std::fprintf(stderr, "[port] copy destination %p rewritten by the CPU: GPU copy dropped\n", dest);
			GXDestroyCopyTex(dest);
			info.applied     = 0;
			info.bytes       = 0;
			info.discardUpTo = sCopySerial; // copies issued so far read the old texture
		}
	}

	std::vector<Job*> done;
	{
		std::lock_guard<std::mutex> lock(sMutex);
		done.swap(sCopyDone);
	}
	for (Job* j : done) {
		if (j->depth) // z24 per texel (u32) -> {Z[23:16], Z[15:8], Z[7:0], 0xFF} for encodeTexture
			for (size_t i = 0; i + 4 <= j->data.size() && i < (size_t)j->dstW * j->dstH * 4; i += 4) {
				const u32 z = j->data[i] | (j->data[i + 1] << 8) | (j->data[i + 2] << 16);
				j->data[i]     = (u8)(z >> 16);
				j->data[i + 1] = (u8)(z >> 8);
				j->data[i + 2] = (u8)z;
				j->data[i + 3] = 0xFF;
			}
		auto it    = sDests.find(j->dest);
		bool alive = it != sDests.end() && now - it->second.lastCopyFrame <= 2;
		if (alive && j->serial > it->second.applied && j->serial > it->second.discardUpTo
		    && j->data.size() >= (size_t)j->dstW * j->dstH * 4) {
			DestInfo& info = it->second;
			const u32 n    = texBytes(j->texFmt, j->dstW, j->dstH);
			if (info.applied == 0) {
				// First store: Aurora skips draws whose pipeline is still compiling, so the
				// first copies of a destination can be empty although the texture they were
				// drawn from (the memory) is not. Such a copy is not stored, and the GPU copy
				// is dropped so the next draw samples the memory again; this repeats until a
				// copy has content. (port/patches/aurora_copy_pass_pipelines.patch removes the
				// cause.)
				u32 before = 0, after = 0;
				for (u32 i = 0; i < n; i++)
					before += ((const u8*)j->dest)[i] != 0;
				for (size_t i = 0; i < (size_t)j->dstW * j->dstH; i++)
					after += (j->data[i * 4] | j->data[i * 4 + 1] | j->data[i * 4 + 2]) != 0;
				if (sLog)
					std::fprintf(stderr, "[port] copy readback %p: first store candidate, memory %u/%u non-zero, copy %s\n",
					             j->dest, before, n, after ? "has content" : "empty");
				if (after == 0 && before != 0) {
					GXDestroyCopyTex(j->dest);
					delete j;
					continue;
				}
			}
			encodeTexture(j->texFmt, j->data.data(), j->dstW, j->dstH, (u8*)j->dest);
			info.applied = j->serial;
			info.bytes   = n;
			info.hash    = hashBytes(j->dest, n);
			if (sTrace) {
				u32 nz = 0;
				u32 zmin = 0xFFFFFF, zmax = 0;
				for (size_t i = 0; i < (size_t)j->dstW * j->dstH; i++) {
					nz += j->data[i * 4] != 0;
					const u32 z = (j->data[i * 4] << 16) | (j->data[i * 4 + 1] << 8) | j->data[i * 4 + 2];
					zmin = z < zmin ? z : zmin;
					zmax = z > zmax ? z : zmax;
				}
				std::fprintf(stderr, "[efbtrace] f=%u store %p %ux%u fmt 0x%x nz=%u hash=%016llx req=%u", now, j->dest,
				             j->dstW, j->dstH, (u32)j->texFmt, nz, (unsigned long long)info.hash, j->frame);
				if (j->depth)
					std::fprintf(stderr, " z %06x..%06x", zmin, zmax);
				std::fprintf(stderr, "\n");
			}
			static u32 sLogged;
			if (sLog && (sLogged++ % 300) == 0) {
				u32 nonzero = 0;
				for (size_t i = 0; i < (size_t)j->dstW * j->dstH; i++)
					nonzero += j->data[i * 4] != 0;
				std::fprintf(stderr,
				             "[port] copy readback %p: %ux%u fmt 0x%x, %u/%u texels with R>0, %u frames late\n",
				             j->dest, j->dstW, j->dstH, (u32)j->texFmt, nonzero, j->dstW * j->dstH,
				             now - j->frame);
			}
		}
		delete j;
	}

	// forget destinations the game stopped copying to (it may free them)
	for (auto it = sDests.begin(); it != sDests.end();) {
		if (now - it->second.lastCopyFrame > 8)
			it = sDests.erase(it);
		else
			++it;
	}
}

// ---- Peeks / pixel metrics -----------------------------------------------------------
u32 peekKey()
{
	const u16 token = GXReadDrawSync();
	const u32 frame = aurora::gfx::current_frame();
	if (token != sPeekToken || frame != sPeekFrame) {
		sPeekToken = token;
		sPeekFrame = frame;
		sPeekIndex = 0;
		++sEpoch; // new callback: new snapshot
	}
	return ((u32)token << 8) | (sPeekIndex++ & 0xFF);
}

alignas(32) u8 sClearScratch[8 * 8 * 4]; // destination of the clearing copy (GPU only)

void clearEfbColour()
{
	// A copy with clear clears the EFB colour (alpha and Z only if their update flags
	// are on; both counter passes turn them off).
	u32 lw, lh;
	logicalSize(lw, lh);
	const GXColor black = { 0, 0, 0, 0 };
	GXSetCopyClear(black, sCopy.clearZ);
	GXSetTexCopySrc(0, 0, (u16)lw, (u16)lh);
	GXSetTexCopyDst(8, 8, GX_TF_RGBA8, GX_FALSE);
	GXCopyTex(sClearScratch, GX_TRUE);
	// restore the game's copy state
	GXSetCopyClear(sCopy.clearColor, sCopy.clearZ);
	GXSetTexCopySrc(sCopy.srcX, sCopy.srcY, sCopy.srcW, sCopy.srcH);
	GXSetTexCopyDst(sCopy.dstW, sCopy.dstH, sCopy.fmt, sCopy.mipmap);
	++sEpoch;
}

// SMS_PORT_EFB_DEBUG=<frame>: EFB screenshot at each counter's first read from that frame on.
void debugShot(u32 key)
{
	static const char* sDbg = std::getenv("SMS_PORT_EFB_DEBUG");
	static std::map<u32, bool> sDone;
	if (sDbg == nullptr || aurora::gfx::current_frame() < (u32)std::atoi(sDbg) || sDone[key])
		return;
	sDone[key] = true;
	char path[64];
	std::snprintf(path, sizeof(path), "efb_metric_%x.bmp", key);
	port_efb_screenshot_ex(path, true);
	++sEpoch;
}

} // namespace

extern "C" {

// port_main.cpp, game main thread, right before each Aurora frame ends.
void port_efb_end_frame(void)
{
	if (sPresentationOnly)
		return; // results are stored at the end of the next frame the game drew
	endFrame();
	++sGameFrame;
}

// Frame-rate interpolation (w9): call with 1 before recording a presented frame that the game
// logic did not produce (an interpolated frame, re-drawn with interpolated matrices) and with
// 0 before the next game frame. While it is 1:
//  - GXCopyTex still copies on the GPU (the frame needs its screen texture, mirror, ...), but
//    no copy is read back into game memory and copy destinations do not age;
//  - GXPeekZ / GXPeekARGB / GXReadPixMetric return the latest values from game frames and
//    queue no readback; GXClearPixMetric still clears (it changes the picture);
//  - port_efb_end_frame does nothing (readbacks finished meanwhile are stored at the end of
//    the next game frame, so game memory only changes between game frames).
// The game frame clock (copy destination ages, counter stage resets) only counts game frames.
void port_efb_set_presentation_only(int on) { sPresentationOnly = on != 0; }
int port_efb_presentation_only(void) { return sPresentationOnly ? 1 : 0; }

void port_GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht)
{
	sCopy.srcX = left, sCopy.srcY = top, sCopy.srcW = wd, sCopy.srcH = ht;
	GXSetTexCopySrc(left, top, wd, ht);
}

void port_GXSetTexCopyDst(u16 wd, u16 ht, GXTexFmt fmt, GXBool mipmap)
{
	sCopy.dstW = wd, sCopy.dstH = ht, sCopy.fmt = fmt, sCopy.mipmap = mipmap;
	GXSetTexCopyDst(wd, ht, fmt, mipmap);
}

void port_GXSetCopyClear(GXColor color, u32 z)
{
	sCopy.clearColor = color;
	sCopy.clearZ     = z;
	GXSetCopyClear(color, z);
}

void port_GXCopyTex(void* dest, GXBool clear)
{
	if (sLog) {
		static std::map<std::pair<void*, u32>, bool> seen;
		const u32 k = ((u32)sCopy.fmt << 24) ^ ((u32)sCopy.dstW << 12) ^ sCopy.dstH;
		if (!seen[{ dest, k }]) {
			seen[{ dest, k }] = true;
			std::fprintf(stderr, "[port] GXCopyTex %p: src %u,%u %ux%u -> %ux%u fmt 0x%x%s%s\n", dest,
			             sCopy.srcX, sCopy.srcY, sCopy.srcW, sCopy.srcH, sCopy.dstW, sCopy.dstH,
			             (u32)sCopy.fmt, sCopy.mipmap ? " mip" : "", clear ? " clear" : "");
		}
	}
	if (sPresentationOnly) { // interpolated frame: the GPU copy only
		if (sTrace) {
			static std::map<void*, bool> seen;
			if (!seen[dest]) {
				seen[dest] = true;
				std::fprintf(stderr, "[efbtrace] interp copy %p: src %u,%u %ux%u -> %ux%u fmt 0x%x%s (GPU only)\n", dest,
				             sCopy.srcX, sCopy.srcY, sCopy.srcW, sCopy.srcH, sCopy.dstW, sCopy.dstH, (u32)sCopy.fmt,
				             clear ? " clear" : "");
			}
		}
		GXCopyTex(dest, clear);
		++sEpoch;
		return;
	}
	auto [destIt, newDest] = sDests.try_emplace(dest);
	if (newDest) // a new (or reused) destination: readbacks of older copies to it are stale
		destIt->second.discardUpTo = sCopySerial;
	destIt->second.lastCopyFrame = sGameFrame;
	queueCopyReadback(dest);
	GXCopyTex(dest, clear);
	++sEpoch;
}

// w9 (port_interp.cpp): clears colour, alpha and Z of the whole EFB before an extra display
// frame is drawn into an Aurora frame that already holds a logic frame's picture (as the
// start of an Aurora frame does, with the game's copy clear values).
void port_efb_clear_all(void)
{
	u32 lw, lh;
	logicalSize(lw, lh);
	GXSetColorUpdate(GX_TRUE);
	GXSetAlphaUpdate(GX_TRUE);
	GXSetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
	GXSetCopyClear(sCopy.clearColor, sCopy.clearZ);
	GXSetTexCopySrc(0, 0, (u16)lw, (u16)lh);
	GXSetTexCopyDst(8, 8, GX_TF_RGBA8, GX_FALSE);
	GXCopyTex(sClearScratch, GX_TRUE);
	GXSetTexCopySrc(sCopy.srcX, sCopy.srcY, sCopy.srcW, sCopy.srcH);
	GXSetTexCopyDst(sCopy.dstW, sCopy.dstH, sCopy.fmt, sCopy.mipmap);
	++sEpoch;
}

// Replaces Aurora's GXPeekZ (see the top of the file).
// EFB reads (GXPeekARGB, GXPeekZ, GXReadPixMetric, the deterministic results' wait) are
// host-only work: the frame drawn so far is resolved on the GPU and results are waited for,
// tens of ms per frame in a heavy stage on a slow PC (the draw-sync callbacks of Mario's
// occlusion probe, the sun and the pollution counters, every frame). The audio thread
// (priority <= 9) may take the CPU meanwhile, as during the present (sdk_os_thread.cpp
// port_preemptible_begin): holding it starved the audio thread, whose every sub-frame needs
// it once, so the AI DMA buffers came late and replayed (stutter, static, drop-outs).
// SMS_PORT_EFB_HOLD=1: the old behaviour (tests).
int port_efb_host_begin(void)
{
	static const bool sHold = std::getenv("SMS_PORT_EFB_HOLD") != nullptr;
	return sHold ? 0 : port_preemptible_begin();
}

void peekZ(u16 x, u16 y, u32* z);
void GXPeekZ(u16 x, u16 y, u32* z)
{
	const int pre = port_efb_host_begin();
	peekZ(x, y, z);
	port_preemptible_end(pre);
}

void peekZ(u16 x, u16 y, u32* z)
{
	const u32 key = peekKey();
	aurora::gfx::ResolvedTargets t;
	if (!sPresentationOnly && !keyBusy(peekBusyKey(key)) && snapshot(true, t, true)) {
		u32 lw, lh;
		logicalSize(lw, lh);
		Job* j       = new Job {};
		j->kind      = JobKind::PeekDepth;
		j->tex       = t.depthTexture;
		u32 tx       = (u32)((x + 0.5f) * t.width / lw);
		u32 ty       = (u32)((y + 0.5f) * t.height / lh);
		j->x         = tx < t.width ? tx : t.width - 1;
		j->y         = ty < t.height ? ty : t.height - 1;
		j->readBytes = 256;
		j->key       = key;
		j->busyKey   = peekBusyKey(key);
		if (!pushJob(j))
			delete j;
		else
			sSnap.epoch = sEpoch; // a texel copy does not change the EFB: keep sharing the snapshot
	}
	u32 v = 0;
	{
		std::lock_guard<std::mutex> lock(sMutex);
		auto it = sPeekDepth.find(key);
		if (it != sPeekDepth.end())
			v = it->second;
	}
	if (sTrace && (sPresentationOnly || (key & 0xFF) == 0))
		std::fprintf(stderr, "[efbtrace] f=%u peekz 0x%x = 0x%06x%s\n", sGameFrame, key, v,
		             sPresentationOnly ? " (IN AN INTERPOLATED FRAME)" : "");
	if (z != nullptr)
		*z = v;
}

void GXClearPixMetric(void) { clearEfbColour(); }

void readPixMetric(u32* top_pixels_in, u32* top_pixels_out, u32* bot_pixels_in, u32* bot_pixels_out,
                   u32* clr_pixels_in, u32* copy_clks);
void GXReadPixMetric(u32* top_pixels_in, u32* top_pixels_out, u32* bot_pixels_in,
                     u32* bot_pixels_out, u32* clr_pixels_in, u32* copy_clks)
{
	const int pre = port_efb_host_begin();
	readPixMetric(top_pixels_in, top_pixels_out, bot_pixels_in, bot_pixels_out, clr_pixels_in, copy_clks);
	port_preemptible_end(pre);
}

void readPixMetric(u32* top_pixels_in, u32* top_pixels_out, u32* bot_pixels_in, u32* bot_pixels_out,
                   u32* clr_pixels_in, u32* copy_clks)
{
	const u32 key = GXReadDrawSync(); // called from the counter's draw-sync callback
	debugShot(key);
	// Tokens are reused by every stage: a counter not read for a while (stage change) starts
	// over, so a new stage never sees the previous stage's count.
	if (!sPresentationOnly) {
		const u32 now = sGameFrame;
		auto last     = sMetricLastRead.find(key);
		if (last != sMetricLastRead.end() && now - last->second > 10) {
			std::lock_guard<std::mutex> lock(sMutex);
			sMetric.erase(key);
			sZeros.erase(key);
			sSeenNonZero.erase(key);
			sMetricEpoch[key] = now; // results of reads before this frame are dropped
		}
		sMetricLastRead[key] = now;
	}
	aurora::gfx::ResolvedTargets t;
	if (!sPresentationOnly && !keyBusy(metricBusyKey(key)) && snapshot(false, t, false, true)) { // w6: sample 0 with MSAA; w8: per-key cap
		u32 lw, lh;
		logicalSize(lw, lh);
		Job* j       = new Job {};
		j->kind      = JobKind::Metric;
		j->tex       = t.colorTexture;
		j->format    = t.colorFormat;
		j->texW      = t.width;
		j->texH      = t.height;
		j->readBytes = 256;
		j->key       = key;
		j->busyKey   = metricBusyKey(key);
		j->dstW      = lw; // w6 (round 5): counted on the logical grid (kCountWGSL)
		j->dstH      = lh;
		j->toLogical = 1.0f;
		j->frame     = sGameFrame;
		if (!pushJob(j))
			delete j;
	}
	u32 n = 0x00FFFFFF; // no result yet: "dirty"
	{
		std::lock_guard<std::mutex> lock(sMutex);
		auto it = sMetric.find(key);
		if (it != sMetric.end())
			n = it->second;
	}
	if (sTrace)
		std::fprintf(stderr, "[efbtrace] f=%u metric 0x%x = %u%s\n", sGameFrame, key, n,
		             sPresentationOnly ? " (IN AN INTERPOLATED FRAME)" : "");
	*top_pixels_in = *top_pixels_out = *bot_pixels_in = *bot_pixels_out = n;
	*clr_pixels_in = n;
	*copy_clks     = 0;
}

} // extern "C"

#endif // SMS_PORT_AURORA
