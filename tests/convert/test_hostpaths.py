"""Host paths and helper programs (ps2rc.convert.hostpaths), the sound-less build of a profile, the movie converter
without ffprobe: the converter app's own ffmpeg is found, TEMP / the work folder move to paths the PSP toolchain can
read, a converter installed under a non-ASCII path is refused with a plain message, --audio none leaves the sound
model's generator and glue out, plans name the movie tools."""
import os
import sys
import types

import pytest

from ps2rc.convert import build as bmod
from ps2rc.convert import hostpaths as hp
from ps2rc.convert import manifest as mf
from ps2rc.convert.pipeline import Conversion, Options
from ps2rc.convert.steps import ConvertError

CYR = chr(0x0434) + chr(0x0438) + chr(0x0441) + chr(0x043A)    # a Cyrillic word (built here: the source stays ASCII)
NOT_ANSI = chr(0x1F600)                                       # in no Windows ANSI code page


def _exe(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"MZ")
    return path


def test_ffmpeg_from_the_app_folder_first(tmp_path, monkeypatch):
    app = tmp_path / "PS2RC_Converter"
    repo = app / "app"
    os.makedirs(repo)
    ff = _exe(str(app / "tools" / "ffmpeg" / ("ffmpeg" + hp.EXE)))
    other = _exe(str(tmp_path / "elsewhere" / ("ffmpeg" + hp.EXE)))
    monkeypatch.delenv("FFMPEG", raising=False)
    monkeypatch.setenv("PATH", os.path.dirname(other))
    assert os.path.samefile(hp.find_ffmpeg(str(repo)), ff)                 # before PATH
    assert hp.find_ffmpeg(str(repo), other) == other                       # --ffmpeg wins
    assert hp.find_ffprobe(ff) is None or "ffprobe" in hp.find_ffprobe(ff)
    line = hp.tools_line(str(repo))
    assert "ffmpeg: " in line and "not needed" in line
    probe = _exe(str(app / "tools" / "ffmpeg" / ("ffprobe" + hp.EXE)))
    assert os.path.samefile(hp.find_ffprobe(ff), probe)
    monkeypatch.setenv("PATH", "")
    os.remove(ff)
    assert "intro movies would be skipped" in hp.tools_line(str(repo))


def test_temp_moved_to_ascii(tmp_path):
    env = {"TEMP": str(tmp_path / CYR / "Temp"), "TMP": str(tmp_path / CYR / "Temp"), "ProgramData": str(tmp_path)}
    work = str(tmp_path / "work")
    t = hp.safe_temp(work, env)
    assert t == os.path.join(work, "tmp") and env["TEMP"] == env["TMP"] == t and os.path.isdir(t)
    env2 = {"TEMP": str(tmp_path / CYR), "ProgramData": str(tmp_path / "pd")}
    t2 = hp.safe_temp(str(tmp_path / CYR / "work"), env2)                  # the work folder is not ASCII either
    assert t2 == os.path.join(str(tmp_path / "pd"), "PS2RC", "tmp") and hp.is_ascii(t2)
    env3 = {"TEMP": str(tmp_path), "TMP": str(tmp_path)}
    assert hp.safe_temp(work, env3) is None and env3["TEMP"] == str(tmp_path)


@pytest.mark.skipif(os.name != "nt", reason="code-page rule of Windows")
def test_default_work_fits_the_code_page(tmp_path):
    out = str(tmp_path / ("out" + NOT_ANSI))
    env = {"LOCALAPPDATA": str(tmp_path / "lad")}
    w = hp.default_work(out, env)
    assert hp.ansi_safe(w) and w.startswith(str(tmp_path / "lad")) and "PS2RC" in w
    assert hp.default_work(str(tmp_path / "plain"), env) == os.path.join(str(tmp_path / "plain"), "_ps2rc_work")


def test_converter_under_a_non_ascii_path_is_refused(tmp_path):
    repo = tmp_path / ("conv_" + CYR) / "app"
    os.makedirs(repo / "games")
    o = Options(str(tmp_path / "x.iso"), str(tmp_path / "out"), repo=str(repo))
    with pytest.raises(ConvertError) as e:
        Conversion(o).run()
    assert e.value.code == "install_path" and "PS2RC_Converter" in e.value.hint


def test_audio_none_leaves_the_sound_model_out(tmp_path):
    pk = str(tmp_path / "pack")
    for n in ("vu/glue.c", "iop/unity.c", "iop/glue.c"):
        os.makedirs(os.path.dirname(os.path.join(pk, n)), exist_ok=True)
        open(os.path.join(pk, n), "w").write("/* x */\n")

    class P:
        dir = pk

        def expand(self, s):
            return os.path.join(pk, s)
    prof = {"extra_sources": ["vu/glue.c"], "audio_extra_sources": ["iop/unity.c"],
            "audio_include_sources": ["iop/glue.c"]}
    on, _ = bmod.profile_sources([], [], P(), prof, str(tmp_path))
    off, _ = bmod.profile_sources([], [], P(), dict(prof, audio_off=True), str(tmp_path))
    assert sorted(n for _s, n in on) == ["glue.c", "rt/glue.c", "unity.c"]
    assert [n for _s, n in off] == ["glue.c"]
    # the pipeline: --audio none drops the generators marked audio = true
    gens = [{"name": "snd", "audio": True}, {"name": "vu"}]
    fake = types.SimpleNamespace(o=types.SimpleNamespace(profile="public", audio="none"),
                                 recipe=types.SimpleNamespace(get=lambda k, d=None: {"convert.profile": "public"}.get(k, d)),
                                 pack=types.SimpleNamespace(name="p", get=lambda k, d=None:
                                                            {"profile.public": {"generators": gens}}.get(k, d)))
    name, tab = Conversion._profile(fake)
    assert name == "public" and [g["name"] for g in tab["generators"]] == ["vu"] and tab["audio_off"]
    fake.o.audio = "all"
    assert [g["name"] for g in Conversion._profile(fake)[1]["generators"]] == ["snd", "vu"]


def test_movie_facts_without_ffprobe():
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                                    "ps2rc", "movies"))
    import convert_movies_open as cmo
    video = ("  Duration: 00:00:15.36, start: 0.055600, bitrate: 2116 kb/s\n"
             "  Stream #0:0[0x1e0]: Video: mpeg2video (Main), yuv420p(tv, bt470bg, progressive), 640x576 [SAR 6:5 DAR "
             "4:3], 25 fps, 25 tbr, 90k tbn, start 0.055600\n")
    kv = cmo.ffmpeg_info(video)
    assert kv == {"duration": "15.360000", "width": "640", "height": "576", "display_aspect_ratio": "4:3",
                  "r_frame_rate": "25/1"}
    audio = ("  Duration: 00:00:15.40, bitrate: 354 kb/s\n  Stream #0:0: Audio: adpcm_ima_wav ([17][0][0][0] / 0x0011), "
             "44100 Hz, stereo, s16p, 352 kb/s\n")
    assert cmo.ffmpeg_info(audio) == {"duration": "15.400000", "codec_name": "adpcm_ima_wav", "sample_rate": "44100",
                                      "channels": "2"}


def test_plan_names_the_movie_tools(tmp_path):
    folder = tmp_path / "PS2RC_X"
    os.makedirs(folder)
    plan = mf.plan_update(str(folder))
    assert any(n.startswith("ffmpeg:") for n in plan.notes)
