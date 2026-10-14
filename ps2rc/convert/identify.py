"""Which game and version is this disc? SYSTEM.CNF (BOOT2) -> boot program + serial -> the game pack whose
[verify] matches -> the boot program's size and SHA-256 against the pack."""
import hashlib
import os

from . import iso9660


class IdentifyError(Exception):
    def __init__(self, code, message, hint=""):
        super().__init__(message)
        self.code, self.message, self.hint = code, message, hint


class Identity:
    def __init__(self, **kw):
        self.__dict__.update(kw)

    def to_dict(self):
        d = dict(self.__dict__)
        d.pop("pack", None)
        d["pack_name"] = self.pack.name if self.pack else None
        return d


def _norm_boot(s):
    return iso9660.boot_file(s).upper() if s else ""


def list_packs(repo):
    """Every game pack of the checkout / bundle: [(name, Pack)] (the template is skipped)."""
    from ps2rc import pack as packmod
    out = []
    gdir = os.path.join(repo, "games")
    for name in sorted(os.listdir(gdir)) if os.path.isdir(gdir) else []:
        if name.startswith("_") or not os.path.exists(os.path.join(gdir, name, "pack.toml")):
            continue
        try:
            out.append((name, packmod.Pack(os.path.join(gdir, name))))
        except (SystemExit, Exception) as e:          # a broken pack must not hide the others
            out.append((name, e))
    return out


def describe_pack(p):
    return "%s (%s %s, %s)" % (p.name, p.get("id.serial", "?"), p.get("id.version", "?"), p.get("id.region", "?"))


def identify(disc, repo, pack_name=None, progress=None):
    """-> Identity(serial, boot, cnf, version, elf_size, elf_sha256, pack). Raises IdentifyError."""
    try:
        cnf = disc.system_cnf()
    except Exception as e:
        raise IdentifyError("not_ps2", str(e), "choose the image (or the root folder) of a PS2 game disc")
    boot2 = cnf.get("BOOT2")
    if not boot2:
        raise IdentifyError("not_ps2", "%s: SYSTEM.CNF has no BOOT2 line (a PS1 disc or not a game disc?)" % disc.label)
    boot = iso9660.boot_file(boot2)
    serial = iso9660.serial_of(boot)
    f = disc.find(boot)
    if f is None:
        raise IdentifyError("not_ps2", "%s: the boot program %s named by SYSTEM.CNF is not on the disc" % (disc.label, boot))
    data = disc.read(f)
    sha = hashlib.sha256(data).hexdigest()
    packs = list_packs(repo)
    good = [(n, p) for n, p in packs if not isinstance(p, Exception)]
    if pack_name:
        good = [(n, p) for n, p in good if n == pack_name]
        if not good:
            raise IdentifyError("no_pack", "no game pack named %s" % pack_name,
                                "packs here: %s" % (", ".join(n for n, _ in packs) or "none"))
    same_game = [p for _n, p in good
                 if _norm_boot(p.get("verify.system_cnf_boot2")) == boot.upper() or
                 (serial and p.get("id.serial", "").upper() == serial) or
                 (not p.get("verify.system_cnf_boot2") and (p.get("id.boot_elf") or "").upper() == boot.upper())]
    ident = Identity(serial=serial, boot=boot, boot2=boot2, cnf=cnf, version=cnf.get("VER"), region=cnf.get("VMODE"),
                     elf_size=len(data), elf_sha256=sha, pack=None, disc_kind=disc.kind, disc_label=disc.label)
    if not same_game:
        raise IdentifyError(
            "no_pack", "%s: no game pack for %s (boot program %s, version %s)" % (
                disc.label, serial or "this disc", boot, cnf.get("VER", "?")),
            "supported here: %s" % ("; ".join(describe_pack(p) for _n, p in good) or "no packs installed"))
    for p in same_game:
        want_sha = (p.get("verify.elf_sha256") or "").lower()
        want_size = p.get("verify.elf_size")
        if (not want_sha or want_sha == sha) and (want_size in (None, len(data))):
            if not want_sha:
                ident.warning = "pack %s has no [verify] elf_sha256: the version is not checked" % p.name
            ident.pack = p
            return ident
    raise IdentifyError(
        "version", "%s: %s version %s is not supported (boot program %d bytes, SHA-256 %s)" % (
            disc.label, serial or boot, cnf.get("VER", "?"), len(data), sha),
        "supported versions of this game: %s" % "; ".join(
            "%s - SHA-256 %s" % (describe_pack(p), (p.get("verify.elf_sha256") or "?")[:16] + "...")
            for p in same_game))
