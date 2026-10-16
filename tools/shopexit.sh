#!/bin/sh
# Leave a mall shop room back to the town map: walk into the shop's exit quad (warp1 from tools/roomscript.py), then try
# Cross without turning, and Cross after each of the 8 facings, each followed by Circle (mall menu -> town map); stops when the
# town-map node table pointer (0x08E4435C) changes.  usage: shopexit.sh <from-slot> <to-slot> <x> <z> <ux,uz,rx,rz>
src=$1; dst=$2; x=$3; z=$4; ax=$5
export P2_EXE=${P2_EXE:-C:/p2r/build/dev/p2.exe} P2_NO_ENCOUNTERS=1
.venv/Scripts/python tools/roomwalk.py $src $((dst+50)) $x $z $ax 25 120 | tail -1
s=$(python -c "
import glob,os
c=[f for f in glob.glob('states/slot*.p2s') if f[11:-4].isdigit() and $((dst+50))<=int(f[11:-4])<$((dst+60))]
print(max(c,key=os.path.getmtime)[11:-4] if c else '$src')")
tab() { python -c "import struct;f=open('states/slot$1.p2s','rb');f.seek(24+0xe4435c);print(struct.unpack('<I',f.read(4))[0])"; }
T0=$(tab $s)
for d in NONE UP DOWN LEFT RIGHT UP+LEFT UP+RIGHT DOWN+LEFT DOWN+RIGHT; do
  if [ $d = NONE ]; then IN="10=CROSS:4,350=CIRCLE:4"; else IN="5=$d:3,40=CROSS:4,380=CIRCLE:4"; fi
  .venv/Scripts/python tools/drive.py $s $dst 900 "$IN" >/dev/null 2>&1
  if [ "$(tab $dst)" != "$T0" ]; then echo "left the shop ($d)"; exit 0; fi
done
echo "still in the shop"
