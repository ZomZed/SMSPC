"""Build the PC port against Aurora (x86_64, GX/VI/PAD/OS from Aurora).

    python port/tools/build_aurora.py            # configure + build + link
    python port/tools/build_aurora.py --stubs    # also regenerate the stub file for every
                                                 # still-undefined symbol, then relink
    python port/tools/build_aurora.py -j 12 --run   # build, then start the exe

Output: port/build_aurora/sms_port.exe (+ the Dawn/SDL DLLs copied next to it).

How it works
- Unit list: census.units() (every game + JSystem unit of configure.py, minus the
  SDK/MSL/TRK/THP libraries the PC build replaces).
- Game units are compiled with the VS clang for x86_64-pc-windows-msvc, with Aurora's
  include/ directory placed BEFORE libs/dolphin/include, so <dolphin/gx.h> & co.
  resolve to Aurora's declarations. AURORA and TARGET_PC are defined like Aurora's own
  targets do (aurora/include/dolphin/types.h keys u32 = uint32_t etc. off TARGET_PC).
- port/src/*.c* is compiled too, except the files in PORT_SRC_SKIP (the i686 stub file
  and files whose functions Aurora already provides). Aurora-only port files
  (port_main.cpp, ...) are guarded by SMS_PORT_AURORA, which only this script defines.
- src/main.cpp is compiled with -Dmain=sms_game_main; port/src/port_main.cpp defines
  aurora_main() (called by Aurora's aurora_main.lib main()) which initialises Aurora and
  then calls sms_game_main().
- Aurora libraries: the ones already built in port_deps/aurora/build are linked as-is
  (aurora_core/gx/vi/main + everything the `simple` example links: Dawn, SDL3, absl,
  fmt, imgui, ...). Components that were never built there (aurora_os, pad, si, mtx, gd,
  card, thp, ms) are compiled by this script from the exact command lines in Aurora's
  build.ninja (same cl.exe, same flags) into port/build_aurora/aurora/<target>.lib, so
  nothing is written into port_deps.
- Units that fail to compile are left out of the link (listed in compile_failures.txt);
  their symbols then show up as undefined and --stubs turns them into logging stubs too.
- Link: lld-link inside a vcvars64 environment. Undefined symbols go to
  port/build_aurora/undefined.txt; --stubs writes port/build_aurora/gen/stubs_aurora.cpp
  (functions log once via port_missing_stub() and return 0; MSVC-mangled data symbols
  get 256 zero bytes in .bss).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import census  # noqa: E402
import sjis_mirror  # noqa: E402

ROOT = census.ROOT
# --asan / --ubsan (w10, BOOTLOG "Sanitizers (w10)"): sanitizer builds of the game and
# JSystem units, each in its own output directory so they never invalidate the
# normal build (port/build_aurora_asan, _ubsan, _asan_ubsan).
SANITIZE = [s for s in ("asan", "ubsan") if f"--{s}" in sys.argv[1:]]


# --region us|jp|eu|all (w13, BOOTLOG "Regional behaviour (w13)"): the game code compiled for
# one disc region (that region's VERSION_ defines, as configure.py sets them for its DOL), in
# its own output directory and exe. Each exe hands the disc over to the right sibling exe at
# startup (port_disc.cpp), so a package ships all three. 'all' builds jp, eu, then us.
def _arg_region():
    args = sys.argv[1:]
    for i, s in enumerate(args):
        if s == "--region" and i + 1 < len(args):
            return args[i + 1]
        if s.startswith("--region="):
            return s.split("=", 1)[1]
    return "us"


REGION = _arg_region()
REGION_DEFINES = {"us": ["BUILD_VERSION=2", "VERSION_GMSE01", "VERSION_GMSP01"],
                  "jp": ["BUILD_VERSION=0", "VERSION_GMSJ01"],
                  "eu": ["BUILD_VERSION=1", "VERSION_GMSP01"]}
REGION_EXE = {"us": "sms_port.exe", "jp": "sms_port_jp.exe", "eu": "sms_port_eu.exe"}
BUILD_REGION = "us" if REGION == "all" else REGION
if BUILD_REGION not in REGION_DEFINES:
    sys.exit(f"--region: unknown region {REGION!r} (us, jp, eu or all)")
EXE_NAME = REGION_EXE[BUILD_REGION]
GAME_DEFINES = ([d for d in census.DEFINES if not d.startswith(("BUILD_VERSION", "VERSION_"))]
                + REGION_DEFINES[BUILD_REGION])
OUT = ROOT / "port" / ("build_aurora" + ("" if BUILD_REGION == "us" else "_" + BUILD_REGION)
                       + "".join("_" + s for s in SANITIZE))
SJIS = OUT / "sjis"  # Shift-JIS literal copies of sources/headers
MIRRORED = set()
LLVM = census.VS_LLVM
CLANG = str(LLVM / "clang.exe")
LLD = str(LLVM / "lld-link.exe")
LLVM_LIB = str(LLVM / "llvm-lib.exe")
NINJA = [sys.executable, "-m", "ninja"]

def _find_aurora():
    # main tree: <top>/sms-main.../  ; workers: <top>/workers/wN/
    for up in (ROOT.parent, ROOT.parents[1]):
        p = up / "port_deps" / "aurora"
        if p.exists():
            return p
    return ROOT.parent / "port_deps" / "aurora"


AURORA = Path(os.environ.get("SMS_AURORA_DIR", str(_find_aurora()))).resolve()
AURORA_BUILD = AURORA / "build"
# bzip2 1.0.8 library sources next to Aurora in port_deps (for WIA/RVZ images compressed
# with bzip2; port_disc_wia.cpp). Compiled into the exe; see port_deps/bzip2-1.0.8/SOURCE.txt.
BZIP2_DIR = AURORA.parent / "bzip2-1.0.8"
BZIP2_SOURCES = ["blocksort.c", "huffman.c", "crctable.c", "randtable.c", "compress.c", "decompress.c",
                 "bzlib.c"]
# liblzma (xz 5.8.4, unmodified release source, see port_deps/xz-5.8.4/SOURCE.txt) for WIA/RVZ
# images compressed with LZMA/LZMA2: only the raw LZMA1/LZMA2 decoder is compiled, with the
# configuration given as defines below (no configure/cmake/scripts from the tree are run).
XZ_DIR = AURORA.parent / "xz-5.8.4"
XZ_SOURCES = ["common/common.c", "common/filter_common.c", "common/filter_decoder.c", "lz/lz_decoder.c",
              "lzma/lzma_decoder.c", "lzma/lzma2_decoder.c"]
XZ_INCLUDES = ["src/liblzma/" + d for d in ("api", "common", "lz", "lzma", "rangecoder", "check", "simple",
                                            "delta")] + ["src/common"]
XZ_DEFINES = ["HAVE_STDINT_H", "HAVE_STDBOOL_H", "HAVE_INTTYPES_H", "HAVE_DECODER_LZMA1", "HAVE_DECODER_LZMA2",
              "LZMA_API_STATIC", "NDEBUG"]
# libopus 1.6.1 (BSD, unmodified release source, see port_deps/opus-1.6.1/SOURCE.txt) for the
# DualSense speaker over Bluetooth (port_speaker.cpp, w9): only the float encoder is compiled,
# configured with the defines below (no configure/cmake/scripts from the tree are run).
OPUS_DIR = AURORA.parent / "opus-1.6.1"
OPUS_SOURCES = (["src/opus.c", "src/opus_encoder.c", "src/extensions.c", "src/repacketizer.c", "src/analysis.c",
                 "src/mlp.c", "src/mlp_data.c"]
                + ["celt/" + s + ".c" for s in ("bands", "celt", "celt_encoder", "cwrs", "entcode", "entdec", "entenc",
                                                 "kiss_fft", "laplace", "mathops", "mdct", "modes", "pitch",
                                                 "celt_lpc", "quant_bands", "rate", "vq")]
                + ["silk/" + s + ".c" for s in (
                    "code_signs", "enc_API", "encode_indices", "encode_pulses", "gain_quant", "interpolate",
                    "LP_variable_cutoff", "NLSF_decode", "NSQ", "NSQ_del_dec", "shell_coder", "tables_gain",
                    "tables_LTP", "tables_NLSF_CB_NB_MB", "tables_NLSF_CB_WB", "tables_other", "tables_pitch_lag",
                    "tables_pulses_per_block", "VAD", "control_audio_bandwidth", "quant_LTP_gains", "VQ_WMat_EC",
                    "HP_variable_cutoff", "NLSF_encode", "NLSF_VQ", "NLSF_unpack", "NLSF_del_dec_quant",
                    "process_NLSFs", "stereo_LR_to_MS", "check_control_input", "control_SNR", "init_encoder",
                    "control_codec", "A2NLSF", "ana_filt_bank_1", "biquad_alt", "bwexpander_32", "bwexpander",
                    "debug", "decode_pitch", "inner_prod_aligned", "lin2log", "log2lin", "LPC_analysis_filter",
                    "LPC_inv_pred_gain", "table_LSF_cos", "NLSF2A", "NLSF_stabilize", "NLSF_VQ_weights_laroia",
                    "pitch_est_tables", "resampler", "resampler_down2_3", "resampler_down2",
                    "resampler_private_AR2", "resampler_private_down_FIR", "resampler_private_IIR_FIR",
                    "resampler_private_up2_HQ", "resampler_rom", "sigm_Q15", "sort", "sum_sqr_shift",
                    "stereo_encode_pred", "stereo_find_predictor", "stereo_quant_pred", "LPC_fit")]
                + ["silk/float/" + s + ".c" for s in (
                    "apply_sine_window_FLP", "corrMatrix_FLP", "encode_frame_FLP", "find_LPC_FLP", "find_LTP_FLP",
                    "find_pitch_lags_FLP", "find_pred_coefs_FLP", "LPC_analysis_filter_FLP",
                    "LTP_analysis_filter_FLP", "LTP_scale_ctrl_FLP", "noise_shape_analysis_FLP", "process_gains_FLP",
                    "regularize_correlations_FLP", "residual_energy_FLP", "warped_autocorrelation_FLP",
                    "wrappers_FLP", "autocorrelation_FLP", "burg_modified_FLP", "bwexpander_FLP", "energy_FLP",
                    "inner_product_FLP", "k2a_FLP", "LPC_inv_pred_gain_FLP", "pitch_analysis_core_FLP",
                    "scale_copy_vector_FLP", "scale_vector_FLP", "schur_FLP", "sort_FLP")])
OPUS_DEFINES = ["OPUS_BUILD", "USE_ALLOCA", "HAVE_LRINTF", "NDEBUG", "_CRT_SECURE_NO_WARNINGS"]
OPUS_INCLUDES = ["include", "celt", "silk", "silk/float", "src"]
VCVARS = Path(r"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat")
VSWHERE_DIR = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer"

# Aurora components that the port needs on top of what the `simple` example links.
# Taken from port_deps/aurora/build if built there, otherwise compiled by this script.
AURORA_EXTRA = ["aurora_os", "aurora_pad", "aurora_si", "aurora_mtx", "aurora_gd",
                "aurora_card", "aurora_thp", "aurora_ms"]

# port/src files not compiled into the Aurora build (see port/AURORA.md, "SDK split").
PORT_SRC_SKIP = {
    "stubs_auto.cpp",       # build_port.py's generated stubs (separate symbol set / i686 asm)
    "sdk_vi_pad_card.cpp",  # VI: Aurora vi + port_main.cpp; PAD and CARD: Aurora
    "sdk_misc.c",           # sinf/cosf: the x64 UCRT exports them; __GDCurrentDL: Aurora gd
}
# port/src files compiled without the force-included port_compat.h (they include
# Aurora/SDL headers and no game headers).
PORT_SRC_NO_COMPAT = {"port_main.cpp"}
# port/src files that include Dawn's C++ API (aurora/gfx.hpp), which needs C++20.
PORT_SRC_CXX20 = {"port_aurora_sdk.cpp", "port_efb.cpp"}
# w5's host SDK files written against our libs/dolphin/include declarations (long vs
# s32, non-const char*, ...). They are compiled with our SDK headers instead of
# Aurora's; only the function signatures differ (extern "C", same ABI on x64). The
# struct layouts they touch were checked to be identical in both header sets
# (OSThread: Aurora's adds error/specific[2] at the end, past everything used here).
# Not sdk_audio.cpp: ARQRequest differs (40 vs 56 bytes), so it must use Aurora's arq.h.
PORT_SRC_SDK_HEADERS = {"sdk_os_thread.cpp", "sdk_dvd.cpp", "sdk_osalloc.cpp"}

# Extra per-unit defines.
UNIT_DEFINES = {
    # The game's own main() becomes sms_game_main(); port/src/port_main.cpp calls it.
    "src/main.cpp": ["main=sms_game_main"],
    # include/fake_tgmath.h defines sqrtf/sqrt/fabs as C `extern inline`. CodeWarrior
    # (and gnu89) treat that as inline-only; clang's MSVC target always emits an
    # external definition, which clashes with the UCRT import of sqrtf. Give this
    # unit's copies private names (calls in the unit still use the game's versions).
    "src/GC2D/hx_wiper.c": ["sqrtf=hx_wiper_sqrtf", "sqrt=hx_wiper_sqrt",
                            "fabs=hx_wiper_fabs", "floor=hx_wiper_floor"],
}

TARGET = "--target=x86_64-pc-windows-msvc"
# w14: optimisation level of the game/JSystem/port units. -O2 (was -O1): about 5-10%
# less CPU in load work (Yaz0, setupObjects), per-frame game time within noise; same
# clean-build time (~50 s at -j 16). SMS_PORT_OPT overrides it for experiments (ninja
# rebuilds everything when it changes). No -march: the x86-64 baseline (SSE2) keeps the
# exe portable, and -march=x86-64-v3 would let clang fuse float multiply-adds (FMA),
# which changes float results. ThinLTO is not usable as is: the llvm-objcopy
# --redefine-syms pass below cannot rewrite LLVM bitcode objects.
OPT = os.environ.get("SMS_PORT_OPT", "-O2")

AURORA_DEFINES = ["AURORA", "TARGET_PC", "SMS_PORT_AURORA"]


def includes(aurora_headers=True):
    inc = list(census.INCLUDES)
    if not aurora_headers:
        return inc
    pos = inc.index("libs/dolphin/include")
    # port/include_aurora: shims + port_aurora.h; then Aurora's SDK headers; then ours
    # (still reached for headers Aurora doesn't have, e.g. dolphin/gd/GDVert.h).
    inc[pos:pos] = ["port/include_aurora", (AURORA / "include").as_posix()]
    return inc


def flags(lang_c, compat=True, extra_inc=(), aurora_headers=True):
    f = [TARGET, "-c", OPT, "-g", "-gcodeview", "-fno-exceptions", "-fno-rtti",
         "-fsigned-char", "-w", "-Wno-everything", "-fms-runtime-lib=dll"]
    if compat:
        f += ["-include", "port/include/port_compat.h"]
        if aurora_headers:
            f += ["-include", "port/include_aurora/port_aurora.h"]
    f += ["-x", "c", "-std=gnu99"] if lang_c else ["-x", "c++", "-std=gnu++17"]
    for i in census.INCLUDES:  # Shift-JIS literal copies of headers first (sjis_mirror.py)
        f += ["-I", (SJIS / i).relative_to(ROOT).as_posix()]
    for i in includes(aurora_headers):
        f += ["-I", i]
    for i in extra_inc:
        f += ["-I", i]
    for d in GAME_DEFINES + (AURORA_DEFINES if aurora_headers else ["SMS_PORT_AURORA"]):  # w13: region
        f += ["-D", d]
    return f


def q(s):
    return s.replace("$", "$$").replace(" ", "$ ").replace(":", "$:")


# ---------------------------------------------------------------------------
# MSVC environment (cl.exe / lib paths for lld-link)

def msvc_env():
    cache = OUT / "msvc_env.json"
    if cache.exists():
        return json.loads(cache.read_text())
    env = dict(os.environ)
    env["PATH"] = VSWHERE_DIR + ";" + env.get("PATH", "")
    p = subprocess.run(f'cmd /s /c ""{VCVARS}" >nul && set"', capture_output=True, text=True,
                       env=env, shell=False)
    out = {}
    for line in p.stdout.splitlines():
        k, sep, v = line.partition("=")
        if sep:
            out[k] = v
    if "LIB" not in out:
        sys.exit("vcvars64 failed:\n" + p.stdout + p.stderr)
    OUT.mkdir(parents=True, exist_ok=True)
    cache.write_text(json.dumps(out))
    return out


# ---------------------------------------------------------------------------
# Aurora's build.ninja

def parse_aurora_ninja():
    """Returns (objects, simple_link_libs). objects: list of dict(target, obj, src, vars, rule)."""
    text = (AURORA_BUILD / "build.ninja").read_text(encoding="utf-8", errors="replace")
    objs, libs = [], []
    cur = None
    for line in text.splitlines():
        if line.startswith("build "):
            cur = None
            m = re.match(r"build (CMakeFiles\\(aurora_\w+)\.dir\\\S+\.obj): (\S+) (\S+)", line)
            if m and m.group(2) in AURORA_EXTRA:
                cur = dict(target=m.group(2), obj=m.group(1), rule=m.group(3),
                           src=m.group(4).replace("$:", ":"), vars={})
                objs.append(cur)
            elif line.startswith("build examples\\simple.exe:"):
                cur = {"simple": True, "vars": {}}
        elif cur is not None and line.startswith("  ") and " = " in line:
            k, _, v = line.strip().partition(" = ")
            cur["vars"][k] = v
            if cur.get("simple") and k == "LINK_LIBRARIES":
                libs = v.split()
        elif not line.startswith("  "):
            cur = None
    return objs, libs


def aurora_cl():
    text = (AURORA_BUILD / "build.ninja").read_text(encoding="utf-8", errors="replace")
    rules = AURORA_BUILD / "CMakeFiles" / "rules.ninja"
    rtext = rules.read_text(encoding="utf-8", errors="replace") if rules.exists() else text
    m = re.search(r'"([^"]+\\cl\.exe)"', rtext)
    return m.group(1) if m else "cl.exe"


def aurora_libs():
    """Static/import libraries for the link, in order, plus ninja rules for the
    Aurora components compiled here."""
    objs, simple_libs = parse_aurora_ninja()
    link = []
    for l in simple_libs:
        if l.startswith("-"):
            continue
        p = AURORA_BUILD / l
        link.append(str(p) if p.exists() else l)  # system libs (kernel32.lib, ...) by name
    rules, extra_libs = [], []
    cl = aurora_cl()
    rules.append(f"rule aurora_cxx\n  command = \"{cl}\" /nologo /TP $defines $includes $flags "
                 f"/showIncludes /Fo$out -c $in\n  deps = msvc\n  description = CL $in")
    rules.append(f"rule aurora_c\n  command = \"{cl}\" /nologo $defines $includes $flags "
                 f"/showIncludes /Fo$out -c $in\n  deps = msvc\n  description = CL $in")
    rules.append(f"rule ar\n  command = \"{LLVM_LIB}\" /nologo /out:$out @$out.rsp\n"
                 f"  rspfile = $out.rsp\n  rspfile_content = $in\n  description = LIB $out")
    by_target = {}
    for o in objs:
        by_target.setdefault(o["target"], []).append(o)
    for t in AURORA_EXTRA:
        prebuilt = AURORA_BUILD / f"{t}.lib"
        if prebuilt.exists():
            extra_libs.append(str(prebuilt))
            continue
        if t not in by_target:
            print(f"warning: {t} not in Aurora build.ninja; skipped")
            continue
        outs = []
        for o in by_target[t]:
            v = o["vars"]
            fl = v.get("FLAGS", "").replace("-Zi", "/Z7")
            obj = (OUT / "aurora" / o["obj"].replace("CMakeFiles\\", "")).as_posix()
            rule = "aurora_c" if o["rule"].startswith("C_COMPILER") else "aurora_cxx"
            rules.append(f"build {q(obj)}: {rule} {q(Path(o['src']).as_posix())}\n"
                         f"  defines = {v.get('DEFINES', '')}\n  includes = {v.get('INCLUDES', '')}\n"
                         f"  flags = {fl}")
            outs.append(obj)
        lib = (OUT / "aurora" / f"{t}.lib").as_posix()
        rules.append(f"build {q(lib)}: ar {' '.join(q(x) for x in outs)}")
        extra_libs.append(lib)
    # Extra components first (they reference aurora_core), then the example's list.
    return extra_libs + link, rules, extra_libs


def aurora_dlls():
    d = AURORA_BUILD / "_deps"
    names = []
    for sub in ("dawn_prebuilt-src/bin", "sdl3_prebuilt-src/bin", "png-build", "zlib-build"):
        names += sorted((d / sub).glob("*.dll"))
    return names


# ---------------------------------------------------------------------------

def port_sources():
    out = []
    for p in sorted((ROOT / "port" / "src").glob("*.c*")):
        if p.name in PORT_SRC_SKIP:
            continue
        out.append(p)
    gen = OUT / "gen" / "stubs_aurora.cpp"
    if gen.exists():
        out.append(gen)
    if "ubsan" in SANITIZE and "asan" not in SANITIZE:
        # Microsoft's clang_rt.ubsan_standalone references two ASan "continue on
        # error" helpers that only the ASan runtime defines; provide them.
        shim = OUT / "gen" / "ubsan_coe_shim.cpp"
        shim.parent.mkdir(parents=True, exist_ok=True)
        text = ("// generated by build_aurora.py --ubsan\n#include <cstdio>\n"
                "namespace __coe_win {\nbool ContinueOnError() { return false; }\n"
                "void RawWrite(const char* s) { std::fputs(s, stderr); }\n}\n")
        if not shim.exists() or shim.read_text(encoding="utf-8") != text:
            shim.write_text(text, encoding="utf-8")
        out.append(shim)
    return out


# JSystem's JKRHeap.cpp replaces the global operator new/delete (allocate from the
# current JKRHeap), which is right for the game but not for Aurora/SDL/Dawn/STL code in
# the same exe (they allocate during static init, before any JKRHeap exists, and must
# never land in a game heap). So in game/JSystem objects only, the global operators are
# renamed (definitions in JKRHeap.obj and every reference), and game units are compiled
# without sized/aligned deallocation so they only use these four.
JKR_NEW_SYMS = {
    "??2@YAPEAX_K@Z": "sms_jkr_operator_new",
    "??_U@YAPEAX_K@Z": "sms_jkr_operator_new_array",
    "??3@YAXPEAX@Z": "sms_jkr_operator_delete",
    "??_V@YAXPEAX@Z": "sms_jkr_operator_delete_array",
    # JKRHeap.cpp also defines the sized forms under SMS_PORT; keep them game-only too.
    "??3@YAXPEAX_K@Z": "sms_jkr_operator_delete_sized",
    "??_V@YAXPEAX_K@Z": "sms_jkr_operator_delete_array_sized",
    # Not an operator: game reads of the pads go through port_main.cpp's port_PADRead,
    # which forwards to Aurora's PADRead and can add scripted presses (SMS_PORT_PADSCRIPT).
    "PADRead": "port_PADRead",
    # port_main.cpp: optional synchronous FIFO processing around display lists
    # (SMS_PORT_GXSYNC=1), so an Aurora FIFO error stops in the game call that caused it.
    "GXCallDisplayList": "port_GXCallDisplayList",
    # port_main.cpp: lets SMS_PORT_WATCH_GD arm a watchpoint when a display list is built.
    "GDInitGDLObj": "port_GDInitGDLObj",
    # port_main.cpp (w9): notes that the game copied a frame to the XFB, so VIWaitForRetrace
    # presents only frames the game finished (the game draws at 30 Hz, VI runs at 60 Hz).
    "GXCopyDisp": "port_GXCopyDisp",
    # port_main.cpp (w14): VIWaitForRetrace presents on the retrace after VIFlush, where
    # the GameCube VI flips to the new frame buffer (steady cadence; see port_main.cpp).
    "VIFlush": "port_VIFlush",
    # port_replay.cpp (w9): the GameCube MSL rand/srand (one global LCG; the host CRT's is a
    # different generator with per-thread state), and the two wall-clock-dependent checks the
    # game logic makes, so input recordings replay identically.
    "__imp_rand": "__imp_port_rand",
    "__imp_srand": "__imp_port_srand",
    "OSIsThreadTerminated": "port_OSIsThreadTerminated",
    "THPPlayerGetState": "port_THPPlayerGetState",
    # port_haptics.cpp (w9): rumble off / original / HD (HD replaces the game's motor pulses
    # on controller 1 with designed waveforms; the game itself is unchanged).
    "PADControlMotor": "port_PADControlMotor",
    # port_efb.cpp (w8): record the copy state (CPU readback of EFB copies the game reads,
    # pixel metrics), then call Aurora's.
    "GXSetTexCopySrc": "port_GXSetTexCopySrc",
    "GXSetTexCopyDst": "port_GXSetTexCopyDst",
    "GXSetCopyClear": "port_GXSetCopyClear",
    "GXCopyTex": "port_GXCopyTex",
    # port_aurora_sdk.cpp: records the logical EFB size (for EFB readbacks), then calls Aurora's.
    "VIConfigure": "port_VIConfigure",
    # port_behaviour.cpp (w13): the European build's PAL 50 Hz / EURGB60 format (the game's
    # timing follows it); NTSC for the US and Japanese builds, as Aurora's.
    "VIGetTvFormat": "port_VIGetTvFormat",
}
GAME_ONLY_FLAGS = ["-fno-sized-deallocation", "-fno-aligned-allocation"]
# CodeWarrior semantics the game code relies on (w10 audit, BOOTLOG "Static audit"):
# - -fcheck-new: MWCC null-checks the result of `new` before running the constructor
#   (JKRHeap's operator new returns null when a heap is full; see BOOTLOG 1).
# - -fwrapv: signed overflow wraps on PowerPC; clang would optimise on it (UB).
# - -fno-strict-return: a non-void function that falls off its end returns garbage
#   (stale r3 on GC) instead of being treated as unreachable (BOOTLOG 3, JASDvd).
# - -fno-delete-null-pointer-checks: keep `if (!p)` tests that follow a dereference.
# (Strict aliasing is already off: clang's MSVC target defaults to relaxed aliasing.)
GAME_ONLY_FLAGS += ["-fcheck-new", "-fwrapv", "-fno-strict-return",
                    "-fno-delete-null-pointer-checks"]
# Stack-smash finder (debug only): SMS_PORT_GS_DEBUG=1 builds game/JSystem units with
# -fstack-protector-all and routes their cookie checks to port_crash.cpp's
# sms_gs_check (which prints a backtrace) instead of the CRT's (fast-fail, no trace,
# and MSVC-compiled Aurora code relies on its register-preserving contract).
if os.environ.get("SMS_PORT_GS_DEBUG"):
    GAME_ONLY_FLAGS += ["-fstack-protector-all"]
    JKR_NEW_SYMS["__security_check_cookie"] = "sms_gs_check"
LLVM_OBJCOPY = str(LLVM / "llvm-objcopy.exe")

# Sanitizers (game/JSystem units only; port/ host code and Aurora stay uninstrumented).
# Runtimes: VS18 LLVM ships lib/clang/<ver>/lib/windows/clang_rt.asan_dynamic-x86_64
# (.lib + .dll) and clang_rt.ubsan_standalone(_cxx)-x86_64.lib.
SAN_LIBDIR = next(iter(sorted((LLVM.parent / "lib" / "clang").glob("*/lib/windows"))), None)
SAN_FLAGS = []
if "asan" in SANITIZE:
    # -fsanitize-recover: keep running after a report, so one run lists every bug.
    # SMS_PORT_ASAN: JKRExpHeap/JKRSolidHeap poison their free memory and slack
    # (port/include/port_asan.h); those two units are compiled uninstrumented
    # (NOSAN_UNITS) because they walk their own (poisoned) free-block headers.
    SAN_FLAGS += ["-fsanitize=address", "-fsanitize-recover=address", "-DSMS_PORT_ASAN=1"]
if "ubsan" in SANITIZE:
    # Minus vptr/function (need RTTI), alignment and shift-base (harmless on x86,
    # very noisy on GameCube-style data code). -fwrapv already disables the signed
    # overflow check. Kept: bounds, float-cast-overflow (PPC saturates, x86 gives
    # 0x80000000), shift-exponent (x86 masks the count, PPC does not), null, bool,
    # enum, return, unreachable, ...
    SAN_FLAGS += ["-fsanitize=undefined", "-fno-sanitize=vptr,function,alignment,shift-base",
                  "-DSMS_PORT_UBSAN=1"]
GAME_ONLY_FLAGS += SAN_FLAGS
NOSAN_UNITS = {"libs/JSystem/src/JKernel/JKRExpHeap.cpp", "libs/JSystem/src/JKernel/JKRSolidHeap.cpp"}


def write_ninja(us, aurora_rules):
    sdl_inc = (AURORA_BUILD / "_deps" / "sdl3_prebuilt-src" / "include").as_posix()
    # Dawn's webgpu_cpp.h, for port files using aurora/gfx.hpp (port_aurora_sdk.cpp: EFB readback)
    dawn_inc = (AURORA_BUILD / "_deps" / "dawn_prebuilt-src" / "include").as_posix()
    # Dear ImGui (Aurora links it), for port_menu.cpp / port_window.cpp
    imgui_inc = (AURORA_BUILD / "_deps" / "imgui-src").as_posix()
    # Decompressors for RVZ/WIA/GCZ disc images (port_disc_wia.cpp): zstd and zlib from
    # Aurora's build (already linked), bzip2 compiled below from port_deps.
    codec_inc = [(AURORA_BUILD / "_deps" / "zstd-src" / "lib").as_posix(),
                 (AURORA_BUILD / "_deps" / "zlib-build").as_posix()]
    codec_defines = []
    if BZIP2_DIR.exists():
        codec_inc.append(BZIP2_DIR.as_posix())
    else:
        codec_defines.append("PORT_DISC_NO_BZIP2")
        print(f"note: {BZIP2_DIR} not found; bzip2-compressed WIA/RVZ images will not be readable")
    if XZ_DIR.exists():
        codec_inc.append((XZ_DIR / "src" / "liblzma" / "api").as_posix())
        codec_defines += ["PORT_DISC_HAVE_LZMA", "LZMA_API_STATIC"]
    else:
        print(f"note: {XZ_DIR} not found; LZMA/LZMA2-compressed WIA/RVZ images will not be readable")
    if OPUS_DIR.exists():  # w9: DualSense speaker over Bluetooth
        codec_inc.append((OPUS_DIR / "include").as_posix())
        codec_defines.append("PORT_HAVE_OPUS")
    else:
        print(f"note: {OPUS_DIR} not found; the DualSense speaker works over USB only")
    syms = OUT / "jkr_new_syms.txt"
    OUT.mkdir(parents=True, exist_ok=True)
    syms_text = "".join(f"{k} {v}\n" for k, v in JKR_NEW_SYMS.items())
    if not syms.exists() or syms.read_text(encoding="utf-8") != syms_text:  # keep the mtime otherwise
        syms.write_text(syms_text, encoding="utf-8")
    lines = ["ninja_required_version = 1.3", f"builddir = {OUT.as_posix()}", "",
             f"rule cc\n  command = \"{CLANG}\" $flags $in -o $out -MD -MF $out.d\n"
             f"  depfile = $out.d\n  deps = gcc\n  description = CC $in", "",
             f"rule cc_game\n  command = cmd /s /c \"\"{CLANG}\" $flags $in -o $out -MD -MF $out.d"
             f" && \"{LLVM_OBJCOPY}\" --redefine-syms={syms.as_posix()} $out\"\n"
             f"  depfile = $out.d\n  deps = gcc\n  description = CC $in", ""]
    lines += aurora_rules
    objs = []
    for u in us:
        src = u["src"]
        obj = (OUT / "obj" / (src + ".obj")).as_posix()
        fl = flags(src.endswith(".c") and not src.startswith("libs/JSystem/"))
        if u.get("pch"):
            pch = "include/SMS.pch"
            fl += ["-include", (SJIS / pch).relative_to(ROOT).as_posix() if pch in MIRRORED else pch]
        for d in UNIT_DEFINES.get(src, []):
            fl += ["-D", d]
        fl += GAME_ONLY_FLAGS
        if src in NOSAN_UNITS and "asan" in SANITIZE:
            fl += ["-fno-sanitize=address"]
        inp = (SJIS / src).relative_to(ROOT).as_posix() if src in MIRRORED else src
        # the rename list is an implicit input: objects are rebuilt when it changes
        lines.append(f"build {q(obj)}: cc_game {q(inp)} | {q(syms.as_posix())}\n  flags = {' '.join(fl)}")
        objs.append(obj)
    for p in port_sources():
        try:
            rel = p.relative_to(ROOT).as_posix()
        except ValueError:
            rel = p.as_posix()
        obj = (OUT / "obj" / (p.relative_to(ROOT).as_posix() + ".obj")).as_posix()
        fl = flags(p.suffix == ".c", compat=p.name not in PORT_SRC_NO_COMPAT,
                   extra_inc=[sdl_inc, dawn_inc, imgui_inc, "port/src"] + codec_inc,
                   aurora_headers=p.name not in PORT_SRC_SDK_HEADERS)
        for d in codec_defines:
            fl += ["-D", d]
        if p.name in PORT_SRC_CXX20:  # Dawn's webgpu_cpp.h needs C++20
            fl = ["-std=gnu++20" if f == "-std=gnu++17" else f for f in fl]
        lines.append(f"build {q(obj)}: cc {q(rel)}\n  flags = {' '.join(fl)}")
        objs.append(obj)
    # bzip2 library (third-party C, unmodified; static) for port_disc_wia.cpp.
    if BZIP2_DIR.exists():
        bfl = [TARGET, "-c", "-O2", "-w", "-fms-runtime-lib=dll", "-x", "c", "-std=gnu99",
               "-D_CRT_SECURE_NO_WARNINGS", "-DBZ_EXPORT", "-I", BZIP2_DIR.as_posix()]
        for s in BZIP2_SOURCES:
            obj = (OUT / "obj" / "third_party" / "bzip2" / (s + ".obj")).as_posix()
            lines.append(f"build {q(obj)}: cc {q((BZIP2_DIR / s).as_posix())}\n  flags = {' '.join(bfl)}")
            objs.append(obj)
    # liblzma raw LZMA1/LZMA2 decoder (third-party C, unmodified; static) for port_disc_wia.cpp.
    if XZ_DIR.exists():
        xfl = [TARGET, "-c", "-O2", "-w", "-fms-runtime-lib=dll", "-x", "c", "-std=gnu11"]
        xfl += [f"-D{d}" for d in XZ_DEFINES] + [f"-I{(XZ_DIR / i).as_posix()}" for i in XZ_INCLUDES]
        for s in XZ_SOURCES:
            obj = (OUT / "obj" / "third_party" / "xz" / (s.replace("/", "_") + ".obj")).as_posix()
            lines.append(f"build {q(obj)}: cc {q((XZ_DIR / 'src' / 'liblzma' / s).as_posix())}\n"
                         f"  flags = {' '.join(xfl)}")
            objs.append(obj)
    # libopus float encoder (third-party C, unmodified; static) for port_speaker.cpp (w9).
    if OPUS_DIR.exists():
        ofl = [TARGET, "-c", "-O2", "-w", "-fms-runtime-lib=dll", "-x", "c", "-std=gnu11"]
        ofl += [f"-D{d}" for d in OPUS_DEFINES] + [f"-I{(OPUS_DIR / i).as_posix()}" for i in OPUS_INCLUDES]
        for s in OPUS_SOURCES:
            obj = (OUT / "obj" / "third_party" / "opus" / (s.replace("/", "_") + ".obj")).as_posix()
            lines.append(f"build {q(obj)}: cc {q((OPUS_DIR / s).as_posix())}\n  flags = {' '.join(ofl)}")
            objs.append(obj)
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "build.ninja").write_text("\n".join(lines) + "\n", encoding="utf-8")
    return objs


def compile_icon_res(env):
    """port/res/sms_port.rc (the Shine icon) -> a .res linked into the exe; None if unavailable."""
    rc = ROOT / "port" / "res" / "sms_port.rc"
    tool = LLVM / "llvm-rc.exe"
    if not rc.exists() or not tool.exists():
        return None
    res = OUT / "sms_port.res"
    p = subprocess.run([str(tool), "/nologo", "/fo", str(res), str(rc)], cwd=rc.parent,
                       capture_output=True, text=True, errors="replace", env=env)
    return str(res) if p.returncode == 0 and res.exists() else None


def link(objs, libs, env, allow_multiple):
    exe = OUT / EXE_NAME  # w13: sms_port.exe / sms_port_jp.exe / sms_port_eu.exe
    rsp = OUT / "link.rsp"
    rsp.write_text("\n".join(f'"{o}"' for o in objs + libs), encoding="utf-8")
    cmd = [LLD, "/nologo", "/machine:x64", "/subsystem:console", "/debug", "/demangle:no",
           "/errorlimit:0", f"/out:{exe}", f"@{rsp}", "/defaultlib:msvcrt", "/defaultlib:ucrt",
           "/defaultlib:vcruntime", "/defaultlib:legacy_stdio_definitions", "/defaultlib:winmm",
           "/defaultlib:user32", "/defaultlib:gdi32", "/ignore:4217", "/ignore:4221"]
    if allow_multiple:
        cmd.append("/force:multiple")
    if "asan" in SANITIZE:  # dynamic ASan runtime + the thunk for a /MD (ucrt DLL) exe
        cmd += [str(SAN_LIBDIR / "clang_rt.asan_dynamic-x86_64.lib"),
                "/wholearchive:" + str(SAN_LIBDIR / "clang_rt.asan_dynamic_runtime_thunk-x86_64.lib")]
    elif "ubsan" in SANITIZE:
        # Not ubsan_standalone_cxx: only vptr needs it, and it is built /MT (the
        # objects name it as a default library, hence the /nodefaultlib).
        cmd += [str(SAN_LIBDIR / "clang_rt.ubsan_standalone-x86_64.lib"),
                "/nodefaultlib:clang_rt.ubsan_standalone_cxx-x86_64.lib",
                "/defaultlib:onecore", "/defaultlib:shell32", "/defaultlib:dbghelp"]
    if exe.exists():
        exe.unlink()
    p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace", env=env)
    return p.returncode == 0 and exe.exists(), p.stdout + p.stderr


def is_data_symbol(s):
    # MSVC mangling: ?name@@3... global data, ?name@Cls@@2... static member data,
    # ??_7Cls@@6B@ vftable.
    return s.startswith("?") and re.search(r"@@[236]", s) is not None


def gen_stubs(undef):
    out = ["// AUTO-GENERATED by port/tools/build_aurora.py --stubs. Do not edit by hand:",
           "// implement the real function in port/src and rebuild with --stubs.",
           "#ifdef SMS_PORT_AURORA", '#include "port_stubs.h"', ""]
    for i, s in enumerate(sorted(undef)):
        if s.startswith("__imp_"):
            out.append(f"// skipped dllimport: {s}")
            continue
        if '"' in s or "\\" in s:
            out.append(f"// skipped unquotable symbol: {s}")
            continue
        esc = s  # MSVC/C symbol names never contain quotes or backslashes
        if is_data_symbol(s):
            out.append('asm(".bss\\n.globl \\"' + esc + '\\"\\n.p2align 4\\n\\"' + esc +
                       '\\":\\n  .zero 256\\n");')
        else:
            out.append('asm(".section .rdata$portstub,\\"dr\\"\\n.Lstub_s' + str(i) + ':\\n  .asciz \\"'
                       + esc + '\\"\\n'
                       '.section .text$portstub,\\"xr\\"\\n.globl \\"' + esc + '\\"\\n.p2align 4\\n\\"'
                       + esc + '\\":\\n  subq $40, %rsp\\n  leaq .Lstub_s' + str(i) +
                       '(%rip), %rcx\\n  callq port_missing_stub\\n  addq $40, %rsp\\n'
                       '  xorl %eax, %eax\\n  retq\\n");')
    out.append("#endif")
    g = OUT / "gen"
    g.mkdir(parents=True, exist_ok=True)
    (g / "stubs_aurora.cpp").write_text("\n".join(out) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stubs", action="store_true", help="regenerate stubs for undefined symbols")
    ap.add_argument("--allow-multiple", action="store_true", help="link with /force:multiple")
    ap.add_argument("--run", action="store_true", help="start sms_port.exe after linking")
    ap.add_argument("--timeout", type=int, default=0, help="with --run: kill the exe after N seconds")
    ap.add_argument("--warp", metavar="AREA,SCENARIO",
                    help="with --run: debug warp, sets SMS_PORT_WARP: after the Nintendo logo, skip "
                         "the intro movie, title and file select and start that stage on a fresh "
                         "save (e.g. 1,0 = Delfino Plaza, 2,0 = Bianco Hills episode 1)")
    ap.add_argument("--nodraw", action="store_true",
                    help="with --run: debug, sets SMS_PORT_NODRAW: drop game display lists (J3D "
                         "models never reach the GPU), to drive game logic past render fatals")
    ap.add_argument("--nullgfx", action="store_true",
                    help="with --run: Aurora Null backend, no GPU device, window off-screen")
    ap.add_argument("--warnings", nargs="?", const="", metavar="FILTER",
                    help="no build: syntax-check every game/JSystem unit (optionally only those whose "
                         "path starts with FILTER) with the exact Aurora flags plus the UB/64-bit "
                         "warnings in WARN_FLAGS; writes port/build_aurora/warnings.txt")
    ap.add_argument("--pads", action="store_true",
                    help="with --run: let the game use physical controllers (by default test runs set "
                         "SMS_PORT_NO_PADS=1, so they never read or rumble the player's pad)")
    ap.add_argument("--package", metavar="DIR",
                    help="after building, copy a self-contained release (exe, DLLs, README; never game "
                         "data) into DIR, which must be empty or not exist")
    ap.add_argument("--asan", action="store_true",
                    help="AddressSanitizer build of the game/JSystem units in port/build_aurora_asan "
                         "(recoverable; JKR heaps poison free memory). With --run, reports go to "
                         "<out>/san.<pid>")
    ap.add_argument("--ubsan", action="store_true",
                    help="UndefinedBehaviorSanitizer build (port/build_aurora_ubsan; both flags: "
                         "_asan_ubsan). With --run, reports go to <out>/san.<pid>")
    ap.add_argument("--region", default="us", choices=("us", "jp", "eu", "all"),
                    help="w13: game code for this disc region (US: port/build_aurora/sms_port.exe, "
                         "JP: port/build_aurora_jp/sms_port_jp.exe, EU: port/build_aurora_eu/"
                         "sms_port_eu.exe); 'all' builds jp, eu and us (then --package ships all three)")
    ap.add_argument("-j", type=int, default=0)
    a = ap.parse_args()

    if a.region == "all":  # w13: the other two regions first, each in its own process (OUT is per region)
        rest = []
        skip = False
        for s in sys.argv[1:]:
            if skip:
                skip = False
                continue
            if s in ("--region", "--package", "--warp", "--timeout"):
                skip = True
                continue
            if s.startswith(("--region=", "--package=", "--warp=", "--timeout=")) or s in ("--run",):
                continue
            rest.append(s)
        for r in ("jp", "eu"):
            print(f"== region {r}")
            subprocess.run([sys.executable, str(Path(__file__).resolve()), *rest, "--region", r])
            sub_exe = OUT.parent / (OUT.name.replace("build_aurora", f"build_aurora_{r}", 1)) / REGION_EXE[r]
            if not sub_exe.exists():  # link() deletes the old exe first, so it exists only if this link worked
                sys.exit(f"--region all: the {r} build failed ({sub_exe} missing)")
        print("== region us")

    if a.warnings is not None:
        sys.exit(warnings_scan(a.warnings, a.j))

    env = msvc_env()
    if a.stubs:
        gen = OUT / "gen" / "stubs_aurora.cpp"
        if gen.exists():
            gen.unlink()
    us = census.units()
    MIRRORED.update(sjis_mirror.mirror(ROOT, ["src", "include", "libs"], SJIS))
    libs, arules, extra_libs = aurora_libs()

    def build():
        objs = write_ninja(us, arules)
        cmd = NINJA + ["-f", str(OUT / "build.ninja")] + (["-j", str(a.j)] if a.j else []) + ["-k", "0"]
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace", env=env)
        log = p.stdout + p.stderr
        (OUT / "build.log").write_text(log, encoding="utf-8")
        failed = sorted(set(re.findall(r"FAILED: (?:\[code=\d+\] )?(\S+\.obj)", log)))
        (OUT / "compile_failures.txt").write_text("\n".join(failed), encoding="utf-8")
        good = [o for o in objs if Path(o).exists() and q(o) not in failed and o not in failed]
        missing_libs = [l for l in extra_libs if not Path(l).exists()]
        if missing_libs:
            print("Aurora component libs failed to build:", missing_libs)
        icon_res = compile_icon_res(env)  # Shine icon (Explorer, taskbar, window)
        ok, llog = link(good + ([icon_res] if icon_res else []), libs, env, a.allow_multiple)
        (OUT / "link.log").write_text(llog, encoding="utf-8")
        undef = sorted(set(re.findall(r"undefined symbol: (\S+)", llog)))
        (OUT / "undefined.txt").write_text("\n".join(undef), encoding="utf-8")
        print(f"compiled: {len(good)}/{len(objs)}  compile failures: {len(failed)}  "
              f"undefined symbols: {len(undef)}  exe: {'yes' if ok else 'no'}")
        if not ok and not undef:
            print(llog[-3000:])
        return ok, undef

    ok, undef = build()
    if a.stubs and undef:
        gen_stubs(undef)
        print("regenerated port/build_aurora/gen/stubs_aurora.cpp; relinking")
        ok, undef = build()
    if ok:
        for d in aurora_dlls():
            dst = OUT / d.name
            if not dst.exists() or dst.stat().st_mtime < d.stat().st_mtime:
                shutil.copy2(d, dst)
        if "asan" in SANITIZE:
            d = SAN_LIBDIR / "clang_rt.asan_dynamic-x86_64.dll"
            if not (OUT / d.name).exists() or (OUT / d.name).stat().st_mtime < d.stat().st_mtime:
                shutil.copy2(d, OUT / d.name)
        copy_pipeline_seed()  # w6
    if ok and a.package:
        package(Path(a.package))
    if ok and a.run:
        # The disc is read from %SMS_PORT_DISC% (default: the extracted disc in
        # ../../dolphin_test/files next to the workers). The working directory stays
        # port/build_aurora so nothing Aurora writes (caches, logs) lands in the disc folder.
        renv = dict(os.environ)
        if not renv.get("SMS_PORT_DISC"):
            renv["SMS_PORT_DISC"] = str(next((u / "dolphin_test" / "files" for u in (ROOT.parent, ROOT.parents[1])
                                          if (u / "dolphin_test" / "files").exists()),
                                         ROOT.parent / "dolphin_test" / "files"))
        # Real D3D12 and a visible window by default. --nullgfx: no GPU device and the
        # window off-screen, never activated (port_main.cpp: Aurora's Null backend,
        # software SDL renderer); game logic and GX command processing still run.
        # SMS_PORT_OFFSCREEN=1 in the environment moves a real window off-screen.
        # Test runs use their own pipeline cache, emptied each run: Aurora recompiles every
        # cached pipeline at startup, so one bad GX state recorded by an earlier run would
        # otherwise fail the next run before the game starts.
        if "SMS_PORT_CACHE_DIR" not in renv:
            renv["SMS_PORT_CACHE_DIR"] = str(OUT / "cache")
            shutil.rmtree(OUT / "cache", ignore_errors=True)
        # Test runs never open the player's physical controllers (no input from them, no
        # rumble sent to them): keyboard and SMS_PORT_KEYSCRIPT/PADSCRIPT only. --pads allows them.
        if not a.pads:
            renv.setdefault("SMS_PORT_NO_PADS", "1")
        # Aurora's user folder holds the memory card (<dir>/USA/Card A/*.gci). Test runs
        # use their own (kept between runs), never the per-user one a player's saves live in.
        if "SMS_PORT_USER_DIR" not in renv:
            renv["SMS_PORT_USER_DIR"] = str(OUT / "user")
        if a.warp:
            renv["SMS_PORT_WARP"] = a.warp
        if a.nodraw:
            renv["SMS_PORT_NODRAW"] = "1"
        if a.nullgfx:
            renv["SMS_PORT_NULLGFX"] = "1"
            renv["SDL_RENDER_DRIVER"] = "software"
            renv["SDL_WINDOW_ACTIVATE_WHEN_SHOWN"] = "0"
        if SANITIZE:
            # Keep going after each report (one run, many bugs), one log per process.
            # detect_leaks is unsupported on Windows; the game never frees much anyway.
            # Reports go to <out>/san.<pid> (both runtimes share one log_path setting, the
            # last one parsed wins; relative because a drive letter's ':' ends an option).
            renv.setdefault("ASAN_OPTIONS", "halt_on_error=0:detect_leaks=0:alloc_dealloc_mismatch=0:"
                            "log_path=san")
            renv.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=0:log_path=san")
        kill_leftover_exe()
        proc = subprocess.Popen([str(OUT / EXE_NAME)], cwd=OUT, env=renv)
        RUN_PID.write_text(str(proc.pid), encoding="utf-8")
        try:
            proc.wait(timeout=a.timeout or None)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
            print(f"sms_port.exe still running after {a.timeout} s; killed")
        RUN_PID.unlink(missing_ok=True)


# --warnings: clang diagnostics for the bug classes that crashed the port so far
# (MWCC tolerated them; clang optimises on them or the x64 layout breaks them).
WARN_FLAGS = ["-Wreturn-type", "-Wuninitialized", "-Wsometimes-uninitialized",
              "-Wpointer-to-int-cast", "-Wint-to-pointer-cast", "-Wvoid-pointer-to-int-cast",
              "-Wint-to-void-pointer-cast", "-Warray-bounds", "-Wshorten-64-to-32",
              "-Wunsequenced", "-Winteger-overflow", "-Wshift-count-overflow",
              "-Wshift-count-negative", "-Wreturn-stack-address", "-Wsizeof-pointer-memaccess",
              "-Wfortify-source", "-Wstatic-self-init"]
WARN_PAT = re.compile(r"^(.*?):(\d+):\d+: warning: (.*?)\s*\[(-W[\w-]+)(?:,[^\]]*)?\]\s*$")


def warnings_scan(flt, jobs):
    import collections
    import concurrent.futures
    us = [u for u in census.units() if u["src"].startswith(flt)]
    MIRRORED.update(sjis_mirror.mirror(ROOT, ["src", "include", "libs"], SJIS))

    def one(u):
        src = u["src"]
        fl = flags(src.endswith(".c") and not src.startswith("libs/JSystem/"))
        fl = [x for x in fl if x not in ("-c", "-g", "-gcodeview", "-w", "-Wno-everything")]
        fl += ["-fsyntax-only", "-ferror-limit=0", "-Wno-everything"] + WARN_FLAGS
        if u.get("pch"):
            pch = "include/SMS.pch"
            fl += ["-include", (SJIS / pch).relative_to(ROOT).as_posix() if pch in MIRRORED else pch]
        for d in UNIT_DEFINES.get(src, []):
            fl += ["-D", d]
        inp = (SJIS / src).relative_to(ROOT).as_posix() if src in MIRRORED else src
        p = subprocess.run([CLANG] + fl + GAME_ONLY_FLAGS + [inp], cwd=ROOT, capture_output=True,
                           text=True, errors="replace")
        hits = []
        for l in p.stderr.splitlines():
            m = WARN_PAT.match(l)
            if m:
                f = m.group(1).replace("\\", "/")
                f = re.sub(r"^.*?port/build_aurora/sjis/", "", f)  # mirror copy -> real path
                f = f.replace(ROOT.as_posix() + "/", "")
                hits.append((f, int(m.group(2)), m.group(4), m.group(3)))
        return src, hits, p.returncode

    with concurrent.futures.ThreadPoolExecutor(jobs or max(1, (os.cpu_count() or 4) - 2)) as ex:
        res = list(ex.map(one, us))
    seen, failed = {}, []
    for src, hits, rc in res:
        if rc:
            failed.append(src)
        for f, ln, k, msg in hits:
            seen.setdefault((f, ln, k), msg)
    lines = [f"{f}:{ln}: [{k}] {m}" for (f, ln, k), m in sorted(seen.items())]
    kinds = collections.Counter(k for (_, _, k) in seen)
    dirs = collections.Counter("/".join(f.split("/")[:3]) for (f, _, _) in seen)
    summary = [f"{len(seen)} warnings in {len(us)} units ({len(failed)} units failed to compile)"]
    summary += [f"  {n:5d} {k}" for k, n in kinds.most_common()]
    summary += [f"  {n:5d} {d}" for d, n in dirs.most_common(40)]
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "warnings.txt").write_text("\n".join(lines + [""] + summary + failed) + "\n", encoding="utf-8")
    print("\n".join(summary))
    print(f"full list: {(OUT / 'warnings.txt').relative_to(ROOT).as_posix()}")
    return 0


# ---------------------------------------------------------------------------
# --package (w9): a folder that runs on any Windows 10/11 x64 PC without the repo.

PACKAGE_README = """\
Super Mario Sunshine - PC port (built from the decompilation)
============================================================

This folder contains only the program. It does NOT contain the game: you need your own
copy of Super Mario Sunshine, US version (game ID GMSE01), Japanese version (GMSJ01) or
European version (GMSP01, experimental), as a disc image (.iso, .gcm, .ciso, or Dolphin's
compressed .rvz, .wia and .gcz), or a folder extracted with Dolphin (right-click the game >
Properties > Filesystem > Extract Entire Disc). Every RVZ/WIA compression Dolphin offers
works (Zstandard, LZMA, LZMA2, bzip2, none).

Regions and game behaviour (w13)
--------------------------------
Always start sms_port.exe: it runs the US disc, and hands a Japanese disc to
sms_port_jp.exe and a European one to sms_port_eu.exe automatically. The European disc
is experimental: it plays from the 60 Hz question through the file select, option screen
and stages, but parts of the European game code are not reconstructed yet (see below).
With it, Esc > Game also offers "PAL video" (60 Hz, 50 Hz, or ask at boot as the GameCube
does) and "PAL language" (the console language: English, German, French, Spanish,
Italian; the game's option screen changes it too).
Esc > Game > "Game behaviour" (or "behaviour =" in port_settings.ini) picks the game-code
differences of the US, Japanese or European release (sound effects, fixes, small gameplay
changes) on whichever disc you use; text, menus and other disc data always stay the disc's.
"Match disc" (the default) plays exactly like the release the disc belongs to. "Europe" is
experimental: a few of the European release's differences are not verified yet. A change
applies when the game restarts (the menu offers a "Restart now" button).

Starting
--------
Run sms_port.exe. The game data is looked for in this order:
  1. a path given on the command line:   sms_port.exe "D:\\Games\\Super Mario Sunshine.iso"
  2. "disc =" in port_settings.ini (created next to sms_port.exe on the first start)
  3. a disc image or a files\\ folder next to sms_port.exe
If nothing is found, the game asks for the disc image (drag and drop it onto the game window, or click OK to browse) and remembers your choice.

Requirements: Windows 10 or 11, 64-bit, a Direct3D 12 capable GPU.

Controls
--------
Controller (DualSense, DualShock, Xbox, Switch Pro, GameCube adapter, ...): the buttons
sit where a GameCube pad has them. PlayStation: Cross = A, Square = B, Circle = X,
Triangle = Y, R1 = Z, L2/R2 = L/R (analog; a full press clicks), Options = Start.
Keyboard: WASD = stick (hold Left Shift to walk), arrow keys = C-stick, Space = A, E = B,
Q = X, F = Y, Z = Z, C = L, R = R (Shift+R = light press), Return = Start, I/J/K/L = D-pad.

  Esc or F1 (or the controller's Create/Back/View)  settings menu
  Tab (in multiplayer)                               player list (hold)
  F11 or Alt+Enter                                     fullscreen
  F3                                                   frame-rate overlay
  F9                                                   save a bug report (see below)

Settings are saved in port_settings.ini next to sms_port.exe (window size and position,
fullscreen, aspect, internal resolution, frame rate, dead zones, rumble, key bindings,
disc path, game behaviour).
Sound (Esc > Audio, or "output =" under [audio] in port_settings.ini):
  Pro Logic II matrix (original)  default: the game's own Sound option decides (mono,
                                  stereo, or surround matrix-encoded into stereo, for a
                                  Pro Logic II decoder), exactly as on the GameCube.
  Stereo                          plain stereo, never the surround matrix (the game's Mono
                                  option still gives mono).
  5.1 / 7.1                       discrete channels for a surround sound card or receiver:
                                  front left/right, rear (5.1) or rear + side (7.1), LFE;
                                  the centre carries the game's centre bus. The game is
                                  switched to its surround mixing for this (your saved Sound
                                  option is not changed). If Windows reports fewer speakers
                                  the menu says so and the sound stays stereo. Movie sound
                                  plays on the front speakers.
Save files (memory card) and the shader cache are kept in your user profile
(%APPDATA%\\Super Mario Sunshine). If the game crashes, sms_port_crash.log next to
sms_port.exe has the details.

Frame rate
----------
The game itself always runs at its original 30 frames per second (and 59.94 Hz video
timing), so it plays exactly like the original. Esc > Display > Frame rate (or
"frame_rate =" in port_settings.ini) can show more frames than that: 30 (original,
default), 60, 120, 144, Match display (your monitor's refresh rate) or Unlimited. Above 30,
extra pictures are drawn between two game frames with the camera and the characters'
and objects' movement interpolated, which makes motion smoother; the game's own pictures
are still shown when they are due. The extra pictures are drawn only during gameplay
(menus, movies and loading screens stay at their original rate), and they never change
the game: a replay plays back identically at every setting. Some effects (particles,
water, shadows, the 2D display) still move in 30 Hz steps. Higher rates need a faster
CPU: each extra picture costs about as much as drawing a game frame. The F3 overlay
shows the pictures per second ("fps") and the game's own rate ("logic").

Controller extras
-----------------
Esc > Controller (and [input] in port_settings.ini):
- Analog L/R rise evenly up to the click; a full press (through the click) is the GameCube's
  digital L/R. FLUDD: a light press sprays while you walk, a full press plants Mario to aim.
- Adaptive triggers (DualSense): GameCube click (light resistance, a firm wall at the click),
  or Full FLUDD feedback (R also goes slack when the tank is empty, pulses while spraying, and
  has its own feel for the hover, rocket and turbo nozzles). Strength slider.
- Lightbar (DualSense, DualShock 4): Fluid level (default) shows FLUDD's water in light blue
  (#00C9FF), or the colour of Yoshi's juice while riding him, and gets darker as it drains; dim white
  without FLUDD and in menus. Or a static colour, or off. Brightness slider.
- Rumble: Original (the game's own motor pulses, default), HD or Off. HD plays designed
  waveforms for the game's rumble and adds FLUDD (spray, hover, rocket, turbo), footsteps by
  ground type, landings, wall kicks, dives, coins, 1-ups and Shines; each group can be
  switched off. DualSense: HD haptics over USB or Bluetooth. Xbox: FLUDD on the impulse
  triggers. A GameCube pad on the adapter keeps the original pulses.
Esc > Audio: Controller speaker (DualSense): FLUDD's sounds (and optionally pickups) on the
controller's speaker, over USB or Bluetooth; the same sounds can be kept, ducked or removed
on the PC speakers.

GameCube controller adapter (Nintendo WUP-028 or compatible, in "Wii U" / PC mode): Windows
needs a one-time driver change before the adapter can be used:
  1. Download Zadig from https://zadig.akeo.ie and start it.
  2. Options > List All Devices; pick "WUP-028" (USB ID 057E 0337).
  3. Choose WinUSB as the driver and press Replace Driver (or Install Driver).
Then plug the adapter in before starting the game; each port is a controller. This is the
same step Dolphin needs, and the adapter keeps working in Dolphin afterwards.

Quality of life
---------------
Esc > Quality of life (and [qol] in port_settings.ini). Everything here is OFF by default,
so the game plays exactly like the original until you turn something on.
- Skip boot logos and intro movie: straight to the title screen.
- Start skips any cutscene or movie: also movies you have not seen yet, the Delfino Plaza
  intros, and in-engine cutscenes (Start fast-forwards the cutscene camera to its end).
- Dialogue text speed: Normal, Fast (as if A were held) or Instant (whole lines at once).
- Auto-save after each Shine: the game is saved to the file you loaded without the save
  prompt ("Game saved" appears); if that is not possible (no file, another file on the
  card, card error) the usual prompt comes up.
- Continue in the level after collecting a Shine: the Shine is counted and saved as usual,
  but you stay in the level and keep playing (it cannot be collected twice). Secret
  courses, the airstrip, Delfino Plaza, Corona Mountain and Shines handed out by scripts
  still end the level as in the original.
- Fast-forward while holding a key (Tab by default, rebindable in the menu): 2x or 4x
  game speed while it is held; a ">> 2x" badge shows it. Off in multiplayer.
- Pause when the window loses focus: game and sound stop until you click back in.
- Camera: invert the C-stick's X and/or Y axis, and a camera speed multiplier (50-200%).
- Volume: separate sliders for music, sound effects and voices.
- Trackers: blue coins per level and the current level's Shine checklist (episode names,
  collected or not) next to the pause screen; F4 shows both at any time.
- Cheats: infinite lives, infinite FLUDD water. Off in multiplayer.
Replays record these settings, so a replay plays back the same whatever your own settings
are. Cheats, auto-save, continue-after-Shine, fast-forward and focus pause are off in a
multiplayer session.

Playing online with friends
---------------------------
Everyone needs this folder and their own copy of the game disc (the same region is best).
No downloads, accounts or IP addresses: the game comes with a lobby server.
  Host:  on the file select screen walk left to the Online block and hit it from below,
         pick Host, give the lobby a name (and a password if you like), Create.
  Join:  the same Online block, pick Join, type the lobby's name, Join.
In a session: T chat, G emote wheel, D-pad quick emotes, hold Tab for the player list,
F2 the lobby panel (the host picks game modes: Free, Hide and Seek, ...).
The Customize block (left of Online) sets your name, name-tag colour, Mario's colours
and the emote wheel.
If joining fails, the host's Windows firewall may have asked to allow sms_port.exe:
allow it (private and public networks).

Reporting a bug
---------------
The game keeps the last 20 seconds of picture and sound in memory, together with its log,
and records your controller input from the moment it starts (a few KB per hour).
When something goes wrong, press F9 (or use "Save a bug report" on the Game tab of the
settings menu (Esc)): it writes a folder bug_reports\\<date>_<time>_f9\\ next to
sms_port.exe with video.mp4 (the last 20 seconds, with sound), log.txt (your Windows and
GPU, the settings, where you were in the game, the frame rate, the last 20 seconds of the
log) and sms_replay_<date>_<time>.bin (your inputs, a copy of your memory card as it was
at start-up and which disc image you used; no game data). After a crash the same folder
(..._crash, with the crash backtrace in log.txt) is written automatically. Send the whole
folder with a short description. Developers play the replay back with
SMS_PORT_REPLAY=<file> to see exactly what happened. The capture costs about nothing;
"enabled = 0" under [bugreport] in port_settings.ini turns it off (seconds, video size and
frame rate can be changed there too), "record = 0" under [debug] turns off the input
recording.

Discord (Esc > Game > Discord, and [discord] in port_settings.ini): Rich Presence and
invites, with no Discord SDK. The game talks to the Discord desktop app running on the same PC.
It needs an Application ID, which you create yourself: discord.com/developers/applications >
New Application > copy the Application ID into "client_id =" (digits) under [discord]. Under
Rich Presence > Art Assets upload one large image named "logo" and, optionally, small images
named delfino, bianco, ricco, gelato, pinna, sirena, monte, noki and corona (missing ones are
simply not shown). Shows the level, episode, Shine and Blue Coin count and the time played;
in an online lobby (lobby server, no password) friends get a Join button, and "Ask to join"
shows a prompt in the game (F7 accept, F8 ignore). Direct (IP) sessions are never advertised.
"enabled = 0" or an empty client_id turns it off completely; allow_join = 0 and show_stage = 0
limit what is shared.

Included libraries: Aurora (MIT), Dawn (BSD-3-Clause), DirectX Shader Compiler
(dxcompiler/dxil, University of Illinois/NCSA), SDL 3 (zlib), libpng (libpng license),
zlib (zlib), Dear ImGui (MIT), libopus (BSD-3-Clause), Microsoft Visual C++ runtime
(redistributable).
"""

# w6: Aurora's bundled pipeline-cache seed. Aurora opens <exe folder>/initial_pipeline_cache.db
# read-only at startup and merges its rows into the per-user cache, then compiles every known
# pipeline in the background, so the first start does not stall on shader compiles.
# Regenerate with port/tools/make_pipeline_seed.py from caches filled by a full sweep.
PIPELINE_SEED = ROOT / "port" / "data" / "initial_pipeline_cache.db"


def copy_pipeline_seed():
    """Puts the seed next to port/build_aurora/sms_port.exe (also used by --run and hand runs)."""
    dst = OUT / PIPELINE_SEED.name
    if not PIPELINE_SEED.exists():
        return
    if not dst.exists() or dst.stat().st_mtime < PIPELINE_SEED.stat().st_mtime:
        shutil.copy2(PIPELINE_SEED, dst)


def vc_redist_dir():
    """Highest VC/Redist/MSVC/<ver>/x64/Microsoft.VC*.CRT folder of the VS install."""
    base = VCVARS.parents[2] / "Redist" / "MSVC"
    best = None
    for ver in sorted((p for p in base.iterdir() if p.is_dir() and p.name[0].isdigit()),
                      key=lambda p: [int(x) for x in p.name.split(".") if x.isdigit()]):
        crt = sorted((ver / "x64").glob("Microsoft.VC*.CRT"))
        if crt:
            best = crt[-1]
    return best


def pe_imports(path):
    p = subprocess.run([str(LLVM / "llvm-readobj.exe"), "--coff-imports", str(path)],
                       capture_output=True, text=True, errors="replace")
    return {m.lower() for m in re.findall(r"Name: (\S+\.dll)", p.stdout, re.I)}


def package(dest):
    dest = dest.resolve()
    if dest.exists() and any(dest.iterdir()):
        sys.exit(f"--package: {dest} is not empty")
    dest.mkdir(parents=True, exist_ok=True)
    # The exe, switched to the Windows (GUI) subsystem: no console window. The entry point
    # stays mainCRTStartup; output redirected by the caller (sms_port.exe > log.txt) still works.
    files = []
    # w13 (BOOTLOG "Regional behaviour (w13)"): the JP and EU builds go next to sms_port.exe when
    # they have been built (--region all); each exe hands a disc of another region to its sibling.
    for name, src in [("sms_port.exe", OUT / "sms_port.exe")] + [
            (REGION_EXE[r], OUT.parent / OUT.name.replace("build_aurora", f"build_aurora_{r}", 1) / REGION_EXE[r])
            for r in ("jp", "eu")]:
        if name != "sms_port.exe" and not src.exists():
            print(f"package: no {name} (build it with --region {name[9:11]} or --region all)")
            continue
        exe = bytearray(src.read_bytes())
        pe = int.from_bytes(exe[0x3C:0x40], "little")
        assert exe[pe:pe + 4] == b"PE\0\0"
        exe[pe + 24 + 68:pe + 24 + 70] = (2).to_bytes(2, "little")  # IMAGE_SUBSYSTEM_WINDOWS_GUI
        (dest / name).write_bytes(bytes(exe))
        files.append(dest / name)
    for d in aurora_dlls():
        shutil.copy2(d, dest / d.name)
        files.append(dest / d.name)
    # Visual C++ runtime DLLs the exe and the bundled DLLs import (app-local deployment).
    # The UCRT (api-ms-win-crt-*) is part of Windows 10/11.
    crt = vc_redist_dir()
    if crt is None:
        sys.exit("--package: Visual C++ redistributable folder not found")
    wanted = set()
    for f in files:
        wanted |= pe_imports(f)
    copied = []
    for dll in sorted(crt.glob("*.dll")):
        if dll.name.lower() in wanted:
            shutil.copy2(dll, dest / dll.name)
            copied.append(dll.name)
    # w6: bundled pipeline-cache seed (see copy_pipeline_seed).
    if PIPELINE_SEED.exists():
        shutil.copy2(PIPELINE_SEED, dest / PIPELINE_SEED.name)
    else:
        print(f"package: WARNING, no pipeline seed at {PIPELINE_SEED} (first start compiles all shaders)")
    (dest / "README.txt").write_text(PACKAGE_README.replace("\n", "\r\n"), encoding="utf-8", newline="")
    present = {p.name.lower() for p in dest.iterdir()}
    system = {"kernel32.dll", "user32.dll", "gdi32.dll", "advapi32.dll", "shell32.dll", "ole32.dll", "oleaut32.dll",
              "imm32.dll", "winmm.dll", "version.dll", "setupapi.dll", "dbghelp.dll", "dxgi.dll", "ntdll.dll",
              "d3d12.dll", "d3d11.dll", "dwmapi.dll"}
    missing = sorted(w for w in wanted if w not in present and w not in system
                     and not w.startswith("api-ms-win-"))
    total = sum(p.stat().st_size for p in dest.iterdir())
    print(f"package: {dest} ({len(present)} files, {total / 1e6:.1f} MB); VC runtime: {', '.join(copied)}")
    if missing:
        print("package: WARNING, imported DLLs not bundled and not part of Windows:", ", ".join(missing))


# PID of the sms_port.exe the last --run started. Only that process is ever killed
# (someone may be running another copy, even of this same exe, by hand).
RUN_PID = OUT / "run_pid.txt"


def kill_leftover_exe():
    """Kills the sms_port.exe a previous --run started, if it is still running."""
    if not RUN_PID.exists():
        return
    pid = RUN_PID.read_text(encoding="utf-8").strip()
    if pid.isdigit():
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        f"Get-Process -Id {pid} -ErrorAction SilentlyContinue | "
                        f"Where-Object {{ $_.ProcessName -eq '{Path(EXE_NAME).stem}' }} | Stop-Process -Force"],
                       capture_output=True)
    RUN_PID.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
