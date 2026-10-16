#!/bin/sh
# One circuit of the Kasugayama Air Raid Shelter loop (F0100 + the two link rooms), stage by stage:
#   1 left half (39,11)->(37,74)  2 door 37 -> bottom room, cross to EXIT_RIGHT  3 right half (56,74)->(58,10)
#   4 door 57 -> top room, cross to EXIT_LEFT -> back at (39,11)
# usage: shelterloop.sh <from-slot> <first-out-slot> [first-stage 1-4]; prints each stage's state; last line "final <slot>".
s=$1; out=$2; stage=${3:-1}
export P2_EXE=C:/p2r/build/dev/p2.exe P2_NO_ENCOUNTERS=1
unset P2_GOTO P2_GOTO_INT P2_GOTO_TOL P2_GOTO_AXES
D=".venv/Scripts/python tools/drive.py"
latest() { python -c "
import glob,os,sys
lo,hi=int(sys.argv[1]),int(sys.argv[2])
c=[f for f in glob.glob('states/slot*.p2s') if f[11:-4].isdigit() and lo<=int(f[11:-4])<hi]
print(max(c,key=os.path.getmtime)[11:-4])" $1 $2; }
cross() { python -c "print(','.join(f'{v}=CROSS:4' for v in range(20,$1,40)))"; }
room() { python -c "
import struct
f=open('states/slot$1.p2s','rb'); f.seek(24); R=f.read(0x2000000); print(R[0xceb2e0],R[0xceb2e4],R[0xce819d],struct.unpack_from('<3i',R,0xe44c14))"; }
flags() { python build/scratch/flags.py 486,487,279,493,494,495 $1; }
walk() { .venv/Scripts/python tools/cellwalk.py build/f100_c0.bin 0 $s $1 $out | tail -1; s=$(latest $out $((out+150))); out=$((out+150)); }
roomcross() { # $1 door direction, $2 goal x,z, $3 axes, $4 exit facing
  python tools/doorpush.py $s $out $1 | tail -1; s=$out; out=$((out+1)); echo "room entered: $(room $s)"
  $D $s $out 3000 "$(cross 3000)" >/dev/null 2>&1; s=$out; out=$((out+1)); echo "after scene: $(room $s)"
  .venv/Scripts/python tools/roomwalk.py $s $out ${2%,*} ${2#*,} $3 40 120 | tail -1; s=$(latest $out $((out+2))); out=$((out+3)); echo "at exit: $(room $s)"
  $D $s $out 400 "5=$4:3,40=CROSS:4,150=CROSS:4" >/dev/null 2>&1; s=$out; out=$((out+1)); echo "after exit: $(room $s)"; flags $s
}
[ $stage -le 1 ] && walk 37,74
[ $stage -le 2 ] && roomcross RIGHT 1340,-400 -1,1,1,1 DOWN+RIGHT
[ $stage -le 3 ] && walk 58,10
[ $stage -le 4 ] && roomcross DOWN 45,-400 1,1,1,-1 DOWN+LEFT
echo "final $s"
