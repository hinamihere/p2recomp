"""Dump the town-map graph (nodes, edges, positions) from a savestate.

usage: mapgraph.py <slot> [count]
Node table pointer at 08E4435C (16-byte nodes, neighbours at +0xA..+0xD, 0xFF = none), position table
pointer at 08E44514 (8-byte entries; node +0..+1 / +2..+3 index them), cursor struct at 08E444F8.
"""
import struct
import sys

RAM_OFF = 24
BASE = 0x08000000


def main():
    slot = int(sys.argv[1])
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    with open(f"states/slot{slot}.p2s", "rb") as f:
        f.seek(RAM_OFF)
        ram = f.read(32 * 1024 * 1024)

    def u32(a): return struct.unpack_from("<I", ram, a - BASE)[0]
    def s16(a): return struct.unpack_from("<h", ram, a - BASE)[0]
    def u16(a): return struct.unpack_from("<H", ram, a - BASE)[0]
    nodes, pos = u32(0x08E4435C), u32(0x08E44514)
    print(f"node table {nodes:08x}, position table {pos:08x}")
    cur = 0x08E444F8
    print("cursor: x=%.2f y=%.2f int=(%d,%d) progress=%08x node=%02x edge=%02x" % (
        s16(cur + 2) + (u16(cur) / 4096.0), s16(cur + 6) + (u16(cur + 4) / 4096.0), u32(cur + 8), u32(cur + 12),
        u32(cur + 16), ram[cur + 0x16 - BASE], ram[cur + 0x17 - BASE]))
    for i in range(count):
        n = nodes + 16 * i
        raw = ram[n - BASE:n - BASE + 16]
        nb = [b for b in raw[10:14]]
        p = u16(n)
        print(f"node {i:2d}: {raw.hex(' ')}  type={raw[4]:02x} nb={['--' if b == 0xFF else f'{b & 0x7f}{chr(33) if b & 0x80 else chr(32)}' for b in nb]}")


if __name__ == "__main__":
    main()
