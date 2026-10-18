#!/bin/bash
# Regenerate config/GMSE01/{symbols,splits}.txt from scratch.
# usage (from sms-main): tools/us_port/build_config.sh WORKDIR
#   WORKDIR holds: us_sections.txt us_refs.json usana/symbols.txt us/main.dol align_*.json
set -e
W=$1; C=config/GMSE01
( cd $W && python3 -I $OLDPWD/tools/us_port/translate2.py $OLDPWD/config/GMSP01/symbols.txt $OLDPWD/config/GMSP01/splits.txt usana/symbols.txt $OLDPWD/$C/symbols.txt $OLDPWD/$C/splits.txt | grep -E "problems|repairs|gaps" )
python3 -I tools/us_port/refsplit.py $C/splits.txt $C/symbols.txt $W/us_refs.json $W/usana/symbols.txt $W/us/main.dol $W/us_sections.txt
python3 -I tools/us_port/bss_refs.py $C/splits.txt $W/us_refs.json $W/usana/symbols.txt $W/us_sections.txt
python3 -I tools/us_port/fixups.py $C/splits.txt $C/symbols.txt $W/us/main.dol $W/us_sections.txt
for round in 1 2 3 4; do
  rm -rf build/GMSE01/obj build/GMSE01/config.json
  build/tools/dtk dol split $C/config.yml build/GMSE01 2>&1 | grep -E "^    [A-Z]|Failed" && exit 1
  n=$(python3 tools/us_port_fix_labels.py $C/symbols.txt | awk '{print $3}')
  echo "split ok, labels converted: $n"
  [ "$n" = "0" ] && break
done
python3 tools/us_port/drop_interior_markers.py $C/splits.txt $C/symbols.txt
python3 tools/us_port/fill_gaps.py $C/splits.txt $C/symbols.txt $W/us/main.dol $W/us_sections.txt
rm -rf build/GMSE01/obj build/GMSE01/config.json
build/tools/dtk dol split $C/config.yml build/GMSE01 2>&1 | grep -E "^    [A-Z]|Failed" && exit 1
echo split after fill ok
ls build/GMSE01/obj | grep auto_ && echo "WARNING: gap objects" || echo "no gap objects"
