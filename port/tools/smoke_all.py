"""Regression smoke suite for the Aurora PC build (worker w10).

    python port/tools/smoke_all.py                 # full suite, 3 scenes at a time
    python port/tools/smoke_all.py --quick         # one scenario per area, 45 s each
    python port/tools/smoke_all.py -j 4 --asan     # ASan build (port/build_aurora_asan)
    python port/tools/smoke_all.py --only 2,0 boot # just these scenes (names or warps)
    python port/tools/smoke_all.py --list          # print the scene list and exit
    python port/tools/smoke_all.py --only fileselect_dvdsync --repeat 6   # flaky hunt
    python port/tools/smoke_all.py --exe <dir>/sms_port.exe ...   # test another build

What it does
1. Builds the exe with build_aurora.py (incremental, so cheap when nothing changed;
   --no-build skips it). --asan / --ubsan pass through to the build and select its
   output directory. The exe, PDB, DLLs and pipeline seed are then copied to
   <out>/smoke/bin_<timestamp>/ and run from there, so a merge can relink
   port/build_aurora/sms_port.exe while the suite runs (the two newest copies are kept).
2. Runs every scene for a fixed time with real D3D12 (SMS_PORT_OFFSCREEN=1: the window
   sits off-screen and is never activated, so it does not steal focus; screenshots are
   EFB readbacks, which work there). Each scene gets its own working directory, memory
   card folder (SMS_PORT_USER_DIR), pipeline cache and settings file, so parallel runs
   never share state and never touch the player's save.
   Scenes:
   - boot: Nintendo logo, intro movie, title and attract loop (no input);
   - newgame: title -> file select -> new file -> opening movie -> airstrip (START
     through the title and file select, then A); STUCK unless area 0 gameplay starts.
     Always runs at least 90 s, --quick included: area 0 is reached around frame
     3000-3600 (about 55-65 s with -j 3), which 45 s cannot cover;
   - fileselect_dvdsync: START through the title into the file select with
     SMS_PORT_DVD_SYNC_BLOCK=1 (hardware-accurate blocking sync DVD reads, w7; the
     default since w10's runs cleared it, the scene keeps it pinned);
   - warps (SMS_PORT_WARP): every area 0-9 with scenarios 0-2, the plaza states, later
     key episodes, Pinna Park, the casino, Noki's undersea area, the secret ("EX")
     courses, the bosses and Corona. The area/scenario table is read from the disc's
     stageArc.bin, and a scene is skipped when its scenario archive is not on the disc.
   Gameplay input (SMS_PORT_PADSCRIPT): A through dialogs, then run, jump, spray (R),
   dive and turn once Mario has control ("+frame" entries).
3. Collects per scene: pass/fail, the crash excerpt (first backtrace frames), frames
   reached, a mid-run and a late screenshot (PNG, half size; taken every 300 frames,
   the one nearest the middle and the last are kept), Aurora ERROR/WARNING
   counts, audio underruns/drops (SMS_PORT_AUDIO_LOG), the last TApplication state and,
   with --asan/--ubsan, the number of sanitizer reports.
4. Writes <out>/smoke/report.md and report.json and compares with the previous run
   (report_prev.json): new failures are listed at the top, then fixed scenes.

A scene FAILS when the process exits before its time is up (crash, abort, FATAL), or
the log has "[port] CRASH", "[aurora FATAL", "STACK COOKIE", or a sanitizer abort.
A warp scene that never reaches gameplay (TApplication state 5 of its area) is marked
STUCK (counted as a failure in the summary, listed separately).
"""
import argparse
import concurrent.futures
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOLS = ROOT / "port" / "tools"


def find_disc():
    for up in (ROOT.parent, ROOT.parents[1]):
        p = up / "dolphin_test" / "files"
        if p.exists():
            return p
    return ROOT.parent / "dolphin_test" / "files"


DISC = Path(os.environ.get("SMS_PORT_DISC", str(find_disc())))


# w13 (BOOTLOG "Regional behaviour (w13)"): --disc may also be a plain disc image (.iso/.gcm,
# e.g. the JP or EU disc for --region jp/eu); its FST is read for stageArc.bin and the scene list.
def _iso_fst(path):
    with open(path, "rb") as f:
        f.seek(0x424)
        off, size = struct.unpack(">II", f.read(8))
        f.seek(off)
        fst = f.read(size)
    count = struct.unpack(">I", fst[8:12])[0]
    strings = fst[count * 12:]
    files, stack = {}, [("", count)]
    for i in range(1, count):
        flags, a, b = fst[i * 12], *struct.unpack(">II", fst[i * 12 + 4:i * 12 + 12])
        nameoff = int.from_bytes(fst[i * 12 + 1:i * 12 + 4], "big")
        name = strings[nameoff:strings.index(b"\0", nameoff)].decode("ascii", "replace")
        while stack and i >= stack[-1][1]:
            stack.pop()
        full = (stack[-1][0] + "/" + name).lstrip("/") if stack else name
        if flags:
            stack.append((full, b))
        else:
            files[full.lower()] = (a, b)
    return files


def disc_read(rel):
    if DISC.is_file():
        files = _iso_fst(DISC)
        off, size = files[rel.lower()]
        with open(DISC, "rb") as f:
            f.seek(off)
            return f.read(size)
    return (DISC / rel).read_bytes()


def disc_stems(rel_dir, ext):
    if DISC.is_file():
        pre = rel_dir.lower().rstrip("/") + "/"
        return {Path(k).stem for k in _iso_fst(DISC) if k.startswith(pre) and k.endswith(ext) and "/" not in k[len(pre):]}
    return {p.stem for p in (DISC / rel_dir).glob("*" + ext)}

# Generic gameplay input. Absolute frames: A through the arrival dialogs. "+frames":
# counted from when the stage gives Mario control.
GAMEPLAY_SCRIPT = ("100-1800/60:A,"
                   "+0-600:S0,+0-3000/45:A,+600-1200:S90+R,+1200-1800:S180,"
                   "+1800-2400:S270,+1800-2400/50:B,+2400-3000:S45+R,+3000-3600:S135")
# Title -> file select -> new file -> opening movie (THP speed 8) -> airstrip.
# START through the title and the "create a file on the Memory Card?" prompt (YES is
# the default). At "Select data." the game hands Mario control: a file is chosen by
# jumping into its block (A alone jumps beside block A, and START reopens the prompt),
# so step right/left and jump until a block is hit (w7). Then A through the airstrip
# dialogs. Absolute frames: the title stage also "gives Mario control".
NEWGAME_SCRIPT = ("650-1700/60:START,"
                  + ",".join(f"{f}-{f + 8}:S90,{f + 30}:A,{f + 200}-{f + 208}:S270,{f + 230}:A"
                             for f in range(1900, 4300, 400))
                  + ",4500-9000/40:A")
# START through the title into the file select (w7's SMS_PORT_DVD_SYNC_BLOCK repro).
FILESELECT_SCRIPT = "650-3000/60:START"


def stage_table():
    """[(area, [scenario archive names])] from stageArc.bin (ScenarioArchiveNameTable
    groups in area order)."""
    b = disc_read("data/stageArc.bin")  # w13: folder or image
    areas = []
    for m in re.finditer(rb"[ -~]{4,}", b):
        s = m.group().decode("ascii", "replace")
        if s.startswith("ScenarioArchiveNameTable"):
            areas.append([])
        elif s.endswith(".arc") and areas:
            areas[-1].append(s[:-4])
    return list(enumerate(areas))


def scene_list(quick):
    table = stage_table()
    # w13: folder or image. Compared case-insensitively: the image's FST names are
    # lowercased by _iso_fst, and stageArc.bin says pinnaBeach0, biancoBoss, ... (w10)
    have = {h.lower() for h in disc_stems("data/scene", ".szs")}
    scenes = [dict(name="boot", kind="boot", warp=None),
              # min_duration: the title, file select and opening movie take about a
              # minute before area 0 starts, so --quick's 45 s would always be STUCK
              dict(name="newgame", kind="newgame", warp=None, min_duration=90),
              # title -> file select with hardware-accurate blocking sync DVD reads (w7)
              dict(name="fileselect_dvdsync", kind="fileselect", warp=None,
                   label="file select, SMS_PORT_DVD_SYNC_BLOCK=1",
                   env={"SMS_PORT_DVD_SYNC_BLOCK": "1"})]
    seen = set()

    def add(area, sc, label=None):
        if area >= len(table) or sc >= len(table[area][1]):
            return
        arc = table[area][1][sc]
        if arc.lower() not in have or (area, sc) in seen:
            return
        seen.add((area, sc))
        scenes.append(dict(name=f"{area},{sc}", kind="warp", warp=f"{area},{sc}",
                           arc=arc, label=label or arc))

    if quick:
        for area in list(range(0, 10)) + [13, 14, 16, 20, 28, 46, 55, 60]:
            add(area, 0)
        return scenes
    for area in range(0, 10):                 # every main area, scenarios 0-2
        for sc in range(3):
            add(area, sc)
    for sc in range(3, len(table[1][1])):     # remaining plaza states
        add(1, sc)
    for area in (2, 3, 4, 5, 6, 8, 9):         # later key episodes
        for sc in (4, 6, 7):
            add(area, sc)
    for area, sc in ((7, 3), (7, 4), (13, 0), (13, 1), (13, 5), (14, 0), (14, 1), (16, 0)):
        add(area, sc)
    for area in range(20, len(table)):         # EX courses, bosses, Corona
        add(area, 0)
    add(58, 1)
    return scenes


# ---------------------------------------------------------------------------- BMP -> PNG

def bmp_to_png(src, dst, scale=2):
    """Uncompressed 24/32-bit BMP -> RGB PNG, downscaled by `scale` (nearest)."""
    b = src.read_bytes()
    if b[:2] != b"BM":
        return False
    off = struct.unpack_from("<I", b, 10)[0]
    w, h = struct.unpack_from("<ii", b, 18)
    bpp = struct.unpack_from("<H", b, 28)[0]
    if bpp not in (24, 32):
        return False
    bottom_up = h > 0
    h = abs(h)
    stride = ((w * bpp // 8) + 3) & ~3
    px = bpp // 8
    ow, oh = w // scale, h // scale
    raw = bytearray()
    for y in range(oh):
        sy = y * scale
        row = off + ((h - 1 - sy) if bottom_up else sy) * stride
        raw.append(0)
        for x in range(ow):
            p = row + x * scale * px
            raw += bytes((b[p + 2], b[p + 1], b[p]))

    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")
    dst.write_bytes(png)
    return True


# ---------------------------------------------------------------------------- one scene

FPS_EST = 55  # presented frames per second seen in test runs


def analyse(log_text, scene, exited_early, exit_code, sanitize):
    r = {}
    frames = [int(x) for x in re.findall(r"\[port\] frame (\d+) \(retrace", log_text)]
    r["frames"] = max(frames) if frames else 0
    states = re.findall(r"\[port\] TApplication state (\d+) \(area (\d+)/(\d+)\)", log_text)
    r["last_state"] = "%s (%s/%s)" % states[-1] if states else "-"
    r["aurora_errors"] = len(re.findall(r"^\[aurora ERROR", log_text, re.M))
    r["aurora_warnings"] = len(re.findall(r"^\[aurora WARNING", log_text, re.M))
    audio = re.findall(r"\[port\] audio: \d+ buffers, queue -?\d+ frames, (\d+) underruns, (\d+) drops",
                       log_text)
    r["underruns"], r["drops"] = (int(audio[-1][0]), int(audio[-1][1])) if audio else (0, 0)
    excerpt, why = [], []
    lines = log_text.splitlines()
    for i, l in enumerate(lines):
        if ("[port] CRASH" in l or "[aurora FATAL" in l or "STACK COOKIE" in l
                or "ERROR: AddressSanitizer" in l):
            why.append(l.strip()[:200])
            if not excerpt:
                frames_seen = 0
                for l2 in lines[i:i + 40]:
                    excerpt.append(l2.rstrip()[:220])
                    if re.match(r"\s*#\d+ ", l2):
                        frames_seen += 1
                    if frames_seen >= 8:
                        break
    fatal = [w for w in why if "AddressSanitizer" not in w]
    r["sanitizer_reports"] = 0
    r["status"] = "PASS"
    if exited_early:
        r["status"] = "FAIL"
        why.insert(0, f"exited early (code {exit_code:#x})" if exit_code is not None else "exited early")
    elif fatal:
        r["status"] = "FAIL"
    elif scene["kind"] in ("warp", "newgame"):
        area = scene["warp"].split(",")[0] if scene["kind"] == "warp" else "0"
        if not any(s == "5" and a == area for s, a, _ in states):
            r["status"] = "STUCK"
            why.append(f"never reached gameplay in area {area} (TApplication state 5)")
    r["why"] = why[:6]
    r["excerpt"] = excerpt
    return r


def run_scene(scene, exe, sdir, duration, sanitize, keep_cache):
    if sdir.exists():
        shutil.rmtree(sdir, ignore_errors=True)
    sdir.mkdir(parents=True)
    # Shots every 300 frames (the frame rate varies with load); the one nearest the
    # middle of the run and the last one are kept.
    shot_frames = list(range(300, int(duration * 70), 300))
    env = dict(os.environ)
    env.update({
        "SMS_PORT_DISC": str(DISC),
        "SMS_PORT_OFFSCREEN": "1",
        "SMS_PORT_NO_PADS": "1",
        "SMS_PORT_USER_DIR": str(sdir / "user"),
        "SMS_PORT_CACHE_DIR": str(sdir / "cache"),
        "SMS_PORT_SETTINGS": str(sdir / "port_settings.ini"),
        "SMS_PORT_THP_SPEED": "8",
        "SMS_PORT_AUDIO_LOG": "1",
        "SMS_PORT_SHOT": ",".join(str(f) for f in shot_frames),
    })
    for k in ("SMS_PORT_WARP", "SMS_PORT_PADSCRIPT", "SMS_PORT_KEYSCRIPT", "SMS_PORT_NULLGFX",
              "SMS_PORT_DVD_SYNC_BLOCK"):
        env.pop(k, None)
    env.update(scene.get("env", {}))
    if scene["kind"] == "warp":
        env["SMS_PORT_WARP"] = scene["warp"]
        env["SMS_PORT_PADSCRIPT"] = GAMEPLAY_SCRIPT
    elif scene["kind"] == "newgame":
        env["SMS_PORT_PADSCRIPT"] = NEWGAME_SCRIPT
    elif scene["kind"] == "fileselect":
        env["SMS_PORT_PADSCRIPT"] = FILESELECT_SCRIPT
    if sanitize:
        env.setdefault("ASAN_OPTIONS", "halt_on_error=0:detect_leaks=0:alloc_dealloc_mismatch=0:log_path=san")
        env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1:halt_on_error=0:log_path=san")
    log = sdir / "log.txt"
    t0 = time.time()
    with open(log, "wb") as f:
        p = subprocess.Popen([str(exe)], cwd=sdir, env=env, stdout=f, stderr=subprocess.STDOUT,
                             creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        try:
            p.wait(timeout=duration)
            exited_early, code = True, p.returncode & 0xFFFFFFFF
        except subprocess.TimeoutExpired:
            exited_early, code = False, None
            p.kill()
            p.wait()
    text = log.read_text(encoding="utf-8", errors="replace")
    if sanitize:  # sanitizer logs live next to the scene
        for s in sdir.glob("san.*"):
            text += "\n" + s.read_text(encoding="utf-8", errors="replace")
    r = analyse(text, scene, exited_early, code, sanitize)
    if sanitize:
        r["sanitizer_reports"] = len(re.findall(r"ERROR: AddressSanitizer|runtime error:", text))
    r["seconds"] = round(time.time() - t0, 1)
    taken = sorted((int(p.stem.split("_")[1]), p) for p in sdir.glob("shot_*.bmp"))
    keep = []
    if taken:
        mid = min(taken, key=lambda t: abs(t[0] - taken[-1][0] / 2))
        keep = [mid] if mid is taken[-1] else [mid, taken[-1]]
    shots = []
    for fr, bmp in keep:
        png = sdir / f"shot_{fr}.png"
        if bmp_to_png(bmp, png):
            shots.append(png.name)
        else:
            shots.append(bmp.name)
            continue
    for _, bmp in taken:
        if bmp.exists() and bmp.name not in shots:
            bmp.unlink()
    r["shots"] = shots
    if not keep_cache:
        shutil.rmtree(sdir / "cache", ignore_errors=True)
    r.update(name=scene["name"], kind=scene["kind"], label=scene.get("label", scene["name"]))
    (sdir / "result.json").write_text(json.dumps(r, indent=1), encoding="utf-8")
    return r


EXE_NAME = "sms_port.exe"  # w13: sms_port_jp.exe / sms_port_eu.exe with --region or --exe


def snapshot_bin(out_build, out, seed=True):
    """Copies the exe, its PDB (for crash backtraces), the DLLs and the pipeline seed into
    smoke/bin_<timestamp>/ and runs from there, so a rebuild or merge can relink
    port/build_aurora/sms_port.exe while the suite runs. Keeps the two newest copies."""
    dst = out / time.strftime("bin_%Y%m%d_%H%M%S")
    dst.mkdir(parents=True)
    for f in out_build.iterdir():
        if f.is_file() and (f.suffix.lower() in (".dll", ".pdb") or f.name == EXE_NAME
                            or (seed and f.name == "initial_pipeline_cache.db")):
            shutil.copy2(f, dst / f.name)
    seed_db = ROOT / "port" / "data" / "initial_pipeline_cache.db"
    if seed and seed_db.exists() and not (dst / seed_db.name).exists():
        shutil.copy2(seed_db, dst / seed_db.name)
    for old in sorted(out.glob("bin_*"))[:-2]:
        shutil.rmtree(old, ignore_errors=True)
    return dst / EXE_NAME


# ---------------------------------------------------------------------------- report

def write_report(out, results, prev, args, wall):
    order = {"FAIL": 0, "STUCK": 1, "PASS": 2}
    prev_by = {r["name"]: r for r in prev.get("results", [])} if prev else {}
    new_fail = [r for r in results if r["status"] != "PASS"
                and prev_by.get(r["name"], {}).get("status", "PASS") == "PASS" and r["name"] in prev_by]
    fixed = [r for r in results if r["status"] == "PASS" and prev_by.get(r["name"], {}).get("status") not in (None, "PASS")]
    counts = {k: sum(1 for r in results if r["status"] == k) for k in order}
    summary = (f"**{'OK' if not counts['FAIL'] and not counts['STUCK'] else 'PROBLEMS'}: "
               f"PASS {counts['PASS']}, FAIL {counts['FAIL']}, STUCK {counts['STUCK']}** "
               f"({time.strftime('%Y-%m-%d %H:%M')}, {len(results)} scenes)")
    if prev:
        summary += (" | NEW FAILURES: " + ", ".join(f"`{r['name']}` {r['status']}" for r in new_fail)
                    if new_fail else " | no new failures")
        if fixed:
            summary += " | fixed: " + ", ".join(f"`{r['name']}`" for r in fixed)
    bad_now = [r for r in results if r["status"] != "PASS"]
    if bad_now:
        summary += " | not passing: " + ", ".join(f"`{r['name']}`" for r in sorted(bad_now, key=lambda r: r["name"]))
    # sanitizer reports do not fail a scene (halt_on_error=0) but must not hide behind PASS
    san_hit = [r for r in results if r.get("sanitizer_reports")]
    if san_hit:
        summary += (f" | SANITIZER REPORTS in {len(san_hit)} scenes: "
                    + ", ".join(f"`{r['name']}` ({r['sanitizer_reports']})" for r in san_hit[:12])
                    + (" ..." if len(san_hit) > 12 else ""))
    L = [summary, "", f"# Smoke run {time.strftime('%Y-%m-%d %H:%M')}", ""]
    L.append(f"Build: `{args.build_dir}`; {len(results)} scenes, {args.duration} s each, -j {args.j}; "
             f"wall time {wall / 60:.1f} min. PASS {counts['PASS']}, FAIL {counts['FAIL']}, STUCK {counts['STUCK']}.")
    L.append("")
    if prev:
        L.append(f"Compared with the previous run ({prev.get('time', '?')}):")
        if new_fail:
            L.append("")
            L.append("**NEW FAILURES:** " + ", ".join(f"`{r['name']}` ({r['label']}, {r['status']})" for r in new_fail))
        else:
            L.append("no new failures.")
        if fixed:
            L.append("")
            L.append("Fixed since then: " + ", ".join(f"`{r['name']}`" for r in fixed))
        L.append("")
    L.append("| Scene | Stage | Status | Frames | Last state | Aurora E/W | Underruns | San | Shots | Reason |")
    L.append("|---|---|---|---|---|---|---|---|---|---|")
    for r in sorted(results, key=lambda r: (order[r["status"]], r["name"])):
        shots = " ".join(f"[{i}]({r['name'].replace(',', '_')}/{s})" for i, s in enumerate(r["shots"], 1))
        why = (r["why"][0] if r["why"] else "").replace("|", "/")
        L.append(f"| `{r['name']}` | {r['label']} | {r['status']} | {r['frames']} | {r['last_state']} | "
                 f"{r['aurora_errors']}/{r['aurora_warnings']} | {r['underruns']} | {r['sanitizer_reports']} | "
                 f"{shots} | {why[:120]} |")
    bad = [r for r in results if r["status"] != "PASS" and r["excerpt"]]
    if bad:
        L += ["", "## Crash excerpts", ""]
        for r in bad:
            L += [f"### `{r['name']}` ({r['label']})", "```"] + r["excerpt"][:24] + ["```", ""]
    (out / "report.md").write_text("\n".join(L) + "\n", encoding="utf-8")
    (out / "report.json").write_text(json.dumps(
        dict(time=time.strftime("%Y-%m-%d %H:%M"), build=args.build_dir, duration=args.duration,
             results=results), indent=1), encoding="utf-8")
    return new_fail, fixed, counts


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("-j", type=int, default=3, help="scenes run in parallel (default 3)")
    ap.add_argument("--quick", action="store_true", help="one scenario per area, 45 s each")
    ap.add_argument("--duration", type=int, default=0, help="seconds per scene (default 75, --quick 45)")
    ap.add_argument("--asan", action="store_true", help="use (and build) the ASan build")
    ap.add_argument("--ubsan", action="store_true", help="use (and build) the UBSan build")
    ap.add_argument("--no-build", action="store_true", help="do not run build_aurora.py first")
    ap.add_argument("--only", nargs="*", help="run only these scenes (boot, newgame, or area,scenario)")
    ap.add_argument("--exe", help="test this sms_port.exe (and the DLLs next to it) instead of building")
    ap.add_argument("--repeat", type=int, default=1,
                    help="run each selected scene N times (for intermittent crashes)")
    ap.add_argument("--no-seed", action="store_true",
                    help="run without the pipeline-cache seed (initial_pipeline_cache.db)")
    ap.add_argument("--list", action="store_true", help="print the scenes and exit")
    ap.add_argument("--keep-cache", action="store_true", help="keep each scene's pipeline cache")
    # w13 (BOOTLOG "Regional behaviour (w13)"): region builds and their discs.
    ap.add_argument("--region", choices=("us", "jp", "eu"), default="us",
                    help="w13: test that region's build (port/build_aurora_jp/sms_port_jp.exe, ...); "
                         "use --disc for that region's disc")
    ap.add_argument("--disc", help="w13: game data to run (extracted files/ folder or a .iso/.gcm image); "
                                   "default %%SMS_PORT_DISC%% or dolphin_test/files")
    a = ap.parse_args()
    global DISC, EXE_NAME
    if a.disc:
        DISC = Path(a.disc).resolve()
    EXE_NAME = Path(a.exe).name if a.exe else {"us": "sms_port.exe", "jp": "sms_port_jp.exe",
                                                 "eu": "sms_port_eu.exe"}[a.region]
    a.duration = a.duration or (45 if a.quick else 75)
    san = [s for s in ("asan", "ubsan") if getattr(a, s)]
    a.build_dir = ("port/build_aurora" + ("" if a.region == "us" else "_" + a.region)
                   + "".join("_" + s for s in san))
    out_build = ROOT / a.build_dir
    scenes = scene_list(a.quick)
    if a.only:
        scenes = [s for s in scenes if s["name"] in a.only]
    if a.repeat > 1:
        scenes = [dict(s, name=f"{s['name']}_r{i + 1}") for s in scenes for i in range(a.repeat)]
    if a.list:
        for s in scenes:
            print(f"{s['name']:8} {s.get('label', s['kind'])}")
        print(f"{len(scenes)} scenes")
        return 0
    if a.exe:
        a.no_build = True
        out_build = Path(a.exe).resolve().parent
    if not a.no_build:
        cmd = [sys.executable, str(TOOLS / "build_aurora.py")] + ["--" + s for s in san] + ["--region", a.region]
        p = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
        tail = (p.stdout + p.stderr).strip().splitlines()[-3:]
        print("build:", " / ".join(tail))
        if "exe: yes" not in p.stdout:
            print("build failed; see", a.build_dir + "/build.log")
            return 2
    if not (out_build / EXE_NAME).exists():
        print("no exe at", out_build / EXE_NAME)
        return 2
    out = (ROOT / a.build_dir / "smoke_ext") if a.exe else out_build / "smoke"
    out.mkdir(parents=True, exist_ok=True)
    exe = snapshot_bin(out_build, out, seed=not a.no_seed)
    prev = None
    if (out / "report.json").exists():
        shutil.copy2(out / "report.json", out / "report_prev.json")
    if (out / "report_prev.json").exists():
        prev = json.loads((out / "report_prev.json").read_text(encoding="utf-8"))
    print(f"{len(scenes)} scenes x {a.duration} s, {a.j} at a time (about "
          f"{len(scenes) * (a.duration + 5) / a.j / 60:.0f} min)")
    t0 = time.time()
    lock = threading.Lock()
    results = []

    def one(s):
        try:
            r = run_scene(s, exe, out / s["name"].replace(",", "_"), max(a.duration, s.get("min_duration", 0)),
                          bool(san), a.keep_cache)
        except Exception as e:  # a harness problem must not stop the other scenes
            r = dict(name=s["name"], kind=s["kind"], label=s.get("label", s["name"]), status="FAIL",
                     frames=0, last_state="-", aurora_errors=0, aurora_warnings=0, underruns=0, drops=0,
                     sanitizer_reports=0, why=[f"harness error: {e!r}"], excerpt=[], shots=[], seconds=0)
        with lock:
            results.append(r)
            print(f"[{len(results):3}/{len(scenes)}] {s['name']:7} {r['status']:5} frames {r['frames']:5} "
                  f"{r['label']} {('- ' + r['why'][0][:90]) if r['why'] else ''}", flush=True)
        return r

    with concurrent.futures.ThreadPoolExecutor(max(1, a.j)) as ex:
        list(ex.map(one, scenes))
    new_fail, fixed, counts = write_report(out, results, prev, a, time.time() - t0)
    print(f"PASS {counts['PASS']}  FAIL {counts['FAIL']}  STUCK {counts['STUCK']}"
          + (f"  NEW FAILURES: {', '.join(r['name'] for r in new_fail)}" if new_fail else ""))
    print("report:", (out / "report.md").relative_to(ROOT).as_posix())
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
