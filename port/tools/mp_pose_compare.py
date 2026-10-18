#!/usr/bin/env python3
"""Puppet pose fidelity: compare a ghost recording's Mario with its playback puppet, joint by joint.

Two runs with SMS_PORT_MP_POSE_DUMP (port/src/port_mp_puppet.cpp, "pose-dump harness"):
  1. recording:  SMS_PORT_MP_GHOST=<ghost> SMS_PORT_MP_POSE_DUMP=<rec.pose> + a movement script
  2. playback:   SMS_PORT_MP_GHOST_PLAY=<ghost> SMS_PORT_MP_POSE_DUMP=<play.pose> (same stage)
then:  python port/tools/mp_pose_compare.py <rec.pose> <play.pose> [--offset 150,0,0] [--trans 4] [--rot 3]
                                           [--slot 15] [--joints] [--worst 25] [--csv out.csv]

The recording's local Mario (kind 0, keyed by snapshot seq) is paired with the puppet's frames
(kind 1) through the ghost map (kind 2: local push frame -> recorded seq). For each paired frame the
joint matrices are compared: translation (world units, after the ghost offset) and rotation (degrees
between the rotation parts). Frames whose worst joint exceeds the thresholds are listed in runs with
the Mario state (status, animation ids, frames, blend) for both sides, so the cause can be read off:
a frame-number difference is a timing/packing issue, a different bck is a capture/wire issue, same
inputs with a different matrix is a puppet calc issue.
"""
import argparse
import math
import struct
import sys
from collections import defaultdict

HDR = struct.Struct("<BBHHHIIfIIHHHHHBBBBfffffhhhhhhhhfffBBhhff")
assert HDR.size == 104, HDR.size

STATUS_NAMES = {}


def load(path):
    data = open(path, "rb").read()
    if data[:8] != b"SMSPOSE1":
        sys.exit("%s: not a pose dump" % path)
    off = 8
    recs = []
    while off + HDR.size <= len(data):
        f = HDR.unpack_from(data, off)
        off += HDR.size
        r = dict(kind=f[0], slot=f[1], nBody=f[2], nFludd=f[3], nNozzle=f[4], seqA=f[5], seqB=f[6], t=f[7],
                 status=f[8], flags=f[9], animId=f[10], bck=((f[11], f[12]), (f[13], f[14])), calc=(f[15], f[16]),
                 hand=f[17], btp=f[18], frame=(f[19], f[20], f[21]), blend=(f[22], f[23]), head=f[24:28],
                 waist=f[28:31], pos=f[32:35], nozzle=f[35], ticks=f[36], fluddBck=f[37], nozzleBck=f[38],
                 fluddFrame=f[39], nozzleFrame=f[40])
        n = r["nBody"] + r["nFludd"] + r["nNozzle"]
        need = n * 48
        if off + need > len(data):
            break
        mats = struct.unpack_from("<%df" % (n * 12), data, off)
        off += need
        r["mats"] = [mats[i * 12:(i + 1) * 12] for i in range(n)]
        recs.append(r)
    return recs


def rot_angle(a, b):
    # angle between the rotation parts of two 3x4 matrices (columns normalised: scale removed)
    def cols(m):
        c = []
        for k in range(3):
            v = (m[k], m[4 + k], m[8 + k])
            n = math.sqrt(v[0] ** 2 + v[1] ** 2 + v[2] ** 2) or 1.0
            c.append((v[0] / n, v[1] / n, v[2] / n))
        return c
    ca, cb = cols(a), cols(b)
    tr = sum(ca[k][i] * cb[k][i] for k in range(3) for i in range(3))
    x = max(-1.0, min(1.0, (tr - 1.0) / 2.0))
    return math.degrees(math.acos(x))


def trans_err(a, b, off):
    return math.sqrt((a[3] - b[3] + off[0]) ** 2 + (a[7] - b[7] + off[1]) ** 2 + (a[11] - b[11] + off[2]) ** 2)


def state_str(r):
    return "st %x anim %d bck %d/%d|%d/%d calc %d/%d fr %.1f/%.1f/%.0f bl %.2f/%.2f hand %d btp %d nz %d fl %d/%.0f nzb %d/%.0f" % (
        r["status"], r["animId"], r["bck"][0][0], r["bck"][0][1], r["bck"][1][0], r["bck"][1][1], r["calc"][0],
        r["calc"][1], r["frame"][0], r["frame"][1], r["frame"][2], r["blend"][0], r["blend"][1], r["hand"],
        r["btp"], r["nozzle"], r["fluddBck"], r["fluddFrame"], r["nozzleBck"], r["nozzleFrame"])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rec")
    ap.add_argument("play")
    ap.add_argument("--offset", default="150,0,0", help="ghost offset used in the playback run")
    ap.add_argument("--trans", type=float, default=4.0, help="translation threshold (units)")
    ap.add_argument("--rot", type=float, default=3.0, help="rotation threshold (degrees)")
    ap.add_argument("--slot", type=int, default=15, help="puppet slot (ghost copy 1 = 15)")
    ap.add_argument("--joints", action="store_true", help="per-joint table")
    ap.add_argument("--worst", type=int, default=25, help="how many worst frames to list")
    ap.add_argument("--csv", default="")
    a = ap.parse_args()
    off = tuple(float(x) for x in a.offset.split(","))
    rec = [r for r in load(a.rec) if r["kind"] == 0]
    play = load(a.play)
    bySeq = {r["seqA"]: r for r in rec}
    gmap = {r["seqA"]: r["seqB"] for r in play if r["kind"] == 2}
    pup = [r for r in play if r["kind"] == 1 and r["slot"] == a.slot]
    print("recording: %d local frames (seq %d..%d); playback: %d puppet frames, %d map entries" % (
        len(rec), rec[0]["seqA"] if rec else 0, rec[-1]["seqA"] if rec else 0, len(pup), len(gmap)))
    ticks = defaultdict(int)
    for r in rec:
        ticks[r["ticks"]] += 1
    print("simulation ticks per logic frame in the recording: " + ", ".join(
        "%d x%d" % (k, v) for k, v in sorted(ticks.items())))
    if not pup or not gmap:
        sys.exit("nothing to compare")
    ts = [r["t"] for r in pup]
    mid = sum(1 for t in ts if 0.02 < t < 0.98)
    print("interpolation fraction t: min %.3f max %.3f, %d of %d frames strictly between two snapshots" % (
        min(ts), max(ts), mid, len(ts)))
    rows = []
    perJointT = defaultdict(list)
    perJointR = defaultdict(list)
    missing = 0
    for p in pup:
        srcA, srcB = gmap.get(p["seqA"]), gmap.get(p["seqB"])
        src = srcA if p["t"] < 0.5 else srcB
        if src is None or src not in bySeq:
            missing += 1
            continue
        l = bySeq[src]
        nb = min(l["nBody"], p["nBody"])
        wt = wr = 0.0
        jt = jr = -1
        for j in range(nb):
            te = trans_err(l["mats"][j], p["mats"][j], off)
            re = rot_angle(l["mats"][j], p["mats"][j])
            perJointT[j].append(te)
            perJointR[j].append(re)
            if te > wt:
                wt, jt = te, j
            if re > wr:
                wr, jr = re, j
        # FLUDD and nozzle blocks (when both have them)
        ft = fr = 0.0
        if l["nFludd"] and p["nFludd"]:
            for j in range(min(l["nFludd"], p["nFludd"])):
                la, pa = l["mats"][l["nBody"] + j], p["mats"][p["nBody"] + j]
                ft = max(ft, trans_err(la, pa, off))
                fr = max(fr, rot_angle(la, pa))
        if l["nNozzle"] and p["nNozzle"]:
            for j in range(min(l["nNozzle"], p["nNozzle"])):
                la = l["mats"][l["nBody"] + l["nFludd"] + j]
                pa = p["mats"][p["nBody"] + p["nFludd"] + j]
                ft = max(ft, trans_err(la, pa, off))
                fr = max(fr, rot_angle(la, pa))
        rows.append(dict(src=src, t=p["t"], wt=wt, jt=jt, wr=wr, jr=jr, ft=ft, fr=fr, l=l, p=p))
    print("paired %d frames (%d puppet frames without a recorded frame)" % (len(rows), missing))
    if not rows:
        return
    bad = [r for r in rows if r["wt"] > a.trans or r["wr"] > a.rot]
    fbad = [r for r in rows if r["ft"] > a.trans or r["fr"] > a.rot]
    wts = sorted(r["wt"] for r in rows)
    wrs = sorted(r["wr"] for r in rows)
    print("body: worst-joint translation error per frame: mean %.2f, median %.2f, p95 %.2f, max %.2f units" % (
        sum(wts) / len(wts), wts[len(wts) // 2], wts[int(len(wts) * 0.95)], wts[-1]))
    print("body: worst-joint rotation error per frame:    mean %.2f, median %.2f, p95 %.2f, max %.2f deg" % (
        sum(wrs) / len(wrs), wrs[len(wrs) // 2], wrs[int(len(wrs) * 0.95)], wrs[-1]))
    print("frames over threshold (trans > %.1f or rot > %.1f): %d of %d (%.1f%%); FLUDD/nozzle: %d" % (
        a.trans, a.rot, len(bad), len(rows), 100.0 * len(bad) / len(rows), len(fbad)))
    # runs of bad frames
    runs = []
    for r in bad:
        if runs and r["src"] - runs[-1][-1]["src"] <= 2:
            runs[-1].append(r)
        else:
            runs.append([r])
    print("\n%d runs of frames over threshold:" % len(runs))
    for run in runs:
        w = max(run, key=lambda r: r["wt"])
        statuses = sorted(set("%x" % r["l"]["status"] for r in run))
        print("  seq %d..%d (%d frames): worst trans %.1f (joint %d) rot %.1f deg (joint %d); status %s" % (
            run[0]["src"], run[-1]["src"], len(run), w["wt"], w["jt"], max(r["wr"] for r in run),
            max(run, key=lambda r: r["wr"])["jr"], " ".join(statuses)))
        print("      local : " + state_str(w["l"]))
        print("      puppet: " + state_str(w["p"]))
    print("\nworst %d frames:" % a.worst)
    for r in sorted(rows, key=lambda r: -max(r["wt"], r["wr"]))[:a.worst]:
        print("  seq %d t %.2f: trans %.1f (j%d) rot %.1f (j%d) fludd %.1f/%.1f | local %s | puppet %s" % (
            r["src"], r["t"], r["wt"], r["jt"], r["wr"], r["jr"], r["ft"], r["fr"], state_str(r["l"]),
            state_str(r["p"])))
    if a.joints:
        print("\nper joint: mean / max translation, mean / max rotation")
        for j in sorted(perJointT):
            t, rr = perJointT[j], perJointR[j]
            print("  j%2d: %6.2f %6.2f   %6.2f %6.2f" % (j, sum(t) / len(t), max(t), sum(rr) / len(rr), max(rr)))
    if a.csv:
        with open(a.csv, "w") as f:
            f.write("src,t,trans,joint,rot,rjoint,fludd_t,fludd_r,status,animId,bck0,bck0old,frame0,frame1,p_status,p_animId,p_bck0,p_frame0\n")
            for r in rows:
                f.write("%d,%.3f,%.2f,%d,%.2f,%d,%.2f,%.2f,%x,%d,%d,%d,%.1f,%.1f,%x,%d,%d,%.1f\n" % (
                    r["src"], r["t"], r["wt"], r["jt"], r["wr"], r["jr"], r["ft"], r["fr"], r["l"]["status"],
                    r["l"]["animId"], r["l"]["bck"][0][0], r["l"]["bck"][0][1], r["l"]["frame"][0],
                    r["l"]["frame"][1], r["p"]["status"], r["p"]["animId"], r["p"]["bck"][0][0], r["p"]["frame"][0]))


if __name__ == "__main__":
    main()
