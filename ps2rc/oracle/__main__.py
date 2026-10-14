"""`python -m ps2rc.oracle setup` / `python -m ps2rc.oracle run <elf> --done-addr 0x... [--read ADDR N] [--interp]`."""
from .pcsx2 import _main

raise SystemExit(_main())
