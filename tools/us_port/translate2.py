# PAL -> US config translator v2
#  .text: function size-sequence alignment
#  data sections: per-TU delta via DP maximizing coverage of the TU's own code refs,
#                 then per-symbol Viterbi inside TUs whose size changed (label hits)
import re,sys,json,bisect,collections,difflib
PAL_SYM,PAL_SPL,US_SYM,OUT_SYM,OUT_SPL=sys.argv[1:6]
LAMBDA=float(__import__('os').environ.get('LAMBDA','4'))
symre=re.compile(r'^(\S+) = (\S+):0x([0-9A-Fa-f]+);(.*)$')
def load(p):
    out=[]
    for l in open(p):
        m=symre.match(l.rstrip('\n'))
        if not m: continue
        sz=re.search(r'size:0x([0-9A-Fa-f]+)',m[4]); ty=re.search(r'type:(\w+)',m[4])
        out.append(dict(name=m[1],sec=m[2],addr=int(m[3],16),size=int(sz[1],16) if sz else None,type=ty[1] if ty else None,attrs=m[4]))
    return out
pal=load(PAL_SYM); us=load(US_SYM)
USSEC={}
for l in open('us_sections.txt'):
    n,a,s=l.split(); USSEC['.sdata2' if n=='.data9' else n]=(int(a,16),int(a,16)+int(s,16))
lines=open(PAL_SPL).read().split('\n')
tus=[];cur=None
for l in lines:
    m=re.match(r'^(\S.*):$',l)
    if m and m[1]!='Sections': cur=m[1]; continue
    m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)',l)
    if m and cur: tus.append(dict(tu=cur,sec=m[1],start=int(m[2],16),end=int(m[3],16)))
stats=collections.Counter()
# ---------- text
palf=sorted([s for s in pal if s['type']=='function' and s['sec']=='.text'],key=lambda x:x['addr'])
usf=sorted([s for s in us if s['type']=='function' and s['sec']=='.text'],key=lambda x:x['addr'])
sm=difflib.SequenceMatcher(None,[s['size'] for s in palf],[s['size'] for s in usf],autojunk=False)
for tag,i1,i2,j1,j2 in sm.get_opcodes():
    if tag=='equal' or (tag=='replace' and i2-i1==j2-j1):
        for k in range(i2-i1):
            p=palf[i1+k]; u=usf[j1+k]; p['us']=u['addr']; p['ussize']=u['size']
for s in palf: stats['text_'+('mapped' if 'us' in s else 'unmapped')]+=1
bysec=collections.defaultdict(list)
for s in pal: bysec[s['sec']].append(s)
for v in bysec.values(): v.sort(key=lambda x:x['addr'])
# fill gaps between anchored functions using US auto-detected functions
usfa_sorted=[u['addr'] for u in usf]
extra=[]
i=0
while i<len(palf):
    if 'us' in palf[i]: i+=1; continue
    j=i
    while j<len(palf) and 'us' not in palf[j]: j+=1
    lo=palf[i-1]['us']+palf[i-1]['ussize'] if i>0 else USSEC['.text'][0]
    hi=palf[j]['us'] if j<len(palf) else USSEC['.text'][1]
    gap=[u for u in usf if lo<=u['addr']<hi]
    pg=palf[i:j]
    if len(gap)==len(pg):
        for p,u in zip(pg,gap): p['us']=u['addr']; p['ussize']=u['size']; stats['text_gapfill']+=1
    else:
        # align by size within gap; leftovers on US side become fn_ symbols, PAL leftovers dropped
        used=set(); gstart={u['addr']:k for k,u in enumerate(gap)}
        # 1) offset alignment (auto-analysis over-splits)
        for p in pg:
            ua=lo+(p['addr']-pg[0]['addr'])
            if ua in gstart:
                end=ua+p['size']
                ks=[k for k,u in enumerate(gap) if ua<=u['addr']<end]
                if ks and gap[ks[-1]]['addr']+gap[ks[-1]]['size']==end:
                    p['us']=ua; p['ussize']=p['size']; used|=set(ks); stats['text_offsetfill']+=1
        # 2) size alignment of the rest
        rp=[p for p in pg if 'us' not in p]; ru=[(k,u) for k,u in enumerate(gap) if k not in used]
        # DP alignment by size similarity (regional size changes)
        A=[p['size'] for p in rp]; Bz=[u['size'] for k,u in ru]
        na,nb=len(A),len(Bz); INF=1e9
        D=[[INF]*(nb+1) for _ in range(na+1)]; P=[[None]*(nb+1) for _ in range(na+1)]
        D[0][0]=0
        for x in range(na+1):
            for y in range(nb+1):
                if D[x][y]>=INF: continue
                if x<na and D[x][y]+1<D[x+1][y]: D[x+1][y]=D[x][y]+1; P[x+1][y]=(x,y,'p')
                if y<nb and D[x][y]+0.7<D[x][y+1]: D[x][y+1]=D[x][y]+0.7; P[x][y+1]=(x,y,'u')
                if x<na and y<nb:
                    c=abs(A[x]-Bz[y])/max(A[x],Bz[y],1)
                    if c<0.75 and D[x][y]+c<D[x+1][y+1]: D[x+1][y+1]=D[x][y]+c; P[x+1][y+1]=(x,y,'m')
        x,y=na,nb
        while (x,y)!=(0,0):
            px,py,t=P[x][y]
            if t=='m':
                rp[px]['us']=ru[py][1]['addr']; rp[px]['ussize']=ru[py][1]['size']; used.add(ru[py][0]); stats['text_simfill']+=1
            x,y=px,py
        for k,u in enumerate(gap):
            if k not in used: extra.append(dict(name=f"fn_{u['addr']:08X}",sec='.text',addr=None,us=u['addr'],size=u['size'],ussize=u['size'],type='function',attrs=f" // type:function size:0x{u['size']:X} scope:global align:4"))
        for p in pg:
            if 'us' not in p: p['drop']=True; stats['text_dropped']+=1; print('dropped',p['name'])
    i=j
pal=[p for p in pal if not p.get('drop')]+extra
# any US function start not covered by a translated text function -> fn_ placeholder
iv=sorted((p['us'],p['us']+(p.get('ussize') or p['size'] or 4)) for p in pal if p['sec']=='.text' and p.get('type')=='function' and 'us' in p)
ivs=[a for a,b in iv]
for u in usf:
    k=bisect.bisect_right(ivs,u['addr'])-1
    if k>=0 and iv[k][0]<=u['addr']<iv[k][1]: continue
    pal.append(dict(name=f"fn_{u['addr']:08X}",sec='.text',addr=None,us=u['addr'],size=u['size'],ussize=u['size'],type='function',attrs=f" // type:function size:0x{u['size']:X} scope:global align:4"))
    stats['text_us_only']+=1
bysec['.text']=[p for p in bysec['.text'] if not p.get('drop')]
prev=0
for s in bysec['.text']:
    if 'us' in s: prev=s['us']-s['addr']
    elif s.get('type')=='function':
        s['drop']=True; stats['text_dropped']+=1; print('dropped',s['name'])
    else: s['us']=s['addr']+prev; s['guess']=True; stats['text_guess']+=1
pal=[p for p in pal if not p.get('drop')]
bysec['.text']=[p for p in bysec['.text'] if not p.get('drop')]
for sec in ('.init','extab','extabindex'):
    for s in bysec.get(sec,[]): s['us']=s['addr']
# ---------- refs per TU
R=json.load(open('us_refs.json'))['refs']
usfa={}
for s in us:
    if s['type']=='function': usfa[s['addr']]=R.get(s['name'],[])
def secof(a):
    for k,(s,e) in USSEC.items():
        if s<=a<e: return k
tu_text={t['tu']:t for t in tus if t['sec']=='.text'}
tutext_funcs=collections.defaultdict(list)
for s in palf:
    if 'us' in s and not s.get('guess'):
        for t in (tu_text.values()):
            pass
textranges=sorted((t['start'],t['end'],t['tu']) for t in tu_text.values())
starts=[r[0] for r in textranges]
tu_refs=collections.defaultdict(lambda:collections.defaultdict(list))
for s in palf:
    if 'us' not in s: continue
    i=bisect.bisect_right(starts,s['addr'])-1
    if i<0 or not (textranges[i][0]<=s['addr']<textranges[i][1]): continue
    tu=textranges[i][2]
    for r in usfa.get(s['us'],[]):
        sc=secof(r)
        if sc: tu_refs[tu][sc].append(r)
# label set (US referenced addresses) per section
labels=collections.defaultdict(set)
for s in us: labels[s['sec']].add(s['addr'])
for v in usfa.values():
    for r in v:
        sc=secof(r)
        if sc: labels[sc].add(r)
# ---------- raw byte reader
import struct as _st
_dol=open('us/main.dol','rb').read()
_SEC=[]
for _i in range(18):
    _o,_a,_z=[_st.unpack('>I',_dol[k+_i*4:k+_i*4+4])[0] for k in (0,0x48,0x90)]
    if _z: _SEC.append((_a,_z,_o))
def _rdb(a):
    for s,z,o in _SEC:
        if s<=a<s+z: return _dol[o+a-s]
    return 0
# ---------- per-section TU DP
init_delta={}
for sec,(s0,e0) in USSEC.items():
    ts=[t for t in tus if t['sec']==sec]
    if ts: init_delta[sec]=s0-min(t['start'] for t in ts)
tu_delta={}
for sec in ('.ctors','.dtors','.rodata','.data','.bss','.sdata','.sbss','.sdata2'):
    ts=sorted([t for t in tus if t['sec']==sec],key=lambda t:t['start'])
    s0,e0=USSEC[sec]
    # candidates per TU
    C=[];COV=[]
    allc=set([init_delta[sec]])
    secsyms=bysec.get(sec,[]); sa=[s['addr'] for s in secsyms]; LBL=labels[sec]; LS=sorted(LBL)
    TS=[]
    for t in ts:
        rs=sorted(tu_refs[t['tu']].get(sec,[]))
        inside=secsyms[bisect.bisect_left(sa,t['start']):bisect.bisect_left(sa,t['end'])]
        TS.append(inside)
        c=set()
        for s in inside[:40]:
            a=bisect.bisect_left(LS,s['addr']+init_delta[sec]-0x400); b=bisect.bisect_right(LS,s['addr']+init_delta[sec]+0x1400)
            for u in LS[a:b]: c.add(u-s['addr'])
        AL=8 if sec in ('.rodata','.data','.bss','.sdata2') else 4
        for r in rs:
            a=r-t['start']; c.add(a-(a%AL))
            b=r-t['end']+1; c.add(b+((-b)%AL))
        # restrict to plausible
        c={d for d in c if s0<=t['start']+d and t['end']+d<=e0 and -0x400<=d-init_delta[sec]<=0x1400}
        allc|=c; C.append(c); COV.append(rs)
    # each TU may also use any delta used nearby (carry); we DP over global candidate list limited per TU
    allc=sorted(allc)
    def cov(rs,t,d):
        a=bisect.bisect_left(rs,t['start']+d); b=bisect.bisect_left(rs,t['end']+d); return b-a
    INF=1e18
    n=len(ts); D=allc; K=len(D)
    cost=[0.0 if d==init_delta[sec] else LAMBDA for d in D]; back=[]
    for i,t in enumerate(ts):
        rs=COV[i]
        ok=[s0<=t['start']+d and t['end']+d<=e0 for d in D]
        inside=TS[i]
        local=[(-(3*cov(rs,t,d)+sum(1 for s in inside if s['addr']+d in LBL)) if ok[k] else INF) for k,d in enumerate(D)]
        if i==0:
            cost=[c+l for c,l in zip(cost,local)]; back.append(list(range(K))); continue
        pt=ts[i-1]
        # feasibility: this start >= prev US start + small min size (allow shrink to 0)
        pm=[];pi=[];bc=INF;bj=-1
        for j in range(K):
            if cost[j]<bc: bc=cost[j]; bj=j
            pm.append(bc); pi.append(bj)
        nc=[];bk=[]
        for k,d in enumerate(D):
            # need pt.start+dj <= t.start+d  -> dj <= t.start-pt.start+d
            lim=t['start']-pt['start']+d
            j=bisect.bisect_right(D,lim)-1
            sw=(pm[j]+LAMBDA) if j>=0 else INF; sj=pi[j] if j>=0 else k
            st=cost[k]
            if st<=sw: nc.append(st+local[k]); bk.append(k)
            else: nc.append(sw+local[k]); bk.append(sj)
        cost=nc; back.append(bk)
    k=min(range(K),key=lambda k:cost[k]); path=[0]*n
    for i in range(n-1,-1,-1): path[i]=k; k=back[i][k]
    tot=sum(len(r) for r in COV); got=sum(cov(COV[i],ts[i],D[path[i]]) for i in range(n))
    stats[sec+'_refcov']=f'{got}/{tot}'; stats[sec+'_switches']=sum(1 for i in range(1,n) if path[i]!=path[i-1])
    for i,t in enumerate(ts): tu_delta[(t['tu'],sec,t['start'])]=D[path[i]]
    # ---- symbols: within TU, delta from TU start; if next TU delta differs (size change inside), Viterbi on labels
    syms=bysec.get(sec,[])
    for i,t in enumerate(ts):
        d0=D[path[i]]; d1=D[path[i+1]] if i+1<n else None
        inside=[s for s in syms if t['start']<=s['addr']<t['end']]
        if d1 is None or d1==d0 or not inside:
            for s in inside: s['us']=s['addr']+d0
            continue
        # size changed: align PAL strides with US boundary strides (labels + string starts)
        ust=t['start']+d0; uen=ts[i+1]['start']+d1
        B=set(u for u in labels[sec] if ust<=u<uen); B.add(ust)
        if sec not in ('.bss','.sbss'):
            for p in range(ust,uen,4):
                bt=_rdb(p)
                if bt and (p==ust or _rdb(p-1)==0) and 32<=bt<127: B.add(p)
        B=sorted(B); us_str=[(B[k+1] if k+1<len(B) else uen)-B[k] for k in range(len(B))]
        pal_str=[((inside[k+1]['addr'] if k+1<len(inside) else t['end'])-inside[k]['addr']) for k in range(len(inside))]
        # zero-stride labels: align only objects with nonzero stride
        idx=[k for k in range(len(inside)) if pal_str[k]>0]
        sm3=difflib.SequenceMatcher(None,[pal_str[k] for k in idx],us_str,autojunk=False)
        for x,y,nn in sm3.get_matching_blocks():
            for q in range(nn): inside[idx[x+q]]['us']=B[y+q]
        # fill unmatched: carry forward from previous placed symbol by PAL stride, bounded
        last=None
        for k,s in enumerate(inside):
            if 'us' in s: last=(s['us'],s['addr']); continue
            if last is None: s['us']=ust+(s['addr']-t['start'])
            else: s['us']=last[0]+(s['addr']-last[1])
            if s['us']+(s['size'] or 0)>uen: s['us']=max(ust,uen-(s['size'] or 0))&~3; stats[sec+'_clamped']+=1
            last=(s['us'],s['addr'])
        # enforce monotone
        for k in range(1,len(inside)):
            if inside[k]['us']<inside[k-1]['us']: inside[k]['us']=inside[k-1]['us']+0; stats[sec+'_nonmono']+=1
        stats[sec+'_resized_tus']+=1
    for s in syms:
        if 'us' not in s:
            # outside any split: nearest previous
            s['us']=s['addr']+init_delta[sec]; stats[sec+'_nosplit']+=1
    for s in syms: stats[sec+('_hit' if s['us'] in labels[sec] else '_nohit')]+=1
# ---------- override with global stride alignment where available
ALIGNED={}
for sec in ('.rodata','.data','.sdata','.sdata2'):
    try: am={int(k):v for k,v in json.load(open(f'align_{sec[1:]}.json')).items()}
    except FileNotFoundError: continue
    ALIGNED[sec]=am
    syms=sorted(bysec.get(sec,[]),key=lambda x:x['addr'])
    anchors=[(s['addr'],am[s['addr']]) for s in syms if s['addr'] in am]
    aa=[a for a,_ in anchors]
    for s in syms:
        if s['addr'] in am: s['us']=am[s['addr']]; stats[sec+'_anchored']+=1; continue
        k=bisect.bisect_right(aa,s['addr'])-1
        if k>=0:
            pa,ua=anchors[k]; cand=ua+(s['addr']-pa)
            if k+1<len(anchors) and cand+(s['size'] or 0)>anchors[k+1][1]:
                # doesn't fit before next anchor: place relative to next anchor instead
                na,nu=anchors[k+1]; cand=nu-(na-s['addr'])
                stats[sec+'_backfill']+=1
            s['us']=cand
        else: s['us']=s['addr']+init_delta[sec]
        stats[sec+'_carried']+=1
# ---------- splits
next_delta={}
_bs=collections.defaultdict(list)
for (tu,sec,st),d in tu_delta.items(): _bs[sec].append((st,d))
for sec,v in _bs.items():
    v.sort()
    for (a,da),(b,db) in zip(v,v[1:]): next_delta[(sec,a)]=db
def text_tr(addr,is_end):
    syms=bysec['.text']; i=bisect.bisect_left([s['addr'] for s in syms],addr)
    if is_end:
        p=syms[i-1]; return p['us']+(p.get('ussize') or p['size'] or 0)+(addr-(p['addr']+(p['size'] if 'ussize' not in p else 0) if False else addr-addr))
    s=syms[i]; return s['us']-(s['addr']-addr)
textsyms=bysec['.text']; ta=[s['addr'] for s in textsyms]
palsize={id(s):None for s in textsyms}
out=[];cur=None;res=[];prev_text_end={};first_us_of={}
for l in lines:
    m=re.match(r'^(\S.*):$',l)
    if m and m[1]!='Sections': cur=m[1]
    m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)(.*)$',l)
    if not m: out.append(l); continue
    sec=m[1]; st=int(m[2],16); en=int(m[3],16)
    if sec in ('.init','extab','extabindex'): ust,uen=st,en
    elif sec=='.text':
        inside=[s for s in textsyms[bisect.bisect_left(ta,st):bisect.bisect_left(ta,en)]]
        if not inside:
            j=bisect.bisect_left(ta,st)-1; p=textsyms[j] if j>=0 else None
            d=(p['us']-p['addr']) if p else 0
            out.append([m[1],st+d,None,m[4],cur,st,en]); continue
        fs=[x for x in inside if x.get('type')=='function']
        f0=fs[0] if fs else inside[0]; l0=inside[-1]
        ust=None   # always chain from previous TU's last known function (US-only code goes to the next TU)
        first_us_of[cur]=f0['us']
        last_fn=max(fs,key=lambda x:x['us']) if fs else None
        if last_fn is not None: prev_text_end[cur]=last_fn['us']+(last_fn.get('ussize') or last_fn['size'] or 0)
        uen=l0['us']+(en-l0['addr']) if 'ussize' not in l0 else l0['us']+l0['ussize']+(en-(l0['addr']+[x for x in pal if x is l0][0]['size'] if False else en-en))
        if 'ussize' in l0:
            # PAL size of l0 unknown after overwrite; recompute from original splits gap: use next TU start instead (fixed below)
            uen=None
    elif sec in ALIGNED:
        syms=bysec[sec]; sa=sorted(syms,key=lambda x:x['addr']); ad=[x['addr'] for x in sa]
        k=bisect.bisect_left(ad,st)
        if k<len(sa) and sa[k]['addr']<en:
            ust=sa[k]['us']-(sa[k]['addr']-st)
        else:
            d=tu_delta[(cur,sec,st)]; ust=st+d
        uen=None  # = next TU start in this section, or own last symbol end
        out.append([m[1],ust,uen,m[4],cur,st,en]); continue
    else:
        d=tu_delta[(cur,sec,st)]; ust,uen=st+d,en+d
        nd=next_delta.get((sec,st))
        if nd is not None and nd<d: uen=en+nd
    out.append([m[1],ust,uen,m[4],cur,st,en])
# fix text ends: end = next text TU start (text TUs are contiguous) or section end
txt=[o for o in out if isinstance(o,list) and o[0]=='.text']
# resolve unknown starts from previous TU's last function end (text TUs are contiguous, PAL order)
_tx=[o for o in out if isinstance(o,list) and o[0]=='.text']
if _tx and _tx[0][1] is None: _tx[0][1]=first_us_of.get(_tx[0][4],USSEC['.text'][0])
for a,b in zip(_tx,_tx[1:]):
    if b[1] is None:
        e=prev_text_end.get(a[4])
        b[1]=(e+3)&~3 if e else first_us_of.get(b[4],a[1])
txt=_tx
for a,b in zip(txt,txt[1:]):
    if a[2] is None or a[2]>b[1]: a[2]=b[1]
if txt[-1][2] is None: txt[-1][2]=USSEC['.text'][1]
# resolve ends for aligned sections: end = next TU start (sections are contiguous), last = section end bound
for sec in ALIGNED:
    v=sorted([o for o in out if isinstance(o,list) and o[0]==sec],key=lambda o:o[1])
    for a,b in zip(v,v[1:]): a[2]=b[1]
    if v:
        syms=[x for x in bysec[sec]]; last=max(syms,key=lambda x:x['us'])
        v[-1][2]=min(USSEC[sec][1],max(last['us']+(last['size'] or 0),v[-1][1]))
# .ctors: place each TU's entry by where its pointer lands in US text
import struct as _st
def _rd(a):
    for i in range(18):
        off,ad,sz=[_st.unpack('>I',_dol[k+i*4:k+i*4+4])[0] for k in (0,0x48,0x90)]
        if sz and ad<=a<ad+sz: return _st.unpack('>I',_dol[off+a-ad:off+a-ad+4])[0]
c0,c1=USSEC['.ctors']
ctor_ptrs=[(a,_rd(a)) for a in range(c0,c1,4)]
txt_ranges=[(o[1],o[2],o[4]) for o in out if isinstance(o,list) and o[0]=='.text']
def tu_for(addr):
    for a,b,n in txt_ranges:
        if a<=addr<b: return n
slot={}
for a,p in ctor_ptrs:
    if p==0: continue
    n=tu_for(p)
    if n and n not in slot: slot[n]=a
miss=0
for o in out:
    if isinstance(o,list) and o[0]=='.ctors' and o[1]!=c0:
        if o[4] in slot: o[1]=slot[o[4]]; o[2]=o[1]+4
        else: miss+=1; print('ctors: no US entry for',o[4])
stats['ctors_placed']=len(slot); stats['ctors_missing']=miss
# ---- enforce PAL order, no overlap, non-empty, per section
palrange={}
for t in tus: palrange[(t['tu'],t['sec'])]=(t['start'],t['end'])
fixed=collections.Counter()
for sec in set(o[0] for o in out if isinstance(o,list)):
    if sec in ('.init','extab','extabindex','.ctors','.dtors'): continue
    v=[o for o in out if isinstance(o,list) and o[0]==sec]   # PAL file order
    v.sort(key=lambda o:(o[5],o[6])[0])
    lo,hi=USSEC[sec]
    # island smoothing: delta returns to same value after a short excursion -> excursion is an error
    if sec not in ('.text',):
        dl=[o[1]-(o[5],o[6])[0] for o in v]
        i=1
        while i<len(v):
            if dl[i]!=dl[i-1]:
                for j in range(i+1,min(len(v),i+16)):
                    if dl[j]==dl[i-1]:
                        base=dl[i-1]
                        for q in range(i,j):
                            ps,pe=(v[q][5],v[q][6])
                            v[q][1]=ps+base; v[q][2]=pe+base; dl[q]=base
                            for s in bysec.get(sec,[]):
                                if ps<=s['addr']<pe: s['us']=s['addr']+base
                            fixed[sec+'_island']+=1
                        break
            i+=1
        # also ends of steady runs: end = PAL end + delta when next file has same delta
        for q in range(len(v)-1):
            if dl[q]==dl[q+1]:
                ps,pe=(v[q][5],v[q][6]); v[q][2]=pe+dl[q]
    # LIS on starts to find order violators
    import bisect as _bb
    starts=[o[1] for o in v]; n=len(v)
    tails=[];tidx=[];prev=[-1]*n
    for i,s in enumerate(starts):
        k=_bb.bisect_right(tails,s)
        if k==len(tails): tails.append(s); tidx.append(i)
        else: tails[k]=s; tidx[k]=i
        prev[i]=tidx[k-1] if k>0 else -1
    keep=set(); k=tidx[-1] if tidx else -1
    while k>=0: keep.add(k); k=prev[k]
    for i,o in enumerate(v):
        ps,pe=(o[5],o[6])
        if i not in keep:
            # interpolate from previous kept file end
            o[1]=v[i-1][2] if i>0 and v[i-1][2] else (v[i-1][1] if i>0 else lo); fixed[sec+'_reordered']+=1
        if i>0 and v[i-1][2] is not None and o[1]<v[i-1][2]:
            o[1]=v[i-1][2]; fixed[sec+'_pushed']+=1
        if o[2] is None or o[2]<=o[1]:
            o[2]=o[1]+(pe-ps); fixed[sec+'_resized']+=1
        o[1]=max(o[1],lo); o[2]=min(o[2],hi)
    for a,b in zip(v,v[1:]):
        if a[2]>b[1]: a[2]=max(a[1],b[1]); fixed[sec+'_trimmed']+=1
    # sections without file content: move symbols with their file
    if sec in ('.bss','.sbss'):
        syms=bysec.get(sec,[])
        for o in v:
            ps,pe=(o[5],o[6])
            for s in syms:
                if ps<=s['addr']<pe: s['us']=o[1]+(s['addr']-ps)
for o in out:
    if isinstance(o,list) and o[0] not in ('.text','.init','extab','extabindex') and o[1]%4:
        o[1]+=4-(o[1]%4); fixed['start_aligned']+=1
print('order fixes',dict(fixed))
# clamp overlaps from padding
bys=collections.defaultdict(list)
for o in out:
    if isinstance(o,list): bys[o[0]].append(o)
bad=0
for sec,v in bys.items():
    v.sort(key=lambda o:o[1])
    for a,b in zip(v,v[1:]):
        if a[2]>b[1] and a[2]-b[1]<=0x40: a[2]=b[1]
        if a[2]>b[1]: bad+=1; print('overlap',sec,a[4],hex(a[1]),hex(a[2]),b[4],hex(b[1]))
    for a in v:
        if a[2]<a[1]: bad+=1; print('neg',sec,a[4])
with open(OUT_SPL,'w') as f:
    f.write('\n'.join(o if isinstance(o,str) else f"\t{o[0]:<11} start:0x{o[1]:08X} end:0x{o[2]:08X}{o[3]}" for o in out))
print('split problems',bad)
print(dict(stats))
# ---------- snap to declared alignment
for s in pal:
    m_=re.search(r'align:(\d+)',s.get('attrs',''))
    if m_ and 'us' in s and s['sec'] not in ('.text','.init'):
        al=int(m_[1])
        if s['us']%al: s['us']+= al-(s['us']%al); stats['snapped']+=1
# ---------- sizes
for sec,syms in bysec.items():
    syms.sort(key=lambda x:x['us'])
    for i,s in enumerate(syms):
        if 'ussize' in s: s['size']=s['ussize']
        if s['size']:
            j=i+1
            # labels (no size) mark positions inside objects/functions; never trim against them
            while j<len(syms) and (syms[j]['us']<=s['us'] or not syms[j]['size'] or syms[j].get('type')=='label'): j+=1
            if j<len(syms):
                nx=syms[j]['us']
                if s['us']<nx<s['us']+s['size']: s['size']=nx-s['us']; stats['size_trimmed']+=1
                elif sec not in ('.bss','.sbss','.text','.init') and s['us']+s['size']<nx:
                    # non-zero bytes in the gap -> object is longer in US
                    gap=[_rdb(a) for a in range(s['us']+s['size'],nx)]
                    if any(gap):
                        last=max(k for k,b in enumerate(gap) if b)
                        s['size']=s['size']+last+1; stats['size_extended']+=1
with open(OUT_SYM,'w') as f:
    order=['.init','extab','extabindex','.text','.ctors','.dtors','.rodata','.data','.bss','.sdata','.sbss','.sdata2']
    for s in sorted(pal,key=lambda x:(order.index(x['sec']),x['us'])):
        attrs=s['attrs']
        if s['size'] is not None: attrs=re.sub(r'size:0x[0-9A-Fa-f]+',f'size:0x{s["size"]:X}',attrs)
        name=re.sub(r'^(lbl|fn)_8[0-9A-F]{7}$',lambda m:f'{m[1]}_{s["us"]:08X}',s['name'])
        f.write(f"{name} = {s['sec']}:0x{s['us']:08X};{attrs}\n")
# ---- repair pass on written files: extend splits over spanning symbols
def _tu_refcount(tu,lo,hi):
    # how many of this TU's code references land in [lo,hi), using final US text ranges
    import re as _re
    if not hasattr(_tu_refcount,'tr'):
        tr={};cur=None
        for l in open(OUT_SPL):
            m=_re.match(r'^(\S.*):$',l)
            if m: cur=m[1]; continue
            m=_re.match(r'^\t\.text\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)',l)
            if m: tr[cur]=(int(m[1],16),int(m[2],16))
        _tu_refcount.tr=tr
    r=_tu_refcount.tr.get(tu)
    if not r: return 0
    n=0
    for a,refs in _us_func_refs:
        if r[0]<=a<r[1]: n+=sum(1 for x in refs if lo<=x<hi)
    return n
_us_func_refs=[(s['addr'],R.get(s['name'],[])) for s in us if s['type']=='function']
def _repair():
    import re as _re
    L=open(OUT_SPL).read().split('\n'); ent=[];cur=None
    for i,l in enumerate(L):
        m=_re.match(r'^(\S.*):$',l)
        if m: cur=m[1]; continue
        m=_re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)(.*)$',l)
        if m and cur!='Sections': ent.append([m[1],int(m[2],16),int(m[3],16),m[4],i,cur])
    sy=collections.defaultdict(list)
    for l in open(OUT_SYM):
        m=_re.match(r'(\S+) = (\S+):0x([0-9A-F]+);(.*)',l)
        if m:
            z=_re.search(r'size:0x([0-9A-F]+)',m[4]); sy[m[2]].append((int(m[3],16),int(z[1],16) if z else 0))
    fixes=0
    for sec in set(e[0] for e in ent):
        v=sorted([e for e in ent if e[0]==sec],key=lambda e:e[1]); st=[e[1] for e in v]
        firsts={}
        ss=sorted(sy.get(sec,[]))
        for a,z in ss:
            k=bisect.bisect_right(st,a)-1
            if k>=0 and v[k][1]<=a<v[k][2]: firsts.setdefault(k,a)
        for a,z in ss:
            if not z: continue
            k=bisect.bisect_right(st,a)-1
            if k<0 or not (v[k][1]<=a<v[k][2]): continue
            if a+z>v[k][2]:
                ne=a+z
                if k+1<len(v):
                    nf=firsts.get(k+1)
                    if nf is not None and nf<ne: print('cannot repair',sec,v[k][5],hex(a)); continue
                    if v[k+1][1]<ne:
                        v[k+1][1]=(ne+3)&~3 if sec!='.text' else ne
                        if v[k+1][2]<v[k+1][1]: v[k+1][2]=v[k+1][1]
                    ne=min(ne, v[k+1][1])
                    v[k][2]=v[k+1][1] if v[k+1][1]>=a+z else ne
                else: v[k][2]=ne
                fixes+=1
            st=[e[1] for e in v]
    # close gaps between consecutive files (dtk would otherwise create auto_ objects)
    gaps=0
    for sec in set(e[0] for e in ent):
        if sec in ('.init','extab','extabindex','.ctors','.dtors'): continue
        v=sorted([e for e in ent if e[0]==sec],key=lambda e:e[1])
        for a,b in zip(v,v[1:]):
            if a[2]<b[1]:
                ga,gb=a[2],b[1]
                ra=_tu_refcount(a[5],ga,gb); rb=_tu_refcount(b[5],ga,gb)
                if rb>ra: b[1]=ga
                else: a[2]=gb
                gaps+=1
    print('gaps closed',gaps)
    for e in ent: L[e[4]]=f"\t{e[0]:<11} start:0x{e[1]:08X} end:0x{e[2]:08X}{e[3]}"
    open(OUT_SPL,'w').write('\n'.join(L)); print('repairs',fixes)
_repair()
