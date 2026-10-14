"""Group VIF MPG packets into VU microprograms and summarize them.
usage: python vu_programs.py <elf> [--json out.json]"""
import sys, os, json, hashlib, collections
sys.path.insert(0, os.path.dirname(__file__))
from census import load_main, words
from find_vu import scan_mpg
from vumicro import decode_pair

def main():
    base, data, entry = load_main(sys.argv[1]); ws = words(data)
    hits = scan_mpg(base, ws)
    progs = []
    cur = None
    for a, num, imm, eb, st in hits:
        contiguous = cur and imm == cur['next_load'] and a == cur['next_addr']
        if not contiguous:
            cur = {'addr': a, 'packets': [], 'code': {}, 'next_load': None, 'next_addr': None}
            progs.append(cur)
        off = (a - base) + 4
        for k in range(num):
            lo, up = ws[off // 4 + 2 * k], ws[off // 4 + 2 * k + 1]
            cur['code'][imm + k] = (lo, up)
        cur['packets'].append((a, num, imm))
        cur['next_load'] = imm + num
        # next MPG VIFcode expected right after payload (maybe after a DMA tag+NOP: allow small gap)
        cur['next_addr'] = a + 4 + num * 8
    # merge programs whose packets are separated only by a DMA tag (16 bytes) + vif nop
    merged = []
    for p in progs:
        if merged and p['packets'][0][2] != 0 and p['packets'][0][2] == merged[-1]['next_load'] \
                and 0 <= p['addr'] - merged[-1]['next_addr'] <= 32:
            q = merged[-1]; q['code'].update(p['code']); q['packets'] += p['packets']; q['next_load'] = p['next_load']; q['next_addr'] = p['next_addr']
        else:
            merged.append(p)
    out = []
    seen = {}
    for i, p in enumerate(merged):
        st = collections.Counter(); fl = collections.Counter()
        blob = b''.join(lo.to_bytes(4, 'little') + up.to_bytes(4, 'little') for _, (lo, up) in sorted(p['code'].items()))
        h = hashlib.sha1(blob).hexdigest()[:10]
        for k, (lo, up) in p['code'].items():
            d = decode_pair(lo, up)
            st[d[0]] += 1; st[d[1]] += 1
            for f, v in d[2].items():
                if v: fl[f] += 1
        size = max(p['code']) + 1
        vu1_only = sum(st[m] for m in st if m in ('XGKICK','XTOP','XITOP','MFP','WAITP') or m.startswith('E') and m not in ('',))
        unit = 'VU1' if (st['XGKICK'] or vu1_only or size > 512) else 'VU0?'
        dup = seen.get(h); seen.setdefault(h, i)
        rec = {'idx': i, 'addr': '%08x' % p['addr'], 'packets': len(p['packets']), 'instrs': len(p['code']),
               'span_instr': size, 'bytes': size * 8, 'sha1': h, 'dup_of': dup, 'unit': unit,
               'XGKICK': st['XGKICK'], 'XTOP': st['XTOP'], 'XITOP': st['XITOP'], 'EFU': sum(st[m] for m in st if m.startswith('E') and m in ('ESADD','ERSADD','ELENG','ERLENG','EATANxy','EATANxz','ESUM','ESQRT','ERSQRT','ERCPR','ESIN','EATAN','EEXP')),
               'MFP_WAITP': st['MFP'] + st['WAITP'], 'DIV/SQRT/RSQRT': st['DIV'] + st['SQRT'] + st['RSQRT'],
               'CLIP': st['CLIP'], 'FCAND/FCGET..': st['FCAND'] + st['FCGET'] + st['FCOR'] + st['FCEQ'] + st['FCSET'],
               'FMAND..': st['FMAND'] + st['FMEQ'] + st['FMOR'], 'FSAND..': st['FSAND'] + st['FSEQ'] + st['FSOR'] + st['FSSET'],
               'RNG': st['RNEXT'] + st['RGET'] + st['RINIT'] + st['RXOR'], 'branches': sum(st[m] for m in ('B','BAL','JR','JALR','IBEQ','IBNE','IBLTZ','IBGTZ','IBLEZ','IBGEZ')),
               'Ebits': fl['E'], 'Mbits': fl['M'], 'Dbits': fl['D'], 'Tbits': fl['T'], 'Ibits(LOI)': fl['I'],
               'ITOF/FTOI': sum(st[m] for m in st if m.startswith('ITOF') or m.startswith('FTOI')),
               'top': st.most_common(12)}
        out.append(rec)
    uniq = [r for r in out if r['dup_of'] is None]
    print('programs: %d  unique: %d' % (len(out), len(uniq)))
    for r in out:
        print('#%-3d @%s pk=%-2d span=%4d %-4s dup=%-4s XGK=%-2d XTOP=%d XITOP=%d EFU=%-2d DIVs=%-2d CLIP=%-2d FC=%-2d FM=%-2d RNG=%-3d br=%-3d E=%d M=%d LOI=%-2d cvt=%-3d' % (
            r['idx'], r['addr'], r['packets'], r['span_instr'], r['unit'], r['dup_of'] if r['dup_of'] is not None else '-',
            r['XGKICK'], r['XTOP'], r['XITOP'], r['EFU'], r['DIV/SQRT/RSQRT'], r['CLIP'], r['FCAND/FCGET..'], r['FMAND..'], r['RNG'], r['branches'], r['Ebits'], r['Mbits'], r['Ibits(LOI)'], r['ITOF/FTOI']))
    tot_all = collections.Counter()
    if '--json' in sys.argv:
        json.dump(out, open(sys.argv[sys.argv.index('--json') + 1], 'w'), indent=1)

main()
