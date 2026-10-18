#!/usr/bin/env python3
"""Cover data bytes that no symbol covers (the linker drops them otherwise).
A gap is left alone only when it is pure alignment padding: zero bytes and
exactly what the next symbol's alignment requires.
usage: fill_gaps.py splits.txt symbols.txt main.dol us_sections.txt"""
import re,sys,struct,bisect,collections
SPL,SYM,DOL,SECS=sys.argv[1:5]
dol=open(DOL,'rb').read(); DS=[]
for i in range(18):
    o,a,z=[struct.unpack('>I',dol[k+i*4:k+i*4+4])[0] for k in (0,0x48,0x90)]
    if z: DS.append((a,z,o))
def rb(a):
    for s,z,o in DS:
        if s<=a<s+z: return dol[o+a-s]
    return 0
USSEC={}
for l in open(SECS):
    n,a,s=l.split(); USSEC['.sdata2' if n=='.data9' else n]=(int(a,16),int(a,16)+int(s,16))
spl=collections.defaultdict(list);cur=None;salign={};common_rng=set()
for l in open(SPL):
    m=re.match(r'^(\S.*):$',l)
    if m: cur=m[1]; continue
    m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)(.*)',l)
    if m and cur!='Sections':
        spl[m[1]].append((int(m[2],16),int(m[3],16)))
        if 'common' in m[4]: common_rng.add((m[1],int(m[2],16)))
    m=re.match(r'^\t(\S+)\s+type:\S+ align:(\d+)',l)
    if m and cur=='Sections': salign[m[1]]=int(m[2])
for v in spl.values(): v.sort()
L=open(SYM).read().split('\n'); syms=collections.defaultdict(list)
for i,l in enumerate(L):
    m=re.match(r'(\S+) = (\S+):0x([0-9A-F]+);(.*)',l)
    if not m: continue
    z=re.search(r'size:0x([0-9A-F]+)',m[4]); al=re.search(r'align:(\d+)',m[4])
    syms[m[2]].append((int(m[3],16),int(z[1],16) if z else 0,int(al[1]) if al else 4,m[1]))
new=[];n=0;extend={}
for sec in ('.rodata','.data','.sdata','.sdata2','.bss','.sbss'):
    S=sorted(s for s in syms.get(sec,[]) if s[1]>0)
    starts=[x[0] for x in spl[sec]]
    def file_of(a):
        k=bisect.bisect_right(starts,a)-1
        return k if k>=0 and spl[sec][k][0]<=a<spl[sec][k][1] else None
    # walk each file range separately
    for (fs,fe) in spl[sec]:
        is_common=(sec,fs) in common_rng
        if is_common: continue   # mwld lays out commons itself (first common reserves the whole block)
        inside=[s for s in S if fs<=s[0]<fe]
        pos=fs; last_sym=None; last_start=None
        for a,z,al,nm in inside+[(fe,0,1,None)]:
            if a>pos:
                need=(pos+al-1)//al*al if nm else pos
                zero=sec in ('.bss','.sbss') or all(rb(x)==0 for x in range(pos,a))
                if not (zero and (need==a or nm is None and a-pos<8)):
                    fa=(pos+3)&~3 if (pos%4 and a-((pos+3)&~3)>0) else pos
                    if a>fa and is_common and last_sym is not None:
                        extend[last_sym]=a-last_start; n+=1
                    elif a>fa:
                        new.append(f"lbl_{fa:08X} = {sec}:0x{fa:08X}; // type:object size:0x{a-fa:X} scope:{'global' if is_common else 'local'} align:{4 if fa%4==0 else 1}"); n+=1
            if nm: last_sym=nm; last_start=a
            pos=max(pos,a+z)
for i,l in enumerate(L):
    m=re.match(r'(\S+) = (\.s?bss):0x([0-9A-F]+); // (.*)size:0x([0-9A-F]+)(.*)',l)
    if m and m[1] in extend: L[i]=f"{m[1]} = {m[2]}:0x{m[3]}; // {m[4]}size:0x{extend[m[1]]:X}{m[6]}"
L+= new
open(SYM,'w').write('\n'.join(l for l in L if l is not None)); print('gap fillers added',n)
