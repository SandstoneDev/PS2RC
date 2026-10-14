"""Runs a pack or pipeline script the way `python SCRIPT ARGS` does, also with an interpreter that leaves the
script's folder off sys.path: the app folder's private Python has a ._pth file, which puts it into isolated path mode
(no script folder, no PYTHONPATH), and pack scripts import their neighbours (a common module next to them).

    python -m ps2rc.convert.runscript SCRIPT [ARGS ...]
"""
import os
import runpy
import sys


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: python -m ps2rc.convert.runscript SCRIPT [ARGS ...]")
    script = os.path.abspath(sys.argv[1])
    sys.argv = [script] + sys.argv[2:]
    sys.path.insert(0, os.path.dirname(script))
    runpy.run_path(script, run_name="__main__")


if __name__ == "__main__":
    main()
