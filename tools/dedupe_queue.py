"""Fill in the banner picture of queued crawl areas (from their entry savestates) and drop the ones whose banner matches an area
that is already known: the same room entered through another door.   usage: dedupe_queue.py"""
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import crawl  # noqa: E402

ROOT = crawl.ROOT


def banner_of_state(slot, out):
    frames = os.path.join(ROOT, "build", "frames")
    shutil.rmtree(frames, ignore_errors=True)
    start = crawl.state_vblank(slot)
    subprocess.run([crawl.EXE, "--headless", "-q", "--load-state", str(slot), "--exit-after", str(start + 40), "--dump-frames", "4"],
                   cwd=ROOT, capture_output=True, env=dict(os.environ, P2_DUMP_VBLANK="1"))
    names = sorted(os.listdir(frames))
    from PIL import Image
    img = Image.open(os.path.join(frames, names[-1])).convert("RGB").crop((0, 0, 480, 28))
    img.save(out)


def main():
    db = crawl.load_db()
    keep = []
    for aid in sorted(int(k) for k in db["areas"]):
        a = db["areas"][str(aid)]
        queued = aid in db["queue"]
        if a.get("banner") and os.path.exists(os.path.join(ROOT, a["banner"])) and not queued:
            continue
        if not a.get("state") and not a.get("start_slot"):
            continue
        out = os.path.join(crawl.MAPS, f"area_{aid}_entry_banner.bmp")
        banner_of_state(a.get("state") or a["start_slot"], out)
        a["banner"] = os.path.relpath(out, ROOT)
        if not queued:
            continue
        dup = None
        for oid, o in db["areas"].items():
            if oid == str(aid) or not o.get("banner") or o.get("status", "").startswith("duplicate") or not os.path.exists(os.path.join(ROOT, o["banner"])):
                continue
            if int(oid) < aid and not crawl.banner_differs(os.path.join(ROOT, o["banner"]), out):
                dup = oid
                break
        if dup is not None:
            a["status"] = f"duplicate of {dup}"
            print(f"area {aid}: same banner as area {dup}, dropped")
        else:
            print(f"area {aid}: new banner")
            keep.append(aid)
    db["queue"] = keep
    crawl.save_db(db)


if __name__ == "__main__":
    main()
