"""The PSP runtime bakes no CPU clock: source checks.

- the only scePower clock setter call in the runtime is the one in hle_aw_clock_policy (hle/psp/hle_aw_psp.c), and it
  can only set 333 (Sony's maximum), never more;
- no runtime file writes the PLL / clock-domain registers (own PLL writes switched consoles off);
- ARK's ladder (sctrlHENSetSpeed) is reached only after the PS Vita check;
- the player templates keep overclock=0 (off) and warn about the per-console limit and the PS Vita.
"""
import os
import re

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
PACKS = [os.path.join(ROOT, "games", d) for d in sorted(os.listdir(os.path.join(ROOT, "games")))
         if os.path.isfile(os.path.join(ROOT, "games", d, "cfg", "player_en.cfg"))]
SETTERS = re.compile(r"\bscePowerSet(?:Clock|CpuClock|BusClock)Frequency\s*\(")
REG_WRITE = re.compile(r"(?:HW|hw)\s*\(\s*0x(?:bc1000fc|bc100068|bc200000|bc200004)u?\s*\)\s*=[^=]", re.I)


def _sources():
    for top in ["hle"] + [os.path.relpath(os.path.join(p, "src"), ROOT) for p in PACKS]:
        for root, _d, files in os.walk(os.path.join(ROOT, top)):
            for f in files:
                if f.endswith((".c", ".h", ".S", ".s")):
                    p = os.path.join(root, f)
                    yield os.path.relpath(p, ROOT).replace("\\", "/"), open(p, encoding="utf-8", errors="replace").read()


def _strip_comments(t):
    """comments and the text of string literals out (log lines may name the functions)"""
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    t = re.sub(r"//[^\n]*", "", t)
    return re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', t)


def _function(text, name):
    m = re.search(r"\n[a-z][^\n;]*\b%s\s*\([^\n;]*\)\s*\{" % re.escape(name), text)
    assert m, name
    depth, i = 0, m.end() - 1
    while True:
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[m.start():i + 1]
        i += 1


def test_one_clock_setter_call_and_only_333():
    calls = []
    for rel, t in _sources():
        for m in SETTERS.finditer(_strip_comments(t)):
            calls.append((rel, m.group(0)))
    assert [c[0] for c in calls] == ["hle/psp/hle_aw_psp.c"], calls
    aw = _strip_comments(open(os.path.join(ROOT, "hle", "psp", "hle_aw_psp.c"), encoding="utf-8").read())
    body = _function(aw, "hle_aw_clock_policy")
    assert "scePowerSetClockFrequency(333, 333, 166)" in body
    assert not SETTERS.search(aw.replace(body, ""))


def test_no_pll_register_writes():
    hits = [rel for rel, t in _sources() if REG_WRITE.search(_strip_comments(t))]
    assert hits == [], hits


def test_ark_ladder_only_after_vita_check():
    aw = _strip_comments(open(os.path.join(ROOT, "hle", "psp", "hle_aw_psp.c"), encoding="utf-8").read())
    for fn in ("hle_aw_auto_boot", "hle_aw_preclock", "hle_aw_clock_boot"):
        body = _function(aw, fn)
        assert "at_sctrlHENSetSpeed(" in body, fn
        assert body.index("cp_vita") < body.index("at_sctrlHENSetSpeed("), fn
    calls = [rel for rel, t in _sources() if "at_sctrlHENSetSpeed(" in _strip_comments(t).replace(
        "void at_sctrlHENSetSpeed(", "")]
    assert calls == ["hle/psp/hle_aw_psp.c"], calls


def test_policy_skips_vita_and_cfw_setting():
    aw = _strip_comments(open(os.path.join(ROOT, "hle", "psp", "hle_aw_psp.c"), encoding="utf-8").read())
    body = _function(aw, "hle_aw_clock_policy")
    # PS Vita and cpu_clock_base=off come before any branch that sets want = 1
    first_want = body.index("want = 1")
    assert body.index("cp_vita") < first_want and body.index("cp_base == 2") < first_want
    # the CFW's own setting and "already at 333" end without a call (no want = 1 in those branches)
    cfw = body[body.index("else if (cfw)"):body.index("else if (cur >= 333)")]
    assert "want = 1" not in cfw
    psp = _strip_comments(open(os.path.join(ROOT, "hle", "psp", "hle_psp.c"), encoding="utf-8").read())
    main = _function(psp, "main")
    assert main.index("hle_aw_clock_entry(") < main.index("hle_aw_auto_boot(") < main.index("hle_aw_clock_policy(")


def test_templates_default_off_and_warn():
    packs = [p for p in PACKS if "overclock" in open(os.path.join(p, "cfg", "player_en.cfg"), encoding="utf-8").read()]
    assert packs
    for p in packs:
        en = open(os.path.join(p, "cfg", "player_en.cfg"), encoding="utf-8").read()
        ru = open(os.path.join(p, "cfg", "player_ru.cfg"), encoding="utf-8").read()
        assert re.search(r"^overclock=0$", en, re.M) and re.search(r"^overclock=0$", ru, re.M)
        assert "only 333 MHz is guaranteed" in en and "PS Vita" in en and "ARK" in en
        assert "333" in ru and "PS Vita" in ru and "ARK" in ru


def test_cfw_stubs_in_order():
    """each import entry: its stubs' NIDs and its NID table in the same order (psp-fixup-imports pairs them)"""
    t = open(os.path.join(ROOT, "hle", "psp", "hle_mem_psp.c"), encoding="utf-8").read()
    stubs = re.findall(r'\.word (at_\w+_stub_entry)\\n"\s*"\s*\.word (0x[0-9A-Fa-f]{8})\\n"', t)
    tables = [re.findall(r"0x[0-9A-Fa-f]{8}", b) for b in
              re.findall(r"\.pushsection \.rodata\.sceNid.*?\.popsection", t, flags=re.S)]
    by_entry = {}
    for e, nid in stubs:
        by_entry.setdefault(e, []).append(nid)
    assert [len(v) for v in by_entry.values()] == [4, 1], by_entry
    assert list(by_entry.values()) == tables, (by_entry, tables)
