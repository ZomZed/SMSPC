import re,sys,bisect,collections
sy,sp=sys.argv[1:3]
spl=collections.defaultdict(list);cur=None
for l in open(sp):
    m=re.match(r'^(\S.*):$',l)
    if m: cur=m[1]; continue
    m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)',l)
    if m and cur!='Sections': spl[m[1]].append((int(m[2],16),int(m[3],16),cur))
for v in spl.values(): v.sort()
syms=collections.defaultdict(list)
for l in open(sy):
    m=re.match(r'(\S+) = (\S+):0x([0-9A-F]+);.*?(?:size:0x([0-9A-F]+))?(?: |$)',l)
    m=re.match(r'(\S+) = (\S+):0x([0-9A-F]+);(.*)',l)
    if m:
        z=re.search(r'size:0x([0-9A-F]+)',m[4]); syms[m[2]].append((int(m[3],16),int(z[1],16) if z else 0,m[1]))
probs=collections.Counter(); ex=collections.defaultdict(list)
for sec,ss in syms.items():
    ss.sort(); v=spl.get(sec,[]); st=[a for a,b,n in v]
    for a,z,n in ss:
        if z==0: continue
        k=bisect.bisect_right(st,a)-1
        if k>=0 and v[k][0]<=a<v[k][1] and a+z>v[k][1]: probs['ends_within',sec]+=1; ex[sec].append(f'{n} {a:08X}+{z:X} split {v[k][2]} end {v[k][1]:08X}')
        if k+1<len(v) and a<v[k+1][0]<a+z: probs['spans_split',sec]+=1; ex[sec].append(f'{n} {a:08X}+{z:X} spans into {v[k+1][2]}')
    for (a,z,n),(b,y,m) in zip(ss,ss[1:]):
        if z and a+z>b and b!=a: probs['overlap',sec]+=1
print(dict(probs))
for sec,e in ex.items(): print(sec,len(e),e[:3])
