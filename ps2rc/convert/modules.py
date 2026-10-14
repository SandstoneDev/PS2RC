"""Game-derived C modules made on the player's PC by generators (IOP modules -> C with `ps2rc.iop`, VU0 micro
programs -> C). Interface, stubbed until the generators are wired into the runtime build:

    [convert]
    generators = [
      { name = "snddrv", kind = "iop", input = "SYSTEM/SNDDRV.IRX", sym = "snddrv", out = "gen_snddrv.c" },
      { name = "x", kind = "python", module = "ps2rc.vu.micro_gen:generate", input = "SLES_123.45", out = "gen_x.c" },
    ]

kind "iop":    python -m ps2rc.iop gen <input extracted from the disc> -o <work>/modules/<out> --sym <sym>
kind "python": <module>:<function>(input_path, out_path, log, **args) -> None (writes out_path); args = the entry's
               `args` table (e.g. chain / entries / sym of a VU0 micro program)
Each generated file is copied into the runtime build next to the hand-written sources, with -D<define> (default
HLE_GEN_<NAME>); include_only = true: the file is #included by a glue module (it goes on the include path, not into
the compiled units). What the runtime does with it (replace a hand-written module) is the pack profile's business
([profile.<name>] exclude_sources / extra_sources). A build profile's own `generators` list replaces
[convert] generators. A pack with no generators builds the checkout's hand-written modules (a LOCAL build).
"""
import importlib
import os

from .steps import ConvertError, run_logged, python_exe


def plan(recipe, profile=None):
    if profile and profile[1].get("generators") is not None:
        return list(profile[1]["generators"])
    return list(recipe.get("convert.generators", []) or [])


def available(gen):
    if gen.get("kind") == "iop":
        try:
            importlib.import_module("ps2rc.iop")
            return True
        except ImportError:
            return False
    if gen.get("kind") == "python":
        mod = gen.get("module", "").split(":")[0]
        try:
            importlib.import_module(mod)
            return True
        except ImportError:
            return False
    return False


def run(recipe, disc, work, repo, env, log, progress, profile=None):
    """-> list of (generated C path, define, include only). Generators whose code is missing are reported and
    skipped."""
    out_dir = os.path.join(work, "modules")
    os.makedirs(out_dir, exist_ok=True)
    made = []
    gens = plan(recipe, profile)
    if not gens:
        progress.info("modules: the pack declares no generators; the checkout's hand-written game modules are "
                      "compiled (a LOCAL build: such a build must never be shared)")
        return made
    for g in gens:
        name = g.get("name", "?")
        if not available(g):
            progress.warn("modules: generator %s (%s) is not in this converter version - skipped" % (name, g.get("kind")))
            continue
        f = disc.find(g["input"])
        if f is None:
            raise ConvertError("generator", "generator %s: %s is not on the disc" % (name, g["input"]))
        src = os.path.join(work, "modules", "in", os.path.basename(f.rel))
        os.makedirs(os.path.dirname(src), exist_ok=True)
        disc.copy_to(f, src)
        out = os.path.join(out_dir, g["out"])
        if g["kind"] == "iop":
            run_logged([python_exe(), "-m", "ps2rc.iop", "gen", src, "-o", out, "--sym", g["sym"]], repo, env, log,
                       progress, "generator " + name)
        else:
            mod, fn = g["module"].split(":")
            try:
                getattr(importlib.import_module(mod), fn)(src, out, progress.log, **dict(g.get("args") or {}))
            except Exception as e:                       # the generator refused this input (another version?)
                raise ConvertError("generator", "generator %s: %s" % (name, e),
                                   "the disc's %s is not the one the pack was made for" % g["input"])
        if not os.path.exists(out):
            raise ConvertError("generator", "generator %s wrote no %s" % (name, out))
        made.append((out, "-D%s" % g.get("define", "HLE_GEN_%s" % name.upper()), bool(g.get("include_only"))))
        progress.log("modules: %s -> %s" % (name, out))
    return made
