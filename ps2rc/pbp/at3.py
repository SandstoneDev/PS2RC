"""SND0.AT3: menu music. This package checks a ready file; it does not encode one.

An .AT3 file is a RIFF/WAVE container (little-endian chunks) holding ATRAC3plus or ATRAC3 frames:

    'fmt '  format tag 0xFFFE (WAVE_FORMAT_EXTENSIBLE) with the ATRAC3plus sub-format GUID (stored as the
            bytes bfaa23e958cb7144a119fffa01e4ce62), or tag 0x0270 = ATRAC3; channels, sample rate,
            bytes per second, block align (= bytes per coded frame)
    'fact'  u32 number of samples (the length), then encoder-specific values
    'smpl'  optional loop: u32 loop count at +28, first loop at +36: cue id, type, start, end (in samples)
    'data'  the coded frames

These are the tags ffmpeg's RIFF demuxer maps to its atrac3plus / atrac3 decoders. The menu music is expected to be
ATRAC3plus, 44.1 kHz, stereo. No open-source ATRAC3plus encoder is known to this project, so WAV/MP3 input is not
converted here: the user brings a ready .AT3 file.
"""
import struct
import uuid

from .spec import PbpError, Issue, SND0_RATE, SND0_CHANNELS

TAG_EXTENSIBLE = 0xFFFE
TAG_ATRAC3 = 0x0270
GUID_ATRAC3PLUS = uuid.UUID(bytes_le=bytes.fromhex('bfaa23e958cb7144a119fffa01e4ce62'))


def parse(data, what='SND0.AT3'):
    """-> dict: codec, channels, rate, byte_rate, block_align, kbps, samples, seconds, loop, data_bytes, chunks"""
    data = bytes(data)
    if len(data) < 12 or data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        raise PbpError('%s: not an AT3 file (no RIFF/WAVE header)' % what)
    riff_len = struct.unpack('<I', data[4:8])[0]
    pos, chunks, fmt, fact, smpl, body = 12, [], None, None, None, None
    while pos + 8 <= len(data):
        cid, size = struct.unpack('<4sI', data[pos:pos + 8])
        if pos + 8 + size > len(data):
            raise PbpError('%s: damaged file (chunk %r runs past the end)' % (what, cid))
        chunk = data[pos + 8:pos + 8 + size]
        chunks.append(cid.decode('ascii', 'replace'))
        if cid == b'fmt ':
            fmt = chunk
        elif cid == b'fact':
            fact = chunk
        elif cid == b'smpl':
            smpl = chunk
        elif cid == b'data':
            body = chunk
        pos += 8 + size + (size & 1)
    if fmt is None or len(fmt) < 16:
        raise PbpError('%s: damaged file (no format chunk)' % what)
    if body is None:
        raise PbpError('%s: damaged file (no data chunk)' % what)
    tag, ch, rate, byte_rate, align = struct.unpack('<HHIIH', fmt[:14])
    codec = 'tag 0x%04X' % tag
    if tag == TAG_ATRAC3:
        codec = 'ATRAC3'
    elif tag == TAG_EXTENSIBLE and len(fmt) >= 40:
        sub = uuid.UUID(bytes_le=fmt[24:40])
        codec = 'ATRAC3plus' if sub == GUID_ATRAC3PLUS else 'extensible %s' % sub
    elif tag == 1:
        codec = 'PCM'
    elif tag == 0x55:
        codec = 'MP3'
    samples = struct.unpack('<I', fact[:4])[0] if fact and len(fact) >= 4 else None
    loop = None
    if smpl and len(smpl) >= 36 + 24 and struct.unpack('<I', smpl[28:32])[0] >= 1:
        _cue, _type, start, end = struct.unpack('<4I', smpl[36:52])
        loop = (start, end)
    return {'codec': codec, 'channels': ch, 'rate': rate, 'byte_rate': byte_rate, 'block_align': align,
            'kbps': round(byte_rate * 8 / 1000.0, 1), 'samples': samples,
            'seconds': (samples / float(rate)) if samples and rate else None, 'loop': loop,
            'data_bytes': len(body), 'chunks': chunks, 'riff_size_ok': riff_len + 8 == len(data)}


def check(data, what='SND0.AT3'):
    try:
        a = parse(data, what)
    except PbpError as e:
        return [Issue('error', what, str(e).split(': ', 1)[-1])]
    out = []
    if a['codec'] == 'ATRAC3':
        out.append(Issue('warn', what, 'ATRAC3 (not ATRAC3plus): outside the documented SND0 format; played by '
                                       'the menu of a PSP-3000 with custom firmware in a test, other models '
                                       'untested'))
    elif a['codec'] != 'ATRAC3plus':
        out.append(Issue('error', what, '%s audio; the menu needs ATRAC3plus (convert it with an ATRAC3plus '
                                        'encoder first, this tool has none)' % a['codec']))
    if a['rate'] != SND0_RATE:
        out.append(Issue('spec', what, '%d Hz, must be %d Hz' % (a['rate'], SND0_RATE)))
    if a['channels'] != SND0_CHANNELS:
        out.append(Issue('spec', what, '%d channel(s), must be %d (stereo)' % (a['channels'], SND0_CHANNELS)))
    if a['block_align'] and a['data_bytes'] % a['block_align']:
        out.append(Issue('warn', what, 'data is %d bytes, not a whole number of %d-byte frames' % (
            a['data_bytes'], a['block_align'])))
    if not a['riff_size_ok']:
        out.append(Issue('warn', what, 'the RIFF size field does not match the file size'))
    if a['seconds'] is None:
        out.append(Issue('warn', what, 'no sample count (fact chunk): length unknown'))
    else:
        out.append(Issue('info', what, '%s %d Hz %d ch %.1f kbit/s, %.2f s' % (
            a['codec'], a['rate'], a['channels'], a['kbps'], a['seconds'])))
    if a['loop'] is None:
        out.append(Issue('info', what, 'no loop points: whether the menu repeats it is Unknown'))
    else:
        out.append(Issue('info', what, 'loops samples %d..%d' % a['loop']))
    return out
