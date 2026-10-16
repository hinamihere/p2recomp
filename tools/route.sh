#!/bin/sh
# Chain cell walks and stair pushes through a dungeon:  route.sh <chunk0.bin> <from-slot> <first-out-slot> <step>...
# step = "walk:<area>:<col>,<row>" (tools/cellwalk.py) or "stairs:<+row|-row|+col|-col>" (tools/stairpush.py).
# Prints each step's result; the last state written is reported at the end (slot numbers grow by 50 per step).
chunk=$1; s=$2; out=$3; shift 3
latest() { python -c "
import glob,os,sys
lo,hi=int(sys.argv[1]),int(sys.argv[2])
c=[f for f in glob.glob('states/slot*.p2s') if f[11:-4].isdigit() and lo<=int(f[11:-4])<hi]
print(max(c,key=os.path.getmtime)[11:-4] if c else '')" $1 $2; }
for step in "$@"; do
  kind=${step%%:*}; rest=${step#*:}
  case $kind in
    walk) area=${rest%%:*}; goal=${rest#*:}
          .venv/Scripts/python tools/cellwalk.py "$chunk" $area $s $goal $out | tail -1
          n=$(latest $out $((out+50))); [ -n "$n" ] && s=$n ;;
    stairs) python tools/stairpush.py $s $out $rest | tail -1; s=$out ;;
  esac
  out=$((out+50))
done
echo "final $s"
