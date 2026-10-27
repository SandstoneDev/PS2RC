"""The converter's update check (gui/updater.py + the window's banner / Help menu). No network: every GitHub answer is
a fake opener here. Version compare, asset per platform, sha256 verify, foreign addresses and zip paths refused,
the "check at start" setting and a skipped version respected, the new version unpacked next to the old one."""
import hashlib
import io
import json
import os
import threading
import urllib.error
import urllib.parse
import zipfile

import pytest

import updater

DL_PATH = "/SandstoneDev/PS2RC/releases/download/v0.3.0/"


def url(host, path=DL_PATH, scheme="https"):
    """addresses of the tests from their parts (scheme, host[:port], path): the real one and look-alikes"""
    return urllib.parse.urlunsplit((scheme, host, path, "", ""))


REL = url("github.com")


def release_json(tag="v0.3.0", draft=False, prerelease=False, names=None, base=REL):
    names = names if names is not None else [
        "PS2RC_Converter_0.3.0_win64.zip", "PS2RC_Converter_0.3.0_win64.zip.sha256",
        "PS2RC_Converter_0.3.0_macos-arm64.dmg", "PS2RC_Converter_0.3.0_macos-arm64.dmg.sha256",
        "PS2RC_Converter_0.3.0_linux-x86_64.AppImage", "PS2RC_Converter_0.3.0_linux-x86_64.AppImage.sha256",
        "PS2RC_Converter_0.3.0_sources.zip"]
    return {"tag_name": tag, "draft": draft, "prerelease": prerelease, "body": "notes",
            "html_url": "https://github.com/SandstoneDev/PS2RC/releases/tag/" + tag,
            "assets": [{"name": n, "browser_download_url": base + n, "size": 10} for n in names]}


class Resp:
    def __init__(self, data, url):
        self.f, self.url = io.BytesIO(data), url
        self.headers = {"Content-Length": str(len(data))}

    def read(self, n=-1):
        return self.f.read(n)

    def geturl(self):
        return self.url

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class Opener:
    """url -> bytes, or an exception to raise; records every request"""

    def __init__(self, table):
        self.table, self.seen = table, []

    def open(self, req, timeout=None):
        url = req.full_url
        self.seen.append((url, dict(req.header_items())))
        v = self.table.get(url)
        if isinstance(v, BaseException):
            raise v
        if v is None:
            raise urllib.error.HTTPError(url, 404, "nf", {}, None)
        return Resp(v, url)


# ------------------------------------------------------------------------------------------------ pure parts
def test_version_compare():
    assert updater.parse_version("v0.2.1") == (0, 2, 1) == updater.parse_version("0.2.1")
    for bad in ("v0.3", "0.3.0-rc1", "latest", "", None, "v1.2.3.x"):
        assert updater.parse_version(bad) is None
    assert updater.is_newer("v0.2.2", "0.2.1") and updater.is_newer("v0.10.0", "0.9.9")
    assert updater.is_newer("v1.0.0", "0.99.99")
    assert not updater.is_newer("v0.2.1", "0.2.1") and not updater.is_newer("v0.2.0", "0.2.1")
    assert not updater.is_newer("v0.3.0-beta", "0.2.1")


def test_drafts_prereleases_and_older_ignored():
    assert updater.evaluate(release_json(draft=True), "0.2.1", "win") is None
    assert updater.evaluate(release_json(prerelease=True), "0.2.1", "win") is None
    assert updater.evaluate(release_json(tag="v0.2.1"), "0.2.1", "win") is None
    assert updater.evaluate(release_json(tag="v0.3.0-rc1"), "0.2.1", "win") is None
    assert updater.evaluate(["not", "a", "dict"], "0.2.1", "win") is None


@pytest.mark.parametrize("plat,name", [("win", "PS2RC_Converter_0.3.0_win64.zip"),
                                       ("mac", "PS2RC_Converter_0.3.0_macos-arm64.dmg"),
                                       ("linux", "PS2RC_Converter_0.3.0_linux-x86_64.AppImage")])
def test_asset_per_platform(plat, name):
    r = updater.evaluate(release_json(), "0.2.1", plat)
    assert r.version == "0.3.0" and r.asset["name"] == name and r.sha_asset["name"] == name + ".sha256"
    assert r.asset["url"] == REL + name and r.page.endswith("/releases/tag/v0.3.0")


def test_platform_names():
    assert updater.platform_key("win32") == "win" and updater.platform_key("darwin") == "mac"
    assert updater.platform_key("linux") == "linux" and updater.platform_key("freebsd13") is None
    assert updater.evaluate(release_json(), "0.2.1", "beos").asset is None


def test_foreign_asset_addresses_ignored():
    for base in (url("github.com", "/Other/PS2RC/releases/download/v0.3.0/"), url("github.com", scheme="http"),
                 url("github.com.evil.example"), url("evil.example"), url("github.com:8443"),
                 url("%s@%s" % ("user", "github.com")), url("github.com", DL_PATH + "../../x/")):
        r = updater.evaluate(release_json(base=base), "0.2.1", "win")
        assert r is not None and r.asset is None and r.sha_asset is None, base
    assert updater.download_url_ok(REL + "f.zip")
    assert updater.redirect_ok(url("objects.githubusercontent.com", "/x"))
    assert updater.redirect_ok(url("release-assets.githubusercontent.com", "/x"))
    for bad in (url("objects.githubusercontent.com", "/x", "http"), url("objects.githubusercontent.com.evil.example"),
                url("evil.example", "/x")):
        assert not updater.redirect_ok(bad)
    h = updater._Redirects(updater.redirect_ok)
    with pytest.raises(updater.UpdateError) as e:
        h.redirect_request(None, None, 302, "Found", {}, url("evil.example", "/x.zip"))
    assert e.value.code == "url"


def test_check_is_quiet_offline_limit_bad():
    import socket
    for exc, code in ((urllib.error.URLError("no route"), "offline"), (socket.timeout("t"), "offline"),
                      (urllib.error.HTTPError(updater.API_URL, 403, "rate", {}, None), "limit"),
                      (urllib.error.HTTPError(updater.API_URL, 429, "rate", {}, None), "limit"),
                      (urllib.error.HTTPError(updater.API_URL, 500, "x", {}, None), "bad")):
        assert updater.check("0.2.1", Opener({updater.API_URL: exc}), "win") == (None, code)
    assert updater.check("0.2.1", Opener({updater.API_URL: b"<html>"}), "win") == (None, "bad")
    op = Opener({updater.API_URL: json.dumps(release_json()).encode()})
    rel, err = updater.check("0.2.1", op, "win")
    assert err is None and rel.version == "0.3.0"
    # exactly one request, to the API, with no data about the PC beyond the converter version
    assert [u for u, _h in op.seen] == [updater.API_URL]
    hdrs = {k.lower(): v for k, v in op.seen[0][1].items()}
    assert hdrs["user-agent"] == "PS2RC-Converter/0.2.1" and set(hdrs) <= {"user-agent", "accept"}


# ------------------------------------------------------------------------------------------------ download
def payload_release(data, sha_text, plat="win"):
    rel = updater.evaluate(release_json(), "0.2.1", plat)
    table = {rel.asset["url"]: data}
    if sha_text is not None:
        table[rel.sha_asset["url"]] = sha_text.encode()
    return rel, Opener(table)


def test_sha256_ok(tmp_path):
    data = b"new converter" * 1000
    rel, op = payload_release(data, "%s  %s\n" % (hashlib.sha256(data).hexdigest(), "PS2RC_Converter_0.3.0_win64.zip"))
    seen = []
    p = updater.download_release(rel, str(tmp_path), lambda d, t: seen.append((d, t)), opener=op)
    assert open(p, "rb").read() == data and os.path.basename(p) == rel.asset["name"]
    assert seen[-1] == (len(data), len(data))
    assert os.listdir(tmp_path) == [rel.asset["name"]]


def test_sha256_mismatch_and_missing_refused(tmp_path):
    data = b"tampered"
    rel, op = payload_release(data, "%s  PS2RC_Converter_0.3.0_win64.zip\n" % ("0" * 64))
    with pytest.raises(updater.UpdateError) as e:
        updater.download_release(rel, str(tmp_path), opener=op)
    assert e.value.code == "sha" and os.listdir(tmp_path) == []
    # no .sha256 asset in the release
    rel2 = updater.evaluate(release_json(names=["PS2RC_Converter_0.3.0_win64.zip"]), "0.2.1", "win")
    with pytest.raises(updater.UpdateError) as e:
        updater.download_release(rel2, str(tmp_path), opener=Opener({}))
    assert e.value.code == "nosha"
    # a .sha256 file that holds no hash for this file
    rel3, op3 = payload_release(data, "%s  other_file.zip\n" % hashlib.sha256(data).hexdigest())
    with pytest.raises(updater.UpdateError) as e:
        updater.download_release(rel3, str(tmp_path), opener=op3)
    assert e.value.code == "nosha" and os.listdir(tmp_path) == []


def test_cancel_leaves_nothing(tmp_path):
    data = b"x" * 300000
    rel, op = payload_release(data, hashlib.sha256(data).hexdigest())
    ev = threading.Event()
    with pytest.raises(updater.UpdateError) as e:
        updater.download_release(rel, str(tmp_path), lambda d, t: ev.set(), ev, opener=op)
    assert e.value.code == "cancel" and os.listdir(tmp_path) == []


def test_parse_sha256():
    h = "AB" * 32
    assert updater.parse_sha256(h) == h.lower()
    assert updater.parse_sha256("%s *dir/f.zip" % h, "f.zip") == h.lower()
    assert updater.parse_sha256("%s  g.zip" % h, "f.zip") is None and updater.parse_sha256("nothing") is None


# ------------------------------------------------------------------------------------------------ install
def make_zip(path, members):
    with zipfile.ZipFile(path, "w") as z:
        for name, data in members:
            if isinstance(data, zipfile.ZipInfo):
                z.writestr(data, b"target")
            else:
                z.writestr(name, data)
    return str(path)


@pytest.mark.parametrize("bad", ["../evil.txt", "PS2RC_Converter/../../evil.txt", "/abs/evil.txt",
                                 "Z:evil.txt", "PS2RC_Converter\\..\\..\\evil.txt", "a/b:c.txt"])
def test_zip_traversal_refused(tmp_path, bad):
    z = make_zip(tmp_path / "u.zip", [("PS2RC_Converter/app/x.py", b"ok"), (bad, b"evil")])
    dest = tmp_path / "out" / "PS2RC_Converter_0.3.0"
    (tmp_path / "out").mkdir()
    with pytest.raises(updater.UpdateError) as e:
        updater.safe_extract(z, str(dest))
    assert e.value.code == "zip"
    assert os.listdir(tmp_path / "out") == [] and not (tmp_path / "evil.txt").exists()


def test_zip_symlink_refused(tmp_path):
    info = zipfile.ZipInfo("PS2RC_Converter/link")
    info.external_attr = (0o120777 << 16)
    z = make_zip(tmp_path / "u.zip", [("PS2RC_Converter/a.txt", b"a"), ("", info)])
    with pytest.raises(updater.UpdateError):
        updater.safe_extract(z, str(tmp_path / "new"))
    assert not (tmp_path / "new").exists()


def test_install_next_to_the_running_folder(tmp_path, monkeypatch):
    parent = tmp_path / "apps"
    cur = parent / "PS2RC_Converter"
    (cur / "app" / "gui").mkdir(parents=True)
    (cur / updater.BUNDLE_EXE).write_bytes(b"old exe")
    (cur / "my_notes.txt").write_text("user file")
    assert updater.bundle_root(str(cur / "app" / "gui")) == str(cur)
    assert updater.bundle_root(str(tmp_path / "checkout" / "gui")) is None
    z = make_zip(tmp_path / "u.zip", [("PS2RC_Converter/" + updater.BUNDLE_EXE, b"new exe"),
                                      ("PS2RC_Converter/app/gui/run_gui.py", b"print()")])
    before = sorted(os.listdir(cur))
    new = updater.install_zip(z, str(cur), "0.3.0")
    assert new == str(parent / "PS2RC_Converter_0.3.0")
    assert open(os.path.join(new, updater.BUNDLE_EXE), "rb").read() == b"new exe"
    assert updater.new_exe(new) and os.path.isfile(os.path.join(new, "app", "gui", "run_gui.py"))
    # the running folder and the player's files are untouched
    assert sorted(os.listdir(cur)) == before and (cur / updater.BUNDLE_EXE).read_bytes() == b"old exe"
    # a second install never overwrites the first
    assert updater.install_zip(z, str(cur), "0.3.0") == str(parent / "PS2RC_Converter_0.3.0_2")
    # from a checkout (no bundle): into Downloads
    dl = tmp_path / "home" / "Downloads"
    dl.mkdir(parents=True)
    monkeypatch.setattr(updater, "downloads_dir", lambda: str(dl))
    assert updater.install_zip(z, None, "0.3.0") == str(dl / "PS2RC_Converter_0.3.0")


def test_settings_helpers():
    rel = updater.evaluate(release_json(), "0.2.1", "win")
    assert updater.should_check_at_start({}) and not updater.should_check_at_start({"update_check": False})
    assert updater.should_show(rel, {}) and not updater.should_show(rel, {"update_skip": "0.3.0"})
    assert updater.should_show(rel, {"update_skip": "0.3.0"}, manual=True)
    assert updater.should_show(rel, {"update_skip": "0.2.9"}) and not updater.should_show(None, {})


# ------------------------------------------------------------------------------------------------ the window
try:                                                  # the window tests need Qt; the parts above do not
    import PySide6  # noqa: F401
    from test_gui_window import gui, pump, qapp  # noqa: E402,F401  (fixtures)
except ImportError:
    @pytest.fixture
    def gui():
        pytest.skip("no PySide6")


def fake_check(monkeypatch, result):
    calls = []

    def check(version, opener=None, plat=None):
        calls.append(version)
        return result
    monkeypatch.setattr(updater, "check", check)
    return calls


def test_check_at_start_setting_respected(gui):
    app, ps2rc_gui, qapp, _tmp, monkeypatch = gui
    rel = updater.evaluate(release_json(), ps2rc_gui.VERSION, "win")
    calls = fake_check(monkeypatch, (rel, None))
    at_start = app.update_actions[1]                  # Help > Check for updates at start: on by default
    assert at_start.isCheckable() and at_start.isChecked()
    at_start.setChecked(False)
    assert ps2rc_gui.load_settings(app.settings_path)["update_check"] is False
    app.show_page("home")                             # the rebuilt menu shows the saved state
    at_start = app.update_actions[1]
    assert not at_start.isChecked()
    app.auto_update_check()
    pump(qapp, timeout=0.3)
    assert calls == [] and app.update_banner is None
    # turned back on: saved, and the start check asks (once) and shows the banner
    at_start.setChecked(True)
    assert app.s["update_check"] is True and ps2rc_gui.load_settings(app.settings_path)["update_check"] is True
    app.auto_update_check()
    assert pump(qapp, lambda: app.update_banner is not None)
    assert calls == [ps2rc_gui.VERSION]
    parts = app.update_banner.parts
    assert "0.3.0" in parts["text"].text()
    for k in ("upd.install", "upd.notes", "upd.later", "upd.skip"):
        assert parts[k].text() == i18n_tr(k)
    # the banner survives a page change; Later hides it for this session
    app.show_page("disc")
    assert app.update_banner is not None
    parts = app.update_banner.parts
    opened = []
    from PySide6.QtGui import QDesktopServices
    monkeypatch.setattr(QDesktopServices, "openUrl", lambda u: opened.append(u.toString()))
    parts["upd.notes"].click()
    assert opened == ["https://github.com/SandstoneDev/PS2RC/releases/tag/v0.3.0"]
    parts["upd.later"].click()
    assert app.update_banner is None
    app.show_page("home")
    assert app.update_banner is None and "update_skip" not in app.s


def i18n_tr(k):
    import i18n
    return i18n.tr(k, "en")


def test_skipped_version_respected(gui):
    app, ps2rc_gui, qapp, _tmp, monkeypatch = gui
    from PySide6.QtWidgets import QMessageBox
    rel = updater.evaluate(release_json(), ps2rc_gui.VERSION, "win")
    fake_check(monkeypatch, (rel, None))
    app.check_updates()
    assert pump(qapp, lambda: app.update_banner is not None)
    app.update_banner.parts["upd.skip"].click()
    assert app.update_banner is None
    assert ps2rc_gui.load_settings(app.settings_path)["update_skip"] == "0.3.0"
    app.update_rel = None
    app.check_updates()                               # automatic: the skipped version stays hidden
    pump(qapp, lambda: not app._update_busy, 2)
    assert app.update_banner is None and app.update_rel is None
    said = []
    monkeypatch.setattr(QMessageBox, "information", lambda *a: said.append(a[2]))
    app.check_updates(manual=True)                    # asked for: shown again
    assert pump(qapp, lambda: app.update_banner is not None)
    assert said == []


def test_manual_check_messages(gui):
    app, ps2rc_gui, qapp, _tmp, monkeypatch = gui
    from PySide6.QtWidgets import QMessageBox
    said = []
    monkeypatch.setattr(QMessageBox, "information", lambda *a: said.append(a[2]))
    fake_check(monkeypatch, (None, None))
    app.check_updates(manual=True)
    assert pump(qapp, lambda: said)
    assert said[-1] == i18n_tr("upd.newest") % ps2rc_gui.VERSION
    fake_check(monkeypatch, (None, "offline"))
    app.check_updates(manual=True)
    assert pump(qapp, lambda: len(said) == 2)
    assert i18n_tr("upd.err.offline") in said[-1]
    fake_check(monkeypatch, (None, "limit"))
    app.check_updates(manual=False)                   # at start: quiet
    pump(qapp, lambda: not app._update_busy, 2)
    assert len(said) == 2 and app.update_banner is None


def test_download_and_install_flow(gui):
    """Windows flow with a fake GitHub: sha256 checked, unpacked into a new sibling folder, offer to open it"""
    app, ps2rc_gui, qapp, tmp, monkeypatch = gui
    from PySide6.QtWidgets import QMessageBox
    zbuf = tmp / "src.zip"
    make_zip(zbuf, [("PS2RC_Converter/" + updater.BUNDLE_EXE, b"new exe")])
    data = zbuf.read_bytes()
    rel = updater.evaluate(release_json(), ps2rc_gui.VERSION, "win")
    op = Opener({rel.asset["url"]: data, rel.sha_asset["url"]: hashlib.sha256(data).hexdigest().encode()})
    real = updater.download_release
    monkeypatch.setattr(updater, "download_release", lambda r, f, p=None, c=None, **k: real(r, f, p, c, opener=op))
    cur = tmp / "apps" / "PS2RC_Converter"
    cur.mkdir(parents=True)
    monkeypatch.setattr(updater, "bundle_root", lambda gui_dir=None: str(cur))
    offers = []
    monkeypatch.setattr(app, "_update_offer", lambda text, exe, folder: offers.append((text, exe, folder)))
    app.update_rel = rel
    app._update_download()
    assert app.update_dialog is not None
    assert pump(qapp, lambda: offers)
    text, exe, folder = offers[0]
    assert folder == str(tmp / "apps" / "PS2RC_Converter_0.3.0") and exe == os.path.join(folder, updater.BUNDLE_EXE)
    assert folder in text and str(cur) in text and app.update_dialog is None
    # a bad checksum: a warning, nothing unpacked
    warned = []
    monkeypatch.setattr(QMessageBox, "warning", lambda *a: warned.append(a[2]))
    op.table[rel.sha_asset["url"]] = ("0" * 64).encode()
    app._update_download()
    assert pump(qapp, lambda: warned)
    assert warned[0] == i18n_tr("upd.err.sha") and len(offers) == 1
    assert sorted(os.listdir(tmp / "apps")) == ["PS2RC_Converter", "PS2RC_Converter_0.3.0"]
