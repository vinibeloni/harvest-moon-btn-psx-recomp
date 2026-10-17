#!/usr/bin/env python3
"""Minimal MIPS R3000 disassembler for the extracted overlays and the main EXE.

Reads straight from build/overlay_static.json (written by tools/regen.sh) or
from SLUS_011.15, so it needs neither Ghidra nor a running game.

    python3 tools/mdis.py 2    0x800DE4C0 0x800DE5A0   # overlay entry 2 (event_s, window A)
    python3 tools/mdis.py exe  0x8001AF44 0x8001AFD0   # main EXE
    python3 tools/mdis.py list                          # overlay entries and load addresses

Entry indexes are positions in build/overlay_static.json. The same member can
appear twice at different bases (event_s is entry 2 at 0x800B7E20 and entry 13
at 0x8012DDE8): code addresses differ by the base delta, so a patch that must
hold wherever the overlay runs needs both addresses.
"""
import base64
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OV = os.path.join(ROOT, 'build', 'overlay_static.json')
EXE = os.path.join(ROOT, 'SLUS_011.15')

# Names are only ever added for addresses read from the disassembly itself.
NAMES = {
    0x80011EF0: 'SetMode', 0x80011F14: 'GetMode',
    0x8001AF44: 'GetFlag', 0x8001AF70: 'SetFlag',
}
R = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3',
     't4', 't5', 't6', 't7', 's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7',
     't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


def load_overlay(idx):
    d = json.load(open(OV))[idx]
    return int(d['load_addr'], 16), base64.b64decode(d['bytes_b64'])


def load_exe():
    return 0x80010000, open(EXE, 'rb').read()[0x800:]


def s16(x):
    return x - 0x10000 if x & 0x8000 else x


def dis1(pc, w):
    op = w >> 26
    rs = (w >> 21) & 31
    rt = (w >> 16) & 31
    rd = (w >> 11) & 31
    sh = (w >> 6) & 31
    fn = w & 63
    imm = w & 0xFFFF
    si = s16(imm)

    def r(x):
        return '$' + R[x]

    if w == 0:
        return 'nop'
    if op == 0:
        t = {0: 'sll', 2: 'srl', 3: 'sra'}
        if fn in t:
            return '%s %s,%s,%d' % (t[fn], r(rd), r(rt), sh)
        t = {4: 'sllv', 6: 'srlv', 7: 'srav'}
        if fn in t:
            return '%s %s,%s,%s' % (t[fn], r(rd), r(rt), r(rs))
        if fn == 8:
            return 'jr %s' % r(rs)
        if fn == 9:
            return 'jalr %s,%s' % (r(rd), r(rs))
        if fn == 12:
            return 'syscall'
        if fn == 16:
            return 'mfhi %s' % r(rd)
        if fn == 18:
            return 'mflo %s' % r(rd)
        if fn in (24, 25, 26, 27):
            return '%s %s,%s' % ({24: 'mult', 25: 'multu', 26: 'div', 27: 'divu'}[fn], r(rs), r(rt))
        t = {32: 'add', 33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or',
             38: 'xor', 39: 'nor', 42: 'slt', 43: 'sltu'}
        if fn in t:
            return '%s %s,%s,%s' % (t[fn], r(rd), r(rs), r(rt))
        return '.special %02x' % fn
    if op == 1:
        n = {0: 'bltz', 1: 'bgez', 16: 'bltzal', 17: 'bgezal'}.get(rt, 'regimm?')
        return '%s %s,0x%08X' % (n, r(rs), (pc + 4 + si * 4) & 0xFFFFFFFF)
    if op in (2, 3):
        t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return '%s 0x%08X %s' % ('j' if op == 2 else 'jal', t, NAMES.get(t, ''))
    if op in (4, 5):
        return '%s %s,%s,0x%08X' % ('beq' if op == 4 else 'bne', r(rs), r(rt), (pc + 4 + si * 4) & 0xFFFFFFFF)
    if op in (6, 7):
        return '%s %s,0x%08X' % ('blez' if op == 6 else 'bgtz', r(rs), (pc + 4 + si * 4) & 0xFFFFFFFF)
    if op in (8, 9, 10, 11):
        return '%s %s,%s,%d' % ({8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu'}[op], r(rt), r(rs), si)
    if op in (12, 13, 14):
        return '%s %s,%s,0x%X' % ({12: 'andi', 13: 'ori', 14: 'xori'}[op], r(rt), r(rs), imm)
    if op == 15:
        return 'lui %s,0x%X' % (r(rt), imm)
    if op == 16:
        if rs == 0:
            return 'mfc0 %s,c0_%d' % (r(rt), rd)
        if rs == 4:
            return 'mtc0 %s,c0_%d' % (r(rt), rd)
        return 'cop0 %08x' % w
    if op == 18:
        return 'cop2 %08x' % w
    n = {32: 'lb', 33: 'lh', 34: 'lwl', 35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr',
         40: 'sb', 41: 'sh', 42: 'swl', 43: 'sw', 46: 'swr', 50: 'lwc2', 58: 'swc2'}.get(op)
    if n:
        return '%s %s,%d(%s)' % (n, r(rt), si, r(rs))
    return '.op %02x' % op


def dis(base, data, lo, hi):
    for a in range(lo, hi, 4):
        o = a - base
        if o < 0 or o + 4 > len(data):
            print('%08X: <outside image>' % a)
            continue
        w = struct.unpack_from('<I', data, o)[0]
        print('%08X: %08X  %s' % (a, w, dis1(a, w)))


def main(argv):
    if len(argv) >= 2 and argv[1] == 'list':
        for i, e in enumerate(json.load(open(OV))):
            print(i, e['load_addr'], e['size'], e['producer_name'])
        return 0
    if len(argv) != 4:
        print(__doc__)
        return 2
    base, data = load_exe() if argv[1] == 'exe' else load_overlay(int(argv[1]))
    dis(base, data, int(argv[2], 16), int(argv[3], 16))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
