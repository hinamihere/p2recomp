"""Decoder for the chunk container of the dungeon/field files (F0097-F0136.BIN ...): [u32 type][u32 size incl header][u32 uncompressed size]
chunks, 4-byte aligned.  Payload method 2 (type & 0xff == 1 with 0x200 in the next byte) is a byte-oriented LZ:
  c < 0x80  literal run of c+1 bytes
  c >= 0x80 copy (c & 0x7f) + 3 bytes from distance next_byte + 1 (overlapping allowed)
"""
import struct
import sys


def lz_decode(src, usize):
    out = bytearray()
    i = 0
    n = len(src)
    while i < n and len(out) < usize:
        c = src[i]; i += 1
        if c < 0x80:
            out += src[i:i + c + 1]; i += c + 1
        else:
            length = (c & 0x7F) + 3
            dist = src[i] + 1; i += 1
            for _ in range(length):
                out.append(out[-dist])
    return bytes(out), i


def chunks(data):
    p = 0
    while p + 12 <= len(data):
        t, s, u = struct.unpack_from("<III", data, p)
        if s < 12:
            break
        yield p, t, s, u, data[p + 12:p + s]
        p += (s + 3) & ~3


if __name__ == "__main__":
    data = open(sys.argv[1], "rb").read()
    for p, t, s, u, pay in chunks(data):
        out, used = lz_decode(pay, u)
        print(hex(p), hex(t), "size", s, "usize", u, "->", len(out), "used", used, "of", len(pay))
