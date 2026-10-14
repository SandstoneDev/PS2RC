"""A synthetic checkout for the converter tests: games/<pack>/pack.toml + convert.toml for the synthetic disc of
isobuild.ps2_files(), and tiny scripts standing in for the analysis / translation / extras commands."""
import hashlib
import os
import textwrap

PACK = "SLES-12345_v1.00"

SCRIPT = """import os, sys, json
work = os.environ["PS2RC_WORK"]
disc = os.environ["PS2RC_DISC"]
elf = open(os.path.join(disc, "SLES_123.45"), "rb").read()
what = sys.argv[1]
if what == "analysis":
    os.makedirs(os.path.join(work, "phase3"), exist_ok=True)
    open(os.path.join(work, "phase3", "functions.txt"), "w").write("%d bytes\\n" % len(elf))
    open(os.path.join(work, "counter_analysis"), "a").write("x")
elif what == "translate":
    out = sys.argv[sys.argv.index("--out") + 1]
    os.makedirs(out, exist_ok=True)
    json.dump({"variant": "v5s4y2", "function_set": "x/v2", "exempt": {"main": [1, 2]}}, open(os.path.join(out, "stats.json"), "w"))
    open(os.path.join(out, "unit_000.c"), "w").write("int f(void) { return %d; }\\n" % len(elf))
    open(os.path.join(work, "counter_translate"), "a").write("x")
elif what == "extras":
    an = os.environ["PS2RC_ANALYSIS"]
    assert os.path.exists(os.path.join(an, "matches.json")), "analysis file not copied"
    os.makedirs(os.path.join(work, "phase6psp"), exist_ok=True)
    open(os.path.join(work, "phase6psp", "trapset.txt"), "w").write("fn_1\\n")
elif what == "fail":
    print("something went wrong on purpose")
    sys.exit(3)
print("fake", what, "ok")
"""


def make_repo(root, elf, recipe_extra="", analysis_cmd='["{repo}/scripts/fake.py", "analysis"]'):
    games = os.path.join(root, "games", PACK)
    os.makedirs(games, exist_ok=True)
    os.makedirs(os.path.join(root, "scripts"), exist_ok=True)
    with open(os.path.join(root, "scripts", "fake.py"), "w", newline="\n") as f:
        f.write(SCRIPT)
    sha = hashlib.sha256(elf).hexdigest()
    with open(os.path.join(games, "pack.toml"), "w", newline="\n") as f:
        f.write(textwrap.dedent("""\
            format = 1
            [id]
            serial = "SLES-12345"
            version = "1.00"
            region = "PAL"
            boot_elf = "SLES_123.45"
            psp_title = "Test game"
            [verify]
            system_cnf_boot2 = 'cdrom0:\\SLES_123.45;1'
            elf_size = %d
            elf_sha256 = "%s"
            [overlays]
            files = []
            [runtime]
            cfg = "test.cfg"
            psp_module = "PS2RC_TEST"
            [build]
            translation = "{work}/trans"
            """ % (len(elf), sha)))
    os.makedirs(os.path.join(games, "analysis"), exist_ok=True)
    with open(os.path.join(games, "analysis", "matches.json"), "w") as f:
        f.write("{}")
    with open(os.path.join(games, "convert.toml"), "w", newline="\n") as f:
        f.write(textwrap.dedent("""\
            [convert]
            analysis = [%s]
            analysis_outputs = ["{work}/phase3/functions.txt"]
            translate = ["{repo}/scripts/fake.py", "translate", "--out", "{translation}"]
            extras = [["{repo}/scripts/fake.py", "extras"]]
            extras_outputs = ["{work}/phase6psp/trapset.txt"]
            analysis_files = ["analysis/matches.json"]
            [convert.build]
            psp_gcc = "15.2.0"
            [stage]
            folder = "PS2RC_TEST"
            include = ["DATA/*", "MODELS/*"]
            exclude = ["MODELS/*.IMG"]
            files = ["CONFIG.INI"]
            audio_all = ["AUDIO/SFX/*01.PAK"]
            audio_min = []
            replace = [{ file = "CONFIG.INI", find = "memory\\t\\t13550\\r\\n", with = "memory\\t\\t\\t9000\\r\\n" }]
            [stage.cfg]
            render = "ge"
            %s
            """ % (analysis_cmd, recipe_extra)))
    return root
