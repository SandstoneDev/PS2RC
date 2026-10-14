"""Command line of the ps2rc package (the recompiler toolkit).

    python -m ps2rc doctor [--json] [--no-wsl] [--no-docker] [--quick]
                                         check this machine and print what is missing, with install hints
    python -m ps2rc skills list|check|sync [--target DIR ...]
                                         the agent skills in skills/ (sync copies them where agents look)
    python -m ps2rc kb list | search WORDS | new CATEGORY SLUG TITLE | index [--check] | check
                                         the knowledge base of field notes in knowledge/
    (skills and kb need the agent kit: skills/, knowledge/ and AGENTS.md; a checkout without it has the doctor only)

Other entry points: python -m ps2rc.convert (the PC converter), python -m ps2rc.pbp (EBOOT packer),
python -m ps2rc.iop (IOP module recompiler), python -m ps2rc.vu.micro_gen (VU0 microcode translator),
python -m ps2rc.oracle (PCSX2 reference runs). None of these commands downloads anything or sends anything off the
machine.
"""
import sys


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    if not argv or argv[0] in ("-h", "--help", "help"):
        print(__doc__.strip())
        return 0
    cmd, rest = argv[0], argv[1:]
    if cmd == "doctor":
        from .kit import doctor
        return doctor.main(rest)
    if cmd in ("skills", "kb", "knowledge"):
        try:
            if cmd == "skills":
                from .kit import skills as mod
            else:
                from .kit import kb as mod
        except ImportError:
            print("%s: the agent kit is not part of this checkout" % cmd, file=sys.stderr)
            return 2
        return mod.main(rest)
    print("unknown command %r (try: python -m ps2rc --help)" % cmd, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
