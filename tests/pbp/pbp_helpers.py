"""Synthetic files for the PBP packer tests: everything is generated, no game data and no platform files."""
import os
import shutil
import struct


def find_tool(name, env=None):
    for cand in ((os.environ.get(env) if env else None), shutil.which(name)):
        if cand and os.path.isfile(cand):
            return cand
    return None


# ------------------------------------------------------------------------------------------ synthetic files
GUID_ATRAC3PLUS_LE = bytes.fromhex('bfaa23e958cb7144a119fffa01e4ce62')


def make_at3(samples=44100 * 3, rate=44100, channels=2, tag=0xFFFE, guid=GUID_ATRAC3PLUS_LE, align=744,
             loop=(0, None), data_bytes=None, fact=True):
    """a RIFF/WAVE AT3 header with zero-filled frames (enough for the container checks)"""
    byte_rate = align * rate // 2048
    if tag == 0xFFFE:
        fmt = struct.pack('<HHIIHHHHI', tag, channels, rate, byte_rate, align, 0, 34, 2048, 3) + guid + \
            bytes.fromhex('0100285c0000000000000000')
    else:
        fmt = struct.pack('<HHIIHHH', tag, channels, rate, byte_rate, align, 0, 14) + b'\0' * 14
    chunks = [(b'fmt ', fmt)]
    if fact:
        chunks.append((b'fact', struct.pack('<II', samples, 0)))
    if loop is not None and loop[1] is not None:
        smpl = struct.pack('<9I', 0, 0, 22676, 60, 0, 0, 0, 1, 0) + struct.pack('<6I', 0, 0, loop[0], loop[1], 0, 0)
        chunks.append((b'smpl', smpl))
    if data_bytes is None:
        data_bytes = align * max(1, samples // 2048)
    chunks.append((b'data', b'\0' * data_bytes))
    body = b'WAVE' + b''.join(cid + struct.pack('<I', len(c)) + c + (b'\0' if len(c) & 1 else b'')
                              for cid, c in chunks)
    return b'RIFF' + struct.pack('<I', len(body)) + body


def make_pmf(width=144, height=80, packs=4, seconds=2.0, audio=False, version=b'0015'):
    """a PSMF header with the given picture size followed by empty 2048-byte packs (container checks only)"""
    h = bytearray(0x800)
    h[0:8] = b'PSMF' + version
    struct.pack_into('>II', h, 8, 0x800, packs * 2048)
    n = 2 if audio else 1
    first, last = 90000, 90000 + int(seconds * 90000)
    h[0x54:0x5A] = first.to_bytes(6, 'big')
    h[0x5A:0x60] = last.to_bytes(6, 'big')
    struct.pack_into('>H', h, 0x80, n)
    h[0x82] = 0xE0
    h[0x82 + 12], h[0x82 + 13] = width // 16, height // 16
    if audio:
        h[0x92] = 0xBD
    return bytes(h) + b'\0' * (packs * 2048)


def gradient(w, h, ch):
    px = bytearray()
    for y in range(h):
        for x in range(w):
            px += bytes((x * 255 // max(1, w - 1), y * 255 // max(1, h - 1), 128, 200)[:ch])
    return bytes(px)


def make_png(w, h, mode='RGB'):
    from ps2rc.pbp import png
    return png.encode(w, h, mode, gradient(w, h, 3 if mode == 'RGB' else 4))


def fake_elf(n=4096):
    return b'\x7fELF' + bytes(range(256)) * (n // 256)


def _ts(prefix, t):
    return bytes((prefix << 4 | ((t >> 29) & 0x0E) | 1, (t >> 22) & 0xFF, ((t >> 14) & 0xFE) | 1, (t >> 7) & 0xFF,
                  ((t << 1) & 0xFE) | 1))


def make_pmf_raps(raps, packs=6, version=b'0015'):
    """a PSMF whose packs carry the structure an entry-point map is built from: raps = {pack index: PTS}; a
    random-access pack is pack header + system header + a video PES with PTS/DTS, other packs a PES without them"""
    head = bytearray(make_pmf(packs=0, version=version)[:0x800])
    struct.pack_into('>I', head, 0x0C, packs * 2048)
    body = bytearray()
    for k in range(packs):
        pk = bytearray(b'\x00\x00\x01\xba' + bytes(10))
        if k in raps:
            pk += b'\x00\x00\x01\xbb' + struct.pack('>H', 6) + bytes(6)
            pk += b'\x00\x00\x01\xe0' + struct.pack('>H', 100) + b'\x81\xc0\x0a' + _ts(3, raps[k]) + \
                _ts(1, raps[k] - 3003)
        else:
            pk += b'\x00\x00\x01\xe0' + struct.pack('>H', 100) + b'\x81\x00\x00'
        pk += bytes((k * 7 + i) & 0xFF for i in range(2048 - len(pk)))
        body += pk
    return bytes(head) + bytes(body)
