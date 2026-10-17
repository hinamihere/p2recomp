"""Delete savestates except milestones: keeps the slots listed in states/KEEP (one number per line) and everything >= <from>.

usage: prune_states.py <from-slot>     e.g. prune_states.py 17000  (keeps the live chain of the current dungeon)"""
import glob
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
lo = int(sys.argv[1])
keep_file = os.path.join(ROOT, 'states', 'KEEP')
keep = {int(l) for l in open(keep_file) if l.strip().isdigit()} if os.path.exists(keep_file) else set()
n = freed = 0
for f in glob.glob(os.path.join(ROOT, 'states', 'slot*.p2s')):
    s = os.path.basename(f)[4:-4]
    if s.isdigit() and int(s) < lo and int(s) not in keep:
        freed += os.path.getsize(f); os.remove(f); n += 1
print(f'deleted {n} states, {freed / 1e9:.1f} GB')
