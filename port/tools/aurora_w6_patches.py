"""w6 round 5: writes port/patches/aurora_{visual_options,msaa_depth,copy_scale}.patch with only
w6's hunks. Each patch's base is the live file with this round's w6 edits reverted (so other
agents' hunks in the same files stay out). Patches apply in the order listed in AURORA.md."""
import difflib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
A = next(p / "port_deps" / "aurora" for p in (ROOT.parent, ROOT.parents[1]) if (p / "port_deps" / "aurora").exists())
OUT = ROOT / "port" / "patches"


def rd(rel):
    return (A / rel).read_bytes().decode("utf-8")


def revert(text, pairs, rel):
    for new, old in pairs:
        n = text.count(new)
        assert n == 1, f"{rel}: {n} matches for {new[:70]!r}"
        text = text.replace(new, old)
    return text


def norm(t):
    return t.replace("\r\n", "\n")


def diff(rel, a, b):
    return "".join(difflib.unified_diff(norm(a).splitlines(True), norm(b).splitlines(True), f"a/{rel}", f"b/{rel}"))


# ---------------------------------------------------------------- copy_scale (GXFrameBuffer)
GXFB = "lib/dolphin/gx/GXFrameBuffer.cpp"
copy_scale_pairs = [
    ("""// SMS PC port patch (aurora_copy_scale.patch): the copy source rectangle is mapped with rounded
""", None),
]
cur = rd(GXFB)
start = cur.index("// SMS PC port patch (aurora_copy_scale.patch): the copy source rectangle")
end = cur.index("void copy_tex(const void* dest, GXBool clear) noexcept {")
map_copy = cur[start:end]
copy_tex_new = """  const auto rect = map_copy_rect(g_gxState.texCopySrc);
  auto [dstWidth, dstHeight] = scale_copy_dst(g_gxState.texCopyDstWidth, g_gxState.texCopyDstHeight);
  // (aurora_copy_scale.patch) 1:1 copies: exactly the source rectangle.
  if (g_gxState.texCopyDstWidth == static_cast<u32>(g_gxState.texCopySrc.width) && rect.width > 0) {
    dstWidth = static_cast<u32>(rect.width);
  }
  if (g_gxState.texCopyDstHeight == static_cast<u32>(g_gxState.texCopySrc.height) && rect.height > 0) {
    dstHeight = static_cast<u32>(rect.height);
  }
"""
copy_tex_old = """  const auto rect = map_logical_scissor(g_gxState.texCopySrc);
  const auto [dstWidth, dstHeight] = scale_copy_dst(g_gxState.texCopyDstWidth, g_gxState.texCopyDstHeight);
"""
vis_gxfb_pairs = [
    ("#include <algorithm>\n#include <atomic>\n", "#include <algorithm>\n"),
    ("""// SMS PC port patch (aurora_visual_options.patch): optional "no copy filter" (sharper
// screen-texture effects; the GameCube applies the vertical filter). Default off.
static std::atomic<bool> g_copyFilterDisabled{false};

""", ""),
    ("""                         gx::is_depth_format(texCopyFmt) || g_copyFilterDisabled.load(std::memory_order_relaxed)
                             ? 0u
                             : g_gxState.copyVFilter,
                         lineHeight);""",
     """                         gx::is_depth_format(texCopyFmt) ? 0u : g_gxState.copyVFilter, lineHeight);"""),
    ("""extern "C" {
// SMS PC port patch (aurora_visual_options.patch); declared in aurora/aurora.h
void aurora_set_copy_filter_enabled(bool enabled) {
  aurora::gx::g_copyFilterDisabled.store(!enabled, std::memory_order_relaxed);
}
""", 'extern "C" {'),
]
cur = cur.replace("\r\n", "\n")
mid = revert(cur, [(a.replace("\r\n", "\n"), b) for a, b in vis_gxfb_pairs], GXFB)  # = base + copy_scale
base = revert(mid, [(map_copy.replace("\r\n", "\n"), ""), (copy_tex_new, copy_tex_old)], GXFB)
snap = Path("/nonexistent")
if snap.exists():
    print("GXFrameBuffer base == pre-edit snapshot:", norm(snap.read_text(encoding="utf-8")) == base)
copy_scale = diff(GXFB, base, mid)
vis = [diff(GXFB, mid, cur)]

# ---------------------------------------------------------------- visual_options
# texture.cpp and aurora.h: pristine copies in port/patches/orig (w6 saved them before editing).
ORIG = OUT / "orig"
for rel in ("lib/gfx/texture.cpp", "include/aurora/aurora.h"):
    vis.insert(0, diff(rel, (ORIG / rel).read_text(encoding="utf-8"), rd(rel)))
GPU = "lib/webgpu/gpu.cpp"
cur = norm(rd(GPU))
gpu_pairs = [
    ("""  // SMS PC port patch (aurora_visual_options.patch): optional post-processing in the present
  // resample pass. Defaults leave the picture untouched (the shader skips them).
  float gamma = 1.f;
  float brightness = 1.f;
  uint32_t fxaa = 0;
  uint32_t pad2 = 0;
};

// SMS PC port patch (aurora_visual_options.patch): set by aurora_set_post_process().
std::atomic<float> g_postGamma{1.f};
std::atomic<float> g_postBrightness{1.f};
std::atomic<bool> g_postFxaa{false};
""", "};\n"),
    ("""    content_scale: vec2<f32>,
    gamma: f32,
    brightness: f32,
    fxaa: u32,
""", "    content_scale: vec2<f32>,\n"),
]
s = cur.index("// SMS PC port patch (aurora_visual_options.patch): FXAA")
e = cur.index("@fragment\nfn fs_main(in: VertexOutput)")
gpu_pairs.append((cur[s:e], ""))
gpu_pairs += [
    ("""    var rgb = color.rgb;
    if (uniforms.fxaa != 0u) {
        rgb = fxaa(in.uv * uniforms.content_scale);
    }
    if (uniforms.brightness != 1.0) {
        rgb = clamp(rgb * uniforms.brightness, vec3<f32>(0.0), vec3<f32>(1.0));
    }
    if (uniforms.gamma != 1.0) {
        rgb = pow(rgb, vec3<f32>(1.0 / uniforms.gamma));
    }
    return vec4(rgb, 1.0);""", "    return vec4(color.rgb, 1.0);"),
    ("""      .gamma = g_postGamma.load(std::memory_order_relaxed),           // aurora_visual_options.patch
      .brightness = g_postBrightness.load(std::memory_order_relaxed), // aurora_visual_options.patch
      .fxaa = g_postFxaa.load(std::memory_order_relaxed) ? 1u : 0u,   // aurora_visual_options.patch
""", ""),
    ("""// SMS PC port patch (aurora_visual_options.patch)
void aurora_set_post_process(float gamma, float brightness, bool fxaa) {
  aurora::webgpu::g_postGamma.store(gamma > 0.f ? gamma : 1.f, std::memory_order_relaxed);
  aurora::webgpu::g_postBrightness.store(brightness > 0.f ? brightness : 1.f, std::memory_order_relaxed);
  aurora::webgpu::g_postFxaa.store(fxaa, std::memory_order_relaxed);
}

""", ""),
]
vis.append(diff(GPU, revert(cur, gpu_pairs, GPU), cur))

# ---------------------------------------------------------------- msaa_depth
msaa = []
TCC = "lib/gfx/tex_copy_conv.cpp"
cur = norm(rd(TCC))
RETRY_LINE = "  return create_render_pipeline_retry(pipelineDescriptor); // aurora_msaa_depth.patch (was CreateRenderPipeline)\n"
assert cur.count(RETRY_LINE) == 2, "retry lines"
base = cur.replace(RETRY_LINE, "  return g_device.CreateRenderPipeline(&pipelineDescriptor);\n")
s = base.index("    // SMS PC port patch (aurora_msaa_depth.patch): with MSAA the EFB depth is\n")
e = base.index("    g_depthSnapshotBindGroupLayout = create_depth_snapshot_layout(false")
s2 = base.index("// SMS PC port patch (aurora_msaa_depth.patch): D3D12 on AMD can fail")
e2 = base.index("bool snapshot_depth_supported() noexcept")
tcc_pairs = [
    (base[s:e], ""),
    (base[s2:e2], ""),
    ("""// SMS PC port patch (aurora_msaa_depth.patch): depth copies from a multisampled EFB.
static wgpu::BindGroupLayout g_depthBindGroupLayoutMS;
static absl::flat_hash_map<GXTexFmt, wgpu::RenderPipeline> g_depthPipelinesMS;
static wgpu::BindGroupLayout g_colorSample0Layout; // sample-0 colour snapshot
static wgpu::ComputePipeline g_colorSample0Compute;
static wgpu::Buffer g_colorSample0Buffer;
static wgpu::RenderPipeline create_render_pipeline_retry(const wgpu::RenderPipelineDescriptor& descriptor);
""", ""),
    ("""
#include <chrono> // aurora_msaa_depth.patch
#include <cstring>
#include <thread>
""", ""),
    ("""  g_depthBindGroupLayoutMS = {};
  g_depthPipelinesMS.clear();
  g_colorSample0Compute = {};
  g_colorSample0Buffer = {};
  g_colorSample0Layout = {};
""", ""),
    ("""        .layout = req.srcSamples > 1 ? g_depthBindGroupLayoutMS : g_depthBindGroupLayout, // aurora_msaa_depth.patch""",
     """        .layout = g_depthBindGroupLayout,"""),
    ("""  if (req.srcSamples > 1 && gx::is_depth_format(req.fmt)) { // aurora_msaa_depth.patch
    const auto ms = g_depthPipelinesMS.find(req.fmt);
    if (ms == g_depthPipelinesMS.end()) {
      Log.fatal("No multisampled depth copy pipeline for format {}", static_cast<int>(req.fmt));
    }
    execute(cmd, req, ms->second);
    return;
  }
""", ""),
]
msaa.append(diff(TCC, revert(base, tcc_pairs, TCC), cur))
TCH = "lib/gfx/tex_copy_conv.hpp"
cur = norm(rd(TCH))
msaa.append(diff(TCH, revert(cur, [
    ("  uint32_t srcSamples = 1; // SMS PC port patch (aurora_msaa_depth.patch): depth source sample count\n", ""),
    ("""// SMS PC port patch (aurora_msaa_depth.patch): copies sample 0 of a multisampled colour view
// into a single-sample (RGBA8/BGRA8) texture of the same format and size.
void snapshot_color_sample0(const wgpu::CommandEncoder& cmd, const wgpu::TextureView& srcColor,
                            const wgpu::Texture& dst, wgpu::TextureFormat format, uint32_t width,
                            uint32_t height);

""", ""),
], TCH), cur))
ENC = "lib/gfx/encoding.cpp"
cur = norm(rd(ENC))
msaa.append(diff(ENC, revert(cur, [
    ("""    // SMS PC port patch (aurora_msaa_depth.patch): depth copies from a multisampled EFB
    // use the multisampled depth conversion pipelines (tex_copy_conv::initialize).
""", """    if (isDepth && passInfo.msaaSamples > 1) {
      Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
    }
"""),
    ("        .srcSamples = isDepth ? passInfo.msaaSamples : 1u,\n", ""),
    ("""  if (passInfo.snapshotColorDst && passInfo.snapshotColorSample0) {
    // SMS PC port patch (aurora_msaa_depth.patch): sample 0 of the multisampled scene colour.
    const auto& scene = passInfo.colorAttachments[SceneColorAttachmentIndex];
    tex_copy_conv::snapshot_color_sample0(cmd, scene.view, passInfo.snapshotColorDst, scene.format,
                                          scene.size.width, scene.size.height);
  } else if (passInfo.snapshotColorDst) {""", "  if (passInfo.snapshotColorDst) {"),
], ENC), cur))
for rel, pairs in (
    ("include/aurora/gfx.hpp", [("""  // SMS PC port patch (aurora_msaa_depth.patch): with a multisampled EFB, the colour snapshot
  // is sample 0 of each pixel instead of the resolve (one sample per pixel, as the GameCube's
  // pixel counters and copies see it). No effect without MSAA.
  bool colorSample0 = false;
""", "")]),
    ("lib/gfx/frame_packet.hpp", [("  bool snapshotColorSample0 = false; // SMS PC port patch (aurora_msaa_depth.patch)\n", "")]),
    ("lib/gfx/recording.cpp", [(
        "      prevPass.snapshotColorSample0 = desc.colorSample0 && prevPass.msaaSamples > 1; // aurora_msaa_depth.patch\n", "")]),
):
    cur = norm(rd(rel))
    msaa.append(diff(rel, revert(cur, pairs, rel), cur))
DP = "lib/gfx/depth_peek.cpp"
msaa.append(diff(DP, (ORIG / DP).read_text(encoding="utf-8"), rd(DP)))

# ---------------------------------------------------------------- efb_floor (w6 round 6, BOOTLOG R23)
floor = []
WIN = "lib/window.cpp"
cur = norm(rd(WIN))
s = cur.index("  // SMS PC port patch (aurora_efb_floor.patch): the EFB is never rendered below")
e = cur.index("  const float scale = SDL_GetWindowDisplayScale(g_window);")
floor.append(diff(WIN, revert(cur, [(cur[s:e], "\n")], WIN), cur))
cur = norm(rd(GPU))
s = cur.index("  // SMS PC port patch (aurora_efb_floor.patch): the render target's size is rounded")
e = cur.index("  return {\n      .left = static_cast<float>((surface_width - viewport_width) / 2),")
floor.append(diff(GPU, revert(cur, [(cur[s:e], "\n")], GPU), cur))

for name, parts in (("visual_options", vis), ("msaa_depth", msaa), ("copy_scale", [copy_scale]),
                    ("efb_floor", floor)):
    text = "".join(parts)
    (OUT / f"aurora_{name}.patch").write_text(text, encoding="utf-8", newline="\n")
    print(name, len(text.splitlines()), "lines")

