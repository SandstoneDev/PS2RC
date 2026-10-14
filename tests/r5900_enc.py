"""Tiny R5900 instruction encoder for synthetic test programs (encodings checked against the decoder in the tests).
Register numbers: zero 0, at 1, v0 2, v1 3, a0-a3 4-7, t0-t3 8-11, s0-s7 16-23, gp 28, sp 29, fp 30, ra 31."""
import struct

from ps2rc.image import Image, Segment

ZERO, AT, V0, V1, A0, A1, A2, A3, T0, T1, T2, T3 = 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11
S0, S1, S2, T9, GP, SP, FP, RA = 16, 17, 18, 25, 28, 29, 30, 31


def r_type(funct, rs=0, rt=0, rd=0, sa=0):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def mmi(funct, sub, rd, rs, rt):
    return (0x1C << 26) | (rs << 21) | (rt << 16) | (rd << 11) | (sub << 6) | funct


def addiu(rt, rs, imm): return i_type(0x09, rs, rt, imm)
def daddiu(rt, rs, imm): return i_type(0x19, rs, rt, imm)
def lui(rt, imm): return i_type(0x0F, 0, rt, imm)
def lw(rt, off, base): return i_type(0x23, base, rt, off)
def sw(rt, off, base): return i_type(0x2B, base, rt, off)
def ld(rt, off, base): return i_type(0x37, base, rt, off)
def sd(rt, off, base): return i_type(0x3F, base, rt, off)
def lq(rt, off, base): return i_type(0x1E, base, rt, off)
def sq(rt, off, base): return i_type(0x1F, base, rt, off)
def addu(rd, rs, rt): return r_type(0x21, rs, rt, rd)
def daddu(rd, rs, rt): return r_type(0x2D, rs, rt, rd)
def slt(rd, rs, rt): return r_type(0x2A, rs, rt, rd)
def dsll32(rd, rt, sa): return r_type(0x3C, rt=rt, rd=rd, sa=sa)
def dsrl32(rd, rt, sa): return r_type(0x3E, rt=rt, rd=rd, sa=sa)
def dsra32(rd, rt, sa): return r_type(0x3F, rt=rt, rd=rd, sa=sa)
def jr(rs): return r_type(0x08, rs=rs)
def mtc1(rt, fs): return (0x11 << 26) | (0x04 << 21) | (rt << 16) | (fs << 11)
def beq(rs, rt, pc, target): return i_type(0x04, rs, rt, (target - pc - 4) >> 2)
def jal(target): return (0x03 << 26) | ((target >> 2) & 0x3FFFFFF)
def paddw(rd, rs, rt): return mmi(0x08, 0x00, rd, rs, rt)
def paddub(rd, rs, rt): return mmi(0x28, 0x18, rd, rs, rt)
def por(rd, rs, rt): return mmi(0x29, 0x12, rd, rs, rt)


NOP = 0


def image(program, base=0x1000, size=0x2000):
    """Image from {address: [words]}."""
    mem = bytearray(size)
    for a, words in program.items():
        for k, w in enumerate(words):
            struct.pack_into("<I", mem, a - base + 4 * k, w)
    seg = Segment(vaddr=base, filesz=size, memsz=size, flags=7, data=bytes(mem))
    return Image(path="<synthetic>", entry=base, gp=0, segments=[seg], overlay_slots=[])
