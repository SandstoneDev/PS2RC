"""R5900 (PS2 Emotion Engine core) instruction decoding."""
from .decode import decode, Insn, GPR_NAMES

__all__ = ["decode", "Insn", "GPR_NAMES"]
