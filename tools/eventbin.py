"""Extract the story scripts of event.bin (from P2PT_ALL.cpk, see cpk.py): a table of absolute offsets at the start,
each script a gzip stream.

usage: eventbin.py <event.bin> <out-dir>      writes script_000.bin ...
       eventbin.py text <out-dir> <text-file>  dumps all dialogue of all scripts as readable text ("script offset | speaker: line")
       eventbin.py grep <text-file> <phrase>   finds a phrase in that dump

Text is 16-bit little-endian: ASCII characters as 0x00NN, 0x1120 = space, 0x1101 = line break, 0x1106/1102/1103/1431 end a
line of dialogue (see DEVELOPER.md of github.com/chenetulipe/P2-FR-IS-PSP for the opcodes); the first string of a box is the speaker.
"""
import os
import struct
import sys
import zlib


def scripts(path):
    d = open(path, "rb").read()
    first = struct.unpack_from("<I", d, 0)[0]
    offsets = []
    for i in range(0, first, 4):
        v = struct.unpack_from("<I", d, i)[0]
        if offsets and v < offsets[-1]:
            break
        offsets.append(v)
    starts = sorted(set(o for o in offsets if o))
    out = []
    for o in starts:
        try:
            out.append((o, zlib.decompressobj(31).decompress(d[o:o + 4_000_000])))
        except Exception:
            out.append((o, None))
    return out


def decode_text(data):
    """Yield (offset, [lines]) for every dialogue string: a run of printable words ending in a 0x1106 terminator."""
    words = struct.unpack_from("<%dH" % (len(data) // 2), data, 0)
    i, n = 0, len(words)
    while i < n:
        if 0x20 < words[i] < 0x7F or words[i] == 0x22:
            j, chars = i, []
            while j < n:
                w = words[j]
                if 0x20 <= w < 0x7F:
                    chars.append(chr(w))
                elif w == 0x1120:
                    chars.append(" ")
                elif w == 0x1101:
                    chars.append(" / ")
                elif w in (0x1112, 0x1113):
                    chars.append("<hero>")
                elif w in (0x1107, 0x1108, 0x1205, 0x001E, 0x121E, 0x1210, 0x120C, 0x120D, 0x120E, 0x120F, 0x1208, 0x0002, 0x0014):
                    pass
                else:
                    break
                j += 1
            text = "".join(chars)
            if len(text) >= 3 and j < n and words[j] in (0x1106, 0x1102, 0x1103, 0x1431, 0x1109):
                yield i * 2, text
            i = max(j, i + 1)
        else:
            i += 1


def main():
    if sys.argv[1] == "grep":
        needle = sys.argv[3].lower()
        for line in open(sys.argv[2], encoding="utf-8"):
            if needle in line.lower():
                print(line.rstrip())
        return
    if sys.argv[1] == "text":
        with open(sys.argv[3], "w", encoding="utf-8", newline="") as out:
            for name in sorted(os.listdir(sys.argv[2])):
                data = open(os.path.join(sys.argv[2], name), "rb").read()
                strings = list(decode_text(data))
                for k, (off, text) in enumerate(strings):
                    out.write(f"{name[7:10]} {off:06x} | {text}\n")
        return
    os.makedirs(sys.argv[2], exist_ok=True)
    n = bad = 0
    for k, (o, data) in enumerate(scripts(sys.argv[1])):
        if data is None:
            bad += 1
            continue
        open(os.path.join(sys.argv[2], f"script_{k:03d}.bin"), "wb").write(data)
        n += 1
    print(f"{n} scripts written, {bad} undecodable")


if __name__ == "__main__":
    main()
