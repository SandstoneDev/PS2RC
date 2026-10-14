"""IOP static recompiler: PS2 IOP modules (IRX, MIPS R3000) -> C for the IOP runtime in ps2rc/iop/rt.

    python -m ps2rc.iop info  MODULE.IRX                 module info, imports, functions, issues
    python -m ps2rc.iop list  MODULE.IRX [-o FILE]       disassembly (functions, import names)
    python -m ps2rc.iop gen   MODULE.IRX -o OUT.c --sym NAME [--base 0x1000]

`gen` writes C translated from the module: run it on the user's machine, on the user's own copy, into a build
directory that is never distributed. The runtime (ps2rc/iop/rt) and this package contain no module-specific code.
"""
from .irx import load, IrxError
from .analysis import analyse
from .gen_c import generate

__all__ = ["load", "IrxError", "analyse", "generate"]
