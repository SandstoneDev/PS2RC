"""The player's control mapping as the converter's controls page sees it (no Qt): the same rules as the runtime's
hle/psp/hle_padmap.c (keys, presets, combinations, checks), so the page shows what the game will do and writes only
values the game accepts.

Model: every PS2 input (16 buttons, right stick, left stick) is produced by up to three alternatives, each a
combination of 1-3 PSP buttons (+ the analog stick: analog, analog_left/right/up/down; + tap / hold). Two layers: on
foot (map_<input>) and in a vehicle (car_<input>; unset = the map_ value or the preset's vehicle layer). The page
edits the layers; `to_values()` gives the ps2rc.cfg keys that differ from the preset (nothing for the built-in
layout). tests/gui/test_controls_model.py checks the texts against the runtime's own "controls:" lines.
"""

INPUTS = ("l2", "r2", "l3", "r3", "rstick", "l1", "r1", "select", "start", "cross", "circle", "square", "triangle",
          "up", "down", "left", "right", "lstick")
STICKS = ("rstick", "lstick")
BUTTON_INPUTS = tuple(i for i in INPUTS if i not in STICKS)
PSP_BUTTONS = ("cross", "circle", "square", "triangle", "l", "r", "up", "down", "left", "right", "select", "start")
PRESETS = ("default", "stories")
OLD_PRESET_NAMES = {"vcs": "stories"}             # an earlier name: accepted when read, never written or shown
MODS = ("select", "l", "r", "none")

# PSP bits (= hle_padmap.h); the name order is the runtime's print order
NAMES = (("select", 0x0001), ("start", 0x0008), ("l", 0x0100), ("r", 0x0200), ("up", 0x0010), ("down", 0x0040),
         ("left", 0x0080), ("right", 0x0020), ("triangle", 0x1000), ("circle", 0x2000), ("cross", 0x4000),
         ("square", 0x8000), ("home", 0x10000), ("dpad", 0x0004), ("analog", 0x0002),
         ("analog_left", 0x100000), ("analog_right", 0x200000), ("analog_up", 0x400000), ("analog_down", 0x800000),
         ("tap", 0x1000000), ("hold", 0x2000000))
BIT = dict(NAMES)
PHYS = 0xF3F9
ASTICK, ADIRS, DPAD_TOK, TAP, HOLD, HOME = 0x0002, 0xF00000, 0x0004, 0x1000000, 0x2000000, 0x10000
DPAD = BIT["up"] | BIT["down"] | BIT["left"] | BIT["right"]
FPS_COMBO = BIT["l"] | BIT["r"] | BIT["select"]
SYS_COMBO = BIT["l"] | BIT["r"] | BIT["down"] | BIT["start"]
FLAGS = ("", "tap", "hold")                       # an alternative's flag
UNSET_WORDS = ("", "foot")                     # a car_ value meaning "as on foot" (= key not set)

STORIES = (
    {"l2": "left", "r2": "right", "l3": "down", "r3": "l+r+hold", "rstick": "l+analog", "l1": "l+hold", "r1": "r",
     "select": "select", "start": "start", "cross": "cross", "circle": "circle", "square": "square",
     "triangle": "triangle", "up": "up", "down": "l+down", "left": "l+left", "right": "l+right", "lstick": "analog"},
    {"l2": "l+analog_left/l+analog_down", "r2": "l+analog_right/l+analog_down", "l3": "down", "r3": "up",
     "rstick": "l+dpad", "l1": "l+hold", "r1": "r", "select": "select", "start": "start", "cross": "cross",
     "circle": "circle", "square": "square", "triangle": "triangle", "up": "right", "down": "left",
     "left": "l+r+left", "right": "l+r+right", "lstick": "analog"},
)
# every key this page owns (ps2rc.cfg friendly names)
CAM_INV = ("off", "x", "y", "xy")                # camera_invert: the analog-stick camera axes reversed
KEYS = ("controls_preset", "modifier", "select_to_game", "select_up", "camera_invert") + tuple("map_" + i for i in INPUTS) + \
    tuple("car_" + i for i in INPUTS)


def popc(v):
    return bin(v).count("1")


def chord_name(c):
    if not c:
        return "none"
    if c & ADIRS:
        c &= ~ASTICK
    return "+".join(n for n, b in NAMES if c & b)


def parse_combo(text):
    """'a+b' (one alternative) -> bits incl. the dpad / tap / hold tokens; ValueError(why)"""
    s = "".join(text.split()).lower()
    if not s:
        raise ValueError("empty")
    out = 0
    parts = s.split("+")
    for k, p in enumerate(parts):
        if not p:
            raise ValueError("ends with +" if k == len(parts) - 1 else "unknown button name")
        if p not in BIT:
            raise ValueError("unknown button name")
        if out & BIT[p]:
            raise ValueError("a button twice")
        out |= BIT[p]
    if out & HOME:
        raise ValueError("HOME belongs to the system")
    if out & ADIRS:
        out |= ASTICK
    return out


def reserved(c):
    c &= PHYS
    if c & FPS_COMBO == FPS_COMBO:
        return "contains L+R+SELECT (the fps counter)"
    if popc(c) == 3 and not c & ~SYS_COMBO:
        return "three of L+R+Down+START (the system menu)"
    return None


class Stick:
    """a stick's value: mode 'none' | 'dpad' | 'analog', the button(s) held with it"""
    def __init__(self, mode="none", bits=0):
        self.mode, self.bits = mode, bits

    def __eq__(self, o):
        return isinstance(o, Stick) and (self.mode, self.bits) == (o.mode, o.bits)

    def copy(self):
        return Stick(self.mode, self.bits)


def parse_value(inp, text, mod=0):
    """a cfg value of an input -> list of (bits, flag) alternatives, or a Stick; ValueError(why)"""
    s = "".join(str(text).split()).lower()
    if s in ("none", "off"):
        return Stick() if inp in STICKS else []
    if not s:
        raise ValueError("empty")
    if inp in STICKS:
        if "/" in s:
            raise ValueError("one value only for a stick")
        c = parse_combo(s)
        if c & (TAP | HOLD):
            raise ValueError("tap / hold not for a stick")
        how, b = c & (DPAD_TOK | ASTICK | ADIRS), c & PHYS
        if inp == "lstick":
            if c == ASTICK:
                return Stick("analog", 0)
            if how != DPAD_TOK or popc(b) > 1 or b & DPAD:
                raise ValueError("use analog, dpad, <button>+dpad or none")
            return Stick("dpad", b)
        if c == ASTICK:
            mb = (BIT["select"], BIT["l"], BIT["r"], 0)[mod]
            if not mb:
                raise ValueError("analog needs a modifier (modifier=none)")
            return Stick("analog", mb)
        if how not in (DPAD_TOK, ASTICK) or b & DPAD or popc(b) > 2 or (how == ASTICK and not b):
            raise ValueError("use <button>+dpad, <button>+analog, analog or none")
        if how == DPAD_TOK:
            for d in ("left", "right", "up", "down"):
                if reserved(b | BIT[d]):
                    raise ValueError(reserved(b | BIT[d]))
        elif reserved(b):
            raise ValueError(reserved(b))
        return Stick("dpad" if how == DPAD_TOK else "analog", b)
    out = []
    parts = s.split("/")
    for k, p in enumerate(parts):
        if len(out) >= 3:
            raise ValueError("more than three alternatives")
        if not p:
            raise ValueError("ends with /" if k == len(parts) - 1 else "empty")
        c = parse_combo(p)
        phys = c & PHYS
        if c & DPAD_TOK:
            raise ValueError("dpad only for map_rstick / map_lstick")
        if c & TAP and c & HOLD:
            raise ValueError("tap and hold together")
        if not phys:
            raise ValueError("needs a button")
        if popc(phys) > 3:
            raise ValueError("more than three buttons")
        if reserved(phys):
            raise ValueError(reserved(phys))
        ch = c & ~(TAP | HOLD)
        if any(x == ch for x, _f in out):
            raise ValueError("the same combination twice")
        out.append((ch, "tap" if c & TAP else "hold" if c & HOLD else ""))
    return out


def fmt_value(inp, v):
    """the runtime's text of an input value (the "controls:" line format)"""
    if inp in STICKS:
        if v.mode == "none":
            return "none"
        if v.mode == "analog" and not v.bits:
            return "analog"
        if not v.bits:
            return "dpad"
        return "%s+%s" % (chord_name(v.bits), v.mode)
    if not v:
        return "none"
    return "/".join(chord_name(c) + ("+" + f if f else "") for c, f in v)


def combo_text(c, flag=""):
    return chord_name(c) + ("+" + flag if flag else "")


def def_text(preset, mod, layer, inp):
    if preset == "stories":
        return STORIES[layer][inp]
    m = MODS[mod]
    if inp in ("l2", "r2", "l3", "r3", "rstick") and m == "none":
        return "none"
    return {"l2": "l+select" if m == "l" else m + "+l", "r2": "r+select" if m == "r" else m + "+r",
            "l3": m + "+triangle", "r3": m + "+circle", "rstick": m + "+dpad", "l1": "l", "r1": "r",
            "lstick": "analog"}.get(inp, inp)


def _entries(layer, sel_up=None):
    """[(chord, owner)] of a layer, as the runtime builds them"""
    out = []
    for i in BUTTON_INPUTS:
        for c, _f in layer[i]:
            out.append((c, i))
    for s in STICKS:
        v = layer[s]
        if v.mode == "dpad":
            for d in ("left", "right", "up", "down"):
                out.append((v.bits | BIT[d], s))
        elif v.mode == "analog" and s == "rstick":
            out.append((v.bits | ASTICK, s))
    return out


def _copy_layer(ly):
    return {i: (ly[i].copy() if i in STICKS else list(ly[i])) for i in INPUTS}


class Layout:
    """the effective mapping of a values dict (friendly ps2rc.cfg keys), as the runtime resolves it"""

    def __init__(self, values=None):
        # car_<input>=foot (or empty) = not set: the on-foot value applies (the in-game menu writes it while a value
        # is unsaved; Save comments the line out) - the same rule as hle_padmap.c car_get
        self.values = {k: v for k, v in (values or {}).items()
                       if k in KEYS and not (k.startswith("car_") and str(v).strip().lower() in UNSET_WORDS)}
        self.notes = []
        self._resolve()

    # ---- the runtime's resolution (hle_padmap.c padmap_config)
    def _resolve(self):
        v = self.values
        p = str(v.get("controls_preset", "default")).lower()
        p = OLD_PRESET_NAMES.get(p, p)
        self.preset = p if p in PRESETS else "default"
        self.mod = 0
        if self.preset == "stories":
            self.mod = 1
        elif str(v.get("modifier", "")).lower() in MODS:
            self.mod = MODS.index(str(v["modifier"]).lower())
        self.sel_tap = str(v.get("select_to_game", "")).lower() == "tap"
        ci = str(v.get("camera_invert", "off")).strip().lower()
        ci = CAM_INV[int(ci)] if ci in ("0", "1", "2", "3") else ci
        self.cam_inv = ci if ci in CAM_INV else "off"
        self.has_car = self.preset == "stories" or any(("car_" + i) in v for i in INPUTS)
        self.defaults, self.layers = [], []
        for L in (0, 1):
            d = {i: parse_value(i, def_text(self.preset, self.mod, L, i), self.mod) for i in INPUTS}
            ly, user = _copy_layer(d), {}
            for i in INPUTS:
                key = "car_" + i if L and ("car_" + i) in v else "map_" + i
                if key not in v:
                    continue
                try:
                    ly[i] = parse_value(i, v[key], self.mod)
                    user[i] = fmt_value(i, ly[i]) != fmt_value(i, d[i])
                except ValueError as e:
                    self.notes.append("%s=%s: %s" % (key, v[key], e))
            for _guard in range(4 * len(INPUTS)):
                ents, victim = _entries(ly), None
                for a in range(len(ents)):
                    for b in range(a + 1, len(ents)):
                        (ca, oa), (cb, ob) = ents[a], ents[b]
                        if ca != cb or oa == ob:
                            continue
                        if user.get(ob):
                            victim, other = ob, oa
                        elif user.get(oa):
                            victim, other = oa, ob
                        else:
                            continue
                        break
                    if victim:
                        break
                if not victim:
                    break
                self.notes.append("%s: same buttons as %s" % (victim, other))
                ly[victim] = d[victim].copy() if victim in STICKS else list(d[victim])
                user[victim] = False
            self.defaults.append(d)
            self.layers.append(ly)
        if not self.has_car:
            self.layers[1] = _copy_layer(self.layers[0])

    # ---- the values that give this mapping
    def to_values(self):
        out = {}
        if "modifier" in self.values and self.preset == "default":
            out["modifier"] = self.values["modifier"]
        if "select_up" in self.values:
            out["select_up"] = self.values["select_up"]
        if self.preset != "default":
            out["controls_preset"] = self.preset
        if self.sel_tap:
            out["select_to_game"] = "tap"
        if self.cam_inv != "off":
            out["camera_invert"] = self.cam_inv
        for i in INPUTS:
            t0, d0 = fmt_value(i, self.layers[0][i]), fmt_value(i, self.defaults[0][i])
            if t0 != d0:
                out["map_" + i] = t0
            t1 = fmt_value(i, self.layers[1][i])
            inherited = t0 if t0 != d0 else fmt_value(i, self.defaults[1][i])
            if t1 != inherited:
                out["car_" + i] = t1
        return out

    def line(self):
        """the runtime's "controls:" line (without the * / ! marks)"""
        parts = ["controls:"]
        if "controls_preset" in self.values:
            parts.append("controls_preset=" + self.preset)
        parts.append("modifier=" + MODS[self.mod])
        parts.append("select_to_game=" + ("tap" if self.sel_tap else "always"))
        for i in INPUTS:
            t = fmt_value(i, self.layers[0][i])
            if INPUTS.index(i) >= INPUTS.index("cross") and ("map_" + i) not in self.values and \
                    t == def_text("default", 0, 0, i):
                continue
            parts.append("map_%s=%s" % (i, t))
        if self.cam_inv != "off":
            parts.append("camera_invert=" + self.cam_inv)
        return " ".join(parts)

    def car_line(self):
        if not self.has_car:
            return ""
        parts = ["controls in a vehicle:"]
        for i in INPUTS:
            t0, t1 = fmt_value(i, self.layers[0][i]), fmt_value(i, self.layers[1][i])
            if ("car_" + i) in self.values or t0 != t1:
                parts.append("car_%s=%s" % (i, t1))
        if len(parts) == 1:
            parts.append("(as on foot)")
        return " ".join(parts)

    # ---- what the page shows
    def button_rows(self, L):
        """{psp button: (input, flag) or None}: what each PSP button gives alone"""
        ly, out = self.layers[L], {}
        for b in PSP_BUTTONS:
            out[b] = None
            for i in BUTTON_INPUTS:
                for c, f in ly[i]:
                    if c == BIT[b]:
                        out[b] = (i, f)
        return out

    def combo_rows(self, L):
        """[(combination text, input, flag)]: the combinations of 2-3 buttons / with the analog stick"""
        out = []
        for i in BUTTON_INPUTS:
            for c, f in self.layers[L][i]:
                if popc(c & PHYS) > 1 or c & (ASTICK | ADIRS):
                    out.append((chord_name(c), i, f))
        return out

    # ---- edits (each keeps the layer free of conflicts: the newest binding wins)
    def _changed(self, L, before):
        """an on-foot edit follows into the vehicle layer for the inputs that were equal there"""
        if L == 0:
            after, car = self.layers[0], self.layers[1]
            pairs = set(_entries(self.defaults[1])) if self.defaults else set()
            olds = {}
            for i in INPUTS:
                if fmt_value(i, before[i]) != fmt_value(i, after[i]) and \
                        fmt_value(i, car[i]) == fmt_value(i, before[i]):
                    olds[i] = car[i]
                    car[i] = after[i].copy() if i in STICKS else list(after[i])
            for _guard in range(len(INPUTS)):              # one that would take a vehicle button: on foot only
                bad = [i for i in olds if any(
                    c in {x for x, p in _entries(car) if p == i} and o != i and
                    not ((c, o) in pairs and (c, i) in pairs) for c, o in _entries(car))]
                if not bad:
                    break
                car[bad[0]] = olds.pop(bad[0])
            self._dedupe(1, None, ())
        self._commit()

    def _dedupe(self, L, keep, chords):
        """remove `chords` (and duplicates in general) from every input but `keep`"""
        ly, notes = self.layers[L], []
        for i in BUTTON_INPUTS:
            if i == keep:
                continue
            new = [(c, f) for c, f in ly[i] if c not in chords]
            if len(new) != len(ly[i]):
                notes.append(i)
                ly[i] = new
        for s in STICKS:
            if s == keep:
                continue
            if any(c in chords for c, o in _entries(ly) if o == s):
                ly[s] = Stick()
                notes.append(s)
        seen, ents = {}, _entries(ly)
        preset_pairs = set(_entries(self.defaults[L])) if self.defaults else set()
        for c, o in ents:                                 # leftovers (e.g. a stick on the D-pad): older gives way
            if c in seen and seen[c] != o and not ((c, o) in preset_pairs and (c, seen[c]) in preset_pairs):
                loser = o if o != keep else seen[c]
                if loser in STICKS:
                    ly[loser] = Stick()
                else:
                    ly[loser] = [(x, f) for x, f in ly[loser] if x != c]
                notes.append(loser)
            seen[c] = o
        return notes

    def set_button(self, L, button, inp, flag=""):
        """PSP `button` pressed alone gives `inp` (None = nothing)"""
        before, c, ly = _copy_layer(self.layers[L]), BIT[button], self.layers[L]
        moved = [i for i in BUTTON_INPUTS if i != inp and any(x == c for x, _f in ly[i])]
        for i in BUTTON_INPUTS:
            ly[i] = [(x, f) for x, f in ly[i] if x != c]
        moved += self._dedupe(L, inp, (c,))
        if inp:
            ly[inp] = (ly[inp] + [(c, flag)])[-3:]
        self._changed(L, before)
        return moved

    def set_combo(self, L, old, text, inp, flag=""):
        """replace the combination row `old` ((text, input, flag) or None) by `text` -> `inp`; ValueError(why)"""
        before, ly = _copy_layer(self.layers[L]), self.layers[L]
        new = parse_value(inp or "l2", text + ("+" + flag if flag else ""), self.mod) if text else []
        if old:
            oc = parse_combo(old[0])
            ly[old[1]] = [(x, f) for x, f in ly[old[1]] if x != oc]
        moved = []
        if new and inp:
            c, f = new[0]
            moved = self._dedupe(L, inp, (c,))
            ly[inp] = [(x, g) for x, g in ly[inp] if x != c][-2:] + [(c, f)]
        self._changed(L, before)
        return moved

    def set_stick(self, L, stick, text):
        before, ly = _copy_layer(self.layers[L]), self.layers[L]
        v = parse_value(stick, text, self.mod)
        ly[stick] = v
        chords = tuple(c for c, o in _entries(ly) if o == stick)
        moved = self._dedupe(L, stick, chords)
        self._changed(L, before)
        return moved

    def set_preset(self, preset):
        keep = {k: self.values[k] for k in ("select_to_game", "select_up", "camera_invert") if k in self.values}
        if preset != "default":
            keep["controls_preset"] = preset
        self.values = keep
        self.notes = []
        self._resolve()

    def set_select_tap(self, on):
        self.sel_tap = bool(on)
        self._commit()

    def set_camera_invert(self, value):
        """off | x | y | xy: the camera on the analog stick (stories: L + stick) with reversed axes"""
        v = str(value).lower()
        if v not in CAM_INV:
            raise ValueError("camera_invert: use off, x, y or xy")
        self.cam_inv = v
        self._commit()

    def copy_foot_to_vehicle(self):
        self.layers[1] = _copy_layer(self.layers[0])
        self._commit()

    def restore(self):
        self.set_preset(self.preset)

    def _commit(self):
        """re-read the values these layers give (so the page always shows what the runtime will do)"""
        self.values = self.to_values()
        self.notes = []
        self._resolve()


def is_control_key(k):
    return k in KEYS
