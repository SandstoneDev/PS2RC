"""Find VU microcode in a PS2 ELF image.

Two strategies:
 1. VIF MPG packets: VIFcode with CMD=0x4A (bit31 = irq allowed), NUM=n (0=>256) 64-bit instrs,
    IMM = load address (in 64-bit units). Payload must decode as valid VU micro pairs.
 2. Raw microcode runs: maximal runs of 8-byte aligned pairs that decode as valid VU pairs and
    contain an E-bit (end) - catches microcode referenced by DMA REF tags without MPG in-line.

usage: python find_vu.py <boot ELF> [--dump DIR]
"""
import sys, os, struct, collections
sys.path.insert(0, os.path.dirname(__file__))
from census import load_main, words
from vumicro import decode_pair, UPPER_NOP, LOWER_NOP


def valid_run(ws, i, n):
    """Check n pairs starting at word index i (i even-aligned to 8 bytes in address)."""
    st = collections.Counter()
    ebits = 0
    for k in range(n):
        lo, up = ws[i + 2 * k], ws[i + 2 * k + 1]
        d = decode_pair(lo, up)
        if d is None:
            return None
        st[d[0]] += 1
        st[d[1]] += 1
        if d[2]['E']:
            ebits += 1
    return st, ebits


def scan_mpg(base, ws):
    hits = []
    for i in range(len(ws) - 2):
        w = ws[i]
        if (w >> 24) & 0x7F != 0x4A:
            continue
        num = (w >> 16) & 0xFF or 256
        imm = w & 0xFFFF
        j = i + 1
        if (base + j * 4) % 8:  # payload must be 64-bit aligned
            continue
        if j + 2 * num > len(ws):
            continue
        r = valid_run(ws, j, num)
        if r is None:
            continue
        st, eb = r
        if st.get('NOP', 0) + st.get('MOVE', 0) > 1.8 * num:  # all-NOP filler is not a program
            continue
        hits.append((base + i * 4, num, imm, eb, st))
    return hits


def scan_raw(base, ws, min_pairs=16):
    runs = []
    i = 0 if base % 8 == 0 else 1
    n = len(ws) - 1
    while i < n:
        k = i
        eb = 0
        while k + 1 < n:
            d = decode_pair(ws[k], ws[k + 1])
            if d is None or (ws[k] == 0 and ws[k + 1] == 0):
                break
            if d[2]['E']:
                eb += 1
            k += 2
        pairs = (k - i) // 2
        if pairs >= min_pairs and eb:
            runs.append((base + i * 4, pairs, eb))
        i = k + 2 if k > i else i + 2
    return runs


def main():
    elf = sys.argv[1]
    dump = sys.argv[sys.argv.index('--dump') + 1] if '--dump' in sys.argv else None
    base, data, entry = load_main(elf)
    ws = words(data)
    hits = scan_mpg(base, ws)
    print('MPG packets: %d' % len(hits))
    for a, num, imm, eb, st in hits:
        top = ', '.join('%s:%d' % kv for kv in st.most_common(8))
        print('  MPG @%08x num=%3d load=%04x(qword %4d) Ebits=%d | %s' % (a, num, imm, imm * 8 // 16, eb, top))
    runs = scan_raw(base, ws)
    print('\nraw microcode runs (>=16 pairs, with E-bit): %d' % len(runs))
    for a, p, eb in runs:
        print('  run @%08x pairs=%4d (%5d bytes) Ebits=%d' % (a, p, p * 8, eb))
    if dump:
        os.makedirs(dump, exist_ok=True)
        for a, num, imm, eb, st in hits:
            off = (a - base) + 4
            open(os.path.join(dump, 'mpg_%08x_n%d_at%04x.vubin' % (a, num, imm)), 'wb').write(data[off:off + num * 8])


if __name__ == '__main__':
    main()
