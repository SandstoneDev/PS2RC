"""Instruction census of a PS2 EE ELF (+ optional Metrowerks overlay files, 'MWo3').

usage: python census.py <boot ELF> [overlay file ...] [--out DIR]

Finds the code region heuristically (valid-decode density + JAL target set),
then counts every R5900 mnemonic, records syscall numbers, COP0 sites, etc.
"""
import sys, os, struct, json, collections
from elftools.elf.elffile import ELFFile
sys.path.insert(0, os.path.dirname(__file__))
from r5900 import decode, jump_target, branch_target

JR_RA = 31 << 21 | 8                                   # jr ra


def load_main(path):
    e = ELFFile(open(path, 'rb'))
    seg = next(s for s in e.iter_segments() if s['p_type'] == 'PT_LOAD' and s['p_filesz'])
    return seg['p_vaddr'], seg.data(), e.header['e_entry']


def words(data):
    n = len(data) // 4
    return struct.unpack('<%dI' % n, data[:n * 4])


def code_extent(base, ws, entry):
    """Code region = window-run with high valid-decode ratio, anchored at entry."""
    W = 256  # words per window (1KB)
    good = []
    for i in range(0, len(ws), W):
        chunk = ws[i:i + W]
        ok = sum(1 for w in chunk if decode(w)[1] != 'INVALID')
        jr = sum(1 for w in chunk if w == JR_RA)
        good.append((ok / max(1, len(chunk)), jr))
    # walk from start until we see 8 consecutive windows with ratio < 0.85 and no jr ra
    last_code = 0
    bad_run = 0
    for i, (r, jr) in enumerate(good):
        if r >= 0.92 or jr:
            last_code = i
            bad_run = 0
        else:
            bad_run += 1
            if bad_run >= 16:
                break
    end = base + (last_code + 1) * W * 4
    return base, end, good


def census(base, ws, start, end):
    cnt = collections.Counter()
    grp = collections.Counter()
    sites = collections.defaultdict(list)
    jal_targets = collections.Counter()
    syscalls = collections.Counter()
    i0 = (start - base) // 4
    i1 = (end - base) // 4
    for i in range(i0, min(i1, len(ws))):
        w = ws[i]
        pc = base + i * 4
        m, g = decode(w)
        cnt[m] += 1
        grp[g] += 1
        if g in ('COP0', 'SYS', 'LS128', 'VU0M') or m in ('LWL', 'LWR', 'SWL', 'SWR', 'LDL', 'LDR', 'SDL', 'SDR'):
            if len(sites[m]) < 4000:
                sites[m].append(pc)
        if m == 'SYSCALL':
            # look back up to 4 instr for addiu v1,zero,imm (v1 = $3)
            num = None
            for k in range(1, 5):
                pw = ws[i - k]
                if (pw >> 26) == 9 and ((pw >> 21) & 31) == 0 and ((pw >> 16) & 31) == 3:
                    imm = pw & 0xFFFF
                    num = imm - 0x10000 if imm & 0x8000 else imm
                    break
            syscalls[num] += 1
            sites['SYSCALL#%s' % num].append(pc)
        t = jump_target(w, pc)
        if m == 'JAL' and t is not None:
            jal_targets[t] += 1
    return cnt, grp, sites, jal_targets, syscalls


def main():
    args = sys.argv[1:]
    out = None
    if '--out' in args:
        k = args.index('--out'); out = args[k + 1]; del args[k:k + 2]
    elf = args[0]
    base, data, entry = load_main(elf)
    ws = words(data)
    start, end, dens = code_extent(base, ws, entry)
    print('segment %08x-%08x entry %08x  code region guess %08x-%08x (%d KB)' %
          (base, base + len(data), entry, start, end, (end - start) // 1024))
    res = {'main': census(base, ws, start, end)}
    for pm in args[1:]:
        d = open(pm, 'rb').read()
        if d[:4] != b'MWo3':
            print('%s: not an MWo3 overlay, skipped' % pm)
            continue
        ob = struct.unpack_from('<I', d, 8)[0]             # the header's load address (the header is loaded too)
        res[os.path.basename(pm)] = census(ob, words(d), ob, ob + len(d))
    report = {}
    for name, (cnt, grp, sites, jt, sc) in res.items():
        tot = sum(cnt.values())
        print('\n=== %s: %d words' % (name, tot))
        print('groups:', dict(grp.most_common()))
        print('syscalls:', dict(sc.most_common()))
        print('functions (distinct JAL targets):', len(jt))
        report[name] = {'total': tot, 'groups': dict(grp), 'mnemonics': dict(cnt.most_common()),
                        'syscalls': {str(k): v for k, v in sc.items()},
                        'sites': {k: ['%08x' % a for a in v[:200]] for k, v in sites.items()},
                        'jal_targets': len(jt)}
    if out:
        os.makedirs(out, exist_ok=True)
        json.dump(report, open(os.path.join(out, 'census.json'), 'w'), indent=1)
        # density map for code extent review
        with open(os.path.join(out, 'density.txt'), 'w') as f:
            for i, (r, jr) in enumerate(dens):
                f.write('%08x %.3f %d\n' % (base + i * 1024, r, jr))
        jt = res['main'][3]
        with open(os.path.join(out, 'jal_targets.txt'), 'w') as f:
            for a, c in sorted(jt.items()):
                f.write('%08x %d\n' % (a, c))


if __name__ == '__main__':
    main()
