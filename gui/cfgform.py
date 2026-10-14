"""The player's settings file (the game pack's cfg template) as a form: groups, settings, the kind of control each
one gets, the template's own values as "our defaults".

The template is plain text the game reads: `key=value` lines, `#key=value` = a setting left at the built-in default,
`#` comment lines above a setting describe it ("key - what it does. 10 to 100. Original: 100. Ours: 40."), a group
header is a comment line between two `# ----` rulers. The kind of control comes from that description:
    "1 = on, 0 = off" (any words)          -> check box
    "10 to 100"                             -> slider with that range (unit from "percent" / "metres" / ...)
    "full = ..., half = ..., low = ..."     -> drop-down (words);  "30, 25, 20, 15 or 60" -> drop-down (numbers)
    anything else                           -> text field
A "RISK" sentence in the description asks the player to confirm a change. The control kinds come from the
English template; another language's template only gives its descriptions (merge_descriptions).
"""
import re

RULER = re.compile(r"^#\s*-{8,}\s*$")
SETTING = re.compile(r"^([A-Za-z_][A-Za-z0-9_]*)\s*=\s*([^#]*?)\s*(?:#.*)?$")
COMMENTED = re.compile(r"^#(?!\s)([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(\S[^#]*?)\s*$")
RANGE = re.compile(r"(?<![\w.-])(-?\d+)\s+(?:to|-)\s+(-?\d+)(?![\d\w]|\.\d)")
PAIR = re.compile(r"(?<![\w=])([A-Za-z0-9]+)\s+=\s+")
NUMLIST = re.compile(r"((?:\d+(?:\s*\([^)]*\))?\s*,\s*)*\d+(?:\s*\([^)]*\))?\s+or\s+\d+)")
UNITS = (("percent", "%"), ("centimetre", "cm"), ("metre", "m"), ("mhz", "MHz"))
ON_WORDS = ("on", "yes", "drawn", "play")


class Group:
    kind = "group"

    def __init__(self, title):
        self.title = title


class Setting:
    def __init__(self, key, value, commented, comment):
        self.key, self.value, self.commented, self.comment = key, value, commented, comment
        self.default = None if commented else value       # "our default" = the template's value
        self.kind, self.options, self.range, self.unit, self.risk = "text", [], None, "", ""
        self._classify()

    @property
    def label(self):
        return self.key.replace("_", " ").capitalize()

    @property
    def text(self):
        return " ".join(self.comment)

    @property
    def summary(self):
        """the description without the leading 'key - ' and without the risk part"""
        t = re.sub(r"^%s\s*[-:]\s*" % re.escape(self.key), "", self.text)
        return t.split("RISK")[0].strip()

    def _classify(self):
        t = self.text
        low = t.lower()
        if "risk" in low:
            self.risk = t[low.find("risk"):].strip()
        for word, unit in UNITS:
            if word in low:
                self.unit = unit
                break
        pairs = [w for w in PAIR.findall(t) if w.lower() != self.key.lower()]
        words = []
        for w in pairs:
            if w not in words:
                words.append(w)
        if set(words) == {"0", "1"} or (len(words) == 2 and "1" in words and "0" in words):
            self.kind, self.options = "bool", ["0", "1"]
            return
        r = RANGE.search(t)
        if r and (self.value is None or re.fullmatch(r"-?\d+", self.value or "")):
            lo, hi = int(r.group(1)), int(r.group(2))
            if lo < hi:
                self.kind, self.range = "range", (lo, hi)
                return
        nums = []
        m = NUMLIST.search(t)
        if m:
            nums = re.findall(r"\d+", re.sub(r"\([^)]*\)", "", m.group(1)))
        opts = []
        for w in words + nums:
            if w not in opts:
                opts.append(w)
        if len(opts) >= 2:
            if self.value is not None and self.value not in opts:
                opts.append(self.value)
            self.kind, self.options = "choice", opts

    def normalize(self, v):
        """a value from a control -> the text written to the file"""
        v = str(v).strip()
        if self.kind == "range":
            try:
                n = int(round(float(v)))
            except ValueError:
                return self.default or ""
            return str(min(max(n, self.range[0]), self.range[1]))
        if self.kind == "bool":
            return "1" if v.lower() in ("1", "true") + ON_WORDS else "0"
        return v


def parse(text):
    """-> list of Group / Setting in file order"""
    out, block = [], []
    lines = text.replace("\r\n", "\n").split("\n")
    i = 0
    while i < len(lines):
        ln = lines[i].strip().lstrip("\ufeff")
        if RULER.match(ln):
            # header: ruler, '#  NAME', ruler
            if i + 2 < len(lines) and RULER.match(lines[i + 2].strip()):
                name = lines[i + 1].strip().lstrip("#").strip()
                if name and name == name.upper():  # group names are capitals; the file's title block is not
                    out.append(Group(name))
                i += 3
                block = []
                continue
            i += 1
            block = []
            continue
        if not ln:
            block = []
            i += 1
            continue
        m = COMMENTED.match(ln)
        if m:
            out.append(Setting(m.group(1), m.group(2), True, block))
            block = []
            i += 1
            continue
        if ln.startswith("#") or ln.startswith(";"):
            block.append(ln.lstrip("#;").strip())
            i += 1
            continue
        m = SETTING.match(ln)
        if m:
            out.append(Setting(m.group(1), m.group(2), False, block))
        block = []
        i += 1
    return out


def settings(items):
    return [s for s in items if isinstance(s, Setting)]


def merge_descriptions(items, other):
    """take the comment text of `other` (the same template in another language) for keys both have"""
    by_key = {s.key: s for s in settings(other)}
    for s in settings(items):
        o = by_key.get(s.key)
        if o and o.comment:
            s.comment_local = o.comment
    groups, other_groups = [g for g in items if isinstance(g, Group)], [g for g in other if isinstance(g, Group)]
    if len(groups) == len(other_groups):
        for g, o in zip(groups, other_groups):
            g.title_local = o.title
    return items


def overrides(items, values):
    """values {key: value from the form} -> {key: text} only where it differs from the template ("our default");
    a commented setting set to its 'not set' choice is not written"""
    out = {}
    for s in settings(items):
        if s.key not in values:
            continue
        v = values[s.key]
        if v is None or v == "":
            continue
        v = s.normalize(v)
        if v != (s.default if s.default is not None else None):
            out[s.key] = v
    return out


def apply_to_text(text, values):
    """write `values` into the template text itself: the value on the setting's own line is replaced (a commented
    `#key=` line is uncommented); keys the template does not have are appended at the end."""
    lines = text.replace("\r\n", "\n").split("\n")
    done = set()
    for i, ln in enumerate(lines):
        s = ln.strip()
        m = SETTING.match(s) or COMMENTED.match(s)
        if m and m.group(1) in values and m.group(1) not in done:
            lines[i] = "%s=%s" % (m.group(1), values[m.group(1)])
            done.add(m.group(1))
    rest = [k for k in values if k not in done]
    if rest:
        if lines and lines[-1] != "":
            lines.append("")
        lines += ["%s=%s" % (k, values[k]) for k in rest]
    return "\n".join(lines)
