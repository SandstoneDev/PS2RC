#!/usr/bin/env python3
"""pss.py - the sound of a PS2 movie file (*.PSS) as a WAV file.

    python pss.py MOVIE.PSS OUT.wav

A PS2 PSS file is an MPEG-2 program stream: video stream 0xE0 (MPEG-2, read by ffmpeg directly) and the sound in
private stream 1 (0xBD). Every sound packet's payload starts with a 4-byte sub-header (sub-stream id + 3 bytes); the
first payload holds an "SShd" header (u32 size, then u32 codec, rate, channels, interleave) followed by "SSbd" (u32
body size) and the body: `interleave` bytes of channel 0, `interleave` bytes of channel 1, ... in turn. ffmpeg does not
read this sound (it skips the sub-stream), so it is demuxed here. Codec 1 = 16-bit PCM little endian is handled; other
codecs are reported (Unknown layout) and the caller goes on without sound.
"""
import struct
import sys
import wave


class PssError(Exception):
    pass


def pes_ts(b):
    return (((b[0] >> 1) & 7) << 30) | ((((b[1] << 8) | b[2]) >> 1) << 15) | (((b[3] << 8) | b[4]) >> 1)


def sound(data):
    """-> (header dict, body bytes, statistics dict); PssError if the file has no SShd sound"""
    i, n = 0, len(data)
    payload = bytearray()
    st = {'packs': 0, 'video_packets': 0, 'audio_packets': 0, 'resyncs': 0, 'substreams': set(),
          'first_audio_pts': None, 'first_video_pts': None}
    while i + 6 <= n:
        if data[i:i + 3] != b'\0\0\1':
            j = data.find(b'\0\0\1', i + 1)
            if j < 0:
                break
            st['resyncs'] += 1
            i = j
            continue
        sid = data[i + 3]
        if sid == 0xB9:                                      # program end
            break
        if sid == 0xBA:                                      # pack header
            st['packs'] += 1
            i += 14 + (data[i + 13] & 7) if (data[i + 4] >> 6) == 1 else 12
            continue
        length = (data[i + 4] << 8) | data[i + 5]
        if sid in (0xE0, 0xBD) and i + 9 <= n:
            flags, hl = data[i + 7], data[i + 8]
            pts = pes_ts(data[i + 9:i + 14]) if flags & 0x80 else None
            if sid == 0xE0:
                st['video_packets'] += 1
                if st['first_video_pts'] is None and pts is not None:
                    st['first_video_pts'] = pts
            else:
                if st['first_audio_pts'] is None and pts is not None:
                    st['first_audio_pts'] = pts
                body = data[i + 9 + hl:i + 6 + length]
                if body:
                    st['substreams'].add(body[0])
                payload += body[4:]
                st['audio_packets'] += 1
        i += 6 + length
    st['substreams'] = sorted(st['substreams'])
    if not payload:
        raise PssError('no private-stream-1 sound')
    if payload[:4] != b'SShd':
        raise PssError('no SShd sound header (%s)' % payload[:16].hex())
    hsize = struct.unpack('<I', payload[4:8])[0]
    codec, rate, channels, interleave = struct.unpack('<4I', payload[8:24])
    at = 8 + hsize
    if payload[at:at + 4] != b'SSbd':
        raise PssError('no SSbd after the sound header')
    size = struct.unpack('<I', payload[at + 4:at + 8])[0]
    body = bytes(payload[at + 8:at + 8 + size])
    hdr = {'codec': codec, 'rate': rate, 'channels': channels, 'interleave': interleave, 'body_size': size,
           'body_got': len(body)}
    return hdr, body, st


def deinterleave_pcm16(hdr, body):
    """codec 1 body (per-channel blocks) -> interleaved s16le bytes, frames, bytes dropped (a partial last block)"""
    if hdr['codec'] != 1:
        raise PssError('sound codec 0x%X (only 1 = 16-bit PCM is handled)' % hdr['codec'])
    ch, il = hdr['channels'], hdr['interleave']
    if ch not in (1, 2) or il <= 0 or il % 2:
        raise PssError('sound: %d channels, interleave %d' % (ch, il))
    block = ch * il
    whole = len(body) // block
    if ch == 1:
        return bytes(body[:whole * block]), whole * il // 2, len(body) - whole * block
    pcm = bytearray(whole * block)
    for k in range(whole):
        o = k * block
        lb, rb = body[o:o + il], body[o + il:o + block]
        pcm[o + 0:o + block:4] = lb[0::2]
        pcm[o + 1:o + block:4] = lb[1::2]
        pcm[o + 2:o + block:4] = rb[0::2]
        pcm[o + 3:o + block:4] = rb[1::2]
    return bytes(pcm), whole * il // 2, len(body) - whole * block


def write_wav(path, pcm, channels, rate):
    with wave.open(path, 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm)


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    with open(sys.argv[1], 'rb') as f:
        data = f.read()
    hdr, body, st = sound(data)
    pcm, frames, dropped = deinterleave_pcm16(hdr, body)
    write_wav(sys.argv[2], pcm, hdr['channels'], hdr['rate'])
    print('%s: codec %d, %d Hz, %d ch, interleave 0x%X, %d frames (%.3f s), %d bytes dropped, first PTS audio %s '
          'video %s' % (sys.argv[2], hdr['codec'], hdr['rate'], hdr['channels'], hdr['interleave'], frames,
                        frames / float(hdr['rate']), dropped, st['first_audio_pts'], st['first_video_pts']))


if __name__ == '__main__':
    main()
