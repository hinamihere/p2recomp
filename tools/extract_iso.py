"""Extract every file from an ISO9660 image into an output directory."""
import os, sys, pycdlib

iso_path, out_dir = sys.argv[1], sys.argv[2]
iso = pycdlib.PyCdlib()
iso.open(iso_path)
for root, dirs, files in iso.walk(iso_path='/'):
    for f in files:
        src = f"{root.rstrip('/')}/{f}"
        name = f.split(';')[0]
        dst = os.path.join(out_dir, root.lstrip('/'), name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        iso.get_file_from_iso(dst, iso_path=src)
        print(f"{os.path.getsize(dst):>12} {root.rstrip('/')}/{name}")
iso.close()
