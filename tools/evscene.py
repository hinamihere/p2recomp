"""Decode an event.bin scene package (build/event/script_NNN.bin from eventbin.py): the parts table, the
collision geometry, the trigger table and the script bytecode.

usage: evscene.py <script_NNN.bin> [parts|triggers|collision|labels|dis [LABEL|all]|flags]

Package = u32 count, count u32 part sizes, then the parts 16-byte aligned:
  0 textures, 1 model (NNNN.smd), 2 work area (8 KB), 3 (1 KB), 4 collision, 5 door props (24-byte records),
  6 (4 bytes), 7 triggers, 8 script, 9 "PSPCHECK".
Geometry parts (4 and 7) start with 7 (count, offset) pairs: points (x i16, y i16, z i32, room units,
same as the leader position words at 0x08e44c14), segments (2 point indices + 8-byte tag),
boxes (16 bytes), quads (4 point indices + 8-byte tag).  Trigger tags: byte 0 = script label index,
byte 1 = facing (0..255 = 0..360 degrees), bytes 2-3 = 01 03.
Script (part 8): header u32 x6 (0x18, 0x18, label count, index offset, code offset, text offset);
labels = 64-byte name + u32 instruction index + u32; index = (opcode, arg offset) pairs; args are u32
with bit 31 = register flag and a 31-bit signed value.  Opcodes seen: 0x5e set flag, 0x60 load flag into
register, 0x01/0x02 jump if register set/clear, 0x07 jump, 0x31 place NPC (char, model, x, y, z, dir),
0x2c place party (entry, x, y, z), 0x46 bind char to label, 0x51 warp (0, floor, door, 30, 0),
0x13 show text (text offset), 0x0e end of handler (label).
"""
import struct
import sys


def parts(d):
    n = struct.unpack_from("<I", d, 0)[0]
    sizes = struct.unpack_from("<%dI" % n, d, 4)
    o = (4 + 4 * n + 15) & ~15
    out = []
    for s in sizes:
        out.append(d[o:o + s])
        o = (o + s + 15) & ~15
    return out


def geometry(p):
    """The 7 tables of a geometry part: 0 points (x, y, z, pad as i16), 1 segments (2 point indices + 8-byte tag),
    3 boxes (16 bytes), 4 and 6 quads (4 point indices + 8-byte tag)."""
    hdr = struct.unpack_from("<14I", p, 0)
    tabs = [(hdr[i], hdr[i + 1]) for i in range(0, 14, 2)]
    c, o = tabs[0]
    points = [struct.unpack_from("<hhh", p, o + 8 * i) for i in range(c)]
    c, o = tabs[1]
    segments = [(struct.unpack_from("<HH", p, o + 12 * i), p[o + 12 * i + 4:o + 12 * i + 12]) for i in range(c)]
    c, o = tabs[3]
    boxes = [p[o + 16 * i:o + 16 * i + 16] for i in range(c)]
    quads = []
    for t in (4, 6):
        c, o = tabs[t]
        quads += [(t, struct.unpack_from("<4H", p, o + 16 * i), p[o + 16 * i + 8:o + 16 * i + 16]) for i in range(c)]
    others = [(t, tabs[t]) for t in (2, 5) if tabs[t][0]]
    return points, segments, boxes, quads, others


class Script:
    def __init__(self, p):
        self.p = p
        _, _, nlab, idx0, code0, self.text0 = struct.unpack_from("<6I", p, 0)
        self.labels = {}    # instruction index -> name
        self.names = []     # label number (trigger tags, op 0x46) -> name
        for i in range(nlab):
            e = 0x18 + i * 72
            name = p[e:e + 64].rstrip(b"\x00").decode("ascii", "replace")
            self.labels[struct.unpack_from("<I", p, e + 64)[0]] = name
            self.names.append(name)
        n = (code0 - idx0) // 8
        idx = [struct.unpack_from("<II", p, idx0 + i * 8) for i in range(n)]
        self.ins = []
        for i in range(n):
            a = idx[i][1]
            b = idx[i + 1][1] if i + 1 < n else self.text0
            args = []
            for o in range(a, b, 4):
                v = struct.unpack_from("<I", p, o)[0]
                val = v & 0x7FFFFFFF
                if val & 0x40000000:
                    val -= 0x80000000
                args.append(("r%d" % val) if v & 0x80000000 else val)
            self.ins.append((idx[i][0] & 0xFFFF, args))

    def text(self, off):
        o, out = self.text0 + off, []
        while o + 1 < len(self.p):
            w = struct.unpack_from("<H", self.p, o)[0]
            o += 2
            if 0x20 <= w < 0x7F:
                out.append(chr(w))
            elif w == 0x1120:
                out.append(" ")
            elif w == 0x1101:
                out.append(" / ")
            elif w in (0x1106, 0x1102, 0x1103, 0x1431):
                break
            else:
                out.append("<%04x>" % w)
        return "".join(out)

    def disassemble(self, lo, hi):
        for i in range(lo, hi):
            if i in self.labels:
                print("%s:" % self.labels[i])
            op, args = self.ins[i]
            extra = ""
            if op == 0x13 and args and isinstance(args[0], int):
                extra = "   ; " + self.text(args[0])[:90]
            print("  %4d  op %04x  %s%s" % (i, op, " ".join(str(a) for a in args), extra))


def main():
    d = open(sys.argv[1], "rb").read()
    what = sys.argv[2] if len(sys.argv) > 2 else "triggers"
    ps = parts(d)
    if what == "parts":
        for i, p in enumerate(ps):
            print("part %d: %7d bytes  %s" % (i, len(p), p[:16].hex(" ")))
        return
    script = Script(ps[8])
    if what in ("triggers", "collision"):
        points, segments, boxes, quads, others = geometry(ps[7] if what == "triggers" else ps[4])
        label = lambda tag: script.names[tag[0]] if tag[0] < len(script.names) else "?"
        print("%d points" % len(points))
        for i, (seg, tag) in enumerate(segments):
            a, b = points[seg[0]], points[seg[1]]
            print("segment %d: (%d,%d,%d)-(%d,%d,%d) tag %s label %s facing %d" % (
                i, *a, *b, tag.hex(" "), label(tag), tag[1] * 360 // 256))
        for i, b in enumerate(boxes):
            print("box %d: point %d  %s" % (i, struct.unpack_from("<H", b, 0)[0], b.hex(" ")))
        for i, (t, q, tag) in enumerate(quads):
            pts = [points[k] for k in q]
            xs, zs = [p[0] for p in pts], [p[2] for p in pts]
            print("quad %d (table %d): x %d..%d z %d..%d y %d tag %s label %s facing %d" % (
                i, t, min(xs), max(xs), min(zs), max(zs), pts[0][1], tag.hex(" "), label(tag), tag[1] * 360 // 256))
        for t, (c, o) in others:
            print("table %d: %d records at 0x%x" % (t, c, o))
        if what == "triggers":
            p5 = ps[5]
            for i in range(struct.unpack_from("<I", p5, 0)[0]):
                r = p5[4 + 24 * i:28 + 24 * i]
                print("prop %d: pos %s  %s" % (i, struct.unpack_from("<hhh", r, 0), r[6:].hex(" ")))
    elif what == "labels":
        for k, n in enumerate(script.names):
            print("%4d %s (instruction %d)" % (k, n, [i for i, v in script.labels.items() if v == n][0]))
    elif what == "flags":
        sets = sorted(set(a[0] for op, a in script.ins if op == 0x5E and a))
        tests = sorted(set(a[1] for op, a in script.ins if op == 0x60 and len(a) > 1 and isinstance(a[1], int)))
        print("sets flags", sets)
        print("tests flags", tests)
    elif what == "dis":
        which = sys.argv[3] if len(sys.argv) > 3 else "all"
        starts = sorted(script.labels)
        if which == "all":
            script.disassemble(0, len(script.ins))
        else:
            inv = {v: k for k, v in script.labels.items()}
            lo = inv[which]
            hi = min([s for s in starts if s > lo] + [len(script.ins)])
            script.disassemble(lo, hi)


if __name__ == "__main__":
    main()
