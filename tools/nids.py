"""Build a NID -> function-name map from PPSSPP's HLE tables."""
import glob, re

PAT = re.compile(r'\{\s*0[xX]([0-9A-Fa-f]{8})\s*,[^,{}]*,\s*"([A-Za-z0-9_]+)"\s*,\s*\'(.)\'\s*,\s*"([^"]*)"')


def load(root='tools/third_party/ppsspp/Core/HLE'):
    nids = {}
    for path in glob.glob(f'{root}/*.cpp'):
        for m in PAT.finditer(open(path, encoding='utf-8', errors='replace').read()):
            nids.setdefault(int(m.group(1), 16), (m.group(2), m.group(3), m.group(4)))
    return nids
