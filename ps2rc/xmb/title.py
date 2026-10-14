"""The menu title (PARAM.SFO TITLE): UTF-8, one line, at most 127 bytes (the field holds 128 with its zero).

Which characters the PSP menu font draws is Unknown here beyond plain Latin letters, digits and common punctuation;
other characters are allowed but flagged, so the player can decide. How many characters fit on one line of the menu
is Unknown too (a long title may be cut or wrap); titles longer than LONG are flagged as a hint only.
"""
import re
import unicodedata

from ps2rc.pbp.spec import TITLE_MAX

LONG = 40
SAFE = re.compile(r"[A-Za-z0-9 !\"#$%&'()*+,\-./:;<=>?@\[\]^_`{|}~]")
CONTROL = re.compile(r"[\x00-\x1f\x7f-\x9f  ]")


def clean_title(text):
    """one line, no control characters, single spaces, trimmed"""
    text = CONTROL.sub(" ", text or "")
    return re.sub(r"\s+", " ", unicodedata.normalize("NFC", text)).strip()


def fit_title(text, limit=TITLE_MAX):
    """cut on a character boundary so the UTF-8 text is at most `limit` bytes"""
    out = text
    while len(out.encode("utf-8")) > limit:
        out = out[:-1]
    return out


def check_title(text):
    """-> list of (level, code, detail): level 'error' (cannot be used), 'warn' (may look wrong), 'info'.
    code is a stable id the GUI translates; detail carries numbers / characters."""
    out = []
    if text is None or not text.strip():
        return [("error", "empty", "")]
    if CONTROL.search(text):
        out.append(("error", "control", ""))
    n = len(text.encode("utf-8"))
    if n > TITLE_MAX:
        out.append(("error", "too_long", "%d/%d" % (n, TITLE_MAX)))
    if text != text.strip() or "  " in text:
        out.append(("warn", "spaces", ""))
    odd = sorted({ch for ch in text if not SAFE.match(ch) and not CONTROL.match(ch)})
    if odd:
        out.append(("warn", "chars", "".join(odd[:12])))
    if len(text) > LONG:
        out.append(("info", "long", "%d" % len(text)))
    return out


def usable(text):
    return not [c for c in check_title(text) if c[0] == "error"]
