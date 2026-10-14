"""Synthetic sound data for the tests of tests/gui (imported by name; no pytest fixtures here)."""
import struct


def tone_pcm(seconds=2.0, freq=440.0, rate=44100, amp=12000):
    import math
    n = int(seconds * rate)
    out = bytearray()
    for i in range(n):
        v = int(amp * math.sin(2 * math.pi * freq * i / rate))
        out += struct.pack("<hh", v, v)
    return bytes(out)


def atracdenc_like(samples, frame=2048, channels=2):
    """the RIFF layout atracdenc writes for ATRAC3plus (40-byte fmt, 4-byte fact, data), zero frames"""
    frames = -(-samples // 2048) + 1
    guid = bytes.fromhex("bfaa23e958cb7144a119fffa01e4ce62")
    fmt = struct.pack("<HHIIHHH", 0xFFFE, channels, 44100, frame * 44100 // 2048, frame, 16, 22) + \
        struct.pack("<HI", 16, 3) + guid
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"fact" + struct.pack("<II", 4, frames * 2048) + \
        b"data" + struct.pack("<I", frames * frame) + b"\x5a" * (frames * frame)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def oma_like(samples, frame=2048, channels=2, tag=True):
    """the OMA layout atracdenc 0.2.3 writes for ATRAC3plus (optionally behind an 'ea3' tag), frames of 0x5A"""
    frames = samples // 2048
    word = (1 << 24) | (1 << 13) | ({1: 1, 2: 2}[channels] << 10) | ((frame - 8) // 8)
    head = bytearray(96)
    head[0:3] = b"EA3"
    head[3] = 1
    head[5] = 96
    head[6:8] = b"\xff\xff"
    head[32:36] = struct.pack(">I", word)
    pre = b"ea3\x03\x00\x00\x00\x00\x01\x76" + b"\0" * 0xF6 if tag else b""
    return pre + bytes(head) + b"\x5a" * (frames * frame)


def atrac3_like(samples, align=384, js=0):
    """atracdenc 0.2.3's ATRAC3 RIFF: 32-byte fmt (tag 0x270, 14 extra bytes), no fact, data"""
    frames = samples // 1024
    fmt = struct.pack("<HHIIHHH", 0x270, 2, 44100, align * 44100 // 1024, align, 0, 14) + \
        struct.pack("<HIHHHH", 1, 0x1000, js, js, 1, 0)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", frames * align) + \
        b"\x11" * (frames * align)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def onset_pcm(seconds=3.0, rate=44100):
    """0.5 s of silence, then a chord under a slow 3 Hz envelope: one clear, non-repeating start to align on"""
    import math
    out = bytearray()
    for i in range(int(seconds * rate)):
        t = i / rate
        v = 0 if t < 0.5 else int(9000 * (math.sin(2 * math.pi * 440 * t) + 0.6 * math.sin(2 * math.pi * 1000 * t)) *
                                  (0.5 + 0.5 * math.sin(2 * math.pi * 3 * t)))
        out += struct.pack("<hh", v, v)
    return bytes(out)
