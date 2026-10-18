# Decode US DOL: per-function data references (lis/addi|ori|D-form, r2/r13 relative)
import struct,sys,json,re
dol=open(sys.argv[1],'rb').read(); syms=sys.argv[2]; out=sys.argv[3]
h=dol[:0x100]
secs=[]
for i in range(18):
    off,addr,size=[struct.unpack('>I',h[k+i*4:k+i*4+4])[0] for k in (0,0x48,0x90)]
    if size: secs.append((addr,size,off))
def rd(a):
    for s,z,o in secs:
        if s<=a<s+z: return struct.unpack('>I',dol[o+a-s:o+a-s+4])[0]
    return None
funcs=[]
for l in open(syms):
    m=re.match(r'(\S+) = \.(?:text|init):0x([0-9A-F]+); // type:function size:0x([0-9A-F]+)',l)
    if m: funcs.append((m[1],int(m[2],16),int(m[3],16)))
# SDA bases from __init_registers: lis r2/r13 + ori/addi
def sx(v): return v-0x10000 if v&0x8000 else v
R2=R13=None
for n,a,z in funcs:
    if n=='__init_registers':
        hi={}
        for p in range(a,a+z,4):
            w=rd(p); op=w>>26; rt=(w>>21)&31; ra=(w>>16)&31; imm=w&0xffff
            if op==15 and ra==0: hi[rt]=imm<<16
            elif op==24 and rt in hi: v=hi[rt]|imm; R2=v if rt==2 else R2; R13=v if rt==13 else R13
            elif op==14 and ra in hi: v=(hi[ra]+sx(imm))&0xffffffff; R2=v if rt==2 else R2; R13=v if rt==13 else R13
print('r2',hex(R2 or 0),'r13',hex(R13 or 0))
DFORM={14,24,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,52,53,54,55}
res={}
for n,a,z in funcs:
    hi={}; refs=set()
    for p in range(a,a+z,4):
        w=rd(p)
        if w is None: break
        op=w>>26; rt=(w>>21)&31; ra=(w>>16)&31; imm=w&0xffff
        if op==15 and ra==0: hi[rt]=imm<<16; continue
        if op in DFORM:
            if ra==2 and R2 and op!=14 or (op==14 and ra==2 and R2): refs.add((R2+sx(imm))&0xffffffff)
            elif ra==13 and R13: refs.add((R13+sx(imm))&0xffffffff)
            elif ra in hi and ra!=0:
                v=(hi[ra]+(imm if op==24 else sx(imm)))&0xffffffff
                if 0x80000000<=v<0x81800000: refs.add(v)
        if op in (14,15,24,32,34,40,42) and rt in hi and op!=15 and not (ra==rt): hi.pop(rt,None)
        if op==18 or (op==19): hi={} if (w&1)==0 and op==19 else hi
    res[n]=sorted(refs)
json.dump({'r2':R2,'r13':R13,'refs':res},open(out,'w'))
print(len(res), sum(len(v) for v in res.values()))
