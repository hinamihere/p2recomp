"""Decode branch.bin (from P2PT_ALL.cpk): the door-to-script selectors.

usage: branchbin.py <branch.bin> [index|all]

branch.bin starts with a table of 512 u32 offsets (each blob is listed twice); blob b_NNN sits at file offset
0x800*(NNN+1), i.e. CPK position 220485632 + 0x800*(NNN+1).  Every blob is a gzip stream named
b_NNN.bin holding a tiny script in the event bytecode (see tools/evscene.py): it loads story flags (op 0x60),
branches on them (0x04 r, value, target; 0x02 r, target = jump if clear) and picks what the door does with
op 0x96: (0, N) runs event.bin script N, (1, n) enters room n of the dungeon's room list.  A path that ends
without 0x96 leaves the door inert.

For Seven Sisters High NNN is the field file's door cell id minus 7 (cells 0xC8nn in the C3 layer of F0097 chunk 0):
b_010 is the Principal's Office (flag 260 clear -> script 008), b_011 the Teachers' Lounge (flag 266 set and 267 clear
-> script 010, the emblem-hunt finale).  The game reads the blob with sceIoRead when Cross is pressed at the door.
"""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import evscene  # noqa: E402


def blobs(d):
    first = struct.unpack_from("<I", d, 0)[0]
    table = [struct.unpack_from("<I", d, i)[0] for i in range(0, first, 4)]
    out = {}
    for o in sorted(set(table)):
        if d[o:o + 2] != b"\x1f\x8b":
            continue
        name = d[o + 10:d.index(b"\0", o + 10)].decode("ascii", "replace")
        data = zlib.decompressobj(31).decompress(d[o:o + 0x4000])
        out[o // 0x800 - 1] = (name, data)
    return out


def describe(script):
    parts = []
    for op, args in script.ins:
        if op == 0x60:
            parts.append("load flag %s->%s" % (args[1], args[0]))
        elif op == 0x04:
            parts.append("if %s==%s goto %s" % (args[0], args[1], args[2]))
        elif op == 0x02:
            parts.append("if %s clear goto %s" % (args[0], args[1]))
        elif op == 0x01:
            parts.append("if %s set goto %s" % (args[0], args[1]))
        elif op == 0x96:
            parts.append("RUN script %d" % args[1] if args[0] == 0 else "ENTER room %d" % args[1])
        elif op == 0x07:
            parts.append("goto %s" % args[0])
        elif op == 0x0E:
            parts.append("end")
        else:
            parts.append("op %02x %s" % (op, args))
    return parts


def main():
    d = open(sys.argv[1], "rb").read()
    which = sys.argv[2] if len(sys.argv) > 2 else "all"
    for k, (name, data) in sorted(blobs(d).items()):
        if which != "all" and int(which) != k:
            continue
        script = evscene.Script(data)
        print("blob %d (%s, cpk pos %d):" % (k, name, 220485632 + 0x800 * (k + 1)))
        for i, line in enumerate(describe(script)):
            print("   %3d  %s" % (i, line))


if __name__ == "__main__":
    main()
