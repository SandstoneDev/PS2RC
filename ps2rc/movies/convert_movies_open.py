#!/usr/bin/env python3
"""convert_movies_open.py - PS2 movies (*.PSS) -> PSP movies with open tools only: <NAME>.pmf (H.264 video in a PSMF
file written by pmfmux.py) + <NAME>.wav (the sound, IMA ADPCM or 16-bit PCM, 44.1 kHz stereo).

    python convert_movies_open.py --src DIR_OR_FILE [...] --out DIR [--names NAME ...] [--ffmpeg PATH]
        [--vkbps 1500] [--no-2pass] [--audio ima|pcm|none] [--aspect auto|fill] [--deblock] [--work DIR]
        [--keep-work] [--preview]

External program: ffmpeg with libx264 (found through --ffmpeg, the FFMPEG environment variable, or PATH; ffprobe is
optional - without it the stream facts come from ffmpeg -i; ffprobe next
to it or on PATH). It is not part of this project: the user installs it. Licences: ffmpeg is LGPL/GPL depending on
the build, libx264 is GPL - both run here as separate programs, nothing of them is linked or copied. (OpenH264, BSD,
would need a different encoder option set: not wired here.)

Per movie NAME.PSS (names are matched case-insensitively; outputs are upper case: NAME.pmf / NAME.wav):
  1. sound: the PSS's own sound track (pss.py: private stream 1, SShd/SSbd, 16-bit PCM) -> WAV at the source rate,
     shifted by (first sound PTS - first video PTS), then ffmpeg: 44.1 kHz stereo (the PSP's sceAudio rate), IMA ADPCM
     (block 2048 bytes; ~44 KB/s) or PCM (~176 KB/s);
  2. video: ffmpeg -> H.264 Main profile level 2.1, 480x272 (the picture's display aspect kept, black bars), 29.97 fps
     progressive (a 25 fps source shows every 5th frame twice; timing kept), the settings in X264 below (one reference
     frame, no B pictures, CABAC, access unit delimiters, SPS/PPS before every IDR, IDR every <= 59 frames, HRD + picture
     timing SEI, VBV 3000 kbit/s / 1875 kbit, no deblocking), average --vkbps (2-pass by default);
  3. pmfmux.py: the elementary stream -> NAME.pmf (checked with pmfinfo.check());
  4. checks: ffmpeg decodes the PMF (frame count), the WAV's length vs the video's.
Output: <out>/NAME.pmf, <out>/NAME.wav, <out>/movies_open_report.txt (sizes, md5, lengths, settings). The same input
and the same ffmpeg build give the same bytes (x264 with a fixed thread count, bitexact flags).
"""
import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import pmfmux  # noqa: E402
import pss  # noqa: E402

PSP_W, PSP_H = 480, 272
VBV_MAXRATE, VBV_BUFSIZE = 3000, 1875          # kbit/s, kbit (the reference files' HRD buffer is 1,874,992 bits)
X264 = ':'.join([
    'ref=1', 'bframes=0', 'weightp=0', 'cabac=1', '8x8dct=0', 'slices=1',
    'keyint=59', 'min-keyint=12',               # an entry point at least every 59 frames (~2 s)
    'aud=1', 'repeat-headers=1',                # an access unit delimiter per picture, SPS/PPS before every IDR
    'nal-hrd=vbr', 'pic-struct=1', 'force-cfr=1',
    'vbv-maxrate=%d' % VBV_MAXRATE, 'vbv-bufsize=%d' % VBV_BUFSIZE,
    'colorprim=smpte170m', 'transfer=smpte170m', 'colormatrix=smpte170m', 'sar=1/1',
    'threads=1',                                # one thread: repeatable output (4 frame threads + VBV were not)
])
IMA_BLOCK = 2048
BITEXACT = ['-fflags', '+bitexact', '-flags', '+bitexact', '-map_metadata', '-1']


def log(msg):
    print(msg, flush=True)


def hidden():
    """no console window for ffmpeg / ffprobe on Windows (this script may run without a console of its own)"""
    if os.name != 'nt':
        return {}
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0                                  # SW_HIDE
    return {'creationflags': 0x08000000, 'startupinfo': si}     # CREATE_NO_WINDOW


def run(cmd, timeout=None):
    t0 = time.time()
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout, **hidden())
    out = p.stdout.decode('utf-8', 'replace')
    if p.returncode != 0:
        log(out[-3000:])
        raise RuntimeError('failed (rc=%d): %s' % (p.returncode, ' '.join(cmd)))
    return out, time.time() - t0


def find_tool(name, given, env):
    for cand in (given, os.environ.get(env)):
        if cand:
            if os.path.isfile(cand):
                return cand
            sys.exit('%s not found: %s' % (name, cand))
    p = shutil.which(name)
    if not p:
        sys.exit('%s not found: pass --%s or set %s' % (name, name, env))
    return p


def md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def sources(args):
    found = {}
    for s in args.src:
        if os.path.isdir(s):
            for fn in sorted(os.listdir(s)):
                if fn.lower().endswith('.pss'):
                    found.setdefault(fn[:-4].upper(), os.path.join(s, fn))
        elif os.path.isfile(s):
            found.setdefault(os.path.splitext(os.path.basename(s))[0].upper(), s)
        else:
            sys.exit('no such source: %s' % s)
    if args.names:
        want = [n.upper() for n in args.names]
        missing = [n for n in want if n not in found]
        if missing:
            sys.exit('not found in the sources: %s' % ', '.join(missing))
        return [(n, found[n]) for n in want]
    return sorted(found.items())


FF_DUR = re.compile(r'Duration: (\d+):(\d\d):(\d\d(?:\.\d+)?)')
FF_VID = re.compile(r'Stream #\S+.*?: Video: .*?(\d{2,5})x(\d{2,5})(?: \[SAR (\d+):(\d+) DAR (\d+):(\d+)\])?')
FF_FPS = re.compile(r'(\d+(?:\.\d+)?) fps')
FF_AUD = re.compile(r'Stream #\S+.*?: Audio: (\w+).*?(\d{4,6}) Hz, (mono|stereo|\d+ channels)')


def ffmpeg_info(text):
    """The facts ffprobe would give, from the header ffmpeg -i prints: {width, height, display_aspect_ratio,
    r_frame_rate, duration} of the first video stream, {codec_name, sample_rate, channels} of the first audio
    stream (keys missing when not found)."""
    kv = {}
    m = FF_DUR.search(text)
    if m:
        kv['duration'] = '%.6f' % (int(m.group(1)) * 3600 + int(m.group(2)) * 60 + float(m.group(3)))
    for line in text.splitlines():
        v = FF_VID.search(line)
        if v and 'width' not in kv:
            kv['width'], kv['height'] = v.group(1), v.group(2)
            if v.group(5):
                kv['display_aspect_ratio'] = '%s:%s' % (v.group(5), v.group(6))
            f = FF_FPS.search(line)
            if f:
                fps = float(f.group(1))
                kv['r_frame_rate'] = '%d/1' % fps if fps == int(fps) else '%s/1' % f.group(1)
        a = FF_AUD.search(line)
        if a and 'codec_name' not in kv:
            kv['codec_name'], kv['sample_rate'] = a.group(1), a.group(2)
            kv['channels'] = {'mono': '1', 'stereo': '2'}.get(a.group(3), a.group(3).split()[0])
    return kv


def stream_facts(args, src, entries):
    """ffprobe's -show_entries as a dict, or (no ffprobe) the same facts from ffmpeg -i"""
    if args.ffprobe:
        out, _ = run([args.ffprobe, '-v', 'error'] + (['-select_streams', 'v:0'] if 'width' in entries else []) +
                     ['-show_entries', entries, '-of', 'default=nw=1', src])
        return dict(line.split('=', 1) for line in out.splitlines() if '=' in line)
    p = subprocess.run([args.ffmpeg, '-hide_banner', '-i', src], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       **hidden())
    return ffmpeg_info(p.stdout.decode('utf-8', 'replace'))


def probe_video(args, src):
    kv = stream_facts(args, src, 'stream=width,height,display_aspect_ratio,r_frame_rate:format=duration')
    w, h = int(kv['width']), int(kv['height'])
    dar = kv.get('display_aspect_ratio', '0:1')
    try:
        num, den = (int(x) for x in dar.split(':'))
    except ValueError:
        num, den = 0, 0
    if num <= 0 or den <= 0:
        num, den = w, h
    return w, h, num / float(den), kv


def video_filter(dar, aspect):
    if aspect == 'fill':
        pw, ph = PSP_W, PSP_H
    elif dar >= PSP_W / float(PSP_H):
        pw, ph = PSP_W, min(PSP_H, int(round(PSP_W / dar / 2.0)) * 2)
    else:
        pw, ph = min(PSP_W, int(round(PSP_H * dar / 2.0)) * 2), PSP_H
    return ('scale=%d:%d:flags=lanczos,pad=%d:%d:%d:%d:black,setsar=1,fps=30000/1001,format=yuv420p' %
            (pw, ph, PSP_W, PSP_H, (PSP_W - pw) // 2, (PSP_H - ph) // 2)), '%dx%d' % (pw, ph)


def has_soxr(ffmpeg):
    out, _ = run([ffmpeg, '-hide_banner', '-version'])
    return '--enable-libsoxr' in out


def convert(name, src, args, work):
    t_all = time.time()
    times = {}
    r = {'name': name, 'src': os.path.basename(src)}
    pmf = os.path.join(args.out, name + '.pmf')
    wav_out = os.path.join(args.out, name + '.wav')
    with open(src, 'rb') as f:
        data = f.read()
    r['src_bytes'] = len(data)
    # 1. sound
    a_src = os.path.join(work, name + '_a_src.wav')
    snd = None
    if args.audio != 'none':
        try:
            hdr, body, st = pss.sound(data)
            pcm, frames, dropped = pss.deinterleave_pcm16(hdr, body)
            shift = 0
            if st['first_audio_pts'] is not None and st['first_video_pts'] is not None:
                shift = st['first_audio_pts'] - st['first_video_pts']          # 90 kHz; > 0: the sound starts later
            fshift = int(round(shift * hdr['rate'] / 90000.0))
            fb = 2 * hdr['channels']
            pcm = (b'\0' * (fshift * fb) + pcm) if fshift > 0 else pcm[-fshift * fb:]
            pss.write_wav(a_src, pcm, hdr['channels'], hdr['rate'])
            snd = {'codec': hdr['codec'], 'rate': hdr['rate'], 'channels': hdr['channels'],
                   'interleave': hdr['interleave'], 'frames': frames, 'dropped': dropped, 'shift_frames': fshift}
        except pss.PssError as e:
            log('%s: no usable sound (%s) - video only' % (name, e))
            r['sound_note'] = str(e)
    if snd:
        rs = 'aresample=44100:resampler=soxr:precision=28' if args.soxr else 'aresample=44100:filter_size=64:cutoff=0.97'
        codec = ['-c:a', 'adpcm_ima_wav', '-block_size', str(IMA_BLOCK)] if args.audio == 'ima' else ['-c:a', 'pcm_s16le']
        _, times['audio'] = run([args.ffmpeg, '-v', 'error', '-y', '-i', a_src, '-af', rs, '-ac', '2'] + codec +
                                BITEXACT + [wav_out])
        with wave.open(a_src, 'rb') as w:
            snd['src_seconds'] = w.getnframes() / float(w.getframerate())
    elif os.path.exists(wav_out):
        os.remove(wav_out)                      # no stale sound next to a new video
    r['sound'] = snd
    # 2. video
    w, h, dar, kv = probe_video(args, src)
    vf, pic = video_filter(dar, args.aspect)
    r['src_video'] = '%dx%d dar %.4f %s fps %s s' % (w, h, dar, kv.get('r_frame_rate'), kv.get('duration'))
    r['picture'] = pic
    es = os.path.join(work, name + '.264')
    x264 = X264 + ('' if args.deblock else ':no-deblock=1')
    base = [args.ffmpeg, '-v', 'error', '-y', '-i', src, '-map', '0:v:0', '-an', '-vf', vf, '-c:v', 'libx264',
            '-profile:v', 'main', '-level:v', '2.1', '-preset', args.preset, '-b:v', '%dk' % args.vkbps,
            '-x264-params', x264] + BITEXACT
    if args.no_2pass:
        _, times['x264'] = run(base + ['-f', 'h264', es])
    else:
        plog = os.path.join(work, name + '_x264')
        _, t1 = run(base + ['-pass', '1', '-passlogfile', plog, '-f', 'null', '-'])
        _, t2 = run(base + ['-pass', '2', '-passlogfile', plog, '-f', 'h264', es])
        times['x264'] = t1 + t2
    # 3. mux
    with open(es, 'rb') as f:
        es_bytes = f.read()
    t0 = time.time()
    st = pmfmux.write_checked(es_bytes, pmf, vbv_kbit=VBV_BUFSIZE)
    times['mux'] = time.time() - t0
    r['pmf'] = {k: v for k, v in st.items() if k != 'sps'}
    # 4. checks
    dec, times['check'] = run([args.ffmpeg, '-hide_banner', '-nostats', '-v', 'info', '-i', pmf, '-map', '0:v:0',
                               '-f', 'null', '-'])
    frames = None
    for line in dec.splitlines():
        if 'frame=' in line:
            try:
                frames = int(line.split('frame=')[1].split()[0])
            except (ValueError, IndexError):
                pass
    if frames != st['access_units']:
        raise RuntimeError('%s: ffmpeg decoded %s frames of %d' % (name, frames, st['access_units']))
    r['decoded_frames'] = frames
    r['pmf_bytes'], r['pmf_md5'] = os.path.getsize(pmf), md5(pmf)
    if snd:
        kv = stream_facts(args, wav_out, 'stream=codec_name,sample_rate,channels,block_align:format=duration')
        snd['wav_seconds'] = float(kv.get('duration', 0) or 0)
        snd['wav_codec'] = kv.get('codec_name')
        r['wav_bytes'], r['wav_md5'] = os.path.getsize(wav_out), md5(wav_out)
        diff = snd['wav_seconds'] - st['seconds']
        if abs(diff) > 0.25:
            log('%s: note - sound %.3f s vs video %.3f s' % (name, snd['wav_seconds'], st['seconds']))
    if args.preview:
        pdir = os.path.join(args.out, 'preview')
        os.makedirs(pdir, exist_ok=True)
        for t in [x for x in (1, 3, 6, 9, 12, 20, 35, 50, 65, 80) if x < st['seconds']][:6]:
            run([args.ffmpeg, '-v', 'error', '-y', '-ss', str(t), '-i', pmf, '-frames:v', '1',
                 os.path.join(pdir, '%s_%02d.png' % (name, t))])
    if not args.keep_work:
        for p in (a_src, es, os.path.join(work, name + '_x264-0.log'), os.path.join(work, name + '_x264-0.log.mbtree')):
            if os.path.exists(p):
                os.remove(p)
    r['times'] = times
    r['total_s'] = time.time() - t_all
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', nargs='+', default=[os.environ['MOVIE_SRC']] if os.environ.get('MOVIE_SRC') else None,
                    help='folders with *.PSS and/or .PSS files (default: $MOVIE_SRC); only read')
    ap.add_argument('--out', required=True)
    ap.add_argument('--names', nargs='*', help='only these movies (base names)')
    ap.add_argument('--work', help='temp folder (default <out>/work)')
    ap.add_argument('--ffmpeg')
    ap.add_argument('--ffprobe')
    ap.add_argument('--vkbps', type=int, default=1500, help='H.264 average kbit/s (default 1500)')
    ap.add_argument('--preset', default='medium', help='x264 preset (default medium)')
    ap.add_argument('--no-2pass', action='store_true', help='one ABR pass instead of two')
    ap.add_argument('--audio', choices=('ima', 'pcm', 'none'), default='ima')
    ap.add_argument('--aspect', choices=('auto', 'fill'), default='auto',
                    help='auto: keep the display aspect (black bars), fill: stretch to 480x272')
    ap.add_argument('--deblock', action='store_true', help='H.264 deblocking on (default off)')
    ap.add_argument('--keep-work', action='store_true')
    ap.add_argument('--preview', action='store_true', help='a few PNG frames of each PMF into <out>/preview')
    args = ap.parse_args()
    if not args.src:
        sys.exit('pass --src (or set MOVIE_SRC)')
    args.ffmpeg = find_tool('ffmpeg', args.ffmpeg, 'FFMPEG')
    if not args.ffprobe:
        cand = os.path.join(os.path.dirname(args.ffmpeg), 'ffprobe' + os.path.splitext(args.ffmpeg)[1])
        args.ffprobe = cand if os.path.isfile(cand) else None
    if args.ffprobe or os.environ.get('FFPROBE') or shutil.which('ffprobe'):
        args.ffprobe = find_tool('ffprobe', args.ffprobe, 'FFPROBE')
    else:
        args.ffprobe = None                     # ffmpeg -i gives the same facts (ffmpeg_info)
    enc, _ = run([args.ffmpeg, '-hide_banner', '-encoders'])
    if 'libx264' not in enc:
        sys.exit('this ffmpeg has no libx264 encoder')
    args.soxr = has_soxr(args.ffmpeg)
    src_abs = [os.path.abspath(s).lower() for s in args.src]
    for p in (args.out, args.work or ''):
        if p and any(os.path.abspath(p).lower().startswith(s.rstrip('\\/') + os.sep) or
                     os.path.abspath(p).lower() == s for s in src_abs if os.path.isdir(s)):
            sys.exit('refusing to write into a source folder: ' + p)
    os.makedirs(args.out, exist_ok=True)
    work = args.work or os.path.join(args.out, 'work')
    os.makedirs(work, exist_ok=True)
    ver = run([args.ffmpeg, '-hide_banner', '-version'])[0].splitlines()[0]
    report = []
    for name, src in sources(args):
        log('%s: converting %s ...' % (name, src))
        r = convert(name, src, args, work)
        s = r['sound']
        log('%s: %.3f s, %d frames, PMF %d bytes (md5 %s)%s, %.0f s' % (
            name, r['pmf']['seconds'], r['decoded_frames'], r['pmf_bytes'], r['pmf_md5'],
            ', WAV %d bytes %s %.3f s (md5 %s)' % (r['wav_bytes'], s['wav_codec'], s['wav_seconds'], r['wav_md5'])
            if s else ', no sound', r['total_s']))
        report.append(r)
    if not args.keep_work:
        try:
            os.rmdir(work)
        except OSError:
            pass
    with open(os.path.join(args.out, 'movies_open_report.txt'), 'a', encoding='utf-8', newline='\n') as f:
        f.write('# %s vkbps=%d 2pass=%d preset=%s audio=%s aspect=%s deblock=%d soxr=%d | %s\n# x264 %s\n' % (
            time.strftime('%Y-%m-%d %H:%M:%S'), args.vkbps, 0 if args.no_2pass else 1, args.preset, args.audio,
            args.aspect, 1 if args.deblock else 0, 1 if args.soxr else 0, ver, X264))
        for r in report:
            f.write('%s\n' % {k: v for k, v in r.items()})
    total = sum(r['pmf_bytes'] + r.get('wav_bytes', 0) for r in report)
    log('done: %d movies, %d bytes (%.2f MiB) in %s' % (len(report), total, total / 1048576.0, args.out))


if __name__ == '__main__':
    main()
