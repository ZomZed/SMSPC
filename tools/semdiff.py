import sys,subprocess,struct,re,collections
od=r"build\binutils\powerpc-eabi-objdump.exe"
def run(*a): return subprocess.run([od,*a],capture_output=True,text=True,errors="replace").stdout
def secbytes(p):
    secs={}
    cur=None
    for l in run("-s",p).splitlines():
        m=re.match(r"Contents of section (\S+):",l)
        if m: cur=m.group(1); secs[cur]=bytearray(); continue
        m=re.match(r" ([0-9a-f]+) ((?:[0-9a-f]+ ?){1,4})",l)
        if m and cur:
            off=int(m.group(1),16); b=bytearray()
            for w in m.group(2).split(): b+=bytes.fromhex(w)
            if len(secs[cur])<off: secs[cur]+=bytes(off-len(secs[cur]))
            secs[cur][off:off+len(b)]=b
    return secs
def symvals(p):
    secs=secbytes(p); vals={}
    for l in run("-t",p).splitlines():
        m=re.match(r"([0-9a-f]+) (\w)\s+O (\.\w+)\s+([0-9a-f]+) (\S+)",l)
        if m and m.group(3) in secs:
            off=int(m.group(1),16); sz=int(m.group(4),16); b=secs[m.group(3)][off:off+sz]
            if sz==4 and m.group(3) in('.sdata2','.rodata','.sdata'): vals[m.group(5)]="F%g/%s"%(struct.unpack(">f",bytes(b))[0],bytes(b).hex())
            elif sz==8 and len(b)==8: vals[m.group(5)]="D%g"%struct.unpack(">d",bytes(b))[0]
    return vals
def funcs(p):
    vals=symvals(p)
    out={}; cur=None; last=None
    for l in run("-dr",p).splitlines():
        m=re.match(r"[0-9a-f]+ <(.+)>:",l)
        if m: cur=m.group(1); out[cur]=[]; continue
        if cur is None: continue
        m=re.match(r"\s+[0-9a-f]+:\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)",l)
        if m:
            op,args=m.group(1),m.group(2)
            args=re.sub(r"\(r1\)","(SP)",args); args=re.sub(r"\br1,\s*-?\d+","SPX",args); args=re.sub(r"<.*","",args); args=re.sub(r"^[0-9a-f]+\s*$","T",args.strip()) if op.startswith("b") and op not in("blr","bctr","blrl","bctrl") else args; args=re.sub(r"-?\d+\(SP\)","STK(SP)",args); args=re.sub(r"\b[rf]\d+\b","R",args); args=re.sub(r"cr\d","cr",args)
            args=re.sub(r"[0-9a-f]+ <.*>","T",args)
            args=re.sub(r"-?0x[0-9a-f]+\(R\)",lambda x:x.group(0),args)
            out[cur].append([op,args,""]); continue
        m=re.match(r"\s+[0-9a-f]+: R_PPC_\S+\s+(\S+)",l)
        if m and out[cur]:
            s=m.group(1); s=re.sub(r"[+-]0x[0-9a-f]+$","",s)
            out[cur][-1][2]=vals.get(s, "L" if re.search(r"[@$]|^lbl_|^instance|^init|^\.\.\.|^__vt|^__dt",s) else s)
    return out
def norm(ins):
    r=[]
    for op,a,s in ins:
        if op in("mr","fmr","nop") or (op in("addi","lis") and ",0 L" in (a+" "+s)) or (op in("addi","lis") and a.endswith(",0") and s) or (op=="addi" and a=="R,R,0") or "STK" in a and op in("addi",) or op=="stwu" or "STK" in a or "SPX" in a: continue
        a=re.sub(r"\b(-?0x[0-9a-f]+|-?\d+)\(R\)",lambda m:("STK" if False else m.group(0)),a)
        r.append(op+" "+a+" "+s)
    return r
u=sys.argv[1]
A=funcs(f"build/GMSE01/obj/{u}.o"); B=funcs(f"build/GMSE01/src/{u}.o")
filt=sys.argv[2] if len(sys.argv)>2 else None
for f in A:
    if f not in B: continue
    if filt and filt not in f: continue
    a=collections.Counter(norm(A[f])); b=collections.Counter(norm(B[f]))
    # ignore stack ops
    def keep(k): return not re.search(r"0x[0-9a-f]+\(R\)",k) or not re.search(r"^(stw|lwz|stfs|lfs|stfd|lfd|addi|stwu|stmw|lmw|stb|lbz|sth|lhz)\b",k) or True
    ta=a-b; tb=b-a
    ta={k:v for k,v in ta.items() if not re.match(r"(stw|lwz|stfs|lfs|stfd|lfd|stwu|addi|stmw|lmw) R,(-?0x[0-9a-f]+)\(R\)\s*$",k) and not re.match(r"addi R,R,-?0x[0-9a-f]+\s*$",k)}
    tb={k:v for k,v in tb.items() if not re.match(r"(stw|lwz|stfs|lfs|stfd|lfd|stwu|addi|stmw|lmw) R,(-?0x[0-9a-f]+)\(R\)\s*$",k) and not re.match(r"addi R,R,-?0x[0-9a-f]+\s*$",k)}
    if ta or tb:
        print("##",f[:90]); 
        for k,v in sorted(ta.items()): print("  T-only",v,k)
        for k,v in sorted(tb.items()): print("  O-only",v,k)



