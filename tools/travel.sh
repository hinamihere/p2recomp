#!/bin/sh
# Travel between districts: from a town-map state, go to an exit node, open the City Map and pick a district.
# usage: travel.sh <from-slot> <to-slot> <exit-node> <district-index 0=Rengedai 1=Hirasaka 2=Yumezaki 3=Kounan 4=Theater>
# The City Map list starts on the district you came from; the script presses UP 5 times (to the top), then DOWN <index>.
# Writes <to-slot> = the new town map, 300 vblanks after it opened (so P2_MAPNAV can read the node table).
src=$1; dst=$2; exitn=$3; idx=$4
export P2_EXE=${P2_EXE:-C:/p2r/build/dev/p2.exe} P2_NO_ENCOUNTERS=1
V=$(python -c "import struct;print(struct.unpack('<IIIQ',open('states/slot$src.p2s','rb').read(20))[3])")
P2_MAPNAV=$exitn,$((dst+90)) $P2_EXE --headless --load-state $src --turbo-until 99999999 --exit-after $((V+4000)) 2>&1 | grep -E "arrived|no route" | tail -1
IN=$(python -c "
v=10; out=[f'{v}=CROSS:4']; v=150
for i in range(5): out.append(f'{v}=UP:3'); v+=25
for i in range($idx): out.append(f'{v}=DOWN:3'); v+=25
out.append(f'{v}=CROSS:4'); print(','.join(out))")
.venv/Scripts/python tools/drive.py $((dst+90)) $dst 900 "$IN" --sheet 4 >/dev/null 2>&1
python tools/mapgraph.py $dst 1 2>&1 | head -2
