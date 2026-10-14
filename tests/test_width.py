"""Width analysis (Phase 3) on synthetic functions: the idioms the ROADMAP names (Metrowerks moves, sign-extension
pairs, LQ/SQ save/restore) must come out reducible to 32-bit operations, real 64/128-bit uses must not, and broken
ABI assumptions must be reported."""
import pytest

from ps2rc.analysis.functions import discover_functions
from ps2rc.analysis.width import analyse_function, argument_livein
from ps2rc.r5900 import decode
from r5900_enc import (A0, A1, A2, S0, SP, T0, T1, V0, V1, NOP, beq, RA, ZERO, addiu, addu, daddiu, daddu, dsll32,
                       dsra32, dsrl32, image, jal, jr, ld, lq, lw, mtc1, paddub, paddw, sd, slt, sq, sw)

F = {
    "move": (0x1000, [daddu(V0, A0, ZERO), jr(RA), NOP]),
    "sext": (0x1100, [dsll32(V0, A0, 0), dsra32(V0, V0, 0), jr(RA), NOP]),
    "zext_low": (0x1180, [dsll32(T0, A0, 0), dsrl32(T0, T0, 0), lw(V0, 0, T0), jr(RA), NOP]),
    "dadd_ret": (0x1200, [daddu(V0, A0, A1), jr(RA), NOP]),
    "dadd_addr": (0x1300, [daddu(T0, A0, A1), lw(V0, 0, T0), jr(RA), NOP]),
    "ld_copy": (0x1400, [ld(T0, 0, A0), sd(T0, 0, A1), jr(RA), NOP]),
    "ld_value": (0x1500, [ld(T0, 0, A0), slt(V0, T0, ZERO), jr(RA), NOP]),
    "save_restore": (0x1600, [addiu(SP, SP, -16), sq(S0, 0, SP), addiu(S0, A0, 1), sw(S0, 0, A1), lq(S0, 0, SP),
                              jr(RA), addiu(SP, SP, 16)]),
    "mmi": (0x1700, [paddw(V0, A0, A1), sq(V0, 0, A2), jr(RA), NOP]),
    "move128": (0x1800, [paddub(T0, A0, ZERO), addiu(V0, T0, 1), jr(RA), NOP]),
    "lq_simd": (0x1900, [lq(T0, 0, A0), paddw(T1, T0, T0), sq(T1, 0, A1), jr(RA), NOP]),
    "callee": (0x1A00, [jr(RA), addiu(V0, A0, 1)]),
    "li64": (0x1C00, [daddiu(V0, ZERO, 1), jr(RA), NOP]),
    "field_mask": (0x1C80, [dsll32(V0, A0, 4), addiu(T1, ZERO, 3), dsrl32(V0, V0, 4), jr(RA), NOP]),
    "ld_split": (0x1D00, [ld(T0, 0, A0), dsrl32(T1, T0, 0), mtc1(T0, 0), mtc1(T1, 1), jr(RA), NOP]),
    "dadd_hiword": (0x1D80, [daddu(T0, A0, A1), dsra32(V0, T0, 0), jr(RA), NOP]),
    "field_other_reg": (0x1E00, [dsll32(V0, A0, 24), dsrl32(V1, V0, 27), jr(RA), daddu(V0, V1, ZERO)]),
    "sext_in_slot": (0x1E80, [dsll32(V0, A0, 16), beq(A1, ZERO, 0x1E84, 0x1E90), dsra32(V0, V0, 16), NOP,
                              jr(RA), NOP]),
    "caller_wide_arg": (0x1B00, [addiu(SP, SP, -16), sq(RA, 0, SP), daddu(A0, A0, A1), jal(0x1A00), NOP,
                                 lq(RA, 0, SP), jr(RA), addiu(SP, SP, 16)]),
}


@pytest.fixture(scope="module")
def analysed():
    img = image({a: w for a, w in F.values()})
    fs = discover_functions(img, [(0x1000, 0x2000)], extra_seeds=[a for a, _ in F.values()])
    livein = argument_livein(img, fs.functions)
    starts = set(fs.functions)
    return img, {name: analyse_function(img, fs.functions[a], starts, livein, keep_verdicts=True)
                 for name, (a, _) in F.items()}, livein


def verdicts(res):
    return sorted((cls, ok) for cls, ok, _why in res.verdict.values())


def test_encodings_decode_as_intended():
    assert decode(paddub(8, 4, 0), 0).text.startswith("paddub")
    assert decode(paddw(2, 4, 5), 0).text.startswith("paddw")
    assert decode(dsra32(2, 2, 0), 0).text.startswith("dsra32")
    assert decode(lq(16, 0, 29), 0).text.startswith("lq")


def test_metrowerks_move_of_a_canonical_value_is_reducible(analysed):
    _, r, _ = analysed
    assert verdicts(r["move"]) == [("move64", True)]


def test_sign_extension_pair_is_reducible(analysed):
    _, r, _ = analysed
    assert verdicts(r["sext"]) == [("sext", True), ("sext", True)]


def test_zero_extension_pair_used_as_address_is_reducible(analysed):
    _, r, _ = analysed
    assert verdicts(r["zext_low"]) == [("zext", True), ("zext", True)]


def test_64bit_sum_returned_is_not_reducible_and_breaks_the_abi_hypothesis(analysed):
    _, r, _ = analysed
    assert verdicts(r["dadd_ret"]) == [("dalu", False)]
    assert r["dadd_ret"].violations["return: non-canonical v0"] == 1


def test_64bit_sum_used_only_as_an_address_is_reducible(analysed):
    _, r, _ = analysed
    assert verdicts(r["dadd_addr"]) == [("dalu", True)]


def test_doubleword_copy_is_reducible_but_a_doubleword_compared_as_a_number_is_not(analysed):
    _, r, _ = analysed
    assert verdicts(r["ld_copy"]) == [("ld", True), ("sd", True)]
    assert verdicts(r["ld_value"]) == [("ld", False)]


def test_lq_sq_save_restore_is_reducible_and_restores_a_canonical_register(analysed):
    _, r, _ = analysed
    assert verdicts(r["save_restore"]) == [("lq", True), ("sq", True)]
    assert not r["save_restore"].violations


def test_simd_and_128bit_values_are_not_reducible(analysed):
    _, r, _ = analysed
    assert verdicts(r["mmi"]) == [("mmi", False), ("sq", False)]
    assert verdicts(r["lq_simd"]) == [("lq", False), ("mmi", False), ("sq", False)]


def test_128bit_move_of_a_canonical_value_is_reducible(analysed):
    _, r, _ = analysed
    assert verdicts(r["move128"]) == [("move128", True)]


def test_non_canonical_argument_is_reported_and_its_producer_is_not_reducible(analysed):
    _, r, livein = analysed
    assert livein[0x1A00] == 1 << A0
    res = r["caller_wide_arg"]
    assert res.violations["call: non-canonical argument"] == 1
    assert ("dalu", False) in verdicts(res)


def test_daddiu_from_zero_is_a_constant(analysed):
    _, r, _ = analysed
    assert verdicts(r["li64"]) == [("move64", True)]
    assert not r["li64"].violations


def test_field_mask_pair_with_an_instruction_in_between_is_reducible(analysed):
    _, r, _ = analysed
    assert verdicts(r["field_mask"]) == [("zext", True), ("zext", True)]
    assert not r["field_mask"].violations


def test_doubleword_load_split_into_two_floats_is_reducible(analysed):
    _, r, _ = analysed
    assert verdicts(r["ld_split"]) == [("dshift", True), ("ld", True)]


def test_high_word_of_a_64bit_sum_is_not_reducible(analysed):
    _, r, _ = analysed
    assert verdicts(r["dadd_hiword"]) == [("dalu", False), ("dshift", True)]


def test_field_extracted_into_another_register_is_reducible_when_the_intermediate_dies(analysed):
    _, r, _ = analysed
    assert verdicts(r["field_other_reg"]) == [("move64", True), ("zext", True), ("zext", True)]


def test_sign_extension_completed_in_a_branch_delay_slot_is_reducible(analysed):
    _, r, _ = analysed
    assert verdicts(r["sext_in_slot"]) == [("sext", True), ("sext", True)]
