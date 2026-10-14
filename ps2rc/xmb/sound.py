"""SND0 (menu music) from WAV / MP3 / OGG / FLAC / any audio ffmpeg reads.

    pcm = read_pcm(src, start, seconds)          44.1 kHz 16-bit stereo, cut to the part the player chose
    pcm = shape(pcm, loop=True)                  fade in, and a seamless loop (the end cross-fades into the start)
    at3, rep = encode(pcm)                       ATRAC3plus through atracdenc, re-wrapped as an AT3 file

Encoder: atracdenc (open source, LGPL-2.1, github.com/dcherednik/atracdenc), run as a separate program:
`-e atrac3plus -i in.wav -o out.oma` (release 0.2.3 writes ATRAC3plus only into an OMA file; later sources can also
write RIFF - both are read). Its ATRAC3plus frame is fixed: 2048 bytes per 2048 samples = 44,100 bytes per second
(352.8 kbit/s, the highest standard ATRAC3plus rate), so the shared ICON1 + SND0 budget of 500 KB holds at most
about 11 s of music (less with an animated icon). Without atracdenc the menu music is off and the window says why;
a ready .at3 file is still accepted.

The AT3 layout written here (normalize()) is the one the PSP's ATRAC library reads (the reader rules of PPSSPP's
Core/Util/AtracTrack.cpp, public source): a 52-byte WAVE_FORMAT_EXTENSIBLE 'fmt ' chunk (atracdenc's own RIFF
writer uses 40 bytes, which that reader refuses) with the ATRAC3plus GUID and the codec word (sample-rate index,
channel layout, frame size), 'fact' = (samples, encoder delay), 'smpl' with one loop over the whole piece, then the
frames. The encoder delay (ENC_DELAY, measured on atracdenc 0.2.3 with measure_delay()) goes into fact / smpl.
Whether the PSP menu plays this file is Unknown until tried on a PSP.
"""
import array
import io
import os
import struct
import sys
import tempfile
import wave

from ps2rc.pbp import at3 as pat3
from ps2rc.pbp.spec import PbpError, blocking, SND0_RATE

from . import media

RATE = SND0_RATE
FRAME_SAMPLES = 2048
FRAME_BYTES = 2048                      # atracdenc's ATRAC3plus frame (fixed)
BYTES_PER_SECOND = FRAME_BYTES * RATE / FRAME_SAMPLES
HEADER_BYTES = 12 + (8 + 52) + (8 + 8) + (8 + 60) + 8
GUID = bytes.fromhex("bfaa23e958cb7144a119fffa01e4ce62")
MIN_SECONDS = 1.0
DEFAULT_SECONDS = 6.0
LOOP_XFADE = 0.35
# atracdenc 0.2.3 ATRAC3plus, measured 2026-10-14 (measure_delay: ffmpeg decode of the re-wrapped file vs the input,
# a 0.5 s silence + chord signal): the input's first sample comes out 4464 samples late = 4096 (two frames, the
# encoder's look-ahead) + 368 (the decoder's own start offset, 0x170 in PPSSPP's reader). fact / smpl count the
# encoder part only (the reader adds its 368), as files of the commonly used encoder do (loop start = fact delay).
ENC_DELAY = 4096
DECODER_EXTRA = 368
PAD = ENC_DELAY + DECODER_EXTRA + FRAME_SAMPLES        # silence appended so the encoder's last frames keep the tail


class Codec:
    """one atracdenc mode: frame samples / bytes, its -e name, output extension (0.2.3: ATRAC3plus only as OMA,
    ATRAC3 as RIFF .at3), the encoder delay written into fact / smpl and the reader's own start offset"""

    def __init__(self, name, label, enc, ext, frame_samples, frame_bytes, delay, extra, header, on_spec):
        self.name, self.label, self.enc, self.ext = name, label, enc, ext
        self.frame_samples, self.frame_bytes, self.delay, self.extra = frame_samples, frame_bytes, delay, extra
        self.header, self.on_spec = header, on_spec
        self.bytes_per_second = frame_bytes * RATE / float(frame_samples)
        self.pad = delay + extra + 2 * frame_samples

    @property
    def kbps(self):
        return round(self.bytes_per_second * 8 / 1000.0, 1)


# ATRAC3 (atracdenc 0.2.3 `-e atrac3` = LP2 384-byte frames, `-e atrac3_lp4` = LP4 192-byte joint-stereo frames):
# measured lag of the ffmpeg-decoded file 1316 samples for both; the PSP reader's own offset for ATRAC3 is 0x45 = 69
# (PPSSPP's reader) -> fact / smpl delay 1247 (I: by analogy with ATRAC3plus, where 4096 + 368 was measured).
# The menu documentation names ATRAC3plus only: an ATRAC3 SND0 is outside it, but all three modes played in the
# menu of a PSP-3000 (custom firmware 6.61) on 2026-10-14 (HW folders MUSIC TEST A / B / C); other models untested.
# The packer reports ATRAC3 as a warning.
CODECS = {
    "atrac3plus": Codec("atrac3plus", "ATRAC3plus 352 kbit/s", "atrac3plus", ".oma", 2048, 2048, ENC_DELAY,
                        DECODER_EXTRA, HEADER_BYTES, True),
    "atrac3": Codec("atrac3", "ATRAC3 132 kbit/s", "atrac3", ".at3", 1024, 384, 1247, 69,
                    12 + (8 + 32) + (8 + 8) + (8 + 60) + 8, False),
    "atrac3_lp4": Codec("atrac3_lp4", "ATRAC3 66 kbit/s", "atrac3_lp4", ".at3", 1024, 192, 1247, 69,
                        12 + (8 + 32) + (8 + 8) + (8 + 60) + 8, False),
}


# the window's choice: (key, codec); ATRAC3 132k by default (about 30 s in the 500 KB budget)
CHOICES = (("best", "atrac3plus"), ("balanced", "atrac3"), ("longest", "atrac3_lp4"))
DEFAULT_CODEC = "atrac3"


# ------------------------------------------------------------------------------------------ encoder lookup
def find_encoder(given=None):
    return media.find_tool("atracdenc", given, "PS2RC_ATRACDENC")


MEDIA_FEATURE_PACK = "https://support.microsoft.com/help/3145500"


def windows_media_missing():
    """True on a Windows without Media Foundation (the N / KN editions before the Media Feature Pack is installed):
    the shipped atracdenc build links Media Foundation and cannot start there"""
    if os.name != "nt":
        return False
    sysdir = os.path.join(os.environ.get("SystemRoot") or os.environ.get("SystemDrive", "C:") + "\\Windows",
                          "System32")
    return os.path.isdir(sysdir) and not os.path.isfile(os.path.join(sysdir, "mfplat.dll"))


def status(given=None):
    """-> (path or None, plain message)"""
    p = find_encoder(given)
    if p and windows_media_missing():
        return None, ("menu music: this Windows has no Media Foundation (a Windows N / KN edition), which the "
                      "ATRAC3plus encoder needs: install Microsoft's Media Feature Pack (%s), or give a ready .at3 "
                      "file" % MEDIA_FEATURE_PACK)
    if p:
        return p, "ATRAC3plus encoder: %s" % p
    return None, ("menu music needs the free ATRAC3plus encoder atracdenc (LGPL), which is not installed: put "
                  "atracdenc.exe into the converter's tools/atracdenc folder or on PATH, or give a ready .at3 file")


# ------------------------------------------------------------------------------------------ PCM
def read_pcm(src, start=0.0, seconds=DEFAULT_SECONDS, ffmpeg=None):
    """-> bytes, s16le stereo 44.1 kHz"""
    start, seconds = max(0.0, float(start or 0)), max(0.05, float(seconds))
    ff = media.find_ffmpeg(ffmpeg)
    if ff:
        cmd = [ff, "-v", "error", "-ss", "%.3f" % start, "-t", "%.3f" % seconds, "-i", src, "-vn", "-ac", "2",
               "-ar", str(RATE), "-f", "s16le", "-acodec", "pcm_s16le", "-"]
        pcm = media.run(cmd).stdout
    else:
        if not str(src).lower().endswith(".wav"):
            raise PbpError("%s: reading MP3 / OGG / other music needs ffmpeg (the converter app ships it); a WAV "
                           "file works without it" % src)
        pcm = _read_wav(src, start, seconds)
    pcm = pcm[:len(pcm) // 4 * 4]
    if len(pcm) < 4 * RATE * 0.05:
        raise PbpError("%s: no sound between %.1f s and %.1f s" % (src, start, start + seconds))
    return pcm


def _read_wav(src, start, seconds):
    try:
        w = wave.open(src, "rb")
    except (wave.Error, EOFError, OSError) as e:
        raise PbpError("%s: not a WAV file Python can read (%s); ffmpeg reads more kinds" % (src, e))
    with w:
        ch, width, rate = w.getnchannels(), w.getsampwidth(), w.getframerate()
        if width != 2 or rate != RATE or ch not in (1, 2):
            raise PbpError("%s: %d Hz %d-bit %d channel(s); without ffmpeg only 44100 Hz 16-bit WAV is read" % (
                src, rate, width * 8, ch))
        w.setpos(min(w.getnframes(), int(start * rate)))
        raw = w.readframes(int(seconds * rate))
    if ch == 1:
        a = array.array("h", raw)
        if sys.byteorder == "big":
            a.byteswap()
        st = array.array("h", bytes(len(a) * 4))
        st[0::2] = a
        st[1::2] = a
        if sys.byteorder == "big":
            st.byteswap()
        raw = st.tobytes()
    return raw


def _samples(pcm):
    a = array.array("h", pcm)
    if sys.byteorder == "big":
        a.byteswap()
    return a


def _bytes(a):
    if sys.byteorder == "big":
        a = array.array("h", a)
        a.byteswap()
    return a.tobytes()


def shape(pcm, loop=True, fade_in=0.02, fade_out=0.6, xfade=LOOP_XFADE):
    """loop=True: the last `xfade` seconds are mixed into the first ones, so the end runs into the start without a
    click (the result is `xfade` shorter). loop=False: fade in / fade out."""
    a = _samples(pcm)
    n = len(a) // 2
    if loop:
        x = min(int(xfade * RATE), n // 3)
        if x > 0:
            body = a[:2 * (n - x)]
            tail = a[2 * (n - x):]
            for i in range(x):
                g = i / float(x)
                for c in (0, 1):
                    k = 2 * i + c
                    body[k] = int(max(-32768, min(32767, round(a[k] * g + tail[k] * (1.0 - g)))))
            a = body
    else:
        n_in, n_out = int(fade_in * RATE), int(fade_out * RATE)
        n_in, n_out = min(n_in, n // 2), min(n_out, n // 2)
        for i in range(n_in):
            g = i / float(n_in)
            a[2 * i] = int(a[2 * i] * g)
            a[2 * i + 1] = int(a[2 * i + 1] * g)
        for i in range(n_out):
            g = i / float(n_out)
            j = n - 1 - i
            a[2 * j] = int(a[2 * j] * g)
            a[2 * j + 1] = int(a[2 * j + 1] * g)
    return _bytes(a)


def wav_bytes(pcm):
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm)
    return buf.getvalue()


# ------------------------------------------------------------------------------------------ budget
def size_for(seconds, codec="atrac3plus"):
    """bytes of the AT3 file encode() makes of `seconds` of music (whole frames of the padded input)"""
    c = CODECS[codec]
    frames = (int(round(seconds * RATE)) + c.pad) // c.frame_samples + 2     # +2: measured on atracdenc 0.2.3
    return c.header + frames * c.frame_bytes


def max_seconds(free_bytes, codec="atrac3plus"):
    """the longest music that fits `free_bytes`"""
    c = CODECS[codec]
    frames = (free_bytes - c.header) // c.frame_bytes - 2
    return max(0.0, (frames * c.frame_samples - c.pad) / float(RATE))


# ------------------------------------------------------------------------------------------ AT3 layout
def config_word(frame_bytes, channels):
    """the 16-bit ATRAC3plus codec word of the fmt extension: bits 13-15 sample-rate index (1 = 44.1 kHz),
    10-12 channel layout (1 mono, 2 stereo), 0-9 frame bytes / 8 - 1 (e.g. 744 bytes stereo -> 0x285C)"""
    if frame_bytes % 8 or not 8 <= frame_bytes <= 8192:
        raise PbpError("ATRAC3plus frame size %d is not a multiple of 8 in 8..8192" % frame_bytes)
    return (1 << 13) | ({1: 1, 2: 2}[channels] << 10) | (frame_bytes // 8 - 1)


def chunks(data):
    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise PbpError("the encoder did not write a RIFF/WAVE file")
    pos, out = 12, {}
    while pos + 8 <= len(data):
        cid, size = struct.unpack("<4sI", data[pos:pos + 8])
        out.setdefault(cid, data[pos + 8:pos + 8 + size])
        pos += 8 + size + (size & 1)
    return out


def parse_oma(data):
    """an OMA file (the container atracdenc 0.2.x writes for ATRAC3plus) -> (frames, frame bytes, channels, rate).
    Layout (liboma of atracdenc, ffmpeg's omadec): an optional 'ea3' ID3v2-style tag (10-byte header, syncsafe size),
    then a 96-byte 'EA3' header whose big-endian word at +32 = codec id << 24 | rate index << 13 |
    channel id << 10 | (frame bytes - 8) / 8; the frames follow."""
    pos = 0
    if data[:3] == b"ea3" and len(data) >= 10:
        size = (data[6] & 0x7F) << 21 | (data[7] & 0x7F) << 14 | (data[8] & 0x7F) << 7 | (data[9] & 0x7F)
        pos = 10 + size
    h = data[pos:pos + 96]
    if len(h) < 96 or h[:3] != b"EA3" or h[5] != 96:
        raise PbpError("the encoder's file is not an OMA file (no EA3 header)")
    word = struct.unpack(">I", h[32:36])[0]
    if word >> 24 != 1:
        raise PbpError("the encoder's OMA file is not ATRAC3plus (codec id %d)" % (word >> 24))
    rate = {0: 32000, 1: 44100, 2: 48000}.get((word >> 13) & 7)
    ch = {1: 1, 2: 2}.get((word >> 10) & 7)
    align = (word & 0x3FF) * 8 + 8
    if not ch:
        raise PbpError("the encoder's OMA file has channel layout %d (only mono / stereo)" % ((word >> 10) & 7))
    return data[pos + 96:], align, ch, rate


def normalize_atrac3(raw, samples, delay, loop=True):
    """atracdenc's ATRAC3 RIFF (32-byte fmt, tag 0x270, no fact) + fact (samples, delay) + smpl loop"""
    c = chunks(raw)
    fmt, body = c.get(b"fmt "), c.get(b"data")
    if not fmt or body is None or struct.unpack("<H", fmt[:2])[0] != 0x0270 or len(fmt) < 32:
        raise PbpError("the encoder's file is not an ATRAC3 RIFF file")
    align = struct.unpack("<H", fmt[12:14])[0]
    body = body[:len(body) // align * align]
    coded = len(body) // align * 1024
    samples = min(int(samples), coded - delay - 69)
    if samples <= 0:
        raise PbpError("the encoded music is empty")
    return _riff([(b"fmt ", fmt[:32]), (b"fact", struct.pack("<II", samples, delay))] + _smpl(samples, delay, loop) +
                 [(b"data", body)])


def _smpl(samples, delay, loop):
    if not loop:
        return []
    return [(b"smpl", struct.pack("<9I", 0, 0, 22676, 60, 0, 0, 0, 1, 0) +
             struct.pack("<6I", 0, 0, delay, delay + samples - 1, 0, 0))]


def _riff(parts):
    out = b"WAVE" + b"".join(cid + struct.pack("<I", len(d)) + d + (b"\0" if len(d) & 1 else b"") for cid, d in parts)
    return b"RIFF" + struct.pack("<I", len(out)) + out


def normalize(raw, samples=None, delay=0, loop=True):
    """re-wrap an encoder's ATRAC3plus file (atracdenc: OMA in 0.2.x, RIFF with a 40-byte fmt later) in the
    52-byte-fmt / fact / smpl AT3 layout. samples = the real length (default: the file's fact value, else all coded
    samples); delay = encoder delay in samples."""
    if raw[:4] == b"RIFF":
        c = chunks(raw)
        fmt, body = c.get(b"fmt "), c.get(b"data")
        if not fmt or body is None:
            raise PbpError("the encoder's file has no fmt or data chunk")
        tag, ch, rate, _byte_rate, align = struct.unpack("<HHIIH", fmt[:14])
        if tag != 0xFFFE or len(fmt) < 40 or fmt[24:40] != GUID:
            raise PbpError("the encoder's file is not ATRAC3plus (format tag 0x%04X)" % tag)
        fact = c.get(b"fact")
        if samples is None and fact and len(fact) >= 4:
            samples = struct.unpack("<I", fact[:4])[0]
    else:
        body, align, ch, rate = parse_oma(raw)
    if rate != RATE:
        raise PbpError("the encoder's file is %s Hz, not %d" % (rate, RATE))
    n_frames = len(body) // align
    body = body[:n_frames * align]
    coded = n_frames * FRAME_SAMPLES
    if samples is None:
        samples = coded
    samples = min(int(samples), coded - delay - (DECODER_EXTRA if delay else 0))
    if samples <= 0:
        raise PbpError("the encoded music is empty")
    byte_rate = align * RATE // FRAME_SAMPLES
    mask = 3 if ch == 2 else 4
    fmt52 = struct.pack("<HHIIHHHHI", 0xFFFE, ch, RATE, byte_rate, align, 0, 34, FRAME_SAMPLES, mask) + GUID + \
        struct.pack(">HH", 1 << 8, config_word(align, ch)) + b"\0" * 8
    return _riff([(b"fmt ", fmt52), (b"fact", struct.pack("<II", samples, delay))] + _smpl(samples, delay, loop) +
                 [(b"data", body)])


# ------------------------------------------------------------------------------------------ encoding
def encode(pcm, encoder=None, delay=None, loop=True, work_dir=None, codec="atrac3plus", allow_off_spec=False):
    """pcm (s16le stereo 44.1 kHz) -> (AT3 bytes, report). codec: CODECS key (ATRAC3 is off-spec: allow_off_spec).
    Raises PbpError with a plain reason."""
    cdc = CODECS[codec]
    delay = cdc.delay if delay is None else delay
    enc, why = status(encoder)
    if not enc:
        raise PbpError(why)
    samples = len(pcm) // 4
    if samples < 6144:                  # shortest input the ATRAC tools accept (I: 3 frames)
        raise PbpError("the music is too short (%.2f s); use at least %.0f s" % (samples / float(RATE), MIN_SECONDS))
    own = work_dir is None
    work = tempfile.mkdtemp(prefix="snd0_") if own else work_dir
    try:
        src, dst = os.path.join(work, "snd0_in.wav"), os.path.join(work, "snd0_out" + cdc.ext)
        with open(src, "wb") as f:
            f.write(wav_bytes(pcm + bytes(4 * cdc.pad)))      # silence after the end: the tail is coded too
        if os.path.exists(dst):
            os.remove(dst)
        # OMA: the one ATRAC3plus container every atracdenc release writes (0.2.3 has no RIFF for it, no --container).
        # File names relative to the work folder: atracdenc 0.2.3 (Windows) aborts (stack buffer overrun) on non-ASCII
        # path (measured 2026-10-14: Cyrillic and accented folder names), e.g. a Cyrillic user name in TEMP.
        media.run([enc, "-e", cdc.enc, "-i", os.path.basename(src), "-o", os.path.basename(dst)], what="atracdenc",
                  cwd=work)
        with open(dst, "rb") as f:
            raw = f.read()
    finally:
        if own:
            import shutil
            shutil.rmtree(work, ignore_errors=True)
    if codec == "atrac3plus":
        data = normalize(raw, samples=samples, delay=delay, loop=loop)
    else:
        data = normalize_atrac3(raw, samples, delay, loop)
    found = pat3.check(data)
    stop = blocking(found, allow_off_spec)
    if stop:
        raise PbpError("SND0: the encoded file fails the check: %s" % stop[0].message)
    return data, {"bytes": len(data), "seconds": samples / float(RATE), "kbps": cdc.kbps, "codec": codec,
                  "encoder": enc, "delay": delay, "loop": loop,
                  "container": "RIFF" if raw[:4] == b"RIFF" else "OMA"}


def decode(at3_bytes, ffmpeg=None):
    """AT3 -> s16le stereo PCM through ffmpeg's ATRAC3plus / ATRAC3 decoder (to listen to the real result)."""
    ff = media.find_ffmpeg(ffmpeg)
    if not ff:
        raise PbpError("listening to the encoded music needs ffmpeg")
    with tempfile.TemporaryDirectory(prefix="snd0_dec_") as d:
        p = os.path.join(d, "snd0.at3")
        with open(p, "wb") as f:
            f.write(at3_bytes)
        return media.run([ff, "-v", "error", "-i", p, "-f", "s16le", "-acodec", "pcm_s16le", "-ac", "2", "-ar",
                          str(RATE), "-"]).stdout


def measure_delay(at3_bytes, pcm, ffmpeg=None, max_lag=8192, probe=4096):
    """samples by which the decoded file lags the input (best match of the first loud stretch). Development aid."""
    dec = _samples(decode(at3_bytes, ffmpeg))
    ref = _samples(pcm)
    mono_r = [ref[2 * i] + ref[2 * i + 1] for i in range(len(ref) // 2)]
    mono_d = [dec[2 * i] + dec[2 * i + 1] for i in range(len(dec) // 2)]
    s0 = next((i for i, v in enumerate(mono_r) if abs(v) > 2000), 0)
    seg = mono_r[s0:s0 + probe]
    best, best_lag = None, 0
    for lag in range(0, max_lag, 4):
        d = mono_d[s0 + lag:s0 + lag + len(seg)]
        if len(d) < len(seg):
            break
        err = sum((a - b) * (a - b) for a, b in zip(seg[::8], d[::8]))
        if best is None or err < best:
            best, best_lag = err, lag
    for lag in range(max(0, best_lag - 4), best_lag + 5):
        d = mono_d[s0 + lag:s0 + lag + len(seg)]
        if len(d) < len(seg):
            continue
        err = sum((a - b) * (a - b) for a, b in zip(seg, d))
        if err < best:
            best, best_lag = err, lag
    return best_lag
