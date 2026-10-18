import re,bisect,collections,sys
def rngs(p):
    r=collections.defaultdict(list);cur=None
    for l in open(p):
        m=re.match(r'^(\S.*):$',l)
        if m: cur=m[1]; continue
        m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)',l)
        if m and cur!='Sections': r[m[1]].append((int(m[2],16),int(m[3],16),cur))
    for v in r.values(): v.sort()
    return r
def owner(r,sec,a):
    v=r.get(sec,[]); k=bisect.bisect_right([x[0] for x in v],a)-1
    return v[k][2] if k>=0 and v[k][0]<=a<v[k][1] else None
def syms(p):
    d={}
    for l in open(p):
        m=re.match(r'(\S+) = (\S+):0x([0-9A-F]+);',l)
        if m and not m[1].startswith(('lbl_','fn_','@','...')) : d.setdefault((m[1],m[2]),[]).append(int(m[3],16))
    return d
PR=rngs('sms/config/GMSP01/splits.txt'); UR=rngs('sms/config/GMSE01/splits.txt')
PS=syms('sms/config/GMSP01/symbols.txt'); US=syms('sms/config/GMSE01/symbols.txt')
mis=collections.Counter(); ex=collections.defaultdict(list); tot=collections.Counter()
for (n,sec),pa in PS.items():
    if len(pa)!=1 or (n,sec) not in US or len(US[(n,sec)])!=1 or sec in ('.text','.init','extab','extabindex'): continue
    po=owner(PR,sec,pa[0]); uo=owner(UR,sec,US[(n,sec)][0])
    tot[sec]+=1
    if po and uo and po!=uo: mis[sec]+=1; ex[sec].append((n,po.split('/')[-1],'->',uo.split('/')[-1],hex(US[(n,sec)][0])))
print({s:f'{mis[s]}/{tot[s]}' for s in tot})
for s,e in ex.items(): print(s,e[:6])
