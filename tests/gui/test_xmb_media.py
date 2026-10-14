"""ps2rc.xmb: title rules, picture crop boxes, the SND0 layout and encoder wrapper, the ICON1 clip path, the size
budget and the EBOOT slots it all ends up in."""
import io
import os
import struct
import wave

import pytest

from gui_helpers import tone_pcm, atracdenc_like, oma_like, onset_pcm
from ps2rc.pbp import Pbp, make_sfo, apply, check, blocking, at3 as pat3, icon1 as picon1, png, placeholder
from ps2rc.pbp.spec import PbpError, MEDIA_BUDGET, TITLE_MAX
from ps2rc.xmb import title, picture, sound, clip, project, media


# ------------------------------------------------------------------------------------------ title
def test_title_rules():
    assert title.check_title("My game") == []
    assert title.check_title("")[0][:2] == ("error", "empty")
    assert ("error", "control") in [c[:2] for c in title.check_title("a\nb")]
    long = "x" * (TITLE_MAX + 1)
    assert ("error", "too_long", "%d/%d" % (TITLE_MAX + 1, TITLE_MAX)) in title.check_title(long)
    assert not [c for c in title.check_title("x" * TITLE_MAX) if c[0] == "error"]
    # bytes, not characters: 64 two-byte letters = 128 bytes
    two = chr(0x416)
    assert len(two.encode("utf-8")) == 2
    assert "too_long" in [c[1] for c in title.check_title(two * 64)]
    assert "too_long" not in [c[1] for c in title.check_title(two * 63)]
    assert [c for c in title.check_title("A" + two) if c[1] == "chars"]
    assert title.clean_title("  a \t b\r\n c ") == "a b c"
    t = title.fit_title(two * 100)
    assert len(t.encode("utf-8")) <= TITLE_MAX and t == two * 63


# ------------------------------------------------------------------------------------------ pictures
def test_default_and_clamped_boxes():
    assert picture.default_box(1600, 1000, 480, 272) == (0, 47, 1600, 953)        # taller: top/bottom cut
    assert picture.default_box(2000, 800, 480, 272) == (294, 0, 1706, 800)        # wider: left/right cut
    x0, y0, x1, y1 = picture.default_box(100, 400, 144, 80)
    assert (x0, x1) == (0, 100) and abs((y1 - y0) - 100 * 80 / 144) <= 1
    b = picture.clamp_box((-50, -50, 2000, 2000), 1600, 1000, 480, 272)
    assert b[0] >= 0 and b[1] >= 0 and b[2] <= 1600 and b[3] <= 1000
    assert abs((b[2] - b[0]) / (b[3] - b[1]) - 480 / 272) < 0.01


def _picture(pillow, w, h, mode="RGB", color=(200, 30, 30)):
    im = pillow.new(mode, (w, h), color + ((128,) if mode == "RGBA" else ()))
    im.paste((10, 200, 10) + ((255,) if mode == "RGBA" else ()), (0, 0, w // 4, h))   # green left quarter
    buf = io.BytesIO()
    im.save(buf, "PNG")
    return buf.getvalue()


def test_render_with_box_sizes_modes(pillow):
    src = _picture(pillow, 800, 600)
    for key, (w, h, mode) in (("icon0", (144, 80, "RGB")), ("pic1", (480, 272, "RGB")),
                              ("pic0", (310, 180, "RGBA"))):
        data, notes = picture.render(src, key, box=(0, 0, 400, 400 * h // w))
        i = png.info(data)
        assert (i["width"], i["height"], i["mode"]) == (w, h, mode), key
        assert notes
    # the box decides what is kept: the left quarter (green) only
    data, _ = picture.render(src, "icon0", box=(0, 0, 180, 100))
    px = pillow.open(io.BytesIO(data)).convert("RGB").getpixel((72, 40))
    assert px[1] > 150 and px[0] < 60
    # without a box: centred cover crop, the green quarter is partly cut
    data, _ = picture.render(src, "icon0")
    assert pillow.open(io.BytesIO(data)).convert("RGB").getpixel((72, 40))[0] > 150


def test_exact_png_kept_byte_for_byte():
    exact = placeholder.icon0("T")
    data, notes = picture.render(exact, "icon0")
    assert data == exact and "as is" in notes[0]


def test_pic0_keeps_transparency(pillow):
    src = _picture(pillow, 620, 360, "RGBA")
    data, _ = picture.render(src, "pic0", box=(0, 0, 620, 360))
    assert pillow.open(io.BytesIO(data)).getpixel((300, 90))[3] == 128


# ------------------------------------------------------------------------------------------ SND0
def test_config_word():
    assert sound.config_word(744, 2) == 0x285C            # the value in the commonly used 128 kbit/s files
    assert sound.config_word(2048, 2) == 0x28FF
    assert sound.config_word(2048, 1) == 0x24FF
    with pytest.raises(PbpError):
        sound.config_word(1001, 2)


def test_normalize_layout_and_ppsspp_reader_rules():
    n = 44100 * 3
    raw = atracdenc_like(n)
    assert len(sound.chunks(raw)[b"fmt "]) == 40          # what atracdenc writes (a 52-byte reader refuses it)
    out = sound.normalize(raw, samples=n, delay=0)
    c = sound.chunks(out)
    fmt = c[b"fmt "]
    assert len(fmt) == 52                                  # WAVE_FORMAT_EXTENSIBLE with the codec bytes
    tag, ch, rate, byte_rate, align, bits, cb, spb, mask = struct.unpack("<HHIIHHHHI", fmt[:24])
    assert (tag, ch, rate, byte_rate, align, cb, spb, mask) == (0xFFFE, 2, 44100, 44100, 2048, 34, 2048, 3)
    assert fmt[24:40] == sound.GUID and fmt[40:44] == bytes([1, 0, 0x28, 0xFF]) and fmt[44:] == b"\0" * 8
    assert struct.unpack("<II", c[b"fact"]) == (n, 0)
    smpl = c[b"smpl"]
    assert len(smpl) == 60 and struct.unpack("<I", smpl[28:32])[0] == 1
    start, end = struct.unpack("<II", smpl[44:52])
    assert (start, end) == (0, n - 1)
    assert len(c[b"data"]) % 2048 == 0 and len(c[b"data"]) // 2048 * 2048 >= n
    # reader rules (PPSSPP Core/Util/AtracTrack.cpp): loop start < end; loop end <= end sample + offsets
    assert start < end <= n + 0x170
    a = pat3.parse(out)
    assert a["codec"] == "ATRAC3plus" and a["loop"] == (0, n - 1) and a["riff_size_ok"]
    assert not blocking(pat3.check(out))
    d = sound.chunks(sound.normalize(raw, samples=n, delay=1024, loop=False))
    assert struct.unpack("<II", d[b"fact"]) == (n, 1024) and b"smpl" not in d


def test_budget_math():
    assert sound.BYTES_PER_SECOND == 44100
    for s in (1.0, 5.0, 8.5):
        assert sound.size_for(s) <= MEDIA_BUDGET or s > sound.max_seconds(MEDIA_BUDGET)
        assert sound.max_seconds(sound.size_for(s)) >= s - 2048 / 44100.0
    assert 11.0 < sound.max_seconds(MEDIA_BUDGET) < 11.4          # about 11 s of music at most
    assert sound.max_seconds(1000) == 0.0


def test_shape_loop_is_seamless_and_fades():
    pcm = tone_pcm(1.0, freq=441.0)
    looped = sound.shape(pcm, loop=True, xfade=0.2)
    a = sound._samples(looped)
    assert len(a) // 2 == 44100 - int(0.2 * 44100)
    # the step from the last sample to the first one is as small as a normal step of the tone
    step = abs(a[-2] - a[0])
    normal = max(abs(a[2 * i] - a[2 * i + 2]) for i in range(1000, 1100))
    assert step <= normal + 2
    faded = sound._samples(sound.shape(pcm, loop=False, fade_in=0.1, fade_out=0.1))
    assert faded[0] == 0 and abs(faded[-2]) <= 1


def test_read_pcm_from_wav_without_ffmpeg(tmp_path, no_ffmpeg):
    p = tmp_path / "m.wav"
    with wave.open(str(p), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(b"\x10\x00" * 44100 * 2)
    pcm = sound.read_pcm(str(p), start=0.5, seconds=1.0)
    assert len(pcm) == 44100 * 4                                       # stereo now
    with pytest.raises(PbpError, match="ffmpeg"):
        sound.read_pcm(str(tmp_path / "x.mp3"))


def test_encode_runs_the_encoder_and_rewraps(fake_encoder):
    pcm = tone_pcm(2.0)
    data, rep = sound.encode(pcm)
    assert rep["container"] == "OMA" and rep["bytes"] == len(data)
    assert len(sound.chunks(data)[b"fmt "]) == 52 and not blocking(pat3.check(data))
    assert abs(rep["seconds"] - 2.0) < 0.01


def test_oma_from_atracdenc_rewrapped():
    n = 44100 * 2
    for tag in (True, False):
        raw = oma_like(n, tag=tag)
        frames, align, ch, rate = sound.parse_oma(raw)
        assert (align, ch, rate) == (2048, 2, 44100) and len(frames) == n // 2048 * 2048
        out = sound.normalize(raw, samples=n, delay=0)
        c = sound.chunks(out)
        assert len(c[b"fmt "]) == 52 and c[b"data"] == frames
        assert struct.unpack("<II", c[b"fact"]) == (min(n, n // 2048 * 2048), 0)
        assert not blocking(pat3.check(out))
    with pytest.raises(PbpError, match="OMA"):
        sound.parse_oma(b"x" * 200)
    bad = bytearray(oma_like(n, tag=False))
    bad[32] = 0                                            # codec id ATRAC3, not plus
    with pytest.raises(PbpError, match="ATRAC3plus"):
        sound.parse_oma(bytes(bad))


def test_encode_without_encoder_says_why(monkeypatch):
    monkeypatch.setattr(sound, "find_encoder", lambda given=None: None)
    with pytest.raises(PbpError, match="atracdenc"):
        sound.encode(tone_pcm(1.0))
    assert sound.status()[0] is None or os.path.isfile(sound.status()[0])


@pytest.mark.skipif(not media.find_tool("atracdenc", None, "PS2RC_ATRACDENC"), reason="no atracdenc")
def test_real_atracdenc_round_trip(ffmpeg):
    pcm = onset_pcm(3.0)
    data, rep = sound.encode(pcm)
    assert not blocking(pat3.check(data)) and rep["container"] in ("OMA", "RIFF")
    n = len(pcm) // 4
    a = pat3.parse(data)
    assert a["samples"] == n and a["loop"] == (sound.ENC_DELAY, sound.ENC_DELAY + n - 1)
    assert len(data) <= sound.size_for(n / 44100.0)       # the budget shown before encoding holds
    dec = sound.decode(data)
    lag = sound.measure_delay(data, pcm)
    assert lag == sound.ENC_DELAY + sound.DECODER_EXTRA    # the measured atracdenc 0.2.3 delay
    assert len(dec) // 4 >= lag + n                        # the whole piece, tail included, is in the file


# ------------------------------------------------------------------------------------------ ICON1
def _gif(pillow, path, n=6, ms=100):
    frames = [pillow.new("RGB", (160, 120), (40 * i, 255 - 40 * i, 90)) for i in range(n)]
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=ms, loop=0)


def test_gif_frames_without_ffmpeg(tmp_path, pillow, no_ffmpeg):
    g = str(tmp_path / "a.gif")
    _gif(pillow, g)
    fr = clip.frames(g, start=0.0, seconds=1.0)
    assert len(fr) == 30 and all(len(f) == 144 * 80 * 3 for f in fr)
    assert fr[0] != fr[-1]                                  # it animates
    fr2 = clip.frames(g, start=0.3, seconds=0.5)            # starts in the 4th GIF frame
    assert fr2[0] == clip.frames(g, start=0.3, seconds=0.1)[0] and fr2[0] != fr[0]
    with pytest.raises(PbpError, match="ffmpeg"):
        clip.make_icon1(g, seconds=1.0)


def test_icon1_from_clip_fits_budget(tmp_path, pillow, ffmpeg):
    g = str(tmp_path / "a.gif")
    _gif(pillow, g, n=20, ms=50)
    info = media.probe(g)
    assert info["video"] and info["width"] == 160
    fr = clip.frames(g, 0.0, 2.0)
    assert 55 <= len(fr) <= 61
    pmf, rep = clip.make_icon1(g, 0.0, 2.0, budget=60000)
    assert len(pmf) <= 60000 and rep["entry_points"] >= 1
    assert not blocking(picon1.check(pmf)) and pmf[:8] == b"PSMF0012"
    png_bytes = clip.still(g, 0.0)
    assert png.is_png(png_bytes)


# ------------------------------------------------------------------------------------------ project -> EBOOT
def test_project_build_cache_errors_and_options(tmp_path, pillow, fake_encoder, no_ffmpeg):
    pic = tmp_path / "p.png"
    pic.write_bytes(_picture(pillow, 900, 500))
    wav = tmp_path / "m.wav"
    with wave.open(str(wav), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(tone_pcm(4.0))
    p = project.Project("Test")
    p.set_file("pic1", str(pic))
    p.set_file("pic0", str(pic), box=[0, 0, 310, 180])
    p.set_file("snd0", str(wav), seconds=3.0)
    p.set_file("icon1", str(tmp_path / "missing.mp4"))
    b = p.build(str(tmp_path / "w"))
    assert set(b.files) == {"icon0", "pic0", "pic1", "snd0"}           # icon0 = default
    assert "icon1" in b.errors and "snd0" not in b.errors
    used, limit, state = b.budget()
    assert used == b.sizes["snd0"] and limit == MEDIA_BUDGET and state == "ok"
    mtimes = {k: os.path.getmtime(v) for k, v in b.files.items()}
    b2 = p.build(str(tmp_path / "w"))
    assert b2.files == b.files and {k: os.path.getmtime(v) for k, v in b2.files.items()} == mtimes
    assert any("cached" in n for n in b2.notes)
    # too long for the budget: a plain refusal, nothing written
    p.slots["snd0"]["seconds"] = 40.0                 # ATRAC3 132k (default): ~30 s fit
    b3 = p.build(str(tmp_path / "w"))
    assert "snd0" in b3.errors and "fit" in b3.errors["snd0"]
    # JSON round trip
    q = project.Project.from_dict(p.to_dict())
    assert q.slots == p.slots and q.title == "Test"
    # into an EBOOT: every slot byte for byte, the pspsdk slot order, nothing blocking
    eb = Pbp()
    eb["PARAM.SFO"] = make_sfo("Test").build()
    eb["DATA.PSP"] = b"\x7fELF" + b"\0" * 60
    opts = b.options()
    apply(eb, icon0=opts["icon0"], pic0=opts["pic0"], pic1=opts["pic1"], snd0=opts["snd0"])
    raw = eb.build()
    assert raw[:4] == b"\0PBP"
    offs = struct.unpack("<8I", raw[8:40])
    assert offs[0] == 40 and list(offs) == sorted(offs)
    back = Pbp.parse(raw)
    for key, slot in (("icon0", "ICON0.PNG"), ("pic0", "PIC0.PNG"), ("pic1", "PIC1.PNG"), ("snd0", "SND0.AT3")):
        assert back[slot] == open(opts[key], "rb").read(), slot
    assert back["ICON1.PMF"] == b""
    assert offs[2] == offs[3]                                           # empty ICON1: zero size
    assert not blocking(check(back))


def test_default_icon1_needs_ffmpeg(tmp_path, no_ffmpeg):
    p = project.Project("T")
    p.set_mode("icon1", "default")
    b = p.build(str(tmp_path))
    assert "icon1" in b.errors and "ffmpeg" in b.errors["icon1"]
    with pytest.raises(PbpError):
        p.set_mode("snd0", "default")


def test_music_is_made_first_and_the_icon_gets_the_rest(tmp_path, fake_encoder, monkeypatch, no_ffmpeg):
    wav = tmp_path / "m.wav"
    with wave.open(str(wav), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(44100)
        w.writeframes(tone_pcm(5.0))
    seen = {}

    def fake_icon1(src, start=0.0, seconds=3.0, box=None, budget=MEDIA_BUDGET, ffmpeg=None, epmap=True,
                   frames_given=None):
        seen["budget"] = budget
        return b"PSMF0012" + b"\0" * 100, {"seconds": 1.0, "kbps": 100, "bytes": 108}
    monkeypatch.setattr(clip, "make_icon1", fake_icon1)
    clipf = tmp_path / "c.mp4"
    clipf.write_bytes(b"x")
    p = project.Project("T")
    p.set_file("snd0", str(wav), seconds=4.0)
    p.set_file("icon1", str(clipf))
    b = p.build(str(tmp_path / "w"))
    assert "snd0" in b.sizes and seen["budget"] == MEDIA_BUDGET - b.sizes["snd0"]


def test_png_metadata_stripped(pillow):
    from PIL import PngImagePlugin
    im = pillow.new("RGB", (144, 80), (10, 20, 30))
    info = PngImagePlugin.PngInfo()
    info.add_text("Comment", "x" * 500)
    buf = io.BytesIO()
    im.save(buf, "PNG", pnginfo=info)
    raw = buf.getvalue()
    assert b"tEXt" in raw
    data, notes = picture.render(raw, "icon0")
    assert b"tEXt" not in data and "metadata removed" in notes[-1]
    assert pillow.open(io.BytesIO(data)).convert("RGB").tobytes() == im.tobytes()     # the picture is unchanged
    assert png.info(data)["mode"] == "RGB"


def _atrac3_riff(samples, align=384, js=0):
    """atracdenc 0.2.3's ATRAC3 RIFF: 32-byte fmt (tag 0x270, 14 extra bytes), no fact, data"""
    frames = samples // 1024
    fmt = struct.pack("<HHIIHHH", 0x270, 2, 44100, align * 44100 // 1024, align, 0, 14) + \
        struct.pack("<HIHHHH", 1, 0x1000, js, js, 1, 0)
    body = b"WAVE" + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", frames * align) + \
        b"\x11" * (frames * align)
    return b"RIFF" + struct.pack("<I", len(body)) + body


def test_atrac3_rewrap_is_valid_and_only_a_warning():
    n = 44100 * 2
    raw = _atrac3_riff(n + 4096)
    out = sound.normalize_atrac3(raw, n, sound.CODECS["atrac3"].delay)
    c = sound.chunks(out)
    assert len(c[b"fmt "]) == 32 and struct.unpack("<II", c[b"fact"]) == (n, 1247)
    a = pat3.parse(out)
    assert a["codec"] == "ATRAC3" and a["loop"] == (1247, 1247 + n - 1)
    found = pat3.check(out)
    assert not blocking(found) and any(i.level == "warn" and "PSP-3000" in i.message for i in found)  # HW-tested
    for name, kbps, longest in (("atrac3plus", 352.8, 11), ("atrac3", 132.3, 30), ("atrac3_lp4", 66.2, 60)):
        cd = sound.CODECS[name]
        assert abs(cd.kbps - kbps) < 0.2 and longest - 2 < sound.max_seconds(500000, name) < longest + 1


@pytest.mark.skipif(not media.find_tool("atracdenc", None, "PS2RC_ATRACDENC"), reason="no atracdenc")
@pytest.mark.parametrize("codec", ["atrac3", "atrac3_lp4"])
def test_real_atracdenc_atrac3(ffmpeg, codec):
    pcm = onset_pcm(3.0)
    data, rep = sound.encode(pcm, codec=codec)            # played by a PSP-3000's menu: no longer refused
    cd = sound.CODECS[codec]
    assert pat3.parse(data)["samples"] == len(pcm) // 4 and len(data) <= sound.size_for(3.0, codec)
    assert sound.measure_delay(data, pcm) == cd.delay + cd.extra


ISO, PACK = os.environ.get("PS2RC_TEST_ISO", ""), os.environ.get("PS2RC_TEST_PACK", "")


@pytest.mark.needs_game
@pytest.mark.skipif(not os.path.isfile(ISO) or not PACK, reason="PS2RC_TEST_ISO (a disc image) + PS2RC_TEST_PACK")
def test_disc_art_from_the_players_disc(tmp_path, pillow):
    from ps2rc.xmb import disc_art
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    pack = os.path.join(root, "games", PACK)
    if not os.path.isfile(os.path.join(pack, "xmb_artwork.py")):
        pytest.skip("the game pack has no disc artwork reader")
    arts = disc_art.extract(ISO, str(tmp_path), pack)
    cats = {}
    for a in arts:
        cats[a.category] = cats.get(a.category, 0) + 1
        assert png.info(open(a.path, "rb").read())["width"] == a.width
    assert cats.get("loading", 0) >= 15 and len(arts) >= 20
    again = disc_art.extract(ISO, str(tmp_path), pack)             # cached
    assert [a.path for a in again] == [a.path for a in arts]
    s = disc_art.suggest(arts, pack)
    assert "pic1" in s and "icon0" in s
    data, _n = picture.render(s["icon0"][0].path, "icon0", box=s["icon0"][1])
    assert png.info(data)["width"] == 144


def test_disc_music_with_a_stand_in_provider(tmp_path, monkeypatch, no_ffmpeg):
    from ps2rc.xmb import disc_music, disc_art
    pack = tmp_path / "pack"
    pack.mkdir()
    (pack / "xmb_artwork.py").write_text(
        "import math, struct\n"
        "def artwork(disc, out_dir, log):\n    return []\n"
        "def music_list(disc, log, progress):\n"
        "    progress(1, 1)\n"
        "    return [{'name': 'T 1', 'category': 'radio', 'seconds': 30.0, 'ref': 7}]\n"
        "def music_pcm(disc, ref, start, seconds):\n"
        "    n = int(seconds * 24000)\n"
        "    return b''.join(struct.pack('<h', int(8000 * math.sin(i / 10.0))) for i in range(n)), 24000, 1\n")

    class FakeDisc:
        def close(self):
            pass
    monkeypatch.setattr(disc_music, "_open", lambda source: FakeDisc())
    iso = tmp_path / "x.iso"
    iso.write_bytes(b"x")
    seen = []
    tr = disc_music.list_music(str(iso), str(tmp_path / "c"), str(pack), progress=lambda a, b: seen.append((a, b)))
    assert [t.name for t in tr] == ["T 1"] and seen == [(1, 1)]
    assert disc_music.list_music(str(iso), str(tmp_path / "c"), str(pack))[0].ref == 7          # cached
    out = disc_music.extract(str(iso), str(pack), tr[0], 5.0, 4.0, str(tmp_path / "m.wav"))
    with wave.open(out) as w:
        assert (w.getframerate(), w.getnchannels()) == (44100, 2) and abs(w.getnframes() - 4 * 44100) < 50
    assert disc_art.find_provider(str(pack)) is disc_art.find_provider(str(pack))                # loaded once


@pytest.mark.needs_game
@pytest.mark.skipif(not os.path.isfile(ISO) or not PACK, reason="PS2RC_TEST_ISO (a disc image) + PS2RC_TEST_PACK")
def test_disc_music_from_the_players_disc(tmp_path):
    from ps2rc.xmb import disc_music
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    pack = os.path.join(root, "games", PACK)
    if not disc_music.available(pack):
        pytest.skip("the game pack offers no music from the disc")
    tracks = disc_music.list_music(ISO, str(tmp_path), pack)
    assert len(tracks) > 100 and all(t.seconds >= 20 for t in tracks)
    t = max(tracks, key=lambda x: x.seconds)
    out = disc_music.extract(ISO, pack, t, 10.0, 5.0, str(tmp_path / "m.wav"))
    pcm = sound.read_pcm(out, 0, 5)
    a = sound._samples(pcm)
    assert max(abs(v) for v in a[::50]) > 1000                       # sound, not silence


def _texture_pack():
    """a local game pack whose disc-picture reader decodes textures itself (none in the public tree: skipped)"""
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    games = os.path.join(root, "games")
    packs = [os.path.join(games, g) for g in sorted(os.listdir(games))] if os.path.isdir(games) else []
    pack = next((p for p in packs if os.path.isfile(os.path.join(p, "xmb_artwork.py")) and
                 os.path.isfile(os.path.join(p, "tests", "texconv", "ps2txd.py"))), None)
    if pack is None:
        pytest.skip("no pack with its own texture decoder in this tree")
    import importlib.util
    spec = importlib.util.spec_from_file_location("pack_xmb_artwork_t", os.path.join(pack, "xmb_artwork.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


@pytest.mark.parametrize("depth,pal,swz,clut", [(8, 8, True, 0x500), (8, 8, False, 0x100), (4, 4, True, 0x500),
                                                (4, 4, False, 0x100), (32, 0, False, 0x500), (32, 0, False, 0x600),
                                                (16, 0, False, 0x100)])
def test_texture_decode_without_numpy_matches_the_model(pillow, depth, pal, swz, clut):
    """the converter app has no numpy: the pack's pure-Python texture decoder must give the numpy model's pixels"""
    np = pytest.importorskip("numpy")
    import random
    xa = _texture_pack()
    m = xa._txd()
    if not hasattr(m, "decode_rgba"):
        pytest.skip("numpy model not importable")
    rnd = random.Random(depth * 100 + pal + swz)

    class P:
        def __init__(self, off, n):
            self.data_off, self.data_len = off, n

    class T:
        pass
    w, h = 64, 32
    t = T()
    t.depth, t.pixfmt, t.pal_kind = depth, clut, pal
    t.psm = {8: m.PSMT8, 4: m.PSMT4}.get(depth, m.PSMCT32)
    sw_w = w
    nbytes = {8: 2 * w * h, 4: w * h, 32: 4 * w * h, 16: 2 * w * h}[depth]
    t.pix = bytes(16) + bytes(rnd.randrange(256) for _ in range(nbytes))
    lv = (w, h, sw_w, h, P(16, nbytes - 5 if pal else nbytes), swz)    # short indices: the clipping rule too
    t.levels = lambda: [lv]
    if pal:
        n = 256 if pal == 8 else 16
        ent = 4 if clut == 0x500 else 2
        t.pal = bytes(8) + bytes(rnd.randrange(256) for _ in range(n * ent))
        t.palette_upload = lambda: P(8, n * ent)
    ref = m.decode_rgba(t)
    got = np.asarray(xa.decode(m, t))
    assert got.shape == ref.shape and (got == ref).all()
