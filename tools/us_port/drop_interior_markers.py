#!/usr/bin/env python3
"""Remove '...<sec>.0' file-start labels that don't sit at a file's start (they hang mwldeppc)."""
import re,sys,collections
SPL,SYM=sys.argv[1:3]
starts=collections.defaultdict(set);cur=None
for l in open(SPL):
    m=re.match(r'^(\S.*):$',l)
    if m: cur=m[1]; continue
    m=re.match(r'^\t(\S+)\s+start:0x([0-9A-F]+) end:0x([0-9A-F]+)',l)
    if m and cur!='Sections': starts[m[1]].add(int(m[2],16))
out=[];n=0
for l in open(SYM).read().split('\n'):
    m=re.match(r'^\.\.\.\S+ = (\S+):0x([0-9A-F]+);',l)
    if m and int(m[2],16) not in starts[m[1]]: n+=1; continue
    out.append(l)
open(SYM,'w').write('\n'.join(out)); print('interior file-start labels removed',n)
