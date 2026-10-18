#!/usr/bin/env python3
"""Place non-common .bss file boundaries from code evidence: MWCC addresses a file's
.bss through its start, so the lowest .bss address used only by that file's code is its start.
usage: bss_refs.py splits.txt us_refs.json usana_symbols.txt us_sections.txt"""
import re,sys,json,bisect,collections
SPL,REFS,USANA,SECS=sys.argv[1:5]
USSEC={}
for l in open(SECS):
    n,a,s=l.split(); USSEC['.sdata2' if n=='.data9' else n]=(int(a,16),int(a,16)+int(s,16))
lines=open(SPL).read().split('\n'); ent=[];cur=None
for i,l in enumerate(lines):
    m=re.match(r'^(\S.*):$',l)
    if m: cur=m[1]; continue
    m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)(.*)$',l)
    if m and cur!='Sections': ent.append(dict(sec=m[1],start=int(m[2],16),end=int(m[3],16),rest=m[4],line=i,tu=cur))
R=json.load(open(REFS))['refs']
funcs=[]
for l in open(USANA):
    m=re.match(r'(\S+) = \.text:0x([0-9A-F]+); // type:function',l)
    if m: funcs.append((int(m[2],16),R.get(m[1],[])))
tr=sorted((e['start'],e['end'],e['tu']) for e in ent if e['sec']=='.text'); ts=[t[0] for t in tr]
def tu_of(a):
    k=bisect.bisect_right(ts,a)-1
    return tr[k][2] if k>=0 and tr[k][0]<=a<tr[k][1] else None
b0,b1=USSEC['.bss']
users=collections.defaultdict(set)
for fa,refs in funcs:
    t=tu_of(fa)
    for r in refs:
        if b0<=r<b1 and t: users[r].add(t)
own=collections.defaultdict(list)
for r,ts_ in users.items():
    if len(ts_)==1: own[next(iter(ts_))].append(r)
v=[e for e in ent if e['sec']=='.bss' and 'common' not in e['rest']]
v.sort(key=lambda e:e['start'])
# proposed starts, then keep only those consistent with file order
prop=[]
for i,e in enumerate(v):
    if own.get(e['tu']): prop.append((i,min(own[e['tu']])))
# LIS on proposed starts by file index order
tails=[];tid=[];prev=[-1]*len(prop)
for j,(i,a) in enumerate(prop):
    k=bisect.bisect_left(tails,a)
    if k==len(tails): tails.append(a); tid.append(j)
    else: tails[k]=a; tid[k]=j
    prev[j]=tid[k-1] if k>0 else -1
keep={};k=tid[-1] if tid else -1
while k>=0: keep[prop[k][0]]=prop[k][1]; k=prev[k]
moved=0
for i,e in enumerate(v):
    if i in keep and i>0 and keep[i]!=e['start']:
        nb=keep[i]&~3
        if nb>v[i-1]['start']:
            v[i-1]['end']=nb; e['start']=nb; moved+=1
for i in range(len(v)-1):
    if v[i]['end']!=v[i+1]['start']: v[i]['end']=v[i+1]['start']
    if v[i]['end']<v[i]['start']: v[i]['end']=v[i]['start']
for e in ent: lines[e['line']]=f"\t{e['sec']:<11} start:0x{e['start']:08X} end:0x{e['end']:08X}{e['rest']}"
open(SPL,'w').write('\n'.join(lines)); print('bss starts from code refs:',len(keep),'moved',moved)
