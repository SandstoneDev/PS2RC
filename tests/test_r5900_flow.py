"""Control-flow classification of single R5900 instructions (needed for CFG recovery and code generation)."""
import pytest

from ps2rc.r5900 import decode
from ps2rc.r5900.flow import flow

PC = 0x00100000


def f(word, pc=PC):
    return flow(decode(word, pc))


def test_plain_alu_has_no_flow():
    fl = f(0x24420001)                      # addiu v0,v0,1
    assert fl.kind == "none" and not fl.has_delay_slot


def test_jal_is_direct_call_with_delay_slot():
    fl = f(0x0C040010)                      # jal 0x100040
    assert (fl.kind, fl.target, fl.has_delay_slot, fl.likely) == ("call", 0x00100040, True, False)


def test_j_is_direct_jump():
    fl = f(0x08040010)                      # j 0x100040
    assert (fl.kind, fl.target) == ("jump", 0x00100040)


def test_jr_ra_is_return():
    fl = f(0x03E00008)
    assert fl.kind == "return" and fl.has_delay_slot


def test_jr_other_register_is_indirect_jump():
    fl = f(0x01C00008)                      # jr t6
    assert (fl.kind, fl.reg) == ("jump_indirect", 14)


def test_jalr_is_indirect_call():
    fl = f(0x0140F809)                      # jalr t2 (rd = ra)
    assert (fl.kind, fl.reg, fl.link) == ("call_indirect", 10, 31)


def test_beq_is_conditional_branch():
    fl = f(0x10850004)                      # beq a0,a1,pc+4+16
    assert (fl.kind, fl.target, fl.likely) == ("branch", PC + 4 + 16, False)


def test_beq_same_register_is_unconditional():
    fl = f(0x10000004)                      # beq zero,zero
    assert (fl.kind, fl.target) == ("jump", PC + 4 + 16)


def test_branch_likely_is_flagged():
    fl = f(0x5085FFFF)                      # beql a0,a1,pc
    assert (fl.kind, fl.likely, fl.target) == ("branch", True, PC)


def test_bc1t_and_bc0f_are_conditional_branches():
    assert f(0x45010003).kind == "branch"   # bc1t
    assert f(0x41000003).kind == "branch"   # bc0f
    assert f(0x45030003).likely             # bc1tl


def test_bgezal_is_conditional_call_and_bgezal_zero_is_call():
    assert f(0x04910004).kind == "call_conditional"     # bgezal a0
    fl = f(0x04110004)                                  # bgezal zero == bal
    assert (fl.kind, fl.target, fl.link) == ("call", PC + 4 + 16, 31)


@pytest.mark.parametrize("word,kind", [(0x0000000C, "syscall"), (0x0000000D, "break"), (0x42000018, "eret")])
def test_terminators_without_delay_slot(word, kind):
    fl = f(word)
    assert fl.kind == kind and not fl.has_delay_slot


def test_invalid_word_is_flagged():
    assert f(0x4C000000).kind == "invalid"   # primary opcode 19 is unused on the R5900
