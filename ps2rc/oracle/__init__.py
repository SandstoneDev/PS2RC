"""PCSX2 execution oracle: run small PS2 EE test ELFs in PCSX2 and read results via PINE.

    from ps2rc.oracle import Pcsx2Runner
    res = Pcsx2Runner().run_elf(elf, done_addr, 0x600DF00D, timeout_s=60,
                                ee_interpreter=False, reads={"results": (addr, size)})
    res["blocks"]["results"]  # bytes read from EE memory after the done flag appeared

The PCSX2 side: ps2rc/oracle/pcsx2.py (a private, portable PCSX2 copy; its folders come from local.toml [tools]
pcsx2_oracle / pcsx2_install / ps2_bios_dir / ps2_bios_name or the PS2RC_* environment variables there).
"""
from .pine import EmuStatus, PineClient, PineCommandError, PineError
from .pcsx2 import (Pcsx2Error, Pcsx2Runner, Pcsx2Session, Pcsx2StartupError,
                    Pcsx2TimeoutError)

__all__ = [
    "EmuStatus", "PineClient", "PineCommandError", "PineError",
    "Pcsx2Error", "Pcsx2Runner", "Pcsx2Session", "Pcsx2StartupError", "Pcsx2TimeoutError",
]
