"""Helper: reconstruct a VU program (instr index -> (lo, up)) by following consecutive MPG packets from a start address."""
import os, sys
sys.path.insert(0, os.path.dirname(__file__))
from find_vu import scan_mpg

_cache = {}


def program_code(base, ws, start):
    key = id(ws)
    if key not in _cache:
        _cache[key] = scan_mpg(base, ws)
    hits = _cache[key]
    code = {}
    nxt_load = nxt_addr = None
    started = False
    for a, num, imm, eb, st in hits:
        if not started:
            if a != start:
                continue
            started = True
        elif not (imm == nxt_load and 0 <= a - nxt_addr <= 32):
            break
        off = (a - base) // 4 + 1
        for k in range(num):
            code[imm + k] = (ws[off + 2 * k], ws[off + 2 * k + 1])
        nxt_load = imm + num
        nxt_addr = a + 4 + num * 8
    return code
