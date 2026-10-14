"""Render a map file written by the in-game mapper (P2_MAPPER) as text and/or a PNG.

usage: mapview.py <map.txt> [out.png]
Cells are 40 world units. Each cell is drawn 4x4 pixels in the text view: '.' visited, passable edges open,
'#' blocked edges, '?' unknown edges, 'E' interaction event, 'X' area exit.
"""
import sys


def load(path):
    cells, visited, events, exits = {}, set(), set(), set()
    for line in open(path):
        p = line.split()
        if not p:
            continue
        if p[0] == "C":
            cells[(int(p[1]), int(p[2]))] = [int(v) for v in p[3:7]]
        elif p[0] == "V":
            visited.add((int(p[1]), int(p[2])))
        elif p[0] == "EV":
            events.add((int(p[1]), int(p[2]), int(p[3])))
        elif p[0] == "EXIT":
            exits.add((int(p[1]), int(p[2]), int(p[3])))
    return cells, visited, events, exits


def main():
    cells, visited, events, exits = load(sys.argv[1])
    xs = [c[0] for c in cells] or [0]
    zs = [c[1] for c in cells] or [0]
    x0, x1, z0, z1 = min(xs) - 1, max(xs) + 1, min(zs) - 1, max(zs) + 1
    print(f"{len(cells)} cells, {len(visited)} visited, {len(events)} events, {len(exits)} exits; x {x0}..{x1}, z {z0}..{z1}")
    if (x1 - x0) > 150:
        print("too wide for text view")
    else:
        for z in range(z0, z1 + 1):
            top, mid = "", ""
            for x in range(x0, x1 + 1):
                e = cells.get((x, z))
                if e is None:
                    top += "  "
                    mid += "  "
                    continue
                up, down, left, right = e
                sym = lambda v: "." if v == 1 else ("#" if v == 0 else "?")
                top += " " + sym(up)
                mid += sym(left) + ("o" if (x, z) in visited else "+")
            print(top)
            print(mid)
    if len(sys.argv) > 2:
        from PIL import Image
        S = 6
        img = Image.new("RGB", ((x1 - x0 + 1) * S, (z1 - z0 + 1) * S), (20, 20, 30))
        px = img.load()
        for (x, z), e in cells.items():
            bx, bz = (x - x0) * S, (z - z0) * S
            col = (200, 200, 200) if (x, z) in visited else (90, 90, 120)
            for i in range(1, S - 1):
                for j in range(1, S - 1):
                    px[bx + i, bz + j] = col
            for d, v in enumerate(e):
                if v == 0:
                    for i in range(S):
                        if d == 0: px[bx + i, bz] = (230, 60, 60)
                        if d == 1: px[bx + i, bz + S - 1] = (230, 60, 60)
                        if d == 2: px[bx, bz + i] = (230, 60, 60)
                        if d == 3: px[bx + S - 1, bz + i] = (230, 60, 60)
        for (x, z, d) in events:
            px[(x - x0) * S + 2, (z - z0) * S + 2] = (60, 255, 60)
        for (x, z, d) in exits:
            px[(x - x0) * S + 3, (z - z0) * S + 3] = (255, 220, 0)
        img.save(sys.argv[2])


if __name__ == "__main__":
    main()
