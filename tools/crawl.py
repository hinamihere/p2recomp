"""Crawl a dungeon area by area: map each area with the in-game mapper, try every interaction spot, and queue the areas that
doors and stairs lead to. Resumable: progress is kept in analysis/maps/crawl.json.

usage: crawl.py <start-slot> [max-areas=12] [name=area]
        crawl.py report                      list what was found

For every area it keeps
  analysis/maps/<name>_<id>.txt   the mapper file (cells, interaction spots, AXES/CELL header)
  analysis/maps/<name>_<id>_banner_*.bmp   the location banner seen after entering it
  states/slot<2000+id>.p2s        savestate at the map's start (after calibration; gitignored)
and in the database the outcome of every spot: DIALOGUE / AREA(child id) / nothing / TIMEOUT / NOROUTE.
Areas count as the same when their banner picture, stick axes and cell size agree.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.environ.get("P2_EXE", os.path.join(ROOT, "build", "release", "p2.exe"))
MAPS = os.path.join(ROOT, "analysis", "maps")
DB = os.path.join(MAPS, "crawl.json")
STATES = os.path.join(ROOT, "states")


def state_vblank(slot):
    with open(os.path.join(STATES, f"slot{slot}.p2s"), "rb") as f:
        return struct.unpack("<IIIQ", f.read(20))[3]


def load_db():
    if os.path.exists(DB):
        return json.load(open(DB))
    return {"areas": {}, "queue": []}


def save_db(db):
    os.makedirs(MAPS, exist_ok=True)
    json.dump(db, open(DB, "w"), indent=1)


def banner_bits(path):
    """Binarised location banner (left 200x28 pixels of the dumped strip): bright text pixels."""
    img = Image.open(path).convert("L").crop((0, 0, 200, 28))
    a = np.asarray(img).astype(float)
    return a > 150


def banner_differs(p1, p2):
    """Different banner text? (1 - intersection/union of the bright pixels; a fade-in of the same text stays below ~0.15,
    "Class 1-C" against "Class 1-D" differs in one glyph and lands well above it)."""
    a, b = banner_bits(p1), banner_bits(p2)
    union = (a | b).sum()
    if union == 0:
        return False
    return 1.0 - (a & b).sum() / union > 0.18


def is_menu_like(path):
    a = np.asarray(Image.open(path).convert("RGB")).astype(int)
    red = (a[3:21, 210:470, 0] > 110) & (a[3:21, 210:470, 1] < 70) & (a[3:21, 210:470, 2] < 70)
    return red.mean() > 0.3


def run(cmd_env, args, timeout=3000):
    env = dict(os.environ, **cmd_env)
    r = subprocess.run([EXE, "--headless", "--turbo-until", "99999999"] + args, cwd=ROOT, capture_output=True, text=True,
                       errors="replace", env=env, timeout=timeout)
    return r.stdout + r.stderr


def parse_map_header(path):
    axes, cell = None, None
    for line in open(path):
        if line.startswith("AXES"):
            axes = [float(v) for v in line.split()[1:5]]
        elif line.startswith("CELL"):
            cell = float(line.split()[1])
            break
    return axes, cell


def same_area(db, banner_png, axes, cell):
    bits = banner_png if banner_png and os.path.exists(banner_png) else None
    for aid, a in db["areas"].items():
        if a.get("axes") is None or axes is None:
            continue
        same_axes = all(abs(x - y) < 0.35 for x, y in zip(a["axes"], axes))
        same_cell = 0.5 < (a["cell"] / cell) < 2.0
        if not (same_axes and same_cell):
            continue
        if bits is not None and a.get("banner") and os.path.exists(a["banner"]):
            if not banner_differs(a["banner"], banner_png):
                return aid
        elif bits is None:
            return aid
    return None


def map_area(db, aid, slot, name):
    a = db["areas"][str(aid)]
    mapfile = os.path.join(MAPS, f"{name}_{aid}.txt")
    ckpt = 900 + (aid % 40)
    start = state_vblank(slot)
    log = run({"P2_NO_ENCOUNTERS": "1", "P2_MAPPER": mapfile, "P2_MAPPER_SLOT": str(ckpt), "P2_MAPPER_EXIT": "1"},
              ["--load-state", str(slot), "--exit-after", str(start + 450000)], timeout=3600)
    if not os.path.exists(mapfile):
        a["status"] = "map-failed"
        return None
    axes, cell = parse_map_header(mapfile)
    a.update(map=os.path.relpath(mapfile, ROOT), axes=axes, cell=cell, complete="mapping complete" in log)
    dest = os.path.join(STATES, f"slot{2000 + aid}.p2s")
    shutil.copy(os.path.join(STATES, f"slot{ckpt}.p2s"), dest)
    a["start_slot"] = 2000 + aid
    return mapfile


def try_spots(db, aid, name, spread=5):
    a = db["areas"][str(aid)]
    mapfile = os.path.join(ROOT, a["map"])
    spots = []
    for line in open(mapfile):
        p = line.split()
        if p and p[0] == "EV" and int(p[3]) < 4:
            spots.append((int(p[1]), int(p[2]), int(p[3])))
    # exits found while walking (stairs): the tour walks into them in the recorded direction(s); numbered after the EV spots
    n_ev = len(spots)
    for line in open(mapfile):
        p = line.split()
        if p and p[0] == "EXIT":
            for dd in range(4):
                if int(p[3]) == 4 or int(p[3]) == dd:
                    spots.append((int(p[1]), int(p[2]), dd + 10))
    done = []
    start = state_vblank(a["start_slot"])
    results = {}
    n = 0
    for i, (x, z, d) in enumerate(spots):
        if d < 10 and any(max(abs(x - u), abs(z - v)) < spread for u, v in done):
            continue
        if d < 10:
            done.append((x, z))
        n += 1
        tmp_slot = 990 + (n % 9)
        banner = os.path.join(MAPS, f"{name}_{aid}_banner_{i}.bmp")
        try:
            log = run({"P2_NO_ENCOUNTERS": "1", "P2_TOUR": f"{mapfile},{i},{i + 1}", "P2_TOUR_SPREAD": "0", "P2_TOUR_AREA_SLOT": str(tmp_slot),
                       "P2_TOUR_BANNER_FILE": banner},
                      ["--load-state", str(a["start_slot"]), "--exit-after", str(start + 9000)], timeout=900)
        except subprocess.TimeoutExpired:
            log = ""
        if re.search(r"AREA CHANGE", log) and os.path.exists(os.path.join(STATES, f"slot{tmp_slot}.p2s")):
            results[str(i)] = {"cell": [x, z, d], "outcome": "AREA", "banner": os.path.relpath(banner, ROOT) if os.path.exists(banner) else None,
                               "tmp_slot": tmp_slot}
            # keep the state under a unique name until classified
            keep = os.path.join(STATES, f"slot{3000 + aid * 100 + n}.p2s")
            shutil.move(os.path.join(STATES, f"slot{tmp_slot}.p2s"), keep)
            results[str(i)]["state"] = 3000 + aid * 100 + n
        else:
            out = "DIALOGUE" if "DIALOGUE" in log else "TIMEOUT" if "timed out" in log else "NOROUTE" if "no known route" in log else "nothing"
            results[str(i)] = {"cell": [x, z, d], "outcome": out}
        print(f"  area {aid} spot #{i} ({x},{z}) dir {d}: {results[str(i)]['outcome']}", flush=True)
    a["spots"] = results


def main():
    if sys.argv[1] == "report":
        db = load_db()
        for aid, a in db["areas"].items():
            spots = a.get("spots", {})
            kinds = {}
            for s in spots.values():
                kinds[s["outcome"]] = kinds.get(s["outcome"], 0) + 1
            print(aid, a.get("status", ""), a.get("map", ""), a.get("cell"), kinds, "parent", a.get("parent"))
        return
    db = load_db()
    name = sys.argv[3] if len(sys.argv) > 3 else "area"
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 12
    if not db["areas"]:
        db["areas"]["0"] = {"parent": None, "state": int(sys.argv[1]), "status": "queued"}
        db["queue"] = [0]
    while db["queue"] and len([a for a in db["areas"].values() if a.get("status") == "done"]) < limit:
        aid = db["queue"].pop(0)
        a = db["areas"][str(aid)]
        if a["status"] != "queued":
            continue
        if a.get("map") and os.path.exists(os.path.join(ROOT, a["map"])) and a.get("start_slot") and os.path.exists(os.path.join(STATES, f"slot{a['start_slot']}.p2s")):
            print(f"== area {aid}: map exists, skipping the mapping", flush=True)
        else:
            print(f"== area {aid}: mapping from slot {a['state']}", flush=True)
            if not map_area(db, aid, a["state"], name):
                save_db(db)
                continue
        # a map of a menu/town-map screen or a duplicate of a known area is not worth touring
        dup = same_area(db, a.get("banner_png"), a["axes"], a["cell"]) if a.get("banner_png") else None
        if dup is not None and dup != str(aid):
            a["status"] = f"duplicate of {dup}"
            save_db(db)
            continue
        print(f"== area {aid}: {len(open(os.path.join(ROOT, a['map'])).readlines())} lines, touring spots", flush=True)
        try_spots(db, aid, name)
        a["status"] = "done"
        # queue the areas reached by doors
        for i, s in a["spots"].items():
            if s["outcome"] != "AREA":
                continue
            banner_png = os.path.join(ROOT, s["banner"]) if s.get("banner") else None
            if banner_png and is_menu_like(banner_png):
                s["outcome"] = "AREA(menu/town map)"
                continue
            # an area reached before (same banner picture, entered from the same side) is not queued again
            known = None
            for oid, o in db["areas"].items():
                ob = o.get("banner")
                if ob and banner_png and os.path.exists(os.path.join(ROOT, ob)) and oid != str(aid) and                         not banner_differs(os.path.join(ROOT, ob), banner_png) and o.get("parent") is not None:
                    known = oid
                    break
            if known is not None:
                s["outcome"] = f"AREA({known})"
                continue
            new_id = max(int(k) for k in db["areas"]) + 1
            db["areas"][str(new_id)] = {"parent": aid, "state": s["state"], "status": "queued", "banner": s.get("banner"), "banner_png": s.get("banner"),
                                        "via": [aid, int(i)]}
            s["outcome"] = f"AREA({new_id})"
            db["queue"].append(new_id)
        save_db(db)
    save_db(db)
    print("queue left:", db["queue"])


if __name__ == "__main__":
    main()
