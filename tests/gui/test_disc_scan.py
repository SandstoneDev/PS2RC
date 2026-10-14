"""ps2rc.xmb.disc_scan / ps2tex: the generic provider of pictures and music from the player's own disc, on synthetic
discs made here (texture dictionaries, a "VER2" archive, indexed interleaved PS-ADPCM tracks, a VAGp file). No game
data. The optional test at the end reads a real disc image (PS2RC_TEST_ISO + PS2RC_TEST_PACK), read-only."""
import os
import struct
import sys
import wave

import pytest

from ps2rc.xmb import disc_art, disc_music, disc_scan, ps2tex

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "convert"))
from isobuild import build_iso, ps2_files  # noqa: E402


# ---------------------------------------------------------------------------------------------- synthetic writers
def chunk(t, body):
    return struct.pack("<III", t, len(body), 0) + body


def upload(w, h, data):
    """A+D (TRXPOS, TRXREG, TRXDIR) + IMAGE tag + data"""
    nq = (len(data) + 15) // 16
    out = struct.pack("<QQ", 3 | (1 << 60), 0xE)
    out += struct.pack("<QQ", 0, 0x51) + struct.pack("<QQ", (h << 32) | w, 0x52) + struct.pack("<QQ", 0, 0x53)
    out += struct.pack("<QQ", nq | (2 << 58), 0)
    return out + data.ljust(16 * nq, b"\0")


def texture(name, w, h, depth, fmt, pix, pal=b""):
    hdr = struct.pack("<iiiHhQIIQQIIII", w, h, depth, fmt, 2, 0, 0, 0, 0, 0, len(pix), len(pal),
                      len(pix) + len(pal), 0)
    raster = chunk(ps2tex.STRUCT, chunk(ps2tex.STRUCT, hdr) + chunk(ps2tex.STRUCT, pix + pal))
    body = (chunk(ps2tex.STRUCT, b"PS2\0" + struct.pack("<I", 0x1102)) +
            chunk(ps2tex.STRING, name.encode().ljust(32, b"\0")) + chunk(ps2tex.STRING, bytes(4)) + raster +
            chunk(ps2tex.EXTENSION, b""))
    return chunk(ps2tex.TEXTURE, body)


def dictionary(texs):
    return chunk(ps2tex.DICTIONARY, chunk(ps2tex.STRUCT, struct.pack("<HH", len(texs), 6)) + b"".join(texs) +
                 chunk(ps2tex.EXTENSION, b""))


def swizzle8_pos(x, y, w):
    """the PSMT8-in-PSMCT32 position as the usual PS2 tools write it (independent of ps2tex)"""
    block = (y & ~0xF) * w + (x & ~0xF) * 2
    swap = (((y + 2) >> 2) & 1) * 4
    posy = (((y & ~3) >> 1) + (y & 1)) & 7
    return block + posy * w * 2 + ((x + swap) & 7) * 4 + ((y >> 1) & 1) + ((x >> 2) & 2)


def pal8(colours, swizzled=False, w=0, h=0, idx=None):
    """(raster format, pixel block, palette block) of a PAL8 texture; colours: 256 x (r, g, b, a 0..0x80)"""
    stored = bytearray()
    for j in range(256):
        stored += bytes(colours[ps2tex.CSM1[j]])
    if swizzled:
        data = bytearray(w * h)
        for y in range(h):
            for x in range(w):
                data[swizzle8_pos(x, y, w)] = idx[y * w + x]
        pix = upload(w // 2, h // 2, bytes(data))
    else:
        pix = upload(w, h, bytes(idx))
    return ps2tex.FMT_PAL8 | ps2tex.FMT_8888, pix, upload(16, 16, bytes(stored))


def grey(a=0x80):
    return [(k, 255 - k, k // 2, a) for k in range(256)]


def mono():
    return [(k, k, k, 0x80) for k in range(256)]


def full8(name, w=512, h=512, k=0, colours=None):
    idx = bytes(((x + 3 * y + k) & 255) for y in range(h) for x in range(w))
    fmt, pix, pal = pal8(colours or grey(), w=w, h=h, idx=idx)
    return texture(name, w, h, 8, fmt, pix, pal)


def pal4_full(name, w=512, h=256):
    pix = bytes((((2 * x) & 15) | (((2 * x + 1) & 15) << 4)) for y in range(h) for x in range(w // 2))
    pal = b"".join(bytes((16 * k, 0, 255 - 16 * k, 0x80)) for k in range(16))
    return texture(name, w, h, 4, ps2tex.FMT_PAL4 | ps2tex.FMT_8888, upload(w, h, pix), upload(8, 2, pal))


def rgba32(name, w, h, alpha):
    pix = b"".join(bytes((x & 255, y & 255, 7, alpha(x, y))) for y in range(h) for x in range(w))
    return texture(name, w, h, 32, ps2tex.FMT_8888, upload(w, h, pix))


def archive(entries):
    """a "VER2" archive: {name: bytes} -> bytes (entries sector-aligned)"""
    n = len(entries)
    first = (8 + 32 * n + 2047) // 2048
    head, body, at = bytearray(b"VER2" + struct.pack("<I", n)), bytearray(), first
    for name, data in entries.items():
        sec = (len(data) + 2047) // 2048
        head += struct.pack("<IHH", at, sec, 0) + name.encode().ljust(24, b"\0")
        body += data.ljust(2048 * sec, b"\0")
        at += sec
    return bytes(head.ljust(2048 * first, b"\0") + body)


# ---------------------------------------------------------------------------------------------- pictures
def test_decoder_swizzled_pal8_matches_the_usual_layout(pillow):
    w, h = 64, 32
    idx = [(x * 7 + y * 13) & 255 for y in range(h) for x in range(w)]
    fmt, pix, pal = pal8(grey(0x40), True, w, h, idx)
    blob = dictionary([texture("t", w, h, 8, fmt, pix, pal)])
    assert ps2tex.is_dictionary(blob[:16], len(blob))
    t, = ps2tex.parse(blob)
    assert t.level0()[4] is True                                   # half-size upload = pre-swizzled
    im = ps2tex.decode(t)
    want = grey(0x40)
    for (x, y) in ((0, 0), (5, 1), (17, 3), (63, 31), (40, 22)):
        r, g, b, a = want[idx[y * w + x]]
        assert im.getpixel((x, y)) == (r, g, b, 0x80)              # PS2 alpha 0x40 -> 0x80


def _picture_disc(tmp_path, inside_archive=False):
    screens = dictionary([full8("text", colours=mono()), full8("screen1", k=1), full8("screen2", k=2)])
    files = ps2_files()
    if inside_archive:
        files["DATA/PACK.ARC"] = archive({"a.bin": b"x" * 5000, "screens.bin": screens})
    else:
        files["MENU/SCREENS.BIN"] = screens
    files["MENU/TITLE.BIN"] = dictionary([pal4_full("title")])
    files["MENU/PARTS.BIN"] = dictionary([
        rgba32("banner", 256, 64, lambda x, y: 0x80 if 8 < y < 56 else 0),
        rgba32("tiny", 64, 64, lambda x, y: 0x80),
        rgba32("panel", 256, 128, lambda x, y: 0x80)])
    files["MENU/NOT_A_DICT.BIN"] = b"\x16\0\0\0" + bytes(100)
    iso = tmp_path / "disc.iso"
    iso.write_bytes(build_iso(files))
    return str(iso)


def test_generic_pictures_from_a_synthetic_disc(tmp_path, pillow):
    iso = _picture_disc(tmp_path)
    pack = tmp_path / "pack"
    pack.mkdir()
    prov = disc_art.find_provider(str(pack))
    assert isinstance(prov, disc_scan.Provider) and disc_art.find_provider(str(pack)) is prov
    assert disc_art.find_provider(str(pack), generic=False) is None and disc_art.find_provider(None) is None
    cache = tmp_path / "cache"
    seen = []
    arts = disc_art.extract(iso, str(cache), str(pack), on_item=seen.append)
    got = [(a.category, a.name, a.width, a.height) for a in arts]
    assert sorted(got[:3]) == [("loading", "screen1", 640, 480), ("loading", "screen2", 640, 480),
                               ("loading", "text", 640, 480)]
    assert got[2][1] == "text"                                    # grey: the least colourful last
    assert got[3:] == [("title", "title", 640, 480), ("background", "panel", 256, 128), ("logo", "banner", 256, 64)]
    assert seen == arts and all(os.path.dirname(a.path).startswith(str(cache)) for a in arts)
    im = pillow.open(arts[0].path)
    assert im.size == (640, 480) and im.mode == "RGB"
    assert pillow.open(arts[-1].path).mode == "RGBA"
    again = disc_art.extract(iso, str(cache), str(pack))           # cached
    assert [a.path for a in again] == [a.path for a in arts]
    s = disc_art.suggest(arts, str(pack))
    assert s["pic1"] == (arts[0], None) and arts[0].name != "text" and s["icon0"] == (arts[0], None)


def test_generic_pictures_inside_an_archive(tmp_path, pillow):
    """no loose full-screen picture: the archives' dictionaries are read (their full-screen pictures only)"""
    iso = _picture_disc(tmp_path, inside_archive=True)
    pack = tmp_path / "pack"
    pack.mkdir()
    arts = disc_art.extract(iso, str(tmp_path / "c"), str(pack))
    got = [(a.category, a.name, a.source) for a in arts]
    assert sorted(got[:3]) == [("loading", n, "screens.bin") for n in ("screen1", "screen2", "text")]
    assert got[3] == ("title", "title", "TITLE.BIN")


# ---------------------------------------------------------------------------------------------- music
FACTS = """
[xmb.music]
names = "SND/NAMES.DAT"
names_record = 16
names_len = 12
index = "SND/TRACKS.DAT"
index_record = "<B3xII"
container = "SND/{name}.STR"
header = 0x100
streams = 0xC0
streams_max = 8
stream_count = 0xF0
period = 8448
period_rate = 49500
default_category = "radio"
"""


def _frames(n, nib):
    """n PS-ADPCM frames, filter 0 / shift 0, every sample of frame k = nib(k) * 4096"""
    out = bytearray()
    for k in range(n):
        v = nib(k) & 15
        out += bytes((0, 0)) + bytes([v | (v << 4)]) * 14
    return bytes(out)


def _track(seconds):
    rate, slow = 24000, 750
    nfr = int(seconds * rate / 28)
    left = _frames(nfr, lambda k: k % 7)
    right = _frames(nfr, lambda k: -1 - (k % 3))
    blk, sblk = rate * 8448 // 49500, slow * 8448 // 49500
    assert (blk, sblk) == (4096, 128)
    hdr = bytearray(0x100)
    for j, (b, r) in enumerate(((len(left), rate), (len(right), rate), (500, slow), (500, slow))):
        struct.pack_into("<II", hdr, 0xC0 + 8 * j, b, r)
    struct.pack_into("<H", hdr, 0xF0, 4)
    data = bytearray()
    for p in range((len(left) + blk - 1) // blk):
        data += left[p * blk:(p + 1) * blk].ljust(blk, b"\0") + right[p * blk:(p + 1) * blk].ljust(blk, b"\0")
        data += b"\xAA" * (2 * sblk)                                # the slow sub-streams: not audio
    return bytes(hdr) + bytes(data)


def _music_disc(tmp_path):
    t1, t2, short = _track(25.0), _track(31.0), _track(5.0)
    files = ps2_files()
    files["SND/NAMES.DAT"] = b"MIX".ljust(12, b"\0") + bytes(4) + b"CUTSCENE".ljust(12, b"\0") + bytes(4)
    files["SND/MIX.STR"] = t1 + short
    files["SND/CUTSCENE.STR"] = t2
    files["SND/TRACKS.DAT"] = (struct.pack("<B3xII", 0, 0, len(t1)) + struct.pack("<B3xII", 0, len(t1), len(short)) +
                               struct.pack("<B3xII", 1, 0, len(t2)))
    vag = _frames(int(22.0 * 8000 / 28), lambda k: 2)
    files["SND/JINGLE.VAG"] = b"VAGp" + struct.pack(">IIII", 3, 0, len(vag), 8000) + bytes(28) + vag
    iso = tmp_path / "music.iso"
    iso.write_bytes(build_iso(files))
    pack = tmp_path / "mpack"
    pack.mkdir()
    (pack / "pack.toml").write_text("format = 1\n" + FACTS, encoding="utf-8")
    return str(iso), str(pack)


def test_generic_music_from_a_synthetic_disc(tmp_path, no_ffmpeg):
    iso, pack = _music_disc(tmp_path)
    assert disc_music.available(pack)
    tracks = disc_music.list_music(iso, str(tmp_path / "c"), pack)
    assert [(t.name, t.category, t.seconds, t.ref) for t in tracks] == [
        ("MIX 1", "radio", 25.0, 0), ("CUTSCENE 1", "cutscene", 31.0, 2), ("JINGLE.VAG", "other", 22.0,
                                                                            "vag:SND/JINGLE.VAG")]
    prov = disc_art.find_provider(pack)
    from ps2rc.convert import disc as discmod
    d = discmod.open_disc(iso)
    try:
        pcm, rate, ch = prov.music_pcm(d, 0, 5.0, 2.0)
        assert (rate, ch) == (24000, 2)
        s = struct.unpack("<%dh" % (len(pcm) // 2), pcm)
        first = int(5.0 * 24000 / 28)                                # first frame read
        for n in (0, 27, 28, 1000, 4000, len(s) // 2 - 1):           # crosses several periods
            k = first + n // 28
            assert s[2 * n] == (k % 7) * 4096 and s[2 * n + 1] == (-1 - (k % 3)) * 4096, n
        pcm, rate, ch = prov.music_pcm(d, "vag:SND/JINGLE.VAG", 1.0, 1.0)
        assert (rate, ch) == (8000, 1) and set(struct.unpack("<%dh" % (len(pcm) // 2), pcm)) == {8192}
    finally:
        d.close()
    out = disc_music.extract(iso, pack, tracks[1], 3.0, 4.0, str(tmp_path / "m.wav"))
    with wave.open(out) as w:
        assert (w.getframerate(), w.getnchannels()) == (44100, 2) and abs(w.getnframes() - 4 * 44100) < 50


def test_generic_music_without_facts_lists_vag_files_only(tmp_path):
    iso, _pack = _music_disc(tmp_path)
    bare = tmp_path / "bare"
    bare.mkdir()
    tracks = disc_music.list_music(iso, str(tmp_path / "c2"), str(bare))
    assert [t.name for t in tracks] == ["JINGLE.VAG"]


# ---------------------------------------------------------------------------------------------- the player's disc
ISO, PACK = os.environ.get("PS2RC_TEST_ISO", ""), os.environ.get("PS2RC_TEST_PACK", "")


@pytest.mark.needs_game
@pytest.mark.skipif(not os.path.isfile(ISO) or not PACK, reason="PS2RC_TEST_ISO (a disc image) + PS2RC_TEST_PACK")
def test_generic_provider_on_the_players_disc(tmp_path, pillow):
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    pack = os.path.join(root, "games", PACK)
    prov = disc_scan.Provider(pack)
    from ps2rc.convert import disc as discmod
    d = discmod.open_disc(ISO)
    try:
        arts = list(prov.artwork(d, str(tmp_path), lambda t: None))
        cats = {}
        for a in arts:
            cats[a["category"]] = cats.get(a["category"], 0) + 1
        assert cats.get("loading", 0) >= 10 and cats.get("title", 0) >= 1 and len(arts) >= 20
        tracks = prov.music_list(d, lambda t: None, lambda a, b: None)
        if prov.facts.get("music"):
            assert len(tracks) > 100 and all(t["seconds"] >= 20 for t in tracks)
            t = max(tracks, key=lambda x: x["seconds"])
            pcm, _rate, _ch = prov.music_pcm(d, t["ref"], 10.0, 2.0)
            assert max(abs(v) for v in struct.unpack("<%dh" % (len(pcm) // 2), pcm)[::50]) > 1000
    finally:
        d.close()
