#!/usr/bin/env python3
"""Scan extracted objects for symbol patterns known to hang mwldeppc."""
import sys,os,collections
sys.path.insert(0,os.path.join(os.path.dirname(__file__),'..')); import us_compare as c
DATA=('.rodata','.data','.bss','.sdata','.sbss','.sdata2')
hits=collections.defaultdict(list)
for root,_,fs in os.walk(sys.argv[1] if len(sys.argv)>1 else 'build/GMSE01/obj'):
    for f in fs:
        if not f.endswith('.o'): continue
        p=os.path.join(root,f); secs,syms,rel=c.elf(p)
        starts=collections.Counter()
        for s in syms:
            if not (0<s['shndx']<len(secs)): continue
            sec=secs[s['shndx']]
            if sec['name'] in DATA and s['type']==0 and s['bind']==1: hits[p].append(('untyped global label',s['name']))
            if s['name'].startswith('...'):
                starts[(s['name'],sec['name'])]+=1
                if s['value']!=0: hits[p].append(('interior file-start label',s['name'],hex(s['value'])))
            if sec['name'] in DATA and sec['size']==0 and s['type'] in (0,1) and s['name']: hits[p].append(('symbol in empty section',s['name'],sec['name']))
            if sec['name'] in DATA and s['value']>sec['size']: hits[p].append(('symbol past section end',s['name']))
        for k,n in starts.items():
            if n>1: hits[p].append(('duplicate file-start label',)+k)
for p,h in sorted(hits.items()): print(p.replace('build/GMSE01/obj/',''),h[:4])
print('objects flagged:',len(hits))
