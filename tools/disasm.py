"""Disassemble a range of the decrypted EBOOT at runtime addresses, naming import stubs.

usage: disasm.py <start_hex> <end_hex>
"""
import struct
import sys

import rabbitizer as rz

sys.path.insert(0, 'tools')
from elfinfo import Prx  # noqa: E402
import nids  # noqa: E402

BASE = 0x08804000
prx = Prx()
names = nids.load()
stubs = {}
for lib, _attr, funcs in prx.stubs():
    for nid, stub in funcs:
        stubs[stub + BASE] = names[nid][0] if nid in names else f'{lib}_{nid:08X}'

start, end = int(sys.argv[1], 16), int(sys.argv[2], 16)
for va in range(start, end, 4):
    word = struct.unpack_from('<I', prx.raw, prx.off(va - BASE))[0]
    ins = rz.Instruction(word, va, rz.InstrCategory.R4000ALLEGREX)
    text = ins.disassemble()
    note = ''
    if ins.isJumpWithAddress():
        # The file is unrelocated: jump targets are module-relative.
        target = (ins.getInstrIndexAsVram() & 0x0FFFFFFF) + BASE
        note = '  ; ' + stubs.get(target, f'{target:08X}')
    print(f'{va:08X}: {word:08X}  {text}{note}')
