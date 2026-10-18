#!/usr/bin/env python3
"""Compare compiled source objects against original extracted objects for one version.
Functions are compared byte-for-byte with relocation fields masked; data sections by size
and (non-relocated) content. Prints objects that don't match."""
import struct,sys,os,json,re
def elf(path):
    d=open(path,'rb').read()
    shoff,=struct.unpack('>I',d[0x20:0x24]); shentsize,shnum,shstrndx=struct.unpack('>HHH',d[0x2E:0x34])
    sh=[]
    for i in range(shnum):
        f=struct.unpack('>IIIIIIIIII',d[shoff+i*shentsize:shoff+i*shentsize+40]); sh.append(f)
    def nm(tab,off): e=d.index(b'\0',sh[tab][4]+off); return d[sh[tab][4]+off:e].decode('latin1')
    secs=[dict(name=nm(shstrndx,s[0]),type=s[1],off=s[4],size=s[5],link=s[6],info=s[7]) for s in sh]
    for s in secs: s['data']=d[s['off']:s['off']+s['size']] if s['type']!=8 else b''
    syms=[]
    for i,s in enumerate(secs):
        if s['type']==2:
            for k in range(0,s['size'],16):
                n,v,z,info,oth,shn=struct.unpack('>IIIBBH',s['data'][k:k+16])
                syms.append(dict(name=nm(s['link'],n),value=v,size=z,type=info&15,bind=info>>4,shndx=shn))
    relocs={}
    for s in secs:
        if s['type']==4:
            tgt=s['info']; L=relocs.setdefault(tgt,[])
            for k in range(0,s['size'],12):
                off,info,add=struct.unpack('>IIi',s['data'][k:k+12]); L.append((off,info&0xff,info>>8))
    return secs,syms,relocs
MASK={1:(0,4,0xffffffff),4:(2,2,0xffff),5:(2,2,0xffff),6:(2,2,0xffff),10:(0,4,0x03fffffc),11:(0,4,0x0000fffc),109:(0,4,0x001fffff),26:(0,4,0xffffffff)}
def masked(secs,relocs,idx):
    b=bytearray(secs[idx]['data'])
    for off,t,_ in relocs.get(idx,[]):
        if secs[idx]['name'].startswith('.text') or t!=1:
            w0=off&~3
            if w0+4>len(b): continue
            if t in (4,5,6): b[w0+2:w0+4]=b'\0\0'
            elif t==10: w=struct.unpack('>I',b[w0:w0+4])[0]&~0x03fffffc; b[w0:w0+4]=struct.pack('>I',w)
            elif t==109: w=struct.unpack('>I',b[w0:w0+4])[0]&~0x001fffff; b[w0:w0+4]=struct.pack('>I',w)
            else: b[w0:w0+4]=b'\0\0\0\0'
        else:
            b[off:off+4]=b'\0\0\0\0'
    return bytes(b)
def funcs(path):
    secs,syms,relocs=elf(path); out={}; data={}
    cache={}
    for s in syms:
        if s['type']==2 and 0<s['shndx']<len(secs) and secs[s['shndx']]['name'].startswith('.text'):
            i=s['shndx']
            if i not in cache: cache[i]=masked(secs,relocs,i)
            out[s['name']]=cache[i][s['value']:s['value']+s['size']]
    for s_ in syms:
        if s_['type']==1 and 0<s_['shndx']<len(secs):
            sec=secs[s_['shndx']]
            if sec['name'] in ('.data','.rodata','.sdata','.sdata2','.bss','.sbss'):
                i=s_['shndx']
                if i not in cache: cache[i]=masked(secs,relocs,i) if sec['type']!=8 else b''
                data[(sec['name'],s_['name'])]=(s_['size'],cache[i][s_['value']:s_['value']+s_['size']])
    return out,data
def main(version):
    cfg=json.load(open(f'build/{version}/config.json'))
    bad={}
    ninja=open('build.ninja').read()
    for root,_,files in os.walk(f'build/{version}/src'):
        for f in files:
            if not f.endswith('.o'): continue
            src=os.path.join(root,f); rel=os.path.relpath(src,f'build/{version}/src')
            tgt=os.path.join(f'build/{version}/obj',rel)
            if not os.path.exists(tgt): continue
            try:
                a,ad=funcs(src); b,bd=funcs(tgt)
            except Exception as e:
                bad[rel]=f'parse error {e}'; continue
            probs=[]
            for n,code in b.items():
                if n not in a: probs.append(f'missing {n}')
                elif a[n]!=code: probs.append(f'differs {n}')
            for k,(sz,bb) in bd.items():
                if k[1].startswith('...') or (k[1].startswith('@') and not re.search(r'_8[0-9A-F]{7}$',k[1])): continue   # compiler-generated local names differ (keep dtk-exported @N_ADDR)
                if k not in ad: probs.append(f'missing data {k[1]}')
                elif ad[k][0]!=sz: probs.append(f'{k[1]} size {ad[k][0]:#x}!={sz:#x}')
                elif ad[k][1]!=bb: probs.append(f'{k[1]} content')
            if probs: bad[rel]=probs
    return bad
if __name__=='__main__':
    bad=main(sys.argv[1])
    for k,v in sorted(bad.items()): print(k, '|', '; '.join(v[:4]) if isinstance(v,list) else v)
    print('mismatching objects:',len(bad))
