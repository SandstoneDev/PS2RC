"""Command line of the converter.

    python -m ps2rc.convert <disc.iso | disc folder> --out DIR [--icon0 PNG] [--icon1 PMF|video] [--pic0 PNG]
        [--pic1 PNG] [--snd0 AT3] [--title TEXT] [--movies auto|skip|convert] [--audio all|min|none] [--jobs N]
        [--lang en|ru] [--set NAME=VALUE] [--toolchain DIR] [--use native|wsl|docker] [--pack NAME] [--estimate] ...
    python -m ps2rc.convert verify <PSP folder> [--quick] [--json]
        check the folder against its ps2rc_manifest.json (no disc image needed; works on the memory stick)
    python -m ps2rc.convert update <disc.iso | disc folder> (--out DIR | --folder PSP_FOLDER) [--dry-run] [...]
        repair / update: only missing, damaged or out-of-date files are written (read back after writing); the
        player's settings are kept (new settings added), saves are never touched; --dry-run lists the changes
    python -m ps2rc.convert plan <PSP folder>
        what an update would change, without the disc image (tells whether the image is needed)

Exit status: 0 done (verify: all files OK), 1 refused / failed (the message says why and what to do), 2 bad
arguments, 3 verify: problems found.
"""
import argparse
import os
import sys
import time

from . import VERSION
from .steps import ConvertError, Progress


def parser():
    ap = argparse.ArgumentParser(prog="python -m ps2rc.convert",
                                 description="Convert your own PS2 disc image into a PSP game folder (on this PC).")
    ap.add_argument("source", nargs="?", help="the disc image (.iso, or a raw .bin) or the disc's root folder "
                    "(update: may be left out when only the settings files need a repair)")
    ap.add_argument("--out", help="output folder; the PSP folder PS2RC_<serial> (e.g. PS2RC_SLUS12345) is made inside it")
    ap.add_argument("--folder", help="the PSP folder itself (update: e.g. the memory stick's PSP/GAME/<folder>)")
    g = ap.add_argument_group("menu assets (optional; see ps2rc/pbp/README.md)")
    g.add_argument("--title", help="title in the PSP menu (default: the pack's)")
    g.add_argument("--icon0", help="icon, 144x80 PNG (other pictures are converted when Pillow is installed)")
    g.add_argument("--icon1", help="animated icon: a 144x80 .pmf, or a video / animated GIF (needs ffmpeg)")
    g.add_argument("--pic0", help="information picture, 310x180 PNG")
    g.add_argument("--pic1", help="background picture, 480x272 PNG")
    g.add_argument("--snd0", help="menu music: a ready ATRAC3plus .at3")
    g.add_argument("--fit", choices=("cover", "contain", "stretch"), default="cover")
    g.add_argument("--disc-id", help="PARAM.SFO DISC_ID (default PSRC + the serial's digits; 'pspsdk' = pspsdk's)")
    g = ap.add_argument_group("game data")
    g.add_argument("--movies", choices=("auto", "skip", "convert"), default="auto",
                   help="intro movies: auto = convert when possible (ffmpeg found), else skip")
    g.add_argument("--audio", choices=("all", "min", "none"), default="all",
                   help="sound files: all (about 2.9 GB), min (boot + first minutes), none")
    g.add_argument("--ffmpeg", help="ffmpeg executable for the movies / animated icon")
    g.add_argument("--layout", help="sector layout file for a disc FOLDER input (default: the pack's)")
    g.add_argument("--iso-meta", choices=("synthetic", "real"), default="synthetic",
                   help="file-system sectors: synthetic (rebuilt, default) or real (the image's own)")
    g.add_argument("--cfg", action="append", default=[], metavar="KEY=VALUE",
                   help="extra run option, appended to the settings file")
    g.add_argument("--set", action="append", default=[], metavar="NAME=VALUE", dest="settings",
                   help="a player setting put on its own line of the settings file (e.g. draw_distance=60)")
    g.add_argument("--lang", help="language of the comments in the settings file: en (default) or ru")
    g.add_argument("--no-dev-cfg", action="store_true", help="do not write the developer run options file")
    g = ap.add_argument_group("build")
    g.add_argument("--jobs", "-j", type=int, default=os.cpu_count(), help="parallel compile jobs (default: all CPUs)")
    g.add_argument("--toolchain", help="a PSP toolchain folder (bin/psp-gcc)")
    g.add_argument("--use", choices=("native", "wsl", "docker"), help="force one kind of toolchain")
    g.add_argument("--allow-docker-pull", action="store_true", help="let Docker download the pspdev image")
    g.add_argument("--no-splash", action="store_true", help="build without the community credit screen")
    g.add_argument("--build-num", type=int, default=0, help="number shown in the log header (default 0)")
    g.add_argument("--prx", default="build", help="kernel helper module: build (default), skip, or a ready .prx")
    g.add_argument("--profile", help="the pack's build profile: public (default for packs that define it: no "
                   "hand-translated game code) or full (the developer build; LOCAL use only)")
    g = ap.add_argument_group("run")
    g.add_argument("--pack", help="game pack name (default: found from the disc)")
    g.add_argument("--work", help="work folder (default %%LOCALAPPDATA%%\\PS2RC\\work, as the window's; never inside "
                   "--out); kept to resume or update quickly")
    g.add_argument("--clean-work", action="store_true", help="remove the whole work folder after a finished run")
    g.add_argument("--estimate", action="store_true", help="only identify, check and print the estimates")
    g.add_argument("--restart", action="store_true", help="ignore what earlier runs finished")
    g.add_argument("--no-manifest", action="store_true", help="skip the md5 manifest of the output folder")
    g.add_argument("--translation", help=argparse.SUPPRESS)          # developer: reuse a translation folder
    g.add_argument("--stop-after", help=argparse.SUPPRESS,           # developer
                   choices=("analysis", "translate", "extras", "prepare", "compile", "link", "eboot"))
    g.add_argument("--allow-gpl-libs", action="store_true", help=argparse.SUPPRESS)   # developer: old trees only
    g.add_argument("--cold-opt", choices=("O1", "Os"), help="compile functions never run in the pack's profiles "
                   "(its cold list) at -O1 / -Os: a faster compile; off by default until checked on hardware")
    g.add_argument("--prebuilt", help="an extra prebuilt object store (see python -m ps2rc.convert.prebuilt)")
    g.add_argument("--quiet", "-q", action="store_true", help="less output (no tool lines)")
    g = ap.add_argument_group("update")
    g.add_argument("--dry-run", action="store_true", help="update: only list what would change")
    g.add_argument("--verify-writes", choices=("on", "off"), help="read every written file back (default: on for "
                   "an update)")
    g.add_argument("--force", action="store_true", help="update: allow a folder made from another game version")
    ap.add_argument("--version", action="version", version="PS2RC converter %s" % VERSION)
    return ap


class ConsoleView:
    def __init__(self, quiet=False):
        self.quiet = quiet
        self.last_pct = {}
        self.t0 = time.time()

    def __call__(self, ev):
        kind = ev[0]
        if kind == "step":
            _k, i, n, name, title = ev
            print("[%2d/%d] %s" % (i, n, title), flush=True)
        elif kind == "done":
            _k, name, secs, cached = ev
            print("       %s %s" % ("kept from the last run" if cached else "done in %.0f s" % secs, ""), flush=True)
        elif kind == "pct":
            _k, name, done, total, unit = ev
            pct = int(100 * done / total) if total else 100
            step = 10 if unit == "bytes" else 5
            if pct // step != self.last_pct.get(name, -1) // step or done == total:
                if self.last_pct.get(name) != pct:
                    extra = (" (%.1f / %.1f GB)" % (done / 1e9, total / 1e9)) if unit == "bytes" else \
                        (" (%d / %d %s)" % (done, total, unit))
                    print("       %s %3d%%%s" % (name, pct, extra), flush=True)
                    self.last_pct[name] = pct
        elif kind == "info":
            print("       " + ev[1], flush=True)
        elif kind == "warn":
            print("       WARNING: " + ev[1], flush=True)
        elif kind == "log" and not self.quiet:
            print("         " + ev[1], flush=True)


def verify_main(argv):
    ap = argparse.ArgumentParser(prog="python -m ps2rc.convert verify", description="Check a PSP folder made by "
                                 "the converter (no disc image needed).")
    ap.add_argument("folder")
    ap.add_argument("--quick", action="store_true", help="sizes only (no hashes)")
    ap.add_argument("--json", action="store_true", help="the report as JSON")
    a = ap.parse_args(argv)
    from .manifest import verify_folder
    prog = Progress()
    prog.listeners.append(ConsoleView(True))
    rep = verify_folder(a.folder, prog, a.quick)
    if a.json:
        import json
        print(json.dumps(rep.to_dict(), indent=1))
    else:
        print(rep.text())
    return 0 if rep.ok else 3


def plan_main(argv):
    ap = argparse.ArgumentParser(prog="python -m ps2rc.convert plan", description="What an update would change.")
    ap.add_argument("folder")
    a = ap.parse_args(argv)
    from .manifest import plan_update
    prog = Progress()
    prog.listeners.append(ConsoleView(True))
    plan = plan_update(a.folder, None, prog)
    print(plan.text())
    return 0


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] == "verify":
        return verify_main(argv[1:])
    if argv and argv[0] == "plan":
        return plan_main(argv[1:])
    update = bool(argv and argv[0] == "update")
    if update:
        argv = argv[1:]
    a = parser().parse_args(argv)
    if not a.out and not a.folder:
        print("ERROR: give --out DIR (or --folder PSP_FOLDER)", file=sys.stderr)
        return 2
    if not a.source and not update:
        print("ERROR: give the disc image (.iso) or the disc's folder", file=sys.stderr)
        return 2
    from .pipeline import Conversion, Options
    if sys.version_info < (3, 11):
        print("PS2RC converter needs Python 3.11 or newer", file=sys.stderr)
        return 1
    for k in ("icon0", "icon1", "pic0", "pic1", "snd0", "layout", "translation"):
        v = getattr(a, k)
        if v and not os.path.exists(v):
            print("ERROR: --%s %s: no such file" % (k, v), file=sys.stderr)
            return 2
    for kv in a.cfg + a.settings:
        if "=" not in kv:
            print("ERROR: %s: KEY=VALUE expected" % kv, file=sys.stderr)
            return 2
    out = a.out or os.path.dirname(os.path.abspath(a.folder))
    opts = Options(a.source, out, work=a.work, pack=a.pack, title=a.title, icon0=a.icon0, icon1=a.icon1,
                   pic0=a.pic0, pic1=a.pic1, snd0=a.snd0, fit=a.fit, movies=a.movies, audio=a.audio, jobs=a.jobs,
                   toolchain=a.toolchain, toolchain_prefer=a.use, allow_docker_pull=a.allow_docker_pull,
                   layout=a.layout, splash=not a.no_splash, build_num=a.build_num, iso_meta=a.iso_meta,
                   disc_id=a.disc_id, translation=a.translation, restart=a.restart, cfg=a.cfg, ffmpeg=a.ffmpeg,
                   manifest=not a.no_manifest, estimate_only=a.estimate, prx=a.prx, stop_after=a.stop_after,
                   allow_gpl_libs=a.allow_gpl_libs, cold_opt=a.cold_opt or "", prebuilt=a.prebuilt,
                   lang=a.lang, dev_cfg=not a.no_dev_cfg, folder=a.folder, profile=a.profile,
                   clean_work=a.clean_work,
                   settings=dict(kv.split("=", 1) for kv in a.settings), update=update or a.dry_run,
                   dry_run=a.dry_run, force=a.force,
                   verify_writes=None if a.verify_writes is None else a.verify_writes == "on")
    prog = Progress()
    prog.listeners.append(ConsoleView(a.quiet))
    print("PS2RC converter %s - your disc stays on this PC; nothing is uploaded." % VERSION, flush=True)
    try:
        res = Conversion(opts, prog).run()
    except ConvertError as e:
        for line in e.lines():
            print(line, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("stopped (finished steps are kept; run the same command again to continue)", file=sys.stderr)
        return 1
    finally:
        prog.close()
    if res.get("plan_text") is not None:
        return 0
    if res.get("folder"):
        print("\nDone. Copy the folder\n    %s\nto PSP/GAME/ on your memory stick." % res["folder"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
