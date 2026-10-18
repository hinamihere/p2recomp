"""Check the `p2-check:` result line pasted into a PR description against the PR itself (no game data needed).

usage: ci_stamp.py <base-sha> <head-sha>      the PR description comes in the environment variable PR_BODY

Passes when the description contains a line printed by tools/ci_check.sh that says PASS, was made at the PR's head
commit, has the head's blob hashes of game/src/native_funcs.cpp and game/overrides.txt, fuzzed every replacement the PR
touches (tools/ci_changed.py) and ran the full set of savestate windows. Catches a missing, stale or edited result line.
It cannot detect a deliberately forged line: the maintainer reruns the check before merging."""
import os
import re
import subprocess
import sys

base, head = sys.argv[1], sys.argv[2]
body = os.environ.get('PR_BODY', '')
git = lambda *a: subprocess.run(['git', *a], capture_output=True, text=True).stdout.strip()

pat = re.compile(r'p2-check: (\w+) head=([0-9a-f]{40}) funcs=([0-9a-f]+) overrides=([0-9a-f]+) fuzzed=(\S+) states=(\d+)/(\d+)')
found = [m for m in map(pat.search, body.splitlines()) if m]
if not found:
    sys.exit('no complete `p2-check:` line in the PR description: run `bash tools/ci_check.sh origin/master` on the PR head and '
             'paste its last line, unedited')
m = found[-1]
result, sha, funcs, overrides, fuzzed, ok, total = m.groups()
errors = []
if result != 'PASS':
    errors.append(f'the check result is {result}, not PASS')
if sha != head:
    errors.append(f'the check ran on {sha[:12]}, but the PR head is {head[:12]}: rerun it on the latest commit')
want_f = git('rev-parse', '--short=12', f'{head}:game/src/native_funcs.cpp')
want_o = git('rev-parse', '--short=12', f'{head}:game/overrides.txt')
if not want_f.startswith(funcs[:12]) or not want_o.startswith(overrides[:12]):
    errors.append('the file hashes in the line do not match the PR head')
changed = subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__), 'ci_changed.py'), base, head],
                         capture_output=True, text=True).stdout.strip()
need = set(filter(None, changed.split(',')))
have = set() if fuzzed == 'none' else set(fuzzed.upper().split(','))
if need - have:
    errors.append(f'replacements changed by the PR but not fuzzed: {",".join(sorted(need - have))}')
if int(total) < 24 or ok != total:
    errors.append(f'savestate windows {ok}/{total}: the full check runs 24')
if errors:
    sys.exit('p2-check line rejected:\n- ' + '\n- '.join(errors))
print(f'p2-check line OK: PASS at {head[:12]}, fuzzed {fuzzed}, {ok}/{total} savestate windows')
