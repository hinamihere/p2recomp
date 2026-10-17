#!/bin/sh
# Try a list of doors on one dungeon floor in order, chaining states: walk to the cell in front of each door (tools/cellwalk.py),
# push into it (tools/doorpush.py), page through the scene with Cross, settle, and report the story flags that changed.
# usage: doorsweep.sh <chunk0.bin> <area> <from-slot> <first-out-slot> <col,row:DIR> ...
#   DIR = the push direction tried first (UP/DOWN/LEFT/RIGHT); each door uses 100 slots from <first-out-slot> on.
# Prints "door <col,row> -> new flags [...] slot <s>"; the chain continues from the state after each door (walking back out first).
chunk=$1; area=$2; s=$3; out=$4; shift 4
export P2_EXE=${P2_EXE:-C:/p2r/build/dev/p2.exe} P2_NO_ENCOUNTERS=1
latest() { python -c "
import glob,os,sys
lo,hi=int(sys.argv[1]),int(sys.argv[2])
c=[f for f in glob.glob('states/slot*.p2s') if f[11:-4].isdigit() and lo<=int(f[11:-4])<hi]
print(max(c,key=os.path.getmtime)[11:-4] if c else '')" $1 $2; }
newflags() { python -c "
f=open('states/slot$1.p2s','rb'); f.seek(24); A=f.read(0x2000000)
f=open('states/slot$2.p2s','rb'); f.seek(24); B=f.read(0x2000000)
a=A[0xe3b414:0xe3b614]; b=B[0xe3b414:0xe3b614]
print([i*8+j for i in range(0x200) for j in range(8) if (b[i]>>j&1) and not (a[i]>>j&1)])"; }
for spec in "$@"; do
  cell=${spec%%:*}; dir=${spec#*:}
  .venv/Scripts/python tools/cellwalk.py $chunk $area $s $cell $out >/dev/null 2>&1
  w=$(latest $out $((out+80))); [ -z "$w" ] && w=$s
  python tools/doorpush.py $w $((out+80)) $dir >/dev/null 2>&1
  IN=$(python -c "print(','.join(f'{v}=CROSS:4' for v in range(100,3000,45)))")
  .venv/Scripts/python tools/drive.py $((out+80)) $((out+81)) 3000 "$IN" --sheet 8 >/dev/null 2>&1
  python tools/settle.py $((out+81)) $((out+82)) MIX 10 UP >/dev/null 2>&1
  echo "door $cell -> new flags $(newflags $s $((out+82))) slot $((out+82)) at $(python tools/where.py $((out+82)) | cut -d' ' -f2-5)"
  cp build/drive/slot$((out+81))_sheet.png progress/door_$cell.png 2>/dev/null
  # leave a room scene (cell 0,0) through any direction before the next door
  case "$(python tools/where.py $((out+82)))" in
    *"cell (0, 0)"*) python tools/roomexit.py $((out+82)) $((out+83)) 0 0 >/dev/null 2>&1; s=$((out+83));;
    *) s=$((out+82));;
  esac
  out=$((out+100))
done
echo "final $s"
