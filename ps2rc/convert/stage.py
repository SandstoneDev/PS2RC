"""The output folder (what the player copies to PSP/GAME/), staged straight from the disc image: the EBOOT, the boot
program (its data is loaded at run time), the disc map, the ISO metadata sectors, the run options, the kernel PRX,
the disc files the pack's recipe selects (same-size replacements applied) and, optionally, the movies.

Disc files go from the image straight into the output folder (no extraction, no intermediate copy; the output folder
may be the memory stick itself), and their md5 / SHA-256 are computed while they are written, so the manifest of the
verify step does not read the 3-4 GB folder a second time. verify_writes: every file written is read back and
compared (USB writes to a PSP can drop data); a mismatch is written again (3 tries)."""
import hashlib
import json
import os
import shutil

from .recipe import apply_replacement
from .steps import ConvertError

README = """This folder was made by the PS2RC converter on this PC from your own copy of the game.

It contains your game's data and program code converted from it. It is for your own use on your own PSP:
do not share it, upload it or give it to anyone. The PS2RC project never distributes games or game files.

Install: copy this whole folder ({folder}) to PSP/GAME/ on the memory stick ({size} needed).
Game: {game}
Converter: PS2RC converter {version}, pack {pack}
"""


def write_cfg(path, cfg, header):
    with open(path, "w", encoding="ascii", newline="\n") as f:
        f.write("# %s\n" % header)
        for k, v in cfg.items():
            f.write("%s=%s\n" % (k, v))


def _same(dst, size):
    return os.path.isfile(dst) and os.path.getsize(dst) == size


def _md5(path, chunk=1 << 20):
    h = hashlib.md5()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


class Md5Cache:
    """md5 (and SHA-256) of files the stage wrote, keyed by path + size + modification time (a changed file is hashed
    again)."""

    def __init__(self, path):
        self.path = path
        self.data = {}
        if path:
            try:
                self.data = json.load(open(path, encoding="utf-8"))
            except (OSError, ValueError):
                self.data = {}

    @staticmethod
    def _key(p):
        return os.path.normcase(os.path.abspath(p))

    def put(self, p, digest, sha256=None):
        st = os.stat(p)
        self.data[self._key(p)] = [st.st_size, st.st_mtime_ns, digest] + ([sha256] if sha256 else [])

    def _valid(self, p):
        v = self.data.get(self._key(p))
        if v:
            st = os.stat(p)
            if v[0] == st.st_size and v[1] == st.st_mtime_ns:
                return v
        return None

    def get(self, p):
        v = self._valid(p)
        return v[2] if v else None

    def hashes(self, p):
        """(sha256, md5) when both are known for the file as it is now, else None."""
        v = self._valid(p)
        return (v[3], v[2]) if v and len(v) > 3 else None

    def md5(self, p):
        d = self.get(p)
        if d is None:
            d = _md5(p)
            self.put(p, d)
        return d

    def save(self):
        if self.path:
            tmp = self.path + ".tmp"
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.data, f)
            os.replace(tmp, self.path)


class Stager:
    def __init__(self, disc, recipe, folder_dir, progress, md5cache=None, link=True, verify_writes=False):
        self.disc, self.recipe, self.dir, self.progress = disc, recipe, folder_dir, progress
        self.md5 = md5cache or Md5Cache(None)
        self.link = link            # a disc folder on the output's volume: hard links instead of copies
        self.linked = 0
        self.verify_writes = verify_writes
        self.rewritten = 0          # files written again after a read-back mismatch
        self.written = 0            # files written (not kept)
        self.trust = None           # set of normalised paths known good (the update's folder check), None = by size

    def _readback(self, path, sha):
        """Flush and read the file back; True = it holds what was written."""
        if not self.verify_writes:
            return True
        with open(path, "rb+") as f:
            f.flush()
            os.fsync(f.fileno())
        from .manifest import hash_file
        return hash_file(path)[0] == sha

    def _write_chunks(self, dst, chunks_fn, tick=None):
        """Write a file from chunks_fn() (an iterable of bytes) through dst.part, hashing on the way; with
        verify_writes read back and rewrite up to 3 times. -> (sha256, md5)."""
        from .manifest import Hasher
        for attempt in range(3):
            h = Hasher()
            with open(dst + ".part", "wb") as out:
                for c in chunks_fn():
                    out.write(c)
                    h.update(c)
                    if tick and attempt == 0:
                        tick(len(c))
            os.replace(dst + ".part", dst)
            sha, md5 = h.result()
            self.written += 1
            if self._readback(dst, sha):
                return sha, md5
            self.rewritten += 1
            self.progress.warn("%s: read back wrong after writing (try %d of 3): writing it again"
                               % (os.path.relpath(dst, self.dir), attempt + 1))
        raise ConvertError("stage", "%s: the copy reads back wrong three times" % dst,
                           "check the memory stick / USB cable, then run the update again")

    def _known_good(self, dst, size):
        """The file is there with its size, and (update) the folder check found its content right."""
        if not _same(dst, size):
            return False
        if self.trust is None:
            return True
        return os.path.normcase(os.path.abspath(dst)) in self.trust

    def _link(self, f, dst):
        """Hard-link a file of a disc folder (same volume) into the output: no bytes written. The staged disc files
        are never modified in place (replacements are written as new files), so the folder's file stays intact."""
        host = getattr(self.disc, "host_path", None)
        if not self.link or host is None:
            return False
        try:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.lexists(dst + ".part"):
                os.remove(dst + ".part")
            os.link(host(f), dst + ".part")
            os.replace(dst + ".part", dst)
        except OSError:             # another volume, FAT/exFAT, no permission: copy instead
            self.link = False
            return False
        self.linked += 1
        return True

    def selection(self, audio, boot):
        files = {f.iso: f for f in self.disc.files}
        sel, missing = self.recipe.stage_selection(list(files), audio, boot)
        if missing:
            raise ConvertError("stage", "files of the pack's staging list are missing on this disc: %s"
                               % ", ".join(missing[:8]), "the disc does not match the pack (damaged dump?)")
        return [files[i] for i in sorted(sel)]

    def plan_bytes(self, audio, boot):
        return sum(f.size for f in self.selection(audio, boot))

    def stage_disc(self, audio, boot):
        """Copy the selected files into disc/, apply replacements, remove files of an older selection.
        -> (files, bytes, list lines for disc_files.txt)."""
        sel = self.selection(audio, boot)
        repl = self.recipe.replacements()
        ddir = os.path.join(self.dir, "disc")
        os.makedirs(ddir, exist_ok=True)
        total = sum(f.size for f in sel)
        done = 0
        lines = []
        keep = set()
        for f in sel:
            dst = os.path.join(ddir, f.rel.replace("/", os.sep))
            keep.add(os.path.normcase(dst))
            key = f.iso.upper().rstrip(".")
            lines.append("%s %d%s" % (f.rel, f.size, "   # REPLACED: staged copy" if key in repl else ""))
            if key in repl:
                data = apply_replacement(self.disc.read(f), repl[key][0], repl[key][1], f.iso)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if not (os.path.isfile(dst) and open(dst, "rb").read() == data):
                    sha, md5 = self._write_chunks(dst, lambda: [data])
                else:
                    sha, md5 = hashlib.sha256(data).hexdigest(), hashlib.md5(data).hexdigest()
                self.md5.put(dst, md5, sha)
                done += f.size
            elif self._known_good(dst, f.size) or self._link(f, dst):
                done += f.size
            else:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                start = done

                def tick(n):
                    nonlocal done
                    done += n
                    self.progress.pct("stage", done, total, "bytes")
                    self.progress.check_cancel()
                sha, md5 = self._write_chunks(dst, lambda: self.disc.iter_chunks(f), tick)
                done = start + f.size
                self.md5.put(dst, md5, sha)
            self.progress.pct("stage", done, total, "bytes")
            self.progress.check_cancel()
        for dirpath, _d, names in os.walk(ddir):
            for n in names:
                p = os.path.join(dirpath, n)
                if os.path.normcase(p) not in keep:
                    os.remove(p)
        self.md5.save()
        return sel, total, lines

    def copy_file(self, src, name):
        dst = os.path.join(self.dir, name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not (_same(dst, os.path.getsize(src)) and _md5(dst) == _md5(src)):
            def chunks():
                with open(src, "rb") as f:
                    while True:
                        b = f.read(1 << 20)
                        if not b:
                            return
                        yield b
            sha, md5 = self._write_chunks(dst, chunks)
            self.md5.put(dst, md5, sha)
        return dst

    def write_bytes(self, name, data):
        dst = os.path.join(self.dir, name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if not (os.path.isfile(dst) and open(dst, "rb").read() == data):
            sha, md5 = self._write_chunks(dst, lambda: [data])
            self.md5.put(dst, md5, sha)
        return dst


def manifest(folder, progress=None, md5cache=None):
    """MANIFEST.md5 of every file (md5 + relative path; md5s known from the stage are not computed again)."""
    cache = md5cache or Md5Cache(None)
    rows, total = [], 0
    allf = []
    for dirpath, _d, names in os.walk(folder):
        for n in names:
            if n == "MANIFEST.md5" or n.endswith(".part"):
                continue
            allf.append(os.path.join(dirpath, n))
    allf.sort()
    size_all = sum(os.path.getsize(p) for p in allf)
    for p in allf:
        rows.append("%s  %s" % (cache.md5(p), os.path.relpath(p, folder).replace("\\", "/")))
        total += os.path.getsize(p)
        if progress:
            progress.pct("verify", total, size_all, "bytes")
    with open(os.path.join(folder, "MANIFEST.md5"), "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(rows) + "\n")
    cache.save()
    return len(rows), total
