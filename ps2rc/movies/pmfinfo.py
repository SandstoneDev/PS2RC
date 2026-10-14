#!/usr/bin/env python3
"""pmfinfo.py - inspect and check a PSP movie file (PSMF, *.pmf).

    python pmfinfo.py MOVIE.pmf [--packs N] [--es-out video.264] [--strict] [--json]

A PSMF file is a 2048-byte header followed by an MPEG-2 program stream of 2048-byte packs. This tool prints the
header (version, stream table, first/last 90 kHz timestamps, mux rate), walks every pack (system headers, PES packets,
padding, the private-stream-2 random access point records), extracts the AVC elementary stream, splits it into
access units at the access unit delimiters and cross-checks the random access point records against the access
units. `--strict` turns the layout rules seen in reference files into errors (see check()).

Layout facts used here (measured on reference files; field names are ours):

  header  0x00 "PSMF" + 4 version digits      0x08 u32 stream offset (0x800)   0x0C u32 stream size
          0x50 u32 = 46 + 16 * streams         0x54 / 0x5A 48-bit first / last timestamp (90 kHz)
          0x60 u32 mux rate (50 bytes/s units) 0x64 u32 (90000 in 10 Mbit/s files; meaning unknown)
          0x68 u8 streams, 0x69 u8 1, 0x6A u32 = 20 + 16 * streams, 0x6E / 0x74 first / last timestamp again,
          0x7A u16 1, 0x7C u32 = 2 + 16 * streams, 0x80 u16 streams, 0x82 + 16 * i: stream entries
  entry   +0 stream id (0xE0.. video, 0xBD private 1), +1 private id (audio: 0x0n compressed audio, 0x1n PCM),
          +2 u16 0x2000 | buffer size in KiB, +4 u32 entry point map offset, +8 u32 entry point count,
          video: +12 width / 16, +13 height / 16; audio: +14 channels, +15 rate code (2 = 44.1 kHz)
  RAP pack (one before every IDR access unit): pack header, system header, private stream 2 record, then the
          video PES that starts with the IDR access unit (PTS + DTS + P-STD buffer extension)
  record  u8 1, u8 video stream id, 4 x u16 = pack offset (from the RAP pack) of the pack holding the last byte
          of reference pictures 1..4 (0xFFFF when the group has fewer), 4 zero bytes, u16 = 2 + 4 * n, u16 n,
          n x (u16 0x0080, u16 access unit size)
          for the n access units of the group of pictures (size = from the delimiter's 4-byte start code to the
          next delimiter)
"""
import argparse
import json
import os
import struct
import sys

PACK = 2048
HEADER = 0x800
AUD = b'\x00\x00\x00\x01\x09'


def ts48(b):
    return int.from_bytes(b[:6], 'big')


def pes_ts(b):
    return (((b[0] >> 1) & 7) << 30) | ((((b[1] << 8) | b[2]) >> 1) << 15) | (((b[3] << 8) | b[4]) >> 1)


def scr_27m(b):
    base = ((((b[0] >> 3) & 7) << 30) | ((b[0] & 3) << 28) | (b[1] << 20) | (((b[2] >> 3) & 0x1F) << 15) |
            ((b[2] & 3) << 13) | (b[3] << 5) | (b[4] >> 3))
    ext = ((b[4] & 3) << 7) | (b[5] >> 1)
    return base * 300 + ext


# ------------------------------------------------------------------------------------------------ header
def read_header(d):
    if len(d) < HEADER or d[:4] != b'PSMF':
        raise ValueError('no PSMF magic')
    h = {'magic': d[:8].decode('ascii', 'replace')}
    h['offset'], h['stream_size'] = struct.unpack('>II', d[8:16])
    h['info_len'] = struct.unpack('>I', d[0x50:0x54])[0]
    h['first_ts'], h['last_ts'] = ts48(d[0x54:0x5A]), ts48(d[0x5A:0x60])
    h['mux_rate'], h['field_64'] = struct.unpack('>II', d[0x60:0x68])
    h['n68'], h['n69'] = d[0x68], d[0x69]
    h['len_6a'] = struct.unpack('>I', d[0x6A:0x6E])[0]
    h['first_ts2'], h['last_ts2'] = ts48(d[0x6E:0x74]), ts48(d[0x74:0x7A])
    h['n7a'] = struct.unpack('>H', d[0x7A:0x7C])[0]
    h['len_7c'] = struct.unpack('>I', d[0x7C:0x80])[0]
    n = struct.unpack('>H', d[0x80:0x82])[0]
    streams = []
    for k in range(n):
        e = d[0x82 + 16 * k:0x92 + 16 * k]
        s = {'id': e[0], 'private': e[1], 'buffer': struct.unpack('>H', e[2:4])[0],
             'ep_offset': struct.unpack('>I', e[4:8])[0], 'ep_count': struct.unpack('>I', e[8:12])[0],
             'raw': e.hex()}
        if 0xE0 <= e[0] <= 0xEF:
            s.update(kind='avc', width=e[12] * 16, height=e[13] * 16)
        elif e[0] == 0xBD:
            kind = {0x00: 'atrac3plus', 0x10: 'pcm', 0x20: 'userdata'}.get(e[1] & 0xF0, 'private')
            s.update(kind=kind, channels=e[14], rate_code=e[15])
        else:
            s['kind'] = 'unknown'
        streams.append(s)
    h['streams'] = streams
    h['seconds'] = (h['last_ts'] - h['first_ts']) / 90000.0
    return h


# ------------------------------------------------------------------------------------------------ packs
def walk(d, offset=HEADER, max_packs=None):
    """-> list of packs: {'index', 'scr', 'mux_rate', 'stuffing', 'items': [(sid, info dict)]}"""
    packs = []
    n = (len(d) - offset) // PACK
    for p in range(n if max_packs is None else min(n, max_packs)):
        o = offset + p * PACK
        pk = d[o:o + PACK]
        if pk[:4] != b'\x00\x00\x01\xba' or (pk[4] >> 6) != 1:
            packs.append({'index': p, 'error': 'no MPEG-2 pack header'})
            continue
        stuff = pk[13] & 7
        pack = {'index': p, 'scr': scr_27m(pk[4:10]), 'mux_rate': (pk[10] << 14) | (pk[11] << 6) | (pk[12] >> 2),
                'stuffing': stuff, 'items': []}
        i = 14 + stuff
        while i < PACK:
            if pk[i:i + 3] != b'\x00\x00\x01' or i + 6 > PACK:
                pack['error'] = 'garbage at +%d' % i
                break
            sid = pk[i + 3]
            ln = (pk[i + 4] << 8) | pk[i + 5]
            end = i + 6 + ln
            if end > PACK:
                pack['error'] = 'packet 0x%02X crosses the pack end' % sid
                break
            it = {'at': i, 'len': ln}
            if sid in (0xBB, 0xBE, 0xBF):
                it['body'] = pk[i + 6:end]
            elif sid == 0xBD or 0xC0 <= sid <= 0xEF:
                f1, f2, hl = pk[i + 6], pk[i + 7], pk[i + 8]
                it.update(flags='%02x%02x' % (f1, f2), hl=hl)
                if f2 & 0x80:
                    it['pts'] = pes_ts(pk[i + 9:i + 14])
                if (f2 & 0xC0) == 0xC0:
                    it['dts'] = pes_ts(pk[i + 14:i + 19])
                if f2 & 0x01:
                    it['ext'] = pk[i + 9 + hl - 3:i + 9 + hl].hex()
                it['body'] = pk[i + 9 + hl:end]
            pack['items'].append((sid, it))
            i = end
        packs.append(pack)
    return packs


def split_aus(es):
    """AVC access units at each 4-byte-start-code access unit delimiter -> [(offset, size)]"""
    starts = []
    i = es.find(AUD)
    while i >= 0:
        starts.append(i)
        i = es.find(AUD, i + 5)
    return [(s, (starts[k + 1] if k + 1 < len(starts) else len(es)) - s) for k, s in enumerate(starts)]


def nal_types(au):
    out, i = [], 0
    while True:
        j = au.find(b'\x00\x00\x01', i)
        if j < 0 or j + 3 >= len(au):
            return out
        out.append(au[j + 3] & 0x1F)
        i = j + 3


# ------------------------------------------------------------------------------------------------ SPS
class Bits:
    def __init__(self, b):
        self.b, self.p = b, 0

    def u(self, n):
        v = 0
        for _ in range(n):
            v = (v << 1) | ((self.b[self.p >> 3] >> (7 - (self.p & 7))) & 1)
            self.p += 1
        return v

    def ue(self):
        z = 0
        while self.u(1) == 0:
            z += 1
        return (1 << z) - 1 + self.u(z)

    def se(self):
        k = self.ue()
        return (k + 1) // 2 if k & 1 else -(k // 2)


def unescape(nal):
    out, z = bytearray(), 0
    for c in nal:
        if z >= 2 and c == 3:
            z = 0
            continue
        out.append(c)
        z = z + 1 if c == 0 else 0
    return bytes(out)


def parse_sps(nal):
    """minimal SPS fields (profiles without the high-profile chroma block)"""
    r = Bits(unescape(nal[1:]))
    s = {'profile': r.u(8), 'constraints': r.u(8), 'level': r.u(8), 'sps_id': r.ue()}
    if s['profile'] in (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135):
        s['unsupported'] = 'high profile'
        return s
    s['log2_max_frame_num'] = r.ue() + 4
    s['poc_type'] = r.ue()
    if s['poc_type'] == 0:
        r.ue()
    elif s['poc_type'] == 1:
        r.u(1)
        r.se()
        r.se()
        for _ in range(r.ue()):
            r.se()
    s['ref_frames'] = r.ue()
    s['gaps'] = r.u(1)
    s['width'] = (r.ue() + 1) * 16
    s['height_units'] = r.ue() + 1
    s['frame_mbs_only'] = r.u(1)
    s['height'] = s['height_units'] * 16 * (2 - s['frame_mbs_only'])
    return s


def find_nal(au, t):
    i = 0
    while True:
        j = au.find(b'\x00\x00\x01', i)
        if j < 0 or j + 3 >= len(au):
            return None
        if au[j + 3] & 0x1F == t:
            k = au.find(b'\x00\x00\x01', j + 3)
            nal = au[j + 3:k if k >= 0 else len(au)]
            return nal.rstrip(b'\x00') if k >= 0 else nal
        i = j + 3


# ------------------------------------------------------------------------------------------------ analysis
def analyze(path, max_packs=None):
    d = open(path, 'rb').read()
    h = read_header(d)
    packs = walk(d, h['offset'], max_packs)
    es = bytearray()
    video_pes, raps, layouts, pes_variants, audio = [], [], {}, {}, {}
    pad_bytes = set()
    for pk in packs:
        if 'items' not in pk:
            continue
        lay = ' '.join('%02x' % s for s, _ in pk['items'])
        layouts[lay] = layouts.get(lay, 0) + 1
        for sid, it in pk['items']:
            if sid == 0xBF:
                raps.append({'pack': pk['index'], 'es_pos': len(es), 'body': it['body']})
            elif 0xE0 <= sid <= 0xEF:
                key = '%02x %s hl%d %s' % (sid, it['flags'], it['hl'], it.get('ext', ''))
                pes_variants[key] = pes_variants.get(key, 0) + 1
                video_pes.append({'pack': pk['index'], 'es_pos': len(es), 'len': len(it['body']),
                                  'pts': it.get('pts'), 'dts': it.get('dts')})
                es += it['body']
            elif sid == 0xBD:
                sub = it['body'][0] if it['body'] else -1
                a = audio.setdefault(sub, {'packets': 0, 'bytes': 0, 'first_pts': None})
                a['packets'] += 1
                a['bytes'] += len(it['body'])
                if a['first_pts'] is None and 'pts' in it:
                    a['first_pts'] = it['pts']
            elif sid == 0xBE:
                pad_bytes |= set(it['body'])
    es = bytes(es)
    aus = split_aus(es)
    sps = None
    for off, size in aus[:1]:
        nal = find_nal(es[off:off + size], 7)
        if nal:
            sps = parse_sps(nal)
    info = {'path': path, 'size': len(d), 'header': h, 'packs': len(packs), 'layouts': layouts,
            'pes_variants': pes_variants, 'video_bytes': len(es), 'aus': len(aus),
            'max_au': max((s for _, s in aus), default=0), 'raps': len(raps), 'audio': {('%02x' % k): v for k, v in
                                                                                         audio.items()},
            'padding_bytes': sorted(pad_bytes), 'sps': sps,
            'scr_first_s': packs[0]['scr'] / 27e6 if packs and 'scr' in packs[0] else None,
            'mux_rates': sorted({p['mux_rate'] for p in packs if 'mux_rate' in p}),
            'stuffing': sorted({p['stuffing'] for p in packs if 'stuffing' in p}),
            'pack_errors': [(p['index'], p['error']) for p in packs if 'error' in p][:10]}
    info['_es'], info['_aus'], info['_raps'], info['_video_pes'], info['_packs'] = es, aus, raps, video_pes, packs
    return info


def pack_of(video_pes, byte):
    lo, hi = 0, len(video_pes) - 1
    while lo <= hi:
        m = (lo + hi) // 2
        v = video_pes[m]
        if byte < v['es_pos']:
            hi = m - 1
        elif byte >= v['es_pos'] + v['len']:
            lo = m + 1
        else:
            return v['pack']
    return None


def check(info, frame_ticks=3003):
    """the rules seen in reference files -> list of problems ('' list = all hold)"""
    p = []
    h, es, aus, raps, vpes = info['header'], info['_es'], info['_aus'], info['_raps'], info['_video_pes']
    n = len(h['streams'])
    if not h['magic'][4:].isdigit():
        p.append('version "%s"' % h['magic'])
    if h['offset'] != HEADER or h['stream_size'] != info['size'] - h['offset'] or h['stream_size'] % PACK:
        p.append('offset 0x%X / stream size %d (file %d)' % (h['offset'], h['stream_size'], info['size']))
    if (h['info_len'], h['len_6a'], h['len_7c'], h['n68'], h['n69'], h['n7a']) != (46 + 16 * n, 20 + 16 * n,
                                                                                    2 + 16 * n, n, 1, 1):
        p.append('header lengths/counts %s for %d streams' % ((h['info_len'], h['len_6a'], h['len_7c'], h['n68'],
                                                              h['n69'], h['n7a']), n))
    if (h['first_ts2'], h['last_ts2']) != (h['first_ts'], h['last_ts']):
        p.append('second timestamp pair differs')
    if info['pack_errors']:
        p.append('pack errors %s' % info['pack_errors'])
    if info['mux_rates'] != [h['mux_rate']]:
        p.append('pack mux rates %s vs header %d' % (info['mux_rates'], h['mux_rate']))
    if info['stuffing'] != [0]:
        p.append('pack stuffing %s' % info['stuffing'])
    if info['padding_bytes'] not in ([], [0xFF]):
        p.append('padding bytes %s' % info['padding_bytes'][:8])
    video = [s for s in h['streams'] if s.get('kind') == 'avc']
    if len(video) != 1:
        p.append('%d video streams' % len(video))
        return p
    v = video[0]
    if not aus or aus[0][0] != 0:
        p.append('the video does not start with an access unit delimiter')
        return p
    sps = info['sps']
    if not sps or 'width' not in sps:
        p.append('no SPS in the first access unit')
    elif (sps['width'], sps['height']) != (v['width'], v['height']):
        p.append('SPS %dx%d vs header %dx%d' % (sps['width'], sps['height'], v['width'], v['height']))
    if info['max_au'] > 0xFFFF:
        p.append('access unit of %d bytes (> 65535: does not fit the 16-bit size field)' % info['max_au'])
    # layouts: RAP packs = bb bf e0; other packs hold one stream (+ padding)
    for lay in info['layouts']:
        parts = lay.split()
        if parts[:2] == ['bb', 'bf']:
            if parts[2:3] != ['%02x' % v['id']] or parts[3:] not in ([], ['be']):
                p.append('RAP pack layout "%s"' % lay)
        elif len([x for x in parts if x != 'be']) != 1 or (parts[-1] != 'be' and 'be' in parts):
            p.append('pack layout "%s"' % lay)
    # timestamps: PTS = first + k * frame for the first access unit starting in the PES, DTS = PTS - frame
    starts = [o for o, _ in aus]
    k = 0
    for pe in vpes:
        if pe['pts'] is None:
            continue
        while k < len(starts) and starts[k] < pe['es_pos']:
            k += 1
        if k >= len(starts) or starts[k] >= pe['es_pos'] + pe['len']:
            p.append('PES at pack %d has a PTS but no access unit start' % pe['pack'])
            break
        if pe['pts'] != h['first_ts'] + frame_ticks * k or pe['dts'] != pe['pts'] - frame_ticks:
            p.append('PES at pack %d: PTS %s DTS %s for access unit %d' % (pe['pack'], pe['pts'], pe['dts'], k))
            break
    if h['last_ts'] != h['first_ts'] + frame_ticks * len(aus):
        p.append('last timestamp %d != first + %d frames' % (h['last_ts'], len(aus)))
    # random access point records
    au_index = {o: i for i, (o, _) in enumerate(aus)}
    rap_aus = []
    for r in raps:
        b = r['body']
        i = au_index.get(r['es_pos'])
        if i is None:
            p.append('RAPI at pack %d not followed by an access unit start' % r['pack'])
            continue
        rap_aus.append(i)
        if 5 not in nal_types(es[aus[i][0]:aus[i][0] + aus[i][1]]):
            p.append('RAPI at pack %d: access unit %d is not IDR' % (r['pack'], i))
        if len(b) < 18 or b[0] != 1 or b[1] != v['id'] or b[10:14] != b'\0\0\0\0':
            p.append('RAPI at pack %d: head %s' % (r['pack'], b[:16].hex()))
            continue
        tlen, cnt = struct.unpack('>HH', b[14:18])
        if tlen != 2 + 4 * cnt or len(b) != 18 + 4 * cnt:
            p.append('RAPI at pack %d: lengths %d/%d/%d' % (r['pack'], tlen, cnt, len(b)))
            continue
        ents = [struct.unpack('>HH', b[18 + 4 * j:22 + 4 * j]) for j in range(cnt)]
        sizes = [s for _, s in aus[i:i + cnt]]
        if [s for _, s in ents] != sizes or any(f != 0x80 for f, _ in ents):
            p.append('RAPI at pack %d: size table differs from the access units' % r['pack'])
        idx = list(struct.unpack('>4H', b[2:10]))
        want = []
        for j in range(4):                         # fewer than four pictures in the group: 0xFFFF
            want.append(pack_of(vpes, aus[i + j][0] + aus[i + j][1] - 1) - r['pack'] if j < cnt and i + j < len(aus)
                        else 0xFFFF)
        if idx != want:
            p.append('RAPI at pack %d: pack offsets %s, expected %s' % (r['pack'], idx, want))
    # every IDR access unit has a RAPI and the GOP lengths add up
    idr = [i for i, (o, s) in enumerate(aus) if 5 in nal_types(es[o:o + s])]
    if idr != rap_aus:
        p.append('IDR access units %s... vs RAPI positions %s...' % (idr[:6], rap_aus[:6]))
    else:
        for r, i, j in zip(raps, rap_aus, rap_aus[1:] + [len(aus)]):
            cnt = struct.unpack('>H', r['body'][16:18])[0]
            if cnt != j - i:
                p.append('RAPI at pack %d counts %d access units, the GOP has %d' % (r['pack'], cnt, j - i))
                break
    return p


def summary(info):
    h = info['header']
    lines = ['%s: %d bytes, %s, stream at 0x%X size %d, %.3f s (ts %d..%d), mux rate %d (%.1f Mbit/s), field_64 %d'
             % (os.path.basename(info['path']), info['size'], h['magic'], h['offset'], h['stream_size'], h['seconds'],
                h['first_ts'], h['last_ts'], h['mux_rate'], h['mux_rate'] * 400 / 1e6, h['field_64'])]
    for s in h['streams']:
        extra = ('%dx%d' % (s['width'], s['height'])) if s['kind'] == 'avc' else \
            ('ch %d rate code %d' % (s.get('channels', 0), s.get('rate_code', 0)))
        lines.append('  stream %02X/%02X %s %s buffer 0x%04X ep_offset %d ep_count %d' % (
            s['id'], s['private'], s['kind'], extra, s['buffer'], s['ep_offset'], s['ep_count']))
    lines.append('  packs %d, first SCR %.4f s, layouts %s' % (info['packs'], info['scr_first_s'] or 0,
                                                                dict(sorted(info['layouts'].items(),
                                                                            key=lambda x: -x[1]))))
    lines.append('  video PES variants %s' % info['pes_variants'])
    lines.append('  video %d bytes, %d access units (max %d bytes), %d RAPI; audio sub-streams %s' % (
        info['video_bytes'], info['aus'], info['max_au'], info['raps'], info['audio']))
    if info['sps']:
        lines.append('  SPS %s' % {k: v for k, v in info['sps'].items()})
    return '\n'.join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pmf')
    ap.add_argument('--packs', type=int, default=0, help='print the first N packs')
    ap.add_argument('--es-out', help='write the AVC elementary stream here')
    ap.add_argument('--strict', action='store_true', help='exit 1 if a layout rule does not hold')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    info = analyze(a.pmf)
    probs = check(info)
    if a.json:
        print(json.dumps({k: v for k, v in info.items() if not k.startswith('_')}, indent=1, default=str))
    else:
        print(summary(info))
        for pk in info['_packs'][:a.packs]:
            items = []
            for sid, it in pk['items']:
                s = '%02x+%d' % (sid, it['len'])
                if 'pts' in it:
                    s += ' pts %d' % it['pts']
                if 'dts' in it:
                    s += ' dts %d' % it['dts']
                items.append(s)
            print('  pack %d scr %.4f %s' % (pk['index'], pk['scr'] / 27e6, ', '.join(items)))
        print('  check: %s' % ('all rules hold' if not probs else '; '.join(probs)))
    if a.es_out:
        with open(a.es_out, 'wb') as f:
            f.write(info['_es'])
    if a.strict and probs:
        sys.exit(1)


if __name__ == '__main__':
    main()
