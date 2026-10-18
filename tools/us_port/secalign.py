# Global stride alignment of a data section: PAL symbols vs US boundaries
import re,sys,json,difflib,struct,collections,bisect
sec=sys.argv[1]
dol=open('us/main.dol','rb').read()
SEC=[]
for i in range(18):
    o,a,z=[struct.unpack('>I',dol[k+i*4:k+i*4+4])[0] for k in (0,0x48,0x90)]
    if z: SEC.append((a,z,o))
def rb(a):
    for s,z,o in SEC:
        if s<=a<s+z: return dol[o+a-s]
    return 0
USSEC={}
for l in open('us_sections.txt'):
    n,a,s=l.split(); USSEC['.sdata2' if n=='.data9' else n]=(int(a,16),int(a,16)+int(s,16))
s0,s1=USSEC[sec]
R=json.load(open('us_refs.json'))
B=set([s0])
for v in R['refs'].values():
    for r in v:
        if s0<=r<s1: B.add(r)
for l in open('usana/symbols.txt'):
    m=re.match(r'\S+ = '+re.escape(sec)+r':0x([0-9A-F]+);',l)
    if m: B.add(int(m[1],16))
# pointer targets inside data (vtables, tables) from .data/.sdata words
for ps in ('.data','.sdata','.rodata','.sdata2'):
    a0,a1=USSEC[ps]
    for p in range(a0,a1-3,4):
        w=(rb(p)<<24)|(rb(p+1)<<16)|(rb(p+2)<<8)|rb(p+3)
        if s0<=w<s1: B.add(w)
if sec not in ('.bss','.sbss'):
    for p in range(s0,s1,4):
        b=rb(p)
        if 32<=b<127 and (p==s0 or rb(p-1)==0): B.add(p)
B=sorted(B); usst=[(B[k+1] if k+1<len(B) else s1)-B[k] for k in range(len(B))]
pal=[]
for l in open('sms/config/GMSP01/symbols.txt'):
    m=re.match(r'(\S+) = '+re.escape(sec)+r':0x([0-9A-F]+);',l)
    if m: pal.append([m[1],int(m[2],16)])
pal.sort(key=lambda x:x[1])
pend=max(a for _,a in pal)+1
pst=[]
for k in range(len(pal)):
    nx=next((pal[j][1] for j in range(k+1,len(pal)) if pal[j][1]>pal[k][1]),None)
    pst.append((nx-pal[k][1]) if nx else 0)
idx=[]; seen=set()
for k in range(len(pal)):
    if pal[k][1] in seen: continue
    seen.add(pal[k][1]); idx.append(k)
sm=difflib.SequenceMatcher(None,[pst[k] for k in idx],usst,autojunk=False)
amap={}
for x,y,n in sm.get_matching_blocks():
    if n>=2:
        for q in range(n): amap[pal[idx[x+q]][1]]=B[y+q]
print(sec,'PAL addrs',len(idx),'US boundaries',len(B),'aligned',len(amap),f'{len(amap)/len(idx):.1%}')
json.dump({str(k):v for k,v in amap.items()},open(f'align_{sec[1:]}.json','w'))
