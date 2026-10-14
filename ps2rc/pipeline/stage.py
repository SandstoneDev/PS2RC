"""Stage a PSP folder (for the memory stick or an emulator) from a game pack's build.

    python ps2rc/pipeline/stage.py [--pack NAME] --out DIR [--eboot FILE] [--prx FILE] [--audio all|min]
                                   [--movies DIR] [--cfg key=value ...]

Writes into DIR (e.g. <somewhere>/PS2RC_EXAMPLE, copied as PSP/GAME/PS2RC_EXAMPLE):
  EBOOT.PBP        --eboot, else the pack's [build] pkg_out/EBOOT.PBP
  <boot_elf>       the program from the pack's disc folder (its data part is loaded at run time)
  discmap.txt      the pack's [run] discmap; iso_meta.bin next to it when there is one
  disc/<file>      the disc files the pack's [stage] selects (the same rules as the PC converter, ps2rc/convert):
                   include - exclude (case-insensitive globs over the disc's paths), + audio_all / audio_min with
                   --audio, + the explicit files; [stage] replace = same-size text replacements of staged files
  movies/          --movies DIR: the files of DIR (converted movies)
  <cfg>            the run options file ([runtime] cfg, default ps2rc.cfg): the pack's [stage.cfg], then --cfg
  ps2rc_exc.prx    --prx FILE: the kernel PRX (crash handler, counters); without it the run goes on without them
"""
import os
import shutil
import sys

if __package__ in (None, ""):
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
from ps2rc import pack as _pack  # noqa: E402
from ps2rc.convert.recipe import Recipe, apply_replacement  # noqa: E402


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)
    return os.path.getsize(src)


def disc_paths(disc):
    """the disc folder's files as ISO-style paths (forward slashes, relative to the disc root)"""
    out = []
    for dp, _d, fs in os.walk(disc):
        for fn in fs:
            out.append(os.path.relpath(os.path.join(dp, fn), disc).replace("\\", "/"))
    return out


def stage(p, out, eboot=None, prx=None, cfg=None, audio=None, movies=None):
    disc = p.disc()
    n = copy(eboot or os.path.join(p.path("build.pkg_out"), "EBOOT.PBP"), os.path.join(out, "EBOOT.PBP"))
    elf = p.get("id.boot_elf")
    n += copy(os.path.join(disc, elf), os.path.join(out, elf))
    dmap = p.path("run.discmap")
    n += copy(dmap, os.path.join(out, "discmap.txt"))
    meta = os.path.join(os.path.dirname(dmap), "iso_meta.bin")
    if os.path.exists(meta):
        n += copy(meta, os.path.join(out, "iso_meta.bin"))
    recipe = Recipe(p)
    sel, missing = recipe.stage_selection(disc_paths(disc), audio, elf)
    if missing:
        sys.exit("stage: [stage] files not in the disc folder: %s" % ", ".join(missing))
    repl = recipe.replacements()
    for f in sorted(sel):
        dst = os.path.join(out, "disc", f)
        if f.upper() in repl:
            find, new = repl[f.upper()]
            data = apply_replacement(open(os.path.join(disc, f), "rb").read(), find, new, f)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(data)
            n += len(data)
        else:
            n += copy(os.path.join(disc, f), dst)
    if movies:
        for fn in sorted(os.listdir(movies)):
            if os.path.isfile(os.path.join(movies, fn)):
                n += copy(os.path.join(movies, fn), os.path.join(out, "movies", fn))
    if prx:
        n += copy(prx, os.path.join(out, "ps2rc_exc.prx"))
    opts = dict((str(k), str(v)) for k, v in (recipe.get("stage.cfg", {}) or {}).items())
    for kv in cfg or []:
        k, v = kv.split("=", 1)
        opts[k] = v
    with open(os.path.join(out, p.get("runtime.cfg", "ps2rc.cfg")), "w", encoding="ascii", newline="\n") as f:
        f.write("# run options (hle/psp/hle_psp.c read_config)\n")
        for k, v in opts.items():
            f.write("%s=%s\n" % (k, v))
    return n


def main(argv):
    def opt(flag):
        return argv[argv.index(flag) + 1] if flag in argv else None
    if "--out" not in argv:
        sys.exit(__doc__)
    cfg = [argv[i + 1] for i, a in enumerate(argv) if a == "--cfg"]
    p = _pack.load(opt("--pack"))
    n = stage(p, os.path.abspath(opt("--out")), opt("--eboot"), opt("--prx"), cfg, opt("--audio"), opt("--movies"))
    print("stage %s: %.1f MB (pack %s)" % (opt("--out"), n / 1e6, p.name))


if __name__ == "__main__":
    main(sys.argv[1:])
