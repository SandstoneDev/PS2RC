"""Music from the player's own disc for the menu music (SND0).

Model A, like disc_art.py: the game pack's provider (`games/<pack>/xmb_artwork.py`) knows the game's audio streams;
this module is generic. Provider functions:
    music_list(disc, log, progress) -> [{name, category, seconds, ref}]      progress(done, total)
    music_pcm(disc, ref, start, seconds) -> (s16le interleaved bytes, rate, channels)
The list is cached per disc (out_dir/<key>/music.json); a chosen piece is written as a 44.1 kHz stereo WAV in the
player's cache and then goes through the normal SND0 path (cut, loop, atracdenc).
"""
import array
import json
import os
import sys

from ps2rc.pbp.spec import PbpError

from . import disc_art, media, sound

INDEX = "music.json"
CATEGORIES = ("radio", "cutscene", "beats", "ambience", "adverts", "other")


class Track:
    def __init__(self, name, category, seconds, ref):
        self.name, self.category, self.seconds, self.ref = name, category, float(seconds), ref

    def to_dict(self):
        return dict(self.__dict__)

    def __repr__(self):
        return "Track(%s, %s, %.1f s)" % (self.name, self.category, self.seconds)


def available(pack_dir):
    prov = disc_art.find_provider(pack_dir)
    return bool(prov and hasattr(prov, "music_list") and hasattr(prov, "music_pcm"))


def _open(source):
    from ps2rc.convert import disc as discmod
    try:
        return discmod.open_disc(source)
    except discmod.DiscError as e:
        raise PbpError("cannot read the disc image: %s" % e)


def list_music(source, out_dir, pack_dir, log=None, progress=None):
    """-> [Track] (cached per disc)"""
    prov = disc_art.find_provider(pack_dir)
    if not available(pack_dir):
        return []
    d = os.path.join(out_dir, disc_art.disc_key(source, prov.__file__))
    idx = os.path.join(d, INDEX)
    if os.path.isfile(idx):
        try:
            with open(idx, encoding="utf-8") as f:
                return [Track(**r) for r in json.load(f)]
        except (OSError, ValueError, TypeError):
            pass
    disc = _open(source)
    try:
        rows = prov.music_list(disc, log or (lambda t: None), progress or (lambda done, total: None))
    finally:
        disc.close()
    tracks = [Track(**r) for r in rows]
    os.makedirs(d, exist_ok=True)
    with open(idx + ".tmp", "w", encoding="utf-8") as f:
        json.dump([t.to_dict() for t in tracks], f)
    os.replace(idx + ".tmp", idx)
    return tracks


def _to_44k_stereo(pcm, rate, channels, ffmpeg=None):
    """s16le -> 44.1 kHz stereo s16le (ffmpeg when present, else linear interpolation)"""
    if rate == sound.RATE and channels == 2:
        return pcm
    ff = media.find_ffmpeg(ffmpeg)
    if ff:
        return media.run([ff, "-v", "error", "-f", "s16le", "-ar", str(rate), "-ac", str(channels), "-i", "-",
                          "-f", "s16le", "-ar", str(sound.RATE), "-ac", "2", "-"], stdin=pcm).stdout
    a = array.array("h", pcm)
    if sys.byteorder == "big":
        a.byteswap()
    frames = len(a) // channels
    n_out = int(frames * sound.RATE / float(rate))
    out = array.array("h", bytes(4 * n_out))
    for i in range(n_out):
        x = i * rate / float(sound.RATE)
        j = min(int(x), frames - 2)
        t = x - j
        for c in range(2):
            src = c if channels > 1 else 0
            v0, v1 = a[j * channels + src], a[(j + 1) * channels + src]
            out[2 * i + c] = int(v0 + (v1 - v0) * t)
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


def extract(source, pack_dir, track, start, seconds, out_wav, ffmpeg=None):
    """write `seconds` of the track from `start` as a 44.1 kHz stereo WAV -> out_wav"""
    prov = disc_art.find_provider(pack_dir)
    if not available(pack_dir):
        raise PbpError("this game pack offers no music from the disc")
    start = max(0.0, min(float(start), max(0.0, track.seconds - 1.0)))
    seconds = max(1.0, min(float(seconds), track.seconds - start))
    disc = _open(source)
    try:
        pcm, rate, channels = prov.music_pcm(disc, track.ref, start, seconds)
    finally:
        disc.close()
    pcm = _to_44k_stereo(pcm, rate, channels, ffmpeg)
    os.makedirs(os.path.dirname(out_wav), exist_ok=True)
    with open(out_wav + ".tmp", "wb") as f:
        f.write(sound.wav_bytes(pcm))
    os.replace(out_wav + ".tmp", out_wav)
    return out_wav
