"""Minimal 32-bit minidump reader: exception, registers, modules, stack return addresses.

Usage: python crash_dump.py <file.dmp> [stack dwords to scan (default 2048)] [hex address ...]
Each extra hex address gets a 0x40-byte dword dump (with module-relative names).
"""
import struct
import sys


def main(path, scan=2048, dumps=()):
    d = open(path, 'rb').read()
    assert d[:4] == b'MDMP'
    nstreams, dir_rva = struct.unpack_from('<II', d, 8)
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from('<III', d, dir_rva + 12 * i)
        streams.setdefault(t, (size, rva))

    def mdstring(rva):
        n = struct.unpack_from('<I', d, rva)[0]
        return d[rva + 4:rva + 4 + n].decode('utf-16le')

    modules = []
    if 4 in streams:
        _, rva = streams[4]
        n = struct.unpack_from('<I', d, rva)[0]
        for i in range(n):
            base, size, _, _, name_rva = struct.unpack_from('<QIIII', d, rva + 4 + 108 * i)
            modules.append((base, size, mdstring(name_rva).split(chr(92))[-1]))

    ranges = []
    if 9 in streams:
        _, rva = streams[9]
        n, base_rva = struct.unpack_from('<QQ', d, rva)
        off = base_rva
        for i in range(n):
            start, size = struct.unpack_from('<QQ', d, rva + 16 + 16 * i)
            ranges.append((start, size, off))
            off += size
    if 5 in streams:
        _, rva = streams[5]
        n = struct.unpack_from('<I', d, rva)[0]
        for i in range(n):
            start, size, mrva = struct.unpack_from('<QII', d, rva + 4 + 16 * i)
            ranges.append((start, size, mrva))

    def read(addr, n):
        for start, size, off in ranges:
            if start <= addr and addr + n <= start + size:
                return d[off + addr - start: off + addr - start + n]
        return None

    def where(a):
        for base, size, name in modules:
            if base <= a < base + size:
                return '%s+0x%X' % (name, a - base)
        return None

    _, rva = streams[6]
    tid = struct.unpack_from('<I', d, rva)[0]
    code, flags, _, addr, nparams = struct.unpack_from('<IIQQI', d, rva + 8)
    params = struct.unpack_from('<2Q', d, rva + 8 + 32)
    ctx_size, ctx_rva = struct.unpack_from('<II', d, rva + 8 + 152)
    print('thread %d exception 0x%08X at 0x%08X (%s) params %s' % (
        tid, code, addr, where(addr), [hex(p) for p in params[:nparams]]))
    regs = {}
    for name, off in [('edi', 0x9C), ('esi', 0xA0), ('ebx', 0xA4), ('edx', 0xA8), ('ecx', 0xAC),
                      ('eax', 0xB0), ('ebp', 0xB4), ('eip', 0xB8), ('esp', 0xC4)]:
        regs[name] = struct.unpack_from('<I', d, ctx_rva + off)[0]
    print(' '.join('%s=%08X' % (k, v) for k, v in regs.items()))
    for base, size, name in modules:
        if name.lower() in ('cnc3game.dat', 'cnc3ep1.dat', 'dinput8.dll'):
            print('module %s base 0x%08X size 0x%X' % (name, base, size))
    print('stack (return-address candidates):')
    esp = regs['esp']
    for i in range(scan):
        b = read(esp + 4 * i, 4)
        if b is None:
            break
        v = struct.unpack('<I', b)[0]
        w = where(v)
        if w and ('cnc3' in w.lower() or 'dinput8' in w.lower()):
            print('  [esp+0x%04X] %08X %s' % (4 * i, v, w))
    for a in dumps:
        print('memory at 0x%08X:' % a)
        for k in range(0, 0x40, 4):
            b = read(a + k, 4)
            if b is None:
                print('  +0x%02X ??' % k)
                continue
            v = struct.unpack('<I', b)[0]
            print('  +0x%02X %08X %s' % (k, v, where(v) or ''))


if __name__ == '__main__':
    main(sys.argv[1], int(sys.argv[2]) if len(sys.argv) > 2 else 2048,
         [int(x, 16) for x in sys.argv[3:]])
