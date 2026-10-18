"""Savestate regression harness: replays a short window from every milestone savestate in deterministic mode and checks the result.

usage: regress.py verify  [--frames N] [--jobs J] [--limit K] [slot ...]
       regress.py compare [--frames N] [--jobs J] [--limit K] [slot ...]
       regress.py record  [--frames N] [--jobs J] [--limit K] [slot ...]
       regress.py check   [--frames N] [--jobs J] [--limit K] [slot ...]

  compare  runs each window twice with the same build - original recompiled code only (P2_NATIVE=off) and with the hand-written
           replacements from game/overrides.txt - and diffs the resulting states (RAM, VRAM, kernel/GE/audio sections).
  verify   runs each window once with P2_NATIVE=verify (every replacement is run next to its original on identical state and
           all registers and RAM are compared on every call); names the replacement that mismatched. Also prints how often each
           replacement was exercised over all windows (a replacement no window calls is untested here).
  record   stores a hash of each window's result (replacements on) in regress/baseline.json.
  check    reruns the windows and compares them against regress/baseline.json (catches any change in behaviour between builds).

The slots default to states/KEEP (one milestone per line; missing files are skipped). Every window gets the same input script:
Cross every 45 vblanks with alternating d-pad holds, so menus, dialogue, battles and walking all get exercised. Runs use
P2_DETERMINISTIC=2 and no cheats (random encounters stay on: battles are deterministic too), so the outcome depends only on the savestate, the input and the code.
Exit status 1 when anything differs."""
import concurrent.futures
import hashlib
import json
import os
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get('P2_EXE', os.path.join(ROOT, 'build', 'dev', 'p2.exe'))
BASELINE = os.path.join(ROOT, 'regress', 'baseline.json')
OUT_BASE = 9_500_000  # scratch slot numbers: OUT_BASE + 2 * index (+1 for the second run)
RAM, RAM_SIZE = 24, 0x2000000
# Overrides that are cheat hooks, not decompiled functions: they call the original themselves, so verify mode runs it twice.
HOOKS = {'08962be8'}


def vblank(slot):
    with open(os.path.join(ROOT, 'states', f'slot{slot}.p2s'), 'rb') as f:
        return struct.unpack('<IIIQ', f.read(20))[3]


def script(start, frames):
    pads = ['UP', 'RIGHT', 'DOWN', 'LEFT']
    out, t, k = [], 10, 0
    while t < frames - 30:
        out.append(f'{start + t}=CROSS:4')
        if k % 3 == 1:
            out.append(f'{start + t + 8}={pads[(k // 3) % 4]}:20')
        t += 45
        k += 1
    return ','.join(out)


def run(slot, out, frames, natives):
    start = vblank(slot)
    end = start + frames
    env = {k: v for k, v in os.environ.items() if k not in ('P2_NO_ENCOUNTERS', 'P2_NATIVE', 'P2_POKE', 'P2_POKE_HOLD')}
    env['P2_DETERMINISTIC'] = '2'
    if not natives:
        env['P2_NATIVE'] = 'off'
    target = os.path.join(ROOT, 'states', f'slot{out}.p2s')
    if os.path.exists(target):
        os.remove(target)
    cmd = [EXE, '--headless', '-q', '--load-state', str(slot), '--input', script(start, frames), '--turbo-until', str(end),
           '--save-state-at', f'{end}:{out}', '--exit-after-save', '--exit-after', str(end + 240)]
    proc = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, errors='replace', timeout=1800)
    if not os.path.exists(target):
        tail = (proc.stdout + proc.stderr).strip().splitlines()[-3:]
        return None, 'no state written: ' + ' | '.join(tail)
    data = open(target, 'rb').read()
    os.remove(target)
    return data, None


def run_verify(slot, frames):
    """Run the window with P2_NATIVE=verify: every replacement runs next to its original on identical state for the whole window.
    Returns {function: (verified, mismatches)} and the first mismatch lines."""
    import re
    start = vblank(slot)
    end = start + frames
    env = {k: v for k, v in os.environ.items() if k not in ('P2_NO_ENCOUNTERS', 'P2_NATIVE', 'P2_POKE', 'P2_POKE_HOLD')}
    env.update(P2_DETERMINISTIC='2', P2_NATIVE='verify')  # first 300 calls per replacement (verify compares all of RAM per call)
    cmd = [EXE, '--headless', '--load-state', str(slot), '--input', script(start, frames), '--turbo-until', str(end),
           '--exit-after', str(end)]
    proc = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True, text=True, errors='replace', timeout=1800)
    text = proc.stdout + proc.stderr
    stats = {m.group(1): (int(m.group(2)), int(m.group(3)))
             for m in re.finditer(r'native\] ([0-9a-f]{8}): (\d+) verified, (\d+) mismatches', text)}
    first = [l.split('native] ', 1)[-1] for l in text.splitlines() if 'MISMATCH' in l][:3]
    return stats, first


def first_diffs(a, b, n=6):
    """Up to n differing RAM address ranges, as guest addresses."""
    out, i = [], RAM
    end = RAM + RAM_SIZE
    while i < end and len(out) < n:
        j = i
        step = 0x1000
        while j < end and a[j:j + step] == b[j:j + step]:
            j += step
        if j >= end:
            break
        k = j
        while k < min(j + step, end) and a[k] == b[k]:
            k += 1
        m = k
        while m < end and a[m] != b[m] and m - k < 0x100:
            m += 1
        out.append(f'{0x08000000 + k - RAM:08x}+{m - k:x}')
        i = (m + 0xFFF) & ~0xFFF
    return out


def job(mode, idx, slot, frames, baseline):
    if mode == 'verify':
        stats, first = run_verify(slot, frames)
        bad = {f: m for f, (v, m) in stats.items() if m and f not in HOOKS}
        if bad:
            return slot, 'DIFF', ', '.join(f'{f}: {m} mismatches' for f, m in bad.items()) + ' | ' + ' | '.join(first)
        return slot, 'ok', stats
    if mode == 'compare':
        a, err = run(slot, OUT_BASE + 2 * idx, frames, False)
        if err:
            return slot, 'error', err
        b, err = run(slot, OUT_BASE + 2 * idx + 1, frames, True)
        if err:
            return slot, 'error', err
        if a == b:
            return slot, 'ok', ''
        ram_same = a[RAM:RAM + RAM_SIZE] == b[RAM:RAM + RAM_SIZE]
        return slot, 'DIFF', 'RAM same, other sections differ' if ram_same else 'RAM ' + ' '.join(first_diffs(a, b))
    data, err = run(slot, OUT_BASE + 2 * idx, frames, True)
    if err:
        return slot, 'error', err
    h = hashlib.sha256(data).hexdigest()
    if mode == 'record':
        return slot, 'recorded', h
    want = baseline.get(str(slot))
    if want is None:
        return slot, 'new', h
    return slot, 'ok' if want == h else 'DIFF', '' if want == h else 'hash differs from baseline'


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ('compare', 'verify', 'record', 'check'):
        sys.exit(__doc__)
    mode = args.pop(0)
    opt = {'--frames': 600, '--jobs': max(1, (os.cpu_count() or 4) // 2), '--limit': 0}
    slots = []
    while args:
        a = args.pop(0)
        if a in opt:
            opt[a] = int(args.pop(0))
        else:
            slots.append(int(a))
    if not slots:
        seen = set()
        for line in open(os.path.join(ROOT, 'states', 'KEEP')):
            line = line.strip()
            if line.isdigit() and int(line) not in seen and os.path.exists(os.path.join(ROOT, 'states', f'slot{line}.p2s')):
                seen.add(int(line))
                slots.append(int(line))
        slots.sort()
    if opt['--limit'] and len(slots) > opt['--limit']:
        step = len(slots) / opt['--limit']
        slots = [slots[int(i * step)] for i in range(opt['--limit'])]
    baseline = json.load(open(BASELINE)) if mode == 'check' and os.path.exists(BASELINE) else {}
    print(f'{mode}: {len(slots)} states, {opt["--frames"]} vblanks each, {opt["--jobs"]} jobs', flush=True)
    results = {}
    with concurrent.futures.ThreadPoolExecutor(opt['--jobs']) as pool:
        futs = [pool.submit(job, mode, i, s, opt['--frames'], baseline) for i, s in enumerate(slots)]
        for f in concurrent.futures.as_completed(futs):
            slot, status, info = f.result()
            results[slot] = (status, info)
            if status not in ('ok', 'recorded'):
                print(f'  {slot}: {status} {info}', flush=True)
    if mode == 'verify':
        total = {}
        for status, info in results.values():
            if status == 'ok':
                for f, (v, m) in info.items():
                    total[f] = total.get(f, 0) + v
        print('calls verified per replacement:', ', '.join(f'{f} {n}' for f, n in sorted(total.items())) or 'none')
    counts = {}
    for status, _ in results.values():
        counts[status] = counts.get(status, 0) + 1
    print('summary:', ', '.join(f'{k} {v}' for k, v in sorted(counts.items())))
    if mode == 'record':
        os.makedirs(os.path.dirname(BASELINE), exist_ok=True)
        old = json.load(open(BASELINE)) if os.path.exists(BASELINE) else {}
        old.update({str(s): info for s, (status, info) in results.items() if status == 'recorded'})
        json.dump(dict(sorted(old.items(), key=lambda kv: int(kv[0]))), open(BASELINE, 'w'), indent=1)
        print('baseline:', BASELINE, len(old), 'entries')
    sys.exit(1 if any(st in ('DIFF', 'error') for st, _ in results.values()) else 0)


if __name__ == '__main__':
    main()
