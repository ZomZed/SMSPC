#!/usr/bin/env python3
"""Turn dtk-generated lbl_ labels in .bss/.sbss into sized, aligned objects.
Unsized labels with alignment 0 in uninitialized sections make mwldeppc hang."""
import re,sys
p=sys.argv[1]
import bisect,collections,os
_spl=collections.defaultdict(list); _common=[]
_sp=os.path.join(os.path.dirname(p),'splits.txt'); _cur=None
for _l in open(_sp):
    _m=re.match(r'^(\S.*):$',_l)
    if _m: _cur=_m[1]; continue
    _m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)',_l)
    if _m and _cur!='Sections':
        _spl[_m[1]].append((int(_m[2],16),int(_m[3],16)))
        if 'common' in _l: _common.append((_m[1],int(_m[2],16),int(_m[3],16)))
for _v in _spl.values(): _v.sort()
def split_end(sec,a):
    v=_spl.get(sec,[]); k=bisect.bisect_right([x[0] for x in v],a)-1
    return v[k][1] if k>=0 and v[k][0]<=a<v[k][1] else None
L=open(p).read().split('\n')
pat=re.compile(r'^(\S+) = (\.s?bss|\.rodata|\.s?data2?):0x([0-9A-F]+); //(.*)$')
syms=[]
for i,l in enumerate(L):
    m=re.match(r'^(\S+) = (\S+):0x([0-9A-F]+);',l)
    if m: syms.append((m[2],int(m[3],16),i))
nxt={}
by={}
for sec,a,i in syms: by.setdefault(sec,[]).append((a,i))
for sec,v in by.items():
    v.sort()
    for k,(a,i) in enumerate(v):
        j=k+1
        while j<len(v) and v[j][0]<=a: j+=1
        nxt[i]=v[j][0] if j<len(v) else None
fixed=0
for i,l in enumerate(L):
    m=pat.match(l)
    if not m or 'type:label' not in m[4] or m[1].startswith('...'): continue
    a=int(m[3],16); n=nxt.get(i)
    if any(cs==m[2] and c0<=a<c1 for cs,c0,c1 in _common): continue   # never inside mwld-managed common blocks
    if n is None: continue
    se=split_end(m[2],a)
    if se is not None: n=min(n,se)
    size=n-a
    if size<=0: continue
    al=4 if a%4==0 and size>=4 else (2 if a%2==0 and size>=2 else 1)
    L[i]=f"{m[1]} = {m[2]}:0x{a:08X}; // type:object size:0x{size:X} scope:global align:{al}"
    fixed+=1
for i,l in enumerate(L):
    m=re.match(r'^(lbl_\S+) = (\S+):0x([0-9A-F]+); // type:object size:0x([0-9A-F]+)(.*)$',l)
    if m and m[2]!='.text':
        a=int(m[3],16); z=int(m[4],16); se=split_end(m[2],a)
        if se is not None and a+z>se: L[i]=f"{m[1]} = {m[2]}:0x{a:08X}; // type:object size:0x{se-a:X}{m[5]}"; fixed+=1
open(p,'w').write('\n'.join(L)); print('labels converted',fixed)
