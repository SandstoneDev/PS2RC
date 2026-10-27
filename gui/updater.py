"""Update check of the converter window (no Qt here: the window runs these functions in worker threads).

One GET to GitHub's "latest release" API of this project (no token, no data about the PC: only a User-Agent with the
converter version), and - only after the player clicks "Download and install" - the download of this system's
release file and its .sha256 file. Nothing else goes over the network; the check at start can be turned off (Help
menu), then no request is made at all.

    check(VERSION)          -> (Release or None, error code or None)   quiet: offline / rate limit / timeout = error code
    download_release(rel, folder, progress, cancel) -> path of the verified file (sha256 checked, else UpdateError)
    install_zip(zip, app_root, version) -> the NEW folder next to the running one (never overwrites anything)

Downloads only from the HTTPS addresses the API returned for this repository (github.com/<repo>/releases/download/),
following redirects only to GitHub's download hosts.
"""
import hashlib
import json
import os
import re
import shutil
import stat
import sys
import tempfile
import urllib.error
import urllib.parse
import urllib.request
import zipfile

REPO = "SandstoneDev/PS2RC"
API_URL = "https://api.github.com/repos/SandstoneDev/PS2RC/releases/latest"
API_HOST = "api.github.com"
RELEASES_PAGE = "https://github.com/SandstoneDev/PS2RC/releases"
DOWNLOAD_HOST = "github.com"
REDIRECT_HOSTS = ("github.com", "objects.githubusercontent.com", "release-assets.githubusercontent.com")
TIMEOUT = 8                     # seconds, per connection / read
SHA_MAX = 4096                  # a .sha256 file is one line
API_MAX = 2 * 1024 * 1024       # the API answer (release notes included)
ASSET_MAX = 2 * 1024 ** 3       # the converter archive is ~125 MB; anything above 2 GB is not ours
BUNDLE_EXE = "PS2RC Converter.exe"
NAME_FMT = "PS2RC_Converter_%s_%s.%s"
PLATFORMS = {"win": ("win64", "zip"), "mac": ("macos-arm64", "dmg"), "linux": ("linux-x86_64", "AppImage")}
_VER = re.compile(r"^v?(\d+)\.(\d+)\.(\d+)$")


class UpdateError(Exception):
    """code: noasset / nosha / sha / url / zip / io / net / cancel (texts: i18n upd.err.<code>)"""

    def __init__(self, code, detail=""):
        super().__init__("%s: %s" % (code, detail) if detail else code)
        self.code, self.detail = code, detail


class Release:
    def __init__(self, version, tag, page, notes, asset, sha_asset, plat):
        self.version, self.tag, self.page, self.notes = version, tag, page, notes
        self.asset, self.sha_asset, self.plat = asset, sha_asset, plat     # assets: dict name / url / size or None

    def __repr__(self):
        return "Release(%s, %s)" % (self.version, self.asset and self.asset["name"])


# ------------------------------------------------------------------------------------------------ versions
def parse_version(s):
    """'v1.2.3' / '1.2.3' -> (1, 2, 3); anything else (pre-release suffixes, two parts, text) -> None"""
    m = _VER.match(str(s or "").strip())
    return tuple(int(x) for x in m.groups()) if m else None


def is_newer(tag, current):
    a, b = parse_version(tag), parse_version(current)
    return bool(a and b and a > b)


def platform_key(name=None):
    """this system's release file kind: 'win' / 'mac' / 'linux', None = no release file for it"""
    name = name or sys.platform
    if name.startswith("win"):
        return "win"
    if name == "darwin":
        return "mac"
    if name.startswith("linux"):
        return "linux"
    return None


def asset_name(version, plat):
    tag, ext = PLATFORMS[plat]
    return NAME_FMT % (version, tag, ext)


# ------------------------------------------------------------------------------------------------ addresses
def download_url_ok(url, repo=REPO):
    """an address the API gave for one of this repository's release files"""
    try:
        u = urllib.parse.urlsplit(url)
    except ValueError:
        return False
    return (u.scheme == "https" and (u.hostname or "").lower() == DOWNLOAD_HOST and u.port in (None, 443)
            and not u.username and u.path.startswith("/%s/releases/download/" % repo) and ".." not in u.path)


def redirect_ok(url):
    try:
        u = urllib.parse.urlsplit(url)
    except ValueError:
        return False
    return u.scheme == "https" and (u.hostname or "").lower() in REDIRECT_HOSTS and u.port in (None, 443) \
        and not u.username


class _Redirects(urllib.request.HTTPRedirectHandler):
    def __init__(self, allowed):
        super().__init__()
        self.allowed = allowed

    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if not self.allowed(newurl):
            raise UpdateError("url", newurl)
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def _opener(allowed):
    return urllib.request.build_opener(_Redirects(allowed))


def _headers(version, accept):
    return {"User-Agent": "PS2RC-Converter/%s" % version, "Accept": accept}


# ------------------------------------------------------------------------------------------------ check
def evaluate(data, current, plat=None):
    """the API's JSON -> Release when it is a newer, published, non-pre-release version, else None"""
    if not isinstance(data, dict) or data.get("draft") or data.get("prerelease"):
        return None
    tag = data.get("tag_name") or ""
    if not is_newer(tag, current):
        return None
    ver = "%d.%d.%d" % parse_version(tag)
    page = data.get("html_url") or ""
    if not page.startswith(RELEASES_PAGE + "/"):
        page = RELEASES_PAGE
    plat = plat if plat is not None else platform_key()
    asset = sha = None
    if plat in PLATFORMS:
        want = asset_name(ver, plat)
        for a in data.get("assets") or ():
            if not isinstance(a, dict):
                continue
            url = a.get("browser_download_url") or ""
            if not download_url_ok(url):
                continue
            info = {"name": a.get("name"), "url": url, "size": a.get("size") or 0}
            if a.get("name") == want:
                asset = info
            elif a.get("name") == want + ".sha256":
                sha = info
    return Release(ver, tag, page, str(data.get("body") or ""), asset, sha, plat)


def fetch_latest(current, opener=None, timeout=TIMEOUT):
    """-> (data, None) or (None, 'offline' / 'limit' / 'bad'); never raises"""
    req = urllib.request.Request(API_URL, headers=_headers(current, "application/vnd.github+json"))
    op = opener or _opener(lambda u: urllib.parse.urlsplit(u).hostname == API_HOST and u.startswith("https://"))
    try:
        with op.open(req, timeout=timeout) as r:
            raw = r.read(API_MAX + 1)
        if len(raw) > API_MAX:
            return None, "bad"
        return json.loads(raw.decode("utf-8")), None
    except urllib.error.HTTPError as e:
        return None, "limit" if e.code in (403, 429) else "bad"
    except (ValueError, UnicodeDecodeError, UpdateError):
        return None, "bad"
    except Exception:              # URLError, timeouts, SSL, connection resets: all "no connection"
        return None, "offline"


def check(current, opener=None, plat=None):
    data, err = fetch_latest(current, opener)
    if err:
        return None, err
    return evaluate(data, current, plat), None


def should_check_at_start(settings):
    return bool(settings.get("update_check", True))


def should_show(rel, settings, manual=False):
    """a found release is shown unless the player skipped exactly this version (a manual check always shows it)"""
    return rel is not None and (manual or settings.get("update_skip") != rel.version)


# ------------------------------------------------------------------------------------------------ sha256
def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def parse_sha256(text, name=None):
    """'<64 hex>  <name>' (sha256sum / our release files) or a bare hash -> lower-case hex, else None"""
    for line in str(text or "").splitlines():
        m = re.match(r"^\s*([0-9a-fA-F]{64})(?:\s+\*?(.+?))?\s*$", line)
        if m and (name is None or m.group(2) is None or os.path.basename(m.group(2)) == name):
            return m.group(1).lower()
    return None


def verify_sha256(path, expected):
    return bool(expected) and sha256_file(path) == expected.lower()


# ------------------------------------------------------------------------------------------------ download
def _get(url, version, dest_f, limit, progress=None, cancel=None, opener=None, total=0):
    """stream url into the open file dest_f; -> sha256 hex of what was written"""
    if not download_url_ok(url):
        raise UpdateError("url", url)
    req = urllib.request.Request(url, headers=_headers(version, "application/octet-stream"))
    op = opener or _opener(redirect_ok)
    h = hashlib.sha256()
    done = 0
    try:
        with op.open(req, timeout=TIMEOUT) as r:
            final = r.geturl() if hasattr(r, "geturl") else url
            if final and not (final == url or redirect_ok(final)):
                raise UpdateError("url", final)
            try:
                total = int(r.headers.get("Content-Length") or total or 0)
            except (TypeError, ValueError, AttributeError):
                pass
            while True:
                if cancel is not None and cancel.is_set():
                    raise UpdateError("cancel")
                b = r.read(1 << 16)
                if not b:
                    break
                done += len(b)
                if done > limit:
                    raise UpdateError("io", "too large")
                h.update(b)
                dest_f.write(b)
                if progress:
                    progress(done, total)
    except UpdateError:
        raise
    except OSError as e:          # URLError / HTTPError / timeouts / disk
        raise UpdateError("net", str(e))
    return h.hexdigest()


def download_release(rel, folder, progress=None, cancel=None, opener=None, version=""):
    """the release file of this system into folder, verified against the release's .sha256 file -> its path.
    No .sha256 file, a different hash, a foreign address: UpdateError, and nothing is left behind."""
    if rel.asset is None:
        raise UpdateError("noasset", rel.plat or sys.platform)
    if rel.sha_asset is None:
        raise UpdateError("nosha")
    import io
    buf = io.BytesIO()
    _get(rel.sha_asset["url"], version, buf, SHA_MAX, cancel=cancel, opener=opener)
    expected = parse_sha256(buf.getvalue().decode("utf-8", "replace"), rel.asset["name"])
    if not expected:
        raise UpdateError("nosha")
    try:
        os.makedirs(folder, exist_ok=True)
    except OSError as e:
        raise UpdateError("io", str(e))
    dest = os.path.join(folder, rel.asset["name"])
    part = dest + ".part"
    try:
        with open(part, "wb") as f:
            got = _get(rel.asset["url"], version, f, ASSET_MAX, progress, cancel, opener, rel.asset.get("size") or 0)
        if got != expected:
            raise UpdateError("sha", got)
        os.replace(part, dest)
    except OSError as e:
        _remove(part)
        raise UpdateError("io", str(e))
    except BaseException:
        _remove(part)
        raise
    return dest


def _remove(p):
    try:
        os.remove(p)
    except OSError:
        pass


# ------------------------------------------------------------------------------------------------ install
def bundle_root(gui_dir=None):
    """the folder of the Windows converter (holds "PS2RC Converter.exe" and app/), None when run from a checkout"""
    gui_dir = gui_dir or os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(gui_dir))          # <bundle>/app/gui -> <bundle>
    return root if os.path.isfile(os.path.join(root, BUNDLE_EXE)) else None


def downloads_dir():
    d = os.path.join(os.path.expanduser("~"), "Downloads")
    return d if os.path.isdir(d) else os.path.expanduser("~")


def free_name(parent, base):
    """parent/base, or parent/base_2, _3 ... - a name that does not exist yet"""
    p, i = os.path.join(parent, base), 2
    while os.path.lexists(p):
        p, i = os.path.join(parent, "%s_%d" % (base, i)), i + 1
    return p


def _member_path(name):
    """a zip member name -> its parts, or None when it is unsafe (absolute, drive, .., empty parts are dropped)"""
    n = name.replace("\\", "/")
    if n.startswith("/") or re.match(r"^[A-Za-z]:", n) or "\0" in n:
        return None
    parts = [p for p in n.split("/") if p not in ("", ".")]
    if any(p == ".." or ":" in p for p in parts):
        return None
    return parts


def safe_extract(zip_path, dest):
    """unpack zip_path into the NEW folder dest (must not exist). The archive's single top folder (PS2RC_Converter/)
    is dropped. Any unsafe member (absolute path, drive, .., a symbolic link) refuses the whole archive before
    anything is written; on any error the half-written folder is removed."""
    if os.path.lexists(dest):
        raise UpdateError("io", "exists: " + dest)
    try:
        z = zipfile.ZipFile(zip_path)
    except (OSError, zipfile.BadZipFile) as e:
        raise UpdateError("zip", str(e))
    with z:
        plan = []
        for info in z.infolist():
            parts = _member_path(info.filename)
            if parts is None or stat.S_ISLNK(info.external_attr >> 16):
                raise UpdateError("zip", info.filename)
            if parts:
                plan.append((info, parts))
        tops = {p[0] for _i, p in plan}
        strip = len(tops) == 1 and all(len(p) > 1 or i.is_dir() for i, p in plan)
        tmp = dest + ".part"
        shutil.rmtree(tmp, ignore_errors=True)
        base = os.path.realpath(tmp)
        try:
            os.makedirs(tmp)
            for info, parts in plan:
                rel = parts[1:] if strip else parts
                if not rel:
                    continue
                target = os.path.realpath(os.path.join(tmp, *rel))
                if os.path.commonpath([base, target]) != base:
                    raise UpdateError("zip", info.filename)
                if info.is_dir():
                    os.makedirs(target, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with z.open(info) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
            os.rename(tmp, dest)
        except UpdateError:
            shutil.rmtree(tmp, ignore_errors=True)
            raise
        except (OSError, zipfile.BadZipFile, RuntimeError) as e:
            shutil.rmtree(tmp, ignore_errors=True)
            raise UpdateError("io", str(e))
    return dest


def install_zip(zip_path, app_root, version):
    """Windows: the new version into a NEW folder next to the running one (<parent>/PS2RC_Converter_<ver>, _2 ... when
    taken); from a checkout, or when that parent is not writable: into Downloads. The running folder and the player's
    files are never touched (settings and control profiles live in %APPDATA%\\PS2RC, shared by both versions)."""
    base = "PS2RC_Converter_%s" % version
    parents = ([os.path.dirname(app_root)] if app_root else []) + [downloads_dir()]
    last = None
    for parent in parents:
        try:
            return safe_extract(zip_path, free_name(parent, base))
        except UpdateError as e:
            if e.code != "io":
                raise
            last = e
    raise last


def new_exe(folder):
    p = os.path.join(folder, BUNDLE_EXE)
    return p if os.path.isfile(p) else None


def appimage_target(version):
    """Linux: next to the running AppImage when that folder is writable, else Downloads"""
    cur = os.environ.get("APPIMAGE")
    if cur and os.access(os.path.dirname(cur) or ".", os.W_OK):
        return os.path.dirname(cur)
    return downloads_dir()


def make_executable(path):
    os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def download_folder(plat):
    """where the downloaded file goes: Windows a temporary folder (unpacked next to the app, then deleted), macOS
    Downloads, Linux next to the running AppImage (or Downloads)"""
    if plat == "win":
        return tempfile.mkdtemp(prefix="ps2rc_update_")
    if plat == "linux":
        return appimage_target(None)
    return downloads_dir()
