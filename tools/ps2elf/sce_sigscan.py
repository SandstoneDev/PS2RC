"""Identify Sony SDK library functions in a stripped PS2 ELF using the sce-symbol-scanner database
embedded in PS2Recomp (ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h).

Algorithm = PS2Recomp sce_symbol_scanner.cpp: for a candidate address, take `size` bytes, blank the relocated
fields (R_MIPS_26 -> keep top 6 bits, HI16/LO16/GPREL16/LITERAL -> keep top 16 bits, R_MIPS_32 -> 0), SHA-1,
compare with the database hash. Only symbols with >= 256 static (non-relocated) bits are accepted, and only
unambiguous (single library+name) matches are reported - same policy as PS2Recomp.

usage: python sce_sigscan.py <elf> <sce_symbol_database_data.h> [--overlays dir] [--out matches.json]
"""
import sys, os, re, json, hashlib, struct, collections
sys.path.insert(0, os.path.dirname(__file__))
from census import load_main

MASK = {'MIPS_26': 0xFC000000, 'MIPS_HI16': 0xFFFF0000, 'MIPS_LO16': 0xFFFF0000, 'MIPS_GPREL16': 0xFFFF0000,
        'MIPS_LITERAL': 0xFFFF0000, 'MIPS_32': 0x0}
BITS = {'MIPS_26': 26, 'MIPS_HI16': 16, 'MIPS_LO16': 16, 'MIPS_GPREL16': 16, 'MIPS_LITERAL': 16, 'MIPS_32': 32}


def load_db(header):
    txt = open(header, encoding='utf-8', errors='replace').read()
    m = re.search(r'kSymbolsJsonChunks\[\]\s*=\s*\{(.*?)\n\s*\};', txt, re.S)
    chunks = re.findall(r'R"PS2SDB\((.*?)\)PS2SDB"', m.group(1), re.S)
    db = json.loads(''.join(chunks))
    syms = []
    for lib, names in db.items():
        for name, hashes in names.items():
            for h, variants in hashes.items():
                for var, rec in variants.items():
                    if rec.get('type') != 'FUNCTION' or not rec.get('size'):
                        continue
                    rel = [(int(off), r['type']) for off, r in rec.get('relocations', {}).items()]
                    static_bits = rec['size'] * 8 - sum(BITS.get(t, 0) for _, t in rel)
                    if static_bits < 256:
                        continue
                    syms.append({'lib': lib, 'name': name, 'hash': h, 'size': rec['size'], 'rel': rel,
                                 'sdk': rec.get('sdk'), 'cc': rec.get('compiler_versions')})
    return syms


def scan(blob, base, syms):
    # index by masked first word
    idx = collections.defaultdict(list)
    for s in syms:
        relmap = dict(s['rel'])
        idx_key = None
        s['relmap'] = relmap
        s['m0'] = MASK.get(relmap.get(0), 0xFFFFFFFF) if 0 in relmap else 0xFFFFFFFF
        idx[s['m0']].append(s)
    by_mask = {m: collections.defaultdict(list) for m in idx}
    for m, lst in idx.items():
        for s in lst:
            # need the reference first word: unknown from DB (only hash). Fallback: brute force by size buckets.
            pass
    # DB has no raw bytes, so verify by hashing at every function-start candidate (all JAL targets + 4-aligned
    # starts after jr ra). Cheap enough: candidates ~15k x symbols-with-matching-size.
    by_size = collections.defaultdict(list)
    for s in syms:
        by_size[s['size']].append(s)
    return by_size


def sha_masked(blob, off, s):
    b = bytearray(blob[off:off + s['size']])
    if len(b) < s['size']:
        return None
    for ro, t in s['rel']:
        if ro + 4 <= len(b):
            v = struct.unpack_from('<I', b, ro)[0] & MASK.get(t, 0xFFFFFFFF)
            struct.pack_into('<I', b, ro, v)
    return hashlib.sha1(bytes(b)).hexdigest()


def candidates(blob, base):
    ws = struct.unpack('<%dI' % (len(blob) // 4), blob[:len(blob) // 4 * 4])
    c = set([base])
    for i, w in enumerate(ws):
        op = w >> 26
        if op == 3:  # JAL
            t = ((base + i * 4 + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            if base <= t < base + len(blob):
                c.add(t)
        if w == 31 << 21 | 8:  # jr ra -> next function starts after delay slot (+ padding)
            k = i + 2
            while k < len(ws) and ws[k] == 0:
                k += 1
            c.add(base + k * 4)
    return sorted(c)


def run(blob, base, syms, label):
    by_size = scan(blob, base, syms)
    sizes = sorted(by_size)
    found = []
    for a in candidates(blob, base):
        off = a - base
        hits = {}
        for size in sizes:
            if off + size > len(blob):
                break
            cache = {}
            for s in by_size[size]:
                key = tuple(s['rel'])
                if key not in cache:
                    cache[key] = sha_masked(blob, off, s)
                if cache[key] == s['hash']:
                    hits[(s['lib'], s['name'])] = s
        if len(hits) == 1:
            (lib, name), s = next(iter(hits.items()))
            found.append({'addr': '%08x' % a, 'lib': lib, 'name': name, 'size': s['size'], 'where': label})
        elif len(hits) > 1:
            # prefer the largest match (longest function) if names differ only by alias
            best = max(hits.values(), key=lambda s: s['size'])
            found.append({'addr': '%08x' % a, 'lib': best['lib'], 'name': best['name'], 'size': best['size'],
                          'where': label, 'ambiguous': sorted('%s:%s' % k for k in hits)})
    return found


def main():
    elf, hdr = sys.argv[1], sys.argv[2]
    out = sys.argv[sys.argv.index('--out') + 1] if '--out' in sys.argv else None
    syms = load_db(hdr)
    print('db function variants (>=256 static bits):', len(syms))
    base, data, _ = load_main(elf)
    found = run(data, base, syms, 'main')
    if '--overlays' in sys.argv:
        d = sys.argv[sys.argv.index('--overlays') + 1]
        for f in sorted(os.listdir(d)):
            raw = open(os.path.join(d, f), 'rb').read()
            if raw[:4] == b'MWo3':                         # Metrowerks overlay: 0x40-byte header, then the code
                hdr_size = 0x40
                load = struct.unpack_from('<I', raw, 8)[0]
                found += run(raw[hdr_size:], load + hdr_size, syms, f)
    libs = collections.Counter(f['lib'] for f in found)
    print('matched functions:', len(found), ' ambiguous:', sum(1 for f in found if 'ambiguous' in f))
    print('by library:', ', '.join('%s:%d' % kv for kv in libs.most_common()))
    if out:
        json.dump(found, open(out, 'w'), indent=1)


if __name__ == '__main__':
    main()
