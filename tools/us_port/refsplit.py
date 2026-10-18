#!/usr/bin/env python3
"""Post-pass for GMSE01 splits: use code-reference evidence from the US DOL to
(1) assign US-only functions at file boundaries to the right file, and
(2) place data-section file boundaries so each file contains the addresses its own code uses.
usage: refsplit.py splits.txt symbols.txt us_refs.json usana_symbols.txt main.dol us_sections.txt"""
import re, sys, json, bisect, collections, struct

SPL, SYM, REFS, USANA, DOL, SECS = sys.argv[1:7]

dol = open(DOL, 'rb').read()
DS = []
for i in range(18):
    o, a, z = [struct.unpack('>I', dol[k + i * 4:k + i * 4 + 4])[0] for k in (0, 0x48, 0x90)]
    if z:
        DS.append((a, z, o))


def rw(a):
    for s, z, o in DS:
        if s <= a < s + z - 3:
            return struct.unpack('>I', dol[o + a - s:o + a - s + 4])[0]
    return None


USSEC = {}
for l in open(SECS):
    n, a, s = l.split()
    USSEC['.sdata2' if n == '.data9' else n] = (int(a, 16), int(a, 16) + int(s, 16))

lines = open(SPL).read().split('\n')
ent = []
cur = None
for i, l in enumerate(lines):
    m = re.match(r'^(\S.*):$', l)
    if m:
        cur = m[1]
        continue
    m = re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)(.*)$', l)
    if m and cur != 'Sections':
        ent.append(dict(sec=m[1], start=int(m[2], 16), end=int(m[3], 16), rest=m[4], line=i, tu=cur))

R = json.load(open(REFS))['refs']
funcs = []
for l in open(USANA):
    m = re.match(r'(\S+) = \.text:0x([0-9A-F]+); // type:function size:0x([0-9A-F]+)', l)
    if m:
        funcs.append((int(m[2], 16), int(m[3], 16), R.get(m[1], [])))
funcs.sort()

known = set()
kname = {}
for l in open(SYM):
    m = re.match(r'(\S+) = \.text:0x([0-9A-F]+); // type:function', l)
    if m and not m[1].startswith('fn_'):
        known.add(int(m[2], 16))
        kname[int(m[2], 16)] = m[1]


def text_ranges():
    return sorted((e['start'], e['end'], e['tu']) for e in ent if e['sec'] == '.text')


def data_owner_map():
    m = {}
    for e in ent:
        if e['sec'] in ('.text', '.init', 'extab', 'extabindex'):
            continue
        m.setdefault(e['sec'], []).append((e['start'], e['end'], e['tu']))
    for v in m.values():
        v.sort()
    return m


def owner_of(dm, a):
    for sec, v in dm.items():
        s0, s1 = USSEC.get(sec, (0, 0))
        if s0 <= a < s1:
            k = bisect.bisect_right([x[0] for x in v], a) - 1
            if k >= 0 and v[k][0] <= a < v[k][1]:
                return v[k][2]
    return None


# data words that point into .text (vtables, function tables)
ptr_from = collections.defaultdict(list)
for sec in ('.data', '.sdata', '.rodata', '.sdata2'):
    a0, a1 = USSEC[sec]
    for p in range(a0, a1 - 3, 4):
        w = rw(p)
        if w and USSEC['.text'][0] <= w < USSEC['.text'][1]:
            ptr_from[w].append(p)

# static initializers = targets of the US constructor table
sinits = set()
c0, c1 = USSEC['.ctors']
for a in range(c0, c1, 4):
    w = rw(a)
    if w and USSEC['.text'][0] <= w < USSEC['.text'][1]:
        sinits.add(w)

# ---- (1) text boundaries: US-only functions between two files go where the evidence points
dm = data_owner_map()
tr = text_ranges()
moved = 0
for (a0, a1, A), (b0, b1, B) in zip(tr, tr[1:]):
    inA = [f for f in funcs if a0 <= f[0] < a1]
    inB = [f for f in funcs if b0 <= f[0] < b1]
    lastA = max([f[0] for f in inA if f[0] in known], default=None)
    firstB = min([f[0] for f in inB if f[0] in known], default=None)
    if lastA is None or firstB is None:
        continue
    gap = [f for f in funcs if lastA < f[0] < firstB and f[0] not in known]
    if not gap:
        continue

    def aff(f):
        sa = sb = 0
        for r in f[2]:
            # only constants are trustworthy here (writable data ranges may themselves be wrong)
            if not any(USSEC[x][0] <= r < USSEC[x][1] for x in ('.rodata', '.sdata2')):
                continue
            o = owner_of(dm, r)
            if o == A:
                sa += 2
            elif o == B:
                sb += 2
        for p in ptr_from.get(f[0], []):
            o = owner_of(dm, p)
            if o == A:
                sa += 3
            elif o == B:
                sb += 3
        return sa, sb

    # an unnamed static initializer inside the gap ends file A (with any 8-byte thunks right after it)
    si = [i for i, f in enumerate(gap) if f[0] in sinits]
    if si and not any(f[0] in sinits for f in inA if f[0] <= lastA):
        j = si[0] + 1
        while j < len(gap) and gap[j][1] == 8:
            j += 1
        newb = gap[j][0] if j < len(gap) else firstB
        if newb != b0:
            for e in ent:
                if e['sec'] == '.text' and e['tu'] == A:
                    e['end'] = newb
                if e['sec'] == '.text' and e['tu'] == B:
                    e['start'] = newb
            moved += 1
        continue
    # a file's static initializer and its thunks are always emitted last: nothing after them belongs to A
    if any(kname.get(f[0], '').startswith('__sinit_') or f[0] in sinits for f in inA if f[0] <= lastA):
        newb = gap[0][0]
        if newb != b0:
            for e in ent:
                if e['sec'] == '.text' and e['tu'] == A:
                    e['end'] = newb
                if e['sec'] == '.text' and e['tu'] == B:
                    e['start'] = newb
            moved += 1
        continue
    affs = [aff(f) for f in gap]
    best = None
    for k in range(len(gap) + 1):  # first k functions to A, the rest to B
        score = sum(x[0] for x in affs[:k]) + sum(x[1] for x in affs[k:])
        # ties: prefer giving functions to A (US-only code is usually appended inside a file)
        if best is None or score > best[0] or (score == best[0] and k > best[1]):
            best = (score, k)
    k = best[1]
    newb = gap[k][0] if k < len(gap) else firstB
    if newb != b0:
        for e in ent:
            if e['sec'] == '.text' and e['tu'] == A:
                e['end'] = newb
            if e['sec'] == '.text' and e['tu'] == B:
                e['start'] = newb
        moved += 1
print('text boundaries moved', moved)

# ---- (2) data boundaries from code-reference ownership
tr = text_ranges()
tst = [x[0] for x in tr]


def fn_tu(a):
    k = bisect.bisect_right(tst, a) - 1
    return tr[k][2] if k >= 0 and tr[k][0] <= a < tr[k][1] else None


refown = collections.defaultdict(collections.Counter)
for a, z, refs in funcs:
    t = fn_tu(a)
    if not t:
        continue
    for r in refs:
        refown[r][t] += 1

symiv = collections.defaultdict(list)
for l in open(SYM):
    m = re.match(r'(\S+) = (\S+):0x([0-9A-F]+);.*?size:0x([0-9A-F]+)', l)
    if m and int(m[4], 16) > 0:
        symiv[m[2]].append((int(m[3], 16), int(m[3], 16) + int(m[4], 16)))
for v_ in symiv.values():
    v_.sort()


def snap(sec, nb, L, Rr):
    iv = symiv.get(sec, [])
    k = bisect.bisect_right([x[0] for x in iv], nb) - 1
    if k >= 0 and iv[k][0] < nb < iv[k][1]:
        s_, e_ = iv[k]
        if (L is None or s_ > L) and s_ != nb:
            return s_
        if Rr is None or e_ <= Rr:
            return e_
    return nb


# PAL ownership of uniquely named symbols, and dtk file-start markers
import os
_cfg = os.path.dirname(os.path.dirname(SPL))
PAL_SPL = os.path.join(_cfg, 'GMSP01', 'splits.txt')
PAL_SYM = os.path.join(_cfg, 'GMSP01', 'symbols.txt')
prng = collections.defaultdict(list)
_c = None
for l in open(PAL_SPL):
    m = re.match(r'^(\S.*):$', l)
    if m:
        _c = m[1]
        continue
    m = re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)', l)
    if m and _c != 'Sections':
        prng[m[1]].append((int(m[2], 16), int(m[3], 16), _c))
for v_ in prng.values():
    v_.sort()


def pal_owner(sec, a):
    v_ = prng.get(sec, [])
    k_ = bisect.bisect_right([x[0] for x in v_], a) - 1
    return v_[k_][2] if k_ >= 0 and v_[k_][0] <= a < v_[k_][1] else None


def load_syms(p):
    d_ = collections.defaultdict(list)
    for l in open(p):
        m = re.match(r'(\S+) = (\S+):0x([0-9A-F]+);(.*)', l)
        if m and not m[1].startswith(('lbl_', 'fn_', '...')):
            z = re.search(r'size:0x([0-9A-F]+)', m[4])
            d_[(m[1], m[2])].append((int(m[3], 16), int(z[1], 16) if z else 0))
    return d_


PS_ = load_syms(PAL_SYM)
US_ = load_syms(SYM)
own_anchors = collections.defaultdict(list)  # sec -> (start, end, tu)
for (n, sec), pa in PS_.items():
    if len(pa) != 1 or len(US_.get((n, sec), [])) != 1:
        continue
    t = pal_owner(sec, pa[0][0])
    if t:
        ua, uz = US_[(n, sec)][0]
        own_anchors[sec].append((ua, ua + max(uz, 1), t))
for l in open(SYM):
    m = re.match(r'^\.\.\.(\S+)\.0 = (\S+):0x([0-9A-F]+);', l)
# (markers are applied in fixups.py)

changed = 0
for sec in ('.rodata', '.sdata2', '.sdata', '.data', '.bss', '.sbss'):
    v = [e for e in ent if e['sec'] == sec]
    v.sort(key=lambda e: e['start'])
    tuix = {}
    for i, e in enumerate(v):
        tuix.setdefault(e['tu'], i)
    s0, s1 = USSEC[sec]
    anc = []
    for r, c in refown.items():
        if not (s0 <= r < s1):
            continue
        t, n = c.most_common(1)[0]
        # constants are file-local; for writable data only trust addresses used by a single file
        if t in tuix and n >= 0.8 * sum(c.values()) and (sec in ('.rodata', '.sdata2') or len(c) == 1):
            anc.append((r, tuix[t]))
    # ownership anchors: symbol start (right constraint) and last byte (left constraint)
    for ua, ue, t in own_anchors.get(sec, []):
        if t in tuix:
            anc.append((ua, tuix[t]))
            anc.append((ue - 1, tuix[t]))
    anc.sort()
    # keep only anchors consistent with file order (longest non-decreasing subsequence)
    tails = []
    tid = []
    prev = [-1] * len(anc)
    for i, (r, ti) in enumerate(anc):
        k = bisect.bisect_right(tails, ti)
        if k == len(tails):
            tails.append(ti)
            tid.append(i)
        else:
            tails[k] = ti
            tid[k] = i
        prev[i] = tid[k - 1] if k > 0 else -1
    keep = []
    k = tid[-1] if tid else -1
    while k >= 0:
        keep.append(anc[k])
        k = prev[k]
    keep.reverse()
    lo = {}
    hi = {}
    for r, ti in keep:
        lo[ti] = min(lo.get(ti, r), r)
        hi[ti] = max(hi.get(ti, r), r)
    for i in range(len(v) - 1):
        b = v[i]['end']
        L = max([hi[j] for j in range(0, i + 1) if j in hi], default=None)
        Rr = min([lo[j] for j in range(i + 1, len(v)) if j in lo], default=None)
        nb = b
        if Rr is not None and nb > Rr:
            nb = Rr
        if L is not None and nb <= L:
            nb = L + 1
            nb = (nb + 3) & ~3
            if Rr is not None and nb > Rr:
                nb = Rr
        nb = snap(sec, nb, L, Rr)
        nb = (nb + 3) & ~3
        if nb != b:
            v[i]['end'] = nb
            v[i + 1]['start'] = nb
            changed += 1
    for e in v:
        if e['end'] < e['start']:
            e['end'] = e['start']
print('data boundaries moved', changed)

for e in ent:
    lines[e['line']] = f"\t{e['sec']:<11} start:0x{e['start']:08X} end:0x{e['end']:08X}{e['rest']}"
open(SPL, 'w').write('\n'.join(lines))

