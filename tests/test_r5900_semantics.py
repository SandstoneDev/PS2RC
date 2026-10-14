"""Register write semantics: which GPR (if any) an instruction writes."""
import pytest

from ps2rc.r5900 import decode
from ps2rc.r5900.semantics import dest_gpr


@pytest.mark.parametrize("word,dest", [
    (0x24420001, 2),      # addiu v0,v0,1
    (0x3C020041, 2),      # lui v0,0x41
    (0x8C820000, 2),      # lw v0,0(a0)
    (0x0085102D, 2),      # daddu v0,a0,a1
    (0x00851018, 2),      # mult v0,a0,a1  (R5900 3-operand form writes rd)
    (0x0000F809 | 2 << 21, 31),   # jalr v0 (links ra)
    (0x0C040010, 31),     # jal (links ra)
    (0x44021000, 2),      # mfc1 v0,$f2
    (0x40026000, 2),      # mfc0 v0,c0_sr
    (0x70e00031 & ~(7 << 21) | 0, None),   # pmthl.lw zero -> writes no GPR (LO/HI only)
])
def test_instructions_that_write_a_gpr(word, dest):
    assert dest_gpr(decode(word, 0x100000)) == dest


@pytest.mark.parametrize("word", [
    0xAC820000,           # sw v0,0(a0)
    0xFC820000,           # sd v0,0(a0)
    0x7C820000,           # sq v0,0(a0)
    0x10850004,           # beq a0,a1,...
    0x03E00008,           # jr ra
    0x00850018,           # mult a0,a1 (2-operand form: HI/LO only)
    0x0085001A,           # div zero,a0,a1
    0x44821000,           # mtc1 v0,$f2
    0x40826000,           # mtc0 v0,c0_sr
    0x00400011,           # mthi v0
    0xBC800000,           # cache
    0x0000000C,           # syscall
    0x00850034,           # teq a0,a1
])
def test_instructions_that_write_no_gpr(word):
    assert dest_gpr(decode(word, 0x100000)) is None


def test_writes_to_zero_register_are_reported_as_none():
    assert dest_gpr(decode(0x24000001, 0)) is None      # addiu zero,zero,1
