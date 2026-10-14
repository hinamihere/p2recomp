"""Minimal CRI CPK archive reader (list / extract). Enough for the game's pack/P2PT_ALL.cpk.

usage: cpk.py list <file.cpk> [substring]
       cpk.py extract <file.cpk> <out-dir> [substring]

CRILAYLA-compressed members are decompressed on extraction.
"""
import os
import struct
import sys


def read_utf(buf, base):
    """Parse an @UTF table at buf[base:]; returns a list of row dicts."""
    assert buf[base:base + 4] == b"@UTF", "not a @UTF table"
    size = struct.unpack_from(">I", buf, base + 4)[0]
    rows_off, str_off, data_off, name_off = struct.unpack_from(">IIII", buf, base + 0x0C - 2 + 0)[0:4] if False else (
        struct.unpack_from(">H", buf, base + 0x0A)[0],
        struct.unpack_from(">I", buf, base + 0x0C)[0],
        struct.unpack_from(">I", buf, base + 0x10)[0],
        struct.unpack_from(">I", buf, base + 0x14)[0],
    )
    ncols, rowlen, nrows = struct.unpack_from(">HHI", buf, base + 0x18)
    start = base + 8
    strings = start + str_off
    data = start + data_off

    def cstr(off):
        end = buf.index(b"\0", strings + off)
        return buf[strings + off:end].decode("utf-8", "replace")

    pos = base + 0x20
    cols = []
    for _ in range(ncols):
        flags = buf[pos]
        name = cstr(struct.unpack_from(">I", buf, pos + 1)[0])
        pos += 5
        kind, storage = flags & 0x0F, flags >> 4
        const = None
        if storage == 3:
            const, pos = read_value(buf, pos, kind, cstr, data)
        cols.append((name, kind, storage, const))
    rows = []
    for r in range(nrows):
        p = start + rows_off + r * rowlen
        row = {}
        for name, kind, storage, const in cols:
            if storage == 5:
                row[name], p = read_value(buf, p, kind, cstr, data)
            elif storage == 3:
                row[name] = const
            else:
                row[name] = None
        rows.append(row)
    return rows


def read_value(buf, p, kind, cstr, data):
    fmt = {0: ">B", 1: ">b", 2: ">H", 3: ">h", 4: ">I", 5: ">i", 6: ">Q", 7: ">q", 8: ">f"}
    if kind in fmt:
        v = struct.unpack_from(fmt[kind], buf, p)[0]
        return v, p + struct.calcsize(fmt[kind])
    if kind == 0xA:
        return cstr(struct.unpack_from(">I", buf, p)[0]), p + 4
    if kind == 0xB:
        off, size = struct.unpack_from(">II", buf, p)
        return (data + off, size), p + 8
    raise ValueError(f"column type {kind}")


def crilayla(src):
    """Decompress a CRILAYLA block (CRI's LZ variant)."""
    assert src[:8] == b"CRILAYLA"
    usize, hsize = struct.unpack_from("<II", src, 8)
    out = bytearray(usize + 0x100)
    out[:0x100] = src[len(src) - 0x100:]
    pos = len(src) - 0x100 - 1
    bitbuf, bits = 0, 0

    def getbits(n):
        nonlocal pos, bitbuf, bits
        v = 0
        for _ in range(n):
            if bits == 0:
                bitbuf, bits = src[pos], 8
                pos -= 1
            bits -= 1
            v = (v << 1) | ((bitbuf >> bits) & 1)
        return v

    o = usize + 0x100 - 1
    lens = (2, 3, 5, 8)
    while o >= 0x100:
        if getbits(1):
            offset = getbits(13) + 3
            length = 3
            for width in lens:
                part = getbits(width)
                length += part
                if part != (1 << width) - 1:
                    break
            else:
                while True:
                    part = getbits(8)
                    length += part
                    if part != 255:
                        break
            for _ in range(length):
                out[o] = out[o + offset]
                o -= 1
        else:
            out[o] = getbits(8)
            o -= 1
    return bytes(out)


def entries(path):
    with open(path, "rb") as f:
        head = f.read(0x10000)
        assert head[:4] == b"CPK ", "not a CPK"
        info = read_utf(head, 0x10)[0]
        toc_off, content_off = info["TocOffset"], info["ContentOffset"]
        f.seek(toc_off)
        toc = f.read(info["TocSize"] + 0x100)
        assert toc[:4] == b"TOC "
        rows = read_utf(toc, 0x10)
    base = min(toc_off, content_off) if content_off else toc_off
    out = []
    for r in rows:
        name = (r["DirName"] + "/" if r.get("DirName") else "") + r["FileName"]
        out.append((name, base + r["FileOffset"], r["FileSize"], r["ExtractSize"]))
    return out


def main():
    cmd, path = sys.argv[1], sys.argv[2]
    ents = entries(path)
    if cmd == "list":
        needle = sys.argv[3] if len(sys.argv) > 3 else ""
        for name, off, size, ext in ents:
            if needle in name:
                print(f"{off:10d} {size:9d} {ext:9d} {name}")
        print(f"{len(ents)} files")
    elif cmd == "extract":
        out, needle = sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else ""
        with open(path, "rb") as f:
            for name, off, size, ext in ents:
                if needle not in name:
                    continue
                f.seek(off)
                data = f.read(size)
                if data[:8] == b"CRILAYLA":
                    data = crilayla(data)
                target = os.path.join(out, name)
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with open(target, "wb") as w:
                    w.write(data)
                print(name, len(data))


if __name__ == "__main__":
    main()
