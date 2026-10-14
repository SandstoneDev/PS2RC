"""The conversion recipe of a game pack: what the converter needs beyond the pack format 1 keys.

Sections `[convert]` (commands of the analysis / translation / extras steps, files they read, generators),
`[convert.build]` (build options), `[stage]` (output folder, staging rules, replacements, cfg, movies) and `[disc]`
(layout for a disc folder). They are read from pack.toml when it has them, else from `convert.toml` next to it.

Placeholders in command arguments and paths: {work} {repo} {disc} {pack} {analysis} {translation}.
"""
import fnmatch
import glob
import os

try:
    import tomllib
except ImportError:                                     # Python < 3.11
    tomllib = None

SECTIONS = ("convert", "stage", "disc")
# a pack without [convert] but with the generic [translate] table: the generic translation driver (it runs the
# function discovery itself when the pack names no function set); no analysis or extra steps
DEFAULT_CONVERT = {"analysis": [], "translate": ["{repo}/ps2rc/pipeline/translate.py", "--pack", "{pack}", "--out",
                                                "{translation}"], "extras": []}


class RecipeError(Exception):
    pass


class Recipe:
    def __init__(self, pack, dev_analysis=None):
        self.pack = pack
        self.dev_analysis = dev_analysis            # the developer checkout's analysis dir (fallback for files)
        side = {}
        p = os.path.join(pack.dir, "convert.toml")
        if os.path.exists(p):
            with open(p, "rb") as f:
                side = tomllib.load(f)
        self.data = {}
        self.source = {}
        for s in SECTIONS:
            if s in pack.data:
                self.data[s] = pack.data[s]
                self.source[s] = "pack.toml"
            elif s in side:
                self.data[s] = side[s]
                self.source[s] = "convert.toml"
            else:
                self.data[s] = {}
                self.source[s] = None
        if self.source["convert"] is None and "translate" in pack.data:
            self.data["convert"] = dict(DEFAULT_CONVERT)
            self.source["convert"] = "default for [translate]"

    def get(self, key, default=None):
        v = self.data
        for k in key.split("."):
            if not isinstance(v, dict) or k not in v:
                return default
            v = v[k]
        return v

    # --- paths -------------------------------------------------------------------------------------------------
    def expand(self, s, ctx):
        """Expand placeholders; ctx: dict with work, repo, disc, analysis, translation."""
        if not isinstance(s, str):
            return s
        out = s
        for k in ("work", "repo", "disc", "analysis", "translation", "pack"):
            if "{%s}" % k in out:
                v = self.pack.dir if k == "pack" else ctx.get(k)
                if v is None:
                    raise RecipeError("pack %s: %r uses {%s}, which is not known here" % (self.pack.name, s, k))
                out = out.replace("{%s}" % k, v)
        if out != s and s.startswith("{"):                # a path argument: native separators
            out = os.path.normpath(out)
        return out

    def pack_file(self, rel):
        """A file the pack provides (relative to the pack folder). Fallback for a developer checkout whose pack does
        not hold the file yet: the same name in the checkout's analysis dir (local.toml) - reported by the caller."""
        p = os.path.join(self.pack.dir, rel)
        if os.path.exists(p):
            return p, False
        if self.dev_analysis and os.path.isdir(self.dev_analysis):
            tail = rel.split("/", 1)[1] if rel.startswith("analysis/") else rel
            q = os.path.join(self.dev_analysis, tail)
            if os.path.exists(q):
                return q, True
            hits = glob.glob(os.path.join(self.dev_analysis, "**", os.path.basename(rel)), recursive=True)
            if hits:
                return sorted(hits)[0], True
        return None, False

    # --- staging -----------------------------------------------------------------------------------------------
    def stage_selection(self, iso_paths, audio, boot):
        """The disc files to stage (ISO paths): include - exclude, + the audio set, minus the boot program (it is staged
        next to the EBOOT), + the explicit `files` (kept even when one of them is the boot program)."""
        up = {p.upper(): p for p in iso_paths}

        def match(pats):
            sel = set()
            for pat in pats or []:
                pat = pat.upper()
                sel |= {orig for u, orig in up.items() if fnmatch.fnmatchcase(u, pat)}
            return sel
        sel = match(self.get("stage.include", [])) - match(self.get("stage.exclude", []))
        if audio in ("all", "min"):
            sel |= match(self.get("stage.audio_%s" % audio, []))
        sel.discard(up.get(boot.upper(), boot))
        missing = []
        for f in self.get("stage.files", []):
            key = f.replace("\\", "/").upper()
            hit = up.get(key) or up.get(key + ".")
            if hit:
                sel.add(hit)
            else:
                missing.append(f)
        return sel, missing

    def replacements(self):
        out = {}
        for r in self.get("stage.replace", []):
            out[r["file"].upper()] = (r["find"].encode("latin-1"), r["with"].encode("latin-1"))
        return out


def apply_replacement(data, find, repl, name):
    """Same-size replacement (the disc map and the ISO metadata keep the file size)."""
    if len(find) != len(repl):
        raise RecipeError("%s: replacement changes the size (%d -> %d bytes)" % (name, len(find), len(repl)))
    if data.count(find) != 1:
        raise RecipeError("%s: the text to replace occurs %d times (expected once) - another disc version?"
                          % (name, data.count(find)))
    return data.replace(find, repl, 1)
