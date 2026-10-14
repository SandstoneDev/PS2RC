"""The window's pure parts: settings form from a template, time left, PSP drive detection, texts, preview mock."""
import json
import os

import pytest

import cfgform
import eta as etamod
import i18n
import psp_drives

TEMPLATE = """# =====================================================================
#  Game settings (this file sits next to EBOOT.PBP)
# =====================================================================
#  Edit it with any text editor.

# ---------------------------------------------------------------------
#  GRAPHICS
# ---------------------------------------------------------------------
# draw_distance - how far the world is drawn, in percent of the original. 10 to 100. Original: 100. Ours: 40.
draw_distance=40

# textures - sharpness: full = as on the original, half = half size, low = softer still. Ours: full.
textures=full

# mirrors - mirrors in interiors: 1 = on, 0 = off. Ours: 0.
mirrors=0

# ---------------------------------------------------------------------
#  PERFORMANCE
# ---------------------------------------------------------------------
# fps_cap - frames-per-second limit: 30 (as the original), 25, 20, 15 or 60 (experimental).
#   off = no limit. Ours: 30.
fps_cap=30
# overclock - automatic CPU overclock: 0 = none (333 MHz), 342 or 365 (MHz). Ours: 0.
#   RISK: on some consoles an overclock can switch the PSP off.
overclock=0
# far_ai - think every second frame beyond this many metres. 0 = off. 0 to 400. Ours: 20.
far_ai = 20   # inline comment
# select_up - what SELECT + Up does: stick = right stick up, r3 = the R3 button, l3 = the L3 button.
#select_up=r3
# name - free text
name=abc
"""


def test_template_kinds():
    items = cfgform.parse(TEMPLATE)
    groups = [i.title for i in items if isinstance(i, cfgform.Group)]
    assert groups == ["GRAPHICS", "PERFORMANCE"]                 # the title block is not a group
    s = {i.key: i for i in cfgform.settings(items)}
    assert list(s) == ["draw_distance", "textures", "mirrors", "fps_cap", "overclock", "far_ai", "select_up", "name"]
    assert (s["draw_distance"].kind, s["draw_distance"].range, s["draw_distance"].unit) == ("range", (10, 100), "%")
    assert (s["textures"].kind, s["textures"].options) == ("choice", ["full", "half", "low"])
    assert s["mirrors"].kind == "bool"
    assert s["fps_cap"].kind == "choice" and s["fps_cap"].options == ["off", "30", "25", "20", "15", "60"]
    assert s["overclock"].options == ["0", "342", "365"] and s["overclock"].risk.startswith("RISK")
    assert (s["far_ai"].kind, s["far_ai"].range, s["far_ai"].unit, s["far_ai"].value) == ("range", (0, 400), "m",
                                                                                           "20")
    assert s["select_up"].commented and s["select_up"].default is None and s["select_up"].options == ["stick", "r3",
                                                                                                    "l3"]
    assert s["name"].kind == "text"
    assert s["draw_distance"].summary.startswith("how far")


def test_overrides_and_writing():
    items = cfgform.parse(TEMPLATE)
    vals = {"draw_distance": "250", "textures": "full", "mirrors": "on", "select_up": "", "fps_cap": "25"}
    ov = cfgform.overrides(items, vals)
    assert ov == {"draw_distance": "100", "mirrors": "1", "fps_cap": "25"}       # clamped, defaults dropped
    out = cfgform.apply_to_text(TEMPLATE, dict(ov, select_up="l3", extra_key="7"))
    again = {i.key: i for i in cfgform.settings(cfgform.parse(out))}
    assert again["draw_distance"].value == "100" and again["mirrors"].value == "1"
    assert again["select_up"].value == "l3" and not again["select_up"].commented
    assert out.rstrip().endswith("extra_key=7")
    assert out.count("# draw_distance - how far") == 1                         # comments kept
    # the converter's own fallback (appended keys, later line wins) means the same thing
    tail = TEMPLATE + "\n".join("%s=%s" % kv for kv in ov.items())
    last = {}
    for ln in tail.splitlines():
        m = cfgform.SETTING.match(ln.strip())
        if m:
            last[m.group(1)] = m.group(2)
    assert all(last[k] == v for k, v in ov.items())


def test_local_descriptions():
    ru = TEMPLATE.replace("GRAPHICS", "GRAFIKA").replace("how far the world is drawn", "dalnost prorisovki")
    items = cfgform.merge_descriptions(cfgform.parse(TEMPLATE), cfgform.parse(ru))
    g = [i for i in items if isinstance(i, cfgform.Group)][0]
    assert g.title_local == "GRAFIKA"
    s = cfgform.settings(items)[0]
    assert "dalnost" in " ".join(s.comment_local) and s.kind == "range"


def test_eta():
    clock = [0.0]
    e = etamod.Eta(["a", "b", "c"], {"a": 10, "b": 100, "c": 10}, clock=lambda: clock[0])
    assert e.left() is None
    e.step("a")
    assert e.left() == pytest.approx(120)
    clock[0] = 20.0                                       # 'a' took twice the estimate
    e.finish("a", 20.0, False)
    e.step("b")
    assert e.speed() == pytest.approx(2.0)
    assert e.left() == pytest.approx(220)
    clock[0] = 70.0
    e.pct("b", 1, 2)                                      # half of b in 50 s -> 50 s more
    assert e.left() == pytest.approx(50 + 20)
    e.finish("b", 100.0, False)
    e.finish("c", 0.0, True)                              # kept from the last run: costs nothing
    assert e.left() == 0 and e.fraction() == pytest.approx(1.0)
    assert etamod.fmt_left(30) == "less than a minute" and etamod.fmt_left(300, "ru") == i18n.tr("eta.min", "ru", 5)


def test_drives_and_folders(tmp_path):
    psp = tmp_path / "psp"
    game = psp / "PSP" / "GAME"
    (game / "PS2RC_X").mkdir(parents=True)
    (game / "PS2RC_X" / "EBOOT.PBP").write_bytes(b"\0PBP")
    (game / "PS2RC_X" / "ps2rc.cfg").write_text("a=1")
    (game / "PS2RC_Y").mkdir()
    (game / "PS2RC_Y" / psp_drives.MANIFEST).write_text(json.dumps({"converter_version": "0.0.9"}))
    (game / "OTHER").mkdir()
    (game / "OTHER" / "EBOOT.PBP").write_bytes(b"\0PBP")
    usb = tmp_path / "usb"
    usb.mkdir()
    fixed = tmp_path / "fixed"
    fixed.mkdir()
    d = psp_drives.drives([(str(usb), "STICK", True), (str(psp), "PSP", True), (str(fixed), "C", False)])
    assert [x["root"] for x in d] == [str(psp), str(usb)]                 # PSP first, plain fixed disks left out
    assert d[0]["is_psp"] and not d[1]["is_psp"] and d[0]["free"] > 0
    f = psp_drives.game_folders(d)
    assert [x["name"] for x in f] == ["PS2RC_X", "PS2RC_Y"]               # a foreign homebrew is not listed
    assert f[1]["version"] == "0.0.9" and f[1]["manifest"] and not f[0]["manifest"]
    assert psp_drives.update_available("0.0.9", "0.1.0") and not psp_drives.update_available("0.1.0", "0.1.0")
    assert not psp_drives.update_available(None, "0.1.0")
    assert isinstance(psp_drives.drives(), list)                          # this PC: never raises


def test_every_text_has_both_languages():
    assert [c for c, _n in i18n.LANGS] == ["en", "ru"]
    for lang, d in i18n.TEXTS.items():
        if lang == "en":
            continue
        missing = sorted(k for k in i18n.T if k not in d)
        assert not missing, (lang, missing)
        extra = sorted(k for k in d if not k.startswith("_") and k not in i18n.T)
        assert not extra, (lang, extra)
        for k, en in i18n.T.items():
            if isinstance(en, str):
                assert en.count("%") == d[k].count("%"), (lang, k)
            else:
                assert len(en) == len(d[k]), (lang, k)
        assert set(d["_errors"]) == set(i18n.ERRORS) and all(len(v) == 2 for v in d["_errors"].values())
        from ps2rc.convert.pipeline import STEPS
        assert set(d["_steps"]) == {n for n, _t in STEPS}
    assert i18n.tr("conv.left", "ru", "x") == i18n.TEXTS["ru"]["conv.left"] % "x" != i18n.tr("conv.left", "en", "x")
    assert i18n.error_text("nope", "msg", "hint") == ("msg", "hint")
    # every text key the window uses exists
    src = open(os.path.join(os.path.dirname(cfgform.__file__), "ps2rc_gui.py"), encoding="utf-8").read()
    import re
    used = {k for k in re.findall(r'self\.t\("([a-z0-9_.]+)"', src) if not k.endswith(".")}   # "x." + dynamic
    missing = sorted(k for k in used if k not in i18n.T)
    assert not missing, missing


def test_preview_timeline(pillow):
    from ps2rc.pbp import placeholder
    from ps2rc.xmb import preview
    frames = placeholder.icon1_frames("ANIM", seconds=1.0)
    m = preview.Mock(pic1=placeholder.pic1(), pic0=placeholder.pic0("X", "Y"), icon0=placeholder.icon0("I"),
                     icon1_frames=frames, title="Title")
    a, b, c = m.compose(0.0), m.compose(preview.T_PIC1 + 0.1), m.compose(preview.T_START + preview.T_FADE + 0.2)
    assert a.size == (480, 272) == c.size
    box = (preview.PIC0_XY[0] + 150, preview.PIC0_XY[1] + 40, preview.PIC0_XY[0] + 160, preview.PIC0_XY[1] + 50)
    assert b.crop(box).tobytes() != c.crop(box).tobytes()                 # PIC0 faded in later
    ix, iy = preview.ICON_XY
    icon_b = b.crop((ix, iy, ix + 144, iy + 80)).tobytes()
    icon_c = c.crop((ix, iy, ix + 144, iy + 80)).tobytes()
    assert icon_b != icon_c                                               # ICON1 replaced ICON0
    assert preview.ppm(c)[:2] == b"P6"


def test_eta_ignores_kept_steps():
    e = etamod.Eta(["a", "b", "c"], {"a": 50, "b": 50, "c": 100}, clock=lambda: 0.0)
    e.finish("a", 0.0, True)                              # kept from the last run (0 s): not a speed sample
    e.finish("b", 100.0, False)                           # really ran at half the estimated speed
    assert e.speed() == pytest.approx(2.0) and e.left() == pytest.approx(200)
