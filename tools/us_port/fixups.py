#!/usr/bin/env python3
"""Final GMSE01 fixups after refsplit.py:
 - trim data symbols that run past their file's end (sizes don't change bytes)
 - reassign .ctors slots from the actual constructor pointers in the US DOL
usage: fixups.py splits.txt symbols.txt main.dol us_sections.txt"""
import re,sys,bisect,collections,struct
SPL,SYM,DOL,SECS=sys.argv[1:5]
dol=open(DOL,'rb').read()
DS=[]
for i in range(18):
    o,a,z=[struct.unpack('>I',dol[k+i*4:k+i*4+4])[0] for k in (0,0x48,0x90)]
    if z: DS.append((a,z,o))
def rw(a):
    for s,z,o in DS:
        if s<=a<s+z-3: return struct.unpack('>I',dol[o+a-s:o+a-s+4])[0]
USSEC={}
for l in open(SECS):
    n,a,s=l.split(); USSEC['.sdata2' if n=='.data9' else n]=(int(a,16),int(a,16)+int(s,16))
lines=open(SPL).read().split('\n'); ent=[];cur=None;tuline={}
for i,l in enumerate(lines):
    m=re.match(r'^(\S.*):$',l)
    if m: cur=m[1]; tuline[cur]=i; continue
    m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)(.*)$',l)
    if m and cur!='Sections': ent.append(dict(sec=m[1],start=int(m[2],16),end=int(m[3],16),rest=m[4],line=i,tu=cur))
# ---- ctors
c0,c1=USSEC['.ctors']
text=sorted((e['start'],e['end'],e['tu']) for e in ent if e['sec']=='.text'); ts=[t[0] for t in text]
def tu_of(a):
    k=bisect.bisect_right(ts,a)-1
    return text[k][2] if k>=0 and text[k][0]<=a<text[k][1] else None
slot={}
for a in range(c0,c1,4):
    p=rw(a)
    if p and USSEC['.text'][0]<=p<USSEC['.text'][1]:
        t=tu_of(p)
        if t and t not in slot: slot[t]=a
first_ctor=min(e['start'] for e in ent if e['sec']=='.ctors')
removed=added=moved=0
drop=set()
have=set()
for e in ent:
    if e['sec']=='.ctors' and e['start']==first_ctor: have.add(e['tu'])
    if e['sec']!='.ctors' or e['start']==first_ctor: continue   # first entry = __init_cpp_exceptions reference
    if e['tu'] in slot:
        if e['start']!=slot[e['tu']]: moved+=1
        e['start']=slot[e['tu']]; e['end']=e['start']+4; have.add(e['tu'])
    else:
        drop.add(e['line']); removed+=1; print('ctors removed for',e['tu'])
for e in ent:
    if e['line'] not in drop: lines[e['line']]=f"\t{e['sec']:<11} start:0x{e['start']:08X} end:0x{e['end']:08X}{e['rest']}"
inserts=[]
for t,a in slot.items():
    if t in have: continue
    # insert after the TU's .text line
    tl=[e for e in ent if e['tu']==t and e['sec']=='.text'][0]['line']
    inserts.append((tl,f"\t.ctors      start:0x{a:08X} end:0x{a+4:08X}")); added+=1; print('ctors added for',t,hex(a))
for i in sorted(drop,reverse=True): lines[i]=None
for tl,txt in sorted(inserts,reverse=True): lines.insert(tl+1,txt)
lines=[l for l in lines if l is not None]
open(SPL,'w').write('\n'.join(lines))
print('ctors moved',moved,'removed',removed,'added',added)
# ---- file-start markers: dtk's '...<sec>.0' labels sit exactly at each original file's start
marks=collections.defaultdict(list)
for l in open(SYM):
    m=re.match(r'^\.\.\.(\S+)\.0 = (\S+):0x([0-9A-F]+);',l)
    if m: marks[m[2]].append(int(m[3],16))
ent=[];cur=None
for i,l in enumerate(lines):
    m=re.match(r'^(\S.*):$',l)
    if m: cur=m[1]; continue
    m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)(.*)$',l)
    if m and cur!='Sections': ent.append(dict(sec=m[1],start=int(m[2],16),end=int(m[3],16),rest=m[4],line=i,tu=cur))
mfix=0
for sec,ms in marks.items():
    if sec in ('.bss','.sbss'): continue   # bss markers were placed by the weakest method; bss_refs.py handles bss
    v=sorted([e for e in ent if e['sec']==sec],key=lambda e:e['start'])
    for k,e in enumerate(v):
        inner=sorted(a for a in ms if e['start']<a<e['end'])
        if inner and k+1<len(v):
            nb=inner[0]
            e['end']=nb; v[k+1]['start']=min(v[k+1]['start'],nb); mfix+=1
            print('marker boundary',sec,e['tu'],'->',hex(nb))
for e in ent: lines[e['line']]=f"\t{e['sec']:<11} start:0x{e['start']:08X} end:0x{e['end']:08X}{e['rest']}"
open(SPL,'w').write('\n'.join(lines)); print('marker fixes',mfix)
# ---- trim symbols crossing file ends
spl=collections.defaultdict(list);cur=None
for l in lines:
    m=re.match(r'^(\S.*):$',l)
    if m: cur=m[1]; continue
    m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)',l)
    if m and cur!='Sections': spl[m[1]].append((int(m[2],16),int(m[3],16)))
for v in spl.values(): v.sort()
S=open(SYM).read().split('\n'); n=0
for i,l in enumerate(S):
    m=re.match(r'(\S+) = (\S+):0x([0-9A-F]+);.*size:0x([0-9A-F]+)',l)
    if not m or m[2] in ('.text','.init'): continue
    a=int(m[3],16); z=int(m[4],16); v=spl.get(m[2],[])
    k=bisect.bisect_right([x[0] for x in v],a)-1
    if k>=0 and v[k][0]<=a<v[k][1] and a+z>v[k][1]:
        S[i]=l.replace(f'size:0x{m[4]}',f'size:0x{v[k][1]-a:X}'); n+=1
open(SYM,'w').write('\n'.join(S)); print('symbols trimmed',n)
