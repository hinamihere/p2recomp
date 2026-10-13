"""Shared helpers for reading the decrypted EBOOT (PSP PRX, base address 0)."""
import struct
from elftools.elf.elffile import ELFFile

EBOOT = 'bin/EBOOT.dec'
FILE_OFF = 0xC0  # file offset of vaddr 0 (first PT_LOAD), both segments contiguous


class Prx:
    def __init__(self, path=EBOOT):
        self.raw = open(path, 'rb').read()
        self.elf = ELFFile(open(path, 'rb'))
        self.sections = {s.name: s for s in self.elf.iter_sections()}
        segs = [p for p in self.elf.iter_segments() if p['p_type'] == 'PT_LOAD']
        self.segs = [(p['p_vaddr'], p['p_offset'], p['p_filesz'], p['p_memsz']) for p in segs]

    def off(self, va):
        for v, o, fs, ms in self.segs:
            if v <= va < v + fs:
                return o + va - v
        raise ValueError(hex(va))

    def u32(self, va):
        return struct.unpack_from('<I', self.raw, self.off(va))[0]

    def cstr(self, va):
        o = self.off(va)
        return self.raw[o:self.raw.index(b'\0', o)].decode('latin-1')

    def sec(self, name):
        s = self.sections[name]
        return s['sh_addr'], s['sh_size']

    def stubs(self):
        """Yield (libname, attr, [(nid, stub_va)]) for every imported library."""
        a, n = self.sec('.lib.stub')
        p = a
        while p < a + n:
            libname, ver_attr, sizes, nids, stubtab = struct.unpack_from('<IIIII', self.raw, self.off(p))[0:5] if False else (
                self.u32(p), self.u32(p + 4), self.u32(p + 8), self.u32(p + 12), self.u32(p + 16))
            entsz = sizes & 0xFF
            nfunc = sizes >> 16
            name = self.cstr(libname) if libname else '<anon>'
            funcs = [(self.u32(nids + 4 * i), stubtab + 8 * i) for i in range(nfunc)]
            yield name, ver_attr, funcs
            p += entsz * 4


R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6


def relocs(prx):
    """Yield (type, offset_seg, addr_seg, va) for every PRX relocation (SHT 0x700000A0)."""
    segbase = [s[0] for s in prx.segs]
    for s in prx.elf.iter_sections():
        if s['sh_type'] != 0x700000A0:
            continue
        data = s.data()
        for i in range(0, len(data), 8):
            r_off, r_info = struct.unpack_from('<II', data, i)
            yield r_info & 0xFF, (r_info >> 8) & 0xFF, (r_info >> 16) & 0xFF, segbase[(r_info >> 8) & 0xFF] + r_off
