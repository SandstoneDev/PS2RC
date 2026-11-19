"""IRX bundles: several IOP modules stored back to back in one disc file (each a complete IRX ELF). A converter
generator (kind "python") that translates one member of a bundle at a chosen load address:

    [[profile.<name>.generators]]
    name = "x"; kind = "python"; module = "ps2rc.iop.bundle:generate"; input = "SYSTEM/MODULES.IRX"
    out = "x_gen.c"; include_only = true
    args = { member = 3, base = 0x3000, sym = "x_irx", md5 = "<md5 of the member>" }

`member` counts the ELF headers in the file from 0. `md5` (optional) is checked against the member's bytes, so a
different disc version is refused instead of translated. Several members of one bundle need distinct `base`s.
"""
import hashlib
import os

from .gen_c import generate as _generate
from .irx import load


def members(data):
    """[(offset, bytes)] of the IRX ELF images in a bundle (split at each ELF header)."""
    offs, k = [], data.find(b"\x7fELF")
    while k >= 0:
        offs.append(k)
        k = data.find(b"\x7fELF", k + 4)
    return [(o, data[o:offs[i + 1] if i + 1 < len(offs) else len(data)]) for i, o in enumerate(offs)]


def generate(input_path, out_path, log=print, member=0, base=0x1000, sym="irx", md5=None, abi=True):
    data = open(input_path, "rb").read()
    ms = members(data)
    if not 0 <= member < len(ms):
        raise ValueError("%s has %d modules, no member %d" % (os.path.basename(input_path), len(ms), member))
    img = ms[member][1]
    if md5 and hashlib.md5(img).hexdigest() != md5.lower():
        raise ValueError("member %d of %s is not the expected module (md5 %s)" % (member, input_path,
                                                                                hashlib.md5(img).hexdigest()))
    x = load(img, int(base))
    src, p = _generate(x, sym, abi=abi)
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", newline="\n") as f:
        f.write(src)
    log("%s (member %d at 0x%x): %d functions -> %s" % (x.name or "?", member, int(base), len(p.entries), out_path))
