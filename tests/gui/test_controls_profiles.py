"""Control profiles (gui/controls_profiles.py, no Qt): the shared file is plain ps2rc.cfg text with a header, export ->
import gives the identical mapping, rename / delete work, and a file with anything the game would not accept is
refused with the line and changes nothing."""
import pytest

import controls_model as cm
import controls_profiles as cp

VALS = {"controls_preset": "stories", "map_cross": "square", "map_square": "cross", "car_l2": "l+analog_left",
        "select_to_game": "tap"}


def test_text_is_cfg_lines_with_a_header():
    t = cp.to_text("My layout", VALS)
    lines = t.splitlines()
    assert lines[0] == "# PS2RC controls profile: My layout" and lines[1].startswith("# ")
    body = [ln for ln in lines if not ln.startswith("#")]
    assert all("=" in ln and ln.split("=", 1)[0] in cm.KEYS for ln in body)
    assert "controls_preset=stories" in body and "map_cross=square" in body and "car_l2=l+analog_left" in body


def test_round_trip_identical_mapping():
    name, vals = cp.parse_text(cp.to_text("Round", VALS))
    a, b = cm.Layout(VALS), cm.Layout(vals)
    assert name == "Round" and a.to_values() == b.to_values()
    assert (a.line(), a.car_line()) == (b.line(), b.car_line())
    # Windows line ends, a BOM, comments after values, upper case keys: as the game reads ps2rc.cfg
    txt = "﻿# PS2RC controls profile: Pasted\r\nMAP_CROSS = square   # mine\r\nmap_square=cross\r\n"
    assert cp.parse_text(txt) == ("Pasted", {"map_cross": "square", "map_square": "cross"})
    assert cp.parse_text("controls_preset=VCS\n")[1] == {"controls_preset": "vcs"}       # the old name: accepted
    assert cm.Layout(cp.parse_text("controls_preset=VCS\n")[1]).to_values() == {"controls_preset": "stories"}


@pytest.mark.parametrize("text,why", [
    ("draw_distance=40\n", "line 1: draw_distance is not a controls setting"),
    ("# x\nmap_l2=foo\n", "map_l2=foo: unknown button name"),
    ("controls_preset=gtx\n", "line 1: controls_preset=gtx: use default, stories"),
    ("map_r3=l+r+select\n", "contains L+R+SELECT"),
    ("map_l2=select+triangle\n", "same buttons as l3"),
    ("just words\n", "line 1: not key=value"),
    ("map_l2=\n", "line 1: map_l2 has no value"),
])
def test_bad_files_refused(text, why):
    with pytest.raises(cp.ProfileError) as e:
        cp.parse_text(text)
    assert why in str(e.value)


def test_store_save_load_rename_delete(tmp_path):
    st = cp.Store(str(tmp_path / "controls_profiles"))
    assert st.names() == []
    assert st.save("  Race: car / bike  ", VALS) == "Race: car / bike"
    assert st.names() == ["Race: car / bike"] and st.load("Race: car / bike") == cm.Layout(VALS).to_values()
    assert st.free_name("Race: car / bike") == "Race: car / bike (2)"
    assert st.rename("Race: car / bike", "Driving") == "Driving"
    assert st.names() == ["Driving"]
    st.save("Other", {})
    with pytest.raises(cp.ProfileError):
        st.rename("Driving", "Other")                    # never over another profile
    (tmp_path / "controls_profiles" / "broken.cfg").write_text("map_l2=foo\n", encoding="utf-8")
    assert st.names() == ["Driving", "Other"]            # a broken file is not offered
    st.delete("Driving")
    assert st.names() == ["Other"]
    with pytest.raises(cp.ProfileError):
        cp.clean_name("   ")


def test_car_foot_is_not_set_on_import():
    """the in-game menu's car_X=foot (not set) is read like a missing line"""
    name, vals = cp.parse_text("\n".join(["controls_preset=stories", "car_l2=foot", "car_l3 = l+r", ""]), "p")
    assert vals == {"controls_preset": "stories", "car_l3": "l+r"}


def test_camera_invert_in_profiles():
    text = cp.to_text("cam", {"controls_preset": "stories", "camera_invert": "y"})
    assert "camera_invert=y" in text
    assert cp.parse_text(text)[1] == {"controls_preset": "stories", "camera_invert": "y"}
    with pytest.raises(cp.ProfileError):
        cp.parse_text("camera_invert=sideways")
