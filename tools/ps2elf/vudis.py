"""VU micro disassembler (operand-level) + listing dumper for programs found by vu_programs.py.

Field positions of the upper / lower instruction formats (as PCSX2 VUops.cpp's field macros read them):
  upper: dest[24:21] ft[20:16] fs[15:11] fd[10:6] (bc = [1:0])
  lower: dest[24:21] it/ft[20:16] is/fs[15:11] id/fd[10:6], imm11 [10:0], imm15 split [24:21]|[10:0], fsf[22:21] ftf[24:23]
usage: python vudis.py <elf> <programs.json> <outdir>
"""
import sys, os, json, struct
sys.path.insert(0, os.path.dirname(__file__))
from vumicro import upper, lower, upper_flags
from census import load_main, words
from vu_programs_lib import program_code

BC_OPS = ('ADD', 'SUB', 'MADD', 'MSUB', 'MAX', 'MINI', 'MUL', 'ADDA', 'SUBA', 'MADDA', 'MSUBA', 'MULA')


def dest(w):
    return ''.join(c for c, b in zip('xyzw', (24, 23, 22, 21)) if w >> b & 1)


def F(n):
    return 'vf%02d' % n


def I(n):
    return 'vi%02d' % n


def dis_upper(w):
    m = upper(w)
    if m is None:
        return '???'
    if m == 'NOP':
        return 'nop'
    d = dest(w)
    ft, fs, fd = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    special = (w & 63) >= 60
    if m == 'CLIP':
        return 'clipw.xyz %s, %sw' % (F(fs), F(ft))
    if m.startswith(('ITOF', 'FTOI')) or m == 'ABS':
        return '%s.%s %s, %s' % (m.lower(), d, F(ft), F(fs))
    dst = 'ACC' if special else F(fd)
    if m[-1] in 'xyzw' and m[:-1] in BC_OPS:
        return '%s.%s %s, %s, %s%s' % (m.lower(), d, dst, F(fs), F(ft), m[-1])
    if m[-1] in 'qi' and m[:-1] in BC_OPS:
        return '%s.%s %s, %s, %s' % (m.lower(), d, dst, F(fs), m[-1].upper())
    return '%s.%s %s, %s, %s' % (m.lower(), d, dst, F(fs), F(ft))


def simm11(w):
    v = w & 0x7FF
    return v - 0x800 if v & 0x400 else v


def dis_lower(w, pc):
    m = lower(w)
    if m is None:
        return '???'
    d = dest(w)
    it, is_, id_ = (w >> 16) & 31, (w >> 11) & 31, (w >> 6) & 31
    ft, fs = it, is_
    if w == 0x8000033C:
        return 'nop'
    if m == 'LQ':
        return 'lq.%s %s, %d(%s)' % (d, F(ft), simm11(w), I(is_))
    if m == 'SQ':
        return 'sq.%s %s, %d(%s)' % (d, F(fs), simm11(w), I(it))
    if m in ('ILW', 'ISW'):
        return '%s.%s %s, %d(%s)' % (m.lower(), d, I(it), simm11(w), I(is_))
    if m in ('IADDIU', 'ISUBIU'):
        return '%s %s, %s, 0x%x' % (m.lower(), I(it), I(is_), ((w >> 10) & 0x7800) | (w & 0x7FF))
    if m in ('IADD', 'ISUB', 'IAND', 'IOR'):
        return '%s %s, %s, %s' % (m.lower(), I(id_), I(is_), I(it))
    if m == 'IADDI':
        v = (w >> 6) & 31
        v = v - 32 if v & 16 else v
        return 'iaddi %s, %s, %d' % (I(it), I(is_), v)
    if m in ('B', 'BAL', 'IBEQ', 'IBNE', 'IBLTZ', 'IBGTZ', 'IBLEZ', 'IBGEZ'):
        tgt = pc + 1 + simm11(w)
        if m == 'B':
            return 'b 0x%03x' % tgt
        if m == 'BAL':
            return 'bal %s, 0x%03x' % (I(it), tgt)
        if m in ('IBEQ', 'IBNE'):
            return '%s %s, %s, 0x%03x' % (m.lower(), I(it), I(is_), tgt)
        return '%s %s, 0x%03x' % (m.lower(), I(is_), tgt)
    if m == 'JR':
        return 'jr %s' % I(is_)
    if m == 'JALR':
        return 'jalr %s, %s' % (I(it), I(is_))
    if m in ('DIV', 'RSQRT'):
        return '%s Q, %s%s, %s%s' % (m.lower(), F(fs), 'xyzw'[(w >> 21) & 3], F(ft), 'xyzw'[(w >> 23) & 3])
    if m == 'SQRT':
        return 'sqrt Q, %s%s' % (F(ft), 'xyzw'[(w >> 23) & 3])
    if m in ('MOVE', 'MR32'):
        return '%s.%s %s, %s' % (m.lower(), d, F(ft), F(fs))
    if m in ('LQI', 'LQD'):
        return '%s.%s %s, (%s%s)' % (m.lower(), d, F(ft), I(is_), '++' if m == 'LQI' else '--')
    if m in ('SQI', 'SQD'):
        return '%s.%s %s, (%s%s)' % (m.lower(), d, F(fs), I(it), '++' if m == 'SQI' else '--')
    if m == 'MTIR':
        return 'mtir %s, %s%s' % (I(it), F(fs), 'xyzw'[(w >> 21) & 3])
    if m == 'MFIR':
        return 'mfir.%s %s, %s' % (d, F(ft), I(is_))
    if m in ('ILWR', 'ISWR'):
        return '%s.%s %s, (%s)' % (m.lower(), d, I(it), I(is_))
    if m == 'XGKICK':
        return 'xgkick %s' % I(is_)
    if m in ('XTOP', 'XITOP'):
        return '%s %s' % (m.lower(), I(it))
    if m in ('FCAND', 'FCOR', 'FCEQ', 'FCSET'):
        return '%s vi01, 0x%06x' % (m.lower(), w & 0xFFFFFF)
    if m == 'FCGET':
        return 'fcget %s' % I(it)
    if m in ('FMAND', 'FMEQ', 'FMOR'):
        return '%s %s, %s' % (m.lower(), I(it), I(is_))
    if m in ('FSAND', 'FSEQ', 'FSOR', 'FSSET'):
        return '%s %s, 0x%x' % (m.lower(), I(it), ((w >> 10) & 0x800) | (w & 0x7FF))
    if m == 'MFP':
        return 'mfp.%s %s, P' % (d, F(ft))
    if m in ('WAITQ', 'WAITP'):
        return m.lower()
    if m.startswith('E'):
        return '%s P, %s' % (m.lower(), F(fs))
    if m in ('RNEXT', 'RGET'):
        return '%s.%s %s, R' % (m.lower(), d, F(ft))
    if m in ('RINIT', 'RXOR'):
        return '%s R, %s%s' % (m.lower(), F(fs), 'xyzw'[(w >> 21) & 3])
    return m.lower()


def main():
    elf, pj, out = sys.argv[1:4]
    base, data, _ = load_main(elf)
    ws = words(data)
    progs = json.load(open(pj))
    os.makedirs(out, exist_ok=True)
    for p in progs:
        code = program_code(base, ws, int(p['addr'], 16))
        hdr = '; VU program #%d  ELF@%s  unit=%s  span=%d instr  sha1=%s' % (
            p['idx'], p['addr'], p['unit'], p['span_instr'], p['sha1'])
        if p['dup_of'] is not None:
            hdr += '  DUPLICATE of #%d' % p['dup_of']
        lines = [hdr]
        for k in sorted(code):
            lo, up = code[k]
            fl = upper_flags(up)
            flags = ''.join(f for f in 'EMDT' if fl[f])
            u = dis_upper(up)
            if fl['I']:
                l = 'loi 0x%08x  ; %g' % (lo, struct.unpack('<f', lo.to_bytes(4, 'little'))[0])
            else:
                l = dis_lower(lo, k)
            lines.append('%03x: %-6s %-38s %s' % (k, '[' + flags + ']' if flags else '', u, l))
        open(os.path.join(out, 'vu_%02d_%s.vsm' % (p['idx'], p['addr'])), 'w').write('\n'.join(lines) + '\n')
    print('wrote', len(progs), 'listings to', out)


if __name__ == '__main__':
    main()
